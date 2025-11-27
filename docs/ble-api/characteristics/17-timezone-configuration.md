# Timezone Configuration Characteristic (UUID: 12345678-1234-5678-9abc-def123456793)

> Operation Summary
| Operation | Payload | Size | Fragmentation | Notes |
|-----------|---------|------|---------------|-------|
| Read | `timezone_config_t` | 11 B | None | Returns the cached timezone snapshot |
| Write | `timezone_config_t` | 11 B | None | Full-frame only; validates bounds before applying |
| Notify | `timezone_config_t` | 11 B | None | Fired on CCC enable and after successful writes |

The characteristic exposes the timezone and Daylight Saving Time rule set the firmware uses when converting between RTC ticks and Unix timestamps. The payload mirrors the packed `timezone_config_t` structure defined in `nvs_config.h`.

## Characteristic Metadata
| Item | Value |
|------|-------|
| UUID | `12345678-1234-5678-9abc-def123456793` |
| Properties | Read, Write, Notify |
| Permissions | Read, Write |
| Payload Size | 11 bytes |
| Notification Priority | Normal (`bt_gatt_notify`) |
| CCC Handler | `timezone_ccc_changed` |

## Payload Layout (`timezone_config_t`)
| Offset | Field | Type | Description |
|--------|-------|------|-------------|
| 0 | `utc_offset_minutes` | `int16_t` | Base UTC offset in minutes (`-720...+840`) |
| 2 | `dst_enabled` | `uint8_t` | `1` enables DST rules, `0` disables them |
| 3 | `dst_start_month` | `uint8_t` | `1...12`; cleared to `0` when DST disabled |
| 4 | `dst_start_week` | `uint8_t` | `1...5`; `5` denotes the last week of the month |
| 5 | `dst_start_dow` | `uint8_t` | `0...6`; `0` represents Sunday |
| 6 | `dst_end_month` | `uint8_t` | `1...12` |
| 7 | `dst_end_week` | `uint8_t` | `1...5` |
| 8 | `dst_end_dow` | `uint8_t` | `0...6` |
| 9 | `dst_offset_minutes` | `int16_t` | Minutes applied while DST is active (`-120...+120`) |

### Field Notes
- When `dst_enabled == 0`, the handler zeroes all DST rule fields and `dst_offset_minutes` before caching the struct.
- DST transitions are applied at runtime (02:00 local by default); `timezone_is_dst_active()` reflects the active offset using the configured rules. Ambiguous fall-back times are resolved by trying the DST offset first, then the base offset.

## Behaviour

### Read (`read_timezone`)
- Retrieves the current struct via `timezone_get_config()` and copies it into the attribute buffer.
- Responds with a single 11-byte snapshot for every read; no history is maintained per connection.
- If `timezone_get_config()` fails, the handler returns `-EIO` (mapped to ATT Unlikely Error) without producing a payload.

### Write (`write_timezone`)
- Demands exactly 11 bytes at offset `0`. Any other length or offset returns `BT_ATT_ERR_INVALID_ATTRIBUTE_LEN` or `BT_ATT_ERR_INVALID_OFFSET`.
- Validates all range constraints listed below; out-of-range values yield `BT_ATT_ERR_VALUE_NOT_ALLOWED`.
- Automatically clears DST rule bytes when `dst_enabled` is `0`.
- Applies the configuration through `timezone_set_config()`, which now persists the struct in NVS when storage is available.
- Updates the attribute buffer and, if notifications are enabled, immediately notifies subscribers.

### Notifications (`timezone_ccc_changed`)
- CCC enable caches the current config and pushes an immediate snapshot using `bt_gatt_notify(default_conn, attr - 1, ...)`.
- Subsequent validated writes also trigger a notification when CCC remains set.
- There is no periodic worker; notifications are strictly reactive.

## Validation & Errors
| Check | Accepted Values | ATT Error on Failure |
|-------|-----------------|----------------------|
| Payload length | 11 bytes | `BT_ATT_ERR_INVALID_ATTRIBUTE_LEN` |
| Offset | 0 | `BT_ATT_ERR_INVALID_OFFSET` |
| `utc_offset_minutes` | `-720`...`+840` | `BT_ATT_ERR_VALUE_NOT_ALLOWED` |
| `dst_enabled` | `0` or `1` | `BT_ATT_ERR_VALUE_NOT_ALLOWED` |
| `dst_offset_minutes` | `-120`...`+120` | `BT_ATT_ERR_VALUE_NOT_ALLOWED` |
| DST rule fields (when enabled) | Months `1...12`, weeks `1...5`, DOW `0...6` | `BT_ATT_ERR_VALUE_NOT_ALLOWED` |
| `timezone_set_config()` failure | - | `BT_ATT_ERR_WRITE_NOT_PERMITTED` |

## Client Guidance
- Always send the full struct; partial writes are not supported and will be rejected.
- Configuration changes are persisted; reissue only if the NVS partition was wiped or a factory reset was performed.
- To observe whether DST is active at runtime, consult the RTC characteristic (`09`), which reports the effective offset and DST flag.

## Firmware References
- `src/bt_irrigation_service.c`: `read_timezone`, `write_timezone`, `timezone_ccc_changed`.
- `src/timezone.c`: `timezone_set_config`, `timezone_get_config`, time conversion helpers.
- `include/nvs_config.h`: `timezone_config_t` definition and size guard.
