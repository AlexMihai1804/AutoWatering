# System Configuration Characteristic

**UUID:** `12345678-1234-5678-1234-56789abcdef6`  
**Properties:** Read, Write, Notify  
**Size:** 16 bytes  
**Description:** Global system configuration including power management, flow sensor calibration, and **🚀 master valve setup**

## Overview

The System Configuration characteristic provides access to global system settings that affect all channels and overall operation. It includes power management modes, flow sensor calibration, master valve configuration, and read-only hardware information.

**Power Management:** ✅ Normal, Energy-Saving, and Ultra-Low power modes  
**Flow Calibration:** ✅ Adjustable pulses-per-liter for accurate volume measurement  
**🚀 Master Valve Setup:** ✅ Complete master valve timing and control configuration  
**Hardware Info:** ✅ Read-only valve and channel limits  
**Fragmentation:** ❌ NOT REQUIRED - 16 bytes fit in single BLE packet  
**Rate Limiting:** ✅ 500ms minimum delay between notifications to prevent buffer overflow

## Data Structure

```c
struct system_config_data {
    uint8_t version;                      // Configuration version (read-only)
    uint8_t power_mode;                   // 0=Normal, 1=Energy-Saving, 2=Ultra-Low
    uint32_t flow_calibration;            // Pulses per liter (100-10000)
    uint8_t max_active_valves;            // Always 1 (read-only)
    uint8_t num_channels;                 // Number of channels (read-only)
    // 🚀 Master Valve Configuration (NEW)
    uint8_t master_valve_enabled;         // 0=disabled, 1=enabled
    int16_t master_valve_pre_delay;       // Pre-start delay (signed, seconds)
    int16_t master_valve_post_delay;      // Post-stop delay (signed, seconds)
    uint8_t master_valve_overlap_grace;   // Overlap grace period: 0-255 seconds
    uint8_t master_valve_auto_mgmt;       // 0=manual control, 1=automatic
    uint8_t master_valve_current_state;   // Current state: 0=closed, 1=open (read-only)
} __packed;                              // TOTAL SIZE: 16 bytes
```

## Field Descriptions

| Offset | Size | Field | Access | Description |
|--------|------|-------|--------|-------------|
| 0 | 1 | `version` | Read-only | Configuration version (1) |
| 1 | 1 | `power_mode` | Read/Write | Power management mode (0-2) |
| 2-5 | 4 | `flow_calibration` | Read/Write | Flow sensor pulses/liter |
| 6 | 1 | `max_active_valves` | Read-only | Maximum concurrent valves (1) |
| 7 | 1 | `num_channels` | Read-only | Total available channels (8) |
| 8 | 1 | `master_valve_enabled` | Read/Write | Master valve system enable |
| 9-10 | 2 | `master_valve_pre_delay` | Read/Write | Pre-start delay (signed, seconds) |
| 11-12 | 2 | `master_valve_post_delay` | Read/Write | Post-stop delay (signed, seconds) |
| 13 | 1 | `master_valve_overlap_grace` | Read/Write | Overlap grace period |
| 14 | 1 | `master_valve_auto_mgmt` | Read/Write | Auto-management mode |
| 15 | 1 | `master_valve_current_state` | Read-only | Current master valve state |

### version (byte 0, read-only)
- **Value:** Always `1` (current configuration format)
- **Purpose:** Configuration compatibility checks

### power_mode (byte 1, read/write)
- `0` - **Normal** (full performance, standard battery life)
- `1` - **Energy-Saving** (80% performance, +40% battery life)
- `2` - **Ultra-Low** (60% performance, +100% battery life)

### flow_calibration (bytes 2-5, little-endian, read/write)
- **Unit:** Pulses per liter from flow sensor
- **Range:** 100-10000 pulses/liter
- **Default:** 450 pulses/liter (typical for common flow sensors)
- **Purpose:** Converts pulse count to actual volume delivered

### max_active_valves (byte 6, read-only)
- **Value:** Always `1` (single valve operation)
- **Purpose:** Hardware limitation for valve conflicts prevention

### num_channels (byte 7, read-only)
- **Value:** Always `8` (channels 0-7 available)
- **Purpose:** Total number of available channels

### 🚀 Master Valve Fields (NEW)

### master_valve_enabled (byte 8, read/write)
- `0` - **Disabled** (master valve system off)
- `1` - **Enabled** (master valve system active)
- **Default:** `1` (enabled)

### master_valve_pre_delay (byte 9, signed, read/write)
- **Range:** -127 to +127 seconds
- **Positive values:** Master valve opens BEFORE zone valve
- **Negative values:** Master valve opens AFTER zone valve
- **Default:** `3` (open 3 seconds before zone valve)

### master_valve_post_delay (byte 10, signed, read/write)
- **Range:** -127 to +127 seconds
- **Positive values:** Master valve closes AFTER zone valve
- **Negative values:** Master valve closes BEFORE zone valve
- **Default:** `2` (close 2 seconds after zone valve)

### master_valve_overlap_grace (byte 11, read/write)
- **Range:** 0-255 seconds
- **Purpose:** Grace period for consecutive tasks
- **Behavior:** Keeps master valve open if next task starts within grace period
- **Default:** `5` (5-second grace period)

### master_valve_auto_mgmt (byte 12, read/write)
- `0` - **Manual control** (use Valve Control characteristic for direct control)
- `1` - **Automatic management** (system controls master valve based on zone valves)
- **Default:** `1` (automatic)

### master_valve_current_state (byte 13, read-only)
- `0` - **Closed** (master valve is currently closed)
- `1` - **Open** (master valve is currently open)
- **Updates:** Real-time status, updated automatically

## Power Management Effects

| Mode | Performance | BLE Response | Sensor Polling | Battery Life |
|------|-------------|--------------|----------------|--------------|
| **Normal** | 100% | <50ms | 100ms intervals | Standard |
| **Energy-Saving** | 80% | <200ms | 500ms intervals | +40% |
| **Ultra-Low** | 60% | <1000ms | 2000ms intervals | +100% |

## Operations

### READ - Query System Configuration
Returns complete system configuration including master valve settings.

```javascript
const data = await systemConfigChar.readValue();
const view = new DataView(data.buffer);

// Parse basic configuration
const version = view.getUint8(0);
const powerMode = view.getUint8(1);
const flowCalibration = view.getUint32(2, true); // little-endian
const maxActiveValves = view.getUint8(6);
const numChannels = view.getUint8(7);

// Parse master valve configuration
const masterEnabled = view.getUint8(8);
const masterPreDelay = view.getInt16(9, true); // signed, little-endian
const masterPostDelay = view.getInt16(11, true); // signed, little-endian
const masterGrace = view.getUint8(13);
const masterAutoMgmt = view.getUint8(14);
const masterState = view.getUint8(15);

console.log(`System Configuration v${version}:`);
console.log(`Power Mode: ${powerMode} (${['Normal', 'Energy-Saving', 'Ultra-Low'][powerMode]})`);
console.log(`Flow Calibration: ${flowCalibration} pulses/liter`);
console.log(`Hardware: ${numChannels} channels, ${maxActiveValves} max active valve`);
console.log(`🚀 Master Valve: ${masterEnabled ? 'Enabled' : 'Disabled'}`);
console.log(`   Pre-delay: ${masterPreDelay}s, Post-delay: ${masterPostDelay}s`);
console.log(`   Grace period: ${masterGrace}s, Auto-mgmt: ${masterAutoMgmt ? 'Yes' : 'No'}`);
console.log(`   Current state: ${masterState ? 'Open' : 'Closed'}`);
```

### WRITE - Update Configuration
Modify writable configuration parameters including master valve settings.

```javascript
// Complete system configuration with master valve
const config = new ArrayBuffer(16);
const view = new DataView(config);

// Basic system settings
view.setUint8(0, 1);                    // version (read-only, but include for completeness)
view.setUint8(1, 1);                    // power_mode = Energy-Saving
view.setUint32(2, 750, true);           // flow_calibration = 750 pps/L (little-endian)
view.setUint8(6, 1);                    // max_active_valves (read-only)
view.setUint8(7, 8);                    // num_channels (read-only)

// 🚀 Master valve configuration
view.setUint8(8, 1);                    // master_valve_enabled = true
view.setInt16(9, 5, true);             // master_valve_pre_delay = 5s (signed, little-endian)
view.setInt16(11, 3, true);            // master_valve_post_delay = 3s (signed, little-endian)
view.setUint8(13, 10);                  // master_valve_overlap_grace = 10s
view.setUint8(14, 1);                   // master_valve_auto_mgmt = automatic
view.setUint8(15, 0);                   // master_valve_current_state (read-only)

await systemConfigChar.writeValue(config);
console.log('System configuration with master valve updated');
```

### Master Valve Control Examples

```javascript
// Enable master valve with custom timing
const config = new ArrayBuffer(16);
const view = new DataView(config);

// Read current config first
const currentData = await systemConfigChar.readValue();
const currentView = new DataView(currentData.buffer);

// Copy existing values
for (let i = 0; i < 16; i++) {
    view.setUint8(i, currentView.getUint8(i));
}

// Modify only master valve settings
view.setUint8(8, 1);     // Enable master valve
view.setInt16(9, -2, true);     // Pre-delay: -2s (open 2s AFTER zone valve)
view.setInt16(11, 5, true);     // Post-delay: 5s (close 5s after zone valve)
view.setUint8(13, 8);    // Grace period: 8s
view.setUint8(14, 0);    // Manual control mode

await systemConfigChar.writeValue(config);

// Now you can use Valve Control characteristic for manual master valve control
```
```

### NOTIFY - Configuration Changes
Notifications when system configuration is modified.

```javascript
await systemConfigChar.startNotifications();

systemConfigChar.addEventListener('characteristicvaluechanged', (event) => {
    const data = event.target.value;
    const view = new DataView(data.buffer);
    
    const powerMode = view.getUint8(1);
    const flowCalibration = view.getUint32(2, true);
    
    console.log(`Configuration updated: Power=${powerMode}, Calibration=${flowCalibration}`);
});
```

## Flow Sensor Calibration

### Calibration Process
```javascript
// Calculate calibration from known volume test
async function calibrateFlowSensor() {
    // 1. Start with a known volume (e.g., 1 liter)
    const knownVolume = 1.0; // liters
    
    // 2. Measure pulses during delivery
    const measuredPulses = 750; // example: 750 pulses measured
    
    // 3. Calculate calibration factor
    const pulsesPerLiter = measuredPulses / knownVolume;
    
    // 4. Update system configuration
    const config = await systemConfigChar.readValue();
    const view = new DataView(config.buffer);
    
    view.setUint32(2, Math.round(pulsesPerLiter), true);
    await systemConfigChar.writeValue(config);
    
    console.log(`Flow sensor calibrated: ${pulsesPerLiter} pulses/liter`);
}
```

### Volume Conversion
```javascript
// Convert flow sensor pulses to volume
function pulsesToVolume(pulses, calibration) {
    return pulses / calibration;
}

// Convert volume to expected pulses
function volumeToPulses(volume, calibration) {
    return Math.round(volume * calibration);
}

// Example usage
const calibration = 750; // pulses per liter
const measuredPulses = 1500;
const deliveredVolume = pulsesToVolume(measuredPulses, calibration);
console.log(`Delivered: ${deliveredVolume} liters`);
```

## Power Mode Management

### Dynamic Power Adjustment
```javascript
// Automatically adjust power mode based on battery level
async function adjustPowerMode(batteryPercentage) {
    const config = await systemConfigChar.readValue();
    const view = new DataView(config.buffer);
    
    let newPowerMode;
    if (batteryPercentage > 50) {
        newPowerMode = 0; // Normal
    } else if (batteryPercentage > 20) {
        newPowerMode = 1; // Energy-Saving
    } else {
        newPowerMode = 2; // Ultra-Low
    }
    
    view.setUint8(1, newPowerMode);
    await systemConfigChar.writeValue(config);
    
    const modeNames = ['Normal', 'Energy-Saving', 'Ultra-Low'];
    console.log(`Power mode set to: ${modeNames[newPowerMode]}`);
}
```

## Related Characteristics

- **[Flow Sensor](02-flow-sensor.md)** - Uses flow_calibration for volume conversion
- **[Channel Configuration](04-channel-configuration.md)** - Channel-specific settings
- **[Statistics](08-statistics.md)** - Volume calculations use calibration
- **[Diagnostics](13-diagnostics.md)** - System health monitoring
