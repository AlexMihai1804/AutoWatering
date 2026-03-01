# OTA Backend API Guide (Binary Hosting + Latest Version)

This document defines a practical backend contract so the app can:
- check the latest firmware version,
- download the OTA binary,
- then perform BLE OTA via mcumgr.

## Goals

- Keep binary hosting server-side (S3/object storage behind API).
- Let clients discover latest version safely.
- Support staged rollout by channel/hardware constraints.
- Avoid exposing private bucket structure directly.

## Version source of truth (recommended)

Use a root `VERSION` file as the single source of truth (for example `3.1.0`).

Why this is better than storing version only in `.conf`:
- `.conf` values are primarily build-time Kconfig settings.
- `VERSION` is tool-friendly and can be reused by CI, release scripts, app metadata, and docs.
- You can still inject it into firmware at build time (for `CONFIG_BT_DIS_FW_REV_STR`) using an auto-generated extra conf fragment.

Current release script detection order:
1. `--version` argument
2. root `VERSION` file
3. `CONFIG_BT_DIS_FW_REV_STR` from `prj.conf`
4. latest git tag

## Recommended architecture

- API Gateway + Lambda (or equivalent REST service)
- S3 bucket for OTA binaries
- DynamoDB/SQL table for release metadata
- Optional CloudFront for download acceleration

## Minimal release metadata model

```json
{
  "version": "2.4.1",
  "channel": "stable",
  "board": "arduino_nano_33_ble",
  "min_bootloader": "1.0.0",
  "artifact": {
    "name": "zephyr.signed.bin",
    "size_bytes": 719444,
    "sha256": "<hex>",
    "storage_key": "ota/stable/2.4.1/zephyr.signed.bin"
  },
  "published": true,
  "published_at": "2026-02-17T10:20:00Z",
  "notes": "Bugfix release"
}
```

## API endpoints

### 1) Publish a new release (backend/admin)

`POST /ota/releases`

Purpose:
- Register metadata for a new binary.
- Optionally return pre-signed upload URL if upload is client-side.

Response (example):

```json
{
  "ok": true,
  "version": "2.4.1",
  "upload_url": "https://...",
  "expires_in": 900
}
```

### 2) Mark release as public

`POST /ota/releases/{version}/publish`

Purpose:
- Make release visible to devices/apps.

### 3) Get latest release

`GET /ota/latest?channel=stable&board=arduino_nano_33_ble&current_version=2.4.0`

Response (example):

```json
{
  "ok": true,
  "update_available": true,
  "latest": {
    "version": "2.4.1",
    "channel": "stable",
    "mandatory": false,
    "artifact": {
      "name": "zephyr.signed.bin",
      "size_bytes": 719444,
      "sha256": "<hex>"
    },
    "download": {
      "url": "https://...",
      "expires_at": "2026-02-17T12:00:00Z"
    },
    "notes": "Bugfix release"
  }
}
```

### 4) Get download URL for specific version

`GET /ota/releases/{version}/download-url?channel=stable&board=arduino_nano_33_ble`

Use when app already decided target version and needs a fresh signed URL.

## App update flow using backend

1. App calls `GET /ota/latest`.
2. If `update_available=true`, app shows release notes + size.
3. App downloads binary from `download.url`.
4. App validates SHA256 locally.
5. App performs BLE OTA (`image upload` -> `image test` -> `os reset`).
6. App reconnects and confirms new version is running.

## Security recommendations

- Use short-lived pre-signed download URLs.
- Authorize `latest`/`download-url` endpoints (JWT/API key/device auth as needed).
- Validate SHA256 in app before OTA upload.
- Keep `published=false` releases hidden.
- Optional staged rollout (% rollout or allowlist by serial/hw_id).

## Version comparison policy

Use semantic versioning (`major.minor.patch`).

Rule:
- Offer update only if `latest.version > current_version` under semver comparison.

## Failure handling

- If download fails: refresh URL via `download-url` endpoint and retry.
- If checksum fails: discard file, report error, do not start OTA.
- If OTA fails on BLE: keep file cached and allow resume/retry.

## Suggested next implementation step

Implement backend endpoints first, then wire app to this order:

- check latest -> download -> verify checksum -> BLE OTA.

This keeps transport concerns separated:
- backend handles release discovery + binary delivery,
- BLE path handles device flashing.

## Automated release publish script

Repository automation script:

- `tools/publish_ota_release.py`

Example:

```powershell
python tools/publish_ota_release.py \
  --api-base-url https://<api-id>.execute-api.<region>.amazonaws.com \
  --admin-token <admin-token> \
  --channel stable \
  --board arduino_nano_33_ble \
  --sysbuild --pristine
```

The script performs automatically:
1. version auto-detection,
2. build,
3. SHA256 + size extraction,
4. `POST /ota/releases`,
5. binary upload (presigned PUT URL),
6. `POST /ota/releases/{version}/publish`.
