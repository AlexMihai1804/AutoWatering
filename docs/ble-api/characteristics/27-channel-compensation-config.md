# Channel Compensation Config Characteristic (UUID: 12345678-1234-5678-1234-56789abcde19)

> Operation Summary
| Operation | Payload | Size | Fragmentation | Notes |
|-----------|---------|------|---------------|-------|
| Read | `struct channel_compensation_config_data` | 44 B | None | Returns config for selected channel |
| Write (1 byte) | Channel selector | 1 B | None | Selects channel for subsequent reads |
| Write (44 bytes) | Full config | 44 B | None | Updates per-channel compensation settings |
| Notify | `struct channel_compensation_config_data` | 44 B | None | Sent after config changes |

This characteristic exposes **per-channel rain and temperature compensation settings**, allowing mobile apps to configure individual channel thresholds instead of relying solely on global defaults from Rain Sensor Config (#18).

## Characteristic Metadata
| Item | Value |
|------|-------|
| UUID | `12345678-1234-5678-1234-56789abcde19` |
| Properties | Read, Write, Notify |
| Permissions | Read (encrypted), Write (encrypted) |
| Payload Size | 44 bytes |
| Notification Priority | Normal |

## ⚠️ Important: Compensation Mode Restrictions

**Compensation only applies to TIME and VOLUME watering modes.**

For FAO-56 automatic modes (`AUTO_QUALITY`, `AUTO_ECO`), compensation is **never applied** because:
- FAO-56 already incorporates rainfall data into ET₀-based water balance calculations
- Temperature is already factored into ET₀ via Penman-Monteith or Hargreaves-Samani equations
- Applying compensation on top of FAO-56 would **double-count** weather impacts

For FAO-56 channels, even if compensation is enabled, the `*_active` flags in Compensation Status (#23) will show inactive.

## Payload Layout (`struct channel_compensation_config_data`)

| Offset | Field | Type | Size | Access | Description | Validation |
|--------|-------|------|------|--------|-------------|------------|
| 0 | `channel_id` | `uint8_t` | 1 | RW | Channel ID (0-7) | 0-7 |
| 1 | `rain_enabled` | `uint8_t` | 1 | RW | Rain compensation enable | 0 or 1 |
| 2 | `rain_sensitivity` | `float` | 4 | RW | Sensitivity factor | 0.0-1.0 |
| 6 | `rain_lookback_hours` | `uint16_t` | 2 | RW | Hours to look back | 1-72 |
| 8 | `rain_skip_threshold_mm` | `float` | 4 | RW | Skip threshold (mm) | 0.0-100.0 |
| 12 | `rain_reduction_factor` | `float` | 4 | RW | Reduction factor | 0.0-1.0 |
| 16 | `temp_enabled` | `uint8_t` | 1 | RW | Temperature compensation enable | 0 or 1 |
| 17 | `temp_base_temperature` | `float` | 4 | RW | Base temperature (°C) | -40.0 to 60.0 |
| 21 | `temp_sensitivity` | `float` | 4 | RW | Temperature sensitivity | 0.1-2.0 |
| 25 | `temp_min_factor` | `float` | 4 | RW | Minimum factor | 0.5-1.0 |
| 29 | `temp_max_factor` | `float` | 4 | RW | Maximum factor | 1.0-2.0 |
| 33 | `last_rain_calc_time` | `uint32_t` | 4 | R | Last rain calculation (Unix) | Read-only |
| 37 | `last_temp_calc_time` | `uint32_t` | 4 | R | Last temp calculation (Unix) | Read-only |
| 41 | `reserved[3]` | `uint8_t[3]` | 3 | - | Reserved | Ignored |

All multi-byte values are little-endian. Floats use IEEE-754 single precision.

## Field Descriptions

### Rain Compensation Fields

| Field | Purpose | Typical Value |
|-------|---------|---------------|
| `rain_enabled` | Master switch for rain-based adjustments | 1 (enabled) |
| `rain_sensitivity` | How aggressively rain affects watering (0=ignore, 1=full effect) | 0.75 |
| `rain_lookback_hours` | Hours of rain history to consider | 24 |
| `rain_skip_threshold_mm` | Skip watering entirely if recent rain exceeds this | 5.0 mm |
| `rain_reduction_factor` | Reduce watering duration/volume by this factor when rain detected | 0.5 |

### Temperature Compensation Fields

| Field | Purpose | Typical Value |
|-------|---------|---------------|
| `temp_enabled` | Master switch for temperature-based adjustments | 1 (enabled) |
| `temp_base_temperature` | Reference temperature for calculations | 25.0 °C |
| `temp_sensitivity` | How much temperature changes affect watering | 1.0 |
| `temp_min_factor` | Minimum multiplier (cold days) | 0.7 |
| `temp_max_factor` | Maximum multiplier (hot days) | 1.5 |

## Operations

### Select Channel (1-byte Write)

Write a single byte to select which channel's config will be returned on subsequent reads:

```javascript
// Select channel 2 for reading
await characteristic.writeValue(new Uint8Array([2]));
```

Values outside 0-7 return `BT_ATT_ERR_VALUE_NOT_ALLOWED`.

### Read Configuration

Returns the 44-byte config for the currently selected channel (defaults to 0):

```javascript
const value = await characteristic.readValue();
const dv = new DataView(value.buffer);

const config = {
    channelId: dv.getUint8(0),
    rain: {
        enabled: dv.getUint8(1) !== 0,
        sensitivity: dv.getFloat32(2, true),
        lookbackHours: dv.getUint16(6, true),
        skipThresholdMm: dv.getFloat32(8, true),
        reductionFactor: dv.getFloat32(12, true),
    },
    temp: {
        enabled: dv.getUint8(16) !== 0,
        baseTemperature: dv.getFloat32(17, true),
        sensitivity: dv.getFloat32(21, true),
        minFactor: dv.getFloat32(25, true),
        maxFactor: dv.getFloat32(29, true),
    },
    lastRainCalcTime: dv.getUint32(33, true),
    lastTempCalcTime: dv.getUint32(37, true),
};
```

### Write Configuration (44-byte Write)

Write the full 44-byte struct to update a channel's compensation settings:

```javascript
async function setChannelCompensation(channelId, rainConfig, tempConfig) {
    const buffer = new ArrayBuffer(44);
    const dv = new DataView(buffer);
    
    dv.setUint8(0, channelId);
    
    // Rain compensation
    dv.setUint8(1, rainConfig.enabled ? 1 : 0);
    dv.setFloat32(2, rainConfig.sensitivity, true);
    dv.setUint16(6, rainConfig.lookbackHours, true);
    dv.setFloat32(8, rainConfig.skipThresholdMm, true);
    dv.setFloat32(12, rainConfig.reductionFactor, true);
    
    // Temperature compensation
    dv.setUint8(16, tempConfig.enabled ? 1 : 0);
    dv.setFloat32(17, tempConfig.baseTemperature, true);
    dv.setFloat32(21, tempConfig.sensitivity, true);
    dv.setFloat32(25, tempConfig.minFactor, true);
    dv.setFloat32(29, tempConfig.maxFactor, true);
    
    // Timestamps and reserved are ignored on write
    dv.setUint32(33, 0, true);
    dv.setUint32(37, 0, true);
    dv.setUint8(41, 0);
    dv.setUint8(42, 0);
    dv.setUint8(43, 0);
    
    await characteristic.writeValue(new Uint8Array(buffer));
}

// Example: Enable rain compensation on channel 0
await setChannelCompensation(0, {
    enabled: true,
    sensitivity: 0.75,
    lookbackHours: 24,
    skipThresholdMm: 5.0,
    reductionFactor: 0.5,
}, {
    enabled: true,
    baseTemperature: 25.0,
    sensitivity: 1.0,
    minFactor: 0.7,
    maxFactor: 1.5,
});
```

## Validation Errors

| Check | Condition | Error |
|-------|-----------|-------|
| Payload length | != 1 and != 44 | `BT_ATT_ERR_INVALID_ATTRIBUTE_LEN` |
| `channel_id` | >= 8 | `BT_ATT_ERR_VALUE_NOT_ALLOWED` |
| `rain_sensitivity` | < 0.0 or > 1.0 | `BT_ATT_ERR_VALUE_NOT_ALLOWED` |
| `rain_lookback_hours` | < 1 or > 72 | `BT_ATT_ERR_VALUE_NOT_ALLOWED` |
| `rain_skip_threshold_mm` | < 0.0 or > 100.0 | `BT_ATT_ERR_VALUE_NOT_ALLOWED` |
| `rain_reduction_factor` | < 0.0 or > 1.0 | `BT_ATT_ERR_VALUE_NOT_ALLOWED` |
| `temp_base_temperature` | < -40.0 or > 60.0 | `BT_ATT_ERR_VALUE_NOT_ALLOWED` |
| `temp_sensitivity` | < 0.1 or > 2.0 | `BT_ATT_ERR_VALUE_NOT_ALLOWED` |
| `temp_min_factor` | < 0.5 or > 1.0 | `BT_ATT_ERR_VALUE_NOT_ALLOWED` |
| `temp_max_factor` | < 1.0 or > 2.0 | `BT_ATT_ERR_VALUE_NOT_ALLOWED` |

## Notifications

Notifications are sent after:
- Successful configuration write
- CCC subscription enable (initial snapshot)

## Relationship to Other Characteristics

| Characteristic | Relationship |
|----------------|--------------|
| **Rain Sensor Config (#18)** | Provides global defaults; this char provides per-channel overrides |
| **Compensation Status (#23)** | Shows runtime compensation results (reduction %, skip flags) |
| **Growing Environment (#14)** | Contains `auto_mode` which determines if compensation applies |
| **Rain Integration Status (#26)** | Shows per-channel reduction percentages and skip flags |

## Onboarding Integration

When compensation is enabled via this characteristic:
- `CHANNEL_EXT_FLAG_RAIN_COMP_SET` is set in onboarding extended flags (for rain)
- `CHANNEL_EXT_FLAG_TEMP_COMP_SET` is set in onboarding extended flags (for temp)

## Best Practices

1. **Check watering mode first**: Only configure compensation for TIME/VOLUME mode channels
2. **Use reasonable defaults**: Start with sensitivity 0.75, lookback 24h, skip threshold 5mm
3. **Monitor results**: Use Compensation Status (#23) to verify compensation is being applied
4. **Consider local climate**: Adjust `rain_lookback_hours` based on typical rain patterns

## Firmware References

- `src/bt_irrigation_service.c`: `read_channel_comp_config`, `write_channel_comp_config`, `channel_comp_config_ccc_changed`
- `src/bt_gatt_structs.h`: `struct channel_compensation_config_data` definition
- `src/watering.h`: `rain_compensation` and `temp_compensation` fields in `watering_channel_t`

## Version History

| Date | Change |
|------|--------|
| Dec 2025 | Initial release - exposes per-channel compensation settings via BLE |
