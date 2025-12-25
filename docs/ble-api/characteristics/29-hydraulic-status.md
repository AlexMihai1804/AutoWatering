# Hydraulic Status Characteristic (UUID: 12345678-1234-5678-1234-56789abcde22)

> Operation Summary
| Operation | Payload | Size | Fragmentation | Notes |
|-----------|---------|------|---------------|-------|
| Write | Channel selector | 1 B | None | Values 0-7 or 0xFF (active channel if watering) |
| Read | `struct hydraulic_status_data` | 48 B | None | Snapshot for selected channel |
| Notify | `struct hydraulic_status_data` | 48 B | None | Normal-priority notifications |

## Channel Selection
- Write a single byte to choose the channel whose hydraulic state will be exposed.
- `0xFF` selects the currently active watering channel if one exists, otherwise channel 0.
- The selected channel id is cached in the characteristic value so subsequent reads and notifications reuse it.

## Payload Layout (`struct hydraulic_status_data`)
| Offset | Field | Type | Meaning |
|--------|-------|------|---------|
| 0 | `channel_id` | `uint8_t` | Channel reflected in this snapshot |
| 1 | `profile_type` | `uint8_t` | `hydraulic_profile_t` (0=auto, 1=spray, 2=drip) |
| 2 | `lock_level` | `uint8_t` | `hydraulic_lock_level_t` (0=none, 1=soft, 2=hard) |
| 3 | `lock_reason` | `uint8_t` | `hydraulic_lock_reason_t` (0=none, 1=high flow, 2=no flow, 3=unexpected, 4=mainline leak) |
| 4 | `nominal_flow_ml_min` | `uint32_t` | Learned nominal flow (ml/min) |
| 8 | `ramp_up_time_sec` | `uint16_t` | Learned ramp-up time (seconds) |
| 10 | `tolerance_high_percent` | `uint8_t` | High flow tolerance percentage |
| 11 | `tolerance_low_percent` | `uint8_t` | Low flow tolerance percentage |
| 12 | `is_calibrated` | `uint8_t` | 1 if stable runs met |
| 13 | `monitoring_enabled` | `uint8_t` | 1 if monitoring enabled |
| 14 | `learning_runs` | `uint8_t` | Total learning runs |
| 15 | `stable_runs` | `uint8_t` | Stable learning runs |
| 16 | `estimated` | `uint8_t` | 1 if nominal flow is estimated |
| 17 | `manual_override_active` | `uint8_t` | 1 if manual override active |
| 18 | `reserved0` | `uint16_t` | Reserved (0) |
| 20 | `lock_at_epoch` | `uint32_t` | Channel lock timestamp (UTC epoch) |
| 24 | `retry_after_epoch` | `uint32_t` | Channel retry timestamp (UTC epoch) |
| 28 | `no_flow_runs` | `uint8_t` | Persistent NO_FLOW count |
| 29 | `high_flow_runs` | `uint8_t` | Persistent HIGH_FLOW count |
| 30 | `unexpected_flow_runs` | `uint8_t` | Persistent UNEXPECTED_FLOW count |
| 31 | `reserved1` | `uint8_t` | Reserved (0) |
| 32 | `last_anomaly_epoch` | `uint32_t` | Last anomaly timestamp (UTC epoch) |
| 36 | `global_lock_level` | `uint8_t` | Global lock level |
| 37 | `global_lock_reason` | `uint8_t` | Global lock reason |
| 38 | `reserved2` | `uint16_t` | Reserved (0) |
| 40 | `global_lock_at_epoch` | `uint32_t` | Global lock timestamp (UTC epoch) |
| 44 | `global_retry_after_epoch` | `uint32_t` | Global retry timestamp (UTC epoch) |

All multi-byte values are little-endian. Total size is exactly 48 bytes.

## Behaviour Details
- Reads rebuild a snapshot from the current watering state using the cached channel id (default 0).
- Writes that change the selected channel update the cached id and, if notifications are enabled, emit an immediate snapshot.
- Notifications are sent when hydraulic learning updates, lock states change, manual overrides are toggled, or low-flow warnings are raised.
- Global lock fields are included in every snapshot and are identical regardless of channel selection.

## Practical Guidance
- If you only need to know whether the system is locked, read `global_lock_level` and `global_lock_reason`.
- `profile_type` is the stored profile (auto, spray, drip); if it remains `auto`, the client may infer from irrigation method.
- Notifications may fail on default 23-byte MTU; use reads or negotiate a larger MTU if you rely on notify.

## Related Characteristics
- **[Alarm Status](10-alarm-status.md)** - high-level alarm codes for anomaly events
- **[System Status](03-system-status.md)** - overall lock state (LOCKED)
- **[Flow Sensor](02-flow-sensor.md)** - live flow rate during watering
