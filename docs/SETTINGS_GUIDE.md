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
| **Auto Enabled** | `Boolean` | `0` / `1` | **Master automation switch**. `0 (OFF)` ignores schedules and sensor data; manual BLE commands only. `1 (ON)` executes schedules and auto-watering logic. |

### 1.2 Physical Coverage
These settings determine the scale of water required. The system calculates water needs in millimeters (depth), which must be converted to liters (volume) or minutes (duration) based on these values.

| Parameter | Type | Unit | Description |
| :--- | :--- | :--- | :--- |
| **Coverage Type** | `Enum` | `0`: Area (m^2)<br>`1`: Plant Count | Defines the measurement mode. **Area**: best for lawns, beds, or sprinklers. **Plant Count**: best for drip emitters with per-plant output. |
| **Coverage Value** | `Float` / `UInt16` | m^2 / count | **Critical setting.** Directly multiplies the calculated water volume. Example: 5 mm over 10 m^2 -> 50 L; 5 mm over 100 m^2 -> 500 L. |
| **Sun Exposure** | `UInt8` | 0-100% | Represents the microclimate of the zone. 100 = full sun; 0 = full shade. Scales the reference evapotranspiration (ET0). |

### 1.3 Growing Environment (Database)
Instead of manual tuning, the system uses agricultural databases (FAO-56 standard) to determine water needs.

| Parameter | Description |
| :--- | :--- |
| **Plant Type** | Selects the crop coefficient (Kc). Standard types are grouped for legacy compatibility; the enhanced database index is the authoritative source. |
| **Soil Type** | Selects standard soil physics profiles (field capacity, wilting point, infiltration). |
| **Irrigation Method** | Defines application efficiency (drip, sprinkler, etc.) and influences gross water volume. |

### 1.4 Custom Soil Physics (Advanced)
If the standard soil presets are insufficient, you can enable `Use Custom Soil` and define specific parameters.

| Parameter | Unit | Description |
| :--- | :--- | :--- |
| **Field Capacity (FC)** | % vol | Maximum water held against gravity. Water above this drains away. |
| **Wilting Point (WP)** | % vol | Moisture level at which plants fail. Available water is `FC - WP`. |
| **Infiltration Rate** | mm/hr | Determines runoff risk. Low values benefit from cycle-and-soak. |
| **Bulk Density** | g/cm^3 | Soil mass per volume. Used in advanced calculations. |
| **Organic Matter** | % | Higher organic matter improves retention. |

### 1.5 Interval Mode (Cycle & Soak)
Designed to prevent runoff on soils with low infiltration rates (like clay) or slopes.

| Parameter | Unit | Description |
| :--- | :--- | :--- |
| **Interval Mode Enabled** | `Bool` | If `ON`, large watering tasks are broken into sub-tasks. |
| **Watering Duration** | min/sec | Valve ON time. Keep short enough to avoid surface pooling. |
| **Pause Duration** | min/sec | Valve OFF time. Allow water to soak into root zone. |

> **Example**: A 30-minute task with `5m Water / 15m Pause` executes as:
> `5m ON` -> `15m OFF` -> `5m ON` -> ... until 30m total ON time is reached.

### 1.6 Local Compensation Overrides
Each channel can override compensation settings.

| Parameter | Description |
| :--- | :--- |
| **Rain Sensitivity** | `0.0 - 1.0`. Multiplier for rain data. 1.0 = full trust. 0.0 = ignore rain. |
| **Rain Skip Threshold** | mm. If rain in the lookback period exceeds this, watering is skipped. **Applies only to TIME/VOLUME modes**. FAO-56 modes already incorporate rainfall. |
| **Temp Sensitivity** | Factor to adjust watering for temperature. Configuration is stored and reported, but **not applied in the current task execution path**. |
| **Lookback Hours** | Time window to analyze recent rain (e.g., 24h, 48h). |

---

## 2. System Configuration (Global)

These settings apply to the entire controller hardware.

### 2.1 Power & Hardware
| Parameter | Options | Description |
| :--- | :--- | :--- |
| **Power Mode** | `Normal`<br>`Energy-Saving`<br>`Ultra-Low` | Adjusts scheduler sleep (60/120/300 s). Zephyr PM is disabled (`CONFIG_PM=n`), so no deep sleep is entered. |
| **Flow Calibration** | `Pulses/Liter` | Defines how many flow pulses equal 1 liter of water. Must be non-zero. |

### 2.2 Master Valve Control
Controls the main pump or master solenoid that supplies the system.

| Parameter | Unit | Description |
| :--- | :--- | :--- |
| **Enabled** | `Bool` | If `ON`, the master valve activates whenever any zone is watering. |
| **Pre-Delay** | Seconds | Time to open master valve before the zone valve. Negative values mean open after. |
| **Post-Delay** | Seconds | Time to keep master valve open after zone closes. Negative values mean close before. |
| **Overlap Grace** | Seconds | Keeps master valve open between consecutive tasks to avoid stop/start cycling. |

### 2.3 Global Compensation Defaults
- **Rain compensation**: Global rain fields are deprecated and ignored; per-channel settings are authoritative.
- **Temperature compensation**: Global enable/base/sensitivity are applied to all channels when System Configuration is written. Defaults are base 20 C, sensitivity 0.05, min factor 0.5, max factor 2.0.

### 2.4 Onboard Sensors (BME280)
| Parameter | Description |
| :--- | :--- |
| **BME280 Enabled** | Enables the I2C driver for the onboard temperature/humidity/pressure sensor. |
| **Measurement Interval** | Seconds between sensor readings (default 60 s in the BME280 driver). |

---

## 3. External Sensor Configuration

### 3.1 Rain Sensor Hardware
Configures the physical tipping bucket or optical rain sensor.

| Parameter | Description |
| :--- | :--- |
| **mm Per Pulse** | Rain depth per pulse (e.g., 0.2 mm). Check sensor datasheet. |
| **Debounce Time** | ms. Minimum time between pulses to ignore noise. |
| **Integration Enabled** | Stored flag only; skip/reduction logic uses per-channel compensation enables. |

---

## 4. Automation & Lifecycle

### 4.1 Watering Modes
The system supports four watering modes:

| Mode | Code | Type | Description |
| :--- | :--- | :--- | :--- |
| **Duration (TIME)** | `0` | Manual | Waters for a fixed time (e.g., 10 minutes). |
| **Volume** | `1` | Manual | Waters until a specific volume is reached (e.g., 5 liters). |
| **Quality** | `2` | FAO-56 Auto | Calculates water need using ET0 and replenishes **100%** of deficit. |
| **Eco** | `3` | FAO-56 Auto | Calculates water need using ET0 and replenishes **~70%** of deficit. |

> **Important:** Quality and Eco modes are **exclusively FAO-56 based**. They require plant/soil/method configuration and valid coverage (area or plant count).

### 4.2 Compensation Behaviour by Mode

| Feature | TIME/VOLUME | QUALITY/ECO (FAO-56) |
| :--- | :--- | :--- |
| **Rain Skip** | Yes (per-channel threshold) | No (rain already in ET0) |
| **Rain Reduction** | Yes | No (rain already in ET0) |
| **Temp Compensation** | Configured, but not applied | Not applied |
| **Scientific Calculation** | No | Yes (Penman-Monteith/Hargreaves) |

### 4.3 Auto-Watering Strategies (FAO-56 Modes Only)
When a channel uses Quality or Eco mode, it calculates a water deficit (D = ETc - effective rain).

| Strategy | Logic |
| :--- | :--- |
| **Quality** | Replenish 100%. Adds exactly what was lost. Maximizes growth. |
| **Eco** | Replenish ~70%. Allows managed stress to save water. |
| **Max Volume Limit** | Hard cap. Never dispense more than the configured maximum in one session. |

### 4.4 Plant Lifecycle Tracking
Water needs change as plants grow.
- **Planting Date**: Unix timestamp of when the crop was planted.
- **Days After Planting**: Calculated automatically.
- **Impact**: Kc is derived from the plant database stage data.

---

## 5. Reset & Maintenance
- **Factory Reset**: Wipes all settings, schedules, and history.
- **Config Reset**: Resets settings to defaults; keeps history.
- **Reboot**: Soft restart of the MCU.
