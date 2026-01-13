# RTC Configuration Characteristic (UUID: def9)

> Operation Summary
| Operation | Payload Struct | Size | Fragmentation | Notes |
|-----------|----------------|------|---------------|-------|
| Read | `struct rtc_data` | 16 B | None | Snapshot of current local time and timezone flags |
| Write | `struct rtc_data` | 16 B | None | Full-frame update; partial writes rejected |
| Notify | `struct rtc_data` | 16 B | None | Sent after CCC enable and after successful write |

The RTC Configuration characteristic exposes the system's notion of local time and timezone. Firmware handlers reside in `read_rtc`, `write_rtc`, `rtc_ccc_changed`, and `bt_irrigation_rtc_notify` inside `src/bt_irrigation_service.c`. Timezone helpers live in `src/rtc.c` and `src/timezone.c`.

## Characteristic Metadata
| Item | Value |
|------|-------|
| UUID | `12345678-1234-5678-1234-56789abcdef9` |
| Properties | Read, Write, Notify |
| Permissions | Read, Write |
| Payload Size | 16 bytes (`BUILD_ASSERT(sizeof(struct rtc_data) == 16)`) |
| Fragmentation | Not supported; frame must be delivered in a single write |
| Notification Priority | Normal (`safe_notify`) |

## Payload Layout
| Offset | Field | Type | Size | Access | Meaning | Source on Read | Validation on Write |
|--------|-------|------|------|--------|---------|----------------|--------------------|
| 0 | `year` | `uint8_t` | 1 | RW | Year minus 2000 (`00` = 2000) | Local calendar year | 0-99 accepted |
| 1 | `month` | `uint8_t` | 1 | RW | Month number | Local month | 1-12 (`BT_ATT_ERR_VALUE_NOT_ALLOWED` otherwise) |
| 2 | `day` | `uint8_t` | 1 | RW | Day of month | Local day | 1-31 plus month-specific clamp (Feb <=29, Apr/Jun/Sep/Nov <=30) |
| 3 | `hour` | `uint8_t` | 1 | RW | Hour in 24 h format | Local hour | 0-23 |
| 4 | `minute` | `uint8_t` | 1 | RW | Minute | Local minute | 0-59 |
| 5 | `second` | `uint8_t` | 1 | RW | Second | Local second | 0-59 |
| 6 | `day_of_week` | `uint8_t` | 1 | R | Day-of-week (0=Sunday) | Recomputed from supplied date | Incoming value ignored |
| 7 | `utc_offset_minutes` | `int16_t` | 2 | RW | Total UTC offset applied to local time | `timezone_get_total_offset()` | Applied directly; no range clamp here (timezone characteristic enforces -720...840) |
| 9 | `dst_active` | `uint8_t` | 1 | RW | 1 if DST logic enabled | `timezone_is_dst_active()` | Any non-zero becomes 1 |
| 10 | `reserved[6]` | `uint8_t[6]` | 6 | R | Always zero | Cleared before send | Must be zero on write; payload is copied but ignored |

Values are encoded little-endian. `day_of_week` is overwritten with firmware's own calculation; clients should not rely on the transmitted bit being echoed back.

## Read Path (`read_rtc`)
1. Grabs the current UTC datetime from `rtc_datetime_get()`.
2. Fetches timezone configuration via `timezone_get_config()` and converts the UTC timestamp to local time with `timezone_unix_to_rtc_local()`.
3. Populates all user-visible fields from the local datetime, converts the total UTC offset to minutes via `timezone_get_total_offset()`, and evaluates `dst_active` with `timezone_is_dst_active()`.
4. If any RTC or timezone call fails, the handler falls back to a hard-coded snapshot (2025-07-13 12:00:00, UTC+02, DST=1) so clients can detect the error and resynchronise.
5. Reserved bytes are zeroed before returning the packed struct with `bt_gatt_attr_read()`.

## Write Semantics (`write_rtc`)
- **Length/offset:** The handler expects one contiguous 16-byte frame at offset 0. Any other combination returns `BT_ATT_ERR_INVALID_OFFSET`.
- **Range checks:** Month/day/hour/minute/second bounds are enforced with `BT_ATT_ERR_VALUE_NOT_ALLOWED`. Additional month/day combinations block impossible dates such as 31 April.
- **Day-of-week:** Firmware recomputes `day_of_week` from the supplied date, ignoring the incoming byte.
- **Local->UTC conversion:** The supplied values are interpreted as *local* time. The `utc_offset_minutes` field represents the **total** UTC offset (already including DST if active). Firmware subtracts this offset directly to compute UTC, then stores via `rtc_datetime_set()`.
- **Timezone update:** The `utc_offset_minutes` is stored as the base timezone offset. **Important:** The `dst_active` field is informational only; firmware does NOT enable automatic DST calculation from RTC writes to prevent double-application of DST offset. For automatic DST transitions, clients must configure DST rules via the Timezone characteristic.
- **Error propagation:** Any conversion failure or RTC write issue produces `BT_ATT_ERR_UNLIKELY`.
- **Confirmation notify:** When notifications are enabled the handler calls `bt_irrigation_rtc_notify()` to broadcast the recomputed struct after a successful update.

### Example: Entering Daylight Saving Time
To advance the clock when DST starts (e.g., switch from UTC+1 to UTC+2 at 02:00 local time on 2025-03-30):

```
+0: year=25, month=3, day=30, hour=3, minute=0, second=0
+6: day_of_week ignored (write 0)
+7: utc_offset_minutes = 120   // +02:00
+9: dst_active = 1
+10: reserved[6] = {0}
```

The handler converts `03:00` local back to UTC using the new offset, persists the timestamp via `rtc_datetime_set()`, and stores the DST-enabled offset in the timezone configuration. Clients should always update the Timezone characteristic with matching rule data if automatic DST switching is required.

## Notification Delivery
- `rtc_ccc_changed()` toggles `notification_state.rtc_notifications_enabled`. When enabling it pushes an immediate snapshot using `safe_notify()`.
- Subsequent notifications are only sent on successful writes; there is no periodic ticking or timer-driven refresh.

## Interaction with Timezone Characteristic
- The Timezone characteristic (`docs/ble-api/characteristics/17-timezone-configuration.md`) exposes full DST rule control. RTC writes only toggle total offset and DST enable flag.
- Out-of-range offsets written via the RTC characteristic result in a logged warning when `timezone_set_config()` rejects the value. The ATT transaction itself still succeeds, so clients must read back the RTC or Timezone characteristic to verify acceptance.

## Error Handling
| Condition | ATT Error | Details |
|-----------|-----------|---------|
| Offset != 0 or length != 16 | `BT_ATT_ERR_INVALID_OFFSET` | Fragmented/partial writes unsupported |
| Month/day/hour/minute/second out of range | `BT_ATT_ERR_VALUE_NOT_ALLOWED` | Includes impossible month/day pairings |
| RTC or timezone conversion failure | `BT_ATT_ERR_UNLIKELY` | Typically triggered if timezone tables are unavailable |

All other failures (e.g., timezone set errors) are logged but do not translate to ATT errors.

## Client Guidance
- Always send the full 16-byte frame. Writes shorter than 16 bytes will be rejected.
- Provide the intended local wall-clock time and offset in one payload; the firmware derives UTC automatically.
- Expect the device to recompute `day_of_week`. Clients should not attempt to force alternative encodings.
- After a write, wait for the confirmation notification (or perform a read) before issuing further time-sensitive operations.
- Keep reserved bytes at zero to maintain forwards compatibility.

## Troubleshooting
| Symptom | Likely Cause | Mitigation |
|---------|--------------|------------|
| Read returns 2025-07-13 12:00, UTC+02 | RTC/Timezone subsystem unreachable, fallback path active | Reconfigure clock immediately; inspect logs for `RTC unavailable` warning |
| Write rejected with `BT_ATT_ERR_INVALID_OFFSET` | Payload length not 16 or write sent as Prepare/Execute | Send a single 16-byte Write Request/Command |
| Time correct but offset ignored | Offset outside permitted range; `timezone_set_config()` rejected update | Use Timezone characteristic to adjust, or clamp offset between -720 and +840 minutes |
| Notifications not received after write | CCC not enabled or connection inactive | Enable notifications (`StartNotifications()`), ensure connection remains established |

## Firmware References
- `src/bt_irrigation_service.c`: `read_rtc`, `write_rtc`, `rtc_ccc_changed`, `bt_irrigation_rtc_notify`.
- `src/rtc.c`: low-level RTC accessors (`rtc_datetime_get`, `rtc_datetime_set`).
- `src/timezone.c`: conversion helpers (`timezone_rtc_to_unix_utc`, `timezone_local_to_utc`, `timezone_unix_to_rtc_local`, `timezone_is_dst_active`, `timezone_set_config`).
- `src/bt_gatt_structs.h`: `struct rtc_data` definition and size assert.

## Related Characteristics
- `docs/ble-api/characteristics/05-schedule-configuration.md` - schedule execution depends on local time.

## Related Characteristics
- `docs/ble-api/characteristics/05-schedule-configuration.md` - schedule execution depends on local time.
- `docs/ble-api/characteristics/17-timezone-configuration.md` - detailed timezone/DST configuration.
- `docs/ble-api/characteristics/10-alarm-status.md` - RTC/Timezone faults can raise alarms.
        self.client = client
        self.char_uuid = "12345678-1234-5678-1234-56789abcdef9"

    async def get_current_time(self):
        """Read current RTC time and configuration"""
        try:
            data = await self.client.read_gatt_char(self.char_uuid)
            if len(data) >= 16:
                rtc_data = self.parse_rtc_data(data)
                print(f" Current RTC Time: {self.format_rtc_data(rtc_data)}")
                return rtc_data
            return None
        except Exception as e:
            print(f"[FAIL] Failed to read RTC time: {e}")
            return None

    async def set_current_time(self, date_time=None):
        """Set RTC time to specified datetime or current system time"""
        try:
            if date_time is None:
                date_time = datetime.now()
            
            # Get current timezone info if available
            current = await self.get_current_time()
            utc_offset = current['utc_offset_minutes'] if current else 0
            dst_active = current['dst_active'] if current else False

            rtc_data = self.create_rtc_data(date_time, utc_offset, dst_active)
            await self.write_rtc_data(rtc_data)
            
            print("[OK] RTC time updated successfully")
            return True
        except Exception as e:
            print(f"[FAIL] Failed to set RTC time: {e}")
            return False

    async def set_timezone(self, utc_offset_minutes, dst_active=False):
        """Set timezone configuration"""
        try:
            # Get current time
            current = await self.get_current_time()
            if not current:
                raise Exception("Failed to read current time")

            # Update timezone while keeping current time
            rtc_data = {**current, 'utc_offset_minutes': utc_offset_minutes, 'dst_active': dst_active}
            await self.write_rtc_data(rtc_data)
            
            offset_hours = utc_offset_minutes / 60
            print(f" Timezone updated: UTC{'+' if offset_hours >= 0 else ''}{offset_hours:.1f} {'(DST)' if dst_active else ''}")
            return True
        except Exception as e:
            print(f"[FAIL] Failed to set timezone: {e}")
            return False

    async def synchronize_with_system_time(self):
        """Synchronize RTC with system time"""
        try:
            now = datetime.now()
            
            # Get timezone offset in minutes
            if now.astimezone().utcoffset():
                timezone_offset = int(now.astimezone().utcoffset().total_seconds() / 60)
            else:
                timezone_offset = 0
            
            # Simple DST detection
            is_dst = self.is_daylight_saving_time(now)
            
            rtc_data = self.create_rtc_data(now, timezone_offset, is_dst)
            await self.write_rtc_data(rtc_data)
            
            print(" RTC synchronized with system time")
            print(f"   Local time: {now.strftime('%Y-%m-%d %H:%M:%S')}")
            print(f"   Timezone: UTC{'+' if timezone_offset >= 0 else ''}{timezone_offset/60:.1f} {'(DST)' if is_dst else ''}")
            
            return True
        except Exception as e:
            print(f"[FAIL] Failed to synchronize RTC: {e}")
            return False

    def create_rtc_data(self, date_time, utc_offset_minutes=0, dst_active=False):
        """Create RTC data structure from datetime"""
        return {
            'year': date_time.year - 2000,
            'month': date_time.month,
            'day': date_time.day,
            'hour': date_time.hour,
            'minute': date_time.minute,
            'second': date_time.second,
            'day_of_week': date_time.weekday() + 1 if date_time.weekday() != 6 else 0,  # Convert to Sunday=0
            'utc_offset_minutes': utc_offset_minutes,
            'dst_active': dst_active
        }

    async def write_rtc_data(self, rtc_data):
        """Write RTC data to characteristic"""
        # Pack RTC data into 16-byte structure
        data = struct.pack('<BBBBBBBhB6x',
            rtc_data['year'],
            rtc_data['month'],
            rtc_data['day'],
            rtc_data['hour'],
            rtc_data['minute'],
            rtc_data['second'],
            rtc_data['day_of_week'],
            rtc_data['utc_offset_minutes'],
            1 if rtc_data['dst_active'] else 0
        )
        
        await self.client.write_gatt_char(self.char_uuid, data)

    def parse_rtc_data(self, data):
        """Parse RTC data from bytes"""
        unpacked = struct.unpack('<BBBBBBBhB6x', data)
        return {
            'year': unpacked[0],
            'month': unpacked[1],
            'day': unpacked[2],
            'hour': unpacked[3],
            'minute': unpacked[4],
            'second': unpacked[5],
            'day_of_week': unpacked[6],
            'utc_offset_minutes': unpacked[7],
            'dst_active': unpacked[8] == 1
        }

    def format_rtc_data(self, rtc_data):
        """Format RTC data for display"""
        day_names = ['Sunday', 'Monday', 'Tuesday', 'Wednesday', 'Thursday', 'Friday', 'Saturday']
        
        try:
            date = datetime(2000 + rtc_data['year'], rtc_data['month'], rtc_data['day'],
                          rtc_data['hour'], rtc_data['minute'], rtc_data['second'])
        except ValueError:
            return "Invalid date/time"
        
        utc_offset_hours = rtc_data['utc_offset_minutes'] / 60
        timezone_str = f"UTC{'+' if utc_offset_hours >= 0 else ''}{utc_offset_hours:.1f}"
        
        return {
            'Date': date.strftime('%Y-%m-%d'),
            'Time': date.strftime('%H:%M:%S'),
            'Day of Week': day_names[rtc_data['day_of_week']],
            'Timezone': f"{timezone_str} {'(DST)' if rtc_data['dst_active'] else ''}",
            'Unix Timestamp': int(date.timestamp())
        }

    def is_daylight_saving_time(self, date_time):
        """Simple DST detection"""
        # This is a basic implementation - real DST rules are more complex
        try:
            jan_offset = datetime(date_time.year, 1, 1).astimezone().utcoffset()
            jul_offset = datetime(date_time.year, 7, 1).astimezone().utcoffset()
            current_offset = date_time.astimezone().utcoffset()
            
            return current_offset != min(jan_offset, jul_offset)
        except:
            return False

    def get_timezone_presets(self):
        """Get common timezone presets"""
        return {
            'UTC': {'utc_offset_minutes': 0, 'dst_active': False},
            'EST': {'utc_offset_minutes': -300, 'dst_active': False},
            'EDT': {'utc_offset_minutes': -240, 'dst_active': True},
            'CST': {'utc_offset_minutes': -360, 'dst_active': False},
            'CDT': {'utc_offset_minutes': -300, 'dst_active': True},
            'MST': {'utc_offset_minutes': -420, 'dst_active': False},
            'MDT': {'utc_offset_minutes': -360, 'dst_active': True},
            'PST': {'utc_offset_minutes': -480, 'dst_active': False},
            'PDT': {'utc_offset_minutes': -420, 'dst_active': True},
            'CET': {'utc_offset_minutes': 60, 'dst_active': False},
            'CEST': {'utc_offset_minutes': 120, 'dst_active': True},
            'JST': {'utc_offset_minutes': 540, 'dst_active': False},
            'AEST': {'utc_offset_minutes': 600, 'dst_active': False},
            'AEDT': {'utc_offset_minutes': 660, 'dst_active': True}
        }

    async def set_timezone_by_name(self, timezone_name):
        """Set timezone using preset name"""
        presets = self.get_timezone_presets()
        preset = presets.get(timezone_name.upper())
        
        if preset:
            return await self.set_timezone(preset['utc_offset_minutes'], preset['dst_active'])
        else:
            print(f"[FAIL] Unknown timezone: {timezone_name}")
            return False

    async def validate_rtc_accuracy(self):
        """Validate RTC accuracy against system time"""
        rtc_time = await self.get_current_time()
        if not rtc_time:
            return False

        try:
            rtc_datetime = datetime(2000 + rtc_time['year'], rtc_time['month'], rtc_time['day'],
                                  rtc_time['hour'], rtc_time['minute'], rtc_time['second'])
            system_time = datetime.now()
            
            time_diff = abs((rtc_datetime - system_time).total_seconds())
            
            print(" RTC Accuracy Check:")
            print(f"   RTC Time: {rtc_datetime.strftime('%Y-%m-%d %H:%M:%S')}")
            print(f"   System Time: {system_time.strftime('%Y-%m-%d %H:%M:%S')}")
            print(f"   Difference: {time_diff:.1f} seconds")
            
            if time_diff > 60:
                print("[WARN] RTC time differs by more than 1 minute - consider synchronization")
                return False
            elif time_diff > 5:
                print("[WARN] RTC time differs by more than 5 seconds")
                return True
            else:
                print("[OK] RTC time is accurate")
                return True
        except ValueError as e:
            print(f"[FAIL] Invalid RTC time data: {e}")
            return False

# Usage example
async def rtc_example():
    async with BleakClient("AutoWatering_Address") as client:
        rtc_manager = RTCManager(client)
        
        # Get current time
        current_time = await rtc_manager.get_current_time()
        
        # Synchronize with system time
        await rtc_manager.synchronize_with_system_time()
        
        # Set specific timezone
        await rtc_manager.set_timezone_by_name('EST')
        
        # Validate RTC accuracy
        await rtc_manager.validate_rtc_accuracy()
        
        # Set specific date/time
        specific_time = datetime(2024, 7, 15, 14, 30, 0)
        await rtc_manager.set_current_time(specific_time)
```

## [WARN] Important Notes

### Time Accuracy
- The firmware provides a single authoritative timestamp snapshot per read/notification; there is no streamed ticking. Client clocks should send periodic writes only when divergence > tolerance (avoid spamming writes <500 ms apart).
- Hardware / fallback: When RTC hardware read fails, firmware populates a hard-coded fallback (currently 2025-07-13 12:00:00 local example in code) and sets reasonable default timezone (UTC+2 DST=1) - clients must treat such values as INVALID and re-synchronize.

### Timezone & DST
- DST rules (start/end month/week/day, offset) are NOT carried in this characteristic-only current offset & DST enabled flag. Use the dedicated Timezone characteristic for full rule configuration.
- The `dst_active` bit toggles whether DST logic is enabled; disabling forces internal DST offset fields to zero.

### Scheduling Impact
- All scheduling logic consumes LOCAL time derived from RTC UTC + active timezone offsets; abrupt time changes will shift schedule execution.
- Perform time updates when the system is idle or directly after a schedule boundary to avoid mid-task inconsistencies.

### Reserved Bytes
- Always zero in current implementation (cleared on each read/notify). Any non-zero content should be ignored by clients.

## Related Characteristics

- **[Schedule Configuration](05-schedule-configuration.md)** - Automatic watering schedules
- **[Timezone Configuration](17-timezone-configuration.md)** - Advanced timezone and DST management
- **[System Status](03-system-status.md)** - RTC error status reporting
- **[Statistics](08-statistics.md)** - Timestamp accuracy for usage tracking

## Troubleshooting

### RTC Not Keeping Time
- Module / fallback path currently abstracts hardware specifics; documentation references to DS3231, CR2032 battery, and pin numbers were speculative and are removed. If persistent time loss occurs, perform additional hardware diagnostics per platform.

### Incorrect Time Display
- Re-read characteristic to confirm you have post-write snapshot.
- Confirm client computed day_of_week matches firmware (firmware recomputes; mismatch indicates client cached stale data).
- Validate offset & dst values using Timezone characteristic if repeated mismatch.

### Scheduling Issues
- **Time accuracy**: Validate RTC time against known accurate source
- **Timezone consistency**: Ensure all schedules use same timezone reference
- **DST transitions**: Manually update DST flag during transitions
- **System status**: Check for RTC error status in System Status characteristic

The RTC Configuration characteristic supplies a compact, single-frame snapshot of local time plus basic timezone state, with strict full-write semantics and notification only on subscription or successful updates.
