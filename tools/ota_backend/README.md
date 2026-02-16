# OTA Backend API (Lambda)

Implementation file:
- `lambda_ota_api.py`

## AWS resources

- S3 bucket for OTA binaries
- DynamoDB table with keys:
  - Partition key: `pk` (String)
  - Sort key: `sk` (String)
- API Gateway HTTP API routing to Lambda

## Required Lambda environment variables

- `OTA_BUCKET` = S3 bucket name
- `OTA_TABLE` = DynamoDB table name
- `OTA_ADMIN_TOKEN` = admin token for release create/publish endpoints
- `OTA_SIGNED_URL_TTL` = pre-signed URL TTL in seconds (default 900)

## Endpoints

- `POST /ota/releases`
- `POST /ota/releases/{version}/publish`
- `GET /ota/latest`
- `GET /ota/releases/{version}/download-url`

## Notes

- `POST` endpoints require header `x-admin-token` when `OTA_ADMIN_TOKEN` is set.
- `GET` endpoints return pre-signed download URLs for OTA binaries.
