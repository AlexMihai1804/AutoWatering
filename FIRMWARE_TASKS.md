# AutoWatering Firmware — Task List

Generated from extensive code analysis on 2026-02-17.
Current version: 0.0.3-release

---

## OPEN — Remaining work

- [ ] **T7** Split `bt_irrigation_service.c` (12,812 lines)
  - Notification engine, calendar utils, history handlers, sensor handlers all merged
  - Any change risks regression in unrelated code
  - **Fix**: Extract into: `bt_notify_engine.c`, `bt_calendar_utils.c`, `bt_history_handlers.c`, `bt_sensor_handlers.c`, `bt_schedule_handlers.c`

- [ ] **T21** Configure hardware watchdog in DT overlay
  - No `wdt` node — firmware hang can leave valve open indefinitely
  - **Fix**: Add nRF WDT config, feed from main loop, assert on timeout

- [ ] **T22** Add Kconfig toggles for major subsystems
  - Currently only 2 toggles (PM_SUPPORT, EXTERNAL_FLASH)
  - Need: hydraulic monitor, temp compensation, rain integration, interval mode, master valve

- [ ] **T23** Add valve feedback (GPIO readback)
  - No verification that valve actually changed state after activation
  - **Fix**: Read back GPIO after set, flag fault if mismatch

- [ ] **T24** Add battery/power ADC monitoring
  - Power modes defined in software but no voltage sensing configured
  - **Fix**: Add ADC config in overlay, periodic voltage sampling

---

## COMPLETED — Fixed in previous releases

| Task | Summary | Version |
|------|---------|---------|
| T1 | NVS key mismatch for flow calibration | 0.0.2 |
| T2 | Valve GPIO double-inversion | 0.0.2 |
| T3 | NVS mutex for thread safety | 0.0.2 |
| T4 | Double snapshot corruption | 0.0.2 |
| T5 | ET0 slew NaN guard + reset | 0.0.2 |
| T6 | 32-bit uptime wrap fix | 0.0.2 |
| T8 | NaN validation on NVS load | 0.0.2 |
| T9 | Task re-queue priority fix | 0.0.2 |
| T10 | Flow ISR debounce braces | 0.0.2 |
| T11 | Atomic notification flags | 0.0.3 |
| T12 | Pack transfer timeout | 0.0.3 |
| T13 | Centralized NVS key registry | 0.0.3 |
| T14 | BUILD_ASSERT for ATTR_IDX | 0.0.3 |
| T15 | Dead code removal | 0.0.3 |
| T16 | Compile-time date fallback | 0.0.3 |
| T17 | Real print_memory_stats | 0.0.3 |
| T18 | Duplicate includes removed | 0.0.3 |
| T19 | printk+LOG dual-logging fixed | 0.0.3 |
| T20 | Legacy fields wrapped in LEGACY_COMPAT | 0.0.3 |
| T25 | Interval mode cycle limit | 0.0.3 |
| T26 | Real stack overflow detection | 0.0.3 |

---

## WELL-DONE — Keep as-is

- Hydraulic learning state machine — professional-grade auto-calibration
- Ring buffer history on external flash — saves ~74KB RAM
- CRC32 + magic + version integrity checks on database load
- Debounced BLE onboarding notifications with `k_work_reschedule`
- Freeze lockout defense-in-depth (checked at enqueue AND start)
- Temperature compensation separation (calc/validation/integration w/ FAO-56 double-count prevention)
- Architecture documentation with accurate Mermaid diagrams
- Fast boot (skip DTR wait, immediate WDT feed, auto MCUboot confirm)
