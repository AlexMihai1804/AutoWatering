# UCP UART v1 Protocol Specification (nRF52840-side)

## Scope

UCP UART v1 defines the custom bidirectional UART protocol exposed by the nRF52840 firmware.
It supports:

- `GET_REQ/GET_RESP` for reading state/configuration
- `SET_REQ/SET_RESP` for modifying settings
- `CMD_REQ/CMD_RESP` for action commands
- `SUB_REQ/UNSUB_REQ` + async `EVENT_CHANGE` for change notifications
- `HELLO` + `PING` session management

This spec intentionally describes only the nRF/UART contract and peer capabilities.

## Framing

- Transport: `uart0` (TTL UART), default `460800 8N1`
- Framing: `COBS` encoded frame + `0x00` delimiter
- Integrity: `CRC32C` (Castagnoli) over decoded `header + payload`
- Endianness: little-endian for all multi-byte values
- Max payload v1: `512 bytes`

Decoded frame layout:

1. `UcpFrameHeaderV1` (24 bytes)
2. `payload` (`payload_len`)
3. `crc32c` (4 bytes LE)

## Message Types (public)

- `HELLO_REQ`, `HELLO_RESP`
- `PING_REQ`, `PING_RESP`
- `GET_REQ`, `GET_RESP`
- `SET_REQ`, `SET_RESP`
- `CMD_REQ`, `CMD_RESP`
- `SUB_REQ`, `SUB_RESP`
- `UNSUB_REQ`, `UNSUB_RESP`
- `EVENT_CHANGE`
- `EVENT_HEARTBEAT`
- `EVENT_OVERFLOW`
- `STREAM_OPEN_REQ`, `STREAM_OPEN_RESP`, `STREAM_DATA`, `STREAM_END`, `STREAM_ABORT`
- `ERROR`

## Session Rules

- Peer must send `HELLO_REQ` first.
- Any message before `HELLO_REQ` may be rejected with protocol error.
- nRF tracks a single active session.
- `PING_REQ` is used for keepalive.
- Session timeout resets subscriptions.
- `EVENT_HEARTBEAT` may include optional coalescing/rate-limit counters (implementation hint for live-only consumers).

## Current Firmware Implementation Status (this repo)

Implemented now:

- Framing (COBS + CRC32C)
- `HELLO`, `PING`
- `GET`, `SET`, `CMD`, `SUB`, `UNSUB`
- `EVENT_CHANGE`, `EVENT_HEARTBEAT`, `EVENT_OVERFLOW`
- Full nRF-side v1 API surface for current firmware domains:
  - `device/system` (`system_status`, `alarm_status`, `system_config`, `statistics`, `auto_calc_status`, `bulk_snapshot`)
  - `channel`, `schedule`, `task`
  - `flow` (`flow.status`, `hydraulic_status`, `calibration_status`)
  - `rain`, `environment` (including `environment.sensor_config`)
  - `history` (watering/rain/environment storage + reset cmds + range/page streams)
  - `rtc_tz`
  - `compensation`
  - `onboarding_reset`
  - `packs`
  - `ota` orchestration (`preflight`, `enter_recovery`, `post_check`)
  - `peer_power`
  - `diagnostics`
  - `schema_discovery`
- Real action command surfaces include:
  - `task.*` queue/current control
  - `channel.*` direct valve/master/queue commands
  - `history.*` reset commands
  - `device.*` stats/alarm/error control commands
  - `flow.hydraulic_*` commands
  - `packs.*` install/delete commands
- Replay buffer (64 events) + `SUB_REQ` `replay_then_live`
- `STREAM_*` bidirectional transport:
  - outbound diagnostic/schema streams
  - outbound schema catalog stream (`purpose=7`, paged text)
  - outbound history range/page streams (`purpose=4/8/9/12`)
  - outbound DB catalog / bulk snapshot streams (`purpose=10/11`)
  - inbound staging upload stream (`STREAM_DATA`/`STREAM_END`)
  - timeout cleanup via `STREAM_ABORT`
- OTA orchestration:
  - `ota.preflight`, `ota.post_check`
  - `ota.enter_recovery` app-side handoff (`bootmode+reboot`) with 2-step confirmation token
- `peer_power` service (`GET`/`SET`/`CMD`) + `peer.power` topic events
- `diagnostics` service (`GET`) for UCP counters + stream status + domain versions/poll timestamps
- `schema_discovery` extended resources (proto info, service/topic masks, stream caps, resource/field/stream-purpose descriptors)
- direct UCP publish hooks used for additional alarm/valve/system-status paths (plus polling-based deltas)

Remaining major items:

- hardware end-to-end validation on target (`ota.enter_recovery` -> MCUboot serial recovery -> `mcumgr`)
- automated protocol integration tests / fault-injection tests
- stream/replay/concurrency stress validation on hardware

Coverage checklist (current firmware):

- `docs/uart-api/read-set-notify-checklist.md`

## Implementation-Grade References

Use these for exact client implementation details (byte layout, TLV fields, request/response payloads, stream formats):

- `docs/uart-api/wire-reference.md`
- `docs/uart-api/api-reference.md`
- `docs/uart-api/raw-payload-abi-catalog.md`
- `docs/uart-api/examples-and-traces.md`
- `docs/uart-api/client-implementation-guide.md`
