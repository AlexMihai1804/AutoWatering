# Onboarding Status Characteristic (UUID: 12345678-1234-5678-1234-56789abcde20)

> **⚠️ CRITICAL: Read vs Notify Format Difference**
> - **Read**: Returns raw 29-byte struct (NO header)
> - **Notify**: Returns 8-byte header + payload (ALWAYS has header, even if single fragment)

> Operation Summary
| Operation | Payload | Size | Fragmentation | Notes |
|-----------|---------|------|---------------|-------|
| Read | `struct onboarding_status_data` | 29 B | None | **Direct data, NO header** |
| Notify | `history_fragment_header_t` + payload | 8 B + ≤29 B | Always uses unified header | **ALWAYS has 8-byte header prefix** |
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

### Parsing Examples (Web Bluetooth)

**IMPORTANT**: Read and Notify return DIFFERENT formats!
- **Read**: Returns raw 29-byte `onboarding_status_data` structure (no header)
- **Notify**: Returns 8-byte `history_fragment_header_t` + payload (may be fragmented)

#### Parsing a Read Response (no header, 29 bytes)
```javascript
function parseOnboardingStatusRead(dataView) {
  // Read response has NO header - starts directly with data
  let offset = 0;
  const readU8 = () => dataView.getUint8(offset++);
  const readU32 = () => { const v = dataView.getUint32(offset, true); offset += 4; return v; };
  const readU64 = () => { const v = dataView.getBigUint64(offset, true); offset += 8; return v; };

  return {
    overall: readU8(),           // offset 0
    channels: readU8(),          // offset 1
    system: readU8(),            // offset 2
    schedules: readU8(),         // offset 3
    channelFlags: readU64(),     // offset 4-11
    systemFlags: readU32(),      // offset 12-15
    scheduleFlags: readU8(),     // offset 16
    onboardingStart: readU32(),  // offset 17-20
    lastUpdate: readU32()        // offset 21-24
    // reserved[4] at offset 25-28 (ignored)
  };
}
```

#### Parsing Notifications (with 8-byte header, may be fragmented)
```javascript
// Buffer to accumulate fragments
let onboardingFragments = [];
let expectedFragments = 0;

function handleOnboardingNotification(dataView) {
  // First 8 bytes are always the fragment header
  const header = {
    dataType: dataView.getUint8(0),           // 0 = onboarding status
    status: dataView.getUint8(1),             // 0 = OK
    entryCount: dataView.getUint16(2, true),  // always 1
    fragmentIndex: dataView.getUint8(4),      // 0-based
    totalFragments: dataView.getUint8(5),     // total count
    fragmentSize: dataView.getUint8(6),       // payload bytes in this fragment
    reserved: dataView.getUint8(7)
  };

  // Extract payload after header (8 bytes)
  const payload = new Uint8Array(dataView.buffer, dataView.byteOffset + 8, header.fragmentSize);

  // Handle fragmentation
  if (header.fragmentIndex === 0) {
    onboardingFragments = [];
    expectedFragments = header.totalFragments;
  }
  onboardingFragments[header.fragmentIndex] = payload;

  // Check if all fragments received
  if (onboardingFragments.length === expectedFragments &&
      onboardingFragments.every(f => f !== undefined)) {
    // Reassemble complete payload
    const totalLength = onboardingFragments.reduce((sum, f) => sum + f.length, 0);
    const complete = new Uint8Array(totalLength);
    let offset = 0;
    for (const frag of onboardingFragments) {
      complete.set(frag, offset);
      offset += frag.length;
    }
    
    // Parse the reassembled 29-byte structure
    const view = new DataView(complete.buffer);
    return parseOnboardingStatusRead(view);
  }
  
  return null; // Still waiting for fragments
}
```

#### Single-Fragment Shortcut (MTU >= 40)
If your MTU is large enough (≥40 bytes), the entire payload fits in one fragment:
```javascript
function parseOnboardingSingleFragment(dataView) {
  // Skip 8-byte header, then parse as read response
  const payloadView = new DataView(dataView.buffer, dataView.byteOffset + 8);
  return parseOnboardingStatusRead(payloadView);
}
```

## Troubleshooting
- **No notifications**: Confirm CCC is set to `Notify` and that another connection is not holding the low-priority queue saturated.
- **Percentages frozen**: Ensure onboarding state updates (`onboarding_update_*`) are invoked; the characteristic is read-only and mirrors persisted flags.
- **Fragmentation surprises**: Even with larger MTUs, the header is always present. Verify client reassembly handles the 8-byte prefix.

## Related Modules
- `src/onboarding_state.c` - flag bookkeeping, NVS integration, completion math.
- `src/bt_irrigation_service.c` - read handler, notification assembler, priority scheduling.
- `docs/ble-api/characteristics/26-reset-control.md` - reset confirmation flow that may clear onboarding state.