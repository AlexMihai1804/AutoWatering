# AutoWatering Troubleshooting (October 2025)# Troubleshooting (Implementation Scope)



This guide covers the issues that occur in the current firmware and how to diagnose them. It intentionally omits historical claims (automatic scoring, cloud sync, WSL tooling, etc.) that are not present in the code base.This file was reduced to issues relevant to the current firmware. Removed: configuration scoring, automatic compensation logic, custom soil hydraulic validation, multi-year analytics, task queue background scheduler threads, advanced notification buffer tuning, extended WSL build env recipes (refer to project README for environment setup).



## Quick Reference## Quick Reference

| Symptom | Likely Cause | Action |

| Symptom | Probable Cause | What to Check ||---------|--------------|--------|

|---------|----------------|----------------|| Alarm code 1 (No Flow) | Valve open but no pulses | Check flow sensor wiring; verify calibration >0; ensure water supply on |

| System Status = `NO_FLOW` | Valve opened but no pulses detected | Flow wiring, `flow_calibration_pulses_per_liter`, water supply || Alarm code 2 (Unexpected Flow) | Pulses with all valves closed | Inspect for leak; stuck valve; reset after fix |

| System Status = `UNEXPECTED_FLOW` | Pulses while all valves closed | Stuck valve, leak, master valve overlap too long || Interval mode never pauses | Interval not configured / wrong mode | Write interval config with configured=1 and select interval mode |

| Rain data always zero | No rain pulses captured | Wiring, debounce (`rain_sensor_configure`), simulate pulse || BLE write rejected | Invalid length or out-of-range field | Recheck struct size & field ranges (see characteristic docs) |

| Interval mode never pauses | Interval config missing/invalid | `interval_timing_is_configured`, ensure durations > 0 || Rain totals always zero | No pulses detected | Confirm wiring & debounce value; manually short pulses to test |

| BLE write rejected (0x0D) | Payload size mismatch | Send full struct length (see `docs/ble-api/` characteristic sizes) || Environmental snapshot invalid flags | BME280 absent or init failed | Verify I2C wiring (SCL P0.31 / SDA P0.29) |> restart |

| Automatic task skipped instantly | Rain integration flagged skip | Check Rain Integration Status characteristic and rainfall totals || Time drift or RTC error | RTC not responding | Check DS3231 power & I2C; reapply RTC config characteristic |

| RTC timestamp incorrect | DS3231 offline | Re-run RTC Config characteristic; confirm module powered |

## Core Areas

## Flow & Alarm Handling

### 1. Flow / Alarms

- Alarms 1 and 2 map to `WATERING_STATUS_NO_FLOW` and `WATERING_STATUS_UNEXPECTED_FLOW`.Only two runtime alarm codes are emitted now:

- Clear alarms via the Alarm Status characteristic after the root cause is fixed.- 1 NO_FLOW: Valve opened; pulses did not appear inside internal window.

- Verify pulses with the Flow Sensor characteristic; it returns raw counts and last pulse timestamp.- 2 UNEXPECTED_FLOW: Pulses detected while all valves closed.

- Calibration must be non-zero; the default is 750 pulses/litre (`flow_sensor_get_calibration`).

Clear procedure: Write the clear opcode to Alarm Status characteristic after resolving cause (does not suppress future alarms).

## Rain Sensor & Integration

Checklist:

- The rain gauge uses interrupt pulses; debounce defaults to 50 ms. Bounces shorter than the debounce window are ignored.1. Inspect flow sensor wires (VCC, GND, signal on expected GPIO).

- Use Rain Sensor Data characteristic to confirm `today_total_mm` increments when pulses are injected.2. Trigger manual watering; confirm pulse counter increases.

- Rain integration applies reductions/skips inside `watering_start_task()`; the Rain Integration Status characteristic exposes the per-channel reduction percentage.3. If pulses extremely low, verify calibration constant (avoid zero) and water supply pressure.

- Skip behaviour currently returns `WATERING_ERROR_BUSY`; enqueueing a replacement task is the caller's responsibility.

### 2. Interval Mode

## Environmental DataSymptoms: Continuous watering or immediate stop.

Causes:

- Environmental snapshots come from the BME280 (`env_sensors.c`). If `valid_flags` are zero, the sensor failed to initialise.- Missing `configured` flag.

- Ensure I^2C bus (default label `i2c0`) is ready and the sensor address (0x77 default) is correct. Initialisation is performed by `sensor_manager_init_bme280`.- Zero durations.

- Temperature compensation requires valid environmental data; otherwise, automatic watering falls back to the unadjusted FAO-56 result.Actions:

1. Set watering and pause < 60 min each, seconds < 60.

## Scheduling & Interval Mode2. Ensure chosen mode enum corresponds to interval mode before starting task.

3. Observe Current Task Status: phase changes W=watering / P=pausing.

- Interval support only runs when `interval_config.configured = 1` and durations are non-zero.

- Observe the Current Task Status characteristic: `phase` toggles between watering and pausing when interval mode is active.### 3. Rain Sensor (Optional)

- Daily/periodic schedules depend on RTC uptime; if `RTC_ERROR` appears, they will not auto-trigger until time is restored.Minimum supported behavior: pulse counting + basic history commands.

If no data:

## Master Valve- Check debounce not too short (<10ms) causing bounce rejection.

- Manually create pulses; verify Rain Sensor Data characteristic increments today_total.

- Master valve logic lives in `valve_control.c`. Configurable fields (pre-delay, post-delay, overlap grace, auto/manual) are in the System Configuration characteristic.- Confirm mm_per_pulse within valid range (rejects out-of-range writes).

- When diagnosing overlap: check the log for `master_valve` messages and ensure the overlap grace is long enough for back-to-back tasks.

### 4. Environmental Data

## BLE InteractionIf all fields invalid:

- BME280 not present or init failed; wiring, address (0x76/0x77) or supply.

- Only one central can connect at a time (`CONFIG_BT_MAX_CONN=1`). Disconnect unused clients before pairing a new device.Environmental packets expose temperature, humidity, and pressure only; gas metrics are not provided by the BME280.

- Fragmented characteristics use a 3-byte header (`fragment_id`, `flags`, `payload_length`). Follow the fragment guide in `docs/ble-api/fragmentation-guide.md`.

- Notification cadence is enforced by `SMART_NOTIFY`/`CRITICAL_NOTIFY`. Task status pushes every ~2 s while a task is active; other notifications respect >=500 ms spacing.### 5. Time / RTC

- CCC values are stored via Zephyr settings; clearing bonding data requires clearing the Zephyr settings partition or issuing a BLE `Clear Bonds` procedure.If scheduling or timestamps off (where used):

1. Reconfigure RTC characteristic with correct UTC time.

## Persistence & History2. Verify timezone config if local conversions needed elsewhere.

3. Hardware: DS3231 crystal & battery present.

- Configuration writes use `nvs_config.c`; data loss typically indicates NVS init failure (logged at boot). If NVS fails to mount, the system halts.

- Watering history keeps the last 30 events per channel (`DETAILED_EVENTS_PER_CHANNEL`). Use the History Control characteristic to page through entries.### 6. BLE Interaction

- Insights data exist (`history_insights_update`) but are not generated automatically; expect zeroed structures unless written externally.Common write failures = ATT error due to wrong payload size. Always send full struct for configuration characteristics (partial writes unsupported). Avoid fragmented custom attempts unless following documented fragmentation header.



## RTC & TimeIf notifications seem slow: Normal characteristics throttle to >=500ms; task status only while running (2s cadence). This is expected.



- `rtc.c` falls back to uptime if the DS3231 is missing. Current fallback date is 2023-12-10.### 7. Build / Environment (Abbreviated)

- Reapply the RTC Config characteristic after replacing hardware; reads should show UTC.For full environment setup consult INSTALLATION.md. Quick checks:

- Timezone configuration is persisted (offset + DST); ensure the Timezone characteristic matches your locality before scheduling (DST rules applied automatically).- west available (`west --version`).

- Zephyr SDK or toolchain in PATH.

## Build & Flash Diagnostics- Pristine build if unexplained compile errors: `west build -p always ...`.



- Use the steps in `docs/INSTALLATION.md`. Quick reminders:### 8. Recovery

  - Create/update workspace: `west init -m https://github.com/AlexMihai1804/AutoWatering.git` followed by `west update`.Soft reset: Use reset control characteristic (two-step confirm) if present, or power-cycle device.

  - Install Python requirements: `pip3 install --user -r zephyr/scripts/requirements.txt`.Clear stuck valve: Disable watering, depower relay module briefly.

  - Build for nRF52840 Pro Micro: `west build -b nrf52840_promicro --pristine`.

  - Native simulation: `west build -b native_sim --pristine -- -DEXTRA_DTC_OVERLAY_FILE=boards/native_sim.overlay`.### 9. When to Ignore Earlier Docs

  - Flash: `west flash` (optionally `--runner jlink`).

- If a build fails unexpectedly, run a pristine rebuild (`west build -p always ...`).

## Minimal Diagnostic Steps

1. Read System Status & Alarm Status.

2. Read Current Task Status while watering.

1. Read System Status, Alarm Status, and Current Task Status characteristics.3. Read Flow Sensor & Rain Data snapshots.

2. For active tasks, watch Flow Sensor pulses and ensure `current_value` trends toward the target.4. If hardware suspected: Inspect wiring, then rebuild, then reflash.

3. Verify rain totals and integration status before automatic tasks to understand reductions/skips.

4. If persistence issues occur, inspect boot logs for NVS mount failures and invoke `nvs_storage_monitor_get_stats` (via BLE) to assess wear.End of simplified troubleshooting guide.

5. When adding new clients, rediscover services to pick up characteristic changes.

## Common Issues

## When Further Help Is Needed

### Issue: System Status Shows Error Conditions

- Cross-reference module behaviour in `docs/system-architecture.md`.```

- Record Zephyr logs (USB CDC or RTT) while reproducing the issue.Error: System status indicates fault conditions

- Capture BLE interaction traces (nRF Connect, Ellisys, etc.) when debugging characteristic writes or notifications.

**Symptoms:**
- System status shows FAULT, RTC_ERROR, or LOW_POWER
- Automatic watering is disabled
- BLE notifications indicate errors

**Solutions:**
```c
// Check system status via BLE
uint8_t status = read_system_status_characteristic();
switch(status) {
    case ENHANCED_STATUS_FAULT:
        // Check error logs and sensor connections
        break;
    case ENHANCED_STATUS_RTC_ERROR:
        // Verify RTC connection and time synchronization
        break;
    case ENHANCED_STATUS_LOW_POWER:
        // Check power supply and connections
        break;
}
```

### Issue: Configuration Incomplete Warnings
```
Warning: Channel configuration incomplete
```

**Symptoms:**
- Automatic watering disabled
- Configuration score below 100%
- Missing configuration groups

**Solutions:**
1. Check configuration status:
```c
configuration_status_t status;
configuration_status_get_channel_status(channel_id, &status);

if (!status.basic_configured) {
    // Configure basic settings: plant type, soil type, coverage
}
if (!status.growing_env_configured) {
    // Configure growing environment: sun exposure, water factor
}
if (!status.compensation_configured) {
    // Configure rain/temperature compensation settings
}
```

2. Use configuration reset if needed:
```c
// Reset specific configuration groups
configuration_reset_group(channel_id, CONFIG_GROUP_COMPENSATION);
```

### Issue: PowerShell Execution Policy Error
```
Error: Execution of scripts is disabled on this system
```

**Symptoms:**
- Cannot run PowerShell scripts
- "ExecutionPolicy" error messages

**Solutions:**
```powershell
# Temporary solution (current session only)
Set-ExecutionPolicy -ExecutionPolicy Bypass -Scope Process

# Permanent solution (requires admin)
Set-ExecutionPolicy -ExecutionPolicy RemoteSigned -Scope CurrentUser

# Alternative: Run with bypass
powershell -ExecutionPolicy Bypass -File run_tests_wsl.ps1
```

### Issue: Insufficient Permissions
```
Error: Access denied / Permission denied
```

**Symptoms:**
- File access errors
- Cannot create directories
- WSL permission issues

**Solutions:**
```powershell
# Run PowerShell as Administrator
Start-Process powershell -Verb RunAs

# Fix WSL file permissions
wsl -- sudo chown -R $USER:$USER ~/zephyrproject
wsl -- sudo chown -R $USER:$USER ~/zephyr-sdk-*

# Fix Windows file permissions
icacls . /grant:r "$env:USERNAME:(OI)(CI)F" /T
```

## Advanced Irrigation Modes Issues

### Issue: Interval Mode Not Working
```
Error: Interval mode fails to start or operate correctly
```

**Symptoms:**
- Interval mode tasks don't start
- Phase transitions don't occur
- Timing is incorrect

**Diagnosis:**
```c
// Check interval configuration
interval_config_t config;
if (interval_timing_is_configured(&config, &is_configured) == 0) {
    if (!is_configured) {
        // Interval mode not configured
    }
}

// Check timing validation
if (interval_timing_validate_config(&config) != 0) {
    // Invalid timing configuration
}
```

**Solutions:**
1. Configure interval timing:
```c
interval_config_t config;
interval_timing_init_config(&config);
interval_timing_set_watering_duration(&config, 2, 30); // 2 min 30 sec
interval_timing_set_pause_duration(&config, 5, 0);     // 5 min pause
```

2. Verify timing ranges:
- Watering duration: 1 second to 60 minutes
- Pause duration: 1 second to 60 minutes
- Total cycle should be reasonable for target duration

### Issue: Enhanced Task Status Not Updating
```
Error: Task status doesn't reflect interval mode phases
```

**Solutions:**
```c
// Enable enhanced task monitoring
enhanced_watering_task_state_t task_state;
if (interval_task_get_enhanced_state(&task_state) == 0) {
    // Check current phase
    if (task_state.is_interval_mode) {
        switch (task_state.current_phase) {
            case TASK_STATE_WATERING:
                // Currently watering
                break;
            case TASK_STATE_PAUSING:
                // Currently in pause phase
                break;
        }
    }
}
```

## BME280 Environmental Sensor Issues

### Issue: BME280 Sensor Not Detected
```
Error: BME280 initialization failed
```

**Symptoms:**
- Sensor initialization fails
- Environmental data unavailable
- Temperature compensation disabled

**Diagnosis:**
```c
// Check I2C connection
const struct device *i2c_dev = DEVICE_DT_GET(DT_NODELABEL(i2c0));
if (!device_is_ready(i2c_dev)) {
    // I2C device not ready
}

// Check BME280 address
uint8_t addr = 0x77; // or 0x76
int ret = sensor_manager_init_bme280(i2c_dev, addr);
if (ret < 0) {
    // BME280 not responding at this address
}
```

**Solutions:**
1. Verify hardware connections:
   - VCC to 3.3V
   - GND to ground
   - SDA to I2C SDA pin
   - SCL to I2C SCL pin

2. Check I2C address:
```c
// Try both possible addresses
uint8_t addresses[] = {0x76, 0x77};
for (int i = 0; i < 2; i++) {
    if (sensor_manager_init_bme280(i2c_dev, addresses[i]) == 0) {
        // Found sensor at this address
        break;
    }
}
```

### Issue: Environmental Data Quality Issues
```
Warning: Environmental data validation failed
```

**Symptoms:**
- Inconsistent sensor readings
- Data validation errors
- Environmental history gaps

**Solutions:**
```c
// Configure sensor for better accuracy
bme280_config_t config = {
    .initialized = true,
    .enabled = true,
    .measurement_interval = 30, // seconds
};
sensor_manager_configure_bme280(&config);

// Enable data validation
environmental_data_config_t data_config = {
    .enable_outlier_detection = true,
    .temperature_range_min = -20.0f,
    .temperature_range_max = 60.0f,
    .humidity_range_min = 0.0f,
    .humidity_range_max = 100.0f
};
```

## Custom Soil Configuration Issues

### Issue: Custom Soil Parameters Invalid
```
Error: Custom soil validation failed
```

**Symptoms:**
- Custom soil configuration rejected
- FAO-56 calculations fail
- Fallback to standard soil types

**Diagnosis:**
```c
custom_soil_entry_t custom_soil;
if (custom_soil_db_read(channel_id, &custom_soil) == WATERING_SUCCESS) {
    // Validate parameters
    if (fao56_validate_custom_soil_for_calculations(&custom_soil) != WATERING_SUCCESS) {
        // Invalid parameters for FAO-56
    }
}
```

**Solutions:**
1. Ensure valid parameter ranges:
```c
custom_soil_entry_t soil = {
    .field_capacity = 25.0f,        // 10-50% typical range
    .wilting_point = 12.0f,         // 5-25% typical range
    .infiltration_rate = 15.0f,     // 1-100 mm/h typical range
    .bulk_density = 1.3f,           // 1.0-1.8 g/cm^3 typical range
    .organic_matter = 3.0f,         // 0-10% typical range
    .ph_level = 6.5f,               // 4.0-9.0 typical range
    .salinity_ec = 0.5f             // 0-4 dS/m typical range
};
```

2. Verify field capacity > wilting point:
```c
if (soil.field_capacity <= soil.wilting_point) {
    // Invalid: field capacity must be greater than wilting point
    soil.field_capacity = soil.wilting_point + 5.0f;
}
```

### Issue: Custom Soil Database Corruption
```
Error: Custom soil database read/write failed
```

**Solutions:**
```c
// Check database integrity
if (!custom_soil_db_exists(channel_id)) {
    // Create new entry
    custom_soil_entry_t default_soil;
    custom_soil_db_init_default(&default_soil);
    custom_soil_db_write(channel_id, &default_soil);
}

// Repair corrupted entries
for (uint8_t ch = 0; ch < WATERING_CHANNELS_COUNT; ch++) {
    custom_soil_entry_t soil;
    if (custom_soil_db_read(ch, &soil) != WATERING_SUCCESS) {
        // Repair with default values
        custom_soil_db_init_default(&soil);
        custom_soil_db_write(ch, &soil);
    }
}
```

## Compensation System Issues

### Issue: Rain Compensation Not Working
```
Error: Rain compensation calculations incorrect
```

**Symptoms:**
- Watering not reduced after rain
- Rain sensor data not integrated
- Compensation confidence low

**Diagnosis:**
```c
// Check rain sensor status
rain_sensor_status_t status;
rain_sensor_get_status(&status);
if (!status.sensor_active) {
    // Rain sensor not working
}

// Check rain compensation configuration
rain_compensation_config_t config;
rain_compensation_get_config(channel_id, &config);
if (!config.enabled) {
    // Rain compensation disabled
}
```

**Solutions:**
1. Configure rain compensation:
```c
rain_compensation_config_t config = {
    .enabled = true,
    .sensitivity_percent = 75,      // 0-100%
    .skip_threshold_mm = 5.0f,      // Skip watering if > 5mm rain
    .lookback_hours = 48,           // Consider last 48 hours
    .confidence_threshold = 60      // Minimum confidence level
};
rain_compensation_set_config(channel_id, &config);
```

2. Calibrate rain sensor:
```c
// Set rain sensor calibration
rain_sensor_config_t sensor_config = {
    .mm_per_pulse = 0.2f,           // 0.2mm per tip
    .debounce_time_ms = 50,         // 50ms debounce
    .enabled = true
};
rain_sensor_configure(&sensor_config);
```

### Issue: Temperature Compensation Errors
```
Error: Temperature compensation calculations fail
```

**Solutions:**
```c
// Configure temperature compensation
temperature_compensation_config_t config = {
    .enabled = true,
    .base_temperature_c = 20.0f,    // Base temperature
    .sensitivity_percent = 50,       // 50% sensitivity
    .min_adjustment = 0.8f,         // Minimum 80% of normal
    .max_adjustment = 1.3f,         // Maximum 130% of normal
    .use_heat_index = true          // Consider humidity
};
temperature_compensation_set_config(channel_id, &config);

// Verify BME280 is working
environmental_data_t env_data;
if (environmental_data_get_current(&env_data) == 0) {
    if (env_data.temperature_valid) {
        // Temperature data available
    }
}
```

## Interval Mode Issues

### Issue: Interval Timing Inaccurate
```
Error: Interval phases don't match configured timing
```

**Diagnosis:**
```c
// Check timing configuration
interval_config_t config;
uint32_t watering_sec = interval_get_watering_duration_sec(&config);
uint32_t pause_sec = interval_get_pause_duration_sec(&config);

// Verify timing is within valid ranges
if (watering_sec < 1 || watering_sec > 3600) {
    // Invalid watering duration
}
if (pause_sec < 1 || pause_sec > 3600) {
    // Invalid pause duration
}
```

**Solutions:**
```c
// Set precise timing
interval_timing_set_watering_duration(&config, 2, 30);  // 2 min 30 sec
interval_timing_set_pause_duration(&config, 5, 0);      // 5 min exactly

// Verify configuration
if (interval_timing_validate_config(&config) != 0) {
    // Fix invalid configuration
    interval_timing_init_config(&config);
}
```

### Issue: Interval Mode State Machine Errors
```
Error: Interval controller state transitions fail
```

**Solutions:**
```c
// Reset interval controller state
interval_controller_t controller;
interval_controller_reset(&controller);

// Check controller status
interval_controller_status_t status;
if (interval_controller_get_status(&controller, &status) == 0) {
    if (status.error_occurred) {
        // Handle controller error
        interval_controller_handle_error(&controller, 
                                       status.last_error, 
                                       "State machine error");
    }
}
```

## Configuration Management Issues

### Issue: Configuration Reset Not Working
```
Error: Configuration reset operations fail
```

**Symptoms:**
- Reset commands don't clear settings
- Configuration status not updated
- Selective reset not working

**Solutions:**
```c
// Reset specific configuration groups
configuration_reset_group_t groups = CONFIG_GROUP_BASIC | 
                                   CONFIG_GROUP_COMPENSATION;
if (configuration_reset_selective(channel_id, groups) != WATERING_SUCCESS) {
    // Reset failed, try individual groups
    configuration_reset_group(channel_id, CONFIG_GROUP_BASIC);
    configuration_reset_group(channel_id, CONFIG_GROUP_COMPENSATION);
}

// Verify reset completed
configuration_status_t status;
configuration_status_get_channel_status(channel_id, &status);
if (status.basic_configured) {
    // Reset didn't work, force clear
    configuration_force_clear_group(channel_id, CONFIG_GROUP_BASIC);
}
```

### Issue: Configuration Score Calculation Errors
```
Error: Configuration completeness score incorrect
```

**Solutions:**
```c
// Recalculate configuration score
uint8_t score = calculate_configuration_score(&channel);
if (score != expected_score) {
    // Force recalculation
    configuration_status_recalculate(channel_id);
    
    // Check individual group status
    configuration_status_t status;
    configuration_status_get_channel_status(channel_id, &status);
    
    // Debug score calculation
    LOG_DBG("Basic: %d, Growing: %d, Compensation: %d, Custom Soil: %d, Interval: %d",
            status.basic_configured,
            status.growing_env_configured,
            status.compensation_configured,
            status.custom_soil_configured,
            status.interval_configured);
}
```

## BLE Interface Issues

### Issue: Enhanced BLE Characteristics Not Working
```
Error: New BLE characteristics not accessible
```

**Symptoms:**
- Environmental data characteristic not found
- Custom soil configuration fails
- Interval mode settings not accessible

**Solutions:**
1. Verify service discovery:
```javascript
// Check for all characteristics
const service = await device.gatt.getPrimaryService('12345678-1234-5678-1234-56789abcdef0');
const characteristics = await service.getCharacteristics();

// Look for new characteristics
const envData = characteristics.find(c => c.uuid.includes('env_data'));
const customSoil = characteristics.find(c => c.uuid.includes('custom_soil'));
const intervalConfig = characteristics.find(c => c.uuid.includes('interval_config'));
```

2. Handle fragmentation properly:
```javascript
// Read fragmented environmental data
const envChar = await service.getCharacteristic('env_data_uuid');
const fragmentedData = await readFragmentedData(envChar);
```

### Issue: BLE Notification Throttling Issues
```
Error: BLE notifications delayed or missing
```

**Solutions:**
```c
// Adjust notification priorities
bt_irrigation_set_notification_priority(BT_CHAR_ENVIRONMENTAL_DATA, 
                                       BLE_NOTIFICATION_PRIORITY_HIGH);

// Check notification buffer status
if (bt_irrigation_notification_buffer_full()) {
    // Clear buffer or increase buffer size
    bt_irrigation_clear_notification_buffer();
}

// Enable adaptive throttling
bt_irrigation_enable_adaptive_throttling(true);
```

### Issue: Antivirus Interference
```
Error: Files being deleted or quarantined
```

**Symptoms:**
- Random file deletions
- Slow file operations
- Build failures

**Solutions:**
1. Add exclusions to antivirus:
   - WSL directories: `%LOCALAPPDATA%\Packages\CanonicalGroupLimited.Ubuntu*`
   - Project directory: `C:\path\to\AutoWatering`
   - Temp directories: `C:\Users\%USERNAME%\AppData\Local\Temp`

2. Temporarily disable real-time protection during setup

## WSL-Related Issues

### Issue: WSL Not Installed
```
Error: 'wsl' is not recognized as an internal or external command
```

**Diagnosis:**
```powershell
# Check if WSL is available
Get-Command wsl -ErrorAction SilentlyContinue
```

**Solutions:**
```powershell
# Install WSL2
wsl --install

# Or enable WSL feature manually
dism.exe /online /enable-feature /featurename:Microsoft-Windows-Subsystem-Linux /all /norestart
dism.exe /online /enable-feature /featurename:VirtualMachinePlatform /all /norestart
Restart-Computer
```

### Issue: WSL Distribution Not Found
```
Error: The specified distribution is not available
```

**Diagnosis:**
```powershell
# List available distributions
wsl --list --online
wsl --list --verbose
```

**Solutions:**
```powershell
# Install Ubuntu 22.04
wsl --install -d Ubuntu-22.04

# Or install from Microsoft Store
start ms-windows-store://pdp/?productid=9PN20MSR04DW

# Import existing distribution
wsl --import Ubuntu-22.04 C:\WSL\Ubuntu-22.04 ubuntu.tar
```

### Issue: WSL2 Kernel Update Required
```
Error: WSL 2 requires an update to its kernel component
```

**Solutions:**
1. Download WSL2 kernel update from Microsoft
2. Install the update package
3. Restart WSL:
```powershell
wsl --shutdown
wsl --set-default-version 2
```

### Issue: WSL Distribution Won't Start
```
Error: The system cannot find the file specified
```

**Diagnosis:**
```powershell
# Check WSL status
wsl --status
wsl --list --verbose

# Check Windows features
Get-WindowsOptionalFeature -Online -FeatureName Microsoft-Windows-Subsystem-Linux
```

**Solutions:**
```powershell
# Restart WSL service
wsl --shutdown
net stop LxssManager
net start LxssManager

# Reset distribution
wsl --terminate Ubuntu-22.04
wsl --unregister Ubuntu-22.04
wsl --install -d Ubuntu-22.04
```

### Issue: WSL Memory/Performance Issues
```
Error: Out of memory / Slow performance
```

**Diagnosis:**
```powershell
# Check WSL resource usage
wsl -- free -h
wsl -- df -h
```

**Solutions:**
Create `.wslconfig` file in `%USERPROFILE%`:
```ini
[wsl2]
memory=8GB
processors=4
swap=2GB
localhostForwarding=true
```

Then restart WSL:
```powershell
wsl --shutdown
```

## Zephyr Environment Issues

### Issue: West Command Not Found
```
Error: west: command not found
```

**Diagnosis:**
```bash
# Check if west is installed
which west
echo $PATH
python3 -m pip list --user | grep west
```

**Solutions:**
```bash
# Install west
python3 -m pip install --user --upgrade west

# Fix PATH
echo 'export PATH="$HOME/.local/bin:$PATH"' >> ~/.bashrc
source ~/.bashrc

# Verify installation
west --version
```

### Issue: Zephyr SDK Not Found
```
Error: Zephyr SDK not found
```

**Diagnosis:**
```bash
# Check SDK installation
ls -la ~/zephyr-sdk-*
echo $ZEPHYR_SDK_INSTALL_DIR
```

**Solutions:**
```bash
# Download and install SDK
cd ~
wget https://github.com/zephyrproject-rtos/sdk-ng/releases/download/v0.17.0/zephyr-sdk-0.17.0_linux-x86_64.tar.xz
tar xf zephyr-sdk-0.17.0_linux-x86_64.tar.xz
cd zephyr-sdk-0.17.0
./setup.sh -t all -h -c

# Set environment variable
echo 'export ZEPHYR_SDK_INSTALL_DIR="$HOME/zephyr-sdk-0.17.0"' >> ~/.bashrc
source ~/.bashrc
```

### Issue: CMake/Ninja Not Found
```
Error: cmake: command not found
Error: ninja: command not found
```

**Solutions:**
```bash
# Install build tools
sudo apt update
sudo apt install -y cmake ninja-build

# Verify installation
cmake --version
ninja --version
```

### Issue: Python Package Issues
```
Error: ModuleNotFoundError: No module named 'west'
```

**Diagnosis:**
```bash
# Check Python environment
python3 --version
python3 -m pip --version
python3 -m pip list --user
```

**Solutions:**
```bash
# Update pip
python3 -m pip install --user --upgrade pip

# Install required packages
python3 -m pip install --user west pyelftools pyyaml pykwalify colorama packaging

# Check installation
python3 -c "import west; print('West imported successfully')"
```

### Issue: Zephyr Project Initialization Failed
```
Error: Failed to initialize Zephyr project
```

**Solutions:**
```bash
# Clean and reinitialize
rm -rf ~/zephyrproject
west init ~/zephyrproject
cd ~/zephyrproject
west update

# Alternative: Initialize in current directory
mkdir -p ~/zephyrproject
cd ~/zephyrproject
west init .
west update
```

## Synchronization Issues

### Issue: Project Sync Fails
```
Error: Project synchronization failed
```

**Diagnosis:**
```powershell
# Check source directory
Get-ChildItem -Path . -Recurse | Measure-Object
Test-Path CMakeLists.txt
Test-Path prj.conf

# Check WSL target
wsl -- ls -la /tmp/autowatering_tests/
wsl -- df -h /tmp
```

**Solutions:**
```powershell
# Clean target directory
wsl -- rm -rf /tmp/autowatering_tests
wsl -- mkdir -p /tmp/autowatering_tests

# Force sync
.\run_tests_wsl.ps1 -TestSuite unit -VerboseOutput

# Check file permissions
wsl -- sudo chown -R $USER:$USER /tmp/autowatering_tests
```

### Issue: Rsync Not Available
```
Error: rsync command not found
```

**Solutions:**
```bash
# Install rsync
sudo apt update
sudo apt install -y rsync

# Verify installation
rsync --version
```

### Issue: File Permission Errors During Sync
```
Error: Permission denied during file copy
```

**Solutions:**
```bash
# Fix permissions on WSL side
sudo chown -R $USER:$USER /tmp/autowatering_tests
chmod -R 755 /tmp/autowatering_tests

# Fix permissions on Windows side
icacls . /reset /T
```

### Issue: Sync Timeout
```
Error: Synchronization timed out
```

**Solutions:**
1. Increase timeout in configuration:
```json
{
  "synchronization": {
    "timeout_seconds": 600
  }
}
```

2. Optimize exclude patterns:
```json
{
  "synchronization": {
    "exclude_patterns": [
      "build*", ".git", "*.bin", "*.hex", "twister-out*",
      "node_modules", ".vscode", "__pycache__"
    ]
  }
}
```

## Test Execution Issues

### Issue: Build Failures
```
Error: Build failed for platform
```

**Diagnosis:**
```bash
# Check build environment
cd /tmp/autowatering_tests/tests
west build -b native_posix_64 --pristine -v

# Check dependencies
which cmake ninja gcc
```

**Solutions:**
```bash
# Clean build
rm -rf build*
west build -b native_posix_64 --pristine

# Install missing dependencies
sudo apt install -y gcc-multilib g++-multilib

# Check Zephyr environment
source ~/.bashrc
echo $ZEPHYR_BASE
```

### Issue: Test Execution Timeout
```
Error: Test execution timed out
```

**Solutions:**
1. Increase timeout in configuration:
```json
{
  "test_execution": {
    "default_timeout_seconds": 1200
  }
}
```

2. Run tests individually:
```powershell
.\run_tests_wsl.ps1 -TestSuite "specific_test" -Platform native
```

### Issue: Hardware Tests Fail
```
Error: Hardware tests failed
```

**Diagnosis:**
```bash
# Check hardware connectivity
lsusb | grep -i nordic
ls -la /dev/ttyACM*

# Check J-Link
which JLinkExe
```

**Solutions:**
```bash
# Install J-Link tools
# Download from SEGGER website and install

# Fix USB permissions
sudo usermod -a -G dialout $USER
sudo udevadm control --reload-rules

# Restart WSL
exit
```

### Issue: Native Tests Fail
```
Error: Native platform tests failed
```

**Solutions:**
```bash
# Check native platform support
west build -b native_posix_64 samples/hello_world

# Install additional dependencies
sudo apt install -y libsdl2-dev

# Check for missing libraries
ldd build/zephyr/zephyr.exe
```

## Performance Issues

### Issue: Slow Test Execution
```
Issue: Tests take too long to complete
```

**Diagnosis:**
```powershell
# Check performance report
Get-Content performance_report.json | ConvertFrom-Json

# Monitor resource usage
wsl -- top
wsl -- free -h
```

**Solutions:**
1. Enable parallel execution:
```json
{
  "test_execution": {
    "parallel_execution": true,
    "max_parallel_jobs": 4
  }
}
```

2. Optimize platform selection:
```powershell
# Use only native platforms for development
.\run_tests_wsl.ps1 -Platform native
```

3. Improve sync performance:
```json
{
  "synchronization": {
    "incremental_sync": true,
    "exclude_patterns": ["build*", ".git", "*.bin"]
  }
}
```

### Issue: High Memory Usage
```
Issue: System runs out of memory
```

**Solutions:**
1. Configure WSL memory limits in `.wslconfig`:
```ini
[wsl2]
memory=4GB
swap=2GB
```

2. Enable cleanup:
```json
{
  "cleanup": {
    "auto_cleanup": true,
    "cleanup_temp_files": true
  }
}
```

### Issue: Disk Space Issues
```
Error: No space left on device
```

**Solutions:**
```bash
# Clean up build artifacts
find . -name "build*" -type d -exec rm -rf {} +
find . -name "twister-out*" -type d -exec rm -rf {} +

# Clean package cache
sudo apt autoremove -y
sudo apt autoclean

# Clean pip cache
python3 -m pip cache purge
```

## Hardware Testing Issues

### Issue: nRF52840 DK Not Detected
```
Error: No hardware detected
```

**Diagnosis:**
```bash
# Check USB devices
lsusb
lsusb | grep -i nordic

# Check serial devices
ls -la /dev/ttyACM*
ls -la /dev/ttyUSB*
```

**Solutions:**
```bash
# Install USB rules
sudo apt install -y udev
sudo udevadm control --reload-rules
sudo udevadm trigger

# Add user to dialout group
sudo usermod -a -G dialout $USER

# Restart WSL
exit
```

### Issue: J-Link Connection Problems
```
Error: J-Link not found
```

**Solutions:**
1. Install J-Link software from SEGGER
2. Configure J-Link in WSL:
```bash
# Add J-Link to PATH
echo 'export PATH="/opt/SEGGER/JLink:$PATH"' >> ~/.bashrc
source ~/.bashrc

# Test J-Link connection
JLinkExe -device nRF52840_xxAA -if SWD -speed 4000 -autoconnect 1
```

### Issue: Flashing Failures
```
Error: Failed to flash firmware
```

**Solutions:**
```bash
# Check board connection
west flash --runner jlink

# Try different runner
west flash --runner pyocd

# Manual flash
nrfjprog --program build/zephyr/zephyr.hex --sectorerase --verify --reset
```

## Diagnostic Tools

### System Information Collection

```powershell
# Create diagnostic script
@"
# AutoWatering WSL Test Runner Diagnostics
Write-Host "=== System Information ===" -ForegroundColor Green
systeminfo | findstr /B /C:"OS Name" /C:"OS Version" /C:"System Type"
Get-ComputerInfo | Select-Object WindowsVersion, TotalPhysicalMemory

Write-Host "`n=== WSL Information ===" -ForegroundColor Green
wsl --status
wsl --list --verbose

Write-Host "`n=== PowerShell Information ===" -ForegroundColor Green
$PSVersionTable

Write-Host "`n=== Disk Space ===" -ForegroundColor Green
Get-WmiObject -Class Win32_LogicalDisk | Select-Object DeviceID, @{Name="Size(GB)";Expression={[math]::Round($_.Size/1GB,2)}}, @{Name="FreeSpace(GB)";Expression={[math]::Round($_.FreeSpace/1GB,2)}}

Write-Host "`n=== WSL Environment ===" -ForegroundColor Green
wsl -- uname -a
wsl -- free -h
wsl -- df -h
wsl -- which west cmake ninja python3

Write-Host "`n=== Zephyr Environment ===" -ForegroundColor Green
wsl -- west --version 2>/dev/null || echo "West not found"
wsl -- ls -la ~/zephyrproject/ 2>/dev/null || echo "Zephyr project not found"
wsl -- ls -la ~/zephyr-sdk-* 2>/dev/null || echo "Zephyr SDK not found"
"@ | Out-File -FilePath "diagnostics.ps1" -Encoding UTF8

# Run diagnostics
.\diagnostics.ps1 | Tee-Object -FilePath "diagnostic_report.txt"
```

### Log Analysis Tools

```powershell
# Analyze error patterns
Get-Content wsl_test_execution.log | Select-String "ERROR" | Group-Object | Sort-Object Count -Descending

# Check recent errors
Get-Content wsl_test_execution.log | Select-String "ERROR" | Select-Object -Last 10

# Generate log summary
Get-Content wsl_test_execution.log | ForEach-Object {
    if ($_ -match '\[(.*?)\].*\[(.*?)\].*\[(.*?)\]') {
        [PSCustomObject]@{
            Timestamp = $matches[1]
            Level = $matches[2]
            Component = $matches[3]
            Message = $_.Substring($_.IndexOf($matches[3]) + $matches[3].Length + 2)
        }
    }
} | Group-Object Level | Select-Object Name, Count
```

### Performance Analysis

```powershell
# Analyze performance report
if (Test-Path "performance_report.json") {
    $perf = Get-Content "performance_report.json" | ConvertFrom-Json
    
    Write-Host "=== Performance Summary ===" -ForegroundColor Green
    Write-Host "Total Duration: $($perf.ExecutionSummary.TotalDurationSeconds) seconds"
    Write-Host "Success Rate: $($perf.TestSummary.SuccessRate)%"
    
    Write-Host "`n=== Phase Durations ===" -ForegroundColor Green
    $perf.ExecutionPhases.PSObject.Properties | ForEach-Object {
        Write-Host "$($_.Name): $($_.Value.Duration) seconds ($($_.Value.Percentage)%)"
    }
    
    if ($perf.Recommendations.Count -gt 0) {
        Write-Host "`n=== Recommendations ===" -ForegroundColor Yellow
        $perf.Recommendations | ForEach-Object { Write-Host "- $_" }
    }
}
```

## Recovery Procedures

### Complete Environment Reset

```powershell
# 1. Stop all WSL processes
wsl --shutdown

# 2. Backup important data (optional)
wsl --export Ubuntu-22.04 ubuntu-backup.tar

# 3. Unregister distribution
wsl --unregister Ubuntu-22.04

# 4. Reinstall distribution
wsl --install -d Ubuntu-22.04

# 5. Run setup
.\run_tests_wsl.ps1 -SetupOnly
```

### Partial Recovery - Zephyr Environment Only

```bash
# Remove Zephyr installation
rm -rf ~/zephyrproject
rm -rf ~/zephyr-sdk-*

# Clean Python packages
python3 -m pip uninstall -y west pyelftools pyyaml pykwalify colorama packaging

# Reinstall
python3 -m pip install --user west pyelftools pyyaml pykwalify colorama packaging
west init ~/zephyrproject
cd ~/zephyrproject
west update
```

### Configuration Reset

```powershell
# Backup current configuration
Copy-Item wsl_test_config.json wsl_test_config.json.backup

# Generate new default configuration
.\run_tests_wsl.ps1 -GenerateConfigTemplate -TemplateName default

# Test with new configuration
.\run_tests_wsl.ps1 -TestSuite unit -Platform native
```

## Advanced Troubleshooting

### Enable Debug Logging

```powershell
# Enable verbose logging
.\run_tests_wsl.ps1 -VerboseOutput -TestSuite unit

# Enable debug mode in configuration
{
  "logging": {
    "level": "DEBUG"
  },
  "enable_debug_mode": true
}
```

### Network Troubleshooting

```bash
# Test network connectivity
ping -c 4 8.8.8.8
curl -I https://github.com

# Check DNS resolution
nslookup github.com

# Test package manager connectivity
sudo apt update -v
```

### WSL Networking Issues

```powershell
# Reset WSL networking
wsl --shutdown
netsh winsock reset
netsh int ip reset
ipconfig /flushdns
```

### File System Issues

```bash
# Check file system integrity
sudo fsck -f /dev/sdc

# Fix permissions
sudo chown -R $USER:$USER $HOME
find $HOME -type d -exec chmod 755 {} \;
find $HOME -type f -exec chmod 644 {} \;
```

### Registry Issues (Windows)

```powershell
# Reset WSL registry entries (requires admin)
Remove-Item -Path "HKCU:\Software\Microsoft\Windows\CurrentVersion\Lxss" -Recurse -Force
```

## Getting Additional Help

### Collecting Support Information

```powershell
# Generate comprehensive support package
$supportDir = "support_$(Get-Date -Format 'yyyyMMdd_HHmmss')"
New-Item -ItemType Directory -Path $supportDir

# Copy logs
Copy-Item "*.log" $supportDir -ErrorAction SilentlyContinue
Copy-Item "performance_report.*" $supportDir -ErrorAction SilentlyContinue
Copy-Item "wsl_test_config.json" $supportDir -ErrorAction SilentlyContinue

# Generate system info
.\diagnostics.ps1 > "$supportDir\system_info.txt"

# Create archive
Compress-Archive -Path $supportDir -DestinationPath "$supportDir.zip"

Write-Host "Support package created: $supportDir.zip"
```

### Community Resources

- **Project Repository**: Report issues and feature requests
- **Documentation**: Check latest documentation updates
- **Community Forum**: Ask questions and share solutions
- **Stack Overflow**: Tag questions with `autowatering-wsl`

### Professional Support

For enterprise environments or complex issues:
1. Collect support package using script above
2. Document reproduction steps
3. Include environment details
4. Contact professional support channels

Remember to check the [User Guide](WSL_TEST_RUNNER_USER_GUIDE.md) and [Installation Guide](INSTALLATION.md) for additional information.
