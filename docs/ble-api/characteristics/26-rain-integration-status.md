# Rain Integration Status Characteristic (UUID: 12345678-1234-5678-1234-56789abcde18)

Snapshot of the rain integration subsystem, exposing recent rainfall, per-channel adjustments, and storage utilisation.

> Operation Summary
| Operation | Payload | Size | Fragmentation | Notes |
|-----------|---------|------|---------------|-------|
| Read | `struct rain_integration_status_ble` | 78 B | None | Full snapshot refreshed on every read |
| Notify | 78 B snapshot or 10 B delta | 10 / 78 B | None | Full snapshot on subscribe; delta helper publishes channel-specific updates |
| Write | - | - | - | Configuration handled via Rain Sensor Config (#18) |

## Payload Layout (`struct rain_integration_status_ble`)
| Offset | Field | Type | Meaning |
|--------|-------|------|---------|
| 0 | `sensor_active` | `uint8_t` | 1 when rain sensor is responding |
| 1 | `integration_enabled` | `uint8_t` | 1 when integration logic applies adjustments |
| 2 | `last_pulse_time` | `uint32_t` | Unix seconds of the latest rain pulse |
| 6 | `calibration_mm_per_pulse` | `float` | Calibration constant (mm per pulse) |
| 10 | `rainfall_last_hour` | `float` | Millimetres accumulated in the current hour |
| 14 | `rainfall_last_24h` | `float` | Millimetres in the last 24 hours |
| 18 | `rainfall_last_48h` | `float` | Millimetres in the last 48 hours |
| 22 | `sensitivity_pct` | `float` | Integration sensitivity percentage |
| 26 | `skip_threshold_mm` | `float` | Rainfall threshold that triggers skips |
| 30 | `channel_reduction_pct[8]` | `float[8]` | Reduction percentages per channel |
| 62 | `channel_skip_irrigation[8]` | `uint8_t[8]` | Skip flags per channel (1 = skip) |
| 70 | `hourly_entries` | `uint16_t` | Hourly history entries retained |
| 72 | `daily_entries` | `uint16_t` | Daily history entries retained |
| 74 | `storage_usage_bytes` | `uint32_t` | Backend storage consumption (bytes) |

Structure size is exactly 78 bytes (build asserted in firmware). Floats follow IEEE-754 single precision, and multi-byte integers are little-endian.

## Notification Behaviour
- CCC enable triggers `read_rain_integration_status()` and immediately notifies the full 78-byte snapshot using normal priority scheduling.
- Runtime modules may call `bt_irrigation_rain_integration_notify(channel_id, reduction_pct, skip)` to emit a compact 10-byte delta frame (`uint8 channel`, `float reduction_pct`, `uint8 skip`, `uint32 timestamp`). The timestamp is derived from `k_uptime_get_32() / 1000`, so treat it as seconds since boot. Clients should accept either payload length to stay forward-compatible.
- No automatic periodic notification runs by default; firmware sends updates when rain activity or compensation logic deems it necessary.

## Data Semantics
- Rainfall totals and storage counters originate from the rain history subsystem; expect them to jump when history is purged or the sensor is recalibrated.
- `channel_reduction_pct[]` maps directly to the percentage reduction currently in effect for each channel. Values over zero but under 100 reduce runtime volume; `channel_skip_irrigation[]` marks complete skip decisions.
- Reduction percentages are expressed as 0-100 floats, while the optional 10-byte delta notification carries the same percentage as a 32-bit float and the timestamp in seconds since boot (derived from `k_uptime_get_32()/1000`); convert to your own epoch if needed.
- If the integration layer cannot pull a status, the firmware zeros the entire struct before responding.
- Calibration and sensitivity fields mirror configuration exposed in characteristic #18 so clients can display a consistent configuration view alongside live impact metrics.

## Client Guidance
- Treat zeroed rainfall totals with `sensor_active = 0` as "sensor offline" rather than "no rain".
- After receiving a 9-byte delta packet, issue a read if you need the full snapshot; firmware does not currently queue both shapes for the same event.
- UI surfaces should make it clear that storage counters are informational and can reset when logs are cleared.

## Related Characteristics
- `19-rain-data.md` - raw rain sensor telemetry.
- `20-rain-history-control.md` - command/response access to archived rainfall data.
- `25-compensation-status.md` - per-channel compensation view consuming rain integration results.
