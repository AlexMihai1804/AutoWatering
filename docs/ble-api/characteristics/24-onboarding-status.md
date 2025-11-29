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
| 4 | `channel_config_flags` | `uint64_t` | 8 bits per channel - basic flags (see below) |
| 12 | `system_config_flags` | `uint32_t` | Eight defined bits |
| 16 | `schedule_config_flags` | `uint8_t` | One bit per channel |
| 17 | `onboarding_start_time` | `uint32_t` | Unix timestamp (s) |
| 21 | `last_update_time` | `uint32_t` | Unix timestamp (s) |
| 25 | `channel_extended_flags` | `uint64_t` | 8 bits per channel - extended flags (see below) |

**Total size: 33 bytes** (was 25 bytes before extended flags were added)

Little-endian packing is required for all multibyte fields.

### Flag Bitmaps

#### Channel Flags (`uint64_t` - 8 bits per channel)
Each channel (0-7) has 8 flag bits. Bit position = `channel_id * 8 + flag_bit`.

| Bit | Flag | Set When |
|-----|------|----------|
| 0 | `CHANNEL_FLAG_PLANT_TYPE_SET` | User configures plant type |
| 1 | `CHANNEL_FLAG_SOIL_TYPE_SET` | User configures soil type |
| 2 | `CHANNEL_FLAG_IRRIGATION_METHOD_SET` | User configures irrigation method |
| 3 | `CHANNEL_FLAG_COVERAGE_SET` | User configures area/plant count |
| 4 | `CHANNEL_FLAG_SUN_EXPOSURE_SET` | User configures sun exposure |
| 5 | `CHANNEL_FLAG_NAME_SET` | User sets channel name |
| 6 | `CHANNEL_FLAG_WATER_FACTOR_SET` | User sets water need factor |
| 7 | `CHANNEL_FLAG_ENABLED` | Channel is enabled |

#### Channel Extended Flags (`uint64_t` - 8 bits per channel)
Advanced configuration flags. Bit position = `channel_id * 8 + flag_bit`.

| Bit | Flag | Set When |
|-----|------|----------|
| 0 | `CHANNEL_EXT_FLAG_FAO56_READY` | Auto-set when all FAO-56 requirements met |
| 1 | `CHANNEL_EXT_FLAG_RAIN_COMP_SET` | Rain compensation enabled for channel |
| 2 | `CHANNEL_EXT_FLAG_TEMP_COMP_SET` | Temperature compensation enabled for channel |
| 3 | `CHANNEL_EXT_FLAG_SCHEDULE_SET` | Schedule configured for channel |
| 4 | `CHANNEL_EXT_FLAG_LATITUDE_SET` | Latitude set for channel (≠ 0) |
| 5 | `CHANNEL_EXT_FLAG_AUTO_MODE_SET` | Auto mode explicitly configured |
| 6 | `CHANNEL_EXT_FLAG_INTERVAL_MODE_SET` | Interval mode configured |
| 7 | Reserved | Reserved for future use |

**FAO-56 Requirements**: For `CHANNEL_EXT_FLAG_FAO56_READY` to be set:
- `CHANNEL_FLAG_PLANT_TYPE_SET` ✓
- `CHANNEL_FLAG_SOIL_TYPE_SET` ✓
- `CHANNEL_FLAG_IRRIGATION_METHOD_SET` ✓
- `CHANNEL_FLAG_COVERAGE_SET` ✓
- `CHANNEL_EXT_FLAG_LATITUDE_SET` ✓

#### System Flags (`uint32_t`)
| Bit | Flag | Set When |
|-----|------|----------|
| 0 | `SYSTEM_FLAG_TIMEZONE_SET` | User saves timezone configuration |
| 1 | `SYSTEM_FLAG_FLOW_CALIBRATED` | User saves flow sensor calibration |
| 2 | `SYSTEM_FLAG_MASTER_VALVE_SET` | User configures master valve |
| 3 | `SYSTEM_FLAG_RTC_CONFIGURED` | User sets date/time via BLE |
| 4 | `SYSTEM_FLAG_RAIN_SENSOR_SET` | User saves rain sensor configuration |
| 5 | `SYSTEM_FLAG_POWER_MODE_SET` | User changes power mode |
| 6 | `SYSTEM_FLAG_LOCATION_SET` | User sets latitude (≠ 0) |
| 7 | `SYSTEM_FLAG_INITIAL_SETUP_DONE` | Auto-set when: 1 channel configured + RTC + timezone |

#### Schedule Flags (`uint8_t`)
Bit `n` indicates channel `n` has a schedule configured with `auto_enabled = true`.

### Flag Behavior
- **Onboarding flags**: Once set to `true`, they remain `true` until factory reset
- **Purpose**: Track whether user has completed configuration for each feature
- **Reset**: All flags reset to `false` only on factory reset
- **Auto-set**: `SYSTEM_FLAG_INITIAL_SETUP_DONE` is automatically set when minimum requirements are met

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

## Mobile App Integration Guide

### Deciding Between Onboarding UI vs Normal UI

```javascript
// System flag constants
const SYSTEM_FLAG_TIMEZONE_SET       = (1 << 0);
const SYSTEM_FLAG_FLOW_CALIBRATED    = (1 << 1);
const SYSTEM_FLAG_MASTER_VALVE_SET   = (1 << 2);
const SYSTEM_FLAG_RTC_CONFIGURED     = (1 << 3);
const SYSTEM_FLAG_RAIN_SENSOR_SET    = (1 << 4);
const SYSTEM_FLAG_POWER_MODE_SET     = (1 << 5);
const SYSTEM_FLAG_LOCATION_SET       = (1 << 6);
const SYSTEM_FLAG_INITIAL_SETUP_DONE = (1 << 7);

function decideUIMode(status) {
  // Case 1: Brand new device - show full onboarding wizard
  if (status.overall === 0) {
    return { mode: 'FULL_ONBOARDING', screen: 'welcome' };
  }
  
  // Case 2: Initial setup complete - show normal dashboard
  if (status.systemFlags & SYSTEM_FLAG_INITIAL_SETUP_DONE) {
    return { mode: 'NORMAL_UI', screen: 'dashboard' };
  }
  
  // Case 3: Partially configured - show resume setup prompt
  return { 
    mode: 'RESUME_SETUP',
    missingSteps: getMissingSteps(status)
  };
}

function getMissingSteps(status) {
  const missing = [];
  
  // Critical requirements for INITIAL_SETUP_DONE
  if (!(status.systemFlags & SYSTEM_FLAG_RTC_CONFIGURED)) {
    missing.push({ step: 'SET_TIME', priority: 'critical' });
  }
  if (!(status.systemFlags & SYSTEM_FLAG_TIMEZONE_SET)) {
    missing.push({ step: 'SET_TIMEZONE', priority: 'critical' });
  }
  
  // Check if at least one channel is fully configured
  const hasConfiguredChannel = checkChannelConfiguration(status.channelFlags);
  if (!hasConfiguredChannel) {
    missing.push({ step: 'CONFIGURE_CHANNEL', priority: 'critical' });
  }
  
  // Optional but recommended
  if (!(status.systemFlags & SYSTEM_FLAG_FLOW_CALIBRATED)) {
    missing.push({ step: 'CALIBRATE_FLOW', priority: 'recommended' });
  }
  if (!(status.systemFlags & SYSTEM_FLAG_LOCATION_SET)) {
    missing.push({ step: 'SET_LOCATION', priority: 'recommended' });
  }
  
  return missing;
}

function checkChannelConfiguration(channelFlags) {
  // Check if any channel has minimum required flags (bits 0-3)
  const REQUIRED_FLAGS = 0x0F; // plant + soil + irrigation + coverage
  
  for (let ch = 0; ch < 8; ch++) {
    const flags = Number((channelFlags >> BigInt(ch * 8)) & 0xFFn);
    if ((flags & REQUIRED_FLAGS) === REQUIRED_FLAGS) {
      return true; // At least one channel is configured
    }
  }
  return false;
}
```

### Best Practices
1. **Subscribe to notifications** before making configuration changes
2. **Cache the status** locally to avoid repeated BLE reads
3. **Show progress indicator** using `overall_completion_pct`
4. **Guide users** through missing steps based on flag analysis
5. **Don't block normal UI** if only optional features are missing

## Related Modules
- `src/onboarding_state.c` - flag bookkeeping, NVS integration, completion math.
- `src/bt_irrigation_service.c` - read handler, notification assembler, priority scheduling.
- `docs/ble-api/characteristics/26-reset-control.md` - reset confirmation flow that may clear onboarding state.