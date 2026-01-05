# Bulk Sync Snapshot Characteristic

## Overview

The Bulk Sync Snapshot characteristic provides a single 60-byte READ that aggregates critical system state, replacing 10+ individual characteristic queries at connection time. This dramatically improves initial sync latency.

## Identification

| Property | Value |
|----------|-------|
| UUID | `12345678-1234-5678-1234-56789abcde60` |
| Size | 60 bytes (packed) |
| Properties | READ only |
| ATTR Index | 83 |

## Data Structure

```c
struct __attribute__((packed)) bulk_sync_snapshot_t {
    /* Header (4 bytes) */
    uint8_t version;              /* Format version (currently 1) */
    uint8_t flags;                /* bit0=rtc_valid, bit1=env_valid, bit2=rain_valid */
    uint16_t reserved;            /* Reserved for future use */
    
    /* Time (8 bytes) */
    uint32_t utc_timestamp;       /* Current UTC timestamp */
    int16_t timezone_offset_min;  /* Total timezone offset in minutes */
    uint8_t dst_active;           /* 1 if DST is currently active */
    uint8_t padding1;             /* Alignment padding */
    
    /* System Status (8 bytes) */
    uint8_t system_mode;          /* 0=idle, 1=watering, 2=paused, 3=error_recovery */
    uint8_t active_alarms;        /* Bitmask of active alarm categories */
    uint8_t valve_states;         /* Bitmask of open valves (bit N = channel N) */
    uint8_t active_channel;       /* Currently active channel (0xFF if none) */
    uint16_t remaining_seconds;   /* Remaining time on active task */
    uint16_t flow_rate_ml_min;    /* Current flow rate in mL/min */
    
    /* Environmental Data (12 bytes) */
    int16_t temperature_c_x10;    /* Temperature in 0.1°C units */
    uint16_t humidity_pct_x10;    /* Humidity in 0.1% units */
    uint16_t pressure_hpa_x10;    /* Pressure in 0.1 hPa units */
    int16_t dew_point_c_x10;      /* Dew point in 0.1°C units */
    uint16_t vpd_kpa_x100;        /* VPD in 0.01 kPa units */
    uint16_t padding2;            /* Alignment padding */
    
    /* Rain Integration (8 bytes) */
    uint16_t rain_today_mm_x10;   /* Today's rainfall in 0.1mm units */
    uint16_t rain_week_mm_x10;    /* Weekly rainfall in 0.1mm units */
    uint8_t rain_integration_enabled; /* 1 if rain integration active */
    uint8_t skip_active;          /* 1 if skip is currently active */
    uint16_t skip_remaining_min;  /* Minutes remaining in skip period */
    
    /* Compensation (4 bytes) */
    uint8_t temp_comp_enabled;    /* 1 if temperature compensation enabled */
    uint8_t rain_comp_enabled;    /* 1 if rain compensation enabled */
    int8_t temp_adjustment_pct;   /* Current temp adjustment (-100 to +100) */
    int8_t rain_adjustment_pct;   /* Current rain adjustment (-100 to 0) */
    
    /* Task Queue (8 bytes) */
    uint8_t pending_task_count;   /* Number of pending tasks in queue */
    uint8_t next_task_channel;    /* Channel of next scheduled task */
    uint16_t next_task_in_min;    /* Minutes until next task (0 = immediate, 0xFFFF = unknown/none) */
    uint32_t next_task_timestamp; /* UTC timestamp of next task (0 = unknown/none) */
    
    /* Channel Status (8 bytes) */
    uint8_t channel_status[8];    /* Per-channel status byte:
                                     bit0: enabled
                                     bit1: schedule_active
                                     bit2: currently_watering
                                     bit3-7: reserved */
};
```

Total: 60 bytes

## Flags Field

| Bit | Name | Description |
|-----|------|-------------|
| 0 | rtc_valid | RTC is synchronized and timestamp is valid |
| 1 | env_valid | Environmental sensor data is valid |
| 2 | rain_valid | Rain sensor data is valid |
| 3-7 | reserved | Reserved for future use |

## System Mode Values

| Value | Name | Description |
|-------|------|-------------|
| 0 | IDLE | No watering active |
| 1 | WATERING | Watering in progress |
| 2 | PAUSED | Watering paused (manual or rain delay) |
| 3 | ERROR_RECOVERY | System recovering from error |

## Channel Status Byte

Each of the 8 bytes in `channel_status[]` represents one channel:

| Bit | Name | Description |
|-----|------|-------------|
| 0 | enabled | Channel is enabled for operation |
| 1 | schedule_active | Channel has an active schedule |
| 2 | currently_watering | Channel is currently watering |
| 3-7 | reserved | Reserved for future use |

## Usage Example (Flutter/Dart)

```dart
class BulkSyncSnapshot {
  final int version;
  final int flags;
  final int utcTimestamp;
  final int timezoneOffsetMin;
  final bool dstActive;
  final int systemMode;
  final int activeAlarms;
  final int valveStates;
  final int activeChannel;
  final int remainingSeconds;
  final int flowRateMlMin;
  final double temperatureC;
  final double humidityPct;
  final double pressureHpa;
  final double dewPointC;
  final double vpdKpa;
  final double rainTodayMm;
  final double rainWeekMm;
  final bool rainIntegrationEnabled;
  final bool skipActive;
  final int skipRemainingMin;
  final bool tempCompEnabled;
  final bool rainCompEnabled;
  final int tempAdjustmentPct;
  final int rainAdjustmentPct;
  final int pendingTaskCount;
  final int nextTaskChannel;
  final int nextTaskInMin;
  final int nextTaskTimestamp;
  final List<int> channelStatus;
  
  factory BulkSyncSnapshot.fromBytes(Uint8List data) {
    if (data.length < 60) throw FormatException('Invalid snapshot size');
    
    final bd = ByteData.sublistView(data);
    return BulkSyncSnapshot(
      version: data[0],
      flags: data[1],
      utcTimestamp: bd.getUint32(4, Endian.little),
      timezoneOffsetMin: bd.getInt16(8, Endian.little),
      dstActive: data[10] != 0,
      systemMode: data[12],
      activeAlarms: data[13],
      valveStates: data[14],
      activeChannel: data[15],
      remainingSeconds: bd.getUint16(16, Endian.little),
      flowRateMlMin: bd.getUint16(18, Endian.little),
      temperatureC: bd.getInt16(20, Endian.little) / 10.0,
      humidityPct: bd.getUint16(22, Endian.little) / 10.0,
      pressureHpa: bd.getUint16(24, Endian.little) / 10.0,
      dewPointC: bd.getInt16(26, Endian.little) / 10.0,
      vpdKpa: bd.getUint16(28, Endian.little) / 100.0,
      rainTodayMm: bd.getUint16(32, Endian.little) / 10.0,
      rainWeekMm: bd.getUint16(34, Endian.little) / 10.0,
      rainIntegrationEnabled: data[36] != 0,
      skipActive: data[37] != 0,
      skipRemainingMin: bd.getUint16(38, Endian.little),
      tempCompEnabled: data[40] != 0,
      rainCompEnabled: data[41] != 0,
      tempAdjustmentPct: bd.getInt8(42),
      rainAdjustmentPct: bd.getInt8(43),
      pendingTaskCount: data[44],
      nextTaskChannel: data[45],
      nextTaskInMin: bd.getUint16(46, Endian.little),
      nextTaskTimestamp: bd.getUint32(48, Endian.little),
      channelStatus: data.sublist(52, 60).toList(),
    );
  }
  
  bool get rtcValid => (flags & 0x01) != 0;
  bool get envValid => (flags & 0x02) != 0;
  bool get rainValid => (flags & 0x04) != 0;
  
  bool isChannelEnabled(int ch) => (channelStatus[ch] & 0x01) != 0;
  bool isChannelScheduleActive(int ch) => (channelStatus[ch] & 0x02) != 0;
  bool isChannelWatering(int ch) => (channelStatus[ch] & 0x04) != 0;
}
```

## Performance Impact

| Metric | Before | After | Improvement |
|--------|--------|-------|-------------|
| Connection sync queries | 10+ READs | 1 READ | 10x fewer round-trips |
| Initial sync latency | ~500ms | ~50ms | 10x faster |
| BLE traffic | ~400 bytes | 60 bytes | 85% reduction |

## Related Characteristics

For detailed configuration or historical data, use the individual characteristics:
- System Status (0xdef3) - Full system status details
- Environmental Data (0xde15) - Raw sensor readings
- Rain Sensor Data (0xde13) - Detailed rain metrics
- Compensation Status (0xde17) - Full compensation history
- Task Queue (0xdef7) - Complete task list
