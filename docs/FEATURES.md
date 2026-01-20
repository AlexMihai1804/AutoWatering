# AutoWatering - Key Features (Code-Verified Summary)

Focused, externally facing list. All items map to existing modules or confirmed limits.
*Last updated: 2026-01-18*

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
| BLE characteristics | 38 (29 irrigation + 5 custom + 4 pack) |
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
- Quality and Eco modes are FAO-56 based and require plant/soil/method, planting date, and coverage (area or plant count) configuration.
- Eco mode exists in firmware, but BLE growing environment writes accept auto_mode 0..2 and cast to `watering_mode_t`, so Eco (3) is not settable via BLE.
- Single active watering task at a time (message queue depth 10). Scheduler avoids queuing more than 2 tasks to leave room for manual overrides.
- Interval mode: per-channel cycle/soak for TIME and VOLUME modes (1-3600 sec per phase).
- State machine states: IDLE, WATERING, PAUSED, ERROR_RECOVERY.

Implementation notes:

- Task orchestration + scheduling loops: `src/watering_tasks.c`, `src/watering.c`.
- Valve/master-valve control path: `src/valve_control.c`, `src/watering.c`.
- Interval-mode state machine: `src/interval_mode_controller.c`, `src/interval_task_integration.c`.

## Scheduling & Automation

- Three schedule modes per channel:
  - **Daily (bitmask)**: Runs on selected days of the week.
  - **Periodic**: Runs every N days.
  - **AUTO (Smart Schedule)**: FAO-56 based - evaluates soil water deficit daily, irrigates only when deficit exceeds the RAW threshold.
- AUTO schedule gate checks plant_id, soil_db_index, planting_date_unix, and coverage; FAO-56 calculations also require a valid irrigation_method_index.
- Solar timing option: sunrise/sunset scheduling with -120..+120 min offset using latitude/longitude; longitude is not exposed via BLE and defaults to 0 unless set elsewhere; falls back to configured start time if solar calc fails.
- AUTO mode features:
  - Daily deficit tracking with ETc accumulation.
  - Environmental stress adjustment on hot/dry days.
  - Automatic rainfall integration (effective rain subtracts from deficit).
  - Multi-day offline gap handling on boot.
- Rain integration: skip/reduction logic is per-channel and applies only to TIME and VOLUME tasks.
  - FAO-56 modes already incorporate weather in ET0 calculations.
  - Rain is applied in realtime (not just at daily check) - prevents watering immediately after rain events.
- Temperature compensation: applied to **TIME and VOLUME modes only** at task start.
  - Calculates factor: `1 + sensitivity * (current_temp - base_temp)`, clamped to min/max factors.
  - FAO-56 modes (Quality/Eco) excluded - they already incorporate temperature in ET0 calculations.
- Auto calculation interval default is 1 hour; the interval is internal and not exposed via BLE.

Implementation notes:

- Schedule storage and scheduling logic: `src/watering_config.c`, `src/watering_tasks.c`.
- AUTO (FAO-56) deficit tracking + daily update loop: `src/fao56_calc.c`, `src/watering.c`.
- Time conversion (RTC + timezone rules): `src/rtc.c`, `src/timezone.c`.

## Sensing & Monitoring

- **Flow sensor**: Pulse counting with calibration (100-10,000 pulses/liter; adjustable via BLE).
  - Default calibration comes from devicetree (`flow-calibration`), 750 in the board overlay; fallback to 450 if DT is missing. NVS overrides when present.
  - Debounce uses devicetree `debounce_ms` (2 ms in overlay; fallback 5 ms if missing).
  - BLE notifications: +10 pulses or 5 s heartbeat (whichever first).
- **Hydraulic Sentinel (H.H.M.)**:
  - Auto-learning: 2-6 runs per channel, measures ramp-up time and nominal flow.
  - Profiles: AUTO (learned), SPRAY (fast), DRIP (slow).
  - Start ignore windows: Fast 8-20 s, Slow 30-90 s, Default 12-25 s (clamped from ramp + offset).
  - HIGH FLOW: 5 s sustained over limit -> close all valves; channel or global lock.
  - NO FLOW: 3 s stall detection + 3 valve toggle retries; soft lock with auto-retry, hard lock after 3 consecutive failures.
  - LOW FLOW: Warning only after 30 s below limit; watering continues.
  - UNEXPECTED FLOW: >10 pulses/30 s for 30 s (2 s post-close ignore) -> global hard lock if persistent.
  - Nightly static test: 03:00, master on 10 s, off 5 s, monitor 60 s; skipped if watering/queued/locked.
  - Mainline leak detection: >3 pulses during the static test -> global hard lock + alarms.
  - Anomaly log: append-only ring on external flash (`/lfs/history/hydraulic_events.bin`).
- **Rain gauge**: Tipping-bucket with 0.2 mm/pulse default, 50 ms debounce.
  - Calibration range: 0.1-10.0 mm/pulse; debounce range 10-1000 ms.
  - Health monitoring with pulse statistics and error counters.
- **Environmental sensor** (BME280): Temperature, humidity, pressure.
  - Default intervals: 15 min (temp/humidity), 60 min (rain aggregation).
  - Data quality score (0-100), calibration offsets, validity flags.
  - BME280 can be enabled and interval set via system config (seconds).
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
- **Plant database**: 223 entries (`PLANT_FULL_SPECIES_COUNT`), 10 categories.
- **Pack storage**: external flash LittleFS at `/lfs_ext` stores built-in plants plus custom plants and packs.
- **Soil database**: 15 enhanced types (`SOIL_ENHANCED_TYPES_COUNT`).
- **Irrigation methods database**: 15 entries (`IRRIGATION_METHODS_COUNT`).
- **Watering history** (NVS):
  - 30 detailed events/channel.
  - 90 daily stats.
  - 36 monthly stats.
  - 10 annual stats.
- **Rain + environmental history** (LittleFS when `CONFIG_HISTORY_EXTERNAL_FLASH=y`):
  - Rain: 720 hourly + 1,825 daily.
  - Environmental: 720 hourly + 372 daily + 60 monthly.
- Automatic NVS cleanup (80/90% thresholds, target 70%); LittleFS wear leveling.

Implementation notes:

- NVS read/write wrappers: `src/nvs_config.c`.
- NVS usage monitoring/cleanup: `src/nvs_storage_monitor.c`.
- External flash history + filesystem mount: `src/history_flash.c`.
- Pack storage: `src/pack_storage.c`, `src/bt_pack_handlers.c`.

## Master Valve

- Optional master valve with pre-start (default +3 s) and post-stop delays (default +2 s).
- Negative delays are supported: pre-start < 0 opens after the zone, post-stop < 0 closes before the zone.
- Overlap grace window (5 s) keeps master open between consecutive tasks.
- Automatic or manual (when auto management is disabled) control paths.

Implementation notes:

- Master valve gating and delays: `src/valve_control.c`, config in `src/watering_config.c`.

## Power Modes

- **Normal**: processing loop 0.5 s, scheduler loop 60 s.
- **Energy Saving**: processing loop 2 s, scheduler loop 120 s.
- **Ultra Low Power**: processing loop 600 s, scheduler loop 300 s.
- Zephyr PM is disabled (`CONFIG_PM=n`), so these modes adjust thread pacing only.

Implementation notes:

- Sleep pacing and power mode switching: `src/power_management.c`, loops in `src/watering_tasks.c`.

## Bluetooth Low Energy

- Primary irrigation service: 29 characteristics.
- Custom configuration service: 5 characteristics (custom soil, soil moisture, config status, interval mode).
- Pack service: 4 characteristics (plant install/list/delete, stats, pack transfer, pack list).
- **Total: 38 characteristics** across all services.
- Notification scheduler: 8-buffer pool, MTU-aware payloads (up to 250 bytes), adaptive throttling.
- Fragmentation for large payloads (history, environmental data).
- **Bulk Sync Snapshot** (UUID 0xde60): 60-byte read of system status, environmental data, and rain snapshot (current-hour rain; week/skip fields are placeholders; flow rate is pulses/sec).

Implementation notes:

- GATT service/characteristic table: `src/bt_irrigation_service.c`.
- Domain handlers (channel config, interval mode, histories, etc.): `src/bt_*_handlers.c`.
- Pack service: `src/bt_pack_handlers.c`.

## Time Handling

- DS3231 RTC integration using UTC timestamps for scheduling and history.
- Timezone configuration with DST support (UTC offset, start/end rules).
- RTC writes treat input as local time and store UTC; DST auto rules are set via the Timezone characteristic.
- Timezone writes validate ranges: UTC offset -720..840, DST offset -120..120, DST months 1..12, weeks 1..5, DOW 0..6; rules are cleared when DST is disabled.

Implementation notes:

- RTC I/O + failure handling: `src/rtc.c`.
- Timezone/DST conversion helpers: `src/timezone.c`.

## Error & Status Reporting

- Status codes: OK, NO_FLOW, UNEXPECTED_FLOW, FAULT, RTC_ERROR, LOW_POWER, FREEZE_LOCKOUT, LOCKED.
- Freeze lockout: blocks enqueue/start when temp <= 2 C or data stale > 10 min; attempts on-demand BME280 read, fails open with warm default if sensor unavailable; clears at >= 4 C; raises BLE alarm.
- Manual override window for hydraulic locks: 10 minutes via direct commands (bypass locks for testing).
- Enhanced system status aggregates interval phase, compensation flags, sensor health, config completeness, and channel bitmaps.

Implementation notes:

- Enhanced status aggregation: `src/enhanced_system_status.c`.
- Error classification/recovery glue: `src/enhanced_error_handling.c`.

## Interval Mode

- State machine: IDLE -> WATERING -> PAUSING -> COMPLETED (or ERROR).
- Configuration per channel: watering/pause durations (minutes + seconds).
- Compatibility: TIME and VOLUME modes only; FAO-56 modes not supported.
- Controller tracks phase timings, volume, and cycle count; master valve stays in sync.

Implementation notes:

- State machine: `src/interval_mode_controller.c`.
- Timing helpers: `src/interval_timing.c`.
- Task integration: `src/interval_task_integration.c`.

## Onboarding & Reset System

- Configuration scoring per channel (0-100%, weighted groups):
  - Basic setup: 25%
  - Growing environment: 25%
  - Compensation: 20%
  - Custom soil: 15%
  - Interval profile: 15%
- Health levels: POOR (<40%), FAIR (40-59%), GOOD (60-79%), EXCELLENT (80%+).
- Reset controller with 32-bit confirmation codes (5 min validity): per-channel, all-channel, system, calibration, histories, factory reset.

Implementation notes:

- Onboarding/completeness scoring: `src/onboarding_state.c`, `src/configuration_status.c`.
- Reset opcodes + confirmation flow: `src/reset_controller.c`.

## Compensation Systems

- **Rain compensation** (TIME/VOLUME modes only): per-channel sensitivity, lookback, skip threshold, reduction factor.
  - BLE range for lookback: 1-72 hours (internal validation supports 1-168).
  - Global rain integration settings (from Rain Sensor Config) are deprecated and not used in task adjustments.
- **Temperature compensation**: per-channel config persisted but not applied during task execution.
  - Per-channel BLE ranges: base -40..60 C, sensitivity 0.1..2.0, min 0.5..1.0, max 1.0..2.0.
  - Global defaults (system config) are clamped to -10..50 C and 0.01..0.20 sensitivity.

Implementation notes:

- Rain integration/adjustments: `src/rain_integration.c`.
- Compensation engines (not wired to tasks yet): `src/rain_compensation.c`, `src/temperature_compensation.c`.

## FAO-56 Calculation Engine

- Reference ET0: Penman-Monteith with fallback to Hargreaves-Samani.
- Kc staging: Initial, Development, Mid-Season, End-Season.
- Water balance: RAW/TAW, deficit tracking, net irrigation requirement.
- Effective rainfall from history + soil infiltration factor.
- Volume conversion: mm -> liters using area or per-plant coverage.
- Constraints: max volume limit, Eco scaling (70%), minimum 1 liter enforced.

Implementation notes:

- FAO-56 calculations: `src/fao56_calc.c`, `src/fao56_custom_soil.c`.
- Water balance types: `src/water_balance_types.h`.

## Firmware Functions

- Task scheduling/queue + interval mode: duration/volume/auto tasks, pause/resume, cycle/soak sequencing.
- Valve + master valve control: GPIO sequencing, delays/overlap, manual overrides.
- FAO-56 irrigation engine: ET0, Kc staging, water balance, automatic volume decisions.
- Sensor stack + monitoring: flow/hydraulic anomalies, rain gauge/integration, BME280 telemetry.
- Persistence + storage: NVS configs/history, LittleFS history + pack storage, cleanup.
- BLE services + time/RTC: GATT handlers, fragmentation/notifications, timezone + freeze lockout, power pacing.

## Extensibility

- Modular C sources (watering, tasks, history, sensors, FAO calc, rain integration).
- Generated databases via Python scripts (`tools/build_database.py`).
- Pack storage and BLE pack service for custom plants and packs.

## Limitations and Behavior Notes

- Temperature compensation is not applied to task execution yet.
- BLE limits: Eco auto mode is not settable; auto-calc interval and longitude for solar timing are not exposed.
- Global rain integration config fields are deprecated; per-channel settings are used.
- Multi-task concurrent irrigation is not supported (single active valve only).
- Soil moisture probes are not supported (manual antecedent moisture estimates only).

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
| Pack service | `src/bt_pack_handlers.c`, `src/pack_storage.c`, `src/pack_schema.h` | Custom plants, pack transfers |
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
