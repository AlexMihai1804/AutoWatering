# Master Backlog (Living)

Last updated: 2026-02-23
Owner: firmware (nRF52840 / AutoWatering)

Purpose:
- single place with all known remaining work (protocol, OTA, size, validation, firmware tasks)
- tracks current size optimization effort and next actions

## Current Status Snapshot

- UCP UART v1 core is implemented (`GET/SET/CMD/SUB/EVENT`, replay/overflow, core `STREAM_*`)
- OTA `ota.enter_recovery` app-side handoff is implemented (`bootmode_set` + reboot)
- MCUboot sysbuild is configured for serial recovery on `uart0` + retention boot mode
- `--sysbuild` build validates (with local `PYTHONPATH` workaround for `jsonschema`)
- App flash usage is now back under control after size work; recent app builds are ~84.3% flash, ~92.0% RAM
- UCP coverage checklist maintained in `docs/uart-api/read-set-notify-checklist.md`
- Exhaustive BLE/custom/pack parity audit now tracked in the same checklist (38-char inventory + non-BLE API surfaces)
- Source-based firmware module inventory audit is now included in the same checklist (not BLE-only); hidden gap `auto_calc interval` was added as UCP `device.auto_calc_control`

## Size Optimization Track (active)

### Baseline (before size-tuning pass)

- Compiler optimization flag observed in `compile_commands.json`: `-Og`
- Zephyr config state:
  - `CONFIG_DEBUG_OPTIMIZATIONS=y`
  - `CONFIG_SIZE_OPTIMIZATIONS` not set
- App footprint (recent build):
  - FLASH ~`736,912 B / 728 KB` (~98.85%)
  - RAM ~`235,156 B / 256 KB` (~89.70%)

### Immediate actions

- [x] Switch app build from `-Og` to `-Os` (`CONFIG_SIZE_OPTIMIZATIONS=y`)
- [x] Rebuild and record flash/RAM delta
- [x] Re-check actual compiler flag in `compile_commands.json`
- [x] Produce top flash contributors (`rom_report` + symbol review)
- [x] Reduce logging compile-time footprint (`CONFIG_LOG_MAX_LEVEL=2`)
- [x] Disable `newlib` float `scanf` support (`CONFIG_NEWLIB_LIBC_FLOAT_SCANF=n`)

### Pass 1 result (`-Og` -> `-Os`)

- Confirmed compiler flag in `build/compile_commands.json`: `-Os`
- New app footprint:
  - FLASH `659,284 B / 728 KB` (~88.44%)
  - RAM `234,836 B / 256 KB` (~89.58%)
- Delta vs baseline:
  - FLASH: about `-77,628 B` (major win)
  - RAM: about `-320 B` (small change)

### Pass 2 result (logging cap + `float scanf` off)

- Changes applied:
  - `CONFIG_LOG_DEFAULT_LEVEL=2`
  - `CONFIG_LOG_MAX_LEVEL=2` (global compile-time cap; trims modules explicitly registered at `DBG/INF`)
  - `CONFIG_NEWLIB_LIBC_FLOAT_SCANF=n`
- New app footprint:
  - FLASH `613,620 B / 728 KB` (~82.31%)
  - RAM `234,836 B / 256 KB` (~89.58%)
- Delta vs Pass 1:
  - FLASH: about `-45,664 B` (major win)
  - RAM: ~`0 B`
- Total delta vs original `-Og` baseline:
  - FLASH: about `-123,292 B`
  - RAM: about `-320 B`
- Notes:
  - `CONFIG_CBPRINTF_COMPLETE=n` was tested, but current Zephyr dependency set still forces `CONFIG_CBPRINTF_COMPLETE=y` in `.config`.

### Next size candidates (likely high impact)

- [x] Reduce logging footprint (`CONFIG_LOG_DEFAULT_LEVEL`, `CONFIG_LOG_MAX_LEVEL`)
- [ ] Revisit `CONFIG_CBPRINTF_COMPLETE` / float logging usage (currently forced `y`)
- [ ] Audit `CONFIG_ASSERT=y` for production profile toggle
- [ ] Evaluate `CONFIG_DEBUG=y` split dev vs release profile
- [ ] Review/trim mcumgr app transports (especially `CONFIG_MCUMGR_TRANSPORT_UART` if recovery-first only)
- [ ] Review BLE mcumgr buffer/workqueue sizes (`CONFIG_MCUMGR_TRANSPORT_WORKQUEUE_STACK_SIZE`, `CONFIG_MCUMGR_TRANSPORT_NETBUF_SIZE`)
- [ ] Split huge `bt_irrigation_service.c` and gate unused handlers/features
- [ ] Add Kconfig feature toggles for optional subsystems (rain/temp/hydraulic/interval/master valve)
- [ ] Review mbedTLS/newlib features for current runtime needs
- [ ] Review USB legacy stack requirements vs production profile
- [ ] Evaluate LTO (`CONFIG_LTO=y`) in a separate branch/test pass

## UCP UART v1 Remaining Work (nRF side)

### Protocol / transport

- [x] Inbound `STREAM_*` uploads (staging/core protocol path)
- [x] Streaming timeouts, cleanup telemetry, and stronger state diagnostics
- [x] Core `diagnostics` + `schema_discovery` resources for protocol/stream capabilities
- [x] `peer_power` protocol service (`GET/SET/CMD`) + topic events
- [x] Per-purpose stream schemas / richer metadata (domain-specific `STREAM_OPEN_RESP` metadata + paged `schema_catalog` stream)
- [x] Event coalescing/rate-limit tuning under heavy churn (topic-based live push rate-limit/coalescing with diagnostics counters)

### Resource coverage (major)

- [x] `channel` resources (state/config GET/SET) + push events (poll-based for non-UCP changes)
- [x] `schedule` resources (GET/SET) + push events (poll-based for non-UCP changes)
- [x] `task` status + core control commands (`stop/pause/resume/cancel_all`) + `task.current` events
- [x] `flow`, `rain`, `environment` resources (core read + config writes where applicable + push events)
- [x] `history` query/export over `STREAM_*` (watering history page stream + reset cmds; richer exports still open)
- [x] `packs` transfer/list/install/delete/status (stats + list streams + staging install cmds + delete cmds)
- [x] `peer_power` hardware-backed EN control integration (wired in DTS: `ESP32 EN -> P0.13`, `DS3231 isw-gpios -> P1.02`)
- [x] `diagnostics` domain-level resources beyond protocol/stream counters (`diag.domain_status`)
- [x] `schema_discovery` field/schema metadata generation (resource descriptors + stream purpose descriptors + paged schema catalog + best-effort field descriptor)
- [x] Add missing UCP parity domains discovered in exhaustive audit: `system_config`, `statistics`, `auto_calc_status`, `hydraulic_status`, `custom_soil`, `soil_moisture`, `interval_mode`, `onboarding/config/reset`, `compensation/rain_integration`, `rain/env history streams`, `bulk_snapshot`

### Events / sync

- [x] Hooks for scheduler-originated changes (`origin=scheduler`) (scheduler alarms + system status path)
- [x] Hooks for sensor/alarm-originated changes (`origin=sensor`) (sensor polling events + direct alarm hook)
- [x] Hooks for task/channel/valve state changes (task/channel polling + direct valve hook)
- [x] Broader coverage so BLE/local changes consistently mirror into UCP events (polling + direct hooks for RTC/TZ/alarm/valve/system-status)
- [x] Event-engine hardening pass (mutex on event seq/replay ring; alarm dedup; schema field/type consistency fixes; valve publish validation order)

### OTA orchestration

- [ ] Hardware validate full flow: `ota.enter_recovery` -> MCUboot serial recovery on `uart0` -> `mcumgr`
- [x] Add optional 2-step confirmation token for `ota.enter_recovery`
- [x] Define/apply post-OTA confirm policy orchestration (app auto-confirm policy surfaced in `ota.preflight` / `ota.post_check`)
- [x] Add explicit UCP status/event reporting for OTA failure / rollback observations (`ota.transition` + `ota.post_check`)

### Testing

- [ ] UART framing fault tests (CRC/cobs/oversized/resync)
- [ ] Replay/overflow/resync tests with controlled event floods
- [ ] Stream open/data/end/abort tests (including cancel mid-stream)
- [ ] BLE + UCP concurrency tests (`last write wins`, `origin`, `resource_version`)
- [ ] OTA handoff integration test on hardware

## Firmware Architecture / Safety / Product Tasks (existing open items)

Imported from `FIRMWARE_TASKS.md` open items:

- [ ] T7 Split `bt_irrigation_service.c` (very high leverage for maintainability + size)
- [ ] T21 Configure hardware watchdog in DT overlay
- [ ] T22 Add Kconfig toggles for major subsystems
- [ ] T23 Add valve feedback (GPIO readback)
- [ ] T24 Add battery/power ADC monitoring

## Build / Tooling / Validation

- [ ] Make sysbuild Python dependency handling deterministic (avoid manual `PYTHONPATH` workaround)
- [ ] Add scripted size report (`size`, map summary, top symbols) to `tools/build_all.py` or separate script
- [ ] Add release profile config fragment (size-oriented)
- [ ] Add CI build profile(s): debug + size/release

## Nice-to-have / follow-up

- [ ] Generate UCP docs/constants from a schema registry file
- [ ] Add UART protocol examples/tests for host integration
- [ ] Add wire-level test vectors (COBS + CRC32C) for regression
