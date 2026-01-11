/**
 * @file fao56_calc.h
 * @brief FAO-56 based irrigation calculation engine
 * 
 * This module implements scientific irrigation calculations based on the
 * FAO-56 methodology for evapotranspiration and water balance calculations.
 */

#ifndef FAO56_CALC_H
#define FAO56_CALC_H

#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include "plant_full_db.inc"
#include "soil_enhanced_db.inc"
#include "irrigation_methods_db.inc"
#include "env_sensors.h"
#include "water_balance_types.h"
#include "watering.h"

/**
 * @brief Phenological growth stages for crop coefficient calculation
 */
typedef enum {
    PHENO_STAGE_INITIAL,      /**< Initial stage - crop establishment */
    PHENO_STAGE_DEVELOPMENT,  /**< Development stage - vegetative growth */
    PHENO_STAGE_MID_SEASON,   /**< Mid-season stage - full canopy */
    PHENO_STAGE_END_SEASON    /**< End season stage - maturation/senescence */
} phenological_stage_t;

/**
 * @brief Irrigation calculation results
 */
typedef struct {
    float net_irrigation_mm;           /**< Net water requirement (mm) */
    float gross_irrigation_mm;         /**< Gross application with losses (mm) */
    float volume_liters;               /**< Total volume needed (L) */
    float volume_per_plant_liters;     /**< Volume per plant if applicable (L) */
    uint8_t cycle_count;               /**< Number of irrigation cycles */
    uint16_t cycle_duration_min;       /**< Duration per cycle (minutes) */
    uint16_t soak_interval_min;        /**< Rest between cycles (minutes) */
    bool volume_limited;               /**< True if limited by max volume constraint */
} irrigation_calculation_t;

/**
 * @brief ET0 calculation cache entry
 */
typedef struct {
    float temperature_min_c;           /**< Minimum temperature used in calculation */
    float temperature_max_c;           /**< Maximum temperature used in calculation */
    float humidity_pct;                /**< Humidity used in calculation */
    float pressure_hpa;                /**< Atmospheric pressure used in calculation */
    float latitude_rad;                /**< Latitude used in calculation */
    uint16_t day_of_year;              /**< Day of year used in calculation */
    float et0_result;                  /**< Cached ET0 result (mm/day) */
    uint32_t calculation_time;         /**< When this was calculated (timestamp) */
    bool valid;                        /**< Whether this cache entry is valid */
} et0_cache_entry_t;

/**
 * @brief Crop coefficient calculation cache entry
 */
typedef struct {
    uint16_t plant_db_index;           /**< Plant database index used */
    uint16_t days_after_planting;      /**< Days after planting used */
    phenological_stage_t stage;        /**< Calculated phenological stage */
    float crop_coefficient;            /**< Cached crop coefficient result */
    uint32_t calculation_time;         /**< When this was calculated (timestamp) */
    bool valid;                        /**< Whether this cache entry is valid */
} crop_coeff_cache_entry_t;

/**
 * @brief Water balance calculation cache entry
 */
typedef struct {
    uint8_t channel_id;                /**< Channel this cache is for */
    uint16_t plant_db_index;           /**< Plant database index */
    uint8_t soil_db_index;             /**< Soil database index */
    uint8_t irrigation_method_index;   /**< Irrigation method index */
    float root_depth_m;                /**< Root depth used */
    water_balance_t balance_result;    /**< Cached water balance result */
    uint32_t calculation_time;         /**< When this was calculated (timestamp) */
    bool valid;                        /**< Whether this cache entry is valid */
} water_balance_cache_entry_t;

/**
 * @brief Performance optimization cache structure
 */
typedef struct {
    et0_cache_entry_t et0_cache[WATERING_CHANNELS_COUNT];           /**< ET0 cache per channel */
    crop_coeff_cache_entry_t crop_coeff_cache[WATERING_CHANNELS_COUNT]; /**< Crop coefficient cache per channel */
    water_balance_cache_entry_t water_balance_cache[WATERING_CHANNELS_COUNT]; /**< Water balance cache per channel */
    uint32_t cache_hit_count;          /**< Number of cache hits for performance monitoring */
    uint32_t cache_miss_count;         /**< Number of cache misses for performance monitoring */
    bool cache_enabled;                /**< Whether caching is enabled */
} fao56_calculation_cache_t;

/**
 * @brief Determine current phenological stage based on days after planting
 * 
 * @param plant Plant database entry
 * @param days_after_planting Days since planting
 * @return Current phenological stage
 */
phenological_stage_t calc_phenological_stage(
    const plant_full_data_t *plant,
    uint16_t days_after_planting
);

/**
 * @brief Calculate crop coefficient with interpolation between stages
 * 
 * @param plant Plant database entry
 * @param stage Current phenological stage
 * @param days_after_planting Days since planting
 * @return Crop coefficient (Kc)
 */
float calc_crop_coefficient(
    const plant_full_data_t *plant,
    phenological_stage_t stage,
    uint16_t days_after_planting
);

/**
 * @brief Calculate reference evapotranspiration using Penman-Monteith equation
 * 
 * @param env Environmental data
 * @param latitude_rad Latitude in radians
 * @param day_of_year Day of year (1-365)
 * @return Reference evapotranspiration ET0 (mm/day)
 */
float calc_et0_penman_monteith(
    const environmental_data_t *env,
    float latitude_rad,
    uint16_t day_of_year
);

/**
 * @brief Calculate reference evapotranspiration using Hargreaves-Samani equation
 * 
 * Fallback method when meteorological data is limited
 * 
 * @param env Environmental data (requires min/max temperature)
 * @param latitude_rad Latitude in radians
 * @param day_of_year Day of year (1-365)
 * @return Reference evapotranspiration ET0 (mm/day)
 */
float calc_et0_hargreaves_samani(
    const environmental_data_t *env,
    float latitude_rad,
    uint16_t day_of_year
);

/**
 * @brief Update water balance tracking
 * 
 * @param plant Plant database entry
 * @param soil Soil database entry
 * @param method Irrigation method database entry
 * @param env Environmental data
 * @param root_depth_current_m Current root depth (m)
 * @param balance Water balance structure to update
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t calc_water_balance(
    const plant_full_data_t *plant,
    const soil_enhanced_data_t *soil,
    const irrigation_method_data_t *method,
    const environmental_data_t *env,
    float root_depth_current_m,
    uint16_t days_after_planting,
    water_balance_t *balance
);

/**
 * @brief Calculate irrigation volume for area-based coverage
 * 
 * @param balance Current water balance
 * @param method Irrigation method database entry
 * @param area_m2 Area to irrigate (m2)
 * @param eco_mode Eco context flag (volume uses current deficit)
 * @param max_volume_limit_l Maximum volume limit (L)
 * @param result Calculation results structure
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t calc_irrigation_volume_area(
    const water_balance_t *balance,
    const irrigation_method_data_t *method,
    float area_m2,
    bool eco_mode,
    float max_volume_limit_l,
    irrigation_calculation_t *result
);

/**
 * @brief Calculate irrigation volume for plant-count-based coverage
 * 
 * @param balance Current water balance
 * @param method Irrigation method database entry
 * @param plant Plant database entry
 * @param plant_count Number of plants
 * @param eco_mode Eco context flag (volume uses current deficit)
 * @param max_volume_limit_l Maximum volume limit (L)
 * @param result Calculation results structure
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t calc_irrigation_volume_plants(
    const water_balance_t *balance,
    const irrigation_method_data_t *method,
    const plant_full_data_t *plant,
    uint16_t plant_count,
    bool eco_mode,
    float max_volume_limit_l,
    irrigation_calculation_t *result
);

/**
 * @brief Calculate current root depth based on plant age and characteristics
 * 
 * @param plant Plant database entry
 * @param days_after_planting Days since planting
 * @return Current root depth (m)
 */
float calc_current_root_depth(
    const plant_full_data_t *plant,
    uint16_t days_after_planting
);

/**
 * @brief Resolve soil parameters for a channel (custom soil if available, else standard DB)
 *
 * @param channel_id Channel ID
 * @param channel Channel configuration (optional, can be NULL to look up)
 * @return Pointer to resolved soil data, or NULL if unavailable
 */
const soil_enhanced_data_t *fao56_get_channel_soil(uint8_t channel_id, const watering_channel_t *channel);

/**
 * @brief Calculate effective precipitation based on soil infiltration
 * 
 * @param rainfall_mm Total rainfall (mm)
 * @param soil Soil database entry
 * @param irrigation_method Irrigation method for runoff assessment
 * @return Effective precipitation (mm)
 */
float calc_effective_precipitation(
    float rainfall_mm,
    const soil_enhanced_data_t *soil,
    const irrigation_method_data_t *irrigation_method
);

/**
 * @brief Determine if cycle and soak irrigation is needed
 * 
 * @param method Irrigation method database entry
 * @param soil Soil database entry
 * @param application_rate_mm_h Planned application rate (mm/h)
 * @param result Calculation results to update with cycle information
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t calc_cycle_and_soak(
    const irrigation_method_data_t *method,
    const soil_enhanced_data_t *soil,
    float application_rate_mm_h,
    irrigation_calculation_t *result
);

/**
 * @brief Apply quality irrigation mode (100% of calculated requirement)
 * 
 * @param balance Current water balance state
 * @param method Irrigation method database entry
 * @param plant Plant database entry (for plant-based calculations)
 * @param area_m2 Area to irrigate (m2)
 * @param plant_count Number of plants (for plant-based calculations, 0 for area-based)
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
);

/**
 * @brief Apply eco irrigation mode (reduced refill target)
 * 
 * @param balance Current water balance state
 * @param method Irrigation method database entry
 * @param plant Plant database entry (for plant-based calculations)
 * @param area_m2 Area to irrigate (m2)
 * @param plant_count Number of plants (for plant-based calculations, 0 for area-based)
 * @param application_rate_mm_h Application rate override (mm/h, 0 to use method defaults)
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
);

/**
 * @brief Apply maximum volume limiting with constraint logging
 * 
 * @param calculated_volume_l Originally calculated volume (liters)
 * @param max_volume_limit_l Maximum allowed volume (liters)
 * @param channel_id Channel ID for logging
 * @param mode_name Mode name for logging
 * @return Limited volume (liters)
 */
float apply_volume_limiting(
    float calculated_volume_l,
    float max_volume_limit_l,
    uint8_t channel_id,
    const char *mode_name
);

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
);

/**
 * @brief Check if irrigation is needed based on Management Allowed Depletion (MAD)
 * 
 * This function implements the irrigation trigger logic based on readily available
 * water depletion thresholds.
 * 
 * @param balance Current water balance state
 * @param plant Plant database entry
 * @param soil Soil database entry
 * @param stress_factor Environmental stress factor (0.8-1.2, default 1.0)
 * @return True if irrigation is needed, false otherwise
 */
bool check_irrigation_trigger_mad(
    const water_balance_t *balance,
    const plant_full_data_t *plant,
    const soil_enhanced_data_t *soil,
    float stress_factor
);

/**
 * @brief Calculate irrigation timing based on readily available water depletion
 * 
 * This function determines when irrigation should occur based on current
 * water balance and depletion rates.
 * 
 * @param balance Current water balance state
 * @param daily_et_rate Current daily ET rate (mm/day)
 * @param plant Plant database entry
 * @param hours_until_irrigation Calculated hours until irrigation needed
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t calc_irrigation_timing(
    const water_balance_t *balance,
    float daily_et_rate,
    const plant_full_data_t *plant,
    float *hours_until_irrigation
);

/**
 * @brief Apply environmental stress adjustments to MAD threshold
 * 
 * Adjusts the management allowed depletion based on environmental conditions
 * such as high temperature, low humidity, or wind stress.
 * 
 * @param base_mad_fraction Base MAD fraction from plant database
 * @param env Environmental data
 * @param plant Plant database entry
 * @return Adjusted MAD fraction
 */
float apply_environmental_stress_adjustment(
    float base_mad_fraction,
    const environmental_data_t *env,
    const plant_full_data_t *plant
);

/**
 * @brief Calculate effective root zone water capacity based on irrigation method wetting fraction
 * 
 * For localized irrigation systems (drip, micro-spray), only a fraction of the
 * root zone is wetted, affecting the available water capacity calculations.
 * 
 * @param total_awc_mm Total available water capacity in root zone (mm)
 * @param method Irrigation method database entry
 * @param plant Plant database entry
 * @param root_depth_m Current root depth (m)
 * @return Effective AWC considering wetting fraction (mm)
 */
float calc_effective_awc_with_wetting_fraction(
    float total_awc_mm,
    const irrigation_method_data_t *method,
    const plant_full_data_t *plant,
    float root_depth_m
);

/**
 * @brief Calculate localized irrigation wetting pattern adjustments
 * 
 * Determines the wetted area and depth characteristics for drip and micro-irrigation
 * systems, accounting for soil texture and emitter spacing.
 * 
 * @param method Irrigation method database entry
 * @param soil Soil database entry
 * @param emitter_spacing_m Spacing between emitters (m)
 * @param wetted_diameter_m Calculated wetted diameter (m)
 * @param wetted_depth_m Calculated wetted depth (m)
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t calc_localized_wetting_pattern(
    const irrigation_method_data_t *method,
    const soil_enhanced_data_t *soil,
    float emitter_spacing_m,
    float *wetted_diameter_m,
    float *wetted_depth_m
);

/**
 * @brief Adjust irrigation volume for partial root zone wetting
 * 
 * For localized irrigation, the irrigation volume needs to be adjusted to account
 * for the fact that only part of the root zone is wetted.
 * 
 * @param base_volume_mm Base irrigation volume for full coverage (mm)
 * @param method Irrigation method database entry
 * @param plant Plant database entry
 * @param soil Soil database entry
 * @return Adjusted irrigation volume (mm)
 */
float adjust_volume_for_partial_wetting(
    float base_volume_mm,
    const irrigation_method_data_t *method,
    const plant_full_data_t *plant,
    const soil_enhanced_data_t *soil
);

/**
 * @brief Calculate irrigation requirement using FAO-56 method
 * 
 * This is the main entry point for FAO-56 based irrigation calculations.
 * 
 * @param channel_id Channel ID
 * @param env Environmental data
 * @param result Calculation result structure to be filled
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t fao56_calculate_irrigation_requirement(uint8_t channel_id, 
                                                       const environmental_data_t *env,
                                                       irrigation_calculation_t *result);

/**
 * @brief Bind a per-channel water balance state for FAO-56 calculations
 *
 * Ensures the channel has a valid water_balance pointer backed by the
 * FAO-56 internal storage. Does not overwrite existing balances.
 *
 * @param channel_id Channel ID (0-7)
 * @param channel Pointer to channel data (optional; if NULL, it will be resolved)
 * @return Pointer to water balance state, or NULL on error
 */
water_balance_t *fao56_bind_channel_balance(uint8_t channel_id, watering_channel_t *channel);

/* ================================================================== */
/* AUTO (Smart Schedule) Mode - Daily Deficit Tracking              */
/* ================================================================== */

/**
 * @brief Result structure for daily AUTO mode evaluation
 */
typedef struct {
    bool should_water;                 /**< True if irrigation is needed today */
    float volume_liters;               /**< Volume to apply if watering (L) */
    float current_deficit_mm;          /**< Current soil water deficit (mm) */
    float raw_threshold_mm;            /**< RAW threshold that triggered decision (mm) */
    float daily_etc_mm;                /**< Daily crop evapotranspiration (mm) */
    float effective_rain_mm;           /**< Effective rainfall subtracted (mm) */
    float stress_factor;               /**< Applied environmental stress factor */
} fao56_auto_decision_t;

/**
 * @brief Perform daily deficit update and irrigation decision for AUTO mode
 * 
 * This function is called once per day (typically at 06:00) for channels configured
 * with SCHEDULE_AUTO. It:
 * 1. Reads current environmental data and 24h rainfall
 * 2. Computes daily ETc (ET0 × Kc based on plant growth stage)
 * 3. Subtracts effective rainfall from the deficit
 * 4. Updates irrigation decision thresholds (deficit is accumulated continuously)
 * 5. Applies environmental stress adjustment on hot/dry days
 * 6. Compares deficit against the plant's RAW threshold
 * 7. Persists updated water balance to NVS
 * 
 * @param channel_id Channel ID (0-7)
 * @param decision Output structure with irrigation decision and diagnostics
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t fao56_daily_update_deficit(uint8_t channel_id, 
                                            fao56_auto_decision_t *decision);

/**
 * @brief Accumulate soil water deficit in (near) real time for AUTO mode
 *
 * Called periodically (e.g., scheduler tick). Uses elapsed uptime since the
 * last update to add a fractional ETc contribution to the current deficit.
 * This does not apply rainfall (handled by the daily AUTO check) and does not
 * persist to NVS (to avoid flash wear).
 *
 * @param channel_id Channel ID (0-7)
 * @param env Current environmental snapshot (may be partially valid)
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t fao56_realtime_update_deficit(uint8_t channel_id,
                                              const environmental_data_t *env);

/**
 * @brief Apply incremental rainfall to AUTO water balance
 *
 * Used for near-real-time rain events to reduce deficit without waiting
 * for the daily AUTO update.
 *
 * @param rainfall_mm Incremental rainfall (mm)
 * @param air_temp_c Ambient air temperature for evaporation estimate (deg C)
 * @param duration_s Duration of the rainfall increment window (seconds)
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t fao56_apply_rainfall_increment(float rainfall_mm, float air_temp_c, uint32_t duration_s);

/**
 * @brief Handle multi-day offline gap by estimating missed deficit accumulation
 * 
 * Called on boot or when AUTO check detects missed days. Applies conservative
 * ETc estimates for each missed day to prevent under-watering after power loss.
 * 
 * @param channel_id Channel ID (0-7)
 * @param days_missed Number of days since last update
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t fao56_apply_missed_days_deficit(uint8_t channel_id, 
                                                  uint16_t days_missed);

/**
 * @brief Reduce channel deficit after successful irrigation
 * 
 * Called after a watering task completes. Converts applied volume to mm
 * and subtracts from current_deficit_mm, then persists to NVS.
 * 
 * @param channel_id Channel ID (0-7)
 * @param volume_applied_liters Volume of water applied (L)
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t fao56_reduce_deficit_after_irrigation(uint8_t channel_id,
                                                        float volume_applied_liters);

/* ================================================================== */
/* Performance Optimization - Calculation Caching Functions         */
/* ================================================================== */

/**
 * @brief Initialize the calculation cache system
 * 
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t fao56_cache_init(void);

/**
 * @brief Enable or disable calculation caching
 * 
 * @param enabled True to enable caching, false to disable
 */
void fao56_cache_set_enabled(bool enabled);

/**
 * @brief Clear all cache entries
 */
void fao56_cache_clear_all(void);

/**
 * @brief Clear cache entries for a specific channel
 * 
 * @param channel_id Channel ID to clear cache for
 */
void fao56_cache_clear_channel(uint8_t channel_id);

/**
 * @brief Get cache performance statistics
 * 
 * @param hit_count Pointer to store cache hit count
 * @param miss_count Pointer to store cache miss count
 * @param hit_ratio Pointer to store cache hit ratio (0.0-1.0)
 */
void fao56_cache_get_stats(uint32_t *hit_count, uint32_t *miss_count, float *hit_ratio);

/**
 * @brief Check if ET0 calculation result is cached and valid
 * 
 * @param env Environmental data
 * @param latitude_rad Latitude in radians
 * @param day_of_year Day of year
 * @param channel_id Channel ID for cache lookup
 * @param cached_result Pointer to store cached result if found
 * @return True if cached result is valid, false otherwise
 */
bool fao56_cache_get_et0(const environmental_data_t *env, float latitude_rad, 
                        uint16_t day_of_year, uint8_t channel_id, float *cached_result);

/**
 * @brief Store ET0 calculation result in cache
 * 
 * @param env Environmental data used in calculation
 * @param latitude_rad Latitude used in calculation
 * @param day_of_year Day of year used in calculation
 * @param channel_id Channel ID for cache storage
 * @param result ET0 result to cache
 */
void fao56_cache_store_et0(const environmental_data_t *env, float latitude_rad,
                          uint16_t day_of_year, uint8_t channel_id, float result);

/**
 * @brief Check if crop coefficient calculation result is cached and valid
 * 
 * @param plant_db_index Plant database index
 * @param days_after_planting Days after planting
 * @param channel_id Channel ID for cache lookup
 * @param cached_stage Pointer to store cached phenological stage if found
 * @param cached_coefficient Pointer to store cached crop coefficient if found
 * @return True if cached result is valid, false otherwise
 */
bool fao56_cache_get_crop_coeff(uint16_t plant_db_index, uint16_t days_after_planting,
                               uint8_t channel_id, phenological_stage_t *cached_stage,
                               float *cached_coefficient);

/**
 * @brief Store crop coefficient calculation result in cache
 * 
 * @param plant_db_index Plant database index used
 * @param days_after_planting Days after planting used
 * @param channel_id Channel ID for cache storage
 * @param stage Calculated phenological stage
 * @param coefficient Calculated crop coefficient
 */
void fao56_cache_store_crop_coeff(uint16_t plant_db_index, uint16_t days_after_planting,
                                 uint8_t channel_id, phenological_stage_t stage,
                                 float coefficient);

/**
 * @brief Check if water balance calculation result is cached and valid
 * 
 * @param channel_id Channel ID
 * @param plant_db_index Plant database index
 * @param soil_db_index Soil database index
 * @param irrigation_method_index Irrigation method index
 * @param root_depth_m Current root depth
 * @param cached_balance Pointer to store cached water balance if found
 * @return True if cached result is valid, false otherwise
 */
bool fao56_cache_get_water_balance(uint8_t channel_id, uint16_t plant_db_index,
                                  uint8_t soil_db_index, uint8_t irrigation_method_index,
                                  float root_depth_m, water_balance_t *cached_balance);

/**
 * @brief Store water balance calculation result in cache
 * 
 * @param channel_id Channel ID
 * @param plant_db_index Plant database index used
 * @param soil_db_index Soil database index used
 * @param irrigation_method_index Irrigation method index used
 * @param root_depth_m Root depth used
 * @param balance Calculated water balance result
 */
void fao56_cache_store_water_balance(uint8_t channel_id, uint16_t plant_db_index,
                                    uint8_t soil_db_index, uint8_t irrigation_method_index,
                                    float root_depth_m, const water_balance_t *balance);

/**
 * @brief Invalidate cache entries based on environmental data changes
 * 
 * This function should be called when environmental conditions change
 * significantly to ensure cache accuracy.
 * 
 * @param env_change_flags Bitmask indicating which environmental parameters changed
 */
void fao56_cache_invalidate_on_env_change(uint32_t env_change_flags);

/**
 * @brief Invalidate cache entries based on time intervals
 * 
 * This function should be called periodically to ensure cache entries
 * don't become stale.
 * 
 * @param max_age_seconds Maximum age for cache entries (seconds)
 */
void fao56_cache_invalidate_by_age(uint32_t max_age_seconds);

/* ================================================================== */
/* Resource-Constrained Operation Mode Functions                     */
/* ================================================================== */

/**
 * @brief Check if system resources are constrained
 * 
 * @return True if resources are limited, false if normal operation is possible
 */
bool fao56_is_resource_constrained(void);

/**
 * @brief Enable or disable resource-constrained operation mode
 * 
 * @param enabled True to enable constrained mode, false for normal operation
 */
void fao56_set_resource_constrained_mode(bool enabled);

/**
 * @brief Calculate simplified irrigation requirement for resource-constrained operation
 * 
 * This function provides a fallback calculation method when full FAO-56 calculations
 * are not feasible due to memory or CPU constraints.
 * 
 * @param channel_id Channel ID
 * @param env Environmental data (may use simplified subset)
 * @param result Calculation result structure to be filled
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t fao56_calculate_simplified_irrigation(uint8_t channel_id,
                                                      const environmental_data_t *env,
                                                      irrigation_calculation_t *result);

/**
 * @brief Get simplified ET0 calculation using temperature-only method
 * 
 * Fallback ET0 calculation when full meteorological data is not available
 * or when system resources are constrained.
 * 
 * @param temp_min_c Minimum temperature (°C)
 * @param temp_max_c Maximum temperature (°C)
 * @param latitude_rad Latitude in radians
 * @param day_of_year Day of year (1-365)
 * @return Simplified ET0 estimate (mm/day)
 */
float fao56_get_simplified_et0(float temp_min_c, float temp_max_c, 
                              float latitude_rad, uint16_t day_of_year);

/**
 * @brief Get simplified crop coefficient based on plant type only
 * 
 * Provides a basic crop coefficient estimate when detailed plant database
 * lookup is not feasible due to resource constraints.
 * 
 * @param plant_type Basic plant type (legacy enum)
 * @param days_after_planting Days since planting
 * @return Simplified crop coefficient estimate
 */
float fao56_get_simplified_crop_coefficient(plant_type_t plant_type, 
                                           uint16_t days_after_planting);

/**
 * @brief Get memory usage statistics for FAO-56 calculations
 * 
 * @param cache_memory_bytes Memory used by calculation cache
 * @param total_memory_bytes Total memory used by FAO-56 system
 */
void fao56_get_memory_usage(uint32_t *cache_memory_bytes, uint32_t *total_memory_bytes);

/* ================================================================== */
/* Error Handling and Fallback Functions                            */
/* ================================================================== */

/**
 * @brief Error recovery modes for FAO-56 calculations
 */
typedef enum {
    FAO56_RECOVERY_NONE = 0,           /**< No recovery needed */
    FAO56_RECOVERY_SIMPLIFIED = 1,     /**< Use simplified calculations */
    FAO56_RECOVERY_DEFAULTS = 2,       /**< Use default values */
    FAO56_RECOVERY_MANUAL_MODE = 3     /**< Fall back to manual irrigation */
} fao56_recovery_mode_t;

/**
 * @brief Detect and handle FAO-56 calculation failures
 * 
 * @param channel_id Channel ID where error occurred
 * @param error_code Original error code
 * @param env Environmental data (may be invalid)
 * @param result Result structure to populate with fallback values
 * @return Recovery mode applied
 */
fao56_recovery_mode_t fao56_handle_calculation_error(uint8_t channel_id,
                                                    watering_error_t error_code,
                                                    const environmental_data_t *env,
                                                    irrigation_calculation_t *result);

/**
 * @brief Handle environmental sensor failures with graceful degradation
 * 
 * @param env Environmental data structure (may have invalid readings)
 * @param fallback_env Fallback environmental data to populate
 * @return WATERING_SUCCESS if fallback data is usable, error code otherwise
 */
watering_error_t fao56_handle_sensor_failure(const environmental_data_t *env,
                                            environmental_data_t *fallback_env);

/**
 * @brief Validate environmental data and apply conservative defaults
 * 
 * @param env Environmental data to validate
 * @param validated_env Validated environmental data output
 * @return WATERING_SUCCESS if data is valid or successfully corrected
 */
watering_error_t fao56_validate_environmental_data(const environmental_data_t *env,
                                                  environmental_data_t *validated_env);

/**
 * @brief Get default irrigation schedule when automatic calculations fail
 * 
 * @param channel_id Channel ID
 * @param plant_type Plant type for default scheduling
 * @param result Default irrigation calculation result
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t fao56_get_default_irrigation_schedule(uint8_t channel_id,
                                                      plant_type_t plant_type,
                                                      irrigation_calculation_t *result);

/**
 * @brief Check system health and recommend recovery actions
 * 
 * @param health_status System health status output
 * @param recommended_action Recommended recovery action
 * @return WATERING_SUCCESS if system is healthy, error code with recommendations
 */
watering_error_t fao56_check_system_health(uint32_t *health_status,
                                          fao56_recovery_mode_t *recommended_action);

/**
 * @brief Log calculation errors with context for debugging
 * 
 * @param channel_id Channel ID where error occurred
 * @param error_code Error code
 * @param function_name Function where error occurred
 * @param additional_info Additional context information
 */
void fao56_log_calculation_error(uint8_t channel_id, watering_error_t error_code,
                               const char *function_name, const char *additional_info);

/* ============================================================================
 * SOLAR TIMING CALCULATIONS (NOAA Algorithm)
 * ============================================================================ */

/**
 * @brief Solar times calculation result
 * 
 * Contains sunrise/sunset times and related solar information
 */
typedef struct {
    uint8_t sunrise_hour;       /**< Sunrise hour (0-23, local time) */
    uint8_t sunrise_minute;     /**< Sunrise minute (0-59) */
    uint8_t sunset_hour;        /**< Sunset hour (0-23, local time) */
    uint8_t sunset_minute;      /**< Sunset minute (0-59) */
    uint16_t day_length_minutes; /**< Day length in minutes */
    bool is_polar_day;          /**< True if sun never sets (midnight sun) */
    bool is_polar_night;        /**< True if sun never rises (polar night) */
    bool calculation_valid;     /**< True if calculation succeeded */
} solar_times_t;

/**
 * @brief Calculate sunrise and sunset times using NOAA algorithm
 * 
 * Uses the NOAA Solar Calculator algorithm for ~1 minute precision.
 * Handles polar regions with appropriate fallbacks.
 * 
 * @param latitude_deg Latitude in degrees (-90 to +90)
 * @param longitude_deg Longitude in degrees (-180 to +180)
 * @param day_of_year Day of year (1-365/366)
 * @param timezone_offset_hours UTC offset in hours (e.g., +2 for UTC+2)
 * @param result Solar times result structure
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t fao56_calc_solar_times(float latitude_deg, 
                                        float longitude_deg,
                                        uint16_t day_of_year,
                                        int8_t timezone_offset_hours,
                                        solar_times_t *result);

/**
 * @brief Get effective start time based on solar timing configuration
 * 
 * If solar timing is enabled, calculates actual start time from sunrise/sunset.
 * If solar calculation fails or polar conditions apply, uses fallback time.
 * 
 * @param event Watering event with solar timing configuration
 * @param latitude_deg Latitude in degrees
 * @param longitude_deg Longitude in degrees
 * @param day_of_year Day of year (1-365/366)
 * @param timezone_offset_hours UTC offset in hours
 * @param effective_hour Output: effective start hour (0-23)
 * @param effective_minute Output: effective start minute (0-59)
 * @return WATERING_SUCCESS on success, WATERING_ERROR_SOLAR_FALLBACK if using fallback
 */
watering_error_t fao56_get_effective_start_time(const watering_event_t *event,
                                                 float latitude_deg,
                                                 float longitude_deg,
                                                 uint16_t day_of_year,
                                                 int8_t timezone_offset_hours,
                                                 uint8_t *effective_hour,
                                                 uint8_t *effective_minute);

#endif // FAO56_CALC_H
