# MCUboot DFU System - AutoWatering

This document describes the firmware update (DFU) system for AutoWatering, supporting both BLE and USB update paths.

## Overview

The AutoWatering firmware uses **MCUboot** as its bootloader, providing:
- **BLE DFU**: Normal firmware updates over Bluetooth (via mcumgr/SMP)
- **USB DFU (Recovery)**: Emergency updates via USB when the app is bricked
- **External Flash Storage**: Secondary slot on W25Q128 for safe rollback

## Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                    Internal Flash (1 MB)                         │
├──────────────────┬──────────────────────────────────────────────┤
│  MCUboot (80KB)  │          Application Slot0 (784KB)           │
│    0x0-0x13FFF   │              0x14000-0xD7FFF                  │
├──────────────────┴─────────────────────────┬────────────────────┤
│           NVS Storage (150KB)              │  Settings (8KB)    │
│            0xD8000-0xFDFFF                 │  0xFE000-0xFFFFF   │
└────────────────────────────────────────────┴────────────────────┘

┌─────────────────────────────────────────────────────────────────┐
│                   External Flash W25Q128 (16 MB)                 │
├──────────────────┬──────────────┬──────────────┬────────────────┤
│  Slot1 (1MB)     │ Scratch(128K)│ Database(512K│ Storage(~14MB) │
│  0x0-0xFFFFF     │ 0x100000-    │ 0x120000-    │ 0x1A0000-      │
│ (Secondary Image)│  0x11FFFF    │  0x19FFFF    │  0xFFFFFF      │
└──────────────────┴──────────────┴──────────────┴────────────────┘
```

## DFU Methods

### 1. BLE DFU (Normal Operation)

When the application is running normally, firmware updates are performed over BLE using the mcumgr protocol.

**Requirements:**
- Application must be running
- BLE connection established
- mcumgr tool installed on host

**Steps:**

1. Connect to device via BLE:
```bash
# List available BLE devices
mcumgr --conntype ble --connstring "ctlr_name=hci0" scan

# Connect and check images
mcumgr --conntype ble --connstring "peer_name=AutoWatering" image list
```

2. Upload new firmware:
```bash
mcumgr --conntype ble --connstring "peer_name=AutoWatering" image upload app_update.bin
```

3. Test the new image (marks for swap on next reboot):
```bash
mcumgr --conntype ble --connstring "peer_name=AutoWatering" image test <hash>
```

4. Reset to boot into new firmware:
```bash
mcumgr --conntype ble --connstring "peer_name=AutoWatering" reset
```

5. Confirm the image (prevents rollback):
```bash
mcumgr --conntype ble --connstring "peer_name=AutoWatering" image confirm
```

### 2. USB DFU (Recovery Mode)

When the application is bricked or non-responsive, use USB recovery mode which runs entirely in MCUboot.

**Entering Recovery Mode:**

**Option A: DFU Button (Recommended)**
1. Connect USB cable
2. Hold the **DFU button** (P0.13 by default)
3. Press and release RESET while holding DFU button
4. Release DFU button after ~1 second
5. Device enters MCUboot serial recovery mode

**Option B: Automatic Timeout**
1. Connect USB cable
2. Reset the device
3. MCUboot waits 3 seconds for DFU connection
4. If mcumgr connects during this window, recovery mode activates

**USB Recovery Steps:**

1. Find the COM port (Windows) or /dev/ttyACM* (Linux):
```bash
# Windows: Check Device Manager for "USB Serial Device (COMx)"
# Linux: ls /dev/ttyACM*
```

2. List current images:
```bash
mcumgr --conntype serial --connstring "dev=COM5,baud=115200" image list
```

3. Upload recovery firmware:
```bash
mcumgr --conntype serial --connstring "dev=COM5,baud=115200" image upload app_update.bin
```

4. Reset to boot new image:
```bash
mcumgr --conntype serial --connstring "dev=COM5,baud=115200" reset
```

## Installing mcumgr

### macOS / Linux
```bash
go install github.com/apache/mynewt-mcumgr-cli/mcumgr@latest
```

### Windows
```bash
go install github.com/apache/mynewt-mcumgr-cli/mcumgr@latest
# Or download pre-built binary from releases
```

### Alternative: nRF Connect (GUI)
Nordic's nRF Connect for Desktop includes a Programmer app that supports mcumgr DFU.

## Image Signing

MCUboot validates image signatures. For production, generate signing keys:

```bash
# Generate RSA-2048 key pair
imgtool keygen -k my_signing_key.pem -t rsa-2048

# Sign the application image
imgtool sign --key my_signing_key.pem \
    --header-size 0x200 \
    --align 4 \
    --version 1.0.0 \
    --slot-size 0xC4000 \
    zephyr.hex \
    app_signed.bin
```

For development, signature checking can be disabled in `sysbuild/mcuboot.conf`:
```
CONFIG_BOOT_SIGNATURE_TYPE_NONE=y
```

## Troubleshooting

### USB Device Not Recognized
- Ensure USB cable supports data (not just charging)
- Check USB drivers are installed
- Try different USB port

### Recovery Mode Not Entering
- Verify DFU button is properly wired (P0.13 to GND)
- Increase timeout in `mcuboot.conf`: `CONFIG_BOOT_SERIAL_WAIT_FOR_DFU_TIMEOUT=5000`
- Try multiple reset attempts

### Image Upload Fails
- Check image is properly signed
- Verify slot sizes match between primary and secondary
- Ensure enough space in slot1 (1MB)

### BLE DFU Slow
- Move closer to device
- Use 2M PHY if supported: device requests 2M PHY automatically
- Increase connection interval for throughput

### Rollback After Crash
MCUboot automatically rolls back if:
- New image fails to boot
- `image confirm` was not called
- Image hash verification fails

To force rollback manually:
```bash
mcumgr image test <old_image_hash>
mcumgr reset
```

## Configuration Files

| File | Purpose |
|------|---------|
| `prj.conf` | Application config (mcumgr, BLE SMP) |
| `sysbuild/mcuboot.conf` | Bootloader config (USB recovery, signing) |
| `sysbuild/mcuboot.overlay` | Bootloader devicetree (partitions, GPIO) |
| `boards/promicro_52840.overlay` | Application devicetree (flash layout) |
| `pm_static.yml` | Partition Manager layout |

## Hardware Wiring

### DFU Button
Connect a momentary button between **P0.13** and **GND**:
```
P0.13 ──┤ ├── GND
        Button
```
The internal pull-up is enabled, so no external resistor needed.

### Status LEDs (Optional)
MCUboot can indicate status via LEDs. Add to `mcuboot.overlay`:
```dts
aliases {
    mcuboot-led0 = &led0;  // Blinks during DFU
};
```

## Version History

| Version | Changes |
|---------|---------|
| 1.0.0 | Initial MCUboot + BLE/USB DFU support |
