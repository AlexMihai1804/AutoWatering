# Software Guide

This guide explains the software architecture of the AutoWatering system and provides code examples for common operations.

## Recent Updates (July 2025)

### BLE System Reliability Improvements

The Bluetooth Low Energy subsystem has been significantly enhanced with:

#### Notification Throttling System
- **Automatic Queuing**: Failed notifications are queued for retry instead of being lost
- **Rate Limiting**: 100ms minimum delay between notifications prevents buffer overflow
- **Background Processing**: Dedicated worker thread handles notification overflow gracefully
- **Memory Management**: Intelligent buffer allocation prevents "Unable to allocate buffer" errors

```c
// New throttled notification API (used internally)
int bt_gatt_notify_throttled(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                             const void *data, uint16_t len);

// All BLE update functions now use throttled notifications
bt_irrigation_valve_status_update(channel_id, true);
bt_irrigation_flow_update(pulse_count);
bt_irrigation_current_task_update(channel_id, start_time, mode, target, current, volume);
```

#### Complete GATT Service Restoration
- **Full Service Definition**: All 15 characteristics properly defined and exposed
- **Proper Handler Functions**: All read/write handlers implemented with error checking
- **Service Structure**: Correctly positioned service definition prevents compilation issues

#### Buffer Pool Optimization
Configuration changes in `prj.conf`:
```kconfig
# Increased buffer pools for BLE reliability
CONFIG_BT_BUF_ACL_RX_SIZE=251
CONFIG_BT_BUF_ACL_TX_SIZE=251
CONFIG_BT_L2CAP_TX_BUF_COUNT=8
CONFIG_BT_ATT_TX_COUNT=8
```

#### Background Task Updates
- **Periodic Status Updates**: Background thread updates BLE clients every 2 seconds
- **Current Task Monitoring**: Real-time task progress updates without overwhelming buffers
- **Queue Status Tracking**: Automatic notifications of pending task changes

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

### 🚀 Master Valve Intelligence

The master valve system provides intelligent water pressure management:

#### Core Features
- **Automatic Timing**: Configurable pre/post delays for optimal pressure control
- **Overlap Detection**: Smart management of consecutive watering tasks
- **Dual Modes**: Automatic management or manual BLE control
- **Real-time Status**: Live monitoring via BLE notifications

#### Configuration Structure
```c
typedef struct {
    bool enabled;                    // Master valve system enable/disable
    int16_t pre_start_delay_sec;     // Delay before zone valve opens (-255 to +255)
    int16_t post_stop_delay_sec;     // Delay after zone valve closes (-255 to +255)
    uint8_t overlap_grace_sec;       // Grace period for consecutive tasks (0-255)
    bool auto_management;            // Automatic vs manual control
    bool is_active;                  // Current state (read-only)
    struct gpio_dt_spec valve;       // GPIO specification for hardware control
} master_valve_config_t;
```

#### Timing Logic
- **Positive delays**: Master valve opens/closes BEFORE zone valve
- **Negative delays**: Master valve opens/closes AFTER zone valve
- **Overlap detection**: Keeps master valve open if next task starts within grace period
- **Emergency closure**: Automatic closure on system shutdown or errors

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

### 🚀 Master Valve Control

```c
// Configure master valve
master_valve_config_t config = {
    .enabled = true,
    .pre_start_delay_sec = 3,      // Open 3 seconds before zone valve
    .post_stop_delay_sec = 2,      // Stay open 2 seconds after zone valve
    .overlap_grace_sec = 5,        // 5-second grace period for consecutive tasks
    .auto_management = true        // Automatic control mode
};
master_valve_set_config(&config);

// Get current configuration
master_valve_config_t current_config;
master_valve_get_config(&current_config);
printk("Master valve enabled: %s\n", current_config.enabled ? "Yes" : "No");

// Manual control (requires auto_management = false)
config.auto_management = false;
master_valve_set_config(&config);

// Manual open/close
watering_error_t err = master_valve_manual_open();
if (err == WATERING_SUCCESS) {
    printk("Master valve opened manually\n");
}

k_sleep(K_SECONDS(10));

err = master_valve_manual_close();
if (err == WATERING_SUCCESS) {
    printk("Master valve closed manually\n");
}

// Check current state
bool is_open = master_valve_is_open();
printk("Master valve is %s\n", is_open ? "open" : "closed");

// Notify about upcoming task (for overlap detection)
uint32_t start_time = k_uptime_get_32() + 30000;  // 30 seconds from now
master_valve_notify_upcoming_task(start_time);
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

For detailed information about the Bluetooth API, see the [BLE Documentation](ble/README.md).

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

## Growing Environment Configuration

The AutoWatering system includes comprehensive growing environment configuration that enables intelligent, plant-specific irrigation control. This system allows precise setup for different plant types, soil conditions, irrigation methods, coverage measurements, and sun exposure.

**📖 For complete plant type lists and detailed specifications, see [Plant Types Documentation](PLANT_TYPES.md).**

### Key Configuration Categories

The system supports the following configuration categories:

- **Plant Types**: 26+ predefined types plus custom plants (full list in [PLANT_TYPES.md](PLANT_TYPES.md))
- **Soil Types**: 8 soil classifications (Loamy, Clay, Sandy, Silty, Peat, Chalk, Potting Mix, Hydroponic)
- **Irrigation Methods**: 6 methods (Drip, Sprinkler, Soaker, Mist, Flood, Subsurface)
- **Coverage Measurement**: Area-based (m²) or plant count-based
- **Sun Exposure**: Percentage of direct sunlight (0-100%)

### Basic API Usage

```c
// Configure a channel for tomatoes with drip irrigation
watering_set_plant_type(0, PLANT_TYPE_TOMATO);
watering_set_soil_type(0, SOIL_TYPE_LOAMY);
watering_set_irrigation_method(0, IRRIGATION_METHOD_DRIP);

// Set coverage (10 square meters)
watering_coverage_t coverage = {
    .use_area = true,
    .area.area_m2 = 10.0
};
watering_set_coverage(0, &coverage);
```

**📖 For detailed API documentation and complete configuration examples, see [Plant Types Documentation](PLANT_TYPES.md).**

// Coverage configuration
watering_error_t watering_set_coverage_area(uint8_t channel_id, float area_m2);
watering_error_t watering_set_coverage_plants(uint8_t channel_id, uint16_t plant_count);

// Sun exposure and custom plants
watering_error_t watering_set_sun_percentage(uint8_t channel_id, uint8_t sun_percentage);
watering_error_t watering_set_custom_plant(uint8_t channel_id, const watering_custom_plant_t *custom_config);

// Complete environment configuration in one call
watering_error_t watering_set_channel_environment(uint8_t channel_id,
                                                 watering_plant_type_t plant_type,
                                                 watering_soil_type_t soil_type,
                                                 watering_irrigation_method_t irrigation_method,
                                                 bool use_area_based,
                                                 float area_m2,
                                                 uint16_t plant_count,
                                                 uint8_t sun_percentage,
                                                 const watering_custom_plant_t *custom_config);
```

### Smart Recommendations

The system provides intelligent recommendations:

- **Measurement Method**: Drip/Soaker systems typically use plant count, while Sprinkler/Mist systems use area
- **Water Factors**: Built-in factors for each plant type, adjustable with custom plants
- **Validation**: Ensures irrigation method and coverage type compatibility

### Example Usage

```c
// Configure tomato plants with drip irrigation
watering_set_plant_type(0, PLANT_TYPE_TOMATO);
watering_set_soil_type(0, SOIL_TYPE_LOAMY);
watering_set_irrigation_method(0, IRRIGATION_METHOD_DRIP);
watering_set_coverage_plants(0, 6);  // 6 tomato plants
watering_set_sun_percentage(0, 80);  // 80% sun exposure

// Configure custom plant
watering_custom_plant_t custom = {
    .custom_name = "Cherry Tomato",
    .water_need_factor = 1.2f,
    .irrigation_freq = 2,
    .prefer_area_based = false
};
watering_set_custom_plant(0, &custom);
```

### Integration Examples

#### Complete Environment Setup
```c
// Configure tomato plants with drip irrigation
watering_error_t result = watering_set_channel_environment(0,
    PLANT_TYPE_TOMATO,            // Specific tomato type
    SOIL_TYPE_LOAMY,              // Good drainage soil
    IRRIGATION_METHOD_DRIP,       // Targeted watering
    false,                        // use plant count (not area)
    0.0f,                         // area unused
    6,                            // 6 tomato plants
    80,                           // 80% sun exposure
    NULL                          // no custom config
);

// Configure lawn area with sprinkler
result = watering_set_channel_environment(1,
    PLANT_TYPE_CUSTOM,            // Will use custom name
    SOIL_TYPE_SANDY,              // Fast-draining soil
    IRRIGATION_METHOD_SPRINKLER,  // Area coverage
    true,                         // use area measurement
    25.5f,                        // 25.5 square meters
    0,                            // plant count unused
    90,                           // full sun
    &(watering_custom_plant_t){
        .custom_name = "Kentucky Bluegrass",
        .water_need_factor = 1.1f,
        .irrigation_freq = 3,
        .prefer_area_based = true
    }
);
```

#### Smart Recommendations Usage
```c
// Get intelligent recommendations
irrigation_method_t method = IRRIGATION_METHOD_DRIP;
bool recommended = watering_get_measurement_recommendation(method);
// recommended = false (plant count recommended for drip)

float water_factor = watering_get_water_factor(PLANT_TYPE_TOMATO, NULL);
// water_factor = 1.2 (tomatoes need more water)

bool valid = watering_validate_coverage_method(method, true); // area + drip
// valid = false (warning: drip usually better with plant count)
```

### Performance Characteristics

The Growing Environment system is designed for efficiency:
- **Memory**: ~80 bytes per channel additional storage
- **CPU**: O(1) access time for all operations
- **Storage**: Automatic NVS persistence with wear leveling
- **Network**: Single 47-byte BLE packet for complete environment

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

## Flash Storage Architecture

### Storage Layout Analysis

The AutoWatering system uses the nRF52840's 1024 KB flash memory efficiently:

| Component | Size | Purpose |
|-----------|------|---------|
| **Application Code** | ~400 KB | Main firmware |
| **NVS Storage** | 144 KB | History data and settings |
| **Bluetooth Settings** | 8 KB | Pairing and connection data |
| **Free Space** | ~472 KB | Available for future features |

### Storage Configuration

The system uses two main storage partitions:

#### NVS Storage Partition (144 KB)
- **Location**: 0x000c0000 - 0x000e3fff
- **Purpose**: History data, configuration, and system state
- **Usage**: Multi-level history storage with automatic aggregation

#### Bluetooth Settings Partition (8 KB)
- **Location**: 0x000e4000 - 0x000e5fff
- **Purpose**: Bluetooth pairing and connection settings
- **Usage**: Persistent BLE configuration

### History Storage Requirements

The system implements a comprehensive history system with multiple retention levels:

| History Level | Entries | Size per Entry | Total Size | Retention Period |
|---------------|---------|----------------|------------|------------------|
| **Detailed Events** | 2,880 | 22 bytes | 63.4 KB | 30 days |
| **Daily Statistics** | 2,920 | 20 bytes | 58.4 KB | 1 year |
| **Monthly Statistics** | 480 | 20 bytes | 9.6 KB | 5 years |
| **Annual Statistics** | 160 | 20 bytes | 3.2 KB | 20 years |
| **Total Required** | **6,440** | **Variable** | **134.6 KB** | **Multi-level** |

### Device Tree Configuration

For optimal storage allocation, use the enhanced overlay:

```dts
/* Enhanced NVS partition: 144 KiB for full history capability */
&flash0 {
    partitions {
        compatible = "fixed-partitions";
        #address-cells = <1>;
        #size-cells = <1>;

        nvs_storage: partition@c0000 {
            label = "nvs_storage";
            reg = <0x000c0000 0x00024000>;  /* 144 KiB */
        };

        settings_partition: partition@e4000 {
            label = "settings";
            reg = <0x000e4000 0x00002000>;  /* 8 KiB */
        };
    };
};
```

### Storage Management

The system implements automatic storage management:

1. **Circular Buffer**: Oldest entries are automatically overwritten
2. **Compression**: Daily/monthly/annual data uses aggregated statistics
3. **Maintenance**: Periodic cleanup and defragmentation
4. **Backup**: Critical settings stored in multiple locations

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

## Watering History System

### Architecture Overview

The AutoWatering system includes a comprehensive history tracking system that provides multi-level data aggregation and long-term trend analysis:

```
History System Architecture:
├── Detailed Events (30 days)    # Individual watering events
├── Daily Statistics (1 year)    # Daily aggregated data
├── Monthly Statistics (5 years) # Monthly trends
└── Annual Statistics (20 years) # Long-term patterns
```

### History Data Structures

#### Detailed Events
```c
typedef struct {
    uint32_t timestamp;          // Unix timestamp
    uint8_t channel_id;          // Channel identifier (0-7)
    uint8_t event_type;          // START, COMPLETE, ERROR
    uint8_t mode;                // DURATION or VOLUME
    uint16_t target_value;       // Target duration (seconds) or volume (ml)
    uint16_t actual_value;       // Actual duration or volume achieved
    uint16_t total_volume;       // Total volume delivered (ml)
    uint8_t trigger_type;        // MANUAL, SCHEDULED, SENSOR
    uint8_t success;             // Success flag
    uint8_t error_code;          // Error code if applicable
} __packed watering_event_detailed_t;
```

#### Daily Statistics
```c
typedef struct {
    uint16_t day;                // Day of year (1-366)
    uint16_t year;               // Year
    uint8_t channel_id;          // Channel identifier
    uint16_t sessions;           // Number of watering sessions
    uint32_t total_volume;       // Total volume (ml)
    uint16_t avg_duration;       // Average duration (seconds)
    uint8_t success_rate;        // Success rate (0-100%)
    uint8_t efficiency;          // Efficiency rating (0-100%)
    uint32_t scheduled_volume;   // Scheduled volume (ml)
    uint32_t manual_volume;      // Manual volume (ml)
    uint8_t errors;              // Number of errors
    uint8_t reserved[3];         // Reserved for future use
} __packed watering_stats_daily_t;
```

### History System Integration

#### System Initialization
```c
int main(void) {
    // ...existing initialization code...
    
    // Initialize history system
    ret = watering_history_init();
    if (ret != WATERING_SUCCESS) {
        printk("ERROR: Failed to initialize history system: %d\n", ret);
        return ret;
    }
    
    // ...rest of initialization...
}
```

#### Task Event Recording
```c
// Record task start
watering_history_on_task_start(
    channel_id,
    task->channel->watering_event.watering_mode,
    target_value,
    is_scheduled
);

// Record task completion
watering_history_on_task_complete(
    channel_id,
    actual_value,
    total_volume,
    success
);

// Record errors
watering_history_on_task_error(
    channel_id,
    error_code
);
```

#### Periodic Maintenance
```c
// Automatic data aggregation and cleanup
static void maintenance_thread(void *p1, void *p2, void *p3) {
    while (1) {
        uint32_t current_time = k_uptime_get_32();
        
        // Daily maintenance (every 24 hours)
        if (time_for_daily_maintenance(current_time)) {
            watering_history_daily_maintenance();
        }
        
        // Monthly maintenance (every 30 days)
        if (time_for_monthly_maintenance(current_time)) {
            watering_history_monthly_maintenance();
        }
        
        // Annual maintenance (every 365 days)
        if (time_for_annual_maintenance(current_time)) {
            watering_history_annual_maintenance();
        }
        
        k_sleep(K_HOURS(1));
    }
}
```

### API Functions

#### Data Retrieval
```c
// Get detailed events for a specific time range
watering_error_t watering_history_get_detailed_events(
    uint8_t channel_id,
    uint32_t start_time,
    uint32_t end_time,
    watering_event_detailed_t *events,
    uint16_t *count
);

// Get daily statistics
watering_error_t watering_history_get_daily_stats(
    uint8_t channel_id,
    uint16_t days,
    watering_stats_daily_t *stats,
    uint16_t *count
);

// Get recent daily volumes for charts
watering_error_t watering_history_get_recent_daily_volumes(
    uint8_t channel_id,
    uint16_t days,
    uint16_t *volumes,
    uint16_t *count
);
```

#### Cache System
```c
// Fast access to recent data
watering_error_t watering_history_cache_get_recent(
    uint8_t channel_id,
    watering_event_detailed_t *events,
    uint16_t *count
);

// Update cache on new events
watering_error_t watering_history_cache_update(
    const watering_event_detailed_t *event
);
```

### Performance Characteristics

The history system is optimized for:
- **Fast Writes**: Events are written to a circular buffer
- **Efficient Queries**: Cached recent data for quick UI updates
- **Automatic Cleanup**: Background maintenance prevents storage overflow
- **Low Memory**: Streaming operations minimize RAM usage

### Memory Usage
- **RAM**: ~5 KB for cache and management structures
- **Flash**: 134.6 KB for complete history storage
- **Performance**: <1ms for event insertion, <10ms for queries

## Next Steps

- Explore the hardware setup in the [Hardware Guide](HARDWARE.md)
- Learn about the comprehensive Bluetooth interface in the [BLE Documentation](ble/README.md)
- Check out common issues and solutions in the [Troubleshooting Guide](TROUBLESHOOTING.md)
- Contribute to the project using the [Contributing Guide](CONTRIBUTING.md)

## Documentation Version

This software guide is current as of June 2025 and documents firmware version 1.6 with full Bluetooth API support.

[Back to main README](../README.md)

## BLE Protocol Implementation

### Overview

The AutoWatering system implements a comprehensive Bluetooth Low Energy (BLE) protocol for remote configuration and monitoring. The implementation includes 15 characteristics with sophisticated fragmentation protocols for large data structures.

### Service Architecture

```c
// Main BLE service UUID
#define BT_UUID_IRRIGATION_SERVICE BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x1234, 0x56789abcdef0)

// Service structure with 15 characteristics
static struct bt_gatt_attr irrigation_service_attrs[] = {
    BT_GATT_PRIMARY_SERVICE(BT_UUID_IRRIGATION_SERVICE),
    
    // Status and control characteristics
    BT_GATT_CHARACTERISTIC(BT_UUID_IRRIGATION_STATUS, ...),
    BT_GATT_CHARACTERISTIC(BT_UUID_IRRIGATION_VALVE_STATUS, ...),
    BT_GATT_CHARACTERISTIC(BT_UUID_IRRIGATION_FLOW, ...),
    BT_GATT_CHARACTERISTIC(BT_UUID_IRRIGATION_CURRENT_TASK, ...),
    BT_GATT_CHARACTERISTIC(BT_UUID_IRRIGATION_TASK_QUEUE, ...),
    BT_GATT_CHARACTERISTIC(BT_UUID_IRRIGATION_SCHEDULE, ...),
    BT_GATT_CHARACTERISTIC(BT_UUID_IRRIGATION_CALIBRATION, ...),
    BT_GATT_CHARACTERISTIC(BT_UUID_IRRIGATION_HISTORY, ...),
    BT_GATT_CHARACTERISTIC(BT_UUID_IRRIGATION_ALARM, ...),
    
    // Configuration characteristics (require fragmentation)
    BT_GATT_CHARACTERISTIC(BT_UUID_IRRIGATION_CHANNEL_CONFIG, ...),
    BT_GATT_CHARACTERISTIC(BT_UUID_IRRIGATION_GROWING_ENV, ...),
    
    // Other characteristics
    BT_GATT_CHARACTERISTIC(BT_UUID_IRRIGATION_DEVICE_INFO, ...),
    BT_GATT_CHARACTERISTIC(BT_UUID_IRRIGATION_SETTINGS, ...),
    BT_GATT_CHARACTERISTIC(BT_UUID_IRRIGATION_NTP_SYNC, ...),
    BT_GATT_CHARACTERISTIC(BT_UUID_IRRIGATION_NAME_FIELDS, ...)
};
```

### Large Data Fragmentation

Two characteristics use large data structures that exceed the 20-byte BLE MTU limit:

#### Channel Configuration (76 bytes)
```c
struct channel_config {
    uint8_t channel_id;
    char name[16];
    uint8_t enabled;
    uint8_t watering_mode;
    uint32_t watering_duration;
    uint16_t watering_volume;
    uint8_t schedule_enabled;
    uint8_t schedule_type;
    uint8_t schedule_days;
    uint8_t schedule_hour;
    uint8_t schedule_minute;
    uint8_t schedule_watering_mode;
    uint32_t schedule_value;
    uint8_t auto_retry_enabled;
    uint8_t retry_count;
    uint16_t retry_interval;
    uint8_t flow_monitoring_enabled;
    uint16_t expected_flow_rate;
    uint8_t flow_timeout;
    uint8_t valve_type;
    uint8_t valve_open_time;
    uint8_t valve_close_time;
    uint8_t moisture_sensor_enabled;
    uint16_t moisture_threshold;
    uint8_t moisture_check_interval;
    uint8_t rain_sensor_enabled;
    uint8_t skip_if_rain;
    uint8_t reserved[12];
} __attribute__((packed));
```

**Protocol**: Uses little-endian universal fragmentation protocol:
```c
// Step 1: Channel selection (1 byte)
static ssize_t channel_config_write(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                   const void *buf, uint16_t len, uint16_t offset,
                                   uint8_t flags)
{
    if (len == 1) {
        // Channel selection
        selected_channel = ((const uint8_t *)buf)[0];
        return len;
    }
    
    // Step 2: Fragmented data with frag_type=2
    const uint8_t *data = (const uint8_t *)buf;
    uint8_t frag_type = data[0];
    
    if (frag_type == 2) {
        if (frag_state == FRAG_IDLE) {
            // First fragment: [2][size_low][size_high][data...]
            uint16_t total_size = data[1] | (data[2] << 8);  // Little-endian
            // Initialize fragmentation...
        } else {
            // Subsequent fragments: [2][data...]
            // Append data...
        }
    }
    
    return len;
}
```

#### Growing Environment (50 bytes)
```c
struct growing_environment {
    uint8_t channel_id;
    uint8_t plant_type;
    uint16_t specific_plant;
    uint8_t soil_type;
    uint8_t irrigation_method;
    uint8_t coverage_type;
    float coverage_area;
    uint16_t plant_count;
    uint8_t sun_exposure;
    uint8_t location_type;
    uint8_t season;
    uint8_t climate_zone;
    uint8_t auto_adjust_enabled;
    float water_need_factor;
    uint8_t irrigation_frequency;
    uint8_t prefer_area_based;
    char custom_name[16];
    uint8_t reserved[8];
} __attribute__((packed));
```

**Protocol**: Uses big-endian custom fragmentation protocol:
```c
static ssize_t growing_env_write(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                const void *buf, uint16_t len, uint16_t offset,
                                uint8_t flags)
{
    const uint8_t *data = (const uint8_t *)buf;
    
    // Format: [channel_id][frag_type=2][size_high][size_low][data...]
    uint8_t channel_id = data[0];
    uint8_t frag_type = data[1];
    
    if (frag_type == 2) {
        if (frag_state == FRAG_IDLE) {
            // First fragment: [channel_id][2][size_high][size_low][data...]
            uint16_t total_size = (data[2] << 8) | data[3];  // Big-endian
            growing_env_channel = channel_id;
            // Initialize fragmentation...
        } else {
            // Subsequent fragments: [channel_id][2][data...]
            // Append data...
        }
    }
    
    return len;
}
```

### Key Implementation Differences

**Channel Selection**:
- **Channel Configuration**: Explicit 1-byte selection protocol
- **Growing Environment**: Implicit (uses channel_id from last write)

**Fragmentation Endianness**:
- **Channel Configuration**: Little-endian size field
- **Growing Environment**: Big-endian size field

**Fragment Structure**:
- **Channel Config**: `[frag_type][size_low][size_high][data...]`
- **Growing Environment**: `[channel_id][frag_type][size_high][size_low][data...]`

### Notification System

The system includes a sophisticated notification throttling mechanism:

```c
// Notification throttling with background worker
static void bt_notification_work_handler(struct k_work *work)
{
    // Process queued notifications with 100ms minimum interval
    while (notification_queue_has_pending()) {
        struct notification_item *item = notification_queue_dequeue();
        
        // Send notification with error handling
        int err = bt_gatt_notify(item->conn, item->attr, item->data, item->len);
        
        if (err == 0) {
            // Success - update last notification time
            last_notification_time = k_uptime_get();
        } else {
            // Error - requeue for retry
            notification_queue_requeue(item);
        }
        
        // Enforce minimum interval
        k_sleep(K_MSEC(100));
    }
}
```

### Error Handling

Comprehensive error handling throughout the BLE stack:

```c
// Error codes returned by BLE operations
#define BT_IRRIGATION_SUCCESS           0
#define BT_IRRIGATION_ERROR_INVALID     -1
#define BT_IRRIGATION_ERROR_BUSY        -2
#define BT_IRRIGATION_ERROR_TIMEOUT     -3
#define BT_IRRIGATION_ERROR_FRAGMENTATION -4

// Error recovery mechanisms
static void bt_error_recovery(int error_code)
{
    switch (error_code) {
        case BT_IRRIGATION_ERROR_FRAGMENTATION:
            // Reset fragmentation state
            frag_state = FRAG_IDLE;
            frag_buffer_pos = 0;
            break;
            
        case BT_IRRIGATION_ERROR_TIMEOUT:
            // Restart advertising
            bt_advertising_restart();
            break;
    }
}
```

### Memory Management

Optimized memory usage for BLE operations:

```c
// Buffer configuration for BLE reliability
CONFIG_BT_BUF_ACL_RX_SIZE=251         // Increased RX buffer size
CONFIG_BT_BUF_ACL_TX_SIZE=251         // Increased TX buffer size
CONFIG_BT_L2CAP_TX_BUF_COUNT=8        // More L2CAP buffers
CONFIG_BT_ATT_TX_COUNT=8              // More ATT buffers

// Memory pools for fragmentation
static uint8_t frag_buffer[128];      // Shared fragmentation buffer
static struct k_mem_pool bt_pool;     // Dynamic allocation pool
```

### Connection Management

Robust connection handling with automatic recovery:

```c
// Connection event handlers
static void bt_connected(struct bt_conn *conn, uint8_t err)
{
    if (err) {
        LOG_ERR("Connection failed: %d", err);
        return;
    }
    
    LOG_INF("Connected");
    current_conn = bt_conn_ref(conn);
    
    // Initialize connection-specific state
    frag_state = FRAG_IDLE;
    notification_queue_clear();
}

static void bt_disconnected(struct bt_conn *conn, uint8_t reason)
{
    LOG_INF("Disconnected: %d", reason);
    
    if (current_conn) {
        bt_conn_unref(current_conn);
        current_conn = NULL;
    }
    
    // Reset fragmentation state
    frag_state = FRAG_IDLE;
    frag_buffer_pos = 0;
    
    // Schedule advertising restart
    k_work_schedule(&advertising_restart_work, K_SECONDS(5));
}
```

For complete BLE protocol specifications and client implementation examples, see [BLE_DOCUMENTATION_COMPLETE.md](BLE_DOCUMENTATION_COMPLETE.md).
