# Statistics Characteristic (UUID: def8)

> Operation Summary
| Operation | Payload Struct | Size | Fragmentation | Notes |
|-----------|----------------|------|---------------|-------|
| Read | `struct statistics_data` | 15 B | None | Snapshot for channel selected in last write (defaults to channel 0) |
| Write | 1 B selector or full `struct statistics_data` | 1 B / 15 B | None | 1 B changes the read/notify channel; full write performs reset or synthetic update |
| Notify | `struct statistics_data` | 15 B | None | After statistics refresh, after reset, and at 30 s cadence while a task runs |

The Statistics characteristic reports coarse usage counters for each watering channel. Firmware side logic lives in `read_statistics`, `write_statistics`, `statistics_ccc_cfg_changed`, `bt_irrigation_statistics_notify`, and `bt_irrigation_update_statistics` inside `src/bt_irrigation_service.c`, backed by helper calls from `src/watering.c` (`watering_get_channel_statistics`, `watering_update_channel_statistics`, `watering_reset_channel_statistics`).

## Characteristic Metadata
| Item | Value |
|------|-------|
| UUID | `12345678-1234-5678-1234-56789abcdef8` |
| Properties | Read, Write, Notify |
| Permissions | Read, Write |
| Payload Size | 15 bytes (`BUILD_ASSERT(sizeof(struct statistics_data) == 15)`) |
| Fragmentation | Not supported; writes must be a single frame |
| Notification Priority | Normal priority buffer (`advanced_notify` with 200 ms base throttle) plus an internal 30 s gate during active watering |

## Payload Layout
| Offset | Field | Type | Size | Access | Meaning | Source on Read | Accepted Values on Write |
|--------|-------|------|------|--------|---------|----------------|--------------------------|
| 0 | `channel_id` | `uint8_t` | 1 | RW | Channel the counters describe (0-7) | Cached selector from `statistics_value` | 0-7 only (`BT_ATT_ERR_VALUE_NOT_ALLOWED` otherwise) |
| 1 | `total_volume` | `uint32_t` | 4 | R | Total dispensed volume in millilitres | Derived by `watering_get_channel_statistics()` (currently heuristic) | Ignored unless entire frame zeroed (reset); non-`0xFFFFFFFF` logged and discarded |
| 5 | `last_volume` | `uint32_t` | 4 | RW | Volume of most recent irrigation in millilitres | Derived by `watering_get_channel_statistics()` | `0xFFFFFFFF` keeps existing value, any other number recorded as part of synthetic update |
| 9 | `last_watering` | `uint32_t` | 4 | RW | Unix UTC timestamp of the last completion | `watering_channel_t->last_watering_time` (persisted via `watering_save_config()`) | `0xFFFFFFFF` keeps existing timestamp, otherwise written to history update |
| 13 | `count` | `uint16_t` | R | Number of completions | Derived by `watering_get_channel_statistics()` (currently 0 or 1) | Non-`0xFFFF` values ignored during writes |

All multibyte integers are little-endian.

## Field Behaviour
- `channel_id` is stored inside `statistics_value`. A 1-byte write updates the selector and influences subsequent reads and notifications. Invalid IDs are rejected with `BT_ATT_ERR_VALUE_NOT_ALLOWED`.
- `total_volume`, `last_volume`, `last_watering`, and `count` are refreshed from runtime data for every read/notify invocation. Current firmware calculates these using placeholder heuristics: totals and counts are effectively zero unless a recent watering occurred.
- `last_watering` is copied directly from `watering_channel_t.last_watering_time`, which is persisted by `watering_update_channel_statistics()` and cleared by `watering_reset_channel_statistics()`.

## Read Path
1. `read_statistics()` copies the currently selected `channel_id` from `statistics_value`; invalid selectors fall back to channel `0`.
2. `watering_get_channel(channel_id, &channel)` fetches the live channel state. Failure returns a zeroed struct with the requested `channel_id`.
3. `watering_get_channel_statistics()` fills volume and count placeholders. Today this helper returns basic estimates (0/0/0 unless `last_watering_time` is set).
4. `last_watering` is pulled from the channel struct without conversion (already Unix seconds).
5. The packed struct is returned via `bt_gatt_attr_read()`.

## Write Semantics
### 1-byte Channel Select
- Requirements: offset `0`, length `1`.
- Behaviour: updates `statistics_value->channel_id` only. No immediate readback or notification is sent.
- Validation: channel >= `WATERING_CHANNELS_COUNT` (8) => `BT_ATT_ERR_VALUE_NOT_ALLOWED`.

### 15-byte Frame
- Requirements: offset `0`, length `15`. Any other length/offset => `BT_ATT_ERR_INVALID_OFFSET`.
- After memcpy into `statistics_value`, firmware validates `channel_id`; invalid channels produce `BT_ATT_ERR_VALUE_NOT_ALLOWED`.
- Two cases are then distinguished:
	- **Reset:** All numeric fields are zero => `watering_reset_channel_statistics(channel_id)` is called. That clears `last_watering_time`, emits a history reset record, and saves configuration.
	- **Synthetic Update:** Any non-zero field leads to `watering_update_channel_statistics(channel_id, volume, timestamp)` where `volume`/`timestamp` honour sentinel values `0xFFFFFFFF`. Missing timestamps default to `timezone_get_unix_utc()`. `total_volume` and `count` are ignored even when explicitly set; a debug log notes the discard.
- On success, if notifications are enabled, firmware refreshes the buffer from live data and calls `safe_notify()` to confirm the change.

## Notification Delivery
- `statistics_ccc_cfg_changed()` sets `notification_state.statistics_notifications_enabled`. When enabling, it seeds channel 0 data in `statistics_value` so the first notify/read is coherent.
- `bt_irrigation_statistics_notify()` wraps `safe_notify()` for immediate pushes. Failures are logged and not retried.
- `bt_irrigation_update_statistics()` is invoked by the watering engine and by the flow sensor wrapper (`bt_irrigation_update_statistics_from_flow()`). While an irrigation task is active (`watering_get_current_task() != NULL`), notifications are throttled to one every 30 seconds even if volume updates occur more often. Outside active tasks the standard 200 ms adaptive throttle applies.

## Persistence and Limits
- Only `last_watering_time` is persisted across reboots via `watering_save_config()`. Volume and count are derived at runtime and currently revert to zero after restart until fresh data is produced.
- The implementation does not yet aggregate multi-day history. Clients should expect conservative numbers and use History characteristics for detailed analytics when available.

## Error Handling
- Bad selector or channel ID => `BT_ATT_ERR_VALUE_NOT_ALLOWED`.
- Any length/offset mismatch => `BT_ATT_ERR_INVALID_OFFSET`.
- Internal watering API failures fall back to zeroed fields but still return success to the client. Logs (`LOG_WRN`) flag the failure; no ATT error is generated.

## Client Guidance
- Always issue a 1-byte selector before reading if you need a specific channel. Reads without a prior write return channel 0.
- Treat `total_volume`/`count` as advisory until the history backend is implemented. For exact completion counts, consume the Task Queue or History characteristics.
- After sending a reset or synthetic update, wait for the confirmation notification before reading again. The confirmation payload reflects the authoritative state after the helper functions have run.
- Do not attempt to batch multiple selectors-each write replaces the cached channel.

## Troubleshooting
| Symptom | Observed Payload | Meaning | Suggested Action |
|---------|-----------------|---------|------------------|
| All zero values even after watering | `total_volume = 0`, `count = 0`, `last_watering = 0` | History aggregation not yet populated or `watering_get_channel_statistics()` failed | Check logs for `Failed to get channel statistics`, ensure watering tasks log completion |
| Notification missing during long task | No packets for >30 s | 30 s active-task cadence gate | Rely on Current Task characteristic for real-time progress, expect statistics burst on completion |
| ATT error `0x0D` (Value Not Allowed) | Write rejected | Channel selector outside 0-7 | Correct the channel ID |
| Reset write accepted but timestamp remains non-zero | Notification still shows previous `last_watering` | `watering_reset_channel_statistics()` failed due to channel fetch error | Inspect logs for `Failed to reset channel` and verify channel exists |

## Firmware References
- `src/bt_irrigation_service.c`: `read_statistics`, `write_statistics`, `statistics_ccc_cfg_changed`, `bt_irrigation_statistics_notify`, `bt_irrigation_update_statistics`.
- `src/watering.c`: `watering_get_channel_statistics`, `watering_update_channel_statistics`, `watering_reset_channel_statistics`.
- `src/flow_sensor.c`: `bt_irrigation_update_statistics_from_flow()` integration.
- `src/bt_gatt_structs.h`: `struct statistics_data` definition and size assert.

## Related Characteristics
- `docs/ble-api/characteristics/07-task-queue-management.md` - emits completion events that drive statistics updates.
- `docs/ble-api/characteristics/10-alarm-status.md` - alarms may accompany unexpected statistics resets.
- `docs/ble-api/characteristics/12-history-management.md` (when enabled) - detailed historic records beyond the coarse counters provided here.
