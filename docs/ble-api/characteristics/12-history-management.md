# History Management Characteristic (UUID: defc)

> Operation Summary
| Operation | Payload | Size | Fragmentation | Notes |
|-----------|---------|------|---------------|-------|
| Write | 12 B query header | 12 B | None | Triggers query or clear command |
| Write | 12 B header (`history_type=0xFF`) | 12 B | None | Clears stored history (best effort) |
| Read | `struct history_data` | 32 B | None | Returns the cached 32-byte `history_value` buffer (query header + latest detailed snapshot) |
| Notify | `history_fragment_header_t` + payload | 8 B + <=232 B | Unified | Multi-fragment responses for queries |
| Notify | `struct history_data` | 32 B | None | Real-time event notifications from watering tasks |

History Management exposes watering history in four aggregation levels. Queries are issued through a compact 12-byte header and responses are delivered asynchronously as one or more fragments. Real-time watering events reuse the same `struct history_data` snapshot that backs the read buffer and are sent independently from queued queries.

## Characteristic Metadata
| Item | Value |
|------|-------|
| UUID | `12345678-1234-5678-1234-56789abcdefc` |
| Properties | Read, Write, Notify |
| Permissions | Read, Write |
| Query Header Size | 12 bytes |
| Response Fragment Header | `history_fragment_header_t` (8 bytes) |
| Max Fragment Payload | 232 bytes (`RAIN_HISTORY_FRAGMENT_SIZE`) |
| Notification Priority | Low (new queries rate-limited to >=100 ms; continuation requests bypass the limiter; fragments stream at ~2 ms spacing) |

Handlers: `read_history`, `write_history`, `history_ccc_changed`, `bt_irrigation_history_notify_event` in `src/bt_irrigation_service.c`.

## Read Path (`read_history`)
The read handler is a thin wrapper around `bt_gatt_attr_read()`. It always returns the static `history_value` buffer, which is sized to `sizeof(struct history_data)` (32 bytes) and allocated in `src/bt_irrigation_service.c`. The buffer is populated by the helpers that surface watering events:
- `history_ccc_changed()` seeds it with default values when notifications are enabled and clears it when they are disabled.
- `bt_irrigation_history_notify_event()` and the `bt_irrigation_history_get_*()` helpers overwrite the union with the most recent detailed/daily/monthly/annual snapshot before sending a notification.
- `write_history()` does not synchronously touch `history_value`; it only dispatches fragmented responses. Clients that need the latest query result should therefore wait for the notification train rather than relying on the read return path.

Offsets other than zero are rejected by the stack before reaching `read_history`, so the entire 32-byte block is always returned on success. When no event has primed the buffer yet, the read returns all zeros (or the default header injected by `history_ccc_changed()`).

## Query Header (`write_history`)
All integers little-endian.

| Byte | Field | Description |
|------|-------|-------------|
| 0 | `channel_id` | Channel 0-7. `0xFF` accepted but effectively queries channel 0. |
| 1 | `history_type` | `0` detailed, `1` daily, `2` monthly, `3` annual, `0xFF` clear command. |
| 2 | `entry_index` | Page/offset semantics (per type below). |
| 3 | `count` | Requested entry count (clamped to 1..50). |
| 4-7 | `start_timestamp` | Optional UTC filter (currently echoed only). |
| 8-11 | `end_timestamp` | Optional UTC filter (currently echoed only). |

The handler rejects payloads that are not exactly 12 bytes or writes with non-zero offset. Accepted queries are answered via fragment notifications; the 32-byte read buffer (`history_value`) is not updated by query responses.

### Clear Command (`history_type = 0xFF`)
- Invokes `watering_history_cleanup_expired()` and emits a single 8-byte header notification (`data_type = 0xFF`, `status = 0`).
- `start_timestamp` and `channel_id` are not currently honoured beyond triggering best-effort cleanup.

## Response Fragment Format
Each fragment sent after a successful query has:

```
history_fragment_header_t header;  // 8 bytes
uint8_t payload[fragment_size];    // Up to 232 bytes (may be smaller for small MTU)
```

`header` fields:
- `data_type`: mirrors the requested `history_type` (0-3). `0xFE` indicates rate-limit notices; `0xFF` acknowledges clear commands.
- `status`: `0` success, `0x07` rate limited. Other codes reserved.
- `entry_count`: total entries encoded across all fragments (LE16).
- `fragment_index`, `total_fragments`, `fragment_size`: standard fragmentation metadata.

The first fragment payload begins with the echoed 12-byte query header, followed by packed entries.

## Packed Entry Layouts
All structures are serialized back-to-back without padding.

### Detailed Entries (`history_type = 0`, 24 bytes each)
| Offset | Field | Type | Notes |
|--------|-------|------|-------|
| 0 | `timestamp` | `uint32_t` | Approximate UTC reconstructed from delta history (counts backwards from current time). |
| 4 | `channel_id` | `uint8_t` | Channel associated with the event. |
| 5 | `event_type` | `uint8_t` | `1` complete, `3` error (start/abort currently not surfaced). |
| 6 | `mode` | `uint8_t` | Watering mode (`0` duration, `1` volume). |
| 7 | `target_value_ml` | `uint16_t` | Target volume or duration proxy. |
| 9 | `actual_value_ml` | `uint16_t` | Measured volume. |
| 11 | `total_volume_ml` | `uint16_t` | Duplicate of `actual_value_ml` (legacy compatibility). |
| 13 | `trigger_type` | `uint8_t` | Origin (`0` manual, `1` schedule, `2` remote). |
| 14 | `success_status` | `uint8_t` | Boolean success flag. |
| 15 | `error_code` | `uint8_t` | Domain error code when `event_type = 3`. |
| 16 | `flow_rate_avg` | `uint16_t` | Average ml/s. |
| 18 | `reserved` | `uint8_t[2]` | Zero-filled. |

Entries are sourced from `watering_history_query_page()`. The current implementation always queries a single channel (either the specified channel or channel 0 when `0xFF` was provided) and reconstructs timestamps by subtracting stored `dt_delta` values from the current UTC timestamp, so historical accuracy degrades for long gaps.

### Daily Entries (`history_type = 1`, 16 bytes each)
| Offset | Field | Type | Notes |
|--------|-------|------|-------|
| 0 | `day_index` | `uint16_t` | Day-of-year of the bucket. |
| 2 | `year` | `uint16_t` | Calendar year. |
| 4 | `watering_sessions_ok` | `uint8_t` | Successful sessions. |
| 5 | `total_volume_ml` | `uint32_t` | Aggregate volume. |
| 9 | `total_duration_est_sec` | `uint16_t` | Estimated from volume and success count. |
| 11 | `avg_flow_rate` | `uint16_t` | Derived average flow. |
| 13 | `success_rate` | `uint8_t` | Percentage (0-100). |
| 14 | `error_count` | `uint8_t` | Failed sessions. |
| 15 | `reserved` | `uint8_t` | Zero. |

Data is obtained through `watering_history_get_daily_stats()` and limited to a single day range starting from the computed index. Multi-day windows are not yet exposed; `entry_index` acts as a backwards offset from today.

### Monthly Entries (`history_type = 2`, 15 bytes each)
| Offset | Field | Type | Notes |
|--------|-------|------|-------|
| 0 | `month` | `uint8_t` | Calendar month (1-12). |
| 1 | `year` | `uint16_t` | Calendar year. |
| 3 | `total_sessions` | `uint16_t` | Computed via `count_sessions_in_period()` for the month. |
| 5 | `total_volume_ml` | `uint32_t` | Volume across the month. |
| 9 | `total_duration_hours` | `uint16_t` | Currently zero pending richer analytics. |
| 11 | `avg_daily_volume` | `uint16_t` | Volume per active day. |
| 13 | `active_days` | `uint8_t` | Days with activity. |
| 14 | `success_rate` | `uint8_t` | Derived from daily stats within the month. |

`entry_index` selects how many months to step back from the current month (0 = current). Only one month is returned per query; `count` is capped by available statistics.

### Annual Entries (`history_type = 3`, 14 bytes each)
| Offset | Field | Type | Notes |
|--------|-------|------|-------|
| 0 | `year` | `uint16_t` | Calendar year. |
| 2 | `total_sessions` | `uint16_t` | Clamped to `UINT16_MAX`. |
| 4 | `total_volume_liters` | `uint32_t` | Millilitres / 1000. |
| 8 | `avg_monthly_volume_liters` | `uint16_t` | Average monthly volume. |
| 10 | `most_active_month` | `uint8_t` | Month with highest volume (computed by sampling monthly stats). |
| 11 | `success_rate` | `uint8_t` | Success percentage for the year. |
| 12 | `peak_month_volume_liters` | `uint16_t` | Volume of the most active month in litres. |

`entry_index` selects the year offset from the current year.

## Rate Limiting & Errors
- New queries are rate-limited to one per 100ms (`HISTORY_QUERY_MIN_INTERVAL_MS`). Flooding emits a standalone notification with `data_type = 0xFE`, `status = 0x07` (no payload) and the write is accepted without running the query.
- Continuation requests for the same `(channel_id, history_type)` bypass the limiter to keep fragment downloads responsive.
- Invalid length -> `BT_ATT_ERR_INVALID_ATTRIBUTE_LEN`.
- Non-zero offset writes -> `BT_ATT_ERR_INVALID_OFFSET`.
- Unsupported history type (other than `0-3, 0xFF`) or channel out of range -> `BT_ATT_ERR_VALUE_NOT_ALLOWED`.

## Fragment Streaming Limits (v3.1.0)
- Inter-fragment delay is reduced to ~2ms for faster streaming.
- Transient `-ENOMEM/-EBUSY` notify failures are retried (up to 5 retries) with exponential backoff (approx. 20ms → 640ms).

## Real-Time Event Notifications
`bt_irrigation_history_notify_event()` publishes immediate detailed-event snapshots using the fixed 32-byte `struct history_data` layout (no fragmentation). These notifications are triggered by watering task transitions (start/complete/error) and appear only when CCC is enabled and a default connection exists. They always report `history_type = 0`, `count = 1`, and populate the union fields directly.

## Client Guidance
- Always wait for the full fragment set before parsing; fragments are ordered and each carries the total fragment count.
- Treat timestamps in detailed history as approximate for older records. For canonical timing, prefer persisted application logs until the history backend stores absolute timestamps.
- When requesting aggregates, expect only a small number of entries (often one). Iterate by incrementing `entry_index` and re-issuing queries if you need older data.
- Clear commands are destructive and global; use with caution until scoped deletion is implemented.

## Firmware References
- `src/bt_irrigation_service.c`: `write_history`, `read_history`, `history_ccc_changed`, `bt_irrigation_history_notify_event`.
- `src/watering_history.c`, `src/watering_history.h`: data sourcing for detailed, daily, monthly, and annual statistics.
- `src/watering_tasks.c`: producers for real-time event notifications.

## Known Limitations
- `channel_id = 0xFF` queries only channel 0 data in the current firmware.
- `start_timestamp`/`end_timestamp` are echoed but not enforced on the server side.
- Daily, monthly, and annual builders request a single bucket per query; batching via `count` is constrained by backend support.
- Timestamp reconstruction for detailed events depends on rolling delta hints and loses precision over long retention periods.
