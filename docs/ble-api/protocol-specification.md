# BLE Protocol Specification (Verified 2026-01-04)

Authoritative specification for the AutoWatering custom BLE Irrigation Service. This file now reflects the refactored unified fragmentation header, current characteristic set (34 total: 29 Irrigation + 5 Custom Config), notification priorities/throttling, and ATT error usage as implemented in `bt_irrigation_service.c`, `bt_custom_soil_handlers.c` & `bt_gatt_structs.h`.

For quick reference tables see `ble-api/README.md`; this spec focuses on protocol rules & guarantees. If a conflict arises, code + this file take precedence.

## Service Overview

### Primary Service (Irrigation)

- **Service UUID**: `12345678-1234-5678-1234-56789abcdef0`
- **Service Type**: Primary GATT Service
- **Characteristics Count**: 29 (indexed 1-29 - see Characteristic Index below)
- **Pairing / Security**: **Encryption Enforced**. All characteristics require an encrypted link (Level 2). "Just Works" pairing is supported.
- **Negotiated MTU**: ATT MTU negotiated by stack (often >20); application purposely caps individual write fragment payloads to 20 bytes for widest client compatibility.

### Connection Parameters (Typical / Constraints)

- **Connection Interval Range**: 7.5 ms - 4000 ms (negotiated by controller)
- **Slave Latency**: 0-499 (no hard dependency in application logic)
- **Supervision Timeout**: 100 ms - 32 s
- **Concurrent Connections**: 1 (application designed for single central at a time)
- **Security Level**: **Level 2 (Encryption Required)**

## Characteristic Specifications

### Irrigation Service Characteristic Index (29)

Condensed list - full per-characteristic semantics in `characteristics/*.md` (all verified for sizes & behaviors). Fragmentation column refers to write-path fragmentation (4-byte first fragment) unless "Unified" (8-byte history-style notify header).

| # | Characteristic | UUID Suffix | Nominal Size* | R | W | N | Frag (Write) | Unified 8B Notify | Priority** |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 1 | Valve Control | def1 | 4B | Y | Y | Y | No | No | High |
| 2 | Flow Sensor | def2 | 4B | Y | - | Y | No | No | Normal |
| 3 | System Status | def3 | 1B | Y | - | Y | No | No | High |
| 4 | Channel Configuration | def4 | 76B | Y | Y | Y | Yes (types 1/2/3) | No | Normal |
| 5 | Schedule Configuration | def5 | 9B | Y | Y | Y | No | No | Normal |
| 6 | System Configuration (Enhanced) | def6 | 56B | Y | Y | Y | Long Write | No | Normal |
| 7 | Task Queue Management | def7 | 9B | Y | Y | Y | No | No | High |
| 8 | Statistics | def8 | 15B | Y | Y (1-byte select) | Y | No | No | Normal |
| 9 | RTC Configuration | def9 | 16B | Y | Y | Y (on write) | No | No | Normal |
| 10 | Alarm Status | defa | 7B | Y | Y (ack/clear) | Y | No | No | Critical |
| 11 | Calibration Management | defb | 13B | Y | Y | Y | No | No | Normal |
| 12 | History Management | defc | 12B cmd / 32B snapshot | Y | Y (query/clear) | Y | No | Yes | Low |
| 13 | Diagnostics | defd | 12B | Y | - | Y | No | No | Low |
| 14 | Growing Environment | defe | 71B | Y | Y | Y | Yes (2/3) | No | Normal |
| 15 | Auto Calc Status | de00 | 64B | Y | Y (select) | Y | Select only | Yes | Normal |
| 16 | Current Task Status | deff | 21B | Y | Y (control opcodes) | Y | No | No | High |
| 17 | Timezone Configuration | 6793 | 16B | Y | Y | Y (on write/CCC) | No | No | Normal |
| 18 | Rain Sensor Configuration | de12 | 18B | Y | Y | Y | No | No | Normal |
| 19 | Rain Sensor Data | de13 | 24B | Y | - | Y | No | No | Normal (adaptive) |
| 20 | Rain History Control | de14 | 16B cmd + var | Y | Y | Y | Cmd struct | Yes | Low |
| 21 | Environmental Data | de15 | 24B | Y | - | Y | No | No | Normal |
| 22 | Environmental History | de16 | 12B hdr + var | Y | Y | Y | Yes (query hdr) | Yes | Low |
| 23 | Compensation Status | de17 | 40B | Y | Y (select) | Y | Select only | No | Normal |
| 24 | Rain Integration Status | de18 | 78B | Y | - | Y | No | No | Normal |
| 25 | Onboarding Status | de20 | 29B | Y | - | Y | No | No | Low |
| 26 | Reset Control | de21 | 16B | Y | Y | Y | No | No | High (gated) |
| 27 | Channel Compensation Config | de22 | 44B | Y | Y | Y | No | No | Normal |
| 28 | Bulk Sync Snapshot | de23 | 60B | Y | - | - | No | No | Normal |
| 29 | Hydraulic Status | de24 | 48B | Y | Y | Y | No | No | Normal |

### Custom Configuration Service (5 characteristics)

**Service UUID**: `12345678-1234-5678-9abc-def123456780`

| # | Characteristic | UUID Suffix | Nominal Size* | R | W | N | Notes |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 30 | Custom Soil Configuration | 6781 | 76B | Y | Y | Y | Per-channel custom soil |
| 31 | Soil Moisture Configuration | 6782 | 8B | Y | Y | Y | Global + per-channel override |
| 32 | Config Reset | 6783 | Var. | Y | - | Y | Configuration reset ops |
| 33 | Config Status | 6784 | Var. | Y | Y | Y | Configuration completeness |
| 34 | Interval Mode Configuration | 6785 | 16B | Y | Y | Y | Cycle & Soak config |

*Nominal Size = packed struct size for single snapshot (excludes unified fragment header & multi-entry payloads).  
**Priority governs minimum notification spacing; adaptive logic may stretch/shrink within bounds.

Legend: Y = supported, - = not supported.

### Property Definitions

#### Read (R)

- Client can read current value
- Returns current state or configuration
- No side effects on system operation

#### Write (W)

- Client can write new values
- May trigger system actions or configuration changes
- Validation performed on all writes

#### Notify (N)

- Characteristic sends automatic updates
- Client must enable notifications via CCCD
- Updates sent based on priority and throttling rules

## Notification System

### Priority & Base Intervals

| Priority | Base Min Interval | Examples |
| --- | --- | --- |
| Critical | Immediate (no enforced delay) | Alarm Status |
| High | ~50 ms | Valve Control, System Status, Task Queue, Current Task, Reset Control results |
| Normal | ~200 ms | Flow Sensor, Configs, Statistics, Growing Env, Rain Data, Auto Calc, Compensation |
| Low | ~1000 ms | Histories (query gating), Diagnostics, Onboarding |

Adaptive adjustment: successful bursts may reduce interval (~-10% down to 70% of base); congestion or errors expand (~+20% up to 200% of base). History queries also enforce a 1000 ms acceptance window (rate-limit status=0x07 via unified header `data_type=0xFE`).

### Unified 8-Byte Fragment Header (Notifications / Multi-Entry Responses)

Used by: (a) History Management (12) multi-entry responses, (b) Rain History (20), (c) Environmental History (22), (d) Auto Calc Status notifications (15, single-fragment analytic wrapper). Onboarding Status (24) does not currently downgrade to a unified header fallback.

```c
uint8_t  data_type;       // Domain-specific sub-type or echo of request type
uint8_t  status;          // 0=OK, non-zero=error / condition (e.g. 0x07 rate limited)
uint16_t entry_count;     // Total entries encompassed in full response (echoed each fragment)
uint8_t  fragment_index;  // 0-based
uint8_t  total_fragments; // Total fragment count for this response
uint8_t  fragment_size;   // Payload bytes following header in THIS fragment
uint8_t  reserved;        // 0
```

Payload immediately follows (<=232 bytes payload after the 8B header at MTU=247). Firmware also clamps payload to the negotiated MTU, so `fragment_size` may be smaller on constrained links. Clients reassemble by concatenation in `fragment_index` order until `fragment_index == total_fragments-1`.

### 4-Byte Write Fragment Header (First Fragment Only)

Applied to large write-path structs (Channel Config, Growing Env, Enhanced System Config, etc.). Formats:

```text
[channel_id] [frag_type] [size_lo] [size_hi]    // channel-scoped
[0x00]       [frag_type] [size_lo] [size_hi]    // non channel-scoped
```

frag_type: 1=name only (Channel Config), 2=full struct (BE size decode), 3=full struct (LE, preferred). Subsequent continuation fragments omit header; each write <=20 bytes data portion.

### Buffering & Flow

The stack handles ATT TX buffering; application does not maintain a custom pool beyond static packing buffers (e.g. 1212-byte history pack, 240-byte total history fragments = 8B header + up to 232B payload). Backpressure manifests as transient `bt_gatt_notify` errors - clients should retry after a short delay (>= base interval).

### Client Configuration Characteristic Descriptor (CCCD)

Each notifiable characteristic includes a CCCD:

- **UUID**: `00002902-0000-1000-8000-00805f9b34fb`
- **Properties**: Read, Write
- **Values**:
  - `0x0000`: Notifications disabled
  - `0x0001`: Notifications enabled
  - `0x0002`: Indications enabled (not supported)

## Fragmentation Summary

Detailed rationale & code samples are in `fragmentation-guide.md`. This section provides normative rules:

1. **Write Fragments**: First fragment must include 4-byte header. `offset != 0` writes are rejected (no long-write method used). Timeouts (~5 s) reset internal state silently.
2. **Name-Only Update (Type 1)**: Channel Config only; size bytes encode UTF-8 name length (LE). Max 63 chars + length byte padded to 64 region.
3. **Full Struct (Types 2 / 3)**: Size must exactly equal current struct size in headers; mismatch -> `BT_ATT_ERR_INVALID_ATTRIBUTE_LEN`.
4. **Unified Notify Header**: Always precedes history-style multi-entry or larger analytic notifications (never used for simple fixed-size single-frame characteristics like Flow Sensor or System Status).
5. **History Rate Limit**: New query <1000 ms after previous accepted query returns single header with `data_type=0xFE`, `status=0x07` and ATT write returns `BT_ATT_ERR_VALUE_NOT_ALLOWED`.

Examples intentionally omitted here to reduce duplication; see guide.

## Data Structure Specifications

### Byte Order and Alignment

- **Byte Order**: Little-endian for all multi-byte values
- **Structure Alignment**: Packed structures (`__packed` attribute)
- **Padding**: No implicit padding between fields
- **String Encoding**: UTF-8 for text data

### Common Data Types

| Type | Size | Range | Endianness |
| --- | --- | --- | --- |
| `uint8_t` | 1 byte | 0-255 | N/A |
| `uint16_t` | 2 bytes | 0-65535 | Little-endian |
| `uint32_t` | 4 bytes | 0-4294967295 | Little-endian |
| `int16_t` | 2 bytes | -32768 to 32767 | Little-endian |
| `float` | 4 bytes | IEEE 754 | Little-endian |
| `char[]` | Variable | UTF-8 text | N/A |

### Special Values

- **Invalid Channel**: `0xFF` (255)
- **No Value Set**: `0xFFFF` (65535) for uint16_t
- **Disabled/Off**: `0x00` (0)
- **Enabled/On**: `0x01` (1)

## Error Handling

### GATT Error Codes

| Error Code | Meaning | Common Causes |
| --- | --- | --- |
| `0x01` | Invalid Handle | Characteristic not found |
| `0x02` | Read Not Permitted | Attempting to read write-only characteristic |
| `0x03` | Write Not Permitted | Attempting to write read-only characteristic |
| `0x05` | Insufficient Authentication | Security requirements not met |
| `0x06` | Request Not Supported | Operation not supported |
| `0x07` | Invalid Offset | Write offset out of range |
| `0x08` | Insufficient Authorization | Permission denied |
| `0x0D` | Invalid Attribute Length | Data size mismatch |
| `0x0E` | Unlikely Error | General error condition |

### Application-Specific Errors

#### Validation Errors

- **Invalid Channel ID**: Channel ID > 7
- **Invalid Parameter Range**: Value outside acceptable range
- **Configuration Conflict**: Incompatible settings

#### Fragmentation Errors

- **Invalid Fragment Type**: Type not 1, 2, or 3
- **Size Mismatch**: Total size doesn't match structure
- **Buffer Overflow**: Fragment data too large
- **Timeout**: Fragmentation incomplete within 5 seconds

#### System Errors

- **System Busy**: Operation cannot be performed now
- **Hardware Fault**: Hardware component failure
- **Resource Exhausted**: System resources unavailable

## Implementation Guidelines

### Client Implementation Requirements

#### Connection Management

1. **Device Discovery**: Scan for device name "AutoWatering"
2. **Service Discovery**: Locate primary service UUID
3. **Characteristic Discovery**: Enumerate all 34 characteristics (29 Irrigation + 5 Custom Config)
4. **CCCD Configuration**: Enable notifications as needed

#### Data Handling

1. **Endianness**: Use little-endian for all multi-byte values
2. **Structure Packing**: Ensure no padding in data structures
3. **Validation**: Validate all parameters before sending
4. **Error Recovery**: Implement retry logic for failed operations

#### Fragmentation Support

1. **Header Detection**: Check for fragmentation headers
2. **Fragment Assembly**: Reassemble fragments in correct order
3. **Timeout Handling**: Implement client-side timeouts
4. **Error Recovery**: Restart fragmentation on errors

### Server Implementation Details

#### Characteristic Handlers

Each characteristic implements:

```c
static ssize_t char_read(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                        void *buf, uint16_t len, uint16_t offset);
static ssize_t char_write(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                         const void *buf, uint16_t len, uint16_t offset, uint8_t flags);
static void char_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value);
```

### Notification Helpers

Implementation uses direct `bt_gatt_notify` calls with local throttle bookkeeping; no generic exported `bt_gatt_notify_throttled` symbol exists (earlier docs were speculative). Client side requires only standard CCC enable and per-priority pacing.

## Performance & Limits (Informational)

Concrete timing varies by controller + central stack. Application does not guarantee fixed latencies; below are design intents rather than hard specs:

- **Notification Burst**: High priority bursts spaced >=50 ms (subject to adaptive changes)
- **Fragment Payload Sizes**: Unified history payloads up to 232 bytes after the 8B header (total <=240 bytes at MTU=247); smaller MTU yields smaller `fragment_size`.
- **Max Packed History Build**: ~1212 bytes (12B header + 50x24B detailed) in static buffer.
- **Concurrent Fragments**: Application sends fragments sequentially with 50 ms inter-fragment delay.
- **MTU Dependence**: Larger negotiated MTU benefits single-frame fixed-size characteristics but write fragmentation still observes <=20B payload rule.

## Testing and Validation

### Protocol Compliance Testing

#### Basic Functionality

1. **Service Discovery**: Verify all 34 characteristics are discoverable (29 Irrigation + 5 Custom Config)
2. **Read Operations**: Test all readable characteristics
3. **Write Operations**: Test all writable characteristics
4. **Notifications**: Verify notification delivery and throttling

#### Fragmentation Testing

1. **Header Validation**: Test all fragment types (1, 2, 3)
2. **Size Validation**: Test various data sizes
3. **Timeout Testing**: Verify 5-second timeout behavior
4. **Error Recovery**: Test fragmentation restart after errors

#### Stress Testing

1. **Rapid Operations**: High-frequency read/write operations
2. **Concurrent Notifications**: Multiple characteristics notifying simultaneously
3. **Connection Stability**: Long-duration connections with activity
4. **Error Injection**: Deliberate errors to test recovery

### Interoperability Testing

#### Client Platforms

- **Web Bluetooth**: Chrome, Edge browsers
- **Android**: Native BLE APIs
- **iOS**: Core Bluetooth framework
- **Windows**: WinRT BLE APIs
- **Linux**: BlueZ stack
- **Python**: Bleak library
- **Node.js**: Noble library

#### BLE Tools

- **nRF Connect**: Nordic Semiconductor
- **LightBlue**: Punch Through
- **BLE Scanner**: Various implementations
- **Custom Applications**: Platform-specific implementations

## Standards Compliance

### Bluetooth SIG Specifications

- **Bluetooth Core Specification**: v5.0+
- **Generic Access Profile (GAP)**: Peripheral role
- **Generic Attribute Profile (GATT)**: Server role
- **Attribute Protocol (ATT)**: Standard implementation
- **Security Manager Protocol (SMP)**: No security requirements

### Custom Extensions

All custom behavior (fragment headers, unified 8B notify wrapper, adaptive throttling, rate-limit status codes) is purely application-layer and coexists with standard ATT/GATT procedures. No vendor-specific HCI commands required.

## Reference Implementation

### Reference

Full service declaration present in `bt_irrigation_service.c` (indices 1772-1970 lines at time of verification). UUID macros follow contiguous suffix sequence (`...def1` through rain/environmental additions). Clients SHOULD rely on discovery rather than hard-coded count to remain forward compatible.

---
This specification is now synchronized with the implemented firmware (commit time of editing). Subsequent changes must update: characteristic index, fragmentation rules (if altered), and any new status/error codes surfaced via unified header.
