# Channel Configuration Characteristic

> Operation Summary
| Operation | Payload Struct | Size | Fragmentation | Notes |
|-----------|----------------|------|---------------|-------|
| Read | `struct channel_config_data` | 76 B | Optional (single packet if MTU >= 79) | Returns currently selected channel config (selector errors reject the write) |
| Write | `struct channel_config_data` (full) or name-only header | 76 B (full) / 4+N (name) | Custom 4B header Types 1/2/3 | Type1 name-only; Type2 BE full; Type3 LE full (preferred); accepts single unfragmented 76B write |
| Notify | `struct channel_config_data` | 76 B | None (always full struct) | Throttled (~200ms) + 500ms guard after name updates |

Legacy/basic per-channel setup (distinct from enhanced growing environment) containing name, automatic scheduling flag, coarse plant/soil/irrigation enums, coverage and sun exposure.

## Overview

- **UUID**: `12345678-1234-5678-1234-56789abcdef4`
- **Properties**: Read, Write, Notify
- **Permissions**: Read, Write
- **Data Size**: 76 bytes (struct `channel_config_data` - MUST remain exact)
- **Fragmentation**: Optional (Types 1/2/3) - only necessary when negotiated ATT_MTU is too small for a 76B payload (classic 23 -> 20B). Header bytes follow `[channel_id][frag_type][size_lo][size_hi]` with Type 1/3 using little-endian `size` and Type 2 using big-endian.
- **Priority**: Normal adaptive (~200ms baseline). Extra 500ms guard for rapid name changes.

## Data Structure (code verified)

```c
struct channel_config_data {
    uint8_t channel_id;      /* 0-7 */
    uint8_t name_len;        /* 0-63 (excludes null) */
    char    name[64];        /* Raw bytes; may not be null-terminated if name_len==63 */
    uint8_t auto_enabled;    /* 1 = schedule auto mode active */
    uint8_t plant_type;      /* 0-7 */
    uint8_t soil_type;       /* 0-7 */
    uint8_t irrigation_method; /* 0-5 */
    uint8_t coverage_type;   /* 0=area (m^2), 1=plant count */
    union {                  /* 4 bytes */
        float    area_m2;    /* little-endian */
        uint16_t plant_count;/* low 2 bytes used */
    } coverage;
    uint8_t sun_percentage;  /* 0-100 */
} __packed; /* TOTAL 76 bytes */
```

### Byte Layout
```
0   channel_id
1   name_len
2-65  name[64]
66  auto_enabled
67  plant_type
68  soil_type
69  irrigation_method
70  coverage_type
71-74 coverage (float area OR uint16 count + padding)
75  sun_percentage
```

## Fragmentation Protocol (implemented)

Used only when MTU < 79 or long write unsupported.

| Type | Meaning | Endian of size | Usage |
|------|---------|----------------|-------|
| 0x01 | Name only | Little-endian size (practically <=63 so high byte stays 0) | Update name only (1-63B) |
| 0x02 | Full struct | Big-endian size | Compatibility |
| 0x03 | Full struct | Little-endian size | Preferred |

Header: `[channel_id][frag_type][size_lo][size_hi]`. Timeout: 5s inactivity.

Name-Only header:
```
Byte0 channel_id
Byte1 0x01
Byte2 name_len (1-63)
Byte3 0x00
Bytes4+ UTF-8 name
```

Full (0x02/0x03): header + 76 raw bytes; continuation fragments carry raw data only.

Example (0x03 LE, MTU 23):
```
F1 [00 03 4C 00] 16 data
F2 20 data
F3 20 data
F4 20 data
```

## Operations

### Select Channel For Read
Write a single byte `[channel_id]` (0-7). No persistence; sets which channel subsequent READ returns. Invalid IDs respond with `VALUE_NOT_ALLOWED`, and the cached selection stays on its previous value. This helper write never touches flash or emits notifications.

### Read
Returns a fresh 76B struct for the selected channel. If the cached selector drifts out of range (e.g., before first select) the firmware silently falls back to channel 0. Names are truncated to 63 bytes and null-terminated before transmission.

### Write
Methods:
1. Fragmentation (Types 1/2/3).
2. Direct 76B write (single, long, or prepare writes that cover the full 76 bytes).

#### Validation (ATT errors)
| Field | Condition | Error |
|-------|-----------|-------|
| channel_id | >= channel count | VALUE_NOT_ALLOWED |
| plant_type | >7 | VALUE_NOT_ALLOWED |
| soil_type | >7 | VALUE_NOT_ALLOWED |
| irrigation_method | >5 | VALUE_NOT_ALLOWED |
| coverage_type | >1 | VALUE_NOT_ALLOWED |
| sun_percentage | >100 | VALUE_NOT_ALLOWED |
| name_len | >=64 | VALUE_NOT_ALLOWED |
| type 2/3 total size | !=76 | INVALID_ATTRIBUTE_LEN |

On success: fields copied, config saved via `watering_save_config_priority(true)` (250 ms throttle), default-settings flag cleared, and a notification is sent (force-enabled if needed). Name-only path enforces >=500 ms spacing between notifications.

#### Name-Only Update (Type 1)
```javascript
async function updateChannelName(characteristic, channelId, name) {
    const nameBytes = new TextEncoder().encode(name);
    const nameLength = Math.min(nameBytes.length, 63);
    
    // Header: [channel_id][0x01][name_length][0x00]
    const header = new Uint8Array([channelId, 0x01, nameLength, 0x00]);
    
    // Send first fragment with header + name start
    const firstFragmentSize = Math.min(16, nameLength); // 20 - 4 header bytes
    const firstFragment = new Uint8Array(4 + firstFragmentSize);
    firstFragment.set(header, 0);
    firstFragment.set(nameBytes.slice(0, firstFragmentSize), 4);
    
    await characteristic.writeValue(firstFragment);
    
    // Send remaining name data if needed
    let offset = firstFragmentSize;
    while (offset < nameLength) {
        const fragmentSize = Math.min(20, nameLength - offset);
        const fragment = nameBytes.slice(offset, offset + fragmentSize);
        await characteristic.writeValue(fragment);
        offset += fragmentSize;
        
        // Small delay between fragments
        await new Promise(resolve => setTimeout(resolve, 10));
    }
}
```

#### Complete Configuration Update (Type 3 / or 2 BE)
```javascript
async function configureChannel(characteristic, channelId, config) {
    // Create 76-byte configuration structure
    const configData = new Uint8Array(76);
    let offset = 0;
    
    // Channel ID
    configData[offset++] = channelId;
    
    // Name length and name (up to 64 bytes)
    const nameBytes = new TextEncoder().encode(config.name || `Channel ${channelId}`);
    const nameLength = Math.min(nameBytes.length, 63);
    configData[offset++] = nameLength;
    configData.set(nameBytes.slice(0, nameLength), offset);
    offset += 64; // Skip full 64-byte name field
    
    // Configuration fields
    configData[offset++] = config.autoEnabled ? 1 : 0;
    configData[offset++] = config.plantType || 0;
    configData[offset++] = config.soilType || 0;
    configData[offset++] = config.irrigationMethod || 0;
    configData[offset++] = config.coverageType || 0;
    
    // Coverage value (union - 4 bytes)
    if (config.coverageType === 0) {
        // Area in m^2 (float, little-endian)
        const view = new DataView(configData.buffer);
        view.setFloat32(offset, config.area || 1.0, true);
    } else {
        // Plant count (uint16, little-endian)
        const plantCount = config.plantCount || 1;
        configData[offset] = plantCount & 0xFF;
        configData[offset + 1] = (plantCount >> 8) & 0xFF;
        // Bytes 72-73 remain 0 (padding)
    }
    offset += 4;
    
    // Sun percentage
    configData[offset++] = config.sunPercentage || 80;
    
    // Send using fragmentation protocol
    await sendFragmentedData(characteristic, configData, channelId);
}

async function sendFragmentedData(characteristic, data, channelId) {
    // Header: [channel_id][0x03][size_low][size_high]
    const size = data.length;
    const header = new Uint8Array([channelId, 0x03, size & 0xFF, size >> 8]);
    
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

### Notify
Always sends full 76B struct (never name-only). Uses `CHANNEL_CONFIG_NOTIFY` (normal priority ~200 ms) with an extra 500 ms guard for rapid name edits. Firmware force-enables the notification flag after successful writes and falls back to `force_enable_all_notifications()` if the client still rejects the notify.

## Enumerations

Plant types 0-7: Vegetables, Herbs, Flowers, Shrubs, Trees, Lawn, Succulents, Custom.
Soil types 0-7: Clay, Sandy, Loamy, Silty, Rocky, Peaty, Potting Mix, Hydroponic.
Irrigation methods 0-5: Drip, Sprinkler, Soaker Hose, Micro Spray, Hand Watering, Flood.
Coverage: type 0 -> area_m2 (float), type 1 -> plant_count (uint16, remaining bytes ignored).
Sun percentage: 0-100.

## Minimal Client Examples

```javascript
// Select channel 2 for subsequent READ
await char.writeValue(new Uint8Array([2]));

// Name-only update (channel 2 -> "Front Beds")
const name = new TextEncoder().encode("Front Beds");
const nameHeader = new Uint8Array([2,0x01,name.length,0x00]);
const firstNamePayload = Math.min(16, name.length);
const firstNameFragment = new Uint8Array(nameHeader.length + firstNamePayload);
firstNameFragment.set(nameHeader, 0);
firstNameFragment.set(name.slice(0, firstNamePayload), nameHeader.length);
await char.writeValue(firstNameFragment);
let off = firstNamePayload;
while (off < name.length) {
    const frag = name.slice(off, off+20);
    await char.writeValue(frag);
    off += frag.length;
}

// Full struct fragmentation (Type 0x03)
const cfg = new Uint8Array(76);
cfg[0]=2; // channel_id
const nm = new TextEncoder().encode("Front Beds");
cfg[1]=nm.length; cfg.set(nm,2);
cfg[66]=1; // auto_enabled
cfg[67]=0; // plant_type
cfg[68]=2; // soil_type Loamy
cfg[69]=0; // irrigation Drip
cfg[70]=1; // plant count
cfg[71]=6; cfg[72]=0; // plant_count=6
cfg[75]=85;
const cfgHeader = new Uint8Array([2,0x03,cfg.length & 0xFF,cfg.length >> 8]);
const cfgFirstPayload = Math.min(16, cfg.length);
const firstCfgFragment = new Uint8Array(cfgHeader.length + cfgFirstPayload);
firstCfgFragment.set(cfgHeader, 0);
firstCfgFragment.set(cfg.slice(0,cfgFirstPayload), cfgHeader.length);
await char.writeValue(firstCfgFragment);
for (let p=cfgFirstPayload; p<cfg.length; p+=20) {
    await char.writeValue(cfg.slice(p,p+20));
}
```

## [WARN] Notes

Fragmentation only needed for small MTU. Types 1/2/3 accepted; prefer 3 (LE). Timeout 5s. Notifications always send full struct; name spam is rate-limited. All validation errors surface as ATT `VALUE_NOT_ALLOWED` except wrong total size (`INVALID_ATTRIBUTE_LEN`).

## Related
- 05-schedule-configuration.md (timed watering parameters)
- 06-system-configuration.md (system-wide + master valve)
- 14-growing-environment.md (enhanced plant database fields; update both characteristics together when plant/soil indices or coverage data change so the channel stays consistent)

Accurate to current firmware (verified against `bt_irrigation_service.c` and `bt_gatt_structs.h`).
