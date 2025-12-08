## Schedule Configuration Characteristic

This characteristic exposes and updates the twelve-byte `struct schedule_config_data` snapshot that determines when each irrigation channel executes its automatic run. The firmware paths are `read_schedule`, `write_schedule`, and `bt_irrigation_schedule_update` inside `src/bt_irrigation_service.c`, backed by the packed definition in `src/bt_gatt_structs.h`. The sections below describe every byte, validation rule, implicit clamp, notification trigger, and client-side best practice.

### Characteristic Metadata
| Item | Value |
|------|-------|
| UUID | `12345678-1234-5678-1234-56789abcdef5` |
| Properties | Read, Write, Notify |
| Permissions | Read, Write |
| Payload Size | 12 bytes (packed, little-endian) — extended from 9 bytes in v1 |
| Fragmentation | Not used (writes must be a single 12-byte frame) |
| Notification Priority | Normal priority through `safe_notify()` (adaptive 200 ms -> ~=2 s throttle) |
| Selector Shortcut | A standalone 1-byte write at offset 0 selects the channel for subsequent reads |

### Buffer Ownership
`schedule_value` (12 B) is the shared buffer. Reads copy from it (after fetching live channel data); full writes replace it and update the system. One-byte selector writes mutate only `schedule_value.channel_id` to point future reads at a different channel.

### Payload Layout
| Offset | Field | Type | Size | Access | Semantics | Read Source | Write Behaviour |
|--------|-------|------|------|--------|-----------|-------------|------------------|
| 0 | `channel_id` | `uint8_t` | 1 | R/W | Target channel (0-`WATERING_CHANNELS_COUNT-1`, currently 0-7) | Cached selector (validated, default 0) | Selector write or full write updates; validated during commit |
| 1 | `schedule_type` | `uint8_t` | 1 | R/W | 0=`SCHEDULE_DAILY`, 1=`SCHEDULE_PERIODIC`, 2=`SCHEDULE_AUTO` (FAO-56 smart) | Derived from `watering_event.schedule_type` | Must be 0, 1, or 2; other values rejected |
| 2 | `days_mask` | `uint8_t` | 1 | R/W | Daily mode: bit mask (bit0=Sunday ... bit6=Saturday); Periodic mode: interval in days; Auto mode: ignored (always checks daily) | Populated from `schedule.daily.days_of_week` or `schedule.periodic.interval_days`; Auto returns 0x7F | Must be >0 when `auto_enabled`=1 (unless `schedule_type`=2); in periodic mode 1-255 recommended |
| 3 | `hour` | `uint8_t` | 1 | R/W | Start hour (0-23, local time); used as fallback if solar timing enabled | `watering_event.start_time.hour` | Must be 0-23 |
| 4 | `minute` | `uint8_t` | 1 | R/W | Start minute (0-59); used as fallback if solar timing enabled | `watering_event.start_time.minute` | Must be 0-59 |
| 5 | `watering_mode` | `uint8_t` | 1 | R/W | 0=`WATERING_BY_DURATION`, 1=`WATERING_BY_VOLUME` | `watering_event.watering_mode` | Must be 0 or 1 |
| 6 | `value` (LSB) | `uint16_t` | 2 | R/W | Duration minutes (mode 0, stored as `uint8_t`) or volume liters (mode 1, `uint16_t`); Auto schedule (type 2): ignored, system calculates volume | Duration: `watering.by_duration.duration_minutes`; Volume: `watering.by_volume.volume_liters` | Duration clipped to 1-255; volume accepts 1-65535; Auto mode accepts 0 |
| 8 | `auto_enabled` | `uint8_t` | 1 | R/W | 0=disabled, 1=automatic schedule active | `watering_event.auto_enabled` | Must be 0 or 1; zero allows `days_mask`/`value` to be zero |
| 9 | `use_solar_timing` | `uint8_t` | 1 | R/W | 0=fixed time (use hour/minute), 1=relative to sunrise/sunset | `watering_event.use_solar_timing` | Must be 0 or 1 |
| 10 | `solar_event` | `uint8_t` | 1 | R/W | 0=`SOLAR_EVENT_SUNSET`, 1=`SOLAR_EVENT_SUNRISE` | `watering_event.solar_event` | Must be 0 or 1 |
| 11 | `solar_offset_minutes` | `int8_t` | 1 | R/W | Offset from solar event (-120 to +120 minutes) | `watering_event.solar_offset_minutes` | Clamped to -120..+120 range |

### Read Flow
1. `read_schedule()` inspects `schedule_value.channel_id`; if it falls outside 0-`WATERING_CHANNELS_COUNT-1`, the handler quietly resets it to 0.
2. The watering subsystem is queried (`watering_get_channel`). On success the struct is populated from the channel's `watering_event` fields. On failure a default template is used: Daily schedule, all days, 06:00, duration mode, 5 minutes, disabled. The channel id field is always set to the requested id (even when falling back to defaults).
3. The assembled struct is returned via `bt_gatt_attr_read()`.
4. Debug logs show the exact values; they are rate-unlimited for this path.

### Write Modes

#### 1. Channel Selector (1-byte write, offset 0, no prepare flag)
- Accepts only values `< WATERING_CHANNELS_COUNT`. Invalid ids trigger `BT_ATT_ERR_VALUE_NOT_ALLOWED`.
- Updates `schedule_value.channel_id` and returns immediately without touching persistent state, without emitting a notification, and without calling `watering_save_config_priority`.
- Intended for clients to set the target channel prior to a read.

#### 2. Full Structure Write (12 bytes, offset 0)
- `offset + len` must equal 12. Any other size results in `BT_ATT_ERR_INVALID_ATTRIBUTE_LEN`; any offset overflow yields `BT_ATT_ERR_INVALID_OFFSET`.
- Data is memcpy'd into `schedule_value`. No validation happens until the full length is present.
- Validation sequence once all 12 bytes are available:
  1. `channel_id` range check.
  2. Channel lookup via `watering_get_channel()` (`BT_ATT_ERR_UNLIKELY` on failure).
  3. `schedule_type` in {0,1,2}; `watering_mode` in {0,1}; `hour <= 23`; `minute <= 59`.
  4. If `auto_enabled == 1` and `schedule_type != 2`, both `value` and `days_mask` must be non-zero.
  5. If `schedule_type == 2` (AUTO), `value` and `days_mask` may be zero (system calculates volume automatically based on FAO-56 deficit).
  6. AUTO mode requires prior configuration of `plant_db_index`, `soil_db_index`, and `planting_date_unix` via Channel Configuration (04).
  7. `solar_offset_minutes` is clamped to -120..+120 range if out of bounds.
  8. Duration mode implicitly truncates `value` to the low byte when assigned to `duration_minutes` (`uint8_t`); clients should limit to 1-255 to avoid wrap.
- Apply order mirrors the struct:
  1. Start time updated (`start_time.hour/minute`).
  2. `auto_enabled` flag set.
  3. Schedule type: sets `schedule_type` enum and writes `days_of_week` or `interval_days` accordingly.
  4. Solar timing: sets `use_solar_timing`, `solar_event`, and `solar_offset_minutes`.
  5. Watering mode: assigns either `watering.by_duration.duration_minutes` (low byte) or `watering.by_volume.volume_liters`.
  6. `watering_save_config_priority(true)` commits to NVS.
  6. `invalidate_channel_cache()` ensures future reads pull fresh data.
  7. If notifications are enabled, `bt_irrigation_schedule_update(channel_id)` rebuilds `schedule_value` from the live channel and sends it through `safe_notify()`.
- Write returns `len` on success as per BLE GATT specification.

### Notification Behaviour
- Trigger: every successful full-structure write that completes validation and apply. Selector-only writes never notify.
- Payload: full 12-byte struct rebuilt from the channel (not simply the cached write) to guarantee read-after-write fidelity.
- Priority: Normal priority queue in the adaptive throttler (first notify after >=200 ms, stretching as required to protect link health).
- Disable: CCC write of 0 clears `schedule_value` to zeros and leaves `channel_id` at 0.

### Validation & Firmware Errors
| Check | Condition | Firmware Action | ATT error returned | Notes |
|-------|-----------|-----------------|--------------------|-------|
| Channel selector | `channel_id >= WATERING_CHANNELS_COUNT` | Log error | `BT_ATT_ERR_VALUE_NOT_ALLOWED` | Applies to selector and full write |
| Handle / buffer validity | Null `conn`/`attr`/`buf` | Log error | `BT_ATT_ERR_INVALID_HANDLE` | Defensive guard |
| Length | `len != 12` for full write | Log error | `BT_ATT_ERR_INVALID_ATTRIBUTE_LEN` | Prepared writes unsupported |
| Offset | `offset + len > 12` | Log error | `BT_ATT_ERR_INVALID_OFFSET` | |
| Time fields | `hour > 23` or `minute > 59` | Log error | `BT_ATT_ERR_VALUE_NOT_ALLOWED` | |
| Enums | `schedule_type > 2` or `watering_mode > 1` | Log error | `BT_ATT_ERR_VALUE_NOT_ALLOWED` | |
| Auto-enabled guard | `auto_enabled == 1 && value == 0` and not AUTO/FAO-56 | Log error | `BT_ATT_ERR_VALUE_NOT_ALLOWED` | Value may be zero for AUTO schedule or FAO-56 enabled channels |
| Auto-enabled guard | `auto_enabled == 1 && days_mask == 0` and not AUTO | Log error | `BT_ATT_ERR_VALUE_NOT_ALLOWED` | Interval/day mask must be set (AUTO mode ignores this) |
| Channel lookup | `watering_get_channel()` fails | Log error | `BT_ATT_ERR_UNLIKELY` | Typically indicates channel table not initialised |
| AUTO prerequisites | `schedule_type == 2` but plant/soil/date not configured | Log warning | Accepted but will not trigger | Configure plant_db_index, soil_db_index, planting_date_unix first |
| Solar offset | `solar_offset_minutes` outside -120..+120 | Clamp to bounds | Accepted | No error, value is silently clamped |

### Client Implementation Guidance
- Use the 1-byte selector to choose a channel once, then read repeatedly without re-sending the id.
- Always bound duration values to `1-255` (minutes). Values >255 silently wrap because the backend stores a `uint8_t`.
- For periodic schedules, treat `days_mask` as "interval days" and keep it within 1-255; zero is only valid while disabled.
- Re-read or rely on the notification to confirm that the system accepted the write. Notifications contain canonical values reflecting any clamping (e.g., duration truncation).
- If multiple channels need to be updated back-to-back, rate-limit writes to respect the 200 ms notify throttle or check for completion notifications before sending the next update.

#### Encoding Helper (JavaScript)
```javascript
export function buildSchedulePayload(cfg) {
    const buf = new ArrayBuffer(12);
    const view = new DataView(buf);
    view.setUint8(0, cfg.channelId);
    view.setUint8(1, cfg.scheduleType);
    view.setUint8(2, cfg.daysMask);
    view.setUint8(3, cfg.hour);
    view.setUint8(4, cfg.minute);
    view.setUint8(5, cfg.wateringMode);
    view.setUint16(6, cfg.value, true);
    view.setUint8(8, cfg.autoEnabled ? 1 : 0);
    view.setUint8(9, cfg.useSolarTiming ? 1 : 0);
    view.setUint8(10, cfg.solarEvent || 0);
    view.setInt8(11, Math.max(-120, Math.min(120, cfg.solarOffsetMinutes || 0)));
    return new Uint8Array(buf);
}

export async function selectScheduleChannel(characteristic, channelId) {
    if (channelId < 0 || channelId >= 8) {
        throw new Error('channelId must be 0..7');
    }
    await characteristic.writeValue(new Uint8Array([channelId]));
}

export async function writeScheduleConfig(characteristic, payload) {
    if (payload.length !== 12) {
        throw new Error('schedule payload must be exactly 12 bytes');
    }
    await characteristic.writeValue(payload);
}
```

#### Decoding Helper (Python)
```python
import struct

def parse_schedule(data: bytes) -> dict:
    if len(data) != 12:
        raise ValueError(f"expected 12 bytes, got {len(data)}")
    channel_id, schedule_type, days_mask, hour, minute, watering_mode, value, auto_enabled, \
        use_solar_timing, solar_event, solar_offset_minutes = \
        struct.unpack('<BBBBBBHBBBb', data)
    if watering_mode == 0:
        duration_minutes = value & 0xFF
        volume_liters = None
    else:
        duration_minutes = None
        volume_liters = value
    return {
        'channel_id': channel_id,
        'schedule_type': schedule_type,
        'days_mask': days_mask,
        'hour': hour,
        'minute': minute,
        'watering_mode': watering_mode,
        'value': value,
        'duration_minutes': duration_minutes,
        'volume_liters': volume_liters,
        'auto_enabled': auto_enabled,
        'use_solar_timing': use_solar_timing,
        'solar_event': solar_event,
        'solar_offset_minutes': solar_offset_minutes,
    }
```

### Operational Notes
- Schedules execute relative to the device's local clock. Keep both `RTC Configuration (09)` and `Timezone Configuration (17)` in sync or watering will drift.
- Only one automatic schedule exists per channel. Writing a new schedule replaces the entire entry without keeping history.
- Automatic runs feed tasks into the queue provided at least two slots remain free; when `k_msgq_num_used_get(&watering_tasks_queue) >= 2` the scheduler logs the skip and the run is dropped.
- Missed runs are not re-enqueued automatically—wait for the next scheduled slot or queue a manual task if delivery is still required.
- Setting `auto_enabled = 0` preserves the configuration but stops automatic launches. In this state `days_mask` and `value` may be zero.
- Manual or critical watering requests can occupy the queue and pre-empt a scheduled task if headroom is exhausted.

### AUTO (Smart Schedule) Mode
AUTO mode (`schedule_type = 2`) implements FAO-56 based smart irrigation that only waters when the soil needs it:
- **Prerequisites**: Before enabling AUTO mode, configure `plant_db_index`, `soil_db_index`, and `planting_date_unix` via Channel Configuration (04).
- **Daily Evaluation**: At the configured time (e.g., 06:00), the system evaluates soil water deficit using crop evapotranspiration (ETc) calculations.
- **Irrigation Decision**: Watering is triggered only when `current_deficit_mm >= RAW_threshold_mm` (Readily Available Water depleted).
- **Volume Calculation**: The system automatically calculates the volume needed to refill the soil to field capacity.
- **Rainfall Integration**: Effective precipitation from the rain sensor is automatically subtracted from the deficit.
- **Environmental Stress**: On hot/dry days (temperature > plant's optimal max or humidity < 30%), the system reduces the MAD threshold for earlier watering.
- **Offline Gap Handling**: If the system was powered off for multiple days, it estimates and accumulates missed ETc on power-up.
- **Days Mask**: Ignored for AUTO mode; the system checks every day at the configured time.

### Solar Timing Mode
Solar timing allows scheduling relative to sunrise or sunset instead of a fixed clock time. This is useful for:
- **Evening watering**: Water after sunset to minimize evaporation loss.
- **Morning watering**: Water at sunrise for optimal plant uptake before heat of day.
- **Seasonal adaptation**: Times automatically adjust as day length changes throughout the year.

#### Configuration Fields
| Field | Value | Description |
|-------|-------|-------------|
| `use_solar_timing` | 0 | Disabled - use fixed `hour:minute` time |
| `use_solar_timing` | 1 | Enabled - calculate time from sunrise/sunset |
| `solar_event` | 0 (`SOLAR_EVENT_SUNSET`) | Base time on sunset |
| `solar_event` | 1 (`SOLAR_EVENT_SUNRISE`) | Base time on sunrise |
| `solar_offset_minutes` | -120 to +120 | Offset from solar event (negative = before, positive = after) |

#### Algorithm
The firmware uses the **NOAA Solar Calculator** algorithm, which provides sunrise/sunset times with approximately 1-minute precision. The calculation requires:
- **Latitude**: From channel's `latitude_deg` field (configured via Channel Configuration).
- **Longitude**: From channel's `longitude_deg` field (configured via Channel Configuration).
- **Day of Year**: Automatically determined from current date.
- **Timezone**: From system timezone configuration.

#### Polar Region Handling
At extreme latitudes (above ~66°), polar day (midnight sun) or polar night conditions may occur:
- **Polar Day**: Sun never sets. Sunset is set to fallback time (20:00).
- **Polar Night**: Sun never rises. Sunrise is set to fallback time (06:00).
- The configured `hour:minute` is used as the ultimate fallback if solar calculation fails.

#### Examples
| Configuration | Result (example for 45°N latitude, summer) |
|---------------|---------------------------------------------|
| `use_solar_timing=1, solar_event=0, solar_offset_minutes=0` | Water at exact sunset (~20:45) |
| `use_solar_timing=1, solar_event=0, solar_offset_minutes=30` | Water 30 min after sunset (~21:15) |
| `use_solar_timing=1, solar_event=0, solar_offset_minutes=-60` | Water 1 hour before sunset (~19:45) |
| `use_solar_timing=1, solar_event=1, solar_offset_minutes=0` | Water at exact sunrise (~05:30) |
| `use_solar_timing=1, solar_event=1, solar_offset_minutes=15` | Water 15 min after sunrise (~05:45) |

### Troubleshooting Matrix
| Symptom | Likely Cause | Firmware Response | Recommended Action |
|---------|--------------|-------------------|--------------------|
| `VALUE_NOT_ALLOWED` on 1-byte write | Channel id out of range | Reject before caching | Clamp to 0-7 |
| `INVALID_ATTRIBUTE_LEN` | Payload not exactly 9 bytes | Write aborted | Always send 9-byte frames |
| `UNLIKELY` error | Channel lookup failed (e.g., system not initialised) | Logs error | Retry after channels are ready |
| Schedule never triggers | `auto_enabled`=0, zero interval, or RTC mismatch | No automatic task generated | Enable auto, ensure interval >0, verify RTC/timezone |
| Duration runs short/long | Value >255 clipped or inaccurate RTC | Duration stored in `uint8_t` | Respect 1-255 range, confirm RTC accuracy |
| Missing notification | Notifications disabled or throttle delaying send | No packet | Enable CCC, wait up to ~=2 s, or confirm via read |

### Related Characteristics
- `docs/ble-api/characteristics/04-channel-configuration.md` - `auto_enabled` flag and plant/soil metadata for each channel.
- `docs/ble-api/characteristics/07-task-queue-management.md` - reflects pending/active tasks generated by schedules.
- `docs/ble-api/characteristics/09-rtc-configuration.md` & `17-timezone-configuration.md` - maintain accurate timing baseline.
