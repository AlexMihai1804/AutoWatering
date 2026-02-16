#!/usr/bin/env python3
"""
AWS Lambda OTA backend API (API Gateway HTTP API compatible).

Endpoints:
- POST /ota/releases
- POST /ota/releases/{version}/publish
- GET  /ota/latest?channel=stable&board=arduino_nano_33_ble&current_version=3.1.0
- GET  /ota/releases/{version}/download-url?channel=stable&board=arduino_nano_33_ble

Environment variables:
- OTA_BUCKET (required)
- OTA_TABLE (required)    DynamoDB table with PK=pk (S), SK=sk (S)
- OTA_ADMIN_TOKEN (optional, enables admin auth for POST endpoints)
- OTA_SIGNED_URL_TTL (optional, default 900)
"""

from __future__ import annotations

import json
import os
import re
from datetime import datetime, timezone
from typing import Any, Dict, List, Optional, Tuple

import boto3
from boto3.dynamodb.conditions import Key

SEMVER_RE = re.compile(r"^v?(\d+)\.(\d+)\.(\d+)(?:[-+][0-9A-Za-z.-]+)?$")


def _utc_now_iso() -> str:
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def _response(status_code: int, body: Dict[str, Any]) -> Dict[str, Any]:
    return {
        "statusCode": status_code,
        "headers": {
            "content-type": "application/json",
            "cache-control": "no-store",
        },
        "body": json.dumps(body),
    }


def _parse_json_body(event: Dict[str, Any]) -> Dict[str, Any]:
    body = event.get("body")
    if not body:
        return {}
    if event.get("isBase64Encoded"):
        import base64

        body = base64.b64decode(body).decode("utf-8")
    return json.loads(body)


def _semver_key(version: str) -> Tuple[int, int, int, str]:
    match = SEMVER_RE.match(version or "")
    if not match:
        raise ValueError(f"Invalid semantic version: {version!r}")
    major = int(match.group(1))
    minor = int(match.group(2))
    patch = int(match.group(3))
    normalized = f"{major}.{minor}.{patch}"
    return major, minor, patch, normalized


def _compare_semver(new_version: str, current_version: str) -> int:
    n1 = _semver_key(new_version)[:3]
    n2 = _semver_key(current_version)[:3]
    if n1 > n2:
        return 1
    if n1 < n2:
        return -1
    return 0


def _paths(event: Dict[str, Any]) -> Tuple[str, str]:
    request_context = event.get("requestContext", {})
    http = request_context.get("http", {})
    method = (http.get("method") or "").upper()
    path = http.get("path") or event.get("rawPath") or ""
    return method, path


def _query(event: Dict[str, Any], key: str, default: str = "") -> str:
    params = event.get("queryStringParameters") or {}
    value = params.get(key)
    if value is None or value == "":
        return default
    return str(value)


def _header(event: Dict[str, Any], key: str, default: str = "") -> str:
    headers = event.get("headers") or {}
    if not headers:
        return default
    for header_key, header_value in headers.items():
        if header_key.lower() == key.lower():
            return str(header_value)
    return default


def _require_admin(event: Dict[str, Any]) -> Optional[Dict[str, Any]]:
    expected = os.getenv("OTA_ADMIN_TOKEN", "").strip()
    if not expected:
        return None
    provided = _header(event, "x-admin-token", "").strip()
    if provided != expected:
        return _response(401, {"ok": False, "error": "Unauthorized"})
    return None


def _extract_version_from_path(path: str) -> Optional[str]:
    match = re.match(r"^/ota/releases/([^/]+)", path)
    if not match:
        return None
    return match.group(1)


def _release_pk(channel: str, board: str) -> str:
    return f"RELEASE#{channel}#{board}"


def _release_sk(version: str) -> str:
    return f"VERSION#{version}"


def _latest_pk(channel: str, board: str) -> str:
    return f"LATEST#{channel}#{board}"


def _latest_sk() -> str:
    return "LATEST"


def _required_env() -> Tuple[str, str, int]:
    bucket = os.getenv("OTA_BUCKET", "").strip()
    table = os.getenv("OTA_TABLE", "").strip()
    ttl_str = os.getenv("OTA_SIGNED_URL_TTL", "900").strip()
    if not bucket or not table:
        raise RuntimeError("OTA_BUCKET and OTA_TABLE env vars are required")
    try:
        ttl = int(ttl_str)
    except ValueError as exc:
        raise RuntimeError("OTA_SIGNED_URL_TTL must be an integer") from exc
    return bucket, table, ttl


def _create_release(event: Dict[str, Any], table, s3_client, bucket: str, url_ttl: int) -> Dict[str, Any]:
    auth_error = _require_admin(event)
    if auth_error:
        return auth_error

    body = _parse_json_body(event)
    version = str(body.get("version", "")).strip()
    channel = str(body.get("channel", "stable")).strip() or "stable"
    board = str(body.get("board", "arduino_nano_33_ble")).strip() or "arduino_nano_33_ble"
    notes = str(body.get("notes", "")).strip()

    artifact = body.get("artifact", {}) if isinstance(body.get("artifact"), dict) else {}
    artifact_name = str(artifact.get("name", "zephyr.signed.bin")).strip() or "zephyr.signed.bin"
    artifact_size = int(artifact.get("size_bytes", 0) or 0)
    artifact_sha256 = str(artifact.get("sha256", "")).strip().lower()
    storage_key = str(
        artifact.get("storage_key", f"ota/{channel}/{board}/{version}/{artifact_name}")
    ).strip()

    try:
        _, _, _, normalized_version = _semver_key(version)
    except ValueError as exc:
        return _response(400, {"ok": False, "error": str(exc)})

    now_iso = _utc_now_iso()

    item = {
        "pk": _release_pk(channel, board),
        "sk": _release_sk(normalized_version),
        "version": normalized_version,
        "channel": channel,
        "board": board,
        "published": False,
        "created_at": now_iso,
        "updated_at": now_iso,
        "artifact": {
            "name": artifact_name,
            "size_bytes": artifact_size,
            "sha256": artifact_sha256,
            "storage_key": storage_key,
        },
        "notes": notes,
    }

    table.put_item(Item=item)

    upload_url = s3_client.generate_presigned_url(
        ClientMethod="put_object",
        Params={
            "Bucket": bucket,
            "Key": storage_key,
            "ContentType": "application/octet-stream",
        },
        ExpiresIn=url_ttl,
    )

    return _response(
        200,
        {
            "ok": True,
            "version": normalized_version,
            "channel": channel,
            "board": board,
            "upload_url": upload_url,
            "expires_in": url_ttl,
            "artifact": {
                "name": artifact_name,
                "size_bytes": artifact_size,
                "sha256": artifact_sha256,
                "storage_key": storage_key,
            },
        },
    )


def _publish_release(event: Dict[str, Any], table) -> Dict[str, Any]:
    auth_error = _require_admin(event)
    if auth_error:
        return auth_error

    method, path = _paths(event)
    version_from_path = _extract_version_from_path(path)
    if not version_from_path:
        return _response(400, {"ok": False, "error": "Version missing in path"})

    try:
        _, _, _, normalized_version = _semver_key(version_from_path)
    except ValueError as exc:
        return _response(400, {"ok": False, "error": str(exc)})

    channel = _query(event, "channel", "stable")
    board = _query(event, "board", "arduino_nano_33_ble")

    pk = _release_pk(channel, board)
    sk = _release_sk(normalized_version)

    existing = table.get_item(Key={"pk": pk, "sk": sk}).get("Item")
    if not existing:
        return _response(404, {"ok": False, "error": "Release not found"})

    now_iso = _utc_now_iso()

    table.update_item(
        Key={"pk": pk, "sk": sk},
        UpdateExpression="SET published = :pub, updated_at = :updated, published_at = :published_at",
        ExpressionAttributeValues={
            ":pub": True,
            ":updated": now_iso,
            ":published_at": now_iso,
        },
    )

    table.put_item(
        Item={
            "pk": _latest_pk(channel, board),
            "sk": _latest_sk(),
            "version": normalized_version,
            "channel": channel,
            "board": board,
            "updated_at": now_iso,
        }
    )

    return _response(
        200,
        {
            "ok": True,
            "published": True,
            "version": normalized_version,
            "channel": channel,
            "board": board,
            "published_at": now_iso,
        },
    )


def _get_latest(event: Dict[str, Any], table, s3_client, bucket: str, url_ttl: int) -> Dict[str, Any]:
    channel = _query(event, "channel", "stable")
    board = _query(event, "board", "arduino_nano_33_ble")
    current_version = _query(event, "current_version", "")

    latest_ptr = table.get_item(Key={"pk": _latest_pk(channel, board), "sk": _latest_sk()}).get("Item")
    if not latest_ptr:
        return _response(200, {"ok": True, "update_available": False, "reason": "no_published_release"})

    version = latest_ptr.get("version")
    if not version:
        return _response(500, {"ok": False, "error": "Invalid latest pointer"})

    release = table.get_item(Key={"pk": _release_pk(channel, board), "sk": _release_sk(version)}).get("Item")
    if not release or not release.get("published"):
        return _response(404, {"ok": False, "error": "Published release metadata not found"})

    artifact = release.get("artifact", {})
    storage_key = artifact.get("storage_key")
    if not storage_key:
        return _response(500, {"ok": False, "error": "Release artifact key missing"})

    update_available = True
    if current_version:
        try:
            update_available = _compare_semver(version, current_version) > 0
        except ValueError:
            update_available = True

    download_url = s3_client.generate_presigned_url(
        ClientMethod="get_object",
        Params={"Bucket": bucket, "Key": storage_key},
        ExpiresIn=url_ttl,
    )

    return _response(
        200,
        {
            "ok": True,
            "update_available": update_available,
            "latest": {
                "version": version,
                "channel": channel,
                "board": board,
                "published_at": release.get("published_at", ""),
                "notes": release.get("notes", ""),
                "artifact": {
                    "name": artifact.get("name", "zephyr.signed.bin"),
                    "size_bytes": int(artifact.get("size_bytes", 0) or 0),
                    "sha256": artifact.get("sha256", ""),
                },
                "download": {
                    "url": download_url,
                    "expires_in": url_ttl,
                },
            },
        },
    )


def _get_download_url(event: Dict[str, Any], table, s3_client, bucket: str, url_ttl: int) -> Dict[str, Any]:
    method, path = _paths(event)
    version_from_path = _extract_version_from_path(path)
    if not version_from_path:
        return _response(400, {"ok": False, "error": "Version missing in path"})

    try:
        _, _, _, normalized_version = _semver_key(version_from_path)
    except ValueError as exc:
        return _response(400, {"ok": False, "error": str(exc)})

    channel = _query(event, "channel", "stable")
    board = _query(event, "board", "arduino_nano_33_ble")

    release = table.get_item(
        Key={"pk": _release_pk(channel, board), "sk": _release_sk(normalized_version)}
    ).get("Item")
    if not release or not release.get("published"):
        return _response(404, {"ok": False, "error": "Release not found or not published"})

    artifact = release.get("artifact", {})
    storage_key = artifact.get("storage_key")
    if not storage_key:
        return _response(500, {"ok": False, "error": "Release artifact key missing"})

    download_url = s3_client.generate_presigned_url(
        ClientMethod="get_object",
        Params={"Bucket": bucket, "Key": storage_key},
        ExpiresIn=url_ttl,
    )

    return _response(
        200,
        {
            "ok": True,
            "version": normalized_version,
            "channel": channel,
            "board": board,
            "download": {
                "url": download_url,
                "expires_in": url_ttl,
            },
            "artifact": {
                "name": artifact.get("name", "zephyr.signed.bin"),
                "size_bytes": int(artifact.get("size_bytes", 0) or 0),
                "sha256": artifact.get("sha256", ""),
            },
        },
    )


def lambda_handler(event: Dict[str, Any], context: Any) -> Dict[str, Any]:
    try:
        bucket, table_name, url_ttl = _required_env()
    except RuntimeError as exc:
        return _response(500, {"ok": False, "error": str(exc)})

    dynamodb = boto3.resource("dynamodb")
    table = dynamodb.Table(table_name)
    s3_client = boto3.client("s3")

    method, path = _paths(event)

    try:
        if method == "POST" and path == "/ota/releases":
            return _create_release(event, table, s3_client, bucket, url_ttl)

        if method == "POST" and path.startswith("/ota/releases/") and path.endswith("/publish"):
            return _publish_release(event, table)

        if method == "GET" and path == "/ota/latest":
            return _get_latest(event, table, s3_client, bucket, url_ttl)

        if method == "GET" and path.startswith("/ota/releases/") and path.endswith("/download-url"):
            return _get_download_url(event, table, s3_client, bucket, url_ttl)

        return _response(404, {"ok": False, "error": "Not found"})

    except ValueError as exc:
        return _response(400, {"ok": False, "error": str(exc)})
    except Exception as exc:  # pylint: disable=broad-except
        return _response(500, {"ok": False, "error": f"Internal error: {exc}"})
