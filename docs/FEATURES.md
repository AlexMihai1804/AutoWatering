## AutoWatering - Key Features (Code-Verified Summary)

Focused, externally facing list. All items map to existing modules or confirmed limits.
*Last updated: 2025-12-13*

---

### Core Control
- 8 independent irrigation channels (`WATERING_CHANNELS_COUNT`).
- Four watering modes:
  - **TIME** (`WATERING_BY_DURATION`): Fixed duration in minutes.
  - **VOLUME** (`WATERING_BY_VOLUME`): Fixed volume in liters (flow-sensor monitored).
  - **Quality** (`WATERING_AUTOMATIC_QUALITY`): FAO-56 based, 100% of calculated requirement.
  - **Eco** (`WATERING_AUTOMATIC_ECO`): FAO-56 based, 70% of calculated requirement.
- Quality and Eco modes are **exclusively FAO-56 based** (require plant/soil/method configuration).
- Single active watering task at a time (message queue dispatch, depth 10).
- Interval mode: watering/pause phase cycling for TIME and VOLUME modes (configurable minutes/seconds).

Implementation notes (where this lives in code):
- Task orchestration + scheduling loops: `src/watering_tasks.c`, `src/watering.c`.
- Valve/master-valve control path: `src/valve_control.c`, `src/watering.c`.
- Interval-mode state machine: `src/interval_mode_controller.c`, `src/interval_task_integration.c`.

### Scheduling & Automation
- Three schedule modes per channel:
  - **Daily (bitmask)**: Runs on selected days of the week.
  - **Periodic**: Runs every N days.
  - **AUTO (Smart Schedule)**: FAO-56 based - evaluates soil water deficit daily, irrigates only when deficit exceeds plant's RAW threshold. Requires plant/soil/planting_date configuration.
- On-demand FAO-56 based irrigation requirement calculation (`watering_run_automatic_calculations()`).
- AUTO mode features:
  - Daily deficit tracking with ETc (crop evapotranspiration) accumulation.
  - Environmental stress adjustment on hot/dry days (reduced MAD threshold).
  - Automatic rainfall integration (subtracts effective precipitation from deficit).
  - Multi-day offline gap handling: estimates missed ETc on power-up.
- Rain integration: skip / reduction logic (channel-specific) using recent rainfall history.
  - ⚠️ Rain Skip and Temperature Compensation apply **only to TIME and VOLUME modes**.
  - FAO-56 modes (Quality/Eco/AUTO) already incorporate rain and temperature in ET₀ calculations.
- Configurable auto-calculation interval (1–24 hours).

Implementation notes:
- Schedule storage and scheduling logic: `src/watering_config.c`, `src/watering_tasks.c`.
- AUTO (FAO-56) deficit tracking + daily update loop: `src/fao56_calc.c`, `src/watering.c`.
- Time conversion (RTC + timezone rules): `src/rtc.c`, `src/timezone.c`.

### Sensing & Monitoring
- **Flow sensor**: Pulse counting with calibration (100–10,000 pulses/liter; adjustable via BLE).
- **Rain gauge**: Tipping-bucket with 0.2 mm/pulse default, debounce, health monitoring.
- **Environmental sensor** (BME280): Temperature, humidity, pressure; 15/60 min polling intervals.
- Environmental, rain, and watering history with multi-resolution aggregation.
- Current task progress & completion notifications via BLE.

Implementation notes:
- Flow sensor (pulse counting, anomaly detection hooks): `src/flow_sensor.c`.
- Rain gauge, integration, and history: `src/rain_sensor.c`, `src/rain_integration.c`, `src/rain_history.c`.
- Environmental sensing and aggregation: `src/env_sensors.c`, `src/environmental_data.c`, `src/environmental_history.c`.

### Data & Persistence (NVS + LittleFS)
- Channel configuration, calibration, schedules, custom soil, compensation settings stored in NVS.
- Plant database: 223 entries (`PLANT_FULL_SPECIES_COUNT`).
- Soil database: 15 enhanced soils (`SOIL_ENHANCED_TYPES_COUNT`).
- Irrigation methods database: 15 entries (`IRRIGATION_METHODS_COUNT`).
- Watering history (NVS): 30 events/channel + 90 daily + 36 monthly + 10 annual.
- Rain + environmental history (external flash via LittleFS when `CONFIG_HISTORY_EXTERNAL_FLASH=y`): rain (720 hourly + 1825 daily), environmental (720 hourly + 372 daily + 60 monthly).
- Hourly rain history is recorded on UTC hour rollover (including 0.00 mm hours); daily summaries are derived from hourly records.
- Environmental history is auto-aggregated hourly/daily/monthly from BME280 snapshots + rain/watering history.
- Automatic NVS storage cleanup (80/90% thresholds, target 70%); LittleFS provides wear levelling for flash-backed history files.

Implementation notes:
- NVS read/write wrappers (single source of truth): `src/nvs_config.c`.
- NVS usage monitoring/cleanup: `src/nvs_storage_monitor.c`.
- External flash history + filesystem mount: `src/history_flash.c`.
- Optional binary DB on LittleFS (`/lfs/db/*.bin`): `src/database_flash.c`.

### Master Valve
- Optional master valve with pre-start (default +3 s) and post-stop delays (default +2 s).
- Overlap grace window (5 s) to keep master open between consecutive tasks.
- Automatic or manual (when auto disabled) control paths.

Implementation notes:
- Master valve gating and delays: `src/valve_control.c`, plus config in `src/watering_config.c`.

### Power Modes
- **Normal**: 60 s scheduler sleep.
- **Energy Saving**: 120 s scheduler sleep.
- **Ultra Low Power**: 300 s scheduler sleep.

Implementation notes:
- Sleep pacing and power mode switching: `src/power_management.c`, scheduler loop in `src/watering_tasks.c`.

### Bluetooth Low Energy
- Custom irrigation service: **27 documented characteristics** (`docs/ble-api/`).
- Notification scheduler: 8×23-byte buffer pool, priority throttling (critical 0 ms, normal 200 ms, low 1 s).
- Fragmentation for large payloads (TLV-framed, sequence-numbered).
- History streaming via write-triggered fragment notifications (no client ACK).

Implementation notes:
- GATT service/characteristic table: `src/bt_irrigation_service.c`.
- Domain handlers (channel config, interval mode, histories, etc.): `src/bt_*_handlers.c`.
- Packed BLE structs/contracts: `src/bt_gatt_structs.h`, `src/bt_gatt_structs_enhanced.h`.

### Time Handling
- DS3231 RTC integration using UTC timestamps for scheduling and history.
- Timezone configuration with DST support (UTC offset, start/end rules).
- Fallback to uptime-derived time if RTC unavailable (5 consecutive failures trigger fallback).

Implementation notes:
- RTC I/O + failure handling: `src/rtc.c`.
- Timezone/DST conversion helpers: `src/timezone.c`.

### Error & Status Reporting
- Status codes: OK, No-Flow, Unexpected-Flow, Fault, RTC Error, Low Power.
- Enhanced system status: interval phase, compensation flags, sensor health, configuration completeness.
- Error recovery strategies (retry, fallback, disable, reset, graceful degrade).
- Rain-based skip events logged via history helpers.

Implementation notes:
- Enhanced status aggregation: `src/enhanced_system_status.c`.
- Error classification/recovery glue: `src/enhanced_error_handling.c`, plus per-module safety checks (flow/rain/etc.).

### Onboarding & Reset System
- Configuration scoring per channel (0–100%, weighted groups).
- Onboarding flags: plant, soil, method, coverage, sun exposure, name, compensation, latitude, planting date.
- Reset controller with confirmation codes (5 min validity): channel, schedule, system, calibration, history, factory.
- Reset log (16 entries/channel) with timestamps and reasons.

Implementation notes:
- Onboarding/completeness scoring: `src/onboarding_state.c`, `src/configuration_status.c`.
- Reset opcodes + confirmation flow: `src/reset_controller.c`.

### Compensation Systems
- **Rain compensation** (TIME/VOLUME modes only):
  - Algorithms: simple, proportional, exponential, adaptive.
  - Channel-specific sensitivity, skip threshold, lookback hours.
- **Temperature compensation** (TIME/VOLUME modes only):
  - Base temperature, sensitivity factor, min/max factors.
  - Default: 20°C base, 0.05 sensitivity, 0.5–2.0 factor range.

  Implementation notes:
  - Rain compensation and integration: `src/rain_compensation.c`, `src/rain_integration.c`.
  - Temperature compensation pipeline: `src/temperature_compensation.c`, `src/temperature_compensation_integration.c`.

### Extensibility
- Modular C sources (watering, tasks, history, sensors, FAO calc, rain integration).
- Generated databases (plant, soil, irrigation methods) via Python scripts (`tools/build_database.py`).
- Hardware targets: nRF52840 (promicro_nrf52840, arduino_nano_33_ble) and native_sim.

Implementation notes:
- Generated databases are committed `.inc` artefacts; sources are CSVs + generators under `tools/`.
- Hardware-specific storage sizing/mapping is defined in `boards/*.overlay`.

### Not Implemented (Removed From Marketing)
- Background FAO thread (calculations are on-demand).
- Generic memory/health monitoring subsystems.
- Multi-task concurrent irrigation (single active task enforced).
- Soil moisture probes (fields reserved for backward compatibility).

This concise sheet avoids speculative metrics (latency, throughput) until measured tests are added.

---

### Future (Roadmap)

Implementation work is tracked in the GitHub Project roadmap:
- https://github.com/users/AlexMihai1804/projects/2

Firmware roadmap items (grouped by implementation batches):
- **B0 Core fixes**: https://github.com/AlexMihai1804/AutoWatering/issues/1, https://github.com/AlexMihai1804/AutoWatering/issues/2, https://github.com/AlexMihai1804/AutoWatering/issues/3, https://github.com/AlexMihai1804/AutoWatering/issues/4, https://github.com/AlexMihai1804/AutoWatering/issues/5
- **B1 Storage foundation**: https://github.com/AlexMihai1804/AutoWatering/issues/10, https://github.com/AlexMihai1804/AutoWatering/issues/6
- **B2 Device packs MVP**: https://github.com/AlexMihai1804/AutoWatering/issues/7, https://github.com/AlexMihai1804/AutoWatering/issues/8, https://github.com/AlexMihai1804/AutoWatering/issues/12
- **B4 Updates**: https://github.com/AlexMihai1804/AutoWatering/issues/13
- **B5 Custom plant integration**: https://github.com/AlexMihai1804/AutoWatering/issues/9
- **B7 Cycle & soak support (firmware dependency for app auto-tuning)**: https://github.com/AlexMihai1804/AutoWatering/issues/14
