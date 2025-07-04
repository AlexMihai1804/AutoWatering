# AutoWatering - Zephyr RTOS-based Smart Irrigation System

[![Zephyr RTOS](https://img.shields.io/badge/Zephyr-RTOS-blue)](https://www.zephyrproject.org/)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![Version](https://img.shields.io/badge/Version-1.2-green.svg)](https://github.com/AlexMihai1804/AutoWatering)
[![BLE Reliability](https://img.shields.io/badge/BLE-Enhanced-brightgreen.svg)](docs/BLUETOOTH.md)

<p align="center">
  <img src="docs/images/logo.png" alt="AutoWatering Logo" width="200"/>
</p>

## 📋 Overview

AutoWatering is a smart irrigation system built on Zephyr RTOS, designed for precision watering with monitoring capabilities. The system can manage up to 8 independent irrigation channels with automatic scheduling, flow monitoring, and remote control via Bluetooth.

### Recent Improvements (July 2025) 🚀

- **🔧 Complete BLE Implementation**: All 15 characteristics fully implemented with proper handlers
- **📡 Enhanced Notification System**: 500ms throttling with CCC state checking and automatic error recovery
- **🔄 Background Updates**: Dedicated thread for automatic status updates every 2 seconds
- **⚡ Simple & Reliable**: Direct notifications without complex queuing for maximum stability
- **📊 Real-time Monitoring**: Complete current task tracking with automatic progress updates
- **🎯 Verified Implementation**: All API functions implemented and documentation updated to match
- **🔄 Error Recovery**: Notification system automatically recovers after temporary errors
- **📈 Complete Documentation**: All BLE API functions documented with exact implementation details

### Key Highlights

- **Precision Control**: Manage up to 8 independent irrigation zones
- **Smart Monitoring**: Track water usage with integrated flow sensor
- **Wireless Control**: Configure and monitor via Bluetooth (fully implemented with 15 characteristics)
- **Energy Efficient**: Multiple power saving modes
- **Reliable**: Fault detection and recovery mechanisms
- **Enhanced BLE**: 500ms notification throttling with CCC state checking and error recovery
- **Complete API**: All BLE functions implemented with background updates and direct notifications
- **Verified Documentation**: Implementation and documentation match exactly

## 📑 Documentation

This project's documentation is organized into several sections:

| Document | Description |
|----------|-------------|
| [📦 Installation Guide](docs/INSTALLATION.md) | Setup instructions and build process |
| [🔩 Hardware Guide](docs/HARDWARE.md) | Hardware components and wiring details |
| [💻 Software Guide](docs/SOFTWARE.md) | Software architecture and code examples |
| [🔌 Bluetooth API](docs/BLUETOOTH.md) | Complete Bluetooth interface documentation |
| [🛠️ Troubleshooting](docs/TROUBLESHOOTING.md) | Common issues and solutions |
| [🤝 Contributing](docs/CONTRIBUTING.md) | Guidelines for contributors |

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
