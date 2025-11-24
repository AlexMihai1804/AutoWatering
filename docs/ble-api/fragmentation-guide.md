# BLE Fragmentation Protocol Guide

This guide documents the current AutoWatering BLE fragmentation mechanisms (verified against `bt_irrigation_service.c` & headers). Legacy references to a hard 20-byte MTU are corrected: the ATT MTU is negotiated (stack may raise it), but application logic still constrains individual write fragments to 20 bytes for maximum client compatibility.

## Overview

The AutoWatering system implements a custom fragmentation protocol to handle BLE characteristics that exceed the standard 20-byte MTU (Maximum Transmission Unit) limit. This protocol ensures reliable transmission of large data structures while maintaining compatibility with standard BLE clients.

### Why Fragmentation is Needed

- Compatibility: Many cross-platform client stacks assume <=20B write chunks
- Larger Structures: Several structs now exceed earlier sizes (e.g., `growing_env_data`, enhanced configs, 78B rain integration status)
- History Payloads: History responses can carry up to 232B payload per notification (after header) for environmental history; rain history uses up to 240B compile-time constant
- Unified Headers: A consistent 8B header wraps all history/auto-calc style notifications simplifying client parsing

## Fragmentation Protocol Specification

### Protocol Header Formats

There are two 4-byte initial fragment headers (writes only), depending on scope:

1) Channel-first header (used by channel-scoped configs like 04-Channel Configuration and 14-Growing Environment)

```
Byte 0: Channel ID (0-7)
Byte 1: Fragment Type
Byte 2: Size Low Byte (LE for type 3, BE high byte for type 2)
Byte 3: Size High Byte (LE for type 3, BE low byte for type 2)
Bytes 4+: Fragment Data
```

2) Reserved-first header (used by some non-channel-scoped writes)

```
Byte 0: Reserved (0x00)
Byte 1: Fragment Type
Byte 2: Size Low Byte (LE for type 3, BE high byte for type 2)
Byte 3: Size High Byte (LE for type 3, BE low byte for type 2)
Bytes 4+: Fragment Data
```

### Fragment Types (Write Path)

| Type | Name | Endianness | Usage |
|------|------|------------|-------|
| **1** | Name Only (LE) | Little-Endian | Channel names only |
| **2** | Full Structure (BE) | Big-Endian | Complete data structures |
| **3** | Full Structure (LE) | Little-Endian | Complete data structures (recommended) |

### Size Encoding

The total data size is encoded in bytes 2-3 of the header:

- **Big-Endian (Type 2)**: `total_size = (data[2] << 8) | data[3]`
- **Little-Endian (Types 1,3)**: `total_size = data[2] | (data[3] << 8)`

## Characteristics Requiring Fragmentation

### Fragmented / Unified Characteristics (Current)

| Characteristic | Write Fragment Header | Fragment Types | Notes |
|----------------|-----------------------|----------------|-------|
| Channel Configuration (76B) | Channel-first | 1 (name),2,3 | Type 1 = name only; 2/3 full struct |
| Growing Environment (~=90B) | Channel-first | 2,3 | Struct grew vs earlier docs; always re-query size |
| History Management | Reserved-first (legacy write still allowed) | 2,3 | Responses use unified 8B header |
| Auto Calc Status (64B) | (writes optional) | 2,3 | Notifications wrapped in unified 8B header + full struct |
| Rain History Control | Reserved-first | 2,3 | All responses unified header + payload slices |
| Environmental History | Reserved-first | 2,3 | Unified header + payload (<=232B) |
| Environmental Data | - | - | Not fragmented (single 28B snapshot) |
| Rain Integration Status (78B) | - | - | Single read (fits MTU after negotiation) |
| Current Task Status (21B) | - | - | No fragmentation; periodic notify |

### Special Cases

#### Channel Configuration (Type 1 - Name Only)
For updating only the channel name without changing other settings:

```
Header: [channel_id] [0x01] [name_length] [0x00]
Data: [UTF-8 channel name bytes]
```

#### Channel Configuration (Types 2/3 - Full Structure)
#### Growing Environment (Read vs Write)
Read path returns the full 71-byte structure using standard GATT long-read; no fragmentation header is present on reads. The write path uses the 4-byte channel-first header with fragment types 2 (BE) or 3 (LE), identical to Channel Configuration full-structure writes.

### Unified 8B Header (Histories & Auto Calc)
Used uniformly by: Irrigation History, Rain History, Environmental History, Auto Calc Status notifications. This replaces earlier ad hoc per-type headers and removes the prior 240B unstructured flood for environmental history.

## Fragmentation State Management

### Per-Characteristic Buffers

Each fragmented write characteristic maintains its own state (example: `channel_frag`, `growing_env_frag`, `history_frag`, etc.):

```c
struct fragmentation_state {
    uint8_t frag_type;        // Fragment type (1, 2, or 3)
    uint16_t expected;        // Total expected size
    uint16_t received;        // Bytes received so far
    uint8_t buf[128];         // Reassembly buffer
    bool in_progress;         // Fragmentation active flag
    uint32_t start_time;      // Timeout tracking
};
```

### Timeout Management

- **Timeout Duration**: 5000ms (5 seconds)
- **Timeout Check**: Performed on each write operation
- **Timeout Action**: Reset fragmentation state and discard partial data
- **Recovery**: Client must restart fragmentation from beginning

##  Implementation Details

### Fragment Processing Rules

1. **Header Detection**: Only processed when `offset == 0`, `len >= 4`, and no fragmentation in progress
2. **Continuation Handling**: All subsequent writes treated as continuation fragments
3. **Size Validation**: Total size must match expected structure size exactly
4. **Buffer Management**: Temporary buffers for reassembly (32-128 bytes per characteristic)
5. **Completion Detection**: When `received >= expected` bytes (exact match required)

### Error Handling

#### Error Conditions
- **Buffer Overflow**: Fragment data exceeds buffer capacity
- **Invalid Header**: Fragment type not 1, 2, or 3
- **Size Mismatch**: Total size doesn't match expected structure size
- **Timeout**: Fragmentation takes longer than 5 seconds
- **Invalid Channel**: Channel ID out of range (for channel-specific characteristics)

#### Error Responses
- **Buffer Overflow**: `BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN)`
- **Invalid Operations**: Treat as standard write (fallback)
- **Timeout**: Silent reset of fragmentation state
- **Invalid Parameters**: `BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED)`

## Client Implementation Examples

### JavaScript/Web Bluetooth

#### Basic Fragmentation Send
```javascript
async function sendFragmentedData(characteristic, data, useLE = true) {
    const fragType = useLE ? 3 : 2;  // 3=little-endian, 2=big-endian
    const totalSize = data.length;
    
    // Create header
    const header = new Uint8Array(4);
    header[0] = 0x00;  // Reserved
    header[1] = fragType;
    
    if (useLE) {
        header[2] = totalSize & 0xFF;        // Size low byte
        header[3] = (totalSize >> 8) & 0xFF; // Size high byte
    } else {
        header[2] = (totalSize >> 8) & 0xFF; // Size high byte
        header[3] = totalSize & 0xFF;        // Size low byte
    }
    
    // Send first fragment with header
    const firstFragmentSize = Math.min(16, data.length); // 20 - 4 header bytes
    const firstFragment = new Uint8Array(4 + firstFragmentSize);
    firstFragment.set(header, 0);
    firstFragment.set(data.slice(0, firstFragmentSize), 4);
    
    await characteristic.writeValue(firstFragment);
    
    // Send remaining fragments
    let offset = firstFragmentSize;
    while (offset < data.length) {
        const fragmentSize = Math.min(20, data.length - offset);
        const fragment = data.slice(offset, offset + fragmentSize);
        await characteristic.writeValue(fragment);
        offset += fragmentSize;
        
        // Small delay between fragments
        await new Promise(resolve => setTimeout(resolve, 10));
    }
}
```

#### Channel Configuration Example
```javascript
async function configureChannel(service, channelId, config) {
    const characteristic = await service.getCharacteristic('12345678-1234-5678-1234-56789abcdef4');
    
    // Create 76-byte channel configuration structure
    const configData = new Uint8Array(76);
    let offset = 0;
    
    // Channel ID
    configData[offset++] = channelId;
    
    // Name length and name (up to 64 bytes)
    const nameBytes = new TextEncoder().encode(config.name);
    configData[offset++] = Math.min(nameBytes.length, 63);
    configData.set(nameBytes.slice(0, 63), offset);
    offset += 64;
    
    // Configuration fields
    configData[offset++] = config.autoEnabled ? 1 : 0;
    configData[offset++] = config.plantType;
    configData[offset++] = config.soilType;
    configData[offset++] = config.irrigationMethod;
    configData[offset++] = config.coverageType;
    
    // Coverage value (union - 4 bytes)
    if (config.coverageType === 0) {
        // Area in m^2 (float)
        const view = new DataView(configData.buffer);
        view.setFloat32(offset, config.area, true); // little-endian
    } else {
        // Plant count (uint16)
        configData[offset] = config.plantCount & 0xFF;
        configData[offset + 1] = (config.plantCount >> 8) & 0xFF;
    }
    offset += 4;
    
    // Sun percentage
    configData[offset++] = config.sunPercentage;
    
    // Send fragmented data
    await sendFragmentedData(characteristic, configData);
}
```

### Python Implementation

```python
import asyncio
from bleak import BleakClient

async def send_fragmented_data(client, char_uuid, data, use_le=True):
    """Send fragmented data to a BLE characteristic"""
    frag_type = 3 if use_le else 2  # 3=little-endian, 2=big-endian
    total_size = len(data)
    
    # Create header
    header = bytearray(4)
    header[0] = 0x00  # Reserved
    header[1] = frag_type
    
    if use_le:
        header[2] = total_size & 0xFF        # Size low byte
        header[3] = (total_size >> 8) & 0xFF # Size high byte
    else:
        header[2] = (total_size >> 8) & 0xFF # Size high byte
        header[3] = total_size & 0xFF        # Size low byte
    
    # Send first fragment with header
    first_fragment_size = min(16, len(data))  # 20 - 4 header bytes
    first_fragment = header + data[:first_fragment_size]
    
    await client.write_gatt_char(char_uuid, first_fragment)
    
    # Send remaining fragments
    offset = first_fragment_size
    while offset < len(data):
        fragment_size = min(20, len(data) - offset)
        fragment = data[offset:offset + fragment_size]
        await client.write_gatt_char(char_uuid, fragment)
        offset += fragment_size
        
        # Small delay between fragments
        await asyncio.sleep(0.01)

async def configure_channel_python(client, channel_id, config):
    """Configure a channel using fragmentation"""
    char_uuid = "12345678-1234-5678-1234-56789abcdef4"
    
    # Create 76-byte configuration structure
    config_data = bytearray(76)
    offset = 0
    
    # Channel ID
    config_data[offset] = channel_id
    offset += 1
    
    # Name length and name
    name_bytes = config['name'].encode('utf-8')[:63]
    config_data[offset] = len(name_bytes)
    offset += 1
    config_data[offset:offset+64] = name_bytes.ljust(64, b'\x00')
    offset += 64
    
    # Configuration fields
    config_data[offset] = 1 if config['auto_enabled'] else 0
    config_data[offset + 1] = config['plant_type']
    config_data[offset + 2] = config['soil_type']
    config_data[offset + 3] = config['irrigation_method']
    config_data[offset + 4] = config['coverage_type']
    offset += 5
    
    # Coverage value (4 bytes)
    if config['coverage_type'] == 0:
        # Area as float (little-endian)
        import struct
        area_bytes = struct.pack('<f', config['area'])
        config_data[offset:offset+4] = area_bytes
    else:
        # Plant count as uint16 (little-endian)
        plant_count = config['plant_count']
        config_data[offset] = plant_count & 0xFF
        config_data[offset + 1] = (plant_count >> 8) & 0xFF
    offset += 4
    
    # Sun percentage
    config_data[offset] = config['sun_percentage']
    
    # Send fragmented data
    await send_fragmented_data(client, char_uuid, config_data)
```

### Node.js Implementation

```javascript
const noble = require('@abandonware/noble');

async function sendFragmentedData(characteristic, data, useLE = true) {
    return new Promise((resolve, reject) => {
        const fragType = useLE ? 3 : 2;
        const totalSize = data.length;
        
        // Create header
        const header = Buffer.alloc(4);
        header[0] = 0x00;  // Reserved
        header[1] = fragType;
        
        if (useLE) {
            header.writeUInt16LE(totalSize, 2);
        } else {
            header.writeUInt16BE(totalSize, 2);
        }
        
        let fragments = [];
        
        // First fragment with header
        const firstFragmentSize = Math.min(16, data.length);
        const firstFragment = Buffer.concat([header, data.slice(0, firstFragmentSize)]);
        fragments.push(firstFragment);
        
        // Remaining fragments
        let offset = firstFragmentSize;
        while (offset < data.length) {
            const fragmentSize = Math.min(20, data.length - offset);
            const fragment = data.slice(offset, offset + fragmentSize);
            fragments.push(fragment);
            offset += fragmentSize;
        }
        
        // Send fragments sequentially
        let fragmentIndex = 0;
        
        function sendNextFragment() {
            if (fragmentIndex >= fragments.length) {
                resolve();
                return;
            }
            
            characteristic.write(fragments[fragmentIndex], false, (error) => {
                if (error) {
                    reject(error);
                    return;
                }
                
                fragmentIndex++;
                setTimeout(sendNextFragment, 10); // 10ms delay between fragments
            });
        }
        
        sendNextFragment();
    });
}
```

##  Debugging and Troubleshooting

### Common Issues

#### Fragmentation Timeout
**Symptoms**: Fragmentation resets after 5 seconds
**Causes**: 
- Network delays between fragments
- Client sending fragments too slowly
- System busy processing other operations

**Solutions**:
- Reduce delay between fragments (recommended: 10-50ms)
- Implement retry logic for failed fragments
- Check BLE connection stability

#### Invalid Fragment Type
**Symptoms**: Fragmentation not starting, treated as regular write
**Causes**:
- Using fragment type 0 or >3
- Incorrect header format
- Missing reserved byte (should be 0x00)

**Solutions**:
- Use fragment type 3 (little-endian) for best compatibility
- Verify header format: `[0x00][0x03][size_low][size_high]`
- Check endianness of size encoding

#### Size Mismatch
**Symptoms**: Fragmentation rejected with size error
**Causes**:
- Header size doesn't match actual data size
- Structure size changed in firmware
- Incorrect size calculation in client

**Solutions**:
- Verify structure sizes match firmware definitions
- Use exact byte counts for each characteristic
- Check for padding in data structures

### Debug Information

The system provides detailed logging for fragmentation operations:

```
 BLE: Fragmentation header detected - channel=0, frag_type=3, total_size=76
 BLE: Fragmentation initialized - cid=0, type=3, expected=76 bytes
 BLE: Fragment received: 32/76 bytes
 BLE: Fragment received: 76/76 bytes - COMPLETE
```

### Testing Fragmentation

#### Test Fragment Header
```javascript
// Test if fragmentation is working
const testHeader = new Uint8Array([0x00, 0x03, 0x4C, 0x00]); // 76 bytes, little-endian
await characteristic.writeValue(testHeader);
// Should see "Fragmentation header detected" in logs
```

#### Verify Buffer State
Monitor system logs for fragmentation state information and timeout warnings.

## Best Practices

### Client Implementation
1. Prefer Type 3 (LE) for new implementations (consistent with most structs)
2. Keep inter-fragment delay modest (5-20 ms usually sufficient)
3. Handle 5s timeout by restarting at header (offset writes are not used)
4. Query actual struct sizes when firmware may evolve (avoid hardcoding > doc)
5. Treat -EBUSY / throttling as back-pressure, apply retry with exponential backoff

### Performance Optimization
1. Minimize fragments; group logically related field changes
2. Avoid interleaving different fragmented writes concurrently
3. Monitor negotiated MTU (could allow future optimization) even if writes stay <=20B
4. Use notifications (unified header) to stream larger result sets progressively

### Testing and Validation
1. Test boundary sizes (exact struct size, name-only, maximal history payload)
2. Simulate mid-sequence drop to confirm timeout reset
3. Validate unified header parsing across all history types
4. Stress sustained history streaming (observe adaptive throttling interactions)

## Related Documentation

- **[BLE API Overview](README.md)** - Complete BLE interface documentation
- **[Channel Configuration](characteristics/04-channel-configuration.md)** - Detailed channel config usage
- **[Growing Environment](characteristics/14-growing-environment.md)** - Plant configuration fragmentation
- **[Current Task Status](characteristics/16-current-task-status.md)** - Real-time task monitoring
- **[History Management](characteristics/12-history-management.md)** - Historical data access

##  Support

If you encounter issues with fragmentation:

1. **Check Logs**: Enable BLE debugging to see fragmentation state
2. **Verify Sizes**: Ensure data structures match firmware definitions
3. **Test Simple Cases**: Start with small fragments before complex data
4. **Review Examples**: Use provided code examples as reference
5. **Report Issues**: Submit detailed logs and code samples for support

Verified: Header formats, fragment states, unified history header adoption, removal of legacy environmental history bulk format, adaptive notifier unaffected by fragmentation path.