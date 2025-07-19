# RTC Configuration Characteristic

**UUID:** `12345678-1234-5678-1234-56789abcdef9`  
**Properties:** Read, Write, Notify  
**Size:** 16 bytes  
**Description:** Real-time clock configuration with timezone support and DST management

## Overview

The RTC Configuration characteristic manages the system's real-time clock with comprehensive timezone support. It handles automatic local time conversion, daylight saving time (DST) detection, and UTC offset management for accurate scheduling.

**🕐 TIME ZONE BEHAVIOR:**
- **READ Operations:** Returns current date/time in **LOCAL TIME** (user's configured timezone)
- **WRITE Operations:** Accepts date/time in **LOCAL TIME** (automatically converts to UTC for storage)
- **Notifications:** Send date/time in **LOCAL TIME** for user display
- **Internal Storage:** System maintains UTC time internally, applies timezone conversion for user interface

**Timezone Support:** ✅ Automatic UTC offset calculation with DST handling  
**Local Time Display:** ✅ Converts UTC internal time to local display time  
**Scheduling Integration:** ✅ Provides accurate timing for watering schedules  
**Fragmentation:** ❌ NOT REQUIRED - 16 bytes fit in single BLE packet  
**Rate Limiting:** ✅ 500ms minimum delay between notifications to prevent buffer overflow

## Data Structure

```c
struct rtc_data {
    uint8_t year;              // Year minus 2000 (0-99)
    uint8_t month;             // Month (1-12)
    uint8_t day;               // Day (1-31)
    uint8_t hour;              // Hour (0-23)
    uint8_t minute;            // Minute (0-59)
    uint8_t second;            // Second (0-59)
    uint8_t day_of_week;       // Day of week (0-6, 0=Sunday)
    int16_t utc_offset_minutes; // UTC offset in minutes
    uint8_t dst_active;        // 1 if DST active, 0 otherwise
    uint8_t reserved[6];       // Reserved for future use
} __packed;                    // TOTAL SIZE: 16 bytes
```

## Field Descriptions

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0 | 1 | `year` | Year minus 2000 (0-99) |
| 1 | 1 | `month` | Month (1-12) |
| 2 | 1 | `day` | Day (1-31) |
| 3 | 1 | `hour` | Hour (0-23) |
| 4 | 1 | `minute` | Minute (0-59) |
| 5 | 1 | `second` | Second (0-59) |
| 6 | 1 | `day_of_week` | Day of week (0-6, 0=Sunday) |
| 7-8 | 2 | `utc_offset_minutes` | UTC offset in minutes (little-endian) |
| 9 | 1 | `dst_active` | DST status (0/1) |
| 10-15 | 6 | `reserved` | Future expansion |

### Date/Time Fields

#### year (byte 0)
- **Range:** 0-99 (represents 2000-2099)
- **Format:** Year minus 2000 (e.g., 25 = 2025)

#### month (byte 1)
- **Range:** 1-12 (1=January, 12=December)

#### day (byte 2)
- **Range:** 1-31 (validated against month)
- **Validation:** Considers leap years and month lengths

#### hour (byte 3)
- **Range:** 0-23 (24-hour format)

#### minute (byte 4)
- **Range:** 0-59

#### second (byte 5)
- **Range:** 0-59

#### day_of_week (byte 6)
- **Range:** 0-6 (0=Sunday, 1=Monday, ..., 6=Saturday)
- **Calculation:** Automatically calculated from date

### Timezone Fields

#### utc_offset_minutes (bytes 7-8, little-endian)
- **Range:** -720 to +840 minutes (-12:00 to +14:00 hours)
- **Format:** Signed 16-bit integer
- **Examples:**
  - `0` = UTC (GMT+0)
  - `60` = GMT+1 (Central European Time)
  - `120` = GMT+2 (Central European Summer Time)
  - `-300` = GMT-5 (Eastern Standard Time)
  - `-240` = GMT-4 (Eastern Daylight Time)

#### dst_active (byte 9)
- `0` = DST not active (standard time)
- `1` = DST active (daylight time)

#### reserved (bytes 10-15)
- **Value:** Always 0 (unused)
- **Purpose:** Future expansion

## Operations

### READ - Query Current Time
Returns current system date/time with timezone information.

```javascript
const data = await rtcChar.readValue();
const view = new DataView(data.buffer);

// Parse date/time
const year = 2000 + view.getUint8(0);
const month = view.getUint8(1);
const day = view.getUint8(2);
const hour = view.getUint8(3);
const minute = view.getUint8(4);
const second = view.getUint8(5);
const dayOfWeek = view.getUint8(6);
const utcOffsetMinutes = view.getInt16(7, true); // little-endian
const dstActive = view.getUint8(9);

// Create JavaScript Date object
const currentTime = new Date(year, month - 1, day, hour, minute, second);

// Display timezone info
const offsetHours = Math.floor(Math.abs(utcOffsetMinutes) / 60);
const offsetMins = Math.abs(utcOffsetMinutes) % 60;
const offsetSign = utcOffsetMinutes >= 0 ? '+' : '-';
const timezoneName = dstActive ? '(DST)' : '(Standard)';

console.log(`Current Time: ${currentTime.toLocaleString()}`);
console.log(`Timezone: GMT${offsetSign}${offsetHours}:${offsetMins.toString().padStart(2, '0')} ${timezoneName}`);
```

### WRITE - Set System Time
Set the system date/time and timezone configuration.

**IMPORTANT:** The write operation requires **exactly 16 bytes**. Partial writes will be rejected with "GATT operation not permitted" error.

```javascript
// Set time to current local time with timezone
const now = new Date();
const timeConfig = new ArrayBuffer(16);  // MUST be exactly 16 bytes
const view = new DataView(timeConfig);

view.setUint8(0, now.getFullYear() - 2000);    // year
view.setUint8(1, now.getMonth() + 1);          // month (0-based to 1-based)
view.setUint8(2, now.getDate());               // day
view.setUint8(3, now.getHours());              // hour
view.setUint8(4, now.getMinutes());            // minute
view.setUint8(5, now.getSeconds());            // second
view.setUint8(6, now.getDay());                // day_of_week

// Set timezone (example: GMT+2 with DST)
view.setInt16(7, 120, true);                   // utc_offset_minutes = +2 hours (little-endian)
view.setUint8(9, 1);                           // dst_active = true

// Reserved bytes (10-15) MUST be set to 0
for (let i = 10; i < 16; i++) {
    view.setUint8(i, 0);
}

try {
    await rtcChar.writeValue(timeConfig);
    console.log('System time synchronized');
} catch (error) {
    if (error.message.includes('not permitted')) {
        console.error('RTC write failed - ensure exactly 16 bytes are sent');
    } else {
        console.error('RTC write failed:', error);
    }
}
```

### NOTIFY - Time Change Notifications
Notifications when time is updated or DST transitions occur.

```javascript
await rtcChar.startNotifications();

rtcChar.addEventListener('characteristicvaluechanged', (event) => {
    const data = event.target.value;
    const view = new DataView(data.buffer);
    
    const year = 2000 + view.getUint8(0);
    const month = view.getUint8(1);
    const day = view.getUint8(2);
    const hour = view.getUint8(3);
    const minute = view.getUint8(4);
    const dstActive = view.getUint8(9);
    
    const timeStr = `${year}-${month.toString().padStart(2, '0')}-${day.toString().padStart(2, '0')} ${hour}:${minute.toString().padStart(2, '0')}`;
    console.log(`Time updated: ${timeStr} ${dstActive ? '(DST)' : '(Standard)'}`);
});
```

## Timezone Management

### Common Timezone Configurations
```javascript
const timezones = {
    // UTC and Western Europe
    'UTC': { offset: 0, dst: false },
    'GMT': { offset: 0, dst: false },
    'BST': { offset: 60, dst: true },  // British Summer Time
    'CET': { offset: 60, dst: false }, // Central European Time
    'CEST': { offset: 120, dst: true }, // Central European Summer Time
    
    // North America
    'EST': { offset: -300, dst: false }, // Eastern Standard Time
    'EDT': { offset: -240, dst: true },  // Eastern Daylight Time
    'CST': { offset: -360, dst: false }, // Central Standard Time
    'CDT': { offset: -300, dst: true },  // Central Daylight Time
    'PST': { offset: -480, dst: false }, // Pacific Standard Time
    'PDT': { offset: -420, dst: true },  // Pacific Daylight Time
    
    // Asia
    'JST': { offset: 540, dst: false },  // Japan Standard Time
    'CST_China': { offset: 480, dst: false }, // China Standard Time
    'IST': { offset: 330, dst: false },  // India Standard Time
};

async function setTimezone(timezoneName) {
    const tz = timezones[timezoneName];
    if (!tz) {
        console.error('Unknown timezone:', timezoneName);
        return;
    }
    
    const data = await rtcChar.readValue();
    const view = new DataView(data.buffer);
    
    view.setInt16(7, tz.offset, true);  // Set UTC offset
    view.setUint8(9, tz.dst ? 1 : 0);  // Set DST status
    
    await rtcChar.writeValue(data);
    console.log(`Timezone set to ${timezoneName}`);
}
```

### Automatic Time Synchronization
```javascript
// Sync with browser's local time
async function syncWithBrowserTime() {
    const now = new Date();
    const timeConfig = new ArrayBuffer(16);
    const view = new DataView(timeConfig);
    
    // Set date/time
    view.setUint8(0, now.getFullYear() - 2000);
    view.setUint8(1, now.getMonth() + 1);
    view.setUint8(2, now.getDate());
    view.setUint8(3, now.getHours());
    view.setUint8(4, now.getMinutes());
    view.setUint8(5, now.getSeconds());
    view.setUint8(6, now.getDay());
    
    // Calculate timezone offset
    const offsetMinutes = -now.getTimezoneOffset();
    view.setInt16(7, offsetMinutes, true);
    
    // Detect DST (simplified - check if offset differs from standard)
    const january = new Date(now.getFullYear(), 0, 1);
    const july = new Date(now.getFullYear(), 6, 1);
    const standardOffset = Math.max(-january.getTimezoneOffset(), -july.getTimezoneOffset());
    const isDST = -now.getTimezoneOffset() > standardOffset;
    view.setUint8(9, isDST ? 1 : 0);
    
    await rtcChar.writeValue(timeConfig);
    console.log('Time synchronized with browser');
}
```

### Time Display Utilities
```javascript
// Format time for display
function formatSystemTime(rtcData) {
    const view = new DataView(rtcData.buffer);
    
    const year = 2000 + view.getUint8(0);
    const month = view.getUint8(1);
    const day = view.getUint8(2);
    const hour = view.getUint8(3);
    const minute = view.getUint8(4);
    const second = view.getUint8(5);
    const utcOffset = view.getInt16(7, true);
    const dstActive = view.getUint8(9);
    
    const timeStr = `${year}-${month.toString().padStart(2, '0')}-${day.toString().padStart(2, '0')} ` +
                   `${hour.toString().padStart(2, '0')}:${minute.toString().padStart(2, '0')}:${second.toString().padStart(2, '0')}`;
    
    const offsetHours = Math.floor(Math.abs(utcOffset) / 60);
    const offsetMins = Math.abs(utcOffset) % 60;
    const offsetSign = utcOffset >= 0 ? '+' : '-';
    const timezoneStr = `GMT${offsetSign}${offsetHours}:${offsetMins.toString().padStart(2, '0')}`;
    
    return {
        datetime: timeStr,
        timezone: timezoneStr,
        dst: dstActive ? '(DST)' : '(Standard)'
    };
}
```

## Troubleshooting

### "GATT operation not permitted" Error

**Symptom:** Write operations fail with permission error.

**Root Cause:** The RTC characteristic requires exactly 16 bytes for write operations.

**Solutions:**
1. **Verify Buffer Size:**
   ```javascript
   // ❌ WRONG - Partial write will fail
   const shortBuffer = new ArrayBuffer(8);
   
   // ✅ CORRECT - Exactly 16 bytes required
   const fullBuffer = new ArrayBuffer(16);
   ```

2. **Initialize All Reserved Bytes:**
   ```javascript
   const timeConfig = new ArrayBuffer(16);
   const view = new DataView(timeConfig);
   
   // Set your time data (bytes 0-9)
   view.setUint8(0, year - 2000);
   // ... other fields ...
   
   // ✅ CRITICAL: Initialize reserved bytes 10-15
   for (let i = 10; i < 16; i++) {
       view.setUint8(i, 0);
   }
   ```

3. **Check Date Validation:**
   - Month: 1-12 (not 0-11 like JavaScript Date)
   - Day: 1-31 (validated against month)
   - Hour: 0-23, Minute: 0-59, Second: 0-59
   - UTC offset: -720 to +840 minutes

### Invalid Date Values Error

**Symptom:** Write succeeds but time is not updated.

**Common Issues:**
- Using 0-based months (JavaScript) instead of 1-based
- Invalid date combinations (e.g., February 30)
- UTC offset outside valid range

**Example Fix:**
```javascript
// ❌ WRONG - JavaScript uses 0-based months
view.setUint8(1, now.getMonth());

// ✅ CORRECT - RTC uses 1-based months
view.setUint8(1, now.getMonth() + 1);
```

## Related Characteristics

- **[Schedule Configuration](05-schedule-configuration.md)** - Uses RTC for schedule timing
- **[Statistics](08-statistics.md)** - Timestamps use RTC configuration
- **[System Status](03-system-status.md)** - RTC errors affect system status
- **[Current Task Status](15-current-task-status.md)** - Task timing uses RTC
