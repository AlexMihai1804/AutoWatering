# Documentation Audit TODOs (2025-10-16)
Curated checklist covering the active documentation set. Each entry records the current verification status against the firmware and the follow-up item to keep the file aligned with future work.
## Latest Pass (2025-10-16)
- Validated & updated: 05-schedule-configuration, 06-system-configuration, 07-task-queue-management, 08-statistics, 10-alarm-status, 11-calibration-management, 14-growing-environment, 21-environmental-data, 22-environmental-history, 23-compensation-status, 24-rain-integration-status, 02-flow-sensor, 03-system-status.
- Firmware touch-up: `bt_irrigation_service.c` environmental-data notifications now respect the 23-byte advanced-notify ceiling.
## Platform & Architecture Docs
- [x] `docs/FEATURES_FULL.md`     Updated threading/flow-sensor sections to match `watering_start_tasks()` and `flow_sensor.c`. **TODO:** Fold rain/temperature compensation auto-pipeline wiring into this overview once implemented.
- [x] `docs/FEATURES.md`     Matches published capabilities (queue gating, single active task). **TODO:** Revisit when simultaneous-valve support graduates from design notes.
- [x] `docs/system-architecture.md`     Data paths and queue orchestration reflect current `watering_tasks.c`. **DONE 2025-11-25:** Added details for `safe_notify` priority classes and adaptive throttling.
- [x] `docs/INSTALLATION.md`     Tooling & Zephyr version instructions align with current `CMakeLists.txt`. **TODO:** Capture Windows-specific west init nuances observed during recent setup runs.
- [x] `docs/plant-database-fao56-system.md`     Database sizing and CSV regeneration scripts match `tools/` outputs. **TODO:** Document checksum/validation step once dataset auto-tests land.
## BLE Service Overview
- [x] `docs/ble-api/README.md`     Characteristic index and UUID mapping consistent with `bt_irrigation_service.c`. **TODO:** Add link to enhanced structs guard rails when Context7 export stabilises.
- [x] `docs/ble-api/IMPLEMENTATION_STATUS.md`     Status flags mirror live handlers. **TODO:** Update once compensation auto-apply transitions from roadmap to code.
- [x] `docs/ble-api/protocol-specification.md`     End-to-end protocol diagrams match fragmentation helpers. **TODO:** Extend with adaptive notification timing table after throttling metrics are gathered.
- [x] `docs/ble-api/ENHANCED_STRUCTS_INTERNAL.md`     Packed layout verified against `bt_gatt_structs_enhanced.h`. **TODO:** Append checksum guidance if struct revision bumps beyond BUILD_ASSERT guard.
- [x] `docs/ble-api/_fragmentation-reference.md`     Header formats validated against channel/growing env writers. **TODO:** Include rain-history TLV example when chunking logic stops fluctuating.
## BLE Characteristics (per UUID)
- [x] `docs/ble-api/characteristics/01-valve-control.md`     Struct and command paths align with `struct valve_control_data`. **DONE 2025-10-16:** Documented manual master-valve rejects as BT_ATT_ERR_WRITE_NOT_PERMITTED.
- [x] `docs/ble-api/characteristics/02-flow-sensor.md`     Notification cadence and calibration limits match `flow_sensor.c`. **DONE 2025-10-16:** Highlighted statistics refresh path via bt_irrigation_update_statistics_from_flow().
- [x] `docs/ble-api/characteristics/03-system-status.md`     Payload mirrors `system_status_data` population. **TODO:** Document new enhanced status bitmaps when `enhanced_system_status_info_t` exports via BLE.
- [x] `docs/ble-api/characteristics/04-channel-configuration.md`     Basic struct verified against `channel_config_data`. **DONE 2025-10-16:** Added cross-link advising simultaneous updates with Growing Environment.
- [x] `docs/ble-api/characteristics/05-schedule-configuration.md`     Interval handling matches `schedule_config_data`; queue back-pressure behaviour now documented.
- [x] `docs/ble-api/characteristics/06-system-configuration.md`     Snapshot confirmed against `enhanced_system_config_data`. **DONE 2025-10-16:** Added troubleshooting entry for BME280 configuration failures.
- [x] `docs/ble-api/characteristics/07-task-queue-management.md`     Control commands align with `task_queue_value` handlers. **DONE 2025-11-25:** Confirmed `active_task_id` uses simple 0/1 logic (no complex recycling).
- [x] `docs/ble-api/characteristics/08-statistics.md`     Content matches `statistics_data` updates. **DONE:** No rolling averages used; simple estimation fallback.
- [x] `docs/ble-api/characteristics/13-diagnostics.md`    Content matches `diagnostics_data` updates. **DONE:** Stack usage not reported (reserved bytes zeroed).
- [x] `docs/ble-api/characteristics/16-current-task-status.md` Content matches `current_task_data` updates. **DONE:** Pause/Resume logic clarified (Interval wait = Running).
- [x] `docs/ble-api/characteristics/09-rtc-configuration.md`     RTC read/write semantics match timezone helpers. **DONE 2025-10-16:** Added DST edge-case write example.
- [x] `docs/ble-api/characteristics/10-alarm-status.md`     Codes align with `watering_monitor.c` enums. **DONE 2025-10-16:** Linked to recovery strategy section in system architecture doc.
- [x] `docs/ble-api/characteristics/11-calibration-management.md`     Actions map to calibration handler logic. **DONE 2025-10-16:** Documented BT_ATT_ERR_UNLIKELY path when NVS persistence fails.
- [x] `docs/ble-api/characteristics/12-history-management.md`     TLV framing matches `history_fragment_header_t`. **TODO:** Include rain-history eject examples after retention policy lands.
- [x] `docs/ble-api/characteristics/14-growing-environment.md`     Struct validated against `growing_env_data`. **DONE 2025-10-16:** Validation matrix already covers coverage/index constraints; updated as part of audit.
- [x] `docs/ble-api/characteristics/15-auto-calc-status.md`     Layout confirmed with `auto_calc_status_data`. **TODO:** Capture heuristics for `next_irrigation_time` when prediction service matures.
- [x] `docs/ble-api/characteristics/17-timezone-configuration.md`     Matches timezone persistence helpers. **DONE 2025-10-16:** Added DST/UTC offset transition write example.
- [x] `docs/ble-api/characteristics/18-rain-sensor-config.md`     Config struct aligns with rain sensor storage. **DONE 2025-10-16:** Validation table calls out 0.1-10.0 mm/pulse calibration and 10-1000 ms debounce guard rails.
- [x] `docs/ble-api/characteristics/19-rain-sensor-data.md`     Snapshot matches `rain_data_data`. **DONE 2025-10-16:** Expanded client guidance on `data_quality` signalling.
- [x] `docs/ble-api/characteristics/20-rain-history-control.md`     Command struct verified against history worker. **DONE 2025-10-16:** Added multi-fragment JavaScript example with TLV ack handling.
- [x] `docs/ble-api/characteristics/25-onboarding-status.md`     Mirrors onboarding ring buffers and flags. **TODO:** Update when onboarding completion thresholds change with new mandatory fields.
- [x] `docs/ble-api/characteristics/26-reset-control.md`     Control flow matches `reset_controller` integration. **TODO:** Attach cautionary note for manual master-valve resets once UI confirmation flow finalises.
- [x] `docs/ble-api/characteristics/21-environmental-data.md`     Fields align with `environmental_data_ble`. **DONE 2025-10-16:** Client guidance now references CONFIG_ENV_SENSORS_SIM simulator.
- [x] `docs/ble-api/characteristics/22-environmental-history.md`     Fragment framing matches environmental history handlers. **DONE 2025-10-16:** Noted 247-byte MTU fragment size and ~50 ms spacing.
- [x] `docs/ble-api/characteristics/23-compensation-status.md`     Struct verified against `compensation_status_data`. **TODO:** Extend with channel bitmap summary once multi-channel push gets wired.
