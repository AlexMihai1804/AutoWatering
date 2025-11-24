# Compensation Status Characteristic (UUID: 12345678-1234-5678-1234-56789abcde17)

> Operation Summary
| Operation | Payload | Size | Fragmentation | Notes |
|-----------|---------|------|---------------|-------|
| Write | Channel selector | 1 B | None | Values 0-7 or 0xFF auto-pick |
| Read | `struct compensation_status_data` | 40 B | None | Snapshot for selected channel |
| Notify | `struct compensation_status_data` | 40 B | None | Normal-priority notifications |

## Channel Selection
- Write a single byte to choose the channel whose compensation state will be exposed. Values outside `0-(WATERING_CHANNELS_COUNT-1)` and `0xFF` return `BT_ATT_ERR_VALUE_NOT_ALLOWED`.<br>- `0xFF` asks firmware to pick the first channel running an automatic mode; if none qualify the handler falls back to channel 0.<br>- The chosen channel id is cached in the characteristic value so subsequent reads (and firmware-triggered notifications) reuse it.

## Payload Layout (`struct compensation_status_data`)
| Offset | Field | Type | Meaning |
|--------|-------|------|---------|
| 0 | `channel_id` | `uint8_t` | Channel reflected in this snapshot |
| 1 | `rain_compensation_active` | `uint8_t` | 1 when rain logic modifies watering |
| 2 | `recent_rainfall_mm` | `float` | Millimetres of rain considered in last calc |
| 6 | `rain_reduction_percentage` | `float` | Percentage reduction currently applied |
| 10 | `rain_skip_watering` | `uint8_t` | 1 when cycle skipped due to rain |
| 11 | `rain_calculation_time` | `uint32_t` | Unix seconds of rain evaluation |
| 15 | `temp_compensation_active` | `uint8_t` | 1 when temperature logic active |
| 16 | `current_temperature` | `float` | Temperature used for compensation (deg C) |
| 20 | `temp_compensation_factor` | `float` | Multiplicative factor applied |
| 24 | `temp_adjusted_requirement` | `float` | Requirement after temperature adjustment |
| 28 | `temp_calculation_time` | `uint32_t` | Unix seconds of temperature evaluation |
| 32 | `any_compensation_active` | `uint8_t` | Logical OR of active flags |
| 33 | `reserved[7]` | `uint8_t[7]` | Always 0 |

All multi-byte values are little-endian; floats use single-precision IEEE-754. Total size is exactly 40 bytes.

## Behaviour Details
- Reads rebuild the struct using the cached channel id. If the requested channel cannot be loaded, firmware returns zeros for most fields and stamps both calculation times with `k_uptime_get_32()` so clients can detect fallbacks.<br>- Writes that change the selected channel update the value buffer; if notifications are enabled the handler immediately pushes the refreshed snapshot.<br>- Runtime modules call `bt_irrigation_compensation_status_notify(channel_id)` when compensation calculations complete. That path populates rainfall and temperature metrics from the latest computation (timestamps taken from the respective compensation subsystems).

## Practical Guidance
- Always select a channel before performing the first read to avoid unintentionally inspecting channel 0.<br>- Treat `recent_rainfall_mm` and `current_temperature` as optional. Some call paths deliver `0.0f` while integration with sensor modules matures; rely on reduction percentages and factors for definitive behaviour.<br>- When subscribing, be prepared for multiple notifications: immediate response to the write followed by future updates triggered by compensation recalculations.

## Related Interfaces
- `21-environmental-data.md` - source for temperature values used by temperature compensation.
- `24-rain-integration-status.md` - storage-level view of rain aggregation feeding compensation.
```javascript
async function selectChannelAndReadCompStatus(channelId) {
    // Optional: write single byte to select channel (0xFF = auto)
    const sel = new Uint8Array([channelId]);
    await compensationStatusChar.writeValue(sel); // ignored if not permitted
    const value = await compensationStatusChar.readValue();
    const dv = new DataView(value.buffer);
    return {
        channelId: dv.getUint8(0),
        rain: {
            active: dv.getUint8(1) !== 0,
            recentRainfallMm: dv.getFloat32(2, true), // often 0.0 currently
            reductionPct: dv.getFloat32(6, true),
            skip: dv.getUint8(10) !== 0,
            calcTime: dv.getUint32(11, true)
        },
        temperature: {
            active: dv.getUint8(15) !== 0,
            current: dv.getFloat32(16, true), // 0.0 if not sourced
            factor: dv.getFloat32(20, true),
            adjustedRequirement: dv.getFloat32(24, true),
            calcTime: dv.getUint32(28, true)
        },
        anyActive: dv.getUint8(32) !== 0
    };
}
```

## Backward Compatibility

Older 24B draft not shipped; only 40B layout should be assumed. No version byte currently present.

## Removed Draft Content

Removed prior example parsing a different 24B layout with multiple domain bitfields & warning flags - not implemented.

## Related Characteristics

- **[Environmental Data](21-environmental-data.md)** - Source data for compensation calculations
- **[Environmental History](22-environmental-history.md)** - Historical environmental trends
- **[Auto Calc Status](15-auto-calc-status.md)** - FAO-56 calculations using compensation
- **[Growing Environment](14-growing-environment.md)** - Plant-specific environmental settings
- **[Current Task Status](16-current-task-status.md)** - How compensation affects active tasks

## Notes

- Compensation packet built on read or channel-select notification; no fixed push interval.
- Rain & temperature factors originate from per-channel last compensation results; rainfall & temperature raw values not yet populated (0.0).
- Only rain skip & reduction percentage plus temp factor & adjusted requirement are surfaced.
- Logging occurs in core watering logic; this characteristic is read-only snapshot.
