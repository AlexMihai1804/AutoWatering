## AutoWatering - Key Features (Code-Verified Summary)

Focused, externally facing list. All items map to existing modules or confirmed limits.

### Core Control
- 8 independent irrigation channels (`WATERING_CHANNELS_COUNT`).
- Four watering modes:
  - **TIME** (`WATERING_BY_DURATION`): Fixed duration in minutes.
  - **VOLUME** (`WATERING_BY_VOLUME`): Fixed volume in liters.
  - **Quality** (`WATERING_AUTOMATIC_QUALITY`): FAO-56 based, 100% of calculated requirement.
  - **Eco** (`WATERING_AUTOMATIC_ECO`): FAO-56 based, 70% of calculated requirement.
- Quality and Eco modes are **exclusively FAO-56 based** (require plant/soil/method configuration).
- Single active watering task at a time (message queue dispatch).

### Scheduling & Automation
- Daily (bitmask) or periodic (every N days) scheduling per channel.
- On-demand FAO-56 based irrigation requirement calculation (`watering_run_automatic_calculations()` + `fao56_calc.*`).
- Rain integration: skip / reduction logic (channel-specific) using recent rainfall history.
  - ⚠️ Rain Skip and Temperature Compensation apply **only to TIME and VOLUME modes**.
  - FAO-56 modes (Quality/Eco) already incorporate rain and temperature in ET₀ calculations.

### Sensing & Monitoring
- Flow pulse counting with calibration (default pulses/liter; adjustable via API/BLE).
- Environmental sensing (BME280) + environmental history.
- Rain gauge pulse integration with 24h/48h lookback for adjustments.
- Current task progress & completion notifications.

### Data & Persistence
- Channel configuration + calibration stored in NVS (module helpers; no generic key framework).
- Plant database: 223 entries (`plant_full_database`, 44-byte struct, scaled integers).
- Separate purpose-built histories: watering, rain, environmental.

### Master Valve
- Optional master valve with pre-start / post-stop delays and overlap grace management.
- Automatic or manual (when auto disabled) control paths.

### Power & Modes
- Power modes influencing task loop sleep: Normal, Energy Saving, Ultra Low (no deep PM state manager abstraction).

### Bluetooth Low Energy
- Custom irrigation service plus history service: 33 documented characteristics (`docs/ble-api/`).
- Notification support uses `SMART_NOTIFY` / `CRITICAL_NOTIFY` macros for priority-aware throttling.
- Fragmentation present for larger payloads (characteristic-level handling).

### Time Handling
- RTC integration using UTC timestamps for scheduling and history.
- Fallback to uptime-derived time if RTC unavailable.

### Error & Status Reporting
- Status codes: OK, No-Flow, Unexpected-Flow, Fault, RTC Error, Low Power.
- Rain-based skip events logged via history helpers.

### Extensibility
- Modular C sources (watering, tasks, history, sensors, FAO calc, rain integration).
- Generated databases (plant, soil, irrigation methods) via Python scripts.

### Not Implemented (Previously Claimed - Removed From Marketing)
- Background FAO thread (calculations are on-demand).
- Generic memory/health monitoring subsystems.
- Multi-task concurrent irrigation (single active task enforced).

This concise sheet avoids speculative metrics (latency, throughput) until measured tests are added.
