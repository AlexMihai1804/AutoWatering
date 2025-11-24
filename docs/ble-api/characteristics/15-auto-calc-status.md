# Auto Calculation Status Characteristic (UUID: de00)

> Operation Summary
| Operation | Payload | Size | Fragmentation | Notes |
|-----------|---------|------|---------------|-------|
| Read | `struct auto_calc_status_data` | 64 B | None | Returns the latest snapshot for the currently cached channel |
| Write | Channel selector (`uint8_t`) | 1 B | None | `0-7` selects a channel, `0xFF` scans for the first channel in automatic mode |
| Notify | `history_fragment_header_t` + struct | 8 B + 64 B | Single fragment | Immediate on subscribe, on channel change, on internal updates, and every 30 min while enabled |

Auto Calculation Status exposes the FAO-56 based irrigation planner for one watering channel at a time. The characteristic mirrors the structure used by the firmware when computing ET0, crop coefficients, water balance, and cycle/soak recommendations. Data originates from `update_auto_calc_calculations()` in `src/bt_irrigation_service.c`, which consolidates plant metadata, environmental readings, and the channel's water balance state.

## Characteristic Metadata
| Item | Value |
|------|-------|
| UUID | `12345678-1234-5678-1234-56789abcde00` |
| Properties | Read, Write, Notify |
| Permissions | Read, Write |
| Payload Size | 64 bytes (`BUILD_ASSERT(sizeof(struct auto_calc_status_data) == 64)`) |
| Notification Envelope | 8-byte `history_fragment_header_t` + payload |
| Notification Priority | Normal (`safe_notify` / `advanced_notify`) |
| Periodic Worker | `auto_calc_status_periodic` every 30 minutes while CCC is enabled |

## Payload Layout (`struct auto_calc_status_data`)
| Offset | Field | Type | Description |
|--------|-------|------|-------------|
| 0 | `channel_id` | `uint8_t` | Channel being reported (0-7) |
| 1 | `calculation_active` | `uint8_t` | `1` when the channel is in an automatic mode (`WATERING_AUTOMATIC_*`) |
| 2 | `irrigation_needed` | `uint8_t` | `1` when the water balance currently requests irrigation |
| 3 | `current_deficit_mm` | `float` | Soil water deficit (mm) from the water balance model |
| 7 | `et0_mm_day` | `float` | Reference evapotranspiration computed from BME280 data (Penman-Monteith with HS fallback) |
| 11 | `crop_coefficient` | `float` | Crop coefficient (Kc) for the plant and phenological stage |
| 15 | `net_irrigation_mm` | `float` | Net irrigation requirement |
| 19 | `gross_irrigation_mm` | `float` | Gross irrigation including efficiency losses |
| 23 | `calculated_volume_l` | `float` | Recommended irrigation volume (litres) |
| 27 | `last_calculation_time` | `uint32_t` | Unix timestamp of the last completed calculation |
| 31 | `next_irrigation_time` | `uint32_t` | Predicted next irrigation time (UTC seconds, `0` if unknown) |
| 35 | `days_after_planting` | `uint16_t` | Days after planting as tracked by the channel |
| 37 | `phenological_stage` | `uint8_t` | Stage index (0-3) produced by the plant database helper |
| 38 | `quality_mode` | `uint8_t` | Manual/Quality/Eco selection (0,1,2) |
| 39 | `volume_limited` | `uint8_t` | `1` when capped by `max_volume_limit_l` |
| 40 | `auto_mode` | `uint8_t` | Copy of the channel's `watering_mode_t` (0=duration, 1=volume, 2=automatic quality, 3=automatic eco) |
| 41 | `raw_mm` | `float` | Raw rainfall accumulated since the previous refresh |
| 45 | `effective_rain_mm` | `float` | Rainfall credited to the balance after infiltration constraints |
| 49 | `calculation_error` | `uint8_t` | `1` when the latest computation failed (fields should be treated as stale) |
| 50 | `etc_mm_day` | `float` | Crop evapotranspiration (`et0_mm_day x crop_coefficient`) |
| 54 | `volume_liters` | `float` | Legacy alias of `calculated_volume_l` |
| 58 | `cycle_count` | `uint8_t` | Recommended number of cycle-and-soak passes (`>= 1`) |
| 59 | `cycle_duration_min` | `uint8_t` | Recommended duration per cycle (minutes, clamped to 255) |
| 60 | `reserved` | `uint8_t[4]` | Zero-filled for future expansion |

### Field Notes
- `calculation_active` follows the channel's `auto_mode`. Manual channels still surface diagnostic data but should not trigger irrigation automatically.
- `irrigation_needed` reflects the live water balance; the calculation may still produce `calculated_volume_l = 0` if a deficit exists but the planner defers watering.
- `phenological_stage` and `crop_coefficient` are sourced from `plant_full_database` using days-after-planting heuristics.
- `raw_mm` and `effective_rain_mm` are pulled from the channel's `water_balance_t` structure to keep rainfall traces aligned with irrigation scheduling.
- `cycle_count` defaults to one but increases when the planner enables cycle-and-soak to mitigate runoff.

## Behaviour

### Read (`read_auto_calc_status`)
- Uses the channel id cached in the attribute buffer (defaults to `0`). Invalid selections fall back to channel `0`.
- Rebuilds the payload on the fly: copies water balance flags, calls `update_auto_calc_calculations()`, and, when necessary, estimates `next_irrigation_time` via `calc_irrigation_timing()`.
- On failure to access the requested channel, returns a zeroed frame with the requested channel id and `calculation_active = 0`.
- Read requests always return raw 64-byte payloads; the unified header is notification-only.

### Write (`write_auto_calc_status`)
- Accepts a single byte at offset `0`.
    - `0-7`: pin the characteristic to that channel.
    - `0xFF`: search for the first channel running `WATERING_AUTOMATIC_QUALITY` or `WATERING_AUTOMATIC_ECO`; if none are found the selection reverts to `0`.
- Values `>= WATERING_CHANNELS_COUNT` raise `BT_ATT_ERR_VALUE_NOT_ALLOWED`. Any other length/offset returns the corresponding ATT error.
- A successful write updates `auto_calc_status_value.channel_id`, triggers `bt_irrigation_auto_calc_status_notify()`, and (when notifications are active) refreshes the 30-minute worker schedule.

### Notifications (`notify_auto_calc_status` / `auto_calc_status_ccc_changed`)
- CCC enable:
    - Initializes the cached payload from channel `0` (including ET/plant calculus) and clears stale fields on failure.
    - Seeds the delayable work item and immediately sends a snapshot.
- Each notification consists of one fragment: the 8-byte `history_fragment_header_t` (with `data_type = 0`, `status = 0`, `entry_count = 1`, `fragment_index = 0`, `total_fragments = 1`, `fragment_size = 64`) followed by the struct.
- `update_auto_calc_calculations()` runs immediately before every notify to guarantee fresh ET/deficit values.
- The periodic worker (`auto_calc_status_periodic`) re-queues itself every 1 800 000 ms (30 min) while notifications remain enabled. Manual triggers (writes or internal events) do not affect the cadence; they simply send additional frames.
- CCC disable clears the cached struct, cancels the delayable work, and stops all further notifications.

## Error Handling
- Invalid write length -> `BT_ATT_ERR_INVALID_ATTRIBUTE_LEN`.
- Non-zero write offset -> `BT_ATT_ERR_INVALID_OFFSET`.
- Channel selection outside `[0, WATERING_CHANNELS_COUNT)` -> `BT_ATT_ERR_VALUE_NOT_ALLOWED`.
- Notification attempts while the connection is absent or CCC disabled are silently ignored (`0` returned after logging).

## Client Guidance
- Reads return the struct directly. Notifications prepend the unified header; clients must skip the first 8 bytes before parsing.
- Treat `calculation_error = 1` as "data stale" and prompt the user to inspect diagnostics/environmental sensors before acting on the numbers.
- `next_irrigation_time = 0` means either no irrigation is currently required or the heuristic could not predict a timestamp (e.g., missing plant data).
- When switching channels, issue the write first and wait for the notify before relying on the read response so both views stay consistent.
- `volume_limited = 1` signals the recommendation hit the configured `max_volume_limit_l`; user interfaces should highlight the cap.

## Firmware References
- `src/bt_irrigation_service.c`: `write_auto_calc_status`, `read_auto_calc_status`, `auto_calc_status_ccc_changed`, `notify_auto_calc_status`, `update_auto_calc_calculations`, periodic worker helpers.
- `src/watering.c`, `src/watering_history.c`: water balance integration and irrigation scheduling.
- `src/environmental_data.c`, `src/plant_db_api.c`, `src/fao56_calc.c`: sensor ingestion and FAO-56 computation utilities.