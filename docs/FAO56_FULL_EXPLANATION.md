# FAO-56 Full Explanation (AutoWatering - current code)

<!-- markdownlint-disable MD022 MD032 MD031 MD040 MD037 -->

This document is a complete, code-accurate explanation of the FAO-56 + related
calculations used by the app. It starts with all database fields, user settings,
constants, and relevant history sources, then details the full data flow and
formulas used in AUTO scheduling and on-demand FAO-56 calculations.

All formulas and behavior are based on the current code in:
- `src/fao56_calc.c`, `src/fao56_calc.h`, `src/water_balance_types.h`
- `src/env_sensors.c`, `src/env_sensors.h`
- `src/watering.h`, `src/watering.c`
- `src/plant_full_db.inc`, `src/soil_enhanced_db.inc`, `src/irrigation_methods_db.inc`
- `src/custom_soil_db.c`, `src/custom_soil_db.h`
- `src/watering_monitor.c`, `src/rain_history.h`
- `src/soil_moisture_config.c`, `src/soil_moisture_config.h`
- `src/watering_tasks.c`

ASCII-only by request.

---

## 1) Databases (all fields)

### 1.1 Plant DB (plant_full_data_t)
Source: `src/plant_full_db.inc`

Fields (scaled):
- subtype_enum
- common_name_en
- scientific_name
- kc_ini_x1000
- kc_mid_x1000
- kc_end_x1000
- kc_dev_x1000
- root_depth_min_m_x1000
- root_depth_max_m_x1000
- stage_days_ini
- stage_days_dev
- stage_days_mid
- stage_days_end
- depletion_fraction_p_x1000
- spacing_row_m_x1000
- spacing_plant_m_x1000
- default_density_plants_m2_x100
- canopy_cover_max_frac_x1000
- frost_tolerance_c
- temp_opt_min_c
- temp_opt_max_c
- growth_cycle
- typ_irrig_method_id

Macros:
- PLANT_KC_INI, PLANT_KC_MID, PLANT_KC_END, PLANT_KC_DEV
- PLANT_ROOT_MIN_M, PLANT_ROOT_MAX_M
- PLANT_DEPL_FRACTION
- PLANT_ROW_SPACING_M, PLANT_PLANT_SPACING_M
- PLANT_DENSITY_P_M2
- PLANT_CANOPY_MAX_FRAC

### 1.2 Soil DB (soil_enhanced_data_t)
Source: `src/soil_enhanced_db.inc`

Fields:
- soil_id
- soil_type
- texture
- fc_pctvol_x100 (field capacity)
- pwp_pctvol_x100 (wilting point)
- awc_mm_per_m (available water capacity, mm/m)
- infil_mm_h (infiltration rate)
- p_raw_x1000 (default depletion fraction)

### 1.3 Irrigation Method DB (irrigation_method_data_t)
Source: `src/irrigation_methods_db.inc`

Fields:
- method_id
- method_name
- code_enum
- efficiency_pct
- wetting_fraction_x1000
- distribution_uniformity_pct (DU)
- depth_typical_min_mm
- depth_typical_max_mm
- application_rate_min_mm_h
- application_rate_max_mm_h

---

## 2) User settings (watering_channel_t)
Source: `src/watering.h`

### 2.1 Scheduling / modes
- watering_event.schedule_type (SCHEDULE_DAILY / SCHEDULE_PERIODIC / SCHEDULE_AUTO)
- watering_event.watering_mode (WATERING_BY_DURATION / WATERING_BY_VOLUME /
  WATERING_AUTOMATIC_QUALITY / WATERING_AUTOMATIC_ECO)
- watering_event.auto_enabled
- start_time (hour/minute), optional solar timing flags

### 2.2 Plant / soil / irrigation selection
- plant_db_index
- soil_db_index
- irrigation_method_index

### 2.3 Coverage (important)
- use_area_based
- coverage.area_m2 (for area based)
- coverage.plant_count (for plant based)

Important: the app expects the **zone area** (not wetted area). Wetting
fraction is applied only to AWC/RAW, not to volume.

### 2.4 AUTO settings
- auto_mode (QUALITY or ECO)
- max_volume_limit_l
- enable_cycle_soak
- planting_date_unix (used for DAP)
- days_after_planting (runtime)
- latitude_deg / longitude_deg
- sun_exposure_pct (0..100, default 75)
- water_balance pointer (runtime state)

### 2.5 Custom soil (per channel)
In `watering_channel_t`:
- soil_config.use_custom_soil
- soil_config.custom.name
- soil_config.custom.field_capacity
- soil_config.custom.wilting_point
- soil_config.custom.infiltration_rate
- soil_config.custom.bulk_density
- soil_config.custom.organic_matter

### 2.6 Soil moisture overrides (antecedent moisture)
Source: `src/soil_moisture_config.c`
- global: enabled, moisture_pct
- per channel override: enabled, moisture_pct
- default if nothing configured: 50%

### 2.7 Flow / hydraulic
Used for cycle/soak and actual volume:
- hydraulic.nominal_flow_ml_min (learned)
- flow calibration pulses per liter (system config)

---

## 3) Custom soil database (NVS)
Source: `src/custom_soil_db.c`, `src/watering_enhanced.h`

Stored fields:
- channel_id, name
- field_capacity (%)
- wilting_point (%)
- infiltration_rate (mm/h)
- bulk_density (g/cm3)
- organic_matter (%)
- created_timestamp, modified_timestamp
- crc32

Validation:
- FC 5..80, WP 1..40, WP < FC
- infil 0.1..1000
- bulk_density 0.5..2.5
- organic_matter 0..100

Conversion to enhanced soil:
- awc_mm_per_m = (FC - WP) * 10
- p_raw (depletion fraction) heuristic:
  - FC < 15 -> p=0.7
  - FC > 35 -> p=0.3
  - else p=0.5
  - if OM > 5 -> p *= 0.9
  - clamp to 0.2..0.8

---

## 4) Relevant histories (for FAO-56)

### 4.1 Rain history
Source: `src/rain_history.h`
- Hourly: 30 days (720 entries), mm, pulse_count, data_quality
- Daily: 5 years (1825 entries), total rainfall, max hourly, active hours
- `rain_history_get_last_24h()` used in daily AUTO update
- `rain_history_get_recent_total()` used in other features

### 4.2 Environmental history (for ET0 climatology)
Source: `src/watering_enhanced.h`
- Hourly, daily, monthly aggregates
- Used in `fao56_build_weekly_et0_climatology()` to compute weekly ET0 averages

### 4.3 Water balance persistence
Source: `src/nvs_config.h`, `src/nvs_config.c`
Stored per channel:
- rwz_awc_mm, wetting_awc_mm, raw_mm
- current_deficit_mm
- effective_rain_mm
- irrigation_needed
- last_update_time
Runtime-only (not persisted):
- wetting_fraction, surface_wet_fraction, surface_wet_update_s
- surface_deficit_mm, surface_tew_mm, surface_rew_mm
- rain_applied_surface_mm, rain_applied_root_mm, rain_applied_raw_mm

---

## 5) Constants used by FAO-56
Source: `src/fao56_calc.c`

Core FAO constants:
- PI
- SOLAR_CONSTANT = 0.0820 (MJ m-2 min-1)
- STEFAN_BOLTZMANN = 4.903e-9

ET0 / radiation:
- ASSUMED_WIND_SPEED_M_S = 2.0
- ASSUMED_SUNSHINE_RATIO = 0.50
- ASSUMED_ALBEDO = 0.23
- STANDARD_ATMOS_PRESSURE_KPA = 101.3
- ET0_ABSOLUTE_MAX_MM_DAY = 15.0
- HARGREAVES_RS_COEFF = 0.16

ET0 heuristic (fallback only):
- HEURISTIC_ET0_COEFF = 0.045
- HEURISTIC_ET0_TEMP_OFFSET = 20.0
- HEURISTIC_ET0_VPD_FLOOR = 0.05
- HEURISTIC_ET0_MIN = 0.5
- HEURISTIC_ET0_MAX = 6.0

ET0 slew (asymmetric):
- ET0_SLEW_MAX_INC_MM_DAY = 5.0
- ET0_SLEW_MAX_DEC_MM_DAY = 2.0
- ET0_SLEW_MAX_INC_HOT_MM_DAY = 7.0
- ET0_SLEW_MIN_STEP_MM_DAY = 0.1
- ET0_SLEW_RESET_SECONDS = 3 days
- ET0_SLEW_HEATWAVE_TMAX_C = 33.0
- ET0_SLEW_HEATWAVE_VPD_KPA = 2.0

ET0 ensemble:
- ET0_ENSEMBLE_MAX_WEIGHT = 0.85 (max PM weight)

Surface bucket (dual-Kc):
- FAO56_SURFACE_LAYER_M = 0.10
- FAO56_SURFACE_TEW_MIN_MM = 4.0
- FAO56_SURFACE_TEW_MAX_MM = 15.0
- FAO56_SURFACE_REW_FRAC = 0.5
- FAO56_SURFACE_REW_MIN_MM = 2.0
- FAO56_SURFACE_REW_MAX_MM = 8.0
- FAO56_KE_MAX_BASE = 0.90
- FAO56_KE_CANOPY_REDUCTION = 0.5
- FAO56_SURFACE_WET_DECAY_SECONDS = 18h
- FAO56_SURFACE_WET_RAIN_FRACTION = 1.0
- FAO56_SURFACE_WET_DECAY_ET0_MM = 3.0

MAD (p) adaptation:
- FAO56_MAD_ETC_REF_MM_DAY = 5.0
- FAO56_MAD_ETC_ADJ_COEFF = 0.04
- FAO56_MAD_MIN_FRACTION = 0.1
- FAO56_MAD_MAX_FRACTION = 0.8

Wetting fraction:
- FAO56_WF_MIN = 0.10
- FAO56_WF_MAX = 1.00
- FAO56_WF_DEPTH_LOG_COEFF = 0.15
- FAO56_WF_SLEW_MAX_FRAC_PER_WEEK = 0.10

PM sanity gates:
- FAO56_ET0_PM_DT_MIN_C = 1.0
- FAO56_ET0_PM_RATIO_MIN = 0.30
- FAO56_ET0_PM_RATIO_MAX = 2.50

Rain:
- RAIN_INTENSITY_MAX_MM_H = 100.0

Eco:
- ECO_ETC_FACTOR = 0.7 (eco management scaling)

Climatology:
- FAO56_DEFAULT_ET0_MM_DAY = 3.0
- Monthly defaults RO: [0.6,0.9,1.6,2.6,3.6,4.5,5.0,4.6,3.2,2.0,1.0,0.6]
- FAO56_CLIMATOLOGY_WEEKS = 53

---

## 6) Environmental data (sensors + derived)
Source: `src/env_sensors.h`, `src/env_sensors.c`

Raw inputs:
- air_temp_mean_c, air_temp_min_c, air_temp_max_c
- rel_humidity_pct
- atmos_pressure_hpa
- rain_mm_24h
- valid flags per sensor
- timestamp, data_quality

Derived values:
- saturation_vapor_pressure_kpa (es)
- vapor_pressure_kpa (ea)
- dewpoint_temp_c
- derived_values_calculated flag

Derivations:
- es = 0.6108 * exp(17.27*T / (T + 237.3))
- ea = es * RH / 100
- if ea > 0: dewpoint = 237.3 * ln(ea/0.6108) / (17.27 - ln(ea/0.6108))
- else: dewpoint = T - 20 (fallback when ea <= 0)
- fallback dewpoint is display-only and not treated as dewpoint-available for PM
- dewpoint_valid = derived_values_calculated and (ea > 0)

Tmin/Tmax are taken from a rolling 24h window (production).

---

## 7) ET0 formulas

### 7.1 Extraterrestrial radiation (Ra)
Used by both HS and PM:
```
declination = 0.409 * sin(2*pi*J/365 - 1.39)
x = -tan(lat_rad) * tan(declination)
x = clamp(x, -1, 1)
sunset_angle = acos(x)
dr = 1 + 0.033 * cos(2*pi*J/365)
Ra = (24*60/pi) * SOLAR_CONSTANT * dr *
     (sunset_angle*sin(lat_rad)*sin(declination) +
      cos(lat_rad)*cos(declination)*sin(sunset_angle))
```

### 7.2 Hargreaves-Samani (HS)
```
dT = max(Tmax - Tmin, 0)
ET0 = 0.0023 * (Tmean + 17.8) * sqrt(dT) * Ra
ET0 clamp: 0..ET0_ABSOLUTE_MAX_MM_DAY
```

### 7.3 Penman-Monteith (PM)
```
es(T) = 0.6108 * exp(17.27*T / (T + 237.3))
es = (es(Tmax) + es(Tmin)) / 2
ea = es * RH/100
if dewpoint is available (dewpoint_valid): ea = es(Tdew) and clamp ea <= es
delta = 4098 * es / (T + 237.3)^2
gamma = 0.000665 * P_kPa
u2 = 2 m/s (assumed)

Rs from delta-T:
  dT = max(Tmax - Tmin, 0)
  Rs = 0.16 * sqrt(dT) * Ra
  sunshine_ratio = (Rs/Ra - 0.25) / 0.50
  sunshine_ratio = clamp(sunshine_ratio, 0, 1)

Rso = (0.75 + 2e-5*alt) * Ra
Rs/Rso is clamped to [0.05..1.0]
Rns = (1 - albedo) * Rs
Rnl = STEFAN_BOLTZMANN * ((TmaxK^4 + TminK^4)/2) * (0.34 - 0.14*sqrt(ea)) * (1.35*Rs/Rso - 0.35)
Rn = Rns - Rnl
G = 0 (daily timestep)

ET0 = [0.408*delta*(Rn-G) + gamma*(900/(T+273))*u2*(es-ea)] /
      [delta + gamma*(1+0.34*u2)]
```
ET0 clamp: 0..ET0_ABSOLUTE_MAX_MM_DAY

Notes/units:
- sunshine_ratio is clamped to 0..1
- lat_rad = latitude_deg * pi/180
- J = day-of-year (1..365/366)
- P_kPa = atmos_pressure_hpa / 10
- TmaxK = Tmax + 273.16, TminK = Tmin + 273.16
- alt is meters

Altitude from pressure (if pressure valid):
```
alt = 44331 * (1 - (P_kPa/101.3)^0.1903)
```
If pressure is invalid, alt is assumed 0 m (sea level) for Rso.

### 7.4 ET0 ensemble (AUTO)
```
ET0 = w * ET0_PM + (1-w) * ET0_HS
```
Weight w increases with sensor quality:
```
w = 0.5 + 0.1*(pressure_valid) + 0.2*(humidity_valid) + 0.1*(data_quality>=80)
w clamped to 0.2..0.85
```
pressure_valid and humidity_valid are treated as 0/1.
If PM invalid, fallback to HS only.
ET0_PM_raw is the pre-clamp PM value used for validity checks.
PM invalid if ET0_PM_raw <= 0.01 or ET0_PM_raw > ET0_ABSOLUTE_MAX_MM_DAY * 1.2.
If valid, ET0_PM = clamp(ET0_PM_raw, 0..ET0_ABSOLUTE_MAX_MM_DAY) before mixing.

Sanity gates on PM weight:
- if ET0_PM / ET0_HS is outside 0.30..2.50 -> weight reduced
- if dT < 1.0 C and PM is much higher than HS -> weight reduced
- if dewpoint_valid and dewpoint > Tmax + 0.5 C -> weight reduced

### 7.5 ET0 slew (asymmetric)
For each channel:
```
max_inc = ET0_SLEW_MAX_INC_MM_DAY
max_dec = ET0_SLEW_MAX_DEC_MM_DAY
if heatwave (Tmax >= 33C or VPD >= 2.0 kPa):
  max_inc = ET0_SLEW_MAX_INC_HOT_MM_DAY

allowed_inc = max_inc * (elapsed_s / 86400)
allowed_dec = max_dec * (elapsed_s / 86400)
min_step = ET0_SLEW_MIN_STEP_MM_DAY * (elapsed_s / 86400)
allowed_inc = max(allowed_inc, min_step)
allowed_dec = max(allowed_dec, min_step)
ET0_new is clamped to [ET0_last - allowed_dec, ET0_last + allowed_inc]
```
Slew reset / min step:
- if elapsed_s == 0 or elapsed_s > ET0_SLEW_RESET_SECONDS, accept ET0_new and reset state
- ET0_last and ET0_new are in mm/day; allowed_inc/allowed_dec are delta(ET0) in mm/day
- min_step is applied on the allowed delta and scales with elapsed_s (ET0_SLEW_MIN_STEP_MM_DAY * elapsed_s/86400)

---

## 8) Crop coefficient Kc (dynamic)
Source: `calc_phenological_stage()` and `calc_crop_coefficient()` in `src/fao56_calc.c`.

Stages:
```
stage_1_end = stage_days_ini
stage_2_end = stage_1_end + stage_days_dev
stage_3_end = stage_2_end + stage_days_mid
```

Kc by stage:
```
Initial: Kc = Kc_ini
Development: Kc = Kc_ini + (Kc_mid - Kc_ini) * (days_in_stage / stage_days_dev)
Mid-season: Kc = Kc_mid
End-season: Kc = Kc_mid + (Kc_end - Kc_mid) * (days_in_stage / stage_days_end)
```
Where:
- days_in_stage = days_after_planting - stage_start_day
- stage_start_day = stage_1_end (development), stage_3_end (end-season)
Guards:
- if stage_days_dev == 0, Kc jumps to Kc_mid (no interpolation, avoid div0)
- if stage_days_end == 0, Kc jumps to Kc_end (no interpolation, avoid div0)

Canopy scaling (new):
```
stage_1_end = stage_days_ini
stage_2_end = stage_1_end + stage_days_dev
if stage_days_dev == 0:
  canopy_progress = (days_after_planting > stage_1_end) ? 1 : 0
else if days_after_planting <= stage_1_end:
  canopy_progress = 0
else if days_after_planting >= stage_2_end:
  canopy_progress = 1
else:
  canopy_progress = (days_after_planting - stage_1_end) / stage_days_dev

canopy_factor = canopy_cover_max_frac * canopy_progress
Kc_eff = Kc_ini + (Kc - Kc_ini) * canopy_factor

Note: canopy_progress is monotonic (no decline in end-season); Kc_end already
captures senescence and reduced transpiration.
Kc_eff clamp: 0.1..2.0
```

---

## 9) Root depth (dynamic)
```
total_season = sum(stage_days_ini/dev/mid/end)
progress = 0
if total_season > 0:
  progress = clamp(days_after_planting / total_season, 0..1)
sigmoid = 1 / (1 + exp(-6*(progress - 0.5)))
root_depth = root_min + (root_max - root_min) * sigmoid
```

---

## 10) Surface bucket (dual-Kc light)
Surface evaporation bucket is tracked per channel:
- TEW = total evaporable water for top layer (zone-avg mm)
- REW = readily evaporable water (zone-avg mm)
- D_surface in [0..TEW], 0=wet, TEW=dry (mm over zone area)

Surface wet area state:
- surface_wet_fraction tracks how much of the surface is effectively wetted (0..1).
- Rain event: set to FAO56_SURFACE_WET_RAIN_FRACTION (1.0).
- Irrigation event: set to wetting_fraction * DU (clamped) to reflect distribution.
- surface_wet_target = wetting_fraction * DU (clamped) is the decay target between events.
- Between events: exponential decay toward surface_wet_target (wetting_fraction * DU) based on ET0:
  ```
  et0_cum = ET0 * dt / 86400
  surface_wet_fraction = surface_wet_target +
                         (surface_wet_fraction - surface_wet_target) * exp(-et0_cum / 3.0)
  ```
  If ET0 is not available, a fallback 18h time constant is used.

Computed:
```
theta_fc = fc_pctvol / 100
theta_wp = pwp_pctvol / 100
TEW_base = clamp(1000 * Ze * (theta_fc - theta_wp), 4..15)
Ze = 0.10 m (FAO56_SURFACE_LAYER_M)
fallback: TEW_base = clamp(awc_mm_per_m * Ze, 4..15)
REW_base = clamp(TEW_base * 0.5, 2..8) or texture default:
  sand: 3 mm, loam: 6 mm, clay: 8 mm
TEW = TEW_base * surface_wet_fraction
REW = min(REW_base * surface_wet_fraction, 0.9*TEW)
if TEW <= 1e-3:
  // no effective wetted surface area
  TEW = 0
  REW = 0
  D_surface = 0
  Ke = 0
```
When TEW changes, D_surface is rescaled to keep D_surface/TEW stable (if TEW > 1e-3; otherwise D_surface is forced to 0).

Ke (soil evaporation coefficient):
```
wet_area = surface_wet_fraction
canopy_reduction = 1 - 0.5*canopy_factor, min 0.3
Ke_max = 0.90 * wet_area * canopy_reduction, clamped
canopy_factor is derived from phenological progress (same factor used for Kc scaling).

if D_surface <= REW:
  Ke = Ke_max
else if (TEW - REW) <= 1e-3:
  Ke = 0
else:
  Ke = Ke_max * (TEW - D_surface) / (TEW - REW)
```

Total crop coefficient used in ETc reporting:
```
Kc_total = Kc_eff + Ke
clamp 0.1..2.0
```

Surface deficit update:
- Realtime: D_surface += ET0 * Ke * (dt/86400) * sun_factor
- Daily: same via daily ET0
- Rain/irrigation: D_surface -= P_eff_surface or irrigation_mm (zone-avg), clamp to 0

---

## 11) Wetting fraction (dynamic)
Used for AWC/RAW and surface wet area:
Note: wetting_fraction is the irrigation-method wet area. The surface bucket uses
surface_wet_fraction, which can jump to 1.0 after rain and then decay toward
surface_wet_target = wetting_fraction * DU (clamped).

Base:
```
wf = method.wetting_fraction_x1000 / 1000
```

Adjustments (only if base < 0.95):
- depth (log1p):
  ```
  wf *= 1 + 0.15*log1p(depth_mm/10)
  ```
- soil texture:
  - clay: wf *= 1.15
  - sand: wf *= 0.85
- infiltration:
  - infil > 20 => wf *= 0.9
  - infil < 5 => wf *= 1.1
- spacing:
  - large spacing (area_per_plant > 1.0) => wf *= 0.9
  - dense (area_per_plant < 0.1) => wf *= 1.1

Clamp:
```
wf in [0.5*base .. 1.5*base], then absolute [0.10..1.00]
```

Slew limit:
- wf change is limited to about 10% per week (per channel, based on uptime)

---

## 12) AWC, RAW, deficit (D)
```
AWC_rz = soil.awc_mm_per_m * root_depth_m
wetting_awc = AWC_rz * wetting_fraction
RAW = wetting_awc * p

Deficit update:
D = clamp(D + ET_root*dt/86400 - P_eff_root - I_eff, 0, wetting_awc)
P_eff_root is the remaining effective rain after filling the surface bucket.
D tracks root depletion only; surface evaporation is tracked separately in D_surface.
```
AWC_rz is zone-averaged mm; wetting_awc is effective root-zone water after wetting_fraction.

AWC change handling:
- when wetting_awc changes (root depth or wf), D is rescaled to keep D/AWC stable

---

## 13) Effective precipitation (P_eff)
Inputs:
rainfall_mm, soil.infil, soil texture, antecedent moisture, temperature, duration/intensity

If rainfall < 1 mm:
```
P_eff = 0.3 * rainfall
```

Otherwise:
```
duration_h = clamp(duration, 0.05, 24.0)
intensity_mm_h = rainfall / duration_h
intensity_mm_h = clamp(intensity_mm_h, 0.1, RAIN_INTENSITY_MAX_MM_H)
duration_h is in hours; intensity_mm_h is in mm/h
if duration is unknown -> heuristic buckets

runoff_coeff:
  infil_eff = infil * (0.6 + 0.4*(1 - m_surface))
  base = max(0, (intensity_mm_h - infil_eff) / intensity_mm_h)
  + texture adjust (clay +0.05, sand -0.05)
  clamp 0..0.8

runoff_loss = rainfall * runoff_coeff
after_runoff = rainfall - runoff_loss

evap_loss:
  base_evap_mm_h = 0.1
  if T > 25: base_evap_mm_h += 0.02*(T-25)
  if T < 15: base_evap_mm_h -= 0.01*(15-T)
  base_evap_mm_h = max(base_evap_mm_h, 0)
  evap_duration_h = min(duration_h + 2, 6)
  evap_factor = (rainfall < 5) ? 1.5 : (rainfall > 20) ? 0.7 : 1.0
  evap_loss = base_evap_mm_h * evap_duration_h * evap_factor
  evap_loss = min(evap_loss, 0.3*after_runoff)

P_eff = max(after_runoff - evap_loss, 0)
```

Antecedent moisture:
- If override enabled -> use it
- Else: EMA of blended surface/root moisture (updated ~6h):
  ```
  m_surface = 1 - D_surface/TEW
  m_root = 1 - D/AWC_wet
  m = 0.7*m_surface + 0.3*m_root
  ```
- Else fallback 50%

Routing to buckets:
- Rain event sets surface_wet_fraction to 1.0 before routing.
- P_eff first reduces D_surface (up to its deficit)
- remaining P_eff reduces D_root (current_deficit_mm)

Daily AUTO uses hourly rain buckets when available:
- Implementation detail (if hourly bins are supported): remainder rainfall after incremental updates is subtracted from the most recent hourly bins first (preserves older intensity patterns).
- Each hour uses duration = 1h, intensity = hourly_mm, then P_eff is summed.
- Fallback: single 24h total if no hourly data.

---

## 14) ETc (crop evapotranspiration)
```
ET_root = ET0 * Kc_eff
ET_surface = ET0 * Ke
sun_factor = clamp(sun_exposure_pct/100, 0.3..1.0)
ET_root *= sun_factor
ET_surface *= sun_factor
ETc_total = ET_root + ET_surface
```
ET_root drives D (root deficit). ET_surface drives D_surface. ETc_total is
reported in UI; ECO does not change ET, only the management decision.

---

## 15) MAD / irrigation trigger (p adaptive)
Base:
```
p_base = plant.depletion_fraction_p
```

ETc adaptation (root transpiration only):
```
ETc_root = ET0 * Kc_eff * sun_factor   (no Ke)
p_etc = p_base + 0.04 * (5.0 - ETc_root)
p_etc clamp 0.1..0.8
```

Stress adjust (temp/humidity):
```
if Tmax > temp_opt_max + 5 => reduce p by up to 30%
if RH < 30% => reduce p by up to 20%
```

Final threshold:
```
MAD_mm = wetting_awc * p_adjusted
trigger if D >= MAD_mm
```

Additional guards:
- if D < 2 mm => no trigger
- if wetting_awc < 5 mm => no trigger

---

## 16) Volume computation (net, gross, liters)
Net requirement:
```
net_mm = D
```
Eco (on-demand mode only):
```
net_mm = net_mm * ECO_ETC_FACTOR
```

Efficiency + DU:
```
eff = method.efficiency_pct / 100 (default 0.8 if invalid)
DU = method.distribution_uniformity_pct / 100 (default 1.0)
gross_mm = net_mm / eff
gross_mm = gross_mm / DU
```

Area:
- area-based: area_m2 = channel.coverage.area_m2
- plant-based:
  - area_per_plant = row_spacing * plant_spacing
  - else area_per_plant = 1 / density
  - clamp area_per_plant to [0.002 .. 100]
  - total_area = area_per_plant * plant_count

Volume:
```
volume_L = gross_mm * area_m2   (1 mm over 1 m2 = 1 L)
```

Max volume:
```
if volume_L > max_volume_limit_l => cap and back-calc net/gross
```

---

## 17) Cycle/soak (application rate from flow)
Application rate:
```
flow_l_min = nominal_flow_ml_min / 1000
app_rate_mm_h = (flow_l_min * 60) / area_m2
```

Cycle/soak logic:
- if app_rate <= 1.2 * soil_infil => single cycle
- else:
  ```
  target_mm_h = 0.8 * soil_infil
  cycles = ceil(app_rate_mm_h / target_mm_h), clamp 2..6
  depth_per_cycle_mm = gross_mm / cycles
  duration_h = depth_per_cycle_mm / app_rate_mm_h
  duration_min = duration_h * 60
  soak_min = duration_min * factor (sand=2, loam=3, clay=4)
  duration_min clamp 5..60 min, soak_min clamp 10..240 min
  ```
Clamping is a practical constraint; total applied depth may be slightly adjusted by the runtime implementation.

If flow not available, app_rate uses DB min/max.

---

## 18) Realtime deficit update (AUTO)
Triggered periodically:
```
ET0 = ensemble(HS, PM)
ET0 = slew_asymmetric(ET0)
ET_root = ET0 * Kc_eff * sun_factor
D += ET_root * dt/86400
D_surface += ET0 * Ke * dt/86400 * sun_factor
```

AWC/RAW updated each tick using root depth and wetting_fraction.

---

## 19) Daily update (AUTO)
Runs once per day:
1) Compute AWC/RAW (root depth + wetting_fraction)
2) Compute P_eff from hourly rain history (if supported; remainder rainfall subtracted from most recent bins);
   otherwise fallback to 24h total
3) If P_eff > 0: set surface_wet_fraction to rain and update surface bucket
4) Route P_eff: fill D_surface first, then subtract remainder from D_root
   (avoid double count of incremental rain)
5) ET0 ensemble + slew
6) Compute Kc_eff and Ke; ET_root used for MAD, Ke for surface bucket
7) Update D_surface with evaporation
8) MAD threshold and decision (ECO increases MAD toward 1.0)
9) Compute volume (net/gross/DU), cap if needed
10) Persist to NVS

---

## 20) Incremental rain update
On tips:
```
delta_mm = pulses * mm_per_pulse
hourly_rate_mm_h from rain sensor (mm/h), duration_s is elapsed seconds since last apply
duration_h = 0
if hourly_rate_mm_h > 0.1 and delta_mm > 0:
  duration_h = clamp(delta_mm / hourly_rate_mm_h, 1.0/60.0, 1.0)
else if duration_s > 0:
  duration_h = clamp(duration_s / 3600.0, 1.0/60.0, 1.0)
P_eff = (duration_h > 0) ?
        calc_effective_precipitation(...duration_h...) :
        calc_effective_precipitation(...no timing...)
surface_wet_fraction <- rain event (1.0) and update surface bucket
P_to_surface = min(P_eff, D_surface)
D_surface -= P_to_surface
D -= max(P_eff - P_to_surface, 0)
```
Applied rain is tracked separately for surface and root to avoid double-counting
in the daily reconciliation; raw rainfall is tracked to compute remainder rainfall.

This prevents watering after mid-day rain.

---

## 21) Offline gap
If device was offline:
```
ET0_day from weekly climatology (history) or monthly default
ET_root_day = ET0_day * Kc_eff * sun_factor
sum for missed days (cap 30 days)
```
ECO management is applied to decision/target only; ET physics are unchanged.
Surface bucket is set to dry (D_surface = TEW).

---

## 22) After irrigation (real flow)
From flow sensor:
```
irrigation_mm = liters_delivered / area_m2
I_eff_root = irrigation_mm * eff_root
D -= I_eff_root
D_surface -= irrigation_mm * eff_surface (capped, zone-avg)
```

This is the *real* soil update, not planned volume.
Surface wet area state is bumped to wetting_fraction * DU (clamped) for irrigation events.
surface_wet_target uses wetting_fraction * DU (clamped).
eff_surface/eff_root:
- eff_surface = 1.0 (surface wetting uses applied depth; DU handled via surface_wet_target)
- eff_root = efficiency * DU

---

## 23) What the app outputs
Per channel:
- current deficit (mm)
- ET0, Kc, Ke, ETc
- volume liters (gross)
- cycle/soak (cycles, duration, soak)
- irrigation needed (MAD trigger)
- next timing (for schedule auto)

---

## 24) Important behaviors / notes
- App expects **zone area**, not wetted area.
- Wetting fraction reduces AWC/RAW; surface_wet_fraction can jump to 1.0 on rain and decays toward wetting_fraction * DU based on ET0.
- Irrigation events use wetting_fraction * DU for surface_wet_fraction to reflect distribution.
- Root deficit D uses ET_root (Kc_eff); Ke only updates D_surface.
- D_surface is zone-averaged; TEW/REW are scaled by surface_wet_fraction.
- Effective rain is routed to surface first, then root.
- Runoff uses blended surface/root moisture (surface-weighted).
- DU affects planned gross depth and root-effective refill; surface wet area uses DU
  via surface_wet_target = wetting_fraction * DU.
- Surface wetting uses applied depth (eff_surface = 1.0); efficiency applies to root refill.
- Canopy now affects Kc, not area.
- ET0 uses HS+PM ensemble with quality-weighting (AUTO and non-AUTO paths); if daily ET0 < 0.05 in non-AUTO balance update, fallback to monthly default (RO) or FAO56_DEFAULT_ET0.
- Slew is asymmetric; rises can be faster on heatwave/VPD.
- Antecedent moisture is auto-derived if no manual override.
- ECO mode increases MAD in AUTO, and scales net refill in on-demand Eco calculations.
- ECO mode increases MAD (management strategy) and does not change ET physics.

---

## 25) Key code entry points
- `fao56_calculate_irrigation_requirement()` main entry
- `fao56_realtime_update_deficit()` realtime D update
- `fao56_daily_update_deficit()` daily reconcile + decision
- `fao56_apply_rainfall_increment()` rain tips
- `fao56_reduce_deficit_after_irrigation()` real delivered volume

---

End of full explanation.
