# Channel Configuration Characteristic

**UUID:** `12345678-1234-5678-1234-56789abcdef4`  
**Properties:** Read, Write, Notify  
**Size:** 76 bytes  
**Description:** Configure individual channel settings including plant types, growing environment, and irrigation parameters

## Overview

The Channel Configuration characteristic manages detailed settings for each watering channel (0-7). It stores plant information, soil conditions, irrigation methods, coverage area/plant count, and environmental parameters to optimize watering schedules.

**Fragmentation:** ✅ REQUIRED - 76 bytes requires fragmentation  
**Plant Database:** ✅ Comprehensive plant and soil type classifications  
**Growing Environment:** ✅ Coverage, irrigation method, and sun exposure  
**Auto-scheduling:** ✅ Integrates with schedule configuration  
**Channel Selection:** ✅ 1-byte write to select channel for subsequent reads  
**Rate Limiting:** ✅ 500ms minimum delay between notifications to prevent buffer overflow

## Data Structure

```c
struct channel_config_data {
    uint8_t channel_id;              // Channel ID (0-7)
    uint8_t name_len;               // String length (≤63)
    char    name[64];               // Channel name
    uint8_t auto_enabled;           // 1=automatic schedule active, 0=disabled
    uint8_t plant_type;             // Plant type (0-7)
    uint8_t soil_type;              // Soil type (0-7)
    uint8_t irrigation_method;      // Irrigation method (0-5)
    uint8_t coverage_type;          // 0=area in m², 1=plant count
    union {
        float area_m2;              // Area in square meters
        uint16_t plant_count;       // Number of plants
    } coverage;
    uint8_t sun_percentage;         // Sun exposure (0-100%)
} __packed;                         // TOTAL SIZE: 76 bytes
```

## Field Descriptions

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0 | 1 | `channel_id` | Channel identifier (0-7) |
| 1 | 1 | `name_len` | Actual string length (0-63) |
| 2-65 | 64 | `name` | Channel name (null-terminated) |
| 66 | 1 | `auto_enabled` | Automatic scheduling (0/1) |
| 67 | 1 | `plant_type` | Plant classification (0-7) |
| 68 | 1 | `soil_type` | Soil composition (0-7) |
| 69 | 1 | `irrigation_method` | Delivery method (0-5) |
| 70 | 1 | `coverage_type` | Coverage measurement type (0/1) |
| 71-74 | 4 | `coverage` | Area (float) or plant count (uint16_t) |
| 75 | 1 | `sun_percentage` | Sun exposure percentage (0-100) |

### Basic Configuration

#### channel_id (byte 0)
- **Range:** 0-7 for valid channels
- **Read:** Returns currently selected channel configuration
- **Write:** 
  - **1-byte write:** Selects channel for reads (no config change)
  - **Full write:** Channel ID in complete configuration

#### name_len (byte 1)
- **Range:** 0-63 (excluding null terminator)
- **Purpose:** Actual length of channel name string
- **Example:** For "Front Garden", name_len = 12

#### name (bytes 2-65)
- **Format:** Null-terminated UTF-8 string
- **Length:** Fixed 64-byte buffer
- **Examples:** "Front Garden", "Vegetable Patch", "Rose Bushes"
- **Default:** "Default" when not configured

#### auto_enabled (byte 66)
- **Values:**
  - `0` - Manual mode only (schedules disabled)
  - `1` - Automatic scheduling enabled
- **Integration:** Works with Schedule Configuration characteristic

### Plant and Environment Configuration

#### plant_type (byte 67)
- `0` - **Vegetables** (tomatoes, lettuce, peppers, etc.)
- `1` - **Herbs** (basil, oregano, thyme, etc.)
- `2` - **Flowers** (roses, petunias, marigolds, etc.)
- `3` - **Shrubs** (hydrangeas, azaleas, boxwood, etc.)
- `4` - **Trees** (fruit trees, ornamentals, etc.)
- `5` - **Lawn** (grass types, ground cover)
- `6` - **Succulents** (cacti, aloe, jade plants, etc.)
- `7` - **Custom** (user-defined plant requirements)

#### soil_type (byte 68)
- `0` - **Clay** (slow drainage, retains water)
- `1` - **Sandy** (fast drainage, requires frequent watering)
- `2` - **Loamy** (ideal balanced soil)
- `3` - **Silty** (medium drainage, good water retention)
- `4` - **Rocky** (poor water retention, fast drainage)
- `5` - **Peaty** (acidic, excellent water retention)
- `6` - **Potting Mix** (commercial container soil)
- `7` - **Hydroponic** (soilless growing medium)

#### irrigation_method (byte 69)
- `0` - **Drip** (precise, low-flow, targeted watering)
- `1` - **Sprinkler** (overhead spray coverage)
- `2` - **Soaker Hose** (even ground-level distribution)
- `3` - **Micro Spray** (fine mist for delicate plants)
- `4` - **Hand Watering** (manual application)
- `5` - **Flood** (basin flooding for specific applications)

#### coverage_type (byte 70)
- `0` - **Area in m²** (coverage.area_m2 field used)
- `1` - **Plant count** (coverage.plant_count field used)

#### coverage (bytes 71-74, union)
**When coverage_type = 0 (Area):**
- **Field:** `coverage.area_m2` (float, 4 bytes, little-endian)
- **Range:** 0.1 - 1000.0 square meters

**When coverage_type = 1 (Plant Count):**
- **Field:** `coverage.plant_count` (uint16_t, little-endian)
- **Range:** 1 - 65535 individual plants
- **Padding:** 2 bytes unused (bytes 73-74, set to 0)

#### sun_percentage (byte 75)
- **Range:** 0-100% of direct sunlight exposure
- **Examples:**
  - `0-20%` - Deep shade (under trees, north-facing walls)
  - `21-40%` - Partial shade (filtered light, morning sun only)
  - `41-60%` - Partial sun (4-6 hours direct sun)
  - `61-80%` - Full sun with some shade (6-8 hours direct sun)
  - `81-100%` - Full sun (8+ hours direct sunlight)

## Channel Selection

**Important:** Before reading configuration, you must select which channel (0-7) you want to read from.

### SELECT Channel for Read (1-byte write)
```javascript
// Select channel 3 for subsequent reads
const selectChannel = new ArrayBuffer(1);
new DataView(selectChannel).setUint8(0, 3); // Channel 3
await channelConfigChar.writeValue(selectChannel);

// Now read the selected channel's configuration
const configData = await channelConfigChar.readValue();
```

**Behavior:**
- **Write length:** Exactly 1 byte
- **Purpose:** Sets which channel subsequent READ operations will return
- **No persistence:** Selection is temporary and for reading only
- **No notifications:** Selection changes do not trigger notifications
- **Validation:** Channel ID must be 0-7, otherwise returns `BT_ATT_ERR_VALUE_NOT_ALLOWED`

## Fragmentation

**Status:** ✅ **REQUIRED** - 76 bytes exceeds single packet capacity

### Fragmentation Strategy
```c
// Standard MTU (23 bytes) fragmentation pattern
Fragment 1: bytes 0-19   (20 bytes) - channel_id, name_len, name[0-17]
Fragment 2: bytes 20-39  (20 bytes) - name[18-37]
Fragment 3: bytes 40-59  (20 bytes) - name[38-57]
Fragment 4: bytes 60-75  (16 bytes) - name[58-63], auto_enabled, plant_type,
                                      soil_type, irrigation_method, coverage_type,
                                      coverage, sun_percentage
```

### Advanced Fragmentation Protocol
The device supports a smart fragmentation protocol for large configuration updates:

```javascript
// Protocol: [channel_id][frag_type][size_low][size_high][data...]
// frag_type: 1=name only, 2=full config (big-endian), 3=full config (little-endian)

// Example: Update only the name for channel 2
const channelId = 2;
const newName = "Herb Garden";
const nameBytes = new TextEncoder().encode(newName);

const fragmentHeader = new ArrayBuffer(4 + nameBytes.length);
const view = new DataView(fragmentHeader);
view.setUint8(0, channelId);           // Channel 2
view.setUint8(1, 1);                   // Name-only update
view.setUint8(2, nameBytes.length);    // Size (little-endian)
view.setUint8(3, 0);                   // Size high byte

// Copy name data
const headerArray = new Uint8Array(fragmentHeader);
headerArray.set(nameBytes, 4);

await channelConfigChar.writeValue(fragmentHeader);
```

## Operations

### READ - Query Channel Configuration
Select channel and read its configuration.

```javascript
// First, select the channel (1-byte write)
const selectChannel = new ArrayBuffer(1);
new DataView(selectChannel).setUint8(0, 2); // Select channel 2
await channelChar.writeValue(selectChannel);

// Then read the full configuration
const configData = await channelChar.readValue();
const view = new DataView(configData.buffer);

// Parse configuration
const channelId = view.getUint8(0);
const nameLen = view.getUint8(1);
const name = new TextDecoder().decode(configData.slice(2, 2 + nameLen));
const autoEnabled = view.getUint8(66);
const plantType = view.getUint8(67);
const soilType = view.getUint8(68);

console.log(`Channel ${channelId}: "${name}" (Auto: ${autoEnabled ? 'ON' : 'OFF'})`);
```

### WRITE - Configure Channel
Write complete channel configuration.

```javascript
// Configure channel for vegetables
const config = new ArrayBuffer(76);
const view = new DataView(config);

// Basic settings
view.setUint8(0, 3);                    // channel_id = 3
view.setUint8(1, 16);                   // name_len = 16
view.setUint8(66, 1);                   // auto_enabled = true

// Plant configuration
view.setUint8(67, 0);                   // plant_type = Vegetables
view.setUint8(68, 2);                   // soil_type = Loamy
view.setUint8(69, 0);                   // irrigation_method = Drip
view.setUint8(70, 0);                   // coverage_type = Area
view.setFloat32(71, 5.5, true);        // coverage.area_m2 = 5.5 m²
view.setUint8(75, 80);                  // sun_percentage = 80%

// Set name
const nameBytes = new TextEncoder().encode("Vegetable Garden");
for (let i = 0; i < nameBytes.length; i++) {
    view.setUint8(2 + i, nameBytes[i]);
}

await channelChar.writeValue(config);
console.log('Channel 3 configured for vegetables');
```

### NOTIFY - Configuration Changes
Notifications when channel configuration is modified.

```javascript
await channelChar.startNotifications();

channelChar.addEventListener('characteristicvaluechanged', (event) => {
    const data = event.target.value;
    const view = new DataView(data.buffer);
    
    const channelId = view.getUint8(0);
    const nameLen = view.getUint8(1);
    const name = new TextDecoder().decode(data.slice(2, 2 + nameLen));
    
    console.log(`Channel ${channelId} configuration updated: "${name}"`);
});
```

## Configuration Examples

### Vegetable Garden Setup
```javascript
const vegetableConfig = {
    channel_id: 1,
    name: "Vegetable Garden",
    auto_enabled: true,
    plant_type: 0,          // Vegetables
    soil_type: 2,           // Loamy
    irrigation_method: 0,   // Drip
    coverage_type: 0,       // Area
    area_m2: 12.5,
    sun_percentage: 85
};
```

### Succulent Collection
```javascript
const succulentConfig = {
    channel_id: 4,
    name: "Succulent Collection",
    auto_enabled: true,
    plant_type: 6,          // Succulents
    soil_type: 1,           // Sandy
    irrigation_method: 0,   // Drip
    coverage_type: 1,       // Plant count
    plant_count: 25,
    sun_percentage: 95
};
```

### Indoor Herb Garden
```javascript
const herbConfig = {
    channel_id: 7,
    name: "Indoor Herbs",
    auto_enabled: true,
    plant_type: 1,          // Herbs
    soil_type: 6,           // Potting Mix
    irrigation_method: 3,   // Micro Spray
    coverage_type: 1,       // Plant count
    plant_count: 8,
    sun_percentage: 45      // Partial sun
};
```

## Related Characteristics

- **[Schedule Configuration](05-schedule-configuration.md)** - Automated watering schedules
- **[Valve Control](01-valve-control.md)** - Manual watering control
- **[System Configuration](06-system-configuration.md)** - Global system settings
- **[Statistics](08-statistics.md)** - Per-channel watering history
