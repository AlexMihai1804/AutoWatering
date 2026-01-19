# AutoWatering Capability Compendium (January 2026 build)

Last updated: 2026-01-18

This document inventories every capability exposed by the current firmware. It is aimed at integrators who need to understand data paths, feature boundaries, and system behaviour without reading the source directly.

---

## Quick Reference

| Metric              | Value                                           |
| ------------------- | ----------------------------------------------- |
| Zephyr RTOS         | v4.3.0                                          |
| Target Board        | Arduino Nano 33 BLE (nRF52840)                  |
| Channels            | 8 (0-7)                                         |
| BLE Characteristics | 38 (29 primary + 5 custom + 4 pack)             |
| Plant Database      | 223 species, 44-byte structs, 10 categories     |
| Soil Database       | 15 enhanced types with hydraulic properties     |
| Irrigation Methods  | 15 types with efficiency parameters             |
| Watering Modes      | 4 (TIME, VOLUME, QUALITY, ECO)                  |
| Schedule Types      | 3 (DAILY, PERIODIC, AUTO)                       |
| Error Codes         | 15 (WATERING_SUCCESS=0, errors -1 to -14)       |
| Phenological Stages | 4 (INITIAL, DEVELOPMENT, MID_SEASON, END)       |
| NVS History         | 30 events per channel (circular)                |
| External Flash      | W25Q128 16MB (LittleFS)                         |
| RTC                 | DS3231 with timezone support                    |

---

## Future (Roadmap)

Planned and in-progress changes are tracked in the GitHub roadmap project:

- <https://github.com/users/AlexMihai1804/projects/2>

Key upcoming firmware work (by batch):

- **B0 Core correctness fixes (FAO/ET0)**: [#1](https://github.com/AlexMihai1804/AutoWatering/issues/1), [#2](https://github.com/AlexMihai1804/AutoWatering/issues/2), [#3](https://github.com/AlexMihai1804/AutoWatering/issues/3), [#4](https://github.com/AlexMihai1804/AutoWatering/issues/4), [#5](https://github.com/AlexMihai1804/AutoWatering/issues/5)
- **B1-B2 Packs on external flash (LittleFS + BLE install/list + built-in DB pack)**: [#10](https://github.com/AlexMihai1804/AutoWatering/issues/10), [#6](https://github.com/AlexMihai1804/AutoWatering/issues/6), [#7](https://github.com/AlexMihai1804/AutoWatering/issues/7), [#8](https://github.com/AlexMihai1804/AutoWatering/issues/8), [#12](https://github.com/AlexMihai1804/AutoWatering/issues/12)
- **B4 Updates (atomic replace + rollback)**: [#13](https://github.com/AlexMihai1804/AutoWatering/issues/13)
- **B5 Custom plants integration (channel references custom plant_id)**: [#9](https://github.com/AlexMihai1804/AutoWatering/issues/9)
- **B7 Cycle & soak support (app auto-tuning dependency; firmware interval mode + BLE config already present)**: [#14](https://github.com/AlexMihai1804/AutoWatering/issues/14)

---

## Core Control Model

### Channel Matrix

- Eight irrigation channels (`WATERING_CHANNELS_COUNT`) are compiled into `watering_channels[]`
- Each channel owns its GPIO descriptor, coverage definition, watering schedule, compensation settings, and persistent runtime state
- Channel state includes: last watering epoch, days after planting, cached FAO-56 metadata, master-valve mask, interval configuration, and configuration scoring

### System State Machine (`watering_system_state_t`)

| State                          | Description                                      |
| ------------------------------ | ------------------------------------------------ |
| `WATERING_STATE_IDLE`          | No active watering, system ready                 |
| `WATERING_STATE_WATERING`      | Zone valve open, water flowing                   |
| `WATERING_STATE_PAUSED`        | Temporarily halted (user pause or soak phase)    |
| `WATERING_STATE_ERROR_RECOVERY`| Attempting recovery after anomaly detection      |

### Mutual Exclusion

- `MAX_SIMULTANEOUS_VALVES` is fixed at 1 so only one zone valve is energised at any time
- The master valve layer (`master_valve_config_t`) wraps every operation with:
  - Pre-start delay: default +3 s
  - Post-stop delay: default +2 s
  - Overlap grace window: 5 s (keeps master open between consecutive tasks)

### Task Pipeline

- All watering work goes through the Zephyr message queue `watering_tasks_queue` (depth 10)
- The scheduler thread refuses to push additional scheduled jobs when two or more tasks are already waiting, leaving space for manual or remote overrides
- `watering_process_next_task()` promotes queued jobs to the execution thread, which tracks the active task in `watering_task_state`

### Threading and Pacing

- `watering_start_tasks()` brings up two 4 kB threads:
  - Processing loop (`watering_task_fn`, published as `watering_task`)
  - Scheduler loop (`scheduler_task_fn`, published as `scheduler_task`)
- Sleep intervals keyed off `current_power_mode`:
  - Processing loop: 0.5 s (normal), 2 s (energy saving), 600 s (ultra low)
  - Scheduler loop: 60 s (normal), 120 s (energy saving), 300 s (ultra low)
- Zephyr power management is disabled (`CONFIG_PM=n`), so these are thread pacing delays only

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
  - `use_solar_timing`, `solar_event`, `solar_offset_minutes` (-120..+120): sunrise/sunset scheduling with fallback to `start_time`
- AUTO schedule eligibility is gated by `watering_channel_auto_mode_valid()` (plant_id, soil_db_index, planting_date_unix, and coverage).
- FAO-56 calculations also require a valid `irrigation_method_index` even if the AUTO gate passes.
- Solar timing uses latitude/longitude; longitude is stored per channel but not exposed via BLE (defaults to 0 unless set elsewhere).
- Schedules stored per channel in NVS and exposed via BLE Channel Configuration characteristic.

### Coverage and Agronomic Metadata

- Channels reference databases via the unified plant system:
  - `plant_id`: 0 = unset, 1-223 = built-in (pack 0), >=1000 = custom (pack storage)
  - `soil_db_index`: 0..14 (`UINT8_MAX` = unset) - 15 enhanced soil types
  - `irrigation_method_index`: 0..14 (`UINT8_MAX` = unset) - 15 irrigation methods

**Database Details**:

| Database           | Count | Struct Size | Source File                    |
| ------------------ | ----- | ----------- | ------------------------------ |
| Plants             | 223   | 44 bytes    | `plant_full_db.inc`            |
| Soils              | 15    | variable    | `soil_enhanced_db.inc`         |
| Irrigation Methods | 15    | variable    | `irrigation_methods_db.inc`    |

Built-in plants are provisioned into pack storage on external flash at first boot. Runtime plant lookups use `pack_storage` (pack_plant_v1, 156 bytes) at `/lfs_ext`, and custom plants use IDs >= 1000.

**Plant Categories** (10) - from `plant_full_db.inc`:

0. AGRICULTURE — field crops
1. GARDENING — general garden plants
2. LANDSCAPING — landscape design plants
3. INDOOR — houseplants
4. SUCCULENT — cacti, succulents
5. FRUIT — fruit trees and bushes
6. VEGETABLE — vegetables (42+ types)
7. HERB — culinary and medicinal herbs (50+ types)
8. LAWN — turf grasses
9. SHRUB — decorative shrubs

**Soil Types** (15) — from `soil_enhanced_db.inc`:
Sand, Loamy Sand, Sandy Loam, Loam, Silt Loam, Sandy Clay Loam, Clay Loam, Silty Clay Loam, Sandy Clay, Silty Clay, Clay, Peat/Organic, Gravelly Loam, Potting Mix, Hydroponic

**Irrigation Methods** (15) — from `irrigation_methods_db.inc`:
Surface Flood, Surface Border, Surface Furrow, Sprinkler Set, Sprinkler Pivot, Sprinkler LEPA, Micro Spray, Drip Surface, Drip Subsurface, Drip Tape, Bubbler, Subirrigation, Wick Bed, Hydroponic Recirculating, Aeroponic

- Coverage selects between area (`area_m2`) and plant count (`plant_count`)
- Legacy fields (`plant_type`, `soil_type`, `irrigation_method`, `coverage_legacy`, `plant_info`, `custom_plant`) remain for backward compatibility

### Automatic Watering Parameters

- `auto_mode`: stored as `watering_mode_t` in the channel (Quality/Eco exist in firmware).
  - BLE growing environment writes accept auto_mode 0..2 and cast to `watering_mode_t`: 0=Duration (manual), 1=Volume (manual), 2=Automatic Quality (100%).
  - Eco (3) exists in firmware but is not settable via BLE.
- `max_volume_limit_l`: caps any single automatic job.
- `enable_cycle_soak`: stores a client flag; current automatic scheduling still enqueues a single volume task (cycle/soak not applied).
- Plant lifecycle tracking:
  - `planting_date_unix`: Unix timestamp when plants were established
  - `days_after_planting`: calculated field for FAO-56 crop coefficient staging
- `latitude_deg`, `longitude_deg`, and `sun_exposure_pct`: location + exposure inputs for solar timing and ET0 calculations (longitude not exposed via BLE).

### Compensation Settings

- Per-channel rain compensation:
  - `enabled`, `sensitivity` (0.0-1.0), `lookback_hours` (1-72 via BLE, 1-168 internal), `skip_threshold_mm` (0-100), `reduction_factor` (0.0-1.0).
  - Task adjustments are applied by `rain_integration` (exponential reduction curve).
  - Global rain integration settings from Rain Sensor Config are deprecated (per-channel only).
- Per-channel temperature compensation:
  - `enabled`, `base_temperature` (-40..60 C via BLE), `sensitivity` (0.1-2.0).
  - `min_factor` (0.5-1.0), `max_factor` (1.0-2.0).
  - Global defaults from System Config are clamped to -10..50 C and 0.01..0.20 sensitivity.
  - Configuration is persisted, but current task execution does not apply temperature compensation.
- Snapshots of last applied adjustments (may stay at defaults unless applied):
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
  - Name (32 chars), field capacity 5-80%, wilting point 1-40% (wilting < field capacity)
  - Infiltration rate 0.1-1000 mm/h, bulk density 0.5-2.5 g/cm3, organic matter 0-100%
- Custom values override database defaults inside FAO-56 water balance routines.

### Antecedent Soil Moisture (Manual)

- Global soil moisture estimate (`enabled`, `moisture_pct` 0-100) stored in NVS; defaults to 50% when disabled.
- Optional per-channel override (`override_enabled`, `moisture_pct` 0-100) stored in NVS.
- Used by FAO-56 effective rainfall/runoff calculations (no physical soil probes required).

### Configuration Scoring

- `config_status` tracks completion flags for:
  - Basic setup (25% weight) — channel enabled, mode selected
  - Growing environment (25% weight) — plant/soil/method assigned
  - Compensation (20% weight) — rain integration configured
  - Custom soil (15% weight) — custom soil parameters defined
  - Interval profile (15% weight) — cycle/soak timing configured
- `configuration_score`: 0-100 powering onboarding feedback
- `last_reset_timestamp`: records when maintenance cleared the configuration group

**Score Calculation**:

```text
score = (basic_complete × 25) + (growing_complete × 25) +
        (compensation_complete × 20) + (custom_soil_complete × 15) +
        (interval_complete × 15)
```

### Global Toggles

- `auto_calc_enabled`, `auto_calc_interval_ms` (default 1 hour; interval is internal and not exposed via BLE)
- `current_power_mode`, `system_initialized`, `system_status`
- Flow calibration: stored under NVS key 1000, accepted range 100-10,000 pulses/liter

### Database Inventories

- Plant species: 223 entries (`PLANT_FULL_SPECIES_COUNT`) — 44-byte packed structs
- Enhanced soils: 15 types (`SOIL_ENHANCED_TYPES_COUNT`)
- Irrigation methods: 15 entries (`IRRIGATION_METHODS_COUNT`)
- Conversion scripts in `tools/` regenerate these tables from CSV
- Optional external-flash binary format: `tools/csv2binary_flash.py` + `database_flash` (mounted at `/lfs/db/*.bin`)

---

## Sensor Stack and Telemetry

### Inventory

The production hardware assumes three physical sensors:

1. Pulse-based flow meter on the irrigation supply
2. Tipping-bucket rain gauge
3. BME280 environmental sensor (temperature, humidity, pressure)

No physical soil moisture probes are present; antecedent moisture comes from the manual global/per-channel estimates stored in NVS.

### Flow Sensor (Pulse Meter)

- **Hardware interface**: Devicetree node `water_flow_sensor` supplies port, pin, and active level; debounce configurable via `sensor_config.debounce_ms` (2 ms in the board overlay, fallback 5 ms if DT missing).
- **Data products**:
  - Atomic pulse count (`get_pulse_count()`) used internally for volume tasks and diagnostics (not exposed via BLE)
  - Smoothed pulses-per-second rate (2-sample buffer, recalculated on a 2 s window when >=1 pulse arrives; falls back to 0 on idle)
  - BLE Flow Sensor characteristic read/notify returns smoothed pps only (4 bytes); notifications triggered on +10 pulses or a 5 s heartbeat (whichever comes first)
  - ISR schedules work item every 5 pulses or 3 s (whichever comes first)
- **Configuration**: Calibration persisted under NVS key 1000, settable via `set_flow_calibration()` or Calibration Management characteristic. Accepted range: 100-10,000 pulses/liter. Defaults come from devicetree (`flow-calibration`, 750 in overlay), or 450 if DT is missing; NVS overrides when present. Reset forces `DEFAULT_PULSES_PER_LITER` (750).
- **Safety & diagnostics**: `check_flow_anomalies()` runs ~1/sec. Raises:
  - `WATERING_STATUS_NO_FLOW` when a valve is open but pulses do not appear or stall; performs up to 3 open/close recovery toggles, then stops the zone and applies channel lock (soft with auto-retry, hard on persistence)
  - `WATERING_STATUS_UNEXPECTED_FLOW` when pulses persist with all valves closed (debounced window)

### Hydraulic Sentinel (H.H.M.)

Continuous flow monitoring system with auto-learning capability.

**Timing Constants** (from `watering_monitor.c`):

| Parameter                            | Value                                   |
| ------------------------------------ | --------------------------------------- |
| `NO_FLOW_STALL_TIMEOUT_MS`           | 3000 ms (3s without pulses = error)     |
| `NO_FLOW_RETRY_COOLDOWN_MS`          | 5000 ms (cooldown before retry)         |
| `FLOW_STARTUP_GRACE_MS`              | 8000 ms (grace period after task start) |
| `HYDRAULIC_RING_SECONDS`             | 60 s (rolling history buffer)           |
| `HYDRAULIC_LEARNING_MIN_RUNS`        | 2 runs (minimum for auto-learn)         |
| `HYDRAULIC_LEARNING_MAX_RUNS`        | 4 runs (standard learning cap)          |
| `HYDRAULIC_LEARNING_MAX_RUNS_EXT`    | 6 runs (extended learning)              |
| `HYDRAULIC_STABLE_WINDOW_S`          | 3 s (stability detection window)        |
| `HYDRAULIC_STABLE_VARIATION_PCT`     | 5% (max variation for stable)           |
| `HYDRAULIC_MEASURE_WINDOW_S`         | 30 s (measurement capture window)       |
| `HYDRAULIC_LEARNING_TIMEOUT_S`       | 60 s (learning phase timeout)           |
| `HYDRAULIC_UNEXPECTED_FLOW_WINDOW_S` | 30 s (debounce window)                  |
| `HYDRAULIC_UNEXPECTED_FLOW_PULSES`   | 10 (threshold pulses)                   |
| `HYDRAULIC_POST_CLOSE_IGNORE_MS`     | 2000 ms (ignore after valve close)      |
| `HYDRAULIC_HIGH_FLOW_HOLD_S`         | 5 s (high flow confirmation)            |
| `HYDRAULIC_LOW_FLOW_HOLD_S`          | 30 s (low flow warning threshold)       |
| `HYDRAULIC_ABS_HIGH_FLOW_ML_MIN`     | 20000 mL/min (absolute high flow)       |
| `HYDRAULIC_MIN_NO_FLOW_ML_MIN`       | 200 mL/min (minimum detectable flow)    |

**Learning Phases** (`hydraulic_learning_phase_t`):

- `LEARNING_IDLE` — Not in learning mode
- `LEARNING_WAIT_STABLE` — Waiting for flow to stabilize (3s window, <5% variation)
- `LEARNING_MEASURE` — Capturing 30s of flow data for baseline

**Profiles** (`hydraulic_profile_t`):

- `HYDRAULIC_PROFILE_AUTO` — Auto-detected from irrigation method
- `HYDRAULIC_PROFILE_SPRAY` — High-flow sprinkler systems (Fast)
- `HYDRAULIC_PROFILE_DRIP` — Low-flow drip systems (Slow)

**Profile Timing (Start Ignore Windows)**:

- Fast (Spray): `clamp(ramp+5s, 8-20s)`
- Slow (Drip): `clamp(ramp+15s, 30-90s)`
- Default: `clamp(ramp+8s, 12-25s)`

**Anomaly Detection**:

- **HIGH FLOW**: `avg_5s` exceeds limit for 5s -> close all valves; channel lock if flow stops, global lock if persists
- **NO FLOW**: Stall detected (3s no pulses) + 3 toggle retries -> mark NO_FLOW, stop zone, soft lock; hard lock after 3 consecutive failures
- **LOW FLOW**: Warning only after 30s below limit; watering continues
- **UNEXPECTED FLOW**: >10 pulses in 30s for 30s with 2s post-close ignore -> global hard lock if persistent

**Nightly Static Test** (03:00):

- Master valve on 10s, off 5s, monitor 60s
- Skipped if: watering active, tasks queued, or global lock active
- Mainline leak detection: >3 pulses during the static test -> global hard lock + alarms

**Lock States** (`hydraulic_lock_state_t`):

- `HYDRAULIC_LOCK_NONE` - Normal operation
- `HYDRAULIC_LOCK_SOFT` - Temporary lock with auto-retry
- `HYDRAULIC_LOCK_HARD` - Permanent lock requiring manual reset
- Soft locks auto-clear after `HYDRAULIC_SOFT_LOCK_RETRY_SEC` (6h); no-flow soft locks use `HYDRAULIC_NO_FLOW_RETRY_COOLDOWN_SEC` (900s) for retry.
- Manual override: direct commands can set a 10-minute override to bypass hydraulic locks; Hydraulic Status reports `manual_override_active`.

**Storage**: Append-only ring log at `/lfs/history/hydraulic_events.bin` (max 4KB)

**BLE**: Hydraulic Status characteristic exposes per-channel profile/flow/tolerances/locks + global state

### Rain Sensor (Tipping Bucket)

- **Hardware interface**: `rain_sensor_config_t.debounce_ms` (default 50 ms; valid 10-1000 ms).
- **Configuration** (`rain_nvs_config_t`):
  - `mm_per_pulse` (default 0.2 mm; valid 0.1-10.0)
  - `sensor_enabled`, `integration_enabled`
  - `rain_sensitivity_pct` (0-100), `skip_threshold_mm` (0-100)
  - `last_reset_time`
- **Data products**:
  - Hourly rain history is flushed on UTC hour rollover via `rain_sensor_update_hourly()` into `rain_history` (LittleFS ring when external flash enabled).
  - Daily summaries are derived from hourly history (maintenance-driven aggregation).
  - BLE characteristics: 18-rain-sensor-config, 19-rain-sensor-data, 20-rain-history-control
  - Persistent counters (`rain_nvs_state_t`): total pulses, last pulse time, current hour/day totals
- **Health & diagnostics**:
  - Pulse statistics (mean/stddev of interval timing), outlier suppression, accuracy percentage, consecutive error counts
  - Watchdog (`check_rain_sensor_health()`) runs every 5 minutes: reinitialises sensor, history, and integration modules on failure; validates NVS payloads
- **Integration controls** (`rain_integration_config_t`):
  - `rain_sensitivity_pct`, `skip_threshold_mm`, `effective_rain_factor`, `lookback_hours`, `integration_enabled`
  - Global rain integration config is deprecated; per-channel compensation settings drive task adjustments.

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
  - System config also exposes BME280 enable flag and measurement interval (seconds, must be > 0)
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
- Note: the algorithm engine exists in code but current task adjustments use `rain_integration` instead.

#### Temperature Compensation

- Channel-local configuration (`temperature_compensation_config_t`):
  - Base temperature (default 20 C)
  - Sensitivity (default 0.05)
  - Min/max factors (default 0.5-2.0)
- Runtime helpers: `temp_compensation_calculate()`, `temp_comp_should_apply()`
- Configuration and telemetry are present, but task execution does not currently apply temperature compensation.
- FAO-56 modes already incorporate temperature in ET0 via Penman-Monteith or Hargreaves-Samani.
- Results remain at defaults unless a future integration path applies them.

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

### Mode Enumeration (`watering_mode_t`)

| Value | Mode                          | Description                                      |
| ----- | ----------------------------- | ------------------------------------------------ |
| 0     | `WATERING_BY_DURATION`        | Channel valve held open for configured minutes   |
| 1     | `WATERING_BY_VOLUME`          | Firmware targets specific litres via flow sensor |
| 2     | `WATERING_AUTOMATIC_QUALITY`  | FAO-56 calculation at 100% requirement           |
| 3     | `WATERING_AUTOMATIC_ECO`      | FAO-56 calculation at 70% requirement            |

Note: BLE growing environment writes accept auto_mode 0..2 and cast to `watering_mode_t` (0=Duration, 1=Volume, 2=Automatic Quality); Eco (3) is not settable via BLE.

### By Duration (`WATERING_BY_DURATION`)

- Channel valve held open for configured minutes
- Interval mode can split runtime into alternating watering/pause phases when `interval_config.configured` is true
- Completion determined by elapsed time minus paused intervals
- Flow monitoring active for anomaly detection only

### By Volume (`WATERING_BY_VOLUME`)

- Firmware targets specific litres
- Before opening valve, resets flow pulse counter
- Completion: `pulses * 1000 / calibration` meets litre target (minimum 1 litre enforced)
- Interval mode supported; adjusts litres per phase accordingly

### Automatic Quality (`WATERING_AUTOMATIC_QUALITY`)

- Channels in Quality mode eligible for automatic FAO-56 calculations
- `watering_run_automatic_calculations()` snapshots existing event, converts computed requirement into one-off volume job (100% of FAO-56 result, minimum 1 litre), and queues it

### Automatic Eco (`WATERING_AUTOMATIC_ECO`)

- Same process as Quality mode but delivers 70% of FAO-56 requirement

### Interval Mode Integration

- `interval_task_should_use_interval()` validates interval profile and ensures watering mode is duration or volume based
- `interval_controller_t` orchestrates:
  - Watering/pause phases
  - Cycle counting
  - Phase time calculation
  - Progress via enhanced BLE task characteristic
- Manual pause/resume/stop commands interact with controller and keep master valve in sync

**State Machine** (`interval_mode_state_t`):

| State                        | Description                                         |
| ---------------------------- | --------------------------------------------------- |
| `INTERVAL_STATE_IDLE`        | No active interval watering session                 |
| `INTERVAL_STATE_WATERING`    | Currently in a watering phase                       |
| `INTERVAL_STATE_PAUSING`     | Currently in a soak/pause phase                     |
| `INTERVAL_STATE_COMPLETED`   | All cycles completed successfully                   |
| `INTERVAL_STATE_ERROR`       | Error occurred during interval execution            |

### Rain-Aware Execution

- Before task starts, `rain_integration_calculate_impact()` recomputes skip flag and percentage reduction
- **Rain Skip and Reduction apply ONLY to TIME and VOLUME modes**
- FAO-56 modes skip this step (rain already in ET0 calculations).
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
- At least `auto_calc_interval_ms` (default 1 hour; adjustable via internal API, not exposed via BLE) elapsed since last run.

### 2. Eligibility

- Channels with `watering_event.watering_mode` set to Quality or Eco and no active task are candidates.
- `fao56_calculate_irrigation_requirement()` prefers `auto_mode` when it is Quality/Eco and falls back to `watering_event.watering_mode` otherwise.

### 3. Environmental Inputs

- `env_sensors_read()` supplies latest measurements; in AUTO decision paths, failure seeds defaults (mean 25 C, min 18 C, max 32 C, RH 50%) and uses rain history for rainfall.
- `fao56_handle_sensor_failure()` applies conservative fallbacks for invalid sensors: min 15 C, max 25 C, mean 20 C, RH 60%, pressure 1013.25 hPa, rain 0.
- FAO-56 realtime updates apply 25/18/32 C if `temp_valid` is false; missing humidity/pressure triggers Hargreaves-Samani.

### 4. FAO-56 Computation Engine

`fao56_calculate_irrigation_requirement()` performs a multi-stage scientific calculation pipeline.
Missing plant, soil, or irrigation method data returns `WATERING_ERROR_INVALID_DATA`.

#### Caching System (from `fao56_calc.h`)

The engine uses three cache structures to avoid redundant calculations:

| Cache                    | Structure                    | Purpose                                          |
| ------------------------ | ---------------------------- | ------------------------------------------------ |
| ET0 Cache                | `et0_cache_entry_t`          | Stores recent ET0 results by input hash          |
| Crop Coefficient Cache   | `crop_coeff_cache_entry_t`   | Per-channel Kc values by phenological stage      |
| Water Balance Cache      | `water_balance_cache_entry_t`| Per-channel deficit/TAW/RAW tracking             |

#### Reference Evapotranspiration (ET0)

- **Primary**: Penman-Monteith equation using temperature, humidity, pressure, latitude, day-of-year
- **Fallback**: Hargreaves-Samani (temperature-only) when humidity/pressure missing; result marked as fallback quality
- Cached results reused when same inputs processed recently

#### Phenological Stages (`phenological_stage_t`)

Crop development stages for Kc interpolation:

- `PHENO_STAGE_INITIAL` — Germination to 10% ground cover
- `PHENO_STAGE_DEVELOPMENT` — 10% to 70-80% ground cover
- `PHENO_STAGE_MID_SEASON` — Full development to start of maturity
- `PHENO_STAGE_END_SEASON` — Maturity to harvest

Stage transitions driven by `days_after_planting` against plant database stage length fields.

#### Crop Coefficient (Kc)

- Combines plant database entries (stage lengths, Kc values) with `days_after_planting`
- Linear interpolation between stages
- Per-channel caching to avoid recomputation in same session
- Legacy plant-type data used only when database indices unset

#### Soil Water Balance

When plant/soil/method indices valid, `calc_water_balance()` computes:

- **RAW** — Readily Available Water (soil moisture plants can easily extract)
- **TAW** — Total Available Water (field capacity minus wilting point × root depth)
- **Current Deficit** — How much water the root zone is missing
- **Net Irrigation Requirement** — Volume needed to return to optimal moisture

Custom soil overrides applied where present. If dataset missing: degrades to simplified deficit model (ETc − effective rainfall).

#### Effective Rainfall

- Hourly/daily rain history processed via `rain_integration_calculate_effective_rainfall()`
- Effective rain factor (default 0.8) reduces gross rainfall to account for runoff/evaporation
- Rain integration toggle enables/disables per-channel

#### Volume Conversion

- Convert mm irrigation into litres using:
  - Area coverage (`area_m2`), or
  - Implicit per-plant area (default 0.5 m²)
- Apply Eco reductions (70%) and enforce `max_volume_limit_l`
- Track cycle-and-soak flag for scheduling strategies
- Record `last_calculation_time` and update channel water balance caches

### 5. Task Materialisation

- Controller snapshots existing event.
- Converts FAO-56 recommendation into temporary `WATERING_BY_VOLUME` task (minimum 1 litre).
- Enqueues task; if queue full, restores snapshot and logs error.

### 6. Compensation Behaviour by Mode

- **TIME/VOLUME modes**: Rain skip/reduction applied (per-channel thresholds). Temperature compensation is configurable but not applied in the current execution path.
- **Quality/Eco modes (FAO-56)**: External compensation **NOT applied** - FAO-56 already incorporates rain and temperature in ET0. Applying additional compensation would double-count.

### 7. Notifications

- When at least one channel schedules successfully, `bt_irrigation_auto_calc_status_notify()` informs connected BLE clients.

---

## Data Persistence and Storage Layout

### NVS Footprint

Stored via `nvs_config` helpers (each validates payload size before committing):

- Channel configuration, custom soil overlays
- Onboarding flags, schedule flags
- System flags: flow calibration, master valve, RTC, rain sensor, power mode, location, initial setup
- Soil moisture estimates (global + per-channel overrides)
- Automatic calculation state
- Timezone configuration
- Days since start

### External Flash + LittleFS (W25Q128)

When `CONFIG_HISTORY_EXTERNAL_FLASH=y`, a LittleFS volume is mounted on `database_partition` at `/lfs` and used for:

- Environmental + rain history ring files (`/lfs/history/*.bin`) via `history_flash`
- Optional binary plant/soil/irrigation databases (`/lfs/db/*.bin`) via `database_flash` (generated by `tools/csv2binary_flash.py`)

### Watering History (NVS Ring Buffers)

Stored in internal NVS flash with circular rotation per channel.

**Record Types** (from `watering_history.h`):

| Record Type     | Entries  | Size    | Content                                              |
| --------------- | -------- | ------- | ---------------------------------------------------- |
| Detailed Events | 30/ch    | 15 B    | dt_delta, trigger, success, target/actual ml, flow   |
| Daily Stats     | 90       | 16 B    | Day epoch, total ml, sessions ok/error, success rate |
| Monthly Stats   | 36       | 12 B    | Year, month, total ml, active days, peak channel     |
| Annual Stats    | 10       | 20 B    | Year, total ml, sessions, errors, max/min month      |

**Storage Constants** (from `watering_history.h`):

- `DETAILED_EVENTS_PER_CHANNEL`: 30
- `DAILY_STATS_DAYS`: 90
- `MONTHLY_STATS_MONTHS`: 36
- `ANNUAL_STATS_YEARS`: 10
- `MAX_CHANNELS`: 8
- `TOTAL_HISTORY_STORAGE_KB`: 144
- `GC_HIGH_WATERMARK_PCT`: 90%
- `GC_LOW_WATERMARK_PCT`: 70%

**Detailed Event Structure** (`history_event_t`, 15 bytes):

- `dt_delta`: Time delta from previous event (compacted)
- `trigger_type`: Manual, scheduled, automatic, or remote
- `success_status`: Complete, partial, or failed
- `target_ml`, `actual_ml`: Requested vs delivered volume
- `avg_flow_rate`: Average mL/min during event

**Insights Cache** (`insights_t`, 33 bytes):

- Weekly volume per channel (8 × 4 bytes)
- Leak indicator per channel (8 bytes)
- Overall efficiency percentage (1 byte)

### Rain History (LittleFS Ring Files)

Stored in external flash via `history_flash` when `CONFIG_HISTORY_EXTERNAL_FLASH=y` (default); otherwise NVS blobs.

| Record Type    | Entries | Size  | Content                                              |
| -------------- | ------- | ----- | ---------------------------------------------------- |
| Hourly Records | 720     | 8 B   | Hour epoch, rainfall 0.01mm, pulses, quality         |
| Daily Records  | 1,825   | 12 B  | Day epoch, total, peak hourly, active hours, %       |

- Hourly entries appended on UTC hour rollover via `rain_sensor_update_hourly()`
- Total pulses and last pulse timestamps retained in NVS

### Environmental History (LittleFS Ring Files)

Stored in external flash via `history_flash` when `CONFIG_HISTORY_EXTERNAL_FLASH=y`.

| Record Type | Entries | Content                                                                              |
| ----------- | ------- | ------------------------------------------------------------------------------------ |
| Hourly      | 720     | Timestamp, BME280 snapshot, rainfall, events, volume                                 |
| Daily       | 372     | Min/max/avg temp/humidity/pressure, totals, sample count, active-channel bitmap      |
| Monthly     | 60      | YYYYMM, aggregated stats, total rainfall, watering events, total volume, active days |

Aggregation is scheduler-driven via `env_history_auto_aggregate()` (runs each scheduler tick, emits the last completed hour and rolls up daily/monthly on boundary changes).

### Storage Health

- `nvs_storage_monitor`:
  - Mounts partition, calculates used/free bytes
  - Counts read and write errors
  - Schedules clean-up runs: returns usage to ~70% once 80/90% thresholds crossed
- External flash history: `history_flash_get_stats()` reports per-file counts and byte usage; LittleFS provides wear levelling and integrity checks.
- BLE visibility: Rain Integration Status includes history storage usage bytes; Pack Stats exposes pack storage usage and counts.

### Onboarding/Reset Logs

- `onboarding_state` stores:
  - Channel config flags (8 channels x 8 bits each)
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

### Performance Optimizations (v3.1.0)

- **PHY 2M**: 2 Mbps physical layer for doubled throughput on compatible devices
- **Data Length Extension (DLE)**: 251-byte payloads (vs default 27 bytes)
- **Binary Plant Search**: O(log n) lookup in 223-species database
- **Fragment Streaming**: Chunked notifications with progress tracking
- **MTU Negotiation**: Automatic optimal packet sizing

### Notification Scheduler

- Pool of 8 buffers for asynchronous notifications (payloads up to 250 bytes, capped by MTU; larger data uses manual fragmentation paths)
- Priority throttles:
  - Critical: 0 ms
  - High: 50 ms
  - Normal: 200 ms
  - Low: 1 s
- Fragmented environmental/history notifications retry with backoff (up to 5 attempts); buffer exhaustion waits 2 s before retrying

### Notification Categories

| Category               | Priority | Throttle | Use Case                              |
| ---------------------- | -------- | -------- | ------------------------------------- |
| `SMART_NOTIFY`         | Normal   | 200 ms   | Standard state updates                |
| `CRITICAL_NOTIFY`      | Critical | 0 ms     | Errors, alarms, lock events           |
| `CHANNEL_CONFIG_NOTIFY`| Low      | 1 s      | Configuration change confirmations    |

### Primary Irrigation Service Characteristics (29)

UUID: `12345678-1234-5678-1234-56789abcdef0`

The primary irrigation service exposes 29 characteristics; the custom configuration service adds 5 and the pack service adds 4 (38 total across services).

| #  | Name                        | Description                                              |
| -- | --------------------------- | -------------------------------------------------------- |
| 01 | Valve Control               | R/W/N - Valve operations for channels 0-7 and master     |
| 02 | Flow Sensor                 | R/N - Flow rate (pps)                                    |
| 03 | System Status               | R/N - System state, status codes                         |
| 04 | Channel Configuration       | R/W/N - Channel settings, growing environment            |
| 05 | Schedule Configuration      | R/W/N - Event timing, schedule params                    |
| 06 | System Configuration        | R/W/N - Power mode, master valve, flow calibration       |
| 07 | Task Queue                  | R/W/N - Task management, queue depth                     |
| 08 | Statistics                  | R/W/N - Usage statistics per channel                     |
| 09 | RTC Configuration           | R/W/N - Date/time synchronisation                        |
| 10 | Alarm Status                | R/W/N - Alarm notifications                              |
| 11 | Calibration Management      | R/W/N - Flow calibration updates                         |
| 12 | History Management          | R/W/N - Compact header + unified history fragments       |
| 13 | Diagnostics                 | R/N - System diagnostics                                 |
| 14 | Growing Environment         | R/W/N - Plant/soil/method settings                       |
| 15 | Auto Calc Status            | R/W/N - Automatic calculation results + selection        |
| 16 | Current Task Status         | R/W/N - Real-time task monitoring                        |
| 17 | Timezone Configuration      | R/W/N - UTC offset, DST rules                            |
| 18 | Rain Sensor Config          | R/W/N - Calibration, thresholds                          |
| 19 | Rain Sensor Data            | R/N - Live readings                                      |
| 20 | Rain History Control        | R/W/N - History streaming                                |
| 21 | Environmental Data          | R/N - BME280 readings                                    |
| 22 | Environmental History       | R/W/N - Multi-resolution history                         |
| 23 | Compensation Status         | R/W/N - Rain/temp compensation state + channel select    |
| 24 | Onboarding Status           | R/N - Configuration progress                             |
| 25 | Reset Control               | R/W/N - Confirmation code workflow                       |
| 26 | Rain Integration Status     | R/N - Full integration state                             |
| 27 | Channel Compensation Config | R/W/N - Per-channel compensation                         |
| 28 | Bulk Sync Snapshot          | R - One-shot system snapshot (time/status/env/rain/queue)|
| 29 | Hydraulic Status            | R/W/N - Hydraulic profile/locks/anomaly counters         |

### Custom Configuration Service (5 Characteristics)

UUID: `12345678-1234-5678-9abc-def123456780`

| #  | Name                 | Description                                       |
| -- | -------------------- | ------------------------------------------------- |
| 01 | Custom Soil Config   | R/W/N - Custom soil parameter overrides           |
| 02 | Soil Moisture Config | R/W/N - Global + per-channel moisture estimates   |
| 03 | Config Reset         | R/N - Reset status/progress (use Reset Control)   |
| 04 | Config Status        | R/W/N - Configuration completeness queries        |
| 05 | Interval Mode Config | R/W/N - Interval profile settings                 |

### Pack Service (4 Characteristics)

UUID: `12345678-1234-5678-9abc-def123456800`

| #  | Name          | Description                                                      |
| -- | ------------- | ---------------------------------------------------------------- |
| 01 | Pack Plant    | R/W/N - Install/delete/list plants (pack_plant_v1_t, list reqs)   |
| 02 | Pack Stats    | R - External flash usage, plant/pack counts                      |
| 03 | Pack List     | R/W - List installed packs or pack contents                      |
| 04 | Pack Transfer | R/W/N - Chunked pack install (START/DATA/COMMIT/ABORT)            |

### History Streaming

- Unified 8-byte fragment header (`history_fragment_header_t`) + payload for watering, rain, and environmental history
- Client-driven: write requests select data/fragment; notifications carry fragments (no ACK path)
- Watering history uses a 12-byte compact query header; rain history uses a 16-byte command; environmental history uses a 20-byte request (12/19 bytes accepted for legacy)
- Environmental history does not auto-iterate fragments; clients request `fragment_id` explicitly
- Rain history caps each transfer at 255 fragments

### Bulk Sync Snapshot (Characteristic 28)

- Read-only snapshot returns time, system status, environmental data, rain snapshot, queue depth, and per-channel quick status
- Flow rate field is the smoothed pulses-per-second value; rain_today is current hour, and rain_week/skip fields are placeholders (0)
- Used at connect time to replace multiple characteristic reads

### Auto Calc Status (Characteristic 15)

- Write selects channel: 0..7 per-channel, 0xFF = global (earliest next irrigation across auto-enabled schedules), 0xFE = first automatic channel
- Read/notify includes next irrigation time derived from water balance (immediate if irrigation_needed) or from the channel schedule (local time with solar timing when enabled)
- Notifications are cached and throttled to reduce BLE busy errors

### Configuration Portals

Key characteristics accept writes for:

- Channel metadata and schedules (legacy + growing environment)
- System configuration (power mode, master valve, flow calibration, BME280)
- Manual control (valve, task queue, current task)
- Sensor configuration (rain sensor) and calibration (flow)
- Compensation configuration (per-channel rain/temp)
- Soil moisture overrides and custom soil definitions
- Interval mode timing (cycle/soak)
- History and environmental history requests
- Pack management (plant install/list/delete, pack transfer)
- Reset control (confirmation code workflow)

Every handler validates arguments before updating RAM or NVS.

---

## User-Configurable Settings (BLE)

This section lists every writable characteristic and the fields accepted by firmware, with ranges validated in code.
Note: Several characteristics require full-struct writes; partial writes are rejected except for documented 1-byte channel selectors. Enforced sizes include Task Queue (9), Statistics (15), RTC (16), Calibration (13), Rain Config (18), Rain History (16), Environmental History (20; 12/19 legacy accepted), Reset Control (16), and Channel Compensation Config (44).

### Valve Control (Characteristic 01)

- `channel_id`: 0-7 for zones, 0xFF for master valve control.
- `task_type`: 0=duration (minutes), 1=volume (liters). For master valve, 0=close, 1=open.
- `value`: 1-1440 minutes or 1-1000 liters; ignored for master valve control.
- Master valve manual open/close is rejected when auto-management is enabled.

### Channel Configuration (Characteristic 04, legacy)

- 1-byte write selects channel for read.
- Full struct writes require the 76-byte `channel_config_data` payload.
- Fragmentation header accepted for large writes: `[channel_id][frag_type][size_lo/size_hi]`.
  - `frag_type` 1 = name-only, size is little-endian.
  - `frag_type` 2 = full struct, size is big-endian.
  - `frag_type` 3 = full struct, size is little-endian.
  - Continuation writes append raw bytes until `size` is reached.
- `name_len`: 0-63, `name[64]` string (null-terminated by firmware).
- `auto_enabled`: 0/1 for schedule toggle.
- `plant_type`: 0-7, `soil_type`: 0-7, `irrigation_method`: 0-5 (legacy enums).
- `coverage_type`: 0=area, 1=plant count; `area_m2` or `plant_count` set accordingly.
- `sun_percentage`: 0-100.
- Legacy metadata; FAO-56 uses the Growing Environment characteristic.

### Schedule Configuration (Characteristic 05)

- 1-byte write selects channel for read.
- `schedule_type`: 0=daily, 1=periodic, 2=auto (FAO-56).
- `days_mask`: daily bitmask or periodic interval days; ignored for auto.
- `hour` 0-23, `minute` 0-59.
- `watering_mode`: 0=duration, 1=volume; `value` is minutes or liters.
- `auto_enabled`: 0/1; `value` may be 0 only when FAO-56 ready or AUTO schedule.
- Solar timing: `use_solar_timing` 0/1, `solar_event` 0=sunset/1=sunrise, `solar_offset_minutes` clamped to -120..+120.

### System Configuration (Characteristic 06, enhanced)

- `power_mode`: 0=normal, 1=energy saving, 2=ultra low.
- `flow_calibration`: 100-10,000 pulses/liter.
- Master valve: `master_valve_enabled`, `pre_delay`/`post_delay` (int16 seconds, negative allowed), `overlap_grace_sec`, `auto_mgmt`.
- Negative `pre_delay` opens the master valve after the zone valve; negative `post_delay` closes it before the zone valve.
- BME280: `bme280_enabled`, `bme280_measurement_interval` (seconds, must be >0).
- Global temperature defaults: `global_temp_compensation_enabled`, `global_temp_sensitivity` (clamped 0.01-0.20), `global_temp_base_temperature` (clamped -10..50).
- Read-only: `version`, `max_active_valves`, `num_channels`, `master_valve_current_state`, `bme280_sensor_status`, `interval_mode_active_channels`, `compensation_active_channels`, `incomplete_config_channels`, `environmental_data_quality`, `last_config_update`, `last_sensor_reading`.
- Deprecated global rain fields (`_reserved_rain_*`) are present in the struct but ignored and read back as zero.

### Task Queue (Characteristic 07)

- `command` codes: 1=start next pending, 2=pause current, 3=resume current, 4=cancel current, 5=clear pending queue.
- Writes require the full 9-byte struct; only `command` is actionable, other fields are read-only snapshots.
- Other fields are read-only snapshots (pending count, current task info).

### Statistics (Characteristic 08)

- 1-byte write selects channel for read.
- Full 15-byte struct writes are required for updates/resets.
- All-zero write resets statistics for that channel.
- Sentinel `0xFFFFFFFF`/`0xFFFF` means "no change"; last_volume/last_watering can create a synthetic completion event.

### RTC Configuration (Characteristic 09)

- Set local time: year 0-99, month 1-12, day 1-31, hour 0-23, minute 0-59, second 0-59.
- `day_of_week` is recalculated by firmware.
- Writes require the full 16-byte struct.
- `utc_offset_minutes` is treated as the total offset (base + DST); DST auto rules are configured separately via Timezone.

### Alarm Status (Characteristic 10)

- Write 1 byte: 0x00 or 0xFF clears all alarms; 1-13 clears the current alarm if it matches.
- Active alarm codes: 1=No Flow, 2=Unexpected Flow, 3=Freeze Lockout, 4=High Flow, 5=Low Flow, 6=Mainline Leak, 7=Channel Lock, 8=Global Lock (0=no alarm).
- Writes accept 1-13 for compatibility, but only 1-8 are generated in current firmware.

### Calibration Management (Characteristic 11)

- `action`: 1=start, 0=stop/abort, 3=calculated (requires `volume_ml`), 4=apply (`pulses_per_liter`), 5=reset default.
- `pulses` and `volume_ml` are used to compute calibration.
- Writes require the full 13-byte struct.

### History Management (Characteristic 12)

- Write 12-byte compact header only: `channel_id` (0..7 or 0xFF), `history_type` (0-3), `entry_index`, `count` (1-50), `start_ts`, `end_ts`.
- `channel_id` 0xFF maps to channel 0 for reads; for clears it applies to all channels.
- `history_type` 0xFF clears detailed events older than `start_ts`.

### Growing Environment (Characteristic 14)

- 1-byte write selects channel for read (cached).
- Fragmentation header accepted for large writes: `[channel_id][frag_type][size_lo/size_hi]` where frag_type 2=big-endian size, 3=little-endian size; following writes append raw payload bytes.
- `plant_id`: 0 unset, 1-223 built-in, >=1000 custom (pack storage).
- `soil_db_index`: 0-14 or `UINT8_MAX`; `irrigation_method_index`: 0-14 or `UINT8_MAX`.
- `use_area_based` 1/0 with `area_m2` > 0 or `plant_count` > 0.
- `auto_mode` accepts 0..2 and casts to `watering_mode_t`: 0=duration (manual), 1=volume (manual), 2=automatic quality (100%); Eco (3) not settable via BLE.
- `max_volume_limit_l` >= 0 (0 disables), `enable_cycle_soak` flag stored.
- `planting_date_unix`, `days_after_planting`, `latitude_deg` (-90..90), `sun_exposure_pct` (0-100); longitude is stored per channel but not exposed via BLE.
- Legacy plant adjustment fields (`water_need_factor`, `irrigation_freq_days`, `prefer_area_based`) are written back into pack storage when `plant_id` exists.

### Auto Calc Status (Characteristic 15)

- Write 1 byte: 0..7 selects channel, 0xFE selects first auto channel, 0xFF selects global earliest next irrigation.

### Current Task Status (Characteristic 16)

- Write 1 byte: 0=stop, 1=pause, 2=resume.

### Timezone Configuration (Characteristic 17)

- Set `timezone_config_t`: `utc_offset_minutes`, `dst_enabled`, `dst_start_month/week/dow`, `dst_end_month/week/dow`, `dst_offset_minutes`.
- Range validation: `utc_offset_minutes` -720..840, `dst_offset_minutes` -120..120.
- When `dst_enabled` is 1, valid ranges are months 1..12, weeks 1..5, DOW 0..6; when `dst_enabled` is 0 the rule fields are cleared.

### Rain Sensor Config (Characteristic 18)

- `mm_per_pulse`: 0.1-10.0; `debounce_ms`: 10-1000.
- `sensor_enabled`, `integration_enabled`.
- `rain_sensitivity_pct` 0-100, `skip_threshold_mm` 0-100 (global/deprecated for task logic).
- Writes require the full 18-byte struct.

### Rain History Control (Characteristic 20)

- Writes require the full 16-byte `rain_history_cmd_data`.
- `command`: 0x01 hourly, 0x02 daily, 0x03 recent totals, 0x10 reset, 0x20 calibrate.
- For hourly/daily, `max_entries` must be >0 and `start_timestamp` must be <= `end_timestamp` when both are set.
- `data_type` should be 0 (hourly) or 1 (daily); firmware overwrites it based on command.
- `command` 0x03 returns a single fragment with `data_type=0xFE` and a 16-byte payload: last_hour_mm_x100, last_24h_mm_x100, last_7d_mm_x100, reserved.
- `command` 0x10 returns a header-only response with `data_type=0xFD` (reset ack).
- `command` 0x20 returns a header-only response with `data_type=0xFC` (calibration ack).

### Environmental History (Characteristic 22)

- Writes accept `ble_history_request_t` (20 bytes); legacy 12/19-byte forms are accepted and zero-filled.
- `command`: 0x01 detailed, 0x02 hourly, 0x03 daily, 0x04 trends, 0x05 clear.
- `data_type`: 0=detailed, 1=hourly, 2=daily (trends returns data_type 0x03).
- `max_records`: 0 means 100; values >100 are clamped to 100.
- `fragment_id`: client must request specific fragments (no auto-iteration).

### Compensation Status (Characteristic 23)

- Write 1 byte: 0..7 selects channel, 0xFF selects first auto channel (defaults to 0 if none).
- Read/notify returns per-channel rain/temp compensation status snapshots.

### Reset Control (Characteristic 25)

- Write `reset_control_data`: `reset_type` (channel config, channel schedule, all channels, all schedules, system config, calibration, history, factory reset), `channel_id` (when applicable), `confirmation_code`.
- Confirmation codes are generated by firmware and valid for 5 minutes.
- Factory reset runs a stepwise wipe state machine; status fields report progress and last error.
- Writes require the full 16-byte struct.

### Channel Compensation Config (Characteristic 27)

- 1-byte write selects channel for read.
- Rain: `enabled`, `sensitivity` 0.0-1.0, `lookback_hours` 1-72, `skip_threshold_mm` 0-100, `reduction_factor` 0.0-1.0.
- Temperature: `enabled`, `base_temperature` -40..60, `sensitivity` 0.1-2.0, `min_factor` 0.5-1.0, `max_factor` 1.0-2.0.
- Writes require the full 44-byte struct.

### Hydraulic Status (Characteristic 29)

- Write 1 byte to select channel; 0xFF selects current active channel.

### Custom Configuration Service

- **Custom Soil Config**: `operation` 0=read, 1=create, 2=update, 3=delete; name max 31 chars; ranges: field capacity 5-80%, wilting point 1-40% (wilting < field capacity), infiltration 0.1-1000 mm/h, bulk density 0.5-2.5 g/cm3, organic matter 0-100%.
- **Soil Moisture Config**: `channel_id` 0xFF global or 0..7, `operation` 0=read / 1=set, `enabled` 0/1, `moisture_pct` 0-100.
- **Config Status**: write `config_status_request_data` (`channel_id` 0xFF or 0..7, `include_reset_log` flag).
- **Interval Mode Config**: 1-byte write selects channel for read; full struct sets `channel_id`, `enabled`, watering/pause minutes 0-60 and seconds 0-59 (total per phase 1-3600 sec). Read/notify includes `configured` and `last_update` (read-only).

### Pack Service

- **Pack Plant**: write 156 bytes `pack_plant_v1_t` to install/update; write 2 bytes to delete by `plant_id`; write 4 bytes list request (`offset`, `max_count`, `filter_pack_id`, where `max_count=0` streams via notifications and `filter_pack_id` is 0xFF custom, 0xFE all, 0x00 built-in, or 0x01-0xFD specific pack). Responses return up to 10 entries.
- **Pack List**: write `bt_pack_list_req_t` with opcode 0x01 (list packs, `offset`) or 0x02 (pack contents, `offset` = pack_id); reads return up to 4 pack entries or up to 16 plant IDs per read.
- **Pack Transfer**: START/DATA/COMMIT/ABORT opcodes; max 64 plants per transfer, 240-byte chunk size, 120 s timeout.
- **Pack Stats**: read-only storage usage and counts.

---

## Onboarding, Scoring, and Reset

### Onboarding Ledger

- Channel-level flags (per channel x 8 bits):
  - Plant type, soil type, irrigation method
  - Coverage, sun exposure, name
  - Water factor, enabled
- Extended flags:
  - FAO-56 ready, rain comp configured, temp comp configured
  - Latitude set, volume limit set, planting date set, cycle-soak enabled
- Schedule flags (8 bits, one per channel)
- System flags: timezone, flow calibrated, master valve, RTC, rain sensor, power mode, location, initial setup

### Configuration Scoring Weights

- Weights per group:
  - Basic setup: 25%
  - Growing environment: 25%
  - Compensation: 20%
  - Custom soil: 15%
  - Interval profile: 15%
- Health levels: POOR (<40%), FAIR (40-59%), GOOD (60-79%), EXCELLENT (80%+)
- Completion percentage recomputed after every update.
- Onboarding complete when at least 1 channel sets all critical flags and RTC flag raised.

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

- `OK`, `FAULT`, `NO_FLOW`, `UNEXPECTED_FLOW`, `RTC_ERROR`, `LOW_POWER`, `FREEZE_LOCKOUT`, `LOCKED`

Enhanced status module derives:

- Interval phase
- Compensation flags
- Sensor health indicators
- Configuration completeness
- Bitmaps of active, interval, and incomplete channels

### Freeze Lockout

- Blocks task enqueue/scheduling when temperature <= 2 C or environmental data is stale (>10 min); firmware attempts an on-demand BME280 read when data is stale/invalid.
- If the sensor is unavailable, it fails open with a warm default (> clear threshold); stale-but-warm readings clear lockout.
- Clears when temperature >= 4 C; raises a BLE alarm while active.

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

## Firmware Functions (Module Capabilities)

- Task scheduling and queue management: create duration/volume tasks, enqueue, start/pause/resume/stop, scheduler scans (`watering_tasks.c`).
- Valve control: zone + master valve sequencing, delays, overlap grace, manual override (`valve_control.c`).
- FAO-56 engine: ET0, Kc staging, water balance, automatic irrigation decisions (`fao56_calc.c`).
- Interval mode: cycle/soak state machine, phase timing, volume-per-phase (`interval_mode_controller.c`).
- Rain integration: per-channel skip/reduce logic, effective rainfall, rain history (`rain_integration.c`, `rain_history.c`).
- Flow/hydraulic monitoring: pulse counting, calibration, anomaly detection, static leak test (`flow_sensor.c`, `watering_monitor.c`).
- Environmental sensing: BME280 reads, data quality, history aggregation (`env_sensors.c`, `environmental_data.c`, `environmental_history.c`).
- Storage and persistence: NVS configs/history, LittleFS history and pack storage, cleanup (`nvs_config.c`, `history_flash.c`, `pack_storage.c`).
- BLE services: irrigation, custom config, pack service, fragmentation/notifications (`bt_irrigation_service.c`, `bt_custom_soil_handlers.c`, `bt_pack_handlers.c`).
- Time and timezone: RTC read/write, DST rules, local/UTC conversion (`rtc.c`, `timezone.c`).
- Power and safety controls: power mode pacing, freeze lockout, scheduler gating (`power_management.c`, `watering_tasks.c`).
- Reset and onboarding: config scoring, confirmation codes, factory wipe state machine (`onboarding_state.c`, `reset_controller.c`).

---

## Error Codes Reference

### Watering Error Codes (`watering_error_t`)

Complete enumeration from `watering.h`:

| Code | Name                              | Description                              |
| ---- | --------------------------------- | ---------------------------------------- |
| 0    | `WATERING_SUCCESS`                | Operation completed successfully         |
| -1   | `WATERING_ERROR_INVALID_PARAM`    | Invalid parameter provided               |
| -2   | `WATERING_ERROR_NOT_INITIALIZED`  | System not initialized                   |
| -3   | `WATERING_ERROR_HARDWARE`         | Hardware failure                         |
| -4   | `WATERING_ERROR_BUSY`             | System busy with another operation       |
| -5   | `WATERING_ERROR_QUEUE_FULL`       | Task queue is full                       |
| -6   | `WATERING_ERROR_TIMEOUT`          | Operation timed out                      |
| -7   | `WATERING_ERROR_CONFIG`           | Configuration error                      |
| -8   | `WATERING_ERROR_RTC_FAILURE`      | RTC communication failure                |
| -9   | `WATERING_ERROR_STORAGE`          | Storage operation failed                 |
| -10  | `WATERING_ERROR_DATA_CORRUPT`     | Data corruption detected                 |
| -11  | `WATERING_ERROR_INVALID_DATA`     | Invalid data format                      |
| -12  | `WATERING_ERROR_BUFFER_FULL`      | Buffer overflow                          |
| -13  | `WATERING_ERROR_NO_MEMORY`        | Memory allocation failed                 |
| -14  | `WATERING_ERROR_SOLAR_FALLBACK`   | Solar calculation failed, using fallback |

### Schedule Type Codes (`schedule_type_t`)

| Code | Name               | Description                                    |
| ---- | ------------------ | ---------------------------------------------- |
| 0    | `SCHEDULE_DAILY`   | Water on specific days of the week (bitmask)   |
| 1    | `SCHEDULE_PERIODIC`| Water every N days (interval-based)            |
| 2    | `SCHEDULE_AUTO`    | FAO-56 smart scheduling (waters at RAW deficit)|

### Reset Status Codes (`reset_status_t`)

| Code | Name            | Description                    |
| ---- | --------------- | ------------------------------ |
| 0    | SUCCESS         | Reset completed successfully   |
| 1    | INVALID_TYPE    | Invalid reset type             |
| 2    | INVALID_CHANNEL | Invalid channel ID             |
| 3    | INVALID_CODE    | Invalid confirmation code      |
| 4    | CODE_EXPIRED    | Confirmation code expired      |
| 5    | STORAGE_ERROR   | NVS storage error              |
| 6    | SYSTEM_ERROR    | General system error           |

### Hydraulic Actions (`hydraulic_log_action_t`)

| Code | Name                                | Description                     |
| ---- | ----------------------------------- | ------------------------------- |
| 0    | `HYDRAULIC_LOG_ACTION_WARN`         | Warning logged, no action taken |
| 1    | `HYDRAULIC_LOG_ACTION_CHANNEL_LOCK` | Channel locked                  |
| 2    | `HYDRAULIC_LOG_ACTION_GLOBAL_LOCK`  | Global lock activated           |

---

## Limitations and Behaviour Notes

### Compensation Engines

- Rain integration applies skip/reduction for **TIME and VOLUME watering modes only**
- Global rain integration config fields (Rain Sensor Config) are deprecated; per-channel settings drive adjustments
- Temperature compensation is configurable but not applied in the current task execution path
- **FAO-56 modes (Quality/Eco) do NOT use external compensation** because they already incorporate rain and temperature in ET0 calculations
- Applying compensation on top of FAO-56 would result in double-counting

### BLE Constraints

- Growing Environment `auto_mode` writes accept 0..2 and cast to `watering_mode_t` (0=duration, 1=volume, 2=auto quality); Eco (3) is not settable via BLE
- Longitude for solar timing is stored per channel but not exposed via BLE
- Auto calculation interval is internal and not configurable via BLE

### Interval Mode

- Interval profiles only control execution cadence; they do not schedule jobs on their own
- Users must still create duration or volume tasks
- Only compatible with TIME and VOLUME modes

### Master Valve Manual Mode

- When automatic master valve management disabled, clients must open and close master valve explicitly
- Manual toggles rejected while auto-management enabled.

### History Rotation

- Watering history uses NVS-backed ring buffers with rotation metadata and GC cleanup.
- Rain/environment history rotation is handled by fixed-size ring files when `CONFIG_HISTORY_EXTERNAL_FLASH=y`.
- Extremely full NVS partitions might require manual clean-up via reset commands.

### Hardware Requirements

- Tested target: Arduino Nano 33 BLE (nRF52840)
- Zephyr version: 4.3.0
- Storage: NVS for configuration + watering history; W25Q128 (16 MB) external SPI flash for LittleFS-backed rain/environment history when enabled

---

## File Reference

| Module             | Primary Files                                                                                          | Purpose                                     |
| ------------------ | ------------------------------------------------------------------------------------------------------ | ------------------------------------------- |
| Core               | `watering.c/h`, `watering_tasks.c`, `watering_internal.h`                                              | Task dispatch, scheduling, channel control  |
| FAO-56             | `fao56_calc.c/h`, `fao56_custom_soil.c/h`                                                              | Scientific irrigation calculations          |
| Flow               | `flow_sensor.c/h`, `watering_monitor.c`                                                                | Pulse counting, calibration, anomaly detect |
| Rain               | `rain_sensor.c/h`, `rain_history.c/h`, `rain_integration.c/h`, `rain_compensation.c/h`                 | Rain sensing, history, compensation         |
| Environment        | `env_sensors.c/h`, `environmental_data.c/h`, `environmental_history.c/h`, `bme280_driver.c/h`          | BME280, data aggregation                    |
| Temperature        | `temperature_compensation.c/h`, `temperature_compensation_integration.c/h`                             | Temperature-based adjustments               |
| Interval           | `interval_mode_controller.c/h`, `interval_timing.c/h`, `interval_task_integration.c/h`                 | Phase cycling                               |
| BLE Primary        | `bt_irrigation_service.c/h`                                                                            | 29-characteristic GATT service              |
| BLE Custom         | `bt_custom_soil_handlers.c/h`                                                                          | 5-characteristic custom config service      |
| BLE History        | `bt_environmental_history_handlers.c/h`                                                                | Environmental history streaming             |
| BLE Pack           | `bt_pack_handlers.c/h`                                                                                | Pack install/list/transfer service          |
| Pack Storage       | `pack_storage.c/h`, `pack_schema.h`                                                                    | Plant/pack files on `/lfs_ext`              |
| Storage (NVS)      | `nvs_config.c/h`, `nvs_storage_monitor.c/h`                                                            | Persistence, health monitoring              |
| Storage (LittleFS) | `database_flash.c/h`, `history_flash.c/h`                                                              | External flash LittleFS mount               |
| Config             | `configuration_status.c/h`, `onboarding_state.c/h`, `reset_controller.c/h`, `soil_moisture_config.c/h` | Scoring, onboarding, reset                  |
| Status             | `enhanced_system_status.c/h`, `enhanced_error_handling.c/h`                                            | Diagnostics, recovery                       |
| Databases          | `plant_full_db.c/inc`, `soil_enhanced_db.c/inc`, `irrigation_methods_db.c/inc`                         | Generated from CSV                          |
| History            | `watering_history.c/h`, `rain_history.c/h`, `environmental_history.c/h`                                | Multi-resolution event tracking             |
| Main               | `main.c`                                                                                               | System initialization sequence              |
| Board              | `boards/arduino_nano_33_ble.overlay`                                                                   | Devicetree overlay for hardware config      |
