# AutoWatering - Zephyr RTOS-based Smart Irrigation System

[![Zephyr RTOS](https://img.shields.io/badge/Zephyr-RTOS-blue)](https://www.zephyrproject.org/)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![Version](https://img.shields.io/badge/Version-1.2-green.svg)](https://github.com/AlexMihai1804/AutoWatering)
[![BLE Reliability](https://img.shields.io/badge/BLE-Enhanced-brightgreen.svg)](docs/ble/README.md)

<p align="center">
  <img src="docs/images/logo.png" alt="AutoWatering Logo" width="200"/>
</p>

## 📋 Overview

AutoWatering is a smart irrigation system built on Zephyr RTOS, designed for precision watering with monitoring capabilities. The system can manage up to 8 independent irrigation channels with automatic scheduling, flow monitoring, and remote control via Bluetooth.

### Recent Improvements (July 2025) 🚀

- **🔧 Complete BLE Implementation**: All 16 characteristics fully implemented with proper handlers
- **📡 Enhanced Notification System**: 500ms throttling with CCC state checking and automatic error recovery
- **🔄 Background Updates**: Dedicated thread for automatic status updates every 2 seconds
- **⚡ Simple & Reliable**: Direct notifications without complex queuing for maximum stability
- **📊 Real-time Monitoring**: Complete current task tracking with automatic progress updates
- **🎯 Verified Implementation**: All API functions implemented and documentation updated to match
- **🔄 Error Recovery**: Notification system automatically recovers after temporary errors
- **📈 Complete Documentation**: All 16 BLE characteristics documented with exact implementation details
- **✅ Full BLE Coverage**: Complete 16/16 characteristics - Valve Control, Flow Sensor, System Status, Channel Config, Schedule Config, System Config, Task Queue, Statistics, RTC, Alarm, Calibration, History, Diagnostics, Growing Environment, Current Task, and Timezone Configuration

### Key Highlights

- **Precision Control**: Manage up to 8 independent irrigation zones
- **Smart Monitoring**: Track water usage with integrated flow sensor
- **Wireless Control**: Configure and monitor via Bluetooth (fully implemented with 16 characteristics)
- **Energy Efficient**: Multiple power saving modes
- **Reliable**: Fault detection and recovery mechanisms
- **Enhanced BLE**: 500ms notification throttling with CCC state checking and error recovery
- **Complete API**: All 16 BLE functions implemented with background updates and direct notifications
- **Verified Documentation**: Implementation and documentation match exactly

## 📑 Documentation

This project's documentation is organized into several sections:

| Document | Description |
|----------|-------------|
| [📦 Installation Guide](docs/INSTALLATION.md) | Setup instructions and build process |
| [🔩 Hardware Guide](docs/HARDWARE.md) | Hardware components and wiring details |
| [💻 Software Guide](docs/SOFTWARE.md) | Software architecture and code examples |
| [📱 **NEW: BLE Documentation**](docs/ble/README.md) | **Reorganized Bluetooth interface with individual characteristic docs** |
| [🌱 Plant Types](docs/PLANT_TYPES.md) | Supported plant configurations |
| [🛠️ Troubleshooting](docs/TROUBLESHOOTING.md) | Common issues and solutions |
| [📝 Changelog](docs/CHANGELOG.md) | Version history and updates |
| [🤝 Contributing](docs/CONTRIBUTING.md) | How to contribute to the project |

### 📱 NEW: Restructured BLE Documentation

The BLE documentation has been **completely reorganized** for better usability:

- **[Main BLE Guide](docs/ble/README.md)** - Overview and quick start
- **[Individual Characteristics](docs/ble/characteristics/)** - Detailed docs for each of the 16 BLE characteristics
- **[Usage Examples](docs/ble/examples/)** - Complete code examples and implementations
- **[Troubleshooting](docs/ble/troubleshooting.md)** - BLE-specific issues and solutions

**✅ Complete BLE Coverage (16/16 Characteristics):**

| # | Characteristic | UUID | Size | Features |
|---|----------------|------|------|----------|
| 1 | **Valve Control** | def1 | 4B | Manual valve operation |
| 2 | **Flow Sensor** | def2 | 4B | Real-time flow monitoring |
| 3 | **System Status** | def3 | 1B | System health & status |
| 4 | **Channel Configuration** | def4 | 76B | Plant & channel setup (fragmented) |
| 5 | **Schedule Configuration** | def5 | 9B | Automatic watering schedules |
| 6 | **System Configuration** | def6 | 10B | Power modes & flow calibration |
| 7 | **Task Queue Management** | def7 | 9B | Task scheduling & monitoring |
| 8 | **Statistics** | def8 | 15B | Usage tracking & metrics |
| 9 | **RTC Configuration** | def9 | 16B | Real-time clock & timezone |
| 10 | **Alarm Status** | defa | 7B | Critical system alerts |
| 11 | **Calibration Management** | defb | 13B | Interactive sensor calibration |
| 12 | **History Management** | defc | 20B | Historical data access (fragmented) |
| 13 | **Diagnostics** | defd | 12B | System health monitoring |
| 14 | **Growing Environment** | defe | 52B | Advanced plant configuration (fragmented) |
| 15 | **Current Task Status** | deff | 21B | Real-time task monitoring |
| 16 | **Timezone Configuration** | 9abc-def123456793 | 12B | DST & timezone management |

**Benefits of the new structure:**
- 📁 **Organized by feature** - Find what you need quickly
- 📖 **Focused content** - Each characteristic has its own detailed documentation
- 💡 **Better examples** - Practical code samples for each feature
- 🔧 **Easier maintenance** - Modular documentation that's easier to update
- ✅ **Complete coverage** - All 16 characteristics fully documented with implementation details

## 🚀 Quick Start

```bash
# Clone the repository
git clone https://github.com/AlexMihai1804/AutoWatering.git
cd AutoWatering

# Build for NRF52840 ProMicro
west build -b nrf52840_promicro

# Flash the board
west flash
```

## 🌟 Key Features

- **Multi-Channel Control**: Manage up to 8 independent irrigation channels
- **🚀 Smart Master Valve**: Intelligent main water control with automatic timing
  - **Pre/Post timing control**: Configurable delays for optimal water pressure
  - **Overlap detection**: Smart grace period for consecutive watering tasks
  - **Dual modes**: Automatic management or manual BLE control
  - **Complete BLE integration**: Remote configuration and real-time status
- **Growing Environment**: Configure plant type, soil, irrigation method, coverage, and sun exposure for optimal watering
- **Real-Time Task Monitoring**: Live progress tracking of current watering tasks with automatic updates
- **Smart Monitoring**: Track water volume with integrated flow sensor
- **Advanced Scheduling**: Weekday or interval-based automatic watering with environment-aware adjustments
- **Bluetooth Interface**: Complete remote monitoring and control via BLE
- **Flow Calibration**: Precise flow sensor calibration and anomaly detection
- **Plant-Specific Intelligence**: 26+ predefined plant types plus custom plant support
- **Coverage Flexibility**: Support both area-based (m²) and plant count measurements
- **Alarm System**: Comprehensive notification and fault detection
- **Energy Efficient**: Multiple power saving modes with intelligent scheduling
- **Real-Time Clock**: Precise timing for all operations
- **Timezone & DST Support**: Complete timezone management with automatic DST transitions

### 🕒 Timezone & DST Features (NEW in v1.2)

The system now includes comprehensive timezone and Daylight Saving Time support:

- **Global Timezone Support**: Configure any timezone from UTC-12 to UTC+14
- **Automatic DST**: Smart DST calculation with configurable start/end rules
- **Local Time Display**: All timestamps shown in local timezone
- **BLE Configuration**: Remote timezone setup via Bluetooth characteristic
- **Persistent Storage**: Timezone settings saved in NVS flash memory
- **Real-time Conversion**: Automatic UTC ↔ Local time conversion
- **DST Transition Handling**: Seamless transitions during spring/fall changes

**Supported DST Rules:**
- European rules (last Sunday March/October)
- US rules (2nd Sunday March, 1st Sunday November)  
- Custom rules (any month/week/day combination)
- Fixed timezones (no DST)

**Example: Romania Configuration**
```c
timezone_config_t romania = {
    .utc_offset_minutes = 2 * 60,    // UTC+2 (EET)
    .dst_enabled = 1,                // Enable DST
    .dst_offset_minutes = 60,        // +1h (EEST = UTC+3)
    .dst_start_month = 3, .dst_start_week = 5, .dst_start_dow = 0,  // Last Sunday March
    .dst_end_month = 10, .dst_end_week = 5, .dst_end_dow = 0        // Last Sunday October
};
```

### 🔄 Real-Time Task Monitoring (NEW in v1.1)

The system now provides live monitoring of active watering tasks:

- **Live Progress**: Real-time updates every 2 seconds during task execution
- **Dual Mode Support**: Monitor both duration-based and volume-based watering
- **Comprehensive Data**: Track channel, start time, progress, total volume, and status
- **Web Interface**: Complete JavaScript client for browser-based monitoring
- **Automatic Notifications**: BLE notifications sent automatically when tasks start, progress, or complete

**Example monitoring data:**
- Channel ID and watering mode
- Task start time and elapsed time
- Target vs. current values (seconds or ml)
- Total volume dispensed
- Real-time progress percentage

### 🌱 Growing Environment Features

The system includes comprehensive plant and environment configuration:

- **26 Plant Types**: Tomato, Lettuce, Basil, Pepper, Herbs, Flowers, Trees, and more
- **8 Soil Types**: Clay, Sandy, Loamy, Peat, Potting Mix, Hydroponic, etc.
- **6 Irrigation Methods**: Drip, Sprinkler, Soaker Hose, Mist, Flood, Subsurface
- **Smart Coverage**: Automatic recommendations for area vs. plant count measurement
- **Custom Plants**: Full support for user-defined plants with custom water factors
- **Sun Exposure**: 0-100% sunlight configuration affecting watering schedules
- **Intelligent Recommendations**: System suggests optimal measurement methods and water factors

## 🔧 Architecture Overview

The AutoWatering system is built with a modular architecture:

```
Core System Components:
├── Watering Engine          # Task scheduling and execution
├── Flow Monitoring          # Real-time water flow tracking
├── History System          # Multi-level data logging and analytics
├── Bluetooth API           # Comprehensive BLE interface
├── Storage Management      # Flash memory and NVS operations
├── Plant Configuration     # Species-specific watering parameters
└── Safety Systems          # Error detection and recovery
```

### Key Features

- **Multi-level History**: 30-day detailed events, 1-year daily stats, 5-year monthly trends
- **Smart Flow Monitoring**: Hardware-debounced pulse counting with 50ms response time
- **Plant-Specific Settings**: Customizable parameters for different plant types and growing conditions
- **Web Browser Compatible**: Fragmentation protocol for 20-byte MTU limitations
- **Real-time Notifications**: Live updates for all system components
- **Storage Optimization**: Automatic data aggregation and circular buffer management

### Storage Requirements

| Component | Current | Recommended |
|-----------|---------|-------------|
| **NVS Storage** | 8 KB | 144 KB |
| **History Capacity** | ~7 days | 30 days detailed |
| **Flash Usage** | 1.6% | 14.8% |

*Note: Enhanced storage configuration available via `boards/promicro_52840_enhanced.overlay`*

## 📄 License

[MIT License] © Project Authors

## 📧 Contact

Project Maintainer - [example@email.com](mailto:example@email.com)

Project Link: [https://github.com/AlexMihai1804/AutoWatering](https://github.com/AlexMihai1804/AutoWatering)

---

<p align="center">Made with ❤️ for smart and efficient irrigation</p>
