# UCP UART v1 Streaming (`STREAM_*`)

## Purpose

Streaming is required for payloads larger than the frame payload limit (`512 bytes`), such as:

- histories (watering/rain/environment)
- pack/blob transfers
- bulk export/import operations

## v1 Target Messages

- `STREAM_OPEN_REQ`
- `STREAM_OPEN_RESP`
- `STREAM_DATA`
- `STREAM_END`
- `STREAM_ABORT`

## v1 Target Rules

- At most one inbound stream and one outbound stream active at a time
- `stream_id` unique per session
- Chunked transfer using `STREAM_DATA`
- Optional final CRC/checksum in `STREAM_END`

## Current Firmware Implementation Status

Implemented now (core transport):

- `STREAM_OPEN_REQ` / `STREAM_OPEN_RESP`
- outbound `STREAM_DATA`
- outbound `STREAM_END` (includes final CRC32C)
- inbound `STREAM_DATA` (staging upload, validated sequence/offset)
- inbound `STREAM_END` (size + CRC32C verification)
- `STREAM_ABORT` (cancel active inbound/outbound stream)
- idle timeout cleanup (`STREAM_ABORT` with timeout status)
- single outbound + single inbound stream at a time

Currently available stream purposes (implementation-specific bootstrap helpers):

1. diagnostic snapshot text (`purpose=1`)
2. schema text summary (`purpose=2`)
3. inbound staging upload (`purpose=3`)
4. watering history page export (`purpose=4`, outbound)
5. pack list page (`purpose=5`, outbound)
6. plant list page (`purpose=6`, outbound)
7. paged schema catalog text (`purpose=7`, outbound)
8. rain history range export (`purpose=8`, outbound)
9. environmental history range export (`purpose=9`, outbound)
10. DB catalog export (plants/soils/irrigation methods) (`purpose=10`, outbound)
11. bulk snapshot export (`purpose=11`, outbound)
12. watering history range export (detailed + aggregated) (`purpose=12`, outbound)

Current domain integrations:

1. `history`:
   - `STREAM_OPEN_REQ` with `purpose=4` + `channel_id` + `page_index`
   - returns compact binary little-endian page payload
2. `packs`:
   - `purpose=5` pack list page (paged by `offset`)
   - `purpose=6` plant list page (paged by `offset`)
   - inbound `purpose=3` staging blob consumed by `packs.install_staged_*` commands
3. `history` (rain/environment):
   - `purpose=8` rain history range export
   - `purpose=9` environmental history range export
4. `device/system`:
   - `purpose=11` bulk snapshot binary export
5. `catalogs`:
   - `purpose=10` DB catalog paged text export (plant/soil/irrigation-method lookup data)
6. `history` (watering aggregated):
   - `purpose=12` watering history range export (`detailed` / `daily` / `monthly` / `annual`)

`STREAM_OPEN_RESP` now includes richer per-purpose metadata when applicable:

- stream schema/content type hints
- page offset / echoed query selectors
- total logical catalog length for paged schema streams
- optional `SCHEMA_ITEM_COUNT` for paged/list responses (separate from resource `SCHEMA_FIELD_COUNT`)

Still recommended / pending hardening (not missing core stream capability):

1. larger pack bundle transport beyond current RAM staging buffer limits (chunk-to-flash / rolling staging)
2. formal binary field-level schemas for every stream payload format (current discovery is descriptors + paged catalog text + metadata hints)
3. automated stream robustness tests (cancel/timeout/CRC mismatch/flood)

See also:

- `docs/uart-api/wire-reference.md` for exact `STREAM_*` TLV payloads and binary content headers (`UHP1`, `UPK1`, `UPT1`, `URH2`, `UEH2`, `UWH2`)
- `docs/uart-api/api-reference.md` for stream purpose query contracts and service-level usage
- `docs/uart-api/raw-payload-abi-catalog.md` for stream record struct ABI references
- `docs/uart-api/examples-and-traces.md` for worked `STREAM_OPEN/STREAM_DATA/STREAM_END` flows
- `docs/uart-api/client-implementation-guide.md` for stream manager/reassembly/upload patterns
