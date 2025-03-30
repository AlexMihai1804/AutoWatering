# Hardware Guide

This guide provides detailed information about the hardware components, connections, and configuration for the AutoWatering system.

## Required Components

### Core Components

- **Controller Board**: NRF52840 ProMicro (or compatible Zephyr-supported board)
- **RTC Module**: DS3231 real-time clock for accurate timekeeping
- **Flow Sensor**: Pulse-generating water flow sensor (YF-S201 or similar)
- **Solenoid Valves**: 12V DC solenoid valves (up to 8)
- **Relay Module**: 8-channel relay module for valve control
- **Power Supply**: 12V DC power supply with sufficient current for valves

### Additional Components

- **Connectors**: Terminal blocks, jumper wires, waterproof connectors
- **Enclosure**: Weatherproof box to protect electronics
- **Circuit Protection**: Fuses, protection diodes
- **Buttons/Switches**: Manual control interfaces (optional)
- **Status LEDs**: Visual indicators (optional)

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

1. Install the flow sensor in-line with your water supply pipe
2. Ensure flow direction matches the arrow on the sensor
3. Connect the red wire to VCC (5V)
4. Connect the black wire to GND
5. Connect the yellow/signal wire to GPIO0_6
6. Use a pull-up resistor (10kΩ) between signal and VCC

## Valve Installation

1. Install the solenoid valves in your irrigation system
2. Connect each valve to its respective relay channel
3. Connect the relay common terminals to 12V power supply
4. Connect the relay control pins to the GPIO pins listed above
5. Ensure proper grounding of all components

## Power Requirements

- **Microcontroller**: 5V DC via USB or regulator
- **Solenoid Valves**: 12V DC, typically 200-500mA each
- **Total Power**: Depends on how many valves operate simultaneously
  - Single valve: 12V, ~500mA
  - System with all components: 12V, 2-3A recommended

## Hardware Configuration

The hardware configuration is defined in the devicetree overlay file:

```
boards/promicro_52840.overlay
```

If you modify any pin connections, update this file accordingly.

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

- Follow the [Installation Guide](INSTALLATION.md) to build and flash the firmware
- Configure the software using the [Software Guide](SOFTWARE.md)

[Back to main README](../README.md)
