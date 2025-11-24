# BLE Implementation Status (Audit 2025-08-12)

This file summarizes the current state (code vs documentation) of the GATT characteristics defined in `bt_irrigation_service.c`.

Legend: R=Read, W=Write, N=Notify, F=Custom fragmentation (4B header), H=Unified history 8B header, LW=Long Write (offset accumulation), S=Single full write (if negotiated MTU >= struct size)

| # | Caracteristica | Struct cod | Proprietati | Dimensiune | Fragmentare | Observatii |
|---|-----------------|-----------|-------------|------------|-------------|-----------|
| 1 | Valve Control | valve_control_data | R/W/N | 4 B | - | Struct consistent |
| 2 | Flow Sensor | (internal snapshot) | R/N | 4 B* | - | Read/notify only (*packed representation) |
| 3 | System Status | (internal byte) | R/N | 1 B | - | Snapshot byte |
| 4 | Channel Configuration | channel_config_data | R/W/N | 76 B | F + S | Header types 1/2/3 (name-only & full) |
| 5 | Schedule Configuration | schedule_config_data | R/W/N | 9 B | LW | First write byte selects channel for subsequent read |
| 6 | System Configuration (Enhanced) | enhanced_system_config_data | R/W/N | 56 B | LW + S | Offset-based accumulation (no custom header) |
| 7 | Task Queue Management | task_queue_data | R/W/N | 9 B | - | `task_id_to_delete` not yet used |
| 8 | Statistics | statistics_data | R/W/N | 15 B | - | Volatile (persistence TBD) |
| 9 | RTC Configuration | rtc_data | R/W/N | 16 B | - | Full write only (no partial semantics) |
| 10 | Alarm Status | alarm_data | R/W/N | 7 B | - | Write clears alarm codes |
| 11 | Calibration Management | calibration_data | R/W/N | 13 B | - | Actions 0-3 implemented |
| 12 | History Management | history_data | R/W/N | 12 B cmd, 32 B notify | H | 12-byte write buffer issues queries; read and real-time notifications return `struct history_data`; bulk responses stream via unified header |
| 13 | Diagnostics | diagnostics_data | R/N | 12 B | - | Snapshot |
| 14 | Growing Environment | growing_env_data | R/W/N | 71 B | F + S | Header types 2/3 |
| 15 | Auto Calc Status | auto_calc_status_data | R/W/N | 64 B | H | 1B channel select write (no custom 4B write header); Unified 8B notify header |
| 16 | Current Task Status | current_task_data | R/W/N | 21 B | - | Struct consistent |
| 17 | Timezone Configuration | rtc_data subset / timezone | R/W/N | 16 B | - | Mirrors rtc_data layout |
| 18 | Rain Sensor Config | rain_config_data | R/W/N | 18 B | - | Struct BUILD_ASSERT enforced |
| 19 | Rain Sensor Data | rain_data_data | R/N | 24 B | - | Snapshot only |
| 20 | Rain History Control | rain_history_cmd_data | R/W/N | 16 B | H | Commands + fragmented responses |
| 21 | Onboarding Status | onboarding_status_data | R/N | 29 B | - | Flags + percentages |
| 22 | Reset Control | reset_control_data | R/W/N | 16 B | - | Controlled resets |
| 23 | Environmental Data | environmental_data_ble | R/N | 24 B | - | Snapshot (3B notify frag header only if MTU small) |
| 24 | Environmental History | (history header + payload) | R/W/N | Var. | H | Fragmented historical data |
| 25 | Compensation Status | compensation_status_data | R/W/N | 40 B | - | Optional 1B channel select write (future) |
| 26 | Rain Integration Status | rain_integration_status_ble | R/N | 78 B | - | Snapshot |

## Key Divergences
1. Original README listed only 17 characteristics - code implements 26.
2. History Management not implemented - moved to FUTURE.
3. Several "future" characteristics (History Compensation, Season Adaptivity, etc.) removed (never implemented).
4. System Configuration uses standard long write (offset), not custom header.
5. Rain Integration Status implemented; documentation added now.

## Recommended Next Actions
- Keep `24-rain-integration-status.md` maintained as logic evolves.
- Clarify fragmentation modes inside each fragmented characteristic file (done partially).
- Update or remove placeholder fields as implementation arrives; sync with FUTURE_FEATURES.md.

---
Auto-generated audit - manual review recommended.
