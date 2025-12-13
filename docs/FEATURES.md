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

### Sensing & Monitoring
- **Flow sensor**: Pulse counting with calibration (100–10,000 pulses/liter; adjustable via BLE).
- **Rain gauge**: Tipping-bucket with 0.2 mm/pulse default, debounce, health monitoring.
- **Environmental sensor** (BME280): Temperature, humidity, pressure; 15/60 min polling intervals.
- Environmental, rain, and watering history with multi-resolution aggregation.
- Current task progress & completion notifications via BLE.

### Data & Persistence (NVS + LittleFS)
- Channel configuration, calibration, schedules, custom soil, compensation settings stored in NVS.
- Plant database: 223 entries (`PLANT_FULL_SPECIES_COUNT`).
- Soil database: 15 enhanced soils (`SOIL_ENHANCED_TYPES_COUNT`).
- Irrigation methods database: 15 entries (`IRRIGATION_METHODS_COUNT`).
- Watering history (NVS): 30 events/channel + 90 daily + 36 monthly + 10 annual.
- Rain + environmental history (external flash via LittleFS when `CONFIG_HISTORY_EXTERNAL_FLASH=y`): rain (720 hourly + 1825 daily), environmental (720 hourly + 372 daily + 60 monthly).
- Automatic NVS storage cleanup (80/90% thresholds, target 70%); LittleFS provides wear levelling for flash-backed history files.

### Master Valve
- Optional master valve with pre-start (default +3 s) and post-stop delays (default +2 s).
- Overlap grace window (5 s) to keep master open between consecutive tasks.
- Automatic or manual (when auto disabled) control paths.

### Power Modes
- **Normal**: 60 s scheduler sleep.
- **Energy Saving**: 120 s scheduler sleep.
- **Ultra Low Power**: 300 s scheduler sleep.

### Bluetooth Low Energy
- Custom irrigation service: **27 documented characteristics** (`docs/ble-api/`).
- Notification scheduler: 8×23-byte buffer pool, priority throttling (critical 0 ms, normal 200 ms, low 1 s).
- Fragmentation for large payloads (TLV-framed, sequence-numbered).
- History streaming via write-triggered fragment notifications (no client ACK).

### Time Handling
- DS3231 RTC integration using UTC timestamps for scheduling and history.
- Timezone configuration with DST support (UTC offset, start/end rules).
- Fallback to uptime-derived time if RTC unavailable (5 consecutive failures trigger fallback).

### Error & Status Reporting
- Status codes: OK, No-Flow, Unexpected-Flow, Fault, RTC Error, Low Power.
- Enhanced system status: interval phase, compensation flags, sensor health, configuration completeness.
- Error recovery strategies (retry, fallback, disable, reset, graceful degrade).
- Rain-based skip events logged via history helpers.

### Onboarding & Reset System
- Configuration scoring per channel (0–100%, weighted groups).
- Onboarding flags: plant, soil, method, coverage, sun exposure, name, compensation, latitude, planting date.
- Reset controller with confirmation codes (5 min validity): channel, schedule, system, calibration, history, factory.
- Reset log (16 entries/channel) with timestamps and reasons.

### Compensation Systems
- **Rain compensation** (TIME/VOLUME modes only):
  - Algorithms: simple, proportional, exponential, adaptive.
  - Channel-specific sensitivity, skip threshold, lookback hours.
- **Temperature compensation** (TIME/VOLUME modes only):
  - Base temperature, sensitivity factor, min/max factors.
  - Default: 20°C base, 0.05 sensitivity, 0.5–2.0 factor range.

### Extensibility
- Modular C sources (watering, tasks, history, sensors, FAO calc, rain integration).
- Generated databases (plant, soil, irrigation methods) via Python scripts (`tools/build_database.py`).
- Hardware targets: nRF52840 (promicro_nrf52840, arduino_nano_33_ble) and native_sim.

### Not Implemented (Removed From Marketing)
- Background FAO thread (calculations are on-demand).
- Generic memory/health monitoring subsystems.
- Multi-task concurrent irrigation (single active task enforced).
- Soil moisture probes (fields reserved for backward compatibility).

This concise sheet avoids speculative metrics (latency, throughput) until measured tests are added.
