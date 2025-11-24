# BLE Adaptive Notification Throttling System (Verified)

## Overview

The AutoWatering system implements a sophisticated adaptive notification throttling system to ensure stable BLE communication while maintaining real-time responsiveness. This system uses priority-based throttling, buffer pool management, and automatic recovery mechanisms to handle high-frequency notifications efficiently.

## Priority-Based Notification System

### Priority Levels

The system defines four priority levels with different throttling characteristics:

```c
typedef enum {
    NOTIFY_PRIORITY_CRITICAL = 0,  /* Alarms, errors - immediate */
    NOTIFY_PRIORITY_HIGH = 1,      /* Status updates, valve changes */
    NOTIFY_PRIORITY_NORMAL = 2,    /* Flow data, statistics */
    NOTIFY_PRIORITY_LOW = 3        /* History, diagnostics */
} notify_priority_t;
```

### Default Throttling Intervals

Each priority level has a base throttling interval (initial static values from code):

```c
#define THROTTLE_CRITICAL_MS    0      /* No throttling for critical */
#define THROTTLE_HIGH_MS        50     /* 50ms for high priority */
#define THROTTLE_NORMAL_MS      200    /* 200ms for normal */
#define THROTTLE_LOW_MS         1000   /* 1s for low priority */
```

### Characteristic Priority Mapping (Current Firmware)

The priority selection logic (excerpt simplified) assigns:

```c
if (attr == &irrigation_svc.attrs[ATTR_IDX_ALARM_VALUE])              CRITICAL;
else if (attr == &irrigation_svc.attrs[ATTR_IDX_STATUS_VALUE] ||
         attr == &irrigation_svc.attrs[ATTR_IDX_VALVE_VALUE]  ||
         attr == &irrigation_svc.attrs[ATTR_IDX_CURRENT_TASK_VALUE] ||
         attr == &irrigation_svc.attrs[ATTR_IDX_TASK_QUEUE_VALUE])    HIGH;
else if (attr == &irrigation_svc.attrs[ATTR_IDX_FLOW_VALUE] ||
         attr == &irrigation_svc.attrs[ATTR_IDX_STATISTICS_VALUE] ||
         attr == &irrigation_svc.attrs[ATTR_IDX_CALIB_VALUE] ||
         attr == &irrigation_svc.attrs[ATTR_IDX_SCHEDULE_VALUE] ||
         attr == &irrigation_svc.attrs[ATTR_IDX_SYSTEM_CFG_VALUE] ||
         attr == &irrigation_svc.attrs[ATTR_IDX_CHANNEL_CFG_VALUE] ||
         attr == &irrigation_svc.attrs[ATTR_IDX_ENVIRONMENTAL_DATA_VALUE] ||
         attr == &irrigation_svc.attrs[ATTR_IDX_COMPENSATION_STATUS_VALUE] ||
         attr == &irrigation_svc.attrs[ATTR_IDX_RTC_VALUE] ||
         attr == &irrigation_svc.attrs[ATTR_IDX_AUTO_CALC_STATUS_VALUE] ||
         attr == &irrigation_svc.attrs[ATTR_IDX_RAIN_INTEGRATION_STATUS_VALUE]) NORMAL;
else if (attr == &irrigation_svc.attrs[ATTR_IDX_ENVIRONMENTAL_HISTORY_VALUE] ||
         attr == &irrigation_svc.attrs[ATTR_IDX_ONBOARDING_STATUS_VALUE] ||
         attr == &irrigation_svc.attrs[ATTR_IDX_DIAGNOSTICS_VALUE])    LOW;
else if (attr == &irrigation_svc.attrs[ATTR_IDX_RESET_CONTROL_VALUE]) NORMAL;
else LOW;
```

| Priority | Throttle (initial) | Characteristics (summary) | Rationale |
|----------|--------------------|---------------------------|-----------|
| Critical | 0 ms | Alarm Status | Never delayed safety/error alerts |
| High | 50 ms | System Status, Valve Control, Current Task, Task Queue | Fast UI reactivity |
| Normal | 200 ms | Flow, Statistics, Calibration, Schedule, System Config, Channel Config, Environmental Data, Compensation Status, RTC, Auto Calc Status, Rain Integration Status, Reset Control | Regular telemetry/config changes |
| Low | 1000 ms | Environmental History, Onboarding Status, Diagnostics, other histories | Bulk / infrequent updates |

## Buffer Pool Management (Static Pool)

### Buffer Pool Structure

The system maintains a pool of notification buffers to prevent memory allocation issues:

```c
#define BLE_BUFFER_POOL_SIZE    8      /* Number of notification buffers */
#define MAX_NOTIFICATION_SIZE   23     /* Maximum BLE notification size */

typedef struct {
    uint8_t data[MAX_NOTIFICATION_SIZE];  /* Notification data */
    uint16_t len;                         /* Data length */
    const struct bt_gatt_attr *attr;      /* Target attribute */
    notify_priority_t priority;           /* Notification priority */
    uint32_t timestamp;                   /* When queued */
    bool in_use;                          /* Buffer allocation status */
} ble_notification_buffer_t;

static ble_notification_buffer_t notification_pool[BLE_BUFFER_POOL_SIZE];
```

### Buffer Allocation Strategy

```c
static ble_notification_buffer_t* allocate_notification_buffer(void) {
    for (int i = 0; i < BLE_BUFFER_POOL_SIZE; i++) {
        uint8_t idx = (pool_head + i) % BLE_BUFFER_POOL_SIZE;
        if (!notification_pool[idx].in_use) {
            notification_pool[idx].in_use = true;
            notification_pool[idx].timestamp = k_uptime_get_32();
            pool_head = (idx + 1) % BLE_BUFFER_POOL_SIZE;
            buffers_in_use++;
            return &notification_pool[idx];
        }
    }
    
    /* No buffers available */
    last_buffer_exhaustion = k_uptime_get_32();
    LOG_WRN("[WARN] BLE buffer pool exhausted (%d/%d in use)", 
            buffers_in_use, BLE_BUFFER_POOL_SIZE);
    return NULL;
}
```

### Buffer Recovery Mechanism

When buffers are exhausted, the system implements a recovery strategy:

```c
#define BUFFER_RECOVERY_TIME_MS 2000   /* Time to wait before retry */

// During throttling check
if (last_buffer_exhaustion > 0 && 
    (now - last_buffer_exhaustion) < BUFFER_RECOVERY_TIME_MS &&
    priority != NOTIFY_PRIORITY_CRITICAL) {
    return true; // Throttle non-critical notifications
}
```

## Adaptive Throttling Algorithm (Adaptive Intervals)

### Dynamic Interval Adjustment

The system automatically adjusts throttling intervals based on success/failure rates:

```c
// Firmware logic (summary):
// Success window: >20 successes & <5 failures triggers 10% reduction (min caps per priority).
// Failure window: >5 failures triggers 20% increase (max caps per priority).
// After an adjustment success/failure counters reset.
```

### Throttling Decision Logic

```c
static bool should_throttle_notification(notify_priority_t priority) {
    uint32_t now = k_uptime_get_32();
    uint32_t elapsed = now - priority_state[priority].last_notification_time;
    
    /* Critical notifications are never throttled */
    if (priority == NOTIFY_PRIORITY_CRITICAL) {
        return false;
    }
    
    /* Check if enough time has passed */
    if (elapsed < priority_state[priority].throttle_interval) {
        return true;
    }
    
    /* Additional throttling during buffer recovery */
    if (last_buffer_exhaustion > 0 && 
        (now - last_buffer_exhaustion) < BUFFER_RECOVERY_TIME_MS &&
        priority != NOTIFY_PRIORITY_CRITICAL) {
        return true;
    }
    
    return false;
}
```

## Current Task Periodic Updates

There is no dedicated thread. A delayed work item reschedules every 2 seconds only while:
1. A task is actively running (not paused)
2. Notifications for Current Task are enabled
3. A BLE connection is active

This reduces idle wakeups compared to a permanent monitoring thread.

## Advanced Notification Functions

### Smart Notification Macro

The system provides intelligent notification macros that handle errors gracefully:

```c
#define SMART_NOTIFY(conn, attr, data, size) \
    do { \
        if ((conn) && (attr) && connection_active) { \
            int _err = advanced_notify((conn), (attr), (data), (size)); \
            if (_err == -EBUSY) { \
                /* Throttled - this is expected and managed by adaptive system */ \
            } else if (_err == -ENOMEM) { \
                /* Buffer pool exhausted - handled by advanced_notify */ \
            } else if (_err != 0 && _err != -ENOTCONN) { \
                static uint32_t _last_err_time = 0; \
                uint32_t _now = k_uptime_get_32(); \
                if (_now - _last_err_time > 5000) { \
                    LOG_ERR("BLE notification failed: %d", _err); \
                    _last_err_time = _now; \
                } \
            } \
        } \
    } while(0)
```

### Critical Notification Macro

Critical notifications bypass most throttling mechanisms:

```c
#define CRITICAL_NOTIFY(conn, attr, data, size) \
    do { \
        if ((conn) && (attr) && connection_active) { \
            /* Critical notifications bypass most checks and use priority handling */ \
            int _err = advanced_notify((conn), (attr), (data), (size)); \
            if (_err != 0 && _err != -ENOTCONN) { \
                LOG_ERR("Critical BLE notification failed: %d", _err); \
            } \
        } \
    } while(0)
```

## Specialized Throttling Features

### Channel Name Notification Throttling

Constants in firmware:
```
#define CHANNEL_NAME_NOTIFICATION_DELAY_MS 1000
#define CHANNEL_NAME_MAX_NOTIFICATIONS 3
```
If more than 3 name writes occur for the same channel within 1 second, subsequent notifications are suppressed until the window elapses or a different channel is edited.

## Performance Monitoring and Diagnostics

### Adaptive Behavior Logging

The system logs adaptive behavior for monitoring and debugging:

```c
/* Log adaptive behavior occasionally */
static uint32_t last_log_time = 0;
uint32_t now = k_uptime_get_32();

if (now - last_log_time > 10000) { /* Every 10 seconds */
    LOG_DBG("Adaptive throttling - P%d: %ums interval, %u/%u success/fail, %d/%d buffers\n",
            priority, priority_state[priority].throttle_interval,
            priority_state[priority].success_count, 
            priority_state[priority].failure_count,
            buffers_in_use, BLE_BUFFER_POOL_SIZE);
    last_log_time = now;
}
```

### Buffer Pool Maintenance

Regular maintenance ensures optimal buffer pool performance:

```c
static void buffer_pool_maintenance(void) {
    uint32_t now = k_uptime_get_32();
    
    /* Clean up old buffers that might be stuck */
    for (int i = 0; i < BLE_BUFFER_POOL_SIZE; i++) {
        if (notification_pool[i].in_use) {
            uint32_t age = now - notification_pool[i].timestamp;
            if (age > 30000) { /* 30 seconds timeout */
                LOG_WRN("Releasing stuck buffer %d (age: %ums)", i, age);
                release_notification_buffer(&notification_pool[i]);
            }
        }
    }
    
    /* Reset buffer exhaustion flag if enough time has passed */
    if (last_buffer_exhaustion > 0 && 
        (now - last_buffer_exhaustion) > BUFFER_RECOVERY_TIME_MS) {
        last_buffer_exhaustion = 0;
    }
}
```

## Platform-Specific Implementation Examples

### JavaScript/Web Bluetooth

```javascript
class BLENotificationThrottler {
    constructor() {
        this.priorities = {
            CRITICAL: { interval: 0, lastTime: 0, successCount: 0, failureCount: 0 },
            HIGH: { interval: 50, lastTime: 0, successCount: 0, failureCount: 0 },
            NORMAL: { interval: 200, lastTime: 0, successCount: 0, failureCount: 0 },
            LOW: { interval: 1000, lastTime: 0, successCount: 0, failureCount: 0 }
        };
        this.bufferPool = [];
        this.maxBuffers = 8;
    }
    
    shouldThrottle(characteristic, priority = 'NORMAL') {
        const now = Date.now();
        const priorityState = this.priorities[priority];
        
        if (priority === 'CRITICAL') {
            return false; // Never throttle critical
        }
        
        const elapsed = now - priorityState.lastTime;
        if (elapsed < priorityState.interval) {
            return true; // Throttle
        }
        
        priorityState.lastTime = now;
        return false;
    }
    
    updateAdaptiveThrottling(priority, success) {
        const state = this.priorities[priority];
        
        if (success) {
            state.successCount++;
            
            // Reduce throttling on consistent success
            if (state.successCount > 20 && state.failureCount < 5) {
                const minInterval = this.getMinInterval(priority);
                if (state.interval > minInterval) {
                    state.interval = Math.floor(state.interval * 0.9);
                }
            }
        } else {
            state.failureCount++;
            
            // Increase throttling on failures
            if (state.failureCount > 5) {
                const maxInterval = this.getMaxInterval(priority);
                if (state.interval < maxInterval) {
                    state.interval = Math.floor(state.interval * 1.1);
                }
            }
        }
    }
    
    getMinInterval(priority) {
        const intervals = { CRITICAL: 0, HIGH: 25, NORMAL: 100, LOW: 500 };
        return intervals[priority] || 500;
    }
    
    getMaxInterval(priority) {
        const intervals = { CRITICAL: 100, HIGH: 500, NORMAL: 2000, LOW: 5000 };
        return intervals[priority] || 5000;
    }
}

// Usage example
const throttler = new BLENotificationThrottler();

function handleNotification(characteristic, data) {
    const priority = getCharacteristicPriority(characteristic);
    
    if (throttler.shouldThrottle(characteristic, priority)) {
        console.log(`Notification throttled for ${characteristic.uuid}`);
        throttler.updateAdaptiveThrottling(priority, false);
        return;
    }
    
    // Process notification
    try {
        processNotificationData(characteristic, data);
        throttler.updateAdaptiveThrottling(priority, true);
    } catch (error) {
        console.error('Notification processing failed:', error);
        throttler.updateAdaptiveThrottling(priority, false);
    }
}
```

### iOS Swift

```swift
class BLENotificationThrottler {
    enum Priority: Int, CaseIterable {
        case critical = 0, high = 1, normal = 2, low = 3
        
        var defaultInterval: TimeInterval {
            switch self {
            case .critical: return 0.0
            case .high: return 0.05
            case .normal: return 0.2
            case .low: return 1.0
            }
        }
        
        var minInterval: TimeInterval {
            switch self {
            case .critical: return 0.0
            case .high: return 0.025
            case .normal: return 0.1
            case .low: return 0.5
            }
        }
        
        var maxInterval: TimeInterval {
            switch self {
            case .critical: return 0.1
            case .high: return 0.5
            case .normal: return 2.0
            case .low: return 5.0
            }
        }
    }
    
    private struct PriorityState {
        var lastTime: TimeInterval = 0
        var interval: TimeInterval
        var successCount: Int = 0
        var failureCount: Int = 0
        
        init(priority: Priority) {
            self.interval = priority.defaultInterval
        }
    }
    
    private var priorityStates: [Priority: PriorityState] = [:]
    private let bufferPool = NSMutableArray()
    private let maxBuffers = 8
    
    init() {
        for priority in Priority.allCases {
            priorityStates[priority] = PriorityState(priority: priority)
        }
    }
    
    func shouldThrottle(characteristic: CBCharacteristic, priority: Priority = .normal) -> Bool {
        let now = CACurrentMediaTime()
        guard var state = priorityStates[priority] else { return false }
        
        if priority == .critical {
            return false // Never throttle critical
        }
        
        let elapsed = now - state.lastTime
        if elapsed < state.interval {
            return true // Throttle
        }
        
        state.lastTime = now
        priorityStates[priority] = state
        return false
    }
    
    func updateAdaptiveThrottling(priority: Priority, success: Bool) {
        guard var state = priorityStates[priority] else { return }
        
        if success {
            state.successCount += 1
            
            // Reduce throttling on consistent success
            if state.successCount > 20 && state.failureCount < 5 {
                let minInterval = priority.minInterval
                if state.interval > minInterval {
                    state.interval = state.interval * 0.9
                }
            }
        } else {
            state.failureCount += 1
            
            // Increase throttling on failures
            if state.failureCount > 5 {
                let maxInterval = priority.maxInterval
                if state.interval < maxInterval {
                    state.interval = state.interval * 1.1
                }
            }
        }
        
        priorityStates[priority] = state
    }
}

// Usage in CBPeripheralDelegate
extension YourBLEManager: CBPeripheralDelegate {
    func peripheral(_ peripheral: CBPeripheral, 
                   didUpdateValueFor characteristic: CBCharacteristic, 
                   error: Error?) {
        
        let priority = getCharacteristicPriority(characteristic)
        
        if throttler.shouldThrottle(characteristic: characteristic, priority: priority) {
            print("Notification throttled for \(characteristic.uuid)")
            throttler.updateAdaptiveThrottling(priority: priority, success: false)
            return
        }
        
        // Process notification
        do {
            try processNotificationData(characteristic: characteristic)
            throttler.updateAdaptiveThrottling(priority: priority, success: true)
        } catch {
            print("Notification processing failed: \(error)")
            throttler.updateAdaptiveThrottling(priority: priority, success: false)
        }
    }
}
```

### Android Kotlin

```kotlin
class BLENotificationThrottler {
    enum class Priority(val defaultInterval: Long, val minInterval: Long, val maxInterval: Long) {
        CRITICAL(0L, 0L, 100L),
        HIGH(50L, 25L, 500L),
        NORMAL(200L, 100L, 2000L),
        LOW(1000L, 500L, 5000L)
    }
    
    private data class PriorityState(
        var lastTime: Long = 0L,
        var interval: Long,
        var successCount: Int = 0,
        var failureCount: Int = 0
    )
    
    private val priorityStates = mutableMapOf<Priority, PriorityState>()
    private val bufferPool = mutableListOf<ByteArray>()
    private val maxBuffers = 8
    
    init {
        Priority.values().forEach { priority ->
            priorityStates[priority] = PriorityState(interval = priority.defaultInterval)
        }
    }
    
    fun shouldThrottle(characteristic: BluetoothGattCharacteristic, 
                      priority: Priority = Priority.NORMAL): Boolean {
        val now = System.currentTimeMillis()
        val state = priorityStates[priority] ?: return false
        
        if (priority == Priority.CRITICAL) {
            return false // Never throttle critical
        }
        
        val elapsed = now - state.lastTime
        if (elapsed < state.interval) {
            return true // Throttle
        }
        
        state.lastTime = now
        return false
    }
    
    fun updateAdaptiveThrottling(priority: Priority, success: Boolean) {
        val state = priorityStates[priority] ?: return
        
        if (success) {
            state.successCount++
            
            // Reduce throttling on consistent success
            if (state.successCount > 20 && state.failureCount < 5) {
                if (state.interval > priority.minInterval) {
                    state.interval = (state.interval * 0.9).toLong()
                }
            }
        } else {
            state.failureCount++
            
            // Increase throttling on failures
            if (state.failureCount > 5) {
                if (state.interval < priority.maxInterval) {
                    state.interval = (state.interval * 1.1).toLong()
                }
            }
        }
    }
}

// Usage in BluetoothGattCallback
class YourBLECallback : BluetoothGattCallback() {
    private val throttler = BLENotificationThrottler()
    
    override fun onCharacteristicChanged(gatt: BluetoothGatt, 
                                       characteristic: BluetoothGattCharacteristic) {
        val priority = getCharacteristicPriority(characteristic)
        
        if (throttler.shouldThrottle(characteristic, priority)) {
            Log.d(TAG, "Notification throttled for ${characteristic.uuid}")
            throttler.updateAdaptiveThrottling(priority, false)
            return
        }
        
        // Process notification
        try {
            processNotificationData(characteristic)
            throttler.updateAdaptiveThrottling(priority, true)
        } catch (e: Exception) {
            Log.e(TAG, "Notification processing failed", e)
            throttler.updateAdaptiveThrottling(priority, false)
        }
    }
}
```

## Best Practices

### Implementation Guidelines (Best Practices)

1. **Always respect priority levels** - Critical notifications should never be throttled
2. **Monitor buffer pool health** - Implement regular maintenance and cleanup
3. **Use adaptive intervals** - Let the system adjust based on performance
4. **Handle errors gracefully** - Don't let notification failures crash the system
5. **Log performance metrics** - Track success/failure rates for optimization

### Performance Optimization

1. **Pre-allocate buffers** - Avoid dynamic allocation during notifications
2. **Batch similar notifications** - Group related updates when possible
3. **Use appropriate priorities** - Don't over-prioritize routine data
4. **Monitor connection quality** - Adjust throttling based on BLE stability
5. **Implement backpressure** - Slow down data generation when buffers are full

### Troubleshooting Common Issues

1. **Buffer Pool Exhaustion**: Increase pool size or reduce notification frequency
2. **High Failure Rates**: Check BLE connection stability and MTU size
3. **Excessive Throttling**: Review priority assignments and intervals
4. **Memory Leaks**: Ensure proper buffer cleanup and release
5. **Performance Degradation**: Monitor adaptive interval adjustments

Verified against `bt_irrigation_service.c` (adaptive intervals: -10% / +20%, channel name delay 1000ms, task queue characteristic = HIGH priority). Legacy references to a standalone monitoring thread removed.