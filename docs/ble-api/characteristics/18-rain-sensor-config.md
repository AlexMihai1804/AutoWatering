# Rain Sensor Configuration Characteristic (UUID: 12345678-1234-5678-1234-5678-9abcde12)

> Operation Summary
| Operation | Payload | Size | Fragmentation | Notes |
|-----------|---------|------|---------------|-------|
| Read | `struct rain_config_data` | 18 B | None | Snapshot rebuilt for every request |
| Write | `struct rain_config_data` | 18 B | None | Full frame only; applies settings to sensor and integration modules |
| Notify | `struct rain_config_data` | 18 B | None | Sent on CCC enable, after validated writes, and from internal triggers |

Provides remote access to the tipping-bucket calibration and the rain-integration tuning that influences irrigation skips. The on-wire layout matches the packed `struct rain_config_data` defined in `bt_gatt_structs.h`.

## Characteristic Metadata
| Item | Value |
|------|-------|
| UUID | `12345678-1234-5678-1234-5678-9abcde12` |
| Properties | Read, Write, Notify |
| Permissions | Read, Write |
| Payload Size | 18 bytes (`BUILD_ASSERT(sizeof(struct rain_config_data) == 18)`) |
| Notification Priority | Normal (`safe_notify`) |
| CCC Handler | `rain_config_ccc_changed` |

## Payload Layout (`struct rain_config_data`)
| Offset | Field | Type | Description |
|--------|-------|------|-------------|
| 0 | `mm_per_pulse` | `float` | Calibration in millimetres per pulse (`0.1...10.0`) |
| 4 | `debounce_ms` | `uint16_t` | Debounce window in milliseconds (`10...1000`) |
| 6 | `sensor_enabled` | `uint8_t` | `1` enables the hardware, `0` leaves it idle |
| 7 | `integration_enabled` | `uint8_t` | `1` allows rain-based irrigation adjustments |
| 8 | `rain_sensitivity_pct` | `float` | Sensitivity curve weight (`0...100`) |
| 12 | `skip_threshold_mm` | `float` | Rainfall threshold that skips irrigation (`0...100`) |
| 16 | `reserved[2]` | `uint8_t[2]` | Reserved; echoed back unchanged |

### Field Notes
- Reads pull live values from the sensor/integration subsystems when the sensor is active; otherwise defaults (`0.2`, `50`, disabled flags, `75%`, `5 mm`) are returned.
- Sensor and integration enable flags are independent to support logging without automation.
- Reserved bytes are not used; zero-fill for forward compatibility.

## Behaviour

### Read (`read_rain_config`)
- Validates parameters and composes a fresh struct via `rain_sensor_get_calibration`, `rain_sensor_get_debounce`, `rain_sensor_is_enabled`, `rain_sensor_is_integration_enabled`, `rain_integration_get_sensitivity`, and `rain_integration_get_skip_threshold`.
- Falls back to default constants when the sensor has not yet been configured or is disabled.
- Copies the struct into `rain_config_value` and returns it with `bt_gatt_attr_read`.

### Write (`write_rain_config`)
- Requires exactly 18 bytes at offset `0`; other sizes or offsets return `-EINVAL` (mapped to `BT_ATT_ERR_INVALID_ATTRIBUTE_LEN` / `BT_ATT_ERR_INVALID_OFFSET`).
- Copies the payload into a local struct and checks each numeric field against the allowed ranges listed below.
- Applies settings in order, stopping on first failure:
  1. `rain_sensor_set_calibration`
  2. `rain_sensor_set_debounce`
  3. `rain_sensor_set_enabled`
  4. `rain_sensor_set_integration_enabled`
  5. `rain_integration_set_sensitivity`
  6. `rain_integration_set_skip_threshold`
- Persists changes via `rain_sensor_save_config()` and `rain_integration_save_config()` when all setters succeed.
- Updates the attribute buffer and, if notifications are enabled, uses `safe_notify` to confirm the applied configuration.

### Notifications (`rain_config_ccc_changed` / `bt_irrigation_rain_config_notify`)
- CCC enable immediately pushes a snapshot so subscribers see the current calibration without issuing a read.
- Other firmware modules call `bt_irrigation_rain_config_notify()` whenever configuration changes originate outside BLE (e.g., local maintenance mode).
- Notification payloads are identical to read responses; no fragmentation or headers are used.

## Validation & Errors
| Check | Accepted Values | Result on Failure |
|-------|-----------------|-------------------|
| Payload length | 18 bytes | `-EINVAL` -> `BT_ATT_ERR_INVALID_ATTRIBUTE_LEN` |
| Offset | 0 | `-EINVAL` -> `BT_ATT_ERR_INVALID_OFFSET` |
| `mm_per_pulse` | `0.1...10.0` | `-EINVAL` |
| `debounce_ms` | `10...1000` | `-EINVAL` |
| `rain_sensitivity_pct` | `0...100` | `-EINVAL` |
| `skip_threshold_mm` | `0...100` | `-EINVAL` |
| Sensor/integration setters | Hardware-specific errors | `-EIO` -> ATT Unlikely Error |

Reserved bytes are ignored; whatever the client sends is echoed back.

## Client Guidance
- Perform a read-modify-write cycle when changing individual fields to avoid overwriting unrelated settings.
- Treat all numeric values as little-endian floats/integers; conversions should use IEEE-754 single precision.
- Expect default calibration values while the sensor is disabled and highlight that state in UI.
- Correlate configuration changes with rain telemetry (`19`) and integration status (`21`) to confirm runtime impact.

## Firmware References
- `src/bt_irrigation_service.c`: `read_rain_config`, `write_rain_config`, `rain_config_ccc_changed`, `bt_irrigation_rain_config_notify`.
- `src/rain_sensor.c`: calibration setters, enable flags, persistence helpers.
- `src/rain_integration.c`: sensitivity and skip-threshold storage.
- `include/bt_gatt_structs.h`: `struct rain_config_data` layout and size assertion.