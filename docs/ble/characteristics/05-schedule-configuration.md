# Schedule Configuration Characteristic

**UUID:** `12345678-1234-5678-1234-56789abcdef5`  
**Properties:** Read, Write, Notify  
**Size:** 9 bytes  
**Description:** Configure automatic watering schedules for individual channels with daily or periodic timing

## Overview

The Schedule Configuration characteristic manages automatic watering schedules for each channel. It supports two schedule types: **daily** (specific days of the week) and **periodic** (interval-based). Each schedule includes timing, watering parameters (duration or volume), and enable/disable control.

**⏰ TIME ZONE BEHAVIOR:**
- **Schedule Times:** All hour/minute values are in **LOCAL TIME** (user's configured timezone)
- **Execution:** System automatically converts local schedule time to internal UTC for precise execution
- **Display:** Schedule times shown to users are always in their local timezone
- **DST Handling:** Automatic adjustment for daylight saving time transitions

**Fragmentation:** ❌ NOT REQUIRED - 9 bytes fit in single BLE packet  
**Schedule Types:** ✅ Daily (day-of-week mask) and Periodic (interval days)  
**Watering Modes:** ✅ Duration-based (minutes) and Volume-based (liters)  
**Auto Control:** ✅ Enable/disable automatic execution  
**Channel Selection:** ✅ 1-byte write to select channel for subsequent reads  
**Rate Limiting:** ✅ 500ms minimum delay between notifications to prevent buffer overflow

## Data Structure

```c
struct schedule_config_data {
    uint8_t channel_id;        // Channel ID (0-7)
    uint8_t schedule_type;     // 0=daily, 1=periodic
    uint8_t days_mask;         // Days for daily or interval days for periodic
    uint8_t hour;              // Hour (0-23)
    uint8_t minute;            // Minute (0-59)
    uint8_t watering_mode;     // 0=duration, 1=volume
    uint16_t value;            // Minutes or liters (little-endian)
    uint8_t auto_enabled;      // 0=disabled, 1=enabled
} __packed;                    // TOTAL SIZE: 9 bytes
```

## Field Descriptions

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0 | 1 | `channel_id` | Channel identifier (0-7) |
| 1 | 1 | `schedule_type` | Schedule pattern (0=daily, 1=periodic) |
| 2 | 1 | `days_mask` | Day bitmask or interval days |
| 3 | 1 | `hour` | Execution hour (0-23) |
| 4 | 1 | `minute` | Execution minute (0-59) |
| 5 | 1 | `watering_mode` | Amount type (0=duration, 1=volume) |
| 6-7 | 2 | `value` | Minutes or liters (little-endian) |
| 8 | 1 | `auto_enabled` | Schedule enabled (0/1) |

### Basic Configuration

#### channel_id (byte 0)
- **Range:** 0-7 for valid channels
- **Read:** Returns currently selected channel schedule
- **Write:** Channel ID in schedule structure

#### schedule_type (byte 1)
- `0` = **Daily** (runs on specific days of the week)
- `1` = **Periodic** (runs every N days from start date)

#### days_mask (byte 2)
**For Daily Schedule (schedule_type = 0):** Bitmask for days of the week
- Bit 0 (0x01) - Sunday
- Bit 1 (0x02) - Monday
- Bit 2 (0x04) - Tuesday
- Bit 3 (0x08) - Wednesday
- Bit 4 (0x10) - Thursday
- Bit 5 (0x20) - Friday
- Bit 6 (0x40) - Saturday

**Examples:**
- `0x3E` - Monday to Friday (weekdays)
- `0x41` - Sunday and Saturday (weekends)
- `0x15` - Sunday, Tuesday, Thursday
- `0x7F` - Every day

**For Periodic Schedule (schedule_type = 1):** Interval in days
- **Range:** 1-255 days between watering cycles
- **Examples:** 1=daily, 2=every other day, 7=weekly, 30=monthly

#### hour (byte 3)
- **Range:** 0-23 (24-hour format)
- **Examples:** 6=6:00 AM, 18=6:00 PM, 0=midnight

#### minute (byte 4)
- **Range:** 0-59
- **Resolution:** 1-minute precision

### Watering Configuration

#### watering_mode (byte 5)
- `0` - **Duration-based** (value field contains minutes)
- `1` - **Volume-based** (value field contains liters)

#### value (bytes 6-7, little-endian)
**When watering_mode = 0 (Duration):**
- **Unit:** Minutes of watering time
- **Range:** 1-65535 minutes
- **Typical range:** 1-120 minutes

**When watering_mode = 1 (Volume):**
- **Unit:** Liters of water to deliver
- **Range:** 1-65535 liters
- **Precision:** 1-liter increments

#### auto_enabled (byte 8)
- `0` - **Disabled** (schedule defined but not executed)
- `1` - **Enabled** (schedule will execute automatically)
- **Note:** Also requires channel auto_enabled in Channel Configuration

## Channel Selection

**Important:** Before reading schedule configuration, you must select which channel (0-7) you want to read from.

### SELECT Channel for Read (1-byte write)
```javascript
// Select channel 5 for subsequent reads
const selectChannel = new ArrayBuffer(1);
new DataView(selectChannel).setUint8(0, 5); // Channel 5
await scheduleChar.writeValue(selectChannel);

// Now read the selected channel's schedule
const scheduleData = await scheduleChar.readValue();
```

**Behavior:**
- **Write length:** Exactly 1 byte
- **Purpose:** Sets which channel subsequent READ operations will return
- **No persistence:** Selection is temporary and for reading only
- **No notifications:** Selection changes do not trigger notifications
- **Validation:** Channel ID must be 0-7, otherwise returns `BT_ATT_ERR_VALUE_NOT_ALLOWED`

## Operations

### READ - Query Schedule Configuration
Select channel and read its schedule.

```javascript
// First, select the channel (1-byte write)
const selectChannel = new ArrayBuffer(1);
new DataView(selectChannel).setUint8(0, 1); // Select channel 1
await scheduleChar.writeValue(selectChannel);

// Then read the schedule
const scheduleData = await scheduleChar.readValue();
const view = new DataView(scheduleData.buffer);

// Parse schedule
const channelId = view.getUint8(0);
const scheduleType = view.getUint8(1);
const daysMask = view.getUint8(2);
const hour = view.getUint8(3);
const minute = view.getUint8(4);
const wateringMode = view.getUint8(5);
const value = view.getUint16(6, true); // little-endian
const autoEnabled = view.getUint8(8);

console.log(`Channel ${channelId} schedule: ${hour}:${minute.toString().padStart(2, '0')}`);
console.log(`Type: ${scheduleType ? 'Periodic' : 'Daily'}, Auto: ${autoEnabled ? 'ON' : 'OFF'}`);
```

### WRITE - Configure Schedule
Write complete schedule configuration.

```javascript
// Create weekday morning schedule for 15 minutes
const schedule = new ArrayBuffer(9);
const view = new DataView(schedule);

view.setUint8(0, 2);              // channel_id = 2
view.setUint8(1, 0);              // schedule_type = Daily
view.setUint8(2, 0x3E);           // days_mask = Monday-Friday (0011 1110)
view.setUint8(3, 7);              // hour = 7 AM
view.setUint8(4, 30);             // minute = 30
view.setUint8(5, 0);              // watering_mode = Duration
view.setUint16(6, 15, true);      // value = 15 minutes (little-endian)
view.setUint8(8, 1);              // auto_enabled = true

await scheduleChar.writeValue(schedule);
console.log('Weekday morning schedule configured');
```

### NOTIFY - Schedule Changes
Notifications when schedule configuration is modified.

```javascript
await scheduleChar.startNotifications();

scheduleChar.addEventListener('characteristicvaluechanged', (event) => {
    const data = event.target.value;
    const view = new DataView(data.buffer);
    
    const channelId = view.getUint8(0);
    const autoEnabled = view.getUint8(8);
    
    console.log(`Channel ${channelId} schedule ${autoEnabled ? 'enabled' : 'disabled'}`);
});
```

## Schedule Examples

### Daily Weekday Schedule
```javascript
// Water vegetables Monday-Friday at 7:30 AM for 20 minutes
const weekdaySchedule = {
    channel_id: 1,
    schedule_type: 0,        // Daily
    days_mask: 0x3E,         // Monday-Friday
    hour: 7,
    minute: 30,
    watering_mode: 0,        // Duration
    value: 20,               // 20 minutes
    auto_enabled: 1
};
```

### Periodic Volume Schedule
```javascript
// Water succulents every 3 days at 6:00 PM with 1 liter
const periodicSchedule = {
    channel_id: 4,
    schedule_type: 1,        // Periodic
    days_mask: 3,            // Every 3 days
    hour: 18,
    minute: 0,
    watering_mode: 1,        // Volume
    value: 1,                // 1 liter
    auto_enabled: 1
};
```

### Weekend Schedule
```javascript
// Deep watering on weekends at 6:00 AM for 45 minutes
const weekendSchedule = {
    channel_id: 0,
    schedule_type: 0,        // Daily
    days_mask: 0x41,         // Sunday and Saturday
    hour: 6,
    minute: 0,
    watering_mode: 0,        // Duration
    value: 45,               // 45 minutes
    auto_enabled: 1
};
```

## Helper Functions

### Day Mask Utilities
```javascript
// Create day mask from day names
function createDayMask(days) {
    const dayMap = {
        'sunday': 0x01, 'monday': 0x02, 'tuesday': 0x04, 'wednesday': 0x08,
        'thursday': 0x10, 'friday': 0x20, 'saturday': 0x40
    };
    
    return days.reduce((mask, day) => mask | dayMap[day.toLowerCase()], 0);
}

// Examples
const weekdays = createDayMask(['monday', 'tuesday', 'wednesday', 'thursday', 'friday']);
const weekends = createDayMask(['saturday', 'sunday']);
const everyDay = createDayMask(['sunday', 'monday', 'tuesday', 'wednesday', 'thursday', 'friday', 'saturday']);

// Parse day mask to readable format
function parseDayMask(mask) {
    const days = ['Sunday', 'Monday', 'Tuesday', 'Wednesday', 'Thursday', 'Friday', 'Saturday'];
    return days.filter((day, index) => mask & (1 << index));
}

console.log(parseDayMask(0x3E)); // ["Monday", "Tuesday", "Wednesday", "Thursday", "Friday"]
```

### Time Validation
```javascript
function validateScheduleTime(hour, minute) {
    if (hour < 0 || hour > 23) {
        throw new Error('Hour must be 0-23');
    }
    if (minute < 0 || minute > 59) {
        throw new Error('Minute must be 0-59');
    }
    return true;
}

function formatScheduleTime(hour, minute) {
    return `${hour}:${minute.toString().padStart(2, '0')}`;
}
```

## Related Characteristics

- **[Channel Configuration](04-channel-configuration.md)** - Must have auto_enabled for schedules to work
- **[RTC Configuration](09-rtc-configuration.md)** - Time synchronization for accurate scheduling
- **[Current Task Status](15-current-task-status.md)** - Monitor scheduled task execution
- **[Valve Control](01-valve-control.md)** - Manual override of scheduled watering
