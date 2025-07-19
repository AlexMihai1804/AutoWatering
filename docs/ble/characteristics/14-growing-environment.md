# Growing Environment Characteristic

**UUID:** `12345678-1234-5678-1234-56789abcdefe`  
**Properties:** Read, Write, Notify  
**Size:** 50 bytes  
**Description:** Advanced plant-specific environment configuration with comprehensive growing parameters

## Overview

The Growing Environment characteristic provides detailed plant-specific configuration for intelligent irrigation control. It includes plant type selection, soil classification, irrigation method, coverage measurement, environmental factors, and custom plant support.

**Plant Database:** ✅ 26+ plant types with specific varieties  
**Soil Classification:** ✅ 8 soil types for optimal watering  
**Irrigation Methods:** ✅ 6 methods for different applications  
**Coverage Options:** ✅ Area-based or plant-count measurement  
**Environmental Factors:** ✅ Sun exposure and water requirements  
**Fragmentation:** ✅ REQUIRED - 50 bytes requires fragmentation  
**Channel Selection:** ✅ 1-byte write to select channel for subsequent reads  
**Rate Limiting:** ✅ 500ms minimum delay between notifications to prevent buffer overflow

## Data Structure

```c
struct growing_env_data {
    uint8_t channel_id;           // Channel ID (0-7)
    uint8_t plant_type;           // Plant type (0-7, see categories below)
    uint16_t specific_plant;      // Specific plant variety ID (little-endian)
    uint8_t soil_type;            // Soil type (0-7)
    uint8_t irrigation_method;    // Irrigation method (0-5)
    uint8_t use_area_based;       // 1=area in m², 0=plant count
    union {
        float area_m2;            // Area in square meters (little-endian)
        uint16_t plant_count;     // Number of plants (little-endian)
    } coverage;                   // 4 bytes
    uint8_t sun_percentage;       // Direct sunlight percentage (0-100%)
    char custom_name[32];         // Custom plant name (null-terminated)
    float water_need_factor;      // Water requirement multiplier (0.1-5.0, little-endian)
    uint8_t irrigation_freq_days; // Recommended frequency in days
    uint8_t prefer_area_based;    // 1=prefers m² measurement, 0=prefers plant count
} __packed;                       // TOTAL SIZE: 50 bytes
```

## Field Descriptions

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0 | 1 | `channel_id` | Channel identifier (0-7) |
| 1 | 1 | `plant_type` | Plant category (0-7) |
| 2-3 | 2 | `specific_plant` | Plant variety ID (little-endian) |
| 4 | 1 | `soil_type` | Soil classification (0-7) |
| 5 | 1 | `irrigation_method` | Delivery method (0-5) |
| 6 | 1 | `use_area_based` | Coverage type (0/1) |
| 7-10 | 4 | `coverage` | Area (float) or count (uint16_t) |
| 11 | 1 | `sun_percentage` | Sun exposure (0-100%) |
| 12-43 | 32 | `custom_name` | Custom plant name |
| 44-47 | 4 | `water_need_factor` | Water multiplier (float, little-endian) |
| 48 | 1 | `irrigation_freq_days` | Recommended days between watering |
| 49 | 1 | `prefer_area_based` | Measurement preference (0/1) |

## Plant Categories

### Plant Types (plant_type)
- `0` - **Custom** (user-defined via custom_name)
- `1` - **Vegetables** (tomatoes, lettuce, peppers, etc.)
- `2` - **Herbs** (basil, oregano, thyme, etc.)
- `3` - **Flowers** (roses, petunias, marigolds, etc.)
- `4` - **Shrubs** (hydrangeas, azaleas, boxwood, etc.)
- `5` - **Trees** (fruit trees, ornamentals, etc.)
- `6` - **Lawn** (grass types, ground cover)
- `7` - **Succulents** (cacti, aloe, jade plants, etc.)

### Soil Types (soil_type)
- `0` - **Loamy** (ideal balanced soil)
- `1` - **Clay** (slow drainage, retains water)
- `2` - **Sandy** (fast drainage, frequent watering)
- `3` - **Silty** (medium drainage, good retention)
- `4` - **Peat** (acidic, excellent water retention)
- `5` - **Chalk** (alkaline, free-draining)
- `6` - **Potting Mix** (commercial container soil)
- `7` - **Hydroponic** (soilless growing medium)

### Irrigation Methods (irrigation_method)
- `0` - **Drip** (precise, low-flow, targeted watering)
- `1` - **Sprinkler** (overhead spray coverage)
- `2` - **Soaker** (even ground-level distribution)
- `3` - **Mist** (fine spray for delicate plants)
- `4` - **Flood** (basin flooding for specific applications)
- `5` - **Subsurface** (underground irrigation system)

## Fragmentation

**Status:** ✅ **REQUIRED** - 50 bytes exceeds single packet capacity

### Fragmentation Strategy
```c
// Standard MTU (23 bytes) fragmentation pattern
Fragment 1: bytes 0-19   (20 bytes) - Basic config + coverage
Fragment 2: bytes 20-39  (20 bytes) - Custom name (first 20 chars)
Fragment 3: bytes 40-49  (10 bytes) - Custom name (last 10 chars) + water_need_factor + frequency + prefer_area_based
```

## Channel Selection

**Important:** Before reading growing environment configuration, you must select which channel (0-7) you want to read from.

### SELECT Channel for Read (1-byte write)
```javascript
// Select channel 6 for subsequent reads
const selectChannel = new ArrayBuffer(1);
new DataView(selectChannel).setUint8(0, 6); // Channel 6
await envChar.writeValue(selectChannel);

// Now read the selected channel's environment configuration
const envData = await envChar.readValue();
```

**Behavior:**
- **Write length:** Exactly 1 byte
- **Purpose:** Sets which channel subsequent READ operations will return
- **No persistence:** Selection is temporary and for reading only
- **No notifications:** Selection changes do not trigger notifications
- **Validation:** Channel ID must be 0-7, otherwise returns `BT_ATT_ERR_VALUE_NOT_ALLOWED`

## Operations

### READ - Query Environment Configuration
Retrieve current growing environment settings for a channel.

```javascript
const data = await envChar.readValue();
const view = new DataView(data.buffer);

// Parse basic configuration
const env = {
    channelId: view.getUint8(0),
    plantType: view.getUint8(1),
    specificPlant: view.getUint16(2, true),
    soilType: view.getUint8(4),
    irrigationMethod: view.getUint8(5),
    useAreaBased: view.getUint8(6) === 1,
    sunPercentage: view.getUint8(11),
    waterNeedFactor: view.getFloat32(44, true),
    irrigationFrequency: view.getUint8(48)
};

// Parse coverage based on type
if (env.useAreaBased) {
    env.coverage = view.getFloat32(7, true); // Area in m²
} else {
    env.coverage = view.getUint16(7, true);  // Plant count
}

// Parse custom name
const nameBytes = new Uint8Array(data.buffer, 12, 32);
const nameLength = nameBytes.indexOf(0); // Find null terminator
env.customName = new TextDecoder().decode(nameBytes.slice(0, nameLength));

console.log('Growing Environment:', env);
```

### WRITE - Configure Growing Environment
Set comprehensive plant environment parameters.

```javascript
// Configure vegetable garden
async function configureVegetableGarden() {
    const config = new ArrayBuffer(50);
    const view = new DataView(config);
    
    // Basic configuration
    view.setUint8(0, 2);                    // channel_id = 2
    view.setUint8(1, 1);                    // plant_type = Vegetables
    view.setUint16(2, 5, true);             // specific_plant = Tomato variety
    view.setUint8(4, 0);                    // soil_type = Loamy
    view.setUint8(5, 0);                    // irrigation_method = Drip
    view.setUint8(6, 1);                    // use_area_based = true
    view.setFloat32(7, 15.5, true);         // coverage.area_m2 = 15.5 m²
    view.setUint8(11, 80);                  // sun_percentage = 80%
    
    // Advanced parameters
    view.setFloat32(44, 1.2, true);         // water_need_factor = 1.2x
    view.setUint8(48, 2);                   // irrigation_frequency = every 2 days
    view.setUint8(49, 1);                   // prefer_area_based = true (prefers m²)
    
    // Custom name (optional for predefined types)
    const customName = "Heritage Tomatoes";
    const nameBytes = new TextEncoder().encode(customName);
    for (let i = 0; i < Math.min(nameBytes.length, 31); i++) {
        view.setUint8(12 + i, nameBytes[i]);
    }
    
    await envChar.writeValue(config);
    console.log('Vegetable garden configured');
}

// Configure custom succulent collection
async function configureCustomSucculents() {
    const config = new ArrayBuffer(50);
    const view = new DataView(config);
    
    view.setUint8(0, 5);                    // channel_id = 5
    view.setUint8(1, 0);                    // plant_type = Custom
    view.setUint16(2, 0, true);             // specific_plant = N/A for custom
    view.setUint8(4, 2);                    // soil_type = Sandy
    view.setUint8(5, 0);                    // irrigation_method = Drip
    view.setUint8(6, 0);                    // use_area_based = false (plant count)
    view.setUint16(7, 25, true);            // coverage.plant_count = 25 plants
    view.setUint8(11, 95);                  // sun_percentage = 95%
    
    // Custom succulent settings
    view.setFloat32(44, 0.3, true);         // water_need_factor = 0.3x (low water)
    view.setUint8(48, 14);                  // irrigation_frequency = every 14 days
    view.setUint8(49, 0);                   // prefer_area_based = false (prefers plant count)
    
    // Custom name for succulent varieties
    const customName = "Mixed Desert Succulents";
    const nameBytes = new TextEncoder().encode(customName);
    for (let i = 0; i < Math.min(nameBytes.length, 31); i++) {
        view.setUint8(12 + i, nameBytes[i]);
    }
    
    await envChar.writeValue(config);
    console.log('Custom succulent collection configured');
}
```

### NOTIFY - Environment Updates
Receive notifications when growing environment changes.

```javascript
await envChar.startNotifications();

envChar.addEventListener('characteristicvaluechanged', (event) => {
    const data = event.target.value;
    const view = new DataView(data.buffer);
    
    const channelId = view.getUint8(0);
    const plantType = view.getUint8(1);
    const waterFactor = view.getFloat32(44, true);
    const frequency = view.getUint8(48);
    
    const plantTypes = [
        'Custom', 'Vegetables', 'Herbs', 'Flowers', 
        'Shrubs', 'Trees', 'Lawn', 'Succulents'
    ];
    
    console.log(`🌱 Environment updated for Channel ${channelId}`);
    console.log(`   Plant type: ${plantTypes[plantType]}`);
    console.log(`   Water needs: ${waterFactor}x normal`);
    console.log(`   Frequency: Every ${frequency} days`);
    
    // Update UI with new environment settings
    updateEnvironmentDisplay(channelId, data);
});

function updateEnvironmentDisplay(channelId, envData) {
    const view = new DataView(envData.buffer);
    
    const display = document.getElementById(`channel-${channelId}-env`);
    if (display) {
        const plantType = view.getUint8(1);
        const plantTypes = ['Custom', 'Vegetables', 'Herbs', 'Flowers', 'Shrubs', 'Trees', 'Lawn', 'Succulents'];
        
        display.innerHTML = `
            <div class="plant-type">${plantTypes[plantType]}</div>
            <div class="water-factor">Water: ${view.getFloat32(44, true)}x</div>
            <div class="frequency">Every ${view.getUint8(48)} days</div>
        `;
    }
}
```

## Environment Optimization

### Smart Environment Presets
```javascript
const environmentPresets = {
    vegetableGarden: {
        plantType: 1,       // Vegetables
        soilType: 0,        // Loamy
        irrigationMethod: 0, // Drip
        sunPercentage: 75,
        waterNeedFactor: 1.2,
        irrigationFrequency: 2
    },
    
    herbContainer: {
        plantType: 2,       // Herbs
        soilType: 6,        // Potting Mix
        irrigationMethod: 3, // Mist
        sunPercentage: 60,
        waterNeedFactor: 0.8,
        irrigationFrequency: 3
    },
    
    succulentDisplay: {
        plantType: 7,       // Succulents
        soilType: 2,        // Sandy
        irrigationMethod: 0, // Drip
        sunPercentage: 90,
        waterNeedFactor: 0.3,
        irrigationFrequency: 14
    },
    
    flowerBed: {
        plantType: 3,       // Flowers
        soilType: 0,        // Loamy
        irrigationMethod: 1, // Sprinkler
        sunPercentage: 70,
        waterNeedFactor: 1.0,
        irrigationFrequency: 1
    }
};

async function applyPreset(channelId, presetName, coverage) {
    const preset = environmentPresets[presetName];
    if (!preset) {
        console.error('Unknown preset:', presetName);
        return;
    }
    
    const config = new ArrayBuffer(50);
    const view = new DataView(config);
    
    view.setUint8(0, channelId);
    view.setUint8(1, preset.plantType);
    view.setUint8(4, preset.soilType);
    view.setUint8(5, preset.irrigationMethod);
    view.setUint8(11, preset.sunPercentage);
    view.setFloat32(44, preset.waterNeedFactor, true);
    view.setUint8(48, preset.irrigationFrequency);
    view.setUint8(49, preset.plantType === 1 || preset.plantType === 3 ? 1 : 0); // Vegetables/Flowers prefer area
    
    // Apply coverage
    if (typeof coverage === 'number') {
        if (coverage < 1000) {
            // Assume plant count
            view.setUint8(6, 0);
            view.setUint16(7, coverage, true);
        } else {
            // Assume area in cm², convert to m²
            view.setUint8(6, 1);
            view.setFloat32(7, coverage / 10000, true);
        }
    }
    
    await envChar.writeValue(config);
    console.log(`Applied ${presetName} preset to channel ${channelId}`);
}

// Usage examples
await applyPreset(0, 'vegetableGarden', 25.5);  // 25.5 m²
await applyPreset(1, 'herbContainer', 12);      // 12 plants
await applyPreset(2, 'succulentDisplay', 30);   // 30 plants
```

### Environmental Recommendations
```javascript
function getEnvironmentRecommendations(envData) {
    const view = new DataView(envData.buffer);
    
    const plantType = view.getUint8(1);
    const soilType = view.getUint8(4);
    const irrigationMethod = view.getUint8(5);
    const sunPercentage = view.getUint8(11);
    const waterFactor = view.getFloat32(44, true);
    
    const recommendations = [];
    
    // Sun exposure recommendations
    if (plantType === 1 && sunPercentage < 60) { // Vegetables
        recommendations.push('Vegetables typically need 6+ hours of direct sunlight');
    }
    if (plantType === 7 && sunPercentage < 80) { // Succulents
        recommendations.push('Succulents prefer bright, direct sunlight');
    }
    
    // Soil type recommendations
    if (plantType === 7 && soilType !== 2) { // Succulents not in sandy soil
        recommendations.push('Succulents prefer well-draining sandy soil');
    }
    if (plantType === 2 && soilType === 1) { // Herbs in clay soil
        recommendations.push('Herbs may struggle in heavy clay soil - consider raised beds');
    }
    
    // Irrigation method optimization
    if (plantType === 3 && irrigationMethod === 0) { // Flowers with drip
        recommendations.push('Flowers often benefit from sprinkler irrigation for fuller coverage');
    }
    
    // Water factor validation
    if (plantType === 7 && waterFactor > 0.5) { // Succulents with high water
        recommendations.push('Succulents require minimal water - consider reducing water factor');
    }
    
    return recommendations;
}

// Display recommendations
function showEnvironmentRecommendations(envData) {
    const recommendations = getEnvironmentRecommendations(envData);
    
    if (recommendations.length > 0) {
        console.log('🌿 Environment Recommendations:');
        recommendations.forEach((rec, index) => {
            console.log(`   ${index + 1}. ${rec}`);
        });
    } else {
        console.log('✅ Environment configuration looks optimal!');
    }
}
```

## Related Characteristics

- **[Channel Configuration](04-channel-configuration.md)** - Basic channel settings and naming
- **[Schedule Configuration](05-schedule-configuration.md)** - Automated watering schedules
- **[Plant Database API](../../../src/plant_db_api.c)** - Plant variety database
- **[System Configuration](06-system-configuration.md)** - Global irrigation settings
- **[Statistics](08-statistics.md)** - Environment-based usage analysis
