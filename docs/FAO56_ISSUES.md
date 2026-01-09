# FAO-56 Issues (Critical)

This file lists the GitHub issues created for the FAO-56 calculation problems and the proposed fix direction.

| Issue | Title | Fix summary |
| --- | --- | --- |
| #15 | AUTO schedule: sun_exposure not applied to deficit accumulation | Apply sun_factor in realtime ETc (or move ETc into daily update) and keep a single path that mutates D. |
| #16 | Auto calc (Quality/Eco) uses a different water balance engine than AUTO schedule | Unify on one water balance engine and shared helpers; auto calc should use stored D; keep heuristic ET0 as explicit fallback only. |
| #17 | AUTO schedule: deficit reduction after irrigation uses fixed 0.8 (double counts losses) | Reduce D by net (gross * efficiency * DU) or model explicit losses; avoid a fixed 0.8 that double-counts. |
| #18 | Wetting fraction likely double-compensated (risk of overwatering for drip) | Pick one interpretation of wetting_fraction and remove the extra adjustment (either in AWC or in volume). |
| #19 | Penman-Monteith uses fixed sun_ratio/altitude/wind assumptions -> ET0 bias | Estimate Rs from deltaT, derive altitude from pressure or config, and optionally expose a wind exposure slider. |
| #20 | AUTO schedule: effective rain applied only at daily check (can water after rain) | Apply incremental effective rain when rain events are recorded and keep daily reconciliation. |
| #21 | Offline gap uses fixed ET0=3.0 mm/day; too coarse | Use local ET0 climatology (weekly/monthly) from history; fallback to defaults if missing. |
| #22 | Custom soil not wired into core FAO-56 paths | Add a single soil resolver (custom or DB) and use it in all FAO-56 calculations. |
| #23 | Root depth and Kc logic inconsistent across paths | Create shared helpers for root depth and Kc; standardize clamping and stage interpolation. |
