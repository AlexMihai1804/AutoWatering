# UCP UART v1 Wire Reference (Exact On-Wire Format)

This document is the wire-level reference for the nRF52840 UART API (`UCP-UART v1`).
It is written against the current firmware implementation in this repo (`src/ucp_protocol.h`, `src/ucp_uart.c`, `src/ucp_tlv.*`).

It describes:

- exact frame bytes
- header field offsets and meanings
- TLV encoding
- message type and status code values
- session / events / stream message payloads
- binary stream payload headers

## 1. Transport and Framing

- Physical link: `uart0` (TTL UART)
- Default UART profile: `460800 8N1`
- Framing: `COBS(frame_bytes) + 0x00 delimiter`
- Integrity: `CRC32C` over decoded `header + payload` (CRC bytes are appended after payload, before COBS encode)
- Endianness: little-endian for all multi-byte integers
- Maximum decoded frame size: `540 bytes` (`24 header + 512 payload + 4 CRC`)

### 1.1 Frame Lifecycle (TX)

The sender builds a decoded frame:

1. `UcpFrameHeaderV1` (24 bytes)
2. `payload` (`payload_len`)
3. `crc32c` LE (`u32`) over bytes `header || payload`

Then:

4. COBS-encodes the whole decoded frame (`header + payload + crc32c`)
5. Appends delimiter byte `0x00`

### 1.2 Frame Lifecycle (RX)

The receiver accumulates bytes until `0x00`, then:

1. COBS decodes
2. validates minimum size (`>= 24 + 4`)
3. validates `proto_version == 1`, `header_len == 24`
4. validates `payload_len <= 512`
5. validates exact decoded length (`24 + payload_len + 4`)
6. validates CRC32C
7. dispatches by `msg_type`

If any validation fails, the frame is dropped and the parser resynchronizes at the next `0x00`.

## 2. Decoded Frame Layout (Bytes)

Decoded frame bytes (before COBS encoding):

```text
+--------------------------+----------------------+------------------+
| UCP header (24 bytes)    | payload (0..512)     | CRC32C (4 bytes) |
+--------------------------+----------------------+------------------+
```

## 3. Header Layout (`UcpFrameHeaderV1`, 24 bytes)

Source: `src/ucp_protocol.h` (`struct ucp_frame_header_v1`)

### 3.1 Byte Offsets

| Offset | Size | Field | Type | Encoding |
|---:|---:|---|---|---|
| 0 | 1 | `proto_version` | `u8` | literal (`1`) |
| 1 | 1 | `msg_type` | `u8` | enum (`UCP_MSG_*`) |
| 2 | 1 | `flags` | `u8` | bitfield (currently TX=`0`, RX ignores semantics) |
| 3 | 1 | `header_len` | `u8` | literal (`24`) |
| 4 | 2 | `service_id` | `u16` | LE |
| 6 | 2 | `method_or_topic_id` | `u16` | LE |
| 8 | 2 | `status_code` | `u16` | LE |
| 10 | 2 | `payload_len` | `u16` | LE |
| 12 | 4 | `request_id` | `u32` | LE |
| 16 | 4 | `seq_tx` | `u32` | LE |
| 20 | 2 | `stream_id` | `u16` | LE |
| 22 | 2 | `reserved` | `u16` | LE |

### 3.2 Header Field Semantics

- `service_id`
  - for `GET/SET/CMD` requests/responses: service domain (`device`, `channel`, etc.)
  - for `EVENT_CHANGE`: service of the changed resource
  - for session / stream control messages: often `0`
- `method_or_topic_id`
  - for `GET/SET/CMD`: resource ID or command ID
  - for `EVENT_CHANGE`: topic ID
  - for session / stream control: often `0`
- `status_code`
  - `0` in most requests
  - response status in `*_RESP`, `STREAM_*` responses, `ERROR`
- `request_id`
  - copied from request into correlated response
  - `0` for async events (`EVENT_*`)
- `seq_tx`
  - monotonic counter per sender direction (starts at `1`)
- `stream_id` (header)
  - currently not used by implementation for stream routing (stream IDs are carried in TLV payloads)
- `flags`
  - field exists in header, but current implementation sends `0x00` and does not interpret flag bits on RX

## 4. Message Types (`msg_type`, exact values)

Source: `src/ucp_protocol.h`

| Name | Hex |
|---|---:|
| `HELLO_REQ` | `0x01` |
| `HELLO_RESP` | `0x02` |
| `PING_REQ` | `0x03` |
| `PING_RESP` | `0x04` |
| `GET_REQ` | `0x10` |
| `GET_RESP` | `0x11` |
| `SET_REQ` | `0x12` |
| `SET_RESP` | `0x13` |
| `CMD_REQ` | `0x14` |
| `CMD_RESP` | `0x15` |
| `SUB_REQ` | `0x20` |
| `SUB_RESP` | `0x21` |
| `UNSUB_REQ` | `0x22` |
| `UNSUB_RESP` | `0x23` |
| `EVENT_CHANGE` | `0x30` |
| `EVENT_OVERFLOW` | `0x31` |
| `EVENT_HEARTBEAT` | `0x32` |
| `STREAM_OPEN_REQ` | `0x40` |
| `STREAM_OPEN_RESP` | `0x41` |
| `STREAM_DATA` | `0x42` |
| `STREAM_END` | `0x43` |
| `STREAM_ABORT` | `0x44` |
| `ERROR` | `0x7F` |

## 5. Status Codes (`status_code`, exact values)

Source: `src/ucp_protocol.h`

| Name | Hex | Meaning |
|---|---:|---|
| `OK` | `0x0000` | Success |
| `INVALID_ARG` | `0x0001` | Bad type/value/required field missing |
| `OUT_OF_RANGE` | `0x0002` | Value type correct, range invalid |
| `NOT_FOUND` | `0x0003` | Resource/item/stream not found |
| `UNSUPPORTED` | `0x0004` | Msg/resource/command unsupported |
| `BUSY` | `0x0005` | Operation cannot run now |
| `LOCKED` | `0x0006` | Inhibited/locked state |
| `CONFLICT` | `0x0007` | Mismatch / state conflict |
| `TIMEOUT` | `0x0008` | Timeout |
| `CHECKSUM_FAIL` | `0x0009` | CRC mismatch |
| `STREAM_STATE_ERR` | `0x000A` | Stream state/chunk order invalid |
| `PRECONDITION_FAILED` | `0x000B` | Precondition not met |
| `CONFIRMATION_REQD` | `0x000C` | 2-step confirmation token required |
| `RATE_LIMITED` | `0x000D` | Rate limiting applied |
| `INTERNAL` | `0x00FE` | Internal error |
| `HW_FAULT` | `0x00FF` | Hardware/driver fault |

## 6. TLV Encoding (Exact Bytes)

Source: `src/ucp_tlv.h`, `src/ucp_tlv.c`

Each payload is a sequence of TLV elements:

```text
+----------+------+---------+--------------------+
| field_id  | type | len_le  | value[len]         |
| (1 byte)  | (1)  | (2)     | (0..65535 bytes)   |
+----------+------+---------+--------------------+
```

- `field_id`: `u8`
- `type`: `u8` (`UCP_TLV_*`)
- `len`: `u16` little-endian
- `value`: raw bytes

### 6.1 TLV Primitive Types (exact numeric values)

Source: `src/ucp_protocol.h`

| Name | Hex | Value bytes |
|---|---:|---|
| `BOOL` | `0x01` | 1 byte (`0` or non-zero; TX uses `0/1`) |
| `U8` | `0x02` | 1 byte |
| `U16` | `0x03` | 2 bytes LE |
| `U32` | `0x04` | 4 bytes LE |
| `U64` | `0x05` | 8 bytes LE |
| `I32` | `0x06` | 4 bytes LE (two's complement) |
| `I64` | `0x07` | defined enum, not currently used in v1 TX |
| `F32` | `0x08` | 4 bytes IEEE-754 binary32, little-endian (`memcpy` of native `float`) |
| `BYTES` | `0x09` | raw bytes |
| `UTF8` | `0x0A` | raw UTF-8 bytes (not NUL-terminated) |

### 6.2 TLV Parsing Rules

- Unknown fields are ignored by most handlers unless a required field is missing.
- Repeated fields: handler behavior is per-handler (usually last occurrence parsed wins because parser walks in order).
- Type mismatch causes `INVALID_ARG`.

## 7. Common TLV Fields (cross-message)

Source: `src/ucp_protocol.h` (`enum ucp_common_field_id`)

| ID | Name | Type (typical) | Description |
|---:|---|---|---|
| 1 | `UCP_F_VERSION` | `U8` | Protocol/schema version |
| 2 | `UCP_F_SESSION_ID` | `U32` | Session identifier |
| 3 | `UCP_F_BOOT_ID` | `U64` | Boot identifier |
| 4 | `UCP_F_FW_VERSION` | `UTF8` | Firmware version string |
| 5 | `UCP_F_CAPABILITIES` | `U32` | Capability bitmask |
| 6 | `UCP_F_EVENT_SEQ` | `U64` | Last/current event sequence |
| 7 | `UCP_F_UART_BAUD` | `U32` | UART baud |
| 8 | `UCP_F_MAX_PAYLOAD` | `U16` | Max payload bytes |
| 10 | `UCP_F_UPTIME_MS` | `U32` | Uptime |
| 11 | `UCP_F_TIME_UTC_S` | `U32`/`U64` | UTC timestamp (resource-specific) |
| 12 | `UCP_F_RTC_AVAILABLE` | `BOOL` | RTC availability |
| 13 | `UCP_F_RESOURCE_VERSION` | `U32` | Per-resource version |
| 14 | `UCP_F_TIMESTAMP_UTC_MS` | `U64` | UTC timestamp in ms |
| 15 | `UCP_F_OK_BOOL` | `BOOL` | Command/SET generic success flag |
| 16 | `UCP_F_TOPIC_MASK` | `U32` | Subscribe topic bitmask |
| 17 | `UCP_F_SUB_MODE` | `U8` | Subscription mode |
| 18 | `UCP_F_SINCE_EVENT_SEQ` | `U64` | Replay cursor |
| 19 | `UCP_F_ACCEPTED_TOPIC_MASK` | `U32` | Active topic mask after subscribe/unsubscribe |
| 20 | `UCP_F_FIRST_AVAILABLE_EVENT_SEQ` | `U64` | Replay buffer oldest seq |
| 21 | `UCP_F_LAST_EVENT_SEQ` | `U64` | Replay buffer newest seq |
| 22 | `UCP_F_RESYNC_REQUIRED` | `BOOL` | Resync required after overflow |
| 23 | `UCP_F_TOPIC_ID` | `U16` | Topic ID |
| 24 | `UCP_F_SERVICE_ID` | `U16` | Service ID |
| 25 | `UCP_F_RESOURCE_ID` | `U16` | Resource ID |
| 26 | `UCP_F_INSTANCE_ID` | `U32` | Resource instance (`channel_id`, `0xFFFFFFFF`, etc.) |
| 27 | `UCP_F_ORIGIN` | `U8` | Change origin enum |
| 28 | `UCP_F_DELTA_TLV` | `BYTES` | Nested TLV blob (event delta payload) |
| 29 | `UCP_F_DROPPED_COUNT` | `U64` | Estimated dropped events |
| 30 | `UCP_F_REPLAY_DEPTH` | `U16` | Replay ring depth |

## 8. Session and Keepalive Messages

## 8.1 `HELLO_REQ`

- `msg_type = 0x01`
- Payload: currently ignored by implementation (can be empty)
- Requirement: must be the first valid request before any `GET/SET/CMD/SUB/...`

## 8.2 `HELLO_RESP`

- `msg_type = 0x02`
- `service_id = 0x0001` (`session`)
- `status_code = OK`
- Payload TLVs (current implementation):
  - `UCP_F_VERSION` (`U8`) = `1`
  - `UCP_F_SESSION_ID` (`U32`)
  - `UCP_F_BOOT_ID` (`U64`)
  - `UCP_F_FW_VERSION` (`UTF8`)
  - `UCP_F_CAPABILITIES` (`U32`)
  - `UCP_F_EVENT_SEQ` (`U64`) = last emitted event seq (`g_evt_seq-1`)
  - `UCP_F_UART_BAUD` (`U32`) = `460800`
  - `UCP_F_MAX_PAYLOAD` (`U16`) = `512`
  - `UCP_F_REPLAY_DEPTH` (`U16`) = `64`

### 8.2.1 `UCP_F_CAPABILITIES` Bit Definitions (current firmware)

Bit positions from `src/ucp_uart.c` (`UCP_CAP_*`):

| Bit | Mask | Capability |
|---:|---:|---|
| 0 | `0x00000001` | `GET` |
| 1 | `0x00000002` | `SET` |
| 2 | `0x00000004` | `CMD` |
| 3 | `0x00000008` | `SUB_EVENTS` |
| 4 | `0x00000010` | `EVENT_REPLAY` |
| 5 | `0x00000020` | `STREAM_OUT` |
| 6 | `0x00000040` | `STREAM_IN` |
| 7 | `0x00000080` | `OTA_HANDOFF` |
| 8 | `0x00000100` | `SCHEMA_DISCOVERY` |
| 9 | `0x00000200` | `DIAGNOSTICS` |
| 10 | `0x00000400` | `PEER_POWER` |

## 8.3 `PING_REQ` / `PING_RESP`

- `PING_REQ` payload: ignored (may be empty)
- `PING_RESP` payload: empty
- `PING_RESP.status_code = OK`

## 8.4 Pre-HELLO Rejection

Any message other than `HELLO_REQ` before session start is rejected with:

- `msg_type = ERROR`
- `status_code = PRECONDITION_FAILED`
- payload UTF-8 message `"HELLO required"`

## 9. Subscription and Events

## 9.1 Topic Mask Encoding (`SUB_REQ` / `UNSUB_REQ`)

Topic mask is a dense `u32` bitmask:

- topic `1` => bit `0`
- topic `2` => bit `1`
- ...
- topic `32` => bit `31`

## 9.2 `SUB_REQ`

- `msg_type = 0x20`
- Payload TLVs:
  - required: `UCP_F_TOPIC_MASK` (`U32`)
  - optional: `UCP_F_SUB_MODE` (`U8`)
    - `0` = `live_only`
    - `1` = `replay_then_live`
  - optional: `UCP_F_SINCE_EVENT_SEQ` (`U64`)

Current behavior:

- `g_sub_mask` is replaced by the requested mask (not additive merge).
- invalid/zero mask => `SUB_RESP INVALID_ARG`
- unsupported `sub_mode` => `SUB_RESP UNSUPPORTED`

## 9.3 `SUB_RESP`

- `msg_type = 0x21`
- `service_id = session`
- payload:
  - `UCP_F_ACCEPTED_TOPIC_MASK` (`U32`)
  - `UCP_F_SUB_MODE` (`U8`)

If `replay_then_live` is requested:

- replay is sent after `SUB_RESP`
- if requested cursor is older than replay buffer start, `EVENT_OVERFLOW` is sent instead

## 9.4 `UNSUB_REQ`

Payload TLVs:

- optional `UCP_F_TOPIC_MASK` (`U32`)

Behavior:

- if mask present: clears only those bits
- if mask absent: clears all subscriptions

## 9.5 `UNSUB_RESP`

Payload TLVs:

- `UCP_F_ACCEPTED_TOPIC_MASK` (`U32`) => remaining active mask

## 9.6 `EVENT_CHANGE` (Exact Payload)

- `msg_type = 0x30`
- `request_id = 0`
- Header:
  - `service_id = changed resource service`
  - `method_or_topic_id = topic_id`
  - `status_code = OK`
- Payload TLVs (exact order currently emitted):
  1. `UCP_F_BOOT_ID` (`U64`)
  2. `UCP_F_EVENT_SEQ` (`U64`)
  3. `UCP_F_TOPIC_ID` (`U16`)
  4. `UCP_F_SERVICE_ID` (`U16`)
  5. `UCP_F_RESOURCE_ID` (`U16`)
  6. `UCP_F_INSTANCE_ID` (`U32`)
  7. `UCP_F_RESOURCE_VERSION` (`U32`)
  8. `UCP_F_ORIGIN` (`U8`)
  9. `UCP_F_TIMESTAMP_UTC_MS` (`U64`)
  10. `UCP_F_DELTA_TLV` (`BYTES`) => nested TLV blob specific to the resource/topic

`UCP_F_DELTA_TLV` contains a complete TLV-encoded delta fragment (TLVs inside bytes).

## 9.7 `EVENT_OVERFLOW` (Exact Payload)

- `msg_type = 0x31`
- payload TLVs:
  - `UCP_F_BOOT_ID` (`U64`)
  - `UCP_F_LAST_EVENT_SEQ` (`U64`)
  - `UCP_F_FIRST_AVAILABLE_EVENT_SEQ` (`U64`)
  - `UCP_F_DROPPED_COUNT` (`U64`)
  - `UCP_F_RESYNC_REQUIRED` (`BOOL`, value `1`)

## 9.8 `EVENT_HEARTBEAT` (Exact Payload)

- `msg_type = 0x32`
- sent only when session active, subscriptions active, and no recent events (`10s`)
- payload TLVs:
  - `UCP_F_BOOT_ID` (`U64`)
  - `UCP_F_EVENT_SEQ` (`U64`) => last emitted event seq
  - `UCP_F_TIMESTAMP_UTC_MS` (`U64`)
  - `DIAG_EVT_COALESCED` (`U32`) optional hint
  - `DIAG_EVT_RATE_LIMITED` (`U32`) optional hint

## 10. Generic `GET/SET/CMD` Payload Rules

The exact per-resource/per-command contracts are documented in `api-reference.md`.

Wire-level common rules:

- `GET_REQ`: payload may be empty or contain selector/query TLVs
- `GET_RESP`: `status_code` indicates success/failure; payload contains resource fields (typed TLV or `RAW_BYTES_F` wrapper)
- `SET_REQ`: payload is a TLV delta (only changed fields)
- `SET_RESP`: on success usually returns:
  - `UCP_F_OK_BOOL` (`BOOL`)
  - `UCP_F_INSTANCE_ID` (`U32`) (generic helper path; some special setters omit this field)
  - `UCP_F_RESOURCE_VERSION` (`U32`)
  - `UCP_F_TIMESTAMP_UTC_MS` (`U64`)
- `CMD_REQ`: payload depends on command
- `CMD_RESP`: payload depends on command, but usually includes `UCP_F_OK_BOOL`

## 11. `ERROR` Message

- `msg_type = 0x7F`
- `status_code` = error code
- payload (current implementation):
  - field `1`, `UTF8`: error text message

Used for protocol-level errors such as:

- `HELLO required`
- unsupported message type

## 12. Stream Transport (`STREAM_*`)

Stream transport is message-based TLV over the same framed UART.

Important implementation detail:

- The logical stream ID is transported in TLV field `STRM_STREAM_ID` (not header `stream_id`)

### 12.1 Stream TLV Fields (`STRM_*`)

Local field IDs (from `src/ucp_uart.c`):

| ID | Name | Typical type |
|---:|---|---|
| 1 | `STRM_PURPOSE` | `U8` |
| 2 | `STRM_STREAM_ID` | `U16` |
| 3 | `STRM_CHUNK_SIZE` | `U16` |
| 4 | `STRM_TOTAL_SIZE` | `U32` |
| 5 | `STRM_DIRECTION` | `U8` |
| 6 | `STRM_CHUNK_INDEX` | `U16` |
| 7 | `STRM_OFFSET` | `U32` |
| 8 | `STRM_DATA` | `BYTES` |
| 9 | `STRM_RESULT` | `U8` |
| 10 | `STRM_FINAL_CRC32C` | `U32` |

### 12.1.1 Stream Direction Values

| Value | Name |
|---:|---|
| 1 | `OUTBOUND` |
| 2 | `INBOUND` |

### 12.1.2 Stream Result Values

| Value | Name |
|---:|---|
| 0 | `OK` |
| 1 | `ABORTED` |
| 2 | `CRC_FAIL` |
| 3 | `TIMEOUT` |

### 12.2 `STREAM_OPEN_REQ`

Payload TLVs:

- required: `STRM_PURPOSE` (`U8`)
- optional: `STRM_DIRECTION` (`U8`)
  - if omitted, firmware infers:
    - `upload_staging` => inbound
    - all others => outbound
- optional: `STRM_CHUNK_SIZE` (`U16`) preferred chunk size
- optional: `STRM_TOTAL_SIZE` (`U32`) total size hint (inbound only)
- optional query TLVs depending on purpose (`Q_*`, see below)

### 12.3 `STREAM_OPEN_RESP`

On success (`status_code = OK`), payload includes:

- `STRM_STREAM_ID` (`U16`)
- `STRM_PURPOSE` (`U8`)
- `STRM_DIRECTION` (`U8`)
- `STRM_CHUNK_SIZE` (`U16`) actual chunk size
- `STRM_TOTAL_SIZE` (`U32`) stream total (or capacity for inbound)

Outbound streams additionally include metadata:

- `SCHEMA_STREAM_SCHEMA` (`U8`) schema/content code
- `SCHEMA_CONTENT_TYPE` (`U8`) `1=text`, `2=binary`
- `SCHEMA_TOTAL_LEN` (`U32`) logical total length (paged streams may expose catalog total)
- `SCHEMA_PAGE_OFFSET` (`U16`) echoed page offset (if applicable)
- echoed query TLVs (`Q_*`) when present
- `SCHEMA_ITEM_COUNT` (`U16`) for paged/range responses when known

### 12.4 `STREAM_DATA`

Outbound (nRF -> peer) payload exact fields:

- `STRM_STREAM_ID` (`U16`)
- `STRM_CHUNK_INDEX` (`U16`)
- `STRM_OFFSET` (`U32`)
- `STRM_DATA` (`BYTES`)

Inbound (peer -> nRF) request must contain the same four fields.

Inbound validation rules:

- active inbound stream required
- stream ID must match
- `chunk_index` must equal expected next chunk
- `offset` must equal current received byte count
- `STRM_DATA` length must be `1..chunk_size`
- cumulative size must not exceed `UCP_STREAM_BUF_MAX`

Failure returns `STREAM_ABORT` with error status.

### 12.5 `STREAM_END`

Inbound `STREAM_END` request payload:

- `STRM_STREAM_ID` (`U16`) required
- `STRM_TOTAL_SIZE` (`U32`) optional but recommended
- `STRM_FINAL_CRC32C` (`U32`) optional but recommended

nRF validates:

- stream exists and ID matches
- total size (if provided) matches bytes received
- expected total hint (if set during open) matches bytes received
- CRC (if provided) matches computed CRC32C

`STREAM_END` response payload:

- `STRM_STREAM_ID` (`U16`)
- `STRM_TOTAL_SIZE` (`U32`) actual received size
- `STRM_FINAL_CRC32C` (`U32`) computed CRC
- `STRM_RESULT` (`U8`)
  - `0` `OK`
  - `1` `ABORTED`
  - `2` `CRC_FAIL`
  - `3` `TIMEOUT`

On success, inbound data becomes the current staging blob (`packs` install commands consume it).

### 12.6 `STREAM_ABORT`

`STREAM_ABORT` request payload:

- optional `STRM_STREAM_ID` (`U16`)

Behavior:

- if no stream active => `STREAM_STATE_ERR`
- if stream ID omitted and both inbound+outbound active => `CONFLICT`
- otherwise aborts selected stream and returns:
  - `STRM_STREAM_ID`
  - `STRM_RESULT = ABORTED`

### 12.7 Async Stream Timeout Abort

If a stream is idle for `UCP_STREAM_IDLE_TIMEOUT_MS` (8s), firmware may asynchronously send `STREAM_ABORT` with:

- `status_code = TIMEOUT`
- payload:
  - `STRM_STREAM_ID`
  - `STRM_RESULT = TIMEOUT`

## 13. Stream Purpose IDs and Query Fields

### 13.1 Stream Purposes (`STRM_PURPOSE_*`, exact values)

| Value | Purpose | Direction |
|---:|---|---|
| 1 | `diag_snapshot` | outbound |
| 2 | `schema_text` | outbound |
| 3 | `upload_staging` | inbound |
| 4 | `history_page` | outbound |
| 5 | `packs_list` | outbound |
| 6 | `packs_plants_list` | outbound |
| 7 | `schema_catalog` | outbound |
| 8 | `rain_history_range` | outbound |
| 9 | `env_history_range` | outbound |
| 10 | `db_catalog` | outbound |
| 11 | `bulk_snapshot` | outbound |
| 12 | `water_history_range` | outbound |

### 13.2 Query Fields (`Q_*`, exact IDs)

| ID | Name | Type |
|---:|---|---|
| 100 | `Q_CHANNEL_ID` | `U8` |
| 101 | `Q_PAGE_INDEX` | `U16` |
| 102 | `Q_OFFSET` | `U16` |
| 103 | `Q_LIMIT` | `U16` |
| 104 | `Q_PACK_ID` | `U16` |
| 105 | `Q_DELETE_PLANTS` | `BOOL` |
| 106 | `Q_PLANT_ID` | `U16` |
| 107 | `Q_DATA_TYPE` | `U8` |
| 108 | `Q_START_TS` | `U32` |
| 109 | `Q_END_TS` | `U32` |
| 110 | `Q_DB_KIND` | `U8` |

### 13.3 Purpose-Specific Query Requirements (current implementation)

- `history_page` (`4`)
  - required: `Q_CHANNEL_ID`, `Q_PAGE_INDEX`
- `rain_history_range` (`8`)
  - required: `Q_DATA_TYPE`
  - optional: `Q_START_TS`, `Q_END_TS`, `Q_LIMIT`
  - `Q_DATA_TYPE`:
    - `0` = hourly
    - `1` = daily
- `env_history_range` (`9`)
  - required: `Q_DATA_TYPE`
  - optional: `Q_START_TS`, `Q_END_TS`, `Q_LIMIT`
  - `Q_DATA_TYPE`:
    - `0` = hourly
    - `1` = daily
    - `2` = monthly
- `water_history_range` (`12`)
  - required: `Q_DATA_TYPE`, `Q_CHANNEL_ID`
  - optional: `Q_START_TS`, `Q_END_TS`, `Q_LIMIT`
  - `Q_DATA_TYPE`:
    - `0` = detailed events
    - `1` = daily stats
    - `2` = monthly stats
    - `3` = annual stats
- `db_catalog` (`10`)
  - required: `Q_DB_KIND`
  - optional: `Q_OFFSET`, `Q_LIMIT`
  - `Q_DB_KIND`:
    - `1` = plants
    - `2` = soils
    - `3` = irrigation methods
- `schema_catalog` (`7`)
  - optional: `Q_OFFSET` (byte offset)
- `upload_staging` (`3`)
  - optional: `STRM_TOTAL_SIZE` (expected total)

## 14. Binary Stream Payload Headers (Exact Bytes)

These are the bytes inside `STRM_DATA` chunks / `STREAM` content, not the UCP frame header.

## 14.1 `history_page` (`UHP1`) Purpose 4

Produced by `stream_prepare_history_page()`.

Header layout:

| Offset | Size | Field | Encoding |
|---:|---:|---|---|
| 0 | 4 | magic `"UHP1"` | ASCII |
| 4 | 1 | `channel_id` | `u8` |
| 5 | 2 | `page_index` | `u16` LE |
| 7 | 2 | `count` | `u16` LE |

Then `count` records, each 12 bytes:

| Offset (record) | Size | Field |
|---:|---:|---|
| 0 | 4 | event timestamp (`u32` LE) |
| 4 | 1 | `dt_delta` |
| 5 | 1 | flags packed |
| 6 | 2 | target ml (`u16` LE) |
| 8 | 2 | actual ml (`u16` LE) |
| 10 | 2 | avg flow ml/s (`u16` LE) |

## 14.2 `packs_list` (`UPK1`) Purpose 5

Header (10 bytes):

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | magic `"UPK1"` |
| 4 | 2 | `offset` (`u16` LE) |
| 6 | 2 | `count` (`u16` LE) |
| 8 | 2 | reserved (`0`) |

Each record is 38 bytes:

| Offset | Size | Field |
|---:|---:|---|
| 0 | 2 | `pack_id` |
| 2 | 2 | `version` |
| 4 | 2 | `plant_count` |
| 6 | 32 | `name[PACK_NAME_MAX_LEN]` raw bytes |

## 14.3 `packs_plants_list` (`UPT1`) Purpose 6

Header (10 bytes):

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | magic `"UPT1"` |
| 4 | 2 | `offset` (`u16` LE) |
| 6 | 2 | `count` (`u16` LE) |
| 8 | 2 | reserved (`0`) |

Each record is 55 bytes:

| Offset | Size | Field |
|---:|---:|---|
| 0 | 2 | `plant_id` |
| 2 | 2 | `pack_id` |
| 4 | 2 | `version` |
| 6 | 1 | `source` |
| 7 | 48 | `name[PACK_COMMON_NAME_MAX_LEN]` raw bytes |

## 14.4 `rain_history_range` (`URH2`) Purpose 8

Header (14 bytes):

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | magic `"URH2"` |
| 4 | 1 | `data_type` (`0=hourly,1=daily`) |
| 5 | 1 | `item_size` (record size in bytes) |
| 6 | 2 | `count` (`u16` LE) |
| 8 | 4 | `start_ts` (`u32` LE) |
| 12 | 4 | `end_ts` (`u32` LE) |

Then `count * item_size` raw records of type:

- `rain_hourly_data_t` when `data_type=0`
- `rain_daily_data_t` when `data_type=1`

See `raw-payload-abi-catalog.md`.

## 14.5 `env_history_range` (`UEH2`) Purpose 9

Header is same shape as `URH2`, magic `"UEH2"`.

`data_type`:

- `0` hourly (`hourly_history_entry_t`)
- `1` daily (`daily_history_entry_t`)
- `2` monthly (`monthly_history_entry_t`)

## 14.6 `water_history_range` (`UWH2`) Purpose 12

Header (20 bytes):

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | magic `"UWH2"` |
| 4 | 1 | `data_type` |
| 5 | 1 | `item_size` |
| 6 | 2 | `count` (`u16` LE) |
| 8 | 1 | `channel_id` |
| 9 | 1 | reserved |
| 10 | 4 | `start_ts` (`u32` LE) |
| 14 | 4 | `end_ts` (`u32` LE) |

`data_type` record types:

- `0` => `history_event_t`
- `1` => `daily_stats_t`
- `2` => `monthly_stats_t`
- `3` => `annual_stats_t`

## 14.7 `db_catalog` Text Format Purpose 10

Text stream (`UTF-8` bytes) beginning with a header line:

```text
udb1 kind=<db_kind> offset=<offset> total=<total_items>\n
```

Then one line per item (format depends on `db_kind`):

- plants: `<index>|<common_name>|<scientific_name>|m=<method_id>\n`
- soils: `<index>|<soil_type>|<texture>|awc=<awc>\n`
- irrigation methods: `<index>|<name>|eff=<pct>\n`

## 15. Implementation Quirks / Important Client Notes

- `HELLO_REQ` payload is ignored in current firmware. Still send `HELLO_REQ` before anything else.
- Header `flags` exists but is currently always zero on TX; do not rely on flags.
- Header `stream_id` is not used; use TLV `STRM_STREAM_ID`.
- `SUB_REQ` replaces the subscription mask instead of union-adding.
- `EVENT_CHANGE.UCP_F_DELTA_TLV` contains nested TLV bytes, not a decoded TLV tree.
- `OTA_CONFIRM` field ID (`9`) is context-dependent:
  - `ota.preflight` / `ota.post_check`: boolean `confirmed` state
  - `ota.enter_recovery` 2-step flow: `u32` confirmation token
- `F32` values are raw 4-byte IEEE-754 bytes (`little-endian` on nRF52840) copied via `memcpy`.

## 16. Where to Read Per-Method Contracts

This file documents the transport and message wire format.

For exact `GET` / `SET` / `CMD` / `STREAM` API methods (request/response TLVs, selectors, side effects, notify topics):

- `docs/uart-api/api-reference.md`
- `docs/uart-api/raw-payload-abi-catalog.md` (for `RAW_BYTES_F` payload ABIs)
- `docs/uart-api/examples-and-traces.md` (worked end-to-end flows)
- `docs/uart-api/client-implementation-guide.md` (host parser/session/replay/stream patterns)
