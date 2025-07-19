# Timezone Configuration Characteristic

**UUID:** `12345678-1234-5678-9abc-def123456793`  
**Properties:** Read, Write, Notify  
**Size:** 16 bytes  
**Description:** Timezone and Daylight Saving Time (DST) configuration for accurate local time conversion

## Overview

The Timezone Configuration characteristic manages timezone settings and DST rules for accurate local time conversion. It supports global timezone configurations with automatic DST transitions, enabling precise scheduling and time-based operations across different geographical locations.

**🌍 TIMEZONE FUNCTIONALITY:**
- **System Impact:** Configures how the system converts between UTC (internal storage) and local time (user display)
- **Scheduling:** Watering schedules are set and executed in **LOCAL TIME** based on this configuration
- **Time Display:** All time-related characteristics (RTC, History, etc.) show **LOCAL TIME** to users
- **Automatic DST:** When enabled, the system automatically adjusts for daylight saving time transitions

**Fragmentation:** ❌ NOT REQUIRED - 16 bytes fit in single BLE packet (MTU ≥ 20 bytes)  
**Real-time Updates:** ✅ Notifications on timezone configuration changes  
**DST Support:** ✅ Automatic DST transitions based on configurable rules  
**Global Coverage:** ✅ Supports all worldwide timezones (-12 to +14 hours)  
**Rate Limiting:** ✅ 500ms minimum delay between notifications to prevent buffer overflow

## Data Structure

```c
typedef struct {
    int16_t utc_offset_minutes;  // UTC offset in minutes (e.g., 120 for UTC+2)
    uint8_t dst_enabled;         // 1 if DST is enabled, 0 otherwise
    uint8_t dst_start_month;     // DST start month (1-12)
    uint8_t dst_start_week;      // DST start week of month (1-5, 5=last)
    uint8_t dst_start_dow;       // DST start day of week (0=Sunday, 1=Monday, etc.)
    uint8_t dst_end_month;       // DST end month (1-12)
    uint8_t dst_end_week;        // DST end week of month (1-5, 5=last)
    uint8_t dst_end_dow;         // DST end day of week (0=Sunday, 1=Monday, etc.)
    int16_t dst_offset_minutes;  // DST offset in minutes (usually 60)
    uint8_t reserved[5];         // Reserved for future use
} __packed timezone_config_t;   // TOTAL SIZE: 16 bytes
```

## Field Descriptions

### Bytes 0-1 - utc_offset_minutes (int16_t, little-endian)
- **Purpose:** Base timezone offset from UTC in minutes
- **Range:** -720 to +840 minutes (-12:00 to +14:00 hours)
- **Examples:** 
  - `120` = UTC+2:00 (Romania, Germany)
  - `-300` = UTC-5:00 (US Eastern)
  - `540` = UTC+9:00 (Japan)
- **Validation:** Firmware rejects values outside valid range

### Byte 2 - dst_enabled (uint8_t)
- **Purpose:** Enable/disable Daylight Saving Time
- **Values:** 0=DST disabled, 1=DST enabled
- **Effect:** When enabled, automatic DST transitions occur based on configured rules
- **Validation:** Firmware rejects values > 1

### Byte 3 - dst_start_month (uint8_t)
- **Purpose:** Month when DST begins (only used if dst_enabled=1)
- **Range:** 1-12 (January=1, December=12)
- **Examples:** 3=March (common in Europe), 2=February (some regions)
- **Validation:** Firmware rejects values outside 1-12

### Byte 4 - dst_start_week (uint8_t)
- **Purpose:** Week of the month when DST begins
- **Range:** 1-5 (1=first week, 5=last week of month)
- **Examples:** 5=last week (common), 2=second week
- **Note:** Week 5 means "last occurrence" even if it's actually week 4

### Byte 5 - dst_start_dow (uint8_t)
- **Purpose:** Day of week when DST begins
- **Range:** 0-6 (0=Sunday, 1=Monday, ..., 6=Saturday)
- **Examples:** 0=Sunday (common in EU/US), 1=Monday (some regions)

### Byte 6 - dst_end_month (uint8_t)
- **Purpose:** Month when DST ends
- **Range:** 1-12 (January=1, December=12)
- **Examples:** 10=October (Europe), 11=November (US)

### Byte 7 - dst_end_week (uint8_t)
- **Purpose:** Week of the month when DST ends
- **Range:** 1-5 (1=first week, 5=last week of month)
- **Examples:** 5=last week (common pattern)

### Byte 8 - dst_end_dow (uint8_t)
- **Purpose:** Day of week when DST ends
- **Range:** 0-6 (0=Sunday, 1=Monday, ..., 6=Saturday)
- **Examples:** 0=Sunday (standard in most regions)

### Bytes 9-10 - dst_offset_minutes (int16_t, little-endian)
- **Purpose:** DST offset added during DST period
- **Range:** Typically 60 minutes (+1 hour), some regions use 30 or 120
- **Examples:** 60=+1 hour (standard), 30=+30 minutes (rare)

### Bytes 11-15 - reserved (5 bytes)
- **Purpose:** Reserved for future use, should be set to 0
- **Value:** Always 0 for all 5 bytes

## Common Timezone Examples

### European (EU) Standard
```javascript
// Romania, Germany, most of Europe (UTC+2, DST enabled)
const europeTimezone = {
    utcOffsetMinutes: 120,    // UTC+2:00
    dstEnabled: 1,            // DST enabled
    dstStartMonth: 3,         // March
    dstStartWeek: 5,          // Last week
    dstStartDow: 0,           // Sunday
    dstEndMonth: 10,          // October
    dstEndWeek: 5,            // Last week
    dstEndDow: 0,             // Sunday
    dstOffsetMinutes: 60      // +1 hour
};
```

### United States Eastern
```javascript
// US Eastern Time (UTC-5, DST enabled)
const usEasternTimezone = {
    utcOffsetMinutes: -300,   // UTC-5:00
    dstEnabled: 1,            // DST enabled
    dstStartMonth: 3,         // March
    dstStartWeek: 2,          // Second week
    dstStartDow: 0,           // Sunday
    dstEndMonth: 11,          // November
    dstEndWeek: 1,            // First week
    dstEndDow: 0,             // Sunday
    dstOffsetMinutes: 60      // +1 hour
};
```

### Japan (No DST)
```javascript
// Japan Standard Time (UTC+9, no DST)
const japanTimezone = {
    utcOffsetMinutes: 540,    // UTC+9:00
    dstEnabled: 0,            // No DST
    dstStartMonth: 0,         // Unused
    dstStartWeek: 0,          // Unused
    dstStartDow: 0,           // Unused
    dstEndMonth: 0,           // Unused
    dstEndWeek: 0,            // Unused
    dstEndDow: 0,             // Unused
    dstOffsetMinutes: 0       // No DST
};
```

## Usage Examples

### Read Current Timezone Configuration
```javascript
// Get timezone characteristic
const timezoneChar = await service.getCharacteristic('12345678-1234-5678-9abc-def123456793');

// Read current timezone settings
const timezoneData = await timezoneChar.readValue();
const view = new DataView(timezoneData.buffer);

// Parse timezone configuration
const config = {
    utcOffsetMinutes: view.getInt16(0, true),     // little-endian
    dstEnabled: view.getUint8(2),
    dstStartMonth: view.getUint8(3),
    dstStartWeek: view.getUint8(4),
    dstStartDow: view.getUint8(5),
    dstEndMonth: view.getUint8(6),
    dstEndWeek: view.getUint8(7),
    dstEndDow: view.getUint8(8),
    dstOffsetMinutes: view.getInt16(9, true)      // little-endian
};

// Display timezone info
const hours = Math.floor(Math.abs(config.utcOffsetMinutes) / 60);
const minutes = Math.abs(config.utcOffsetMinutes) % 60;
const sign = config.utcOffsetMinutes >= 0 ? '+' : '-';

console.log(`Timezone: UTC${sign}${hours}:${minutes.toString().padStart(2, '0')}`);
console.log(`DST: ${config.dstEnabled ? 'Enabled' : 'Disabled'}`);

if (config.dstEnabled) {
    const dayNames = ['Sunday', 'Monday', 'Tuesday', 'Wednesday', 'Thursday', 'Friday', 'Saturday'];
    const monthNames = ['', 'Jan', 'Feb', 'Mar', 'Apr', 'May', 'Jun', 'Jul', 'Aug', 'Sep', 'Oct', 'Nov', 'Dec'];
    
    console.log(`DST Start: ${monthNames[config.dstStartMonth]} week ${config.dstStartWeek} ${dayNames[config.dstStartDow]}`);
    console.log(`DST End: ${monthNames[config.dstEndMonth]} week ${config.dstEndWeek} ${dayNames[config.dstEndDow]}`);
    console.log(`DST Offset: +${config.dstOffsetMinutes} minutes`);
}
```

### Set Timezone Configuration

**IMPORTANT:** The write operation requires **exactly 16 bytes**. Partial writes will be rejected with "GATT operation not permitted" or "invalid attribute length" errors.

```javascript
// Configure for Romania/Germany (UTC+2, EU DST rules)
async function setEuropeanTimezone() {
    const timezoneBuffer = new ArrayBuffer(16);  // MUST be exactly 16 bytes
    const view = new DataView(timezoneBuffer);
    
    // Set timezone configuration
    view.setInt16(0, 120, true);        // UTC+2:00 (120 minutes, little-endian)
    view.setUint8(2, 1);                // DST enabled
    view.setUint8(3, 3);                // DST starts March
    view.setUint8(4, 5);                // Last week
    view.setUint8(5, 0);                // Sunday
    view.setUint8(6, 10);               // DST ends October
    view.setUint8(7, 5);                // Last week
    view.setUint8(8, 0);                // Sunday
    view.setInt16(9, 60, true);         // +1 hour DST offset (little-endian)
    
    // CRITICAL: Initialize all reserved bytes (11-15) to 0
    for (let i = 11; i < 16; i++) {
        view.setUint8(i, 0);
    }
    
    try {
        await timezoneChar.writeValue(timezoneBuffer);
        console.log('✅ European timezone configured');
    } catch (error) {
        if (error.message.includes('not permitted') || error.message.includes('attribute length')) {
            console.error('❌ Timezone write failed - ensure exactly 16 bytes are sent');
        } else {
            console.error('❌ Timezone configuration failed:', error);
        }
    }
}

// Configure for US Eastern Time
async function setUSEasternTimezone() {
    const timezoneBuffer = new ArrayBuffer(16);  // MUST be exactly 16 bytes
    const view = new DataView(timezoneBuffer);
    
    view.setInt16(0, -300, true);       // UTC-5:00 (-300 minutes, little-endian)
    view.setUint8(2, 1);                // DST enabled
    view.setUint8(3, 3);                // DST starts March
    view.setUint8(4, 2);                // Second week
    view.setUint8(5, 0);                // Sunday
    view.setUint8(6, 11);               // DST ends November
    view.setUint8(7, 1);                // First week
    view.setUint8(8, 0);                // Sunday
    view.setInt16(9, 60, true);         // +1 hour DST offset (little-endian)
    
    // CRITICAL: Initialize all reserved bytes (11-15) to 0
    for (let i = 11; i < 16; i++) {
        view.setUint8(i, 0);
    }
    
    await timezoneChar.writeValue(timezoneBuffer);
    console.log('✅ US Eastern timezone configured');
}

// Configure for timezone without DST (e.g., Japan)
async function setNoDSTTimezone() {
    const timezoneBuffer = new ArrayBuffer(16);  // MUST be exactly 16 bytes
    const view = new DataView(timezoneBuffer);
    
    view.setInt16(0, 540, true);        // UTC+9:00 (540 minutes, little-endian)
    view.setUint8(2, 0);                // DST disabled
    view.setUint8(3, 0);                // Unused
    view.setUint8(4, 0);                // Unused
    view.setUint8(5, 0);                // Unused
    view.setUint8(6, 0);                // Unused
    view.setUint8(7, 0);                // Unused
    view.setUint8(8, 0);                // Unused
    view.setInt16(9, 0, true);          // No DST offset (little-endian)
    
    // CRITICAL: Initialize all reserved bytes (11-15) to 0
    for (let i = 11; i < 16; i++) {
        view.setUint8(i, 0);
    }
    
    await timezoneChar.writeValue(timezoneBuffer);
    console.log('✅ Japan timezone (no DST) configured');
}
    
    await timezoneChar.writeValue(timezoneBuffer);
    console.log('✅ Japan timezone (no DST) configured');
}
```

### Monitor Timezone Changes
```javascript
// Enable notifications for timezone updates
await timezoneChar.startNotifications();

// Handle timezone configuration changes
timezoneChar.addEventListener('characteristicvaluechanged', (event) => {
    const data = new DataView(event.target.value.buffer);
    
    const utcOffset = data.getInt16(0, true);
    const dstEnabled = data.getUint8(2);
    
    const hours = Math.floor(Math.abs(utcOffset) / 60);
    const minutes = Math.abs(utcOffset) % 60;
    const sign = utcOffset >= 0 ? '+' : '-';
    
    console.log(`🌍 Timezone updated: UTC${sign}${hours}:${minutes.toString().padStart(2, '0')}`);
    console.log(`📅 DST: ${dstEnabled ? 'Enabled' : 'Disabled'}`);
    
    // Update UI with new timezone settings
    updateTimezoneDisplay(data);
});
```

## Error Handling

### Validation Errors
- **Invalid UTC offset:** Returns `BT_ATT_ERR_VALUE_NOT_ALLOWED` for offsets outside -12:00 to +14:00
- **Invalid DST setting:** Returns `BT_ATT_ERR_VALUE_NOT_ALLOWED` for dst_enabled > 1
- **Invalid month/week/dow:** Silently corrected to valid ranges
- **Invalid structure size:** Returns `BT_ATT_ERR_INVALID_ATTRIBUTE_LEN`

### System Errors
- **NVS write failure:** Returns `BT_ATT_ERR_WRITE_NOT_PERMITTED`
- **Configuration corruption:** System uses default timezone settings
- **RTC failure:** Timezone calculations may be inaccurate

### Recovery Procedures
```javascript
// Handle timezone configuration errors
try {
    await timezoneChar.writeValue(configBuffer);
} catch (error) {
    if (error.message.includes('VALUE_NOT_ALLOWED')) {
        console.error('❌ Invalid timezone configuration');
        // Check and correct timezone values
    } else if (error.message.includes('WRITE_NOT_PERMITTED')) {
        console.error('❌ Failed to save timezone settings');
        // Retry after delay or check system status
    } else if (error.message.includes('not permitted') || error.message.includes('attribute length')) {
        console.error('❌ Timezone write failed - ensure exactly 16 bytes are sent');
        // Check buffer size and reserved bytes initialization
    }
}
```

## Troubleshooting

### "GATT operation not permitted" Error

**Symptom:** Timezone configuration writes fail with permission error.

**Root Cause:** The timezone characteristic requires exactly 16 bytes for write operations.

**Solutions:**
1. **Verify Buffer Size:**
   ```javascript
   // ❌ WRONG - Partial write will fail
   const incompleteBuffer = new ArrayBuffer(11);
   
   // ✅ CORRECT - Exactly 16 bytes required
   const completeBuffer = new ArrayBuffer(16);
   ```

2. **Initialize All Reserved Bytes:**
   ```javascript
   const timezoneBuffer = new ArrayBuffer(16);
   const view = new DataView(timezoneBuffer);
   
   // Set configuration fields (bytes 0-10)
   view.setInt16(0, utcOffset, true);
   // ... other fields ...
   
   // ✅ CRITICAL: Initialize reserved bytes 11-15
   for (let i = 11; i < 16; i++) {
       view.setUint8(i, 0);
   }
   ```

3. **Validate Configuration Values:**
   - UTC offset: -720 to +840 minutes
   - dst_enabled: 0 or 1 only
   - Months: 1-12, Weeks: 1-5, Days: 0-6

### Invalid Timezone Values Error

**Symptom:** Write accepted but timezone not applied correctly.

**Common Issues:**
- UTC offset outside valid range
- Invalid DST transition dates
- Inconsistent DST rules

**Example Fix:**
```javascript
// ❌ WRONG - Invalid UTC offset
view.setInt16(0, 900, true);  // UTC+15 is invalid

// ✅ CORRECT - Valid UTC offset
view.setInt16(0, 840, true);  // UTC+14 is the maximum
```
        // Check and correct timezone values
    } else if (error.message.includes('WRITE_NOT_PERMITTED')) {
        console.error('❌ Failed to save timezone settings');
        // Retry after delay or check system status
    }
}
```

## Integration with Other Characteristics

- **[RTC Configuration](09-rtc-configuration.md)** - Provides UTC time base for timezone calculations
- **[Schedule Configuration](05-schedule-configuration.md)** - Uses local time for schedule execution
- **[Statistics](08-statistics.md)** - Timestamps converted to local time for display
- **[History Management](12-history-management.md)** - Historical data uses local timestamps

## DST Transition Behavior

### Automatic DST Detection
- System automatically detects DST transitions based on configured rules
- No manual intervention required for DST changes
- Schedules automatically adjust to local time changes

### Spring Forward (DST Start)
- Clocks "spring forward" - e.g., 2:00 AM becomes 3:00 AM
- Scheduled tasks during the "lost hour" are skipped
- New tasks use DST-adjusted local time

### Fall Back (DST End)
- Clocks "fall back" - e.g., 2:00 AM becomes 1:00 AM
- Scheduled tasks may execute twice during the "repeated hour"
- Task deduplication prevents duplicate watering

### Example DST Transition
```javascript
// Monitor DST transitions
function onTimezoneUpdate(timezoneConfig) {
    const now = new Date();
    const isDSTActive = checkDSTActive(now, timezoneConfig);
    
    console.log(`Current time: ${now.toLocaleString()}`);
    console.log(`DST active: ${isDSTActive ? 'Yes' : 'No'}`);
    
    if (isDSTActive !== previousDSTState) {
        console.log(`🔄 DST transition detected: ${isDSTActive ? 'Started' : 'Ended'}`);
        // Update schedule notifications
        updateScheduleNotifications();
    }
    
    previousDSTState = isDSTActive;
}
```
