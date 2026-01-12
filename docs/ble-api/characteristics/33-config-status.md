# Config Status Characteristic

## Overview

Part of the **Custom Configuration Service** (`12345678-1234-5678-9abc-def123456780`).

Provides configuration completeness status and triggers configuration operations including resets.

## Identification

| Property | Value |
| --- | --- |
| UUID | `12345678-1234-5678-9abc-def123456783` |
| Size | Variable |
| Properties | READ, WRITE, NOTIFY |
| Service | Custom Configuration Service |
| Characteristic Index | 33 (in overall numbering) |

## Properties

| Property | Supported | Notes |
| --- | --- | --- |
| Read | Yes | Returns configuration completeness status |
| Write | Yes | Triggers configuration operations |
| Notify | Yes | Status updates after operations |

## Read Response Structure

```c
struct __attribute__((packed)) config_status_response {
    uint8_t overall_completeness;   /* 0-100% overall config completion */
    uint8_t channel_mask;           /* Bitmask: which channels are configured */
    uint8_t schedule_mask;          /* Bitmask: which channels have schedules */
    uint8_t compensation_status;    /* 0=default, 1=customized */
    uint8_t custom_soil_count;      /* Number of custom soil profiles defined */
    uint8_t onboarding_complete;    /* 0=incomplete, 1=complete */
    uint16_t flags;                 /* Additional status flags */
};
```

## Write Commands

| Command | Value | Description |
| --- | --- | --- |
| Query Status | 0x00 | Request status update (triggers notify) |
| Validate Config | 0x01 | Run validation on all configs |
| Reset Subsystem | 0x10-0x15 | Initiate reset (see Config Reset #32 for codes) |

## Status Flags

| Bit | Name | Description |
| --- | --- | --- |
| 0 | NEEDS_SYNC | Configuration changed, needs app sync |
| 1 | NVS_ERROR | Persistent storage error detected |
| 2 | VALIDATION_ERROR | Config validation failed |
| 3-7 | Reserved | For future use |

## Usage Examples

### Query Configuration Status

```text
Write: [0x00]
Response (Notify): [75, 0xFF, 0x03, 0x01, 0x02, 0x01, 0x00, 0x00]
  - 75% complete
  - All 8 channels configured
  - 2 channels have schedules
  - Compensation customized
  - 2 custom soil profiles
  - Onboarding complete
```

---

Part of AutoWatering BLE Documentation - Updated January 2026
