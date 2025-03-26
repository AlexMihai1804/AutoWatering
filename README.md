# Automatic Plant Watering System

A Zephyr RTOS-based intelligent irrigation system that controls up to 8 watering zones with precise water volume measurement.

## Features

- **Multi-channel control:** Manages up to 8 independent watering zones
- **Precise water measurement:** Uses flow sensor to dispense exact water volumes
- **Flexible scheduling:** Supports daily or periodic watering schedules
- **Fault tolerance:** Automatic detection of flow anomalies and system faults
- **Low power operation:** Efficient design for battery-powered applications
- **Persistent configuration:** Saves settings across power cycles

## Hardware Requirements

- **MCU Board:** Pro Micro nRF52840 or compatible
- **Valves:** 8 solenoid valves (12V recommended)
- **Flow Sensor:** Water flow sensor with pulse output (YF-S201 or similar)
- **Power Supply:** 12V for valves, 5V or 3.3V for logic

## Devicetree Configuration

The project uses custom devicetree bindings for valve control and flow sensor:

```
/ {
    water_flow_sensor: water_flow_sensor {
        compatible = "gpio-flow-sensor";
        status = "okay";
        gpios = <&gpio0 6 (GPIO_ACTIVE_HIGH | GPIO_PULL_UP)>;
        label = "Water Flow Sensor";
    };
    
    valves {
        compatible = "gpio-valves";
        status = "okay";
        
        valve1 {
            gpios = <&gpio0 9 GPIO_ACTIVE_HIGH>;
            label = "Valve 1";
        };
        
        // ... other valves
    };
};
```

## Software Setup

1. Clone this repository
2. Build using Zephyr's build system:

```bash
west build -b promicro_52840
```

3. Flash to your device:

```bash
west flash
```

## Configuration

The system can be configured for:

- Flow sensor calibration (pulses per liter)
- Watering schedules (daily or periodic)
- Water quantity per zone (time-based or volume-based)

## Usage

After flashing, the system will:

1. Initialize all hardware components
2. Load saved configurations or set defaults
3. Run test cycle to verify all valves
4. Begin autonomous watering based on schedules

## Monitoring

The system provides detailed console output for monitoring:
- Water flow rates
- Valve activations
- Error conditions
- Scheduling information

## System Architecture

- **Main Application**: Overall control and monitoring
- **Flow Sensor Module**: Pulse counting and volume calculation
- **Watering Module**: Valve control and scheduling
- **Task System**: Concurrent operation management

## Troubleshooting

| Problem | Possible Solution |
|---------|-------------------|
| No water flow detected | Check water source, pressure, and valve operation |
| Unexpected water flow | Check valves for leaking or incomplete closure |
| System in fault mode | Reset the system by calling `watering_reset_fault()` |
