# Bootloader + OTA (Current State)

This document captures the current MCUboot + OTA status after the stabilization work completed in February 2026.

## Current status

- MCUboot is stable, with USB CDC recovery enabled.
- Internal partition layout is stabilized for Arduino Nano 33 BLE:
  - `boot_partition`: `0x00000000..0x00022000` (136 KB)
  - `slot0` (primary): `0x00022000..0x000D8000` (728 KB)
- `slot1` + `scratch` are on external flash (W25Q128).
- Factory flow (`tools/factory_flash.py`) performs provisioning + build + flash end-to-end.
- Application image is signed (`zephyr.signed.hex` / `zephyr.signed.bin`) so MCUboot sees a valid image header.
- Image confirmation after successful boot is enabled in app (`boot_write_img_confirmed()`), so the upgrade becomes permanent.

## Key fixes applied

1. **Correct flash artifact selection**
   - `zephyr.signed.hex` is preferred over unsigned `zephyr.hex`.

2. **MCUboot + USB CDC boot stability**
   - Fixed serial recovery trapping by using proper reset handling.
   - OpenOCD flow clears `RESETREAS` and triggers software reset (`AIRCR`) to avoid accidental `PIN_RESET` entry.

3. **Correct BLE + SMP initialization path**
   - Removed duplicate SMP BT registration that caused GATT handle conflicts.
   - SMP BT service is initialized by Zephyr mcumgr transport handlers.

4. **Robust factory script execution on Windows**
   - Script enforces `WEST_PYTHON`/`PYTHON_EXECUTABLE` to the active interpreter to avoid mixed-venv issues.

## Recommended factory command

```powershell
python tools/factory_flash.py
```

The script:
- detects `HW ID`;
- requests provisioning from backend;
- generates a serial-based Zephyr conf fragment;
- builds + flashes + verifies;
- stores manifest files under `build/factory-manifests/`.

## OTA over BLE (application side)

Full implementation guide for mobile/desktop app:

- [OTA BLE App Guide](ota-ble-app-guide.md)

Includes:
- exact command order (`image list/upload/test/reset/confirm`);
- resumable upload using `off`;
- preflight validation + rollback handling;
- practical UX recommendations.

## OTA backend support (latest version + download)

Backend contract and deployment model:

- [OTA Backend API Guide](ota-backend-api.md)

Includes:
- release upload + publish flow;
- `latest` endpoint for app update checks;
- secure download URLs for binary retrieval.

## Safe GitHub publishing

Use this checklist before every push:

- [GitHub Safe Publish Checklist](github-safe-publish.md)

Critical rules:
- never commit `tools/factory.settings.json`;
- never commit tokens/keys/private URLs;
- commit only placeholder examples such as `tools/factory.settings.json.example`.
