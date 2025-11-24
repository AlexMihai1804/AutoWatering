# BLE Characteristics Reference

Updated to reflect the 26 characteristics implemented in firmware (`bt_irrigation_service.c`). Items not implemented have been moved to `../FUTURE_FEATURES.md`.

See `../GLOSSARY.md` for standardized terminology (e.g., "Unified 8B header", fragmentation types, long write semantics). A concise cross-characteristic summary of fragmentation/long-write schemes is available in [`_fragmentation-reference.md`](_fragmentation-reference.md).

## Complete Characteristics List

### Core Control Characteristics

| # | Characteristic | UUID (suffix) | Size | Properties | Purpose |
|---|----------------|--------------|------|------------|---------|
| 1 | **[Valve Control](01-valve-control.md)** | def1 | 4B | R/W/N | Manual valve & master valve control |
| 2 | **[Flow Sensor](02-flow-sensor.md)** | def2 | 4B | R/N | Real-time water flow |
| 3 | **[System Status](03-system-status.md)** | def3 | 1B | R/N | System health |

### Configuration Characteristics

| # | Characteristic | UUID | Size | Properties | Purpose |
|---|----------------|------|------|------------|---------|
| 4 | **[Channel Configuration](04-channel-configuration.md)** | def4 | 76B | R/W/N | Channel setup (fragmented header types 1/2/3) |
| 5 | **[Schedule Configuration](05-schedule-configuration.md)** | def5 | 9B | R/W/N | Watering schedules |
| 6 | **[System Configuration](06-system-configuration.md)** | def6 | 67B | R/W/N | System + master valve + sensor (long write) |

### Management Characteristics

| # | Characteristic | UUID | Size | Properties | Purpose |
|---|----------------|------|------|------------|---------|
| 7 | **[Task Queue Management](07-task-queue-management.md)** | def7 | 9B | R/W/N | Task queue & commands |
| 8 | **[Statistics](08-statistics.md)** | def8 | 15B | R/W/N | Usage metrics |
| 9 | **[RTC Configuration](09-rtc-configuration.md)** | def9 | 16B | R/W/N | Time & date |

### Status & Alert Characteristics

| # | Characteristic | UUID | Size | Properties | Purpose |
|---|----------------|------|------|------------|---------|
| 10 | **[Alarm Status](10-alarm-status.md)** | defa | 7B | R/W/N | Alarm snapshot & clear |
| 11 | **[Calibration Management](11-calibration-management.md)** | defb | 13B | R/W/N | Flow sensor calibration |
| 12 | **(History Management)** | defc | - | - | Moved to FUTURE (not implemented) |
| 13 | **[Diagnostics](13-diagnostics.md)** | defd | 12B | R/N | System health metrics |

### Advanced Feature Characteristics

| # | Characteristic | UUID | Size | Properties | Purpose |
|---|----------------|------|------|------------|---------|
| 14 | **[Growing Environment](14-growing-environment.md)** | defe | 71B | R/W/N | Enhanced plant config (fragmented 2/3) |
| 15 | **[Auto Calc Status](15-auto-calc-status.md)** | de00 | 64B | R/W/N | Auto irrigation calc (Unified 8B notify header) |
| 16 | **[Current Task Status](16-current-task-status.md)** | deff | 21B | R/W/N | Active task progress |
| 17 | **[Timezone Configuration](17-timezone-configuration.md)** | 6793 | 16B | R/W/N | Timezone & DST |

### Environmental & Compensation Characteristics

| # | Characteristic | UUID | Size | Properties | Purpose |
|---|----------------|------|------|------------|---------|
| 18 | **[Rain Sensor Config](18-rain-sensor-config.md)** | de12 | 18B | R/W/N | Rain sensor settings |
| 19 | **[Rain Sensor Data](19-rain-sensor-data.md)** | de13 | 24B | R/N | Rain metrics live |
| 20 | **[Rain History Control](20-rain-history-control.md)** | de14 | 16B+ | R/W/N | Rain history queries (fragmented) |
| 21 | **[Onboarding Status](21-onboarding-status.md)** | de20 | 29B | R/N | Onboarding progress |
| 22 | **[Reset Control](22-reset-control.md)** | de21 | 16B | R/W/N | Controlled resets |
| 23 | **[Environmental Data](23-environmental-data.md)** | de15 | 24B | R/N | BME280 snapshot |
| 24 | **[Environmental History](24-environmental-history.md)** | de16 | Var. | R/W/N | Env. history (fragmented) |
| 25 | **[Compensation Status](25-compensation-status.md)** | de17 | 40B | R/W/N | Compensation metrics |
| 26 | **[Rain Integration Status](26-rain-integration-status.md)** | de18 | 78B | R/N | Rain integration snapshot |

**Legend**: R=Read, W=Write, N=Notify

## Quick Reference by Use Case

### Getting Started (Essential Characteristics)

For basic system operation, focus on these characteristics:

1. **[Valve Control](01-valve-control.md)** - Start/stop watering manually
2. **[System Status](03-system-status.md)** - Check if system is working properly
3. **[Flow Sensor](02-flow-sensor.md)** - Monitor water usage
4. **[Current Task Status](16-current-task-status.md)** - See active watering progress

### System Configuration

To set up your irrigation system:

1. **[Channel Configuration](04-channel-configuration.md)** - Configure plants and irrigation settings
2. **[System Configuration](06-system-configuration.md)** - Set up master valve and system preferences
3. **[RTC Configuration](09-rtc-configuration.md)** - Set correct time and date
4. **[Timezone Configuration](17-timezone-configuration.md)** - Configure timezone and DST

### Automatic Operation

For hands-off irrigation:

1. **[Schedule Configuration](05-schedule-configuration.md)** - Set up automatic watering schedules
2. **[Task Queue Management](07-task-queue-management.md)** - Monitor and control task execution
3. **[Growing Environment](14-growing-environment.md)** - Advanced plant-specific settings
4. **[Auto Calc Status](15-auto-calc-status.md)** - Monitor automatic calculations

### Monitoring & Maintenance

To track system performance:

1. **[Statistics](08-statistics.md)** - View water usage and system metrics
2. History Management (not implemented) - Placeholder only
3. **[Diagnostics](13-diagnostics.md)** - Monitor system health
4. **[Alarm Status](10-alarm-status.md)** - Check for system alerts
5. **[Environmental Data](23-environmental-data.md)** - Monitor real-time environmental conditions
6. **[Environmental History](24-environmental-history.md)** - Access historical environmental data
7. **[Compensation Status](25-compensation-status.md)** - View environmental compensation calculations

### Calibration & Setup

For system optimization:

1. **[Calibration Management](11-calibration-management.md)** - Calibrate flow sensor for accurate measurements
2. **[System Configuration](06-system-configuration.md)** - Adjust system parameters
3. **[Growing Environment](14-growing-environment.md)** - Fine-tune plant-specific settings
4. History Management (not implemented)

## Technical Categories

### Fragmented Characteristics (Custom Header or Long Write)

- Channel Configuration (4B header types 1/2/3, <=20B fragments)
- Growing Environment (4B header types 2/3)
- Auto Calc Status (Unified 8B notify header)
- Rain / Environmental History (unified 8B header for notifications / responses)
- Rain History Control (commands + fragmented response)
- System Configuration (long write offset accumulation; no custom header)

History Management (12) not implemented (see FUTURE).

### Read-Only (From business logic perspective)

- Flow Sensor (R/N)
- System Status (R/N)
- Diagnostics (R/N)
- Environmental Data (R/N)
- Rain Sensor Data (R/N)
- Rain Integration Status (R/N)
Note: Auto Calc Status & Compensation Status expose writable fields but internal logic may ignore some writes.

### Notification Priority Notes

- Standard minimum interval now unified at >=500 ms for regular characteristics unless explicitly documented (e.g., task queue 5s heartbeat, statistics 30s cadence, calibration progress 200 ms internal loop).
- `Alarm Status` uses normal path (no special critical bypass).
- Legacy references to 50 ms or unthrottled alarm delivery have been removed.

## Documentation Format

Each characteristic document includes:

### Standard Sections
- **Overview** - Purpose and key features
- **UUID and Properties** - Technical specifications
- **Data Structure** - Byte-level format
- **Operations** - Read/Write/Notify behavior
- **Examples** - Code samples and use cases
- **Error Handling** - Common issues and solutions

### Additional Sections (where applicable)
- **Fragmentation** - Protocol details for large characteristics
- **Calibration** - Sensor calibration procedures
- **Scheduling** - Time-based operation details
- **Plant Database** - Plant-specific configuration

## Getting Started

### For Beginners
1. Start with **[Valve Control](01-valve-control.md)** to test basic functionality
2. Check **[System Status](03-system-status.md)** to ensure everything is working
3. Monitor **[Flow Sensor](02-flow-sensor.md)** during watering
4. Configure your first channel with **[Channel Configuration](04-channel-configuration.md)**

### For Advanced Users
1. Set up comprehensive plant profiles with **[Growing Environment](14-growing-environment.md)**
2. Configure automatic scheduling with **[Schedule Configuration](05-schedule-configuration.md)**
3. Monitor system performance with **[Statistics](08-statistics.md)** and **[History Management](12-history-management.md)**
4. Fine-tune system behavior with **[System Configuration](06-system-configuration.md)**

### For Developers
1. Understand the fragmentation protocol used by large characteristics
2. Implement proper error handling for all operations
3. Use notification priorities to optimize performance
4. Follow the data structure specifications exactly
 5. For internal-only extended layouts see `ENHANCED_STRUCTS_INTERNAL.md` (not part of public API)

## Related Documentation

- **[BLE API Overview](../README.md)** - Complete API documentation
- **[Fragmentation Guide](../fragmentation-guide.md)** - Fragmentation protocol details
- **[Integration Examples](../integration-examples.md)** - Real-world implementation examples
- **[Protocol Specification](../protocol-specification.md)** - Technical protocol details

## Tips for Success

### Connection Management
- Always check connection status before operations
- Handle disconnections gracefully
- Use appropriate timeouts for operations

### Data Handling
- Respect the fragmentation protocol for large characteristics
- Use little-endian byte order for multi-byte values
- Validate data before sending to characteristics

### Performance Optimization
- Enable notifications only for characteristics you need
- Batch operations when possible
- Use appropriate notification priorities

### Error Recovery
- Implement retry logic for failed operations
- Check system status when operations fail
- Use diagnostic information for troubleshooting

## Verification Progress Summary

Full audit - see `../IMPLEMENTATION_STATUS.md`. Characteristic 12 moved to FUTURE. Rain Integration Status documented.