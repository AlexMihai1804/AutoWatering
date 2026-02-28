# UCP UART v1 API Reference (Exhaustive nRF-Side Surface)

This document describes every implemented UCP UART API method in the nRF52840 firmware:

- `GET` resources
- `SET` resources
- `CMD` commands
- `SUB/UNSUB` events
- `STREAM` purposes and query contracts

Wire-level framing/bytes are in `wire-reference.md`.
Binary raw payload ABI structs are in `raw-payload-abi-catalog.md`.

## 1. Conventions

## 1.1 Terminology

- `service_id`: UCP service domain (e.g. `device`, `channel`)
- `resource_id`: method ID used with `GET`/`SET`
- `cmd_id`: method ID used with `CMD`
- `topic_id`: used by `EVENT_CHANGE`
- `instance_id`:
  - per-channel resources: channel index (`0..N-1`)
  - some global/all-channel events use `0xFFFFFFFF`

## 1.2 Standard `SET_RESP` Success Payload

Many setters use the common helper (`send_set_ok_resp`), which returns:

- `UCP_F_OK_BOOL` (`BOOL`) = `1`
- `UCP_F_INSTANCE_ID` (`U32`) (resource instance)
- `UCP_F_RESOURCE_VERSION` (`U32`)
- `UCP_F_TIMESTAMP_UTC_MS` (`U64`)

Some specialized setters (`rtc_tz.timezone_config`, `rtc_tz.rtc_time`, `peer_power.policy`) return the same shape but may omit `UCP_F_INSTANCE_ID`.

## 1.3 Event Origin Values (`UCP_F_ORIGIN`)

Source: `src/ucp_uart.h`

| Value | Meaning |
|---:|---|
| 1 | `uart` |
| 2 | `ble` |
| 3 | `scheduler` |
| 4 | `sensor` |
| 5 | `local_fw` |
| 6 | `boot_recovery` |

## 1.4 Selector Fields

Selectors accepted across resources/commands:

- `RES_CH_ID` (`field_id=1` in channel/schedule namespaces): `U8`
- `Q_CHANNEL_ID` (`field_id=100`): `U8`
- Some handlers accept either.

Special values:

- `0xFF` means global selector for some resources (`auto_calc_status`, `soil_moisture`)

## 1.5 Raw-Bytes Resources

Resources documented as `RAW_BYTES_F wrapper` return payload in this pattern:

- `RAW_BYTES_F` + metadata (`resource_version`, `timestamp`, optional `instance_id`)

See:

- `raw-payload-abi-catalog.md`

## 2. Message Families

## 2.1 `GET`

- `GET_REQ` header:
  - `service_id = svc`
  - `method_or_topic_id = resource_id`
- payload: resource-specific selectors/queries or empty
- `GET_RESP` payload: typed TLV fields or `RAW_BYTES_F` wrapper

## 2.2 `SET`

- `SET_REQ` payload is delta TLV (only fields to change)
- unknown fields are mostly ignored in typed setters; missing required selector => `INVALID_ARG`
- successful set emits `EVENT_CHANGE` on the resource's topic

## 2.3 `CMD`

- `CMD_REQ` payload depends on command
- `CMD_RESP` always uses `method_or_topic_id = cmd_id`
- many commands emit follow-up `EVENT_CHANGE`

## 2.4 `STREAM`

Exact framing is in `wire-reference.md`.

This document lists purpose/query semantics under relevant services and in the `STREAM Purposes` section.

## 3. Field Sets for Typed Resources (TLV resources)

This section documents field IDs for typed resources (not `RAW_BYTES_F` wrappers).

## 3.1 `device.system_status` field IDs (`SYS_*`)

| ID | Name | Type |
|---:|---|---|
| 1 | `uptime_ms` | `U32` |
| 2 | `unix_utc_s` | `U32` |
| 3 | `rtc_available` | `BOOL` |
| 4 | `fw_version` | `UTF8` |
| 5 | `boot_id` | `U64` |
| 6 | `watering_status` | `U8` |
| 7 | `watering_state` | `U8` |
| 8 | `power_mode` | `U8` |
| 13 | `resource_version` | `U32` |
| 14 | `timestamp_utc_ms` | `U64` |

## 3.2 `rtc_tz.timezone_config` field IDs (`TZ_*`)

| ID | Name | Type | Notes |
|---:|---|---|---|
| 1 | `utc_offset_minutes` | `I32` | valid `[-720..840]` |
| 2 | `dst_enabled` | `BOOL` | `0/1` |
| 3 | `dst_start_month` | `U8` | 1..12 if DST enabled |
| 4 | `dst_start_week` | `U8` | 1..5 if DST enabled |
| 5 | `dst_start_dow` | `U8` | 0..6 if DST enabled |
| 6 | `dst_end_month` | `U8` | 1..12 if DST enabled |
| 7 | `dst_end_week` | `U8` | 1..5 if DST enabled |
| 8 | `dst_end_dow` | `U8` | 0..6 if DST enabled |
| 9 | `dst_offset_minutes` | `I32` | valid `[-120..120]` |

## 3.3 `rtc_tz.rtc_time` field IDs (`RTC_*`)

| ID | Name | Type |
|---:|---|---|
| 1 | `second` | `U8` |
| 2 | `minute` | `U8` |
| 3 | `hour` | `U8` |
| 4 | `day` | `U8` |
| 5 | `month` | `U8` |
| 6 | `year` | `U16` |
| 7 | `day_of_week` | `U8` |
| 8 | `unix_utc` | `U32` |

## 3.4 `task.status` field IDs (`TASK_*`)

| ID | Name | Type |
|---:|---|---|
| 1 | `active` | `BOOL` |
| 2 | `paused` | `BOOL` |
| 3 | `pending_count` | `U8` |
| 4 | `current_channel_id` | `U8` (`0xFF` if none) |
| 5 | `trigger_type` | `U8` |
| 6 | `watering_mode` | `U8` |
| 7 | `target_value` | `U32` |
| 8 | `completed_count` | `U32` |
| 9 | `has_current` | `BOOL` |

## 3.5 `channel.state` / `channel.config` field IDs (`RES_CH_*`)

| ID | Name | Type |
|---:|---|---|
| 1 | `channel_id` | `U8` |
| 2 | `name` | `UTF8` |
| 3 | `active` | `BOOL` |
| 4 | `last_watering_ts` | `U32` |
| 5 | `plant_id` | `U16` |
| 6 | `config_score` | `U8` |
| 7 | `soil_db_index` | `U8` |
| 8 | `irrigation_method_index` | `U8` |
| 9 | `use_area_based` | `BOOL` |
| 10 | `area_m2` | `F32` |
| 11 | `plant_count` | `U16` |
| 12 | `auto_mode` | `U8` |
| 13 | `max_volume_limit_l` | `F32` |
| 14 | `cycle_soak_enabled` | `BOOL` |
| 15 | `planting_date_unix` | `U32` |
| 16 | `sun_exposure_pct` | `U8` |
| 17 | `latitude_deg` | `F32` |
| 18 | `longitude_deg` | `F32` |

## 3.6 `schedule.config` field IDs (`RES_SCH_*`)

| ID | Name | Type |
|---:|---|---|
| 1 | `schedule_type` | `U8` |
| 2 | `watering_mode` | `U8` |
| 3 | `days_mask` | `U8` |
| 4 | `interval_days` | `U8` |
| 5 | `duration_minutes` | `U8` |
| 6 | `volume_liters` | `U16` |
| 7 | `start_hour` | `U8` |
| 8 | `start_minute` | `U8` |
| 9 | `auto_enabled` | `BOOL` |
| 10 | `use_solar_timing` | `BOOL` |
| 11 | `solar_event` | `U8` |
| 12 | `solar_offset_minutes` | `I32` (validated to `[-127..127]`) |

## 3.7 `flow.status` field IDs (`FLOW_*`)

| ID | Name | Type |
|---:|---|---|
| 1 | `pulse_count` | `U32` |
| 2 | `rate_pps` | `U32` |
| 3 | `calibration_pulses_per_liter` | `U32` |

## 3.8 `rain.status` field IDs (`RAIN_*`)

| ID | Name | Type |
|---:|---|---|
| 1 | `total_pulses` | `U32` |
| 2 | `last_pulse_ts` | `U32` |
| 3 | `current_hour_mm` | `F32` |
| 4 | `hourly_rate_mmh` | `F32` |
| 5 | `sensor_status` | `U8` |
| 6 | `data_quality` | `U8` |
| 7 | `enabled` | `BOOL` |
| 8 | `integration_enabled` | `BOOL` |
| 9 | `calibration_mm_per_pulse` | `F32` |
| 10 | `debounce_ms` | `U16` |
| 11 | `last_24h_mm` | `F32` |
| 12 | `history_hourly_count` | `U16` |
| 13 | `history_daily_count` | `U16` |

## 3.9 `environment.current` field IDs (`ENV_*`)

| ID | Name | Type |
|---:|---|---|
| 1 | `temperature_c` | `F32` |
| 2 | `humidity_pct` | `F32` |
| 3 | `pressure_hpa` | `F32` |
| 4 | `reading_ts` | `U32` |
| 5 | `valid` | `BOOL` |
| 6 | `readings_count` | `U16` |
| 7 | `last_update_ts` | `U32` |
| 8 | `hist_hourly_used` | `U16` |
| 9 | `hist_daily_used` | `U16` |
| 10 | `hist_monthly_used` | `U16` |

## 3.10 `history.storage` field IDs

| ID | Name | Type |
|---:|---|---|
| 1 | `detailed_events_bytes` | `U32` |
| 2 | `daily_stats_bytes` | `U32` |
| 3 | `monthly_stats_bytes` | `U32` |
| 4 | `annual_stats_bytes` | `U32` |
| 5 | `total_storage_kb` | `U32` |

## 3.11 `packs.stats` field IDs

| ID | Name | Type |
|---:|---|---|
| 1 | `total_bytes` | `U32` |
| 2 | `used_bytes` | `U32` |
| 3 | `free_bytes` | `U32` |
| 4 | `pack_count` | `U16` |
| 5 | `plant_count` | `U16` |
| 6 | `custom_plant_count` | `U16` |
| 7 | `change_counter` | `U32` |
| 8 | `staging_valid` | `BOOL` |
| 9 | `staging_len` | `U16` |
| 10 | `staging_crc32c` | `U32` |

## 3.12 `ota` command field IDs (`OTA_*`)

| ID | Name | Type (depends on command) |
|---:|---|---|
| 1 | `supported` | `BOOL` |
| 2 | `mode` | `U8` |
| 3 | `wait_ms` | `U32` |
| 4 | `accepted` | `BOOL` |
| 5 | `reason` | `UTF8` |
| 6 | `fw_version` | `UTF8` |
| 7 | `ota_state` | `U8` |
| 8 | `pending` | `BOOL` |
| 9 | `confirm` | `BOOL` or `U32 token` (context-dependent) |
| 10 | `mcuboot_swap_type` | `I32` |
| 11 | `confirm_policy` | `U8` |
| 12 | `rollback_observed` | `BOOL` |

## 3.13 `schema_discovery` helper fields (`SCHEMA_*`)

| ID | Name | Type |
|---:|---|---|
| 2 | `impl_mask` | `U32` |
| 3 | `proto_caps` | `U32` |
| 4 | `service_mask` | `U32` |
| 5 | `topic_mask` | `U32` |
| 6 | `stream_dir_mask` | `U32` |
| 7 | `stream_purpose_mask` | `U32` |
| 8 | `name` | `UTF8` |
| 9 | `kind` | `U8` |
| 10 | `access_mask` | `U8` |
| 11 | `event_topic` | `U16` |
| 12 | `field_count` | `U8` |
| 13 | `field_id` | `U8` |
| 14 | `field_type` | `U8` |
| 15 | `field_flags` | `U8` |
| 16 | `unit` | `UTF8` |
| 17 | `desc_text` | `UTF8` |
| 18 | `query_index` | `U16` |
| 19 | `query_field_id` | `U8` |
| 20 | `stream_schema` | `U8` |
| 21 | `content_type` | `U8` |
| 22 | `page_offset` | `U16` |
| 23 | `total_len` | `U32` |
| 24 | `item_count` | `U16` |

## 3.14 `peer_power` field IDs

| ID | Name | Type |
|---:|---|---|
| 1 | `peer_on` | `BOOL` |
| 2 | `peer_hw_backed` | `BOOL` |
| 3 | `peer_inhibit` | `BOOL` |
| 4 | `peer_grace_ms` | `U16` |
| 5 | `peer_reason` | `U8` |
| 6 | `peer_policy_mode` | `U8` (`0=manual`, `1=auto`) |

## 3.15 `diagnostics` field IDs (selected groups)

- `diag.counters`: `DIAG_*` IDs `1..21`
- `diag.stream_status`: `DSTRM_*` IDs `1..9`
- `diag.domain_status`: `DDOM_*` IDs `1..17`
- `diag.storage_health`: `DSTOR_*` IDs `1..6`

See `src/ucp_uart.c` enums near top for numeric mapping.

## 3.16 Command / Query / Event Helper Field IDs (Exact)

### Query fields (`Q_*`)

| ID | Name | Type |
|---:|---|---|
| 100 | `Q_CHANNEL_ID` | `U8` |
| 101 | `Q_PAGE_INDEX` | `U16` |
| 102 | `Q_OFFSET` | `U16` |
| 103 | `Q_LIMIT` | `U16` |
| 104 | `Q_PACK_ID` | `U16` |
| 105 | `Q_DELETE_PLANTS` | `BOOL` |
| 106 | `Q_PLANT_ID` | `U16` |
| 107 | `Q_DATA_TYPE` | `U8` |
| 108 | `Q_START_TS` | `U32` |
| 109 | `Q_END_TS` | `U32` |
| 110 | `Q_DB_KIND` | `U8` |

### Alarm event fields (`ALARM_*`)

| ID | Name | Type |
|---:|---|---|
| 1 | `alarm_code` | `U8` |
| 2 | `alarm_data` | `U16` |
| 3 | `alarm_ts` | `U32` |
| 4 | `alarm_cleared` | `BOOL` |

### Valve publish fields (`VALVE_*`, direct valve event helper)

| ID | Name | Type |
|---:|---|---|
| 1 | `channel_id` | `U8` |
| 2 | `open` | `BOOL` |
| 3 | `is_master` | `BOOL` |

### Reset command fields (`RESET_*`)

| ID | Name | Type |
|---:|---|---|
| 1 | `reset_type` | `U8` |
| 2 | `reset_channel` | `U8` |
| 3 | `reset_code` | `U32` |
| 4 | `reset_group` | `U8` |
| 5 | `reset_reason` | `UTF8` |

### Hydraulic delta helper fields (`HYD_*`, used in some deltas/sets)

| ID | Name | Type |
|---:|---|---|
| 32 | `channel_profile` | resource-specific |
| 33 | `channel_tol_high` | resource-specific |
| 34 | `channel_tol_low` | resource-specific |
| 35 | `channel_monitor_enabled` | resource-specific |

## 4. Services and Methods (Exhaustive)

All IDs below come from `src/ucp_protocol.h`.

## 4.1 Service `session` (`0x0001`)

### Session Messages (not resource-based)

- `HELLO_REQ` / `HELLO_RESP`
- `PING_REQ` / `PING_RESP`
- `SUB_REQ` / `SUB_RESP`
- `UNSUB_REQ` / `UNSUB_RESP`

See exact wire payloads in `wire-reference.md`.

## 4.2 Service `device/system` (`0x0010`)

### `GET device.system_status` (`res=0x0001`)

- Request payload: empty
- Response payload: typed TLV (`SYS_*` fields) + common metadata
- Notify topic: `UCP_TOPIC_SYSTEM_STATUS` (`1`)

### `EVENT device.alarm_event` (`res=0x0002`, event-only)

- No `GET/SET`
- Event topic: `UCP_TOPIC_ALARM` (`5`)
- Delta payload fields (`ALARM_*`)
  - `ALARM_CODE_F` (`U8`)
  - `ALARM_DATA_F` (`U16`)
  - `ALARM_TS_F` (`U32`)
  - `ALARM_CLEARED_F` (`BOOL`)

### `GET/SET/EVENT device.system_config` (`res=0x0003`)

- `GET_REQ` payload: empty
- `GET_RESP` payload: `RAW_BYTES_F` wrapper (`enhanced_system_config_data`)
- `SET_REQ` payload:
  - `RAW_BYTES_F` required (`len == sizeof(enhanced_system_config_data)`)
- `SET_RESP`: standard `SET_OK`
- Side effects:
  - applies system config
  - increments `g_ver_syscfg`
  - emits `UCP_TOPIC_SYSTEM_CONFIG`

### `GET/CMD/EVENT device.statistics` (`res=0x0004`)

- `GET_REQ` payload:
  - optional `RES_CH_ID` or `Q_CHANNEL_ID` (`U8`)
  - default channel `0`
- `GET_RESP` payload:
  - `RAW_BYTES_F` wrapper (`statistics_data`)
- `SET`: unsupported (reset via commands)
- Commands:
  - `device.stats_reset_channel` (`cmd=0x0001`)
    - req: optional selector (`RES_CH_ID`/`Q_CHANNEL_ID`), default `0`
    - resp: `RES_CH_ID`, `UCP_F_OK_BOOL`
    - emits `UCP_TOPIC_STATISTICS`
  - `device.stats_reset_all` (`cmd=0x0002`)
    - req: empty
    - resp: `UCP_F_OK_BOOL`
    - emits `UCP_TOPIC_STATISTICS` (`instance_id=0xFFFFFFFF`)

### `GET/SET/CMD/EVENT device.auto_calc_status` (`res=0x0005`)

- `GET_REQ`:
  - optional `RES_CH_ID` or `Q_CHANNEL_ID`
  - default `0xFF`, `0xFF` allowed (global)
- `GET_RESP`:
  - `RAW_BYTES_F` wrapper (`auto_calc_status_data`)
- `SET_REQ`:
  - `RAW_BYTES_F` required (`auto_calc_status_data`)
  - current implementation applies global enable via `calculation_active`
- `SET_RESP`: standard `SET_OK`
- Events: `UCP_TOPIC_AUTO_CALC_STATUS`

Command:

- `device.run_auto_calc` (`cmd=0x0003`)
  - req: empty
  - resp: `UCP_F_OK_BOOL`
  - emits `UCP_TOPIC_AUTO_CALC_STATUS`

### `GET/SET/EVENT device.auto_calc_control` (`res=0x0008`)

- `GET_REQ`: empty
- `GET_RESP`: `RAW_BYTES_F` wrapper (`ucp_auto_calc_control_data_v1`)
- `SET_REQ`:
  - `RAW_BYTES_F` required
  - validation:
    - `enabled` in `{0,1}`
    - `interval_hours` in `1..24`
- `SET_RESP`: standard `SET_OK`
- Events: `UCP_TOPIC_AUTO_CALC_STATUS`

### `GET/STREAM/EVENT device.bulk_snapshot` (`res=0x0006`)

- `GET_REQ`: empty
- `GET_RESP`: `RAW_BYTES_F` wrapper (`ucp_bulk_snapshot_v1`)
- `STREAM_OUT`: `purpose=11` (`bulk_snapshot`)
- `SET`: unsupported
- Topic: `UCP_TOPIC_BULK_SNAPSHOT`

### `GET/CMD/EVENT device.alarm_status` (`res=0x0007`)

- `GET_REQ`: empty
- `GET_RESP`: typed alarm status TLV (alarm fields + metadata)
- `SET`: unsupported
- Command `device.alarm_clear` (`cmd=0x0005`)
  - req: optional `ALARM_CODE_F` (`U8`)
  - resp: `ALARM_CODE_F`, `UCP_F_OK_BOOL`
  - topic: `UCP_TOPIC_ALARM`

### `CMD device.clear_errors` (`cmd=0x0004`)

- req: empty
- resp: `UCP_F_OK_BOOL`
- clears watering + rain sensor errors

## 4.3 Service `channel` (`0x0011`)

### `GET channel.state` (`res=0x0001`)

- `GET_REQ`:
  - required `RES_CH_ID` (`U8`)
- `GET_RESP`:
  - typed TLV fields: `RES_CH_ID`, `RES_CH_ACTIVE`, `RES_CH_LAST_WATER_TS`, `RES_CH_PLANT_ID`, `RES_CH_CFG_SCORE`
  - metadata: `UCP_F_INSTANCE_ID`, `UCP_F_RESOURCE_VERSION`, `UCP_F_TIMESTAMP_UTC_MS`
- `SET`: unsupported (use commands for valve/manual actions)
- topic: `UCP_TOPIC_CHANNEL_STATE`

### `GET/SET/EVENT channel.config` (`res=0x0002`)

- `GET_REQ`: required `RES_CH_ID`
- `GET_RESP`: typed TLV (`RES_CH_*` config fields) + metadata
- `SET_REQ`:
  - required selector: `RES_CH_ID`
  - accepted delta fields:
    - `RES_CH_NAME` (`UTF8`, up to 63 bytes copied)
    - `RES_CH_PLANT_ID` (`U16`)
    - `RES_CH_SOIL_DB` (`U8`)
    - `RES_CH_IRR_METHOD` (`U8`)
    - `RES_CH_AUTO_MODE` (`U8`)
    - `RES_CH_SUN_PCT` (`U8`, `0..100`)
    - `RES_CH_USE_AREA` (`BOOL`)
    - `RES_CH_CYCLE_SOAK` (`BOOL`)
    - `RES_CH_PLANT_COUNT` (`U16`)
    - `RES_CH_PLANTING_TS` (`U32`)
    - `RES_CH_AREA_M2` (`F32`, `0..100000`)
    - `RES_CH_MAX_VOL_L` (`F32`, `0..10000`)
    - `RES_CH_LAT_DEG` (`F32`, `-90..90`)
    - `RES_CH_LON_DEG` (`F32`, `-180..180`)
  - unknown fields ignored
- `SET_RESP`: standard `SET_OK`
- Side effects:
  - updates channel, recomputes days-after-planting, persists config
  - emits `UCP_TOPIC_CHANNEL_CONFIG` with delta

### `GET/SET/EVENT channel.custom_soil` (`res=0x0003`)

- `GET_REQ`: required `RES_CH_ID`
- `GET_RESP`: `RAW_BYTES_F` wrapper (`custom_soil_config_data`)
- `SET_REQ`:
  - `RAW_BYTES_F` required
  - optional selector alias (`RAW_SELECTOR_F` / `RES_CH_ID` / `Q_CHANNEL_ID`)
- `SET_RESP`: standard `SET_OK`
- topic: `UCP_TOPIC_CUSTOM_SOIL`

### `GET/SET/EVENT channel.soil_moisture` (`res=0x0004`)

- `GET_REQ`:
  - optional selector (`RES_CH_ID` or `Q_CHANNEL_ID`)
  - default `0xFF` (global), `0xFF` accepted
- `GET_RESP`: `RAW_BYTES_F` wrapper (`soil_moisture_config_data`)
- `SET_REQ`: `RAW_BYTES_F` + optional selector alias
- `SET_RESP`: standard `SET_OK`
- topic: `UCP_TOPIC_SOIL_MOISTURE`

### `GET/SET/EVENT channel.interval_config` (`res=0x0005`)

- `GET_REQ`: required `RES_CH_ID`
- `GET_RESP`: `RAW_BYTES_F` wrapper (concatenated interval config+status bytes)
- `SET_REQ`: `RAW_BYTES_F` required (must contain at least config prefix), optional selector alias
- `SET_RESP`: standard `SET_OK`
- topic: `UCP_TOPIC_INTERVAL_MODE`

### `GET/EVENT channel.config_status` (`res=0x0006`)

- `GET_REQ`: required `RES_CH_ID`
- `GET_RESP`: `RAW_BYTES_F` wrapper (`config_status_response_data`)
- `SET`: unsupported
- topic: `UCP_TOPIC_CHANNEL_CFG_STATUS`

### Channel Commands (`CMD`, service `0x0011`)

#### `channel.valve_open` (`0x0001`) / `channel.valve_close` (`0x0002`)

- req:
  - required `RES_CH_ID` or `Q_CHANNEL_ID` (`U8`)
- resp:
  - `RES_CH_ID` (`U8`)
  - `UCP_F_OK_BOOL` (`BOOL`)
- side effects:
  - direct valve control
  - emits valve/channel state event path

#### `channel.master_open` (`0x0003`) / `channel.master_close` (`0x0004`)

- req: empty
- resp:
  - `UCP_F_OK_BOOL`
- side effects:
  - manual master valve control
  - publishes valve state event (`is_master=true`)

#### `channel.queue_duration_task` (`0x0005`) / `channel.queue_volume_task` (`0x0006`)

- req:
  - required `RES_CH_ID` or `Q_CHANNEL_ID` (`U8`)
  - required value (`U16`) via `Q_LIMIT` (current parser also accepts `RES_CH_ACTIVE` as alias)
- resp:
  - `RES_CH_ID`
  - `Q_LIMIT` (echo value)
  - `UCP_F_OK_BOOL`
- side effects:
  - queues watering task
  - triggers task status event refresh

## 4.4 Service `schedule` (`0x0012`)

### `GET/SET/EVENT schedule.config` (`res=0x0001`)

- `GET_REQ`: required `RES_CH_ID`
- `GET_RESP`: typed TLV `RES_SCH_*` + `RES_CH_ID` + metadata
- `SET_REQ`:
  - required `RES_CH_ID`
  - accepted delta fields:
    - all `RES_SCH_*`
  - validation:
    - `RES_SCH_SOLAR_OFF_MIN` in `[-127..127]`
    - `watering_validate_event_config(after)` must pass
    - if `schedule_type=auto` and `auto_enabled=1`, auto mode must be valid for channel
- `SET_RESP`: standard `SET_OK`
- topic: `UCP_TOPIC_SCHEDULE_CONFIG`

## 4.5 Service `task` (`0x0013`)

### `GET/EVENT task.status` (`res=0x0001`)

- `GET_REQ`: empty
- `GET_RESP`: typed TLV `TASK_*` + metadata
- `SET`: unsupported
- topic: `UCP_TOPIC_TASK_CURRENT`

### Task Commands (`CMD`)

- `task.stop_current` (`0x0001`)
- `task.pause_current` (`0x0002`)
- `task.resume_current` (`0x0003`)
  - req: empty
  - resp: `UCP_F_OK_BOOL`
  - if command cannot apply, returns `BUSY`

- `task.cancel_all` (`0x0004`)
- `task.start_next` (`0x0005`)
- `task.clear_pending` (`0x0006`)
  - req: empty
  - resp:
    - field `2` (`I32`) raw return code from internal function
    - `UCP_F_OK_BOOL`
  - failure status: `BUSY`

All successful task commands trigger task status refresh/event.

## 4.6 Service `flow` (`0x0014`)

### `GET/SET/EVENT flow.status` (`res=0x0001`)

- `GET_REQ`: empty
- `GET_RESP`: typed `FLOW_*` + metadata
- `SET_REQ`:
  - accepted field: `FLOW_CAL_PPL` (`U32`) only
- `SET_RESP`: standard `SET_OK`
- topic: `UCP_TOPIC_FLOW_LIVE`

### `GET/SET/CMD/EVENT flow.hydraulic_status` (`res=0x0002`)

- `GET_REQ`: optional selector (`RES_CH_ID` or `Q_CHANNEL_ID`), default `0`
- `GET_RESP`: `RAW_BYTES_F` wrapper (`hydraulic_status_data`)
- `SET_REQ`: `RAW_BYTES_F` required; optional selector alias may override embedded `channel_id`
- `SET_RESP`: standard `SET_OK`
- topic: `UCP_TOPIC_HYDRAULIC_STATUS`

Commands (`flow.hydraulic_*`):

- `clear_channel_lock` (`0x0001`)
  - req: optional selector (defaults `0`, send explicit channel)
  - resp: `RES_CH_ID`
- `clear_global_lock` (`0x0002`)
  - req: empty
  - resp: empty payload
- `set_manual_override` (`0x0003`)
  - req: optional selector (channel)
  - duration is fixed internally (`300000 ms`)
  - resp: `RES_CH_ID`
- `clear_manual_override` (`0x0004`)
  - req: empty
  - resp: empty payload

All successful commands emit `UCP_TOPIC_HYDRAULIC_STATUS`.

### `GET/SET/EVENT flow.calibration_status` (`res=0x0003`)

- `GET_REQ`: empty
- `GET_RESP`: `RAW_BYTES_F` wrapper (`calibration_data`)
- `SET_REQ`: `RAW_BYTES_F` wrapper (`calibration_data`) interpreted by `action`
- `SET_RESP`: standard `SET_OK` or error
- topic: `UCP_TOPIC_FLOW_CALIBRATION`

## 4.7 Service `rain` (`0x0015`)

### `GET/SET/EVENT rain.status` (`res=0x0001`)

- `GET_REQ`: empty
- `GET_RESP`: typed TLV `RAIN_*` + metadata
- `SET_REQ` accepted fields:
  - `RAIN_CALIB_MM_PER_PULSE` (`F32`)
  - `RAIN_DEBOUNCE_MS` (`U16`)
  - `RAIN_ENABLED` (`BOOL`)
  - `RAIN_INTEGRATION_ENABLED` (`BOOL`)
- `SET_RESP`: standard `SET_OK`
- topic: `UCP_TOPIC_RAIN_LIVE`

## 4.8 Service `environment` (`0x0016`)

### `GET/EVENT environment.current` (`res=0x0001`)

- `GET_REQ`: empty
- `GET_RESP`: typed TLV `ENV_*` + metadata
- topic: `UCP_TOPIC_ENV_LIVE`

### `GET/SET environment.sensor_config` (`res=0x0002`)

- `GET_REQ`: empty
- `GET_RESP`: `RAW_BYTES_F` wrapper (`bme280_config_t`)
- `SET_REQ`: `RAW_BYTES_F` required (`len == sizeof(bme280_config_t)`)
- `SET_RESP`: standard `SET_OK`
- current implementation does not emit a dedicated environment config topic for this resource

## 4.9 Service `history` (`0x0017`)

### `GET/EVENT history.storage` (`res=0x0001`)

- `GET_REQ`: empty
- `GET_RESP`: typed TLV storage summary + metadata
- topic: `UCP_TOPIC_HISTORY_READY`

### `GET/EVENT history.rain_storage` (`res=0x0002`)

- `GET_REQ`: empty
- `GET_RESP`: `RAW_BYTES_F` wrapper (`rain_history_stats_t`)
- topic: `UCP_TOPIC_HISTORY_READY`

### `GET/EVENT history.environment_storage` (`res=0x0003`)

- `GET_REQ`: empty
- `GET_RESP`: `RAW_BYTES_F` wrapper (`env_history_stats_t`)
- topic: `UCP_TOPIC_HISTORY_READY`

### History Commands

- `history.reset_all` (`0x0001`)
  - req: empty
  - resp: `UCP_F_OK_BOOL`
  - event: `UCP_TOPIC_HISTORY_READY`
- `history.reset_channel` (`0x0002`)
  - req: required `Q_CHANNEL_ID` or `RES_CH_ID`
  - resp: `UCP_F_OK_BOOL`, `RES_CH_ID`
  - event: `UCP_TOPIC_HISTORY_READY` with `instance_id = channel`
- `history.reset_rain` (`0x0003`)
  - req: empty
  - resp: `UCP_F_OK_BOOL`
  - event resource: `history.rain_storage`
- `history.reset_env` (`0x0004`)
  - req: empty
  - resp: `UCP_F_OK_BOOL`
  - event resource: `history.environment_storage`

### History Streams (`STREAM_*`)

- `purpose=4` `history_page`
  - req query: `Q_CHANNEL_ID`, `Q_PAGE_INDEX`
- `purpose=8` `rain_history_range`
  - req query: `Q_DATA_TYPE` (+ optional range/limit)
- `purpose=9` `env_history_range`
  - req query: `Q_DATA_TYPE` (+ optional range/limit)
- `purpose=12` `water_history_range`
  - req query: `Q_CHANNEL_ID`, `Q_DATA_TYPE` (+ optional range/limit)

Exact stream binary headers are in `wire-reference.md`.

## 4.10 Service `rtc_tz` (`0x0018`)

### `GET/SET/EVENT rtc_tz.timezone_config` (`res=0x0001`)

- `GET_REQ`: empty
- `GET_RESP`: typed TLV `TZ_*` + metadata
- `SET_REQ`: delta fields from `TZ_*`
- Validation:
  - `tz_valid()` checks all ranges and DST rules
- `SET_RESP`: `UCP_F_OK_BOOL`, `UCP_F_RESOURCE_VERSION`, `UCP_F_TIMESTAMP_UTC_MS`
- topic: `UCP_TOPIC_RTC_TZ_CHANGED`

### `GET/SET/EVENT rtc_tz.rtc_time` (`res=0x0002`)

- `GET_REQ`: empty
- `GET_RESP`: typed TLV `RTC_*` + metadata
- `SET_REQ`: accepts delta of:
  - `RTC_SEC`, `RTC_MIN`, `RTC_HOUR`, `RTC_DAY`, `RTC_MONTH`, `RTC_YEAR`, `RTC_DOW`
  - `RTC_UNIX_UTC` (alternative full timestamp setter)
- Validation:
  - `rtc_datetime_set()` range validation
  - `RTC_UNIX_UTC` converts via `timezone_unix_to_rtc_utc()`
- `SET_RESP`: `UCP_F_OK_BOOL`, `UCP_F_RESOURCE_VERSION`, `UCP_F_TIMESTAMP_UTC_MS`
- topic: `UCP_TOPIC_RTC_TZ_CHANGED`

## 4.11 Service `compensation` (`0x0019`)

### `GET/EVENT compensation.status` (`res=0x0001`)

- `GET_REQ`: optional selector (`RES_CH_ID` or `Q_CHANNEL_ID`), default `0`
- `GET_RESP`: `RAW_BYTES_F` wrapper (`compensation_status_data`)
- topic: `UCP_TOPIC_COMP_STATUS`

### `GET/SET/EVENT compensation.channel_config` (`res=0x0002`)

- `GET_REQ`: optional selector (`RES_CH_ID` or `Q_CHANNEL_ID`), default `0`
- `GET_RESP`: `RAW_BYTES_F` wrapper (`channel_compensation_config_data`)
- `SET_REQ`: `RAW_BYTES_F` required; optional selector alias may override `channel_id`
- `SET_RESP`: standard `SET_OK`
- topic: `UCP_TOPIC_COMP_CONFIG`

### `GET/EVENT compensation.rain_integration` (`res=0x0003`)

- `GET_REQ`: empty
- `GET_RESP`: `RAW_BYTES_F` wrapper (`rain_integration_status_ble`)
- topic: `UCP_TOPIC_RAIN_INTEGRATION`

## 4.12 Service `onboarding_reset` (`0x001A`)

### `GET/EVENT onboarding_reset.onboarding_status` (`res=0x0001`)

- `GET_REQ`: empty
- `GET_RESP`: `RAW_BYTES_F` wrapper (`onboarding_status_data`)
- topic: `UCP_TOPIC_ONBOARDING_STATUS`

### `GET/EVENT onboarding_reset.system_overview` (`res=0x0002`)

- `GET_REQ`: empty
- `GET_RESP`: `RAW_BYTES_F` wrapper (`config_system_overview_t`)
- topic: `UCP_TOPIC_ONBOARDING_STATUS`

### `GET/CMD/EVENT onboarding_reset.config_status` (`res=0x0003`)

- `GET_REQ`: required `RES_CH_ID`
- `GET_RESP`: `RAW_BYTES_F` wrapper (`config_status_response_data`)
- topic: `UCP_TOPIC_CHANNEL_CFG_STATUS`

### `GET/CMD/EVENT onboarding_reset.reset_control` (`res=0x0004`)

- `GET_REQ`: empty
- `GET_RESP`: `RAW_BYTES_F` wrapper (`reset_control_data`)
- topic: `UCP_TOPIC_RESET_PROGRESS`

### Onboarding/Reset Commands (`CMD`)

Field IDs used:

- `RESET_TYPE_F` (`U8`)
- `RESET_CHANNEL_F` (`U8`)
- `RESET_CODE_F` (`U32`)
- `RESET_GROUP_F` (`U8`)
- `RESET_REASON_F` (`UTF8`, max 31 bytes copied)

Commands:

- `onb.reset_gen_code` (`0x0001`)
  - req: `RESET_TYPE_F` required, `RESET_CHANNEL_F` optional
  - resp:
    - `RESET_CODE_F`
    - `RESET_TYPE_F`
    - `RESET_CHANNEL_F`
    - `OTA_WAIT` (`U32`) reused as confirmation TTL ms
  - event: `UCP_TOPIC_RESET_PROGRESS`

- `onb.reset_execute` (`0x0002`)
  - req: `RESET_TYPE_F`, `RESET_CODE_F` required; `RESET_CHANNEL_F` optional
  - resp: `RESET_TYPE_F`, `RESET_CHANNEL_F`, `UCP_F_OK_BOOL`
  - events:
    - `UCP_TOPIC_RESET_PROGRESS`
    - `UCP_TOPIC_ONBOARDING_STATUS`

- `onb.reset_clear_confirm` (`0x0003`)
  - req: empty
  - resp: `UCP_F_OK_BOOL`
  - event: `UCP_TOPIC_RESET_PROGRESS`

- `onb.reset_clear_wipe_state` (`0x0004`)
  - req: empty
  - resp: `UCP_F_OK_BOOL`
  - event: `UCP_TOPIC_RESET_PROGRESS`

- `onb.onboarding_reset_state` (`0x0005`)
  - req: empty
  - resp: `UCP_F_OK_BOOL`
  - event: `UCP_TOPIC_ONBOARDING_STATUS`

- `onb.config_reset_group` (`0x0006`)
  - req: `RESET_CHANNEL_F` + `RESET_GROUP_F` required, `RESET_REASON_F` optional
  - resp: `RESET_CHANNEL_F`, `RESET_GROUP_F`, `UCP_F_OK_BOOL`
  - events:
    - `UCP_TOPIC_CHANNEL_CFG_STATUS` (instance=channel)
    - `UCP_TOPIC_ONBOARDING_STATUS`

## 4.13 Service `packs` (`0x001B`)

### `GET/CMD/EVENT packs.stats` (`res=0x0001`)

- `GET_REQ`: empty
- `GET_RESP`: typed `PACKS_*` fields + metadata
- topic: `UCP_TOPIC_PACKS_CHANGED`

### Packs Commands

Shared query field IDs:

- `Q_PACK_ID` (`U16`)
- `Q_PLANT_ID` (`U16`)
- `Q_DELETE_PLANTS` (`BOOL`)
- `Q_LIMIT` (`U16`) (used for echoing pack bundle plant count)

#### `packs.delete_pack` (`0x0001`)

- req:
  - `Q_PACK_ID` required
  - `Q_DELETE_PLANTS` optional (`default false`)
- resp:
  - `Q_PACK_ID`
  - `Q_DELETE_PLANTS`
  - optional `PACKS_CHANGE_COUNTER`
- event: `UCP_TOPIC_PACKS_CHANGED`

#### `packs.delete_plant` (`0x0002`)

- req: `Q_PLANT_ID` required
- resp:
  - `Q_PLANT_ID`
  - optional `PACKS_CHANGE_COUNTER`
- event: `UCP_TOPIC_PACKS_CHANGED`

#### `packs.install_staged_plant` (`0x0003`)

- precondition:
  - inbound staging blob valid
  - blob length `== sizeof(pack_plant_v1_t)`
- req: empty
- resp:
  - `Q_PLANT_ID` (from staged blob)
  - optional `PACKS_CHANGE_COUNTER`
- on success:
  - staging blob cleared
  - emits `UCP_TOPIC_PACKS_CHANGED`

#### `packs.install_staged_pack_bundle` (`0x0004`)

- precondition:
  - inbound staging blob valid
  - blob starts with `pack_pack_v1_t`
  - exact length matches `sizeof(pack_pack_v1_t) + plant_count*sizeof(pack_plant_v1_t)`
- req: empty
- resp:
  - `Q_PACK_ID`
  - `Q_LIMIT` (echo pack `plant_count`)
  - optional `PACKS_CHANGE_COUNTER`
- on success:
  - staging blob cleared
  - emits `UCP_TOPIC_PACKS_CHANGED`

### Packs Streams

- inbound:
  - `purpose=3` `upload_staging`
- outbound:
  - `purpose=5` `packs_list`
  - `purpose=6` `packs_plants_list`
  - `purpose=10` `db_catalog` (built-in catalogs)

## 4.14 Service `ota` (`0x001C`) - Commands Only

No `GET`/`SET` resources in v1. OTA orchestration is command-based.

Enumerations used by OTA payloads:

- `OTA_STATE`
  - `0` = idle
  - `1` = entering_recovery
  - `2` = reboot_failed
  - `3` = postcheck

### `ota.preflight` (`cmd=0x0001`)

- req: empty
- resp fields (current implementation):
  - `OTA_SUP` (`BOOL`) = supported
  - `OTA_MODE` (`U8`) = `1` (recovery-first)
  - `OTA_CONFIRM_POLICY` (`U8`) = `1`
  - `OTA_CONFIRM` (`BOOL`) current image confirmed
  - `OTA_SWAP` (`I32`) `mcuboot_swap_type()`
  - `OTA_ROLLBACK_OBS` (`BOOL`)
  - `OTA_ACC` (`BOOL`) whether buttonless recovery entry is available (`CONFIG_RETENTION_BOOT_MODE`)
  - `OTA_REASON` (`UTF8`) if unsupported/unavailable
  - `OTA_WAIT` (`U32`) currently `0` for bootmode path
  - `OTA_PENDING` (`BOOL`) reboot pending

### `ota.enter_recovery` (`cmd=0x0002`) - 2-step confirmation

Request:

- first call:
  - usually empty (or wrong/missing token)
- confirm call:
  - `OTA_CONFIRM` (`U32`) token from first response

Behavior:

1. If confirmation token missing/invalid/expired:
   - returns `CONFIRMATION_REQD`
   - payload includes:
     - `OTA_ACC=0`
     - `OTA_CONFIRM` (`U32` token)
     - `OTA_WAIT` (`U32` token TTL ms)
     - `OTA_REASON` (instructions)
2. If token valid:
   - sets reboot pending
   - returns `OK` with:
     - `OTA_ACC=1`
     - `OTA_MODE=1`
     - `OTA_WAIT=0`
     - `OTA_REASON="bootmode+reboot"`
   - emits `UCP_TOPIC_OTA_TRANSITION`
   - emits `UCP_TOPIC_PEER_POWER` (inhibit state change)
   - schedules reboot to MCUboot recovery handoff

Possible failure statuses:

- `UNSUPPORTED` if `RETENTION_BOOT_MODE` unavailable
- `BUSY` if reboot already pending
- `INVALID_ARG` if token TLV type wrong

### `ota.post_check` (`cmd=0x0003`)

- req: empty
- resp:
  - `OTA_FW` (`UTF8`) app version
  - `UCP_F_UPTIME_MS` (`U32`)
  - `OTA_STATE` (`U8` = postcheck)
  - `OTA_PENDING` (`BOOL`)
  - `OTA_CONFIRM` (`BOOL`) image confirmed
  - `OTA_CONFIRM_POLICY` (`U8`)
  - `OTA_SWAP` (`I32`)
  - `OTA_ROLLBACK_OBS` (`BOOL`)
  - optional `OTA_REASON` if rollback observed
- also emits `UCP_TOPIC_OTA_TRANSITION` (`postcheck`)

## 4.15 Service `peer_power` (`0x001D`)

### `GET/CMD/EVENT peer_power.state` (`res=0x0001`)

- `GET_REQ`: empty
- `GET_RESP` typed fields:
  - `PEER_STATE_ON` (`BOOL`)
  - `PEER_STATE_HW_BACKED` (`BOOL`)
  - `PEER_STATE_INHIBIT` (`BOOL`)
  - `PEER_STATE_GRACE_MS` (`U16`)
  - metadata
- topic: `UCP_TOPIC_PEER_POWER`

### `GET/SET/EVENT peer_power.policy` (`res=0x0002`)

- `GET_REQ`: empty
- `GET_RESP` typed fields:
  - `PEER_POLICY_MODE` (`U8`) (`0=manual`, `1=auto`)
  - `PEER_STATE_GRACE_MS` (`U16`)
  - metadata
- `SET_REQ` accepted fields:
  - `PEER_POLICY_MODE` (`U8`, must be `0` or `1`)
  - `PEER_STATE_GRACE_MS` (`U16`, max `10000`)
- `SET_RESP`:
  - `UCP_F_OK_BOOL`
  - `UCP_F_RESOURCE_VERSION`
  - `UCP_F_TIMESTAMP_UTC_MS`
- topic: `UCP_TOPIC_PEER_POWER`

Enumerations used by `peer_power` payloads:

- `PEER_POLICY_MODE`
  - `0` = manual
  - `1` = auto
- `PEER_REASON`
  - `0` = none
  - `1` = uart_cmd
  - `2` = ota

### Peer Power Commands

- `peer_power.off` (`0x0001`)
  - req: empty
  - resp (success): `PEER_STATE_ON=0`, `PEER_STATE_HW_BACKED`
  - may return `LOCKED` with `PEER_STATE_INHIBIT=1` when OTA inhibits power changes
- `peer_power.on` (`0x0002`)
  - req: empty
  - resp: `PEER_STATE_ON=1`, `PEER_STATE_HW_BACKED`
- `peer_power.cycle` (`0x0003`)
  - req: empty
  - resp: `PEER_STATE_ON=1`
  - may return `LOCKED` if inhibited

All successful peer power commands emit `UCP_TOPIC_PEER_POWER`.

## 4.16 Service `diagnostics` (`0x001E`)

### `GET/EVENT diag.counters` (`res=0x0001`)

- `GET_REQ`: empty
- `GET_RESP`: typed `DIAG_*` fields + metadata
- topic: `UCP_TOPIC_DIAGNOSTICS`

### `GET diag.stream_status` (`res=0x0002`)

- `GET_REQ`: empty
- `GET_RESP`: typed `DSTRM_*` fields + metadata

### `GET diag.domain_status` (`res=0x0003`)

- `GET_REQ`: empty
- `GET_RESP`: typed `DDOM_*` fields + metadata

### `GET diag.storage_health` (`res=0x0004`)

- `GET_REQ`: empty
- `GET_RESP`: typed `DSTOR_*` fields + metadata

## 4.17 Service `schema_discovery` (`0x00F0`)

### `GET schema.impl_mask` (`res=0x0001`)

- `GET_REQ`: empty
- `GET_RESP`:
  - `UCP_F_VERSION` (`U8`) = `1`
  - `SCHEMA_IMPL_MASK` (`U32`) currently `0x0000000F`

### `GET schema.proto_info` (`res=0x0002`)

- `GET_REQ`: empty
- `GET_RESP`:
  - `UCP_F_VERSION`
  - `UCP_F_CAPABILITIES`
  - `UCP_F_MAX_PAYLOAD`
  - `UCP_F_REPLAY_DEPTH`
  - `SCHEMA_STREAM_DIR_MASK`
  - `SCHEMA_STREAM_PURPOSE_MASK`
  - metadata

### `GET schema.service_mask` (`res=0x0003`)

- `GET_REQ`: empty
- `GET_RESP`:
  - `UCP_F_VERSION`
  - `SCHEMA_SERVICE_MASK` (`U32`) compact bitmask for service IDs `1..32`
  - metadata
- Note:
  - sparse service `0x00F0` is not represented in this compact mask; use `schema_catalog` / `schema.resource_desc`

### `GET schema.topic_mask` (`res=0x0004`)

- `GET_REQ`: empty
- `GET_RESP`:
  - `UCP_F_TOPIC_MASK` (`U32`) implemented topics
  - metadata

### `GET schema.stream_caps` (`res=0x0005`)

- `GET_REQ`: empty
- `GET_RESP`:
  - `SCHEMA_STREAM_DIR_MASK`
  - `SCHEMA_STREAM_PURPOSE_MASK`
  - `STRM_CHUNK_SIZE` (max chunk)
  - `STRM_TOTAL_SIZE` (max stream buffer)
  - metadata

### `GET schema.resource_desc` (`res=0x0006`)

- `GET_REQ` required fields:
  - `UCP_F_SERVICE_ID` (`U16`)
  - `UCP_F_RESOURCE_ID` (`U16`)
- `GET_RESP`:
  - `UCP_F_SERVICE_ID`
  - `UCP_F_RESOURCE_ID`
  - `SCHEMA_KIND`
  - `SCHEMA_ACCESS_MASK`
  - `SCHEMA_EVENT_TOPIC`
  - `SCHEMA_FIELD_COUNT`
  - `SCHEMA_NAME`
  - `SCHEMA_DESC_TEXT`
  - metadata

### `GET schema.field_desc` (`res=0x0007`)

- `GET_REQ` required:
  - `UCP_F_SERVICE_ID` (`U16`)
  - `UCP_F_RESOURCE_ID` (`U16`)
  - `SCHEMA_QUERY_FIELD_ID` (`U8`) or `SCHEMA_FIELD_ID` (`U8`)
- `GET_RESP` (best-effort metadata):
  - `UCP_F_SERVICE_ID`
  - `UCP_F_RESOURCE_ID`
  - `SCHEMA_FIELD_ID`
  - `SCHEMA_FIELD_TYPE`
  - `SCHEMA_FIELD_FLAGS`
  - `SCHEMA_NAME`
  - `SCHEMA_UNIT`
  - `SCHEMA_DESC_TEXT`
  - metadata

### `GET schema.stream_purpose_desc` (`res=0x0008`)

- `GET_REQ` required:
  - `STRM_PURPOSE` (`U8`)
- `GET_RESP`:
  - `STRM_PURPOSE`
  - `SCHEMA_STREAM_DIR_MASK`
  - `STRM_CHUNK_SIZE`
  - `STRM_TOTAL_SIZE`
  - `SCHEMA_NAME`
  - `SCHEMA_DESC_TEXT`
  - metadata

### `STREAM_OUT schema_catalog` (`purpose=7`)

- query:
  - optional `Q_OFFSET` (`U16`) byte offset into catalog text
- content:
  - text catalog lines (`R ...`, `S ...`)
- wire format:
  - streamed as text bytes via `STREAM_DATA`

## 5. Topics (`EVENT_CHANGE`)

Implemented topic IDs and main resource payload families:

| Topic ID | Name | Typical resource / delta fields |
|---:|---|---|
| 1 | `system.status` | `device.system_status` (`SYS_*` subset) |
| 3 | `rtc_tz.changed` | `rtc_tz.timezone_config` / `rtc_tz.rtc_time` deltas |
| 4 | `task.current` | `task.status` |
| 5 | `alarm` | `device.alarm_event` (`ALARM_*`) and alarm-status related notifications |
| 6 | `ota.transition` | `OTA_*` subset |
| 7 | `peer.power` | `PEER_*` subset |
| 8 | `diagnostics` | `DIAG_*` subset |
| 9 | `channel.state` | `RES_CH_*` subset |
| 10 | `channel.config` | `RES_CH_*` config delta |
| 11 | `schedule.config` | `RES_SCH_*` delta |
| 12 | `flow.live` | `FLOW_*` subset |
| 13 | `rain.live` | `RAIN_*` subset |
| 14 | `env.live` | `ENV_*` subset |
| 15 | `history.ready` | history reset/change signal |
| 16 | `packs.changed` | packs stats/command result delta |
| 17 | `system_config` | `RAW_BYTES_F` wrapper delta |
| 18 | `statistics` | raw or command-result delta |
| 19 | `auto_calc_status` | raw or control delta |
| 20 | `hydraulic_status` | raw or command delta |
| 21 | `comp_status` | raw delta |
| 22 | `comp_config` | raw delta |
| 23 | `onboarding_status` | raw delta |
| 24 | `reset.progress` | reset progress/result delta |
| 25 | `custom_soil` | raw delta |
| 26 | `soil_moisture` | raw delta |
| 27 | `interval_mode` | raw delta |
| 28 | `channel_cfg_status` | raw delta |
| 29 | `rain_integration` | raw delta |
| 30 | `bulk_snapshot` | raw delta |
| 31 | `flow_calibration` | raw delta |

Topic `2` (`config.changed`) is reserved in IDs but not used as a primary emitted topic in current implementation.

## 6. Stream Purposes (Summary)

For exact wire payloads and binary headers:

- `wire-reference.md`

For raw record struct types:

- `raw-payload-abi-catalog.md`

Implemented purposes:

- `1` `diag_snapshot` (text)
- `2` `schema_text` (text)
- `3` `upload_staging` (inbound binary)
- `4` `history_page` (binary)
- `5` `packs_list` (binary)
- `6` `packs_plants_list` (binary)
- `7` `schema_catalog` (text)
- `8` `rain_history_range` (binary)
- `9` `env_history_range` (binary)
- `10` `db_catalog` (text)
- `11` `bulk_snapshot` (binary TLV blob)
- `12` `water_history_range` (binary)

## 7. Error/Validation Behavior (Implementation Notes)

- `HELLO` required before all other requests (`PRECONDITION_FAILED`)
- Type mismatch in TLVs => `INVALID_ARG`
- Range validation in typed setters returns `OUT_OF_RANGE`
- Backend/driver failures map to `BUSY`, `TIMEOUT`, `HW_FAULT`, `INTERNAL`
- Inbound stream chunk sequence/offset mismatch aborts stream with `STREAM_STATE_ERR`

## 8. What a Client Should Implement

Minimum robust client behavior:

1. Send `HELLO_REQ`
2. Parse `HELLO_RESP` (`capabilities`, `boot_id`, `event_seq`)
3. `SUB_REQ` with topic mask (optionally replay)
4. Parse `EVENT_CHANGE` and nested `UCP_F_DELTA_TLV`
5. Support `EVENT_OVERFLOW` -> resync via `GET`
6. Support `STREAM_*` if using history/packs/schema export
7. Handle `ota.enter_recovery` confirmation-token flow

## 9. Source of Truth References

- Protocol IDs and wire header: `src/ucp_protocol.h`
- TLV encoding: `src/ucp_tlv.h`, `src/ucp_tlv.c`
- Runtime handlers/builders: `src/ucp_uart.c`
- Event origin/topic enum mirror: `src/ucp_uart.h`

Implementation aids (docs-first):

- `docs/uart-api/examples-and-traces.md` (worked `HELLO`/`GET`/`SET`/`SUB`/`STREAM`/OTA flows)
- `docs/uart-api/client-implementation-guide.md` (host architecture, replay/resync, stream management)
