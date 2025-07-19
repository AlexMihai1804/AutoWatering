# Troubleshooting Guide

This guide provides solutions for common issues that may occur when setting up or using the AutoWatering system.

## Recent Fixes (January 2025)

### Memory Optimization and System Stability - FIXED ✅

**Previous Issue**: System experiencing lockup, freezing, or reset issues due to high RAM usage (96.47% of available memory).

**Resolution Applied**:
- **Memory Optimization**: Reduced RAM usage from 96.47% to 84.37% (31KB saved)
- **BLE Buffer Optimization**: Reduced TX/RX buffer counts from 10 to 6 each
- **Thread Stack Optimization**: Reduced main thread stack from 2048 to 1536 bytes
- **Runtime Memory Monitoring**: Added periodic memory usage reporting every 60 seconds
- **Stack Overflow Detection**: Added runtime stack usage monitoring for system stability

**Result**: System now operates stably with 84.37% RAM usage, preventing lockup issues and improving overall reliability.

### BLE Notification State Tracking - FIXED ✅

**Previous Issue**: BLE notifications sent even when client not subscribed, causing "Unable to allocate buffer" errors (-22).

**Resolution Applied**:
- **Subscription State Tracking**: Implemented proper CCC state tracking for all characteristics
- **Notification Gating**: Notifications only sent when client is properly subscribed
- **Error Prevention**: Eliminates unnecessary buffer allocation attempts
- **Enhanced Debugging**: Added logging for notification state changes

**Result**: BLE notifications now work reliably without buffer allocation errors.

### Advertising Restart After Disconnect - FIXED ✅

**Previous Issue**: Advertising did not restart reliably after BLE disconnect, making device undiscoverable.

**Resolution Applied**:
- **Robust Restart Logic**: Automatic advertising restart after BLE disconnect
- **Retry Mechanism**: Up to 3 retry attempts with 5-second intervals
- **Linear Backoff**: Intelligent retry timing to prevent resource exhaustion
- **Delayable Work**: Uses Zephyr's delayable work system for reliable scheduling

**Result**: Device now reliably restarts advertising after disconnect, remaining discoverable.

### BLE Buffer Allocation Errors - FIXED ✅

**Previous Issue**: System experiencing "Unable to allocate buffer within timeout" and "No buffer available to send notification" errors.

**Resolution Applied**:
- **Notification Throttling**: Implemented 100ms minimum delay between BLE notifications
- **Buffer Pool Increases**: Increased ACL, ATT, and L2CAP buffer pool sizes in firmware
- **Queue System**: Added automatic queuing and retry for failed notifications
- **Background Processing**: Dedicated worker thread handles notification overflow

**Result**: System now handles high-frequency BLE communication reliably without buffer overflow.

## System Stability Issues

### Memory Exhaustion and System Lockup

**Symptoms**: Device freezes, resets unexpectedly, or becomes unresponsive during operation.

**Solutions**:

1. **Memory Usage Monitoring** (Built-in as of v1.2.0):
   - System now reports memory usage every 60 seconds via debug log
   - Current RAM usage: 84.37% (216,920 bytes of 256KB)
   - Stack usage monitoring detects potential overflow conditions

2. **If Memory Issues Persist**:
   - Check for memory leaks in custom code
   - Review thread stack size requirements
   - Consider reducing BLE buffer counts further if needed

3. **System Reset Recovery**:
   - Device automatically recovers from watchdog resets
   - BLE advertising restarts automatically after system recovery
   - Configuration preserved in NVS storage

### High RAM Usage (Legacy Issue - Now Resolved)

**Previous State**: System used 96.47% of available RAM (248,192 bytes).

**Current State**: Optimized to 84.37% RAM usage (216,920 bytes).

**Optimizations Applied**:
- Reduced BLE TX/RX buffers from 10 to 6 each
- Optimized thread stack sizes across all threads
- Reduced log buffer from 2048 to 1024 bytes
- Tuned Zephyr kernel memory settings

**Monitoring**: Runtime memory reporting provides ongoing visibility into system health.

## Bluetooth/BLE Issues

### BLE Connection Problems

**Symptoms**: Cannot connect to device via Bluetooth, frequent disconnections, or missing characteristics.

**Solutions**:

1. **Check Device Discovery**:
   - Ensure the device is advertising (LED should be blinking during advertising)
   - Verify device name appears as "AutoWatering" in BLE scanner apps
   - Try restarting the device if not visible

2. **Connection Issues**:
   - **Buffer Overflow (FIXED)**: Previous "Unable to allocate buffer" errors resolved with throttling system
   - Clear Bluetooth cache on mobile device and retry connection
   - Ensure only one client is connected at a time (CONFIG_BT_MAX_CONN=1)

3. **Missing Characteristics**:
   - **Service Restoration (FIXED)**: Complete GATT service with all 15 characteristics restored
   - Verify client app is reading the correct service UUID: `12345678-1234-5678-1234-56789abcdef0`
   - Some clients cache service discoveries - clear app data or restart

4. **Notification Issues**:
   - **Reliability Improved**: Notification throttling prevents message loss during high-frequency updates
   - Enable notifications on required characteristics before expecting updates
   - Check that client is properly handling GATT_CCC_NOTIFY events

### BLE Performance Optimization

**For High-Frequency Monitoring**:
- The system automatically throttles notifications to prevent buffer overflow
- Real-time updates (flow, current task) are prioritized over less critical notifications
- Background thread ensures no notifications are lost, only delayed if necessary

**Connection Parameters**:
- Supervision timeout: 4 seconds
- Connection interval: Optimized for balance between power consumption and responsiveness
- Automatic parameter negotiation on connection

### Legacy BLE Issues (Now Resolved)

1. ~~**"Unable to allocate buffer within timeout"**~~ - **FIXED**: Notification throttling system prevents this error
2. ~~**Missing GATT service definition**~~ - **FIXED**: Complete service restored with all characteristics
3. ~~**Notification flooding**~~ - **FIXED**: Intelligent queuing prevents notification bursts from overwhelming buffers

## Hardware Issues

### Valves Not Activating

**Symptoms**: Valves don't open when tasks are running, or the system reports valve activation but no water flows.

**Possible Solutions**:

1. **Check Power Supply**:
   - Verify that your power supply provides sufficient current (at least 500mA per valve)
   - Measure the voltage at the relay inputs (should be 12V)

2. **Check Connections**:
   - Verify GPIO connections between the NRF52840 and relay module
   - Ensure the relay common terminal is connected to your power supply
   - Check for loose wires or bad connections

3. **Test Relay Operation**:
   ```bash
   # Use the test command mode to directly test a valve
   AutoWatering> test_valve 2 on
   # Wait a few seconds
   AutoWatering> test_valve 2 off
   ```

4. **Check Devicetree Configuration**:
   - Verify that the pin assignments in `boards/promicro_52840.overlay` match your wiring

### Flow Sensor Not Detecting Flow

**Symptoms**: Water is flowing but the system reports no flow or incorrect measurements.

**Possible Solutions**:

1. **Check Sensor Installation**:
   - Ensure the flow sensor is installed in the correct orientation (arrow pointing in flow direction)
   - Check that water is actually flowing through the sensor
   - Make sure the sensor is appropriate for your water pressure range

2. **Check Wiring**:
   - Verify sensor connections (red to 5V, black to GND, yellow to GPIO)
   - Check that the pull-up resistor is properly connected
   - Try a different GPIO pin and update the devicetree overlay

3. **Test Sensor Signal**:
   ```bash
   # Monitor flow pulses directly
   AutoWatering> flow_test
   # Should show pulse counts increasing when water flows
   ```

4. **Recalibrate the Sensor**:
   - Run the calibration procedure with a precisely measured water volume
   - Use a lower flow rate during calibration for more accurate results
   - See [Flow Calibration Instructions](#flow-sensor-calibration)

### RTC Issues

**Symptoms**: Scheduling doesn't work, time doesn't persist after power cycle, or wrong time.

**Possible Solutions**:

1. **Check I2C Connections**:
   - Verify SDA, SCL connections between DS3231 and NRF52840
   - Check for the correct I2C address (typically 0x68)

2. **Check RTC Battery**:
   - Measure the voltage of the RTC backup battery (should be above 2.5V)
   - Replace the CR2032 battery if depleted

3. **Reset and Synchronize RTC**:
   ```bash
   # Set the RTC time manually
   AutoWatering> rtc_set 2023 12 31 23 59 45
   # Or use the Bluetooth interface to set the time
   ```

4. **Verify RTC in I2C Bus**:
   ```bash
   # Scan I2C bus for devices
   AutoWatering> i2c_scan
   # Should show at least 0x68 for DS3231
   ```

## Software Issues

### Build Errors

**Symptoms**: Build fails with compilation errors.

**Possible Solutions**:

1. **Update Dependencies**:
   ```bash
   west update
   pip install -r requirements.txt
   ```

2. **Clean Build Directory**:
   ```bash
   west build -t clean
   ```

3. **Check Zephyr Version**:
   - Ensure you're using a compatible version of Zephyr (3.0 or newer)
   - Check that the west manifest points to the correct Zephyr version

4. **Check for Missing Configs**:
   - Verify that all required Kconfig options are enabled in `prj.conf`
   - Common missing configs: `CONFIG_BT`, `CONFIG_SETTINGS`, `CONFIG_GPIO`

### Flash Failures

**Symptoms**: Unable to flash the firmware to the board.

**Possible Solutions**:

1. **Check USB Connection**:
   - Try a different USB cable
   - Use a powered USB hub if power issues are suspected

2. **Reset the Board**:
   - Press the reset button twice quickly to enter bootloader mode
   - Check if the board appears as a USB storage device

3. **Specify the Correct Runner**:
   ```bash
   west flash --runner nrfjprog
   # or
   west flash --runner jlink
   ```

4. **Update Programmer Drivers**:
   - For J-Link: Download and install the latest J-Link software
   - For nRF: Update nRF Command Line Tools

### Runtime Issues

**Symptoms**: System crashes, freezes, or behaves unpredictably.

**Possible Solutions**:

1. **Monitor Debug Output**:
   ```bash
   west attach
   ```

2. **Check for Hardware Conflicts**:
   - Verify no pin conflicts in the devicetree overlay
   - Check for overlapping timer or interrupt usage

3. **Increase Stack Size**:
   - If stack overflow is indicated, increase sizes in `watering_tasks.c`:
   ```c
   #define WATERING_STACK_SIZE 3072  // Increase from 2048
   #define SCHEDULER_STACK_SIZE 2048  // Increase from 1024
   ```

4. **Enable More Detailed Logs**:
   - Update `prj.conf` to enable more verbose logging:
   ```
   CONFIG_LOG_DEFAULT_LEVEL=4
   CONFIG_LOG_BUFFER_SIZE=4096
   ```

### Bluetooth Connection Issues

**Symptoms**: Unable to connect to the device via Bluetooth, or connections drop frequently.

**Possible Solutions**:

1. **Check Bluetooth Status**:
   - Verify Bluetooth is enabled in firmware (`CONFIG_BT=y` in prj.conf)
   - Check if the device is advertising (use a BLE scanner app like nRF Connect)
   - Device should appear as "AutoWatering"

2. **Reset Bluetooth Stack**:
   ```bash
   # From the console (if available via USB)
   bt reset
   # Or use Task Queue command 4 via Bluetooth to clear errors
   ```

3. **Check for Interference**:
   - Move away from WiFi routers and other sources of 2.4GHz interference
   - Try in a different location
   - Ensure no other devices are connected (supports only 1 connection)

4. **MTU Issues**:
   - For web browsers: Use fragmented writes for large structures
   - For mobile apps: Negotiate larger MTU after connection
   - See [BLE Documentation](ble/README.md) for MTU handling

5. **Connection Parameters**:
   - Use recommended connection parameters (30-50ms interval)
   - Enable notifications before expecting real-time updates

## System Operation Issues

### No-Flow Detection

**Symptoms**: System reports "No flow detected" errors when a valve is activated (status code 1).

**Possible Solutions**:

1. **Check Water Supply**:
   - Verify that the water main is turned on
   - Check for closed valves upstream of the irrigation system
   - Ensure adequate water pressure (typically 15-45 PSI)

2. **Check for Blockages**:
   - Inspect filters for debris
   - Check for kinked hoses or blocked irrigation lines
   - Verify flow sensor is not obstructed

3. **Flow Sensor Calibration**:
   - Recalibrate the flow sensor using the Bluetooth interface
   - Use the calibration characteristic (UUID …efb)
   - Follow the procedure in [Flow Calibration](#flow-sensor-calibration)

4. **Clear Error State**:
   ```bash
   # Use Task Queue command 4 to clear error states
   # This can be done via Bluetooth or console
   ```

### Unexpected Flow Errors

**Symptoms**: System reports unexpected flow when no valves should be active (status code 2).

**Possible Solutions**:

1. **Check for Leaks**:
   - Inspect the irrigation system for leaks or damaged pipes
   - Verify that all valves fully close when deactivated
   - Check valve seals and replace if worn

2. **Check for Valve Failure**:
   - Test each valve individually to ensure it closes properly
   - Look for signs of debris preventing full valve closure
   - Verify relay operation (should show 0V when off)

3. **Adjust Detection Sensitivity**:
   - The system uses a threshold to detect unexpected flow
   - Small leaks might trigger false alarms
   - Consider adjusting system sensitivity if needed

4. **Clear Error State**:
   - Use Bluetooth Task Queue command 4 to reset error conditions
   - Monitor system status via the Status characteristic (UUID …ef3)

### Cannot Add Tasks After Error Clearing

**Symptoms**: After clearing an alarm/error, new tasks cannot be added to the system.

**Root Cause**: The system remained in fault state even after alarm clearing (before firmware fix).

**Solutions**:

1. **Clear Alarms via BLE (Recommended)**:
   ```python
   # Clear all alarms - this automatically resets system status
   clear_command = struct.pack("<B", 0)  # 0 = clear all
   await client.write_gatt_char(ALARM_UUID, clear_command)
   ```

2. **Use Task Queue Error Clear Command**:
   ```python
   # Alternative method using task queue command 4
   command_data = struct.pack("<B", 4) + b'\x00' * 14
   await client.write_gatt_char(TASK_QUEUE_UUID, command_data)
   ```

3. **Verify System Status Reset**:
   ```python
   # Check that system status changed from FAULT (3) to OK (0)
   status_data = await client.read_gatt_char(STATUS_UUID)
   status_code = status_data[0]
   print(f"System status: {status_code}")  # Should be 0 (OK)
   ```

**Note**: The firmware automatically calls `watering_clear_errors()` when alarms are cleared, which:
- Resets system status from FAULT to OK
- Clears all error counters
- Enables normal task addition and operation

### Task Scheduling Issues

**Symptoms**: Scheduled tasks don't execute at the expected time.

**Possible Solutions**:

1. **Verify RTC Time**:
   - Check current time via Bluetooth RTC characteristic (UUID …ef9)
   - Synchronize time using a mobile app or Python script
   - Ensure RTC battery is functional (DS3231 backup battery)

2. **Check Schedule Configuration**:
   - Verify schedule using the Schedule Config characteristic (UUID …ef5)
   - Ensure `auto_enabled` flag is set to 1
   - Check day mask for daily schedules (bit 0=Sunday, 1=Monday, etc.)

3. **Schedule Format Verification**:
   - For daily schedules: Use bitmap format (0x3E = Monday-Friday)
   - For periodic schedules: Use interval in days (e.g., 3 = every 3 days)
   - Time format: 24-hour format (hour 0-23, minute 0-59)

4. **Check System Status**:
   - Ensure system is not in fault state (status code 3)
   - Clear any error conditions using Task Queue command 4
   - Monitor system status via Bluetooth notifications

**Example Schedule Configuration (Python)**:
```python
# Monday-Friday at 07:00 for 10 minutes
schedule_data = struct.pack("<6BHB", 
    channel_id,  # 0-7
    0,           # schedule_type (0=Daily)
    0x3E,        # days_mask (Monday-Friday)
    7,           # hour
    0,           # minute
    0,           # watering_mode (0=duration)
    10,          # value (10 minutes)
    1            # auto_enabled
)
```

<a id="flow-sensor-calibration"></a>
## Flow Sensor Calibration

For accurate volume measurement, calibrate the flow sensor using the Bluetooth interface:

### Method 1: Using Bluetooth (Recommended)

1. **Start Calibration**:
   ```python
   # Using Python with Bleak
   import struct
   # Start calibration: action=1, other fields=0
   data = struct.pack("<B3I", 1, 0, 0, 0)
   await client.write_gatt_char(CALIBRATION_UUID, data, response=True)
   ```

2. **Measure Precise Volume**:
   - Use a measuring container with known volume (1 liter recommended)
   - Activate a valve and collect exactly the measured amount
   - Stop valve when container is full

3. **Complete Calibration**:
   ```python
   # Stop calibration and provide volume in ml
   data = struct.pack("<B3I", 0, 0, 1000, 0)  # 1000ml = 1 liter
   await client.write_gatt_char(CALIBRATION_UUID, data, response=True)
   ```

4. **Read Result**:
   ```python
   # Read calibration result
   data = await client.read_gatt_char(CALIBRATION_UUID)
   action, pulses, volume, result = struct.unpack("<B3I", data)
   print(f"New calibration: {result} pulses/liter")
   ```

### Method 2: Manual Calculation

If Bluetooth is not available:

1. **Reset Pulse Counter**:
   - Use flow monitoring to track pulse count from zero

2. **Measure Water**:
   - Collect exactly 1 liter of water through the flow sensor
   - Note the total pulse count

3. **Calculate Calibration**:
   - pulses_per_liter = total_pulses_counted
   - Update system configuration with this value

**Note**: The system automatically saves the new calibration to non-volatile storage.

## Growing Environment Configuration Issues

### Plant Configuration Not Saving

**Symptoms**: Growing environment settings reset after reboot or don't persist.

**Possible Solutions**:

1. **Check NVS Storage**:
   ```c
   // Test NVS functionality
   watering_error_t result = watering_set_plant_type(0, PLANT_TYPE_TOMATO);
   if (result != WATERING_SUCCESS) {
       printk("NVS write failed: %d\n", result);
   }
   
   // Verify by reading back
   watering_plant_type_t plant_type;
   result = watering_get_plant_type(0, &plant_type);
   assert(plant_type == PLANT_TYPE_TOMATO);
   ```

2. **Check Storage Space**:
   - Ensure NVS partition has sufficient space
   - Use diagnostics to check storage status

3. **Validate Parameters**:
   ```c
   // All parameters must be within valid ranges
   assert(plant_type < PLANT_TYPE_COUNT);
   assert(soil_type < SOIL_TYPE_COUNT);
   assert(irrigation_method < IRRIGATION_METHOD_COUNT);
   assert(sun_percentage <= 100);
   ```

### BLE Growing Environment Communication Failures

**Symptoms**: Cannot read/write growing environment via Bluetooth, or data corruption.

**Possible Solutions**:

1. **Check MTU Size**:
   ```python
   # Growing environment requires 50 bytes minimum
   mtu = await client.get_mtu()
   if mtu < 53:  # 50 bytes + 3 ATT overhead
       print("MTU too small, request larger MTU")
       await client.request_mtu(250)
   ```

2. **Validate Data Structure**:
   ```python
   # Ensure correct packing format
   data = struct.pack("<5B f H B 32s f 2B",
       channel_id,           # uint8_t (1 byte)
       plant_type,           # uint8_t (1 byte)  
       soil_type,            # uint8_t (1 byte)
       irrigation_method,    # uint8_t (1 byte)
       use_area_based,       # uint8_t (1 byte)
       area_m2,              # float (4 bytes)
       plant_count,          # uint16_t (2 bytes) - overlays area
       sun_percentage,       # uint8_t (1 byte)
       custom_name.encode('utf-8').ljust(32, b'\0'),  # 32 bytes
       water_need_factor,    # float (4 bytes)
       irrigation_freq_days, # uint8_t (1 byte)
       prefer_area_based     # uint8_t (1 byte)
   )
   assert len(data) == 47
   ```

3. **Test Individual Components**:
   ```python
   # Test channel selection first
   await client.write_gatt_char(GROWING_ENV_UUID, bytes([channel_id]))
   
   # Then test reading
   data = await client.read_gatt_char(GROWING_ENV_UUID)
   if len(data) != 47:
       print(f"Invalid data length: {len(data)}, expected 47")
   ```

### Custom Plant Configuration Issues

**Symptoms**: Custom plant names not displaying, water factors not applied correctly.

**Possible Solutions**:

1. **Check String Encoding**:
   ```python
   # Ensure UTF-8 encoding and proper null termination
   custom_name = "Hibiscus rosa-sinensis"
   name_bytes = custom_name.encode('utf-8')
   if len(name_bytes) > 31:
       name_bytes = name_bytes[:31]  # Truncate if too long
   name_padded = name_bytes + b'\0' * (32 - len(name_bytes))
   ```

2. **Validate Water Factors**:
   ```c
   // Water need factor must be in valid range
   if (custom_config->water_need_factor < 0.5f || 
       custom_config->water_need_factor > 3.0f) {
       return WATERING_ERROR_INVALID_PARAM;
   }
   ```

3. **Check Plant Type Selection**:
   ```c
   // Custom plants must use PLANT_TYPE_CUSTOM
   if (plant_type == PLANT_TYPE_CUSTOM) {
       // Custom configuration fields are used
   } else {
       // Built-in plant configuration applies
   }
   ```

### Irrigation Method Validation Warnings

**Symptoms**: System warns about irrigation method and coverage combination.

**Solutions**:

1. **Understand Recommendations**:
   - **Drip/Micro Spray**: Better with plant count measurement
   - **Sprinkler/Soaker/Flood**: Better with area measurement
   - **Subsurface**: Usually area-based

2. **Override When Appropriate**:
   ```c
   // You can override recommendations if your setup is different
   bool valid = watering_validate_coverage_method(
       IRRIGATION_METHOD_DRIP, true  // drip + area
   );
   // valid = false (warning), but still allowed
   ```

3. **Test Water Distribution**:
   - Run short test cycles to verify coverage
   - Adjust based on actual plant response

### Environment Data Integration Issues

**Symptoms**: Water factors not affecting irrigation amounts, scheduling not considering environment.

**Solutions**:

1. **Check Integration Status**:
   ```c
   // Verify environment data is being used
   float factor = watering_get_water_factor(plant_type, custom_config);
   printk("Water factor for channel %d: %.2f\n", channel_id, factor);
   ```

2. **Monitor Applied Calculations**:
   - Use diagnostics to see calculated watering amounts
   - Check if sun percentage affects frequency
   - Verify soil type impacts duration

3. **Validate API Integration**:
   ```c
   // Test complete environment configuration
   watering_error_t result = watering_set_channel_environment(
       channel_id, plant_type, soil_type, irrigation_method,
       use_area_based, area_m2, plant_count, sun_percentage,
       custom_config
   );
   
   if (result != WATERING_SUCCESS) {
       printk("Environment setup failed: %d\n", result);
   }
   ```

## Factory Reset

If you need to reset the system to factory defaults:

1. **Reset Configuration**:
   ```bash
   # Warning: This will erase all schedules and settings
   AutoWatering> factory_reset
   ```

2. **Manual Flash Erasure**:
   - If the system is unresponsive, you can erase the settings partition:
   ```bash
   west flash --erase
   ```

3. **Rebuild and Flash**:
   ```bash
   west build -t clean
   west build -b nrf52840_promicro
   west flash
   ```

## Getting Support

If you continue to experience issues:

1. **Check Documentation**:
   - Review the [BLE Documentation](ble/README.md) for interface issues
   - Check the [Hardware Guide](HARDWARE.md) for wiring problems
   - Consult the [Software Guide](SOFTWARE.md) for configuration questions

2. **Generate System Information**:
   - Use Bluetooth Diagnostics characteristic (UUID …efd) to get system info
   - Check system status and error counts
   - Monitor real-time status via notifications

3. **GitHub Issues**:
   - Search for similar issues on the [GitHub repository](https://github.com/AlexMihai1804/AutoWatering/issues)
   - Create a new issue with detailed information if needed
   - Include system diagnostics and reproduction steps

4. **Community Support**:
   - Check project discussions for common solutions
   - Share your experience and solutions with others

## Documentation Version

This troubleshooting guide is current as of June 2025 and covers firmware version 1.6 with updated error codes and Bluetooth diagnostics.

[Back to main README](../README.md)

## Bluetooth and History Issues

### Manual Tasks Not Appearing in History

**Symptoms**: Manual watering tasks created via Bluetooth are not visible in web applications or are shown as "remote" tasks instead of "manual" tasks.

**Root Cause**: This was a firmware issue where all Bluetooth-initiated tasks were incorrectly categorized as remote tasks instead of manual tasks.

**Solution (Fixed in Firmware v1.6+)**:

The issue has been resolved through multiple improvements:

1. **Trigger Type Tracking**: Tasks now properly track how they were initiated (manual, scheduled, or remote)
2. **Correct BLE Structure**: Fixed Bluetooth history structure to include all necessary fields
3. **Proper Categorization**: Manual tasks via Bluetooth are correctly recorded as `WATERING_TRIGGER_MANUAL`

**For Updated Firmware**:
- Manual tasks via Bluetooth are shown with green highlighting in web applications
- Scheduled tasks appear in blue
- Remote tasks (if any) appear in purple
- Use the provided `history_viewer_web.html` for visual verification

**For Older Firmware**:
If you cannot update firmware immediately:
- Manual tasks will still function correctly, they're just mis-categorized
- Look for tasks with `trigger_type = 2` in the history data
- These are actually your manual tasks, despite being labeled as "remote"

**Verification Steps**:
1. Flash firmware v1.6 or later
2. Open `history_viewer_web.html` in Chrome/Edge browser
3. Connect to your AutoWatering device
4. Create a manual task (e.g., 2-minute duration on any channel)
5. Check history - task should appear with "Manual" badge and green highlighting

**Related Files**:
- `history_viewer_web.html` - Web application for viewing categorized history
- Corrected history characteristic UUID: `12345678-1234-5678-1234-56789abcdefc`

### Bluetooth History Structure Mismatch

**Symptoms**: Web applications cannot parse history data correctly, showing empty or garbled history events.

**Root Cause**: The Bluetooth service history structure was missing fields that are present in the actual history system.

**Solution**: 
The detailed event structure now includes all necessary fields:
```c
struct {
    uint32_t timestamp;
    uint8_t channel_id;      // Channel that performed the watering
    uint8_t event_type;      // START/COMPLETE/ABORT/ERROR
    uint8_t mode;            // DURATION/VOLUME
    uint16_t target_value;
    uint16_t actual_value;
    uint16_t total_volume_ml;
    uint8_t trigger_type;    // 0=manual, 1=scheduled, 2=remote
    uint8_t success_status;  // 0=failed, 1=success, 2=partial
    uint8_t error_code;
    uint16_t flow_rate_avg;
    uint8_t reserved[2];     // For alignment
} detailed;
```

**For Web Developers**:
- Updated structure size is 24 bytes (previously 22 bytes)
- Parse `channel_id` and `event_type` fields for complete event information
- Use correct history UUID: `…efc` not `…ef8`

## BLE Protocol Issues

### Large Data Fragmentation (Channel Configuration & Growing Environment)

**Symptoms**: Writes to Channel Configuration or Growing Environment characteristics fail, appear to succeed but don't update the device, or only partially write data.

**Root Cause**: These characteristics use large data structures (76 bytes for Channel Config, 50 bytes for Growing Environment) that exceed the 20-byte BLE MTU limit and require fragmentation protocols.

**Solutions**:

1. **Use Proper Fragmentation for Channel Configuration**:
   ```python
   # Channel Configuration uses little-endian fragmentation
   # Structure: 76 bytes total
   def write_channel_config_fragmented(client, channel_id, config_data):
       # Step 1: Select channel (1 byte)
       await client.write_gatt_char(CHANNEL_CONFIG_UUID, bytes([channel_id]))
       
       # Step 2: Send fragmented data
       data_size = len(config_data)  # Should be 76 bytes
       size_bytes = struct.pack("<H", data_size)  # Little-endian
       
       # First fragment: [frag_type=2][size_low][size_high][data...]
       first_fragment = bytes([2]) + size_bytes + config_data[:17]
       await client.write_gatt_char(CHANNEL_CONFIG_UUID, first_fragment)
       
       # Subsequent fragments: [frag_type=2][data...]
       offset = 17
       while offset < len(config_data):
           fragment_data = config_data[offset:offset+19]
           fragment = bytes([2]) + fragment_data
           await client.write_gatt_char(CHANNEL_CONFIG_UUID, fragment)
           offset += 19
   ```

2. **Use Proper Fragmentation for Growing Environment**:
   ```python
   # Growing Environment uses big-endian fragmentation and different protocol
   # Structure: 50 bytes total
   def write_growing_env_fragmented(client, channel_id, env_data):
       # No channel selection - uses last written channel_id
       data_size = len(env_data)  # Should be 50 bytes
       size_high = (data_size >> 8) & 0xFF
       size_low = data_size & 0xFF
       
       # First fragment: [channel_id][frag_type=2][size_high][size_low][data...]
       first_fragment = bytes([channel_id, 2, size_high, size_low]) + env_data[:16]
       await client.write_gatt_char(GROWING_ENV_UUID, first_fragment)
       
       # Subsequent fragments: [channel_id][frag_type=2][data...]
       offset = 16
       while offset < len(env_data):
           fragment_data = env_data[offset:offset+18]
           fragment = bytes([channel_id, 2]) + fragment_data
           await client.write_gatt_char(GROWING_ENV_UUID, fragment)
           offset += 18
   ```

**Common Mistakes**:
- Using the same fragmentation protocol for both characteristics (they're different!)
- Mixing up endianness (Channel Config: little-endian, Growing Env: big-endian)
- Not sending channel selection for Channel Configuration
- Trying to send channel selection for Growing Environment (not supported)
- Exceeding 20-byte MTU without fragmentation

### Channel Selection Issues

**Symptoms**: Reading Channel Configuration returns data for wrong channel, or Growing Environment reads return unexpected data.

**Solutions**:

1. **Channel Configuration - Explicit Selection Required**:
   ```python
   # Always select channel before reading
   await client.write_gatt_char(CHANNEL_CONFIG_UUID, bytes([channel_id]))
   # Now read returns data for the selected channel
   data = await client.read_gatt_char(CHANNEL_CONFIG_UUID)
   ```

2. **Growing Environment - Implicit Selection**:
   ```python
   # No explicit selection protocol - uses last written channel
   # To read channel 3, you must have previously written to channel 3
   # Or write an empty/minimal update to set the channel:
   minimal_data = bytes(50)  # 50-byte structure with zeros
   await write_growing_env_fragmented(client, 3, minimal_data)
   # Now reads will return data for channel 3
   data = await client.read_gatt_char(GROWING_ENV_UUID)
   ```

**Warning**: Growing Environment does NOT have a 1-byte channel selection protocol like Channel Configuration. Attempting to write a single byte will be interpreted as the start of fragmented data.

### MTU Negotiation Issues

**Symptoms**: Fragmentation fails even when implemented correctly.

**Solutions**:

1. **Check MTU Size**:
   ```python
   # In most BLE implementations, check current MTU
   current_mtu = await client.get_mtu()
   print(f"Current MTU: {current_mtu}")
   # Should be 23 (20 bytes data + 3 bytes overhead) for standard BLE
   ```

2. **Account for ATT Overhead**:
   - Total MTU includes 3 bytes of ATT protocol overhead
   - For writing: 20 bytes maximum data per operation
   - Fragmentation protocols account for this automatically

### Data Structure Size Mismatches

**Symptoms**: Partial data writes, unexpected behavior after configuration updates.

**Verification**:
```python
# Verify expected structure sizes
CHANNEL_CONFIG_SIZE = 76  # bytes
GROWING_ENV_SIZE = 50     # bytes

def verify_structure_size(data, expected_size, name):
    if len(data) != expected_size:
        raise ValueError(f"{name} data must be exactly {expected_size} bytes, got {len(data)}")
```

**Solution**: Always ensure your data structures match the exact sizes expected by the firmware:
- Channel Configuration: 76 bytes (verified by manual structure layout)
- Growing Environment: 50 bytes (verified by manual structure layout)

### BLE Service Discovery Issues

**Symptoms**: Cannot find characteristics, service appears incomplete.

**Solutions**:

1. **Complete Service UUID**: `12345678-1234-5678-1234-56789abcdef0`

2. **Verify All Characteristics Present**:
   - 15 total characteristics should be discovered
   - Check that both Channel Configuration and Growing Environment UUIDs are found
   - Some BLE clients cache service information - clear cache if missing characteristics

**Related Documentation**: See `BLE_DOCUMENTATION_COMPLETE.md` for complete protocol specifications and examples.
