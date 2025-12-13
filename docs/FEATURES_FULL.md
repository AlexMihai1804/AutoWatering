# AutoWatering Capability Compendium (December 2025 build)

This document inventories every capability exposed by the current firmware. It is aimed at integrators who need to understand data paths, feature boundaries, and system behaviour without reading the source directly.

---

## Core Control Model

### Channel Matrix
- Eight irrigation channels (`WATERING_CHANNELS_COUNT`) are compiled into `watering_channels[]`.
- Each channel owns its GPIO descriptor, coverage definition, watering schedule, compensation settings, and persistent runtime state.
- Channel state includes: last watering epoch, days after planting, cached FAO-56 metadata, master-valve mask, interval configuration, and configuration scoring.

### Mutual Exclusion
- `MAX_SIMULTANEOUS_VALVES` is fixed at 1 so only one zone valve is energised at any time.
- The master valve layer (`master_valve_config_t`) wraps every operation with:
  - Pre-start delay: default +3 s
  - Post-stop delay: default +2 s
  - Overlap grace window: 5 s (keeps master open between consecutive tasks)

### Task Pipeline
- All watering work goes through the Zephyr message queue `watering_tasks_queue` (depth 10).
- The scheduler thread refuses to push additional scheduled jobs when two or more tasks are already waiting, leaving space for manual or remote overrides.
- `watering_process_next_task()` promotes queued jobs to the execution thread, which tracks the active task in `watering_task_state`.

### Threading and Pacing
- `watering_start_tasks()` brings up two 2 kB threads:
  - Processing loop (`watering_task_fn`, published as `watering_task`)
  - Scheduler loop (`scheduler_task_fn`, published as `scheduler_task`)
- Sleep intervals keyed off `current_power_mode`:
  - Normal: 60 s
  - Energy-saving: 120 s
  - Ultra-low-power: 300 s

---

## Channel Configuration Catalog

### Identity and Hardware
- `watering_channel_t` stores:
  - Channel name (64 characters)
  - GPIO spec for the zone valve
  - Runtime state (`is_active`, `last_watering_time`)
- Master-valve participation controlled centrally by `master_valve_config_t`:
  - Enabled flag, GPIO, pre-start/post-stop delays, overlap grace window
  - Auto-management switch, active flag

### Scheduling Core
- `watering_event_t` defines the base schedule:
  - `schedule_type`: daily mask or periodic interval
  - `start_time`: hour/minute in local time
  - `watering_mode`: duration, volume, or automatic modes
  - Quantity unions (`duration_minutes` or `volume_liters`)
  - `auto_enabled`: scheduler toggle
- Schedules stored per channel in NVS and exposed via BLE Channel Configuration characteristic.

### Coverage and Agronomic Metadata
- Channels reference compiled databases by index:
  - `plant_db_index`: 0..222 (`UINT16_MAX` = unset)
  - `soil_db_index`: 0..14 (`UINT8_MAX` = unset)
  - `irrigation_method_index`: 0..14
- Coverage selects between area (`area_m2`) and plant count (`plant_count`).
- Legacy fields (`plant_type`, `soil_type`, `irrigation_method`, `coverage_legacy`, `plant_info`, `custom_plant`) remain for backward compatibility.

### Automatic Watering Parameters
- `auto_mode`: Quality (100%) or Eco (70%) FAO-56 outputs.
- `max_volume_limit_l`: caps any single automatic job.
- `enable_cycle_soak`: allows clients to flag soils that need cycle-and-soak strategies.
- Plant lifecycle tracking:
  - `planting_date_unix`: Unix timestamp when plants were established
  - `days_after_planting`: calculated field for FAO-56 crop coefficient staging
- `latitude_deg` and `sun_exposure_pct`: personalise evapotranspiration calculations.

### Compensation Settings
- Per-channel rain compensation:
  - `enabled`, `sensitivity`, `lookback_hours`, `skip_threshold_mm`, `reduction_factor`
- Per-channel temperature compensation:
  - `enabled`, `base_temperature` (default 20°C), `sensitivity` (default 0.05)
  - `min_factor` (default 0.5), `max_factor` (default 2.0)
- Snapshots of last applied adjustments:
  - `last_rain_compensation`: reduction percentage, skip flag
  - `last_temp_compensation`: compensation factor, adjusted requirement

### Interval Mode Settings
- Inline structure:
  - `watering_minutes`, `watering_seconds`
  - `pause_minutes`, `pause_seconds`
  - `phase_start_time`, `configured` flag
- Shadow copy (`interval_config_shadow`) compatible with API controllers:
  - `total_target`, `cycles_completed`, `currently_watering`
  - `phase_remaining_sec`

### Custom Soil Overlays
- `soil_config` toggles between standard database entries and full custom definition:
  - Name (32 chars), field capacity (%), wilting point (%)
  - Infiltration rate (mm/h), bulk density (g/cm³), organic matter (%)
- Custom values override database defaults inside FAO-56 water balance routines.

### Configuration Scoring
- `config_status` tracks completion flags for:
  - Basic setup (25% weight)
  - Growing environment (25% weight)
  - Compensation (20% weight)
  - Custom soil (15% weight)
  - Interval profile (15% weight)
- `configuration_score`: 0-100 powering onboarding feedback.
- `last_reset_timestamp`: records when maintenance cleared the configuration group.

### Global Toggles
- `auto_calc_enabled`, `auto_calc_interval_ms` (1–24 hours)
- `current_power_mode`, `system_initialized`, `system_status`
- Flow calibration: stored under NVS key 1000, accepted range 100–10,000 pulses/liter.

### Database Inventories
- Plant species: 223 entries (`PLANT_FULL_SPECIES_COUNT`)
- Enhanced soils: 15 types (`SOIL_ENHANCED_TYPES_COUNT`)
- Irrigation methods: 15 entries (`IRRIGATION_METHODS_COUNT`)
- Conversion scripts in `tools/` regenerate these tables from CSV.
- Optional external-flash binary format: `tools/csv2binary_flash.py` + `database_flash` (mounted at `/lfs/db/*.bin`).

---

## Sensor Stack and Telemetry

### Inventory
The production hardware assumes three physical sensors:
1. Pulse-based flow meter on the irrigation supply
2. Tipping-bucket rain gauge
3. BME280 environmental sensor (temperature, humidity, pressure)

No soil moisture probes are present in this build; related fields remain reserved for backward compatibility.

### Flow Sensor (Pulse Meter)
- **Hardware interface**: Devicetree node `water_flow_sensor` supplies port, pin, and active level; debounce configurable via `sensor_config.debounce_ms` (default 5 ms).
- **Data products**:
  - Atomic pulse count (`get_pulse_count()`)
  - Smoothed pulses-per-second rate (two-sample buffer updated every 10 s if ≥5 pulses arrived)
  - BLE notifications via Flow Sensor characteristic: triggered when cumulative pulse count advances by ≥100 or 30 s guard timer elapses
  - ISR schedules work item every 25 pulses
- **Configuration**: Calibration persisted under NVS key 1000, settable via `set_flow_calibration()` or Calibration Management characteristic. Accepted range: 100–10,000 pulses/liter (default from devicetree, fallback 450).
- **Safety & diagnostics**: `check_flow_anomalies()` runs ~1/sec. Raises:
  - `WATERING_STATUS_NO_FLOW` after three consecutive 3 s stalls with open valve
  - `WATERING_STATUS_UNEXPECTED_FLOW` when ≥10 pulses arrive while all valves closed

### Rain Sensor (Tipping Bucket)
- **Hardware interface**: `rain_sensor_config_t.debounce_ms` (default 5 ms).
- **Configuration** (`rain_nvs_config_t`):
  - `mm_per_pulse` (default 0.2 mm)
  - `sensor_enabled`, `integration_enabled`
  - `rain_sensitivity_pct`, `skip_threshold_mm`
  - `last_reset_time`
- **Data products**:
  - Hourly and daily accumulators: rainfall in 0.01 mm increments, pulse counts, data-quality scores
  - BLE characteristics: 18-rain-sensor-config, 19-rain-sensor-data, 20-rain-history-control
  - Persistent counters (`rain_nvs_state_t`): total pulses, last pulse time, current hour/day totals
- **Health & diagnostics**:
  - Pulse statistics (mean/stddev of interval timing), outlier suppression, accuracy percentage, consecutive error counts
  - Watchdog (`check_rain_sensor_health()`) runs every 5 minutes: reinitialises sensor, history, and integration modules on failure; validates NVS payloads
- **Integration controls** (`rain_integration_config_t`):
  - `rain_sensitivity_pct`, `skip_threshold_mm`, `effective_rain_factor`, `lookback_hours`, `integration_enabled`

### Environmental Suite (BME280)
- **Hardware interface**: `sensor_manager_init_bme280()` initialises on primary I2C bus. Simulation paths behind `CONFIG_ENV_SENSORS_SIM` generate deterministic data.
- **Data products** (`environmental_data_t`):
  - Mean/min/max temperature, humidity, pressure
  - Rainfall over last 24 h
  - Derived dewpoint and vapour pressures
  - Measurement interval, timestamp, validity flags
  - Data quality score (0-100)
- **Configuration** (`env_sensor_config_t`):
  - Sensor enable flags
  - Measurement intervals: temp/humidity 15 min, rain 60 min (default)
  - Calibration offsets: `temp_offset_c`, `humidity_offset_pct`, `rain_calibration_factor`
  - Quality thresholds: `min_data_quality` (default 80), `max_sensor_age_min` (default 120)
- **Health & diagnostics** (`env_sensor_status_t`):
  - Sensor online flags, last successful timestamps
  - Per-sensor error counts, overall health score
  - BLE characteristics: 21-environmental-data, 22-environmental-history

### Derived Telemetry and Compensation

#### Rain Integration
- Combines recent rainfall (up to `lookback_hours`), soil infiltration (`effective_rain_factor`), and channel sensitivity.
- Produces `rain_irrigation_impact_t`:
  - Recent rainfall, effective rainfall
  - Reduction percentage, skip boolean
  - Confidence score
- Module caches last result per channel and emits diagnostics (success/failure counts, calculation success rate).

#### Rain Compensation Engine
- `rain_compensation_config_t` adds higher-level policies:
  - Algorithm selection: simple, proportional, exponential, adaptive
  - Reduction factors, skip thresholds, lookback hours
- Each calculation fills `rain_compensation_calculation_t`:
  - Effective rainfall, adjusted requirement
  - Reduction percent, skip flag, confidence, status

#### Temperature Compensation
- Channel-local configuration (`temperature_compensation_config_t`):
  - Base temperature (default 20°C)
  - Sensitivity (default 0.05)
  - Min/max factors (default 0.5–2.0)
- Runtime helpers: `temp_compensation_calculate()`, `temp_comp_should_apply()`
- **Temperature compensation applies only to TIME and VOLUME modes**.
- FAO-56 modes already incorporate temperature in ET₀ via Penman-Monteith or Hargreaves-Samani.
- Results stored in `last_temp_compensation` for BLE telemetry.

#### Enhanced System Status
- `enhanced_system_status_info_t` aggregates:
  - Active interval phases
  - Compensation state
  - Sensor health
  - Configuration completeness
  - Channel bitmaps (active, interval, incomplete)
  - Timestamps

---

## Watering Modes and Execution Lifecycle

### By Duration (`WATERING_BY_DURATION`)
- Channel valve held open for configured minutes.
- Interval mode can split runtime into alternating watering/pause phases when `interval_config.configured` is true.
- Completion determined by elapsed time minus paused intervals.
- Flow monitoring active for anomaly detection only.

### By Volume (`WATERING_BY_VOLUME`)
- Firmware targets specific litres.
- Before opening valve, resets flow pulse counter.
- Completion: `pulses * 1000 / calibration` meets litre target (minimum 1 litre enforced).
- Interval mode supported; adjusts litres per phase accordingly.

### Automatic Quality (`WATERING_AUTOMATIC_QUALITY`)
- Channels in Quality mode eligible for automatic FAO-56 calculations.
- `watering_run_automatic_calculations()` snapshots existing event, converts computed requirement into one-off volume job (100% of FAO-56 result, minimum 1 litre), and queues it.

### Automatic Eco (`WATERING_AUTOMATIC_ECO`)
- Same process as Quality mode but delivers 70% of FAO-56 requirement.

### Interval Mode Integration
- `interval_task_should_use_interval()` validates interval profile and ensures watering mode is duration or volume based.
- `interval_controller_t` orchestrates:
  - Watering/pause phases
  - Cycle counting
  - Phase time calculation
  - Progress via enhanced BLE task characteristic
- Manual pause/resume/stop commands interact with controller and keep master valve in sync.
- State machine states: IDLE, WATERING, PAUSING, COMPLETED, ERROR.

### Rain-Aware Execution
- Before task starts, `rain_integration_calculate_impact()` recomputes skip flag and percentage reduction.
- **Rain Skip and Reduction apply ONLY to TIME and VOLUME modes**.
- FAO-56 modes skip this step (rain already in ET₀ calculations).
- Skipped tasks restore snapshots and log rain-skip history entry.
- Partial reductions adjust duration or volume but never below 1 minute or 1 litre.

### Task Completion
- On completion (success or error):
  - Log actual duration/volume
  - Raise BLE notifications
  - Restore original event configuration
  - Free queue slot
- Master-valve logic: close immediately or after configured delay (if another task pending within overlap window).

---

## Scheduling and Time Base

### RTC Anchoring
- Each scheduler pass obtains wall-clock time from DS3231 RTC and converts to local time via `timezone` helpers.
- If RTC fails five times in a row, system temporarily falls back to uptime estimates until RTC recovers.

### Day Tracking
- When day component changes:
  - `days_since_start` increments
  - Configuration saved to NVS
  - Periodic schedules test modulo condition

### Queue Policies
- Scheduled jobs enqueued only when:
  - Current local hour/minute matches `event.start_time`
  - Day mask or periodic interval condition satisfied
- Manual jobs bypass scheduler and go straight into queue if space allows.

### Power-Aware Sleeping
- After every scheduling pass, thread sleeps for 60/120/300 s depending on active power mode.

---

## Automatic Irrigation Calculation Pipeline

### 1. Trigger Cadence
- Scheduler verifies `auto_calc_enabled` is true.
- At least `auto_calc_interval_ms` (default 1 hour, configurable 1–24 hours) elapsed since last run.

### 2. Eligibility
- Channels in Quality or Eco mode without active task are candidates.

### 3. Environmental Inputs
- `env_sensors_read()` supplies latest measurements.
- If parameter invalid, pipeline substitutes safe defaults (20°C, 60% RH) or aborts channel when no reliable data available.

### 4. FAO-56 Computation
`fao56_calculate_irrigation_requirement()` performs:

#### Reference Evapotranspiration (ET₀)
- Retrieve cached ET₀ when same inputs processed recently.
- Otherwise compute Penman-Monteith using temperature, humidity, pressure, latitude, day-of-year.
- If humidity or pressure missing, fall back to Hargreaves-Samani (temperature-only) but mark result as fallback.

#### Crop Coefficient
- Combine plant database entries (stage lengths, Kc values) with `days_after_planting`.
- Values cached per channel to avoid recomputation in same session.
- Legacy plant-type data used only when database indices unset.

#### Soil Water Balance
- If plant/soil/method indices valid: `calc_water_balance()` computes:
  - Readily available water (RAW)
  - Total available water (TAW)
  - Current deficit
  - Net irrigation requirement
- Uses custom soil overrides where present.
- If dataset missing: degrades to simplified deficit model (ETc minus effective rainfall).

#### Effective Rainfall
- Hourly/daily rain history plus `rain_integration_calculate_effective_rainfall()` determine offset.
- Effective rain factor (default 0.8) and rain integration toggle influence this step.

#### Volume Conversion
- Convert mm irrigation into litres using:
  - Area coverage (`area_m2`), or
  - Implicit per-plant area (default 0.5 m²)
- Apply Eco reductions (70%) and enforce `max_volume_limit_l`.
- Track cycle-and-soak flag for scheduling strategies.
- Record `last_calculation_time` and update channel water balance caches.

### 5. Task Materialisation
- Controller snapshots existing event.
- Converts FAO-56 recommendation into temporary `WATERING_BY_VOLUME` task (minimum 1 litre).
- Enqueues task; if queue full, restores snapshot and logs error.

### 6. Compensation Behaviour by Mode
- **TIME/VOLUME modes**: Rain Skip, Rain Reduction, and Temperature Compensation applied (per-channel thresholds).
- **Quality/Eco modes (FAO-56)**: Compensation **NOT applied** – FAO-56 already incorporates rain and temperature in ET₀. Applying additional compensation would double-count.

### 7. Notifications
- When at least one channel schedules successfully, `bt_irrigation_auto_calc_status_notify()` informs connected BLE clients.

---

## Data Persistence and Storage Layout

### NVS Footprint
Stored via `nvs_config` helpers (each validates payload size before committing):
- Channel configuration, custom soil overlays
- Onboarding flags, schedule flags
- System flags: flow calibration, master valve, RTC, rain sensor, power mode, location, initial setup
- Automatic calculation state
- Timezone configuration
- Days since start

### External Flash + LittleFS (W25Q128)
When `CONFIG_HISTORY_EXTERNAL_FLASH=y`, a LittleFS volume is mounted on `database_partition` at `/lfs` and used for:
- Environmental + rain history ring files (`/lfs/history/*.bin`) via `history_flash`
- Optional binary plant/soil/irrigation databases (`/lfs/db/*.bin`) via `database_flash` (generated by `tools/csv2binary_flash.py`)

### Watering History
- **Detailed events** (30 per channel, 15 bytes each):
  - `dt_delta`, trigger type, success/error flags
  - Target ml/minutes, actual ml
  - Average flow rate, reserved bytes
- **Daily stats** (90 entries, 16 bytes):
  - Day epoch, total ml
  - Sessions ok/error, success rate
  - Busiest channel
- **Monthly stats** (36 entries, 12 bytes):
  - Year, month, total ml
  - Active days, peak channel
- **Annual stats** (10 entries, 20 bytes):
  - Year, total ml, total sessions, errors
  - Max/min month ml, peak channel
- Insights cache and rotation metadata persist alongside ring buffers.

### Rain History
- Stored in LittleFS via `history_flash` when `CONFIG_HISTORY_EXTERNAL_FLASH=y` (default); otherwise stored in NVS blobs.
- **Hourly records** (720 entries, 8 bytes):
  - Hour epoch, rainfall in 0.01 mm units
  - Raw pulse count, data quality (0-100)
- **Daily records** (1,825 entries, 12 bytes):
  - Day epoch, total rainfall
  - Peak hourly rainfall, hours with activity
  - Completeness percentage
- Total pulses and last pulse timestamps retained in NVS.

### Environmental History
- Stored in LittleFS via `history_flash` when `CONFIG_HISTORY_EXTERNAL_FLASH=y` (default); otherwise stored in NVS blobs.
- **Hourly entries** (720):
  - Timestamp, BME280 snapshot
  - Rainfall mm, watering events count
  - Total volume ml, active-channel bitmap
- **Daily entries** (372):
  - Min/max/avg temperature, humidity, pressure
  - Total rainfall, watering events, total volume
  - Sample count, active-channel bitmap
- **Monthly entries** (60):
  - YYYYMM, aggregated stats
  - Total rainfall, watering events, total volume
  - Active days

### Storage Health
- `nvs_storage_monitor`:
  - Mounts partition, calculates used/free bytes
  - Counts read and write errors
  - Schedules clean-up runs: returns usage to ~70% once 80/90% thresholds crossed
- External flash history: `history_flash_get_stats()` reports per-file counts and byte usage; LittleFS provides wear levelling and integrity checks.
- BLE characteristic exposes current health message and utilisation percentage.

### Onboarding/Reset Logs
- `onboarding_state` stores:
  - Channel config flags (8 channels × 8 bits each)
  - Extended channel flags (FAO-56 ready, rain comp, temp comp, latitude, volume limit, planting date, cycle-soak)
  - System flags, schedule bitmask
  - Timestamps, completion percentage
- `config_reset_log_t` (16-entry rings per channel):
  - Reset group, timestamp, channel, reason string
- Reset controller confirmation codes:
  - Requested scope, channel ID, code
  - Generation time, expiry (5 minutes)

---

## Bluetooth Low Energy Surfaces

### Notification Scheduler
- Pool of 8 × 23-byte buffers for asynchronous notifications.
- Priority throttles:
  - Critical: 0 ms
  - High: 50 ms
  - Normal: 200 ms
  - Low: 1 s
- Failed transmissions retry 3 times; buffer exhaustion waits 2 s.

### Characteristics (27 Documented)
| # | Name | Description |
|---|------|-------------|
| 01 | Valve Control | R/W/N - Valve operations for channels 0-7 and master |
| 02 | Flow Sensor | R/N - Flow rate data, pulse counts |
| 03 | System Status | R/N - System state, status codes |
| 04 | Channel Configuration | R/W/N - Channel settings, growing environment |
| 05 | Schedule Configuration | R/W/N - Event timing, schedule params |
| 06 | System Configuration | R/W/N - Power mode, master valve, flow calibration |
| 07 | Task Queue | R/W/N - Task management, queue depth |
| 08 | Statistics | R/W/N - Usage statistics per channel |
| 09 | RTC Configuration | R/W/N - Date/time synchronisation |
| 10 | Alarm Status | R/W/N - Alarm notifications |
| 11 | Calibration Management | R/W/N - Flow calibration updates |
| 12 | History Management | R/W/N - TLV-framed history streaming |
| 13 | Diagnostics | R/N - System diagnostics |
| 14 | Growing Environment | R/W/N - Plant/soil/method settings |
| 15 | Auto Calc Status | R/N - Automatic calculation results |
| 16 | Current Task Status | R/W/N - Real-time task monitoring |
| 17 | Timezone Configuration | R/W/N - UTC offset, DST rules |
| 18 | Rain Sensor Config | R/W - Calibration, thresholds |
| 19 | Rain Sensor Data | R/N - Live readings |
| 20 | Rain History Control | R/W/N - History streaming |
| 21 | Environmental Data | R/N - BME280 readings |
| 22 | Environmental History | R/N - Multi-resolution history |
| 23 | Compensation Status | R/N - Rain/temp compensation state |
| 24 | Onboarding Status | R/N - Configuration progress |
| 25 | Reset Control | R/W/N - Confirmation code workflow |
| 26 | Rain Integration Status | R/N - Full integration state |
| 27 | Channel Compensation Config | R/W/N - Per-channel compensation |

### History Streaming
- TLV-framed characteristics with fragment sequencing.
- Client acknowledgements required.
- Rain history caps each transfer at 20 fragments.

### Configuration Portals
Key characteristics accept writes for:
- Channel metadata and schedules
- Interval profiles
- Custom soil data
- Compensation flags
- Onboarding flags
- Reset codes
- Automatic calculation settings

Every handler validates arguments before updating RAM or NVS.

---

## Onboarding, Scoring, and Reset

### Onboarding Ledger
- Channel-level flags (per channel × 8 bits):
  - Plant type, soil type, irrigation method
  - Coverage, sun exposure, name
  - Water factor, enabled
- Extended flags:
  - FAO-56 ready, rain comp configured, temp comp configured
  - Latitude set, volume limit set, planting date set, cycle-soak enabled
- Schedule flags (8 bits, one per channel)
- System flags: timezone, flow calibrated, master valve, RTC, rain sensor, power mode, location, initial setup

### Configuration Scoring
- Weights per group:
  - Basic setup: 25%
  - Growing environment: 25%
  - Compensation: 20%
  - Custom soil: 15%
  - Interval profile: 15%
- Health levels: POOR (<40%), FAIR (40-59%), GOOD (60-79%), EXCELLENT (80%+)
- Completion percentage recomputed after every update.
- Onboarding complete when ≥1 channel sets all critical flags and RTC flag raised.

### Reset Mechanics
- Reset commands require 32-bit confirmation code.
- Generated by controller, valid for 5 minutes (`RESET_CONFIRMATION_VALIDITY_SEC`).
- Supported scopes:
  - Single-channel configuration
  - Single-channel schedule
  - All channels
  - All schedules
  - System configuration
  - Calibration
  - Histories
  - Full factory reset
- Channel-specific resets embed target channel ID in confirmation context.

---

## System Status, Recovery, and Diagnostics

### Status Taxonomy
Base status values:
- `OK`, `FAULT`, `NO_FLOW`, `UNEXPECTED_FLOW`, `RTC_ERROR`, `LOW_POWER`

Enhanced status module derives:
- Interval phase
- Compensation flags
- Sensor health indicators
- Configuration completeness
- Bitmaps of active, interval, and incomplete channels

### Recovery Strategies
`enhanced_error_handling` maps error codes to strategies:
- Retry
- Fallback
- Disable
- Reset
- Graceful degrade

With configurable retry ceilings and cool-down timers.

Dedicated recovery contexts exist for:
- BME280 initialisation
- Sensor read failures
- Compensation errors
- Interval controller failures
- Storage issues
- Sensor degradation detection

### Diagnostics Availability
Subsystems exposing detailed diagnostics and statistics:
- Flow sensor
- Rain sensor/integration
- Environmental sensors
- Storage monitor
- Task manager

BLE mirrors many diagnostics so field tools can pull them without serial access.

### Maintenance Hooks
Scheduled routines:
- **Daily**: History compaction, data validation
- **Weekly**: Fresh NVS snapshots
- **Monthly**: Extended cleanup

Automatic:
- Rain sensor and integration watchdogs attempt recovery
- Storage monitors trigger cleanups at thresholds

---

## Error Codes Reference

### Watering Error Codes (`watering_error_t`)
| Code | Name | Description |
|------|------|-------------|
| 0 | SUCCESS | Operation completed successfully |
| -1 | INVALID_PARAM | Invalid parameter provided |
| -2 | NOT_INITIALIZED | System not initialized |
| -3 | HARDWARE | Hardware failure |
| -4 | BUSY | System busy with another operation |
| -5 | QUEUE_FULL | Task queue is full |
| -6 | TIMEOUT | Operation timed out |
| -7 | CONFIG | Configuration error |
| -8 | RTC_FAILURE | RTC communication failure |
| -9 | STORAGE | Storage operation failed |
| -10 | DATA_CORRUPT | Data corruption detected |
| -11 | INVALID_DATA | Invalid data format |
| -12 | BUFFER_FULL | Buffer overflow |
| -13 | NO_MEMORY | Memory allocation failed |

### Reset Status Codes (`reset_status_t`)
| Code | Name | Description |
|------|------|-------------|
| 0 | SUCCESS | Reset completed successfully |
| 1 | INVALID_TYPE | Invalid reset type |
| 2 | INVALID_CHANNEL | Invalid channel ID |
| 3 | INVALID_CODE | Invalid confirmation code |
| 4 | CODE_EXPIRED | Confirmation code expired |
| 5 | STORAGE_ERROR | NVS storage error |
| 6 | SYSTEM_ERROR | General system error |

---

## Limitations and Behaviour Notes

### Compensation Engines
- Rain and temperature compensation APIs available for **TIME and VOLUME watering modes only**.
- **FAO-56 modes (Quality/Eco) do NOT use external compensation** because they already incorporate rain and temperature in ET₀ calculations.
- Applying compensation on top of FAO-56 would result in double-counting.

### Interval Mode
- Interval profiles only control execution cadence; they do not schedule jobs on their own.
- Users must still create duration or volume tasks.
- Only compatible with TIME and VOLUME modes.

### Master Valve Manual Mode
- When automatic master valve management disabled, clients must open and close master valve explicitly.
- Manual toggles rejected while auto-management enabled.

### History Rotation
- Watering history rotation is scaffolded (NVS-backed).
- Rain/environment history rotation is handled by fixed-size ring files when `CONFIG_HISTORY_EXTERNAL_FLASH=y`.
- Extremely full NVS partitions might require manual clean-up via reset commands.

### Hardware Requirements
- Tested targets: nRF52840 (promicro_nrf52840, arduino_nano_33_ble)
- Simulation: native_sim with emulated RTC, GPIO, and BLE (requires a host GCC toolchain)
- Storage: NVS for configuration + watering history; W25Q128 (16 MB) external SPI flash for LittleFS-backed rain/environment history when enabled

---

## File Reference

| Module | Primary Files | Purpose |
|--------|--------------|---------|
| Core | `watering.c/h`, `watering_tasks.c`, `watering_internal.h` | Task dispatch, scheduling, channel control |
| FAO-56 | `fao56_calc.c/h`, `fao56_custom_soil.c/h` | Scientific irrigation calculations |
| Flow | `flow_sensor.c/h` | Pulse counting, calibration, anomaly detection |
| Rain | `rain_sensor.c/h`, `rain_history.c/h`, `rain_integration.c/h`, `rain_compensation.c/h` | Rain sensing, history, compensation |
| Environment | `env_sensors.c/h`, `environmental_data.c/h`, `environmental_history.c/h`, `bme280_driver.c/h` | BME280, data aggregation |
| Temperature | `temperature_compensation.c/h`, `temperature_compensation_integration.c/h` | Temperature-based adjustments |
| Interval | `interval_mode_controller.c/h`, `interval_timing.c/h`, `interval_task_integration.c/h` | Phase cycling |
| BLE | `bt_irrigation_service.c/h`, `bt_*_handlers.c/h` | GATT service, characteristics |
| Storage (NVS) | `nvs_config.c/h`, `nvs_storage_monitor.c/h` | Persistence, health monitoring |
| Storage (LittleFS) | `database_flash.c/h`, `history_flash.c/h` | External flash LittleFS mount, flash-backed history + optional binary DB |
| Config | `configuration_status.c/h`, `onboarding_state.c/h`, `reset_controller.c/h` | Scoring, onboarding, reset |
| Status | `enhanced_system_status.c/h`, `enhanced_error_handling.c/h` | Diagnostics, recovery |
| Databases | `plant_full_db.c`, `soil_enhanced_db.c`, `irrigation_methods_db.c` | Generated from CSV |

---

This document reflects the behaviour of the firmware located in `c:\Users\tapir\Documents\Zephyr\AutoWatering`. Update it alongside code changes to keep integration and support teams aligned with the shipped feature set.
