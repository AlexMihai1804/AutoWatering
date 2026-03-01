# OTA Update over BLE (Application Guide)

This guide describes the recommended firmware OTA flow over BLE using MCUboot + mcumgr SMP.

## Firmware-side prerequisites

- MCUboot bootloader enabled.
- mcumgr over BLE enabled (`CONFIG_MCUMGR_TRANSPORT_BT=y`).
- SMP UUIDs:
  - Service: `8D53DC1D-1DB7-4CD3-868B-8A527460AA84`
  - Characteristic: `DA2E7828-FBCE-4E01-AE9E-261174997C48`
- Recommended OTA artifact: `zephyr.signed.bin` (from sysbuild output).

## App-side preconditions

1. BLE connection with security (pairing/encryption).
2. Maximum practical MTU (ideally 247+ when supported).
3. Generous operation timeouts (upload can take time).
4. Resume support using mcumgr `off` offsets.

## Recommended OTA flow (strict order)

1. **Image List**
   - Send `image list`.
   - Read current active image hash and slot states.

2. **Image Upload (`zephyr.signed.bin`)**
   - Send SMP `image upload` chunks.
   - Always continue from returned `off`.
   - On disconnect, reconnect and resume from current `off`.

3. **Image Test**
   - After upload completes, send `image test <new_hash>`.
   - This marks the new image as pending for next boot.

4. **Device Reset**
   - Send `os reset` (or your internal reset path if required).
   - Device boots into the new image.

5. **Post-boot verification**
   - Reconnect BLE.
   - Validate firmware version via your app API.
   - Firmware auto-confirms image after successful boot (`boot_write_img_confirmed()`), making update permanent.

## Rollback policy

Trigger app-level rollback handling if:
- new version is not visible after reconnect timeout;
- primary BLE service fails to initialize;
- startup health checks repeatedly fail.

Note: MCUboot also rolls back automatically if the new image is not confirmed.

## App UX recommendations

- Show upload progress as `off / total`.
- Use phase-based status: Upload -> Test -> Reboot -> Reconnect -> Validate.
- Provide `Retry` that resumes from offset, not from zero.
- Temporarily block critical irrigation-control operations during OTA.

## OTA package constraints

- Use `zephyr.signed.bin` built with the same partition layout used on devices.
- Verify it fits in `slot1` (external flash) before publishing.

## Release pipeline (recommended)

1. Build release (`--sysbuild --pristine`).
2. Store artifacts:
   - `zephyr.signed.bin`
   - release metadata (version, SHA256, build timestamp)
3. Publish to OTA backend.
4. App downloads binary and runs the SMP flow above.

## Integrating with backend latest-version + download

Use the backend contract from [OTA Backend API Guide](ota-backend-api.md):

1. App requests latest release metadata (`GET /ota/latest?...`).
2. App compares current firmware vs backend version.
3. App requests secure download URL (`GET /ota/releases/{version}/download-url`).
4. App downloads binary to local storage.
5. App performs BLE OTA using the downloaded file.

## Quick OTA debug checklist

- SMP service/characteristic are visible after connect.
- Pairing/encryption are active.
- Upload returns `rc=0` and increasing offsets.
- `image test` returns success.
- Device reboots and reports the new version.
