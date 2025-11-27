# AutoWatering Complete Settings Reference

This document serves as the authoritative reference for every user-configurable setting in the AutoWatering firmware. It details the data types, valid ranges, units, and system behaviors associated with each parameter.

---

## 1. Channel Configuration (Per-Zone)

Each of the 8 channels (Zones 0-7) operates as an independent entity with its own configuration.

### 1.1 Basic Identity & Status
These settings define the fundamental properties of the zone.

| Parameter | Type | Range | Description |
| :--- | :--- | :--- | :--- |
| **Channel Name** | `String` | 64 chars | A human-readable identifier (e.g., "North Garden"). Used in logs and notifications. |
| **Auto Enabled** | `Boolean` | `0` / `1` | **Master Automation Switch**. <br>• `0 (OFF)`: The channel ignores all schedules and sensor data. Only manual BLE commands work.<br>• `1 (ON)`: The channel executes schedules and auto-watering logic. |

### 1.2 Physical Coverage
These settings determine the *scale* of water required. The system calculates water needs in millimeters (depth), which must be converted to Liters (volume) or Minutes (duration) based on these values.

| Parameter | Type | Unit | Description |
| :--- | :--- | :--- | :--- |
| **Coverage Type** | `Enum` | `0`: Area ($m^2$)<br>`1`: Plant Count | Defines the measurement mode.<br>• **Area**: Best for lawns, garden beds, or sprinklers.<br>• **Plant Count**: Best for drip emitters where each plant has a specific emitter. |
| **Coverage Value** | `Float` / `UInt16` | $m^2$ / count | **CRITICAL SETTING**. Directly multiplies the calculated water volume.<br>• *Example*: If the system calculates a need for 5mm of water:<br>  - Area $10m^2$ $\rightarrow$ 50 Liters.<br>  - Area $100m^2$ $\rightarrow$ 500 Liters. |
| **Sun Exposure** | `UInt8` | 0-100% | Represents the microclimate of the zone.<br>• `100%`: Full sun, maximum evaporation.<br>• `0%`: Full shade, reduced evaporation.<br>• *Impact*: Scales the Reference Evapotranspiration ($ET_0$). |

### 1.3 Growing Environment (Database)
Instead of manual tuning, the system uses agricultural databases (FAO-56 standard) to determine water needs.

| Parameter | Description |
| :--- | :--- |
| **Plant Type** | Selects the Crop Coefficient ($K_c$).<br>• *Standard Types*: Vegetables, Lawn, Trees, etc.<br>• *Database Index*: Links to `plant_full_db.inc` which contains growth stage curves (Initial, Mid, Late). |
| **Soil Type** | Selects standard soil physics profiles.<br>• *Types*: Sand, Loam, Clay, Silt.<br>• *Impact*: Determines "Field Capacity" (how much water soil holds) and "Infiltration Rate" (how fast it absorbs). |
| **Irrigation Method** | Defines application efficiency.<br>• *Drip*: ~90% efficiency (low waste).<br>• *Sprinkler*: ~75% efficiency (evaporation/wind loss).<br>• *Impact*: The system increases gross water volume to compensate for inefficiency. |

### 1.4 Custom Soil Physics (Advanced)
If the standard "Soil Type" presets are insufficient, you can enable `Use Custom Soil` and define specific hydrological parameters.

| Parameter | Unit | Description |
| :--- | :--- | :--- |
| **Field Capacity (FC)** | % Vol | The maximum amount of water the soil can hold against gravity. Any water added above this limit drains away (waste).<br>• *Sand*: ~10%<br>• *Clay*: ~35% |
| **Wilting Point (WP)** | % Vol | The moisture level at which plants die. The "Available Water" is `FC - WP`. |
| **Infiltration Rate** | mm/hr | **Crucial for Runoff Prevention**. The speed at which soil absorbs water.<br>• *Clay*: Very low (~5 mm/hr). Requires "Cycle & Soak".<br>• *Sand*: Very high (>20 mm/hr). |
| **Bulk Density** | g/cm³ | The weight of the soil. Used for advanced gravimetric calculations. |
| **Organic Matter** | % | Affects water retention capacity. Higher organic matter generally increases retention. |

### 1.5 Interval Mode (Cycle & Soak)
Designed to prevent runoff on soils with low infiltration rates (like Clay) or slopes.

| Parameter | Unit | Description |
| :--- | :--- | :--- |
| **Interval Mode Enabled** | `Bool` | If `ON`, large watering tasks are broken into sub-tasks. |
| **Watering Duration** | min/sec | The "ON" time for the valve. Should be short enough that water doesn't pool on the surface. |
| **Pause Duration** | min/sec | The "OFF" time. Should be long enough for the standing water to soak into the root zone. |

> **Example**: A 30-minute task on Clay soil with `5m Water / 15m Pause` will execute as:
> `5m ON` $\rightarrow$ `15m OFF` $\rightarrow$ `5m ON` $\rightarrow$ ... until 30m total ON time is reached.

### 1.6 Local Compensation Overrides
Each channel can override the global system defaults for weather compensation.

| Parameter | Description |
| :--- | :--- |
| **Rain Sensitivity** | `0.0 - 1.0`. Multiplier for rain data.<br>• `1.0`: Full trust. 10mm rain reduces watering by 10mm.<br>• `0.0`: Ignore rain.<br>• *Use Case*: A zone under a tree canopy might only receive 50% of actual rainfall (`0.5`). |
| **Rain Skip Threshold** | `mm`. If rain in the lookback period exceeds this, watering is skipped entirely. |
| **Temp Sensitivity** | Factor to adjust duration based on heat.<br>• *Positive*: Hotter = More Water.<br>• *Negative*: Hotter = Less Water (rare). |
| **Lookback Hours** | Time window to analyze past weather (e.g., `24h`, `48h`). |

---

## 2. System Configuration (Global)

These settings apply to the entire controller hardware.

### 2.1 Power & Hardware
| Parameter | Options | Description |
| :--- | :--- | :--- |
| **Power Mode** | `Normal`<br>`Energy-Saving`<br>`Ultra-Low` | • **Normal**: CPU/Radio active. Fast response.<br>• **Energy-Saving**: Radio sleeps (latency ~1-2s).<br>• **Ultra-Low**: Deep sleep. Wakes only for schedules. |
| **Flow Calibration** | `Pulses/Liter` | **Must be calibrated**. Defines how many electrical pulses from the flow meter equal 1 Liter of water. |

### 2.2 Master Valve Control
Controls the main pump or master solenoid that supplies the system.

| Parameter | Unit | Description |
| :--- | :--- | :--- |
| **Enabled** | `Bool` | If `ON`, the Master Valve output activates whenever *any* zone is watering. |
| **Pre-Delay** | Seconds | Time to open Master Valve *before* Zone Valve.<br>• *Positive*: Pressurize line before opening zone.<br>• *Negative*: Open zone first (rare). |
| **Post-Delay** | Seconds | Time to keep Master Valve open *after* Zone Valve closes.<br>• *Use*: Prevent water hammer or ensure full flush. |
| **Overlap Grace** | Seconds | If Zone A stops at 10:00 and Zone B starts at 10:00, this grace period keeps the Master Valve OPEN to prevent a "Stop-Start" cycle for the pump. |

### 2.3 Global Compensation Defaults
These values are used by any channel that doesn't have specific overrides set.
*   **Global Rain Sensitivity**: Default `1.0`.
*   **Global Temp Base**: The reference temperature (e.g., 25°C) where compensation factor is 1.0 (neutral).

### 2.4 Onboard Sensors (BME280)
| Parameter | Description |
| :--- | :--- |
| **BME280 Enabled** | Enables the I2C driver for the onboard Temp/Humidity/Pressure sensor. |
| **Measurement Interval** | Seconds between sensor readings. Higher = Better battery life. |

---

## 3. External Sensor Configuration

### 3.1 Rain Sensor Hardware
Configures the physical tipping bucket or optical rain sensor.

| Parameter | Description |
| :--- | :--- |
| **mm Per Pulse** | The volume of rain represented by one "click" of the sensor (e.g., `0.2794 mm`). Check sensor datasheet. |
| **Debounce Time** | `ms`. Minimum time between clicks to ignore electrical noise. |
| **Integration Enabled** | If `ON`, rain data is fed into the compensation algorithms. If `OFF`, rain is logged but doesn't affect watering. |

---

## 4. Automation & Lifecycle

### 4.1 Auto-Watering Strategies
When a channel is in `Auto Mode`, it calculates a "Water Deficit" ($D = ET_c - Rain_{effective}$).

| Strategy | Logic |
| :--- | :--- |
| **Manual** | Ignores deficit. Runs fixed schedules only. |
| **Quality** | **Replenish 100%**. Adds exactly what was lost. Maximizes growth. |
| **Eco** | **Replenish ~70%**. Allows "Managed Stress". Saves water, trains deeper roots. |
| **Max Volume Limit** | **Hard Cap**. Regardless of calculation, never dispense more than $X$ Liters in one session. Safety against leaks or calculation errors. |

### 4.2 Plant Lifecycle Tracking
Water needs change as plants grow.
*   **Planting Date**: Unix timestamp of when the crop was planted.
*   **Days After Planting**: Calculated automatically.
*   **Impact**: The system looks up the $K_c$ (Crop Coefficient) for the specific age of the plant.
    *   *Initial Stage*: Low water use.
    *   *Mid-Season*: Peak water use.
    *   *Late Season*: Reduced water use.

---

## 5. Reset & Maintenance
*   **Factory Reset**: Wipes ALL settings, schedules, and history.
*   **Config Reset**: Resets only settings to defaults; keeps history.
*   **Reboot**: Soft restart of the MCU.
