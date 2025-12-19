# BLE API Documentation

This guide reflects the current Bluetooth Low Energy interface implemented in `src/bt_irrigation_service.c`. Previous speculative content, legacy Web Bluetooth samples, and stale UUID summaries have been removed.

## Connection Facts

- **Advertised name:** `AutoWatering` (`CONFIG_BT_DEVICE_NAME`)
- **Primary service UUID:** `12345678-1234-5678-1234-56789abcdef0`
- **Simultaneous links:** 1 peripheral connection (`CONFIG_BT_MAX_CONN=1`)
- **Pairing:** Optional, central-initiated. The firmware does not enforce a minimum security level; all characteristics remain accessible after a successful connection even without bonding.
- **Bond slots:** 1 (`CONFIG_BT_MAX_PAIRED=1`) for faster reconnects when the central opts to pair.
- **ATT MTU:** Zephyr negotiates up to the central's request. Application logic still caps write fragments at 20 payload bytes for compatibility; larger structs are transferred via fragmentation.

## Characteristic Inventory (Verified)

All characteristics live under the irrigation primary service. Properties come directly from the `BT_GATT_CHARACTERISTIC` declarations; sizes match the packed structs in `bt_gatt_structs.h` and `bt_gatt_structs_enhanced.h`.

| # | Characteristic | UUID | Size | Properties | Notes |
|---|----------------|------|------|------------|-------|
| 1 | Valve Control | `12345678-1234-5678-1234-56789abcdef1` | 4 bytes | R / W / N | Manual watering commands and valve status snapshots. First byte selects channel. |
| 2 | Flow Sensor | `12345678-1234-5678-1234-56789abcdef2` | 4 bytes | R / N | Unsigned 32-bit pulse count per second with notifications when flow changes. |
| 3 | System Status | `12345678-1234-5678-1234-56789abcdef3` | 1 byte | R / N | Encoded health flags (idle, no flow, unexpected flow, RTC error, etc.). |
| 4 | Channel Configuration | `12345678-1234-5678-1234-56789abcdef4` | 76 bytes | R / W / N | Fragmented write (4-byte header). First byte selects channel when issuing short writes. |
| 5 | Schedule Configuration | `12345678-1234-5678-1234-56789abcdef5` | 9 bytes | R / W / N | Daily or periodic schedules. Initial one-byte write selects channel. |
| 6 | System Configuration | `12345678-1234-5678-1234-56789abcdef6` | 56 bytes | R / W / N | Enhanced system block with master valve, sensor cadence, and global compensation fields. |
| 7 | Task Queue | `12345678-1234-5678-1234-56789abcdef7` | 9 bytes | R / W / N | Queue depth, active task metadata, and control commands. |
| 8 | Statistics | `12345678-1234-5678-1234-56789abcdef8` | 15 bytes | R / W / N | Per-channel totals (ml, sessions, timestamps). |
| 9 | RTC Configuration | `12345678-1234-5678-1234-56789abcdef9` | 16 bytes | R / W / N | Wall-clock time settings including UTC offset and DST flag. |
| 10 | Alarm Status | `12345678-1234-5678-1234-56789abcdefa` | 7 bytes | R / W / N | Active alarm codes plus 32-bit timestamp. |
| 11 | Calibration Management | `12345678-1234-5678-1234-56789abcdefb` | 13 bytes | R / W / N | Flow calibration session control and progress reporting. |
| 12 | History Management | `12345678-1234-5678-1234-56789abcdefc` | command header 12 bytes | R / W / N | Commands use the 12-byte header in `struct history_data`; responses are sent with the 8-byte `history_fragment_header_t` plus payload fragments. |
| 13 | Diagnostics | `12345678-1234-5678-1234-56789abcdefd` | 12 bytes | R / N | Uptime, last error, battery level snapshot (read-only). |
| 14 | Growing Environment | `12345678-1234-5678-1234-56789abcdefe` | 71 bytes | R / W / N | Plant DB linkage, eco/quality mode, and volume limits. Fragment types 2 and 3 supported. |
| 15 | Auto Calc Status | `12345678-1234-5678-1234-56789abcde00` | 64 bytes | R / W / N | Result of FAO-56 calculations, including ET0, deficit, cycle planning, and MAD status. |
| 16 | Current Task Status | `12345678-1234-5678-1234-56789abcdeff` | 21 bytes | R / W / N | Active watering task snapshot (elapsed time, volume, state). Writes allow channel selection. |
| 17 | Timezone Configuration | `12345678-1234-5678-9abc-def123456793` | 16 bytes | R / W / N | Packed `timezone_config_t` (UTC offset plus DST rules). |
| 18 | Rain Sensor Configuration | `12345678-1234-5678-1234-56789abcde12` | 18 bytes | R / W / N | mm-per-pulse calibration, debounce, integration toggle. |
| 19 | Rain Sensor Data | `12345678-1234-5678-1234-56789abcde13` | 24 bytes | R / N | Live rainfall totals, hourly rate, and sensor quality flags. |
| 20 | Rain History Control | `12345678-1234-5678-1234-56789abcde14` | command header 16 bytes | R / W / N | Queries for aggregated rain history. Responses use the unified fragment header. |
| 21 | Environmental Data | `12345678-1234-5678-1234-56789abcde15` | 24 bytes | R / N | BME280 snapshot (temperature, humidity, pressure, data quality). |
| 22 | Environmental History | `12345678-1234-5678-1234-56789abcde16` | fragment header + payload | R / W / N | Buffered history with the same 8-byte fragment header used by irrigation history. |
| 23 | Compensation Status | `12345678-1234-5678-1234-56789abcde17` | 40 bytes | R / W / N | Rain and temperature compensation activity, factors, and timestamps per channel. |
| 24 | Rain Integration Status | `12345678-1234-5678-1234-56789abcde18` | 78 bytes | R / N | Aggregated rain integration metrics, per-channel reduction percentages, storage usage. |
| 25 | Onboarding Status | `12345678-1234-5678-1234-56789abcde20` | 29 bytes | R / N | Read-only progress flags for onboarding workflows. |
| 26 | Reset Control | `12345678-1234-5678-1234-56789abcde21` | 16 bytes | R / W / N | Two-step reset interface (code generate + confirmation). Notifies result and refreshed confirmation code. |
| 27 | Channel Compensation Config | `12345678-1234-5678-1234-56789abcde19` | 44 bytes | R / W / N | Per-channel rain/temp compensation settings. 1-byte write selects channel; 44-byte write updates config. |
| 28 | Bulk Sync Snapshot | `12345678-1234-5678-1234-56789abcde60` | 60 bytes | R | **NEW** Single-read aggregate of system state, environmental data, rain totals, compensation status, and channel states. Use at connection to replace multiple queries. |

Legend: R = Read, W = Write, N = Notify.

## Performance Optimizations (v2.1+)

The following optimizations have been implemented to maximize BLE throughput:

### PHY 2M and Data Length Extension
- PHY is upgraded to 2M (2 Mbps) at connection when supported
- Data Length Extension (DLE) is requested to 251 bytes
- Combined effect: ~2x faster raw data transfer

### Binary Search for Flash History
- History queries now use O(log n) binary search instead of O(n) linear scan
- Applies to: environmental hourly/daily, rain hourly/daily
- Typical speedup: 10-30x for queries on 720+ entry history

### Fragment Streaming Improvements
- Inter-fragment delay reduced from 5ms to 2ms
- Retry logic with exponential backoff (20ms → 640ms, 5 retries)
- New-query rate-limit reduced from 1000ms to 100ms (continuation/fragment requests bypass the limiter)
- Query continuation bypasses rate-limit entirely

### Buffer Configuration
```
CONFIG_BT_BUF_ACL_TX_COUNT=12
CONFIG_BT_BUF_EVT_RX_COUNT=16
CONFIG_BT_L2CAP_TX_BUF_COUNT=12
CONFIG_BT_ATT_TX_COUNT=12
CONFIG_BT_CONN_TX_MAX=24
CONFIG_BT_CTLR_RX_BUFFERS=18
CONFIG_BT_GATT_NOTIFY_MULTIPLE=y
```

### Bulk Sync Snapshot (UUID 0xde60)
A single 60-byte READ replaces 10+ individual characteristic queries at connection:
- System status, active channel, valve states
- Environmental data (temperature, humidity, pressure, dew point, VPD)
- Rain totals (today, week), skip status
- Compensation status and adjustments
- Task queue status and next scheduled task
- Per-channel status byte array (8 channels)

## New Limits & Behavioral Changes (v3.1.0)

These are the practical limits clients should follow for stable high-throughput operation:

- **Bulk Sync Snapshot**: single 60-byte READ (`0xde60`), no notifications. Use it at connection to avoid multiple round-trips.
- **History Management (watering history)**: new queries are accepted at most every **100ms**; fragment/continuation requests are **not** rate-limited.
- **Environmental History**: accepted command rate is **>= 50ms** between requests.
- **Rain History**: one in-flight command at a time; fragments stream via work queue with **~2ms spacing** between fragments.
- **Fragment payload sizing**: history fragments carry an 8-byte `history_fragment_header_t` plus payload; payload is MTU-aware and capped at **232 bytes** (`RAIN_HISTORY_FRAGMENT_SIZE`).
- **Retries**: history/rain/environment fragment streaming retries transient `-ENOMEM/-EBUSY` failures (up to **5 retries**) with exponential backoff.
- **Transfer cache**: repeated history/environment range reads may hit a **30s cache** to avoid re-reading flash between fragment requests.

## Fragmentation Rules

The firmware keeps 20-byte payload compatibility for writes even when a larger MTU is negotiated.

- **Channel Configuration / Growing Environment:** First fragment carries `[channel_id][frag_type][len_lo][len_hi]`. Fragment types 1 (name-only), 2 (big-endian struct), and 3 (little-endian struct) are supported; new tooling should prefer type 3.
- **Auto Calc Status:** Uses the generic `[0x00][frag_type][len_lo][len_hi]` header because it is not channel-scoped.
- **History responses (irrigation, rain, environmental):** Every fragment starts with the packed 8-byte `history_fragment_header_t` followed by up to 232 bytes of payload (`RAIN_HISTORY_FRAGMENT_SIZE`) and may be smaller to fit the negotiated MTU. Digest fields expose `fragment_index`, `total_fragments`, entry count, and status codes.
- **Notifications:** Managed through `SMART_NOTIFY`, `CRITICAL_NOTIFY`, and `CHANNEL_CONFIG_NOTIFY`. Adaptive throttling widens or shrinks the per-priority interval (critical 0 ms, high 50 ms, normal 200 ms, low 1000 ms) based on success and congestion counters. Channel name notifications are additionally rate-limited (maximum three within one second per channel).

## ATT Error Behaviour

| Error | Likely Cause | Client Action |
|-------|--------------|---------------|
| `BT_ATT_ERR_INVALID_ATTRIBUTE_LEN` | Fragment length mismatch, incorrect total size | Recalculate header and ensure first fragment declares the full struct length. |
| `BT_ATT_ERR_INVALID_OFFSET` | Long write with non-zero offset (not supported) | Always send fragments starting at offset 0. |
| `BT_ATT_ERR_VALUE_NOT_ALLOWED` | Channel out of range, unsupported reset type, or malformed enum | Validate inputs before writing. |
| `BT_ATT_ERR_INSUFFICIENT_RESOURCES` | Notification buffer exhaustion | Back off and retry after the firmware logs buffer recovery. |
| `BT_ATT_ERR_UNLIKELY` | Stack-side transient | Retrying after a short delay is usually sufficient. |

## Related Documents

- `docs/ble-api/fragmentation-guide.md` - Detailed walkthrough of fragmentation headers and examples.
- `docs/ble-api/notification-throttling.md` - Priority ladder, adaptive timers, and CCC state tracking.
- `docs/ble-api/protocol-specification.md` - Message sequencing reference (update pending final review cycle).
- Characteristic-specific files under `docs/ble-api/characteristics/` - Deep dives into each attribute's payload and state machine.
- `docs/TROUBLESHOOTING.md` - BLE troubleshooting steps now aligned with the current firmware.

## Tooling & Testing

- **nRF Connect (mobile or desktop):** Validated against the current service definition for manual read/write/notify testing.
- **Bleak (Python):** Recommended for scripted validation; maintains parity with fragmentation requirements.
- **Zephyr logging:** Enable `LOG_LEVEL_DBG` for `bt_irrigation_service` to observe fragment assembly, throttling decisions, and ATT errors during development.

## Status

Characteristic definitions, UUID mappings, and fragmentation notes in this document match the latest `main` branch (`bt_irrigation_service.c`, `bt_gatt_structs*.h`). Remaining action items live in `docs/ble-api/IMPLEMENTATION_STATUS.md` and will be updated as the characteristic deep-dive files are revalidated.
