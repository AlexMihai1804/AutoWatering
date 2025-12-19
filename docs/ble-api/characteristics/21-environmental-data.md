# Environmental Data Characteristic (UUID: 12345678-1234-5678-1234-56789abcde15)

> Operation Summary
| Operation | Payload | Size | Fragmentation | Notes |
|-----------|---------|------|---------------|-------|
| Read | `struct environmental_data_ble` | 24 B | None | Processed BME280 snapshot |
| Notify | `struct environmental_data_ble` | 24 B | 3-byte `[seq,total,len]` header (see below) | Uses compatibility path when payload > 23 bytes |
| Write | - | - | - | Interface is read/notify only |

## Characteristic Metadata
- **Properties:** Read, Notify (no write)
- **Payload Size:** 24 bytes, little-endian, IEEE-754 floats
- **Notification Priority:** `NOTIFY_PRIORITY_NORMAL`
- **Fragmentation (notify path):** A single frame is attempted only when `sizeof(payload) <= ATT_MTU-3` **and** `sizeof(payload) <= 23`. For compatibility, the current firmware fragments 24-byte environmental notifications using a 3-byte `[seq,total,len]` header.

## Payload Layout (`struct environmental_data_ble`)
| Offset | Field | Type | Meaning |
|--------|-------|------|---------|
| 0 | `temperature` | `float` | deg C from processed pipeline (fallback to raw driver if needed) |
| 4 | `humidity` | `float` | %RH |
| 8 | `pressure` | `float` | hPa |
| 12 | `timestamp` | `uint32_t` | Unix seconds (processed sample or uptime fallback) |
| 16 | `sensor_status` | `uint8_t` | 1 when any reading considered valid, 0 otherwise |
| 17 | `measurement_interval` | `uint16_t` | Seconds from persisted BME280 config (default 60) |
| 19 | `data_quality` | `uint8_t` | Validation score (0-100) clipped to sensor health |
| 20 | `reserved[4]` | `uint8_t[4]` | Always 0 |

Reads rebuild the struct on demand using `environmental_data_get_current()`. Notifications reuse the same helper and fall back to direct sensor reads when the processed cache is unavailable.

> **Implementation note:** The pooled notifier currently caps payloads at 23 bytes. Because the environmental payload is 24 bytes, the firmware fragments every notification even when the negotiated MTU could carry the whole struct. Clients should therefore always expect the 3-byte `[seq,total,len]` header until the pool limit is raised.
## Notification Behaviour
Requires CCC enable. If notifications were previously disabled, no snapshot is pushed automatically.

- Because the payload is 24 bytes and the compatibility single-frame path is capped at 23 bytes, current firmware uses the fragmentation header.
- Assemble by concatenating payload slices in `seq` order until `seq + 1 == total`.
- Fragmented sends use `bt_gatt_notify` and schedule the next fragment after ~2ms.
- Transient `-ENOMEM/-EBUSY` notify failures are retried (up to 5 retries) with exponential backoff (approx. 20ms → 320ms).

### Minimal Fragment Reassembly
```javascript
function handleEnvironmentalFragment(view) {
  if (view.byteLength === 24) {
    return view.buffer.slice(view.byteOffset, view.byteOffset + 24);
  }
  const seq = view.getUint8(0);
  const total = view.getUint8(1);
  const len = view.getUint8(2);
  return { seq, total, chunk: new Uint8Array(view.buffer, view.byteOffset + 3, len) };
}
```

## Data Quality Notes
- `sensor_status` is a coarse flag (0/1). Use `data_quality` for client-side warnings; firmware clamps it against `env_sensors_get_status()` overall health when available.
- Data quality derives from the validation pipeline; the BME280 does not expose gas metrics.

## Client Guidance
- Read operations always return 24 bytes; no long-read procedure is required.
- To keep pace with measurement intervals, subscribe to notifications and reassemble fragments when MTU constraints demand.<br>- Treat missing data (`sensor_status = 0`) as an indicator to fall back to cached values or suppress alerts.
- Developers can enable `CONFIG_ENV_SENSORS_SIM` to exercise this characteristic without hardware; the simulator drives deterministic temperature/humidity/pressure values that surface through the same payload.

## Related Characteristics
- `22-environmental-history.md` - aggregated and trend data using the same sensor feed.
- `23-compensation-status.md` - per-channel adjustments driven by environmental inputs.
- `15-auto-calc-status.md` - FAO-56 calculations that consume this telemetry.
