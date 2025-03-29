# AutoWatering - Zephyr-based Irrigation Controller

A programmable irrigation controller system built on the Zephyr RTOS, featuring Bluetooth connectivity and support for up to 8 channels.

## Features

- **Multi-channel support**: Control up to 8 independent irrigation valves
- **Task-based operation**: All irrigation operations execute as scheduled tasks
- **Flexible scheduling**: Daily or periodic schedules with time or volume-based watering
- **Flow monitoring**: Built-in support for flow sensors with volume measurement
- **Bluetooth connectivity**: Full control and monitoring via BLE
- **Persistent storage**: All settings are saved to flash memory
- **Robust error handling**: Flow anomaly detection and recovery mechanisms
- **Power management**: Configurable power modes to extend battery life

## Hardware Requirements

- nRF52840-based board (Pro Micro BT)
- DS3231 RTC module (via I2C)
- YF-S201 flow sensor
- 8-channel relay module
- 12V solenoid valves

## Devicetree Configuration

The project uses custom devicetree bindings for valve control and flow sensor:

```
/ {
    water_flow_sensor: water_flow_sensor {
        compatible = "gpio-flow-sensor";
        status = "okay";
        gpios = <&gpio0 6 (GPIO_ACTIVE_HIGH | GPIO_PULL_UP)>;
        label = "Water Flow Sensor";
    };
    
    valves {
        compatible = "gpio-valves";
        status = "okay";
        
        valve1 {
            gpios = <&gpio0 9 GPIO_ACTIVE_HIGH>;
            label = "Valve 1";
        };
        
        // ... other valves
    };
};
```

## Bluetooth API

The system provides a comprehensive Bluetooth API for control and monitoring:

### Services and Characteristics

#### Irrigation Service (UUID: 12345678-1234-5678-1234-56789abcdef0)

1. **Task Creation** (UUID: 12345678-1234-5678-1234-56789abcdef1)
   - **Purpose**: Create watering tasks for specific channels
   - **Operations**: Read, Write, Notify
   - **Data Format**:
     ```
     {
       uint8_t channel_id;  // Channel (0-7)
       uint8_t task_type;   // Type (0=duration, 1=volume)
       uint16_t value;      // Value (minutes or liters)
     }
     ```

2. **Flow Data** (UUID: 12345678-1234-5678-1234-56789abcdef2)
   - **Purpose**: Monitor water flow
   - **Operations**: Read, Notify
   - **Data Format**: `uint32_t` (pulse count)

3. **System Status** (UUID: 12345678-1234-5678-1234-56789abcdef3)
   - **Purpose**: Monitor system status
   - **Operations**: Read, Notify
   - **Data Format**: `uint8_t` (status code)
   - **Values**:
     - 0: OK
     - 1: No flow detected
     - 2: Unexpected flow
     - 3: Fault
     - 4: RTC error
     - 5: Low power

4. **Channel Configuration** (UUID: 12345678-1234-5678-1234-56789abcdef4)
   - **Purpose**: Configure irrigation channels
   - **Operations**: Read, Write, Notify
   - **Data Format**:
     ```
     {
       uint8_t channel_id;   // Channel (0-7)
       uint8_t name_len;     // Name length
       char name[16];        // Channel name
       uint8_t auto_enabled; // Auto mode (0=off, 1=on)
     }
     ```

5. **Schedule Configuration** (UUID: 12345678-1234-5678-1234-56789abcdef5)
   - **Purpose**: Configure watering schedules
   - **Operations**: Read, Write, Notify
   - **Data Format**:
     ```
     {
       uint8_t channel_id;     // Channel (0-7)
       uint8_t schedule_type;  // Type (0=daily, 1=periodic)
       uint8_t days_mask;      // Days bitmap or interval days
       uint8_t hour;           // Start hour (0-23)
       uint8_t minute;         // Start minute (0-59)
       uint8_t watering_mode;  // Mode (0=duration, 1=volume)
       uint16_t value;         // Value (minutes or liters)
     }
     ```

6. **System Configuration** (UUID: 12345678-1234-5678-1234-56789abcdef6)
   - **Purpose**: Configure system settings
   - **Operations**: Read, Write, Notify
   - **Data Format**:
     ```
     {
       uint8_t power_mode;        // Power mode (0=normal, 1=energy saving, 2=ultra low)
       uint32_t flow_calibration; // Flow calibration (pulses per liter)
       uint8_t max_active_valves; // Maximum simultaneous valves (read-only)
     }
     ```

7. **Task Queue** (UUID: 12345678-1234-5678-1234-56789abcdef7)
   - **Purpose**: Monitor and manage task queue
   - **Operations**: Read, Write, Notify
   - **Data Format**:
     ```
     {
       uint8_t pending_tasks;     // Number of pending tasks
       uint8_t completed_tasks;   // Number of completed tasks
       uint8_t current_channel;   // Current active channel (0xFF if none)
       uint8_t current_task_type; // Current task type (0=duration, 1=volume)
       uint16_t current_value;    // Current task value (minutes or liters)
     }
     ```

8. **Statistics** (UUID: 12345678-1234-5678-1234-56789abcdef8)
   - **Purpose**: Monitor water usage statistics
   - **Operations**: Read, Notify
   - **Data Format**:
     ```
     {
       uint8_t channel_id;     // Channel (0-7)
       uint32_t total_volume;  // Total volume dispensed (ml)
       uint32_t last_volume;   // Last watering volume (ml)
       uint32_t last_watering; // Timestamp of last watering
       uint16_t count;         // Total count of watering events
     }
     ```

### API Usage Examples

#### Creating a Watering Task

To create a 10-minute watering task for channel 2:

```python
# Client code (Python example using the bleak library)
import asyncio
from bleak import BleakClient

async def add_irrigation_task(device_address):
    # Task creation characteristic UUID
    task_char_uuid = "12345678-1234-5678-1234-56789abcdef1"
    
    async with BleakClient(device_address) as client:
        # Create data for a 10-minute watering task on channel 2
        data = bytearray([
            2,    # Channel 2 (zero-indexed)
            0,    # Duration mode (0=time, 1=volume)
            10,   # 10 minutes (low byte)
            0     # 10 minutes (high byte)
        ])
        
        # Write to the characteristic
        await client.write_gatt_char(task_char_uuid, data)
        print("Task added successfully")

# Run the example
asyncio.run(add_irrigation_task("XX:XX:XX:XX:XX:XX"))  # Replace with device address
```

#### Reading System Status

```python
# Client code (Python example using the bleak library)
import asyncio
from bleak import BleakClient

async def read_system_status(device_address):
    # System status characteristic UUID
    status_char_uuid = "12345678-1234-5678-1234-56789abcdef3"
    
    async with BleakClient(device_address) as client:
        # Read the value
        value = await client.read_gatt_char(status_char_uuid)
        status = value[0]
        
        status_texts = [
            "OK", "No flow", "Unexpected flow", "Fault", "RTC error", "Low power"
        ]
        
        status_text = status_texts[status] if status < len(status_texts) else "Unknown"
        print(f"System status: {status_text} ({status})")

# Run the example
asyncio.run(read_system_status("XX:XX:XX:XX:XX:XX"))  # Replace with device address
```

#### Configuring a Watering Schedule

```python
# Client code (Python example using the bleak library)
import asyncio
from bleak import BleakClient

async def configure_schedule(device_address):
    # Schedule configuration characteristic UUID
    schedule_char_uuid = "12345678-1234-5678-1234-56789abcdef5"
    
    async with BleakClient(device_address) as client:
        # Create data for schedule: Channel 1, Daily (Mon,Wed,Fri), 7:30 AM, Duration 5 minutes
        data = bytearray([
            1,             # Channel 1 (zero-indexed)
            0,             # Schedule type (0=daily)
            0b00101010,    # Days bitmap (bit 1=Mon, bit 3=Wed, bit 5=Fri)
            7,             # Hour (7 AM)
            30,            # Minute (30)
            0,             # Watering mode (0=duration)
            5,             # 5 minutes (low byte)
            0              # 5 minutes (high byte)
        ])
        
        # Write to the characteristic
        await client.write_gatt_char(schedule_char_uuid, data)
        print("Schedule configured successfully")

# Run the example
asyncio.run(configure_schedule("XX:XX:XX:XX:XX:XX"))  # Replace with device address
```

## Task Management System

The system operates entirely based on tasks:

1. A task represents a single watering operation for a specific channel
2. Tasks can be created manually via Bluetooth or automatically via scheduling
3. Tasks are processed sequentially, with only one valve active at a time
4. The task scheduler checks for scheduled tasks every minute

### Task Types

- **Duration tasks**: Water for a specific amount of time
- **Volume tasks**: Water until a specific volume has been dispensed

### API Functions

The system provides functions for manually creating tasks:

```c
// Create a duration-based task
watering_error_t watering_add_duration_task(uint8_t channel_id, uint16_t minutes);

// Create a volume-based task
watering_error_t watering_add_volume_task(uint8_t channel_id, uint16_t liters);
```

## Building and Flashing

Make sure you have the Zephyr SDK installed and properly configured.

```bash
# Initialize west workspace if not already done
west init -l .

# Update west modules
west update

# Build the project
west build -b promicro_nrf52840

# Flash the device
west flash
```

## License

[Insert License Information]
