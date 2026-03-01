# UCP UART v1 Documentation (nRF52840 API)

This folder documents the UART API (`UCP-UART v1`) exposed by the nRF52840 firmware.

Use these documents in this order:

1. `wire-reference.md`
   Exact wire format: frame bytes, header layout, TLV encoding, message types, status codes, event and stream framing.
2. `api-reference.md`
   Exhaustive API reference: every implemented `GET`/`SET`/`CMD`/`SUB`/`STREAM` surface, request and response TLVs, side effects, notify topics.
3. `raw-payload-abi-catalog.md`
   Binary ABI catalog for resources that use `RAW_BYTES_F` wrappers (legacy BLE-compatible payload structs and stream record structs).
4. `examples-and-traces.md`
   Worked flows (`HELLO`, `GET`, `SET+EVENT`, `SUB replay`, `STREAM_*`, OTA 2-step handoff) showing how messages are used end-to-end.
5. `client-implementation-guide.md`
   Host-side implementation guidance: parser/session/request dispatcher/cache/replay/resync/stream/OTA integration patterns.

Supporting docs:

- `protocol-specification.md` (overview)
- `service-index.md` (service inventory and quick lookup)
- `events-and-subscriptions.md`
- `streaming.md`
- `ota-over-uart-mcuboot.md`
- `esp32-firmware-handoff-checklist.md` (practical implementation order + DoD for ESP32 UCP client layer)
- `read-set-notify-checklist.md` (coverage audit / parity checklist)

Recommended reading path by role:

- `Protocol implementer (driver/parser)`:
  - `wire-reference.md`
  - `examples-and-traces.md`
  - `client-implementation-guide.md`
- `Application/API integrator`:
  - `api-reference.md`
  - `raw-payload-abi-catalog.md`
  - `events-and-subscriptions.md`
- `OTA integrator`:
  - `ota-over-uart-mcuboot.md`
  - `examples-and-traces.md` (OTA section)
  - `client-implementation-guide.md` (OTA orchestration section)
- `ESP32 firmware implementer (handoff pack)`:
  - `esp32-firmware-handoff-checklist.md`
  - `examples-and-traces.md`
  - `client-implementation-guide.md`

Self-contained note:

- The combination of:
  - `wire-reference.md`
  - `api-reference.md`
  - `raw-payload-abi-catalog.md`
  - `examples-and-traces.md`
  is intended to be sufficient for implementing a client without reading firmware source code.

Source of truth:

- `src/ucp_protocol.h`
- `src/ucp_uart.c`
- `src/ucp_tlv.h`
- `src/ucp_tlv.c`
