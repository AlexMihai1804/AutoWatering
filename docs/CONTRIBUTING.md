# Contributing to AutoWatering

Thank you for your interest in contributing to the AutoWatering project! This guide provides information on how to contribute effectively.

## Code of Conduct

By participating in this project, you agree to abide by our code of conduct:

- Be respectful and inclusive
- Focus on constructive feedback
- Resolve disagreements professionally
- Put the project's best interests first
- Maintain a harassment-free environment for everyone

## How to Contribute

There are many ways to contribute to AutoWatering:

1. **Code Contributions**: Add new features or fix bugs
2. **Documentation**: Improve or expand documentation and examples
3. **Testing**: Test the system and report issues
4. **Feature Suggestions**: Propose new features or enhancements
5. **Bug Reports**: Report issues you find while using the system

## Getting Started

### Development Setup

1. **Fork the Repository**:
   - Fork the project on GitHub
   - Clone your fork locally:
   ```bash
   git clone https://github.com/<your-username>/AutoWatering.git
   cd AutoWatering
   ```

2. **Set Up Development Environment**:
   - Follow the [Installation Guide](INSTALLATION.md)
   - Make sure you can successfully build and flash the project

3. **Create a Branch**:
   ```bash
   git checkout -b feature/my-feature
   # or
   git checkout -b fix/my-fix
   # or  
   git checkout -b docs/update-documentation
   ```

### Hardware Testing

If contributing hardware-related changes:

1. **Test on Real Hardware**:
   - Verify changes work on actual nRF52840 hardware
   - Test with real solenoid valves and flow sensors
   - Document any new hardware requirements

2. **Power Consumption**:
   - Measure power consumption for new features
   - Ensure changes don't significantly impact battery life
   - Document power requirements

3. **Bluetooth Compatibility**:
   - Test with multiple client platforms (Android, iOS, Web)
   - Verify MTU handling and fragmentation for Channel Config (76 bytes) and Growing Environment (50 bytes)
   - Check notification performance
   - Validate plant configuration enum ranges (plant_type: 0-7, soil_type: 0-7, irrigation_method: 0-5)

## System Architecture and Functions

Understanding the internal architecture is crucial for effective contributions:

### Core System Components

1. **Main Event Loop** (`main.c`):
   - Initializes all subsystems in sequence
   - Handles USB console interface
   - Manages system-wide error recovery
   - Coordinates between different modules

2. **Watering Core** (`watering.c`):
   - Implements the main state machine
   - Manages channel configurations and events
   - Handles system status transitions
   - Provides the public API for all watering operations

3. **Task Management** (`watering_tasks.c`):
   - Queues and executes watering tasks
   - Handles both manual and scheduled operations
   - Manages concurrent task limitations (max 1 active valve)
   - Implements scheduling logic with RTC integration

4. **Flow Monitoring** (`watering_monitor.c`):
   - Real-time flow anomaly detection
   - Implements no-flow and unexpected-flow detection
   - Provides flow rate calculations
   - Handles calibration procedures

5. **Valve Control** (`valve_control.c`):
   - Hardware abstraction for valve operations
   - GPIO management and safety checks
   - Valve state tracking and reporting
   - Hardware fault detection

### Operation Modes

The system operates in several distinct modes:

#### 1. Normal Operation Mode
```c
// System actively monitoring and executing tasks
// All subsystems operational
// Bluetooth and USB interfaces active
// Regular flow monitoring and scheduling
```

#### 2. Power Saving Modes
```c
typedef enum {
    POWER_MODE_NORMAL,          // Full operation, 100ms monitoring
    POWER_MODE_ENERGY_SAVING,   // Reduced polling, 500ms intervals
    POWER_MODE_ULTRA_LOW_POWER  // Minimal activity, 2s intervals
} power_mode_t;
```

#### 3. Error Recovery Mode
- Triggered by hardware faults or communication failures
- Safely shuts down all valves
- Attempts automatic recovery
- Logs detailed error information

### Key Function Categories

#### Configuration Management
```c
// Load/save persistent configuration
watering_error_t watering_load_config(void);
watering_error_t watering_save_config(void);

// Channel-specific operations
watering_error_t watering_get_channel(uint8_t channel_id, watering_channel_t **channel);
```

#### Task Operations
```c
// Task creation and management
watering_error_t watering_add_duration_task(uint8_t channel_id, uint16_t minutes);
watering_error_t watering_add_volume_task(uint8_t channel_id, uint16_t liters);
int watering_cancel_all_tasks(void);
```

#### Hardware Interface
```c
// Direct valve control
watering_error_t watering_channel_on(uint8_t channel_id);
watering_error_t watering_channel_off(uint8_t channel_id);

// Flow sensor interface
uint32_t get_pulse_count(void);
void reset_pulse_count(void);
```

#### Bluetooth Integration
```c
// Status updates
int bt_irrigation_system_status_update(watering_status_t status);
int bt_irrigation_valve_status_update(uint8_t channel_id, bool state);
int bt_irrigation_flow_update(uint32_t pulses);

// Alarm notifications
int bt_irrigation_alarm_notify(uint8_t alarm_code, uint16_t alarm_data);
```

### Thread Architecture

The system uses multiple threads for concurrent operation:

#### 1. Main Thread (Priority 0)
- System initialization and monitoring
- Bluetooth service management
- USB console handling
- Global error handling

#### 2. Watering Task Thread (Priority 5)
- Processes queued watering tasks
- Manages valve activation/deactivation
- Handles task completion and cleanup
- Interfaces with flow monitoring

#### 3. Scheduler Thread (Priority 7)
- Checks scheduled events against RTC time
- Creates automatic tasks based on channel configuration
- Handles day/time calculations
- Manages schedule persistence

#### 4. Flow Monitor Thread (Priority 6)
- Continuous flow sensor monitoring
- Anomaly detection (no-flow, unexpected flow)
- Flow rate calculations
- Safety shutdowns on critical errors

### State Machine Implementation

The core watering system implements a comprehensive state machine:

```c
typedef enum {
    WATERING_STATE_IDLE,           // No active operations
    WATERING_STATE_WATERING,       // Valve active, monitoring flow
    WATERING_STATE_PAUSED,         // Temporarily suspended
    WATERING_STATE_ERROR_RECOVERY  // Handling system errors
} watering_state_t;
```

#### State Transitions
- **IDLE → WATERING**: Task starts, valve opens
- **WATERING → IDLE**: Task completes normally
- **Any → ERROR_RECOVERY**: Critical error detected
- **ERROR_RECOVERY → IDLE**: Recovery successful
- **WATERING → PAUSED**: Manual pause request

### Data Flow Architecture

```
┌─────────────┐    ┌──────────────┐    ┌─────────────┐
│   RTC       │    │  Flow Sensor │    │   Valves    │
│   (Time)    │    │  (Pulses)    │    │ (GPIO Out)  │
└──────┬──────┘    └──────┬───────┘    └──────┬──────┘
       │                  │                   │
       ▼                  ▼                   ▼
┌─────────────────────────────────────────────────────┐
│              Core Watering System                   │
│  ┌─────────────┐ ┌─────────────┐ ┌─────────────┐   │
│  │ Scheduler   │ │   Monitor   │ │   Control   │   │
│  │   Thread    │ │   Thread    │ │   Thread    │   │
│  └─────────────┘ └─────────────┘ └─────────────┘   │
└─────────────────┬───────────────────────────────────┘
                  │
                  ▼
┌─────────────────────────────────────────────────────┐
│              Interface Layer                        │
│  ┌─────────────┐           ┌─────────────┐         │
│  │ Bluetooth   │           │    USB      │         │
│  │   GATT      │           │  Console    │         │
│  └─────────────┘           └─────────────┘         │
└─────────────────────────────────────────────────────┘
```

### Coding Standards

Follow these standards when writing code:

1. **Zephyr Coding Style**:
   - Follow the [Zephyr Coding Style Guidelines](https://docs.zephyrproject.org/latest/contribute/coding_guidelines/index.html)
   - Use 4-space indentation (not tabs)
   - Maximum line length of 100 characters

2. **Documentation**:
   - Add Doxygen comments to all functions, structures, and enums
   - Keep comments updated if you modify code
   - Example:
   ```c
   /**
    * @brief Function description
    *
    * Detailed description of function purpose and operation.
    *
    * @param param1 Description of parameter 1
    * @param param2 Description of parameter 2
    *
    * @return Description of return value
    */
   ```

3. **Error Handling**:
   - Use the defined `watering_error_t` error codes consistently
   - Check all API returns and handle errors appropriately
   - No silent failures

4. **Resource Management**:
   - Clean up all resources in error paths
   - Use the RAII pattern where possible
   - No memory leaks

5. **Thread Safety**:
   - Use appropriate synchronization primitives (mutexes, semaphores)
   - Document thread safety expectations in header files

### Pull Request Process

1. **Create a Pull Request**:
   - Push your branch to your fork:
   ```bash
   git push origin feature/my-feature
   ```
   - Go to GitHub and create a pull request against the main repository

2. **PR Description**:
   - Describe what your changes do and why
   - Link to any related issues
   - Include test results or screenshots if relevant
   
3. **Code Review**:
   - All PRs must be reviewed by at least one maintainer
   - Address reviewer feedback promptly
   - Maintainers may request changes before merging

4. **Continuous Integration**:
   - Wait for CI checks to complete successfully
   - Fix any issues raised by automated tests

5. **Merging**:
   - Maintainers will merge your PR when it's ready
   - You may be asked to rebase if there are conflicts

## Development Guidelines

### Feature Development

When implementing new features:

1. **Discuss First**:
   - Open an issue to discuss the feature before implementing
   - Get consensus on the approach and design

2. **Minimal Changes**:
   - Keep changes focused and minimal
   - Split large features into smaller, incremental PRs

3. **Tests**:
   - Add tests for new functionality where possible
   - Ensure existing tests continue to pass
   - Test with real hardware when applicable

### Bluetooth API Changes

Special considerations for Bluetooth interface modifications:

1. **Backward Compatibility**:
   - Maintain compatibility with existing clients when possible
   - Document breaking changes clearly in ble/README.md
   - Provide migration guidance for API changes

2. **Structure Packing**:
   - Ensure all structures use `__packed` attribute
   - Verify byte alignment matches documentation
   - Test on different platforms for endianness issues

3. **MTU Considerations**:
   - Test with default MTU (23 bytes) for web browser compatibility
   - Verify fragmentation protocols work correctly
   - Document structure size requirements clearly

4. **Testing Requirements**:
   - Test with multiple client platforms (Android, iOS, Web)
   - Verify notification behavior and timing
   - Check error handling and recovery

### Bug Fixes

When fixing bugs:

1. **Reproduce First**:
   - Make sure you can reproduce the issue
   - Create a test case that demonstrates the bug

2. **Fix the Root Cause**:
   - Address the root cause, not just symptoms
   - Consider if similar bugs might exist elsewhere

3. **Documentation**:
   - Update documentation if the bug was related to unclear docs
   - Add comments explaining non-obvious fixes

### Documentation

When updating documentation:

1. **Clear Language**:
   - Use simple, clear language
   - Avoid jargon where possible
   - Consider non-native English speakers

2. **Examples**:
   - Include practical examples for new features
   - Update examples to match code changes

3. **Formatting**:
   - Use Markdown formatting consistently
   - Check rendering before submitting

### Critical System Functions

When modifying core system functions, pay special attention to:

#### 1. Error Handling Patterns
```c
// Always use standardized error codes
watering_error_t function_name(params) {
    // Validate inputs first
    if (invalid_input) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    // Acquire resources with cleanup on failure
    if (acquire_resource() != 0) {
        cleanup_partial_state();
        return WATERING_ERROR_HARDWARE;
    }
    
    // Main operation with error checking
    int result = main_operation();
    if (result != 0) {
        release_resources();
        return WATERING_ERROR_TIMEOUT;
    }
    
    return WATERING_SUCCESS;
}
```

#### 2. Thread Safety Requirements
```c
// Always protect shared state with mutexes
K_MUTEX_DEFINE(shared_state_mutex);

void modify_shared_state(void) {
    k_mutex_lock(&shared_state_mutex, K_FOREVER);
    
    // Critical section - modify shared variables
    shared_variable = new_value;
    
    k_mutex_unlock(&shared_state_mutex);
}
```

#### 3. Bluetooth Notification Patterns
```c
// Check connection before sending notifications
if (default_conn) {
    int err = bt_gatt_notify(default_conn, 
                            &irrigation_svc.attrs[ATTR_IDX_STATUS_VALUE],
                            &status_value, sizeof(status_value));
    if (err) {
        LOG_WRN("Notification failed: %d", err);
    }
}
```

#### 4. Resource Management
```c
// Always clean up resources in error paths
watering_error_t complex_operation(void) {
    void *resource1 = allocate_resource1();
    if (!resource1) return WATERING_ERROR_CONFIG;
    
    void *resource2 = allocate_resource2();
    if (!resource2) {
        free_resource1(resource1);  // Cleanup on error
        return WATERING_ERROR_CONFIG;
    }
    
    // Main work...
    
    // Normal cleanup
    free_resource2(resource2);
    free_resource1(resource1);
    return WATERING_SUCCESS;
}
```

### Testing Functions

When adding new functionality, implement corresponding test functions:

#### 1. Unit Test Pattern
```c
// Add test functions for new features
static int test_new_feature(void) {
    // Setup test conditions
    reset_system_state();
    
    // Execute function under test
    watering_error_t result = new_feature_function(test_params);
    
    // Verify results
    if (result != WATERING_SUCCESS) {
        printk("Test failed: unexpected error %d\n", result);
        return -1;
    }
    
    // Verify side effects
    if (get_system_state() != EXPECTED_STATE) {
        printk("Test failed: incorrect state\n");
        return -1;
    }
    
    return 0;
}
```

#### 2. Hardware Integration Tests
```c
// Test hardware interfaces with real components
static int test_valve_operation(uint8_t channel) {
    // Test valve activation
    watering_error_t err = watering_channel_on(channel);
    if (err != WATERING_SUCCESS) return -1;
    
    k_sleep(K_SECONDS(2));  // Allow settling time
    
    // Verify valve state via GPIO
    if (!is_valve_active(channel)) {
        printk("Valve %d failed to activate\n", channel);
        return -1;
    }
    
    // Test deactivation
    err = watering_channel_off(channel);
    if (err != WATERING_SUCCESS) return -1;
    
    k_sleep(K_SECONDS(1));
    
    if (is_valve_active(channel)) {
        printk("Valve %d failed to deactivate\n", channel);
        return -1;
    }
    
    return 0;
}
```

### Performance Considerations

#### 1. Memory Usage
- Keep stack usage minimal in thread functions
- Use static allocation where possible
- Monitor heap usage if dynamic allocation is needed
- Consider memory fragmentation in long-running operations

#### 2. Timing Constraints
```c
// Critical timing sections
#define MAX_VALVE_RESPONSE_TIME_MS 1000
#define FLOW_DETECTION_TIMEOUT_MS 3000
#define BLUETOOTH_NOTIFICATION_INTERVAL_MS 100

// Always use appropriate timeouts
if (!wait_for_valve_response(MAX_VALVE_RESPONSE_TIME_MS)) {
    return WATERING_ERROR_TIMEOUT;
}
```

#### 3. Power Optimization
```c
// Use appropriate sleep modes
void low_power_operation(void) {
    // Reduce polling frequency in power save mode
    if (current_power_mode == POWER_MODE_ULTRA_LOW_POWER) {
        k_sleep(K_SECONDS(2));
    } else {
        k_sleep(K_MSEC(100));
    }
}
```

## Development Workflow

1. **Issue Tracking**:
   - All work should be linked to a GitHub issue
   - Use issue numbers in commit messages and PR titles

2. **Branching**:
   - Feature branches: `feature/description`
   - Bug fix branches: `fix/issue-description`
   - Release branches: `release/version`

3. **Commit Messages**:
   - Use clear, descriptive commit messages
   - Format: `[Component] Brief description (max 50 chars)`
   - Add detailed description in commit body if needed

4. **Version Control**:
   - Make atomic commits (one logical change per commit)
   - Rebase branches before creating PRs
   - Avoid merge commits when possible

### Common Development Patterns

When implementing new features, follow these established patterns:

#### 1. Adding New Bluetooth Characteristics
```c
// 1. Define the UUID in bt_irrigation_service.c
#define BT_UUID_IRRIGATION_NEW_FEATURE_VAL \
    BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x1234, 0x56789abcdefX)

// 2. Define the data structure
struct new_feature_data {
    uint8_t  param1;
    uint16_t param2;
    uint32_t param3;
} __packed;

// 3. Implement read/write handlers
static ssize_t read_new_feature(struct bt_conn *conn, 
                               const struct bt_gatt_attr *attr,
                               void *buf, uint16_t len, uint16_t offset) {
    return bt_gatt_attr_read(conn, attr, buf, len, offset,
                            attr->user_data, sizeof(struct new_feature_data));
}

// 4. Add to GATT service definition
BT_GATT_CHARACTERISTIC(&new_feature_uuid.uuid,
                      BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE | BT_GATT_CHRC_NOTIFY,
                      BT_GATT_PERM_READ | BT_GATT_PERM_WRITE,
                      read_new_feature, write_new_feature, new_feature_value),

// 5. Update documentation in ble/README.md
```

#### 2. Adding New System States
```c
// 1. Extend the state enum in watering.h
typedef enum {
    WATERING_STATE_IDLE,
    WATERING_STATE_WATERING,
    WATERING_STATE_PAUSED,
    WATERING_STATE_ERROR_RECOVERY,
    WATERING_STATE_NEW_STATE  // Add new state
} watering_state_t;

// 2. Handle transitions in state machine
watering_error_t transition_to_state(watering_state_t new_state) {
    // Validate transition
    if (!is_valid_transition(system_state, new_state)) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    // Perform state-specific cleanup/setup
    switch (new_state) {
        case WATERING_STATE_NEW_STATE:
            // Initialize new state
            break;
        // ...other cases
    }
    
    system_state = new_state;
    bt_irrigation_system_status_update(new_state);
    return WATERING_SUCCESS;
}
```

#### 3. Adding New Configuration Parameters
```c
// 1. Add to watering_channel_t structure
typedef struct {
    char name[WATERING_CHANNEL_NAME_MAX_LEN];
    watering_event_t watering_event;
    bool auto_enabled;
    uint32_t new_parameter;  // Add new field
} watering_channel_t;

// 2. Update load/save functions in watering_config.c
// 3. Add Bluetooth interface if needed
// 4. Update default initialization
// 5. Document in SOFTWARE.md
```

### Debugging and Diagnostic Functions

The system includes several debugging aids:

#### 1. Console Commands
```c
// Add console commands for testing
void cmd_test_new_feature(const struct shell *shell, size_t argc, char **argv) {
    if (argc != 2) {
        shell_print(shell, "Usage: test_feature <param>");
        return;
    }
    
    int param = atoi(argv[1]);
    watering_error_t err = test_new_feature(param);
    
    if (err == WATERING_SUCCESS) {
        shell_print(shell, "Test passed");
    } else {
        shell_print(shell, "Test failed: %d", err);
    }
}

SHELL_CMD_REGISTER(test_feature, NULL, "Test new feature", cmd_test_new_feature);
```

#### 2. Diagnostic Reporting
```c
// Add to diagnostics characteristic
void update_diagnostics_for_new_feature(void) {
    // Update diagnostic counters
    diagnostic_data.feature_usage_count++;
    
    // Send notification if enabled
    bt_irrigation_diagnostics_update();
}
```

### Integration Testing Procedures

When adding significant features:

1. **Verify Core Functionality**:
   - Test basic watering operations still work
   - Verify scheduling continues to function
   - Check flow monitoring remains accurate

2. **Test Bluetooth Integration**:
   - Connect with multiple client types
   - Verify all notifications work correctly
   - Test error conditions and recovery

3. **Hardware Compatibility**:
   - Test with different valve types
   - Verify power consumption within limits
   - Check electromagnetic compatibility

4. **Long-term Stability**:
   - Run extended tests (24+ hours)
   - Monitor for memory leaks
   - Check for timing drift or issues

## Releasing

The release process is managed by maintainers:

1. **Version Numbers**:
   - We follow [Semantic Versioning](https://semver.org/)
   - Format: MAJOR.MINOR.PATCH

2. **Release Notes**:
   - All significant changes must be documented
   - Group changes by type: Features, Bug Fixes, etc.

3. **Release Approval**:
   - Releases require approval by at least two maintainers
   - All tests must pass on the release branch

## Getting Help

If you need help with your contribution:

- Ask in the related issue
- Contact the project maintainers
- Check the [Troubleshooting Guide](TROUBLESHOOTING.md)

Thank you for contributing to AutoWatering!

[Back to main README](../README.md)
