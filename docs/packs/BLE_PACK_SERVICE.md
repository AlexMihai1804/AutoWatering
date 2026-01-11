# BLE Pack Service Documentation

**Version**: 1.0.0  
**Files**: `src/bt_pack_handlers.h`, `src/bt_pack_handlers.c`

## Overview

The Pack Service is a dedicated BLE GATT service for managing custom plant packs. It provides three characteristics for plant operations, storage statistics, and multi-part pack transfers.

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
| 4 bytes | List request | `bt_pack_plant_list_req_t` |
| 2 bytes | Delete request | `bt_pack_plant_delete_t` |
| 120 bytes | Install request | `pack_plant_v1_t` |

#### List Request (4 bytes)

```c
typedef struct __attribute__((packed)) {
    uint16_t offset;            // Pagination offset
    uint8_t max_results;        // Max results to return (≤8)
    uint8_t filter_pack_id;     // Filter by pack (0xFF = all)
} bt_pack_plant_list_req_t;
```

**Example (hex):**
```
00 00 08 FF    // offset=0, max=8, filter=all
```

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

#### Install Request (120 bytes)

Write a full `pack_plant_v1_t` structure. See [PACK_SCHEMA.md](PACK_SCHEMA.md) for structure details.

### Read Response

Returns plant list with pagination:

```c
typedef struct __attribute__((packed)) {
    uint16_t total_count;           // Total plants available
    uint8_t returned_count;         // Entries in this response
    uint8_t reserved;
    bt_pack_plant_list_entry_t entries[8];  // Up to 8 entries
} bt_pack_plant_list_resp_t;
```

**Entry structure (20 bytes each):**
```c
typedef struct __attribute__((packed)) {
    uint16_t plant_id;
    uint8_t pack_id;
    uint8_t version;
    char name[16];                  // Truncated name
} bt_pack_plant_list_entry_t;
```

**Maximum response size:** 4 + (8 × 20) = 164 bytes

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
3. Write 120-byte pack_plant_v1_t
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
    uint16_t plant_count;       // Installed custom plants
    uint16_t pack_count;        // Installed packs
    uint16_t builtin_count;     // ROM plants (223)
    uint8_t status;             // 0=ok, 1=not mounted, 2=error
    uint8_t reserved;
} bt_pack_stats_resp_t;
```

**Size:** 20 bytes

### Example Response (hex)

```
00 00 DC 00    // total_bytes = 14,417,920 (0x00DC0000)
00 20 00 00    // used_bytes = 8,192
00 E0 DB 00    // free_bytes = 14,409,728
05 00          // plant_count = 5
01 00          // pack_count = 1
DF 00          // builtin_count = 223
00             // status = OK
00             // reserved
```

---

## Characteristic 3: Pack Transfer

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
- Single-plant install (120 bytes) requires MTU ≥ 123
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
[bt_pack] Pack transfer started: pack_id=1 v1, plants=5, size=600
[bt_pack] Received chunk offset=0, len=240, total=240/600
[bt_pack] Received chunk offset=240, len=240, total=480/600
[bt_pack] Received chunk offset=480, len=120, total=600/600
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
E9 03           // plant_id = 1001
01 00           // pack_id = 1
01 00           // version = 1
02              // source = PLANT_SOURCE_PACK
00              // flags = 0
00 00 00 00     // reserved_id
54 6F 6D 61 74 6F 00 ... (32 bytes) // "Tomato"
53 6F 6C 61 6E 75 6D ... (32 bytes) // "Solanum lycopersicum"
3C 00           // kc_ini = 60 (0.60)
73 00           // kc_mid = 115 (1.15)
50 00           // kc_end = 80 (0.80)
00 00           // kc_flags
23 00           // l_ini_days = 35
28 00           // l_dev_days = 40
28 00           // l_mid_days = 40
14 00           // l_end_days = 20
2C 01           // root_depth_min = 300
DC 05           // root_depth_max = 1500
19 00           // root_growth_rate = 25
00 00           // root_flags
28 00           // depletion_fraction = 40
6E 00           // yield_response = 110
3C 00           // critical_depletion = 60
00 00           // water_flags
0A              // temp_min = 10
23              // temp_max = 35
14              // temp_optimal_low = 20
1B              // temp_optimal_high = 27
32              // humidity_min = 50
50              // humidity_max = 80
1E              // light_min = 30
50              // light_max = 80
00 00 00 00     // reserved
```

**Total: 120 bytes**

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
5. Write 120-byte plant data
6. Verify notification received

### Automated Testing

```python
import struct

# Create plant payload
plant = struct.pack('<HHHBB4x',
    1001,  # plant_id
    1,     # pack_id
    1,     # version
    2,     # source
    0      # flags
)
plant += b'Tomato\x00' + b'\x00' * 25  # common_name (32)
plant += b'Solanum\x00' + b'\x00' * 24  # scientific_name (32)
plant += struct.pack('<HHHH', 60, 115, 80, 0)  # kc values
# ... continue with remaining fields
```
