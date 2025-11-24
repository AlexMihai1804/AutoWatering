# Alarm Status Characteristic (UUID: defa)

> Operation Summary
| Operation | Payload Struct | Size | Fragmentation | Notes |
|-----------|----------------|------|---------------|-------|
| Read | `struct alarm_data` | 7 B | None | Returns the last alarm snapshot stored in `alarm_value` |
| Write | 1 B command or full `struct alarm_data` | 1 B / 7 B | None | Single-byte writes clear alarms; full writes are diagnostics only |
| Notify | `struct alarm_data` | 7 B | None | Immediate on raise/clear and once when notifications are enabled |

The Alarm Status characteristic transports critical fault information raised by the watering engine. Firmware logic resides in `read_alarm`, `write_alarm`, `alarm_ccc_changed`, `bt_irrigation_alarm_notify`, and `bt_irrigation_alarm_clear` within `src/bt_irrigation_service.c`. Alarm producers (flow monitoring, unexpected flow, etc.) live in `src/watering_monitor.c` and call `bt_irrigation_alarm_notify()` directly. The packed structure definition and size assert are in `src/bt_gatt_structs.h` (`BUILD_ASSERT(sizeof(struct alarm_data) == 7)`).

## Characteristic Metadata
| Item | Value |
|------|-------|
| UUID | `12345678-1234-5678-1234-56789abcdefa` |
| Properties | Read, Write, Notify |
| Permissions | Read, Write |
| Payload Size | 7 bytes |
| Fragmentation | Not supported |
| Notification Priority | Critical (sent through `advanced_notify` with no throttle) |

## Payload Layout
| Offset | Field | Type | Size | Access | Meaning | Source on Read | Validation on Write |
|--------|-------|------|------|--------|---------|----------------|--------------------|
| 0 | `alarm_code` | `uint8_t` | 1 | RW | Current alarm identifier (0 = none) | Cached in `alarm_value` | Single-byte writes accept 0x00, 0xFF, or 0x01-0x0D; full-frame writes accept any value |
| 1 | `alarm_data` | `uint16_t` | 2 | RW | Extra context attached to the alarm | Cached in `alarm_value` | Unchecked for 7-byte writes; cleared to 0 by single-byte clear |
| 3 | `timestamp` | `uint32_t` | 4 | RW | UTC seconds from `timezone_get_unix_utc()` when the alarm snapshot was taken | Cached in `alarm_value` | Overwritten in firmware during clears and notifications |

Structure fields are little-endian. `alarm_value` is initialised to zeros at boot so first reads return the cleared state.

## Field Behaviour
- `alarm_code`: runtime producers only emit codes 1 (`ALARM_NO_FLOW`) or 2 (`ALARM_UNEXPECTED_FLOW`) today. `write_alarm()` treats 0x00 and 0xFF as clear-all aliases and allows 0x01-0x0D as selective clears (must match the active code). Other single-byte values return `BT_ATT_ERR_VALUE_NOT_ALLOWED`.
- `alarm_data`: writers such as `watering_monitor.c` use this as context (retry attempt count or number of stray pulses). Clears reset the field to 0 before calling `bt_irrigation_alarm_notify()`.
- `timestamp`: `bt_irrigation_alarm_notify()` always stamps the structure with `timezone_get_unix_utc()`, even for clear operations supplied with `alarm_data = 0`. Clients should treat the timestamp as "last state change" rather than "alarm onset".

## Read Path (`read_alarm`)
1. Returns the current contents of `alarm_value` via `bt_gatt_attr_read()` without additional validation.
2. Logs the triple `(alarm_code, alarm_data, timestamp)` at debug level for diagnostics.
3. Because the buffer is updated by every notification, reads and notifications share the same payload.

## Write Semantics (`write_alarm`)
- **Offset/length**: Requires offset 0. Single-byte writes (length 1) invoke clear logic; other lengths must match `sizeof(struct alarm_data)` or the handler returns `BT_ATT_ERR_INVALID_ATTRIBUTE_LEN`.
- **Clear commands**:
  - `0x00` or `0xFF` clear any active alarm (`watering_clear_errors()` plus zeroing of `alarm_value` before `bt_irrigation_alarm_notify(0, 0)`).
  - `0x01`-`0x0D` clear only if the current `alarm_code` matches; otherwise the write is ignored (no error).
  - Other values raise `BT_ATT_ERR_VALUE_NOT_ALLOWED`.
- **Full 7-byte writes**: Copied into `alarm_value` for diagnostics/emulation but do not trigger a notification. No range validation is performed on embedded fields. These writes are typically used only during integration testing.

`bt_irrigation_alarm_clear()` is a helper invoked by internal code paths and mirrors the single-byte semantics above.

## Notification Flow
- `alarm_ccc_changed()` toggles `notification_state.alarm_notifications_enabled`. When switched on it immediately calls `safe_notify()` with the current snapshot so subscribers get the latest state.
- `bt_irrigation_alarm_notify(alarm_code, alarm_data)` updates `alarm_value`, stamps `timestamp` with `timezone_get_unix_utc()`, and emits the payload through `advanced_notify()` with critical priority. Critical notifications bypass normal throttling and only fail if the connection is unavailable or buffers are exhausted.
- If notifications are disabled or there is no active connection, the helper returns without sending and without staging a backlog; clients must poll via reads after re-subscribing.

## Alarm Producers
- `src/watering_monitor.c` raises alarms for flow anomalies:
  - Code 1 (`ALARM_NO_FLOW`): emitted when a commanded watering task fails to register pulses after various retries. `alarm_data` records the retry attempt count; clears use `alarm_data = 0`.
  - Code 2 (`ALARM_UNEXPECTED_FLOW`): emitted when pulses are detected while all valves are closed. `alarm_data` carries the raw pulse count observed before mitigation.
- Other subsystems may call `bt_irrigation_alarm_notify()` in future; consumers should not hard-code the code list and must handle unknown non-zero values.

## Error Handling
| Condition | ATT Error | Notes |
|-----------|-----------|-------|
| Offset != 0 on write | `BT_ATT_ERR_INVALID_OFFSET` | Applies to both 1-byte and full-frame writes |
| Length > 7 with full write | `BT_ATT_ERR_INVALID_ATTRIBUTE_LEN` | Short full-frame writes are accepted because `memcpy` uses `len` |
| Single-byte command outside {0x00, 0xFF, 0x01-0x0D} | `BT_ATT_ERR_VALUE_NOT_ALLOWED` | No change performed |

Transport errors during notification are logged (`LOG_ERR`) but do not change the stored alarm state.

## Client Guidance
- Subscribe to notifications to receive alarms with minimal latency; there is no queue when notifications are disabled.
- Treat the `timestamp` as the instant the snapshot was generated, not necessarily the beginning of the underlying incident.
- Use a single-byte write (`0x00` recommended) to clear alarms after remediation. Follow up with a read if you need to confirm the clear without waiting for the notification.
- If multiple alarm sources are expected, rely on the code value only for branching; the descriptive mapping is intentionally kept internal and may evolve.

## Troubleshooting
| Symptom | Likely Cause | Mitigation |
|---------|--------------|------------|
| Notifications never arrive | CCC not enabled or link dropped | Ensure `StartNotifications()` succeeded and connection remains active |
| Clear command ignored | Sent 1-byte code that does not match active alarm | Read current snapshot and resend with 0x00/0xFF or the exact code |
| Timestamp advances but code/data unchanged | Producer re-issued the same alarm (e.g., retry) | Use received timestamp to correlate with system logs and check underlying sensor state |
| Alarm persists across reboot | Firmware stores alarm snapshot in RAM; check if producer re-raises during boot (flow anomalies can fire quickly) |

## Firmware References
- `src/bt_irrigation_service.c`: `read_alarm`, `write_alarm`, `alarm_ccc_changed`, `bt_irrigation_alarm_notify`, `bt_irrigation_alarm_clear`.
- `src/watering_monitor.c`: flow anomaly detection that currently generates alarm codes 1 and 2.
- `src/bt_gatt_structs.h`: `struct alarm_data` definition and size assert.

## Related Interfaces
- `docs/ble-api/characteristics/07-task-queue-management.md` – task queue controls surface follow-up actions once alarms clear
- `docs/ble-api/characteristics/03-system-status.md` – system-wide fault codes raised alongside alarms
- `docs/system-architecture.md` – enhanced recovery strategy mappings (`enhanced_error_handling`) referenced by alarm producers
