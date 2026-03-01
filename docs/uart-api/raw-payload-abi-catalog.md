# UCP UART v1 Raw Payload ABI Catalog (`RAW_BYTES_F`)

This document covers UCP resources that use the `RAW_BYTES_F` wrapper instead of typed TLV fields.

These payloads are intentionally ABI-compatible with existing internal/BLE-oriented structs.
On wire, UCP wraps them in a TLV envelope but the `RAW_BYTES_F` content is copied verbatim from C structs.

## 1. Wrapper Contract (Exact UCP TLVs)

### 1.1 `GET_RESP` / `EVENT_CHANGE` delta wrappers for raw resources

Current helper path (`build_raw_bytes_with_meta(...)`) emits:

1. `RAW_BYTES_F` (`field_id=1`, `type=BYTES`) - raw struct bytes (mandatory)
2. `UCP_F_INSTANCE_ID` (`field_id=26`, `type=U32`) - optional (present for per-channel / per-instance resources)
3. `UCP_F_RESOURCE_VERSION` (`field_id=13`, `type=U32`) - mandatory
4. `UCP_F_TIMESTAMP_UTC_MS` (`field_id=14`, `type=U64`) - mandatory

### 1.2 `SET_REQ` for raw resources

Parsed by `parse_raw_bytes_req(...)`. Accepted TLVs:

- `RAW_BYTES_F` (`BYTES`) - required
- `RAW_SELECTOR_F` (`U8`) - optional selector alias
- `RES_CH_ID` (`U8`) - optional selector alias
- `Q_CHANNEL_ID` (`U8`) - optional selector alias

Notes:

- selector alias (if present) may overwrite `channel_id` embedded in the raw struct, depending on resource handler
- `RAW_FORMAT_F` (`field_id=2`) exists but is not currently required by raw handlers

## 2. ABI Encoding Rules

- Endianness: little-endian host representation (`nRF52840`)
- Packing/alignment: exactly the in-memory layout of the struct passed to `memcpy`
- Floats: native IEEE-754 binary32
- Strings/chars inside raw structs: as defined by the struct (often fixed-size char arrays)

This document includes self-contained ABI layouts for the current firmware build.
Source file references are kept for provenance and diff tracking.

## 3. Raw Resource Catalog (Current Firmware)

## 3.1 `device/system` (`0x0010`)

### `device.system_config` (`res=0x0003`)

- UCP access: `GET`, `SET`, `EVENT`
- `RAW_BYTES_F` ABI type: `struct enhanced_system_config_data`
- Source: `src/bt_gatt_structs_enhanced.h`
- Selector: none
- `SET` validation: full apply via `apply_system_config_bytes()`

### `device.statistics` (`res=0x0004`)

- UCP access: `GET`, `CMD`, `EVENT`
- `RAW_BYTES_F` ABI type: `struct statistics_data`
- Source: `src/bt_gatt_structs.h`
- Selector:
  - `GET`: optional `RES_CH_ID` or `Q_CHANNEL_ID`; default channel `0`
  - `EVENT`: `UCP_F_INSTANCE_ID` = channel or `0xFFFFFFFF` (all)
- `SET`: not supported (reset via commands)

### `device.auto_calc_status` (`res=0x0005`)

- UCP access: `GET`, `SET`, `CMD`, `EVENT`
- `RAW_BYTES_F` ABI type: `struct auto_calc_status_data`
- Source: `src/bt_gatt_structs.h`
- Selector:
  - `GET`: optional selector; default `0xFF` (global), `0xFF` allowed
  - `SET`: uses payload struct `channel_id`; no separate selector alias used by handler

### `device.auto_calc_control` (`res=0x0008`)

- UCP access: `GET`, `SET`, `EVENT`
- `RAW_BYTES_F` ABI type: `struct ucp_auto_calc_control_data_v1`
- Source: local type in `src/ucp_uart.c`
- Fields (binary struct)
  - `enabled` (`u8`)
  - `interval_hours` (`u8`, valid `1..24`)
  - remaining bytes reserved/padding per struct definition

### `device.bulk_snapshot` (`res=0x0006`)

- UCP access: `GET`, `STREAM_OUT`, `EVENT`
- `RAW_BYTES_F` ABI type: `struct ucp_bulk_snapshot_v1`
- Source: local type in `src/ucp_uart.c`
- Also available as stream purpose `11` (`bulk_snapshot`)

## 3.2 `channel` (`0x0011`)

### `channel.custom_soil` (`res=0x0003`)

- UCP access: `GET`, `SET`, `EVENT`
- `RAW_BYTES_F` ABI type: `struct custom_soil_config_data`
- Source: `src/bt_gatt_structs_enhanced.h`
- Selector:
  - `GET`: required `RES_CH_ID`
  - `SET`: selector alias may override `channel_id` inside struct

### `channel.soil_moisture` (`res=0x0004`)

- UCP access: `GET`, `SET`, `EVENT`
- `RAW_BYTES_F` ABI type: `struct soil_moisture_config_data`
- Source: `src/bt_gatt_structs_enhanced.h`
- Selector:
  - `GET`: optional selector, default `0xFF` (global), `0xFF` allowed
  - `SET`: selector alias may override `channel_id`
- `EVENT` instance mapping:
  - global (`channel_id=0xFF`) -> `UCP_F_INSTANCE_ID = 0xFFFFFFFF`
  - per-channel -> `UCP_F_INSTANCE_ID = channel_id`

### `channel.interval_config` (`res=0x0005`)

- UCP access: `GET`, `SET`, `EVENT`
- `RAW_BYTES_F` ABI format: concatenated binary blob
  1. `struct interval_mode_config_data`
  2. `struct interval_mode_status_data`
- Source:
  - `src/bt_interval_mode_handlers.h`
  - builder/apply logic in `src/ucp_uart.c`
- `SET` consumes config prefix (`interval_mode_config_data`) and ignores trailing status bytes

### `channel.config_status` (`res=0x0006`)

- UCP access: `GET`, `EVENT`
- `RAW_BYTES_F` ABI type: `struct config_status_response_data` (per channel)
- Source: `src/bt_gatt_structs_enhanced.h`
- Selector: required `RES_CH_ID`

## 3.3 `flow` (`0x0014`)

### `flow.hydraulic_status` (`res=0x0002`)

- UCP access: `GET`, `SET`, `CMD`, `EVENT`
- `RAW_BYTES_F` ABI type: `struct hydraulic_status_data`
- Source: `src/bt_gatt_structs_enhanced.h`
- Selector:
  - `GET`: optional selector, default channel `0`
  - `SET`: selector alias may override embedded `channel_id`

### `flow.calibration_status` (`res=0x0003`)

- UCP access: `GET`, `SET`, `EVENT`
- `RAW_BYTES_F` ABI type: `struct calibration_data`
- Source: `src/bt_gatt_structs.h`
- `SET` supports action codes interpreted by firmware (`start/stop/set/reset`)

## 3.4 `environment` (`0x0016`)

### `environment.sensor_config` (`res=0x0002`)

- UCP access: `GET`, `SET`
- `RAW_BYTES_F` ABI type: `bme280_config_t`
- Source: `src/watering_enhanced.h`

## 3.5 `history` (`0x0017`)

### `history.rain_storage` (`res=0x0002`)

- UCP access: `GET`, `CMD`, `STREAM_OUT`, `EVENT`
- `RAW_BYTES_F` ABI type: `rain_history_stats_t`
- Source: `src/rain_history.h`

### `history.environment_storage` (`res=0x0003`)

- UCP access: `GET`, `CMD`, `STREAM_OUT`, `EVENT`
- `RAW_BYTES_F` ABI type: `env_history_stats_t`
- Source: `src/environmental_history.h`

## 3.6 `compensation` (`0x0019`)

### `compensation.status` (`res=0x0001`)

- UCP access: `GET`, `EVENT`
- `RAW_BYTES_F` ABI type: `struct compensation_status_data`
- Source: `src/bt_gatt_structs_enhanced.h`
- Selector: optional channel selector (default `0`)

### `compensation.channel_config` (`res=0x0002`)

- UCP access: `GET`, `SET`, `EVENT`
- `RAW_BYTES_F` ABI type: `struct channel_compensation_config_data`
- Source: `src/bt_gatt_structs.h`
- Selector:
  - `GET`: optional selector (default `0`)
  - `SET`: selector alias may override `channel_id`

### `compensation.rain_integration` (`res=0x0003`)

- UCP access: `GET`, `EVENT`
- `RAW_BYTES_F` ABI type: `struct rain_integration_status_ble`
- Source: `src/bt_gatt_structs.h`

## 3.7 `onboarding_reset` (`0x001A`)

### `onboarding_reset.onboarding_status` (`res=0x0001`)

- UCP access: `GET`, `EVENT`
- `RAW_BYTES_F` ABI type: `struct onboarding_status_data`
- Source: `src/bt_gatt_structs.h`

### `onboarding_reset.system_overview` (`res=0x0002`)

- UCP access: `GET`, `EVENT`
- `RAW_BYTES_F` ABI type: `config_system_overview_t`
- Source: `src/configuration_status.h`

### `onboarding_reset.config_status` (`res=0x0003`)

- UCP access: `GET`, `CMD`, `EVENT`
- `RAW_BYTES_F` ABI type: `struct config_status_response_data`
- Source: `src/bt_gatt_structs_enhanced.h`
- Selector: required `RES_CH_ID` for `GET`

### `onboarding_reset.reset_control` (`res=0x0004`)

- UCP access: `GET`, `CMD`, `EVENT`
- `RAW_BYTES_F` ABI type: `struct reset_control_data`
- Source: `src/bt_gatt_structs.h`

## 4. Stream Record ABIs (Binary Streams)

These are not wrapped in `RAW_BYTES_F`. They are raw binary stream payload formats sent inside `STREAM_DATA`.

### Watering history streams

- `history_page` (`purpose=4`)
  - custom `UHP1` header + flattened records (`src/ucp_uart.c`)
- `water_history_range` (`purpose=12`)
  - `data_type=0`: `history_event_t`
  - `data_type=1`: `daily_stats_t`
  - `data_type=2`: `monthly_stats_t`
  - `data_type=3`: `annual_stats_t`

Source: `src/watering_history.h`, `src/ucp_uart.c`

### Rain history stream (`purpose=8`)

- `data_type=0`: `rain_hourly_data_t`
- `data_type=1`: `rain_daily_data_t`

Source: `src/rain_history.h`

### Environment history stream (`purpose=9`)

- `data_type=0`: `hourly_history_entry_t`
- `data_type=1`: `daily_history_entry_t`
- `data_type=2`: `monthly_history_entry_t`

Source: `src/watering_enhanced.h` (record structs), `src/environmental_history.h` (query/stats API)

### Packs list streams

- `packs_list` (`purpose=5`) -> custom serialized rows derived from `pack_pack_list_entry_t`
- `packs_plants_list` (`purpose=6`) -> custom serialized rows derived from `pack_plant_list_entry_t`

Source: `src/pack_storage.h`, packing code in `src/ucp_uart.c`

## 5. Staging Upload Blob (`packs`, inbound stream purpose 3)

Inbound staging stream (`upload_staging`, `purpose=3`) stores a raw blob consumed by pack install commands:

- `packs.install_staged_plant`
  - expects `sizeof(pack_plant_v1_t)`
- `packs.install_staged_pack_bundle`
  - expects `pack_pack_v1_t` + `plant_count * pack_plant_v1_t`

Sources: `src/pack_storage.h`, `src/ucp_uart.c`

## 6. Client Guidance

- Always inspect `RAW_BYTES_F` length before decoding.
- Version caches by:
  - `service_id`
  - `resource_id`
  - `instance_id`
  - `resource_version`
- Treat these raw ABIs as firmware-version-specific contracts.
- Use `schema_discovery` and `schema_catalog` for capability detection.

## 7. Self-Contained ABI Layouts (Current Firmware)

This section mirrors the exact field order/types used by the current firmware for
`RAW_BYTES_F` resources and binary stream records.

Rules:

- All integer/floating-point scalars are little-endian on wire (nRF52840 host LE).
- `packed` structs have exact byte layout as shown (no padding between fields).
- Non-`packed` structs are copied using native ARM GCC ABI layout. For those:
  - treat `RAW_BYTES_F len` (or stream header `item_size`) as authoritative
  - the declarations below are still the correct field order/types

### 7.1 Raw Resource ABIs (`RAW_BYTES_F`) - Packed Layouts

#### `statistics_data` (`device.statistics`)

`packed`, `sizeof = 15` (guarded in `src/bt_gatt_structs.h`)

```c
struct statistics_data {
    uint8_t  channel_id;
    uint32_t total_volume;      // ml
    uint32_t last_volume;       // ml
    uint32_t last_watering;     // unix timestamp
    uint16_t count;             // watering count
} __packed;
```

#### `calibration_data` (`flow.calibration_status`)

`packed`, `sizeof = 13` (guarded)

```c
struct calibration_data {
    uint8_t  action;            // 0=stop,1=start,2=in progress,3=calculated
    uint32_t pulses;
    uint32_t volume_ml;
    uint32_t pulses_per_liter;
} __packed;
```

#### `auto_calc_status_data` (`device.auto_calc_status`)

`packed`, `sizeof = 64` (guarded)

```c
struct auto_calc_status_data {
    uint8_t  channel_id;
    uint8_t  calculation_active;
    uint8_t  irrigation_needed;
    float    current_deficit_mm;
    float    et0_mm_day;
    float    crop_coefficient;
    float    net_irrigation_mm;
    float    gross_irrigation_mm;
    float    calculated_volume_l;
    uint32_t last_calculation_time;
    uint32_t next_irrigation_time;
    uint16_t days_after_planting;
    uint8_t  phenological_stage;
    uint8_t  quality_mode;
    uint8_t  volume_limited;
    uint8_t  auto_mode;
    float    raw_mm;
    float    effective_rain_mm;
    uint8_t  calculation_error;
    float    etc_mm_day;
    float    volume_liters;
    uint8_t  cycle_count;
    uint8_t  cycle_duration_min;
    uint8_t  reserved[4];
} __packed;
```

#### `ucp_auto_calc_control_data_v1` (`device.auto_calc_control`)

Local UCP struct (`src/ucp_uart.c`), `packed`, `sizeof = 4`

```c
struct ucp_auto_calc_control_data_v1 {
    uint8_t enabled;
    uint8_t interval_hours;     // valid 1..24
    uint8_t reserved[2];
} __packed;
```

#### `ucp_bulk_snapshot_v1` (`device.bulk_snapshot`)

Local UCP struct (`src/ucp_uart.c`), `packed`, `sizeof = 48`

```c
struct ucp_bulk_snapshot_v1 {
    uint8_t  version;
    uint8_t  system_status;
    uint8_t  system_state;
    uint8_t  power_mode;
    uint8_t  active_channels_bitmap;
    uint8_t  interval_mode_channels_bitmap;
    uint8_t  incomplete_config_channels_bitmap;
    uint8_t  rain_sensor_status;
    uint32_t uptime_ms;
    uint32_t unix_utc;
    uint32_t flow_calibration;
    uint32_t rain_last_pulse;
    float    rain_last_24h_mm;
    float    env_temp_c;
    float    env_humidity_pct;
    float    env_pressure_hpa;
    uint32_t env_ts;
    uint8_t  peer_power_on;
    uint8_t  peer_power_inhibit;
    uint8_t  reserved[2];
} __packed;
```

#### `custom_soil_config_data` (`channel.custom_soil`)

`packed` (`src/bt_gatt_structs_enhanced.h`)

```c
struct custom_soil_config_data {
    uint8_t  channel_id;
    uint8_t  operation;              // 0=read,1=create,2=update,3=delete
    char     name[32];
    float    field_capacity;
    float    wilting_point;
    float    infiltration_rate;
    float    bulk_density;
    float    organic_matter;
    uint32_t created_timestamp;
    uint32_t modified_timestamp;
    uint32_t crc32;
    uint8_t  status;
    uint8_t  reserved[3];
} __attribute__((packed));
```

#### `soil_moisture_config_data` (`channel.soil_moisture`)

`packed`, `sizeof = 8` (guarded)

```c
struct soil_moisture_config_data {
    uint8_t channel_id;      // 0-7 or 0xFF global
    uint8_t operation;       // 0=read, 1=set
    uint8_t enabled;
    uint8_t moisture_pct;    // 0-100
    uint8_t status;
    uint8_t has_data;
    uint8_t reserved[2];
} __attribute__((packed));
```

#### `interval_mode_config_data` (`channel.interval_config` blob prefix)

`packed`, `sizeof = 17`

```c
struct interval_mode_config_data {
    uint8_t  channel_id;
    uint8_t  enabled;
    uint16_t watering_minutes;
    uint8_t  watering_seconds;
    uint16_t pause_minutes;
    uint8_t  pause_seconds;
    uint8_t  configured;
    uint32_t last_update;
    uint8_t  reserved[4];
} __attribute__((packed));
```

#### `interval_mode_status_data` (`channel.interval_config` blob suffix)

`packed`, `sizeof = 37`

```c
struct interval_mode_status_data {
    uint8_t  channel_id;
    uint8_t  is_active;
    uint8_t  current_state;
    uint8_t  currently_watering;
    uint32_t phase_remaining_sec;
    uint32_t cycles_completed;
    uint32_t total_elapsed_sec;
    uint32_t total_volume_ml;
    uint8_t  progress_percent;
    uint32_t cycles_remaining;
    uint32_t next_phase_time;
    uint32_t estimated_completion;
    uint8_t  reserved[4];
} __attribute__((packed));
```

`channel.interval_config` `RAW_BYTES_F` payload is:

1. `interval_mode_config_data` bytes (required for `SET`)
2. `interval_mode_status_data` bytes (present in `GET`/`EVENT`; ignored on `SET`)

#### `config_status_response_data` (`channel.config_status`, `onboarding_reset.config_status`)

`packed` (`src/bt_gatt_structs_enhanced.h`)

```c
struct config_status_response_data {
    uint8_t  channel_id;
    uint8_t  basic_complete;
    uint8_t  growing_env_complete;
    uint8_t  compensation_complete;
    uint8_t  custom_soil_complete;
    uint8_t  interval_complete;
    uint8_t  config_score;
    uint8_t  can_auto_water;
    uint32_t last_reset_timestamp;
    uint8_t  reset_count;
    uint8_t  last_reset_group;
    char     last_reset_reason[32];
    uint8_t  reserved[4];
} __attribute__((packed));
```

#### `hydraulic_status_data` (`flow.hydraulic_status`)

`packed`, `sizeof = 48` (guarded)

```c
struct hydraulic_status_data {
    uint8_t  channel_id;
    uint8_t  profile_type;
    uint8_t  lock_level;
    uint8_t  lock_reason;
    uint32_t nominal_flow_ml_min;
    uint16_t ramp_up_time_sec;
    uint8_t  tolerance_high_percent;
    uint8_t  tolerance_low_percent;
    uint8_t  is_calibrated;
    uint8_t  monitoring_enabled;
    uint8_t  learning_runs;
    uint8_t  stable_runs;
    uint8_t  estimated;
    uint8_t  manual_override_active;
    uint16_t reserved0;
    uint32_t lock_at_epoch;
    uint32_t retry_after_epoch;
    uint8_t  no_flow_runs;
    uint8_t  high_flow_runs;
    uint8_t  unexpected_flow_runs;
    uint8_t  reserved1;
    uint32_t last_anomaly_epoch;
    uint8_t  global_lock_level;
    uint8_t  global_lock_reason;
    uint16_t reserved2;
    uint32_t global_lock_at_epoch;
    uint32_t global_retry_after_epoch;
} __attribute__((packed));
```

#### `enhanced_system_config_data` (`device.system_config`)

`packed`, `sizeof = 56` (guarded)

```c
struct enhanced_system_config_data {
    uint8_t  version;
    uint8_t  power_mode;
    uint32_t flow_calibration;
    uint8_t  max_active_valves;
    uint8_t  num_channels;

    uint8_t  master_valve_enabled;
    int16_t  master_valve_pre_delay;
    int16_t  master_valve_post_delay;
    uint8_t  master_valve_overlap_grace;
    uint8_t  master_valve_auto_mgmt;
    uint8_t  master_valve_current_state;

    uint8_t  bme280_enabled;
    uint16_t bme280_measurement_interval;
    uint8_t  bme280_sensor_status;

    uint8_t  _reserved_rain_enabled;
    uint8_t  global_temp_compensation_enabled;
    float    _reserved_rain_sensitivity;
    float    global_temp_sensitivity;
    uint16_t _reserved_rain_lookback;
    float    _reserved_rain_threshold;
    float    global_temp_base_temperature;

    uint8_t  interval_mode_active_channels;
    uint8_t  compensation_active_channels;
    uint8_t  incomplete_config_channels;
    uint8_t  environmental_data_quality;

    uint32_t last_config_update;
    uint32_t last_sensor_reading;
    uint8_t  reserved[4];
} __attribute__((packed));
```

#### `compensation_status_data` (`compensation.status`)

`packed`

```c
struct compensation_status_data {
    uint8_t  channel_id;
    uint8_t  rain_compensation_active;
    float    recent_rainfall_mm;
    float    rain_reduction_percentage;
    uint8_t  rain_skip_watering;
    uint32_t rain_calculation_time;

    uint8_t  temp_compensation_active;
    float    current_temperature;
    float    temp_compensation_factor;
    float    temp_adjusted_requirement;
    uint32_t temp_calculation_time;

    uint8_t  any_compensation_active;
    uint8_t  reserved[7];
} __attribute__((packed));
```

#### `channel_compensation_config_data` (`compensation.channel_config`)

`packed`, `sizeof = 44` (guarded)

```c
struct channel_compensation_config_data {
    uint8_t  channel_id;

    uint8_t  rain_enabled;
    float    rain_sensitivity;
    uint16_t rain_lookback_hours;
    float    rain_skip_threshold_mm;
    float    rain_reduction_factor;

    uint8_t  temp_enabled;
    float    temp_base_temperature;
    float    temp_sensitivity;
    float    temp_min_factor;
    float    temp_max_factor;

    uint32_t last_rain_calc_time;
    uint32_t last_temp_calc_time;

    uint8_t  reserved[3];
} __packed;
```

#### `rain_integration_status_ble` (`compensation.rain_integration`)

`packed`, `sizeof = 78` (guarded)

```c
struct rain_integration_status_ble {
    uint8_t  sensor_active;
    uint8_t  integration_enabled;
    uint32_t last_pulse_time;
    float    calibration_mm_per_pulse;
    float    rainfall_last_hour;
    float    rainfall_last_24h;
    float    rainfall_last_48h;
    float    sensitivity_pct;
    float    skip_threshold_mm;
    float    channel_reduction_pct[8];
    uint8_t  channel_skip_irrigation[8];
    uint16_t hourly_entries;
    uint16_t daily_entries;
    uint32_t storage_usage_bytes;
} __packed;
```

#### `onboarding_status_data` (`onboarding_reset.onboarding_status`)

`packed`, `sizeof = 33` (guarded)

```c
struct onboarding_status_data {
    uint8_t  overall_completion_pct;
    uint8_t  channels_completion_pct;
    uint8_t  system_completion_pct;
    uint8_t  schedules_completion_pct;
    uint64_t channel_config_flags;
    uint32_t system_config_flags;
    uint8_t  schedule_config_flags;
    uint32_t onboarding_start_time;
    uint32_t last_update_time;
    uint64_t channel_extended_flags;
} __packed;
```

#### `reset_control_data` (`onboarding_reset.reset_control`)

`packed`, `sizeof = 16` (guarded)

```c
struct reset_control_data {
    uint8_t  reset_type;
    uint8_t  channel_id;
    uint32_t confirmation_code;
    uint8_t  status;
    uint32_t timestamp;
    uint8_t  reserved[5]; // [0]=progress%, [1]=step, [2]=retries, [3..4]=error_le16
} __packed;
```

### 7.2 Raw Resource ABIs (`RAW_BYTES_F`) - Native (Non-Packed) Layouts

These use native ARM GCC layout because the source structs are not declared `packed`.
Layouts below are the **exact current firmware build layouts**, verified from
`build/zephyr/zephyr.elf` DWARF metadata.

For cross-language clients, still validate `RAW_BYTES_F len` for compatibility.

#### `bme280_config_t` (`environment.sensor_config`)

Source `src/watering_enhanced.h`. Exact current layout size: `4` bytes.

```c
typedef struct {
    uint16_t measurement_interval; // seconds
    bool     initialized;
    bool     enabled;
} bme280_config_t;
```

Exact current byte layout:

```text
offset  size  type   field
0       2     u16le  measurement_interval
2       1     u8     initialized (bool)
3       1     u8     enabled (bool)
```

#### `rain_history_stats_t` (`history.rain_storage`)

Source `src/rain_history.h`. Exact current layout size: `24` bytes.

```c
typedef struct {
    uint16_t hourly_entries;
    uint16_t daily_entries;
    uint32_t oldest_hourly;
    uint32_t newest_hourly;
    uint32_t oldest_daily;
    uint32_t newest_daily;
    uint32_t total_storage_bytes;
} rain_history_stats_t;
```

Exact current byte layout:

```text
offset  size  type   field
0       2     u16le  hourly_entries
2       2     u16le  daily_entries
4       4     u32le  oldest_hourly
8       4     u32le  newest_hourly
12      4     u32le  oldest_daily
16      4     u32le  newest_daily
20      4     u32le  total_storage_bytes
```

#### `env_history_stats_t` (`history.environment_storage`)

Source `src/environmental_history.h`. Exact current layout size: `28` bytes.

```c
typedef struct {
    uint16_t hourly_entries_used;
    uint16_t daily_entries_used;
    uint16_t monthly_entries_used;
    uint32_t oldest_hourly_timestamp;
    uint32_t oldest_daily_timestamp;
    uint32_t oldest_monthly_timestamp;
    uint32_t total_storage_bytes;
    uint8_t  storage_utilization_pct;
} env_history_stats_t;
```

Exact current byte layout (includes padding):

```text
offset  size  type   field
0       2     u16le  hourly_entries_used
2       2     u16le  daily_entries_used
4       2     u16le  monthly_entries_used
6       2     pad    alignment padding
8       4     u32le  oldest_hourly_timestamp
12      4     u32le  oldest_daily_timestamp
16      4     u32le  oldest_monthly_timestamp
20      4     u32le  total_storage_bytes
24      1     u8     storage_utilization_pct
25      3     pad    tail padding (struct alignment)
```

#### `config_system_overview_t` (`onboarding_reset.system_overview`)

Source `src/configuration_status.h`. `config_health_level_t` is a C enum (`int` on current GCC toolchain).
Exact current layout size: `17` bytes.

```c
typedef struct {
    uint8_t channel_scores[8];
    uint8_t overall_system_score;
    uint8_t channels_ready_for_auto_watering;
    uint8_t fully_configured_channels;
    uint8_t channels_with_basic;
    uint8_t channels_with_growing_env;
    uint8_t channels_with_compensation;
    uint8_t channels_with_custom_soil;
    uint8_t channels_with_interval;
    config_health_level_t system_health; // enum, current build stores as 1 byte
} config_system_overview_t;
```

Exact current byte layout (enum serialized as 8-bit in current firmware build/toolchain):

```text
offset  size  type   field
0       8     bytes  channel_scores[8]
8       1     u8     overall_system_score
9       1     u8     channels_ready_for_auto_watering
10      1     u8     fully_configured_channels
11      1     u8     channels_with_basic
12      1     u8     channels_with_growing_env
13      1     u8     channels_with_compensation
14      1     u8     channels_with_custom_soil
15      1     u8     channels_with_interval
16      1     u8     system_health (enum underlying size in current build)
```

## 8. Binary Stream Record ABIs (Exact)

This section documents record payloads inside `STREAM_DATA`.
See `wire-reference.md` for stream frame headers (`UHP1`, `URH2`, `UEH2`, `UWH2`, `UPK1`, `UPT1`, `udb1`).

### 8.1 Watering History Records (`water_history_range`, `purpose=12`)

#### `history_event_t` (`data_type=0`)

`packed`, `sizeof = 15`

```c
typedef struct __attribute__((packed)) {
    uint8_t dt_delta;
    union {
        struct {
            uint8_t mode    :1;
            uint8_t trigger :2;
            uint8_t success :2;
            uint8_t err     :3;
        };
        uint8_t packed;
    } flags;
    uint16_t target_ml;
    uint16_t actual_ml;
    uint16_t avg_flow_ml_s;
    uint8_t  reserved[3];
} history_event_t;
```

#### `daily_stats_t` (`data_type=1`)

`packed`, `sizeof = 16`

```c
typedef struct __attribute__((packed)) {
    uint32_t day_epoch;
    uint32_t total_ml;
    uint16_t sessions_ok;
    uint16_t sessions_err;
    uint8_t  max_channel;
    uint8_t  success_rate;
    uint8_t  reserved[2];
} daily_stats_t;
```

#### `monthly_stats_t` (`data_type=2`, alias of `monthly_stats_raw_t`)

`packed`, `sizeof = 12`

```c
typedef struct __attribute__((packed)) {
    uint16_t year;
    uint8_t  month;
    uint32_t total_ml;
    uint16_t active_days;
    uint8_t  peak_channel;
    uint8_t  reserved;
} monthly_stats_raw_t;

typedef monthly_stats_raw_t monthly_stats_t;
```

#### `annual_stats_t` (`data_type=3`)

`packed`, `sizeof = 20`

```c
typedef struct __attribute__((packed)) {
    uint16_t year;
    uint32_t total_ml;
    uint32_t sessions;
    uint32_t errors;
    uint16_t max_month_ml;
    uint16_t min_month_ml;
    uint8_t  peak_channel;
    uint8_t  reserved[3];
} annual_stats_t;
```

### 8.2 `history_page` (`purpose=4`) flattened record rows (`UHP1`)

`history_page` does **not** stream raw `history_event_t` bytes.
After the `UHP1` header, each row is a fixed 12-byte flattened record:

```text
offset  size  type    field
0       4     u32le   absolute_timestamp
4       1     u8      dt_delta
5       1     u8      flags_packed   (same bit layout as history_event_t.flags.packed)
6       2     u16le   target_ml
8       2     u16le   actual_ml
10      2     u16le   avg_flow_ml_s
```

### 8.3 Rain History Records (`rain_history_range`, `purpose=8`)

#### `rain_hourly_data_t` (`data_type=0`)

`packed`, `sizeof = 8`

```c
typedef struct __attribute__((packed)) {
    uint32_t hour_epoch;
    uint16_t rainfall_mm_x100;
    uint8_t  pulse_count;
    uint8_t  data_quality;
} rain_hourly_data_t;
```

#### `rain_daily_data_t` (`data_type=1`)

`packed`, `sizeof = 12`

```c
typedef struct __attribute__((packed)) {
    uint32_t day_epoch;
    uint32_t total_rainfall_mm_x100;
    uint16_t max_hourly_mm_x100;
    uint8_t  active_hours;
    uint8_t  data_completeness;
} rain_daily_data_t;
```

### 8.4 Environment History Records (`env_history_range`, `purpose=9`)

These record structs are native (non-packed) C structs.
Layouts below are the **exact current firmware build layouts** (DWARF verified).
`UEH2.item_size` remains authoritative for forward compatibility/versioning.

#### `bme280_reading_t` (nested)

```c
typedef struct {
    float    temperature;
    float    humidity;
    float    pressure;
    uint32_t timestamp;
    bool     valid;
} bme280_reading_t;
```

Exact current layout size: `20` bytes.

```text
offset  size  type    field
0       4     f32le   temperature
4       4     f32le   humidity
8       4     f32le   pressure
12      4     u32le   timestamp
16      1     u8      valid (bool)
17      3     pad     tail padding
```

#### `hourly_history_entry_t` (`data_type=0`)

```c
typedef struct {
    uint32_t         timestamp;
    bme280_reading_t environmental;
    float            rainfall_mm;
    uint8_t          watering_events;
    uint32_t         total_volume_ml;
    uint16_t         active_channels;
} hourly_history_entry_t;
```

Exact current layout size: `40` bytes.

```text
offset  size  type    field
0       4     u32le   timestamp
4       20    bytes   environmental (bme280_reading_t; see above)
24      4     f32le   rainfall_mm
28      1     u8      watering_events
29      3     pad     alignment padding
32      4     u32le   total_volume_ml
36      2     u16le   active_channels
38      2     pad     tail padding
```

#### `daily_history_entry_t` (`data_type=1`)

```c
typedef struct {
    uint32_t date;  // YYYYMMDD
    struct { float min, max, avg; } temperature;
    struct { float min, max, avg; } humidity;
    struct { float min, max, avg; } pressure;
    float    total_rainfall_mm;
    uint16_t watering_events;
    uint32_t total_volume_ml;
    uint16_t sample_count;
    uint8_t  active_channels_bitmap;
} daily_history_entry_t;
```

Exact current layout size: `56` bytes.

```text
offset  size  type    field
0       4     u32le   date (YYYYMMDD)
4       12    bytes   temperature { min,max,avg } as 3 x f32le
16      12    bytes   humidity    { min,max,avg } as 3 x f32le
28      12    bytes   pressure    { min,max,avg } as 3 x f32le
40      4     f32le   total_rainfall_mm
44      2     u16le   watering_events
46      2     pad     alignment padding
48      4     u32le   total_volume_ml
52      2     u16le   sample_count
54      1     u8      active_channels_bitmap
55      1     pad     tail padding
```

#### `monthly_history_entry_t` (`data_type=2`)

```c
typedef struct {
    uint16_t year_month; // YYYYMM
    struct { float min, max, avg; } temperature;
    struct { float min, max, avg; } humidity;
    struct { float min, max, avg; } pressure;
    float    total_rainfall_mm;
    uint32_t watering_events;
    uint64_t total_volume_ml;
    uint8_t  days_active;
} monthly_history_entry_t;
```

Exact current layout size: `64` bytes.

```text
offset  size  type    field
0       2     u16le   year_month (YYYYMM)
2       2     pad     alignment padding
4       12    bytes   temperature { min,max,avg } as 3 x f32le
16      12    bytes   humidity    { min,max,avg } as 3 x f32le
28      12    bytes   pressure    { min,max,avg } as 3 x f32le
40      4     f32le   total_rainfall_mm
44      4     u32le   watering_events
48      8     u64le   total_volume_ml
56      1     u8      days_active
57      7     pad     tail padding
```

### 8.5 Pack List Streams (`purpose=5` / `purpose=6`)

Important: these are **custom serialized rows**, not `memcpy()` of native structs.

#### Native source structs (for semantics)

```c
typedef struct {
    uint16_t pack_id;
    uint16_t version;
    uint16_t plant_count;
    char     name[32];  // PACK_NAME_MAX_LEN
} pack_pack_list_entry_t;

typedef enum {
    PLANT_SOURCE_BUILTIN = 0,
    PLANT_SOURCE_PACK = 1,
    PLANT_SOURCE_CUSTOM = 2,
} plant_source_t;

typedef struct {
    uint16_t      plant_id;
    uint16_t      pack_id;
    uint16_t      version;
    plant_source_t source;       // enum in native struct (ABI-dependent width)
    char          name[48];      // PACK_COMMON_NAME_MAX_LEN
} pack_plant_list_entry_t;
```

#### `UPK1` row format (`packs_list`, purpose=5) - exact on wire

Fixed 38-byte row:

```text
offset  size  type    field
0       2     u16le   pack_id
2       2     u16le   version
4       2     u16le   plant_count
6       32    bytes   name[32] (fixed-size, may include trailing NUL/padding)
```

#### `UPT1` row format (`packs_plants_list`, purpose=6) - exact on wire

Fixed 55-byte row:

```text
offset  size  type    field
0       2     u16le   plant_id
2       2     u16le   pack_id
4       2     u16le   version
6       1     u8      source   (serialized enum value)
7       48    bytes   name[48] (fixed-size, may include trailing NUL/padding)
```

## 9. Packs Inbound Staging Blob ABIs (purpose=3)

Inbound staging stream stores a raw blob later consumed by `packs.install_staged_*` commands.

### 9.1 `pack_plant_v1_t` (`packs.install_staged_plant`)

Blob must be exactly `sizeof(pack_plant_v1_t)` (current packed size: `156` bytes).

```c
typedef struct __attribute__((packed)) {
    uint16_t plant_id;
    uint16_t pack_id;
    uint16_t version;
    uint16_t reserved;

    char common_name[48];
    char scientific_name[64];

    uint16_t kc_ini_x1000;
    uint16_t kc_dev_x1000;
    uint16_t kc_mid_x1000;
    uint16_t kc_end_x1000;

    uint16_t root_depth_min_mm;
    uint16_t root_depth_max_mm;

    uint8_t  stage_days_ini;
    uint8_t  stage_days_dev;
    uint16_t stage_days_mid;
    uint8_t  stage_days_end;
    uint8_t  growth_cycle;

    uint16_t depletion_fraction_p_x1000;
    uint16_t spacing_row_mm;
    uint16_t spacing_plant_mm;
    uint16_t density_x100;
    uint16_t canopy_max_x1000;

    int8_t   frost_tolerance_c;
    uint8_t  temp_opt_min_c;
    uint8_t  temp_opt_max_c;

    uint8_t  typ_irrig_method_id;

    uint16_t water_need_factor_x100;
    uint8_t  irrigation_freq_days;
    uint8_t  prefer_area_based;
} pack_plant_v1_t;
```

### 9.2 `pack_pack_v1_t` + plant array (`packs.install_staged_pack_bundle`)

Blob format:

1. `pack_pack_v1_t` (packed, `sizeof = 40`)
2. `plant_count * pack_plant_v1_t` (packed, 156 bytes each in current build)

```c
typedef struct __attribute__((packed)) {
    uint16_t pack_id;
    uint16_t version;
    char     name[32];     // PACK_NAME_MAX_LEN
    uint16_t plant_count;
    uint16_t reserved;
} pack_pack_v1_t;
```

Notes:

- Staging blob does **not** include `pack_file_header_t`.
- Firmware validates lengths, IDs, and CRC/content as part of install command handlers.

## 10. Decoder Implementation Notes (Practical)

- For `RAW_BYTES_F` resources:
  - first decode the UCP TLV wrapper (`RAW_BYTES_F`, `RESOURCE_VERSION`, `TIMESTAMP`, optional `INSTANCE_ID`)
  - then decode the raw bytes using the ABI in this document
- For non-packed raw ABIs and non-packed stream records:
  - use the runtime length (`RAW_BYTES_F len` or stream `item_size`)
  - do not hardcode sizes without checking firmware version / schema hash
- For pack list streams (`UPK1`/`UPT1`):
  - decode the documented custom row layouts, not the native C structs
