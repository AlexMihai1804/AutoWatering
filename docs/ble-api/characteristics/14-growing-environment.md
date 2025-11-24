# Growing Environment Characteristic (UUID: defe)

> Operation Summary
| Operation | Payload | Size | Fragmentation | Notes |
|-----------|---------|------|---------------|-------|
| Read | `struct growing_env_data` | 71 B | None | Snapshot for the last selected channel; defaults to channel 0 on CCC enable |
| Write | Channel select byte | 1 B | None | Updates cached channel context only; no notification |
| Write | Fragmented update | 4 B header + payload | Custom (write-only header) | Header `ch, type, size` then raw payload fragments; completes at declared size |
| Write | Full struct update | >=71 B | None | First 71 bytes parsed as struct; extra bytes ignored |
| Notify | `struct growing_env_data` | 71 B | None | Emitted after successful struct write (direct or fragmented) |

Growing Environment stores the per-channel agronomic configuration used by automatic irrigation calculations. The handlers live in `read_growing_env`, `write_growing_env`, `notify_growing_env`, and `growing_env_ccc_changed` inside `src/bt_irrigation_service.c`. Updates flow straight into `watering_channel_t` instances and are persisted through `watering_save_config_priority(true)` (debounced at 250 ms).

## Characteristic Metadata
| Item | Value |
|------|-------|
| UUID | `12345678-1234-5678-1234-56789abcdefe` |
| Properties | Read, Write, Notify |
| Permissions | Read, Write |
| Payload Size | 71 bytes (`BUILD_ASSERT(sizeof(struct growing_env_data) == 71)`) |
| Fragmentation | Supported on write: 4-byte header + payload fragments (frag_type 2=big-endian size, 3=little-endian) |
| Notification Priority | Normal (managed by `advanced_notify`, adaptive throttle around 500 ms under load) |

## Payload Layout (`struct growing_env_data`)
| Offset | Field | Type | Size | Access | Notes |
|--------|-------|------|------|--------|-------|
| 0 | `channel_id` | `uint8_t` | 1 | RW | Channel 0-7; validated on write |
| 1 | `plant_db_index` | `uint16_t` | 2 | RW | 0-based enhanced plant index, `0xFFFF` = unset |
| 3 | `soil_db_index` | `uint8_t` | 1 | RW | 0-based enhanced soil index, `0xFF` = unset |
| 4 | `irrigation_method_index` | `uint8_t` | 1 | RW | 0-based method index, `0xFF` = unset |
| 5 | `use_area_based` | `uint8_t` | 1 | RW | 1 = use `coverage.area_m2`, 0 = use `coverage.plant_count` |
| 6 | `coverage.area_m2` | `float` | 4 | RW | Used when `use_area_based` = 1; must be > 0 |
| 6 | `coverage.plant_count` | `uint16_t` | 2 (of 4) | RW | Used when `use_area_based` = 0; must be > 0 |
| 10 | `auto_mode` | `uint8_t` | 1 | RW | `0`=manual, `1`=quality, `2`=eco |
| 11 | `max_volume_limit_l` | `float` | 4 | RW | Maximum irrigation volume; must be > 0 |
| 15 | `enable_cycle_soak` | `uint8_t` | 1 | RW | Non-zero enables cycle-and-soak |
| 16 | `planting_date_unix` | `uint32_t` | 4 | RW | Unix timestamp (UTC) |
| 20 | `days_after_planting` | `uint16_t` | 2 | RW | Stored value; handler does not recompute |
| 22 | `latitude_deg` | `float` | 4 | RW | Must be within [-90.0, 90.0] |
| 26 | `sun_exposure_pct` | `uint8_t` | 1 | RW | 0-100; used for FAO-56 adjustments |
| 27 | `plant_type` | `uint8_t` | 1 | RW | Legacy field (0-7); not written back to channel |
| 28 | `specific_plant` | `uint16_t` | 2 | RW | Legacy field; ignored by handler |
| 30 | `soil_type` | `uint8_t` | 1 | RW | Legacy field; ignored by handler |
| 31 | `irrigation_method` | `uint8_t` | 1 | RW | Legacy field; ignored by handler |
| 32 | `sun_percentage` | `uint8_t` | 1 | RW | Legacy field; ignored by handler |
| 33 | `custom_name` | `char[32]` | 32 | RW | Copied only when `plant_type == PLANT_TYPE_OTHER` |
| 65 | `water_need_factor` | `float` | 4 | RW | Custom plant multiplier when `plant_type == PLANT_TYPE_OTHER` |
| 69 | `irrigation_freq_days` | `uint8_t` | 1 | RW | Custom plant frequency when `plant_type == PLANT_TYPE_OTHER` |
| 70 | `prefer_area_based` | `uint8_t` | 1 | RW | Custom plant preference flag |

Little-endian encoding is used for all multi-byte fields. The union at offset 6 always occupies four bytes; clients must zero the unused members.

## Read Path (`read_growing_env`)
- Copies the current shared buffer (`growing_env_value`) to discover the selected channel; falls back to channel 0 if the cached value is invalid.
- Calls `watering_get_channel()` and mirrors the live configuration into a local struct before returning. Enhanced indices, coverage data, automatic mode flags, planting data, latitude, and sun exposure are refreshed on every read.
- Legacy fields remain zeroed except when the channel is configured for a custom plant (`PLANT_TYPE_OTHER`); in that case the custom plant block is populated from `channel->custom_plant`.
- If the channel lookup fails, the handler fills the struct with conservative defaults (indices unset, 1 m^2 coverage, manual mode, 10 L limit, latitude 45.0, sun 75%).
- Returns the packed struct via `bt_gatt_attr_read`; no fragmentation header is prepended.

## Write Path (`write_growing_env`)

### 1. Channel Selection (length = 1)
- Treats the single byte as a channel id. Values outside `[0, WATERING_CHANNELS_COUNT)` return `-EINVAL`.
- On success, caches the id in `growing_env_last_channel`, refreshes the global buffer with the live channel contents, and exits without touching storage or notifications.
- Use this before a read to fetch non-zero channels when notifications are disabled.

### 2. Fragmented Update (header + payload)
- First frame must be at least four bytes: `byte0 = channel_id`, `byte1 = frag_type`, `byte2/3 = total size` (endianness depends on `frag_type`: `2` = big-endian, `3` = little-endian).
- Declared size must be `<= sizeof(struct growing_env_data)` (71); larger sizes are rejected.
- Header initializes `growing_env_frag` (128-byte staging buffer) and copies any accompanying payload bytes.
- Continuation frames contain payload only. The handler appends up to the declared size and ignores surplus bytes.
- A 5 s watchdog (`FRAGMENTATION_TIMEOUT_MS`) clears partial state if fragments stop arriving.
- After `received >= expected`, the assembled struct is validated (channel bounds, mode range, indices, latitude, volume limit, and coverage constraints). Violations return `-EINVAL` and abort the update.
- On success the struct is applied to the channel (only enhanced fields, coverage, auto settings, planting data, latitude, sun exposure, and custom plant block when `plant_type == PLANT_TYPE_OTHER`). Legacy scalar bytes (plant_type/soil_type/etc.) are not written back.
- The global buffer is updated, configuration is scheduled for persistence via `watering_save_config_priority(true)`, and `notify_growing_env()` dispatches a Normal-priority notification.

### 3. Direct Struct Write (length >= 71)
- For MTUs large enough, the handler casts the incoming buffer to `struct growing_env_data` (ignoring extra bytes) and runs the same validation and apply logic as the fragmented path.
- Any write shorter than 71 bytes is rejected with `-EINVAL`.

`offset` and `flags` parameters are not supported; all writes must start at offset 0.

## Notifications (`notify_growing_env` / `growing_env_ccc_changed`)
- Notifications send the 71-byte struct exactly as stored in `growing_env_value` with Normal priority throttling through `advanced_notify`.
- CCC enable seeds the buffer with channel 0 (or sensible defaults if `watering_get_channel(0)` fails) but does **not** emit an automatic notification; clients should issue a read or full write after enabling notifications.
- CCC disable clears the shared buffer to zeros.
- Logging for notifications is rate-limited to one message every 30 s.

## Validation Matrix
| Field | Validation | Failure Result |
|-------|------------|----------------|
| `channel_id` | `< WATERING_CHANNELS_COUNT` | `-EINVAL` |
| `auto_mode` | `<= 2` | `-EINVAL` |
| `sun_exposure_pct` | `<= 100` | `-EINVAL` |
| `plant_db_index` | `< PLANT_FULL_SPECIES_COUNT` or `0xFFFF` | `-EINVAL` |
| `soil_db_index` | `< SOIL_ENHANCED_TYPES_COUNT` or `0xFF` | `-EINVAL` |
| `irrigation_method_index` | `< IRRIGATION_METHODS_COUNT` or `0xFF` | `-EINVAL` |
| `latitude_deg` | `-90.0 <= value <= 90.0` | `-EINVAL` |
| `max_volume_limit_l` | `> 0.0` | `-EINVAL` |
| `coverage.area_m2` | `> 0.0` when `use_area_based != 0` | `-EINVAL` |
| `coverage.plant_count` | `> 0` when `use_area_based == 0` | `-EINVAL` |

## Client Guidance
- Always supply all 71 bytes when performing struct writes; zero the fields you do not control so firmware validation has defined inputs.
- Use the channel-select byte to stage a read for channels other than zero; the cached id is also used for subsequent notifications.
- Do not interleave fragmented updates with other writes. Start a new fragmentation session only after the previous one succeeds or times out (5 s).
- Legacy scalar fields (`plant_type`, `soil_type`, `irrigation_method`, `sun_percentage`, `specific_plant`) are currently ignored on write and reported as zeros on read. Rely on enhanced database indices instead.
- After a successful write, expect a single notification carrying the new struct. Issuing an immediate read is still recommended to confirm the persisted values.

## Troubleshooting
| Symptom | Likely Cause | Mitigation |
|---------|--------------|------------|
| Write rejected with `-EINVAL` before completion | Fragment header declared size > 71 or channel id invalid | Recalculate header and retry |
| Write rejected after final fragment | Validation failure (coverage zero, latitude out of range, etc.) | Inspect the last fragment payload and fix invalid fields |
| Fragmentation times out | >5 s gap between fragments | Restart the transfer from the header frame |
| Notification not received after channel select | Channel select does not trigger notify | Issue a read or send a full struct write |
| Legacy fields always read as zero | Handler currently ignores legacy scalar bytes | Use enhanced indices and custom plant block |

## Firmware References
- `src/bt_irrigation_service.c`: `read_growing_env`, `write_growing_env`, `notify_growing_env`, `growing_env_ccc_changed`, `growing_env_frag`, `check_fragmentation_timeout`.
- `src/watering.c`, `src/watering_config.c`: channel structure definitions and persistence helpers.
- `src/bt_gatt_structs.h`: `struct growing_env_data` layout and size assertion.
