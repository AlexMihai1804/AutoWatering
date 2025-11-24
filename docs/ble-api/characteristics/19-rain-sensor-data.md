# Rain Sensor Data Characteristic (UUID: 12345678-1234-5678-1234-5678-9abcde13)

> Operation Summary
| Operation | Payload | Size | Fragmentation | Notes |
|-----------|---------|------|---------------|-------|
| Read | `struct rain_data_data` | 24 B | None | Snapshot composed from history and sensor state |
| Notify | `struct rain_data_data` | 24 B | None | Adaptive cadence plus pulse-driven updates |
| Write | - | - | - | Not supported |

This characteristic streams the most recent rainfall accumulation metrics, pulse statistics, and sensor health state. The payload matches the packed `struct rain_data_data` defined in `bt_gatt_structs.h`.

## Characteristic Metadata
| Item | Value |
|------|-------|
| UUID | `12345678-1234-5678-1234-5678-9abcde13` |
| Properties | Read, Notify |
| Permissions | Read |
| Payload Size | 24 bytes (`BUILD_ASSERT(sizeof(struct rain_data_data) == 24)`) |
| Notification Priority | Normal (`bt_gatt_notify`) |
| CCC Handler | `rain_data_ccc_changed` |

## Payload Layout (`struct rain_data_data`)
| Offset | Field | Type | Description |
|--------|-------|------|-------------|
| 0 | `current_hour_mm_x100` | `uint32_t` | Rainfall this hour x100 (0.01 mm resolution) |
| 4 | `today_total_mm_x100` | `uint32_t` | Local-day total x100 |
| 8 | `last_24h_mm_x100` | `uint32_t` | Trailing 24-hour accumulation x100 |
| 12 | `current_rate_mm_h_x100` | `uint16_t` | Instantaneous rate mm/h x100 |
| 14 | `last_pulse_time` | `uint32_t` | Timestamp (UTC seconds) of last pulse |
| 18 | `total_pulses` | `uint32_t` | Total pulses since last reset |
| 22 | `sensor_status` | `uint8_t` | 0 = inactive, 1 = active, 2 = error |
| 23 | `data_quality` | `uint8_t` | Heuristic 0-100 quality score |

`data_quality` mirrors `rain_sensor_data_t.data_quality` when the driver provides it. During periodic snapshots the firmware currently reports `80` while active and `0` when inactive or in error; pulse-triggered notifications reuse the instantaneous quality from the driver.

## Behaviour

### Read (`read_rain_data`)
- Validates parameters, then queries the rain history subsystem (`rain_history_get_current_hour`, `rain_history_get_today`, `rain_history_get_last_24h`) for accumulations.
- Populates rate, pulse counts, and status using `rain_sensor_get_data()`. When the driver call fails, the firmware marks `sensor_status = 2` and `data_quality = 0`.
- Copies the populated struct into the attribute buffer and returns it via `bt_gatt_attr_read`.

### Notifications
- `rain_data_ccc_changed` immediately schedules `bt_irrigation_rain_data_notify()` when CCC is enabled so subscribers receive an initial snapshot.
- `bt_irrigation_rain_data_notify()` rebuilds the payload and sends it through `bt_gatt_notify(NULL, ...)` whenever:
  - The periodic worker `bt_irrigation_rain_periodic_update()` decides the cadence (30 s while raining, 5 min when idle).
  - The sensor status changes (`rain_last_status_sent` guard).
  - A BLE client enables notifications.
- `bt_irrigation_rain_pulse_notify()` supplements the periodic cadence with pulse-driven updates, throttled to one every 5 seconds to avoid flooding during heavy rain. It includes the latest pulse count, rate, and data quality from `rain_sensor_get_data()`.
- All notifications share the same 24-byte payload and are not fragmented.

## Errors
- Read/write argument validation failures return `-EINVAL` (mapped to ATT Invalid Offset/Length).
- Sensor retrieval failures revert to status `2` and zeroed quality but still return a valid frame; no ATT error is raised.
- `bt_gatt_notify` failures are logged but otherwise ignored by the caller.

## Client Guidance
- Divide fields ending with `_x100` by 100 to obtain millimetres or mm/h in human-readable units.
- Treat `sensor_status != 1` as "no current rain"; the accumulation counters may still increase as history rolls over.
- Use `data_quality` to gate alerts—`0` means the driver detected an error or the sensor is offline, while values above ~50 denote healthy pulse cadence; sudden drops usually precede rain-related alarms.
- Combine these readings with the rain history characteristic (`20`) for historical analysis and with the integration status characteristic (`21`) to assess watering impact.

## Firmware References
- `src/bt_irrigation_service.c`: `read_rain_data`, `rain_data_ccc_changed`, `bt_irrigation_rain_data_notify`, `bt_irrigation_rain_pulse_notify`, `bt_irrigation_rain_periodic_update`.
- `src/rain_sensor.c`: data acquisition, status, and quality scoring.
- `src/rain_history.c`: hourly/daily accumulation helpers consumed during reads.
- `include/bt_gatt_structs.h`: `struct rain_data_data` definition and size assertion.
