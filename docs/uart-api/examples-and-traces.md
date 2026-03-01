# UCP UART v1 Examples and Traces (Implementation Walkthrough)

This document shows end-to-end message flows using the exact UCP concepts defined in:

- `wire-reference.md` (frame/header/TLV bytes)
- `api-reference.md` (method contracts)
- `raw-payload-abi-catalog.md` (raw ABI payloads)

Goal: a host implementer should be able to build a working client by following these traces.

## 1. Conventions Used in Examples

- Examples show the **decoded UCP frame** (before COBS encoding, before trailing `0x00` delimiter).
- Header values are shown field-by-field (not packed hex blobs), to avoid ambiguity.
- TLVs are shown as `(field_id, type, len, value)` and, where useful, byte breakdown.
- CRC32C bytes are omitted in examples unless explicitly needed; clients must compute CRC32C over `header+payload`.
- All multi-byte values are little-endian.
- `request_id` is chosen by client and echoed by server.
- `seq_tx` is monotonic per direction.

## 2. Session Bring-Up (HELLO + PING)

## 2.1 `HELLO_REQ` (peer -> nRF)

Purpose:

- declare client identity and supported protocol range
- optionally request replay resume and default subscription profile

Example semantic payload:

- `client_name = "esp32-bridge"`
- `client_fw_version = "0.1.0"`
- `supported_proto_min = 1`
- `supported_proto_max = 1`
- `client_capabilities = 0`

Header (decoded):

- `proto_version=1`
- `msg_type=HELLO_REQ (0x01)`
- `flags=0`
- `header_len=24`
- `service_id=0x0001` (`session`)
- `method_or_topic_id=0x0000`
- `status_code=0`
- `payload_len=<computed>`
- `request_id=1`
- `seq_tx=1` (first client frame after UART open)
- `stream_id=0`
- `reserved=0`

TLV payload example (semantic):

- `UCP_F_CLIENT_NAME` (`UTF8`) = `"esp32-bridge"`
- `UCP_F_CLIENT_FW_VERSION` (`UTF8`) = `"0.1.0"`
- `UCP_F_PROTO_MIN` (`U8`) = `1`
- `UCP_F_PROTO_MAX` (`U8`) = `1`
- `UCP_F_CAPABILITIES` (`U32`) = `0`

Client transmit sequence:

1. Build decoded `header + payload`.
2. Append CRC32C (`LE`).
3. COBS-encode result.
4. Append `0x00` delimiter.
5. Write to UART.

## 2.2 `HELLO_RESP` (nRF -> peer)

Purpose:

- accept protocol version
- publish session and boot identity
- publish schema/version/capabilities/event replay state

Critical fields clients should cache immediately:

- `UCP_F_SESSION_ID`
- `UCP_F_BOOT_ID`
- `UCP_F_SCHEMA_VERSION`
- `UCP_F_REGISTRY_HASH`
- `UCP_F_EVENT_SEQ`
- `UCP_F_EVENT_REPLAY_DEPTH`
- `UCP_F_CAPABILITIES`

If `BOOT_ID` changed from previous connection:

- discard old replay assumptions
- invalidate cached resource versions (or mark stale)
- run snapshot sync for subscribed domains

## 2.3 `PING_REQ` / `PING_RESP`

Use `PING` for keepalive when no request/event traffic exists.

Recommended client behavior:

- send `PING_REQ` every `5s` idle
- declare connection dead after `15s` without valid traffic (`PING_RESP` or any valid frame)

## 3. Basic Read (`GET`) Example

## 3.1 `GET device.system_status`

Request:

- `msg_type=GET_REQ`
- `service_id=0x0010` (`device/system`)
- `method_or_topic_id=0x0001` (`device.system_status`)
- `request_id=2`

Optional query TLVs:

- none (simple global resource)

Response:

- `msg_type=GET_RESP`
- same `request_id=2`
- `status_code=OK`

Payload (typed TLV resource; see `api-reference.md` field set `SYS_*`):

- `SYS_STATUS` (`ENUM_U16`)
- `SYS_STATE` (`ENUM_U16`)
- `SYS_UPTIME_MS` (`U32`)
- `SYS_UNIX_UTC` (`U32` or `U64` depending field definition used)
- `UCP_F_RESOURCE_VERSION` (`U32`)
- `UCP_F_TIMESTAMP_UTC_MS` (`U64`)

Client handling:

1. Validate frame (COBS, CRC, header).
2. Verify `request_id` matches in-flight request.
3. Parse TLVs; ignore unknown optional fields.
4. Cache by `(service_id, resource_id, instance_id=global)`.

## 4. Settings Write (`SET`) Example + Notify Echo

## 4.1 `SET rtc_tz.timezone_config`

Goal:

- update timezone settings from host
- receive immediate `SET_RESP`
- also receive `EVENT_CHANGE` with applied delta (`origin=uart`)

Request:

- `msg_type=SET_REQ`
- `service_id=0x0018` (`rtc_tz`)
- `method_or_topic_id=0x0001` (`timezone_config`)
- `request_id=3`

Typical payload TLVs (example):

- `TZ_UTC_OFFSET_MIN` (`I32`) = `120`
- `TZ_DST_ENABLED` (`BOOL`) = `1`
- `TZ_APPLY_MODE` (`ENUM_U16`) = `apply`

Response (`SET_RESP`):

- `status_code=OK`
- `UCP_F_RESOURCE_VERSION` new version
- `UCP_F_TIMESTAMP_UTC_MS`
- applied fields (delta or canonicalized values)

Then asynchronous event (`EVENT_CHANGE`):

- `topic_id = rtc_tz.config` (exact topic ID per `api-reference.md`)
- `resource_id = rtc_tz.timezone_config`
- `origin = uart`
- `event_seq = N`
- `delta_tlv` with applied values

Important client rule:

- Do not assume `SET_RESP` replaces the notify path.
- Treat `EVENT_CHANGE` as the canonical change feed for cache synchronization.

## 4.2 `SET` Validation Failure Example

If client sends invalid timezone offset (example out of range):

- server returns `SET_RESP` with `status_code=OUT_OF_RANGE` (or `INVALID_ARG`)
- no `EVENT_CHANGE` should be emitted for failed apply

Client should:

- surface error to caller
- not mutate local cache optimistically unless explicitly designed to do so

## 5. Subscriptions and Replay (`SUB_REQ`)

## 5.1 Live-Only Subscription

Use when client already has fresh snapshot and only wants future deltas.

`SUB_REQ` key fields:

- `subscription_id`
- topic mask/list
- `delivery_mode=live_only`
- optional instance filter(s)
- `snapshot_before_live=false`

## 5.2 Replay-Then-Live Subscription (recommended after reconnect without reboot)

Use when:

- same `BOOT_ID`
- client knows its last processed `event_seq`

`SUB_REQ` key fields:

- `delivery_mode=replay_then_live`
- `since_event_seq = last_processed_event_seq`
- `snapshot_before_live = true` (recommended)

Expected behavior:

1. `SUB_RESP` indicates accepted topics and whether replay starts.
2. Server may emit snapshot events (or client may fetch snapshots via `GET`, depending flow).
3. Server replays buffered `EVENT_CHANGE`s after `since_event_seq`.
4. Server continues with live events.

If replay buffer cannot satisfy request:

- server emits `EVENT_OVERFLOW`
- client must resync by `GET` snapshots + fresh subscription

## 6. `EVENT_CHANGE` Decoding Walkthrough

Minimum fields every client should parse from an event:

- `UCP_F_BOOT_ID`
- `UCP_F_EVENT_SEQ`
- `UCP_F_TOPIC_ID`
- `UCP_F_RESOURCE_ID`
- `UCP_F_RESOURCE_VERSION`
- `UCP_F_ORIGIN`
- `UCP_F_TIMESTAMP_UTC_MS`
- optional `UCP_F_INSTANCE_ID`
- delta payload (typed TLV fields or `RAW_BYTES_F` wrapper)

Recommended processing order:

1. Verify `BOOT_ID` matches session boot.
2. Check `event_seq` monotonicity.
3. If gap detected:
   - if `EVENT_OVERFLOW` received -> full resync path
   - if no overflow and gap coincides with coalescing/rate-limit counters in heartbeat -> treat as benign compression
4. Apply delta to cache for `(service,resource,instance)`.
5. Advance `last_processed_event_seq`.

## 7. `EVENT_OVERFLOW` Recovery Flow

When server emits `EVENT_OVERFLOW`, payload includes:

- `boot_id`
- `last_event_seq`
- `first_available_event_seq`
- `dropped_count_estimate`
- `resync_required=true`

Client recovery sequence (recommended):

1. Pause cloud/local forwarding that assumes strong cache consistency.
2. Mark subscribed topics as dirty.
3. Re-fetch snapshots via `GET` for affected resources/topics.
4. Re-subscribe `live_only` (or `replay_then_live` from current).
5. Resume forwarding.

## 8. Stream Example (`STREAM_*`)

## 8.1 Open Rain History Range Stream (`purpose=8`)

Goal:

- request rain history records for `[start_ts, end_ts]`

`STREAM_OPEN_REQ` header:

- `msg_type=STREAM_OPEN_REQ`
- `request_id=4`

Payload TLVs (example):

- `STRM_PURPOSE = 8` (`rain_history_range`)
- `Q_DATA_TYPE = 0` (`rain_hourly_data_t`)
- `Q_RANGE_START = <unix>`
- `Q_RANGE_END = <unix>`
- `Q_LIMIT = 48`
- `STRM_CRC_MODE = end_crc32c`

`STREAM_OPEN_RESP` success returns:

- `status_code=OK`
- `stream_id`
- `STRM_DIRECTION=outbound`
- `STRM_CHUNK_SIZE`
- optional total size / item count metadata

## 8.2 `STREAM_DATA` Frames

Each `STREAM_DATA` frame carries:

- same `stream_id`
- `STRM_CHUNK_INDEX`
- `STRM_OFFSET`
- `STRM_DATA_BYTES`

For `rain_history_range`, the data payload begins with `URH2` header (see `wire-reference.md`):

- magic `URH2`
- `data_type`
- `item_size`
- `count`
- `start_ts`
- `end_ts`
- contiguous raw records (`rain_hourly_data_t` or `rain_daily_data_t`)

Client decode:

1. Reassemble by `stream_id`, `offset`, `chunk_index`.
2. Validate no overlap/gap.
3. Once complete, parse `URH2` and record array.

## 8.3 `STREAM_END`

`STREAM_END` finalizes the stream:

- `STRM_FINAL_SIZE`
- `STRM_FINAL_CRC32C` (if enabled)
- `STRM_RESULT`

Client should verify:

- total bytes received == `STRM_FINAL_SIZE`
- computed CRC matches `STRM_FINAL_CRC32C` (if present)

## 8.4 Inbound Upload Stream (`purpose=3`, staging)

Used for packs staging uploads.

Flow:

1. `STREAM_OPEN_REQ` (`purpose=3`, inbound)
2. `STREAM_OPEN_RESP` returns accepted `stream_id`
3. client sends `STREAM_DATA` chunks in order
4. client sends `STREAM_END` with final size and optional CRC
5. server validates and stores staging blob
6. client issues `packs.install_staged_*` command

If chunk order/offset is wrong:

- server aborts stream (`STREAM_ABORT`, `STREAM_STATE_ERR`)

## 9. OTA `ota.enter_recovery` 2-Step Confirmation Flow

`ota.enter_recovery` is destructive (reboots out of app), so it uses confirmation.

## 9.1 Step 1: Request without confirmation token

Client sends `CMD_REQ ota.enter_recovery` without token.

Expected response:

- `CMD_RESP`
- `status_code=CONFIRMATION_REQUIRED`
- payload contains:
  - confirmation token
  - timeout window / validity metadata
  - optional reason text/code

Client should:

- prompt operator / enforce policy
- resend command with exact token before token timeout

## 9.2 Step 2: Confirmed request

Client sends `CMD_REQ ota.enter_recovery` with `OTA_CONFIRM_TOKEN`.

Expected sequence:

1. `CMD_RESP` `OK`, with recovery parameters (`uart`, `baud`, `wait_window_ms`)
2. `EVENT_CHANGE` topic `ota.transition` (`entering_recovery`)
3. device reboots
4. app UCP session drops
5. MCUboot serial recovery takes over UART

## 9.3 After OTA Upload/Reset

After `mcumgr` session and reboot back to app:

1. reconnect UART
2. run `HELLO_REQ`
3. compare firmware version / boot IDs
4. run `ota.post_check`
5. resume subscriptions and snapshot sync

## 10. Error Handling Patterns (Client-Side)

## 10.1 Transport-Level Corruption

Symptoms:

- COBS decode failure
- CRC mismatch
- oversize decoded frame

Action:

- drop frame
- resync at next `0x00`
- keep session alive unless repeated failures indicate line issues

## 10.2 Protocol-Level Errors (`ERROR` message)

Examples:

- pre-HELLO traffic rejected
- malformed TLV
- unsupported message combination

Action:

- log full header + TLV dump
- treat as request failure or session fault depending `status_code` and context

## 10.3 Application-Level Errors (`GET/SET/CMD_RESP`)

Examples:

- `OUT_OF_RANGE`
- `BUSY`
- `CONFLICT`
- `PRECONDITION_FAILED`

Action:

- do not drop session
- surface error at API layer
- retry only when semantics permit (e.g., `BUSY`)

## 11. Minimal Host Startup Recipe (Recommended)

1. Open UART (`460800 8N1`).
2. Send `HELLO_REQ`.
3. Parse `HELLO_RESP`, cache `BOOT_ID`, `SCHEMA_VERSION`, `REGISTRY_HASH`.
4. If unknown schema hash:
   - fetch `schema.proto_info`, `schema.service_mask`, `schema.topic_mask`
   - optionally stream `schema_catalog`
5. Fetch initial snapshots (`GET`) for resources your application needs.
6. Send `SUB_REQ` for change topics (`replay_then_live` if reconnect on same boot).
7. Start keepalive (`PING`) + parser loop + request dispatcher.

## 12. Minimal Host Reconnect Recipe (Same Boot)

1. Reopen UART.
2. Send `HELLO_REQ` with last known `resume_event_seq`.
3. Confirm same `BOOT_ID`.
4. `SUB_REQ replay_then_live`.
5. If `EVENT_OVERFLOW`, do snapshot resync.

## 13. Minimal Host Reconnect Recipe (New Boot)

1. Reopen UART.
2. `HELLO_REQ`.
3. Detect changed `BOOT_ID`.
4. Invalidate cache (or mark stale).
5. Full snapshot `GET`s.
6. Fresh `SUB_REQ live_only`.

## 14. Implementation Pitfalls (Do Not Miss)

- `UART` is full-duplex, but app-level UCP v1 uses **one in-flight request** per session.
- Events may arrive between request and response; route by `msg_type` and `request_id`.
- `SET_RESP` success does not replace `EVENT_CHANGE`.
- Some resources use `RAW_BYTES_F`; decode wrapper first, raw ABI second.
- `env_history_range` records are native ABI; use `UEH2.item_size`.
- `UPK1`/`UPT1` streams are custom row encodings, not raw native structs.
- `EVENT_HEARTBEAT` may include coalescing/rate-limit counters; event sequence gaps are not always transport loss.

## 15. Cross-References

- Exact wire bytes: `docs/uart-api/wire-reference.md`
- Every method contract: `docs/uart-api/api-reference.md`
- Raw ABI and stream record layouts: `docs/uart-api/raw-payload-abi-catalog.md`
- Topics/subscription semantics: `docs/uart-api/events-and-subscriptions.md`
- OTA handoff specifics: `docs/uart-api/ota-over-uart-mcuboot.md`
