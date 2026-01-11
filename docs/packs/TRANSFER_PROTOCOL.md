# Pack Transfer Protocol Documentation

**Version**: 1.0.0  
**Characteristic UUID**: `12345678-1234-5678-9abc-def123456788`

## Overview

The Pack Transfer Protocol enables reliable transfer of large plant packs (multiple plants) over BLE. It uses a simple state machine with opcodes for multi-part data transfer, CRC32 validation, and atomic installation.

---

## Why Multi-Part Transfer?

| Scenario | Single Plant | Pack Transfer |
|----------|--------------|---------------|
| Plants | 1 | 1-64 |
| Size | 120 bytes | Up to 7,680 bytes |
| Writes | 1 | 1 START + N DATA + 1 COMMIT |
| Use case | Quick install | Bulk pack install |

Single-plant install via Pack Plant characteristic works for 1 plant (120 bytes). For packs with multiple plants, the transfer protocol provides:

- **Chunking**: Works with any MTU
- **Progress**: Real-time transfer progress
- **Integrity**: CRC32 verification before commit
- **Atomicity**: All plants installed or none

---

## Protocol State Machine

```
                    ┌─────────────┐
                    │    IDLE     │◄──────────────────┐
                    └──────┬──────┘                   │
                           │ START                    │
                           ▼                          │
                    ┌─────────────┐                   │
           ┌───────►│  RECEIVING  │───────┐           │
           │        └──────┬──────┘       │           │
           │               │              │           │
         DATA              │            ABORT         │
           │               │              │           │
           │            COMMIT            │           │
           │               │              │           │
           │      ┌────────┴────────┐     │           │
           │      ▼                 ▼     ▼           │
           │ ┌─────────┐       ┌─────────┐            │
           │ │COMPLETE │       │  ERROR  │            │
           │ └────┬────┘       └────┬────┘            │
           │      │                 │                 │
           │      └─────────────────┴─────────────────┘
           │                  (auto-reset on next START)
           │
           └──────────────────────────────────────────┘
```

### States

| State | Value | Description |
|-------|-------|-------------|
| `IDLE` | 0 | No transfer in progress |
| `RECEIVING` | 1 | Accepting data chunks |
| `COMPLETE` | 2 | Transfer successful |
| `ERROR` | 3 | Transfer failed |

---

## Opcodes

| Opcode | Name | Direction | Description |
|--------|------|-----------|-------------|
| 0x01 | START | Client→Device | Begin transfer |
| 0x02 | DATA | Client→Device | Send data chunk |
| 0x03 | COMMIT | Client→Device | Finalize transfer |
| 0x04 | ABORT | Client→Device | Cancel transfer |
| 0x05 | STATUS | Client→Device | Request status update |

---

## Message Formats

### START (0x01) - 47 bytes

Begin a new pack transfer.

```
Offset  Size  Field           Description
------  ----  -----           -----------
0       1     opcode          0x01 (START)
1       2     pack_id         Pack identifier
3       2     version         Pack version
5       2     plant_count     Number of plants
7       4     total_size      Total payload bytes
11      4     crc32           CRC32 of entire payload
15      32    name            Pack name (null-terminated)
```

**C Structure:**
```c
typedef struct __attribute__((packed)) {
    uint8_t opcode;             // 0x01
    uint16_t pack_id;
    uint16_t version;
    uint16_t plant_count;
    uint32_t total_size;
    uint32_t crc32;
    char name[32];
} bt_pack_xfer_start_t;

#define BT_PACK_XFER_START_SIZE 47
```

**Constraints:**
- `plant_count` must be 1-64
- `total_size` must equal `plant_count × 120`
- `total_size` must be ≤ 7,680 bytes

**Example (hex):**
```
01                          // opcode = START
01 00                       // pack_id = 1
01 00                       // version = 1
05 00                       // plant_count = 5
58 02 00 00                 // total_size = 600 (5 × 120)
AB CD EF 12                 // crc32 = 0x12EFCDAB
56 65 67 65 74 61 62 6C 65 73 00 ...  // "Vegetables"
```

---

### DATA (0x02) - 7+N bytes

Send a data chunk.

```
Offset  Size  Field           Description
------  ----  -----           -----------
0       1     opcode          0x02 (DATA)
1       4     offset          Byte offset in transfer
5       2     length          Chunk data length
7       N     data            Chunk payload
```

**C Structure (header only):**
```c
typedef struct __attribute__((packed)) {
    uint8_t opcode;             // 0x02
    uint32_t offset;            // Byte offset
    uint16_t length;            // Chunk length
    // uint8_t data[N];         // Payload follows
} bt_pack_xfer_data_header_t;

#define BT_PACK_XFER_DATA_HEADER_SIZE 7
```

**Constraints:**
- `offset` must match `bytes_received` on device
- `length` must match actual payload size
- `offset + length` must not exceed `total_size`

**Recommended chunk size:** 240 bytes (fits in 247-byte MTU)

**Example (hex):**
```
02                          // opcode = DATA
00 00 00 00                 // offset = 0
F0 00                       // length = 240
[240 bytes of plant data]   // First 2 plants (partial)
```

---

### COMMIT (0x03) - 1 byte

Finalize the transfer and install all plants.

```
Offset  Size  Field           Description
------  ----  -----           -----------
0       1     opcode          0x03 (COMMIT)
```

**Example (hex):**
```
03                          // opcode = COMMIT
```

**Process:**
1. Verify `bytes_received == total_size`
2. Calculate CRC32 of received data
3. Compare with expected CRC32 from START
4. Parse plants from buffer
5. Install each plant via `pack_storage_install_plant()`
6. Set state to COMPLETE or ERROR

---

### ABORT (0x04) - 1 byte

Cancel the current transfer.

```
Offset  Size  Field           Description
------  ----  -----           -----------
0       1     opcode          0x04 (ABORT)
```

**Example (hex):**
```
04                          // opcode = ABORT
```

**Effect:** Immediately resets state to IDLE, discards buffer.

---

### STATUS (0x05) - 1 byte

Request status notification.

```
Offset  Size  Field           Description
------  ----  -----           -----------
0       1     opcode          0x05 (STATUS)
```

**Effect:** Updates internal status and triggers notification (if enabled).

---

## Status Response (Read/Notify) - 16 bytes

```
Offset  Size  Field           Description
------  ----  -----           -----------
0       1     state           Transfer state (0-3)
1       1     progress_pct    Progress 0-100%
2       2     pack_id         Current pack ID (0 if idle)
4       4     bytes_received  Bytes received so far
8       4     bytes_expected  Total bytes expected
12      1     last_error      Last error code
13      3     reserved        Reserved (zero)
```

**C Structure:**
```c
typedef struct __attribute__((packed)) {
    uint8_t state;              // pack_transfer_state_t
    uint8_t progress_pct;       // 0-100
    uint16_t pack_id;
    uint32_t bytes_received;
    uint32_t bytes_expected;
    uint8_t last_error;         // pack_result_t
    uint8_t reserved[3];
} bt_pack_xfer_status_t;

#define BT_PACK_XFER_STATUS_SIZE 16
```

**Example (hex) - Transfer in progress:**
```
01                          // state = RECEIVING
32                          // progress_pct = 50%
01 00                       // pack_id = 1
2C 01 00 00                 // bytes_received = 300
58 02 00 00                 // bytes_expected = 600
00                          // last_error = SUCCESS
00 00 00                    // reserved
```

---

## Transfer Limits

| Parameter | Value | Notes |
|-----------|-------|-------|
| `PACK_TRANSFER_MAX_PLANTS` | 64 | Maximum plants per transfer |
| `PACK_TRANSFER_BUFFER_SIZE` | 7,680 | 64 × 120 bytes |
| `PACK_TRANSFER_CHUNK_SIZE` | 240 | Recommended chunk (MTU-7) |
| `PACK_TRANSFER_TIMEOUT_SEC` | 120 | Inactivity timeout |

---

## Complete Transfer Example

### Scenario: Install 5-plant vegetable pack

**Pack details:**
- pack_id: 1
- version: 1
- plant_count: 5
- total_size: 600 bytes (5 × 120)
- CRC32: 0x12EFCDAB

### Step 1: Calculate CRC32

Client-side:
```python
import zlib
plants_data = b''.join([plant.to_bytes() for plant in plants])
crc = zlib.crc32(plants_data) & 0xFFFFFFFF
```

### Step 2: Send START

**Write to Pack Transfer characteristic:**
```hex
01 01 00 01 00 05 00 58 02 00 00 AB CD EF 12
56 65 67 65 74 61 62 6C 65 73 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
```
(47 bytes)

**Expected notification:**
```hex
01 00 01 00 00 00 00 00 58 02 00 00 00 00 00 00
```
(state=RECEIVING, progress=0%, pack_id=1)

### Step 3: Send DATA chunks

**Chunk 1 (offset 0, 240 bytes):**
```hex
02 00 00 00 00 F0 00 [240 bytes plant data]
```

**Notification:**
```hex
01 28 01 00 F0 00 00 00 58 02 00 00 00 00 00 00
```
(state=RECEIVING, progress=40%, bytes=240/600)

**Chunk 2 (offset 240, 240 bytes):**
```hex
02 F0 00 00 00 F0 00 [240 bytes plant data]
```

**Notification:**
```hex
01 50 01 00 E0 01 00 00 58 02 00 00 00 00 00 00
```
(progress=80%, bytes=480/600)

**Chunk 3 (offset 480, 120 bytes):**
```hex
02 E0 01 00 00 78 00 [120 bytes plant data]
```

**Notification:**
```hex
01 64 01 00 58 02 00 00 58 02 00 00 00 00 00 00
```
(progress=100%, bytes=600/600)

### Step 4: Send COMMIT

**Write:**
```hex
03
```

**Expected notification (success):**
```hex
02 64 01 00 58 02 00 00 58 02 00 00 00 00 00 00
```
(state=COMPLETE, last_error=SUCCESS)

---

## Error Handling

### Error Scenarios

| Error | last_error | Recovery |
|-------|------------|----------|
| Invalid plant count | INVALID_DATA | Send correct START |
| Size mismatch | INVALID_DATA | Recalculate and retry |
| Offset mismatch | INVALID_DATA | Resend from correct offset |
| CRC mismatch | CRC_MISMATCH | Verify data and retry |
| Storage full | STORAGE_FULL | Delete unused plants |
| Timeout | IO_ERROR | Restart transfer |
| Plant install fail | IO_ERROR | Check individual plants |

### Timeout Handling

Transfer times out after 120 seconds of inactivity (no DATA chunks received).

**Detection:**
```c
if ((k_uptime_get_32() - xfer_state.last_activity_time) > 120000) {
    xfer_state.state = PACK_XFER_STATE_ERROR;
    xfer_state.last_error = PACK_RESULT_IO_ERROR;
}
```

**Recovery:** Client should retry from START.

### Partial Failure

If some plants fail to install during COMMIT:
- State becomes ERROR
- `last_error` is set to IO_ERROR
- Successfully installed plants remain
- Client can retry individual plants via Pack Plant characteristic

---

## Implementation Notes

### Buffer Allocation

```c
static uint8_t transfer_buffer[PACK_TRANSFER_BUFFER_SIZE] __attribute__((aligned(4)));
```

7,680 bytes allocated statically to avoid heap fragmentation.

### CRC32 Calculation

Uses IEEE polynomial (CRC-32/ISO-HDLC):

```c
uint32_t pack_storage_crc32(const void *data, size_t len) {
    const uint8_t *buf = data;
    uint32_t crc = 0xFFFFFFFF;
    
    for (size_t i = 0; i < len; i++) {
        crc ^= buf[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
        }
    }
    return ~crc;
}
```

### Thread Safety

The transfer state machine is protected by implicit BLE serialization (writes processed sequentially).

---

## Client Implementation Guide

### Pseudocode

```python
class PackTransfer:
    def __init__(self, device, plants):
        self.device = device
        self.plants = plants
        self.char = device.get_characteristic(PACK_XFER_UUID)
        
    async def transfer(self, pack_id, version, name):
        # Enable notifications
        await self.char.start_notify(self.on_notify)
        
        # Prepare payload
        payload = b''.join([p.serialize() for p in self.plants])
        crc = zlib.crc32(payload) & 0xFFFFFFFF
        
        # Send START
        start = struct.pack('<BHHHI32s',
            0x01,           # opcode
            pack_id,
            version,
            len(self.plants),
            len(payload),
            crc,
            name.encode('utf-8')[:31]
        )
        await self.char.write(start)
        
        # Send DATA chunks
        offset = 0
        while offset < len(payload):
            chunk = payload[offset:offset+240]
            data = struct.pack('<BIH', 0x02, offset, len(chunk)) + chunk
            await self.char.write(data)
            offset += len(chunk)
            
            # Wait for progress notification
            await self.wait_for_notify()
        
        # Send COMMIT
        await self.char.write(bytes([0x03]))
        
        # Wait for final status
        status = await self.wait_for_notify()
        return status.state == COMPLETE
        
    def on_notify(self, data):
        status = parse_status(data)
        print(f"Progress: {status.progress_pct}%")
```

### Recommended Timing

| Event | Delay |
|-------|-------|
| After START | Wait for notification |
| Between chunks | 10-50ms or wait for notification |
| After COMMIT | Wait for notification |
| Retry on error | 500ms-2s backoff |

---

## Wireshark Analysis

### BLE ATT Filter

```
btatt.handle == 0x001a && btatt.opcode == 0x12
```

### Sample Capture

```
Frame 1: ATT Write Request (START)
  Handle: 0x001a
  Value: 01 01 00 01 00 05 00 58 02 00 00 ...

Frame 2: ATT Handle Value Notification
  Handle: 0x001a
  Value: 01 00 01 00 00 00 00 00 58 02 00 00 00 00 00 00

Frame 3: ATT Write Request (DATA)
  Handle: 0x001a
  Value: 02 00 00 00 00 f0 00 [data...]
```

---

## Security Considerations

1. **Encryption Required**: All operations require encrypted connection
2. **Validation**: All data validated before storage
3. **CRC32**: Ensures data integrity during transfer
4. **Timeout**: Prevents resource exhaustion attacks
5. **Size Limits**: Buffer size prevents overflow
