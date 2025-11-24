# Rain History Control Characteristic (UUID: 12345678-1234-5678-9abcde14)

> Operation Summary
| Operation | Payload | Size | Fragmentation | Notes |
|-----------|---------|------|---------------|-------|
| Read | `struct rain_history_cmd_data` | 16 B | None | Echoes the last command that was accepted |
| Write | `struct rain_history_cmd_data` | 16 B | None | Triggers processing (hourly, daily, recent, reset, calibration) |
| Notify | `history_fragment_header_t` + payload | 8 B + <=240 B | Unified history header | Multi-fragment responses spaced ~50 ms apart |
| Error Notify | `history_fragment_header_t` + 1 B | 9 B | Unified history header | `data_type = 0xFF`, `status = error`, payload[0] = code |

Provides on-demand access to rainfall history. Clients issue a 16-byte command and receive results via notifications that reuse the shared 8-byte `history_fragment_header_t`. Hourly and daily responses are auto-fragmented up to 240-byte payloads; recent totals, reset acknowledgements, and calibration acknowledgements fit in a single fragment.

## Characteristic Metadata
| Item | Value |
|------|-------|
| Properties | Read, Write, Notify |
| Permissions | Read, Write |
| Command Size | 16 bytes (packed) |
| Fragment Header | 8 bytes (`history_fragment_header_t`) |
| Max Fragment Payload | 240 bytes (`RAIN_HISTORY_FRAGMENT_SIZE`) |
| Notification Priority | Normal (`bt_gatt_notify` with ~50 ms pacing) |
| Internal Guard | One active command per controller (`rain_history_state.command_active`) |

## Command Payload (`struct rain_history_cmd_data`)
| Offset | Field | Type | Notes |
|--------|-------|------|-------|
| 0 | `command` | `uint8_t` | See supported commands below |
| 1 | `start_timestamp` | `uint32_t` | Unix seconds, inclusive start (0 = oldest) |
| 5 | `end_timestamp` | `uint32_t` | Unix seconds, inclusive end (0 = now) |
| 9 | `max_entries` | `uint16_t` | 1...65535; rejected when 0 for data commands |
| 11 | `data_type` | `uint8_t` | Must match command expectations (see table) |
| 12 | `reserved[4]` | `uint8_t[4]` | Set to 0; echoed back on read |

Little-endian packing is required for all integer fields.

### Supported Commands
| Command | Value | `data_type` expectation | Result |
|---------|-------|------------------------|--------|
| `RAIN_CMD_GET_HOURLY` | `0x01` | Must be `0` | Streams hourly entries (`rain_hourly_data_t`, 8 bytes each) |
| `RAIN_CMD_GET_DAILY` | `0x02` | Must be `1` | Streams daily entries (`rain_daily_data_t`, 12 bytes each) |
| `RAIN_CMD_GET_RECENT` | `0x03` | Ignored (set to `0xFE` internally) | Single fragment with last hour / 24 h / 7 d totals |
| `RAIN_CMD_RESET_DATA` | `0x10` | Ignored | Single fragment acknowledgement (data_type `0xFD`) |
| `RAIN_CMD_CALIBRATE` | `0x20` | Ignored | Single fragment acknowledgement (data_type `0xFC`) |

Any other opcode returns error `0x04` (unknown command).

### Response Formats
- **Hourly entry (8 bytes)**: `uint32_t hour_epoch`, `uint16_t rainfall_mm_x100`, `uint8_t pulse_count`, `uint8_t data_quality`.
- **Daily entry (12 bytes)**: `uint32_t day_epoch`, `uint32_t total_rainfall_mm_x100`, `uint16_t max_hourly_mm_x100`, `uint8_t active_hours`, `uint8_t data_completeness`.
- **Recent totals fragment** (`data_type = 0xFE`, `fragment_size = 16`): `uint32_t` last hour, `uint32_t` last 24 h, `uint32_t` last 7 d, `uint32_t` reserved (0).
- **Reset acknowledgement** (`data_type = 0xFD`, `fragment_size = 0`): header only.
- **Calibration acknowledgement** (`data_type = 0xFC`, `fragment_size = 0`): header only.

## Notification Header (`history_fragment_header_t`)
| Field | Meaning |
|-------|---------|
| `data_type` | `0` hourly, `1` daily, `0xFE` recent totals, `0xFD` reset ack, `0xFC` calibration ack, `0xFF` error |
| `status` | `0` success, non-zero error code (see table) |
| `entry_count` | Reserved for future batching (currently 0) |
| `fragment_index` | 0-based index |
| `total_fragments` | Total fragments for this command |
| `fragment_size` | Payload bytes in this fragment (<=240) |
| `reserved` | 0 |

Entries are sent sequentially; clients know the transfer is complete when `fragment_index + 1 == total_fragments`. Successive fragments are scheduled roughly 50 ms apart. The firmware enforces `RAIN_HISTORY_MAX_FRAGMENTS = 20`; exceeding that cap produces error code `0x07`.

## Error Codes (`status` and payload[0])
| Code | Description | Emission Point |
|------|-------------|----------------|
| `0x01` | Busy - another connection already owns the command slot | Early validation |
| `0x02` | Invalid parameter - bad `data_type`, inverted timestamps, etc. | Early validation |
| `0x03` | Operation failed - storage clear error | Reset command |
| `0x04` | Unknown command | Early validation |
| `0x05` | Memory error - allocation failed for history buffer | Hourly/Daily retrieval |
| `0x06` | Data error - backend could not supply entries | Hourly/Daily retrieval |
| `0x07` | Too much data - fragment count would exceed limit | Hourly/Daily retrieval |
| `0xFE` | Invalid parameters - `max_entries == 0` or disallowed `data_type` | Pre-flight checks |
| `0xFF` | Invalid command (legacy guard) | Never emitted in current path |

All errors are transmitted as a single 9-byte notification (`header` + 1 byte payload) and reset the active-command flag.

## Behaviour Summary
- **One command at a time**: A new request from another connection while a command is active returns `0x01`.
- **Reads are diagnostic only**: A read returns the last accepted 16-byte command; data delivery always happens via notifications.
- **Hourly / Daily retrieval**: The handler allocates a buffer with `k_malloc`, fills it with `rain_history_get_hourly()` or `rain_history_get_daily()`, then iterates fragments (`bt_gatt_notify`) with ~50 ms spacing. Buffers are freed after the last fragment.
- **Recent totals**: Built on-the-fly and sent immediately in one fragment.
- **Reset / Calibrate**: Trigger subsystem actions (`rain_history_clear_all()`, `rain_sensor_reset_*()`), then emit a zero-length acknowledgement fragment. Failures surface as `0x03`.

## Client Guidance
- Always send the full 16-byte command at offset 0; partial writes are rejected.
- Use realistic `max_entries` values so that `actual_count * entry_size` stays below `RAIN_HISTORY_MAX_FRAGMENTS * 240` (e.g., <=480 entries for hourly data).
- Expect little-endian floats/integers; convert `*_mm_x100` by dividing by 100.
- Subscribe to notifications before issuing commands; the firmware does not retry when notifications are disabled.
- Treat errors as terminal for the in-flight command; resend a new command once the previous one reports an error or completion.

### Example - Request last 24 hourly entries
```javascript
const now = Math.floor(Date.now() / 1000);
const cmd = new DataView(new ArrayBuffer(16));
cmd.setUint8(0, 0x01);                 // Hourly
cmd.setUint32(1, now - 24 * 3600, true);
cmd.setUint32(5, now, true);
cmd.setUint16(9, 24, true);
cmd.setUint8(11, 0);                   // Required for hourly
await characteristic.writeValue(cmd.buffer);

// Assemble notifications using the header metadata
```

### Example - Multi-Fragment Reassembly with TLV ACK
```javascript
function handleRainHistoryNotification(dataView) {
  const header = {
    dataType: dataView.getUint8(0),
    status: dataView.getUint8(1),
    entryCount: dataView.getUint16(2, true),
    fragmentIndex: dataView.getUint8(4),
    totalFragments: dataView.getUint8(5),
    fragmentSize: dataView.getUint8(6)
  };

  if (header.status === 0x06) {
    console.warn('Invalid fragment', header.fragmentIndex,
                 'expected range 0..', header.totalFragments - 1);
    return;
  }
  if (header.status === 0x07) {
    setTimeout(resendLastCommand, 1000); // obey 1 s throttle
    return;
  }

  const payload = new Uint8Array(dataView.buffer, dataView.byteOffset + 8, header.fragmentSize);
  appendFragment(header.fragmentIndex, payload);

  if (header.fragmentIndex + 1 === header.totalFragments) {
    acknowledgeHistoryDownload(); // optional application-level ACK
  }
}
```

## Related Characteristics
- `18-rain-sensor-config.md`
- `19-rain-sensor-data.md`
- `24-rain-integration-status.md`
- `12-history-management.md`
