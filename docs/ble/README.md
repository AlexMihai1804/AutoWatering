# BLE AutoWatering Service Documentation

## Overview

The BLE AutoWatering service provides complete control of the irrigation system through 16 GATT characteristics. All characteristics support read operations, most support write and notify operations.

**Service UUID:** `12345678-1234-5678-1234-56789abcdef0`

## Quick Start

### Basic Connection
```javascript
// Connect to the AutoWatering device
const device = await navigator.bluetooth.requestDevice({
    filters: [{ services: ['12345678-1234-5678-1234-56789abcdef0'] }]
});

const server = await device.gatt.connect();
const service = await server.getPrimaryService('12345678-1234-5678-1234-56789abcdef0');

// Get valve control characteristic for basic watering
const valveChar = await service.getCharacteristic('12345678-1234-5678-1234-56789abcdef1');

// Start watering channel 0 for 10 minutes
const command = new ArrayBuffer(4);
const view = new DataView(command);
view.setUint8(0, 0);        // channel_id = 0
view.setUint8(1, 0);        // task_type = 0 (duration mode)
view.setUint16(2, 10, true); // value = 10 minutes

await valveChar.writeValue(command);
console.log('Watering started!');
```

## BLE Characteristics Overview

The AutoWatering system implements 16 BLE characteristics for complete system control and monitoring:

| # | Characteristic | UUID | R | W | N | Size | Description | Documentation |
|---|----------------|------|---|---|---|------|-------------|---------------|
| 1 | Valve Control | ...def1 | ✓ | ✓ | ✓ | 4B | Direct valve control, zone valves & **🚀 master valve** | [Details](characteristics/01-valve-control.md) |
| 2 | Flow Sensor | ...def2 | ✓ | - | ✓ | 4B | Flow rate monitoring and calibration | [Details](characteristics/02-flow-sensor.md) |
| 3 | System Status | ...def3 | ✓ | - | ✓ | 1B | Overall system status and diagnostics | [Details](characteristics/03-system-status.md) |
| 4 | Channel Configuration | ...def4 | ✓ | ✓ | ✓ | 76B | Per-channel watering configuration (fragmented) | [Details](characteristics/04-channel-configuration.md) |
| 5 | Schedule Configuration | ...def5 | ✓ | ✓ | ✓ | 9B | Automatic scheduling configuration | [Details](characteristics/05-schedule-configuration.md) |
| 6 | System Configuration | ...def6 | ✓ | ✓ | ✓ | 16B | Global system config + **🚀 master valve setup** | [Details](characteristics/06-system-configuration.md) |
| 7 | Task Queue Management | ...def7 | ✓ | ✓ | ✓ | 9B | Task queue control | [Details](characteristics/07-task-queue-management.md) |
| 8 | Statistics | ...def8 | ✓ | ✓ | ✓ | 15B | Usage statistics and metrics | [Details](characteristics/08-statistics.md) |
| 9 | RTC Configuration | ...def9 | ✓ | ✓ | ✓ | 16B | Real-time clock and time sync | [Details](characteristics/09-rtc-configuration.md) |
| 10 | Alarm Status | ...defa | ✓ | ✓ | ✓ | 7B | Error and alarm management | [Details](characteristics/10-alarm-status.md) |
| 11 | Calibration | ...defb | ✓ | ✓ | ✓ | 13B | Sensor calibration parameters | [Details](characteristics/11-calibration-management.md) |
| 12 | History Data | ...defc | ✓ | ✓ | ✓ | 32B | Watering history and analytics (fragmented) | [Details](characteristics/12-history-management.md) |
| 13 | Diagnostics | ...defd | ✓ | - | ✓ | 12B | System diagnostics and health | [Details](characteristics/13-diagnostics.md) |
| 14 | Growing Environment | ...defe | ✓ | ✓ | ✓ | 50B | Plant-specific environment config (fragmented) | [Details](characteristics/14-growing-environment.md) |
| 15 | Current Task Status | ...deff | ✓ | ✓ | ✓ | 21B | Active task monitoring | [Details](characteristics/15-current-task-status.md) |
| 16 | Timezone Configuration | ...9abc-def123456793 | ✓ | ✓ | ✓ | 16B | DST & timezone management | [Details](characteristics/16-timezone-configuration.md) |

**Legend:**
- **R** = Read, **W** = Write, **N** = Notify
- **Size** = Data structure size in bytes
- **Fragmented** = Characteristics > 20 bytes use fragmentation for BLE compatibility
- All characteristics support proper error handling and validation

## 🚀 Real-Time Notifications Features

**NEW: Ultra-Responsive 5Hz Progress Monitoring**
- **Active Tasks:** 5Hz (200ms) ultra-responsive updates with 1% progress sensitivity
- **Task Completion:** Immediate notification bypassing rate limiting
- **Idle System:** 1Hz (1000ms) power-efficient monitoring
- **Status Changes:** Instant notifications for pause/resume/completion events

**Key Features:**
- ⚡ **5Hz Frequency** - 200ms update intervals when tasks are active
- 🎯 **1% Sensitivity** - Detects minimal progress changes
- 🎉 **Instant Completion** - Zero-latency completion notifications
- 🔋 **Power Adaptive** - Frequency adjusts based on power mode
- 📡 **Dual Source** - Flow monitor + BLE background thread notifications

## Documentation Structure

This documentation is organized as follows:

- **[README.md](README.md)** - This overview document
- **[characteristics/](characteristics/)** - Individual characteristic documentation
- **[examples/](examples/)** - Code examples and use cases
- **[troubleshooting.md](troubleshooting.md)** - Common issues and solutions

## Common Use Cases

### Basic Operations
- **Start Watering:** Use [Valve Control](characteristics/01-valve-control.md) characteristic
- **Monitor Flow:** Use [Flow Sensor](characteristics/02-flow-sensor.md) characteristic
- **Check Status:** Use [System Status](characteristics/03-system-status.md) characteristic

### Configuration
- **Setup Channels:** Use [Channel Configuration](characteristics/04-channel-configuration.md) characteristic
- **Schedule Watering:** Use [Schedule Configuration](characteristics/05-schedule-configuration.md) characteristic
- **System Settings:** Use [System Configuration](characteristics/06-system-configuration.md) characteristic
- **Timezone Setup:** Use [Timezone Configuration](characteristics/16-timezone-configuration.md) characteristic

### Monitoring & Analytics
- **View Statistics:** Use [Statistics](characteristics/08-statistics.md) characteristic
- **Check History:** Use [History Data](characteristics/12-history-management.md) characteristic
- **System Health:** Use [Diagnostics](characteristics/13-diagnostics.md) characteristic

## Development Notes

- **16/16 characteristics** implement proper error handling and validation
- **Rate limiting** - All characteristics use 500ms minimum notification delay to prevent BLE buffer overflow
- **Channel selection** - Multi-channel characteristics (4, 5, 8, 14) support 1-byte write for channel selection
- **Fragmentation support** - Large characteristics (>20 bytes) use proper fragmentation protocols
- **Data structure verification** - All sizes and field definitions verified against source code implementation
- **Notifications optimized** for real-time performance with adaptive frequency
- **Full backwards compatibility** maintained throughout implementation
- **Complete coverage** - 16/16 characteristics documented with working JavaScript examples

**✅ Complete BLE Implementation:**
- **16/16 characteristics** fully documented and verified against code implementation
- **Correct data structure sizes** and field definitions confirmed
- **Rate limiting implemented** - 500ms minimum delay prevents BLE buffer overflow
- **Channel selection documented** for multi-channel characteristics (4, 5, 8, 14)
- **Fragmentation protocols** for large characteristics (76B, 50B, 20-32B)
- **Real-time notifications** with intelligent throttling and adaptive frequency
- **Comprehensive error handling** and input validation for all operations
- **Production ready** with complete JavaScript examples and best practices

For detailed implementation information, see the individual characteristic documentation files.
