# BLE Parity Mapping (UCP UART v1)

## Goal

UCP UART v1 is intended to reach near-functional parity with existing BLE control/config/status capabilities.

## Current Increment Coverage

Implemented in UCP:

- `GET` device/system summary (new semantic resource)
- `GET/SET` timezone configuration (maps to BLE timezone behavior conceptually)
- OTA orchestration command stubs (`preflight`, `post_check`, `enter_recovery` placeholder)
- async change event delivery model (`EVENT_CHANGE`) for UCP-applied timezone changes

## Not Yet Covered (next increments)

- channel configuration
- schedule configuration
- task queue/current task control
- flow/rain/environment telemetry and histories
- compensation, onboarding, reset controller
- packs and large transfer flows
- RTC read/write resource
- replay/overflow event model

## Mapping Strategy

- UCP uses a semantic resource/command model (not strict BLE characteristic mirroring)
- Internal implementation may reuse existing BLE handlers/validation paths where practical
- `origin` metadata in UCP events is intended to represent BLE-originated changes too (hooks pending)
