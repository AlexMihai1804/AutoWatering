# Task Queue Management Characteristic (UUID: def7)

> Operation Summary
| Operation | Payload Struct | Size | Fragmentation | Notes |
|-----------|----------------|------|---------------|-------|
| Read | `struct task_queue_data` | 9 B | None | Snapshot of queue/status (command field zero) |
| Write | `struct task_queue_data` (command significant) | 9 B | None | Full 9B only; partial rejected; only command (and future delete) interpreted |
| Notify | `struct task_queue_data` | 9 B | None | On state change + 5s heartbeat while active; errors use current_task_type=0xFF |

## Overview

The Task Queue Management characteristic provides task scheduling, FIFO queue management, and task control functionality. It serves as the central interface for managing irrigation tasks, monitoring queue status, and controlling task execution.

## Characteristic Details

- **UUID**: `12345678-1234-5678-1234-56789abcdef7`
- **Properties**: Read, Write, Notify
- **Size**: 9 bytes
- **Fragmentation**: Not required
- **Notification Priority**: High (50 ms throttle via `NOTIFY_PRIORITY_HIGH`, plus explicit 5 s periodic updates while active)

## Data Structure

```c
struct task_queue_data {
    uint8_t pending_count;       /* Number of pending tasks in queue */
    uint8_t completed_tasks;     /* Number of completed tasks since boot */
    uint8_t current_channel;     /* Currently active channel (0xFF if none) */
    uint8_t current_task_type;   /* 0=duration, 1=volume */
    uint16_t current_value;      /* Current task value (minutes or liters) */
    uint8_t command;             /* Command to execute (write-only) */
    uint8_t task_id_to_delete;   /* Task ID for deletion (future use) */
    uint8_t active_task_id;      /* Currently active task ID */
} __packed;
```

## Byte Layout

## Task Queue Management Characteristic

This characteristic exposes the live scheduler queue state and accepts control commands using the packed `struct task_queue_data`. The runtime logic lives in `src/bt_irrigation_service.c` (`read_task_queue`, `write_task_queue`, `task_queue_ccc_changed`, `task_queue_periodic_timer_handler`, `bt_irrigation_queue_status_notify`, and `task_queue_send_error`). The packed layout is defined in `src/bt_gatt_structs.h` and enforced by `BUILD_ASSERT(sizeof(struct task_queue_data) == 9)`.

### Characteristic Metadata
| Item | Value |
|------|-------|
| UUID | `12345678-1234-5678-1234-56789abcdef7` |
| Properties | Read, Write, Notify |
| Permissions | Read, Write |
| Payload Size | 9 bytes (packed, little-endian) |
| Fragmentation | Not used (writes must be a single 9-byte frame) |
| Notification Priority | High (`safe_notify` with adaptive 50 ms baseline, stretching toward ~=2 s when buffers back up) |

### Payload Layout
| Offset | Field | Type | Size | Access | Meaning | Source on Read | Effect on Write |
|--------|-------|------|------|--------|---------|----------------|-----------------|
| 0 | `pending_count` | `uint8_t` | 1 | R | Tasks waiting in queue | `watering_get_pending_tasks_count()` | Ignored |
| 1 | `completed_tasks` | `uint8_t` | 1 | R | Total completed tasks since boot | `watering_get_completed_tasks_count()` | Ignored |
| 2 | `current_channel` | `uint8_t` | 1 | R | Channel currently executing (`0xFF` if idle) | `watering_get_current_task()` | Ignored |
| 3 | `current_task_type` | `uint8_t` | 1 | R | 0=duration, 1=volume, 0xFF during error notify | Derived from active task | Ignored |
| 4 | `current_value` | `uint16_t` | 2 | R | Active task's configured duration (minutes) or volume (liters); or error code when `current_task_type==0xFF` | Populated from `watering_event` or `task_queue_send_error()` | Ignored |
| 6 | `command` | `uint8_t` | 1 | W | Command opcode (see below) | Cleared to 0 for read/notify | Decides action when non-zero |
| 7 | `task_id_to_delete` | `uint8_t` | 1 | RW | Reserved for future targeted deletion | Zeroed during read/notify | Currently ignored |
| 8 | `active_task_id` | `uint8_t` | 1 | R | Identifier of active task (present implementation uses 0=none, 1=active) | `watering_get_queue_status()` | Ignored |

### Read Flow
1. `read_task_queue()` validates arguments and casts `task_queue_value` to `struct task_queue_data`.
2. It queries `watering_get_queue_status()` to learn pending count and whether a task is active. Errors collapse pending count to zero.
3. It primes the struct with defaults: current channel `0xFF`, task type 0, value 0, command 0, task_id_to_delete 0, active_task_id 0.
4. If there is an active task (`watering_get_current_task()` succeeds), the handler copies the channel id, picks mode (duration/volume), and copies the configured setpoint from the channel's `watering_event`. Duration mode writes the configured minutes (not the live remaining time).
5. `pending_count` is refreshed again via `watering_get_pending_tasks_count()`; `completed_tasks` is read from `watering_get_completed_tasks_count()`.
6. The filled struct is returned by `bt_gatt_attr_read()`.

### Write Flow & Command Processing
- Writes must be exactly 9 bytes at offset 0. Any other length triggers `BT_ATT_ERR_INVALID_ATTRIBUTE_LEN`; any offset overflow returns `BT_ATT_ERR_INVALID_OFFSET`.
- The payload is memcpy'd into `task_queue_value`. No effect occurs if `command == 0` (no-op).
- When `command != 0`, the handler executes the switch shown below. After successful handling the firmware resets `command` to 0 and, if notifications are enabled, calls `bt_irrigation_queue_status_notify()` to push the updated snapshot.
- On failure (invalid state, no tasks, system busy), `task_queue_send_error(error_code)` fires an error notification with `current_task_type = 0xFF` and `current_value = error_code`, leaving the command field cleared.

| Command | Firmware Action | Success Path | Error Handling |
|---------|-----------------|--------------|----------------|
| 0 | No-op | Ignored | None |
| 1 (`Start next task`) | Validates there are pending tasks, no task active, and `watering_get_status()` reports `WATERING_STATUS_OK`; then calls `watering_process_next_task()` | Sets `command=0`, notifies | `task_queue_send_error(1)` if no pending; `task_queue_send_error(3)` if busy/system not ready |
| 2 (`Pause current task`) | Calls `watering_pause_current_task()` | Clears command, notifies | `task_queue_send_error(2)` when no pausable task |
| 3 (`Resume current task`) | Calls `watering_resume_current_task()` | Clears command, notifies | `task_queue_send_error(2)` when no resumable task |
| 4 (`Cancel current task`) | Calls `watering_stop_current_task()` to abort active work | Clears command, notifies | `task_queue_send_error(2)` when idle |
| 5 (`Clear pending tasks`) | Calls `watering_clear_task_queue()` | Clears command, notifies | `task_queue_send_error(3)` if call fails |
| Any other value | Treated as invalid | - | `task_queue_send_error(2)` |

**Error Notification Encoding (`task_queue_send_error`)**
- Marks `current_task_type = 0xFF` to flag an error response.
- Populates `current_value` with the error code:
    - `1` - start requested but queue empty.
    - `2` - command invalid in current state (pause/resume/cancel without active task, unknown opcode).
    - `3` - system busy/not ready or queue clear failure.
- Leaves `pending_count`, `completed_tasks`, and channel fields reflecting the current system state, allowing the client to adjust.
- `command` and `task_id_to_delete` reset to 0 before the notify is sent.

### Notification Behaviour
- `bt_irrigation_queue_status_notify()` is used after every successful command and whenever internal firmware events call it (e.g., queue updates driven by the watering engine).
- When notifications are enabled, `task_queue_periodic_timer_handler` emits a refresh every 5 seconds while a task remains active.
- `bt_irrigation_queue_status_update(pending_count)` allows other subsystems to trigger a notify when counts change; it simply updates `pending_count` and defers to `bt_irrigation_queue_status_notify()`.
- Notifications carry the same 9-byte payload; before sending, the firmware clears `command` and `task_id_to_delete` to zero to avoid echoing stale commands.

### Client Guidance
- Treat `current_value` as the configured target (not remaining runtime). For live progress monitoring use the Current Task characteristic.
- Keep command writes sparse: issue a command, wait for either success notification or error notification before sending the next control message.
- Monitor `current_task_type == 0xFF` to detect command failures programmatically.
- For `command = 1`, ensure at least one task is pending and the system status is OK to prevent error `3`.
- `task_id_to_delete` is reserved; keep it zero until a future firmware exposes per-task deletion.

### Troubleshooting Matrix
| Symptom | Observed Payload | Interpretation | Action |
|---------|-----------------|----------------|--------|
| Command ignored | `command` remains 0 in next read, no notify | Command value was 0 | Re-send with desired opcode |
| Error code `1` | `current_task_type = 0xFF`, `current_value = 1` | Start requested without pending tasks | Queue additional work before retry |
| Error code `2` | `current_task_type = 0xFF`, `current_value = 2` | Command incompatible with state (pause/resume/cancel while idle, invalid opcode) | Verify an active/paused task exists; check opcode |
| Error code `3` | `current_task_type = 0xFF`, `current_value = 3` | System busy or queue clear failure | Allow system to settle; check watering status |
| No notifications | No BLE packets received after commands | CCC disabled or throttle delaying send | Enable notifications, wait up to ~2 s, or poll read |
| `pending_count` never drops | Tasks not completing | Active task stuck or flow error | Inspect Current Task and diagnostics; consider cancel command |

### Related Characteristics
- `docs/ble-api/characteristics/05-schedule-configuration.md` - automatic schedules that enqueue tasks.
- `docs/ble-api/characteristics/10-valve-control.md` - manual task injection shortcuts.
- `docs/ble-api/characteristics/11-current-task.md` (if present) - real-time progress tracking.
- `docs/ble-api/characteristics/12-statistics.md` - historical completion counts referenced by `completed_tasks`.
**Effects**:
- Valve reopens
- Task timer/volume tracking resumes
- Task continues from where it was paused

#### Cancel Current Task (Command: 4)
Cancels the currently executing task.

**Write Example**:
```c
struct task_queue_data cmd = {
    .command = 4  // Cancel current task
};
```

**Effects**:
- Valve closes immediately
- Task is marked as cancelled
- Next task in queue becomes ready
- Cancelled task is not counted as completed

#### Clear All Pending Tasks (Command: 5)
Removes all pending tasks from the queue.

**Write Example**:
```c
struct task_queue_data cmd = {
    .command = 5  // Clear all pending tasks
};
```

**Effects**:
- All pending tasks removed from queue
- Currently executing task continues
- pending_count becomes 0

## Read Operations

### Queue Status Query
Reading the characteristic returns current queue status.

**Example Response** (2 pending tasks, channel 1 active):
```c
struct task_queue_data status = {
    .pending_count = 2,
    .completed_tasks = 15,
    .current_channel = 1,
    .current_task_type = 0,    // Duration-based
    .current_value = 30,       // 30 minutes
    .command = 0,              // No command
    .task_id_to_delete = 0,    // Not used
    .active_task_id = 42       // Task ID 42
};
```

**Byte Representation**: `02 0F 01 00 1E 00 00 00 2A`

### Empty Queue Status
```c
struct task_queue_data status = {
    .pending_count = 0,
    .completed_tasks = 20,
    .current_channel = 0xFF,   // No active task
    .current_task_type = 0,
    .current_value = 0,
    .command = 0,
    .task_id_to_delete = 0,
    .active_task_id = 0        // No active task
};
```

**Byte Representation**: `00 14 FF 00 00 00 00 00 00`

## Notification Operations

### Queue Status Change Notifications
Emitted after command processing that changes state (start, pause, resume, cancel, clear, task completion, task addition). All notifications carry the full 9-byte structure. Command & task_id_to_delete fields are zeroed before send.

### Periodic Status Updates
Timer-driven every 5 seconds while notifications are enabled (regardless of change) to provide a heartbeat during long tasks.

## Task Priority and Scheduling

### FIFO Queue Management
Tasks are executed in First-In-First-Out order:

1. **Task Addition**: New tasks added to end of queue
2. **Task Execution**: Tasks executed from front of queue
3. **Queue Persistence**: Not currently guaranteed across reset (doc claim removed)
4. **Maximum Queue Size**: Not enforced/documented in code path here (remove fixed 16 claim; client should gracefully handle saturation errors if added later)

### Task Scheduling Algorithm

```c
void process_task_queue(void) {
    // Check if we can start next task
    if (current_channel == 0xFF && pending_count > 0) {
        task_t *next_task = get_next_task_from_queue();
        
        if (next_task && can_start_task(next_task)) {
            start_task_execution(next_task);
            update_queue_status();
            notify_queue_change();
        }
    }
    
    // Check current task completion
    if (current_channel != 0xFF) {
        if (is_current_task_complete()) {
            complete_current_task();
            completed_tasks++;
            update_queue_status();
            notify_queue_change();
        }
    }
}
```

## Integration with Master Valve

### Master Valve Coordination
The task queue coordinates with master valve timing:

1. **Pre-delay**: Master valve opens before task starts
2. **Task Execution**: Channel valve opens after pre-delay
3. **Grace Period**: Master valve stays open between consecutive tasks
4. **Post-delay**: Master valve closes after final task

### Overlapping Task Optimization

```c
bool can_start_next_task_immediately(void) {
    // Check if next task can start within grace period
    if (pending_count > 0 && master_valve_is_open()) {
        uint32_t time_since_last_task = get_time_since_last_task_end();
        return time_since_last_task < master_valve_grace_period_ms;
    }
    return false;
}
```

## Error Handling & Error Notifications

Errors are signaled via a notification where:
- `current_task_type = 0xFF`
- `current_value = <error_code>`
- Other fields updated to most recent queue snapshot (command cleared).

Implemented error codes (observed in firmware logic):
1 = No pending tasks
2 = Invalid command or invalid in current state
3 = System not ready / busy / could not start

### Examples

#### Command on Empty Queue (Start with no pending)
```c
// Error response for start command with no pending tasks
struct task_queue_data error = {
    .pending_count = 0,
    .completed_tasks = current_completed_count,
    .current_channel = 0xFF,
    .current_task_type = 0xFF,  // Error indicator
    .current_value = 1,         // Error code: No pending tasks
    .command = 0,
    .task_id_to_delete = 0,
    .active_task_id = 0
};
```

#### Invalid Command Value (e.g. 10)
```c
// Error response for invalid command (e.g., command = 10)
struct task_queue_data error = {
    .pending_count = current_pending_count,
    .completed_tasks = current_completed_count,
    .current_channel = current_channel,
    .current_task_type = 0xFF,  // Error indicator
    .current_value = 2,         // Error code: Invalid command
    .command = 0,
    .task_id_to_delete = 0,
    .active_task_id = current_task_id
};
```

#### System Not Ready / Busy
When system status prevents starting/resuming:
```c
struct task_queue_data error = {
    .pending_count = current_pending_count,
    .completed_tasks = current_completed_count,
    .current_channel = 0xFF,
    .current_task_type = 0xFF,  // Error indicator
    .current_value = 3,         // Error code: System not ready
    .command = 0,
    .task_id_to_delete = 0,
    .active_task_id = 0
};
```

## Write Requirements

- Writes MUST be exactly 9 bytes (`sizeof(task_queue_data)`); otherwise `BT_ATT_ERR_INVALID_ATTRIBUTE_LEN`.
- Partial writes / fragmentation not supported.
- Only the `command` (and future `task_id_to_delete`) fields are interpreted; other bytes may be zero.

## Implementation Examples

### Web Bluetooth (JavaScript)

```javascript
class TaskQueueManager {
    constructor(characteristic) {
        this.characteristic = characteristic;
        this.queueStatus = null;
        this.statusCallbacks = new Map();
        
        this.characteristic.addEventListener('characteristicvaluechanged', 
                                          this.handleQueueUpdate.bind(this));
    }

    async startMonitoring() {
        try {
            await this.characteristic.startNotifications();
            console.log('Task queue monitoring started');
            
            // Get initial status
            await this.refreshQueueStatus();
        } catch (error) {
            console.error('Failed to start queue monitoring:', error);
        }
    }

    async refreshQueueStatus() {
        try {
            const value = await this.characteristic.readValue();
            this.handleQueueData(value);
        } catch (error) {
            console.error('Failed to read queue status:', error);
        }
    }

    handleQueueUpdate(event) {
        this.handleQueueData(event.target.value);
    }

    handleQueueData(dataView) {
        const status = {
            pendingCount: dataView.getUint8(0),
            completedTasks: dataView.getUint8(1),
            currentChannel: dataView.getUint8(2),
            currentTaskType: dataView.getUint8(3),
            currentValue: dataView.getUint16(4, true), // Little-endian
            command: dataView.getUint8(6),
            taskIdToDelete: dataView.getUint8(7),
            activeTaskId: dataView.getUint8(8)
        };
        
        // Check for error responses
        if (status.currentTaskType === 0xFF) {
            this.handleErrorResponse(status);
            return;
        }
        
        const previousStatus = this.queueStatus;
        this.queueStatus = status;
        
        // Log status changes
        if (previousStatus) {
            this.logStatusChanges(previousStatus, status);
        }
        
        // Update UI
        this.updateQueueDisplay(status);
        
        // Notify callbacks
        this.notifyStatusChange(status, previousStatus);
        
        console.log(`Queue: ${status.pendingCount} pending, ${status.completedTasks} completed`);
        if (status.currentChannel !== 0xFF) {
            const taskType = status.currentTaskType === 0 ? 'duration' : 'volume';
            console.log(`Active: Channel ${status.currentChannel}, ${taskType}, ${status.currentValue}`);
        }
    }

    handleErrorResponse(status) {
        const errorMessages = {
            1: 'No pending tasks in queue',
            2: 'Invalid command',
            3: 'System not ready'
        };
        
        const errorMsg = errorMessages[status.currentValue] || 'Unknown error';
        console.error(`Task queue error: ${errorMsg}`);
        
        if (this.errorCallback) {
            this.errorCallback(status.currentValue, errorMsg);
        }
    }

    logStatusChanges(previous, current) {
        if (previous.pendingCount !== current.pendingCount) {
            console.log(`Pending tasks changed: ${previous.pendingCount} -> ${current.pendingCount}`);
        }
        
        if (previous.currentChannel !== current.currentChannel) {
            if (current.currentChannel === 0xFF) {
                console.log(`Task completed on channel ${previous.currentChannel}`);
            } else {
                console.log(`Task started on channel ${current.currentChannel}`);
            }
        }
        
        if (previous.completedTasks !== current.completedTasks) {
            console.log(`Completed tasks: ${previous.completedTasks} -> ${current.completedTasks}`);
        }
    }

    updateQueueDisplay(status) {
        // Update pending count
        const pendingElement = document.getElementById('pending-count');
        if (pendingElement) {
            pendingElement.textContent = status.pendingCount;
        }
        
        // Update completed count
        const completedElement = document.getElementById('completed-count');
        if (completedElement) {
            completedElement.textContent = status.completedTasks;
        }
        
        // Update current task display
        const currentTaskElement = document.getElementById('current-task');
        if (currentTaskElement) {
            if (status.currentChannel === 0xFF) {
                currentTaskElement.textContent = 'No active task';
                currentTaskElement.className = 'current-task idle';
            } else {
                const taskType = status.currentTaskType === 0 ? 'min' : 'L';
                currentTaskElement.textContent = 
                    `Channel ${status.currentChannel}: ${status.currentValue} ${taskType}`;
                currentTaskElement.className = 'current-task active';
            }
        }
        
        // Update queue visualization
        this.updateQueueVisualization(status);
    }

    updateQueueVisualization(status) {
        const queueElement = document.getElementById('task-queue-visual');
        if (!queueElement) return;
        
        // Clear existing visualization
        queueElement.innerHTML = '';
        
        // Show current task if active
        if (status.currentChannel !== 0xFF) {
            const currentDiv = document.createElement('div');
            currentDiv.className = 'queue-item current';
            currentDiv.textContent = `Ch${status.currentChannel} (Active)`;
            queueElement.appendChild(currentDiv);
        }
        
        // Show pending tasks
        for (let i = 0; i < status.pendingCount; i++) {
            const pendingDiv = document.createElement('div');
            pendingDiv.className = 'queue-item pending';
            pendingDiv.textContent = `Task ${i + 1}`;
            queueElement.appendChild(pendingDiv);
        }
        
        // Show empty state if no tasks
        if (status.currentChannel === 0xFF && status.pendingCount === 0) {
            const emptyDiv = document.createElement('div');
            emptyDiv.className = 'queue-empty';
            emptyDiv.textContent = 'Queue is empty';
            queueElement.appendChild(emptyDiv);
        }
    }

    // Command methods
    async startNextTask() {
        return this.sendCommand(1, 'Start next task');
    }

    async pauseCurrentTask() {
        return this.sendCommand(2, 'Pause current task');
    }

    async resumeCurrentTask() {
        return this.sendCommand(3, 'Resume current task');
    }

    async cancelCurrentTask() {
        return this.sendCommand(4, 'Cancel current task');
    }

    async clearAllPendingTasks() {
        return this.sendCommand(5, 'Clear all pending tasks');
    }

    async sendCommand(commandCode, description) {
        try {
            const data = new Uint8Array(9);
            data[6] = commandCode; // Set command field
            
            await this.characteristic.writeValue(data);
            console.log(`Command sent: ${description}`);
            
            // Wait a bit then refresh status
            setTimeout(() => this.refreshQueueStatus(), 500);
            
            return true;
        } catch (error) {
            console.error(`Failed to send command (${description}):`, error);
            return false;
        }
    }

    // Status monitoring
    onStatusChange(callback) {
        const id = Date.now() + Math.random();
        this.statusCallbacks.set(id, callback);
        return id;
    }

    offStatusChange(id) {
        this.statusCallbacks.delete(id);
    }

    notifyStatusChange(current, previous) {
        this.statusCallbacks.forEach(callback => {
            try {
                callback(current, previous);
            } catch (error) {
                console.error('Error in status change callback:', error);
            }
        });
    }

    setErrorCallback(callback) {
        this.errorCallback = callback;
    }

    // Utility methods
    getCurrentStatus() {
        return this.queueStatus;
    }

    isQueueEmpty() {
        return this.queueStatus && 
               this.queueStatus.pendingCount === 0 && 
               this.queueStatus.currentChannel === 0xFF;
    }

    hasActiveTask() {
        return this.queueStatus && this.queueStatus.currentChannel !== 0xFF;
    }

    getActiveTaskInfo() {
        if (!this.hasActiveTask()) return null;
        
        return {
            channel: this.queueStatus.currentChannel,
            type: this.queueStatus.currentTaskType === 0 ? 'duration' : 'volume',
            value: this.queueStatus.currentValue,
            taskId: this.queueStatus.activeTaskId
        };
    }
}

// Usage example
async function initializeTaskQueue(service) {
    const characteristic = await service.getCharacteristic('12345678-1234-5678-1234-56789abcdef7');
    const manager = new TaskQueueManager(characteristic);
    
    // Set up error callback
    manager.setErrorCallback((errorCode, message) => {
        showAlert('Task Queue Error', message, 'error');
    });
    
    // Set up status change monitoring
    manager.onStatusChange((current, previous) => {
        console.log('Queue status changed:', current);
        
        // Update dashboard
        updateTaskQueueDashboard(current);
        
        // Handle specific events
        if (previous && previous.currentChannel !== current.currentChannel) {
            if (current.currentChannel === 0xFF) {
                showNotification('Task Completed', 'Irrigation task finished');
            } else {
                showNotification('Task Started', `Watering channel ${current.currentChannel}`);
            }
        }
    });
    
    // Start monitoring
    await manager.startMonitoring();
    
    return manager;
}
```

## Testing and Validation

### Test Cases

1. **Empty Queue Status**
   - Expected: `00 XX FF 00 00 00 00 00 00` (XX = completed count)
   - Condition: No pending or active tasks

2. **Queue with Pending Tasks**
   - Expected: `03 XX FF 00 00 00 00 00 00` (3 pending tasks)
   - Condition: Tasks in queue, none executing

3. **Active Task Status**
   - Expected: `02 XX 01 00 1E 00 00 00 YY` (Channel 1, 30 min, task ID YY)
   - Condition: Task executing on channel 1

4. **Command Execution**
   - Write: `00 00 00 00 00 00 01 00 00` (Start next task)
   - Expected: Queue status change notification

5. **Error Response**
   - Write: `00 00 00 00 00 00 01 00 00` (Start with empty queue)
   - Expected: Error response with currentTaskType = 0xFF

## Related Characteristics

- **Valve Control (def1)**: Task execution triggers valve operations
- **Current Task Status (deff)**: Real-time task progress monitoring
- **System Status (def3)**: System readiness for task execution
- **Statistics (def8)**: Task completion statistics
- **Schedule Configuration (def5)**: Automatic task creation

## Best Practices

1. **Enable notifications** for real-time queue status updates
2. **Check queue status** before sending commands
3. **Handle error responses** gracefully with user feedback
4. **Monitor task completion** to update UI appropriately
5. **Implement command confirmation** with status refresh
6. **Use appropriate timeouts** for command operations
7. **Provide queue visualization** for better user experience
