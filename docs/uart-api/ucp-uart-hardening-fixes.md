# UCP UART nRF52840 Hardening Fixes (Exhaustive)

Last updated: 2026-02-24
Owner: nRF firmware (UCP UART server)
Scope: fixes/hardening/performance/correctness/validation for existing UCP UART implementation (not new product features)

## Current status snapshot

- `HELLO / SUB / PING` path is functional.
- UART RX on nRF is interrupt-driven and stable at `460800`.
- `SUB_REQ` is additive (`g_sub_mask |= mask`).
- `broad polling` fallback is disabled (`broad polling=0`) for stability.
- Recent `LOCKUP` issue was mitigated by preventing deferred-rescan backlog accumulation before `HELLO+SUB`.
- Remaining work is mostly hardening, deterministic notify behavior, and test validation.

## Completed in stabilization passes (already done)

- [x] `uart0` speed set explicitly to `460800` in DTS (`boards/arduino_nano_33_ble.overlay`).
- [x] UCP RX switched from polling to interrupt-driven (`uart_irq_*` + ring buffer).
- [x] `SUB_REQ` changed to additive subscriptions.
- [x] Broad notify polling disabled by default to avoid transport starvation (`broad polling=0`).
- [x] Deferred rescan requests ignored before active session + subscriptions (prevents boot-time backlog lockup).
- [x] Session timeout path clears session state and now resets UCP parser/ring buffer state.
- [x] `uart_configure()` `-ENOTSUP` message clarified (runtime reconfig unsupported; DTS speed authoritative).
- [x] UCP transport thread can now wake immediately from UART IRQ and deferred-rescan requests (`k_sem` wakeup), instead of only periodic sleep polling.

## P0 (Must fix next)

- [~] **Topic-aware deferred rescan (no full `maybe_domain_events()` on any rescan)**
  - Problem: deferred rescan currently still triggers monolithic `maybe_domain_events()`.
  - Risk: long blocking slices -> delayed `PING_RESP`, request timeouts, possible watchdog/lockup regressions.
  - Progress: deferred path now scopes topic checks using `g_notify_rescan_mask & g_sub_mask`, processes one topic-bit at a time, and is additionally gated to run only after a post-`HELLO` grace period and only when RX has been quiet briefly (reduces interference with `SUB/GET` bursts from ESP32). Added dedicated deferred handlers for `ONBOARDING_STATUS`, `CHANNEL_CFG_STATUS`, `AUTO_CALC_STATUS`, `COMP_STATUS`, `SYSTEM_CONFIG`, and `BULK_SNAPSHOT` so these topics no longer invoke the full monolithic `maybe_domain_events()` path; `CHANNEL_CFG_STATUS` / `AUTO_CALC_STATUS` / `COMP_STATUS` rescans are sliced in small steps with requeue, and `ONBOARDING_STATUS` deferred processing is now split across slices (`status` and `system_overview`) with requeue to reduce single-slice latency spikes. Optimized `CHANNEL_CFG_STATUS` builders by removing duplicate completeness validation, adding a deferred-rescan fast builder that uses in-memory `channel->config_status` (no full `channel_get_config_status()` reassessment), and replacing deferred-path `config_status_get_reset_log()` NVS reads with cached reset metadata populated by full builders. Deferred `ONBOARDING_STATUS` now debounces `system_overview` recomputation (lower rate than raw onboarding flags) and uses a fast in-memory `system_overview` builder (no `config_status_get_system_overview()`/NVS sweep) to reduce config-flag storm cost.
  - Remaining: split `maybe_domain_events()` into per-domain/per-topic functions so a single topic no longer walks all channels/system/sensor branches.
  - Files: `src/ucp_uart.c`

- [~] **Remove slow `pack_storage_get_stats()` from UCP notify/control timing path**
  - Problem: observed `pack_storage_get_stats slow: ~1.4-1.7s` in logs.
  - Risk: starves UCP responsiveness and can trigger ESP32 session timeouts.
  - Progress: packs stats polling is skipped in deferred-rescan path (`broad polling=0` path); UCP uses a local pack-stats cache; `GET packs.stats`, `diag.storage_health`, and pack commands no longer perform synchronous `pack_storage_get_stats()` in request/response path (they request async refresh and use cache/stale-or-busy behavior). Async pack/env history cache refreshes were moved off `system workqueue` and now run on the UCP thread (larger stack), reducing lockup risk under ESP32 request load. Automatic stale refresh for pack/env caches is now also disabled when `broad polling=0` (on-demand refresh only), preventing heavy cache refresh from firing during subscription/fetch bursts. `GET environment.current` no longer schedules env-history stats refresh (cache read only), avoiding a hidden heavy refresh right before subsequent `peer_power`/follow-up GET requests.
  - Remaining: remove the remaining synchronous refresh in broad-polling fallback code (currently disabled by default), and optionally split cache refresh into a dedicated producer if UCP thread stack pressure remains high under soak.
  - Files: `src/ucp_uart.c`, `src/pack_storage.c`

- [~] **UCP transport thread wakeup should be event-driven (not `k_msleep(5)` loop)**
  - Problem: parser still drains ring in periodic loop.
  - Risk: latency jitter, wasted CPU, starvation interaction with heavy work.
  - Progress: IRQ callback and deferred-rescan requests now `k_sem_give()` the UCP thread; thread waits on semaphore with timeout (hybrid event-driven). Notify worker no longer wakes every `50 ms` in active session when `broad polling=0`; heartbeat scheduling is now deadline-driven from `g_last_evt_ms`, and no-session idle wakeups are reduced (worker idle reschedule moved from `250 ms` to `2000 ms`, then to `10000 ms`, with explicit fast reschedule on `HELLO/SUB/UNSUB`). `task.status` polling is now adaptive (`active` fast, idle slower) to reduce background wakeups. The UCP thread wait timeout is now deadline-driven (session timeout / stream idle timeout / deferred-rescan quiet/grace / pending cache refresh intervals) with a larger max idle fallback, reducing periodic wakeups further.
  - Remaining: remove remaining fixed timeout dependency where possible and tune deadlines using soak/latency measurements.
  - Files: `src/ucp_uart.c`

- [~] **Time budget/yield for deferred rescan processing**
  - Problem: a single rescan batch can run too long.
  - Progress: deferred rescans now use a time budget and requeue leftover topic bits for later processing; deferred processing is also postponed while RX is active to prioritize request/response and keepalive. UCP timestamp generation (`now_utc_ms`) now uses a cached UTC-seconds + uptime interpolation path (periodic refresh) instead of hitting `timezone_get_unix_utc()` / RTC on every build/event, significantly reducing per-slice cost during event storms.
  - Remaining: enforce finer-grained budgets inside heavy per-topic handlers (after splitting monolithic `maybe_domain_events()`).
  - Files: `src/ucp_uart.c`

## P1 (Correctness / notify coverage)

- [ ] **Audit notify coverage with `broad polling=0`**
  - Confirm all important UCP topics emit via hooks (BLE/local/scheduler/sensor), not only polling fallback.
  - Produce pass/fail matrix against `read-set-notify-checklist` + runtime verification.
  - Files: `src/bt_irrigation_service.c`, `src/watering_tasks.c`, `src/ucp_uart.c`, docs

- [ ] **Add non-BLE hooks for internal scheduler/sensor/local changes**
  - Goal: “notify for everything” must include non-BLE origins, not just BLE notify wrappers.
  - Files: `src/watering_tasks.c`, `src/watering.c`, sensor/history modules, `src/ucp_uart.c`

- [~] **Guard/coalesce deferred rescan storms per-topic**
  - Progress: added per-topic deferred-rescan request rate limiting (small min interval) plus existing pending-mask coalescing.
  - Remaining: add explicit per-topic pending counters/diagnostics and tune intervals with soak/stress data.
  - Files: `src/ucp_uart.c`

- [ ] **Session boundary parser hygiene (extended)**
  - Already partially fixed (timeout path clears parser/ring).
  - Add equivalent cleanup on explicit session reset paths if introduced later; verify boot-noise handling.
  - Files: `src/ucp_uart.c`

- [ ] **`snapshot-before-live` behavior documented/guaranteed operationally**
  - Since pre-session rescans are intentionally dropped, clients must bootstrap with snapshot after subscribe.
  - Ensure docs + ESP32 behavior align.
  - Files: docs (`events-and-subscriptions`, `client guide`), ESP32 client (external)

## P1 (ESP32 client robustness; not protocol changes)

- [ ] **Pause dispatcher queue when session is not `ACTIVE`**
  - Prevent queued `SUB_REQ`/`GET`/`SET` from timing out while `HELLO` is still in progress.
  - Symptom seen in logs: request timeouts continue during reconnect/HELLO retries.
  - Owner: ESP32 firmware (external repo)

- [ ] **Adaptive HELLO backoff for long nRF boot**
  - Reduce UART spam and log noise before nRF UCP is initialized.
  - Owner: ESP32 firmware (external repo)

- [ ] **Per-session diagnostic counters / resettable counters**
  - Avoid misleading accumulated timeout metrics after fixes and reconnects.
  - Owner: ESP32 firmware (external repo)

## P2 (Diagnostics / observability)

- [ ] **Add stack watermark diagnostics for UCP thread + system workqueue**
  - Goal: replace guesswork (`12 KB` stack workaround) with measured data.
  - Progress: UCP debug helper is wired in code, but build-time availability still depends on Zephyr stack instrumentation support (`k_thread_stack_space_get` implementation in current config/toolchain path).
  - Remaining: enable/verify supported stack instrumentation in build profile and add periodic reporting for both UCP thread and (if feasible) system workqueue.
  - Files: `src/ucp_uart.c`, `prj.conf` (thread runtime/stack instrumentation if needed)

- [ ] **Gate verbose UCP debug logs behind Kconfig/runtime flag**
  - Keep `UCP UART dbg...` and slow warnings switchable.
  - Files: `Kconfig`, `prj.conf`, `src/ucp_uart.c`

- [ ] **Add retained crash breadcrumbs / last-UCP-state marker**
  - Helps diagnose future lockups without live serial.
  - Files: `src/main.c`, `src/ucp_uart.c` (and retained storage helpers)

## P2 (Validation / test closure)

- [ ] **Soak test 1-2h: UART/UCP stable under normal operation**
  - Criteria: no lockups, no reconnects, no session timeouts, no ring drops.

- [ ] **Stress test notify + BLE writes + scheduler updates**
  - Criteria: no `PING` starvation, stable `EVENT_CHANGE`, no false overflow.

- [ ] **Reconnect stress (ESP reset / nRF reset / EN power cycle)**
  - Criteria: clean resync, no queue timeout storms, no parser desync.

- [ ] **STREAM concurrency test (`STREAM_*` + `PING` + events)**
  - Criteria: keepalive remains responsive during streaming.

- [ ] **BLE/UCP concurrent write test (`last write wins`)**
  - Criteria: correct `origin`, consistent `resource_version`, expected event ordering.

- [ ] **OTA E2E hardware test (`ota.enter_recovery -> MCUboot -> mcumgr -> app`)**
  - Criteria: recovery entry works reliably, upload/swap/confirm/post-check pass.

## Notes / constraints

- `UCP UART runtime configure rc=-134` is acceptable on current nRF UART driver; runtime reconfigure unsupported. DTS `current-speed` is authoritative.
- Broad polling is intentionally disabled for stability; missing notifications should be fixed by hooks or topic-specific deferred rescans, not by re-enabling broad polling globally.
- RAM usage is high; large new worker stacks/buffers should be avoided unless measured and justified.

