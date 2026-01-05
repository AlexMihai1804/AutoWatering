# Config Reset Characteristic

## Overview

Part of the **Custom Configuration Service** (`12345678-1234-5678-9abc-def123456780`).

Provides the ability to reset configuration data to factory defaults or clear specific configuration subsystems.

## Identification

| Property | Value |
| --- | --- |
| UUID | `12345678-1234-5678-9abc-def123456783` |
| Size | Variable |
| Properties | READ, NOTIFY |
| Service | Custom Configuration Service |
| Characteristic Index | 32 (in overall numbering) |

## Properties

| Property | Supported | Notes |
| --- | --- | --- |
| Read | Yes | Returns current reset status |
| Write | No | Reset commands via Config Status (#33) |
| Notify | Yes | Notifies on reset completion |

## Data Structure

```c
struct __attribute__((packed)) config_reset_response {
    uint8_t status;           /* 0=idle, 1=in_progress, 2=complete, 3=error */
    uint8_t subsystem;        /* Which subsystem was reset */
    uint16_t reserved;        /* Reserved for future use */
};
```

## Reset Subsystems

| Code | Subsystem | Description |
| --- | --- | --- |
| 0x00 | All | Full factory reset |
| 0x01 | Channels | Reset all channel configurations |
| 0x02 | Schedules | Reset all schedules |
| 0x03 | Compensation | Reset rain/temp compensation settings |
| 0x04 | Custom Soil | Clear custom soil profiles |
| 0x05 | History | Clear all history data |

## Usage Flow

1. Client initiates reset via Config Status (#33) write
2. Firmware processes reset request
3. Config Reset (#32) sends notify with completion status
4. Client can read for current status

---

Part of AutoWatering BLE Documentation - Updated January 2026
