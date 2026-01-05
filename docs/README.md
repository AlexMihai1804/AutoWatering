# AutoWatering - Smart Irrigation System Documentation

AutoWatering is an advanced smart irrigation system built on Zephyr RTOS for precision watering control with comprehensive monitoring capabilities.

## Documentation Structure

### For Users

- **[Installation](INSTALLATION.md)** - Toolchain setup and flashing steps
- **[Feature Overview](FEATURES.md)** - High-level capabilities and usage notes
- **[Troubleshooting](TROUBLESHOOTING.md)** - Common issues & resolutions

### For Developers

- **[Full Feature Set](FEATURES_FULL.md)** - Engineering depth and configuration switches
- **[System Architecture](system-architecture.md)** - Code structure and runtime design
- **[Plant Database & FAO-56](plant-database-fao56-system.md)** - Scientific irrigation foundations
- **[Change History](CHANGELOG.md)** - Release notes and deferred items

### BLE API Reference

- **[BLE API Overview](ble-api/README.md)**
- **[Characteristics Reference](ble-api/characteristics/README.md)**
- **[Fragmentation Guide](ble-api/fragmentation-guide.md)**

## Quick Navigation

### Most Common Tasks

- **Start Watering**: [Valve Control](ble-api/characteristics/01-valve-control.md)
- **Monitor Progress**: [Current Task Status](ble-api/characteristics/16-current-task-status.md)
- **Setup Schedules**: [Schedule Configuration](ble-api/characteristics/05-schedule-configuration.md)
- **Configure Growing Environment**: [Growing Environment](ble-api/characteristics/14-growing-environment.md)
- **View Statistics**: [Statistics](ble-api/characteristics/08-statistics.md)

### System Management

- **System Status**: [System Status](ble-api/characteristics/03-system-status.md)
- **Flow Monitoring**: [Flow Sensor](ble-api/characteristics/02-flow-sensor.md)
- **Diagnostics & Alarms**: [Diagnostics](ble-api/characteristics/13-diagnostics.md), [Alarms](ble-api/characteristics/10-alarm-status.md)
- **Time Setup**: [RTC Configuration](ble-api/characteristics/09-rtc-configuration.md)

## Key Features (Summary)

High-level only; see `FEATURES.md` for market overview or `FEATURES_FULL.md` for engineering depth.

- Intelligent multi-zone irrigation (adaptive, science-based)
- Rain & environmental integration (compensation & eco modes)
- Extensive plant / soil knowledge base
- Rich historical analytics & insights
- Comprehensive, open BLE API for integration
- Local-first operation (no mandatory cloud)

## External Resources

- **[GitHub Repository](https://github.com/AlexMihai1804/AutoWatering)** - Source code and issues
- **[AutoWatering Smart Dashboard](https://github.com/AlexMihai1804/autowatering-smart-dashboard)** - Client dashboard (TypeScript)
- **Community Discussions** - Project Q&A and ideas (GitHub Discussions)

---

**Firmware version**: 3.1.0 (January 2026)  
**Docs updated**: 2026-01-05  
**License**: MIT  
**Maintainer**: Alex Mihai
