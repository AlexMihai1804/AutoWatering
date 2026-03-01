# UCP UART v1 Service Index

## Public Services (v1 target)

- `0x0001` `session`
- `0x0010` `device/system`
- `0x0011` `channel`
- `0x0012` `schedule`
- `0x0013` `task`
- `0x0014` `flow`
- `0x0015` `rain`
- `0x0016` `environment`
- `0x0017` `history`
- `0x0018` `rtc_tz`
- `0x0019` `compensation`
- `0x001A` `onboarding_reset`
- `0x001B` `packs`
- `0x001C` `ota`
- `0x001D` `peer_power`
- `0x001E` `diagnostics`
- `0x00F0` `schema_discovery`

## Implemented in Current Firmware (Source of Truth: `g_schema_resources`)

This section summarizes the live UCP surface implemented in `src/ucp_uart.c`. The authoritative machine-readable view is exposed by `schema_discovery` (`schema.resource_desc`, `schema.stream_purpose_desc`, `schema_catalog` stream).

### `device/system` (`0x0010`)

- Resources:
  - `0x0001` `device.system_status` (`GET`, `EVENT`)
  - `0x0002` `device.alarm_event` (`EVENT` only)
  - `0x0003` `device.system_config` (`GET`, `SET`, `EVENT`)
  - `0x0004` `device.statistics` (`GET`, `CMD`, `EVENT`)
  - `0x0005` `device.auto_calc_status` (`GET`, `SET`, `CMD`, `EVENT`)
  - `0x0008` `device.auto_calc_control` (`GET`, `SET`, `EVENT`)
  - `0x0006` `device.bulk_snapshot` (`GET`, `STREAM_OUT`, `EVENT`)
  - `0x0007` `device.alarm_status` (`GET`, `CMD`, `EVENT`)
- Commands:
  - `device.stats_reset_channel`, `device.stats_reset_all`
  - `device.run_auto_calc`
  - `device.clear_errors`
  - `device.alarm_clear`
- Notes:
  - `device.auto_calc_status` carries BLE-compatible FAO status/output bytes
  - `device.auto_calc_control` carries global auto-calc enable + interval-hours (`1..24`) control/state

### `channel` (`0x0011`)

- Resources:
  - `0x0001` `channel.state` (`GET`, `CMD`, `EVENT`)
  - `0x0002` `channel.config` (`GET`, `SET`, `EVENT`)
  - `0x0003` `channel.custom_soil` (`GET`, `SET`, `EVENT`)
  - `0x0004` `channel.soil_moisture` (`GET`, `SET`, `EVENT`)
  - `0x0005` `channel.interval_config` (`GET`, `SET`, `EVENT`)
  - `0x0006` `channel.config_status` (`GET`, `EVENT`)
- Commands:
  - `channel.valve_open`, `channel.valve_close`
  - `channel.master_open`, `channel.master_close`
  - `channel.queue_duration_task`, `channel.queue_volume_task`

### `schedule` (`0x0012`)

- Resource `0x0001` `schedule.config` (`GET`, `SET`, `EVENT`)

### `task` (`0x0013`)

- Resource `0x0001` `task.status` (`GET`, `CMD`, `EVENT`)
- Commands:
  - `task.stop_current`, `task.pause_current`, `task.resume_current`
  - `task.cancel_all`, `task.start_next`, `task.clear_pending`

### `flow` (`0x0014`)

- Resources:
  - `0x0001` `flow.status` (`GET`, `SET`, `EVENT`)
  - `0x0002` `flow.hydraulic_status` (`GET`, `SET`, `CMD`, `EVENT`)
  - `0x0003` `flow.calibration_status` (`GET`, `SET`, `EVENT`)
- Commands (`flow.hydraulic_*`):
  - clear per-channel lock, clear global lock
  - set/clear manual override

### `rain` (`0x0015`)

- Resource `0x0001` `rain.status` (`GET`, `SET`, `EVENT`)

### `environment` (`0x0016`)

- Resources:
  - `0x0001` `environment.current` (`GET`, `EVENT`)
  - `0x0002` `environment.sensor_config` (`GET`, `SET`)

### `history` (`0x0017`)

- Resources:
  - `0x0001` `history.storage` (`GET`, `CMD`, `STREAM_OUT`, `EVENT`)
  - `0x0002` `history.rain_storage` (`GET`, `CMD`, `STREAM_OUT`, `EVENT`)
  - `0x0003` `history.environment_storage` (`GET`, `CMD`, `STREAM_OUT`, `EVENT`)
- Commands:
  - `history.reset_all`, `history.reset_channel`
  - `history.reset_rain`, `history.reset_env`
- Streams:
  - `purpose=4` `history_page` (watering page export)
  - `purpose=8` `rain_history_range`
  - `purpose=9` `env_history_range`
  - `purpose=12` `water_history_range` (aggregated/detailed range export)

### `rtc_tz` (`0x0018`)

- Resources:
  - `0x0001` `rtc_tz.timezone_config` (`GET`, `SET`, `EVENT`)
  - `0x0002` `rtc_tz.rtc_time` (`GET`, `SET`, `EVENT`)

### `compensation` (`0x0019`)

- Resources:
  - `0x0001` `compensation.status` (`GET`, `EVENT`)
  - `0x0002` `compensation.channel_config` (`GET`, `SET`, `EVENT`)
  - `0x0003` `compensation.rain_integration` (`GET`, `EVENT`)

### `onboarding_reset` (`0x001A`)

- Resources:
  - `0x0001` `onboarding_reset.onboarding_status` (`GET`, `EVENT`)
  - `0x0002` `onboarding_reset.system_overview` (`GET`, `EVENT`)
  - `0x0003` `onboarding_reset.config_status` (`GET`, `CMD`, `EVENT`)
  - `0x0004` `onboarding_reset.reset_control` (`GET`, `CMD`, `EVENT`)
- Commands:
  - `onb.reset_gen_code`, `onb.reset_execute`, `onb.reset_clear_confirm`, `onb.reset_clear_wipe_state`
  - `onb.onboarding_reset_state`, `onb.config_reset_group`

### `packs` (`0x001B`)

- Resource `0x0001` `packs.stats` (`GET`, `CMD`, `STREAM_IN`, `STREAM_OUT`, `EVENT`)
- Commands:
  - `packs.delete_pack`, `packs.delete_plant`
  - `packs.install_staged_plant`, `packs.install_staged_pack_bundle`
- Streams:
  - `purpose=3` inbound `upload_staging`
  - `purpose=5` `packs_list`
  - `purpose=6` `packs_plants_list`
  - `purpose=10` `db_catalog` (built-in plant/soil/irrigation-method catalogs)

### `ota` (`0x001C`)

- Commands:
  - `0x0001` `ota.preflight`
  - `0x0002` `ota.enter_recovery` (2-step token confirmation, bootmode+reboot handoff)
  - `0x0003` `ota.post_check`
- Events:
  - `ota.transition`

### `peer_power` (`0x001D`)

- Resources:
  - `0x0001` `peer_power.state` (`GET`, `CMD`, `EVENT`)
  - `0x0002` `peer_power.policy` (`GET`, `SET`, `EVENT`)
- Commands:
  - `peer_power.off`, `peer_power.on`, `peer_power.cycle`
- Notes:
  - Hardware-backed if DTS alias `esp32_en` is present (`P0.13` in current board overlay)

### `diagnostics` (`0x001E`)

- Resources:
  - `0x0001` `diag.counters` (`GET`, `EVENT`)
  - `0x0002` `diag.stream_status` (`GET`)
  - `0x0003` `diag.domain_status` (`GET`)
  - `0x0004` `diag.storage_health` (`GET`)

### `schema_discovery` (`0x00F0`)

- Resources:
  - `0x0001` `schema.impl_mask`
  - `0x0002` `schema.proto_info`
  - `0x0003` `schema.service_mask`
  - `0x0004` `schema.topic_mask`
  - `0x0005` `schema.stream_caps`
  - `0x0006` `schema.resource_desc`
  - `0x0007` `schema.field_desc` (best-effort)
  - `0x0008` `schema.stream_purpose_desc`
- Streams:
  - `purpose=7` `schema_catalog`

## Notes

- `SET` uses delta TLV semantics (only provided fields are changed).
- Unknown TLV fields are ignored for forward compatibility where safe.
- `STREAM_*` transport is implemented bidirectional and now used by history/packs domain adapters.
- `schema.service_mask` is a compact `u32` mask for service IDs `1..32`; use `schema_catalog` / `schema.resource_desc` for sparse IDs (e.g. `0x00F0`).
- Remaining open items are primarily hardware validation/testing (UART fault/replay/stream/OTA E2E) and any future product features not implemented in firmware yet.
- Coverage checklist (`GET/SET/CMD/NOTIFY/STREAM`) is tracked in `docs/uart-api/read-set-notify-checklist.md`.
- Exhaustive per-method request/response/side-effect docs are in `docs/uart-api/api-reference.md`.
- Exact wire framing/header/TLV/event/stream bytes are in `docs/uart-api/wire-reference.md`.
- Raw `RAW_BYTES_F` ABI mappings and stream record struct references are in `docs/uart-api/raw-payload-abi-catalog.md`.
- Worked end-to-end usage traces are in `docs/uart-api/examples-and-traces.md`.
- Host implementation/state-machine guidance is in `docs/uart-api/client-implementation-guide.md`.
