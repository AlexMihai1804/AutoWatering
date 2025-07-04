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

## Growing Environment Configuration

The AutoWatering system includes comprehensive growing environment configuration that enables intelligent, plant-specific irrigation control. This system allows precise setup for different plant types, soil conditions, irrigation methods, coverage measurements, and sun exposure.

### Plant Type Support

The system supports 26 predefined plant types plus custom plants:

```c
typedef enum {
    PLANT_TYPE_CUSTOM = 0,        // User-defined custom plants
    PLANT_TYPE_TOMATO = 1,        // High water needs, deep roots
    PLANT_TYPE_LETTUCE = 2,       // Shallow roots, frequent watering
    PLANT_TYPE_BASIL = 3,         // Moderate water, likes warmth
    PLANT_TYPE_PEPPER = 4,        // Similar to tomato, heat-loving
    PLANT_TYPE_SPINACH = 5,       // Cool weather, steady moisture
    PLANT_TYPE_CILANTRO = 6,      // Quick-growing, light watering
    PLANT_TYPE_MINT = 7,          // Loves water, spreads quickly
    PLANT_TYPE_ROSEMARY = 8,      // Drought-tolerant, minimal water
    PLANT_TYPE_STRAWBERRY = 9,    // Shallow roots, consistent moisture
    PLANT_TYPE_CUCUMBER = 10,     // High water needs, regular feeding
    // ... and 15 more predefined types
    PLANT_TYPE_COUNT
} watering_plant_type_t;
```

### Soil Type Classification

Eight soil types are supported to optimize watering patterns:

```c
typedef enum {
    SOIL_TYPE_LOAMY = 0,      // Balanced soil, good drainage and retention
    SOIL_TYPE_CLAY = 1,       // Heavy soil, retains water longer
    SOIL_TYPE_SANDY = 2,      // Light soil, drains quickly
    SOIL_TYPE_SILTY = 3,      // Fine particles, moderate drainage
    SOIL_TYPE_PEAT = 4,       // Organic soil, retains moisture well
    SOIL_TYPE_CHALK = 5,      // Alkaline soil, drains well
    SOIL_TYPE_POTTING_MIX = 6, // Commercial potting mix
    SOIL_TYPE_HYDROPONIC = 7,  // Soilless growing medium
    SOIL_TYPE_COUNT
} watering_soil_type_t;
```

### Irrigation Methods

Six irrigation methods are supported:

```c
typedef enum {
    IRRIGATION_METHOD_DRIP = 0,     // Targeted drip irrigation
    IRRIGATION_METHOD_SPRINKLER = 1, // Overhead sprinkler system
    IRRIGATION_METHOD_SOAKER = 2,   // Soaker hose system
    IRRIGATION_METHOD_MIST = 3,     // Misting system for delicate plants
    IRRIGATION_METHOD_FLOOD = 4,    // Flood/basin irrigation
    IRRIGATION_METHOD_SUBSURFACE = 5, // Subsurface drip irrigation
    IRRIGATION_METHOD_COUNT
} watering_irrigation_method_t;
```

### Coverage Configuration

Flexible coverage measurement supports both area and plant count:

```c
typedef struct {
    bool use_area;                  // True for area-based, false for plant count
    union {
        struct {
            float area_m2;          // Coverage area in square meters
        } area;
        struct {
            uint16_t count;         // Number of plants
        } plants;
    };
} watering_coverage_t;
```

### Custom Plant Support

For plants not in the predefined list:

```c
typedef struct {
    char custom_name[32];           // Custom plant name
    float water_need_factor;        // Water requirement multiplier (0.5-3.0)
    uint8_t irrigation_freq;        // Recommended irrigation frequency (days)
    bool prefer_area_based;         // Preferred measurement method
} watering_custom_plant_t;
```

### Growing Environment API

Key functions for environment configuration:

```c
// Plant type configuration
watering_error_t watering_set_plant_type(uint8_t channel_id, watering_plant_type_t plant_type);
watering_error_t watering_get_plant_type(uint8_t channel_id, watering_plant_type_t *plant_type);

// Soil and irrigation method
watering_error_t watering_set_soil_type(uint8_t channel_id, watering_soil_type_t soil_type);
watering_error_t watering_set_irrigation_method(uint8_t channel_id, watering_irrigation_method_t method);

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
- Learn about the comprehensive Bluetooth interface in the [Bluetooth API Documentation](BLUETOOTH.md)
- Check out common issues and solutions in the [Troubleshooting Guide](TROUBLESHOOTING.md)
- Contribute to the project using the [Contributing Guide](CONTRIBUTING.md)

## Documentation Version

This software guide is current as of June 2025 and documents firmware version 1.6 with full Bluetooth API support.

[Back to main README](../README.md)
