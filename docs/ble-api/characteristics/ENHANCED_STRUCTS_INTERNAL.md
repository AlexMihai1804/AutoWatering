# Enhanced BLE Structs (Internal Reference)

These structures exist in firmware for advanced or aggregated functionality but are NOT (yet) formally exposed as standalone public characteristics. They are documented here strictly for internal developer reference and future roadmap decisions. Any external client should rely only on the officially documented characteristic files.

> Status: Internal - do not rely on stability across releases. Size locked via BUILD_ASSERT.

## enhanced_channel_config_data (193 B)
Source: `bt_gatt_structs_enhanced.h`
Purpose: Rich superset of basic channel configuration including custom soil, compensation, interval mode, scoring and timestamps. Currently used during multi-stage configuration workflows and reset operations. Not directly mapped 1:1 to a public BLE characteristic (public Channel Configuration uses the 76B base struct + fragmentation).

Key Groups:
- Identity & basic: channel_id, name[64], auto_enabled
- Plant/growing: plant_type, soil_type, irrigation_method, coverage_type + (area_m2 | plant_count), sun_percentage
- Custom soil (guarded by use_custom_soil): custom_soil_name[32], field_capacity, wilting_point, infiltration_rate, bulk_density, organic_matter
- Rain compensation: enabled flag + sensitivity / lookback / thresholds
- Temperature compensation: enable + base + sensitivity + min/max factors
- Interval mode parameters (watering/pause durations in min+sec)
- Configuration progress flags + composite score
- Timestamps: last_config_update, custom_soil_created, custom_soil_modified
- Reserved[8] for forward compatibility

Rationale for keeping internal:
- Exposes many fields not intended for initial mobile client versions
- Avoids versioning churn while advanced features stabilize

Migration path (future): Provide a read-only "Enhanced Channel Snapshot" characteristic with version byte + selective masks for incremental updates.

## enhanced_task_status_data (60 B)
Purpose: Provides richer runtime insight for interval mode tasks vs public `Current Task Status` (21B). Adds interval phase timing, cycle counters and applied compensation factors.

Fields (grouped):
- Core task: channel_id, task_state, task_mode, remaining_time, total_elapsed, total_volume
- Interval mode: is_interval_mode, currently_watering, phase_remaining_sec, cycles_completed, watering/pause configured durations (minutes+seconds)
- Compensation: rain_reduction_percentage, rain_skip_watering, temp_compensation_factor, temp_adjusted_requirement
- Timestamps: task_start_time, phase_start_time, next_phase_time
- reserved[4]

Why internal:
- Interval mode UI/logic experimental; public API avoids rapid changes
- Some values derivable client-side from existing smaller snapshot + history

Future exposure option: Offer as optional extended read (single frame) if feature flag enabled; otherwise keep hidden.

## Integrity & Size Guards
Both structs enforced with BUILD_ASSERT ensuring binary layout stability for any code that serializes them internally.

## Guidelines for Future Public Exposure
1. Add version byte as first field for forward evolution.
2. Provide capability bit in System Status or a dedicated Capabilities characteristic before exposing.
3. Supply migration notes & diff vs base structs.
4. Avoid leaking internal scoring heuristics unless frozen.

## Decision Log
- Aug 13 2025: Marked internal; no BLE characteristic UUID assigned.

_This file is auto-audited alongside characteristic docs; update if layout changes._
