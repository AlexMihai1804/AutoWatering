# Valve Control Characteristic

**UUID:** `12345678-1234-5678-1234-56789abcdef1`  
**Properties:** Read, Write, Notify  
**Size:** 4 bytes  
**Description:** Direct valve control and state monitoring

## Overview

The Valve Control characteristic provides direct control over individual irrigation channels and real-time monitoring of valve states. It supports both duration-based and volume-based watering tasks with immediate feedback through notifications.

**Fragmentation:** ❌ NOT REQUIRED - 4 bytes fit in single BLE packet  
**Real-time Updates:** ✅ Notifications when valve state changes  
**Task Creation:** ✅ Duration or volume-based watering tasks  
**Error Handling:** ✅ Input validation with BLE error responses  

## Data Structure

```c
struct valve_control_data {
    uint8_t  channel_id;   // 0-7: target channel
    uint8_t  task_type;    // WRITE: 0=duration [min], 1=volume [L]
                           // READ/NOTIFY: 0=inactive, 1=active
    uint16_t value;        // WRITE: minutes or liters
                           // READ/NOTIFY: always 0
} __packed;               // TOTAL SIZE: 4 bytes
```

## Field Descriptions

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0 | 1 | `channel_id` | Channel selector (0-7) |
| 1 | 1 | `task_type` | Mode/Status indicator |
| 2-3 | 2 | `value` | Task parameter (little-endian) |

### channel_id (byte 0)
- **Range:** 0-7 (8 channels maximum)
- **Write:** Target channel for watering command
- **Read/Notify:** Channel reporting status

### task_type (byte 1)
**Write mode:** Watering task type
- `0` = Duration-based (minutes)
- `1` = Volume-based (liters)

**Read/Notify mode:** Channel status
- `0` = Valve closed (inactive)
- `1` = Valve open (active)

### value (bytes 2-3, little-endian)
**Write mode:** Task parameter
- Duration mode: minutes (1-65535)
- Volume mode: liters (1-65535) ⚠️ **Units are LITERS, not milliliters**

**Read/Notify mode:** Always 0

⚠️ **IMPORTANT:** Value=0 is rejected. Use Task Queue Management to stop tasks.

## 🚀 Master Valve Control (NEW)

The Valve Control characteristic now supports master valve control using a special channel ID.

### Master Valve Commands

| Field | Value | Description |
|-------|-------|-------------|
| `channel_id` | `0xFF` | Special master valve identifier |
| `task_type` | `0` | Close master valve |
| `task_type` | `1` | Open master valve |
| `value` | `0` | Ignored for master valve |

### Requirements
- **Auto-management must be disabled** for manual control
- Configure via System Configuration characteristic first

### Examples

```javascript
// Open master valve manually
const command = new ArrayBuffer(4);
const view = new DataView(command);
view.setUint8(0, 0xFF);     // channel_id = 0xFF (master valve)
view.setUint8(1, 1);        // task_type = 1 (open)
view.setUint16(2, 0, true); // value = 0 (ignored)

await valveChar.writeValue(command);
```

```javascript
// Close master valve manually
const command = new ArrayBuffer(4);
const view = new DataView(command);
view.setUint8(0, 0xFF);     // channel_id = 0xFF (master valve)
view.setUint8(1, 0);        // task_type = 0 (close)
view.setUint16(2, 0, true); // value = 0 (ignored)

await valveChar.writeValue(command);
```

### Master Valve Notifications

Master valve state changes generate automatic notifications:

```javascript
valveChar.addEventListener('characteristicvaluechanged', (event) => {
    const data = event.target.value;
    const view = new DataView(data.buffer);
    
    const channelId = view.getUint8(0);
    const isActive = view.getUint8(1);
    
    if (channelId === 0xFF) {
        console.log(`Master valve is ${isActive ? 'OPEN' : 'CLOSED'}`);
    } else {
        console.log(`Zone ${channelId} is ${isActive ? 'ACTIVE' : 'INACTIVE'}`);
    }
});
```

## Operations

### READ - Query Valve Status
Returns current valve state for any channel.

```javascript
const data = await valveChar.readValue();
const view = new DataView(data);

const channelId = view.getUint8(0);     // Which channel
const isActive = view.getUint8(1);      // 0=inactive, 1=active
const statusValue = view.getUint16(2, true); // Always 0
```

### WRITE - Start Watering Task
Creates and starts a new watering task.

```javascript
// Duration-based: Water channel 2 for 10 minutes
const command = new ArrayBuffer(4);
const view = new DataView(command);
view.setUint8(0, 2);        // channel_id = 2
view.setUint8(1, 0);        // task_type = 0 (duration)
view.setUint16(2, 10, true); // value = 10 minutes

await valveChar.writeValue(command);
```

```javascript
// Volume-based: Water channel 5 for 3 liters
const command = new ArrayBuffer(4);
const view = new DataView(command);
view.setUint8(0, 5);        // channel_id = 5
view.setUint8(1, 1);        // task_type = 1 (volume)
view.setUint16(2, 3, true); // value = 3 liters

await valveChar.writeValue(command);
```

### NOTIFY - Real-time Status Updates
Automatic notifications when valve states change.

```javascript
await valveChar.startNotifications();

valveChar.addEventListener('characteristicvaluechanged', (event) => {
    const data = event.target.value;
    const view = new DataView(data.buffer);
    
    const channelId = view.getUint8(0);
    const isActive = view.getUint8(1);
    
    console.log(`Channel ${channelId} is ${isActive ? 'ACTIVE' : 'INACTIVE'}`);
});
```

## Stopping Tasks

⚠️ **This characteristic does NOT support stopping tasks!**

Use **Task Queue Management** characteristic instead:

```javascript
// Stop current task
const taskQueueChar = await service.getCharacteristic('12345678-1234-5678-1234-56789abcdef7');
const stopCommand = new ArrayBuffer(9);
const view = new DataView(stopCommand);
view.setUint8(6, 2);    // command = 2 (stop current task)

await taskQueueChar.writeValue(stopCommand);
```

## Error Handling

| Error Code | Meaning | Solution |
|------------|---------|----------|
| `BT_ATT_ERR_INVALID_ATTRIBUTE_LEN` | Wrong data size | Send exactly 4 bytes |
| `BT_ATT_ERR_VALUE_NOT_ALLOWED` | Invalid parameter | Check channel_id (0-7), task_type (0-1), value (≥1) |
| `BT_ATT_ERR_UNLIKELY` | Task creation failed | Retry after delay |

## Related Characteristics

- **[Task Queue Management](07-task-queue-management.md)** - Stop tasks and manage queue
- **[Current Task Status](15-current-task-status.md)** - Monitor task progress
- **[Flow Sensor](02-flow-sensor.md)** - Monitor water flow during irrigation
- **[System Status](03-system-status.md)** - Overall system state monitoring
