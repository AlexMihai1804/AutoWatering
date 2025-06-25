# Hardware Guide

This guide provides detailed information about the hardware components, connections, and configuration for the AutoWatering system.

## Required Components

### Core Components

- **Controller Board**: NRF52840 ProMicro (or compatible nRF52840-based board)
- **RTC Module**: DS3231 real-time clock with backup battery for accurate timekeeping
- **Flow Sensor**: Pulse-generating water flow sensor (YF-S201 or similar, 5V compatible)
- **Solenoid Valves**: 12V DC solenoid valves (up to 8 channels supported)
- **Relay Module**: 8-channel relay module for valve control (5V coil, 12V switching)
- **Power Supply**: 12V DC power supply with sufficient current for valves (3A recommended)

### Additional Components

- **USB Cable**: For programming and console access (micro-USB or USB-C depending on board)
- **Connectors**: Terminal blocks, jumper wires, waterproof connectors for outdoor installation
- **Enclosure**: Weatherproof box to protect electronics (IP65 or better)
- **Circuit Protection**: Fuses, protection diodes for valve circuits
- **Pull-up Resistors**: 10kΩ resistor for flow sensor signal line
- **Status LEDs**: Optional visual indicators for system status
- **Backup Battery**: CR2032 for DS3231 RTC (typically included with module)

## Connection Diagram

```
                           ┌───────────────┐
                           │               │
                           │   NRF52840    │
                           │   ProMicro    │
                           │               │
                           └───┬───┬───┬───┘
                               │   │   │
                 I2C           │   │   │         GPIO
          ┌──────────────────┐ │   │   │ ┌─────────────────────┐
          │                  │ │   │   │ │                     │
    ┌─────┴─────┐      ┌─────┴─┐ │   │ └┴────┐      ┌─────────┐
    │  DS3231   │      │ Flow  │ │   │      ┌┴─┐    │ Valve 1 │
    │  RTC      │      │ Sensor│ │   │      │R │    └─────────┘
    └───────────┘      └───────┘ │   │      │e │    ┌─────────┐
                                 │   │      │l │    │ Valve 2 │
                    Bluetooth    │   │      │a │    └─────────┘
                   ┌─────────────┴┐  │      │y │        ...
                   │  Mobile App  │  │      │s │    ┌─────────┐
                   │  or Gateway  │  │      │  │    │ Valve 8 │
                   └──────────────┘  │      └──┘    └─────────┘
                                     │
                                ┌────┴─────┐
                                │  Debug   │
                                │  Console │
                                └──────────┘
```

## Pin Connections

### GPIO Pin Assignments

| Component | GPIO Pin | Description |
|-----------|----------|-------------|
| Flow Sensor | GPIO0_6 | Interrupt-capable input pin |
| Valve 1 | GPIO0_9 | Output to relay 1 |
| Valve 2 | GPIO0_10 | Output to relay 2 |
| Valve 3 | GPIO1_11 | Output to relay 3 |
| Valve 4 | GPIO1_13 | Output to relay 4 |
| Valve 5 | GPIO1_15 | Output to relay 5 |
| Valve 6 | GPIO0_2 | Output to relay 6 |
| Valve 7 | GPIO0_29 | Output to relay 7 |
| Valve 8 | GPIO0_31 | Output to relay 8 |

### I2C Connections for DS3231 RTC

| RTC Pin | NRF52840 Pin | Description |
|---------|--------------|-------------|
| VCC | 3.3V | Power supply |
| GND | GND | Ground |
| SDA | SDA | I2C data line |
| SCL | SCL | I2C clock line |
| SQW | GPIO0_14 | Square wave output (optional) |

## Flow Sensor Installation

1. **Sensor Placement**:
   - Install the flow sensor in-line with your main water supply pipe
   - Ensure flow direction matches the arrow marked on the sensor body
   - Position after any filters but before the valve manifold

2. **Electrical Connections**:
   - Connect the red wire to VCC (5V or 3.3V depending on sensor)
   - Connect the black wire to GND
   - Connect the yellow/signal wire to GPIO0_6 on the nRF52840
   - **Important**: Add a 10kΩ pull-up resistor between signal and VCC

3. **Sensor Specifications**:
   - Flow rate range: 1-30 L/min (typical for YF-S201)
   - Pulse rate: ~450 pulses per liter (varies by model)
   - Operating voltage: 5-18V DC
   - Working pressure: ≤1.75 MPa

## Valve Installation

1. **Valve Placement**:
   - Install solenoid valves in your irrigation distribution manifold
   - Ensure proper flow direction (usually marked with an arrow)
   - Install after the flow sensor in the water path

2. **Electrical Connections**:
   - Connect each valve to its respective relay channel (NO terminal)
   - Connect all relay common terminals to 12V power supply positive
   - Connect valve negative terminals to power supply ground
   - Connect relay control pins to the GPIO pins listed above

3. **Valve Specifications**:
   - Operating voltage: 12V DC
   - Current consumption: 200-500mA per valve
   - Operating pressure: Up to 10 bar (varies by model)
   - Thread size: Typically 1/2" or 3/4" BSP/NPT

## Power Requirements

- **Microcontroller**: 5V DC via USB or 3.3V regulated supply
- **Solenoid Valves**: 12V DC, typically 200-500mA each
- **Flow Sensor**: 5V DC, ~10mA
- **RTC Module**: 3.3V, ~1mA (plus backup battery)
- **Relay Module**: 5V for coils, 12V switching capability
- **Total Power Consumption**:
  - Idle: ~100mA at 12V
  - Single valve active: ~600mA at 12V  
  - Multiple valves: Add ~500mA per additional valve
  - **Recommended supply**: 12V, 3A for reliable operation

**Power Supply Selection**:
- Use a switching power supply for efficiency
- Ensure adequate current rating for peak usage
- Consider adding a fuse (3A) for protection
- For outdoor installation, use a weatherproof supply

## Hardware Configuration

The hardware configuration is defined in the devicetree overlay file:

```
boards/promicro_52840.overlay
```

If you modify any pin connections, update this file accordingly. Key configuration sections:

- **GPIO pin assignments** for valves and sensors
- **I2C configuration** for RTC communication  
- **USB descriptor** settings for console access
- **Bluetooth** device name and advertising parameters

## System Interfaces

### USB Interface
- **Purpose**: Programming, debugging, and console access
- **Type**: CDC-ACM virtual COM port
- **Speed**: Full-speed USB 2.0
- **Console**: Available at 115200 baud for system monitoring

### Bluetooth Interface  
- **Standard**: Bluetooth Low Energy (BLE) 5.0
- **Range**: Up to 100m line-of-sight (Class 2)
- **Services**: Custom irrigation service with 13 characteristics
- **Security**: Open (no pairing required in default configuration)
- **Device Name**: "AutoWatering"

### I2C Interface
- **Purpose**: RTC communication
- **Speed**: 100 kHz (standard mode)
- **Address**: 0x68 (DS3231 RTC)
- **Pull-ups**: Internal pull-ups enabled

## Optional Components

### Manual Control Panel

For systems requiring manual control without Bluetooth:
- Add push buttons connected to additional GPIO pins
- Add status LEDs for visual feedback
- Configure these in the devicetree overlay

### Battery Backup

For systems installed in areas with unreliable power:
- Add a 12V sealed lead-acid or LiFePO4 battery
- Add a solar panel and charge controller if needed
- Add a voltage monitoring circuit to GPIO ADC

## Weatherproofing

1. Place all electronics in a waterproof enclosure (IP65 or better)
2. Use waterproof connectors for external connections
3. Install the enclosure away from direct water exposure
4. Ensure proper ventilation to prevent condensation

## Next Steps

After hardware setup:

- Follow the [Installation Guide](INSTALLATION.md) to build and flash the firmware
- Configure the software using the [Software Guide](SOFTWARE.md)
- Set up remote monitoring with the [Bluetooth API](BLUETOOTH.md)
- Test your installation and troubleshoot any issues using the [Troubleshooting Guide](TROUBLESHOOTING.md)

## Documentation Version

This hardware guide is current as of June 2025 and covers hardware setup for firmware version 1.6.

[Back to main README](../README.md)
