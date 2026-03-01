# ESP32 Firmware Handoff Checklist (UCP UART v1 Client)

This document is the practical handoff checklist for the team implementing the ESP32-side firmware that talks to the nRF52840 over `UCP-UART v1`.

It assumes the nRF-side UCP firmware/API is available and the ESP32 side needs to implement the client/bridge logic.

## 1. Scope (What This Checklist Covers)

This checklist covers ESP32 firmware implementation of:

- UCP UART client transport and parser
- session management (`HELLO`, `PING`)
- `GET` / `SET` / `CMD`
- subscriptions + async event processing (`EVENT_CHANGE`, `EVENT_OVERFLOW`, `EVENT_HEARTBEAT`)
- stream transport (`STREAM_*`)
- OTA orchestration handoff (`ota.*` + switch to MCUboot/mcumgr on same UART)

This checklist does **not** prescribe:

- local HTTPS API design
- cloud backend contract
- app/UI behavior

## 2. Required Input Docs (Read First)

Implementers should read these in order:

1. `docs/uart-api/wire-reference.md`
2. `docs/uart-api/api-reference.md`
3. `docs/uart-api/raw-payload-abi-catalog.md`
4. `docs/uart-api/examples-and-traces.md`
5. `docs/uart-api/client-implementation-guide.md`
6. `docs/uart-api/ota-over-uart-mcuboot.md`

## 3. Implementation Order (Recommended)

Follow this order to reduce integration risk.

## 3.1 Phase 1: UART Transport + Framing

Build:

- UART driver wrapper (`460800 8N1`)
- RX ring buffer
- delimiter scanner (`0x00`)
- COBS decode/encode
- CRC32C verify/generate
- decoded frame parser (`UcpFrameHeaderV1` + payload)

Definition of done:

- Can send and receive valid framed packets in a test harness
- Corrupt frames are dropped and parser resyncs correctly
- Oversized/malformed frames do not crash parser state

## 3.2 Phase 2: Session (`HELLO` / `PING`)

Build:

- `HELLO_REQ` builder
- `HELLO_RESP` parser + cache of:
  - `session_id`
  - `boot_id`
  - `schema_version`
  - `registry_hash`
  - `event_seq_current`
  - `event_replay_depth`
  - `capabilities`
- keepalive timers (`PING_REQ` / `PING_RESP`)
- reconnect detection and state reset

Definition of done:

- ESP32 can establish a stable session after boot
- reconnect is detected correctly
- `BOOT_ID` change is detected and surfaced

## 3.3 Phase 3: Request Dispatcher (`GET` / `SET` / `CMD`)

Build:

- one in-flight request rule (queue or reject concurrent requests)
- request timeout handling
- response correlation by `request_id`
- typed TLV encode/decode helpers
- `RAW_BYTES_F` wrapper decode path

Minimum methods to validate first:

- `GET device.system_status`
- `GET rtc_tz.timezone_config`
- `SET rtc_tz.timezone_config`
- `GET task.status`
- `CMD ota.preflight`

Definition of done:

- `GET`, `SET`, `CMD` all work end-to-end
- status codes are surfaced correctly (`OK`, `OUT_OF_RANGE`, `BUSY`, etc.)
- `SET` errors do not corrupt client cache

## 3.4 Phase 4: Events / Subscriptions / Replay

Build:

- `SUB_REQ` / `UNSUB_REQ`
- topic mask handling
- `EVENT_CHANGE` parser
- `EVENT_HEARTBEAT` parser
- `EVENT_OVERFLOW` recovery path
- local cache apply (`resource_version`, `timestamp`, `origin`, `event_seq`)

Recommended initial topics:

- `system.status`
- `alarm`
- `task.current`
- `channel.state`
- `ota.transition`
- `peer.power`

Definition of done:

- ESP32 receives and applies async deltas
- `event_seq` is tracked monotonically
- replay-then-live works on reconnect (same `BOOT_ID`)
- overflow triggers snapshot resync path

## 3.5 Phase 5: Stream Transport (`STREAM_*`)

Build:

- `STREAM_OPEN_REQ/RESP`
- outbound stream reassembly (`STREAM_DATA`, `STREAM_END`)
- inbound upload stream sender (`purpose=3` staging)
- `STREAM_ABORT` handling
- final CRC/size verification

Minimum streams to validate:

- `purpose=8` rain history range
- `purpose=9` environment history range
- `purpose=7` schema_catalog
- `purpose=5/6` packs list streams

Definition of done:

- stream open/data/end is robust to chunking
- offset/chunk mismatch is detected
- final CRC verification is enforced when provided

## 3.6 Phase 6: OTA Handoff (`ota.*` + mcumgr switch)

Build:

- `CMD ota.preflight`
- `CMD ota.enter_recovery` 2-step confirmation-token flow
- UART session shutdown / mode switch logic
- MCUboot `mcumgr` serial session start on same UART after reboot
- post-OTA return path (`HELLO` + `ota.post_check`)

Definition of done:

- confirmed `ota.enter_recovery` causes controlled transition
- ESP32 can switch from UCP session to `mcumgr` session
- after reboot to app, ESP32 re-establishes UCP and validates update state

## 4. Client Architecture Checklist (ESP32)

Implement these modules explicitly:

- `ucp_uart_io`
- `ucp_frame_codec` (COBS + CRC32C)
- `ucp_parser`
- `ucp_tlv_codec`
- `ucp_session`
- `ucp_request_dispatcher`
- `ucp_event_engine`
- `ucp_stream_manager`
- `ucp_resource_cache`
- `ucp_ota_orchestrator`

Recommended cross-cutting utilities:

- structured logging / trace events
- binary frame hexdump (debug builds only)
- monotonic timers + deadlines
- persistent small state store (`last_boot_id`, `last_event_seq`, schema hash)

## 5. Data Handling Checklist (Must Support)

## 5.1 Typed TLV Resources

- parse unknown optional TLVs safely (ignore, do not fail)
- preserve signed vs unsigned types correctly
- parse nested TLV deltas (`UCP_F_DELTA_TLV`)

## 5.2 `RAW_BYTES_F` Resources

- always validate `RAW_BYTES_F len`
- decode wrapper metadata (`resource_version`, `timestamp`, `instance_id`)
- decode raw bytes using `docs/uart-api/raw-payload-abi-catalog.md`

## 5.3 Stream Payloads

- parse stream content headers (`UHP1`, `URH2`, `UEH2`, `UWH2`, `UPK1`, `UPT1`, `udb1`)
- treat `item_size` in range streams as authoritative
- treat `UPK1`/`UPT1` as custom row encoding (not native structs)

## 6. Cache/Sync Checklist (Strongly Recommended)

Resource cache key:

- `service_id`
- `resource_id`
- `instance_id` (or global)

Cache metadata:

- `resource_version`
- `timestamp_utc_ms`
- `origin`
- `last_event_seq`
- decode format/version used

Required sync behaviors:

- full snapshot sync on new `BOOT_ID`
- replay-then-live on same `BOOT_ID`
- snapshot resync on `EVENT_OVERFLOW`

## 7. Error Handling Checklist

Transport layer:

- COBS decode failure -> drop frame + resync
- CRC mismatch -> drop frame + resync
- malformed header/payload length -> drop frame + resync

Protocol layer:

- unexpected `msg_type` in current state -> log + ignore/fail current op
- pre-HELLO rejection -> restart session

Application layer:

- non-OK `status_code` in `*_RESP` -> return explicit error to caller
- no optimistic cache mutation on failed `SET`/`CMD`

## 8. OTA-Specific Checklist

- handle `CONFIRMATION_REQUIRED` response for `ota.enter_recovery`
- store and reuse confirmation token within validity window
- stop normal UCP request traffic after accepted `enter_recovery`
- hand over UART to mcumgr logic
- re-open UCP after app returns
- run `ota.post_check`

## 9. Test Checklist (ESP32-Side Integration)

## 9.1 Parser / Transport Tests

- valid frame round-trip
- COBS corruption
- CRC corruption
- truncated frame
- oversized frame
- back-to-back frames in one UART read
- fragmented frame across many reads

## 9.2 Session / Request Tests

- `HELLO` success
- `PING` keepalive timeout/recovery
- one in-flight enforcement
- request timeout path
- response with wrong `request_id`

## 9.3 Event / Replay Tests

- `SUB_REQ` live-only
- `SUB_REQ` replay-then-live
- monotonic `event_seq`
- `EVENT_OVERFLOW` resync path
- coalesced/rate-limited event handling (heartbeat hints)

## 9.4 Stream Tests

- `STREAM_OPEN/RESP` success
- chunk reassembly by offset/index
- final CRC mismatch handling
- `STREAM_ABORT` handling
- inbound staging upload for packs

## 9.5 OTA Flow Tests

- `ota.preflight`
- `ota.enter_recovery` token flow
- UCP disconnect on reboot
- `mcumgr` serial session on same UART
- return to app and `ota.post_check`

## 10. Definition of Done (ESP32 UCP Client Layer)

The ESP32 UCP client layer is considered done when:

1. It can establish and maintain a session (`HELLO`/`PING`).
2. It supports `GET`, `SET`, `CMD`, `SUB`, `UNSUB`, `EVENT_*`, `STREAM_*`.
3. It can decode both typed and `RAW_BYTES_F` resources.
4. It handles replay/overflow resync correctly.
5. It performs OTA orchestration handoff (`ota.*` -> `mcumgr`) correctly.
6. It survives malformed/corrupted UART frames without parser lockup.
7. It logs enough metadata to debug integration issues.

## 11. Common Pitfalls (Read Before Coding)

- Assuming `SET_RESP` is enough and ignoring `EVENT_CHANGE`
- Treating pack list streams as native struct `memcpy` blobs
- Ignoring `BOOT_ID` changes on reconnect
- Hardcoding native struct sizes instead of validating `len` / `item_size`
- Sending concurrent requests (UCP v1 expects one in-flight)
- Forgetting to switch protocol owner on OTA (`UCP` app mode vs `mcumgr` bootloader mode)

## 12. Handoff Summary for ESP32 Team

If you implement in this order:

1. framing/parser
2. `HELLO`/`PING`
3. `GET/SET/CMD`
4. `SUB/EVENT` + replay/resync
5. `STREAM_*`
6. OTA handoff

you can start integration early and de-risk the hard parts incrementally.

Primary references remain:

- `docs/uart-api/wire-reference.md`
- `docs/uart-api/api-reference.md`
- `docs/uart-api/raw-payload-abi-catalog.md`
- `docs/uart-api/examples-and-traces.md`
- `docs/uart-api/client-implementation-guide.md`
