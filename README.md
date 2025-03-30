# AutoWatering - Zephyr RTOS-based Smart Irrigation System

[![Zephyr RTOS](https://img.shields.io/badge/Zephyr-RTOS-blue)](https://www.zephyrproject.org/)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![Version](https://img.shields.io/badge/Version-1.0-green.svg)](https://github.com/AlexMihai1804/AutoWatering)

<p align="center">
  <img src="docs/images/logo.png" alt="AutoWatering Logo" width="200"/>
</p>

## 📋 Overview

AutoWatering is a smart irrigation system built on Zephyr RTOS, designed for precision watering with monitoring capabilities. The system can manage up to 8 independent irrigation channels with automatic scheduling, flow monitoring, and remote control via Bluetooth.

### Key Highlights

- **Precision Control**: Manage up to 8 independent irrigation zones
- **Smart Monitoring**: Track water usage with integrated flow sensor
- **Wireless Control**: Configure and monitor via Bluetooth
- **Energy Efficient**: Multiple power saving modes
- **Reliable**: Fault detection and recovery mechanisms

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

- Control up to 8 independent irrigation channels
- Water volume monitoring with flow sensor
- Advanced scheduling (weekday or interval-based)
- Bluetooth interface for remote monitoring and control
- Flow sensor calibration and anomaly detection
- Alarm and notification system
- Energy-saving modes
- Real-time clock for precise scheduling operations

## 📄 License

[MIT License] © Project Authors

## 📧 Contact

Project Maintainer - [example@email.com](mailto:example@email.com)

Project Link: [https://github.com/AlexMihai1804/AutoWatering](https://github.com/AlexMihai1804/AutoWatering)

---

<p align="center">Made with ❤️ for smart and efficient irrigation</p>
