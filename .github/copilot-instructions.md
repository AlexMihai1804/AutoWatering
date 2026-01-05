# AutoWatering Copilot Instructions

- **Scope**: Guide AI changes for the Zephyr 4.3-based smart irrigation firmware targeting Arduino Nano 33 BLE (nRF52840).
- **Version**: 3.1.0 (January 2026)

## System Overview
- **Entry Stack**: `src/main.c` sequences USB, NVS, sensors, compensation, histories, and finally `watering_start_tasks();` keep new init blocks before the main loop and honour existing warning patterns.
- **Core Engine**: Watering logic spans `watering.c`, `watering_tasks.c`, and `watering_internal.*`; tasks move through the `watering_tasks_queue` message queue, so avoid long blocking calls inside queue consumers.
- **Sensor & Compensation Layer**: Flow (`flow_sensor.c`), rain (`rain_sensor.c`, `rain_integration.c`), environmental (`env_sensors.c`, `temperature_compensation_integration.c`) each expose init + periodic APIs that main binds together; reuse those helpers instead of re-touching hardware directly.
- **Time Handling**: Relative timing uses `k_uptime_get_32()` while persisted timestamps run through `timezone_rtc_to_unix_utc()`; follow this split when adding timers or history records.

## BLE Layer
- **Services**: Two GATT services with **34 total characteristics**:
  - **Irrigation Service** (`12345678-1234-5678-1234-56789abcdef0`): 29 characteristics for valve control, scheduling, history, sensors, compensation.
  - **Custom Configuration Service** (`12345678-1234-5678-9abc-def123456780`): 5 characteristics for custom soil, soil moisture, config reset/status, interval mode.
- **Primary Service**: `bt_irrigation_service.c` implements 29 characteristics with hard-coded attribute indices (`ATTR_IDX_*`); inserting/removing characteristics requires renumbering the constants and keeping docs aligned.
- **Custom Config Service**: `bt_custom_soil_handlers.c` implements 5 additional characteristics for advanced configuration.
- **Notification System**: Use `SMART_NOTIFY`, `CRITICAL_NOTIFY`, or `CHANNEL_CONFIG_NOTIFY`; bypassing them breaks the adaptive throttling/priority behaviour.
- **Data Contracts**: Structures in `bt_gatt_structs_enhanced.h` and `bt_gatt_structs.h` are packed with size asserts; update `_EXPECTED_SIZE` and BLE read/write handlers when fields change.
- **History Handlers**: `bt_environmental_history_handlers.c`, `bt_custom_soil_handlers.c`, and similar files marshal module structs into BLE payloads—mirror their packing style for any new domain object.

## Persistence & Generated Data
- **NVS Wrappers**: All persistent writes go through `nvs_config.c` (`nvs_save_*` / `nvs_load_*`) so onboarding flags, enhanced configs, and water balance mirrors stay in sync—extend those helpers instead of writing raw `nvs_write` calls.
- **Channel Structs**: Changes to `watering_channel_t` demand matching updates in BLE packers, NVS serialization (`nvs_save_complete_channel_config`), and onboarding bookkeeping.
- **Generated Databases**: Plant/soil/method data in `plant_full_db.inc`, `soil_enhanced_db.inc`, `irrigation_methods_db.inc` comes from CSV via `python tools/build_database.py`; never edit the `.inc` files manually.
  - **Plant database**: 223 species (`PLANT_FULL_SPECIES_COUNT`)
  - **Soil database**: 15 enhanced types (`SOIL_ENHANCED_TYPES_COUNT`)
  - **Irrigation methods**: 15 entries (`IRRIGATION_METHODS_COUNT`)

## Build & Test Workflow
- **Hardware Build**: `west build -b arduino_nano_33_ble --pristine` (uses `boards/arduino_nano_33_ble.overlay` automatically via CMake).
- **Flash**: Double-tap reset to enter bootloader, then `west flash -r bossac`.
- **Config Variants**: Toggle feature flags in `prj.conf`; `prj_backup.conf` holds the last known-good defaults when experimenting.
- **Logs**: Every module relies on Zephyr logging (`LOG_MODULE_REGISTER`); prefer `LOG_DBG/LOG_INF/LOG_WRN` over `printk` unless bootstrapping early init.

## Coding Patterns & Conventions
- **Threading**: Background work typically uses Zephyr work queues or message queues; favour those over bespoke threads unless there is a clear need.
- **Error Flow**: Return `watering_error_t` or Zephyr `-errno` consistently, and propagate warnings via `printk` + `LOG_*` exactly as surrounding code does to preserve the existing telemetry cadence.
- **Fragmentation**: Large BLE payloads rely on the fragmentation helpers already in the repo; reuse existing fragment builders instead of inventing new formats.
- **Documentation Sync**: When altering protocol shapes or init ordering, reflect the change in `docs/system-architecture.md` and the relevant BLE docs under `docs/ble-api/`.

## Key Files Reference
| Component | Files |
|-----------|-------|
| Main entry | `src/main.c` |
| Watering engine | `src/watering.c`, `src/watering_tasks.c`, `src/watering_internal.*` |
| BLE Irrigation Service | `src/bt_irrigation_service.c` (29 chars) |
| BLE Custom Config | `src/bt_custom_soil_handlers.c` (5 chars) |
| NVS persistence | `src/nvs_config.c` |
| FAO-56 calculations | `src/fao56_calc.c` |
| Board overlay | `boards/arduino_nano_33_ble.overlay` |

## When Unsure
- **Reference**: `docs/system-architecture.md` captures the verified architecture; `docs/TROUBLESHOOTING.md` lists runtime expectations that new code must respect.
- **BLE API**: Full characteristic documentation in `docs/ble-api/` with per-characteristic files in `docs/ble-api/characteristics/`.
- **Ask**: Highlight uncertainties (e.g., new characteristic UUIDs, storage offsets) in review notes so maintainers can confirm before merging.
