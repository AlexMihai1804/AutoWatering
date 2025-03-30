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
   - Check if the device is advertising (use a BLE scanner app)

2. **Reset Bluetooth Stack**:
   ```bash
   # From the console
   AutoWatering> bt_reset
   ```

3. **Check for Interference**:
   - Move away from WiFi routers and other sources of 2.4GHz interference
   - Try in a different location

4. **Update Client Application**:
   - Ensure your BLE client app is up-to-date
   - Try a different BLE client for testing (e.g., nRF Connect)

## System Operation Issues

### No-Flow Detection

**Symptoms**: System reports "No flow detected" errors when a valve is activated.

**Possible Solutions**:

1. **Check Water Supply**:
   - Verify that the water main is turned on
   - Check for closed valves upstream of the irrigation system

2. **Check for Blockages**:
   - Inspect filters for debris
   - Check for kinked hoses or blocked irrigation lines

3. **Adjust Flow Detection Parameters**:
   ```bash
   # Increase the flow detection timeout (in seconds)
   AutoWatering> config set flow_timeout 10
   # Adjust the minimum flow threshold
   AutoWatering> config set min_flow_rate 5
   ```

4. **Calibrate Flow Sensor**:
   - Follow the calibration procedure in the [Flow Calibration](#flow-sensor-calibration) section

### Unexpected Flow Errors

**Symptoms**: System reports unexpected flow when no valves should be active.

**Possible Solutions**:

1. **Check for Leaks**:
   - Inspect the irrigation system for leaks or damaged pipes
   - Verify that all valves fully close when deactivated

2. **Adjust Flow Threshold**:
   ```bash
   # Increase the unexpected flow threshold
   AutoWatering> config set unexpected_flow_threshold 20
   ```

3. **Check for Valve Failure**:
   - Test each valve individually to ensure it closes properly
   - Look for signs of debris preventing full valve closure

### Task Scheduling Issues

**Symptoms**: Scheduled tasks don't execute at the expected time.

**Possible Solutions**:

1. **Verify RTC Time**:
   ```bash
   # Check current RTC time
   AutoWatering> rtc_status
   ```

2. **Check Schedule Configuration**:
   ```bash
   # Print schedule for a specific channel
   AutoWatering> channel 2 show_schedule
   ```

3. **Enable Auto Scheduling**:
   - Make sure the auto scheduling flag is enabled for the channel
   ```bash
   AutoWatering> channel 2 set_auto_enabled 1
   ```

4. **Check Day/Time Settings**:
   - For daily schedules, verify the day of week is correct
   - Check that hour/minute values are in 24-hour format

<a id="flow-sensor-calibration"></a>
## Flow Sensor Calibration

For accurate volume measurement, calibrate the flow sensor:

1. **Prepare a Measuring Container**:
   - Get a container with precise volume measurements (1 liter or more)

2. **Start Calibration Mode**:
   ```bash
   # Via console
   AutoWatering> flow_calibrate start
   # Or via Bluetooth using the Flow Calibration characteristic
   ```

3. **Measure Water**:
   - Activate a valve and fill the container to exactly 1 liter
   - Turn off the valve once the container is filled

4. **Complete Calibration**:
   ```bash
   # Enter the precise volume in milliliters
   AutoWatering> flow_calibrate stop 1000
   # System will calculate the pulses per liter
   ```

5. **Save Configuration**:
   ```bash
   AutoWatering> config save
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

1. **Generate System Report**:
   ```bash
   AutoWatering> system_report
   # Save the output for support
   ```

2. **Check GitHub Issues**:
   - Search for similar issues on the [GitHub repository](https://github.com/AlexMihai1804/AutoWatering/issues)
   - Create a new issue with detailed information if needed

3. **Contact Maintainers**:
   - Email the project maintainers with your system report
   - Include details about your hardware setup and the issue symptoms

[Back to main README](../README.md)
