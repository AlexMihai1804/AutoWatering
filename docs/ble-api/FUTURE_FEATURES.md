# FUTURE FEATURES / ROADMAP (Extracted from documentation)

This file consolidates all unimplemented, placeholder, or planned features referenced in the BLE documentation so characteristic files remain accurate and implementation-only.

## 1. History Management (Characteristic #12)
CURRENT STATUS: Implemented (experimental) - detailed/daily/monthly/annual queries working with unified 8B header, rate limiting, clear command.
REMAINING / FUTURE:
- Accurate persisted timestamps (currently reconstructed heuristically for detailed)
- True multi-page pagination beyond first batch
- Precise monthly `total_sessions` (placeholder constant) & annual `most_active_month`
- Server-side time filtering using start/end timestamps
- Extended status codes (no data window, partial, filter invalid)
- Persistent storage (currently RAM-only) & retention policies

## 2. Global Compensation (System Configuration)
- Global rain/temperature compensation fields ignored on write; no effect yet.
- Global sensitivity/enable overriding per-channel planned.

## 3. Eco / Quality Dynamic Modes
- Automatic eco vs quality switching based on seasonal / climatological analysis.
- Mode 2 (eco 70%) accepted but lacks adaptive reduction logic.

## 4. Extended Environmental History
- Real min/max vs avg (currently min=max=avg). `sample_count` now reflects real aggregation depth; remaining to expose authentic min/max capture.
- Multi-entry packing (`entry_count >1`) in fragments.

## 5. Additional Compensation Domains
- Humidity, pressure, wind, seasonal, manual override bitmaps (Compensation Status extension) - absent.

## 6. Statistics Persistence & Milestones
- Persist channel statistics in NVS/flash (currently volatile).
- Milestone events (e.g., every 10L dispensed).

## 7. Task Queue Enhancements
- Implement `task_id_to_delete` selective deletion.
- Dynamic reordering / prioritization.

## 8. Advanced Reset Operations
- Additional reset codes (granular channel history, statistics-only) - currently rejected.

## 9. Rain / Weather Adaptation (Removed from current list)
- Separate characteristics (*History Compensation*, *Season Adaptivity*, *Efficiency Control*, *Weather Adaptation*, *Log Settings*) never implemented.
- May surface via Auto Calc Status extensions or new characteristics.

## 10. Rain Integration Extensions
- Expanded hourly/daily storage with adaptive space management.
- Dynamic update of `channel_reduction_pct[]` via rainfall model.

## 11. Calibration Workflow
- Extra states (validating, aborted with reason codes) beyond actions 0-3.

## 12. Data Quality Heuristics
- Multi-factor dynamic scoring (currently simple placeholder e.g. 85 or 0).

## 13. Security / Access Control
- Characteristic-level permission gating / pairing flags.

## 14. Fragmentation Optimizations
- Auto single-write fast path (MTU >= struct size) skipping buffer staging (partial for system config only).
- Retransmit / resend window control for large histories.

## 15. Onboarding Metrics
- Expanded flags and time-to-complete statistics derived from events.

---
Roadmap aggregated from "placeholder", "not implemented", and "future" references. Update as features ship; remove delivered items and reflect real behavior in characteristic docs.
