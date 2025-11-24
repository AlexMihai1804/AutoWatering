## System Configuration Characteristic

The System Configuration characteristic delivers and accepts the canonical 56-byte `struct enhanced_system_config_data` snapshot that drives every system-wide behaviour: controller power modes, flow calibration, master valve orchestration, BME280 environmental sampling, and the global rain/temperature compensation baselines that are pushed into each irrigation channel. This document spells out all semantics exactly as implemented inside `src/bt_irrigation_service.c` (functions `read_system_config`, `write_system_config`, and `system_config_ccc_changed`) and `include/bt_gatt_structs_enhanced.h`. Nothing is left implicit-if a field is ignored, clamped, or persisted elsewhere, it is documented below.

### Characteristic Metadata
| Item | Value |
|------|-------|
| UUID | `12345678-1234-5678-1234-56789abcdef6` |
| Properties | Read, Write, Notify |
| Permissions | Read, Write |
| Fixed Payload Size | 56 bytes (packed, little-endian) |
| Fragmentation | Required for writes when MTU < 56; handled via standard ATT long write semantics (offset accumulation, no app-defined header) |
| Notification Priority | Normal priority through `safe_notify()` with adaptive throttle (starts at 200 ms, expands up to ~=2 s when the queue backs up) |

### Runtime Role
`system_config_value` (56 B) is the single working buffer inside the firmware. Reads populate it on demand from live subsystems; writes stream fragments into it and, once complete, drive a validation -> apply -> persist pipeline. `system_config_bytes_received` tracks how many bytes of the current write have landed. Disabling CCC purges the buffer to guarantee that stale data is never echoed back.

### Payload Layout (byte-accurate)
All integers are little-endian. Floats are IEEE-754 single precision.

| Offset | Field | Type | Size (B) | Access | Units / Encoding | Source on Read | Handling on Write |
|--------|-------|------|----------|--------|------------------|----------------|-------------------|
| 0 | `version` | `uint8_t` | 1 | R | Constant `2` | Hard-coded (`config->version = 2`) | Ignored; firmware overwrites with `2` before notify |
| 1 | `power_mode` | `uint8_t` | 1 | R/W | 0=Normal, 1=Energy-Saving, 2=Ultra-Low | `watering_get_power_mode()` -> `power_mode_t` | Validated 0-2; applied via `watering_set_power_mode()` |
| 2 | `flow_calibration` | `uint32_t` | 4 | R/W | Pulses per liter | `get_flow_calibration()` | Must be 100-10000; applied via `set_flow_calibration()` (persists internally) |
| 6 | `max_active_valves` | `uint8_t` | 1 | R | Always `1` | Build constant | Ignored on write |
| 7 | `num_channels` | `uint8_t` | 1 | R | Always equals `WATERING_CHANNELS_COUNT` (currently 8) | Build constant | Ignored on write |
| 8 | `master_valve_enabled` | `uint8_t` | 1 | R/W | 0=disabled, 1=enabled | `master_valve_get_config()` | Routed into `master_valve_set_config()` |
| 9 | `master_valve_pre_delay` | `int16_t` | 2 | R/W | Seconds (can be negative) | Ditto | Routed into `master_valve_set_config()` |
| 11 | `master_valve_post_delay` | `int16_t` | 2 | R/W | Seconds (can be negative) | Ditto | Routed into `master_valve_set_config()` |
| 13 | `master_valve_overlap_grace` | `uint8_t` | 1 | R/W | Seconds | Ditto | Routed into `master_valve_set_config()` |
| 14 | `master_valve_auto_mgmt` | `uint8_t` | 1 | R/W | 0=manual, 1=automatic | Ditto | Routed into `master_valve_set_config()` |
| 15 | `master_valve_current_state` | `uint8_t` | 1 | R | 0=closed, 1=open | `master_valve_get_config()` | Ignored on write |
| 16 | `bme280_enabled` | `uint8_t` | 1 | R/W | 0/1 | `bme280_system_get_config()` fallback defaults | Forwarded into `sensor_manager_configure_bme280()` (best-effort) |
| 17 | `bme280_measurement_interval` | `uint16_t` | 2 | R/W | Seconds | Same as above | Non-zero updates the interval immediately; `0` leaves the previously configured interval in place |
| 19 | `bme280_sensor_status` | `uint8_t` | 1 | R | 0=missing,1=ok,2=error,3=disabled | `sensor_manager` status | Ignored on write |
| 20 | `global_rain_compensation_enabled` | `uint8_t` | 1 | R/W | 0/1 | `rain_integration_get_config()` | Applied via `rain_integration_set_config()` (+ save) |
| 21 | `global_temp_compensation_enabled` | `uint8_t` | 1 | R/W | 0/1 | Derived from channels with temp comp enabled | Propagated to every channel struct |
| 22 | `global_rain_sensitivity` | `float` | 4 | R/W | 0.0-1.0 (fraction) | Rain integration (converted from % -> fraction) | Clamped 0.0-1.0; stored as fraction, converted back to % for subsystem |
| 26 | `global_temp_sensitivity` | `float` | 4 | R/W | Factor (see `TEMP_COMP_MIN/MAX_SENSITIVITY`) | Averaged from enabled channels or defaults | Clamped to [`TEMP_COMP_MIN_SENSITIVITY`, `TEMP_COMP_MAX_SENSITIVITY`] and pushed to channels |
| 30 | `global_rain_lookback_hours` | `uint16_t` | 2 | R/W | Hours | Rain integration config | Applied directly |
| 32 | `global_rain_skip_threshold` | `float` | 4 | R/W | Millimetres | Rain integration config | Applied directly |
| 36 | `global_temp_base_temperature` | `float` | 4 | R/W | deg C | Averaged or defaults | Clamped to [`TEMP_COMP_MIN_TEMP_C`, `TEMP_COMP_MAX_TEMP_C`] and pushed to channels |
| 40 | `interval_mode_active_channels` | `uint8_t` | 1 | R | Bitmask, bit n <-> channel n | `enhanced_system_is_interval_mode_active()` | Ignored on write |
| 41 | `compensation_active_channels` | `uint8_t` | 1 | R | Bitmask | Computed from per-channel rain/temp enable flags | Ignored on write |
| 42 | `incomplete_config_channels` | `uint8_t` | 1 | R | Bitmask | `enhanced_system_has_incomplete_config()` | Ignored on write |
| 43 | `environmental_data_quality` | `uint8_t` | 1 | R | 0-100 | Derived from environmental validation score (temperature/humidity/pressure) | Ignored on write |
| 44 | `last_config_update` | `uint32_t` | 4 | R | Unix UTC seconds | `timezone_get_unix_utc()` at read time | Ignored on write |
| 48 | `last_sensor_reading` | `uint32_t` | 4 | R | Unix UTC seconds | Latest environmental sample or fallback to now | Ignored on write |
| 52 | `reserved` | `uint8_t[4]` | 4 | R/W | Must be zero | Persisted sentinel | Must write zeros |
| 59 | `reserved[8]` | `uint8_t[8]` | 8 | R/W | Must be zero | Preserved | Caller must write zeros |

### Read Flow (step-by-step)
1. `bt_gatt_attr_read()` requests the snapshot; firmware fills a local `enhanced_system_config_data enhanced_config` structure.
2. Core fields come from watering engine helpers (`watering_get_power_mode`, `get_flow_calibration`).
3. Master valve parameters and `master_valve_current_state` mirror the most recent `master_valve_config_t`.
4. The BME block is initialised using `bme280_system_get_config()` when the sensor manager is active; otherwise safe defaults (disabled, 60 s interval, status 0) are used.
5. Rain defaults are loaded via `rain_integration_get_config()` and normalised to fractional sensitivity (0.0-1.0) before storage.
6. Temperature defaults are computed by averaging the live channel configurations: if any channel has temperature compensation enabled we average `sensitivity` and `base_temperature`; otherwise we fall back to `TEMP_COMP_DEFAULT_*` and flag the compensation disabled.
7. Bitmaps are built using helper utilities (`enhanced_system_is_interval_mode_active`, `enhanced_system_has_incomplete_config`, plus a manual iteration that ORs any channel with rain or temp compensation enabled).
8. Environmental quality queries `environmental_data_get_current()`. When valid, the reading is re-validated and scored (0-100) via `env_data_calculate_quality_score()` using temperature, humidity, and pressure. The accompanying timestamp becomes `last_sensor_reading`; if no reading is available the timestamp collapses to the current Unix time.
9. `last_config_update` is set to `timezone_get_unix_utc()` for every read.
10. The populated struct is copied to the GATT response; read logging is rate-limited (>=5 s between logs) but has no effect on payload timing.

### Write Flow (exact order)
1. Every `write_system_config()` call validates pointer arguments; invalid handles return `BT_ATT_ERR_INVALID_HANDLE`.
2. The fragment is bounds-checked. Any `offset + len > 56` triggers `BT_ATT_ERR_INVALID_OFFSET` and ignores the fragment.
3. Valid fragments are memcpy'd into `system_config_value` at the given offset. `system_config_bytes_received` is updated to `offset + len`.
4. Only when `offset + len == 56` does the firmware treat the transfer as complete and continue:
   - `power_mode` range check (0-2). Failure -> `BT_ATT_ERR_VALUE_NOT_ALLOWED`.
   - `flow_calibration` check (100-10000). Failure -> `BT_ATT_ERR_VALUE_NOT_ALLOWED`.
   - When BME280 is enabled and `measurement_interval` is zero the firmware keeps the prior interval (no ATT error).
   - If all basic validation passes, systems are updated in this order:
   1. Power mode via `watering_set_power_mode()`. On `WATERING_ERROR_BUSY` the write aborts with `BT_ATT_ERR_UNLIKELY`, signalling "retry later".
   2. Flow calibration via `set_flow_calibration()` (which persists internally).
   3. Master valve config using `master_valve_set_config()` (invalid combinations propagate `BT_ATT_ERR_VALUE_NOT_ALLOWED`).
   4. BME280 config: the previous config is fetched, interval/value sanitised, and forwarded to `sensor_manager_configure_bme280()`. Non-zero return values are logged as warnings but do not cancel the write.
   5. Rain integration: fractional sensitivity is clamped to `[0.0, 1.0]`, converted back to percent (x100), pushed into `rain_integration_set_config()`, and persisted with `rain_integration_save_config()`.
   6. Temperature defaults: sensitivity is clamped to [`TEMP_COMP_MIN_SENSITIVITY`, `TEMP_COMP_MAX_SENSITIVITY`], base temperature to [`TEMP_COMP_MIN_TEMP_C`, `TEMP_COMP_MAX_TEMP_C`]. Each channel retrieved via `watering_get_channel()` has its temperature block overwritten (enable flag, base, sensitivity, min/max factors reset to `TEMP_COMP_DEFAULT_MIN/MAX_FACTOR`).
   7. `watering_save_config_priority(true)` asks NVS to persist the broader watering configuration (ensures master valve + temp defaults survive resets).
5. If notifications are enabled (`notification_state.system_config_notifications_enabled`), the firmware rewrites read-only fields (`version`, `max_active_valves`, `num_channels`) in-place before calling `safe_notify()` with the complete 56-byte buffer.
6. Firmware logs a success message and the write returns the original `len`, as required by GATT.

**Important:** There is no parallelism guard. Issuing a second write before receiving the confirmation notification can race the first transaction because fragments share the same buffer. Clients must treat the notification (or read-back verification) as the commit acknowledgement.

### Notification Cadence and Semantics
- Channel: same 56-byte struct as read/write.
- Trigger: every successful validated write. The system does **not** emit periodic heartbeats.
- Priority: Normal priority path within the adaptive BLE scheduler. First eligible notify goes out after >=200 ms; high link load can stretch the interval up to about 2 s.
- Source: `safe_notify(default_conn, attr, system_config_value, sizeof(struct enhanced_system_config_data))`.
- Side effect: `system_config_value` retains the final, refreshed data until the next write or CCC disable. CCC disable clears the buffer and resets `system_config_bytes_received` to prevent stale snapshots.

### Validation Rules and Constants
| Field(s) | Rule | Firmware Constant / Function | Failure Code |
|----------|------|------------------------------|--------------|
| `power_mode` | 0 <= value <= 2 | Enum `power_mode_t`; enforced in `watering_set_power_mode()` | `BT_ATT_ERR_VALUE_NOT_ALLOWED` (invalid) or `BT_ATT_ERR_UNLIKELY` (busy) |
| `flow_calibration` | 100 <= value <= 10000 pulses/L | Limits inline | `BT_ATT_ERR_VALUE_NOT_ALLOWED` |
| BME measurement interval | 0 = keep prior interval; >0 applied | Passed to sensor manager without additional validation | None (zero is a no-op) |
| Rain sensitivity | Clamped to 0.0-1.0 | Hard clamp inside handler | No hard failure; out-of-range is snapped |
| Rain lookback / skip | Any `uint16_t` / float accepted | `rain_integration_set_config()` further checks hardware limits | Warnings logged on subsystem failure |
| Temperature sensitivity | Clamped to [`TEMP_COMP_MIN_SENSITIVITY` (0.01), `TEMP_COMP_MAX_SENSITIVITY` (0.20)] | See `temperature_compensation.h` | Clamp only |
| Base temperature | Clamped to [`TEMP_COMP_MIN_TEMP_C` (-10 deg C), `TEMP_COMP_MAX_TEMP_C` (50 deg C)] | See `temperature_compensation.h` | Clamp only |
| Offset / length | 0 <= offset <= 56 and no overflow | Inline checks | `BT_ATT_ERR_INVALID_OFFSET` |

### Module Interactions
- `watering_set_power_mode()` / `watering_get_power_mode()` - scheduler + background services.
- `set_flow_calibration()` / `get_flow_calibration()` - flow sensor pulses-to-litres scaling (persists itself).
- `master_valve_set_config()` / `master_valve_get_config()` - controls upstream valve sequencing.
- `sensor_manager_configure_bme280()` / `bme280_system_get_config()` - environmental sensor runtime configuration.
- `rain_integration_set_config()`, `rain_integration_save_config()`, `rain_integration_get_config()` - global rain compensation.
- `watering_get_channel()` loop - pushes temperature defaults into every channel struct.
- `watering_save_config_priority(true)` - persists master valve + watering state to NVS.
- `environmental_data_get_current()` - supplies data quality and sensor timestamp.
- `timezone_get_unix_utc()` - unifies timestamps.

### Client Implementation Reference

#### Transaction Checklist
1. **Enable notifications** by writing `0x0001` to the CCC descriptor (optional but recommended for commit confirmation).
2. **Read** the characteristic to capture the current baseline (sets the buffer and gives read-only values to reuse).
3. **Populate** a 56-byte buffer respecting field formats and little-endian encoding. Always zero the reserved block.
4. **Split** the payload into MTU-sized fragments (<=20 B on default ATT) and send consecutively. Web Bluetooth/bleak automatically maintains offsets when writes are sequential.
5. **Wait** for either the notification or perform a follow-up read to prove the update stuck before issuing further writes.

#### Encoding Helper (JavaScript / Web Bluetooth)
```javascript
function buildEnhancedSystemConfig(config) {
  const buf = new ArrayBuffer(56);
  const view = new DataView(buf);
  let o = 0;

  view.setUint8(o++, 2); // version (read-only but populate for clarity)
  view.setUint8(o++, config.powerMode ?? 0);
  view.setUint32(o, config.flowCalibration ?? 750, true);
  view.setUint8(o + 4, 1); // max_active_valves (read-only)
  view.setUint8(o + 5, 8); // num_channels (read-only)
  o += 6;

  const mv = config.masterValve ?? {};
  view.setUint8(o++, mv.enabled ? 1 : 0);
  view.setInt16(o, mv.preDelay ?? 0, true);
  view.setInt16(o + 2, mv.postDelay ?? 0, true);
  view.setUint8(o + 4, mv.overlapGrace ?? 10);
  view.setUint8(o + 5, mv.autoManagement ? 1 : 0);
  view.setUint8(o + 6, 0); // current_state placeholder
  o += 7;

  const bme = config.bme280 ?? {};
  view.setUint8(o++, bme.enabled ? 1 : 0);
  view.setUint16(o, bme.measurementInterval ?? 60, true);
  view.setUint8(o + 2, 0); // sensor_status placeholder
  o += 3;

  const comp = config.compensation ?? {};
  view.setUint8(o++, comp.rainEnabled ? 1 : 0);
  view.setUint8(o++, comp.tempEnabled ? 1 : 0);
  view.setFloat32(o, comp.rainSensitivity ?? 0.50, true);
  view.setFloat32(o + 4, comp.tempSensitivity ?? 0.05, true);
  view.setUint16(o + 8, comp.rainLookbackHours ?? 24, true);
  view.setFloat32(o + 10, comp.rainSkipThreshold ?? 5.0, true);
  view.setFloat32(o + 14, comp.tempBaseTemperature ?? 20.0, true);
  o += 18;

  // Zero status, timestamps, reserved
  while (o < 56) {
    view.setUint8(o++, 0);
  }

  return new Uint8Array(buf);
}

async function writeSystemConfig(characteristic, data) {
  const mtu = 20; // adjust if a larger MTU is negotiated
  for (let offset = 0; offset < data.length; offset += mtu) {
    const fragment = data.slice(offset, Math.min(offset + mtu, data.length));
    await characteristic.writeValueWithResponse(fragment);
  }
}
```

#### Decoding & Validation Helper (Python / Bleak)
```python
import struct

def decode_system_config(payload: bytes) -> dict:
  if len(payload) != 56:
    raise ValueError(f"expected 56 bytes, got {len(payload)}")

  view = memoryview(payload)
  cfg = {}
  (cfg['version'], cfg['power_mode']) = struct.unpack_from('<BB', view, 0)
  cfg['flow_calibration'] = struct.unpack_from('<I', view, 2)[0]
  cfg['max_active_valves'], cfg['num_channels'] = struct.unpack_from('<BB', view, 6)

  mv_offset = 8
  cfg['master_valve'] = {
    'enabled': view[mv_offset] == 1,
    'pre_delay': struct.unpack_from('<h', view, mv_offset + 1)[0],
    'post_delay': struct.unpack_from('<h', view, mv_offset + 3)[0],
    'overlap_grace': view[mv_offset + 5],
    'auto_management': view[mv_offset + 6] == 1,
    'current_state': view[mv_offset + 7] == 1,
  }

  bme_offset = 16
  cfg['bme280'] = {
    'enabled': view[bme_offset] == 1,
    'measurement_interval': struct.unpack_from('<H', view, bme_offset + 1)[0],  # 0 keeps prior cadence when re-applied
    'status': view[bme_offset + 3],
  }

  comp_offset = 20
  cfg['compensation'] = {
    'rain_enabled': view[comp_offset] == 1,
    'temp_enabled': view[comp_offset + 1] == 1,
    'rain_sensitivity': struct.unpack_from('<f', view, comp_offset + 2)[0],
    'temp_sensitivity': struct.unpack_from('<f', view, comp_offset + 6)[0],
    'rain_lookback_hours': struct.unpack_from('<H', view, comp_offset + 10)[0],
    'rain_skip_threshold': struct.unpack_from('<f', view, comp_offset + 12)[0],
    'temp_base_temperature': struct.unpack_from('<f', view, comp_offset + 16)[0],
  }

  status_offset = 40
  cfg['status'] = {
    'interval_mode_channels': view[status_offset],
    'compensation_active_channels': view[status_offset + 1],
    'incomplete_config_channels': view[status_offset + 2],
    'environmental_data_quality': view[status_offset + 3],
  }

  ts_offset = 44
  cfg['last_config_update'], cfg['last_sensor_reading'] = struct.unpack_from('<II', view, ts_offset)
  return cfg


def validate_payload(cfg: dict) -> None:
  if not 0 <= cfg['power_mode'] <= 2:
    raise ValueError('power_mode must be 0..2')
  if not 100 <= cfg['flow_calibration'] <= 10000:
    raise ValueError('flow_calibration must be 100..10000')
  sens = cfg['compensation']['temp_sensitivity']
  if not 0.01 <= sens <= 0.20:
    raise ValueError('temp_sensitivity must be 0.01..0.20')
  base = cfg['compensation']['temp_base_temperature']
  if not -10.0 <= base <= 50.0:
    raise ValueError('temp_base_temperature must be -10..50 deg C')
```

#### Recommended Client Behaviour
- Re-read immediately after a write to ensure round-trip integrity when notifications are disabled.
- Treat `UNLIKELY` as "busy": wait >=200 ms before retrying to give the scheduler time to adjust.
- When pushing global compensation values, follow up with `docs/ble-api/characteristics/25-compensation-status.md` to confirm the runtime calculators accepted the new baselines.
- If you need per-channel overrides after applying a global write, re-write the relevant Channel Configuration characteristic; the global defaults will have reset channel-level temp compensation blocks.

### Troubleshooting Matrix
| Symptom | Underlying Cause | Firmware Behaviour | Client Resolution |
|---------|-----------------|--------------------|-------------------|
| `BT_ATT_ERR_INVALID_OFFSET` | Fragment overshoots length (offset + len > 56) | Write rejected, buffer unchanged | Rebuild fragment sequence; always send contiguous slices starting at 0 |
| `BT_ATT_ERR_VALUE_NOT_ALLOWED` on first fragment | Payload passed validation but master valve or calibration check failed | Early exit with log | Audit ranges: power mode 0-2, flow 100-10000, OSR <=5, ensure master valve overlaps make sense |
| `BT_ATT_ERR_UNLIKELY` | `watering_set_power_mode()` returned `WATERING_ERROR_BUSY` | No subsystems updated | Retry after scheduler settles; typically within 1-2 seconds |
| Notification absent | CCC disabled or throttle delaying dispatch | No BLE packet issued | Enable notifications, allow for adaptive delay, or poll with read |
| Rain sensitivity not applied | Value outside 0-1 or subsystem rejected | Value is clamped, warning logged | Clamp client-side; confirm via readback; inspect logs for rain subsystem errors |
| Temp compensation seemingly unchanged | Channel overrides applied afterwards | Global write rewrites every channel temperature block | Reapply per-channel overrides as needed |
| BME280 settings unchanged | `sensor_manager_configure_bme280()` returned non-zero | Handler logs warning, continues with previous interval/enable state | Inspect sensor wiring/config; resend once underlying issue resolved |
| Environmental data quality zero | No valid BME280 sample | Last sensor read timestamp mirrors config update time | Inspect `docs/ble-api/characteristics/23-environmental-data.md`; ensure sensor enabled and connected |

### Cross-References
- `docs/ble-api/characteristics/02-flow-sensor.md` - details of flow calibration persistence.
- `docs/ble-api/characteristics/04-channel-configuration.md` - per-channel settings overwritten by global temperature defaults.
- `docs/ble-api/characteristics/23-environmental-data.md` - verifies BME280 runtime configuration.
- `docs/ble-api/characteristics/25-compensation-status.md` - confirms rain/temperature compensation activity post update.

### Revision Notes
- **Enhanced snapshot only:** The legacy 12-byte format has been fully retired; the struct size is enforced at compile time via `BUILD_ASSERT(sizeof(struct enhanced_system_config_data) == 56)`.
- **Log wording:** Firmware still logs "68B" when enabling notifications; this is a harmless legacy string. Payload size remains 56 bytes.
- **Reserved bytes:** Four tail bytes remain for forward compatibility. Always write zeros to avoid unpredictable behaviour in future firmware that may repurpose them.


