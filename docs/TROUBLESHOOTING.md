# Troubleshooting Guide

This guide provides solutions for common issues that may occur when setting up or using the AutoWatering system.

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
   - See [Bluetooth API Documentation](BLUETOOTH.md) for MTU handling

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
   - Review the [Bluetooth API Documentation](BLUETOOTH.md) for interface issues
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
