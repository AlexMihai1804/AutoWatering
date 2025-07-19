# Flow Sensor Characteristic

**UUID:** `12345678-1234-5678-1234-56789abcdef2`  
**Properties:** Read, Notify  
**Size:** 4 bytes  
**Description:** Real-time flow sensor data with signal processing

## Overview

The Flow Sensor characteristic provides real-time water flow monitoring with advanced signal processing for stable, accurate readings. It measures flow rate in pulses per second with hardware debouncing and smoothing algorithms.

**Fragmentation:** ❌ NOT REQUIRED - 4 bytes fit in single BLE packet  
**Real-time Updates:** ✅ Notifications with automatic rate limiting (500ms minimum delay)  
**Read-only:** ✅ Pure sensor data, no write operations  
**Signal Processing:** ✅ Hardware debouncing + averaging + 500ms rate windows  
**Flow Detection:** ✅ Automatic notification start/stop based on flow presence  
**Buffer Protection:** ✅ Rate limiting prevents BLE buffer overflow during rapid flow changes

## Data Structure

```c
// Simple 4-byte uint32_t value
uint32_t flow_rate;     // Flow rate in pulses/second (little-endian)
```

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0-3 | 4 | `flow_rate` | Flow rate in pulses/second |

### flow_rate (bytes 0-3, little-endian)
- **Units:** Pulses per second from hardware flow sensor
- **Range:** 0 to 4,294,967,295 pps
- **Value 0:** No water flowing
- **Value > 0:** Water flowing, higher = faster flow
- **Typical values:** 1-1000 pps for normal irrigation
- **Processing:** Smoothed over 500ms windows with averaging

## Signal Processing Pipeline

The flow sensor data goes through multiple processing stages:

### 1. Hardware Level
- **Raw pulse detection:** Digital input from flow sensor
- **Hardware debouncing:** 5ms debouncing eliminates noise
- **Pulse counting:** Accumulates pulses over measurement windows
- **Interrupt-driven:** High precision timing

### 2. Signal Processing
- **2-sample circular buffer:** Averages last 2 measurements
- **Smoothing algorithm:** Stable readings from averaging
- **Rate calculation:** Measures over 500ms windows
- **Ultra-low threshold:** Detects flow from 1 pulse

### 3. BLE Interface
- **Stabilized output:** Returns smoothed, stable readings
- **Real-time updates:** Immediate reflection of flow changes
- **Notification management:** Rate limiting prevents overflow

## Operations

### READ - Query Current Flow Rate
Returns immediate reading of smoothed flow rate.

```javascript
const data = await flowChar.readValue();
const view = new DataView(data);
const flowRate = view.getUint32(0, true); // Little-endian

if (flowRate === 0) {
    console.log('💧 No water flowing');
} else {
    console.log(`💧 Water flowing at ${flowRate} pulses/second`);
}
```

### NOTIFY - Real-time Flow Monitoring
Automatic notifications based on flow changes.

```javascript
await flowChar.startNotifications();

flowChar.addEventListener('characteristicvaluechanged', (event) => {
    const data = event.target.value;
    const view = new DataView(data.buffer);
    const flowRate = view.getUint32(0, true);
    
    if (flowRate === 0) {
        console.log('🚫 Flow stopped');
    } else {
        console.log(`🌊 Flow rate: ${flowRate} pps`);
    }
});
```

## Notification System

### Triggers
- **Flow start/stop:** Immediate notification when flow begins/ends
- **Significant changes:** When flow rate changes substantially
- **Periodic updates:** Regular notifications every 200ms for connectivity
- **High-frequency monitoring:** Up to 20Hz during active flow

### Rate Limiting
- **Minimum interval:** 50ms between notifications
- **Adaptive frequency:** Higher rate during active flow
- **Change detection:** Only sends when flow actually changes
- **Connection protection:** Prevents BLE buffer overflow

## Converting to Volume Flow Rates

```javascript
// Convert pulses to meaningful units
async function getFlowCalibration() {
    // Get calibration from System Configuration characteristic
    const configChar = await service.getCharacteristic('12345678-1234-5678-1234-56789abcdef6');
    const configData = await configChar.readValue();
    const view = new DataView(configData.buffer);
    return view.getUint32(2, true); // flow_calibration field
}

async function displayFlowRate() {
    const PULSES_PER_LITER = await getFlowCalibration();
    
    const flowData = await flowChar.readValue();
    const flowRatePps = new DataView(flowData.buffer).getUint32(0, true);
    
    if (flowRatePps === 0) {
        console.log('💧 No flow detected');
        return;
    }
    
    const litersPerSecond = flowRatePps / PULSES_PER_LITER;
    const litersPerMinute = litersPerSecond * 60;
    
    console.log(`Flow: ${flowRatePps} pps = ${litersPerMinute.toFixed(2)} L/min`);
}
```

## Troubleshooting

| Issue | Symptoms | Solution |
|-------|----------|----------|
| **Always reads 0** | No flow detected during watering | Check sensor connections |
| **Erratic readings** | Random high/low values | Already handled by debouncing |
| **Slow response** | Delay in flow detection | Normal - 500ms processing window |
| **No notifications** | Not receiving updates | Enable notifications, check connection |

## Related Characteristics

- **[Valve Control](01-valve-control.md)** - Start watering to generate flow
- **[Current Task Status](15-current-task-status.md)** - Monitor volume progress
- **[Calibration Management](11-calibration-management.md)** - Flow sensor calibration
- **[System Configuration](06-system-configuration.md)** - Flow calibration settings
- **[Statistics](08-statistics.md)** - Historical flow data
