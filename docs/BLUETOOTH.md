# Bluetooth API Documentation

The AutoWatering system features a comprehensive Bluetooth Low Energy (BLE) interface that allows remote monitoring and control from mobile applications or gateways.

## Service Overview

The main service is identified by UUID: `12345678-1234-5678-1234-56789abcdef0`

All communication happens through this service and its characteristics.

## Connection Information

- **Device Name**: "AutoWatering"
- **Connection Type**: Bluetooth Low Energy (BLE)
- **Security**: Standard pairing mechanism
- **Maximum Connections**: 1 concurrent connection

## Characteristic Details

### 1. Task Creation

- **UUID**: `12345678-1234-5678-1234-56789abcdef1`
- **Operations**: Read, Write, Notify
- **Format**: 
  ```
  struct {
    uint8_t channel_id;    // Channel ID (0-7)
    uint8_t task_type;     // 0=duration, 1=volume
    uint16_t value;        // Minutes or liters
  }
  ```
- **Description**: Creates watering tasks. Write to add a new task, read to get current task status, notifications inform about task execution.

### 2. Flow Data

- **UUID**: `12345678-1234-5678-1234-56789abcdef2`
- **Operations**: Read, Notify
- **Format**: `uint32_t` (pulse count)
- **Description**: Reports the current flow sensor pulse count. Divide by calibration value to get volume in liters.

### 3. System Status

- **UUID**: `12345678-1234-5678-1234-56789abcdef3`
- **Operations**: Read, Notify
- **Format**: `uint8_t`
  - 0: Normal operation
  - 1: No flow detected
  - 2: Unexpected flow
  - 3: System fault
  - 4: RTC error
  - 5: Low power mode
- **Description**: Indicates the current system status. Notifications are sent when status changes.

### 4. Channel Configuration

- **UUID**: `12345678-1234-5678-1234-56789abcdef4`
- **Operations**: Read, Write, Notify
- **Format**: 
  ```
  struct {
    uint8_t channel_id;    // Channel ID (0-7)
    uint8_t name_len;      // Name length
    char name[16];         // Channel name
    uint8_t auto_enabled;  // Auto scheduling enabled (0=off, 1=on)
  }
  ```
- **Description**: Configures individual channel settings. Write to update settings, read to retrieve current configuration.

### 5. Schedule Configuration

- **UUID**: `12345678-1234-5678-1234-56789abcdef5`
- **Operations**: Read, Write, Notify
- **Format**: 
  ```
  struct {
    uint8_t channel_id;      // Channel ID (0-7)
    uint8_t schedule_type;   // 0=daily, 1=periodic
    uint8_t days_mask;       // Weekdays for daily schedule or interval days for periodic
    uint8_t hour;            // Start hour (0-23)
    uint8_t minute;          // Start minute (0-59)
    uint8_t watering_mode;   // 0=duration, 1=volume
    uint16_t value;          // Minutes or liters
  }
  ```
- **Description**: Configures automatic watering schedules for channels. For daily schedule, days_mask is a bitmap where bit 0=Sunday, bit 1=Monday, etc.

### 6. System Configuration

- **UUID**: `12345678-1234-5678-1234-56789abcdef6`
- **Operations**: Read, Write, Notify
- **Format**: 
  ```
  struct {
    uint8_t power_mode;           // 0=normal, 1=energy-saving, 2=ultra-low power
    uint32_t flow_calibration;    // Pulses per liter
    uint8_t max_active_valves;    // Maximum valves active simultaneously (read-only)
  }
  ```
- **Description**: Configures global system settings. The flow_calibration determines how pulse counts are converted to volume.

### 7. Task Queue Management

- **UUID**: `12345678-1234-5678-1234-56789abcdef7`
- **Operations**: Read, Write, Notify
- **Format**: 
  ```
  struct {
    uint8_t pending_tasks;       // Number of tasks in queue
    uint8_t completed_tasks;     // Number of completed tasks
    uint8_t current_channel;     // Current active channel (0xFF=none)
    uint8_t current_task_type;   // 0=duration, 1=volume
    uint16_t current_value;      // Minutes or liters
    uint8_t command;             // Queue command: 0=none, 1=cancel current, 2=clear queue
    uint8_t task_id_to_delete;   // Specific task ID to delete (if implemented)
  }
  ```
- **Description**: Manages the task queue. Write the command field to 1 to cancel the current task, 2 to clear all pending tasks.

### 8. Statistics

- **UUID**: `12345678-1234-5678-1234-56789abcdef8`
- **Operations**: Read, Notify
- **Format**: 
  ```
  struct {
    uint8_t channel_id;       // Channel ID (0-7)
    uint32_t total_volume;    // Total volume dispensed (ml)
    uint32_t last_volume;     // Last watering volume (ml)
    uint32_t last_watering;   // Timestamp of last watering
    uint16_t count;           // Total count of waterings
  }
  ```
- **Description**: Provides statistics about watering activities. Read with different channel_id to get stats for each channel.

### 9. RTC (Real-Time Clock)

- **UUID**: `12345678-1234-5678-1234-56789abcdef9`
- **Operations**: Read, Write, Notify
- **Format**: 
  ```
  struct {
    uint8_t year;          // Year minus 2000 (0-99)
    uint8_t month;         // Month (1-12)
    uint8_t day;           // Day (1-31)
    uint8_t hour;          // Hour (0-23)
    uint8_t minute;        // Minute (0-59)
    uint8_t second;        // Second (0-59)
    uint8_t day_of_week;   // Day of week (0=Sunday, 6=Saturday)
  }
  ```
- **Description**: Gets or sets the system's real-time clock. Essential for proper schedule operation.

### 10. Alarms

- **UUID**: `12345678-1234-5678-1234-56789abcdefa`
- **Operations**: Read, Notify
- **Format**: 
  ```
  struct {
    uint8_t alarm_code;    // Alarm code
    uint16_t alarm_data;   // Additional alarm-specific data
    uint32_t timestamp;    // Timestamp when alarm occurred
  }
  ```
- **Description**: Receives system alarm notifications. Clients should enable notifications to receive alerts about system issues.

### 11. Flow Calibration

- **UUID**: `12345678-1234-5678-1234-56789abcdefb`
- **Operations**: Read, Write, Notify
- **Format**: 
  ```
  struct {
    uint8_t action;            // 0=stop, 1=start, 2=in progress, 3=calculated
    uint32_t pulses;           // Pulse count
    uint32_t volume_ml;        // Volume in ml (input or calculated)
    uint32_t pulses_per_liter; // Calibration result
  }
  ```
- **Description**: Assists with flow sensor calibration. To calibrate: 1) Write with action=1 to start, 2) Dispense known water volume, 3) Write with action=0 and volume_ml=actual volume to complete calibration.

### 12. History

- **UUID**: `12345678-1234-5678-1234-56789abcdefc`
- **Operations**: Read, Write, Notify
- **Format**: 
  ```
  struct {
    uint8_t channel_id;    // Channel ID (0-7)
    uint8_t entry_index;   // History entry index (0=most recent)
    uint32_t timestamp;    // When it occurred
    uint8_t mode;          // 0=duration, 1=volume
    uint16_t duration;     // Duration in seconds or volume in ml
    uint8_t success;       // 1=successful, 0=failed
  }
  ```
- **Description**: Access irrigation history. To retrieve entries, write the channel_id and entry_index, then read to get the data.

### 13. Diagnostics

- **UUID**: `12345678-1234-5678-1234-56789abcdefd`
- **Operations**: Read, Notify
- **Format**: 
  ```
  struct {
    uint32_t uptime;        // System uptime in minutes
    uint8_t error_count;    // Number of errors
    uint8_t last_error;     // Last error code
    uint8_t valve_status;   // Valve status bitmap
    uint8_t battery_level;  // Battery level in percent or 0xFF if not applicable
  }
  ```
- **Description**: Provides system diagnostic information. The valve_status field is a bitmap where each bit represents a valve's state (1=open).

## Usage Examples

### Creating a Watering Task

```python
# Python example using BlueZ GATT libraries (simplified)

# Connect to the device
device = await connect_to_device("AutoWatering")

# Define the watering task
task_data = bytearray([
    0x01,        # Channel 1
    0x00,        # Duration mode
    0x05, 0x00   # 5 minutes (little endian)
])

# Write to the task creation characteristic
await device.write_gatt_char(
    "12345678-1234-5678-1234-56789abcdef1",
    task_data,
    response=True
)
```

### Reading Flow Data

```python
# Python example using BlueZ GATT libraries

# Connect to the device
device = await connect_to_device("AutoWatering")

# Read flow value
flow_bytes = await device.read_gatt_char("12345678-1234-5678-1234-56789abcdef2")
flow_pulses = int.from_bytes(flow_bytes, byteorder='little')

# Get calibration value
config_bytes = await device.read_gatt_char("12345678-1234-5678-1234-56789abcdef6")
calibration = int.from_bytes(config_bytes[1:5], byteorder='little')

# Calculate volume in liters
volume = flow_pulses / calibration
print(f"Current flow: {flow_pulses} pulses ({volume:.2f} liters)")
```

### Setting Up a Schedule

```python
# Python example using BlueZ GATT libraries

# Connect to the device
device = await connect_to_device("AutoWatering")

# Define a daily schedule for Channel 2
# Water every Monday, Wednesday, Friday at 7:30 AM for 10 minutes
schedule_data = bytearray([
    0x02,        # Channel 2
    0x00,        # Daily schedule
    0x2A,        # Days mask: bit 1 (Monday) + bit 3 (Wed) + bit 5 (Fri) = 0x2A
    0x07,        # Hour: 7
    0x1E,        # Minute: 30
    0x00,        # Duration mode
    0x0A, 0x00   # 10 minutes
])

# Write to the schedule configuration characteristic
await device.write_gatt_char(
    "12345678-1234-5678-1234-56789abcdef5",
    schedule_data,
    response=True
)

# Enable automatic scheduling for this channel
channel_config = bytearray([
    0x02,        # Channel 2
    0x00,        # Name length (keep existing name)
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  # Placeholder for name
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  # Placeholder for name
    0x01         # Enable auto scheduling
])

# Write to the channel configuration characteristic
await device.write_gatt_char(
    "12345678-1234-5678-1234-56789abcdef4",
    channel_config,
    response=True
)
```

### Calibrating the Flow Sensor

```python
# Python example using BlueZ GATT libraries

# Connect to the device
device = await connect_to_device("AutoWatering")

# Start calibration
start_calibration = bytearray([
    0x01,                # Action: Start
    0x00, 0x00, 0x00, 0x00,  # Pulses (not used when starting)
    0x00, 0x00, 0x00, 0x00,  # Volume (not used when starting)
    0x00, 0x00, 0x00, 0x00   # Pulses per liter (not used when starting)
])

# Write to start calibration
await device.write_gatt_char(
    "12345678-1234-5678-1234-56789abcdefb",
    start_calibration,
    response=True
)

# User instructions:
print("Please dispense exactly 1 liter of water through the system")
print("Press Enter when complete...")
input()

# Complete calibration with the known volume (1000 ml = 1 liter)
finish_calibration = bytearray([
    0x00,                # Action: Stop
    0x00, 0x00, 0x00, 0x00,  # Pulses (will be filled by device)
    0xE8, 0x03, 0x00, 0x00,  # Volume: 1000 ml
    0x00, 0x00, 0x00, 0x00   # Pulses per liter (will be calculated by device)
])

# Write to complete calibration
await device.write_gatt_char(
    "12345678-1234-5678-1234-56789abcdefb",
    finish_calibration,
    response=True
)

# Read the result
result = await device.read_gatt_char("12345678-1234-5678-1234-56789abcdefb")
pulses_per_liter = int.from_bytes(result[8:12], byteorder='little')
print(f"Calibration complete: {pulses_per_liter} pulses per liter")
```

## Client Implementation Tips

1. **Connection Management**:
   - Implement connection retries and timeout handling
   - Save the device address after first discovery for faster reconnection
   - Handle connection drops gracefully with automatic reconnection

2. **Notifications**:
   - Subscribe to system status and flow notifications for real-time monitoring
   - Consider batching multiple notification subscriptions to reduce setup time

3. **Error Handling**:
   - Check return values from all write operations
   - Implement timeouts for operations that may hang
   - Handle unexpected values gracefully

4. **Power Considerations**:
   - Minimize connection time to preserve device battery
   - Consider using a connection parameter update to extend connection interval when idle
   - Use the system configuration characteristic to set appropriate power mode

## Security Considerations

The current implementation uses standard Bluetooth pairing mechanisms. For enhanced security in production environments, consider implementing:

1. Secure connections with LE Secure Connections pairing
2. Application-level authentication
3. Encryption for sensitive data
4. Limited discovery window for the device

[Back to main README](../README.md)
