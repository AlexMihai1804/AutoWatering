# Pack Service (UUID: def123456800)

## Overview

The Pack Service manages custom plant databases stored on external flash. It provides four characteristics for plant management, storage statistics, pack listing, and multi-part pack transfers.

**Service UUID**: `12345678-1234-5678-9abc-def123456800`

## Characteristics Summary

| Characteristic | UUID (suffix) | Size | Properties | Purpose |
|----------------|---------------|------|------------|---------|
| **[Pack Plant](#pack-plant-characteristic)** | 6786 | Variable | R/W/N | Install/delete/list/stream plants |
| **[Pack Stats](#pack-stats-characteristic)** | 6787 | 22B | R | Storage statistics |
| **[Pack List](#pack-list-characteristic)** | 6789 | Variable | R/W/N | List packs and pack contents |
| **[Pack Transfer](#pack-transfer-characteristic)** | 6788 | Variable | R/W/N | Multi-part pack installation |

---

## Pack Plant Characteristic

**UUID**: `12345678-1234-5678-9abc-def123456786`

### Operation Summary

| Operation | Payload | Size | Description |
|-----------|---------|------|-------------|
| Write (Install) | `pack_plant_v1_t` | 156B | Install/update a plant |
| Write (Delete) | `bt_pack_plant_delete_t` | 2B | Delete a plant |
| Write (List) | `bt_pack_plant_list_req_t` | 4B | Request plant list (pagination or streaming) |
| Read | `bt_pack_plant_list_resp_t` | 4-224B | Returns plant list (after list request) |
| Notify | `bt_pack_plant_list_resp_t` or `bt_pack_op_result_t` | Variable | Streaming notifications or operation results |

### Plant List Streaming (Recommended)

For large plant databases (223 built-in + custom), use **streaming mode** to avoid BLE timeouts.

#### Request Structure (4 bytes)

```c
typedef struct __packed {
    uint16_t offset;         // Must be 0 for streaming
    uint8_t filter_pack_id;  // Filter (see below)
    uint8_t max_count;       // 0 = streaming mode, >0 = pagination
} bt_pack_plant_list_req_t;
```

#### Filter Values

| Value | Constant | Description |
|-------|----------|-------------|
| `0xFF` | `PACK_FILTER_CUSTOM_ONLY` | Only custom plants (default, app has CSV for built-in) |
| `0xFE` | `PACK_FILTER_ALL` | Built-in (223) + custom plants |
| `0x00` | `PACK_FILTER_BUILTIN_ONLY` | Only built-in plants (Pack 0) |
| `0x01-0xFD` | Specific pack ID | Only plants from that pack |

#### Streaming Response (Notifications)

When `max_count=0`, firmware streams plants via notifications:

```c
typedef struct __packed {
    uint16_t total_count;   // Total plants matching filter
    uint8_t returned_count; // Entries in this notification (0-10)
    uint8_t flags;          // Stream status flags
    bt_pack_plant_list_entry_t entries[10]; // Up to 10 plants
} bt_pack_plant_list_resp_t;
```

**Entry Structure (22 bytes each)**:
```c
typedef struct __packed {
    uint16_t plant_id;  // Plant ID (0-222=built-in, ≥1000=custom)
    uint16_t pack_id;   // Pack ID (0=built-in, ≥1=custom pack)
    uint16_t version;   // Installed version
    char name[16];      // Truncated common name (null-terminated)
} bt_pack_plant_list_entry_t;
```

#### Stream Flags

| Flag | Value | Meaning |
|------|-------|---------|
| `BT_PACK_STREAM_FLAG_STARTING` | `0x80` | First notification of stream |
| `BT_PACK_STREAM_FLAG_NORMAL` | `0x00` | More notifications coming |
| `BT_PACK_STREAM_FLAG_COMPLETE` | `0x01` | Stream finished successfully |
| `BT_PACK_STREAM_FLAG_ERROR` | `0x02` | Stream aborted (error) |

#### Performance

| Metric | Value |
|--------|-------|
| Plants per notification | 10 (220 bytes) |
| Notification interval | 2ms |
| MTU payload | 244 bytes max |
| 223 built-in plants | ~23 notifications, ~50ms |
| Full transfer (223 + custom) | ~400ms typical |

**Comparison**: Old pagination took ~5.6 seconds (28 pages × 200ms timeout each).

### Plant Install (156 bytes)

Write a full `pack_plant_v1_t` structure to install or update a plant:

```c
typedef struct __packed {
    uint16_t plant_id;       // ≥1000 for custom plants
    uint16_t pack_id;        // Pack that owns this plant
    uint16_t version;        // Version number
    uint8_t source;          // PLANT_SOURCE_FAO56_COMPLETE = 0x50
    uint8_t flags;           // 0 = normal
    char common_name[32];    // Plant name
    char scientific_name[48];// Scientific name
    // ... FAO-56 parameters (Kc, root depth, stages, etc.)
} pack_plant_v1_t;           // Total: 156 bytes
```

### Plant Delete (2 bytes)

```c
typedef struct __packed {
    uint16_t plant_id;       // Plant ID to delete
} bt_pack_plant_delete_t;
```

### Operation Result Notification

After install/delete, firmware sends:

```c
typedef struct __packed {
    uint8_t operation;   // 0=install, 1=delete
    uint8_t result;      // pack_result_t value
    uint16_t plant_id;   // Affected plant
    uint16_t version;    // Installed version (for install)
    uint16_t reserved;
} bt_pack_op_result_t;   // 8 bytes
```

---

## Pack Stats Characteristic

**UUID**: `12345678-1234-5678-9abc-def123456787`

Read-only storage statistics.

```c
typedef struct __packed {
    uint32_t total_bytes;    // Total flash capacity
    uint32_t used_bytes;     // Used space
    uint32_t free_bytes;     // Available space
    uint16_t plant_count;    // Custom plants installed
    uint16_t pack_count;     // Packs installed
    uint16_t builtin_count;  // Built-in plants (223)
    uint8_t status;          // 0=OK, 1=not mounted, 2=error
    uint8_t reserved;
    uint32_t change_counter; // Increments on install/delete
} bt_pack_stats_resp_t;      // 22 bytes
```

---

## Pack List Characteristic

**UUID**: `12345678-1234-5678-9abc-def123456789`

### Operations

| Opcode | Operation | Request Size |
|--------|-----------|--------------|
| `0x00` | List all packs | 2B (opcode + reserved) |
| `0x01` | Get pack contents | 4B (opcode + reserved + pack_id) |
| `0x02` | Get pack info | 4B (opcode + reserved + pack_id) |

### Pack 0 (Built-in Database)

Pack 0 is the built-in plant database (223 FAO-56 plants from ROM):

```
Pack ID: 0
Name: "Built-in Database"
Version: 1
Plant Count: 223
Plant IDs: 0-222
```

---

## Pack Transfer Characteristic

**UUID**: `12345678-1234-5678-9abc-def123456788`

Multi-part pack installation for large packs. Uses a state machine for reliable transfers.

### Transfer States

| State | Value | Description |
|-------|-------|-------------|
| `IDLE` | 0 | No transfer in progress |
| `HEADER_RECEIVED` | 1 | Pack header received |
| `RECEIVING_PLANTS` | 2 | Receiving plant data |
| `FINALIZING` | 3 | Validating and committing |
| `COMPLETE` | 4 | Transfer successful |
| `ERROR` | 5 | Transfer failed |

### Transfer Protocol

1. **Start**: Write pack header (metadata)
2. **Data**: Write plant data chunks (up to 512 bytes each)
3. **Finish**: Write finalize command with CRC32
4. **Status**: Read or receive notifications for progress

---

## Plant ID Ranges

| Range | Type | Storage |
|-------|------|---------|
| 0-222 | Built-in plants | ROM (read-only) |
| 1000+ | Custom plants | External flash |

---

## Error Codes (pack_result_t)

| Code | Name | Description |
|------|------|-------------|
| 0 | `PACK_RESULT_SUCCESS` | Operation successful |
| 1 | `PACK_RESULT_NOT_FOUND` | Plant/pack not found |
| 2 | `PACK_RESULT_ALREADY_EXISTS` | Already installed |
| 3 | `PACK_RESULT_ALREADY_CURRENT` | Same version exists |
| 4 | `PACK_RESULT_UPDATED` | Updated to newer version |
| 5 | `PACK_RESULT_STORAGE_FULL` | No space available |
| 6 | `PACK_RESULT_IO_ERROR` | Flash read/write error |
| 7 | `PACK_RESULT_INVALID_DATA` | Data validation failed |
| 8 | `PACK_RESULT_CRC_ERROR` | CRC mismatch |

---

## Implementation Notes

### Android Implementation

```kotlin
// Enable notifications first
gatt.setCharacteristicNotification(packPlantChar, true)
descriptor.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
gatt.writeDescriptor(descriptor)

// Request streaming (all plants including built-in)
val request = byteArrayOf(
    0x00, 0x00,  // offset = 0
    0xFE.toByte(), // filter = ALL (built-in + custom)
    0x00         // max_count = 0 (streaming mode)
)
packPlantChar.value = request
gatt.writeCharacteristic(packPlantChar)

// Handle notifications in callback
override fun onCharacteristicChanged(gatt: BluetoothGatt, char: BluetoothGattCharacteristic) {
    val data = char.value
    val totalCount = data.getShort(0)
    val returnedCount = data[2].toInt() and 0xFF
    val flags = data[3].toInt() and 0xFF
    
    when {
        flags and 0x80 != 0 -> { /* First notification - clear list */ }
        flags == 0x01 -> { /* Complete - all done */ }
        flags == 0x02 -> { /* Error - abort */ }
    }
    
    // Parse entries starting at offset 4
    for (i in 0 until returnedCount) {
        val offset = 4 + i * 22
        val plantId = data.getShort(offset)
        val packId = data.getShort(offset + 2)
        val version = data.getShort(offset + 4)
        val name = String(data, offset + 6, 16).trim('\u0000')
        // Add to plant list
    }
}
```

### iOS Implementation

```swift
// Enable notifications
peripheral.setNotifyValue(true, for: packPlantCharacteristic)

// Request streaming
var request = Data(count: 4)
request[0] = 0x00  // offset low
request[1] = 0x00  // offset high
request[2] = 0xFE  // filter = ALL
request[3] = 0x00  // streaming mode
peripheral.writeValue(request, for: packPlantCharacteristic, type: .withResponse)

// Handle notifications
func peripheral(_ peripheral: CBPeripheral, didUpdateValueFor characteristic: CBCharacteristic, error: Error?) {
    guard let data = characteristic.value else { return }
    
    let totalCount = data.withUnsafeBytes { $0.load(as: UInt16.self) }
    let returnedCount = Int(data[2])
    let flags = data[3]
    
    if flags & 0x80 != 0 { /* First notification */ }
    if flags == 0x01 { /* Complete */ }
    if flags == 0x02 { /* Error */ }
    
    // Parse entries
    for i in 0..<returnedCount {
        let offset = 4 + i * 22
        let plantId = data.subdata(in: offset..<offset+2).withUnsafeBytes { $0.load(as: UInt16.self) }
        let packId = data.subdata(in: offset+2..<offset+4).withUnsafeBytes { $0.load(as: UInt16.self) }
        let version = data.subdata(in: offset+4..<offset+6).withUnsafeBytes { $0.load(as: UInt16.self) }
        let name = String(data: data.subdata(in: offset+6..<offset+22), encoding: .utf8)?.trimmingCharacters(in: .controlCharacters) ?? ""
    }
}
```

### Timing Considerations

- **Notification Interval**: 2ms between notifications
- **Backoff on Buffer Full**: Exponential 10→20→40→80→160→320ms
- **Max Retries**: 6 before aborting with ERROR flag
- **Typical Full Transfer**: ~400ms for 223 built-in plants
- **Connection Interval**: Ensure 7.5-15ms for best performance

### Best Practices

1. **Enable notifications before requesting stream**
2. **Use filter 0xFF (custom only)** if app has built-in CSV
3. **Use filter 0xFE (all)** for full database sync
4. **Check flags in every notification** for stream state
5. **Handle ERROR flag gracefully** - retry after delay
6. **Use change_counter** from Stats to detect database changes
