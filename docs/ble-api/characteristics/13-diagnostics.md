# Diagnostics Characteristic (UUID: defd)

> Operation Summary
| Operation | Payload | Size | Fragmentation | Notes |
|-----------|---------|------|---------------|-------|
| Read | `struct diagnostics_data` | 12 B | None | Returns current health snapshot |
| Write | - | - | - | Not supported (no Write property) |
| Notify | `struct diagnostics_data` | 12 B | None | Snapshot on subscribe and whenever health metrics change |

Diagnostics exposes a compact system health summary: uptime, cumulative error counters, last error code, valve activity bitmap, and battery placeholder. The handlers reside in `read_diagnostics`, `bt_irrigation_diagnostics_notify`, and `diagnostics_ccc_changed` under `src/bt_irrigation_service.c`.

## Characteristic Metadata
| Item | Value |
|------|-------|
| UUID | `12345678-1234-5678-1234-56789abcdefd` |
| Properties | Read, Notify |
| Permissions | Read |
| Payload Size | 12 bytes (`BUILD_ASSERT(sizeof(struct diagnostics_data) == 12)`) |
| Notification Priority | Normal (`advanced_notify`) |

## Payload Layout (`struct diagnostics_data`)
| Offset | Field | Type | Description |
|--------|-------|------|-------------|
| 0 | `uptime` | `uint32_t` | Minutes since boot. Prefers RTC-calculated uptime when wall time is available, otherwise falls back to monotonic ticks. |
| 4 | `error_count` | `uint16_t` | Total number of recorded errors (`diagnostics_error_count`). |
| 6 | `last_error` | `uint8_t` | Most recent error code; `0` when none. |
| 7 | `valve_status` | `uint8_t` | Bitmask of active valves (`bit n` corresponds to channel `n`). |
| 8 | `battery_level` | `uint8_t` | `0xFF` because the platform is mains powered (reserved for future battery hardware). |
| 9 | `reserved` | `uint8_t[3]` | Zero-filled.

All multi-byte fields are little-endian.

## Read Behaviour (`read_diagnostics`)
- Validates parameters and rebuilds the payload each read.
- Computes uptime by comparing the current RTC (`timezone_get_unix_utc`) with a cached boot timestamp when RTC data is available; otherwise derives minutes from `k_uptime_get()`.
- Copies the current `diagnostics_error_count`, `diagnostics_last_error`, and recalculates the valve bitmap by iterating every channel via `watering_get_channel()`.
- Clears the reserved bytes and forces `battery_level = 0xFF`.

## Notification Behaviour (`diagnostics_ccc_changed` / `bt_irrigation_diagnostics_notify`)
- Enabling CCC prepares the payload as above and immediately sends a notification so subscribers begin with an up-to-date snapshot.
- Subsequent notifications must be pushed explicitly by firmware (`bt_irrigation_diagnostics_notify()`), typically invoked when error counts change or valve activity toggles. The notification path uses the same 12-byte struct with Normal priority (`advanced_notify`).
- Disabling CCC zeros the attribute buffer to avoid leaking stale data to future subscribers.

## Error Handling
- Writes are rejected by GATT permissions (Write property is not set).
- READ honours the BLE stack's default bounds checking; offsets and partial reads are handled by `bt_gatt_attr_read`.

## Client Guidance
- Treat `battery_level = 0xFF` as "not applicable".
- The characteristic only reports cumulative counters; use `10-alarm-status.md` for contextual fault details and `14-growing-environment.md` for configuration state.
- Polling via READ and subscribing to notifications are both supported; notifications provide fresher data because updates are pushed on change.

## Firmware References
- `src/bt_irrigation_service.c`: `read_diagnostics`, `diagnostics_ccc_changed`, `bt_irrigation_diagnostics_notify`.
- `src/enhanced_error_handling.c`: producers for diagnostics counters.
- `src/watering.c`: valve activity flags consumed when building the bitmap.
