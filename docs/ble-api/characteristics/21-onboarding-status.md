# Onboarding Status Characteristic (UUID: 12345678-1234-5678-9abcde20)

> Operation Summary
| Operation | Payload | Size | Fragmentation | Notes |
|-----------|---------|------|---------------|-------|
| Read | `struct onboarding_status_data` | 29 B | None | Direct snapshot, no header |
| Notify | `history_fragment_header_t` + payload | 8 B + <=29 B | Always uses unified header | Emitted on CCC enable and every state mutation |
| Write | - | - | - | Flags mutate through internal onboarding APIs only |

Snapshot of onboarding progress used by clients to drive setup workflows. Firmware stores the underlying state in NVS and recomputes percentages on each read/notify.

## Characteristic Metadata
| Item | Value |
|------|-------|
| Properties | Read, Notify |
| Permissions | Read |
| Notification Priority | Low (`NOTIFY_PRIORITY_LOW`, background queue ~1000 ms cadence) |
| Notification Helper | `safe_notify` with 20 ms inter-fragment delay |
| Fragment Header | `history_fragment_header_t` (`data_type = 0`) |

## Payload Layout (`struct onboarding_status_data`)
| Offset | Field | Type | Notes |
|--------|-------|------|-------|
| 0 | `overall_completion_pct` | `uint8_t` | Weighted total (60% channels, 30% system, 10% schedules) |
| 1 | `channels_completion_pct` | `uint8_t` | (# set channel flag bits / 64) x 100 |
| 2 | `system_completion_pct` | `uint8_t` | (# set system flag bits / 8) x 100 |
| 3 | `schedules_completion_pct` | `uint8_t` | (# set schedule bits / 8) x 100 |
| 4 | `channel_config_flags` | `uint64_t` | 8 bits per channel (see below) |
| 12 | `system_config_flags` | `uint32_t` | Eight defined bits |
| 16 | `schedule_config_flags` | `uint8_t` | One bit per channel |
| 17 | `onboarding_start_time` | `uint32_t` | Unix timestamp (s) |
| 21 | `last_update_time` | `uint32_t` | Unix timestamp (s) |
| 25 | `reserved[4]` | `uint8_t[4]` | Must be 0 |

Little-endian packing is required for all multibyte fields.

### Flag Bitmaps
- Channel flags (`uint64_t`): 8 bits per channel, in channel order `[0...7]` and bit order `CHANNEL_FLAG_*` (plant type, soil type, irrigation method, coverage, sun exposure, name, water factor, enabled).
- System flags (`uint32_t`): `SYSTEM_FLAG_*` (timezone placeholder, flow calibration, master valve, RTC, rain sensor, power mode, location, initial setup).
- Schedule flags (`uint8_t`): bit `n` indicates channel `n` has at least one schedule.

## Notification Format
Notifications always include the unified 8-byte header so clients can reuse the existing history reassembly logic:

| Header Field | Value |
|--------------|-------|
| `data_type` | `0` (identifies onboarding status stream) |
| `status` | `0` (no error path implemented) |
| `entry_count` | `1` (single logical structure) |
| `fragment_index` | 0...`total_fragments-1` |
| `total_fragments` | Number of chunks (typically `1`) |
| `fragment_size` | Bytes of payload appended to this header |
| `reserved` | 0 |

The handler computes the negotiated ATT payload (`mtu - 3`). If it exceeds 29 bytes, only one fragment is produced, but the header is still present. For smaller MTUs, the struct is split across multiple packets with a 20 ms delay between sends. Clients should buffer fragments until all (`fragment_index + 1 == total_fragments`) arrive, then parse the 29-byte payload as the structure above.

## Behaviour
- **Read path**: Calls `onboarding_get_state()` and mirrors the contents directly into the BLE struct. If state retrieval fails, a `BT_ATT_ERR_UNLIKELY` is returned.
- **Notify path**: Guarded by CCC state; invoked on each onboarding flag update and when notifications are enabled. Uses the same percentage calculations as the read path, then sends via `safe_notify` with low-priority scheduling.
- **Throttling**: Notifications enter the global low-priority queue, which enforces ~1 s minimum spacing relative to other low-priority items. Within a single burst, fragments are still spaced at 20 ms.
- **Error handling**: No characteristic-specific error payloads; transport failures are logged and surfaced as standard ATT errors.

## Client Notes
- Subscribe before issuing configuration changes so the initial snapshot arrives immediately after CCC enable.
- Treat the struct as authoritative; percentages and flags are recomputed at emission time and remain consistent across read/notify.
- Persist timestamps to track overall onboarding duration (`onboarding_start_time`) and last activity (`last_update_time`).

### Minimal Parsing Example (Web Bluetooth)
```javascript
function parseOnboardingStatus(dataView) {
  let offset = 0;
  const readU8 = () => dataView.getUint8(offset++);
  const readU32 = () => dataView.getUint32((offset += 4) - 4, true);
  const readU64 = () => dataView.getBigUint64((offset += 8) - 8, true);

  return {
    overall: readU8(),
    channels: readU8(),
    system: readU8(),
    schedules: readU8(),
    channelFlags: readU64(),
    systemFlags: readU32(),
    scheduleFlags: readU8(),
    onboardingStart: readU32(),
    lastUpdate: readU32()
  };
}
```

## Troubleshooting
- **No notifications**: Confirm CCC is set to `Notify` and that another connection is not holding the low-priority queue saturated.
- **Percentages frozen**: Ensure onboarding state updates (`onboarding_update_*`) are invoked; the characteristic is read-only and mirrors persisted flags.
- **Fragmentation surprises**: Even with larger MTUs, the header is always present. Verify client reassembly handles the 8-byte prefix.

## Related Modules
- `src/onboarding_state.c` - flag bookkeeping, NVS integration, completion math.
- `src/bt_irrigation_service.c` - read handler, notification assembler, priority scheduling.
- `docs/ble-api/characteristics/22-reset-control.md` - reset confirmation flow that may clear onboarding state.