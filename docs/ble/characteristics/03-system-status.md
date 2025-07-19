# System Status Characteristic

**UUID:** `12345678-1234-5678-1234-56789abcdef3`  
**Properties:** Read, Notify  
**Size:** 1 byte  
**Description:** Overall system operational status with automatic state tracking

## Overview

The System Status characteristic provides real-time monitoring of the overall system operational state. It automatically tracks system health, flow anomalies, hardware faults, and power conditions with intelligent state machine management.

**Fragmentation:** ❌ NOT REQUIRED - 1 byte fits in any BLE packet  
**Real-time Updates:** ✅ Notifications only on status changes (event-driven)  
**Read-only:** ✅ Status automatically determined by system monitoring  
**State Machine:** ✅ Intelligent transitions with automatic fault detection  
**Rate Limiting:** ✅ 500ms minimum delay between notifications to prevent buffer overflow

## Data Structure

```c
// Single byte status value
uint8_t status;     // System operational state (0-5)
```

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0 | 1 | `status` | System operational state |

### status (byte 0)
**Range:** 0-5 (6 defined states, extensible)  
**Source:** Real-time query from system state machine

## Status Code Reference

| Value | Status | Icon | Priority | Description |
|-------|--------|------|----------|-------------|
| 0 | **OK** | ✅ | Normal | System operating normally |
| 1 | **NO_FLOW** | 💧 | Warning | No flow when valve open |
| 2 | **UNEXPECTED_FLOW** | ⚠️ | Warning | Flow when valves closed |
| 3 | **FAULT** | ❌ | Critical | System fault - manual reset required |
| 4 | **RTC_ERROR** | 🕐 | Warning | Real-time clock failure |
| 5 | **LOW_POWER** | 🔋 | Warning | Low power mode active |

### Common Causes and Actions

| Status | Common Causes | Actions Required |
|--------|---------------|------------------|
| **NO_FLOW** | Water supply issue, blocked pipes, sensor fault | Check water supply, verify connections |
| **UNEXPECTED_FLOW** | Leak in system, valve malfunction | Inspect for leaks, check valve sealing |
| **FAULT** | Hardware failure, critical error | Manual intervention, system reset |
| **RTC_ERROR** | RTC battery, clock drift | Check RTC, synchronize time |
| **LOW_POWER** | Battery low, power saving mode | Check power supply, charge battery |

## Operations

### READ - Query Current Status
Returns current system status at any time.

```javascript
const data = await statusChar.readValue();
const statusCode = data.getUint8(0);

const statusNames = ['OK', 'NO_FLOW', 'UNEXPECTED_FLOW', 'FAULT', 'RTC_ERROR', 'LOW_POWER'];
const statusName = statusNames[statusCode] || 'UNKNOWN';

console.log(`System Status: ${statusName} (${statusCode})`);
```

### NOTIFY - Status Change Notifications
Event-driven notifications only when status changes.

```javascript
await statusChar.startNotifications();

let previousStatus = null;

statusChar.addEventListener('characteristicvaluechanged', (event) => {
    const currentStatus = event.target.value.getUint8(0);
    const statusNames = ['OK', 'NO_FLOW', 'UNEXPECTED_FLOW', 'FAULT', 'RTC_ERROR', 'LOW_POWER'];
    
    console.log(`Status changed to: ${statusNames[currentStatus]} (${currentStatus})`);
    
    // Handle specific status changes
    switch (currentStatus) {
        case 0: // OK
            if (previousStatus !== null) {
                console.log('✅ System recovered to normal operation');
            }
            break;
        case 1: // NO_FLOW
            console.warn('💧 No water flow detected - check water supply');
            break;
        case 2: // UNEXPECTED_FLOW
            console.warn('⚠️ Unexpected flow detected - possible leak!');
            break;
        case 3: // FAULT
            console.error('❌ CRITICAL: System fault - manual intervention required');
            break;
        case 4: // RTC_ERROR
            console.warn('🕐 RTC error - scheduling may be affected');
            break;
        case 5: // LOW_POWER
            console.warn('🔋 Low power mode - limited functionality');
            break;
    }
    
    previousStatus = currentStatus;
});
```

## System State Machine

### State Monitoring Sources
1. **Flow Anomaly Detection** - Continuous monitoring of flow vs. valve states
2. **Hardware Health Checks** - Periodic validation of sensors and actuators
3. **RTC Integrity** - Real-time clock functionality validation
4. **Power Management** - Battery level and power consumption tracking
5. **Communication Health** - Internal subsystem communication validation

### Automatic Recovery Mechanisms
- **NO_FLOW:** Retry attempts, automatic task cancellation after timeout
- **UNEXPECTED_FLOW:** Emergency valve closure, leak detection protocols
- **RTC_ERROR:** Attempt RTC resynchronization, fallback timing
- **LOW_POWER:** Automatic transition to energy-saving mode
- **FAULT:** Safe shutdown, preserve critical data, require manual reset

## Notification System

### Event-Driven Architecture
- **Change-only notifications:** Only sends when status actually changes
- **No periodic updates:** Prevents notification spam
- **Immediate critical alerts:** FAULT status bypasses normal rate limiting
- **Priority system:** Critical faults get highest priority

### Notification Triggers
- **Status transitions:** Immediate notification when state changes
- **Critical events:** Priority notifications for fault conditions
- **Recovery events:** Notifications when system returns to normal

## Safety Checks

```javascript
// Check if safe to start watering
async function isSafeToStartWatering() {
    const statusData = await statusChar.readValue();
    const status = statusData.getUint8(0);
    
    switch (status) {
        case 0: // OK
            console.log('✅ Safe to start watering');
            return true;
        case 1: // NO_FLOW
            console.log('⚠️ No flow detected - check water supply');
            return false;
        case 2: // UNEXPECTED_FLOW
            console.log('❌ Unexpected flow - possible leak detected');
            return false;
        case 3: // FAULT
            console.log('❌ System fault - resolve before watering');
            return false;
        case 4: // RTC_ERROR
            console.log('🕐 RTC error - sync time first');
            return false;
        case 5: // LOW_POWER
            console.log('🔋 Low power mode - limited operations');
            return false;
        default:
            console.log('❓ Unknown status - unsafe to proceed');
            return false;
    }
}
```

## Error Handling

When status indicates an error, use related characteristics for details:
- **[Diagnostics](13-diagnostics.md)** - For system health information
- **[Flow Sensor](02-flow-sensor.md)** - For flow-related issues
- **[RTC Configuration](09-rtc-configuration.md)** - For time-related problems

## Related Characteristics

- **[Valve Control](01-valve-control.md)** - Status affects valve operation
- **[Flow Sensor](02-flow-sensor.md)** - Flow anomalies trigger status changes
- **[Task Queue Management](07-task-queue-management.md)** - Status affects task execution
- **[Diagnostics](13-diagnostics.md)** - Detailed diagnostic data for faults
