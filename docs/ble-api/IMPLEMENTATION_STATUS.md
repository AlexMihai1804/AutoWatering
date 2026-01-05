# BLE Implementation Status (Audit 2026-01-04)

This file summarizes the current state (code vs documentation) of the GATT characteristics defined in `bt_irrigation_service.c` and `bt_custom_soil_handlers.c`.

Legend: R=Read, W=Write, N=Notify, F=Custom fragmentation (4B header), H=Unified history 8B header, LW=Long Write (offset accumulation), S=Single full write (if negotiated MTU >= struct size)

## Irrigation Service (29 characteristics)

| # | Caracteristica | Struct cod | Proprietati | Dimensiune | Fragmentare | Observatii |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | Valve Control | valve_control_data | R/W/N | 4 B | - | Struct consistent |
| 2 | Flow Sensor | (internal snapshot) | R/N | 4 B* | - | Read/notify only (*packed representation) |
| 3 | System Status | (internal byte) | R/N | 1 B | - | Snapshot byte |
| 4 | Channel Configuration | channel_config_data | R/W/N | 76 B | F + S | Header types 1/2/3 (name-only & full) |
| 5 | Schedule Configuration | schedule_config_data | R/W/N | 9 B | LW | First write byte selects channel for subsequent read |
| 6 | System Configuration (Enhanced) | enhanced_system_config_data | R/W/N | 56 B | LW + S | Offset-based accumulation (no custom header) |
| 7 | Task Queue Management | task_queue_data | R/W/N | 9 B | - | `task_id_to_delete` not yet used |
| 8 | Statistics | statistics_data | R/W/N | 15 B | - | Volatile (reset on reboot) |
| 9 | RTC Configuration | rtc_data | R/W/N | 16 B | - | Full write only (no partial semantics) |
| 10 | Alarm Status | alarm_data | R/W/N | 7 B | - | Write clears alarm codes |
| 11 | Calibration Management | calibration_data | R/W/N | 13 B | - | Actions 0-3 implemented |
| 12 | History Management | history_data | R/W/N | 12 B cmd, 32 B notify | H | 12-byte write buffer issues queries; read and real-time notifications return `struct history_data`; bulk responses stream via unified header |
| 13 | Diagnostics | diagnostics_data | R/N | 12 B | - | Snapshot |
| 14 | Growing Environment | growing_env_data | R/W/N | 71 B | F + S | Header types 2/3 |
| 15 | Auto Calc Status | auto_calc_status_data | R/W/N | 64 B | H | 1B channel select write (no custom 4B write header); Notify sends unified 8B header + 64B payload |
| 16 | Current Task Status | current_task_data | R/W/N | 21 B | - | Struct consistent |
| 17 | Timezone Configuration | rtc_data subset / timezone | R/W/N | 16 B | - | Mirrors rtc_data layout |
| 18 | Rain Sensor Config | rain_config_data | R/W/N | 18 B | - | Struct BUILD_ASSERT enforced |
| 19 | Rain Sensor Data | rain_data_data | R/N | 24 B | - | Snapshot only |
| 20 | Rain History Control | rain_history_cmd_data | R/W/N | 16 B | H | Commands + fragmented responses |
| 21 | Environmental Data | environmental_data_ble | R/N | 24 B | - | Snapshot (3B notify frag header only if MTU small) |
| 22 | Environmental History | (history header + payload) | R/W/N | Var. | H | Fragmented historical data |
| 23 | Compensation Status | compensation_status_data | R/W/N | 40 B | - | Optional 1B channel select write |
| 24 | Rain Integration Status | rain_integration_status_ble | R/N | 78 B | - | Snapshot |
| 25 | Onboarding Status | onboarding_status_data | R/N | 29 B | - | Flags + percentages |
| 26 | Reset Control | reset_control_data | R/W/N | 16 B | - | Controlled resets |
| 27 | Channel Compensation Config | channel_comp_config_data | R/W/N | 44 B | - | Per-channel rain/temp settings |
| 28 | Bulk Sync Snapshot | bulk_sync_snapshot_data | R | 60 B | - | Connection-time aggregate |
| 29 | Hydraulic Status | hydraulic_status_data | R/W/N | 48 B | - | Hydraulic profile + lock/anomaly |

## Custom Configuration Service (5 characteristics)

| # | Caracteristica | Struct cod | Proprietati | Dimensiune | Observatii |
| --- | --- | --- | --- | --- | --- |
| 30 | Custom Soil Configuration | custom_soil_config_data | R/W/N | 76 B | Per-channel custom soil |
| 31 | Soil Moisture Configuration | soil_moisture_config_data | R/W/N | 8 B | Global + per-channel override |
| 32 | Config Reset | reset_response | R/N | Var. | Configuration reset ops |
| 33 | Config Status | status_response | R/W/N | Var. | Configuration completeness |
| 34 | Interval Mode Configuration | interval_mode_config_data | R/W/N | 16 B | Cycle & Soak config |

## Total: 34 characteristics (29 Irrigation Service + 5 Custom Configuration Service)

## Key Updates Since Last Audit

1. Added 3 new characteristics to Irrigation Service: Channel Compensation Config (27), Bulk Sync Snapshot (28), Hydraulic Status (29).
2. Custom Configuration Service fully documented with 5 characteristics.
3. Total count corrected from 26 to 34.

---
Audit updated: 2026-01-04
