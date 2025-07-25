# Statistics Characteristic

**UUID:** `12345678-1234-5678-1234-56789abcdef8`  
**Properties:** Read, Write, Notify  
**Size:** 15 bytes  
**Description:** Watering usage statistics and metrics for individual channels

## Overview

The Statistics characteristic provides comprehensive usage tracking for each watering channel, including total volume used, last watering details, and watering count. It supports reading current statistics and resetting channel statistics.

**📊 TIME ZONE BEHAVIOR:**
- **Timestamps:** `last_watering` field contains **UTC timestamps** for internal consistency
- **User Display:** Applications should convert UTC to local time using timezone configuration
- **Storage:** Internal system uses UTC for precise event recording and calculations
- **Conversion:** Use timezone characteristic data to convert to user's local time

**Individual Channel Tracking:** ✅ Per-channel volume, timing, and count metrics  
**Reset Capability:** ✅ Selective reset for maintenance and recalibration  
**Real-time Updates:** ✅ Notifications after each watering event  
**Fragmentation:** ❌ NOT REQUIRED - 15 bytes fit in single BLE packet  
**Channel Selection:** ✅ 1-byte write to select channel for subsequent reads  
**Rate Limiting:** ✅ 500ms minimum delay between notifications to prevent buffer overflow

## Data Structure

```c
struct statistics_data {
    uint8_t channel_id;        // Channel ID (0-7)
    uint32_t total_volume;     // Total volume in milliliters
    uint32_t last_volume;      // Last watering volume in milliliters
    uint32_t last_watering;    // Last watering Unix timestamp
    uint16_t count;            // Total watering operation count
} __packed;                    // TOTAL SIZE: 15 bytes
```

## Field Descriptions

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0 | 1 | `channel_id` | Channel identifier (0-7) |
| 1-4 | 4 | `total_volume` | Cumulative volume in ml (little-endian) |
| 5-8 | 4 | `last_volume` | Last watering volume in ml (little-endian) |
| 9-12 | 4 | `last_watering` | Last watering Unix timestamp (little-endian) |
| 13-14 | 2 | `count` | Total watering count (little-endian) |

### channel_id (byte 0)
- **Range:** 0-7 for valid channels
- **Read:** Returns currently selected channel statistics
- **Write:** 
  - **1-byte write:** Selects channel for reads
  - **Full write:** Channel ID in statistics structure

### total_volume (bytes 1-4, little-endian)
- **Unit:** Milliliters (ml)
- **Range:** 0-4,294,967,295 ml (~4.3 million liters)
- **Purpose:** Cumulative water dispensed by this channel
- **Persistence:** Stored in NVS, survives reboot

### last_volume (bytes 5-8, little-endian)
- **Unit:** Milliliters (ml)
- **Purpose:** Volume from most recent watering operation
- **Usage:** Monitor individual session efficiency

### last_watering (bytes 9-12, little-endian)
- **Format:** Unix timestamp in seconds since epoch (UTC)
- **Time Zone:** UTC for internal consistency and persistence
- **Resolution:** 1-second precision
- **Source:** Generated by `timezone_get_unix_utc()` using RTC
- **Persistence:** Maintains accurate time across system reboots
- **Conversion:** Applications should convert to local time for user display

### count (bytes 13-14, little-endian)
- **Range:** 0-65,535 operations
- **Purpose:** Total number of watering operations
- **Rollover:** Resets to 0 after 65,535

## Channel Selection

**Important:** Before reading statistics, you must select which channel (0-7) you want to read from.

### SELECT Channel for Read (1-byte write)
```javascript
// Select channel 4 for subsequent reads
const selectChannel = new ArrayBuffer(1);
new DataView(selectChannel).setUint8(0, 4); // Channel 4
await statsChar.writeValue(selectChannel);

// Now read the selected channel's statistics
const statsData = await statsChar.readValue();
```

**Behavior:**
- **Write length:** Exactly 1 byte
- **Purpose:** Sets which channel subsequent READ operations will return
- **No persistence:** Selection is temporary and for reading only
- **No notifications:** Selection changes do not trigger notifications
- **Validation:** Channel ID must be 0-7, otherwise returns `BT_ATT_ERR_VALUE_NOT_ALLOWED`

## Operations

### READ - Query Channel Statistics
Select channel and read its statistics.

```javascript
// First, select the channel (1-byte write)
const selectChannel = new ArrayBuffer(1);
new DataView(selectChannel).setUint8(0, 3); // Select channel 3
await statsChar.writeValue(selectChannel);

// Then read the statistics
const statsData = await statsChar.readValue();
const view = new DataView(statsData.buffer);

// Parse statistics
const channelId = view.getUint8(0);
const totalVolume = view.getUint32(1, true);   // little-endian
const lastVolume = view.getUint32(5, true);
const lastWatering = view.getUint32(9, true);
const count = view.getUint16(13, true);

// Convert and display
const totalLiters = totalVolume / 1000;
const lastLiters = lastVolume / 1000;
const lastDate = new Date(lastWatering * 1000);

console.log(`Channel ${channelId} Statistics:`);
console.log(`  Total: ${totalLiters.toFixed(2)} L (${count} operations)`);
console.log(`  Last: ${lastLiters.toFixed(2)} L on ${lastDate.toLocaleString()}`);
```

### WRITE - Reset Channel Statistics
Reset statistics for a specific channel.

```javascript
// Reset statistics for channel 2
const resetStats = new ArrayBuffer(15);
const view = new DataView(resetStats);

view.setUint8(0, 2);              // channel_id = 2
view.setUint32(1, 0, true);       // total_volume = 0
view.setUint32(5, 0, true);       // last_volume = 0
view.setUint32(9, 0, true);       // last_watering = 0
view.setUint16(13, 0, true);      // count = 0

await statsChar.writeValue(resetStats);
console.log('Channel 2 statistics reset');
```

### NOTIFY - Statistics Updates
Notifications when statistics are updated after watering.

```javascript
await statsChar.startNotifications();

statsChar.addEventListener('characteristicvaluechanged', (event) => {
    const data = event.target.value;
    const view = new DataView(data.buffer);
    
    const channelId = view.getUint8(0);
    const lastVolume = view.getUint32(5, true);
    const count = view.getUint16(13, true);
    
    const lastLiters = lastVolume / 1000;
    console.log(`Channel ${channelId}: Watering completed - ${lastLiters.toFixed(2)} L (${count} total)`);
});
```

## Usage Analysis

### Generate Usage Report
```javascript
// Comprehensive usage report for all channels
async function generateUsageReport() {
    console.log('📊 Watering Usage Report');
    console.log('========================');
    
    let systemTotal = 0;
    let systemCount = 0;
    
    for (let channel = 0; channel < 8; channel++) {
        // Select and read channel
        const selectCmd = new ArrayBuffer(1);
        new DataView(selectCmd).setUint8(0, channel);
        await statsChar.writeValue(selectCmd);
        
        const data = await statsChar.readValue();
        const view = new DataView(data.buffer);
        
        const totalVolume = view.getUint32(1, true);
        const lastVolume = view.getUint32(5, true);
        const lastWatering = view.getUint32(9, true);
        const count = view.getUint16(13, true);
        
        if (count > 0) {
            const totalLiters = totalVolume / 1000;
            const lastLiters = lastVolume / 1000;
            const lastDate = new Date(lastWatering * 1000);
            const avgPerOperation = totalLiters / count;
            
            console.log(`Channel ${channel}:`);
            console.log(`  Total: ${totalLiters.toFixed(2)} L (${count} operations)`);
            console.log(`  Average: ${avgPerOperation.toFixed(2)} L per operation`);
            console.log(`  Last: ${lastLiters.toFixed(2)} L on ${lastDate.toLocaleDateString()}`);
            console.log('');
            
            systemTotal += totalVolume;
            systemCount += count;
        }
    }
    
    const systemLiters = systemTotal / 1000;
    console.log(`System Total: ${systemLiters.toFixed(2)} L (${systemCount} operations)`);
}
```

### Efficiency Analysis
```javascript
// Analyze watering efficiency for a channel
async function analyzeEfficiency(channelId, targetVolumeLiters) {
    // Get channel statistics
    const selectCmd = new ArrayBuffer(1);
    new DataView(selectCmd).setUint8(0, channelId);
    await statsChar.writeValue(selectCmd);
    
    const data = await statsChar.readValue();
    const view = new DataView(data.buffer);
    
    const lastVolume = view.getUint32(5, true);
    const count = view.getUint16(13, true);
    
    const actualLiters = lastVolume / 1000;
    const efficiency = (actualLiters / targetVolumeLiters) * 100;
    
    console.log(`📈 Channel ${channelId} Efficiency Analysis:`);
    console.log(`  Target: ${targetVolumeLiters} L`);
    console.log(`  Actual: ${actualLiters.toFixed(2)} L`);
    console.log(`  Efficiency: ${efficiency.toFixed(1)}%`);
    
    if (efficiency < 90) {
        console.log('⚠️ Low efficiency - check for blockages or calibration');
    } else if (efficiency > 110) {
        console.log('⚠️ Over-delivery - check calibration or flow sensor');
    } else {
        console.log('✅ Efficiency within acceptable range');
    }
}
```

### Maintenance Scheduler
```javascript
// Check if channels need maintenance based on usage
async function checkMaintenanceNeeds() {
    console.log('🔧 Maintenance Check');
    
    for (let channel = 0; channel < 8; channel++) {
        // Get statistics
        const selectCmd = new ArrayBuffer(1);
        new DataView(selectCmd).setUint8(0, channel);
        await statsChar.writeValue(selectCmd);
        
        const data = await statsChar.readValue();
        const view = new DataView(data.buffer);
        
        const totalVolume = view.getUint32(1, true);
        const count = view.getUint16(13, true);
        const lastWatering = view.getUint32(9, true);
        
        // Check maintenance criteria
        const totalLiters = totalVolume / 1000;
        const daysSinceLastWatering = (Date.now() / 1000 - lastWatering) / (24 * 3600);
        
        let maintenanceNeeded = false;
        let reasons = [];
        
        if (count > 1000) {
            maintenanceNeeded = true;
            reasons.push('High operation count');
        }
        
        if (totalLiters > 500) {
            maintenanceNeeded = true;
            reasons.push('High volume usage');
        }
        
        if (count > 0 && daysSinceLastWatering > 30) {
            reasons.push('Long time since last use');
        }
        
        if (maintenanceNeeded || reasons.length > 0) {
            console.log(`Channel ${channel}: ${maintenanceNeeded ? '🔴' : '🟡'} ${reasons.join(', ')}`);
        }
    }
}
```

## Related Characteristics

- **[Flow Sensor](02-flow-sensor.md)** - Provides volume measurements for statistics
- **[System Configuration](06-system-configuration.md)** - Flow calibration affects volume accuracy
- **[Channel Configuration](04-channel-configuration.md)** - Channel settings context for statistics
- **[Current Task Status](15-current-task-status.md)** - Real-time progress updates statistics
