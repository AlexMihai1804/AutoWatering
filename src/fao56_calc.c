/**
 * @file fao56_calc.c
 * @brief FAO-56 based irrigation calculation engine implementation
 */

#include "fao56_calc.h"
#include "watering_log.h"
#include "plant_db.h" // accessor prototypes for plant, soil, irrigation method
#include "rain_history.h"
#include "nvs_config.h"
#include "environmental_history.h"
#include "custom_soil_db.h"
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include "rtc.h"
#include "timezone.h"
#include "soil_moisture_config.h"
#include "pack_storage.h"

LOG_MODULE_REGISTER(fao56_calc, LOG_LEVEL_INF);

// Mathematical constants
#define PI 3.14159265359f
#define SOLAR_CONSTANT 0.0820f  // MJ m-2 min-1
#define STEFAN_BOLTZMANN 4.903e-9f  // MJ K-4 m-2 day-1

// Cache configuration constants
#define CACHE_MAX_AGE_SECONDS 3600      // 1 hour cache validity
#define ET0_CACHE_TOLERANCE 0.5f        // Temperature tolerance for ET0 cache hits
#define HUMIDITY_CACHE_TOLERANCE 5.0f   // Humidity tolerance for cache hits
#define PRESSURE_CACHE_TOLERANCE 2.0f   // Pressure tolerance for cache hits

// Heuristic ET0 assumption constants (used when only temp+humidity available)
#define HEURISTIC_ET0_COEFF        0.045f   // Multiplier for (Tmean + offset) * sqrt(VPD)
#define HEURISTIC_ET0_TEMP_OFFSET  20.0f    // Temperature offset (deg C)
#define HEURISTIC_ET0_VPD_FLOOR    0.05f    // Minimum VPD to avoid zeroing ET0
#define HEURISTIC_ET0_MIN          0.5f     // Clamp lower bound (mm/day)
#define HEURISTIC_ET0_MAX          6.0f     // Clamp upper bound (mm/day)
#define ET0_SLEW_MAX_INC_MM_DAY    5.0f     // Max daily ET0 increase (mm/day)
#define ET0_SLEW_MAX_DEC_MM_DAY    2.0f     // Max daily ET0 decrease (mm/day)
#define ET0_SLEW_MAX_INC_HOT_MM_DAY 7.0f    // Hot/windy override for fast ET0 rises
#define ET0_SLEW_RESET_SECONDS     (3U * 86400U) // Reset slew window after long gaps
#define ET0_SLEW_MIN_STEP_MM_DAY   0.1f     // Avoid overly small slew steps
#define ECO_ETC_FACTOR             0.7f     // Eco mode management scaling factor
#define RAIN_INTENSITY_MAX_MM_H    100.0f   // Safety clamp for intensity-based runoff
#define ET0_SLEW_HEATWAVE_TMAX_C   33.0f    // Heatwave trigger for faster ET0 ramp
#define ET0_SLEW_HEATWAVE_VPD_KPA  2.0f     // VPD trigger for faster ET0 ramp
#define ET0_ENSEMBLE_MAX_WEIGHT    0.85f    // Max PM weight in ET0 ensemble
#define FAO56_SURFACE_LAYER_M      0.10f    // 10 cm surface layer for TEW
#define FAO56_SURFACE_TEW_MIN_MM   4.0f
#define FAO56_SURFACE_TEW_MAX_MM   15.0f
#define FAO56_SURFACE_REW_FRAC     0.5f
#define FAO56_SURFACE_REW_MIN_MM   2.0f
#define FAO56_SURFACE_REW_MAX_MM   8.0f
#define FAO56_KE_MAX_BASE          0.90f
#define FAO56_KE_CANOPY_REDUCTION  0.5f     // Max 50% reduction at full canopy
#define FAO56_SURFACE_WET_DECAY_SECONDS (18U * 3600U)
#define FAO56_SURFACE_WET_DECAY_ET0_MM  3.0f
#define FAO56_SURFACE_WET_RAIN_FRACTION 1.0f
#define FAO56_MAD_ETC_REF_MM_DAY   5.0f
#define FAO56_MAD_ETC_ADJ_COEFF    0.04f
#define FAO56_MAD_MIN_FRACTION     0.1f
#define FAO56_MAD_MAX_FRACTION     0.8f
#define FAO56_WF_MIN               0.10f
#define FAO56_WF_MAX               1.00f
#define FAO56_WF_DEPTH_LOG_COEFF   0.15f
#define FAO56_WF_SLEW_MAX_FRAC_PER_WEEK 0.10f
#define FAO56_WF_SLEW_MAX_FRAC_PER_DAY (FAO56_WF_SLEW_MAX_FRAC_PER_WEEK / 7.0f)
#define FAO56_ET0_PM_DT_MIN_C      1.0f
#define FAO56_ET0_PM_RATIO_MIN     0.30f
#define FAO56_ET0_PM_RATIO_MAX     2.50f

// Assumed meteorological constants (no wind or solar sensors present)
#define ASSUMED_WIND_SPEED_M_S     2.0f     // Typical moderate daytime wind speed for reference ET
#define ASSUMED_SUNSHINE_RATIO     0.50f    // Fallback sunshine ratio (n/N) when delta-T is unavailable
#define ASSUMED_ALBEDO             0.23f    // Standard grass reference surface albedo (FAO-56)
#define STANDARD_ATMOS_PRESSURE_KPA 101.3f  // Sea level standard pressure used if sensor invalid
#define ET0_ABSOLUTE_MAX_MM_DAY    15.0f    // Hard safety clamp for ET0 extremes
#define HARGREAVES_RS_COEFF        0.16f    // kRs coefficient for Rs estimation (inland default)
#define FAO56_DEFAULT_ET0_MM_DAY   3.0f     // Conservative default ET0 fallback (mm/day)
#define FAO56_CLIMATOLOGY_WEEKS    53       // 52 weeks + 1 partial week bucket
#define FAO56_CUSTOM_SOIL_CACHE_TIMEOUT_MS (5 * 60 * 1000)

// NOTE: All above assumption constants are centralized to allow easy future tuning
//       and transparent audit trail (explicitly replacing former in-line literals).

// Global cache instance
static fao56_calculation_cache_t calculation_cache = {0};

static const float fao56_default_et0_monthly_ro[12] = {
    0.6f, 0.9f, 1.6f, 2.6f, 3.6f, 4.5f,
    5.0f, 4.6f, 3.2f, 2.0f, 1.0f, 0.6f
};

static custom_soil_entry_t s_custom_soil_cache[WATERING_CHANNELS_COUNT];
static soil_enhanced_data_t s_custom_soil_data_cache[WATERING_CHANNELS_COUNT];
static bool s_custom_soil_cache_valid[WATERING_CHANNELS_COUNT] = {false};
static uint32_t s_custom_soil_cache_timestamp[WATERING_CHANNELS_COUNT] = {0};

static float s_et0_slew_last_mm_day[WATERING_CHANNELS_COUNT] = {0.0f};
static uint32_t s_et0_slew_last_time_s[WATERING_CHANNELS_COUNT] = {0U};
static bool s_et0_slew_valid[WATERING_CHANNELS_COUNT] = {false};
static float s_antecedent_moisture_ema[WATERING_CHANNELS_COUNT] = {0.0f};
static bool s_antecedent_moisture_valid[WATERING_CHANNELS_COUNT] = {false};
static uint32_t s_antecedent_moisture_last_update_s[WATERING_CHANNELS_COUNT] = {0U};
static float s_wetting_fraction_last[WATERING_CHANNELS_COUNT] = {0.0f};
static uint32_t s_wetting_fraction_last_time_s[WATERING_CHANNELS_COUNT] = {0U};
static float s_rain_applied_raw_mm[WATERING_CHANNELS_COUNT] = {0.0f};

/* Plant cache for pack storage plants converted to plant_full_data_t format */
static plant_full_data_t s_plant_cache[WATERING_CHANNELS_COUNT];
static uint16_t s_plant_cache_id[WATERING_CHANNELS_COUNT] = {0};

/**
 * @brief Get plant data for a channel from pack storage (unified system)
 * 
 * All plants are stored in pack storage (provisioned from ROM at first boot).
 * This function loads plant data and caches it for efficient access.
 * 
 * @param channel Pointer to watering channel
 * @param channel_id Channel ID (for caching)
 * @return Pointer to plant_full_data_t (cached), NULL on error
 */
const plant_full_data_t *fao56_get_channel_plant(const watering_channel_t *channel, 
                                                  uint8_t channel_id)
{
    if (!channel || channel_id >= WATERING_CHANNELS_COUNT) {
        return NULL;
    }
    
    /* plant_id: 0 = not set, 1+ = plant in pack storage */
    if (channel->plant_id == 0) {
        LOG_ERR("No plant configured for channel %u (plant_id=0)", channel_id);
        return NULL;
    }
    
    /* Check cache first */
    if (s_plant_cache_id[channel_id] == channel->plant_id) {
        return &s_plant_cache[channel_id];
    }
    
    /* Load from pack storage (all plants are there after provisioning) */
    pack_plant_v1_t pack_plant;
    pack_result_t res = pack_storage_get_plant(channel->plant_id, &pack_plant);
    if (res != PACK_RESULT_SUCCESS) {
        LOG_ERR("Failed to load plant %u for channel %u: %d", 
                channel->plant_id, channel_id, res);
        return NULL;
    }
    
    /* Convert pack_plant_v1_t to plant_full_data_t */
    plant_full_data_t *cached = &s_plant_cache[channel_id];
    memset(cached, 0, sizeof(*cached));
    
    /* Note: ROM uses common_name_en (const char*), cache can't store string directly.
     * For FAO-56 calculations the name isn't used, so we leave it NULL.
     * If name is needed, we'd need a separate string buffer.
     */
    
    /* Crop coefficients (both use x1000) */
    cached->kc_ini_x1000 = pack_plant.kc_ini_x1000;
    cached->kc_dev_x1000 = pack_plant.kc_dev_x1000;
    cached->kc_mid_x1000 = pack_plant.kc_mid_x1000;
    cached->kc_end_x1000 = pack_plant.kc_end_x1000;
    
    /* Growth stages */
    cached->stage_days_ini = pack_plant.stage_days_ini;
    cached->stage_days_dev = pack_plant.stage_days_dev;
    cached->stage_days_mid = pack_plant.stage_days_mid;
    cached->stage_days_end = pack_plant.stage_days_end;
    cached->growth_cycle = pack_plant.growth_cycle;
    
    /* Root depth (pack uses mm, ROM uses m×1000 - same numeric value!) */
    cached->root_depth_min_m_x1000 = pack_plant.root_depth_min_mm;
    cached->root_depth_max_m_x1000 = pack_plant.root_depth_max_mm;
    
    /* Depletion fraction */
    cached->depletion_fraction_p_x1000 = pack_plant.depletion_fraction_p_x1000;
    
    /* Canopy cover (pack: canopy_max_x1000, ROM: canopy_cover_max_frac_x1000) */
    cached->canopy_cover_max_frac_x1000 = pack_plant.canopy_max_x1000;
    if (cached->canopy_cover_max_frac_x1000 == 0) {
        cached->canopy_cover_max_frac_x1000 = 800; /* Default 80% */
    }
    
    /* Temperature limits */
    cached->frost_tolerance_c = pack_plant.frost_tolerance_c;
    cached->temp_opt_min_c = pack_plant.temp_opt_min_c;
    cached->temp_opt_max_c = pack_plant.temp_opt_max_c;
    if (cached->temp_opt_max_c == 0) {
        cached->temp_opt_max_c = 30; /* Default */
    }
    
    /* Spacing (pack uses mm, ROM uses m×1000 - same numeric value!) */
    cached->spacing_row_m_x1000 = pack_plant.spacing_row_mm;
    cached->spacing_plant_m_x1000 = pack_plant.spacing_plant_mm;
    if (cached->spacing_row_m_x1000 == 0) {
        cached->spacing_row_m_x1000 = 500; /* 0.5m default */
    }
    if (cached->spacing_plant_m_x1000 == 0) {
        cached->spacing_plant_m_x1000 = 300; /* 0.3m default */
    }
    
    /* Default density (pack: density_x100, ROM: default_density_plants_m2_x100) */
    cached->default_density_plants_m2_x100 = pack_plant.density_x100;
    if (cached->default_density_plants_m2_x100 == 0) {
        cached->default_density_plants_m2_x100 = 400; /* 4 plants/m² default */
    }
    
    /* Irrigation method */
    cached->typ_irrig_method_id = pack_plant.typ_irrig_method_id;
    
    /* Update cache ID */
    s_plant_cache_id[channel_id] = channel->plant_id;
    
    LOG_DBG("Loaded plant %u for channel %u: Kc=%.2f/%.2f/%.2f",
            channel->plant_id, channel_id,
            (double)(cached->kc_ini_x1000 / 1000.0f),
            (double)(cached->kc_mid_x1000 / 1000.0f),
            (double)(cached->kc_end_x1000 / 1000.0f));
    
    return cached;
}

static float fao56_calc_plant_irrigated_area_m2(
    const plant_full_data_t *plant,
    uint16_t plant_count,
    float *area_per_plant_m2_out,
    float *canopy_factor_out,
    bool log_details
);
static float calc_saturation_vapor_pressure(float temp_c);
static float fao56_apply_canopy_to_kc(const plant_full_data_t *plant,
                                      float kc,
                                      uint16_t days_after_planting);
static float fao56_get_dynamic_canopy_factor(const plant_full_data_t *plant,
                                             uint16_t days_after_planting);
static bool fao56_get_vpd_kpa(const environmental_data_t *env, float *vpd_out);
static void fao56_get_et0_slew_limits(const environmental_data_t *env,
                                      float *max_inc_mm_day,
                                      float *max_dec_mm_day);
static float fao56_apply_et0_slew(uint8_t channel_id,
                                  float et0_mm_day,
                                  uint32_t now_s,
                                  float max_inc_mm_day,
                                  float max_dec_mm_day);
static float fao56_calc_et0_ensemble(const environmental_data_t *env,
                                     float latitude_rad,
                                     uint16_t day_of_year,
                                     float *et0_hs_out,
                                     float *et0_pm_out);
static float fao56_apply_wetting_fraction_slew(uint8_t channel_id, float wetting_fraction);
static void fao56_update_surface_bucket(water_balance_t *balance,
                                        const soil_enhanced_data_t *soil,
                                        float surface_wet_fraction);
static float fao56_get_surface_wet_fraction(water_balance_t *balance,
                                            float target_wet_fraction,
                                            float et0_mm_day);
static void fao56_apply_surface_wet_event(water_balance_t *balance,
                                          float event_fraction);
static void fao56_rescale_deficit_for_awc_change(water_balance_t *balance,
                                                 float new_wetting_awc_mm);
static float fao56_route_effective_precipitation(water_balance_t *balance,
                                                 float effective_mm);
static void fao56_get_efficiency_split(const irrigation_method_data_t *method,
                                       float wetting_fraction,
                                       float *eff_surface,
                                       float *eff_root);
static float fao56_calc_surface_tew_mm(const soil_enhanced_data_t *soil);
static float fao56_calc_surface_rew_mm(const soil_enhanced_data_t *soil, float tew_mm);
static float fao56_get_surface_wet_target(const irrigation_method_data_t *method,
                                          float wetting_fraction);
static float fao56_calc_ke(const water_balance_t *balance,
                           float tew_mm,
                           float rew_mm,
                           const irrigation_method_data_t *method,
                           const plant_full_data_t *plant,
                           uint16_t days_after_planting);
static float fao56_get_effective_wetting_fraction(const irrigation_method_data_t *method,
                                                  const soil_enhanced_data_t *soil,
                                                  const plant_full_data_t *plant);
static float fao56_get_antecedent_moisture_pct(uint8_t channel_id,
                                               const water_balance_t *balance);
static float fao56_calc_effective_rain_hourly(uint32_t now_ts,
                                              float rainfall_remainder_mm,
                                              const soil_enhanced_data_t *soil,
                                              const irrigation_method_data_t *irrigation_method,
                                              float antecedent_moisture_pct,
                                              float temperature_c);

static uint16_t fao56_calc_day_of_year_from_date(uint16_t year, uint8_t month, uint8_t day)
{
    bool is_leap = ((year % 4 == 0 && year % 100 != 0) ||
                    (year % 400 == 0));
    static const uint8_t month_lengths[12] = {31, 28, 31, 30, 31, 30,
                                              31, 31, 30, 31, 30, 31};

    uint16_t day_of_year = day;
    for (uint8_t m = 1; m < month; m++) {
        day_of_year += month_lengths[m - 1];
        if (m == 2 && is_leap) {
            day_of_year += 1;
        }
    }

    return day_of_year;
}

static bool fao56_get_vpd_kpa(const environmental_data_t *env, float *vpd_out)
{
    if (!env || !vpd_out) {
        return false;
    }

    if (env->derived_values_calculated) {
        float vpd = env->saturation_vapor_pressure_kpa - env->vapor_pressure_kpa;
        if (vpd < 0.0f) vpd = 0.0f;
        *vpd_out = vpd;
        return true;
    }

    if (!env->temp_valid || !env->humidity_valid) {
        return false;
    }

    float es = calc_saturation_vapor_pressure(env->air_temp_mean_c);
    float ea = es * env->rel_humidity_pct / 100.0f;
    float vpd = es - ea;
    if (vpd < 0.0f) vpd = 0.0f;
    *vpd_out = vpd;
    return true;
}

static void fao56_get_et0_slew_limits(const environmental_data_t *env,
                                      float *max_inc_mm_day,
                                      float *max_dec_mm_day)
{
    if (!max_inc_mm_day || !max_dec_mm_day) {
        return;
    }

    float inc = ET0_SLEW_MAX_INC_MM_DAY;
    float dec = ET0_SLEW_MAX_DEC_MM_DAY;

    if (env && env->temp_valid) {
        float vpd = 0.0f;
        bool vpd_valid = fao56_get_vpd_kpa(env, &vpd);
        if (env->air_temp_max_c >= ET0_SLEW_HEATWAVE_TMAX_C ||
            (vpd_valid && vpd >= ET0_SLEW_HEATWAVE_VPD_KPA)) {
            inc = ET0_SLEW_MAX_INC_HOT_MM_DAY;
        }
    }

    *max_inc_mm_day = inc;
    *max_dec_mm_day = dec;
}

static float fao56_apply_et0_slew(uint8_t channel_id,
                                  float et0_mm_day,
                                  uint32_t now_s,
                                  float max_inc_mm_day,
                                  float max_dec_mm_day)
{
    if (channel_id >= WATERING_CHANNELS_COUNT) {
        return et0_mm_day;
    }

    if (!s_et0_slew_valid[channel_id]) {
        s_et0_slew_valid[channel_id] = true;
        s_et0_slew_last_time_s[channel_id] = now_s;
        s_et0_slew_last_mm_day[channel_id] = et0_mm_day;
        return et0_mm_day;
    }

    uint32_t elapsed_s = now_s - s_et0_slew_last_time_s[channel_id];
    if (elapsed_s == 0U || elapsed_s > ET0_SLEW_RESET_SECONDS) {
        s_et0_slew_last_time_s[channel_id] = now_s;
        s_et0_slew_last_mm_day[channel_id] = et0_mm_day;
        return et0_mm_day;
    }

    float max_inc = max_inc_mm_day * ((float)elapsed_s / 86400.0f);
    float max_dec = max_dec_mm_day * ((float)elapsed_s / 86400.0f);
    float min_step = ET0_SLEW_MIN_STEP_MM_DAY * ((float)elapsed_s / 86400.0f);
    if (max_inc < min_step) {
        max_inc = min_step;
    }
    if (max_dec < min_step) {
        max_dec = min_step;
    }

    float last = s_et0_slew_last_mm_day[channel_id];
    if (et0_mm_day > last) {
        float max_et0 = last + max_inc;
        if (et0_mm_day > max_et0) {
            et0_mm_day = max_et0;
        }
    } else if (et0_mm_day < last) {
        float min_et0 = last - max_dec;
        if (et0_mm_day < min_et0) {
            et0_mm_day = min_et0;
        }
    }

    s_et0_slew_last_mm_day[channel_id] = et0_mm_day;
    s_et0_slew_last_time_s[channel_id] = now_s;

    return et0_mm_day;
}

static bool fao56_get_local_datetime_from_timestamp(uint32_t timestamp, rtc_datetime_t *datetime)
{
    if (!datetime) {
        return false;
    }

    if (timezone_unix_to_rtc_local(timestamp, datetime) == 0) {
        return true;
    }

    return timezone_unix_to_rtc_utc(timestamp, datetime) == 0;
}

static bool fao56_get_day_of_year_from_timestamp(uint32_t timestamp, uint16_t *day_of_year)
{
    rtc_datetime_t dt;
    if (!day_of_year || !fao56_get_local_datetime_from_timestamp(timestamp, &dt)) {
        return false;
    }

    *day_of_year = fao56_calc_day_of_year_from_date(dt.year, dt.month, dt.day);
    return true;
}

static uint8_t fao56_get_month_from_timestamp(uint32_t timestamp)
{
    rtc_datetime_t dt;
    if (!fao56_get_local_datetime_from_timestamp(timestamp, &dt)) {
        return 0;
    }
    return dt.month;
}

static float fao56_get_default_et0_for_month(uint8_t month)
{
    if (month < 1 || month > 12) {
        return FAO56_DEFAULT_ET0_MM_DAY;
    }
    return fao56_default_et0_monthly_ro[month - 1];
}

static uint16_t fao56_get_days_after_planting(const watering_channel_t *channel, uint32_t current_time)
{
    if (!channel || channel->planting_date_unix == 0 || current_time <= channel->planting_date_unix) {
        return 0;
    }
    return (uint16_t)((current_time - channel->planting_date_unix) / 86400);
}

static float fao56_get_kc_base_for_day(const plant_full_data_t *plant, uint16_t days_after_planting)
{
    phenological_stage_t stage = calc_phenological_stage(plant, days_after_planting);
    return calc_crop_coefficient(plant, stage, days_after_planting);
}

static float fao56_get_kc_for_day(const plant_full_data_t *plant, uint16_t days_after_planting)
{
    float kc_base = fao56_get_kc_base_for_day(plant, days_after_planting);
    return fao56_apply_canopy_to_kc(plant, kc_base, days_after_planting);
}

static float fao56_get_root_depth_m(const plant_full_data_t *plant, uint16_t days_after_planting)
{
    return calc_current_root_depth(plant, days_after_planting);
}

static float fao56_calc_et0_ensemble(const environmental_data_t *env,
                                     float latitude_rad,
                                     uint16_t day_of_year,
                                     float *et0_hs_out,
                                     float *et0_pm_out)
{
    if (!env) {
        return 0.0f;
    }
    if (!env->temp_valid) {
        return 0.0f;
    }

    float et0_hs = calc_et0_hargreaves_samani(env, latitude_rad, day_of_year);
    float et0_pm = 0.0f;
    float et0_pm_raw = 0.0f;
    bool pm_valid = env->temp_valid && env->humidity_valid;
    if (pm_valid) {
        et0_pm = calc_et0_penman_monteith(env, latitude_rad, day_of_year, &et0_pm_raw);
        if (et0_pm_raw <= 0.01f || et0_pm_raw > ET0_ABSOLUTE_MAX_MM_DAY * 1.2f) {
            pm_valid = false;
        }
    }

    if (et0_hs_out) {
        *et0_hs_out = et0_hs;
    }
    if (et0_pm_out) {
        *et0_pm_out = et0_pm;
    }

    if (!pm_valid) {
        return et0_hs;
    }

    float weight = 0.5f;
    if (env->pressure_valid) {
        weight += 0.1f;
    }
    if (env->humidity_valid) {
        weight += 0.2f;
    }
    if (env->data_quality >= 80) {
        weight += 0.1f;
    }
    if (env->temp_valid && et0_hs > 0.1f) {
        float temp_range = env->air_temp_max_c - env->air_temp_min_c;
        float ratio = et0_pm / et0_hs;
        if (ratio < FAO56_ET0_PM_RATIO_MIN || ratio > FAO56_ET0_PM_RATIO_MAX) {
            weight *= 0.6f;
        }
        if (temp_range < FAO56_ET0_PM_DT_MIN_C && et0_pm > et0_hs + 1.0f) {
            weight *= 0.6f;
        }
    }
    if (env->derived_values_calculated && env->vapor_pressure_kpa > 0.0f &&
        env->dewpoint_temp_c > env->air_temp_max_c + 0.5f) {
        weight *= 0.5f;
    }
    if (weight > ET0_ENSEMBLE_MAX_WEIGHT) {
        weight = ET0_ENSEMBLE_MAX_WEIGHT;
    }
    if (weight < 0.2f) {
        weight = 0.2f;
    }

    return weight * et0_pm + (1.0f - weight) * et0_hs;
}

static float fao56_apply_canopy_to_kc(const plant_full_data_t *plant,
                                      float kc,
                                      uint16_t days_after_planting)
{
    if (!plant) {
        return kc;
    }

    float canopy_factor = fao56_get_dynamic_canopy_factor(plant, days_after_planting);
    if (canopy_factor <= 0.0f) {
        return kc;
    }

    float kc_ini = plant->kc_ini_x1000 / 1000.0f;
    float kc_eff = kc_ini + (kc - kc_ini) * canopy_factor;
    if (kc < kc_ini && kc_eff > kc) {
        kc_eff = kc;
    }
    if (kc_eff < 0.1f) kc_eff = 0.1f;
    if (kc_eff > 2.0f) kc_eff = 2.0f;

    return kc_eff;
}

static float fao56_get_dynamic_canopy_factor(const plant_full_data_t *plant,
                                             uint16_t days_after_planting)
{
    if (!plant) {
        return 0.0f;
    }

    float canopy_max = plant->canopy_cover_max_frac_x1000 / 1000.0f;
    if (canopy_max <= 0.0f) {
        return 0.0f;
    }
    if (canopy_max > 1.0f) {
        canopy_max = 1.0f;
    }

    uint16_t stage_1_end = plant->stage_days_ini;
    uint16_t stage_2_end = stage_1_end + plant->stage_days_dev;
    float progress = 1.0f;

    if (plant->stage_days_dev == 0U) {
        progress = (days_after_planting <= stage_1_end) ? 0.0f : 1.0f;
    } else if (days_after_planting <= stage_1_end) {
        progress = 0.0f;
    } else if (days_after_planting >= stage_2_end) {
        progress = 1.0f;
    } else {
        progress = (float)(days_after_planting - stage_1_end) /
                   (float)plant->stage_days_dev;
        if (progress < 0.0f) progress = 0.0f;
        if (progress > 1.0f) progress = 1.0f;
    }

    float canopy_factor = canopy_max * progress;
    if (canopy_factor < 0.0f) canopy_factor = 0.0f;
    if (canopy_factor > 1.0f) canopy_factor = 1.0f;
    return canopy_factor;
}

const soil_enhanced_data_t *fao56_get_channel_soil(uint8_t channel_id, const watering_channel_t *channel)
{
    if (channel_id >= WATERING_CHANNELS_COUNT) {
        return NULL;
    }

    const watering_channel_t *resolved = channel;
    if (!resolved) {
        watering_channel_t *fetched = NULL;
        if (watering_get_channel(channel_id, &fetched) != WATERING_SUCCESS || !fetched) {
            return NULL;
        }
        resolved = fetched;
    }

    if (resolved->soil_config.use_custom_soil) {
        uint32_t now_ms = k_uptime_get_32();
        if (s_custom_soil_cache_valid[channel_id] &&
            (now_ms - s_custom_soil_cache_timestamp[channel_id]) < FAO56_CUSTOM_SOIL_CACHE_TIMEOUT_MS) {
            return &s_custom_soil_data_cache[channel_id];
        }

        custom_soil_entry_t entry;
        watering_error_t err = custom_soil_db_read(channel_id, &entry);
        if (err == WATERING_SUCCESS) {
            s_custom_soil_cache[channel_id] = entry;
            watering_error_t conv = custom_soil_db_to_enhanced_format(
                &s_custom_soil_cache[channel_id],
                &s_custom_soil_data_cache[channel_id]);
            if (conv == WATERING_SUCCESS) {
                s_custom_soil_cache_valid[channel_id] = true;
                s_custom_soil_cache_timestamp[channel_id] = now_ms;
                return &s_custom_soil_data_cache[channel_id];
            }
            LOG_WRN("Custom soil conversion failed for channel %u (err=%d)", channel_id, conv);
        } else {
            LOG_DBG("Custom soil unavailable for channel %u (err=%d)", channel_id, err);
        }
    }

    return soil_db_get_by_index(resolved->soil_db_index);
}

static float fao56_calc_surface_tew_mm(const soil_enhanced_data_t *soil)
{
    float tew_mm = 0.0f;
    if (soil) {
        float fc = soil->fc_pctvol_x100 / 100.0f;
        float wp = soil->pwp_pctvol_x100 / 100.0f;
        if (fc > 0.0f && wp >= 0.0f && fc > wp) {
            tew_mm = 1000.0f * FAO56_SURFACE_LAYER_M * (fc - wp);
        } else if (soil->awc_mm_per_m > 0.0f) {
            tew_mm = soil->awc_mm_per_m * FAO56_SURFACE_LAYER_M;
        }
    }
    if (tew_mm <= 0.0f) {
        tew_mm = 8.0f;
    }
    if (tew_mm < FAO56_SURFACE_TEW_MIN_MM) tew_mm = FAO56_SURFACE_TEW_MIN_MM;
    if (tew_mm > FAO56_SURFACE_TEW_MAX_MM) tew_mm = FAO56_SURFACE_TEW_MAX_MM;
    return tew_mm;
}

static float fao56_calc_surface_rew_mm(const soil_enhanced_data_t *soil, float tew_mm)
{
    float rew_mm = tew_mm * FAO56_SURFACE_REW_FRAC;
    if (soil && soil->texture[0] != '\0') {
        if (strstr(soil->texture, "Sand") || strstr(soil->texture, "sand")) {
            rew_mm = 3.0f;
        } else if (strstr(soil->texture, "Clay") || strstr(soil->texture, "clay")) {
            rew_mm = 8.0f;
        } else if (strstr(soil->texture, "Loam") || strstr(soil->texture, "loam")) {
            rew_mm = 6.0f;
        }
    }
    if (rew_mm < FAO56_SURFACE_REW_MIN_MM) rew_mm = FAO56_SURFACE_REW_MIN_MM;
    if (rew_mm > FAO56_SURFACE_REW_MAX_MM) rew_mm = FAO56_SURFACE_REW_MAX_MM;
    if (rew_mm > tew_mm) rew_mm = tew_mm;
    return rew_mm;
}

static float fao56_apply_wetting_fraction_slew(uint8_t channel_id, float wetting_fraction)
{
    if (channel_id >= WATERING_CHANNELS_COUNT) {
        return wetting_fraction;
    }

    if (wetting_fraction < FAO56_WF_MIN) wetting_fraction = FAO56_WF_MIN;
    if (wetting_fraction > FAO56_WF_MAX) wetting_fraction = FAO56_WF_MAX;

    uint32_t now_s = k_uptime_get_32() / 1000U;
    float last = s_wetting_fraction_last[channel_id];
    uint32_t last_s = s_wetting_fraction_last_time_s[channel_id];

    if (last_s == 0U || last <= 0.0f) {
        s_wetting_fraction_last[channel_id] = wetting_fraction;
        s_wetting_fraction_last_time_s[channel_id] = now_s;
        return wetting_fraction;
    }

    uint32_t dt_s = (now_s >= last_s) ? (now_s - last_s) : 0U;
    if (dt_s > 0U) {
        float max_delta = FAO56_WF_SLEW_MAX_FRAC_PER_DAY * ((float)dt_s / 86400.0f);
        float delta = wetting_fraction - last;
        if (fabsf(delta) > max_delta) {
            wetting_fraction = last + (delta > 0.0f ? max_delta : -max_delta);
        }
    } else {
        wetting_fraction = last;
    }

    if (wetting_fraction < FAO56_WF_MIN) wetting_fraction = FAO56_WF_MIN;
    if (wetting_fraction > FAO56_WF_MAX) wetting_fraction = FAO56_WF_MAX;

    s_wetting_fraction_last[channel_id] = wetting_fraction;
    s_wetting_fraction_last_time_s[channel_id] = now_s;
    return wetting_fraction;
}

static float fao56_get_surface_wet_fraction(water_balance_t *balance,
                                            float target_wet_fraction,
                                            float et0_mm_day)
{
    if (!balance) {
        return target_wet_fraction;
    }

    if (target_wet_fraction < 0.0f) target_wet_fraction = 0.0f;
    if (target_wet_fraction > 1.0f) target_wet_fraction = 1.0f;

    uint32_t now_s = k_uptime_get_32() / 1000U;
    float current = balance->surface_wet_fraction;
    uint32_t last_s = balance->surface_wet_update_s;

    if (last_s == 0U || current <= 0.0f) {
        balance->surface_wet_fraction = target_wet_fraction;
        balance->surface_wet_update_s = now_s;
        return target_wet_fraction;
    }

    uint32_t dt_s = (now_s >= last_s) ? (now_s - last_s) : 0U;
    if (dt_s > 0U) {
        if (et0_mm_day > 0.05f) {
            float et0_cum = et0_mm_day * ((float)dt_s / 86400.0f);
            float decay = expf(-et0_cum / FAO56_SURFACE_WET_DECAY_ET0_MM);
            current = target_wet_fraction + (current - target_wet_fraction) * decay;
        } else {
            float alpha = expf(-((float)dt_s) / (float)FAO56_SURFACE_WET_DECAY_SECONDS);
            current = target_wet_fraction + (current - target_wet_fraction) * alpha;
        }
        balance->surface_wet_fraction = current;
        balance->surface_wet_update_s = now_s;
    }

    if (balance->surface_wet_fraction < 0.0f) {
        balance->surface_wet_fraction = 0.0f;
    }
    if (balance->surface_wet_fraction > 1.0f) {
        balance->surface_wet_fraction = 1.0f;
    }

    return balance->surface_wet_fraction;
}

static void fao56_apply_surface_wet_event(water_balance_t *balance,
                                          float event_fraction)
{
    if (!balance) {
        return;
    }

    if (event_fraction < 0.0f) event_fraction = 0.0f;
    if (event_fraction > 1.0f) event_fraction = 1.0f;

    balance->surface_wet_fraction = event_fraction;
    balance->surface_wet_update_s = k_uptime_get_32() / 1000U;
}

static void fao56_update_surface_bucket(water_balance_t *balance,
                                        const soil_enhanced_data_t *soil,
                                        float surface_wet_fraction)
{
    if (!balance) {
        return;
    }

    float base_tew_mm = fao56_calc_surface_tew_mm(soil);
    if (surface_wet_fraction < 0.0f) surface_wet_fraction = 0.0f;
    if (surface_wet_fraction > 1.0f) surface_wet_fraction = 1.0f;
    balance->surface_wet_fraction = surface_wet_fraction;

    float tew_mm = base_tew_mm * surface_wet_fraction;
    if (tew_mm < 0.0f) tew_mm = 0.0f;
    float base_rew_mm = fao56_calc_surface_rew_mm(soil, base_tew_mm);
    float rew_mm = base_rew_mm * surface_wet_fraction;
    const float eps = 1e-3f;
    if (tew_mm <= eps) {
        balance->surface_tew_mm = 0.0f;
        balance->surface_rew_mm = 0.0f;
        balance->surface_deficit_mm = 0.0f;
        return;
    }
    if (rew_mm > tew_mm * 0.9f) {
        rew_mm = tew_mm * 0.9f;
        LOG_DBG("Surface REW clamped to TEW for channel bucket (TEW=%.2f)", (double)tew_mm);
    }
    if (rew_mm < 0.0f) rew_mm = 0.0f;

    float old_tew_mm = balance->surface_tew_mm;
    if (fabsf(old_tew_mm - tew_mm) > 0.01f) {
        float frac = 1.0f;
        if (old_tew_mm > eps) {
            frac = balance->surface_deficit_mm / old_tew_mm;
            if (frac < 0.0f) frac = 0.0f;
            if (frac > 1.0f) frac = 1.0f;
        }
        balance->surface_deficit_mm = frac * tew_mm;
    }

    balance->surface_tew_mm = tew_mm;
    balance->surface_rew_mm = rew_mm;

    if (balance->surface_deficit_mm < 0.0f ||
        balance->surface_deficit_mm > balance->surface_tew_mm ||
        (balance->last_update_time == 0U && balance->surface_deficit_mm == 0.0f)) {
        balance->surface_deficit_mm = balance->surface_tew_mm;
        LOG_DBG("Surface deficit clamped to TEW (TEW=%.2f)", (double)balance->surface_tew_mm);
    }
}

static void fao56_rescale_deficit_for_awc_change(water_balance_t *balance,
                                                 float new_wetting_awc_mm)
{
    if (!balance) {
        return;
    }

    float old_awc_mm = balance->wetting_awc_mm;
    if (old_awc_mm > 0.0f && new_wetting_awc_mm > 0.0f &&
        fabsf(new_wetting_awc_mm - old_awc_mm) > 0.01f) {
        float frac = balance->current_deficit_mm / old_awc_mm;
        if (frac < 0.0f) frac = 0.0f;
        if (frac > 1.0f) frac = 1.0f;
        balance->current_deficit_mm = frac * new_wetting_awc_mm;
    }

    balance->wetting_awc_mm = new_wetting_awc_mm;
}

static float fao56_route_effective_precipitation(water_balance_t *balance,
                                                 float effective_mm)
{
    if (!balance || effective_mm <= 0.0f) {
        return 0.0f;
    }

    float surface_recharge = 0.0f;
    if (balance->surface_tew_mm > 0.0f && balance->surface_deficit_mm > 0.0f) {
        surface_recharge = effective_mm;
        if (surface_recharge > balance->surface_deficit_mm) {
            surface_recharge = balance->surface_deficit_mm;
        }
        balance->surface_deficit_mm -= surface_recharge;
        if (balance->surface_deficit_mm < 0.0f) {
            balance->surface_deficit_mm = 0.0f;
        }
    }

    if (surface_recharge >= effective_mm) {
        return 0.0f;
    }

    return effective_mm - surface_recharge;
}

static void fao56_get_efficiency_split(const irrigation_method_data_t *method,
                                       float wetting_fraction,
                                       float *eff_surface,
                                       float *eff_root)
{
    float efficiency = 0.8f;
    float du = 1.0f;
    float wf = wetting_fraction;

    if (method) {
        efficiency = method->efficiency_pct / 100.0f;
        if (efficiency <= 0.0f || efficiency > 1.0f) {
            efficiency = 0.8f;
        }
        du = method->distribution_uniformity_pct / 100.0f;
        if (du <= 0.0f || du > 1.0f) {
            du = 1.0f;
        }
        if (wf <= 0.0f || wf > 1.0f) {
            wf = method->wetting_fraction_x1000 / 1000.0f;
        }
    }

    if (wf <= 0.0f || wf > 1.0f) {
        wf = 1.0f;
    }

    float root_eff = efficiency * du;
    float surface_eff = 1.0f;

    if (root_eff < 0.0f) root_eff = 0.0f;
    if (root_eff > 1.0f) root_eff = 1.0f;
    if (surface_eff < 0.0f) surface_eff = 0.0f;
    if (surface_eff > 1.0f) surface_eff = 1.0f;

    if (eff_surface) *eff_surface = surface_eff;
    if (eff_root) *eff_root = root_eff;
}

static float fao56_get_surface_wet_target(const irrigation_method_data_t *method,
                                          float wetting_fraction)
{
    float wf = wetting_fraction;
    if (wf <= 0.0f || wf > 1.0f) {
        if (method) {
            wf = method->wetting_fraction_x1000 / 1000.0f;
        }
    }
    if (wf <= 0.0f || wf > 1.0f) {
        wf = 1.0f;
    }

    float du = 1.0f;
    if (method) {
        du = method->distribution_uniformity_pct / 100.0f;
        if (du <= 0.0f || du > 1.0f) {
            du = 1.0f;
        }
    }

    float target = wf * du;
    if (target <= 0.0f || target > 1.0f) {
        target = wf;
    }

    return target;
}

static float fao56_calc_ke(const water_balance_t *balance,
                           float tew_mm,
                           float rew_mm,
                           const irrigation_method_data_t *method,
                           const plant_full_data_t *plant,
                           uint16_t days_after_planting)
{
    const float eps = 1e-3f;
    if (!balance || !method || tew_mm <= eps) {
        return 0.0f;
    }

    float d_surface = balance->surface_deficit_mm;
    if (d_surface < 0.0f) d_surface = 0.0f;
    if (d_surface > tew_mm) d_surface = tew_mm;

    float wet_area = method->wetting_fraction_x1000 / 1000.0f;
    if (balance) {
        if (balance->surface_wet_fraction > 0.0f && balance->surface_wet_fraction <= 1.0f) {
            wet_area = balance->surface_wet_fraction;
        } else if (balance->wetting_fraction > 0.0f && balance->wetting_fraction <= 1.0f) {
            wet_area = balance->wetting_fraction;
        }
    }
    if (wet_area <= eps || wet_area > 1.0f) {
        wet_area = 1.0f;
    }
    float canopy_factor = 0.0f;
    if (plant) {
        canopy_factor = fao56_get_dynamic_canopy_factor(plant, days_after_planting);
    }
    float canopy_reduction = 1.0f - FAO56_KE_CANOPY_REDUCTION * canopy_factor;
    if (canopy_reduction < 0.3f) canopy_reduction = 0.3f;

    float ke_max = FAO56_KE_MAX_BASE * wet_area * canopy_reduction;
    if (ke_max < 0.0f) ke_max = 0.0f;
    if (ke_max > 1.2f) ke_max = 1.2f;

    if ((tew_mm - rew_mm) <= eps) {
        return 0.0f;
    }
    if (d_surface <= rew_mm || tew_mm <= rew_mm) {
        return ke_max;
    }

    float ke = ke_max * (tew_mm - d_surface) / (tew_mm - rew_mm);
    if (ke < 0.0f) ke = 0.0f;
    if (ke > ke_max) ke = ke_max;
    return ke;
}

static float fao56_get_effective_wetting_fraction(const irrigation_method_data_t *method,
                                                  const soil_enhanced_data_t *soil,
                                                  const plant_full_data_t *plant)
{
    if (!method) {
        return 1.0f;
    }

    float base_wf = method->wetting_fraction_x1000 / 1000.0f;
    if (base_wf <= 0.0f || base_wf > 1.0f) {
        base_wf = 1.0f;
    }

    float wf = base_wf;
    if (base_wf < 0.95f) {
        float depth_mm = 0.0f;
        if (method->depth_typical_min_mm > 0 && method->depth_typical_max_mm > 0) {
            depth_mm = (method->depth_typical_min_mm + method->depth_typical_max_mm) * 0.5f;
        }
        if (depth_mm <= 0.0f) {
            depth_mm = 10.0f;
        }
        wf *= 1.0f + FAO56_WF_DEPTH_LOG_COEFF * log1pf(depth_mm / 10.0f);

        if (soil) {
            if (strstr(soil->texture, "Clay") || strstr(soil->texture, "clay")) {
                wf *= 1.15f;
            } else if (strstr(soil->texture, "Sand") || strstr(soil->texture, "sand")) {
                wf *= 0.85f;
            }

            if (soil->infil_mm_h > 20.0f) {
                wf *= 0.9f;
            } else if (soil->infil_mm_h > 0.0f && soil->infil_mm_h < 5.0f) {
                wf *= 1.1f;
            }
        }

        if (plant) {
            float row_spacing_m = plant->spacing_row_m_x1000 / 1000.0f;
            float plant_spacing_m = plant->spacing_plant_m_x1000 / 1000.0f;
            float area_per_plant = 0.0f;
            if (row_spacing_m > 0.0f && plant_spacing_m > 0.0f) {
                area_per_plant = row_spacing_m * plant_spacing_m;
            } else {
                float density = plant->default_density_plants_m2_x100 / 100.0f;
                if (density > 0.0f) {
                    area_per_plant = 1.0f / density;
                }
            }
            if (area_per_plant > 1.0f) {
                wf *= 0.9f;
            } else if (area_per_plant > 0.0f && area_per_plant < 0.1f) {
                wf *= 1.1f;
            }
        }

        float min_rel = base_wf * 0.5f;
        float max_rel = base_wf * 1.5f;
        if (max_rel > 1.0f) max_rel = 1.0f;
        if (wf < min_rel) wf = min_rel;
        if (wf > max_rel) wf = max_rel;
    }

    if (wf < FAO56_WF_MIN) wf = FAO56_WF_MIN;
    if (wf > FAO56_WF_MAX) wf = FAO56_WF_MAX;
    return wf;
}

static float fao56_get_antecedent_moisture_pct(uint8_t channel_id,
                                               const water_balance_t *balance)
{
    bool enabled = false;
    uint8_t moisture_pct = 0;
    bool has_data = false;

    if (soil_moisture_get_channel_override_with_presence(channel_id, &enabled, &moisture_pct, &has_data) == 0 &&
        enabled) {
        return (float)moisture_pct;
    }

    if (soil_moisture_get_global_with_presence(&enabled, &moisture_pct, &has_data) == 0 &&
        enabled) {
        return (float)moisture_pct;
    }

    if (balance && channel_id < WATERING_CHANNELS_COUNT) {
        bool have_root = (balance->wetting_awc_mm > 0.0f);
        bool have_surface = (balance->surface_tew_mm > 0.0f);
        float m_root = 0.0f;
        float m_surface = 0.0f;

        if (have_root) {
            m_root = 1.0f - (balance->current_deficit_mm / balance->wetting_awc_mm);
            if (m_root < 0.0f) m_root = 0.0f;
            if (m_root > 1.0f) m_root = 1.0f;
        }
        if (have_surface) {
            m_surface = 1.0f - (balance->surface_deficit_mm / balance->surface_tew_mm);
            if (m_surface < 0.0f) m_surface = 0.0f;
            if (m_surface > 1.0f) m_surface = 1.0f;
        }

        float m_est = 0.0f;
        if (have_surface && have_root) {
            m_est = 0.7f * m_surface + 0.3f * m_root;
        } else if (have_surface) {
            m_est = m_surface;
        } else if (have_root) {
            m_est = m_root;
        } else {
            return (float)soil_moisture_get_effective_pct(channel_id);
        }
        if (m_est < 0.0f) m_est = 0.0f;
        if (m_est > 1.0f) m_est = 1.0f;

        uint32_t now_s = k_uptime_get_32() / 1000U;
        bool update = true;
        if (s_antecedent_moisture_last_update_s[channel_id] > 0U &&
            (now_s - s_antecedent_moisture_last_update_s[channel_id]) < (6U * 3600U)) {
            update = false;
        }

        if (!s_antecedent_moisture_valid[channel_id]) {
            s_antecedent_moisture_ema[channel_id] = m_est;
            s_antecedent_moisture_valid[channel_id] = true;
            s_antecedent_moisture_last_update_s[channel_id] = now_s;
        } else if (update) {
            float ema = s_antecedent_moisture_ema[channel_id];
            ema += 0.5f * (m_est - ema);
            s_antecedent_moisture_ema[channel_id] = ema;
            s_antecedent_moisture_last_update_s[channel_id] = now_s;
        }

        return s_antecedent_moisture_ema[channel_id] * 100.0f;
    }

    return (float)soil_moisture_get_effective_pct(channel_id);
}

static bool fao56_build_weekly_et0_climatology(float latitude_rad,
                                               float week_et0_avg[FAO56_CLIMATOLOGY_WEEKS],
                                               float *overall_avg)
{
    if (!week_et0_avg) {
        return false;
    }

    if (env_history_get_storage() == NULL || env_history_get_entry_count(ENV_HISTORY_TYPE_DAILY) <= 0) {
        return false;
    }

    uint32_t now_sec = timezone_get_unix_utc();
    uint32_t current_day_index = now_sec / ENV_HISTORY_DAILY_INTERVAL_SEC;
    uint32_t lookback_days = 366U;
    uint32_t start_day_index = (current_day_index > lookback_days) ?
                               (current_day_index - lookback_days) : 0U;

    static daily_history_entry_t daily_entries[ENV_HISTORY_DAILY_ENTRIES];
    uint16_t actual_count = 0;
    int rc = env_history_get_daily_range(start_day_index,
                                         current_day_index,
                                         daily_entries,
                                         ENV_HISTORY_DAILY_ENTRIES,
                                         &actual_count);
    if (rc != 0 || actual_count == 0) {
        return false;
    }

    float week_sum[FAO56_CLIMATOLOGY_WEEKS] = {0};
    uint16_t week_count[FAO56_CLIMATOLOGY_WEEKS] = {0};
    float total_sum = 0.0f;
    uint16_t total_count = 0;

    for (uint16_t i = 0; i < actual_count; i++) {
        const daily_history_entry_t *entry = &daily_entries[i];
        if (entry->sample_count == 0) {
            continue;
        }

        uint32_t entry_ts = entry->date * ENV_HISTORY_DAILY_INTERVAL_SEC;
        uint16_t day_of_year = 0;
        if (!fao56_get_day_of_year_from_timestamp(entry_ts, &day_of_year) || day_of_year == 0) {
            continue;
        }

        environmental_data_t env = {0};
        env.temp_valid = true;
        env.air_temp_min_c = entry->temperature.min;
        env.air_temp_max_c = entry->temperature.max;
        env.air_temp_mean_c = entry->temperature.avg;
        if (env.air_temp_mean_c < env.air_temp_min_c || env.air_temp_mean_c > env.air_temp_max_c) {
            env.air_temp_mean_c = (env.air_temp_min_c + env.air_temp_max_c) / 2.0f;
        }

        float et0 = calc_et0_hargreaves_samani(&env, latitude_rad, day_of_year);
        if (et0 < HEURISTIC_ET0_MIN) et0 = HEURISTIC_ET0_MIN;
        if (et0 > HEURISTIC_ET0_MAX) et0 = HEURISTIC_ET0_MAX;

        uint8_t week_index = (uint8_t)((day_of_year - 1U) / 7U);
        if (week_index >= FAO56_CLIMATOLOGY_WEEKS) {
            week_index = FAO56_CLIMATOLOGY_WEEKS - 1U;
        }

        week_sum[week_index] += et0;
        week_count[week_index]++;
        total_sum += et0;
        total_count++;
    }

    if (total_count == 0) {
        return false;
    }

    float avg = total_sum / total_count;
    if (overall_avg) {
        *overall_avg = avg;
    }

    for (uint8_t week = 0; week < FAO56_CLIMATOLOGY_WEEKS; week++) {
        if (week_count[week] > 0) {
            week_et0_avg[week] = week_sum[week] / week_count[week];
        } else {
            week_et0_avg[week] = avg;
        }
    }

    return true;
}

static uint16_t fao56_get_current_day_of_year(void)
{
    rtc_datetime_t datetime;
    if (rtc_datetime_get(&datetime) != 0) {
        uint64_t uptime_sec = (uint64_t)(k_uptime_get() / 1000);
        uint16_t fallback = (uint16_t)((uptime_sec / 86400ULL) % 365ULL) + 1U;
        return fallback;
    }

    uint32_t utc_timestamp = timezone_rtc_to_unix_utc(&datetime);
    rtc_datetime_t local_datetime;
    if (fao56_get_local_datetime_from_timestamp(utc_timestamp, &local_datetime)) {
        datetime = local_datetime;
    }

    return fao56_calc_day_of_year_from_date(datetime.year, datetime.month, datetime.day);
}

/* ================================================================== */
/* Performance Optimization - Calculation Caching Implementation    */
/* ================================================================== */

/**
 * @brief Initialize the calculation cache system
 */
watering_error_t fao56_cache_init(void) {
    memset(&calculation_cache, 0, sizeof(calculation_cache));
    calculation_cache.cache_enabled = true;
    
    LOG_INF("FAO-56 calculation cache initialized");
    return WATERING_SUCCESS;
}

/**
 * @brief Enable or disable calculation caching
 */
void fao56_cache_set_enabled(bool enabled) {
    calculation_cache.cache_enabled = enabled;
    if (!enabled) {
        fao56_cache_clear_all();
    }
    LOG_INF("FAO-56 cache %s", enabled ? "enabled" : "disabled");
}

/**
 * @brief Clear all cache entries
 */
void fao56_cache_clear_all(void) {
    for (int i = 0; i < WATERING_CHANNELS_COUNT; i++) {
        calculation_cache.et0_cache[i].valid = false;
        calculation_cache.crop_coeff_cache[i].valid = false;
        calculation_cache.water_balance_cache[i].valid = false;
    }
    LOG_DBG("All FAO-56 cache entries cleared");
}

/**
 * @brief Clear cache entries for a specific channel
 */
void fao56_cache_clear_channel(uint8_t channel_id) {
    if (channel_id >= WATERING_CHANNELS_COUNT) {
        return;
    }
    
    calculation_cache.et0_cache[channel_id].valid = false;
    calculation_cache.crop_coeff_cache[channel_id].valid = false;
    calculation_cache.water_balance_cache[channel_id].valid = false;
    
    LOG_DBG("Cache cleared for channel %u", channel_id);
}

/**
 * @brief Get cache performance statistics
 */
void fao56_cache_get_stats(uint32_t *hit_count, uint32_t *miss_count, float *hit_ratio) {
    if (hit_count) *hit_count = calculation_cache.cache_hit_count;
    if (miss_count) *miss_count = calculation_cache.cache_miss_count;
    
    if (hit_ratio) {
        uint32_t total = calculation_cache.cache_hit_count + calculation_cache.cache_miss_count;
        *hit_ratio = total > 0 ? (float)calculation_cache.cache_hit_count / total : 0.0f;
    }
}

/**
 * @brief Check if environmental data matches cached data within tolerance
 */
static bool env_data_matches(const environmental_data_t *env, const et0_cache_entry_t *cache_entry) {
    return (fabsf(env->air_temp_min_c - cache_entry->temperature_min_c) < ET0_CACHE_TOLERANCE) &&
           (fabsf(env->air_temp_max_c - cache_entry->temperature_max_c) < ET0_CACHE_TOLERANCE) &&
           (fabsf(env->rel_humidity_pct - cache_entry->humidity_pct) < HUMIDITY_CACHE_TOLERANCE) &&
           (fabsf(env->atmos_pressure_hpa - cache_entry->pressure_hpa) < PRESSURE_CACHE_TOLERANCE);
}

/**
 * @brief Check if ET0 calculation result is cached and valid
 */
bool fao56_cache_get_et0(const environmental_data_t *env, float latitude_rad, 
                        uint16_t day_of_year, uint8_t channel_id, float *cached_result) {
    if (!calculation_cache.cache_enabled || channel_id >= WATERING_CHANNELS_COUNT || !env || !cached_result) {
        return false;
    }
    
    et0_cache_entry_t *entry = &calculation_cache.et0_cache[channel_id];
    
    if (!entry->valid) {
        calculation_cache.cache_miss_count++;
        return false;
    }
    
    // Check if cache entry is too old
    uint32_t current_time = k_uptime_get_32() / 1000; // Convert to seconds
    if (current_time - entry->calculation_time > CACHE_MAX_AGE_SECONDS) {
        entry->valid = false;
        calculation_cache.cache_miss_count++;
        return false;
    }
    
    // Check if environmental conditions match within tolerance
    if (!env_data_matches(env, entry) || 
        fabsf(latitude_rad - entry->latitude_rad) > 0.01f ||
        entry->day_of_year != day_of_year) {
        calculation_cache.cache_miss_count++;
        return false;
    }
    
    // Cache hit!
    *cached_result = entry->et0_result;
    calculation_cache.cache_hit_count++;
    LOG_DBG("ET0 cache hit for channel %u: %.2f mm/day", channel_id, (double)*cached_result);
    return true;
}

/**
 * @brief Store ET0 calculation result in cache
 */
void fao56_cache_store_et0(const environmental_data_t *env, float latitude_rad,
                          uint16_t day_of_year, uint8_t channel_id, float result) {
    if (!calculation_cache.cache_enabled || channel_id >= WATERING_CHANNELS_COUNT || !env) {
        return;
    }
    
    et0_cache_entry_t *entry = &calculation_cache.et0_cache[channel_id];
    
    entry->temperature_min_c = env->air_temp_min_c;
    entry->temperature_max_c = env->air_temp_max_c;
    entry->humidity_pct = env->rel_humidity_pct;
    entry->pressure_hpa = env->atmos_pressure_hpa;
    entry->latitude_rad = latitude_rad;
    entry->day_of_year = day_of_year;
    entry->et0_result = result;
    entry->calculation_time = k_uptime_get_32() / 1000;
    entry->valid = true;
    
    LOG_DBG("ET0 cached for channel %u: %.2f mm/day", channel_id, (double)result);
}

/**
 * @brief Check if crop coefficient calculation result is cached and valid
 */
bool fao56_cache_get_crop_coeff(uint16_t plant_id, uint16_t days_after_planting,
                               uint8_t channel_id, phenological_stage_t *cached_stage,
                               float *cached_coefficient) {
    if (!calculation_cache.cache_enabled || channel_id >= WATERING_CHANNELS_COUNT || 
        !cached_stage || !cached_coefficient) {
        return false;
    }
    
    crop_coeff_cache_entry_t *entry = &calculation_cache.crop_coeff_cache[channel_id];
    
    if (!entry->valid) {
        calculation_cache.cache_miss_count++;
        return false;
    }
    
    // Check if cache entry is too old
    uint32_t current_time = k_uptime_get_32() / 1000;
    if (current_time - entry->calculation_time > CACHE_MAX_AGE_SECONDS) {
        entry->valid = false;
        calculation_cache.cache_miss_count++;
        return false;
    }
    
    // Check if parameters match
    if (entry->plant_id != plant_id || 
        entry->days_after_planting != days_after_planting) {
        calculation_cache.cache_miss_count++;
        return false;
    }
    
    // Cache hit!
    *cached_stage = entry->stage;
    *cached_coefficient = entry->crop_coefficient;
    calculation_cache.cache_hit_count++;
    LOG_DBG("Crop coeff cache hit for channel %u: stage=%d, Kc=%.3f", 
            channel_id, *cached_stage, (double)*cached_coefficient);
    return true;
}

/**
 * @brief Store crop coefficient calculation result in cache
 */
void fao56_cache_store_crop_coeff(uint16_t plant_id, uint16_t days_after_planting,
                                 uint8_t channel_id, phenological_stage_t stage,
                                 float coefficient) {
    if (!calculation_cache.cache_enabled || channel_id >= WATERING_CHANNELS_COUNT) {
        return;
    }
    
    crop_coeff_cache_entry_t *entry = &calculation_cache.crop_coeff_cache[channel_id];
    
    entry->plant_id = plant_id;
    entry->days_after_planting = days_after_planting;
    entry->stage = stage;
    entry->crop_coefficient = coefficient;
    entry->calculation_time = k_uptime_get_32() / 1000;
    entry->valid = true;
    
    LOG_DBG("Crop coeff cached for channel %u: stage=%d, Kc=%.3f", 
            channel_id, stage, (double)coefficient);
}

/**
 * @brief Check if water balance calculation result is cached and valid
 */
bool fao56_cache_get_water_balance(uint8_t channel_id, uint16_t plant_id,
                                  uint8_t soil_db_index, uint8_t irrigation_method_index,
                                  float root_depth_m, water_balance_t *cached_balance) {
    if (!calculation_cache.cache_enabled || channel_id >= WATERING_CHANNELS_COUNT || 
        !cached_balance) {
        return false;
    }
    
    water_balance_cache_entry_t *entry = &calculation_cache.water_balance_cache[channel_id];
    
    if (!entry->valid) {
        calculation_cache.cache_miss_count++;
        return false;
    }
    
    // Check if cache entry is too old (water balance changes more frequently)
    uint32_t current_time = k_uptime_get_32() / 1000;
    if (current_time - entry->calculation_time > (CACHE_MAX_AGE_SECONDS / 4)) { // 15 minutes for water balance
        entry->valid = false;
        calculation_cache.cache_miss_count++;
        return false;
    }
    
    // Check if parameters match
    if (entry->channel_id != channel_id ||
        entry->plant_id != plant_id ||
        entry->soil_db_index != soil_db_index ||
        entry->irrigation_method_index != irrigation_method_index ||
        fabsf(entry->root_depth_m - root_depth_m) > 0.01f) {
        calculation_cache.cache_miss_count++;
        return false;
    }
    
    // Cache hit!
    *cached_balance = entry->balance_result;
    calculation_cache.cache_hit_count++;
    LOG_DBG("Water balance cache hit for channel %u: deficit=%.2f mm",
            channel_id, (double)cached_balance->current_deficit_mm);
    return true;
}

/**
 * @brief Store water balance calculation result in cache
 */
void fao56_cache_store_water_balance(uint8_t channel_id, uint16_t plant_id,
                                    uint8_t soil_db_index, uint8_t irrigation_method_index,
                                    float root_depth_m, const water_balance_t *balance) {
    if (!calculation_cache.cache_enabled || channel_id >= WATERING_CHANNELS_COUNT || !balance) {
        return;
    }
    
    water_balance_cache_entry_t *entry = &calculation_cache.water_balance_cache[channel_id];
    
    entry->channel_id = channel_id;
    entry->plant_id = plant_id;
    entry->soil_db_index = soil_db_index;
    entry->irrigation_method_index = irrigation_method_index;
    entry->root_depth_m = root_depth_m;
    entry->balance_result = *balance;
    entry->calculation_time = k_uptime_get_32() / 1000;
    entry->valid = true;
    
    LOG_DBG("Water balance cached for channel %u: deficit=%.2f mm",
            channel_id, (double)balance->current_deficit_mm);
}

/**
 * @brief Invalidate cache entries based on environmental data changes
 */
void fao56_cache_invalidate_on_env_change(uint32_t env_change_flags) {
    // If temperature, humidity, or pressure changed significantly, invalidate ET0 cache
    if (env_change_flags & (0x01 | 0x02 | 0x04)) { // Temperature, humidity, pressure flags
        for (int i = 0; i < WATERING_CHANNELS_COUNT; i++) {
            calculation_cache.et0_cache[i].valid = false;
            calculation_cache.water_balance_cache[i].valid = false; // Water balance depends on ET0
        }
        LOG_DBG("Cache invalidated due to environmental changes (flags: 0x%02X)", env_change_flags);
    }
}

/**
 * @brief Invalidate cache entries based on time intervals
 */
void fao56_cache_invalidate_by_age(uint32_t max_age_seconds) {
    uint32_t current_time = k_uptime_get_32() / 1000;
    int invalidated_count = 0;
    
    for (int i = 0; i < WATERING_CHANNELS_COUNT; i++) {
        if (calculation_cache.et0_cache[i].valid && 
            (current_time - calculation_cache.et0_cache[i].calculation_time) > max_age_seconds) {
            calculation_cache.et0_cache[i].valid = false;
            invalidated_count++;
        }
        
        if (calculation_cache.crop_coeff_cache[i].valid && 
            (current_time - calculation_cache.crop_coeff_cache[i].calculation_time) > max_age_seconds) {
            calculation_cache.crop_coeff_cache[i].valid = false;
            invalidated_count++;
        }
        
        if (calculation_cache.water_balance_cache[i].valid && 
            (current_time - calculation_cache.water_balance_cache[i].calculation_time) > max_age_seconds) {
            calculation_cache.water_balance_cache[i].valid = false;
            invalidated_count++;
        }
    }
    
    if (invalidated_count > 0) {
        LOG_DBG("Invalidated %d cache entries older than %u seconds", invalidated_count, max_age_seconds);
    }
}

/* ================================================================== */
/* Resource-Constrained Operation Mode Implementation               */
/* ================================================================== */

// Global resource constraint state
static bool resource_constrained_mode = false;

/**
 * @brief Check if system resources are constrained
 */
bool fao56_is_resource_constrained(void) {
    return resource_constrained_mode;
}

/**
 * @brief Enable or disable resource-constrained operation mode
 */
void fao56_set_resource_constrained_mode(bool enabled) {
    resource_constrained_mode = enabled;
    
    if (enabled) {
        // Disable caching to save memory
        fao56_cache_set_enabled(false);
        LOG_WRN("FAO-56 resource-constrained mode enabled - using simplified calculations");
    } else {
        // Re-enable caching for normal operation
        fao56_cache_set_enabled(true);
        LOG_INF("FAO-56 normal operation mode restored");
    }
}

/**
 * @brief Get simplified ET0 calculation using temperature-only method
 */
float fao56_get_simplified_et0(float temp_min_c, float temp_max_c, 
                              float latitude_rad, uint16_t day_of_year) {
    // Simplified Hargreaves-Samani equation with reduced complexity
    float temp_mean = (temp_min_c + temp_max_c) / 2.0f;
    float temp_range = temp_max_c - temp_min_c;
    
    // Simplified extraterrestrial radiation calculation
    float dr = 1.0f + 0.033f * cosf(2.0f * PI * day_of_year / 365.0f);
    float ra_simplified = 15.0f * dr; // Simplified constant for mid-latitudes
    
    // Simplified Hargreaves-Samani
    float et0 = 0.0023f * (temp_mean + 17.8f) * sqrtf(temp_range) * ra_simplified;
    
    // Sanity check
    if (et0 < 0.0f) et0 = 0.0f;
    if (et0 > 12.0f) et0 = 12.0f; // Conservative upper limit
    
    LOG_DBG("Simplified ET0: %.2f mm/day (T_mean=%.1f°C, T_range=%.1f°C)", 
            (double)et0, (double)temp_mean, (double)temp_range);
    
    return et0;
}

/**
 * @brief Get simplified crop coefficient based on plant type only
 */
float fao56_get_simplified_crop_coefficient(plant_type_t plant_type, 
                                           uint16_t days_after_planting) {
    float kc = 1.0f; // Default coefficient
    
    // Simplified growth stage estimation (rough approximation)
    float growth_factor = 1.0f;
    if (days_after_planting < 30) {
        growth_factor = 0.7f; // Initial stage
    } else if (days_after_planting < 90) {
        growth_factor = 1.0f + (days_after_planting - 30) / 60.0f * 0.3f; // Development
    } else if (days_after_planting < 150) {
        growth_factor = 1.3f; // Mid-season
    } else {
        growth_factor = 1.0f; // End season
    }
    
    // Simplified plant type coefficients
    switch (plant_type) {
        case PLANT_TYPE_VEGETABLES:
            kc = 1.1f * growth_factor;
            break;
        case PLANT_TYPE_HERBS:
            kc = 0.9f * growth_factor;
            break;
        case PLANT_TYPE_FLOWERS:
            kc = 1.0f * growth_factor;
            break;
        case PLANT_TYPE_SHRUBS:
            kc = 0.8f * growth_factor;
            break;
        case PLANT_TYPE_TREES:
            kc = 0.7f * growth_factor;
            break;
        case PLANT_TYPE_LAWN:
            kc = 1.2f * growth_factor;
            break;
        case PLANT_TYPE_SUCCULENTS:
            kc = 0.4f * growth_factor;
            break;
        default:
            kc = 1.0f * growth_factor;
            break;
    }
    
    // Clamp to reasonable bounds
    if (kc < 0.3f) kc = 0.3f;
    if (kc > 1.5f) kc = 1.5f;
    
    LOG_DBG("Simplified Kc=%.3f for plant type %d, days=%d", (double)kc, plant_type, days_after_planting);
    
    return kc;
}

/**
 * @brief Calculate simplified irrigation requirement for resource-constrained operation
 */
watering_error_t fao56_calculate_simplified_irrigation(uint8_t channel_id,
                                                      const environmental_data_t *env,
                                                      irrigation_calculation_t *result) {
    if (!env || !result || channel_id >= WATERING_CHANNELS_COUNT) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    // Get channel data
    watering_channel_t *channel;
    watering_error_t err = watering_get_channel(channel_id, &channel);
    if (err != WATERING_SUCCESS) {
        return err;
    }
    
    // Initialize result
    memset(result, 0, sizeof(irrigation_calculation_t));
    
    // Simplified ET0 calculation using only temperature
    float et0 = fao56_get_simplified_et0(env->air_temp_min_c, env->air_temp_max_c, 
                                        channel->latitude_deg * PI / 180.0f, 
                                        180); // Assume mid-year
    
    // Simplified crop coefficient
    float kc = fao56_get_simplified_crop_coefficient(channel->plant_type, 
                                                    channel->days_after_planting);
    
    // Calculate crop evapotranspiration
    float etc = et0 * kc;
    
    // Simplified water requirement (assume 50% soil depletion)
    result->net_irrigation_mm = etc * 0.5f;
    
    // Apply irrigation efficiency (assume 80% for simplicity)
    result->gross_irrigation_mm = result->net_irrigation_mm / 0.8f;
    
    // Convert to volume based on coverage
    if (channel->use_area_based) {
        result->volume_liters = result->gross_irrigation_mm * channel->coverage.area_m2;
    } else {
        const plant_full_data_t *plant = fao56_get_channel_plant(channel, 0); /* channel_id unknown here */
        float area_m2 = 0.0f;
        if (plant) {
            area_m2 = fao56_calc_plant_irrigated_area_m2(
                plant, channel->coverage.plant_count, NULL, NULL, false);
        }
        if (area_m2 <= 0.0f) {
            area_m2 = channel->coverage.plant_count * 0.5f;
        }
        result->volume_liters = result->gross_irrigation_mm * area_m2;
        result->volume_per_plant_liters = result->volume_liters / channel->coverage.plant_count;
    }
    // Apply eco mode if enabled
    if (channel->auto_mode == WATERING_AUTOMATIC_ECO) {
        result->volume_liters *= ECO_ETC_FACTOR;
        result->volume_per_plant_liters *= ECO_ETC_FACTOR;
    }
    
    // Apply volume limits
    if (channel->max_volume_limit_l > 0 && result->volume_liters > channel->max_volume_limit_l) {
        result->volume_liters = channel->max_volume_limit_l;
        result->volume_limited = true;
    }
    
    // Simple single-cycle irrigation
    result->cycle_count = 1;
    result->cycle_duration_min = (uint16_t)(result->volume_liters / 10.0f); // Assume 10L/min flow rate
    result->soak_interval_min = 0;
    
    LOG_INF("Simplified irrigation calc for ch%u: ET0=%.2f, Kc=%.3f, vol=%.1fL", 
            channel_id, (double)et0, (double)kc, (double)result->volume_liters);
    
    return WATERING_SUCCESS;
}

/**
 * @brief Get memory usage statistics for FAO-56 calculations
 */
void fao56_get_memory_usage(uint32_t *cache_memory_bytes, uint32_t *total_memory_bytes) {
    if (cache_memory_bytes) {
        *cache_memory_bytes = sizeof(fao56_calculation_cache_t);
    }
    
    if (total_memory_bytes) {
        // Estimate total memory usage including static data
        *total_memory_bytes = sizeof(fao56_calculation_cache_t) + 
                             sizeof(resource_constrained_mode) + 
                             1024; // Estimated stack usage
    }
}

/* ================================================================== */
/* Error Handling and Fallback Implementation                       */
/* ================================================================== */

/**
 * @brief Detect and handle FAO-56 calculation failures
 */
fao56_recovery_mode_t fao56_handle_calculation_error(uint8_t channel_id,
                                                    watering_error_t error_code,
                                                    const environmental_data_t *env,
                                                    irrigation_calculation_t *result) {
    if (!result) {
        return FAO56_RECOVERY_MANUAL_MODE;
    }
    
    fao56_log_calculation_error(channel_id, error_code, __func__, "Calculation failure detected");
    
    // Try simplified calculations first
    if (env && fao56_calculate_simplified_irrigation(channel_id, env, result) == WATERING_SUCCESS) {
        LOG_WRN("Channel %u: Using simplified calculations due to error %d", channel_id, error_code);
        return FAO56_RECOVERY_SIMPLIFIED;
    }
    
    // Fall back to default schedule
    watering_channel_t *channel;
    if (watering_get_channel(channel_id, &channel) == WATERING_SUCCESS) {
        if (fao56_get_default_irrigation_schedule(channel_id, channel->plant_type, result) == WATERING_SUCCESS) {
            LOG_WRN("Channel %u: Using default schedule due to calculation failure", channel_id);
            return FAO56_RECOVERY_DEFAULTS;
        }
    }
    
    // Complete failure - recommend manual mode
    LOG_ERR("Channel %u: All automatic calculations failed, recommend manual mode", channel_id);
    memset(result, 0, sizeof(irrigation_calculation_t));
    return FAO56_RECOVERY_MANUAL_MODE;
}

/**
 * @brief Handle environmental sensor failures with graceful degradation
 */
watering_error_t fao56_handle_sensor_failure(const environmental_data_t *env,
                                            environmental_data_t *fallback_env) {
    if (!env || !fallback_env) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    // Copy original data
    *fallback_env = *env;
    
    // Apply fallback values for invalid sensors
    if (!env->temp_valid || env->air_temp_min_c < -40.0f || env->air_temp_max_c > 60.0f) {
        LOG_WRN("Temperature sensor failure, using defaults");
        fallback_env->air_temp_min_c = 15.0f;  // Conservative minimum
        fallback_env->air_temp_max_c = 25.0f;  // Conservative maximum
        fallback_env->air_temp_mean_c = 20.0f; // Conservative mean
        fallback_env->temp_valid = true;
    }
    
    if (!env->humidity_valid || env->rel_humidity_pct < 0.0f || env->rel_humidity_pct > 100.0f) {
        LOG_WRN("Humidity sensor failure, using default 60%%");
        fallback_env->rel_humidity_pct = 60.0f; // Conservative humidity
        fallback_env->humidity_valid = true;
    }
    
    if (!env->pressure_valid || env->atmos_pressure_hpa < 800.0f || env->atmos_pressure_hpa > 1200.0f) {
        LOG_WRN("Pressure sensor failure, using sea level default");
        fallback_env->atmos_pressure_hpa = 1013.25f; // Sea level pressure
        fallback_env->pressure_valid = true;
    }
    
    /* solar & wind removed (no sensors) */
    
    if (!env->rain_valid || env->rain_mm_24h < 0.0f) {
        LOG_WRN("Rain sensor failure, assuming no rainfall");
        fallback_env->rain_mm_24h = 0.0f; // Conservative assumption
        fallback_env->rain_valid = true;
    }
    
    return WATERING_SUCCESS;
}

/**
 * @brief Validate environmental data and apply conservative defaults
 */
watering_error_t fao56_validate_environmental_data(const environmental_data_t *env,
                                                  environmental_data_t *validated_env) {
    if (!env || !validated_env) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    // Start with original data
    *validated_env = *env;
    
    // Validate temperature data
    if (env->temp_valid) {
        if (env->air_temp_min_c > env->air_temp_max_c) {
            LOG_WRN("Invalid temperature range, swapping min/max");
            validated_env->air_temp_min_c = env->air_temp_max_c;
            validated_env->air_temp_max_c = env->air_temp_min_c;
        }
        
        // Recalculate mean if it's outside the min/max range
        if (env->air_temp_mean_c < validated_env->air_temp_min_c || 
            env->air_temp_mean_c > validated_env->air_temp_max_c) {
            validated_env->air_temp_mean_c = (validated_env->air_temp_min_c + validated_env->air_temp_max_c) / 2.0f;
            LOG_WRN("Recalculated mean temperature: %.1f°C", (double)validated_env->air_temp_mean_c);
        }
    }
    
    // Validate humidity
    if (env->humidity_valid) {
        if (env->rel_humidity_pct < 0.0f) {
            validated_env->rel_humidity_pct = 0.0f;
        } else if (env->rel_humidity_pct > 100.0f) {
            validated_env->rel_humidity_pct = 100.0f;
        }
    }
    
    /* solar & wind removed */
    
    // Validate rainfall
    if (env->rain_valid && env->rain_mm_24h < 0.0f) {
        validated_env->rain_mm_24h = 0.0f;
        LOG_WRN("Corrected negative rainfall to 0");
    }
    
    return WATERING_SUCCESS;
}

/**
 * @brief Get default irrigation schedule when automatic calculations fail
 */
watering_error_t fao56_get_default_irrigation_schedule(uint8_t channel_id,
                                                      plant_type_t plant_type,
                                                      irrigation_calculation_t *result) {
    if (!result || channel_id >= WATERING_CHANNELS_COUNT) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    // Get channel data
    watering_channel_t *channel;
    watering_error_t err = watering_get_channel(channel_id, &channel);
    if (err != WATERING_SUCCESS) {
        return err;
    }
    
    // Initialize result
    memset(result, 0, sizeof(irrigation_calculation_t));
    
    // Default irrigation amounts based on plant type (conservative values)
    float default_volume_l = 1.0f; // Default 1 liter
    
    switch (plant_type) {
        case PLANT_TYPE_VEGETABLES:
            default_volume_l = 2.0f;
            break;
        case PLANT_TYPE_HERBS:
            default_volume_l = 1.0f;
            break;
        case PLANT_TYPE_FLOWERS:
            default_volume_l = 1.5f;
            break;
        case PLANT_TYPE_SHRUBS:
            default_volume_l = 3.0f;
            break;
        case PLANT_TYPE_TREES:
            default_volume_l = 5.0f;
            break;
        case PLANT_TYPE_LAWN:
            default_volume_l = 4.0f;
            break;
        case PLANT_TYPE_SUCCULENTS:
            default_volume_l = 0.5f;
            break;
        default:
            default_volume_l = 1.5f;
            break;
    }
    
    // Scale based on coverage
    if (channel->use_area_based) {
        result->volume_liters = default_volume_l * channel->coverage.area_m2;
    } else {
        result->volume_liters = default_volume_l * channel->coverage.plant_count;
        result->volume_per_plant_liters = default_volume_l;
    }
    
    // Apply eco mode if enabled
    if (channel->auto_mode == WATERING_AUTOMATIC_ECO) {
        result->volume_liters *= ECO_ETC_FACTOR;
        result->volume_per_plant_liters *= ECO_ETC_FACTOR;
    }
    
    // Apply volume limits
    if (channel->max_volume_limit_l > 0 && result->volume_liters > channel->max_volume_limit_l) {
        result->volume_liters = channel->max_volume_limit_l;
        result->volume_limited = true;
    }
    
    // Simple single-cycle irrigation
    result->cycle_count = 1;
    result->cycle_duration_min = (uint16_t)(result->volume_liters / 5.0f); // Assume 5L/min flow rate
    result->soak_interval_min = 0;
    
    // Set conservative values for other fields
    result->net_irrigation_mm = result->volume_liters / (channel->use_area_based ? channel->coverage.area_m2 : 1.0f);
    result->gross_irrigation_mm = result->net_irrigation_mm;
    
    LOG_INF("Default irrigation schedule for ch%u: %.1fL (%s)", 
            channel_id, (double)result->volume_liters, 
            plant_type == PLANT_TYPE_VEGETABLES ? "vegetables" :
            plant_type == PLANT_TYPE_HERBS ? "herbs" :
            plant_type == PLANT_TYPE_FLOWERS ? "flowers" :
            plant_type == PLANT_TYPE_SHRUBS ? "shrubs" :
            plant_type == PLANT_TYPE_TREES ? "trees" :
            plant_type == PLANT_TYPE_LAWN ? "lawn" :
            plant_type == PLANT_TYPE_SUCCULENTS ? "succulents" : "other");
    
    return WATERING_SUCCESS;
}

/**
 * @brief Check system health and recommend recovery actions
 */
watering_error_t fao56_check_system_health(uint32_t *health_status,
                                          fao56_recovery_mode_t *recommended_action) {
    if (!health_status || !recommended_action) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    *health_status = 0;
    *recommended_action = FAO56_RECOVERY_NONE;
    
    // Check memory usage
    uint32_t cache_memory, total_memory;
    fao56_get_memory_usage(&cache_memory, &total_memory);
    
    if (total_memory > 32768) { // 32KB threshold
        *health_status |= 0x01; // Memory pressure
        LOG_WRN("FAO-56 memory usage high: %u bytes", total_memory);
    }
    
    // Check cache performance
    uint32_t hit_count, miss_count;
    float hit_ratio;
    fao56_cache_get_stats(&hit_count, &miss_count, &hit_ratio);
    
    if (hit_ratio < 0.5f && (hit_count + miss_count) > 100) {
        *health_status |= 0x02; // Poor cache performance
        LOG_WRN("FAO-56 cache hit ratio low: %.2f", (double)hit_ratio);
    }
    
    // Check if resource-constrained mode is active
    if (fao56_is_resource_constrained()) {
        *health_status |= 0x04; // Resource constrained
        LOG_INF("FAO-56 running in resource-constrained mode");
    }
    
    // Recommend actions based on health status
    if (*health_status & 0x01) {
        *recommended_action = FAO56_RECOVERY_SIMPLIFIED;
    } else if (*health_status & 0x02) {
        // Poor cache performance - consider clearing cache
        fao56_cache_clear_all();
        LOG_INF("Cleared FAO-56 cache due to poor performance");
    }
    
    return (*health_status == 0) ? WATERING_SUCCESS : WATERING_ERROR_CONFIG;
}

/**
 * @brief Log calculation errors with context for debugging
 */
void fao56_log_calculation_error(uint8_t channel_id, watering_error_t error_code,
                               const char *function_name, const char *additional_info) {
    const char *error_str;
    
    switch (error_code) {
        case WATERING_ERROR_INVALID_PARAM:
            error_str = "Invalid parameter";
            break;
        case WATERING_ERROR_NOT_INITIALIZED:
            error_str = "Not initialized";
            break;
        case WATERING_ERROR_HARDWARE:
            error_str = "Hardware failure";
            break;
        case WATERING_ERROR_TIMEOUT:
            error_str = "Timeout";
            break;
        case WATERING_ERROR_CONFIG:
            error_str = "Configuration error";
            break;
        default:
            error_str = "Unknown error";
            break;
    }
    
    LOG_ERR("FAO-56 calculation error - Channel: %u, Function: %s, Error: %s (%d), Info: %s",
            channel_id, 
            function_name ? function_name : "unknown",
            error_str, 
            error_code,
            additional_info ? additional_info : "none");
}

/**
 * @brief Determine current phenological stage based on days after planting
 */
phenological_stage_t calc_phenological_stage(
    const plant_full_data_t *plant,
    uint16_t days_after_planting
)
{
    if (!plant) {
        LOG_ERR("Invalid plant data");
        return PHENO_STAGE_INITIAL;
    }

    // Calculate cumulative stage boundaries
    uint16_t stage_1_end = plant->stage_days_ini;
    uint16_t stage_2_end = stage_1_end + plant->stage_days_dev;
    uint16_t stage_3_end = stage_2_end + plant->stage_days_mid;

    LOG_DBG("Plant stages: ini=%d, dev=%d, mid=%d, end=%d, days=%d",
            plant->stage_days_ini, plant->stage_days_dev,
            plant->stage_days_mid, plant->stage_days_end,
            days_after_planting);

    if (days_after_planting <= stage_1_end) {
        return PHENO_STAGE_INITIAL;
    } else if (days_after_planting <= stage_2_end) {
        return PHENO_STAGE_DEVELOPMENT;
    } else if (days_after_planting <= stage_3_end) {
        return PHENO_STAGE_MID_SEASON;
    } else {
        return PHENO_STAGE_END_SEASON;
    }
}

/**
 * @brief Calculate crop coefficient with interpolation between stages
 */
float calc_crop_coefficient(
    const plant_full_data_t *plant,
    phenological_stage_t stage,
    uint16_t days_after_planting
)
{
    if (!plant) {
        LOG_ERR("Invalid plant data");
        return 1.0f;  // Default Kc
    }

    // Convert scaled values to float
    float kc_ini = plant->kc_ini_x1000 / 1000.0f;
    float kc_mid = plant->kc_mid_x1000 / 1000.0f;
    float kc_end = plant->kc_end_x1000 / 1000.0f;

    // Calculate stage boundaries
    uint16_t stage_1_end = plant->stage_days_ini;
    uint16_t stage_2_end = stage_1_end + plant->stage_days_dev;
    uint16_t stage_3_end = stage_2_end + plant->stage_days_mid;

    float kc_result;

    switch (stage) {
        case PHENO_STAGE_INITIAL:
            // Constant Kc during initial stage
            kc_result = kc_ini;
            break;

        case PHENO_STAGE_DEVELOPMENT:
            // Linear interpolation from Kc_ini to Kc_mid
            {
                if (plant->stage_days_dev == 0U) {
                    kc_result = kc_mid;
                    break;
                }
                uint16_t days_in_stage = days_after_planting - stage_1_end;
                float stage_progress = (float)days_in_stage / plant->stage_days_dev;
                if (stage_progress > 1.0f) stage_progress = 1.0f;
                if (stage_progress < 0.0f) stage_progress = 0.0f;
                kc_result = kc_ini + (kc_mid - kc_ini) * stage_progress;
            }
            break;

        case PHENO_STAGE_MID_SEASON:
            // Constant Kc during mid-season
            kc_result = kc_mid;
            break;

        case PHENO_STAGE_END_SEASON:
            // Linear interpolation from Kc_mid to Kc_end
            {
                if (plant->stage_days_end == 0U) {
                    kc_result = kc_end;
                    break;
                }
                uint16_t days_in_stage = days_after_planting - stage_3_end;
                float stage_progress = (float)days_in_stage / plant->stage_days_end;
                // Clamp progress to avoid extrapolation beyond end stage
                if (stage_progress > 1.0f) stage_progress = 1.0f;
                if (stage_progress < 0.0f) stage_progress = 0.0f;
                kc_result = kc_mid + (kc_end - kc_mid) * stage_progress;
            }
            break;

        default:
            LOG_WRN("Unknown phenological stage, using mid-season Kc");
            kc_result = kc_mid;
            break;
    }

    // Sanity check - Kc should be positive and reasonable
    if (kc_result < 0.1f) {
        LOG_WRN("Calculated Kc too low (%.3f), clamping to 0.1", (double)kc_result);
        kc_result = 0.1f;
    } else if (kc_result > 2.0f) {
        LOG_WRN("Calculated Kc too high (%.3f), clamping to 2.0", (double)kc_result);
        kc_result = 2.0f;
    }

    LOG_DBG("Calculated Kc=%.3f for stage=%d, days=%d", 
            (double)kc_result, stage, days_after_planting);

    return kc_result;
}

/**
 * @brief Calculate current root depth based on plant age and characteristics
 */
float calc_current_root_depth(
    const plant_full_data_t *plant,
    uint16_t days_after_planting
)
{
    if (!plant) {
        LOG_ERR("Invalid plant data");
        return 0.5f;  // Default 0.5m root depth
    }

    float root_min = plant->root_depth_min_m_x1000 / 1000.0f;
    float root_max = plant->root_depth_max_m_x1000 / 1000.0f;

    // Calculate total growing season length
    uint16_t total_season = plant->stage_days_ini + plant->stage_days_dev + 
                           plant->stage_days_mid + plant->stage_days_end;

    float season_progress = 0.0f;
    if (total_season > 0) {
        season_progress = (float)days_after_planting / total_season;
        if (season_progress > 1.0f) season_progress = 1.0f;
        if (season_progress < 0.0f) season_progress = 0.0f;
    } else {
        LOG_WRN("Zero season length, using sigmoid(0) root depth");
    }

    // Root development follows a sigmoid curve, reaching ~90% max depth by mid-season

    // Sigmoid function for root development: f(x) = 1 / (1 + e^(-k*(x-0.5)))
    // where k=6 gives good root development curve
    float sigmoid_progress = 1.0f / (1.0f + expf(-6.0f * (season_progress - 0.5f)));
    
    float current_depth = root_min + (root_max - root_min) * sigmoid_progress;

    LOG_DBG("Root depth: %.3fm (progress=%.2f, days=%d/%d)", 
            (double)current_depth, (double)season_progress, days_after_planting, total_season);
    
    return current_depth;
}

/**
 * @brief Calculate extraterrestrial radiation for a given day and latitude
 */
static float calc_extraterrestrial_radiation(float latitude_rad, uint16_t day_of_year)
{
    // Solar declination
    float solar_declination = 0.409f * sinf(2.0f * PI * day_of_year / 365.0f - 1.39f);
    
    // Sunset hour angle
    float x = -tanf(latitude_rad) * tanf(solar_declination);
    if (x < -1.0f) x = -1.0f;
    if (x > 1.0f) x = 1.0f;
    float sunset_angle = acosf(x);
    
    // Inverse relative distance Earth-Sun
    float dr = 1.0f + 0.033f * cosf(2.0f * PI * day_of_year / 365.0f);
    
    // Extraterrestrial radiation
    float ra = (24.0f * 60.0f / PI) * SOLAR_CONSTANT * dr * 
               (sunset_angle * sinf(latitude_rad) * sinf(solar_declination) + 
                cosf(latitude_rad) * cosf(solar_declination) * sinf(sunset_angle));
    
    return ra;
}

/**
 * @brief Estimate altitude (m) from atmospheric pressure (kPa)
 */
static float calc_altitude_from_pressure_kpa(float pressure_kpa)
{
    if (pressure_kpa <= 0.0f) {
        return 0.0f;
    }

    float ratio = pressure_kpa / STANDARD_ATMOS_PRESSURE_KPA;
    if (ratio <= 0.0f) {
        return 0.0f;
    }

    float altitude_m = 44331.0f * (1.0f - powf(ratio, 0.1903f));
    if (altitude_m < 0.0f) {
        altitude_m = 0.0f;
    }

    return altitude_m;
}

/**
 * @brief Calculate saturation vapor pressure at given temperature
 */
static float calc_saturation_vapor_pressure(float temp_c)
{
    return 0.6108f * expf(17.27f * temp_c / (temp_c + 237.3f));
}

/**
 * @brief Calculate slope of saturation vapor pressure curve
 */
static float calc_vapor_pressure_slope(float temp_c)
{
    float es = calc_saturation_vapor_pressure(temp_c);
    return 4098.0f * es / powf(temp_c + 237.3f, 2.0f);
}

/**
 * @brief Calculate psychrometric constant (FAO-56 equation)
 * 
 * γ = 0.000665 × P (kPa/°C)
 * where P is atmospheric pressure in kPa
 * 
 * Reference: FAO-56 equation 8
 */
static float calc_psychrometric_constant(float pressure_kpa)
{
    return 0.000665f * pressure_kpa;
}

/**
 * @brief Calculate reference evapotranspiration using Penman-Monteith equation
 */
float calc_et0_penman_monteith(
    const environmental_data_t *env,
    float latitude_rad,
    uint16_t day_of_year,
    float *et0_raw_out
)
{
    if (!env) {
        LOG_ERR("Invalid environmental data");
        if (et0_raw_out) {
            *et0_raw_out = 0.0f;
        }
        return 0.0f;
    }

    // Check data validity
    if (!env->temp_valid || !env->humidity_valid) {
        LOG_WRN("Missing required temperature or humidity data for Penman-Monteith");
        float fallback = calc_et0_hargreaves_samani(env, latitude_rad, day_of_year);
        if (et0_raw_out) {
            *et0_raw_out = fallback;
        }
        return fallback;
    }

    // Convert atmospheric pressure from hPa to kPa
    float pressure_kpa = env->atmos_pressure_hpa / 10.0f;
    if (!env->pressure_valid || pressure_kpa < 50.0f || pressure_kpa > 110.0f) {
        // Use standard atmospheric pressure at sea level if invalid
        pressure_kpa = STANDARD_ATMOS_PRESSURE_KPA;
        LOG_DBG("Using standard atmospheric pressure (%.1f kPa)", (double)STANDARD_ATMOS_PRESSURE_KPA);
    }

    // Mean temperature
    float temp_mean = env->air_temp_mean_c;
    
    // Saturation vapor pressure
    float es_tmax = calc_saturation_vapor_pressure(env->air_temp_max_c);
    float es_tmin = calc_saturation_vapor_pressure(env->air_temp_min_c);
    float es = (es_tmax + es_tmin) / 2.0f;
    
    // Actual vapor pressure from relative humidity
    float ea = es * env->rel_humidity_pct / 100.0f;
    if (env->derived_values_calculated && env->vapor_pressure_kpa > 0.0f) {
        float dew_es = calc_saturation_vapor_pressure(env->dewpoint_temp_c);
        if (dew_es > 0.0f) {
            ea = dew_es;
        }
    }
    if (ea > es) ea = es;
    
    // Slope of saturation vapor pressure curve
    float delta = calc_vapor_pressure_slope(temp_mean);
    
    // Psychrometric constant
    float gamma = calc_psychrometric_constant(pressure_kpa);

    // Estimate altitude from pressure for clear-sky radiation
    float altitude_m = calc_altitude_from_pressure_kpa(pressure_kpa);
    
    // Wind removed: use assumed constant
    float wind_speed = ASSUMED_WIND_SPEED_M_S;
    
    // Net radiation calculation
    float ra = calc_extraterrestrial_radiation(latitude_rad, day_of_year);
    float rn;
    
    // Estimate incoming solar radiation from diurnal temperature range
    float temp_range = env->air_temp_max_c - env->air_temp_min_c;
    if (temp_range < 0.0f) {
        temp_range = 0.0f;
    }

    float rs = HARGREAVES_RS_COEFF * sqrtf(temp_range) * ra;
    float sunshine_ratio = 0.0f;
    if (ra > 0.0f) {
        sunshine_ratio = (rs / ra - 0.25f) / 0.50f;
        if (sunshine_ratio < 0.0f) sunshine_ratio = 0.0f;
        if (sunshine_ratio > 1.0f) sunshine_ratio = 1.0f;
    }

    float rso = (0.75f + 2e-5f * altitude_m) * ra;
    if (rso <= 0.0f) {
        rso = 0.0001f;
    }
    if (rs > rso) {
        rs = rso;
    }
    if ((rs / rso) < 0.05f) {
        rs = rso * 0.05f;
    }

    float rns = (1.0f - ASSUMED_ALBEDO) * rs;
    float rnl = STEFAN_BOLTZMANN *
               (powf(env->air_temp_max_c + 273.16f, 4.0f) +
                powf(env->air_temp_min_c + 273.16f, 4.0f)) / 2.0f *
               (0.34f - 0.14f * sqrtf(ea)) *
               (1.35f * rs / rso - 0.35f);
    rn = rns - rnl;
    
    // Soil heat flux (assumed negligible for daily calculations)
    float g = 0.0f;
    
    // Penman-Monteith equation
    float numerator = 0.408f * delta * (rn - g) + 
                     gamma * 900.0f / (temp_mean + 273.0f) * wind_speed * (es - ea);
    float denominator = delta + gamma * (1.0f + 0.34f * wind_speed);
    
    float et0_raw = numerator / denominator;
    
    // Sanity check
    if (et0_raw < 0.0f) et0_raw = 0.0f;
    if (et0_raw_out) {
        *et0_raw_out = et0_raw;
    }

    float et0 = et0_raw;
    if (et0 > ET0_ABSOLUTE_MAX_MM_DAY) {
        LOG_WRN("ET0 calculation unusually high (%.2f mm/day), clamping to %.1f", (double)et0, (double)ET0_ABSOLUTE_MAX_MM_DAY);
        et0 = ET0_ABSOLUTE_MAX_MM_DAY;
    }
    
    LOG_DBG("Penman-Monteith ET0: %.2f mm/day (T=%.1f°C RH=%.0f%% wind=%.1fm/s sun_ratio=%.2f)", 
            (double)et0, (double)temp_mean, (double)env->rel_humidity_pct,
            (double)wind_speed, (double)sunshine_ratio);
    
    return et0;
}

/**
 * @brief Calculate reference evapotranspiration using Hargreaves-Samani equation
 */
float calc_et0_hargreaves_samani(
    const environmental_data_t *env,
    float latitude_rad,
    uint16_t day_of_year
)
{
    if (!env) {
        LOG_ERR("Invalid environmental data");
        return 0.0f;
    }

    // Check for required temperature data
    if (!env->temp_valid) {
        LOG_ERR("Temperature data required for Hargreaves-Samani calculation");
        return 0.0f;
    }

    // Mean temperature
    float temp_mean = env->air_temp_mean_c;
    
    // Temperature range
    float temp_range = env->air_temp_max_c - env->air_temp_min_c;
    if (temp_range < 0.0f) {
        temp_range = 0.0f;
    }
    
    // Extraterrestrial radiation
    float ra = calc_extraterrestrial_radiation(latitude_rad, day_of_year);
    
    // Hargreaves-Samani equation
    float et0 = 0.0023f * (temp_mean + 17.8f) * sqrtf(temp_range) * ra;
    
    // Sanity check
    if (et0 < 0.0f) et0 = 0.0f;
    if (et0 > 15.0f) {
        LOG_WRN("ET0 calculation unusually high (%.2f mm/day), clamping to 15", (double)et0);
        et0 = 15.0f;
    }
    
    LOG_DBG("Hargreaves-Samani ET0: %.2f mm/day (T_mean=%.1f°C, T_range=%.1f°C)", 
            (double)et0, (double)temp_mean, (double)temp_range);
    
    return et0;
}

/**
 * @brief Calculate rainfall intensity and duration characteristics
 * 
 * @param rainfall_mm Total rainfall amount (mm)
 * @param duration_h Estimated rainfall duration (hours)
 * @param intensity_mm_h Calculated rainfall intensity (mm/h)
 */
static void calc_rainfall_characteristics(
    float rainfall_mm,
    float *duration_h,
    float *intensity_mm_h
)
{
    // Estimate rainfall duration based on total amount
    // These are typical relationships for different rainfall intensities
    if (rainfall_mm < 2.0f) {
        *duration_h = 0.5f;        // Light drizzle
    } else if (rainfall_mm < 5.0f) {
        *duration_h = 1.0f;        // Light rain
    } else if (rainfall_mm < 10.0f) {
        *duration_h = 1.5f;        // Moderate rain
    } else if (rainfall_mm < 25.0f) {
        *duration_h = 3.0f;        // Heavy rain
    } else if (rainfall_mm < 50.0f) {
        *duration_h = 6.0f;        // Very heavy rain
    } else {
        *duration_h = 12.0f;       // Extreme rainfall
    }
    
    *intensity_mm_h = rainfall_mm / *duration_h;
    
    LOG_DBG("Rainfall characteristics: %.1f mm over %.1f h (%.1f mm/h)",
            (double)rainfall_mm, (double)*duration_h, (double)*intensity_mm_h);
}

/**
 * @brief Calculate runoff losses based on soil and rainfall characteristics
 * 
 * @param rainfall_intensity_mm_h Rainfall intensity (mm/h)
 * @param soil Soil database entry
 * @param antecedent_moisture_pct Antecedent soil moisture (0-100%)
 * @return Runoff coefficient (0-1)
 */
static float calc_runoff_coefficient(
    float rainfall_intensity_mm_h,
    const soil_enhanced_data_t *soil,
    float antecedent_moisture_pct
)
{
    float infiltration_rate = soil->infil_mm_h;
    float moisture_frac = antecedent_moisture_pct / 100.0f;
    if (moisture_frac < 0.0f) moisture_frac = 0.0f;
    if (moisture_frac > 1.0f) moisture_frac = 1.0f;
    float infil_eff = infiltration_rate * (0.6f + 0.4f * (1.0f - moisture_frac));
    float runoff_coeff = 0.0f;
    
    // Base runoff calculation - if rainfall exceeds infiltration capacity
    if (rainfall_intensity_mm_h > infil_eff) {
        runoff_coeff = (rainfall_intensity_mm_h - infil_eff) / rainfall_intensity_mm_h;
    }
    
    // Adjust for soil texture
    if (strstr(soil->texture, "Clay") || strstr(soil->texture, "clay")) {
        runoff_coeff += 0.05f;  // Clay soils have higher runoff
    } else if (strstr(soil->texture, "Sand") || strstr(soil->texture, "sand")) {
        runoff_coeff -= 0.05f;  // Sandy soils have lower runoff
    }
    
    // Clamp runoff coefficient to reasonable bounds
    if (runoff_coeff < 0.0f) runoff_coeff = 0.0f;
    if (runoff_coeff > 0.8f) runoff_coeff = 0.8f;  // Max 80% runoff
    
    LOG_DBG("Runoff coefficient: %.2f (intensity=%.1f, infil_eff=%.1f, moisture=%.0f%%)",
            (double)runoff_coeff, (double)rainfall_intensity_mm_h, (double)infil_eff, (double)antecedent_moisture_pct);
    
    return runoff_coeff;
}

/**
 * @brief Calculate evaporation losses from rainfall
 * 
 * @param effective_rainfall Rainfall after runoff losses (mm)
 * @param duration_h Rainfall duration (hours)
 * @param temperature_c Air temperature (°C)
 * @return Evaporation loss (mm)
 */
static float calc_evaporation_losses(
    float effective_rainfall,
    float duration_h,
    float temperature_c
)
{
    // Evaporation losses are minimal during rainfall but occur afterward
    // Base evaporation rate depends on temperature and humidity
    float base_evap_rate = 0.1f;  // mm/h base rate
    
    // Adjust for temperature
    if (temperature_c > 25.0f) {
        base_evap_rate += 0.02f * (temperature_c - 25.0f);
    } else if (temperature_c < 15.0f) {
        base_evap_rate -= 0.01f * (15.0f - temperature_c);
    }
    
    // Evaporation occurs mainly in the hours following rainfall
    float evap_duration = fminf(duration_h + 2.0f, 6.0f);  // Max 6 hours
    
    // Light rainfall evaporates more readily
    float evap_factor = 1.0f;
    if (effective_rainfall < 5.0f) {
        evap_factor = 1.5f;  // 50% higher evaporation for light rain
    } else if (effective_rainfall > 20.0f) {
        evap_factor = 0.7f;  // 30% lower evaporation for heavy rain
    }
    
    float evaporation_loss = base_evap_rate * evap_duration * evap_factor;
    
    // Evaporation cannot exceed the available rainfall
    if (evaporation_loss > effective_rainfall * 0.3f) {
        evaporation_loss = effective_rainfall * 0.3f;  // Max 30% loss to evaporation
    }
    
    LOG_DBG("Evaporation loss: %.2f mm (rate=%.2f mm/h, duration=%.1f h, temp=%.1f°C)",
        (double)evaporation_loss, (double)base_evap_rate, (double)evap_duration, (double)temperature_c);
    
    return evaporation_loss;
}

/**
 * @brief Calculate effective precipitation based on soil infiltration capacity
 * 
 * Enhanced version that accounts for rainfall intensity, soil characteristics,
 * antecedent moisture conditions, and evaporation losses.
 * 
 * @param rainfall_mm Total rainfall in mm
 * @param soil Soil data for infiltration calculations
 * @param irrigation_method Irrigation method data
 * @param antecedent_moisture_pct Current soil moisture percentage
 * @param temperature_c Ambient temperature for evaporation calculation (°C)
 */
static float calc_effective_precipitation_core(
    float rainfall_mm,
    const soil_enhanced_data_t *soil,
    const irrigation_method_data_t *irrigation_method,
    float antecedent_moisture_pct,
    float temperature_c,
    float duration_h,
    float intensity_mm_h,
    bool intensity_valid
)
{
    (void)irrigation_method;

    if (!soil || rainfall_mm <= 0.0f) {
        return 0.0f;
    }

    // For very light rainfall (< 1mm), most is lost to evaporation
    if (rainfall_mm < 1.0f) {
        float effective = rainfall_mm * 0.3f;
        LOG_DBG("Light rainfall: %.2f mm -> %.2f mm effective", (double)rainfall_mm, (double)effective);
        return effective;
    }

    if (antecedent_moisture_pct < 0.0f) {
        antecedent_moisture_pct = 0.0f;
    }
    if (antecedent_moisture_pct > 100.0f) {
        antecedent_moisture_pct = 100.0f;
    }

    if (!intensity_valid || intensity_mm_h <= 0.0f || duration_h <= 0.0f) {
        calc_rainfall_characteristics(rainfall_mm, &duration_h, &intensity_mm_h);
    } else {
        if (duration_h <= 0.0f) {
            duration_h = rainfall_mm / intensity_mm_h;
        }
    }

    if (duration_h <= 0.0f) {
        duration_h = 0.5f;
    }
    if (duration_h < 0.05f) {
        duration_h = 0.05f;
    }
    if (duration_h > 24.0f) {
        duration_h = 24.0f;
    }

    intensity_mm_h = rainfall_mm / duration_h;
    if (intensity_mm_h < 0.1f) {
        intensity_mm_h = 0.1f;
    }
    if (intensity_mm_h > RAIN_INTENSITY_MAX_MM_H) {
        intensity_mm_h = RAIN_INTENSITY_MAX_MM_H;
    }

    float runoff_coeff = calc_runoff_coefficient(intensity_mm_h, soil, antecedent_moisture_pct);
    float runoff_loss = rainfall_mm * runoff_coeff;
    float after_runoff = rainfall_mm - runoff_loss;

    float temp_for_evap = temperature_c;
    if (temp_for_evap < -20.0f || temp_for_evap > 50.0f) {
        temp_for_evap = 20.0f;
    }
    float evap_loss = calc_evaporation_losses(after_runoff, duration_h, temp_for_evap);

    float effective_rainfall = after_runoff - evap_loss;
    if (effective_rainfall < 0.0f) {
        effective_rainfall = 0.0f;
    }

    float effectiveness_pct = rainfall_mm > 0 ? (effective_rainfall / rainfall_mm * 100.0f) : 0.0f;
    LOG_INF("Effective precipitation: %.1f mm from %.1f mm rainfall (%.0f%% effective)",
            (double)effective_rainfall, (double)rainfall_mm, (double)effectiveness_pct);
    LOG_DBG("Losses: runoff=%.1f mm (%.0f%%), evaporation=%.1f mm",
            (double)runoff_loss, (double)(runoff_coeff * 100.0f), (double)evap_loss);

    return effective_rainfall;
}

static float calc_effective_precipitation_with_moisture_timing(
    float rainfall_mm,
    const soil_enhanced_data_t *soil,
    const irrigation_method_data_t *irrigation_method,
    float antecedent_moisture_pct,
    float temperature_c,
    float duration_h,
    float intensity_mm_h
)
{
    return calc_effective_precipitation_core(rainfall_mm, soil, irrigation_method,
                                             antecedent_moisture_pct, temperature_c,
                                             duration_h, intensity_mm_h, true);
}
static float calc_effective_precipitation_with_moisture(
    float rainfall_mm,
    const soil_enhanced_data_t *soil,
    const irrigation_method_data_t *irrigation_method,
    float antecedent_moisture_pct,
    float temperature_c
)
{
    return calc_effective_precipitation_core(rainfall_mm, soil, irrigation_method,
                                             antecedent_moisture_pct, temperature_c,
                                             0.0f, 0.0f, false);
}

float calc_effective_precipitation(
    float rainfall_mm,
    const soil_enhanced_data_t *soil,
    const irrigation_method_data_t *irrigation_method
)
{
    float antecedent = (float)soil_moisture_get_global_effective_pct();
    // Use default 20°C for backward compatibility when temperature not available
    return calc_effective_precipitation_with_moisture(rainfall_mm, soil, irrigation_method, antecedent, 20.0f);
}

static float fao56_calc_effective_rain_hourly(uint32_t now_ts,
                                              float rainfall_remainder_mm,
                                              const soil_enhanced_data_t *soil,
                                              const irrigation_method_data_t *irrigation_method,
                                              float antecedent_moisture_pct,
                                              float temperature_c)
{
    if (!soil || rainfall_remainder_mm <= 0.0f) {
        return 0.0f;
    }

    if (temperature_c < -20.0f || temperature_c > 50.0f) {
        temperature_c = 20.0f;
    }

    uint32_t end_hour = now_ts - (now_ts % 3600U);
    uint32_t start_hour = (end_hour >= (24U * 3600U)) ? (end_hour - (24U * 3600U)) : 0U;

    rain_hourly_data_t hourly_entries[32];
    uint16_t entry_count = 0;
    if (rain_history_get_hourly(start_hour, end_hour, hourly_entries,
                                (uint16_t)(sizeof(hourly_entries) / sizeof(hourly_entries[0])),
                                &entry_count) != WATERING_SUCCESS ||
        entry_count == 0U) {
        return -1.0f;
    }

    float total_raw_mm = 0.0f;
    float hourly_mm[32] = {0};
    uint16_t valid_indices[32] = {0};
    uint16_t valid_count = 0;
    for (uint16_t i = 0; i < entry_count; i++) {
        if (hourly_entries[i].data_quality < RAIN_QUALITY_POOR) {
            continue;
        }
        float mm = hourly_entries[i].rainfall_mm_x100 / 100.0f;
        if (mm > 0.0f) {
            total_raw_mm += mm;
            hourly_mm[i] = mm;
            if (valid_count < (uint16_t)(sizeof(valid_indices) / sizeof(valid_indices[0]))) {
                valid_indices[valid_count++] = i;
            }
        }
    }

    if (total_raw_mm <= 0.0f) {
        return 0.0f;
    }

    if (rainfall_remainder_mm > total_raw_mm) {
        rainfall_remainder_mm = total_raw_mm;
    }
    if (rainfall_remainder_mm <= 0.0f) {
        return 0.0f;
    }

    float excess = total_raw_mm - rainfall_remainder_mm;
    if (excess > 0.0f && valid_count > 1U) {
        for (uint16_t i = 0; i < valid_count - 1U; i++) {
            for (uint16_t j = i + 1U; j < valid_count; j++) {
                uint16_t idx_i = valid_indices[i];
                uint16_t idx_j = valid_indices[j];
                if (hourly_entries[idx_j].hour_epoch > hourly_entries[idx_i].hour_epoch) {
                    valid_indices[i] = idx_j;
                    valid_indices[j] = idx_i;
                }
            }
        }
    }

    for (uint16_t k = 0; k < valid_count && excess > 0.0f; k++) {
        uint16_t idx = valid_indices[k];
        float available = hourly_mm[idx];
        if (available <= 0.0f) {
            continue;
        }
        float reduce = (available < excess) ? available : excess;
        hourly_mm[idx] = available - reduce;
        excess -= reduce;
    }

    float effective_total_mm = 0.0f;
    for (uint16_t i = 0; i < entry_count; i++) {
        if (hourly_entries[i].data_quality < RAIN_QUALITY_POOR) {
            continue;
        }
        float mm = hourly_mm[i];
        if (mm <= 0.0f) {
            continue;
        }
        float effective = calc_effective_precipitation_with_moisture_timing(
            mm, soil, irrigation_method, antecedent_moisture_pct, temperature_c, 1.0f, mm);
        effective_total_mm += effective;
    }

    return effective_total_mm;
}

/**
 * @brief Integrate rainfall with irrigation scheduling to prevent over-watering
 * 
 * This function determines if scheduled irrigation should be reduced or cancelled
 * based on recent effective precipitation.
 * 
 * @param scheduled_irrigation_mm Originally scheduled irrigation (mm)
 * @param recent_effective_rain_mm Effective precipitation in last 24-48 hours (mm)
 * @param plant Plant database entry for water requirements
 * @param current_deficit_mm Current soil water deficit (mm)
 * @return Adjusted irrigation amount (mm)
 */
float integrate_rainfall_with_irrigation(
    float scheduled_irrigation_mm,
    float recent_effective_rain_mm,
    const plant_full_data_t *plant,
    float current_deficit_mm
)
{
    if (!plant || scheduled_irrigation_mm <= 0.0f) {
        return scheduled_irrigation_mm;
    }

    // If recent rainfall has already satisfied the deficit, no irrigation needed
    if (recent_effective_rain_mm >= current_deficit_mm) {
        LOG_INF("Recent rainfall (%.1f mm) satisfied deficit (%.1f mm), cancelling irrigation",
                (double)recent_effective_rain_mm, (double)current_deficit_mm);
        return 0.0f;
    }
    
    // Reduce irrigation by the amount of effective rainfall
    float adjusted_irrigation = scheduled_irrigation_mm - recent_effective_rain_mm;
    
    // Ensure we don't over-irrigate beyond the current deficit
    if (adjusted_irrigation > current_deficit_mm) {
        adjusted_irrigation = current_deficit_mm;
    }
    
    // Minimum irrigation threshold - don't irrigate if amount is too small
    float min_irrigation_threshold = 2.0f;  // 2mm minimum
    if (adjusted_irrigation < min_irrigation_threshold) {
        LOG_INF("Adjusted irrigation (%.1f mm) below threshold, cancelling",
                (double)adjusted_irrigation);
        return 0.0f;
    }
    
    // Ensure non-negative result
    if (adjusted_irrigation < 0.0f) {
        adjusted_irrigation = 0.0f;
    }
    
    LOG_INF("Rainfall integration: scheduled=%.1f mm, rain=%.1f mm, adjusted=%.1f mm",
            (double)scheduled_irrigation_mm, (double)recent_effective_rain_mm, (double)adjusted_irrigation);
    
    return adjusted_irrigation;
}

/**
 * @brief Apply environmental stress adjustments to MAD threshold
 * 
 * Adjusts the management allowed depletion based on environmental conditions
 * such as high temperature, low humidity, or wind stress.
 */
float apply_environmental_stress_adjustment(
    float base_mad_fraction,
    const environmental_data_t *env,
    const plant_full_data_t *plant
)
{
    if (!env || !plant) {
        return base_mad_fraction;
    }

    float adjusted_mad = base_mad_fraction;
    
    // Temperature stress adjustment
    if (env->temp_valid) {
        float temp_max = env->air_temp_max_c;
        float temp_opt_max = plant->temp_opt_max_c;
        
        if (temp_max > temp_opt_max + 5.0f) {
            // High temperature stress - reduce MAD to trigger irrigation earlier
            float temp_stress = (temp_max - temp_opt_max - 5.0f) / 10.0f;  // 0-1 scale
            temp_stress = fminf(temp_stress, 0.3f);  // Max 30% reduction
            adjusted_mad -= adjusted_mad * temp_stress;
        LOG_DBG("Temperature stress: %.1f°C > %.1f°C, MAD reduced by %.1f%%",
            (double)temp_max, (double)temp_opt_max, (double)(temp_stress * 100.0f));
        }
    }
    
    // Humidity stress adjustment
    if (env->humidity_valid) {
        if (env->rel_humidity_pct < 30.0f) {
            // Low humidity stress - reduce MAD
            float humidity_stress = (30.0f - env->rel_humidity_pct) / 30.0f;  // 0-1 scale
            humidity_stress = fminf(humidity_stress, 0.2f);  // Max 20% reduction
            adjusted_mad -= adjusted_mad * humidity_stress;
        LOG_DBG("Low humidity stress: %.0f%% < 30%%, MAD reduced by %.1f%%",
            (double)env->rel_humidity_pct, (double)(humidity_stress * 100.0f));
        }
    }
    // Wind sensor removed; wind stress adjustment eliminated
    
    // Ensure MAD doesn't go below reasonable minimum (20% of original)
    float min_mad = base_mad_fraction * 0.2f;
    if (adjusted_mad < min_mad) {
        adjusted_mad = min_mad;
    }
    
    // Ensure MAD doesn't exceed original value
    if (adjusted_mad > base_mad_fraction) {
        adjusted_mad = base_mad_fraction;
    }
    
    LOG_DBG("MAD adjustment: %.3f -> %.3f (%.1f%% of original)",
            (double)base_mad_fraction, (double)adjusted_mad, 
            (double)((adjusted_mad / base_mad_fraction) * 100.0f));
    
    return adjusted_mad;
}

/**
 * @brief Check if irrigation is needed based on Management Allowed Depletion (MAD)
 * 
 * This function implements the irrigation trigger logic based on readily available
 * water depletion thresholds.
 */
bool check_irrigation_trigger_mad(
    const water_balance_t *balance,
    const plant_full_data_t *plant,
    const soil_enhanced_data_t *soil,
    float stress_factor
)
{
    if (!balance || !plant || !soil) {
        LOG_ERR("Invalid parameters for MAD irrigation trigger check");
        return false;
    }

    // Get base MAD fraction from plant database
    float base_mad_fraction = plant->depletion_fraction_p_x1000 / 1000.0f;
    
    // Apply stress factor adjustment (external stress conditions)
    float adjusted_mad_fraction = base_mad_fraction * stress_factor;
    
    // Calculate MAD threshold in mm
    float mad_threshold_mm = balance->wetting_awc_mm * adjusted_mad_fraction;
    
    // Check if current deficit exceeds MAD threshold
    bool irrigation_needed = (balance->current_deficit_mm >= mad_threshold_mm);
    
    // Additional checks for edge cases
    if (irrigation_needed) {
        // Don't trigger if deficit is very small (< 2mm)
        if (balance->current_deficit_mm < 2.0f) {
            irrigation_needed = false;
        LOG_DBG("MAD trigger suppressed: deficit too small (%.1f mm < 2.0 mm)",
            (double)balance->current_deficit_mm);
        }
        
        // Don't trigger if AWC is very small (indicates data issue)
        if (balance->wetting_awc_mm < 5.0f) {
            irrigation_needed = false;
        LOG_WRN("MAD trigger suppressed: AWC too small (%.1f mm < 5.0 mm)",
            (double)balance->wetting_awc_mm);
        }
    }
    
    LOG_DBG("MAD trigger check: deficit=%.1f mm, threshold=%.1f mm (%.1f%% of %.1f mm AWC), trigger=%s",
            (double)balance->current_deficit_mm, (double)mad_threshold_mm,
            (double)(adjusted_mad_fraction * 100.0f), (double)balance->wetting_awc_mm,
            irrigation_needed ? "YES" : "NO");
    
    return irrigation_needed;
}

/**
 * @brief Calculate irrigation timing based on readily available water depletion
 * 
 * This function determines when irrigation should occur based on current
 * water balance and depletion rates.
 */
watering_error_t calc_irrigation_timing(
    const water_balance_t *balance,
    float daily_et_rate,
    const plant_full_data_t *plant,
    float *hours_until_irrigation
)
{
    if (!balance || !plant || !hours_until_irrigation || daily_et_rate <= 0.0f) {
        LOG_ERR("Invalid parameters for irrigation timing calculation");
        return WATERING_ERROR_INVALID_PARAM;
    }

    // Get MAD threshold
    float mad_fraction = plant->depletion_fraction_p_x1000 / 1000.0f;
    float mad_threshold_mm = balance->wetting_awc_mm * mad_fraction;
    
    // Calculate remaining water before irrigation trigger
    float remaining_water_mm = mad_threshold_mm - balance->current_deficit_mm;
    
    // If already at or past trigger point, irrigation needed immediately
    if (remaining_water_mm <= 0.0f) {
        *hours_until_irrigation = 0.0f;
        LOG_DBG("Irrigation needed immediately (deficit=%.1f >= threshold=%.1f mm)",
                (double)balance->current_deficit_mm, (double)mad_threshold_mm);
        return WATERING_SUCCESS;
    }
    
    // Calculate hourly ET rate
    float hourly_et_rate = daily_et_rate / 24.0f;
    
    // Calculate hours until MAD threshold is reached
    *hours_until_irrigation = remaining_water_mm / hourly_et_rate;
    
    // Apply safety margin - trigger 2-4 hours earlier than calculated
    float safety_margin_hours = 3.0f;  // Default 3 hours early
    
    // Adjust safety margin based on ET rate
    if (daily_et_rate > 8.0f) {
        safety_margin_hours = 2.0f;  // High ET - less margin needed
    } else if (daily_et_rate < 3.0f) {
        safety_margin_hours = 4.0f;  // Low ET - more margin for safety
    }
    
    *hours_until_irrigation -= safety_margin_hours;
    
    // Ensure non-negative result
    if (*hours_until_irrigation < 0.0f) {
        *hours_until_irrigation = 0.0f;
    }
    
    // Reasonable maximum - don't schedule more than 7 days out
    if (*hours_until_irrigation > 168.0f) {  // 7 days
        *hours_until_irrigation = 168.0f;
        LOG_WRN("Irrigation timing capped at 7 days (was %.1f hours)", 
                (double)*hours_until_irrigation);
    }
    
    LOG_DBG("Irrigation timing: %.1f hours (remaining=%.1f mm, ET=%.2f mm/h, margin=%.1f h)",
            (double)*hours_until_irrigation, (double)remaining_water_mm, (double)hourly_et_rate, (double)safety_margin_hours);
    
    return WATERING_SUCCESS;
}

/**
 * @brief Calculate localized irrigation wetting pattern adjustments
 * 
 * Determines the wetted area and depth characteristics for drip and micro-irrigation
 * systems, accounting for soil texture and emitter spacing.
 */
watering_error_t calc_localized_wetting_pattern(
    const irrigation_method_data_t *method,
    const soil_enhanced_data_t *soil,
    float emitter_spacing_m,
    float *wetted_diameter_m,
    float *wetted_depth_m
)
{
    if (!method || !soil || !wetted_diameter_m || !wetted_depth_m) {
        LOG_ERR("Invalid parameters for wetting pattern calculation");
        return WATERING_ERROR_INVALID_PARAM;
    }

    // Initialize outputs
    *wetted_diameter_m = 0.0f;
    *wetted_depth_m = 0.0f;

    // Get soil infiltration characteristics
    float infiltration_rate = soil->infil_mm_h;
    
    // Base wetting pattern depends on irrigation method
    float base_diameter_m = 0.5f;  // Default 0.5m diameter
    float base_depth_m = 0.3f;     // Default 0.3m depth
    
    // Adjust based on irrigation method type
    if (strstr(method->method_name, "Drip") || strstr(method->method_name, "drip")) {
        // Drip irrigation - smaller, deeper wetting pattern
        base_diameter_m = 0.4f;
        base_depth_m = 0.4f;
    } else if (strstr(method->method_name, "Micro") || strstr(method->method_name, "micro")) {
        // Micro-spray - wider, shallower pattern
        base_diameter_m = 0.8f;
        base_depth_m = 0.25f;
    } else if (strstr(method->method_name, "Bubbler") || strstr(method->method_name, "bubbler")) {
        // Bubbler - medium pattern
        base_diameter_m = 0.6f;
        base_depth_m = 0.35f;
    }
    
    // Adjust for soil texture
    if (strstr(soil->texture, "Clay") || strstr(soil->texture, "clay")) {
        // Clay soils - wider, shallower wetting
        base_diameter_m *= 1.3f;
        base_depth_m *= 0.8f;
    } else if (strstr(soil->texture, "Sand") || strstr(soil->texture, "sand")) {
        // Sandy soils - narrower, deeper wetting
        base_diameter_m *= 0.8f;
        base_depth_m *= 1.2f;
    } else if (strstr(soil->texture, "Loam") || strstr(soil->texture, "loam")) {
        // Loam soils - balanced wetting (no adjustment)
    }
    
    // Adjust for infiltration rate
    if (infiltration_rate > 20.0f) {
        // High infiltration - deeper, narrower pattern
        base_diameter_m *= 0.9f;
        base_depth_m *= 1.1f;
    } else if (infiltration_rate < 5.0f) {
        // Low infiltration - wider, shallower pattern
        base_diameter_m *= 1.1f;
        base_depth_m *= 0.9f;
    }
    
    // Consider emitter spacing if provided
    if (emitter_spacing_m > 0.0f) {
        // Ensure wetting diameter doesn't exceed spacing for good overlap
        float max_diameter = emitter_spacing_m * 0.8f;  // 80% of spacing
        if (base_diameter_m > max_diameter) {
            base_diameter_m = max_diameter;
        }
        
        // Minimum diameter should be at least 60% of spacing
        float min_diameter = emitter_spacing_m * 0.6f;
        if (base_diameter_m < min_diameter) {
            base_diameter_m = min_diameter;
        }
    }
    
    // Apply reasonable limits
    if (base_diameter_m < 0.2f) base_diameter_m = 0.2f;  // Min 20cm diameter
    if (base_diameter_m > 2.0f) base_diameter_m = 2.0f;  // Max 2m diameter
    if (base_depth_m < 0.1f) base_depth_m = 0.1f;        // Min 10cm depth
    if (base_depth_m > 1.0f) base_depth_m = 1.0f;        // Max 1m depth
    
    *wetted_diameter_m = base_diameter_m;
    *wetted_depth_m = base_depth_m;
    
    LOG_DBG("Wetting pattern: %.2f m diameter, %.2f m depth (method=%s, soil=%s)",
            (double)*wetted_diameter_m, (double)*wetted_depth_m, method->method_name, soil->texture);
    
    return WATERING_SUCCESS;
}

/**
 * @brief Calculate effective root zone water capacity based on irrigation method wetting fraction
 * 
 * For localized irrigation systems (drip, micro-spray), only a fraction of the
 * root zone is wetted, affecting the available water capacity calculations.
 */
float calc_effective_awc_with_wetting_fraction(
    float total_awc_mm,
    const irrigation_method_data_t *method,
    const plant_full_data_t *plant,
    float root_depth_m
)
{
    if (!method || !plant || total_awc_mm <= 0.0f || root_depth_m <= 0.0f) {
        LOG_WRN("Invalid parameters for effective AWC calculation, using total AWC");
        return total_awc_mm;
    }

    // Get wetting fraction from irrigation method (dynamic estimate)
    float wetting_fraction = fao56_get_effective_wetting_fraction(method, NULL, plant);
    
    // Validate wetting fraction
    if (wetting_fraction <= 0.0f || wetting_fraction > 1.0f) {
    LOG_WRN("Invalid wetting fraction (%.3f), using 1.0", (double)wetting_fraction);
        wetting_fraction = 1.0f;
    }
    
    // For localized irrigation, consider root distribution
    // Most roots are in the upper portion of the root zone
    float root_distribution_factor = 1.0f;
    
    // Adjust based on plant characteristics
    float canopy_cover = plant->canopy_cover_max_frac_x1000 / 1000.0f;
    if (canopy_cover > 0.0f && canopy_cover <= 1.0f) {
        // Plants with larger canopy cover can better utilize localized irrigation
        root_distribution_factor = 0.7f + (canopy_cover * 0.3f);  // 0.7-1.0 range
    }
    
    // Calculate effective AWC
    float effective_awc = total_awc_mm * wetting_fraction * root_distribution_factor;
    
    // For very localized systems (wetting fraction < 0.3), apply additional adjustment
    if (wetting_fraction < 0.3f) {
        // Account for lateral water movement in soil
        float lateral_movement_factor = 1.2f;  // 20% increase due to lateral flow
        effective_awc *= lateral_movement_factor;
        
        // But don't exceed the total AWC
        if (effective_awc > total_awc_mm) {
            effective_awc = total_awc_mm;
        }
    }
    
    // Ensure minimum effective AWC (at least 20% of total)
    float min_effective_awc = total_awc_mm * 0.2f;
    if (effective_awc < min_effective_awc) {
        effective_awc = min_effective_awc;
    LOG_DBG("Effective AWC increased to minimum (%.1f mm)", (double)effective_awc);
    }
    
    LOG_DBG("Effective AWC: %.1f mm from %.1f mm total (wetting=%.2f, distribution=%.2f)",
            (double)effective_awc, (double)total_awc_mm, (double)wetting_fraction, (double)root_distribution_factor);
    
    return effective_awc;
}

/**
 * @brief Adjust irrigation volume for partial root zone wetting
 * 
 * For localized irrigation, the irrigation volume needs to be adjusted to account
 * for the fact that only part of the root zone is wetted.
 */
float adjust_volume_for_partial_wetting(
    float base_volume_mm,
    const irrigation_method_data_t *method,
    const plant_full_data_t *plant,
    const soil_enhanced_data_t *soil
)
{
    if (!method || !plant || !soil || base_volume_mm <= 0.0f) {
        return base_volume_mm;
    }

    // Get wetting fraction
    float wetting_fraction = method->wetting_fraction_x1000 / 1000.0f;
    if (wetting_fraction <= 0.0f || wetting_fraction > 1.0f) {
        wetting_fraction = 1.0f;  // Default to full coverage
    }
    
    // For full coverage systems (sprinkler, flood), no adjustment needed
    if (wetting_fraction >= 0.9f) {
        LOG_DBG("Full coverage irrigation, no volume adjustment needed");
        return base_volume_mm;
    }
    
    // Calculate adjustment factor for localized irrigation
    // NOTE (Issue #18 fix): Do NOT divide by wetting_fraction here!
    // The wetting_fraction is already applied to AWC in calc_effective_available_water_capacity()
    // at line 3245: effective_awc = total_awc_mm * wetting_fraction * root_distribution_factor
    // Applying 1/wetting_fraction here would double-compensate.
    // Instead, we only apply soil-specific lateral movement adjustments.
    float volume_adjustment = 1.0f;
    
    // Soil texture adjustment for lateral water movement
    // Sandy soils have less lateral movement, clay soils have more
    if (strstr(soil->texture, "Sand") || strstr(soil->texture, "sand")) {
        // Sandy soils have less lateral water movement, slight increase
        volume_adjustment *= 1.05f;
    } else if (strstr(soil->texture, "Clay") || strstr(soil->texture, "clay")) {
        // Clay soils have better lateral movement, slight decrease
        volume_adjustment *= 0.9f;
    }
    
    // Plant spacing consideration
    float plant_spacing_m = plant->spacing_plant_m_x1000 / 1000.0f;
    if (plant_spacing_m > 0.0f) {
        // Closer spacing allows better water sharing between plants
        if (plant_spacing_m < 0.5f) {
            volume_adjustment *= 0.95f;  // 5% reduction for close spacing
        } else if (plant_spacing_m > 1.5f) {
            volume_adjustment *= 1.05f;  // 5% increase for wide spacing
        }
    }
    
    // Apply reasonable limits - now much tighter since we removed the 1/wf factor
    if (volume_adjustment < 0.9f) volume_adjustment = 0.9f;   // Min 90% of base
    if (volume_adjustment > 1.15f) volume_adjustment = 1.15f; // Max 115% of base
    
    float adjusted_volume = base_volume_mm * volume_adjustment;
    
    LOG_DBG("Volume adjustment for partial wetting: %.1f mm -> %.1f mm (factor=%.2f, wetting=%.2f)",
            (double)base_volume_mm, (double)adjusted_volume, (double)volume_adjustment, (double)wetting_fraction);
    
    return adjusted_volume;
}

/**
 * @brief Track soil water deficit using accumulation method
 * 
 * This function implements deficit accumulation using daily evapotranspiration
 * and precipitation, tracking water balance over time since last irrigation.
 * 
 * @param balance Water balance structure to update
 * @param daily_et Daily evapotranspiration (mm)
 * @param effective_precipitation Effective precipitation (mm)
 * @param irrigation_applied Recent irrigation applied (mm)
 * @return WATERING_SUCCESS on success, error code on failure
 */
static watering_error_t track_deficit_accumulation(
    water_balance_t *balance,
    float daily_et,
    float effective_precipitation,
    float irrigation_applied
)
{
    if (!balance) {
        return WATERING_ERROR_INVALID_PARAM;
    }

    // Add daily ET to deficit
    balance->current_deficit_mm += daily_et;
    
    // Subtract effective precipitation from deficit
    balance->current_deficit_mm -= effective_precipitation;
    
    // Subtract any irrigation applied
    balance->current_deficit_mm -= irrigation_applied;
    
    // Deficit cannot be negative (soil cannot hold more than field capacity)
    if (balance->current_deficit_mm < 0.0f) {
        balance->current_deficit_mm = 0.0f;
    }
    
    // Deficit cannot exceed total available water capacity
    if (balance->current_deficit_mm > balance->wetting_awc_mm) {
        balance->current_deficit_mm = balance->wetting_awc_mm;
    LOG_WRN("Water deficit exceeds AWC, clamping to %.1f mm", (double)balance->wetting_awc_mm);
    }
    
    LOG_DBG("Deficit tracking: ET=%.2f, rain=%.2f, irrigation=%.2f, deficit=%.2f mm",
            (double)daily_et, (double)effective_precipitation, (double)irrigation_applied, (double)balance->current_deficit_mm);
    
    return WATERING_SUCCESS;
}

/**
 * @brief Update water balance tracking with enhanced deficit calculation
 */
watering_error_t calc_water_balance(
    uint8_t channel_id,
    const plant_full_data_t *plant,
    const soil_enhanced_data_t *soil,
    const irrigation_method_data_t *method,
    const environmental_data_t *env,
    float root_depth_current_m,
    uint16_t days_after_planting,
    water_balance_t *balance
)
{
    if (!plant || !soil || !method || !env || !balance) {
        LOG_ERR("Invalid parameters for water balance calculation");
        return WATERING_ERROR_INVALID_PARAM;
    }

    // Calculate root zone available water capacity (mm)
    float awc_mm_per_m = soil->awc_mm_per_m;
    balance->rwz_awc_mm = awc_mm_per_m * root_depth_current_m;
    
    // Adjust for irrigation method wetting fraction (dynamic estimate)
    float wetting_fraction = fao56_get_effective_wetting_fraction(method, soil, plant);
    if (wetting_fraction < FAO56_WF_MIN) wetting_fraction = FAO56_WF_MIN;
    if (wetting_fraction > FAO56_WF_MAX) wetting_fraction = FAO56_WF_MAX;
    balance->wetting_fraction = wetting_fraction;
    fao56_rescale_deficit_for_awc_change(balance, balance->rwz_awc_mm * wetting_fraction);
    
    // Calculate readily available water (RAW) based on management allowed depletion
    float depletion_fraction = plant->depletion_fraction_p_x1000 / 1000.0f;
    balance->raw_mm = balance->wetting_awc_mm * depletion_fraction;
    
    // Calculate effective precipitation
    float effective_rain_mm = calc_effective_precipitation(
        env->rain_mm_24h, soil, method);
    balance->effective_rain_mm = effective_rain_mm;

    if (effective_rain_mm > 0.0f) {
        fao56_apply_surface_wet_event(balance, FAO56_SURFACE_WET_RAIN_FRACTION);
    }

    // Soil sensors removed: always use accumulation method.
    // Compute daily ET0 (mm/day) using the same ensemble + slew pipeline as AUTO.
    float daily_et0 = 0.0f;
    uint16_t day_of_year = fao56_get_current_day_of_year();
    if (day_of_year == 0) {
        day_of_year = 1;
    }
    float latitude_rad = 0.0f;
    if (channel_id < WATERING_CHANNELS_COUNT) {
        watering_channel_t *channel = NULL;
        if (watering_get_channel(channel_id, &channel) == WATERING_SUCCESS && channel) {
            latitude_rad = channel->latitude_deg * (PI / 180.0f);
        }
    }

    float et0_hs = 0.0f;
    float et0_pm = 0.0f;
    daily_et0 = fao56_calc_et0_ensemble(env, latitude_rad, day_of_year, &et0_hs, &et0_pm);

    // Fallback safety: use monthly default when ensemble is unavailable or near-zero.
    if (daily_et0 < 0.05f) {
        uint8_t month = fao56_get_month_from_timestamp(env->timestamp);
        if (month >= 1 && month <= 12) {
            daily_et0 = fao56_get_default_et0_for_month(month);
        } else {
            daily_et0 = FAO56_DEFAULT_ET0_MM_DAY;
        }
    }
    if (daily_et0 < 0.0f) {
        daily_et0 = 0.0f;
    }
    if (daily_et0 > ET0_ABSOLUTE_MAX_MM_DAY) {
        daily_et0 = ET0_ABSOLUTE_MAX_MM_DAY;
    }

    float max_inc = ET0_SLEW_MAX_INC_MM_DAY;
    float max_dec = ET0_SLEW_MAX_DEC_MM_DAY;
    fao56_get_et0_slew_limits(env, &max_inc, &max_dec);
    uint32_t now_s = k_uptime_get_32() / 1000U;
    if (channel_id < WATERING_CHANNELS_COUNT) {
        daily_et0 = fao56_apply_et0_slew(channel_id, daily_et0, now_s, max_inc, max_dec);
    }

    // Initialize surface evaporation bucket (dual-Kc light)
    float surface_wet_target = fao56_get_surface_wet_target(method, wetting_fraction);
    float surface_wet_fraction = fao56_get_surface_wet_fraction(balance, surface_wet_target, daily_et0);
    fao56_update_surface_bucket(balance, soil, surface_wet_fraction);
    
    float daily_kc = 1.0f; // default safety
    float kc_base = fao56_get_kc_base_for_day(plant, days_after_planting);
    daily_kc = fao56_apply_canopy_to_kc(plant, kc_base, days_after_planting);
    float ke = fao56_calc_ke(balance, balance->surface_tew_mm, balance->surface_rew_mm,
                             method, plant, days_after_planting);
    float daily_etc_root = daily_et0 * daily_kc; // mm/day (root only)
    float root_recharge = fao56_route_effective_precipitation(balance, effective_rain_mm);
    watering_error_t err = track_deficit_accumulation(balance, daily_etc_root, root_recharge, 0.0f);
    if (err != WATERING_SUCCESS) {
        LOG_ERR("Deficit accumulation failed");
        return err;
    }
    if (ke > 0.0f && balance->surface_tew_mm > 0.0f) {
        balance->surface_deficit_mm += daily_et0 * ke;
        if (balance->surface_deficit_mm > balance->surface_tew_mm) {
            balance->surface_deficit_mm = balance->surface_tew_mm;
        }
    }
    LOG_DBG("Water balance (assumed met) ET0=%.2f mm Kc=%.2f Ke=%.2f ETroot=%.2f mm deficit=%.2f mm",
            (double)daily_et0, (double)daily_kc, (double)ke, (double)daily_etc_root,
            (double)balance->current_deficit_mm);
    
    // Determine if irrigation is needed based on readily available water depletion
    balance->irrigation_needed = (balance->current_deficit_mm >= balance->raw_mm);
    
    // Update timestamp
    balance->last_update_time = k_uptime_get_32();
    
    LOG_DBG("Water balance: AWC=%.1f mm, RAW=%.1f mm, deficit=%.1f mm, irrigation=%s",
            (double)balance->wetting_awc_mm, (double)balance->raw_mm, (double)balance->current_deficit_mm,
            balance->irrigation_needed ? "needed" : "not needed");
    
    return WATERING_SUCCESS;
}

static float fao56_calc_plant_irrigated_area_m2(
    const plant_full_data_t *plant,
    uint16_t plant_count,
    float *area_per_plant_m2_out,
    float *canopy_factor_out,
    bool log_details
)
{
    if (!plant || plant_count == 0) {
        if (area_per_plant_m2_out) {
            *area_per_plant_m2_out = 0.0f;
        }
        if (canopy_factor_out) {
            *canopy_factor_out = 1.0f;
        }
        return 0.0f;
    }

    float row_spacing_m = plant->spacing_row_m_x1000 / 1000.0f;
    float plant_spacing_m = plant->spacing_plant_m_x1000 / 1000.0f;
    float area_per_plant_m2 = 0.0f;

    if (row_spacing_m > 0.0f && plant_spacing_m > 0.0f) {
        area_per_plant_m2 = row_spacing_m * plant_spacing_m;
        if (log_details) {
            LOG_DBG("Using spacing: %.2f m x %.2f m = %.2f m2/plant",
                    (double)row_spacing_m, (double)plant_spacing_m, (double)area_per_plant_m2);
        }
    } else {
        float density_plants_per_m2 = plant->default_density_plants_m2_x100 / 100.0f;
        if (density_plants_per_m2 > 0.0f) {
            area_per_plant_m2 = 1.0f / density_plants_per_m2;
            if (log_details) {
                LOG_DBG("Using density: %.2f plants/m2 = %.2f m2/plant",
                        (double)density_plants_per_m2, (double)area_per_plant_m2);
            }
        } else {
            area_per_plant_m2 = 1.0f;
            if (log_details) {
                LOG_WRN("No spacing/density data, using default 1 m2/plant");
            }
        }
    }

    if (area_per_plant_m2 < 0.002f) {
        if (log_details) {
            LOG_DBG("Dense crop detected: %.4f m2/plant clamped to 0.002 m2",
                    (double)area_per_plant_m2);
        }
        area_per_plant_m2 = 0.002f;
    } else if (area_per_plant_m2 > 100.0f) {
        area_per_plant_m2 = 100.0f;
        if (log_details) {
            LOG_WRN("Area per plant too large, using maximum 100 m2");
        }
    }

    float canopy_cover = plant->canopy_cover_max_frac_x1000 / 1000.0f;
    float canopy_factor = 1.0f;
    if (canopy_cover > 0.0f && canopy_cover <= 1.0f) {
        canopy_factor = canopy_cover;
        if (log_details) {
            LOG_DBG("Canopy cover factor: %.1f%%", (double)(canopy_cover * 100.0f));
        }
    } else if (canopy_cover > 1.0f) {
        canopy_factor = 1.0f;
        if (log_details) {
            LOG_DBG("Canopy cover > 100%%; using full canopy factor");
        }
    }

    if (area_per_plant_m2_out) {
        *area_per_plant_m2_out = area_per_plant_m2;
    }
    if (canopy_factor_out) {
        *canopy_factor_out = canopy_factor;
    }

    return area_per_plant_m2 * plant_count;
}

/**
 * @brief Calculate irrigation volume for area-based coverage with enhanced accuracy
 * 
 * Enhanced version that includes proper metric conversions and distribution
 * uniformity adjustments.
 */
watering_error_t calc_irrigation_volume_area(
    const water_balance_t *balance,
    const irrigation_method_data_t *method,
    float area_m2,
    bool eco_mode,
    float max_volume_limit_l,
    irrigation_calculation_t *result
)
{
    if (!balance || !method || !result || area_m2 <= 0.0f) {
        LOG_ERR("Invalid parameters for area-based volume calculation");
        return WATERING_ERROR_INVALID_PARAM;
    }

    // Initialize result structure
    memset(result, 0, sizeof(irrigation_calculation_t));

    // Start with net irrigation requirement (mm)
    result->net_irrigation_mm = balance->current_deficit_mm;
    
    if (eco_mode) {
        result->net_irrigation_mm *= ECO_ETC_FACTOR;
        LOG_DBG("Eco mode: net refill scaled by %.2f", (double)ECO_ETC_FACTOR);
    }
    
    // Get irrigation method efficiency
    float efficiency = method->efficiency_pct / 100.0f;
    if (efficiency <= 0.0f || efficiency > 1.0f) {
        LOG_WRN("Invalid irrigation efficiency (%d%%), using 80%%", 
                (int)method->efficiency_pct);
        efficiency = 0.8f;
    }
    
    // Wetting fraction already applied in water balance (AWC/RAW); avoid double scaling here.
    
    // Convert net to gross irrigation accounting for efficiency
    result->gross_irrigation_mm = result->net_irrigation_mm / efficiency;
    
    // Apply distribution uniformity adjustment
    float distribution_uniformity = method->distribution_uniformity_pct / 100.0f;
    if (distribution_uniformity > 0.0f && distribution_uniformity < 1.0f) {
        // Poor distribution uniformity requires more water to ensure adequate coverage
        float uniformity_adjustment = 1.0f / distribution_uniformity;
        result->gross_irrigation_mm *= uniformity_adjustment;
        
    LOG_DBG("Distribution uniformity adjustment: factor=%.2f (uniformity=%.0f%%)",
        (double)uniformity_adjustment, (double)(distribution_uniformity * 100.0f));
    }
    
    // Convert depth (mm) to volume (L) with proper metric conversion
    // 1 mm over 1 m² = 1 liter (exact conversion)
    result->volume_liters = result->gross_irrigation_mm * area_m2;
    
    // Volume per plant not applicable for area-based calculation
    result->volume_per_plant_liters = 0.0f;
    
    // Apply reasonable minimum volume threshold
    float min_volume_threshold = 0.5f;  // 0.5L minimum
    if (result->volume_liters < min_volume_threshold) {
    LOG_DBG("Volume below threshold (%.2f L < %.2f L), setting to zero",
        (double)result->volume_liters, (double)min_volume_threshold);
        result->volume_liters = 0.0f;
        result->gross_irrigation_mm = 0.0f;
        result->net_irrigation_mm = 0.0f;
        return WATERING_SUCCESS;
    }
    
    // Check against maximum volume limit
    if (max_volume_limit_l > 0.0f && result->volume_liters > max_volume_limit_l) {
    LOG_INF("Volume limited: %.1f L reduced to %.1f L", 
        (double)result->volume_liters, (double)max_volume_limit_l);
        result->volume_liters = max_volume_limit_l;
        result->volume_limited = true;
        
        // Recalculate actual application depth
        result->gross_irrigation_mm = result->volume_liters / area_m2;
        result->net_irrigation_mm = result->gross_irrigation_mm * efficiency;
    }
    
    // Initialize cycle parameters (will be set by cycle_and_soak if needed)
    result->cycle_count = 1;
    result->cycle_duration_min = 0;  // To be calculated based on flow rate
    result->soak_interval_min = 0;
    
    LOG_DBG("Enhanced area-based volume: %.1f L for %.1f m² (%.2f mm gross, eff=%.0f%%)",
            (double)result->volume_liters, (double)area_m2, (double)result->gross_irrigation_mm, 
            (double)(efficiency * 100.0f));
    
    return WATERING_SUCCESS;
}

/**
 * @brief Calculate irrigation volume for plant-count-based coverage with enhanced accuracy
 * 
 * Enhanced version that includes spacing calculations, density factors, canopy cover
 * adjustments, and localized irrigation considerations.
 */
watering_error_t calc_irrigation_volume_plants(
    const water_balance_t *balance,
    const irrigation_method_data_t *method,
    const plant_full_data_t *plant,
    uint16_t plant_count,
    bool eco_mode,
    float max_volume_limit_l,
    irrigation_calculation_t *result
)
{
    if (!balance || !method || !plant || !result || plant_count == 0) {
        LOG_ERR("Invalid parameters for plant-based volume calculation");
        return WATERING_ERROR_INVALID_PARAM;
    }

    // Initialize result structure
    memset(result, 0, sizeof(irrigation_calculation_t));

    float area_per_plant_m2 = 0.0f;
    float canopy_factor = 1.0f;
    float total_irrigated_area_m2 = fao56_calc_plant_irrigated_area_m2(
        plant, plant_count, &area_per_plant_m2, &canopy_factor, true);

    if (total_irrigated_area_m2 <= 0.0f) {
        LOG_ERR("Invalid irrigated area for plant-based calculation");
        return WATERING_ERROR_INVALID_DATA;
    }
    
    // Get irrigation method characteristics
    float efficiency = method->efficiency_pct / 100.0f;
    if (efficiency <= 0.0f || efficiency > 1.0f) {
    LOG_WRN("Invalid irrigation efficiency (%d%%), using 80%%",
        (int)method->efficiency_pct);
        efficiency = 0.8f;
    }
    
    // Start with net irrigation requirement
    result->net_irrigation_mm = balance->current_deficit_mm;
    
    if (eco_mode) {
        result->net_irrigation_mm *= ECO_ETC_FACTOR;
        LOG_DBG("Eco mode: net refill scaled by %.2f", (double)ECO_ETC_FACTOR);
    }
    
    // Wetting fraction already applied in water balance (AWC/RAW); avoid double scaling here.
    
    // Convert net to gross irrigation accounting for efficiency
    result->gross_irrigation_mm = result->net_irrigation_mm / efficiency;
    
    // Apply distribution uniformity if available
    float distribution_uniformity = method->distribution_uniformity_pct / 100.0f;
    if (distribution_uniformity > 0.0f && distribution_uniformity < 1.0f) {
        float uniformity_adjustment = 1.0f / distribution_uniformity;
        result->gross_irrigation_mm *= uniformity_adjustment;
    LOG_DBG("Distribution uniformity adjustment: factor=%.2f", (double)uniformity_adjustment);
    }
    
    // Wetting fraction already reflected in water balance; keep area consistent here.
    float effective_irrigated_area_m2 = total_irrigated_area_m2;
    
    // Convert to total volume with proper metric conversion
    result->volume_liters = result->gross_irrigation_mm * effective_irrigated_area_m2;
    
    // Calculate volume per plant
    result->volume_per_plant_liters = result->volume_liters / plant_count;
    
    // Apply minimum TOTAL volume threshold (not per-plant for dense crops)
    // For dense crops like wheat (300+ plants/m²), per-plant threshold makes no sense.
    // Use total area-based minimum: 0.1L per m² of effective irrigated area.
    float min_total_volume = effective_irrigated_area_m2 * 0.1f;  // 0.1 L/m² minimum
    if (min_total_volume < 0.5f) {
        min_total_volume = 0.5f;  // Absolute minimum 0.5L to trigger valve
    }
    
    if (result->volume_liters > 0.0f && result->volume_liters < min_total_volume) {
        LOG_DBG("Total volume below threshold (%.3f L < %.1f L for %.2f m²), setting to zero",
                (double)result->volume_liters, (double)min_total_volume, 
                (double)effective_irrigated_area_m2);
        result->volume_liters = 0.0f;
        result->volume_per_plant_liters = 0.0f;
        result->gross_irrigation_mm = 0.0f;
        result->net_irrigation_mm = 0.0f;
        return WATERING_SUCCESS;
    }
    
    // Check against maximum volume limit
    if (max_volume_limit_l > 0.0f && result->volume_liters > max_volume_limit_l) {
    LOG_INF("Volume limited: %.1f L reduced to %.1f L", 
        (double)result->volume_liters, (double)max_volume_limit_l);
        result->volume_liters = max_volume_limit_l;
        result->volume_per_plant_liters = result->volume_liters / plant_count;
        result->volume_limited = true;
        
        // Recalculate actual application depth
        result->gross_irrigation_mm = result->volume_liters / effective_irrigated_area_m2;
        result->net_irrigation_mm = result->gross_irrigation_mm * efficiency;
    }
    
    // Initialize cycle parameters
    result->cycle_count = 1;
    result->cycle_duration_min = 0;
    result->soak_interval_min = 0;
    
    LOG_DBG("Enhanced plant-based volume: %.1f L for %d plants (%.2f L/plant, %.2f m²/plant, canopy=%.1f%%)",
            (double)result->volume_liters, plant_count, (double)result->volume_per_plant_liters,
            (double)area_per_plant_m2, (double)(canopy_factor * 100.0f));
    
    return WATERING_SUCCESS;
}

/**
 * @brief Determine if cycle and soak irrigation is needed
 */
watering_error_t calc_cycle_and_soak(
    const irrigation_method_data_t *method,
    const soil_enhanced_data_t *soil,
    float application_rate_mm_h,
    irrigation_calculation_t *result
)
{
    if (!method || !result) {
        LOG_ERR("Invalid parameters for cycle and soak calculation");
        return WATERING_ERROR_INVALID_PARAM;
    }

    /* Soil may be unavailable in some call paths (e.g., BLE/UI preview).
     * In that case, default to a single continuous cycle and avoid error spam.
     */
    if (!soil) {
        result->cycle_count = 1;
        result->soak_interval_min = 0;

        if (application_rate_mm_h <= 0.0f) {
            application_rate_mm_h = (method->application_rate_min_mm_h +
                                    method->application_rate_max_mm_h) / 2.0f;
        }

        if (result->gross_irrigation_mm > 0.0f && application_rate_mm_h > 0.0f) {
            float duration_hours = result->gross_irrigation_mm / application_rate_mm_h;
            result->cycle_duration_min = (uint16_t)(duration_hours * 60.0f);
        } else {
            result->cycle_duration_min = 0;
        }

        LOG_DBG("Cycle/soak skipped (no soil data) - single irrigation of %d minutes",
                result->cycle_duration_min);
        return WATERING_SUCCESS;
    }

    // Get soil infiltration rate
    float soil_infiltration_rate = soil->infil_mm_h;
    
    // If application rate is not provided, use method's typical rate
    if (application_rate_mm_h <= 0.0f) {
        // Use average of min and max application rates
        application_rate_mm_h = (method->application_rate_min_mm_h + 
                                method->application_rate_max_mm_h) / 2.0f;
        
        if (application_rate_mm_h <= 0.0f) {
            // Fallback default rates based on irrigation method
            if (strstr(method->method_name, "Drip") || 
                strstr(method->method_name, "drip")) {
                application_rate_mm_h = 2.0f;  // Typical drip rate
            } else if (strstr(method->method_name, "Sprinkler") || 
                      strstr(method->method_name, "sprinkler")) {
                application_rate_mm_h = 10.0f;  // Typical sprinkler rate
            } else {
                application_rate_mm_h = 5.0f;   // Generic default
            }
        }
    }
    
    LOG_DBG("Application rate: %.1f mm/h, Soil infiltration: %.1f mm/h",
            (double)application_rate_mm_h, (double)soil_infiltration_rate);
    
    // Check if application rate exceeds soil infiltration capacity
    if (application_rate_mm_h <= soil_infiltration_rate * 1.2f) {
        // No cycle and soak needed - soil can handle the application rate
        // (1.2x factor provides some safety margin)
        result->cycle_count = 1;
        result->soak_interval_min = 0;
        
        // Calculate duration based on total volume and application rate
        if (result->gross_irrigation_mm > 0.0f && application_rate_mm_h > 0.0f) {
            float duration_hours = result->gross_irrigation_mm / application_rate_mm_h;
            result->cycle_duration_min = (uint16_t)(duration_hours * 60.0f);
        }
        
    LOG_DBG("No cycle/soak needed - single irrigation of %d minutes",
        result->cycle_duration_min);
        
        return WATERING_SUCCESS;
    }
    
    // Cycle and soak is needed
    LOG_INF("Cycle and soak required: app rate %.1f > soil rate %.1f mm/h",
            (double)application_rate_mm_h, (double)soil_infiltration_rate);
    
    // Calculate optimal cycle parameters
    // Target application rate should not exceed 80% of soil infiltration rate
    float target_rate = soil_infiltration_rate * 0.8f;
    
    // Calculate number of cycles needed
    float cycle_ratio = application_rate_mm_h / target_rate;
    result->cycle_count = (uint8_t)ceilf(cycle_ratio);
    
    // Limit to reasonable number of cycles (2-6)
    if (result->cycle_count < 2) result->cycle_count = 2;
    if (result->cycle_count > 6) result->cycle_count = 6;
    
    // Calculate cycle duration and soak interval using actual application rate
    float depth_per_cycle_mm = (result->cycle_count > 0) ?
        (result->gross_irrigation_mm / result->cycle_count) : 0.0f;
    float cycle_duration_hours = 0.0f;
    if (depth_per_cycle_mm > 0.0f && application_rate_mm_h > 0.0f) {
        cycle_duration_hours = depth_per_cycle_mm / application_rate_mm_h;
    }

    result->cycle_duration_min = (uint16_t)(cycle_duration_hours * 60.0f);
    
    // Soak interval should allow water to infiltrate
    // Rule of thumb: soak time = 2-4x cycle time, depending on soil type
    float soak_multiplier;
    if (strstr(soil->texture, "Clay") || strstr(soil->texture, "clay")) {
        soak_multiplier = 4.0f;  // Clay soils need longer soak time
    } else if (strstr(soil->texture, "Loam") || strstr(soil->texture, "loam")) {
        soak_multiplier = 3.0f;  // Loam soils moderate soak time
    } else {
        soak_multiplier = 2.0f;  // Sandy soils shorter soak time
    }
    
    result->soak_interval_min = (uint16_t)(result->cycle_duration_min * soak_multiplier);
    
    // Ensure minimum and maximum limits
    if (result->cycle_duration_min < 5) result->cycle_duration_min = 5;    // Min 5 minutes
    if (result->cycle_duration_min > 60) result->cycle_duration_min = 60;  // Max 1 hour
    
    if (result->soak_interval_min < 10) result->soak_interval_min = 10;    // Min 10 minutes
    if (result->soak_interval_min > 240) result->soak_interval_min = 240;  // Max 4 hours
    
    LOG_INF("Cycle and soak: %d cycles of %d min with %d min soak intervals",
            result->cycle_count, result->cycle_duration_min, result->soak_interval_min);
    
    return WATERING_SUCCESS;
}

/**
 * @brief Apply quality irrigation mode (100% of calculated requirement)
 * 
 * Quality mode applies the full calculated irrigation requirement for optimal plant health.
 * This mode prioritizes plant performance over water conservation.
 * 
 * @param balance Current water balance state
 * @param method Irrigation method database entry
 * @param plant Plant database entry (for plant-based calculations)
 * @param area_m2 Area to irrigate (for area-based calculations, 0 for plant-based)
 * @param plant_count Number of plants (for plant-based calculations, 0 for area-based)
 * @param application_rate_mm_h Application rate override (mm/h, 0 to use method defaults)
 * @param application_rate_mm_h Application rate override (mm/h, 0 to use method defaults)
 * @param max_volume_limit_l Maximum volume limit (liters)
 * @param result Calculation results structure
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t apply_quality_irrigation_mode(
    const water_balance_t *balance,
    const irrigation_method_data_t *method,
    const soil_enhanced_data_t *soil,
    const plant_full_data_t *plant,
    float area_m2,
    uint16_t plant_count,
    float application_rate_mm_h,
    float max_volume_limit_l,
    irrigation_calculation_t *result
)
{
    if (!balance || !method || !result) {
        LOG_ERR("Invalid parameters for quality irrigation mode");
        return WATERING_ERROR_INVALID_PARAM;
    }

    watering_error_t err;
    bool eco_mode = false;  // Quality mode = 100% of calculated requirement

    // Determine calculation method based on parameters
    if (area_m2 > 0.0f && plant_count == 0) {
        // Area-based calculation
        err = calc_irrigation_volume_area(balance, method, area_m2, 
                                        eco_mode, max_volume_limit_l, result);
    LOG_INF("Quality mode: %.1f L for %.1f m² (100%% requirement)", 
        (double)result->volume_liters, (double)area_m2);
    } else if (plant_count > 0 && area_m2 == 0.0f && plant != NULL) {
        // Plant-based calculation
        err = calc_irrigation_volume_plants(balance, method, plant, plant_count,
                                          eco_mode, max_volume_limit_l, result);
    LOG_INF("Quality mode: %.1f L for %d plants (100%% requirement)", 
        (double)result->volume_liters, plant_count);
    } else {
        LOG_ERR("Invalid parameters: must specify either area_m2 OR plant_count");
        return WATERING_ERROR_INVALID_PARAM;
    }

    if (err != WATERING_SUCCESS) {
        LOG_ERR("Quality mode calculation failed: %d", err);
        return err;
    }

    // Apply cycle and soak logic if needed
    err = calc_cycle_and_soak(method, soil, application_rate_mm_h, result);
    if (err != WATERING_SUCCESS) {
        LOG_WRN("Cycle and soak calculation failed, using single cycle");
        result->cycle_count = 1;
        result->soak_interval_min = 0;
    }

    LOG_INF("Quality irrigation mode applied: %.1f L total, %d cycles", 
            (double)result->volume_liters, result->cycle_count);

    return WATERING_SUCCESS;
}

/**
 * @brief Apply eco irrigation mode (reduced refill target)
 * 
 * Eco mode reduces refill volume without altering ET physics.
 * 
 * @param balance Current water balance state
 * @param method Irrigation method database entry
 * @param plant Plant database entry (for plant-based calculations)
 * @param area_m2 Area to irrigate (for area-based calculations, 0 for plant-based)
 * @param plant_count Number of plants (for plant-based calculations, 0 for area-based)
 * @param max_volume_limit_l Maximum volume limit (liters)
 * @param result Calculation results structure
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t apply_eco_irrigation_mode(
    const water_balance_t *balance,
    const irrigation_method_data_t *method,
    const soil_enhanced_data_t *soil,
    const plant_full_data_t *plant,
    float area_m2,
    uint16_t plant_count,
    float application_rate_mm_h,
    float max_volume_limit_l,
    irrigation_calculation_t *result
)
{
    if (!balance || !method || !result) {
        LOG_ERR("Invalid parameters for eco irrigation mode");
        return WATERING_ERROR_INVALID_PARAM;
    }

    watering_error_t err;
    bool eco_mode = true;  // Eco scales net refill volume

    // Determine calculation method based on parameters
    if (area_m2 > 0.0f && plant_count == 0) {
        // Area-based calculation
        err = calc_irrigation_volume_area(balance, method, area_m2, 
                                        eco_mode, max_volume_limit_l, result);
    LOG_INF("Eco mode: %.1f L for %.1f m2 (scaled refill)",
        (double)result->volume_liters, (double)area_m2);
    } else if (plant_count > 0 && area_m2 == 0.0f && plant != NULL) {
        // Plant-based calculation
        err = calc_irrigation_volume_plants(balance, method, plant, plant_count,
                                          eco_mode, max_volume_limit_l, result);
    LOG_INF("Eco mode: %.1f L for %d plants (scaled refill)",
        (double)result->volume_liters, plant_count);
    } else {
        LOG_ERR("Invalid parameters: must specify either area_m2 OR plant_count");
        return WATERING_ERROR_INVALID_PARAM;
    }

    if (err != WATERING_SUCCESS) {
        LOG_ERR("Eco mode calculation failed: %d", err);
        return err;
    }

    // Apply cycle and soak logic if needed
    err = calc_cycle_and_soak(method, soil, application_rate_mm_h, result);
    if (err != WATERING_SUCCESS) {
        LOG_WRN("Cycle and soak calculation failed, using single cycle");
        result->cycle_count = 1;
        result->soak_interval_min = 0;
    }

    LOG_INF("Eco irrigation mode applied: %.1f L total, %d cycles",
            (double)result->volume_liters, result->cycle_count);

    return WATERING_SUCCESS;
}

/**
 * @brief Apply maximum volume limiting with constraint logging
 * 
 * This function enforces user-configurable maximum irrigation limits per channel
 * and logs when calculated volumes exceed the limits.
 * 
 * @param calculated_volume_l Originally calculated volume (liters)
 * @param max_volume_limit_l Maximum allowed volume (liters)
 * @param channel_id Channel ID for logging
 * @param mode_name Mode name for logging ("Quality" or "Eco")
 * @return Limited volume (liters)
 */
float apply_volume_limiting(
    float calculated_volume_l,
    float max_volume_limit_l,
    uint8_t channel_id,
    const char *mode_name
)
{
    if (max_volume_limit_l <= 0.0f) {
        // No limit configured
        return calculated_volume_l;
    }

    if (calculated_volume_l <= max_volume_limit_l) {
        // Within limits
    LOG_DBG("Channel %d %s mode: %.1f L within limit (%.1f L)", 
        channel_id, mode_name ? mode_name : "irrigation", 
        (double)calculated_volume_l, (double)max_volume_limit_l);
        return calculated_volume_l;
    }

    // Volume exceeds limit - apply constraint and log
    float reduction_pct = ((calculated_volume_l - max_volume_limit_l) / calculated_volume_l) * 100.0f;
    
    LOG_WRN("Channel %d %s volume limited: %.1f L reduced to %.1f L (%.1f%% reduction)",
            channel_id, mode_name ? mode_name : "irrigation",
            (double)calculated_volume_l, (double)max_volume_limit_l, (double)reduction_pct);

    // Add to watering log for historical tracking
    watering_log_constraint(channel_id, calculated_volume_l, max_volume_limit_l, mode_name);

    return max_volume_limit_l;
}

/**
 * @brief Calculate irrigation requirement using FAO-56 method
 * 
 * This is the main entry point for FAO-56 based irrigation calculations.
 * It integrates all the components to provide a complete irrigation recommendation.
 * 
 * @param channel_id Channel ID
 * @param env Environmental data
 * @param result Calculation result structure to be filled
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t fao56_calculate_irrigation_requirement(uint8_t channel_id, 
                                                       const environmental_data_t *env,
                                                       irrigation_calculation_t *result)
{
    if (!env || !result || channel_id >= WATERING_CHANNELS_COUNT) {
        LOG_ERR("Invalid parameters for FAO-56 calculation");
        return WATERING_ERROR_INVALID_PARAM;
    }

    // Initialize result structure
    memset(result, 0, sizeof(irrigation_calculation_t));

    // Check if resource-constrained mode is active
    if (fao56_is_resource_constrained()) {
        LOG_INF("Using simplified calculation for resource-constrained mode");
        return fao56_calculate_simplified_irrigation(channel_id, env, result);
    }

    // Get channel configuration
    watering_channel_t *channel;
    watering_error_t err = watering_get_channel(channel_id, &channel);
    if (err != WATERING_SUCCESS) {
        LOG_ERR("Failed to get channel %u configuration: %d", channel_id, err);
        return err;
    }

    // Check if automatic mode is enabled (prefer auto_mode, fall back to event mode)
    watering_mode_t calc_mode = channel->auto_mode;
    if (calc_mode != WATERING_AUTOMATIC_QUALITY && calc_mode != WATERING_AUTOMATIC_ECO) {
        watering_mode_t event_mode = channel->watering_event.watering_mode;
        if (event_mode == WATERING_AUTOMATIC_QUALITY || event_mode == WATERING_AUTOMATIC_ECO) {
            calc_mode = event_mode;
        }
    }
    if (calc_mode != WATERING_AUTOMATIC_QUALITY && calc_mode != WATERING_AUTOMATIC_ECO) {
        LOG_DBG("Channel %u not in automatic mode, skipping FAO-56 calculation", channel_id);
        return WATERING_ERROR_CONFIG;
    }

    // Validate environmental data
    environmental_data_t validated_env;
    err = fao56_validate_environmental_data(env, &validated_env);
    if (err != WATERING_SUCCESS) {
        LOG_WRN("Environmental data validation failed, attempting sensor failure handling");
        err = fao56_handle_sensor_failure(env, &validated_env);
        if (err != WATERING_SUCCESS) {
            LOG_ERR("Cannot recover from sensor failures, using fallback");
            return fao56_handle_calculation_error(channel_id, err, env, result) == FAO56_RECOVERY_NONE ? 
                   WATERING_ERROR_HARDWARE : WATERING_SUCCESS;
        }
    }

    // Update deficit using the same AUTO engine (ETc in realtime + daily rain reconciliation)
    err = fao56_realtime_update_deficit(channel_id, &validated_env);
    if (err != WATERING_SUCCESS) {
        LOG_WRN("Realtime deficit update failed for channel %u: %d", channel_id, err);
        return err;
    }

    if (channel->watering_event.schedule_type != SCHEDULE_AUTO) {
        uint16_t current_julian_day = fao56_get_current_day_of_year();
        if (current_julian_day != 0 &&
            (channel->last_auto_check_julian_day != current_julian_day || !channel->auto_check_ran_today)) {
            fao56_auto_decision_t daily_decision;
            watering_error_t daily_err = fao56_daily_update_deficit(channel_id, &daily_decision);
            if (daily_err == WATERING_SUCCESS) {
                channel->last_auto_check_julian_day = current_julian_day;
                channel->auto_check_ran_today = true;
            } else {
                LOG_WRN("Daily rain update failed for channel %u: %d", channel_id, daily_err);
            }
        }
    }

    water_balance_t *balance = channel->water_balance;
    if (!balance) {
        LOG_ERR("Channel %u has no water balance state", channel_id);
        return WATERING_ERROR_INVALID_PARAM;
    }

    const plant_full_data_t *plant = fao56_get_channel_plant(channel, channel_id);
    const irrigation_method_data_t *method = irrigation_db_get_by_index(channel->irrigation_method_index);
    const soil_enhanced_data_t *soil = fao56_get_channel_soil(channel_id, channel);
    if (!plant || !method || !soil) {
        LOG_ERR("Channel %u missing plant/soil/method data for FAO-56 calc", channel_id);
        return WATERING_ERROR_INVALID_DATA;
    }

    float area_m2 = channel->use_area_based ? channel->coverage.area_m2 : 0.0f;
    uint16_t plant_count = channel->use_area_based ? 0 : channel->coverage.plant_count;
    float application_rate_mm_h = 0.0f;
    if (channel->hydraulic.nominal_flow_ml_min > 0) {
        float flow_l_min = channel->hydraulic.nominal_flow_ml_min / 1000.0f;
        float area_for_rate = area_m2;
        if (!channel->use_area_based) {
            float area_per_plant_m2 = 0.0f;
            float canopy_factor = 1.0f;
            area_for_rate = fao56_calc_plant_irrigated_area_m2(
                plant, plant_count, &area_per_plant_m2, &canopy_factor, false);
            if (area_for_rate <= 0.0f) {
                area_for_rate = plant_count * 0.5f;
            }
        }
        if (area_for_rate > 0.0f) {
            application_rate_mm_h = (flow_l_min * 60.0f) / area_for_rate;
        }
    }

    if (calc_mode == WATERING_AUTOMATIC_ECO) {
        err = apply_eco_irrigation_mode(balance, method, soil, plant,
                                        area_m2, plant_count,
                                        application_rate_mm_h,
                                        channel->max_volume_limit_l, result);
    } else {
        err = apply_quality_irrigation_mode(balance, method, soil, plant,
                                            area_m2, plant_count,
                                            application_rate_mm_h,
                                            channel->max_volume_limit_l, result);
    }
    if (err != WATERING_SUCCESS) {
        LOG_ERR("FAO-56 volume calculation failed for channel %u: %d", channel_id, err);
        return err;
    }

    LOG_INF("FAO-56 auto calc for channel %u: deficit=%.2f mm, volume=%.1f L %s",
            channel_id,
            (double)balance->current_deficit_mm,
            (double)result->volume_liters,
            calc_mode == WATERING_AUTOMATIC_ECO ? "(eco)" : "(quality)");

    // Update channel's last calculation time
    channel->last_calculation_time = k_uptime_get_32() / 1000;

    return WATERING_SUCCESS;
}

/* ================================================================== */
/* AUTO (Smart Schedule) Mode - Daily Deficit Tracking              */
/* ================================================================== */

/* Shared per-channel water balance backing store for AUTO mode.
 * This avoids per-call static allocation differences between helpers.
 */
static water_balance_t s_auto_channel_balance[WATERING_CHANNELS_COUNT];
static float s_rain_applied_surface_mm[WATERING_CHANNELS_COUNT];
static float s_rain_applied_root_mm[WATERING_CHANNELS_COUNT];

water_balance_t *fao56_bind_channel_balance(uint8_t channel_id, watering_channel_t *channel)
{
    if (channel_id >= WATERING_CHANNELS_COUNT) {
        return NULL;
    }

    watering_channel_t *resolved = channel;
    if (!resolved) {
        if (watering_get_channel(channel_id, &resolved) != WATERING_SUCCESS || !resolved) {
            return NULL;
        }
    }

    if (resolved->water_balance) {
        return resolved->water_balance;
    }

    resolved->water_balance = &s_auto_channel_balance[channel_id];
    memset(resolved->water_balance, 0, sizeof(water_balance_t));
    s_rain_applied_surface_mm[channel_id] = 0.0f;
    s_rain_applied_root_mm[channel_id] = 0.0f;
    s_rain_applied_raw_mm[channel_id] = 0.0f;
    return resolved->water_balance;
}

watering_error_t fao56_realtime_update_deficit(uint8_t channel_id,
                                              const environmental_data_t *env)
{
    if (channel_id >= WATERING_CHANNELS_COUNT || !env) {
        return WATERING_ERROR_INVALID_PARAM;
    }

    watering_channel_t *channel = NULL;
    watering_error_t err = watering_get_channel(channel_id, &channel);
    if (err != WATERING_SUCCESS || !channel) {
        return WATERING_ERROR_INVALID_PARAM;
    }

    if (!watering_channel_auto_mode_valid(channel)) {
        return WATERING_ERROR_CONFIG;
    }

    // Ensure water balance structure exists
    water_balance_t *balance = fao56_bind_channel_balance(channel_id, channel);
    if (!balance) {
        return WATERING_ERROR_INVALID_PARAM;
    }

    // Get plant, soil, and irrigation method from database
    const plant_full_data_t *plant = fao56_get_channel_plant(channel, channel_id);
    if (!plant || channel->irrigation_method_index >= IRRIGATION_METHODS_COUNT) {
        return WATERING_ERROR_INVALID_DATA;
    }
    const irrigation_method_data_t *method = &irrigation_methods_database[channel->irrigation_method_index];
    const soil_enhanced_data_t *soil = fao56_get_channel_soil(channel_id, channel);
    if (!soil) {
        return WATERING_ERROR_INVALID_DATA;
    }

    // Compute days after planting and root depth
    uint32_t current_time = timezone_get_unix_utc();
    uint16_t days_after_planting = fao56_get_days_after_planting(channel, current_time);
    channel->days_after_planting = days_after_planting;

    float root_depth_m = fao56_get_root_depth_m(plant, days_after_planting);

    // Update AWC/RAW parameters (needed for clamping and trigger)
    float awc_mm_per_m = soil->awc_mm_per_m;
    balance->rwz_awc_mm = awc_mm_per_m * root_depth_m;

    float wetting_fraction = fao56_get_effective_wetting_fraction(method, soil, plant);
    if (wetting_fraction < FAO56_WF_MIN) wetting_fraction = FAO56_WF_MIN;
    if (wetting_fraction > FAO56_WF_MAX) wetting_fraction = FAO56_WF_MAX;
    wetting_fraction = fao56_apply_wetting_fraction_slew(channel_id, wetting_fraction);
    balance->wetting_fraction = wetting_fraction;
    fao56_rescale_deficit_for_awc_change(balance, balance->rwz_awc_mm * wetting_fraction);

    float depletion_fraction = plant->depletion_fraction_p_x1000 / 1000.0f;
    if (depletion_fraction < 0.1f) depletion_fraction = 0.5f;
    balance->raw_mm = balance->wetting_awc_mm * depletion_fraction;

    // Compute instantaneous (daily) ETc estimate (mm/day)
    environmental_data_t env_data = *env;
    if (!env_data.temp_valid) {
        // Conservative defaults if sensors are unavailable
        env_data.air_temp_mean_c = 25.0f;
        env_data.air_temp_min_c = 18.0f;
        env_data.air_temp_max_c = 32.0f;
        env_data.temp_valid = true;
    }

    uint16_t day_of_year = fao56_get_current_day_of_year();
    float latitude_rad = channel->latitude_deg * (PI / 180.0f);
    float et0_hs = 0.0f;
    float et0_pm = 0.0f;
    float daily_et0 = fao56_calc_et0_ensemble(&env_data, latitude_rad, day_of_year, &et0_hs, &et0_pm);
    (void)et0_hs;
    (void)et0_pm;
    uint32_t now_ms = k_uptime_get_32();
    uint32_t now_s = now_ms / 1000U;
    float max_inc = ET0_SLEW_MAX_INC_MM_DAY;
    float max_dec = ET0_SLEW_MAX_DEC_MM_DAY;
    fao56_get_et0_slew_limits(&env_data, &max_inc, &max_dec);
    daily_et0 = fao56_apply_et0_slew(channel_id, daily_et0, now_s, max_inc, max_dec);

    // Initialize surface evaporation bucket (dual-Kc light)
    float surface_wet_target = fao56_get_surface_wet_target(method, wetting_fraction);
    float surface_wet_fraction = fao56_get_surface_wet_fraction(balance, surface_wet_target, daily_et0);
    fao56_update_surface_bucket(balance, soil, surface_wet_fraction);

    float kc_base = fao56_get_kc_base_for_day(plant, days_after_planting);
    float kc = fao56_apply_canopy_to_kc(plant, kc_base, days_after_planting);
    float ke = fao56_calc_ke(balance, balance->surface_tew_mm, balance->surface_rew_mm,
                             method, plant, days_after_planting);
    float et_root_mm_day = daily_et0 * kc;

    // Apply sun exposure adjustment (consistent with daily update)
    float sun_factor = channel->sun_exposure_pct / 100.0f;
    if (sun_factor < 0.3f) sun_factor = 0.3f;
    if (sun_factor > 1.0f) sun_factor = 1.0f;
    et_root_mm_day *= sun_factor;
    float surface_evap_mm_day = daily_et0 * ke * sun_factor;
    // Accumulate fractional ETc based on elapsed uptime
    if (balance->last_update_time == 0U) {
        balance->last_update_time = now_ms;
        balance->irrigation_needed = (balance->current_deficit_mm >= balance->raw_mm);
        return WATERING_SUCCESS;
    }

    uint32_t delta_ms = now_ms - balance->last_update_time;
    float delta_s = (float)delta_ms / 1000.0f;
    if (delta_s <= 0.0f) {
        balance->last_update_time = now_ms;
        return WATERING_SUCCESS;
    }

    float delta_etc_mm = et_root_mm_day * (delta_s / 86400.0f);
    
    // Issue #20 fix: Apply rainfall in realtime, not just at daily check
    // This prevents watering immediately after a rain event
    float realtime_rain_mm = 0.0f;
    if (env->rain_valid && env->rain_mm_24h > 0.0f) {
        // Check if we've already applied this rain (track per-channel)
        float rain_applied_raw = s_rain_applied_raw_mm[channel_id];
        float new_rain = env->rain_mm_24h - rain_applied_raw;
        if (new_rain > 0.5f) {  // Only apply if significant new rain (>0.5mm)
            // Calculate effective rain using simplified estimation
            float antecedent_moisture_pct = fao56_get_antecedent_moisture_pct(channel_id, balance);
            float effectiveness = 0.7f;  // Conservative 70% effectiveness
            // Adjust effectiveness based on soil and antecedent moisture
            if (antecedent_moisture_pct > 80.0f) {
                effectiveness = 0.5f;  // Wet soil = more runoff
            } else if (antecedent_moisture_pct < 40.0f) {
                effectiveness = 0.85f; // Dry soil = better absorption
            }
            realtime_rain_mm = new_rain * effectiveness;
            s_rain_applied_raw_mm[channel_id] = env->rain_mm_24h;
            LOG_INF("Realtime rain update ch%u: +%.1f mm raw -> %.1f mm effective",
                    channel_id, (double)new_rain, (double)realtime_rain_mm);
            // Mark surface as wet from rain
            if (realtime_rain_mm > 0.0f) {
                fao56_apply_surface_wet_event(balance, FAO56_SURFACE_WET_RAIN_FRACTION);
            }
        }
    }
    
    // Route effective rain through surface and root buckets
    float root_rain_recharge = 0.0f;
    if (realtime_rain_mm > 0.0f) {
        root_rain_recharge = fao56_route_effective_precipitation(balance, realtime_rain_mm);
    }
    
    err = track_deficit_accumulation(balance, delta_etc_mm, root_rain_recharge, 0.0f);
    if (err != WATERING_SUCCESS) {
        return err;
    }

    if (surface_evap_mm_day > 0.0f) {
        float delta_surface_mm = surface_evap_mm_day * (delta_s / 86400.0f);
        balance->surface_deficit_mm += delta_surface_mm;
        if (balance->surface_deficit_mm > balance->surface_tew_mm) {
            balance->surface_deficit_mm = balance->surface_tew_mm;
        }
    }

    balance->last_update_time = now_ms;
    balance->irrigation_needed = (balance->current_deficit_mm >= balance->raw_mm);

    return WATERING_SUCCESS;
}

/**
 * @brief Perform daily deficit update and irrigation decision for AUTO mode
 */
watering_error_t fao56_daily_update_deficit(uint8_t channel_id, 
                                            fao56_auto_decision_t *decision)
{
    if (channel_id >= WATERING_CHANNELS_COUNT || !decision) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    // Initialize decision output
    memset(decision, 0, sizeof(fao56_auto_decision_t));
    decision->should_water = false;
    decision->stress_factor = 1.0f;
    
    // Get channel configuration
    watering_channel_t *channel = NULL;
    watering_error_t err = watering_get_channel(channel_id, &channel);
    if (err != WATERING_SUCCESS || !channel) {
        LOG_ERR("AUTO mode: Failed to get channel %u", channel_id);
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    // Validate AUTO mode prerequisites
    if (!watering_channel_auto_mode_valid(channel)) {
        LOG_WRN("AUTO mode: Channel %u missing required configuration (plant/soil/planting date)", 
                channel_id);
        return WATERING_ERROR_CONFIG;
    }
    
    // Get plant, soil, and irrigation method from database
    const plant_full_data_t *plant = fao56_get_channel_plant(channel, channel_id);
    if (!plant) {
        LOG_ERR("AUTO mode: No valid plant for channel %u (plant_id=%u)", 
                channel_id, channel->plant_id);
        return WATERING_ERROR_INVALID_DATA;
    }
    
    if (channel->irrigation_method_index >= IRRIGATION_METHODS_COUNT) {
        LOG_ERR("AUTO mode: Invalid irrigation_method_index %u for channel %u", 
                channel->irrigation_method_index, channel_id);
        return WATERING_ERROR_INVALID_DATA;
    }
    const irrigation_method_data_t *method = &irrigation_methods_database[channel->irrigation_method_index];
    const soil_enhanced_data_t *soil = fao56_get_channel_soil(channel_id, channel);
    if (!soil) {
        LOG_ERR("AUTO mode: Invalid soil configuration for channel %u", channel_id);
        return WATERING_ERROR_INVALID_DATA;
    }
    
    // Read current environmental data
    environmental_data_t env_data;
    watering_error_t sensor_err = env_sensors_read(&env_data);
    if (sensor_err != WATERING_SUCCESS) {
        LOG_WRN("AUTO mode: Failed to read env sensors, using defaults");
        // Use conservative defaults
        env_data.air_temp_mean_c = 25.0f;
        env_data.air_temp_min_c = 18.0f;
        env_data.air_temp_max_c = 32.0f;
        env_data.temp_valid = true;
        env_data.rel_humidity_pct = 50.0f;
        env_data.humidity_valid = true;
    }
    
    // Get 24h rainfall from rain history
    float rainfall_24h = rain_history_get_last_24h();
    env_data.rain_mm_24h = rainfall_24h;
    env_data.rain_valid = true;
    
    // Calculate days after planting
    uint32_t current_time = timezone_get_unix_utc();
    uint16_t days_after_planting = fao56_get_days_after_planting(channel, current_time);
    channel->days_after_planting = days_after_planting;
    
    // Calculate current root depth
    float root_depth_m = fao56_get_root_depth_m(plant, days_after_planting);
    
    // Ensure water balance structure exists
    water_balance_t *balance = fao56_bind_channel_balance(channel_id, channel);
    if (!balance) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    // Calculate AWC and RAW parameters
    float awc_mm_per_m = soil->awc_mm_per_m;
    balance->rwz_awc_mm = awc_mm_per_m * root_depth_m;
    
    float wetting_fraction = fao56_get_effective_wetting_fraction(method, soil, plant);
    if (wetting_fraction < FAO56_WF_MIN) wetting_fraction = FAO56_WF_MIN;
    if (wetting_fraction > FAO56_WF_MAX) wetting_fraction = FAO56_WF_MAX;
    wetting_fraction = fao56_apply_wetting_fraction_slew(channel_id, wetting_fraction);
    balance->wetting_fraction = wetting_fraction;
    fao56_rescale_deficit_for_awc_change(balance, balance->rwz_awc_mm * wetting_fraction);
    
    float depletion_fraction = plant->depletion_fraction_p_x1000 / 1000.0f;
    if (depletion_fraction < 0.1f) depletion_fraction = 0.5f; // Default to 50%
    balance->raw_mm = balance->wetting_awc_mm * depletion_fraction;

    // Calculate daily ET0 using Hargreaves-Samani (temperature-based method)
    uint16_t day_of_year = fao56_get_current_day_of_year();
    float latitude_rad = channel->latitude_deg * (PI / 180.0f);
    
    float et0_hs = 0.0f;
    float et0_pm = 0.0f;
    float daily_et0 = fao56_calc_et0_ensemble(&env_data, latitude_rad, day_of_year, &et0_hs, &et0_pm);
    (void)et0_hs;
    (void)et0_pm;
    uint32_t now_s = k_uptime_get_32() / 1000U;
    float max_inc = ET0_SLEW_MAX_INC_MM_DAY;
    float max_dec = ET0_SLEW_MAX_DEC_MM_DAY;
    fao56_get_et0_slew_limits(&env_data, &max_inc, &max_dec);
    daily_et0 = fao56_apply_et0_slew(channel_id, daily_et0, now_s, max_inc, max_dec);

    float rain_applied_raw = s_rain_applied_raw_mm[channel_id];
    float rainfall_remainder = rainfall_24h;
    if (rain_applied_raw > 0.0f) {
        if (rain_applied_raw >= rainfall_remainder) {
            rainfall_remainder = 0.0f;
        } else {
            rainfall_remainder -= rain_applied_raw;
        }
    }

    // Calculate effective precipitation (uses global/per-channel antecedent moisture estimate)
    float antecedent_moisture_pct = fao56_get_antecedent_moisture_pct(channel_id, balance);
    float effective_rain = 0.0f;
    float hourly_effective = fao56_calc_effective_rain_hourly(
        current_time, rainfall_remainder, soil, method, antecedent_moisture_pct, env_data.air_temp_mean_c);
    if (hourly_effective >= 0.0f) {
        effective_rain = hourly_effective;
    } else {
        effective_rain = calc_effective_precipitation_with_moisture(
            rainfall_remainder, soil, method, antecedent_moisture_pct, env_data.air_temp_mean_c);
    }

    balance->effective_rain_mm = effective_rain;
    decision->effective_rain_mm = effective_rain;
    if (effective_rain > 0.0f) {
        fao56_apply_surface_wet_event(balance, FAO56_SURFACE_WET_RAIN_FRACTION);
    }
    float surface_wet_target = fao56_get_surface_wet_target(method, wetting_fraction);
    float surface_wet_fraction = fao56_get_surface_wet_fraction(balance, surface_wet_target, daily_et0);
    fao56_update_surface_bucket(balance, soil, surface_wet_fraction);
    float root_recharge = fao56_route_effective_precipitation(balance, effective_rain);
    
    // Calculate crop coefficient (Kc) based on growth stage
    float kc_base = fao56_get_kc_base_for_day(plant, days_after_planting);
    float kc = fao56_apply_canopy_to_kc(plant, kc_base, days_after_planting);
    float ke = fao56_calc_ke(balance, balance->surface_tew_mm, balance->surface_rew_mm,
                             method, plant, days_after_planting);
    float kc_total = kc + ke;
    if (kc_total > 2.0f) kc_total = 2.0f;
    if (kc_total < 0.1f) kc_total = 0.1f;
    
    // Calculate daily ETc (crop evapotranspiration)
    float daily_etc = daily_et0 * kc_total;
    
    // B0 #4: Apply sun exposure adjustment (shaded areas have lower evapotranspiration)
    // sun_exposure_pct: 100% = full sun, 0% = full shade
    float sun_factor = channel->sun_exposure_pct / 100.0f;
    if (sun_factor < 0.3f) sun_factor = 0.3f;  // Minimum 30% to avoid underestimation in deep shade
    if (sun_factor > 1.0f) sun_factor = 1.0f;
    daily_etc *= sun_factor;
    
    bool eco_mode = (channel->auto_mode == WATERING_AUTOMATIC_ECO ||
                     channel->watering_event.watering_mode == WATERING_AUTOMATIC_ECO);
    
    decision->daily_etc_mm = daily_etc;
    
    LOG_DBG("AUTO mode: ET0=%.2f, Kc=%.2f, sun=%.0f%% -> ETc=%.2f mm (eco_mad=%d) ch%u",
            (double)daily_et0, (double)kc, (double)(sun_factor * 100.0f),
            (double)daily_etc, eco_mode ? 1 : 0, channel_id);
    
    // Apply ETc-based MAD adjustment + environmental stress adjustment (use Kc only, no Ke)
    float base_mad = plant->depletion_fraction_p_x1000 / 1000.0f;
    float etc_for_mad = daily_et0 * kc * sun_factor;
    float etc_adjusted_mad = base_mad + FAO56_MAD_ETC_ADJ_COEFF * (FAO56_MAD_ETC_REF_MM_DAY - etc_for_mad);
    if (etc_adjusted_mad < FAO56_MAD_MIN_FRACTION) etc_adjusted_mad = FAO56_MAD_MIN_FRACTION;
    if (etc_adjusted_mad > FAO56_MAD_MAX_FRACTION) etc_adjusted_mad = FAO56_MAD_MAX_FRACTION;
    float adjusted_mad = apply_environmental_stress_adjustment(etc_adjusted_mad, &env_data, plant);
    if (eco_mode) {
        float eco_boost = 1.0f - ECO_ETC_FACTOR;
        adjusted_mad = adjusted_mad + (1.0f - adjusted_mad) * eco_boost;
        if (adjusted_mad > 1.0f) adjusted_mad = 1.0f;
    }
    decision->stress_factor = (base_mad > 0.0f) ? (adjusted_mad / base_mad) : 1.0f;
    
    // Daily check: subtract effective rain routed to root (ETc is accumulated continuously)
    err = track_deficit_accumulation(balance, 0.0f, root_recharge, 0.0f);
    if (err != WATERING_SUCCESS) {
        LOG_ERR("AUTO mode: Deficit tracking failed for channel %u", channel_id);
        return err;
    }

    s_rain_applied_surface_mm[channel_id] = 0.0f;
    s_rain_applied_root_mm[channel_id] = 0.0f;
    s_rain_applied_raw_mm[channel_id] = 0.0f;
    
    // Update decision output values
    decision->current_deficit_mm = balance->current_deficit_mm;
    decision->raw_threshold_mm = balance->wetting_awc_mm * adjusted_mad;
    
    // Check if irrigation is needed using adjusted MAD threshold
    bool irrigation_needed = check_irrigation_trigger_mad(balance, plant, soil, decision->stress_factor);
    decision->should_water = irrigation_needed;
    
    if (irrigation_needed) {
        // Calculate volume to apply: refill entire deficit
        float net_irrigation_mm = balance->current_deficit_mm;
        
        // Apply irrigation efficiency (efficiency_pct is 0-100)
        float efficiency = method->efficiency_pct / 100.0f;
        if (efficiency < 0.5f) efficiency = 0.8f; // Default 80% if invalid
        float distribution_uniformity = method->distribution_uniformity_pct / 100.0f;
        if (distribution_uniformity <= 0.0f || distribution_uniformity > 1.0f) {
            distribution_uniformity = 1.0f;
        }
        float gross_irrigation_mm = net_irrigation_mm / efficiency;
        if (distribution_uniformity < 1.0f) {
            gross_irrigation_mm /= distribution_uniformity;
        }
        
        // Convert mm to liters based on coverage
        float area_m2 = channel->coverage.area_m2;
        if (!channel->use_area_based) {
            float area_per_plant_m2 = 0.0f;
            float canopy_factor = 1.0f;
            area_m2 = fao56_calc_plant_irrigated_area_m2(
                plant, channel->coverage.plant_count, &area_per_plant_m2, &canopy_factor, false);
            if (area_m2 <= 0.0f) {
                area_m2 = channel->coverage.plant_count * 0.5f;
            }
        }
        decision->volume_liters = gross_irrigation_mm * area_m2;
        
        // Apply max volume limit if configured
        if (channel->max_volume_limit_l > 0 && decision->volume_liters > channel->max_volume_limit_l) {
            decision->volume_liters = channel->max_volume_limit_l;
            LOG_INF("AUTO mode: Volume capped to %.1f L limit for channel %u", 
                    (double)channel->max_volume_limit_l, channel_id);
        }
        
        LOG_INF("AUTO mode: Channel %u NEEDS WATER - deficit=%.1f mm >= threshold=%.1f mm, volume=%.1f L",
                channel_id, (double)balance->current_deficit_mm, 
                (double)decision->raw_threshold_mm, (double)decision->volume_liters);
    } else {
        decision->volume_liters = 0.0f;
        LOG_INF("AUTO mode: Channel %u SKIP - deficit=%.1f mm < threshold=%.1f mm",
                channel_id, (double)balance->current_deficit_mm, (double)decision->raw_threshold_mm);
    }
    
    // Update balance timestamp
    balance->last_update_time = k_uptime_get_32();
    balance->irrigation_needed = irrigation_needed;
    
    // Persist updated water balance to NVS
    int nvs_ret = nvs_save_complete_channel_config(channel_id, channel);
    if (nvs_ret < 0) {
        LOG_WRN("AUTO mode: Failed to persist water balance for channel %u: %d", channel_id, nvs_ret);
    }
    
    return WATERING_SUCCESS;
}

/**
 * @brief Apply incremental rainfall to AUTO water balance (best-effort)
 *
 * Uses the provided duration to estimate rainfall intensity for runoff/evap losses.
 */
watering_error_t fao56_apply_rainfall_increment(float rainfall_mm, float air_temp_c, uint32_t duration_s)
{
    if (rainfall_mm <= 0.0f) {
        return WATERING_SUCCESS;
    }

    if (air_temp_c < -20.0f || air_temp_c > 50.0f) {
        air_temp_c = 20.0f;
    }

    for (uint8_t channel_id = 0; channel_id < WATERING_CHANNELS_COUNT; channel_id++) {
        watering_channel_t *channel = &watering_channels[channel_id];
        if (!watering_channel_auto_mode_valid(channel)) {
            continue;
        }

        bool uses_auto_balance =
            channel->watering_event.schedule_type == SCHEDULE_AUTO ||
            channel->auto_mode == WATERING_AUTOMATIC_QUALITY ||
            channel->auto_mode == WATERING_AUTOMATIC_ECO ||
            channel->watering_event.watering_mode == WATERING_AUTOMATIC_QUALITY ||
            channel->watering_event.watering_mode == WATERING_AUTOMATIC_ECO;
        if (!uses_auto_balance) {
            continue;
        }

        if (channel->irrigation_method_index >= IRRIGATION_METHODS_COUNT) {
            continue;
        }

        water_balance_t *balance = fao56_bind_channel_balance(channel_id, channel);
        if (!balance) {
            continue;
        }

        const plant_full_data_t *plant = fao56_get_channel_plant(channel, channel_id);
        if (!plant) {
            continue;
        }
        const irrigation_method_data_t *method = &irrigation_methods_database[channel->irrigation_method_index];
        const soil_enhanced_data_t *soil = fao56_get_channel_soil(channel_id, channel);
        if (!soil) {
            continue;
        }

        s_rain_applied_raw_mm[channel_id] += rainfall_mm;

        float wetting_fraction = fao56_get_effective_wetting_fraction(method, soil, plant);
        if (wetting_fraction < FAO56_WF_MIN) wetting_fraction = FAO56_WF_MIN;
        if (wetting_fraction > FAO56_WF_MAX) wetting_fraction = FAO56_WF_MAX;
        wetting_fraction = fao56_apply_wetting_fraction_slew(channel_id, wetting_fraction);
        balance->wetting_fraction = wetting_fraction;

        if (balance->wetting_awc_mm <= 0.0f || balance->raw_mm <= 0.0f) {
            uint32_t current_time = timezone_get_unix_utc();
            uint16_t days_after_planting = fao56_get_days_after_planting(channel, current_time);
  
            float root_depth_m = fao56_get_root_depth_m(plant, days_after_planting);
            balance->rwz_awc_mm = soil->awc_mm_per_m * root_depth_m;

            fao56_rescale_deficit_for_awc_change(balance, balance->rwz_awc_mm * wetting_fraction);

            float depletion_fraction = plant->depletion_fraction_p_x1000 / 1000.0f;
            if (depletion_fraction < 0.1f) depletion_fraction = 0.5f;
            balance->raw_mm = balance->wetting_awc_mm * depletion_fraction;
        }

        float antecedent_moisture_pct = fao56_get_antecedent_moisture_pct(channel_id, balance);
        float duration_h = 0.0f;
        float intensity_mm_h = 0.0f;
        if (duration_s > 0U) {
            duration_h = (float)duration_s / 3600.0f;
            if (duration_h > 0.0f) {
                if (duration_h < (1.0f / 60.0f)) {
                    duration_h = 1.0f / 60.0f;
                }
                if (duration_h > 1.0f) {
                    duration_h = 1.0f;
                }
                intensity_mm_h = rainfall_mm / duration_h;
            }
        }
        float effective_rain = 0.0f;
        if (duration_h > 0.0f && intensity_mm_h > 0.0f) {
            effective_rain = calc_effective_precipitation_with_moisture_timing(
                rainfall_mm, soil, method, antecedent_moisture_pct, air_temp_c,
                duration_h, intensity_mm_h);
        } else {
            effective_rain = calc_effective_precipitation_with_moisture(
                rainfall_mm, soil, method, antecedent_moisture_pct, air_temp_c);
        }
        if (effective_rain <= 0.0f) {
            continue;
        }

        fao56_apply_surface_wet_event(balance, FAO56_SURFACE_WET_RAIN_FRACTION);
        float surface_wet_target = fao56_get_surface_wet_target(method, wetting_fraction);
        float surface_wet_fraction = fao56_get_surface_wet_fraction(balance, surface_wet_target, 0.0f);
        fao56_update_surface_bucket(balance, soil, surface_wet_fraction);

        float root_recharge = fao56_route_effective_precipitation(balance, effective_rain);
        float surface_recharge = effective_rain - root_recharge;
        if (surface_recharge < 0.0f) {
            surface_recharge = 0.0f;
        }
        watering_error_t err = track_deficit_accumulation(balance, 0.0f, root_recharge, 0.0f);
        if (err != WATERING_SUCCESS) {
            continue;
        }

        s_rain_applied_surface_mm[channel_id] += surface_recharge;
        s_rain_applied_root_mm[channel_id] += root_recharge;
        balance->irrigation_needed = (balance->current_deficit_mm >= balance->raw_mm);
    }

    return WATERING_SUCCESS;
}

/**
 * @brief Handle multi-day offline gap by estimating missed deficit accumulation
 */
watering_error_t fao56_apply_missed_days_deficit(uint8_t channel_id, 
                                                  uint16_t days_missed)
{
    if (channel_id >= WATERING_CHANNELS_COUNT || days_missed == 0) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    if (days_missed > 30) {
        // Cap at 30 days to prevent unreasonable deficit accumulation
        days_missed = 30;
        LOG_WRN("AUTO mode: Capping missed days to 30 for channel %u", channel_id);
    }
    
    watering_channel_t *channel = NULL;
    watering_error_t err = watering_get_channel(channel_id, &channel);
    if (err != WATERING_SUCCESS || !channel) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    if (!watering_channel_auto_mode_valid(channel)) {
        return WATERING_ERROR_CONFIG;
    }
    
    if (!channel->water_balance) {
        LOG_WRN("AUTO mode: No water balance for channel %u, skipping missed days", channel_id);
        return WATERING_SUCCESS;
    }

    water_balance_t *balance = channel->water_balance;
    
    // Get plant data for Kc
    const plant_full_data_t *plant = fao56_get_channel_plant(channel, channel_id);
    if (!plant) {
        return WATERING_ERROR_INVALID_DATA;
    }
    if (channel->irrigation_method_index >= IRRIGATION_METHODS_COUNT) {
        return WATERING_ERROR_INVALID_DATA;
    }
    const irrigation_method_data_t *method = &irrigation_methods_database[channel->irrigation_method_index];
    const soil_enhanced_data_t *soil = fao56_get_channel_soil(channel_id, channel);
    if (!soil) {
        return WATERING_ERROR_INVALID_DATA;
    }
    
    uint32_t current_time = timezone_get_unix_utc();
    uint16_t days_after_planting = fao56_get_days_after_planting(channel, current_time);
    float root_depth_m = fao56_get_root_depth_m(plant, days_after_planting);

    float wetting_fraction = fao56_get_effective_wetting_fraction(method, soil, plant);
    if (wetting_fraction < FAO56_WF_MIN) wetting_fraction = FAO56_WF_MIN;
    if (wetting_fraction > FAO56_WF_MAX) wetting_fraction = FAO56_WF_MAX;
    wetting_fraction = fao56_apply_wetting_fraction_slew(channel_id, wetting_fraction);
    balance->wetting_fraction = wetting_fraction;
    balance->rwz_awc_mm = soil->awc_mm_per_m * root_depth_m;
    fao56_rescale_deficit_for_awc_change(balance, balance->rwz_awc_mm * wetting_fraction);

    float depletion_fraction = plant->depletion_fraction_p_x1000 / 1000.0f;
    if (depletion_fraction < 0.1f) depletion_fraction = 0.5f;
    balance->raw_mm = balance->wetting_awc_mm * depletion_fraction;

    float surface_wet_target = fao56_get_surface_wet_target(method, wetting_fraction);
    float surface_wet_fraction = fao56_get_surface_wet_fraction(balance, surface_wet_target, 0.0f);
    fao56_update_surface_bucket(balance, soil, surface_wet_fraction);

    float latitude_rad = channel->latitude_deg * (PI / 180.0f);
    float week_et0_avg[FAO56_CLIMATOLOGY_WEEKS] = {0};
    float climatology_avg = FAO56_DEFAULT_ET0_MM_DAY;
    bool have_climatology = fao56_build_weekly_et0_climatology(latitude_rad,
                                                               week_et0_avg,
                                                               &climatology_avg);

    uint32_t current_day_index = current_time / ENV_HISTORY_DAILY_INTERVAL_SEC;
    float sun_factor = channel->sun_exposure_pct / 100.0f;
    if (sun_factor < 0.3f) sun_factor = 0.3f;
    if (sun_factor > 1.0f) sun_factor = 1.0f;

    float total_missed_deficit = 0.0f;

    for (uint16_t offset = 1; offset <= days_missed; offset++) {
        uint32_t day_index = (current_day_index >= offset) ? (current_day_index - offset) : 0U;
        uint32_t day_ts = day_index * ENV_HISTORY_DAILY_INTERVAL_SEC;
        uint16_t day_of_year = 0;
        float et0_day = FAO56_DEFAULT_ET0_MM_DAY;

        if (have_climatology &&
            fao56_get_day_of_year_from_timestamp(day_ts, &day_of_year) &&
            day_of_year > 0) {
            uint8_t week_index = (uint8_t)((day_of_year - 1U) / 7U);
            if (week_index >= FAO56_CLIMATOLOGY_WEEKS) {
                week_index = FAO56_CLIMATOLOGY_WEEKS - 1U;
            }
            et0_day = week_et0_avg[week_index];
        } else {
            uint8_t month = fao56_get_month_from_timestamp(day_ts);
            et0_day = fao56_get_default_et0_for_month(month);
        }

        if (et0_day < HEURISTIC_ET0_MIN) et0_day = HEURISTIC_ET0_MIN;
        if (et0_day > HEURISTIC_ET0_MAX) et0_day = HEURISTIC_ET0_MAX;
        if (et0_day <= 0.0f) {
            et0_day = (climatology_avg > 0.0f) ? climatology_avg : FAO56_DEFAULT_ET0_MM_DAY;
        }

        uint16_t dap_day = (days_after_planting >= offset) ? (days_after_planting - offset) : 0U;
        float kc = fao56_get_kc_for_day(plant, dap_day);
        if (kc > 2.0f) kc = 2.0f;
        if (kc < 0.1f) kc = 0.1f;
        total_missed_deficit += et0_day * kc * sun_factor;
    }

    balance->current_deficit_mm += total_missed_deficit;
    if (balance->surface_tew_mm > 0.0f) {
        balance->surface_deficit_mm = balance->surface_tew_mm;
    }
    
    // Clamp to AWC maximum
    if (balance->current_deficit_mm > balance->wetting_awc_mm && balance->wetting_awc_mm > 0) {
        balance->current_deficit_mm = balance->wetting_awc_mm;
    }
    
    LOG_INF("AUTO mode: Applied %u missed days deficit to channel %u: +%.1f mm (new total: %.1f mm)",
            days_missed, channel_id, (double)total_missed_deficit, 
            (double)balance->current_deficit_mm);

    // Start realtime accumulation from now
    balance->last_update_time = k_uptime_get_32();
    
    return WATERING_SUCCESS;
}

/**
 * @brief Reduce channel deficit after successful irrigation
 */
watering_error_t fao56_reduce_deficit_after_irrigation(uint8_t channel_id,
                                                        float volume_applied_liters)
{
    if (channel_id >= WATERING_CHANNELS_COUNT || volume_applied_liters <= 0) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    watering_channel_t *channel = NULL;
    watering_error_t err = watering_get_channel(channel_id, &channel);
    if (err != WATERING_SUCCESS || !channel) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    if (!channel->water_balance) {
        LOG_WRN("AUTO mode: No water balance for channel %u, cannot reduce deficit", channel_id);
        return WATERING_SUCCESS;
    }
    
    water_balance_t *balance = channel->water_balance;

    float efficiency = 0.8f;
    float distribution_uniformity = 1.0f;
    float efficiency_surface = 1.0f;
    float efficiency_root = 0.8f;

    const irrigation_method_data_t *method = NULL;
    if (channel->irrigation_method_index < IRRIGATION_METHODS_COUNT) {
        method = &irrigation_methods_database[channel->irrigation_method_index];
    }

    const plant_full_data_t *plant = NULL;
    if (!channel->use_area_based) {
        plant = fao56_get_channel_plant(channel, channel_id);
    }
    const soil_enhanced_data_t *soil = fao56_get_channel_soil(channel_id, channel);

    float wetting_fraction = balance->wetting_fraction;
    if (wetting_fraction <= 0.0f || wetting_fraction > 1.0f) {
        if (soil && method) {
            wetting_fraction = fao56_get_effective_wetting_fraction(method, soil, plant);
        } else {
            wetting_fraction = 1.0f;
        }
    }
    wetting_fraction = fao56_apply_wetting_fraction_slew(channel_id, wetting_fraction);
    if (wetting_fraction < FAO56_WF_MIN) wetting_fraction = FAO56_WF_MIN;
    if (wetting_fraction > 1.0f) wetting_fraction = 1.0f;
    balance->wetting_fraction = wetting_fraction;

    if (method) {
        efficiency = method->efficiency_pct / 100.0f;
        if (efficiency < 0.5f) {
            efficiency = 0.8f;
        }
        distribution_uniformity = method->distribution_uniformity_pct / 100.0f;
        if (distribution_uniformity <= 0.0f || distribution_uniformity > 1.0f) {
            distribution_uniformity = 1.0f;
        }
        fao56_get_efficiency_split(method, wetting_fraction, &efficiency_surface, &efficiency_root);
    } else {
        LOG_WRN("AUTO mode: Invalid irrigation_method_index %u for channel %u, using 80%%",
                channel->irrigation_method_index, channel_id);
        efficiency_root = efficiency * distribution_uniformity;
        efficiency_surface = 1.0f;
    }

    // Convert liters to mm based on coverage area
    float area_m2 = channel->coverage.area_m2;
    if (!channel->use_area_based) {
        float area_per_plant_m2 = 0.0f;
        float canopy_factor = 1.0f;
        if (plant) {
            area_m2 = fao56_calc_plant_irrigated_area_m2(
                plant, channel->coverage.plant_count, &area_per_plant_m2, &canopy_factor, false);
        } else {
            area_m2 = 0.0f;
        }
        if (area_m2 <= 0.0f) {
            area_m2 = channel->coverage.plant_count * 0.5f;
        }
    }

    if (area_m2 <= 0) {
        LOG_WRN("AUTO mode: Invalid coverage area for channel %u", channel_id);
        return WATERING_SUCCESS;
    }
    
    float irrigation_mm = volume_applied_liters / area_m2;
    
    // Apply irrigation efficiency (match gross->net in AUTO schedule)
    float effective_irrigation_mm = irrigation_mm * efficiency_root;

    // Reduce deficit
    float old_deficit = balance->current_deficit_mm;
    balance->current_deficit_mm -= effective_irrigation_mm;
    
    // Clamp to zero (cannot have negative deficit)
    if (balance->current_deficit_mm < 0) {
        balance->current_deficit_mm = 0;
    }

    if (soil) {
        float surface_event_fraction = wetting_fraction;
        if (method) {
            float du = method->distribution_uniformity_pct / 100.0f;
            if (du > 0.0f && du < 1.0f) {
                surface_event_fraction *= du;
            }
        }
        if (surface_event_fraction < FAO56_WF_MIN) surface_event_fraction = FAO56_WF_MIN;
        if (surface_event_fraction > 1.0f) surface_event_fraction = 1.0f;

        float surface_wet_target = fao56_get_surface_wet_target(method, wetting_fraction);
        fao56_apply_surface_wet_event(balance, surface_event_fraction);
        float surface_state = fao56_get_surface_wet_fraction(balance, surface_wet_target, 0.0f);
        fao56_update_surface_bucket(balance, soil, surface_state);

        float surface_recharge = irrigation_mm * efficiency_surface;
        if (surface_recharge > 0.0f) {
            balance->surface_deficit_mm -= surface_recharge;
            if (balance->surface_deficit_mm < 0.0f) {
                balance->surface_deficit_mm = 0.0f;
            }
        }
    }
    
    balance->irrigation_needed = false;
    balance->last_update_time = k_uptime_get_32();
    
    LOG_INF("AUTO mode: Channel %u deficit reduced %.1f -> %.1f mm (applied %.1f L = %.1f mm effective)",
            channel_id, (double)old_deficit, (double)balance->current_deficit_mm,
            (double)volume_applied_liters, (double)effective_irrigation_mm);
    
    // Persist updated balance to NVS
    int nvs_ret = nvs_save_complete_channel_config(channel_id, channel);
    if (nvs_ret < 0) {
        LOG_WRN("AUTO mode: Failed to persist reduced deficit for channel %u: %d", channel_id, nvs_ret);
    }
    
    return WATERING_SUCCESS;
}

/* ============================================================================
 * SOLAR TIMING CALCULATIONS (NOAA Algorithm)
 * ============================================================================
 * 
 * Implementation of the NOAA Solar Calculator algorithm for computing
 * sunrise and sunset times with approximately 1 minute precision.
 * 
 * Reference: https://gml.noaa.gov/grad/solcalc/solareqns.PDF
 * ============================================================================ */

/**
 * @brief Calculate the fractional year (gamma) in radians
 * 
 * @param day_of_year Day of year (1-365/366)
 * @param is_leap_year True if leap year
 * @return Fractional year in radians
 */
static float calc_fractional_year(uint16_t day_of_year, bool is_leap_year)
{
    float days_in_year = is_leap_year ? 366.0f : 365.0f;
    return (2.0f * PI / days_in_year) * ((float)day_of_year - 1.0f);
}

/**
 * @brief Calculate the equation of time in minutes
 * 
 * The equation of time accounts for the eccentricity of Earth's orbit
 * and the axial tilt of the Earth.
 * 
 * @param gamma Fractional year in radians
 * @return Equation of time in minutes
 */
static float calc_equation_of_time(float gamma)
{
    float eqtime = 229.18f * (0.000075f +
                   0.001868f * cosf(gamma) -
                   0.032077f * sinf(gamma) -
                   0.014615f * cosf(2.0f * gamma) -
                   0.040849f * sinf(2.0f * gamma));
    return eqtime;
}

/**
 * @brief Calculate the solar declination angle in radians
 * 
 * @param gamma Fractional year in radians
 * @return Solar declination in radians
 */
static float calc_solar_declination(float gamma)
{
    float decl = 0.006918f -
                 0.399912f * cosf(gamma) +
                 0.070257f * sinf(gamma) -
                 0.006758f * cosf(2.0f * gamma) +
                 0.000907f * sinf(2.0f * gamma) -
                 0.002697f * cosf(3.0f * gamma) +
                 0.00148f * sinf(3.0f * gamma);
    return decl;
}

/**
 * @brief Calculate the hour angle at sunrise/sunset
 * 
 * For sunrise/sunset, the zenith angle is 90.833 degrees (90° + 50 arcminutes)
 * to account for atmospheric refraction and the sun's apparent radius.
 * 
 * @param latitude_rad Latitude in radians
 * @param declination Solar declination in radians
 * @param hour_angle_out Output: hour angle in radians (positive = before noon)
 * @return 0 if normal day/night, 1 if polar day, -1 if polar night
 */
static int calc_sunrise_hour_angle(float latitude_rad, float declination, float *hour_angle_out)
{
    // Zenith angle for sunrise/sunset: 90.833 degrees
    float zenith = 90.833f * PI / 180.0f;
    
    float cos_ha = (cosf(zenith) / (cosf(latitude_rad) * cosf(declination))) -
                   (tanf(latitude_rad) * tanf(declination));
    
    // Check for polar conditions
    if (cos_ha > 1.0f) {
        // Polar night - sun never rises
        *hour_angle_out = 0;
        return -1;
    } else if (cos_ha < -1.0f) {
        // Polar day - sun never sets
        *hour_angle_out = PI;
        return 1;
    }
    
    *hour_angle_out = acosf(cos_ha);
    return 0;
}

watering_error_t fao56_calc_solar_times(float latitude_deg, 
                                        float longitude_deg,
                                        uint16_t day_of_year,
                                        int8_t timezone_offset_hours,
                                        solar_times_t *result)
{
    if (result == NULL) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    // Initialize result
    memset(result, 0, sizeof(solar_times_t));
    
    // Validate latitude
    if (latitude_deg < -90.0f || latitude_deg > 90.0f) {
        LOG_WRN("Solar calc: Invalid latitude %.2f", (double)latitude_deg);
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    // Validate longitude
    if (longitude_deg < -180.0f || longitude_deg > 180.0f) {
        LOG_WRN("Solar calc: Invalid longitude %.2f", (double)longitude_deg);
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    // Validate day of year
    if (day_of_year < 1 || day_of_year > 366) {
        LOG_WRN("Solar calc: Invalid day of year %u", day_of_year);
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    // Convert latitude to radians
    float latitude_rad = latitude_deg * PI / 180.0f;
    
    // Assume non-leap year for simplicity (error is minimal)
    bool is_leap_year = false;
    
    // Calculate fractional year
    float gamma = calc_fractional_year(day_of_year, is_leap_year);
    
    // Calculate equation of time
    float eqtime = calc_equation_of_time(gamma);
    
    // Calculate solar declination
    float declination = calc_solar_declination(gamma);
    
    // Calculate hour angle at sunrise
    float hour_angle;
    int polar_status = calc_sunrise_hour_angle(latitude_rad, declination, &hour_angle);
    
    if (polar_status != 0) {
        // Polar conditions - use fallback times
        if (polar_status > 0) {
            // Polar day (midnight sun)
            result->is_polar_day = true;
            result->day_length_minutes = 24 * 60;
            LOG_INF("Solar calc: Polar day detected at lat %.2f", (double)latitude_deg);
        } else {
            // Polar night
            result->is_polar_night = true;
            result->day_length_minutes = 0;
            LOG_INF("Solar calc: Polar night detected at lat %.2f", (double)latitude_deg);
        }
        
        // Use fallback times
        result->sunrise_hour = SOLAR_FALLBACK_SUNRISE_HOUR;
        result->sunrise_minute = 0;
        result->sunset_hour = SOLAR_FALLBACK_SUNSET_HOUR;
        result->sunset_minute = 0;
        result->calculation_valid = false;
        
        return WATERING_SUCCESS;
    }
    
    // Convert hour angle to minutes from solar noon
    float ha_minutes = hour_angle * 180.0f / PI * 4.0f; // 1 degree = 4 minutes
    
    // Calculate solar noon in local standard time (minutes from midnight)
    // Solar noon = 720 - 4*longitude - eqtime + 60*timezone
    float solar_noon = 720.0f - 4.0f * longitude_deg - eqtime + 60.0f * timezone_offset_hours;
    
    // Calculate sunrise and sunset in minutes from midnight
    float sunrise_minutes = solar_noon - ha_minutes;
    float sunset_minutes = solar_noon + ha_minutes;
    
    // Normalize to 0-1440 range
    while (sunrise_minutes < 0) sunrise_minutes += 1440.0f;
    while (sunrise_minutes >= 1440.0f) sunrise_minutes -= 1440.0f;
    while (sunset_minutes < 0) sunset_minutes += 1440.0f;
    while (sunset_minutes >= 1440.0f) sunset_minutes -= 1440.0f;
    
    // Convert to hours and minutes
    result->sunrise_hour = (uint8_t)((int)sunrise_minutes / 60);
    result->sunrise_minute = (uint8_t)((int)sunrise_minutes % 60);
    result->sunset_hour = (uint8_t)((int)sunset_minutes / 60);
    result->sunset_minute = (uint8_t)((int)sunset_minutes % 60);
    
    // Calculate day length
    result->day_length_minutes = (uint16_t)(2.0f * ha_minutes);
    
    result->is_polar_day = false;
    result->is_polar_night = false;
    result->calculation_valid = true;
    
    LOG_DBG("Solar calc: lat=%.2f, lon=%.2f, DOY=%u, TZ=%+d => sunrise=%02u:%02u, sunset=%02u:%02u",
            (double)latitude_deg, (double)longitude_deg, day_of_year, timezone_offset_hours,
            result->sunrise_hour, result->sunrise_minute,
            result->sunset_hour, result->sunset_minute);
    
    return WATERING_SUCCESS;
}

watering_error_t fao56_get_effective_start_time(const watering_event_t *event,
                                                 float latitude_deg,
                                                 float longitude_deg,
                                                 uint16_t day_of_year,
                                                 int8_t timezone_offset_hours,
                                                 uint8_t *effective_hour,
                                                 uint8_t *effective_minute)
{
    if (event == NULL || effective_hour == NULL || effective_minute == NULL) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    // If solar timing not enabled, use configured start time directly
    if (!event->use_solar_timing) {
        *effective_hour = event->start_time.hour;
        *effective_minute = event->start_time.minute;
        return WATERING_SUCCESS;
    }
    
    // Calculate solar times
    solar_times_t solar;
    watering_error_t ret = fao56_calc_solar_times(latitude_deg, longitude_deg,
                                                   day_of_year, timezone_offset_hours,
                                                   &solar);
    
    if (ret != WATERING_SUCCESS) {
        // Use fallback time
        *effective_hour = event->start_time.hour;
        *effective_minute = event->start_time.minute;
        return WATERING_ERROR_SOLAR_FALLBACK;
    }
    
    // Get base time from selected solar event
    int base_minutes;
    if (event->solar_event == SOLAR_EVENT_SUNRISE) {
        base_minutes = solar.sunrise_hour * 60 + solar.sunrise_minute;
    } else {
        // Default to sunset
        base_minutes = solar.sunset_hour * 60 + solar.sunset_minute;
    }
    
    // Apply offset
    int8_t offset = event->solar_offset_minutes;
    
    // Clamp offset to valid range
    if (offset < SOLAR_OFFSET_MIN) offset = SOLAR_OFFSET_MIN;
    if (offset > SOLAR_OFFSET_MAX) offset = SOLAR_OFFSET_MAX;
    
    int effective_minutes = base_minutes + offset;
    
    // Normalize to 0-1440 range
    while (effective_minutes < 0) effective_minutes += 1440;
    while (effective_minutes >= 1440) effective_minutes -= 1440;
    
    *effective_hour = (uint8_t)(effective_minutes / 60);
    *effective_minute = (uint8_t)(effective_minutes % 60);
    
    // Check if we're using fallback times (polar conditions)
    if (solar.is_polar_day || solar.is_polar_night || !solar.calculation_valid) {
        LOG_INF("Solar timing: Using fallback for polar conditions, effective=%02u:%02u",
                *effective_hour, *effective_minute);
        return WATERING_ERROR_SOLAR_FALLBACK;
    }
    
    LOG_DBG("Solar timing: event=%s, offset=%+d min => effective=%02u:%02u",
            event->solar_event == SOLAR_EVENT_SUNRISE ? "sunrise" : "sunset",
            offset, *effective_hour, *effective_minute);
    
    return WATERING_SUCCESS;
}

