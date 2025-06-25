# Software Guide

This guide explains the software architecture of the AutoWatering system and provides code examples for common operations.

## Architecture Overview

The AutoWatering system is built on Zephyr RTOS with a modular architecture organized into the following components:

```
src/
├── main.c               # Application entry point and main loop
├── watering.c           # Core watering system implementation
├── watering.h           # Public API for the watering system
├── watering_internal.h  # Internal shared definitions
├── flow_sensor.c        # Flow sensor interface
├── flow_sensor.h        # Flow sensor API
├── watering_tasks.c     # Task scheduling and execution
├── watering_monitor.c   # Flow monitoring and anomaly detection
├── watering_config.c    # Configuration storage management
├── valve_control.c      # Hardware interface for valves
├── watering_log.c       # Logging system
├── watering_log.h       # Logging interface
├── rtc.c                # Real-time clock interface
├── rtc.h                # RTC API
├── bt_irrigation_service.c # Bluetooth service implementation
├── bt_irrigation_service.h # Bluetooth service API
├── usb_descriptors.c    # USB device descriptors
├── usb_descriptors.h    # USB descriptors API
└── nvs_config.c         # Non-volatile storage management
```

## Key Concepts

### Channels and Events

Each irrigation channel (up to 8) can be configured with a watering event which defines:
- Schedule type (daily or periodic interval)
- Start time
- Watering mode (duration or volume-based)
- Auto-enabled flag

### Tasks and Scheduling

Watering operations are managed through a task system:
- Tasks can be created manually or automatically based on schedules
- Tasks are queued and executed sequentially
- Tasks can be time-based or volume-based
- Real-time scheduling is handled by a dedicated thread

### Flow Monitoring

The system monitors water flow to:
- Measure water consumption
- Detect anomalies (no-flow or unexpected flow)
- Trigger alerts for potential issues
- Support calibration operations

## Code Examples

### System Initialization

```c
// Initialize the watering system
watering_init();

// Start background tasks
watering_start_tasks();

// Load saved configuration
watering_load_config();
```

### Adding Watering Tasks

```c
// Duration-based watering (5 minutes on channel 0)
watering_add_duration_task(0, 5);

// Volume-based watering (2 liters on channel 1)
watering_add_volume_task(1, 2);
```

### Managing Tasks

```c
// Cancel all tasks and clear the queue
int cancelled_tasks = watering_cancel_all_tasks();

// Get queue status
uint8_t pending_count;
bool active_task;
watering_get_queue_status(&pending_count, &active_task);
```

### Configuring Schedules

```c
// Configure a daily watering event
watering_event_t event;
event.schedule_type = SCHEDULE_DAILY;
event.schedule.daily.days_of_week = 0x7E;  // Monday-Friday
event.start_time.hour = 7;
event.start_time.minute = 30;
event.watering_mode = WATERING_BY_VOLUME;
event.watering.by_volume.volume_liters = 5;
event.auto_enabled = true;

// Apply configuration to channel 2
watering_channel_t *channel;
watering_get_channel(2, &channel);
memcpy(&channel->watering_event, &event, sizeof(watering_event_t));
watering_save_config();
```

### Direct Valve Control

```c
// Turn on a valve
watering_channel_on(3);  // Turn on channel 3

// Wait some time
k_sleep(K_SECONDS(10));

// Turn off the valve
watering_channel_off(3);
```

### Flow Sensor Calibration

```c
// Start calibration using Bluetooth interface
int bt_irrigation_start_flow_calibration(1, 0);

// Reset pulse counter before calibration
reset_pulse_count();

// Run water through the system for a known volume
// (e.g., fill a 1-liter container)

// Get pulse count
uint32_t pulses = get_pulse_count();

// Stop calibration and set the measured volume
bt_irrigation_start_flow_calibration(0, 1000); // 1000ml

// The system automatically calculates and saves the new calibration
```

### Bluetooth Integration

The system provides a comprehensive Bluetooth Low Energy interface for remote monitoring and control:

```c
// Initialize Bluetooth service
int bt_irrigation_service_init(void);

// Send valve status updates
bt_irrigation_valve_status_update(channel_id, true);  // Valve on
bt_irrigation_valve_status_update(channel_id, false); // Valve off

// Update flow measurements
bt_irrigation_flow_update(pulse_count);

// Report system status changes
bt_irrigation_system_status_update(WATERING_STATUS_OK);

// Send alarm notifications
bt_irrigation_alarm_notify(alarm_code, alarm_data);
```

For detailed information about the Bluetooth API, see the [Bluetooth API Documentation](BLUETOOTH.md).

### Error Handling

```c
// Check system status
watering_status_t status;
watering_get_status(&status);

if (status == WATERING_STATUS_FAULT) {
    // System is in fault state
    // Attempt to reset
    watering_reset_fault();
}

// Check for errors after operations
watering_error_t err = watering_add_duration_task(0, 5);
if (err != WATERING_SUCCESS) {
    printk("Error adding task: %d\n", err);
}
```

## System States

The watering system has the following states:

| State | Description |
|-------|-------------|
| `WATERING_STATE_IDLE` | No active watering in progress |
| `WATERING_STATE_WATERING` | Actively watering one or more zones |
| `WATERING_STATE_PAUSED` | Watering temporarily paused |
| `WATERING_STATE_ERROR_RECOVERY` | System recovering from an error |

## Status Codes

The system can report the following status codes:

| Status | Description |
|--------|-------------|
| `WATERING_STATUS_OK` | System operating normally |
| `WATERING_STATUS_NO_FLOW` | No flow detected when valve is open |
| `WATERING_STATUS_UNEXPECTED_FLOW` | Flow detected when all valves are closed |
| `WATERING_STATUS_FAULT` | System in fault state requiring manual reset |
| `WATERING_STATUS_RTC_ERROR` | Real-time clock failure detected |
| `WATERING_STATUS_LOW_POWER` | System operating in low power mode |

## Power Modes

The system supports multiple power modes:

| Mode | Description |
|------|-------------|
| `POWER_MODE_NORMAL` | Standard operation, frequent updates |
| `POWER_MODE_ENERGY_SAVING` | Reduced update frequency to save power |
| `POWER_MODE_ULTRA_LOW_POWER` | Minimal activity mode for battery operation |

## Configuration Storage

The system uses Zephyr's settings subsystem to store configuration in non-volatile memory:

```c
// Save configuration to flash
watering_save_config();

// Load configuration from flash
watering_load_config();
```

## Thread Structure

The system uses several dedicated threads:

| Thread | Priority | Description |
|--------|----------|-------------|
| Watering Task | 5 | Processes watering operations |
| Scheduler Task | 7 | Checks schedules and creates tasks |
| Flow Monitor | 6 | Monitors flow sensor and detects anomalies |
| Main Thread | 0 | System monitoring and Bluetooth handling |

## Memory Considerations

- The system is optimized for memory-constrained devices
- Thread stack sizes are kept minimal but sufficient
- Memory allocation is primarily static to avoid fragmentation
- Configuration storage uses a dedicated flash partition

## Next Steps

- Explore the hardware setup in the [Hardware Guide](HARDWARE.md)
- Learn about the comprehensive Bluetooth interface in the [Bluetooth API Documentation](BLUETOOTH.md)
- Check out common issues and solutions in the [Troubleshooting Guide](TROUBLESHOOTING.md)
- Contribute to the project using the [Contributing Guide](CONTRIBUTING.md)

## Documentation Version

This software guide is current as of June 2025 and documents firmware version 1.6 with full Bluetooth API support.

[Back to main README](../README.md)
