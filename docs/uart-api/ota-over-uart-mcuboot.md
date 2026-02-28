# OTA Over UART (UCP + MCUboot/mcumgr, Recovery-First)

## Decision

Firmware OTA is **not** transferred through custom UCP `SET/CMD` payloads.
UCP is used to orchestrate the transition, while image upload stays standard:

- `MCUboot` + `mcumgr/SMP` over UART

## UCP Contract (v1 target)

- `ota.preflight`
- `ota.enter_recovery`
- `ota.post_check`

## Recovery-First Flow (target)

1. Peer sends `ota.preflight`
2. Peer sends `ota.enter_recovery`
3. nRF emits `ota.transition` event
4. nRF reboots into MCUboot serial recovery on `uart0`
5. Peer performs `mcumgr` upload/test/reset
6. Peer reconnects via UCP and runs `ota.post_check`

## Current Firmware Implementation Status

Implemented now:

- `ota.preflight` response (recovery-first support + pending state + MCUboot confirm/swap/rollback hints)
- `ota.post_check` response (firmware version/uptime + MCUboot confirm/swap/rollback hints)
- `ota.enter_recovery` command path in app:
  - 2-step confirmation token (`CONFIRMATION_REQUIRED` first, replay with `OTA_CONFIRM` token)
  - sends `CMD_RESP`
  - emits `ota.transition` (`EVENT_CHANGE`)
  - schedules reboot
  - sets MCUboot boot mode (`bootmode_set(BOOT_MODE_TYPE_BOOTLOADER)`) in reboot worker
  - emits OTA failure/post-check status events for better observability

Not implemented yet:

- hardware-validated end-to-end handoff + mcumgr recovery session on target board
- external policy orchestration beyond app auto-confirm behavior (current app confirms image on successful boot)

## Important Note

The repository now includes MCUboot sysbuild config/overlay changes targeting:

- serial recovery on `uart0`
- boot-mode retention entry (`zephyr,boot-mode` on `gpregret1`)

`--sysbuild` build validation now passes in this environment. One local workaround was needed during validation:

- set `PYTHONPATH` to the Zephyr venv `site-packages` so the interpreter selected by the mcuboot sub-build can import `jsonschema`.

Hardware flash/recovery testing on the target board is still required.
