# Task Queue Management Characteristic

**UUID:** `12345678-1234-5678-1234-56789abcdef7`  
**Properties:** Read, Write, Notify  
**Size:** 9 bytes  
**Description:** Task queue control and status monitoring for watering operations

## Overview

The Task Queue Management characteristic provides comprehensive control over the watering task queue, including status monitoring, task cancellation, queue clearing, and error handling. This is the **primary interface for stopping watering operations**.

**Fragmentation:** ❌ NOT REQUIRED - 9 bytes fit in single BLE packet  
**Real-time Updates:** ✅ Notifications on queue changes  
**Command Interface:** ✅ Immediate command execution with confirmation  
**Critical Operations:** ✅ Emergency stop capability for all watering tasks  
**Rate Limiting:** ✅ 500ms minimum delay between notifications to prevent buffer overflow

## Data Structure

```c
struct task_queue_data {
    uint8_t pending_count;       // Number of pending tasks (0-255)
    uint8_t completed_tasks;     // Completed tasks since boot (0-255)
    uint8_t current_channel;     // Active channel (0-7, 0xFF if none)
    uint8_t current_task_type;   // 0=duration, 1=volume
    uint16_t current_value;      // Task value (minutes/liters)
    uint8_t command;             // Command to execute (write-only)
    uint8_t task_id_to_delete;   // Task ID for deletion
    uint8_t active_task_id;      // Currently active task ID
} __packed;                     // TOTAL SIZE: 9 bytes
```

## Field Descriptions

| Offset | Size | Field | Access | Description |
|--------|------|-------|--------|-------------|
| 0 | 1 | `pending_count` | Read-only | Tasks waiting in queue |
| 1 | 1 | `completed_tasks` | Read-only | Tasks completed since boot |
| 2 | 1 | `current_channel` | Read-only | Active channel (0-7, 0xFF=none) |
| 3 | 1 | `current_task_type` | Read-only | Active task type (0/1) |
| 4-5 | 2 | `current_value` | Read-only | Task target (little-endian) |
| 6 | 1 | `command` | Write-only | Command to execute |
| 7 | 1 | `task_id_to_delete` | Read/Write | Task ID for deletion |
| 8 | 1 | `active_task_id` | Read-only | Running task ID |

### Status Fields (Read-only)

#### pending_count (byte 0)
- **Range:** 0-255 tasks waiting in queue
- **Value 0:** No pending tasks

#### completed_tasks (byte 1)
- **Range:** 0-255 (rolls over after 255)
- **Purpose:** Activity statistics since boot

#### current_channel (byte 2)
- **Range:** 0-7 for active channels, 0xFF for no active task
- **Purpose:** Which channel is currently watering

#### current_task_type (byte 3)
- `0` = Duration-based task (minutes)
- `1` = Volume-based task (liters)

#### current_value (bytes 4-5, little-endian)
- **Units:** Minutes or liters depending on task_type
- **Range:** 0-65535
- **Value 0:** No active task

#### active_task_id (byte 8)
- **Values:** 1 if task active, 0 if none
- **Purpose:** Task execution status

### Command Interface

#### command (byte 6, write-only)
Command codes for task queue operations:

| Command | Name | Action |
|---------|------|--------|
| 0 | **NO_COMMAND** | No action (default state) |
| 1 | **CANCEL_CURRENT** | Stop currently running task |
| 2 | **CLEAR_QUEUE** | Clear all pending tasks |
| 3 | **DELETE_TASK** | Delete specific task by ID |
| 4 | **CLEAR_ERRORS** | Reset error states |

#### task_id_to_delete (byte 7)
- **Purpose:** Specifies task ID for DELETE_TASK command
- **Range:** 1-255 (task IDs), 0 = no task

## Operations

### READ - Query Queue Status
Returns current queue state and active task information.

```javascript
const data = await queueChar.readValue();
const view = new DataView(data.buffer);

// Parse queue status
const pendingCount = view.getUint8(0);
const completedTasks = view.getUint8(1);
const currentChannel = view.getUint8(2);
const currentTaskType = view.getUint8(3);
const currentValue = view.getUint16(4, true); // little-endian
const activeTaskId = view.getUint8(8);

// Display status
if (currentChannel === 0xFF) {
    console.log('No active task');
} else {
    const taskMode = currentTaskType === 0 ? 'duration' : 'volume';
    const unit = currentTaskType === 0 ? 'min' : 'L';
    console.log(`Active: Channel ${currentChannel}, ${currentValue} ${unit} (${taskMode})`);
}

console.log(`Queue: ${pendingCount} pending, ${completedTasks} completed`);
```

### WRITE - Execute Commands
Send commands to control the task queue.

```javascript
// EMERGENCY STOP - Cancel current task
async function emergencyStop() {
    const command = new ArrayBuffer(9);
    const view = new DataView(command);
    
    view.setUint8(6, 1); // command = CANCEL_CURRENT
    
    await queueChar.writeValue(command);
    console.log('🛑 Emergency stop executed');
}

// CLEAR QUEUE - Remove all pending tasks
async function clearQueue() {
    const command = new ArrayBuffer(9);
    const view = new DataView(command);
    
    view.setUint8(6, 2); // command = CLEAR_QUEUE
    
    await queueChar.writeValue(command);
    console.log('🗑️ Queue cleared');
}

// DELETE SPECIFIC TASK
async function deleteTask(taskId) {
    const command = new ArrayBuffer(9);
    const view = new DataView(command);
    
    view.setUint8(6, 3);        // command = DELETE_TASK
    view.setUint8(7, taskId);   // task_id_to_delete
    
    await queueChar.writeValue(command);
    console.log(`🗑️ Task ${taskId} deleted`);
}

// CLEAR ERRORS - Reset error states
async function clearErrors() {
    const command = new ArrayBuffer(9);
    const view = new DataView(command);
    
    view.setUint8(6, 4); // command = CLEAR_ERRORS
    
    await queueChar.writeValue(command);
    console.log('🔄 Error states cleared');
}
```

### NOTIFY - Queue Change Monitoring
Real-time notifications when queue status changes.

```javascript
await queueChar.startNotifications();

queueChar.addEventListener('characteristicvaluechanged', (event) => {
    const data = event.target.value;
    const view = new DataView(data.buffer);
    
    const pendingCount = view.getUint8(0);
    const currentChannel = view.getUint8(2);
    const activeTaskId = view.getUint8(8);
    
    if (activeTaskId === 0) {
        console.log('✅ Task completed');
    } else if (currentChannel !== 0xFF) {
        console.log(`🚿 Task started on channel ${currentChannel}`);
    }
    
    console.log(`Queue status: ${pendingCount} pending tasks`);
});
```

## Common Use Cases

### Emergency Stop All Operations
```javascript
// Complete emergency shutdown
async function emergencyShutdown() {
    // 1. Stop current task
    await emergencyStop();
    
    // 2. Clear all pending tasks
    await clearQueue();
    
    // 3. Clear any error states
    await clearErrors();
    
    console.log('🚨 Complete emergency shutdown executed');
}
```

### Queue Monitoring Dashboard
```javascript
class QueueMonitor {
    constructor(queueCharacteristic) {
        this.queueChar = queueCharacteristic;
        this.setupMonitoring();
    }
    
    async setupMonitoring() {
        await this.queueChar.startNotifications();
        
        this.queueChar.addEventListener('characteristicvaluechanged', (event) => {
            this.updateDisplay(event.target.value);
        });
        
        // Initial status
        const data = await this.queueChar.readValue();
        this.updateDisplay(data);
    }
    
    updateDisplay(data) {
        const view = new DataView(data.buffer);
        
        const status = {
            pending: view.getUint8(0),
            completed: view.getUint8(1),
            currentChannel: view.getUint8(2),
            taskType: view.getUint8(3),
            value: view.getUint16(4, true),
            activeId: view.getUint8(8)
        };
        
        console.log('📊 Queue Status:');
        console.log(`   Pending: ${status.pending}`);
        console.log(`   Completed: ${status.completed}`);
        
        if (status.currentChannel !== 0xFF) {
            const mode = status.taskType === 0 ? 'duration' : 'volume';
            const unit = status.taskType === 0 ? 'min' : 'L';
            console.log(`   Active: Ch${status.currentChannel} - ${status.value} ${unit} (${mode})`);
        } else {
            console.log('   Active: None');
        }
    }
}

// Usage
const monitor = new QueueMonitor(queueChar);
```

### Smart Queue Management
```javascript
// Intelligent queue management based on conditions
async function smartQueueControl() {
    const queueData = await queueChar.readValue();
    const view = new DataView(queueData.buffer);
    
    const pendingCount = view.getUint8(0);
    const currentChannel = view.getUint8(2);
    
    // If too many tasks pending, clear older ones
    if (pendingCount > 5) {
        console.log('⚠️ Queue overloaded, clearing pending tasks');
        await clearQueue();
    }
    
    // Check if stuck (task running too long)
    if (currentChannel !== 0xFF) {
        // Could implement timeout logic here
        console.log(`Task running on channel ${currentChannel}`);
    }
}
```

## Related Characteristics

- **[Valve Control](01-valve-control.md)** - Creates tasks that appear in this queue
- **[Current Task Status](15-current-task-status.md)** - Detailed progress of active task
- **[Schedule Configuration](05-schedule-configuration.md)** - Automatic task creation
- **[System Status](03-system-status.md)** - Overall system state affects queue operation
