# UCP UART v1 Client Implementation Guide (Host Side)

This guide explains how to implement a robust UCP UART v1 client from scratch.

It is intentionally independent of any specific peer platform (ESP32, Linux, etc.).

## 1. What You Need to Implement

Minimum production-ready client modules:

1. `uart_io`
   - open/close UART
   - non-blocking read/write
   - reconnect handling
2. `frame_codec`
   - COBS encode/decode
   - CRC32C verify/generate
   - frame delimiter scanning (`0x00`)
3. `ucp_parser`
   - parse header
   - parse TLVs
   - validate lengths/types
4. `session_manager`
   - `HELLO`, `PING`
   - keepalive timeout
   - `BOOT_ID` / schema cache
5. `request_dispatcher`
   - one in-flight request rule
   - request timeout handling
   - response correlation by `request_id`
6. `event_engine`
   - `EVENT_CHANGE` apply path
   - replay/resync handling
   - overflow recovery
7. `stream_manager`
   - outbound stream reassembly
   - inbound upload streaming
   - CRC/size validation
8. `resource_cache` (recommended)
   - indexed by `(service_id, resource_id, instance_id)`
   - stores `resource_version`, `timestamp`, raw/typed decoded value

## 2. Runtime State Model (Recommended)

Use a small explicit state machine:

- `DISCONNECTED`
- `UART_OPEN`
- `HELLO_SENT`
- `ACTIVE` (requests/events/streams allowed)
- `RECOVERY_EXPECTED` (after `ota.enter_recovery` confirm accepted)
- `BOOTLOADER_MODE` (optional, if same process also drives mcumgr)

State transitions:

1. `DISCONNECTED -> UART_OPEN`
2. `UART_OPEN -> HELLO_SENT` (send `HELLO_REQ`)
3. `HELLO_SENT -> ACTIVE` (valid `HELLO_RESP`)
4. `ACTIVE -> RECOVERY_EXPECTED` (`ota.enter_recovery` accepted)
5. `RECOVERY_EXPECTED -> DISCONNECTED` (UART app session drops on reboot)
6. reconnect back to `UART_OPEN`

## 3. Read Loop Architecture (Recommended)

Do not parse directly in the UART interrupt callback (or equivalent platform callback).

Recommended pipeline:

1. UART RX pushes bytes into ring buffer.
2. Parser thread/task scans for `0x00` delimiters.
3. For each delimited encoded frame:
   - COBS decode
   - validate min length (`header + crc`)
   - CRC32C verify
   - parse header
   - parse TLVs
4. Dispatch by `msg_type`.

Benefits:

- controlled memory usage
- deterministic error handling
- easier tracing/logging

## 4. Frame Validation Checklist (Must Pass Before Dispatch)

For every received frame:

1. `COBS decode` succeeded
2. decoded length `>= 28` (`24-byte header + 4-byte CRC`)
3. `header_len == 24`
4. `proto_version == 1`
5. `payload_len` matches actual bytes present
6. CRC32C matches
7. `msg_type` known (or safely ignorable if policy supports future extension)

If any check fails:

- drop frame
- resync at next delimiter (`0x00`)
- increment transport error counters

## 5. Request/Response Dispatcher (One In-Flight Rule)

UCP v1 allows only one request in flight per session.

Recommended implementation:

- maintain `current_request` struct:
  - `request_id`
  - `msg_type`
  - `service_id`
  - `resource/cmd`
  - `deadline_ms`
  - completion callback / promise
- reject or queue new requests while one is active
- match responses by:
  - `msg_type` family (`GET_RESP`, `SET_RESP`, `CMD_RESP`, `SUB_RESP`, `STREAM_OPEN_RESP`, etc.)
  - `request_id`

Important:

- `EVENT_*` may arrive while waiting for a response
- stream messages may arrive asynchronously after a `STREAM_OPEN_RESP`

## 6. Session Management

## 6.1 `HELLO` Startup

On connection:

1. send `HELLO_REQ`
2. wait for `HELLO_RESP`
3. cache:
   - `SESSION_ID`
   - `BOOT_ID`
   - `SCHEMA_VERSION`
   - `REGISTRY_HASH`
   - `EVENT_SEQ_CURRENT`
   - `EVENT_REPLAY_DEPTH`
   - `CAPABILITIES`

Fail connection if:

- `HELLO_RESP` not received within timeout
- protocol version unsupported

## 6.2 Keepalive (`PING`)

Recommended defaults:

- send `PING_REQ` every `5s` when idle
- connection timeout `15s` without valid traffic

Any valid frame can refresh the receive liveness timer, not only `PING_RESP`.

## 6.3 Reconnect / Boot Detection

Compare `BOOT_ID` after every reconnect:

- same `BOOT_ID`: replay path may be possible
- different `BOOT_ID`: full resync required

## 7. Event Processing and Cache Coherency

## 7.1 Event Application Order

Process `EVENT_CHANGE` in this order:

1. validate session/boot
2. validate monotonic `event_seq`
3. decode identity:
   - `topic_id`
   - `resource_id`
   - `instance_id` (if present)
4. decode payload:
   - typed TLV delta, or
   - `RAW_BYTES_F` wrapper + raw ABI decode
5. apply delta to cache
6. update `resource_version`
7. publish to upper layers

## 7.2 Event Sequence Gaps

Possible causes:

- replay buffer overflow (explicit `EVENT_OVERFLOW`)
- event coalescing/rate-limiting (heartbeat counters hint this)
- connection loss / parser drops

Recommended handling:

- if `EVENT_OVERFLOW`: force resync
- if uncertain: conservatively resync affected resources

## 7.3 Resync Strategy (Recommended)

Maintain a topic -> resource snapshot plan in the client.

When resync required:

1. stop assuming cache coherence
2. fetch snapshots for critical resources first:
   - `device.system_status`
   - `task.status`
   - active channels (`channel.state`)
3. fetch configuration/resources required by application
4. re-subscribe (`live_only` or `replay_then_live`)
5. resume dependent logic

## 8. Subscription Strategy (Recommended)

Use subscriptions selectively; do not subscribe to every topic unless needed.

Typical minimum operational set:

- `system.status`
- `system.alarm`
- `task.current`
- `channel.state`
- `channel.config` (if config UI/cache is active)
- `schedule.config` (if scheduler UI/cache is active)
- `flow.live`, `rain.live`, `env.live` (if telemetry UI is active)
- `ota.transition`
- `peer.power`

For reboot-safe clients:

- store `last_processed_event_seq` persistently (optional)
- store `BOOT_ID` with it
- on reconnect, use replay only if `BOOT_ID` matches

## 9. Resource Cache Design (Recommended)

Cache key:

- `service_id`
- `resource_id`
- `instance_id` (`0xFFFFFFFF` or absent for global)

Cache value:

- decoded value (typed or raw bytes)
- `resource_version`
- `timestamp_utc_ms`
- `last_origin`
- `last_event_seq`
- decode status/schema hash used

Why cache both typed and raw:

- typed resources are easy for app logic
- raw resources may need exact pass-through or later re-interpretation

## 10. Raw-Bytes Resources (`RAW_BYTES_F`) Handling

For resources marked raw:

1. Parse UCP wrapper TLVs:
   - `RAW_BYTES_F`
   - optional `INSTANCE_ID`
   - `RESOURCE_VERSION`
   - `TIMESTAMP`
2. Validate `RAW_BYTES_F len`
3. Decode with ABI from `raw-payload-abi-catalog.md`

Do not:

- assume packed layout for resources documented as native/non-packed
- ignore `RAW_BYTES_F len`
- decode pack list streams as native structs (they are custom wire row formats)

## 11. Stream Manager Design

Maintain per-stream state:

- `stream_id`
- direction (`in` / `out`)
- purpose
- `expected_next_offset`
- `next_chunk_index`
- `buffer`
- `crc_mode`
- `opened_at`
- `last_activity`

UCP v1 practical limits:

- max 1 outbound stream active
- max 1 inbound stream active

## 11.1 Outbound Stream Reassembly

On `STREAM_DATA`:

1. verify `stream_id`
2. verify `chunk_index` and `offset`
3. append bytes
4. update `expected_next_offset`

On `STREAM_END`:

1. verify final size
2. verify CRC (if enabled)
3. decode stream payload by purpose and content header (`UHP1`, `URH2`, etc.)

## 11.2 Inbound Upload Streams

For `purpose=3` staging uploads:

1. send `STREAM_OPEN_REQ`
2. wait `STREAM_OPEN_RESP`
3. send ordered `STREAM_DATA` chunks
4. send `STREAM_END`
5. wait for command phase (`packs.install_staged_*`)

If server sends `STREAM_ABORT`:

- discard local transfer state
- inspect `status_code`
- decide retry/reopen

## 12. OTA Orchestration Integration (App-Side UCP + Bootloader Handoff)

`ota.enter_recovery` is an app-side orchestration command, not firmware upload transport.

Client responsibilities:

1. call `ota.preflight`
2. run 2-step `ota.enter_recovery` confirmation flow
3. stop UCP request/event expectations once reboot is accepted
4. switch tooling to MCUboot `mcumgr` serial session on same UART
5. after boot back to app, re-run `HELLO` + resync + `ota.post_check`

Important:

- UCP and MCUboot SMP are not multiplexed simultaneously in app mode on same UART

## 13. Error Handling Policy (Recommended)

Classify errors into three layers:

1. `transport` (COBS/CRC/framing)
   - drop frame, keep session unless repeated
2. `protocol` (`ERROR` msg / malformed semantics)
   - log and fail current operation; may reset session if parser state uncertain
3. `application` (`*_RESP status_code != OK`)
   - return error to caller; no session reset

Retry policy suggestions:

- `BUSY`: retry with backoff
- `TIMEOUT`: retry idempotent `GET`, cautious on `SET/CMD`
- `CONFLICT`: refetch (`GET`) and reapply user intent
- `PRECONDITION_FAILED`: surface to operator or sequence proper prerequisite command

## 14. Logging / Tracing (Strongly Recommended)

At least in debug builds, log:

- RX/TX frame header summaries
- request lifecycle (`send`, `resp`, timeout)
- `event_seq` progression and gaps
- stream open/data/end summaries
- resync triggers and results
- OTA transition sequence

Do not log by default:

- raw payload bytes containing sensitive deployment-specific data (if any)
- huge stream chunks (log metadata instead)

## 15. Compatibility / Schema Strategy

At startup cache:

- `proto_version`
- `schema_version`
- `registry_hash`

When `registry_hash` changes:

- invalidate cached generated decoders or strict assumptions
- re-read `schema.proto_info` / `schema.resource_desc` / `schema.field_desc`
- if client supports multiple firmware builds, select decoder profile by `(schema_version, registry_hash)`

## 16. Production Checklist (Host Side)

- UART open/close + reconnect works
- COBS + CRC32C implementation tested with corrupt frames
- parser rejects malformed lengths safely
- one in-flight request enforcement implemented
- `HELLO`/`PING` timeouts implemented
- `EVENT_CHANGE` cache apply implemented
- `EVENT_OVERFLOW` resync implemented
- `STREAM_*` reassembly + CRC verification implemented
- `RAW_BYTES_F` wrappers decoded safely (length checked)
- OTA 2-step confirmation + reboot handoff flow implemented
- schema hash/version checks implemented

## 17. Where to Read Exact Contracts

- `docs/uart-api/wire-reference.md` (exact bytes, IDs, message types)
- `docs/uart-api/api-reference.md` (all methods/resources/commands)
- `docs/uart-api/raw-payload-abi-catalog.md` (raw ABI and stream record layouts)
- `docs/uart-api/examples-and-traces.md` (worked flows)
