# UCP UART v1 Read / Set / Notify Coverage Checklist

Purpose:
- verify, in one place, that each firmware domain has explicit coverage for:
  - `GET` (read)
  - `SET` (write/update settings)
  - `CMD` (actions)
  - `EVENT_CHANGE`/event topic (notify)
  - `STREAM_*` (large payloads)
- highlight remaining gaps before calling the nRF-side UART API "feature-complete"

Legend:
- `Y` = implemented in current firmware
- `P` = partial (implemented, but limited subset / best-effort / polling-based notify)
- `N` = not implemented in current firmware
- `N/A` = not applicable for that resource/service in v1

Audit inputs used for this checklist (exhaustive pass):
- `docs/FEATURES.md`
- `docs/system-architecture.md`
- `docs/ble-api/README.md`
- `docs/ble-api/IMPLEMENTATION_STATUS.md`
- `docs/packs/BLE_PACK_SERVICE.md`
- `src/bt_irrigation_service.c` (`ATTR_IDX_*` inventory)
- `src/ucp_uart.c` (`g_schema_resources`, `g_schema_stream_purposes`, `handle_get/set/cmd`, `maybe_domain_events`)
- `src/` runtime module inventory (feature modules, storage/history modules, sensor/automation modules)

## Global Protocol Checklist (Transport + Session)

- [x] `HELLO_REQ/HELLO_RESP`
- [x] `PING_REQ/PING_RESP`
- [x] `GET_REQ/GET_RESP`
- [x] `SET_REQ/SET_RESP` (delta semantics)
- [x] `CMD_REQ/CMD_RESP`
- [x] `SUB_REQ/SUB_RESP` / `UNSUB_REQ/UNSUB_RESP`
- [x] `EVENT_CHANGE`
- [x] `EVENT_OVERFLOW`
- [x] `EVENT_HEARTBEAT`
- [x] `STREAM_OPEN_REQ/RESP`
- [x] `STREAM_DATA`
- [x] `STREAM_END`
- [x] `STREAM_ABORT`
- [x] Replay buffer + `replay_then_live`
- [x] `COBS + CRC32C` framing
- [x] OTA orchestration commands (`recovery-first` handoff contract)

## Coverage Matrix (By Service / System)

| Service | Resource / Area | GET | SET | CMD | Notify | Stream | Status | Notes |
|---|---|---:|---:|---:|---:|---:|---|---|
| `session` | handshake / keepalive / subscriptions | N/A | N/A | P | Y | N/A | `Y` | `HELLO/PING/SUB/UNSUB`; not modeled as data resources |
| `device/system` | `device.system_status` | Y | N | N | Y | N | `Y` | direct + poll-backed notify |
| `device/system` | `device.alarm_event` | N/A | N/A | N/A | Y | N | `Y` | event stream (`topic=alarm`) |
| `device/system` | `device.alarm_status` | Y | N | Y | Y | N | `Y` | read current alarm + clear cmd + alarm events |
| `device/system` | `device.system_config` | Y | Y | N | Y | N | `Y` | enhanced system config bytes (power mode, master-valve config, BME config, defaults) |
| `device/system` | `device.statistics` | Y | N | Y | Y | N | `Y` | per-channel statistics `GET`, reset cmds, notify |
| `device/system` | `device.auto_calc_status` | Y | Y | Y | Y | N | `Y` | FAO-56 status/output bytes + enable `SET` compatibility + run cmd + notify |
| `device/system` | `device.auto_calc_control` | Y | Y | N | Y | N | `Y` | global auto-calc enable + interval-hours (`1..24`) control surface |
| `device/system` | `device.bulk_snapshot` | Y | N | N | Y | Y | `Y` | compact snapshot resource + stream export (`purpose=11`) |
| `channel` | `channel.state` | Y | N | Y | Y | N | `Y` | manual valve/master/queue direct cmds + state notify |
| `channel` | `channel.config` | Y | Y | N | Y | N | `Y` | per-channel config write/read |
| `schedule` | `schedule.config` | Y | Y | N | Y | N | `Y` | per-channel schedule config |
| `task` | `task.status` | Y | N | Y | Y | N | `Y` | queue/current status + `start_next/stop/pause/resume/clear_pending/cancel_all` |
| `flow` | `flow.status` | Y | Y | N | Y | N | `Y` | live sensor + direct pulses/L calibration set |
| `flow` | `flow.calibration_status` | Y | Y | N | Y | N | `Y` | BLE-compatible calibration workflow status (`start/stop/calc/apply/reset` via `SET`) |
| `rain` | `rain.status` | Y | Y | N | Y | N | `Y` | config subset write + live notify |
| `environment` | `environment.current` | Y | N | N | Y | N | `Y` | live env notify from polling |
| `history` | `history.storage` | Y | N | Y | Y | Y | `Y` | reset cmds + detailed page stream (`purpose=4`) + aggregate range stream (`purpose=12`) |
| `rtc_tz` | `rtc_tz.timezone_config` | Y | Y | N | Y | N | `Y` | validated delta writes |
| `rtc_tz` | `rtc_tz.rtc_time` | Y | Y | N | Y | N | `Y` | time/date write + notify |
| `compensation` | rain/temp/channel compensation | Y | Y | N | Y | N | `Y` | `compensation.status/channel_config/rain_integration` |
| `onboarding_reset` | onboarding status / reset progress / reset ops | Y | N | Y | Y | N | `Y` | status/resources + reset/onboarding/config-reset cmds |
| `packs` | `packs.stats` + staging/install/delete | Y | N | Y | Y | Y | `Y` | list streams (`purpose=5/6`), inbound staging (`purpose=3`) + staged-upload notify on `STREAM_END` |
| `ota` | preflight / enter_recovery / post_check | N | N | Y | Y | N | `Y` | `ota.transition` events; firmware upload is via MCUboot/mcumgr |
| `peer_power` | `peer_power.state` | Y | N | Y | Y | N | `Y` | `off/on/cycle` cmds; hw-backed if `esp32_en` DT alias present |
| `peer_power` | `peer_power.policy` | Y | Y | N | Y | N | `Y` | policy + grace period |
| `diagnostics` | counters / stream status / domain status | Y | N | N | P | N | `Y` | diag topic notify exists; mostly on UCP internal changes |
| `diagnostics` | `diag.storage_health` | Y | N | N | N | N | `Y` | NVS + pack storage + watering history storage summary |
| `schema_discovery` | schema/info/desc lookups | Y | N | N | N | Y | `Y` | paged `schema_catalog` stream (`purpose=7`) |

## Command Coverage Checklist (Action Surfaces)

### `task`
- [x] `task.start_next`
- [x] `task.stop_current`
- [x] `task.pause_current`
- [x] `task.resume_current`
- [x] `task.cancel_all`
- [x] `task.clear_pending`

### `channel`
- [x] `channel.valve_open`
- [x] `channel.valve_close`
- [x] `channel.master_open`
- [x] `channel.master_close`
- [x] `channel.queue_duration_task`
- [x] `channel.queue_volume_task`

### `device/system`
- [x] `device.alarm_clear`

### `history`
- [x] `history.reset_all`
- [x] `history.reset_channel`

### `packs`
- [x] `packs.delete_pack`
- [x] `packs.delete_plant`
- [x] `packs.install_staged_plant`
- [x] `packs.install_staged_pack_bundle`

### `ota`
- [x] `ota.preflight`
- [x] `ota.enter_recovery` (2-step token confirmation)
- [x] `ota.post_check`

### `peer_power`
- [x] `peer_power.off`
- [x] `peer_power.on`
- [x] `peer_power.cycle`

## Notify Coverage Checklist (What must be mirrored to peer)

### Implemented topics (current firmware)
- [x] `system.status`
- [x] `alarm`
- [x] `rtc_tz.changed` (timezone + RTC time changes)
- [x] `channel.state`
- [x] `channel.config`
- [x] `schedule.config`
- [x] `task.current`
- [x] `flow.live`
- [x] `flow.calibration`
- [x] `rain.live`
- [x] `env.live`
- [x] `history.ready` (history operations/status surface)
- [x] `packs.changed`
- [x] `ota.transition`
- [x] `peer.power`
- [x] `diagnostics.health` (UCP/diag changes; limited scope)

### Notify source quality (important for correctness)
- [x] UCP-originated writes emit notify (`origin=uart`)
- [x] BLE/local-originated timezone/RTC changes emit notify
- [x] BLE/local valve state changes emit notify
- [x] Scheduler-originated alarm/system changes emit notify
- [x] Polling covers many external mutations (channel/schedule/sensors/packs/task)
- [x] Replay + overflow handling for reconnect/resync

### Still recommended to add/improve (coverage hardening)
- [ ] Direct hooks (non-polling) for every high-frequency domain mutation source where available (to reduce poll latency)
- [ ] Automated tests that assert notify parity for each `SET` path and each major BLE/local mutation path

## Read / Write Completeness Checklist (By Planned Service)

This section is the "do we expose everything we need?" checkpoint against the v1 service plan.

### Implemented service families (usable now)
- [x] `device/system`
- [x] `channel`
- [x] `schedule`
- [x] `task`
- [x] `flow`
- [x] `rain`
- [x] `environment`
- [x] `history`
- [x] `rtc_tz`
- [x] `packs`
- [x] `ota` (command-only orchestration)
- [x] `peer_power`
- [x] `diagnostics`
- [x] `schema_discovery`

### Planned service families still missing (v1 scope gaps)
- [x] `compensation` (implemented)
- [x] `onboarding_reset` (implemented)

## Exhaustive BLE Parity Audit (38 Characteristics -> UCP UART)

Goal of this section:
- ensure nothing user-facing from BLE/custom/pack services was accidentally omitted from the UCP plan
- mark what is already covered, partially covered, or still missing on UCP

Legend (this section):
- `Covered` = UCP has an equivalent usable surface
- `Partial` = some capability exists, but parity is incomplete
- `Missing` = no practical UCP surface yet

### Irrigation Service (29)

| # | BLE Characteristic | BLE R/W/N | UCP Equivalent (current / expected) | Parity | Notes / Gap |
|---|---|---|---|---|---|
| 1 | Valve Control | R/W/N | `channel.state` + `channel` direct cmds | `Covered` | manual valve open/close + master open/close + notify covered |
| 2 | Flow Sensor | R/N | `flow.status` | `Covered` | live + config/calibration subset via `SET` |
| 3 | System Status | R/N | `device.system_status` | `Covered` | notify present |
| 4 | Channel Configuration | R/W/N | `channel.config` | `Covered` | core config + notify present |
| 5 | Schedule Configuration | R/W/N | `schedule.config` | `Covered` | read/write/notify present |
| 6 | System Configuration | R/W/N | `device.system_config` | `Covered` | bytes resource + `SET` + notify implemented |
| 7 | Task Queue | R/W/N | `task.status` + task/queue cmds + `channel.queue_*` cmds | `Covered` | queue control/start-next/clear-pending + enqueue via channel direct cmds |
| 8 | Statistics | R/W/N | `device.statistics` | `Covered` | per-channel bytes `GET`, reset cmds, notify |
| 9 | RTC Configuration | R/W/N | `rtc_tz.rtc_time` + `rtc_tz.timezone_config` | `Covered` | split cleanly in UCP |
| 10 | Alarm Status | R/W/N | `device.alarm_status` + `device.alarm_event` | `Covered` | read current alarm + clear cmd + notify |
| 11 | Calibration Management | R/W/N | `flow.calibration_status` + `flow.status` | `Covered` | workflow status/progress via `flow.calibration_status`; direct ppl set via `flow.status` |
| 12 | History Management (watering) | R/W/N | `history.storage` + `history` cmds + `STREAM purpose=4/12` | `Covered` | detailed page + aggregated daily/monthly/annual/detailed range exports |
| 13 | Diagnostics | R/N | `diagnostics.*` | `Covered` | UCP diagnostics richer for transport; payload shape differs |
| 14 | Growing Environment | R/W/N | `channel.config` (+ custom soil / soil moisture / interval resources) | `Covered` | plant/soil/method/coverage/sun/lat/lon in `channel.config`, advanced settings split into dedicated resources |
| 15 | Auto Calc Status | R/W/N | `device.auto_calc_status` + `device.auto_calc_control` | `Covered` | status/output via `auto_calc_status`; global enable + interval via `auto_calc_control`; run via cmd |
| 16 | Current Task Status | R/W/N | `task.status` | `Covered` | current task + notify exists |
| 17 | Timezone Configuration | R/W/N | `rtc_tz.timezone_config` | `Covered` | read/write/notify present |
| 18 | Rain Sensor Config | R/W/N | `rain.status` + `compensation.rain_integration` | `Covered` | sensor config split from integration/sensitivity thresholds across two UCP resources |
| 19 | Rain Sensor Data | R/N | `rain.status` | `Covered` | live/status snapshot + notify |
| 20 | Rain History Control | R/W/N | `history.rain_storage` + `STREAM purpose=8` + reset cmd | `Covered` | rain stats + range stream + reset command implemented |
| 21 | Environmental Data | R/N | `environment.current` | `Covered` | live snapshot + notify |
| 22 | Environmental History | R/W/N | `history.environment_storage` + `STREAM purpose=9` + reset cmd | `Covered` | env stats + hourly/daily/monthly range stream + reset command implemented |
| 23 | Compensation Status | R/W/N | `compensation.status` | `Covered` | per-channel bytes `GET` + notify (derived status) |
| 24 | Onboarding Status | R/N | `onboarding_reset.onboarding_status` | `Covered` | `GET` + notify implemented |
| 25 | Reset Control | R/W/N | `onboarding_reset.reset_control` | `Covered` | `GET` + reset controller cmds + notify implemented |
| 26 | Rain Integration Status | R/N | `compensation.rain_integration` | `Covered` | dedicated resource + notify implemented |
| 27 | Channel Compensation Config | R/W/N | `compensation.channel_config` | `Covered` | bytes `GET/SET` + notify implemented |
| 28 | Bulk Sync Snapshot | R | `device.bulk_snapshot` + `STREAM purpose=11` | `Covered` | compact snapshot resource + stream export implemented |
| 29 | Hydraulic Status | R/W/N | `flow.hydraulic_status` | `Covered` | bytes `GET/SET`, lock/manual-override cmds, notify |

### Custom Configuration Service (5)

| # | BLE Characteristic | BLE R/W/N | UCP Equivalent (current / expected) | Parity | Notes / Gap |
|---|---|---|---|---|---|
| 30 | Custom Soil Configuration | R/W/N | `channel.custom_soil` | `Covered` | per-channel bytes `GET/SET` + notify implemented |
| 31 | Soil Moisture Configuration | R/W/N | `channel.soil_moisture` | `Covered` | global + per-channel `GET/SET` + notify implemented |
| 32 | Config Reset | R/N | `onboarding_reset` cmds (`config_reset_group`) | `Covered` | config reset command path implemented (in onboarding/reset service) |
| 33 | Config Status | R/W/N | `onboarding_reset.config_status` / `channel.config_status` | `Covered` | `GET` + notify + reset-group driven updates implemented |
| 34 | Interval Mode Configuration | R/W/N | `channel.interval_config` | `Covered` | bytes `GET/SET` + notify implemented |

### Pack Service (4)

| # | BLE Characteristic | BLE R/W/N | UCP Equivalent (current / expected) | Parity | Notes / Gap |
|---|---|---|---|---|---|
| 35 | Pack Plant | R/W/N | `packs` cmds + `STREAM purpose=6` + `packs.stats` notify | `Covered` | install/delete/list + change notifications via packs topic/stats |
| 36 | Pack Stats | R | `packs.stats` | `Covered` | read parity present |
| 37 | Pack Transfer | R/W/N | inbound `STREAM purpose=3` + `packs.stats` + install cmds | `Covered` | staged upload + status fields (`stage_valid/len/crc`) + notify on staged upload/install |
| 38 | Pack List | R/W/N | `STREAM purpose=5` + pack cmds | `Covered` | list/query/delete/install surfaces present in UCP |

## Additional Non-BLE Firmware Surfaces That Should Be Considered for UCP

These are not necessarily public BLE characteristics today, but they are relevant for local API/cloud and should be reviewed for UART exposure.

| Area / Feature | Should expose via UCP? | Current UCP | Why it matters |
|---|---|---|---|
| Global system configuration (master valve delays, BME280 enable/interval, defaults) | `Yes` | `Covered` | `device.system_config` bytes `GET/SET` + notify |
| Power mode config/status (`Normal` / `Energy` / `ULP`) | `Yes` | `Covered` | status in `device.system_status`; config/control via `device.system_config` |
| Hydraulic Sentinel (profile, thresholds/locks, anomaly counters, overrides) | `Yes` | `Covered` | `flow.hydraulic_status` + hydraulic cmds + notify |
| Rain history exports (hourly/daily) | `Yes` | `Covered` | `STREAM purpose=8` range export + history reset cmd |
| Environmental history exports (hourly/daily/monthly) | `Yes` | `Covered` | `STREAM purpose=9` range export + env history reset cmd |
| Statistics summaries (coarse counters) | `Yes` | `Covered` | `device.statistics` + reset cmds + notify |
| FAO-56 / auto-calc status outputs + global scheduling interval | `Yes` | `Covered` | `device.auto_calc_status` + `device.auto_calc_control` + notify + run cmd |
| Compensation status + per-channel compensation config | `Yes` | `Covered` | `compensation.*` resources + notify |
| Onboarding/config completeness status | `Yes` | `Covered` | `onboarding_reset.onboarding_status` + `config_status` + notify |
| Reset controller / config reset (two-step) | `Yes` | `Covered` | reset controller cmds + `reset_control` resource + notify |
| Custom soil profile CRUD | `Yes` | `Covered` | `channel.custom_soil` bytes surface + notify |
| Soil moisture override config (global + per-channel) | `Yes` | `Covered` | `channel.soil_moisture` bytes surface + notify |
| Interval mode (cycle/soak) config | `Yes` | `Covered` | `channel.interval_config` bytes surface + notify |
| Bulk snapshot aggregate resource | `Yes` | `Covered` | `device.bulk_snapshot` + stream export |
| Plant/soil/irrigation-method catalog queries | `Yes` | `Covered` | `STREAM purpose=10` `db_catalog` (paged text) |
| Storage health (NVS/LittleFS usage/cleanup status) | `Yes` | `Covered` | `diag.storage_health` + `packs.stats` + `history.storage` |
| Battery/power ADC (future hardware) | `Eventually` | `N/A` | Not a current finished feature, but reserve schema/service space |

## Firmware Module Inventory Audit (Source-Based, Not BLE-Only)

This section verifies UCP coverage against the actual firmware module inventory under `src/`, not only BLE characteristics.
Rule used in this audit:

- if a module owns product-facing state/config/control that matters for local/cloud, it must be reachable through at least one UCP resource/command/topic/stream
- if a module is internal-only plumbing/algorithm/helper, direct UCP exposure is not required as long as its effects are observable through higher-level UCP surfaces

| Module / Subsystem (src/) | Product-facing? | UCP Surface(s) | Status | Notes |
|---|---|---|---|---|
| `watering.c`, `watering_tasks.c` (core engine, scheduler, queue) | Yes | `task.status`, `channel.state`, `schedule.config`, `channel`/`task` cmds, `task.current`, `system.status` topics | `Covered` | queue/current task/state transitions and control reachable via UCP |
| `watering_config.c` | Yes | `channel.config`, `schedule.config`, `device.system_config`, `compensation.*`, `rain.status`, `rtc_tz.*` | `Covered` | persisted config owners are surfaced through domain resources |
| `valve_control.c` | Yes | `channel.state` + `channel.valve_*` / `channel.master_*` cmds + `peer_power` (ESP side power only) | `Covered` | manual zone/master control exposed; master delays/config via `device.system_config` |
| `watering_monitor.c` (flow/hydraulic monitoring) | Yes | `flow.status`, `flow.hydraulic_status`, `device.alarm_status`, `alarm` topic | `Covered` | hydraulic locks/overrides/status + alarms visible and controllable |
| `flow_sensor.c` | Yes | `flow.status`, `flow.calibration_status`, `flow.live`, `flow.calibration` topic | `Covered` | calibration workflow + direct calibration set exposed |
| `rain_sensor.c`, `rain_config.c` | Yes | `rain.status`, `rain.live`, `history.rain_storage`, `STREAM purpose=8` | `Covered` | live rain + config + history exports/reset |
| `rain_history.c` | Yes | `history.rain_storage`, `history` reset cmds, `STREAM purpose=8` | `Covered` | range/history export is on UCP streams |
| `environmental_data.c`, `env_sensors.c`, `bme280_driver.c`, `sensor_manager.c` | Yes | `environment.current`, `environment.sensor_config`, `env.live`, `device.system_config` (BME config fields) | `Covered` | BME current readings + config exposure present |
| `environmental_history.c`, `history_flash.c` | Yes | `history.environment_storage`, reset cmd, `STREAM purpose=9` | `Covered` | env history stats + range streams |
| `watering_history.c` | Yes | `history.storage`, reset cmds, `STREAM purpose=4` and `purpose=12` | `Covered` | page export + aggregated water history ranges |
| `rtc.c`, `timezone.c` | Yes | `rtc_tz.rtc_time`, `rtc_tz.timezone_config`, `rtc_tz.changed` topic | `Covered` | read/write + notify implemented |
| `enhanced_system_status.c`, `enhanced_error_handling.c` | Yes | `device.system_status`, `device.bulk_snapshot`, `device.alarm_status`, `device.alarm_clear`, `device.clear_errors`, topics | `Covered` | aggregated status/error surfaces visible through UCP device resources/cmds |
| `configuration_status.c`, `onboarding_state.c` | Yes | `onboarding_reset.onboarding_status`, `onboarding_reset.system_overview`, `onboarding_reset.config_status`, notify topics | `Covered` | completeness/onboarding status exported |
| `reset_controller.c` | Yes | `onboarding_reset.reset_control` + reset commands + `reset.progress` topic | `Covered` | two-step reset controller surfaced |
| `power_management.c` | Yes | `device.system_status` (current mode), `device.system_config` (mode config) | `Covered` | mode switching/status exposed via system surfaces |
| `fao56_calc.c`, `fao56_custom_soil.c`, `water_balance_types.h` | Yes | `device.auto_calc_status`, `device.auto_calc_control`, `channel.config`, `schedule.config`, `compensation.*`, `bulk_snapshot` | `Covered` | algorithm internals not direct API, but inputs/outputs/status and global interval control are exposed |
| `rain_integration.c`, `rain_compensation.c`, `temperature_compensation.c`, `temperature_compensation_integration.c` | Yes | `compensation.status`, `compensation.channel_config`, `compensation.rain_integration`, `rain.status` | `Covered` | config/status/output surfaces exposed; internals remain internal |
| `soil_moisture_config.c` | Yes | `channel.soil_moisture`, `soil_moisture` topic | `Covered` | global/per-channel overrides exposed |
| `interval_mode_controller.c`, `interval_task_integration.c`, `interval_timing.c` | Yes | `channel.interval_config`, `task.status`, `channel.state`, `interval_mode` topic | `Covered` | interval config/state observable via higher-level task/channel resources |
| `custom_soil_db.c`, `bt_custom_soil_handlers.c` | Yes | `channel.custom_soil`, `custom_soil` topic | `Covered` | custom soil config CRUD surface is present (bytes resource) |
| `pack_storage.c`, `bt_pack_handlers.c`, `database_flash.c`, `plant_db_api.c` | Yes | `packs.stats`, `packs` cmds, `STREAM purpose=3/5/6`, `db_catalog` (`purpose=10`), `packs.changed` topic | `Covered` | pack/plant install/list/delete/staging status and built-in catalogs exposed |
| `nvs_config.c`, `nvs_storage_monitor.c` | Yes (ops/health) | domain resources (all config/history owners) + `diag.storage_health` | `Covered` | low-level KV API not exposed directly by design |
| `ota_upload_guard.c` | Yes (OTA orchestration guarding) | `ota.preflight`, `ota.enter_recovery`, `ota.post_check`, `ota.transition` topic | `Covered` | policy/guard outcome surfaced through OTA command/status flow |
| `bt_irrigation_service.c` + BLE handlers (`bt_*_handlers.c`) | Yes (alternative transport) | UCP parity surfaces tracked in BLE parity audit table | `Covered` | UCP mirrors/extends BLE feature surfaces |
| `ucp_uart.c`, `ucp_protocol.h`, `ucp_tlv/cobs/crc32c` | Yes (transport itself) | `schema_discovery`, `diagnostics.*`, protocol core messages | `Covered` | protocol self-describes and exposes transport diagnostics |
| `usb_descriptors.c`, `main.c`, `main_minimal.c` | No (platform/bootstrap) | Not directly exposed | `N/A` | bootstrap/USB descriptor plumbing, not product state |
| `test_current_task.c` | No (test helper) | Not exposed | `N/A` | test-only helper module |
| Generated DB blobs (`*.inc`, fingerprints) | No (data assets) | Exposed indirectly via `db_catalog` / resource lookups | `N/A` | no direct raw blob API required in v1 |

## Consolidated Gaps Discovered in Exhaustive Audit (beyond original minimal plan)

These are the big items that were easy to miss if only looking at the first UCP plan:

- [x] `device/system_config` (global system settings; BLE #6 parity)
- [x] `task.queue` richer queue CRUD / injection parity (BLE #7 parity)
- [x] `statistics.summary` (BLE #8 parity)
- [x] `alarm.status` read/clear surface (BLE #10 parity; event-only is not enough)
- [x] `flow.calibration` session management/progress (BLE #11 parity)
- [x] `history.watering_*` aggregated range exports (BLE #12 parity completion beyond page export)
- [x] `history.rain_*` queries/streams (BLE #20 parity)
- [x] `history.environment_*` queries/streams (BLE #22 parity)
- [x] `auto_calc.status` / FAO-56 decision visibility (BLE #15 parity)
- [x] `auto_calc interval` global scheduling configuration (source-audit gap; not BLE-exposed but now UCP-exposed)
- [x] `hydraulic.status/config` (BLE #29 parity)
- [x] Full `compensation` service (`status`, `rain_integration`, `channel_comp_config`) (BLE #23/#26/#27 parity)
- [x] Full `onboarding_reset` service (`onboarding_status`, `reset_control`, `config_reset`, `config_status`) (BLE #24/#25/#32/#33 parity)
- [x] `custom_soil` CRUD + `soil_moisture` config + `interval_mode` config (BLE custom service #30/#31/#34 parity)
- [x] Bulk aggregate snapshot resource for fast sync (BLE #28 concept parity)
- [x] Explicit DB catalog query surfaces (built-in plants / soils / irrigation methods) for local/cloud UX

## Validation Checklist (what still needs execution, not code)

- [ ] Hardware E2E: `ota.enter_recovery` -> MCUboot serial recovery on `uart0` -> `mcumgr` upload/test/reset/confirm
- [ ] UART fault-injection tests (COBS/CRC/oversize/resync)
- [ ] Replay/overflow/resync flood tests
- [ ] BLE + UCP concurrency tests (`last write wins`, `origin`, `resource_version`)
- [ ] Stream robustness tests (cancel/timeouts/CRC mismatch on inbound upload)

## Practical Definition of "Complete" (nRF-side UART API)

For this project phase, treat UCP UART as complete when:
- all planned v1 service families needed by product behavior are implemented or explicitly deferred,
- each implemented mutable domain has `GET` + `SET/CMD` + `notify`,
- large data surfaces use `STREAM_*`,
- replay/overflow/heartbeat behavior is validated on hardware,
- OTA recovery-first handoff is validated end-to-end on hardware.

Current conclusion after source-based audit:

- product-facing runtime surfaces in the current firmware (`src/` inventory + BLE/custom/pack APIs + feature docs) are mapped to UCP resources/commands/topics/streams
- remaining unchecked items are validation/execution on hardware and automated tests, not missing UCP API surface definitions
