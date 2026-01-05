# AutoWatering - Key Features (Code-Verified Summary)

Focused, externally facing list. All items map to existing modules or confirmed limits.
*Last updated: 2026-01-05*

---

## Quick Reference

| Category | Count/Limit |
| --- | --- |
| Irrigation channels | 8 (`WATERING_CHANNELS_COUNT`) |
| Watering modes | 4 (TIME, VOLUME, Quality, Eco) |
| Schedule types | 3 (Daily, Periodic, AUTO) |
| Plant species | 223 (`PLANT_FULL_SPECIES_COUNT`) |
| Soil types | 15 (`SOIL_ENHANCED_TYPES_COUNT`) |
| Irrigation methods | 15 (`IRRIGATION_METHODS_COUNT`) |
| BLE characteristics | 34 (29 + 5) |
| Concurrent tasks | 1 (queue depth 10) |
| Hardware target | Arduino Nano 33 BLE (nRF52840) |
| Zephyr version | 4.3.0 |

---

## Core Control

- 8 independent irrigation channels (`WATERING_CHANNELS_COUNT`).
- Four watering modes:
  - **TIME** (`WATERING_BY_DURATION`): Fixed duration in minutes.
  - **VOLUME** (`WATERING_BY_VOLUME`): Fixed volume in liters (flow-sensor monitored).
  - **Quality** (`WATERING_AUTOMATIC_QUALITY`): FAO-56 based, 100% of calculated requirement.
  - **Eco** (`WATERING_AUTOMATIC_ECO`): FAO-56 based, 70% of calculated requirement.
- Quality and Eco modes are **exclusively FAO-56 based** (require plant/soil/method configuration).
- Single active watering task at a time (message queue dispatch, depth 10).
- Interval mode: watering/pause phase cycling for TIME and VOLUME modes (configurable minutes/seconds).
- State machine states: IDLE, WATERING, PAUSED, ERROR_RECOVERY.

Implementation notes (where this lives in code):

- Task orchestration + scheduling loops: `src/watering_tasks.c`, `src/watering.c`.
- Valve/master-valve control path: `src/valve_control.c`, `src/watering.c`.
- Interval-mode state machine: `src/interval_mode_controller.c`, `src/interval_task_integration.c`.

## Scheduling & Automation

- Three schedule modes per channel:
  - **Daily (bitmask)**: Runs on selected days of the week.
  - **Periodic**: Runs every N days.
  - **AUTO (Smart Schedule)**: FAO-56 based - evaluates soil water deficit daily, irrigates only when deficit exceeds plant's RAW threshold. Requires plant/soil/planting_date and coverage configuration.
- Solar timing option: sunrise/sunset scheduling with +/-120 min offset using latitude/longitude; falls back to configured start time if solar calc fails.
- On-demand FAO-56 based irrigation requirement calculation (`watering_run_automatic_calculations()`).
- AUTO mode features:
  - Daily deficit tracking with ETc (crop evapotranspiration) accumulation.
  - Environmental stress adjustment on hot/dry days (reduced MAD threshold).
  - Automatic rainfall integration (subtracts effective precipitation from deficit).
  - Multi-day offline gap handling: estimates missed ETc on power-up.
- Rain integration: skip / reduction logic (channel-specific) using recent rainfall history.
  - Rain skip/reduction apply **only to TIME and VOLUME modes**.
  - FAO-56 modes (Quality/Eco/AUTO) already incorporate rain in ET0 calculations.
  - Temperature compensation is configurable but not applied in the current task execution path.
- Configurable auto-calculation interval (1-24 hours).

Implementation notes:

- Schedule storage and scheduling logic: `src/watering_config.c`, `src/watering_tasks.c`.
- AUTO (FAO-56) deficit tracking + daily update loop: `src/fao56_calc.c`, `src/watering.c`.
- Time conversion (RTC + timezone rules): `src/rtc.c`, `src/timezone.c`.

## Sensing & Monitoring

- **Flow sensor**: Pulse counting with calibration (100-10,000 pulses/liter; adjustable via BLE).
  - Default 450 pulses/liter from devicetree.
  - Debounce configurable (default 5 ms).
  - BLE notifications: +10 pulses or 5 s heartbeat (whichever first).
- **Hydraulic Sentinel (H.H.M.)**:
  - **Auto-learning**: 2-6 runs per channel, measures ramp-up time and nominal flow.
  - **Profile types**: AUTO (learned), SPRAY (fast), DRIP (slow).
  - **Start ignore windows**: Fast 8-20 s, Slow 30-90 s, Default 12-25 s (clamped from ramp + offset).
  - **HIGH FLOW**: 5 s sustained over limit → close all valves; channel or global lock.
  - **NO FLOW**: 3 s stall detection + 3 valve toggle retries; soft lock with auto-retry, hard lock after 3 consecutive failures.
  - **LOW FLOW**: Warning only after 30 s below limit; watering continues.
  - **UNEXPECTED FLOW**: >10 pulses/30 s for 30 s (2 s post-close ignore); global hard lock if persistent.
  - **Nightly static test**: 03:00, master on 10 s, off 5 s, monitor 60 s; skipped if watering/queued/locked.
  - **Anomaly log**: Append-only ring on external flash (`/lfs/history/hydraulic_events.bin`).
- **Hydraulic Status BLE**: Per-channel profile, tolerances, lock state/reason, anomaly counters, plus global lock snapshot.
- **Rain gauge**: Tipping-bucket with 0.2 mm/pulse default, 50 ms debounce, health monitoring with pulse statistics.
- **Environmental sensor** (BME280): Temperature, humidity, pressure; 15/60 min polling intervals.
  - Data quality score (0-100), calibration offsets, validity flags.
  - Derived dewpoint and vapor pressures.
- Manual soil moisture estimate (global + per-channel override) feeds effective rainfall modeling; no physical probes.
- Environmental, rain, and watering history with multi-resolution aggregation.
- Current task progress & completion notifications via BLE.

Implementation notes:

- Flow sensor + hydraulic monitor: `src/flow_sensor.c`, `src/watering_monitor.c`.
- Rain gauge, integration, and history: `src/rain_sensor.c`, `src/rain_integration.c`, `src/rain_history.c`.
- Environmental sensing and aggregation: `src/env_sensors.c`, `src/environmental_data.c`, `src/environmental_history.c`.

## Data & Persistence (NVS + LittleFS)

- Channel configuration, calibration, schedules, custom soil, compensation settings stored in NVS.
- Soil moisture estimates (global + per-channel overrides) stored in NVS.
- **Plant database**: 223 entries (`PLANT_FULL_SPECIES_COUNT`), 10 categories, 44-byte structs.
  - Kc coefficients (initial/development/mid/end season).
  - Root depth, depletion fraction, spacing, density.
  - Stage lengths (ini/dev/mid/end days), frost tolerance.
  - Optimal temperature range, growth cycle, typical irrigation method.
- **Soil database**: 15 enhanced types (`SOIL_ENHANCED_TYPES_COUNT`).
  - Field capacity, wilting point, available water capacity.
  - Infiltration rate, default depletion fraction.
- **Irrigation methods database**: 15 entries (`IRRIGATION_METHODS_COUNT`).
  - Efficiency, wetting fraction, distribution uniformity.
  - Application depth and rate ranges.
- **Watering history** (NVS):
  - 30 detailed events/channel (15 bytes: delta, flags, target/actual, flow rate).
  - 90 daily stats (16 bytes: epoch, total ml, sessions ok/err, success rate).
  - 36 monthly stats (12 bytes: year/month, total ml, active days, peak channel).
  - 10 annual stats (20 bytes: year, total ml, sessions, errors, max/min month).
- **Rain + environmental history** (LittleFS when `CONFIG_HISTORY_EXTERNAL_FLASH=y`):
  - Rain: 720 hourly (8 bytes) + 1,825 daily (12 bytes).
  - Environmental: 720 hourly + 372 daily + 60 monthly.
- Hourly rain history recorded on UTC hour rollover; daily summaries derived from hourly.
- Environmental history auto-aggregated hourly/daily/monthly from BME280 snapshots.
- Automatic NVS storage cleanup (80/90% thresholds, target 70%); LittleFS wear levelling.
- Storage monitor: mounts partition, tracks read/write errors, schedules cleanup.

Implementation notes:

- NVS read/write wrappers (single source of truth): `src/nvs_config.c`.
- NVS usage monitoring/cleanup: `src/nvs_storage_monitor.c`.
- External flash history + filesystem mount: `src/history_flash.c`.
- Optional binary DB on LittleFS (`/lfs/db/*.bin`): `src/database_flash.c`.

## Master Valve

- Optional master valve with pre-start (default +3 s) and post-stop delays (default +2 s).
- Overlap grace window (5 s) to keep master open between consecutive tasks.
- Automatic or manual (when auto disabled) control paths.

Implementation notes:

- Master valve gating and delays: `src/valve_control.c`, plus config in `src/watering_config.c`.

## Power Modes

- **Normal**: 60 s scheduler sleep.
- **Energy Saving**: 120 s scheduler sleep.
- **Ultra Low Power**: 300 s scheduler sleep.
- Zephyr PM is disabled (`CONFIG_PM=n`), so these modes only adjust scheduler pacing.

Implementation notes:

- Sleep pacing and power mode switching: `src/power_management.c`, scheduler loop in `src/watering_tasks.c`.

## Bluetooth Low Energy

- Primary irrigation service: **29 characteristics** (`docs/ble-api/`), including Bulk Sync Snapshot and Hydraulic Status.
- Custom configuration service: **5 characteristics** for custom soil, soil moisture, config status/reset, and interval mode.
- **Total: 34 characteristics** across both services.
- Notification scheduler: 8-buffer pool, MTU-aware payloads (up to 250 bytes), adaptive throttling:
  - Critical: 0 ms (alarms, errors).
  - High: 50 ms (status updates, valve changes).
  - Normal: 200 ms (flow data, statistics).
  - Low: 1 s (history, diagnostics).
- Fragmentation for large payloads (TLV-framed, sequence-numbered).
- History streaming via write-triggered fragment notifications (no client ACK).
- **Bulk Sync Snapshot** (UUID 0xde60): Single 60-byte READ replaces 10+ queries at connection; contains system status, environmental data, rain totals, compensation, and channel states.
- Auto Calc Status characteristic: per-channel selection plus global mode (0xFF = earliest next irrigation) and helper select (0xFE = first automatic channel).

### BLE Performance Optimizations (v3.1.0)

- **PHY 2M**: Automatically requested at connection (~2x throughput).
- **Data Length Extension (DLE)**: Requests 251-byte packets (gracefully handles failure).
- **Binary Search**: O(log n) history queries instead of linear scan (~10-70x speedup).
- **Fragment Streaming**: 2 ms inter-fragment delay, retry with exponential backoff.
- **Transfer Caching**: 30 s cache for history transfers avoids re-reading flash.

Implementation notes:

- GATT service/characteristic table: `src/bt_irrigation_service.c`.
- Domain handlers (channel config, interval mode, histories, etc.): `src/bt_*_handlers.c`.
- Packed BLE structs/contracts: `src/bt_gatt_structs.h`, `src/bt_gatt_structs_enhanced.h`.

## Time Handling

- DS3231 RTC integration using UTC timestamps for scheduling and history.
- Timezone configuration with DST support (UTC offset, start/end rules).
- Fallback to uptime-derived time if RTC unavailable (5 consecutive failures trigger fallback).

Implementation notes:

- RTC I/O + failure handling: `src/rtc.c`.
- Timezone/DST conversion helpers: `src/timezone.c`.

## Error & Status Reporting

- **Status codes**: OK, No-Flow, Unexpected-Flow, Fault, RTC Error, Low Power, Freeze Lockout, Locked.
- **Error codes** (`watering_error_t`): 15 defined codes (0 = SUCCESS, -1 to -14 = errors):
  - INVALID_PARAM, NOT_INITIALIZED, HARDWARE, BUSY, QUEUE_FULL, TIMEOUT, CONFIG, RTC_FAILURE, STORAGE, DATA_CORRUPT, INVALID_DATA, BUFFER_FULL, NO_MEMORY, SOLAR_FALLBACK.
- **Schedule types** (`schedule_type_t`): DAILY (bitmask), PERIODIC (every N days), AUTO (FAO-56 smart).
- **Freeze lockout**: Blocks watering when temp ≤ 2°C or environmental data stale (>10 min); clears at ≥ 4°C; raises BLE alarm.
- **Manual override**: Explicit BLE commands can temporarily bypass locks for testing.
- **Enhanced system status** aggregates:
  - Interval phase.
  - Compensation state (rain/temp flags).
  - Sensor health indicators.
  - Configuration completeness.
  - Channel bitmaps (active, interval, incomplete).
- **Error recovery strategies**: Retry, Fallback, Disable, Reset, Graceful Degrade.
- **Recovery contexts**: BME280 init, sensor reads, compensation errors, interval controller, storage issues.
- Rain-based skip events logged via history helpers.


Implementation notes:

- Enhanced status aggregation: `src/enhanced_system_status.c`.
- Error classification/recovery glue: `src/enhanced_error_handling.c`, plus per-module safety checks.

## Interval Mode

- **State machine**: IDLE → WATERING → PAUSING → COMPLETED (or ERROR).
- **Configuration** per channel:
  - Watering phase duration (minutes + seconds).
  - Pause phase duration (minutes + seconds).
  - Total target (duration or volume), cycles completed, current phase.
- **Compatibility**: TIME and VOLUME modes only; FAO-56 modes not supported.
- **Controller tracks**: Task/phase start times, total elapsed, total volume, cycle count.
- **Flow rate integration**: Uses ml/sec for volume-based phase calculations.
- **Master valve sync**: Keeps master valve in sync during phase transitions.
- Manual pause/resume/stop commands interact with controller.


Implementation notes:

- State machine: `src/interval_mode_controller.c`.
- Timing helpers: `src/interval_timing.c`.
- Task integration: `src/interval_task_integration.c`.

## Onboarding & Reset System

- **Configuration scoring** per channel (0-100%, weighted groups):
  - Basic setup: 25%
  - Growing environment: 25%
  - Compensation: 20%
  - Custom soil: 15%
  - Interval profile: 15%
- **Health levels**: POOR (<40%), FAIR (40-59%), GOOD (60-79%), EXCELLENT (80%+).
- **Onboarding flags**: plant, soil, method, coverage, sun exposure, name, compensation, latitude, planting date.
- **Extended flags**: FAO-56 ready, rain comp, temp comp, volume limit, cycle-soak enabled.
- **Reset controller** with 32-bit confirmation codes (5 min validity):
  - Single-channel configuration or schedule.
  - All channels or all schedules.
  - System configuration, calibration, histories.
  - Full factory reset.
- **Reset log**: 16-entry ring per channel with timestamps, group, and reason.
- Onboarding complete when ≥1 channel has all critical flags and RTC flag raised.

Implementation notes:

- Onboarding/completeness scoring: `src/onboarding_state.c`, `src/configuration_status.c`.
- Reset opcodes + confirmation flow: `src/reset_controller.c`.

## Compensation Systems

- **Rain compensation** (TIME/VOLUME modes only):
  - Per-channel sensitivity, skip threshold, lookback hours.
  - Task adjustments use the rain integration curve in `src/rain_integration.c`.
  - The standalone algorithm set in `src/rain_compensation.c` exists but is not wired into task execution.
- **Temperature compensation**:
  - Configuration and status are stored per channel, but the execution path does not apply it yet.
  - Default config uses 20 C base, 0.05 sensitivity, 0.5-2.0 factor range.

  Implementation notes:

  - Rain integration/adjustments: `src/rain_integration.c`.
  - Compensation engines (not wired to tasks yet): `src/rain_compensation.c`, `src/temperature_compensation.c`, `src/temperature_compensation_integration.c`.

## FAO-56 Calculation Engine

- **Reference Evapotranspiration (ET0)**:
  - Penman-Monteith using temperature, humidity, pressure, latitude, day-of-year.
  - Hargreaves-Samani fallback when humidity/pressure missing (marked as fallback result).
  - Per-channel caching to avoid recomputation.
- **Crop Coefficient (Kc)**:
  - Four phenological stages: Initial, Development, Mid-Season, End-Season.
  - Uses plant database Kc values + days after planting.
  - Per-channel caching.
- **Water Balance**:
  - Readily Available Water (RAW) and Total Available Water (TAW).
  - Current deficit tracking, net irrigation requirement.
  - Custom soil overlays override database defaults.
- **Effective Rainfall**: Hourly/daily rain history + soil infiltration factor (default 0.8).
- **Volume Conversion**: mm → liters using area or per-plant coverage (default 0.5 m²/plant).
- **Constraints**: Max volume limit, Eco mode (70%), minimum 1 liter enforced.


Implementation notes:

- FAO-56 calculations: `src/fao56_calc.c`, `src/fao56_custom_soil.c`.
- Water balance types: `src/water_balance_types.h`.

## Extensibility

- Modular C sources (watering, tasks, history, sensors, FAO calc, rain integration).
- Generated databases (plant, soil, irrigation methods) via Python scripts (`tools/build_database.py`).
- Hardware target: Arduino Nano 33 BLE (nRF52840).


Implementation notes:

- Generated databases are committed `.inc` artefacts; sources are CSVs + generators under `tools/`.
- Hardware-specific storage sizing/mapping is defined in `boards/*.overlay`.

## Not Implemented (Removed From Marketing)

- Background FAO thread (calculations are on-demand).
- Generic memory/health monitoring subsystems.
- Multi-task concurrent irrigation (single active task enforced).
- Soil moisture probes (no physical sensor integration; manual antecedent moisture estimate only).
- Temperature compensation in task execution (config exists, not applied).
- Advanced rain compensation algorithms (simple integration used instead).

This concise sheet avoids speculative metrics (latency, throughput) until measured tests are added.

---

## File Reference

| Component | Primary Files | Purpose |
| --- | --- | --- |
| Main entry | `src/main.c` | Boot sequence, module init |
| Watering engine | `src/watering.c`, `src/watering_tasks.c`, `src/watering_internal.h` | Task dispatch, scheduling |
| Valve control | `src/valve_control.c` | GPIO, master valve, sequencing |
| FAO-56 engine | `src/fao56_calc.c`, `src/fao56_custom_soil.c`, `src/water_balance_types.h` | ET0, Kc, water balance |
| Flow sensor | `src/flow_sensor.c`, `src/watering_monitor.c` | Pulse counting, hydraulic sentinel |
| Rain sensor | `src/rain_sensor.c`, `src/rain_history.c`, `src/rain_integration.c` | Rain gauge, history, integration |
| Environment | `src/env_sensors.c`, `src/environmental_data.c`, `src/environmental_history.c`, `src/bme280_driver.c` | BME280, aggregation |
| Interval mode | `src/interval_mode_controller.c`, `src/interval_timing.c`, `src/interval_task_integration.c` | Phase cycling |
| BLE service | `src/bt_irrigation_service.c`, `src/bt_*_handlers.c` | GATT service, characteristics |
| BLE structs | `src/bt_gatt_structs.h`, `src/bt_gatt_structs_enhanced.h` | Packed payloads |
| NVS storage | `src/nvs_config.c`, `src/nvs_storage_monitor.c` | Persistence, health |
| Flash storage | `src/history_flash.c`, `src/database_flash.c` | LittleFS mount, binary DB |
| Config/onboard | `src/configuration_status.c`, `src/onboarding_state.c`, `src/reset_controller.c` | Scoring, reset |
| Status/error | `src/enhanced_system_status.c`, `src/enhanced_error_handling.c` | Diagnostics, recovery |
| Time | `src/rtc.c`, `src/timezone.c` | DS3231 RTC, DST |
| Compensation | `src/rain_compensation.c`, `src/temperature_compensation.c` | Adjustment engines |
| Databases | `src/plant_full_db.c`, `src/soil_enhanced_db.c`, `src/irrigation_methods_db.c` | Generated from CSV |
| Board config | `boards/arduino_nano_33_ble.overlay` | Hardware mapping |

---

## Future (Roadmap)

Implementation work is tracked in the GitHub Project roadmap:

- <https://github.com/users/AlexMihai1804/projects/2>

Firmware roadmap items (grouped by implementation batches):

- **B0 Core fixes**: [#1](https://github.com/AlexMihai1804/AutoWatering/issues/1), [#2](https://github.com/AlexMihai1804/AutoWatering/issues/2), [#3](https://github.com/AlexMihai1804/AutoWatering/issues/3), [#4](https://github.com/AlexMihai1804/AutoWatering/issues/4), [#5](https://github.com/AlexMihai1804/AutoWatering/issues/5)
- **B1 Storage foundation**: [#10](https://github.com/AlexMihai1804/AutoWatering/issues/10), [#6](https://github.com/AlexMihai1804/AutoWatering/issues/6)
- **B2 Device packs MVP**: [#7](https://github.com/AlexMihai1804/AutoWatering/issues/7), [#8](https://github.com/AlexMihai1804/AutoWatering/issues/8), [#12](https://github.com/AlexMihai1804/AutoWatering/issues/12)
- **B4 Updates**: [#13](https://github.com/AlexMihai1804/AutoWatering/issues/13)
- **B5 Custom plant integration**: [#9](https://github.com/AlexMihai1804/AutoWatering/issues/9)
- **B7 Cycle & soak support (firmware dependency for app auto-tuning)**: [#14](https://github.com/AlexMihai1804/AutoWatering/issues/14)
