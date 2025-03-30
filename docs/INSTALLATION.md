# Installation Guide

This guide covers the process of installing, building, and flashing the AutoWatering system.

## Prerequisites

Before you begin, make sure you have the following:

- **Zephyr RTOS Environment** (version 3.0 or newer)
- **ARM GCC Compiler** 
- **West build tool**
- **Development environment** (recommended: Visual Studio Code with Zephyr extension)
- **Hardware components** listed in the [Hardware Guide](HARDWARE.md)

## Setting Up the Development Environment

### 1. Install Zephyr RTOS

Follow the [official Zephyr getting started guide](https://docs.zephyrproject.org/latest/develop/getting_started/index.html) to set up your development environment.

```bash
# Install dependencies (Ubuntu example)
sudo apt install --no-install-recommends git cmake ninja-build gperf ccache dfu-util device-tree-compiler wget python3-dev python3-pip python3-setuptools python3-tk python3-wheel xz-utils file make gcc gcc-multilib g++-multilib libsdl2-dev

# Install west
pip3 install west

# Get Zephyr source code
west init ~/zephyrproject
cd ~/zephyrproject
west update
west zephyr-export

# Install Python dependencies
pip3 install -r ~/zephyrproject/zephyr/scripts/requirements.txt
```

### 2. Clone the AutoWatering Repository

```bash
cd ~/zephyrproject
git clone https://github.com/AlexMihai1804/AutoWatering.git
cd AutoWatering
```

### 3. Install Additional Dependencies

```bash
pip3 install -r requirements.txt
west update
```

## Building the Project

### 1. Build for NRF52840 ProMicro

```bash
# Navigate to the project directory
cd ~/zephyrproject/AutoWatering

# Build the project
west build -b nrf52840_promicro
```

### 2. Configuration Options

You can customize the build using the following commands:

```bash
# Open menuconfig to adjust settings
west build -t menuconfig

# Edit the prj.conf file directly
nano prj.conf
```

### 3. Flash to the Device

Connect your NRF52840 ProMicro board and flash the firmware:

```bash
west flash
```

If you have multiple boards connected, you might need to specify the runner:

```bash
west flash --runner nrfjprog
# or
west flash --runner jlink
```

### 4. Monitor Debug Output

To view the system logs and debug information:

```bash
west attach
```

## Devicetree Configuration

The system uses Devicetree for hardware configuration. The main overlay file is:

```
boards/promicro_52840.overlay
```

Edit this file if you need to change pin assignments for valves or the flow sensor.

## Updating the Firmware

To update an existing installation:

1. Pull the latest changes:
   ```bash
   git pull
   ```

2. Clean the build directory if necessary:
   ```bash
   west build -t clean
   ```

3. Rebuild and flash:
   ```bash
   west build -b nrf52840_promicro
   west flash
   ```

## Next Steps

- Configure your hardware following the [Hardware Guide](HARDWARE.md)
- Learn about the software architecture in the [Software Guide](SOFTWARE.md)
- Set up the Bluetooth interface using the [Bluetooth API Documentation](BLUETOOTH.md)

[Back to main README](../README.md)
