# AutoWatering System Architecture (October 2025)

AutoWatering is a Zephyr-based irrigation controller targeted at the nRF52840. This document maps the current firmware structure as validated in `src/main.c` and the associated modules.

## High-Level Stack

```mermaid
graph LR
    subgraph Client
        CLIENT[BLE client (mobile/desktop)]
    end
    subgraph Communication
        BLE[Zephyr BLE peripheral]
        FRAG[Fragmentation helpers]
        NOTIFY[Notification throttling]
    end
    subgraph Application
        TASKS[watering_tasks.c]
        AUTO[fao56_calc.c / watering.c]
        CFG[nvs_config.c]
        HIST[history modules]
    end
    subgraph Sensors & Drivers
        FLOW[flow_sensor.c]
        RAIN[rain_sensor.c]
        ENV[env_sensors.c]
        VALVE[valve_control.c]
        RTC[rtc.c]
    end
    CLIENT --> BLE
    BLE --> FRAG
    BLE --> NOTIFY
    FRAG --> TASKS
    NOTIFY --> TASKS
    TASKS --> VALVE
    TASKS --> HIST
    FLOW --> TASKS
    RAIN --> TASKS
    ENV --> AUTO
    AUTO --> TASKS
    AUTO --> HIST
    CFG --> TASKS
    CFG --> HIST
    RTC --> AUTO
```

## Boot & Initialization

`main.c` performs a deterministic start-up sequence:

1. USB CDC ACM (optional console) via `setup_usb_cdc_acm()`.
2. Persistent storage: `nvs_config_init()`.
3. Valve hardware (`valve_init()`), flow sensor, and rain sensor drivers.
4. RTC (`rtc_init()` with uptime fallback) and timezone helpers (UTC baseline only).
5. Core watering engine: `watering_init()` followed by `watering_start_tasks()` (spawns processing threads and work items).
6. Support services: storage monitor, configuration status, onboarding state, reset controller, enhanced status/error handlers.
7. Sensor stack: `sensor_manager`, BME280 driver, environmental data + history modules.
8. Compensation layers: custom soil DB, rain compensation, temperature compensation & integration, interval task integration.
9. History subsystems: watering history, rain history, rain integration.
10. BLE service (`bt_irrigation_service_init()`) when `CONFIG_BT` is enabled.

All module initializations log warnings on failure but keep the system running unless NVS cannot be mounted.

## Runtime Components

### Watering Engine
- `watering_tasks_queue` (depth 10) feeds `watering_start_task()`; only one task is active at a time.
- Modes: duration, volume, automatic quality, automatic eco (70% reduction) with temperature compensation hooks.
- Interval mode support integrates through `interval_task_integration.c` and `interval_mode_controller.c`.
- Master valve sequencing and overlap handling implemented in `valve_control.c`.

### Scheduling & Automation
- Daily bitmask and periodic schedules (`schedule_type_t`) converted to queued tasks.
- FAO-56 calculations executed on demand (`apply_quality_irrigation_mode`, `apply_eco_irrigation_mode`) with maximum volume limiting.
- Rain integration (`rain_integration_calculate_impact`) applies volume/time reductions or skips before tasks start.
- Temperature compensation adjusts FAO-56 outputs when enabled per channel (`temperature_compensation_integration.c`).

### Sensors & Telemetry
- Flow pulses captured via GPIO interrupts (`flow_sensor.c`) with calibration and no-flow/unexpected-flow alarms.
- Rain gauge pulses recorded with hourly/daily aggregates (`rain_sensor.c`, `rain_history.c`).
- Environmental snapshots collected from the BME280 (`env_sensors.c`, `environmental_history.c`).
- RTC access provided by `rtc.c` with uptime fallback when the DS3231 is unavailable.

### Persistence & Histories
- All writes/read flow through `nvs_config.c`; individual modules own their keys.
- `watering_history.c`: 30 detailed events/channel, 90 daily, 36 monthly, 10 annual summaries plus optional insights cache.
- `rain_history.c` and `environmental_history.c` maintain rolling datasets for BLE export.
- Generated databases (`plant_full_db.inc`, `soil_enhanced_db.inc`, `irrigation_methods_db.inc`) are included directly with compile-time size asserts.

### BLE Interface
- `bt_irrigation_service.c` exposes 33 characteristics (26 irrigation + 7 history) documented under `docs/ble-api/`.
- Notification macros (`SMART_NOTIFY`, `CRITICAL_NOTIFY`, `CHANNEL_CONFIG_NOTIFY`) enforce throttling tiers.
- Large payloads (channel config, histories) use the shared fragmentation helpers with 3-byte headers.
- History control/insights characteristics mirror the structures defined in the history modules; settings persist through NVS.

#### BLE Notification Priority System
- **Priority Classes**:
  - `NOTIFY_PRIORITY_CRITICAL` (0ms): Alarms, errors - immediate delivery.
  - `NOTIFY_PRIORITY_HIGH` (50ms): Status updates, valve changes.
  - `NOTIFY_PRIORITY_NORMAL` (200ms): Flow data, statistics.
  - `NOTIFY_PRIORITY_LOW` (1000ms): History, diagnostics.
- **Mechanism**: `safe_notify` (aliased to `advanced_notify`) uses a buffer pool (`BLE_BUFFER_POOL_SIZE=8`) and adaptive throttling.
- **Macros**: `SMART_NOTIFY` handles standard throttling, `CRITICAL_NOTIFY` bypasses checks for urgent alerts, `CHANNEL_CONFIG_NOTIFY` handles name change throttling.

### Diagnostics & Power
- Status codes (`WATERING_STATUS_*`) published via BLE and logs; leak detection relies on flow anomaly detection.
- Power modes (`POWER_MODE_NORMAL`, `POWER_MODE_ENERGY_SAVING`, `POWER_MODE_ULTRA_LOW_POWER`) adjust sleep intervals inside monitoring loops.
- Enhanced error handling and reset controller modules centralize fault logging and recovery entry points.
- Periodic memory statistics logged from the idle loop every 10 minutes.

## Threading Model

- Watering processing relies on Zephyr work queue callbacks plus the message queue for tasks.
- Flow and rain sensors use interrupt callbacks with minimal work handler scheduling.
- History garbage collection runs in a dedicated thread (`watering_history.c`).
- BLE host/controller threads follow Zephyr defaults; stack sizes are increased in `prj.conf` to handle fragmentation workloads.

## Data & Control Flows

### Task Lifecycle

```mermaid
sequenceDiagram
    participant BLE as BLE Client
    participant SVC as bt_irrigation_service
    participant CORE as watering.c / watering_tasks.c
    participant VALVE as valve_control.c
    participant HIST as watering_history.c

    BLE->>SVC: Write task command
    SVC->>CORE: Enqueue `watering_task_t`
    CORE->>CORE: rain_integration_calculate_impact()
    alt Skip due to rainfall
        CORE-->>BLE: Error (skip)
    else Proceed
        CORE->>VALVE: master valve + channel sequencing
        CORE->>HIST: watering_history_record_task_start()
        loop During task
            VALVE-->>CORE: Flow pulse updates
            CORE-->>BLE: SMART_NOTIFY current task status
        end
        CORE->>HIST: watering_history_record_task_complete()
        CORE-->>BLE: Completion notification
    end
```

### Sensor Data Pipeline

```mermaid
graph LR
    RAW[Sensor pulses/readings] --> CAL[Calibration & validation]
    CAL --> STORE[History modules]
    CAL --> COMP[Compensation engines]
    COMP --> TASKS
    STORE --> BLEOUT[BLE history characteristics]
```

## Known Limits

- 8 irrigation channels; concurrency is single-task by design.
- One BLE connection at a time (`CONFIG_BT_MAX_CONN=1`).
- Plant database holds 223 entries (44-byte structs).
- History retention: detailed last 30 events/channel, daily 90 days, monthly 36 months, annual 10 years.
- Insights data exists but requires external producers (`history_insights_update`).

## Open Follow-Ups

- Automated generation of insights and advanced analytics is pending.
- Rain skip currently returns `WATERING_ERROR_BUSY`; explicit "skipped" history logging may be desirable.
- External weather ingestion, secure boot, and OTA update paths remain deferred (see `docs/CHANGELOG.md`).

This architecture description reflects only modules and flows present in the repository as of October 2025, reducing drift between documentation and firmware behavior.
