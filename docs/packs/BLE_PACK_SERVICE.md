# BLE Pack Service Documentation

**Version**: 1.2.0  
**Files**: `src/bt_pack_handlers.h`, `src/bt_pack_handlers.c`  
**Updated**: January 2026 - Added streaming mode for plant lists

## Overview

The Pack Service is a dedicated BLE GATT service for managing custom plant packs. It provides four characteristics for plant operations, storage statistics, pack listing, and multi-part pack transfers.

---

## Service UUID

```
Primary Service: 12345678-1234-5678-9abc-def123456800
```

---

## Characteristics Summary

| Name | UUID | Properties | Description |
|------|------|------------|-------------|
| Pack Plant | `...def123456786` | R/W/N | Install/delete/list plants |
| Pack Stats | `...def123456787` | R | Storage statistics |
| Pack List | `...def123456789` | R/W | List packs and pack contents |
| Pack Transfer | `...def123456788` | R/W/N | Multi-part pack transfer |

---

## Characteristic 1: Pack Plant

**UUID**: `12345678-1234-5678-9abc-def123456786`  
**Properties**: Read, Write, Notify  
**Permissions**: Encrypted Read/Write

### Purpose

Single-plant operations: install, delete, and list plants.

### Write Operations

The write operation is polymorphic based on payload size:

| Size | Operation | Payload |
|------|-----------|---------|
| 4 bytes | List/Stream request | `bt_pack_plant_list_req_t` |
| 2 bytes | Delete request | `bt_pack_plant_delete_t` |
| 156 bytes | Install request | `pack_plant_v1_t` |

#### List Request (4 bytes) - Pagination or Streaming

```c
typedef struct __attribute__((packed)) {
    uint16_t offset;            // Pagination offset (ignored in streaming)
    uint8_t filter_pack_id;     // Filter (see below)
    uint8_t max_count;          // 0 = STREAMING MODE, >0 = pagination
} bt_pack_plant_list_req_t;
```

**Filter Values:**

| Value | Constant | Description |
|-------|----------|-------------|
| `0xFF` | `PACK_FILTER_CUSTOM_ONLY` | Only custom plants (default) |
| `0xFE` | `PACK_FILTER_ALL` | Built-in (223) + custom plants |
| `0x00` | `PACK_FILTER_BUILTIN_ONLY` | Only built-in plants (Pack 0) |
| `0x01-0xFD` | Specific pack | Only plants from that pack |

**Pagination Example (hex):**
```
00 00 FF 08    // offset=0, filter=custom only, max=8
```

**Streaming Example (hex):**
```
00 00 FE 00    // offset=0, filter=ALL, max=0 (STREAMING)
```

---

### Streaming Mode (Recommended)

> **New in v1.2.0** - High-speed plant list transfer via notifications

When `max_count=0`, firmware streams the entire plant list via notifications instead of pagination. This solves Android BLE timeout issues with large databases.

#### Why Streaming?

| Method | 223 plants | Time |
|--------|------------|------|
| Pagination | 28 pages × 200ms | **~5.6 seconds** |
| Streaming | 23 notifications × 2ms | **~50ms** |

#### Stream Response Structure

Each notification contains up to 10 plants:

```c
typedef struct __attribute__((packed)) {
    uint16_t total_count;   // Total plants matching filter
    uint8_t returned_count; // Entries in this notification (0-10)
    uint8_t flags;          // Stream status flags
    bt_pack_plant_list_entry_t entries[10]; // Up to 10 plants
} bt_pack_plant_list_resp_t;
```

**Entry structure (22 bytes each):**
```c
typedef struct __attribute__((packed)) {
    uint16_t plant_id;      // 0-222=built-in, ≥1000=custom
    uint16_t pack_id;       // 0=built-in, ≥1=custom pack
    uint16_t version;       // Installed version
    char name[16];          // Truncated name (null-terminated)
} bt_pack_plant_list_entry_t;
```

**Maximum notification size:** 4 + (10 × 22) = 224 bytes

#### Stream Flags

| Flag | Value | Meaning |
|------|-------|---------|
| `BT_PACK_STREAM_FLAG_STARTING` | `0x80` | First notification |
| `BT_PACK_STREAM_FLAG_NORMAL` | `0x00` | More coming |
| `BT_PACK_STREAM_FLAG_COMPLETE` | `0x01` | Stream finished |
| `BT_PACK_STREAM_FLAG_ERROR` | `0x02` | Stream aborted |

#### Streaming Protocol

```
┌─────────┐                              ┌──────────┐
│  App    │                              │ Firmware │
└────┬────┘                              └────┬─────┘
     │                                        │
     │  Enable notifications (CCC=0x0100)     │
     │───────────────────────────────────────►│
     │                                        │
     │  Write: 00 00 FE 00 (stream ALL)       │
     │───────────────────────────────────────►│
     │                                        │
     │  Notify: flags=0x80 (STARTING)         │
     │◄───────────────────────────────────────│
     │  [10 plants, 224 bytes]                │
     │                                        │
     │  Notify: flags=0x00 (NORMAL)           │
     │◄───────────────────────────────────────│ (2ms delay)
     │  [10 plants]                           │
     │                                        │
     │  ... repeats every 2ms ...             │
     │                                        │
     │  Notify: flags=0x01 (COMPLETE)         │
     │◄───────────────────────────────────────│
     │  [remaining plants]                    │
     │                                        │
```

#### Error Handling

- **Buffer exhaustion**: Firmware retries with exponential backoff (10→320ms)
- **Max retries**: 6 before sending ERROR flag
- **On ERROR**: App should retry after 500ms delay

#### Android Implementation

```kotlin
// Enable notifications first
gatt.setCharacteristicNotification(packPlantChar, true)
val descriptor = packPlantChar.getDescriptor(CCC_UUID)
descriptor.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
gatt.writeDescriptor(descriptor)

// Request streaming
val request = byteArrayOf(
    0x00, 0x00,       // offset = 0
    0xFE.toByte(),    // filter = ALL
    0x00              // max_count = 0 (streaming)
)
packPlantChar.value = request
gatt.writeCharacteristic(packPlantChar)

// Handle notifications
override fun onCharacteristicChanged(
    gatt: BluetoothGatt, 
    char: BluetoothGattCharacteristic
) {
    val data = char.value
    val totalCount = data.getShort(0)
    val returnedCount = data[2].toInt() and 0xFF
    val flags = data[3].toInt() and 0xFF
    
    when {
        flags and 0x80 != 0 -> plantList.clear() // STARTING
        flags == 0x02 -> { /* ERROR - retry */ }
    }
    
    // Parse entries
    for (i in 0 until returnedCount) {
        val offset = 4 + i * 22
        val plantId = data.getShort(offset)
        val packId = data.getShort(offset + 2)
        val version = data.getShort(offset + 4)
        val name = String(data, offset + 6, 16).trim('\u0000')
        plantList.add(Plant(plantId, packId, version, name))
    }
    
    if (flags == 0x01) {
        // COMPLETE - all done
        onPlantListReady(plantList)
    }
}
```

#### iOS Implementation

```swift
// Enable notifications
peripheral.setNotifyValue(true, for: packPlantCharacteristic)

// Request streaming
var request = Data(count: 4)
request[0] = 0x00; request[1] = 0x00  // offset
request[2] = 0xFE                      // filter = ALL
request[3] = 0x00                      // streaming mode
peripheral.writeValue(request, for: packPlantCharacteristic, type: .withResponse)

// Handle notifications
func peripheral(_ peripheral: CBPeripheral, 
                didUpdateValueFor characteristic: CBCharacteristic, 
                error: Error?) {
    guard let data = characteristic.value else { return }
    
    let totalCount = data.withUnsafeBytes { $0.load(as: UInt16.self) }
    let returnedCount = Int(data[2])
    let flags = data[3]
    
    if flags & 0x80 != 0 { plantList.removeAll() } // STARTING
    if flags == 0x02 { /* ERROR */ return }
    
    for i in 0..<returnedCount {
        let offset = 4 + i * 22
        let plantId = data.subdata(in: offset..<offset+2)
            .withUnsafeBytes { $0.load(as: UInt16.self) }
        // ... parse other fields
    }
    
    if flags == 0x01 { onComplete() } // COMPLETE
}
```

---

### Legacy Pagination Mode

For backwards compatibility, set `max_count > 0` for pagination:

```
00 00 FF 08    // offset=0, filter=custom, max=8 (pagination)
```

This returns up to 8 plants per read, requiring multiple read operations.

#### Delete Request (2 bytes)

```c
typedef struct __attribute__((packed)) {
    uint16_t plant_id;          // Plant ID to delete
} bt_pack_plant_delete_t;
```

**Example (hex):**
```
E9 03          // Delete plant 1001 (0x03E9)
```

#### Install Request (156 bytes)

Write a full `pack_plant_v1_t` structure. See [PACK_SCHEMA.md](PACK_SCHEMA.md) for structure details.

### Read Response (Legacy Pagination)

> **Note**: For new implementations, use streaming mode instead.

Returns plant list with pagination:

```c
typedef struct __attribute__((packed)) {
    uint16_t total_count;           // Total plants available
    uint8_t returned_count;         // Entries in this response (0-10)
    uint8_t flags;                  // 0 for pagination reads
    bt_pack_plant_list_entry_t entries[10];  // Up to 10 entries
} bt_pack_plant_list_resp_t;
```

**Entry structure (22 bytes each):**
```c
typedef struct __attribute__((packed)) {
    uint16_t plant_id;      // Plant ID
    uint16_t pack_id;       // Pack ID (0=built-in)
    uint16_t version;       // Version
    char name[16];          // Truncated name
} bt_pack_plant_list_entry_t;
```

**Maximum response size:** 4 + (10 × 22) = 224 bytes

### Notifications

After install/delete operations, a notification is sent:

```c
typedef struct __attribute__((packed)) {
    uint8_t operation;      // 0=install, 1=delete
    uint8_t result;         // pack_result_t
    uint16_t plant_id;      // Affected plant
    uint16_t version;       // Plant version (install only)
    uint16_t reserved;
} bt_pack_op_result_t;
```

**Size:** 8 bytes

### Usage Examples

#### Install a Plant

```
1. Connect to device
2. Enable notifications on Pack Plant (write 0x0100 to CCC)
3. Write 156-byte pack_plant_v1_t
4. Receive notification with result
```

#### List Plants (Page 1)

```
1. Write: 00 00 08 FF (offset=0, max=8, all packs)
2. Read characteristic
3. Parse bt_pack_plant_list_resp_t
```

#### Delete a Plant

```
1. Enable notifications
2. Write: E9 03 (plant_id=1001)
3. Receive notification: 01 00 E9 03 00 00 00 00
   (operation=delete, result=success, plant_id=1001)
```

---

## Characteristic 2: Pack Stats

**UUID**: `12345678-1234-5678-9abc-def123456787`  
**Properties**: Read  
**Permissions**: Encrypted Read

### Purpose

Retrieve storage usage statistics.

### Read Response

```c
typedef struct __attribute__((packed)) {
    uint32_t total_bytes;       // Total partition size
    uint32_t used_bytes;        // Currently used
    uint32_t free_bytes;        // Available
    uint16_t plant_count;       // Total plants in flash storage (provisioned + custom)
    uint16_t custom_plant_count;// Custom plants only (pack_id != 0) - for sync logic
    uint16_t pack_count;        // Installed packs
    uint16_t builtin_count;     // ROM plants (223, constant)
    uint8_t status;             // 0=ok, 1=not mounted, 2=error
    uint8_t reserved;
    uint32_t change_counter;    // Increments on each install/delete (for cache invalidation)
} bt_pack_stats_resp_t;
```

**Size:** 26 bytes

### Plant Count Fields Explained

| Field | Description | Typical Value |
|-------|-------------|---------------|
| `builtin_count` | ROM plants (constant) | 223 |
| `plant_count` | Total in flash storage | 223 (if provisioned) |
| `custom_plant_count` | Custom only (pack_id != 0) | 0-N |

**For App Sync Logic:**
- Use `custom_plant_count` to decide if streaming is needed
- If `custom_plant_count == 0`, no custom plants to sync
- Built-in plants are always available from ROM

### Cache Invalidation

The `change_counter` field enables efficient caching in mobile apps:

1. **On first connect**: Read Pack Stats, cache `change_counter` value
2. **On reconnect**: Read Pack Stats, compare `change_counter`
   - If same: Skip plant list refresh, use cached data
   - If different: Re-fetch plant list, update cache

**Important:** The counter is **persisted to flash** (`/lfs_ext/packs/counter.bin`), so it survives device reboots. This ensures the cache invalidation remains reliable across power cycles.

### Example Response (hex)

```
00 00 DC 00    // total_bytes = 14,417,920 (0x00DC0000)
00 20 00 00    // used_bytes = 8,192
00 E0 DB 00    // free_bytes = 14,409,728
DF 00          // plant_count = 223 (all provisioned)
05 00          // custom_plant_count = 5 (custom only)
01 00          // pack_count = 1
DF 00          // builtin_count = 223
00             // status = OK
00             // reserved
07 00 00 00    // change_counter = 7 (persisted, survives reboot)
```

---

## Characteristic 3: Pack List

**UUID**: `12345678-1234-5678-9abc-def123456789`  
**Properties**: Read, Write  
**Permissions**: Encrypted Read/Write

### Purpose

List installed packs and retrieve pack contents (which plants are in each pack).

### Write Operations

The write operation selects what to return on the next read:

```c
typedef struct __attribute__((packed)) {
    uint8_t opcode;         // 0x01=list packs, 0x02=get pack content
    uint16_t offset;        // Pagination offset (for list) or pack_id (for content)
    uint8_t reserved;
} bt_pack_list_req_t;
```

**Size:** 4 bytes

| Opcode | Name | offset field meaning |
|--------|------|---------------------|
| 0x01 | LIST_PACKS | Pagination offset |
| 0x02 | GET_CONTENT | Pack ID to query |

### Read Response (after opcode 0x01 - List Packs)

Returns a paginated list of installed packs:

```c
typedef struct __attribute__((packed)) {
    uint16_t total_count;       // Total packs (including builtin)
    uint8_t returned_count;     // Entries in this response
    uint8_t include_builtin;    // 1 if builtin pack 0 is included
    bt_pack_list_entry_t entries[4];  // Up to 4 entries per read
} bt_pack_list_resp_t;
```

**Entry structure (30 bytes each):**
```c
typedef struct __attribute__((packed)) {
    uint16_t pack_id;       // Pack ID (0 = built-in)
    uint16_t version;       // Pack version
    uint16_t plant_count;   // Number of plants in pack
    char name[24];          // Pack name (truncated)
} bt_pack_list_entry_t;
```

**Maximum response size:** 4 + (4 × 30) = 124 bytes

**Note:** The built-in pack (pack_id=0) is always included first when offset=0. It contains all 223 built-in plant species.

### Read Response (after opcode 0x02 - Get Pack Content)

Returns the plant IDs contained in a specific pack:

```c
typedef struct __attribute__((packed)) {
    uint16_t pack_id;       // Pack ID
    uint16_t version;       // Pack version
    uint16_t total_plants;  // Total plants in pack
    uint8_t returned_count; // Number of plant IDs in this response
    uint8_t offset;         // Current offset (for pagination)
    uint16_t plant_ids[16]; // Up to 16 plant IDs per read
} bt_pack_content_resp_t;
```

**Maximum response size:** 8 + (16 × 2) = 40 bytes

**Note:** For the built-in pack (pack_id=0), `total_plants=223` and `returned_count=0` (too many to enumerate via BLE).

### Usage Examples

#### List All Installed Packs

```
1. Write: 01 00 00 00 (opcode=LIST, offset=0)
2. Read characteristic
3. Parse bt_pack_list_resp_t
4. If total_count > returned_count, repeat with higher offset
```

**Example Response (hex):**
```
02 00          // total_count = 2 (builtin + 1 custom)
02             // returned_count = 2
01             // include_builtin = 1
// Entry 0: Built-in pack
00 00          // pack_id = 0
01 00          // version = 1
DF 00          // plant_count = 223
"Built-in Plants\0\0\0\0\0\0\0\0\0"  // name (24 bytes)
// Entry 1: Custom pack
01 00          // pack_id = 1
02 00          // version = 2
05 00          // plant_count = 5
"Mediterranean Herbs\0\0\0\0\0"      // name (24 bytes)
```

#### Get Pack Contents (Plants in Pack)

```
1. Write: 02 01 00 00 (opcode=CONTENT, pack_id=1)
2. Read characteristic
3. Parse bt_pack_content_resp_t
```

**Example Response (hex):**
```
01 00          // pack_id = 1
02 00          // version = 2
05 00          // total_plants = 5
05             // returned_count = 5
00             // offset = 0
// Plant IDs:
E9 03          // plant_id = 1001
EA 03          // plant_id = 1002
EB 03          // plant_id = 1003
EC 03          // plant_id = 1004
ED 03          // plant_id = 1005
// Remaining 11 slots unused (zeros)
```

---

## Characteristic 4: Pack Transfer

**UUID**: `12345678-1234-5678-9abc-def123456788`  
**Properties**: Read, Write, Notify  
**Permissions**: Encrypted Read/Write

### Purpose

Multi-part transfer for installing large packs with many plants. See [TRANSFER_PROTOCOL.md](TRANSFER_PROTOCOL.md) for full details.

### Write Operations (Opcode-based)

| Opcode | Name | Size | Description |
|--------|------|------|-------------|
| 0x01 | START | 47 | Begin new transfer |
| 0x02 | DATA | 7+N | Send data chunk |
| 0x03 | COMMIT | 1 | Finalize transfer |
| 0x04 | ABORT | 1 | Cancel transfer |
| 0x05 | STATUS | 1 | Query status |

#### START Request (47 bytes)

```c
typedef struct __attribute__((packed)) {
    uint8_t opcode;             // 0x01
    uint16_t pack_id;           // Pack ID
    uint16_t version;           // Pack version
    uint16_t plant_count;       // Number of plants
    uint32_t total_size;        // Total payload bytes
    uint32_t crc32;             // CRC32 of payload
    char name[32];              // Pack name
} bt_pack_xfer_start_t;
```

#### DATA Chunk (7+N bytes)

```
[0x02][offset:4][length:2][data:N]
```

Note: When writing, the opcode is stripped before processing, so the effective header is:
```c
// After opcode stripped:
[offset:4][length:2][data:N]
```

#### COMMIT/ABORT Request (1 byte)

```
[0x03]  // COMMIT
[0x04]  // ABORT
```

### Read Response / Notifications

```c
typedef struct __attribute__((packed)) {
    uint8_t state;              // pack_transfer_state_t
    uint8_t progress_pct;       // 0-100%
    uint16_t pack_id;           // Current pack (0 if idle)
    uint32_t bytes_received;    // Bytes so far
    uint32_t bytes_expected;    // Total expected
    uint8_t last_error;         // pack_result_t
    uint8_t reserved[3];
} bt_pack_xfer_status_t;
```

**Size:** 16 bytes

### Transfer States

```c
typedef enum {
    PACK_XFER_STATE_IDLE = 0,       // No transfer
    PACK_XFER_STATE_RECEIVING = 1,  // Receiving chunks
    PACK_XFER_STATE_COMPLETE = 2,   // Success
    PACK_XFER_STATE_ERROR = 3,      // Failed
} pack_transfer_state_t;
```

---

## Service Definition (C Code)

```c
BT_GATT_SERVICE_DEFINE(pack_svc,
    BT_GATT_PRIMARY_SERVICE(&pack_service_uuid.uuid),
    
    // Pack Plant characteristic
    BT_GATT_CHARACTERISTIC(&pack_plant_uuid.uuid,
                           BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE | BT_GATT_CHRC_NOTIFY,
                           BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT,
                           bt_pack_plant_read, bt_pack_plant_write,
                           &list_response),
    BT_GATT_CCC(pack_plant_ccc_changed, 
                BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT),
    
    // Pack Stats characteristic
    BT_GATT_CHARACTERISTIC(&pack_stats_uuid.uuid,
                           BT_GATT_CHRC_READ,
                           BT_GATT_PERM_READ_ENCRYPT,
                           bt_pack_stats_read, NULL,
                           &stats_response),
    
    // Pack Transfer characteristic
    BT_GATT_CHARACTERISTIC(&pack_xfer_uuid.uuid,
                           BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE | BT_GATT_CHRC_NOTIFY,
                           BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT,
                           bt_pack_xfer_read, bt_pack_xfer_write,
                           &xfer_status),
    BT_GATT_CCC(pack_xfer_ccc_changed,
                BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT)
);
```

### Attribute Indices

| Index | Attribute |
|-------|-----------|
| 0 | Service Declaration |
| 1 | Pack Plant Declaration |
| 2 | Pack Plant Value |
| 3 | Pack Plant CCC |
| 4 | Pack Stats Declaration |
| 5 | Pack Stats Value |
| 6 | Pack Transfer Declaration |
| 7 | Pack Transfer Value |
| 8 | Pack Transfer CCC |

---

## Initialization

```c
int bt_pack_handlers_init(void);
```

Called from `main.c` after Bluetooth is enabled:

```c
// In main.c
bt_enable(NULL);
// ... other BLE init ...
bt_pack_handlers_init();
```

---

## Security

All characteristics require encryption:
- `BT_GATT_PERM_READ_ENCRYPT`
- `BT_GATT_PERM_WRITE_ENCRYPT`

This requires pairing/bonding before access.

---

## MTU Considerations

| MTU | Max Write | Max Read | Notes |
|-----|-----------|----------|-------|
| 23 | 20 bytes | 22 bytes | Default BLE |
| 247 | 244 bytes | 244 bytes | Negotiated |
| 512 | 509 bytes | 509 bytes | Maximum |

**Recommendations:**
- Request MTU of 247+ for pack transfers
- Single-plant install (156 bytes) requires MTU ≥ 159
- Transfer protocol works with any MTU via chunking

---

## Error Handling

### Operation Results

| Code | Name | Description |
|------|------|-------------|
| 0 | SUCCESS | Operation completed |
| 1 | UPDATED | Plant updated to new version |
| 2 | ALREADY_CURRENT | Same version exists |
| 3 | INVALID_DATA | Validation failed |
| 4 | INVALID_VERSION | Unsupported schema |
| 5 | STORAGE_FULL | No space left |
| 6 | IO_ERROR | Filesystem error |
| 7 | NOT_FOUND | Plant/pack not found |
| 8 | CRC_MISMATCH | Data corrupted |

### ATT Errors

| Error | Code | Trigger |
|-------|------|---------|
| Invalid Offset | 0x07 | Non-zero write offset |
| Invalid Attribute Length | 0x0D | Wrong payload size |
| Not Supported | 0x06 | Unknown opcode |

---

## Logging

```c
LOG_MODULE_REGISTER(bt_pack, LOG_LEVEL_DBG);
```

Example output:
```
[bt_pack] Pack plant install: id=1001, pack=1, name=Tomato
[bt_pack] Plant 1001 installed (version 1)
[bt_pack] Pack transfer started: pack_id=1 v1, plants=5, size=780
[bt_pack] Received chunk offset=0, len=240, total=240/780
[bt_pack] Received chunk offset=240, len=240, total=480/780
[bt_pack] Received chunk offset=480, len=240, total=720/780
[bt_pack] Received chunk offset=720, len=60, total=780/780
[bt_pack] CRC32 verified, installing 5 plants...
[bt_pack] Pack transfer complete: installed=5, updated=0, errors=0
```

---

## Public API

### Handlers (called by BLE stack)

```c
// Read handlers
ssize_t bt_pack_plant_read(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                           void *buf, uint16_t len, uint16_t offset);
ssize_t bt_pack_stats_read(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                           void *buf, uint16_t len, uint16_t offset);
ssize_t bt_pack_xfer_read(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                          void *buf, uint16_t len, uint16_t offset);

// Write handlers
ssize_t bt_pack_plant_write(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                            const void *buf, uint16_t len, uint16_t offset, uint8_t flags);
ssize_t bt_pack_xfer_write(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                           const void *buf, uint16_t len, uint16_t offset, uint8_t flags);
```

### Notification

```c
void bt_pack_notify_result(const bt_pack_op_result_t *result);
```

### Transfer Control

```c
pack_transfer_state_t bt_pack_get_transfer_state(void);
void bt_pack_abort_transfer(void);
```

---

## Wire Format Examples

### Install Tomato Plant (Write to Pack Plant)

```hex
E9 03           // plant_id = 1001 (0x03E9)
01 00           // pack_id = 1
01 00           // version = 1
00 00           // reserved

// common_name[48] - "Tomato" padded with zeros
54 6F 6D 61 74 6F 00 00 00 00 00 00 00 00 00 00  // "Tomato\0..."
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  // padding
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  // padding

// scientific_name[64] - "Solanum lycopersicum" padded with zeros  
53 6F 6C 61 6E 75 6D 20 6C 79 63 6F 70 65 72 73  // "Solanum lycoper"
69 63 75 6D 00 00 00 00 00 00 00 00 00 00 00 00  // "sicum\0..."
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  // padding
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  // padding

// Kc coefficients (×1000)
5E 01           // kc_ini_x1000 = 350 (0.35)
BC 02           // kc_dev_x1000 = 700 (0.70)
7B 04           // kc_mid_x1000 = 1147 (1.147)
BC 02           // kc_end_x1000 = 700 (0.70)

// Root depth (mm)
2C 01           // root_depth_min_mm = 300
DC 05           // root_depth_max_mm = 1500

// Growth stages (days)
23              // stage_days_ini = 35
28              // stage_days_dev = 40
50 00           // stage_days_mid = 80
14              // stage_days_end = 20
00              // growth_cycle = 0 (annual)

// Depletion and spacing
90 01           // depletion_fraction_p_x1000 = 400 (0.40)
20 03           // spacing_row_mm = 800
F4 01           // spacing_plant_mm = 500
10 27           // density_x100 = 10000 (100 plants/m²)
E8 03           // canopy_max_x1000 = 1000 (1.0)

// Temperature
F6              // frost_tolerance_c = -10 (signed)
12              // temp_opt_min_c = 18
1C              // temp_opt_max_c = 28

// Irrigation
03              // typ_irrig_method_id = 3 (drip)

// User-adjustable
64 00           // water_need_factor_x100 = 100 (1.0×)
03              // irrigation_freq_days = 3
00              // prefer_area_based = 0 (plant count)
```

**Total: 156 bytes**

### Notification Response

```hex
00              // operation = install
00              // result = SUCCESS
E9 03           // plant_id = 1001
01 00           // version = 1
00 00           // reserved
```

---

## Testing

### Using nRF Connect

1. Scan and connect to "AutoWatering"
2. Bond with device (required for encrypted characteristics)
3. Find service `...def123456800`
4. Enable notifications on Pack Plant
5. Write 156-byte plant data
6. Verify notification received

### Automated Testing

```python
import struct

# Create plant payload (156 bytes total)
plant = struct.pack('<HHHH',
    1001,  # plant_id
    1,     # pack_id  
    1,     # version
    0      # reserved
)
# Names
plant += b'Tomato\x00' + b'\x00' * 41        # common_name[48]
plant += b'Solanum lycopersicum\x00' + b'\x00' * 43  # scientific_name[64]
# Kc coefficients ×1000
plant += struct.pack('<HHHH', 350, 700, 1147, 700)
# Root depth mm
plant += struct.pack('<HH', 300, 1500)
# Growth stages
plant += struct.pack('<BBHBB', 35, 40, 80, 20, 0)
# Depletion and spacing
plant += struct.pack('<HHHHH', 400, 800, 500, 10000, 1000)
# Temperature
plant += struct.pack('<bBB', -10, 18, 28)
# Irrigation method
plant += struct.pack('<B', 3)
# User-adjustable
plant += struct.pack('<HBB', 100, 3, 0)

assert len(plant) == 156
```
