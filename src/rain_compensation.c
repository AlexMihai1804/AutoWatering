/**
 * @file rain_compensation.c
 * @brief Rain compensation calculation engine implementation
 */

#include "rain_compensation.h"
#include "rain_sensor.h"
#include "rain_history.h"
#include "custom_soil_db.h"
#include "plant_db.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <math.h>
#include <string.h>

LOG_MODULE_REGISTER(rain_compensation, LOG_LEVEL_DBG);

/* Global configuration and state */
static rain_compensation_algorithm_t current_algorithm = RAIN_COMP_ALGORITHM_PROPORTIONAL;
static bool system_initialized = false;

/* Statistics tracking per channel */
typedef struct {
    uint32_t total_calculations;
    uint32_t skip_count;
    float total_reduction_pct;
    uint32_t last_calculation_time;
} rain_compensation_stats_t;

static rain_compensation_stats_t channel_stats[WATERING_CHANNELS_COUNT];

/* Algorithm names for debugging */
static const char *algorithm_names[] = {
    "Simple",
    "Proportional", 
    "Exponential",
    "Adaptive"
};

/* Default configuration values */
#define DEFAULT_SENSITIVITY         0.75f   // 75% sensitivity
#define DEFAULT_LOOKBACK_HOURS      48      // 48 hours
#define DEFAULT_SKIP_THRESHOLD_MM   5.0f    // 5mm threshold
#define DEFAULT_REDUCTION_FACTOR    0.8f    // 80% reduction factor

/* Calculation constants */
#define MIN_CONFIDENCE_LEVEL        20      // Minimum confidence level
#define MAX_CONFIDENCE_LEVEL        100     // Maximum confidence level
#define EXPONENTIAL_DECAY_FACTOR    0.693f  // ln(2) for half-life calculations
#define ADAPTIVE_SOIL_FACTOR_MIN    0.5f    // Minimum soil adaptation factor
#define ADAPTIVE_SOIL_FACTOR_MAX    1.5f    // Maximum soil adaptation factor

watering_error_t rain_compensation_init(void)
{
    LOG_INF("Initializing rain compensation calculation engine");
    
    /* Clear statistics */
    memset(channel_stats, 0, sizeof(channel_stats));
    
    /* Set default algorithm */
    current_algorithm = RAIN_COMP_ALGORITHM_PROPORTIONAL;
    
    system_initialized = true;
    
    LOG_INF("Rain compensation engine initialized with %s algorithm", 
            algorithm_names[current_algorithm]);
    
    return WATERING_SUCCESS;
}

watering_error_t rain_compensation_calculate(uint8_t channel_id,
                                           const rain_compensation_config_t *config,
                                           float base_requirement_mm,
                                           rain_compensation_calculation_t *result)
{
    if (!system_initialized) {
        LOG_ERR("Rain compensation system not initialized");
        return WATERING_ERROR_NOT_INITIALIZED;
    }
    
    if (channel_id >= WATERING_CHANNELS_COUNT || !config || !result) {
        LOG_ERR("Invalid parameters for rain compensation calculation");
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    if (!config->enabled) {
        LOG_DBG("Rain compensation disabled for channel %d", channel_id);
        /* Fill result with no compensation */
        memset(result, 0, sizeof(*result));
        result->base_water_requirement_mm = base_requirement_mm;
        result->adjusted_requirement_mm = base_requirement_mm;
        result->calculation_timestamp = k_uptime_get_32();
        result->confidence_level = 100;
        result->calculation_status = WATERING_SUCCESS;
        return WATERING_SUCCESS;
    }
    
    /* Validate configuration */
    watering_error_t err = rain_compensation_validate_config(config);
    if (err != WATERING_SUCCESS) {
        LOG_ERR("Invalid rain compensation configuration for channel %d", channel_id);
        return err;
    }
    
    /* Update statistics */
    channel_stats[channel_id].total_calculations++;
    channel_stats[channel_id].last_calculation_time = k_uptime_get_32();
    
    /* Delegate to specific algorithm */
    switch (current_algorithm) {
        case RAIN_COMP_ALGORITHM_SIMPLE:
            err = rain_compensation_calculate_simple(channel_id, config, base_requirement_mm, result);
            break;
            
        case RAIN_COMP_ALGORITHM_PROPORTIONAL:
            err = rain_compensation_calculate_proportional(channel_id, config, base_requirement_mm, result);
            break;
            
        case RAIN_COMP_ALGORITHM_EXPONENTIAL:
            err = rain_compensation_calculate_exponential(channel_id, config, base_requirement_mm, result);
            break;
            
        case RAIN_COMP_ALGORITHM_ADAPTIVE:
            err = rain_compensation_calculate_adaptive(channel_id, config, base_requirement_mm, result);
            break;
            
        default:
            LOG_ERR("Unknown rain compensation algorithm: %d", current_algorithm);
            return WATERING_ERROR_INVALID_PARAM;
    }
    
    if (err == WATERING_SUCCESS) {
        /* Update skip statistics */
        if (result->skip_watering) {
            channel_stats[channel_id].skip_count++;
        }
        
        /* Update reduction statistics */
        channel_stats[channel_id].total_reduction_pct += result->reduction_percentage;
        
        rain_compensation_log_calculation(channel_id, config, result, 
                                        algorithm_names[current_algorithm]);
    }
    
    return err;
}

watering_error_t rain_compensation_calculate_simple(uint8_t channel_id,
                                                   const rain_compensation_config_t *config,
                                                   float base_requirement_mm,
                                                   rain_compensation_calculation_t *result)
{
    /* Clear result structure */
    memset(result, 0, sizeof(*result));
    result->base_water_requirement_mm = base_requirement_mm;
    result->calculation_timestamp = k_uptime_get_32();
    result->calculation_status = WATERING_SUCCESS;
    
    /* Get recent rainfall */
    float total_rainfall_mm, effective_rainfall_mm;
    watering_error_t err = rain_compensation_get_recent_rainfall(channel_id, config->lookback_hours,
                                                               &total_rainfall_mm, &effective_rainfall_mm);
    if (err != WATERING_SUCCESS) {
        LOG_WRN("Failed to get rainfall data for channel %d, using zero rainfall", channel_id);
        total_rainfall_mm = 0.0f;
        effective_rainfall_mm = 0.0f;
        result->confidence_level = MIN_CONFIDENCE_LEVEL;
    } else {
        result->confidence_level = rain_compensation_calculate_confidence(channel_id, config, 80);
    }
    
    result->recent_rainfall_mm = total_rainfall_mm;
    result->effective_rainfall_mm = effective_rainfall_mm;
    
    /* Simple threshold algorithm: skip if rainfall exceeds threshold */
    if (effective_rainfall_mm >= config->skip_threshold_mm) {
        result->skip_watering = true;
        result->reduction_percentage = 100.0f;
        result->adjusted_requirement_mm = 0.0f;
        
    LOG_INF("Simple algorithm: Skipping watering for channel %d (%.1fmm rain >= %.1fmm threshold)",
        channel_id, (double)effective_rainfall_mm, (double)config->skip_threshold_mm);
    } else {
        result->skip_watering = false;
        result->reduction_percentage = 0.0f;
        result->adjusted_requirement_mm = base_requirement_mm;
        
    LOG_DBG("Simple algorithm: No reduction for channel %d (%.1fmm rain < %.1fmm threshold)",
        channel_id, (double)effective_rainfall_mm, (double)config->skip_threshold_mm);
    }
    
    return WATERING_SUCCESS;
}

watering_error_t rain_compensation_calculate_proportional(uint8_t channel_id,
                                                         const rain_compensation_config_t *config,
                                                         float base_requirement_mm,
                                                         rain_compensation_calculation_t *result)
{
    /* Clear result structure */
    memset(result, 0, sizeof(*result));
    result->base_water_requirement_mm = base_requirement_mm;
    result->calculation_timestamp = k_uptime_get_32();
    result->calculation_status = WATERING_SUCCESS;
    
    /* Get recent rainfall */
    float total_rainfall_mm, effective_rainfall_mm;
    watering_error_t err = rain_compensation_get_recent_rainfall(channel_id, config->lookback_hours,
                                                               &total_rainfall_mm, &effective_rainfall_mm);
    if (err != WATERING_SUCCESS) {
        LOG_WRN("Failed to get rainfall data for channel %d, using zero rainfall", channel_id);
        total_rainfall_mm = 0.0f;
        effective_rainfall_mm = 0.0f;
        result->confidence_level = MIN_CONFIDENCE_LEVEL;
    } else {
        result->confidence_level = rain_compensation_calculate_confidence(channel_id, config, 85);
    }
    
    result->recent_rainfall_mm = total_rainfall_mm;
    result->effective_rainfall_mm = effective_rainfall_mm;
    
    /* Proportional algorithm: reduction based on rainfall ratio */
    if (effective_rainfall_mm >= config->skip_threshold_mm) {
        /* Skip watering if threshold exceeded */
        result->skip_watering = true;
        result->reduction_percentage = 100.0f;
        result->adjusted_requirement_mm = 0.0f;
        
    LOG_INF("Proportional algorithm: Skipping watering for channel %d (%.1fmm rain >= %.1fmm threshold)",
        channel_id, (double)effective_rainfall_mm, (double)config->skip_threshold_mm);
    } else if (effective_rainfall_mm > 0.0f) {
        /* Calculate proportional reduction */
        float rain_ratio = effective_rainfall_mm / config->skip_threshold_mm;
        float base_reduction = rain_ratio * 100.0f; /* Convert to percentage */
        
        /* Apply sensitivity factor */
        result->reduction_percentage = base_reduction * config->sensitivity;
        
        /* Ensure reduction doesn't exceed 100% */
        if (result->reduction_percentage > 100.0f) {
            result->reduction_percentage = 100.0f;
        }
        
        /* Apply reduction factor */
        result->reduction_percentage *= config->reduction_factor;
        
        /* Calculate adjusted requirement */
        result->adjusted_requirement_mm = base_requirement_mm * 
                                        (1.0f - (result->reduction_percentage / 100.0f));
        
        result->skip_watering = false;
        
    LOG_INF("Proportional algorithm: %.1f%% reduction for channel %d (%.1fmm rain, ratio=%.2f)",
        (double)result->reduction_percentage, channel_id, (double)effective_rainfall_mm, (double)rain_ratio);
    } else {
        /* No rainfall, no reduction */
        result->skip_watering = false;
        result->reduction_percentage = 0.0f;
        result->adjusted_requirement_mm = base_requirement_mm;
        
        LOG_DBG("Proportional algorithm: No reduction for channel %d (no recent rainfall)", channel_id);
    }
    
    return WATERING_SUCCESS;
}

watering_error_t rain_compensation_calculate_exponential(uint8_t channel_id,
                                                        const rain_compensation_config_t *config,
                                                        float base_requirement_mm,
                                                        rain_compensation_calculation_t *result)
{
    /* Clear result structure */
    memset(result, 0, sizeof(*result));
    result->base_water_requirement_mm = base_requirement_mm;
    result->calculation_timestamp = k_uptime_get_32();
    result->calculation_status = WATERING_SUCCESS;
    
    /* Get recent rainfall */
    float total_rainfall_mm, effective_rainfall_mm;
    watering_error_t err = rain_compensation_get_recent_rainfall(channel_id, config->lookback_hours,
                                                               &total_rainfall_mm, &effective_rainfall_mm);
    if (err != WATERING_SUCCESS) {
        LOG_WRN("Failed to get rainfall data for channel %d, using zero rainfall", channel_id);
        total_rainfall_mm = 0.0f;
        effective_rainfall_mm = 0.0f;
        result->confidence_level = MIN_CONFIDENCE_LEVEL;
    } else {
        result->confidence_level = rain_compensation_calculate_confidence(channel_id, config, 90);
    }
    
    result->recent_rainfall_mm = total_rainfall_mm;
    result->effective_rainfall_mm = effective_rainfall_mm;
    
    /* Exponential decay algorithm: recent rain has more impact */
    if (effective_rainfall_mm >= config->skip_threshold_mm) {
        /* Skip watering if threshold exceeded */
        result->skip_watering = true;
        result->reduction_percentage = 100.0f;
        result->adjusted_requirement_mm = 0.0f;
        
    LOG_INF("Exponential algorithm: Skipping watering for channel %d (%.1fmm rain >= %.1fmm threshold)",
        channel_id, (double)effective_rainfall_mm, (double)config->skip_threshold_mm);
    } else if (effective_rainfall_mm > 0.0f) {
        /* Calculate exponential decay based on time */
        /* For simplicity, assume uniform distribution over lookback period */
        float half_life_hours = config->lookback_hours / 4.0f; /* Quarter of lookback period */
        float decay_factor = expf(-EXPONENTIAL_DECAY_FACTOR * (config->lookback_hours / 2.0f) / half_life_hours);
        
        /* Calculate base reduction */
        float rain_ratio = effective_rainfall_mm / config->skip_threshold_mm;
        float base_reduction = rain_ratio * 100.0f * decay_factor;
        
        /* Apply sensitivity factor */
        result->reduction_percentage = base_reduction * config->sensitivity;
        
        /* Apply reduction factor */
        result->reduction_percentage *= config->reduction_factor;
        
        /* Ensure reduction doesn't exceed 100% */
        if (result->reduction_percentage > 100.0f) {
            result->reduction_percentage = 100.0f;
        }
        
        /* Calculate adjusted requirement */
        result->adjusted_requirement_mm = base_requirement_mm * 
                                        (1.0f - (result->reduction_percentage / 100.0f));
        
        result->skip_watering = false;
        
    LOG_INF("Exponential algorithm: %.1f%% reduction for channel %d (%.1fmm rain, decay=%.3f)",
        (double)result->reduction_percentage, channel_id, (double)effective_rainfall_mm, (double)decay_factor);
    } else {
        /* No rainfall, no reduction */
        result->skip_watering = false;
        result->reduction_percentage = 0.0f;
        result->adjusted_requirement_mm = base_requirement_mm;
        
        LOG_DBG("Exponential algorithm: No reduction for channel %d (no recent rainfall)", channel_id);
    }
    
    return WATERING_SUCCESS;
}

watering_error_t rain_compensation_calculate_adaptive(uint8_t channel_id,
                                                     const rain_compensation_config_t *config,
                                                     float base_requirement_mm,
                                                     rain_compensation_calculation_t *result)
{
    /* Clear result structure */
    memset(result, 0, sizeof(*result));
    result->base_water_requirement_mm = base_requirement_mm;
    result->calculation_timestamp = k_uptime_get_32();
    result->calculation_status = WATERING_SUCCESS;
    
    /* Get recent rainfall */
    float total_rainfall_mm, effective_rainfall_mm;
    watering_error_t err = rain_compensation_get_recent_rainfall(channel_id, config->lookback_hours,
                                                               &total_rainfall_mm, &effective_rainfall_mm);
    if (err != WATERING_SUCCESS) {
        LOG_WRN("Failed to get rainfall data for channel %d, using zero rainfall", channel_id);
        total_rainfall_mm = 0.0f;
        effective_rainfall_mm = 0.0f;
        result->confidence_level = MIN_CONFIDENCE_LEVEL;
    } else {
        result->confidence_level = rain_compensation_calculate_confidence(channel_id, config, 95);
    }
    
    result->recent_rainfall_mm = total_rainfall_mm;
    result->effective_rainfall_mm = effective_rainfall_mm;
    
    /* Adaptive algorithm: adjust based on soil and plant characteristics */
    if (effective_rainfall_mm >= config->skip_threshold_mm) {
        /* Skip watering if threshold exceeded */
        result->skip_watering = true;
        result->reduction_percentage = 100.0f;
        result->adjusted_requirement_mm = 0.0f;
        
    LOG_INF("Adaptive algorithm: Skipping watering for channel %d (%.1fmm rain >= %.1fmm threshold)",
        channel_id, (double)effective_rainfall_mm, (double)config->skip_threshold_mm);
    } else if (effective_rainfall_mm > 0.0f) {
        /* Get soil adaptation factor */
        float soil_factor = 1.0f;
        
        /* Check if custom soil is being used */
        if (custom_soil_db_exists(channel_id)) {
            custom_soil_entry_t custom_soil;
            if (custom_soil_db_read(channel_id, &custom_soil) == WATERING_SUCCESS) {
                /* Adapt based on infiltration rate */
                if (custom_soil.infiltration_rate < 10.0f) {
                    soil_factor = ADAPTIVE_SOIL_FACTOR_MAX; /* Clay soil - more impact from rain */
                } else if (custom_soil.infiltration_rate > 100.0f) {
                    soil_factor = ADAPTIVE_SOIL_FACTOR_MIN; /* Sandy soil - less impact from rain */
                } else {
                    /* Linear interpolation between min and max */
                    float normalized_rate = (custom_soil.infiltration_rate - 10.0f) / 90.0f;
                    soil_factor = ADAPTIVE_SOIL_FACTOR_MAX - 
                                (normalized_rate * (ADAPTIVE_SOIL_FACTOR_MAX - ADAPTIVE_SOIL_FACTOR_MIN));
                }
            }
        }
        
        /* Calculate base reduction with soil adaptation */
        float rain_ratio = effective_rainfall_mm / config->skip_threshold_mm;
        float base_reduction = rain_ratio * 100.0f * soil_factor;
        
        /* Apply sensitivity factor */
        result->reduction_percentage = base_reduction * config->sensitivity;
        
        /* Apply reduction factor */
        result->reduction_percentage *= config->reduction_factor;
        
        /* Ensure reduction doesn't exceed 100% */
        if (result->reduction_percentage > 100.0f) {
            result->reduction_percentage = 100.0f;
        }
        
        /* Calculate adjusted requirement */
        result->adjusted_requirement_mm = base_requirement_mm * 
                                        (1.0f - (result->reduction_percentage / 100.0f));
        
        result->skip_watering = false;
        
    LOG_INF("Adaptive algorithm: %.1f%% reduction for channel %d (%.1fmm rain, soil_factor=%.2f)",
        (double)result->reduction_percentage, channel_id, (double)effective_rainfall_mm, (double)soil_factor);
    } else {
        /* No rainfall, no reduction */
        result->skip_watering = false;
        result->reduction_percentage = 0.0f;
        result->adjusted_requirement_mm = base_requirement_mm;
        
        LOG_DBG("Adaptive algorithm: No reduction for channel %d (no recent rainfall)", channel_id);
    }
    
    return WATERING_SUCCESS;
}

watering_error_t rain_compensation_get_recent_rainfall(uint8_t channel_id,
                                                      uint16_t lookback_hours,
                                                      float *total_rainfall_mm,
                                                      float *effective_rainfall_mm)
{
    if (channel_id >= WATERING_CHANNELS_COUNT || !total_rainfall_mm || !effective_rainfall_mm) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    /* Initialize outputs */
    *total_rainfall_mm = 0.0f;
    *effective_rainfall_mm = 0.0f;
    
    /* Get rainfall data from rain history system */
    /* This would integrate with the rain_history module */
    /* For now, simulate with a simple approach */
    
    /* Try to get recent rainfall from rain sensor */
    float recent_rain = 0.0f;
    /* Use rain history aggregation over lookback window */
    recent_rain = rain_history_get_recent_total(lookback_hours);
    if (recent_rain >= 0.0f) {
        *total_rainfall_mm = recent_rain;
        
        /* Calculate effective rainfall based on soil infiltration */
        watering_error_t err = rain_compensation_calculate_effective_rainfall(channel_id, 
                                                                            recent_rain, 
                                                                            effective_rainfall_mm);
        if (err != WATERING_SUCCESS) {
            /* Use a default infiltration efficiency of 80% */
            *effective_rainfall_mm = recent_rain * 0.8f;
        }
        
    LOG_DBG("Recent rainfall for channel %d: total=%.1fmm, effective=%.1fmm",
        channel_id, (double)*total_rainfall_mm, (double)*effective_rainfall_mm);
        
        return WATERING_SUCCESS;
    } else {
        LOG_WRN("Failed to get recent rainfall data");
        return WATERING_ERROR_HARDWARE;
    }
}

watering_error_t rain_compensation_calculate_effective_rainfall(uint8_t channel_id,
                                                              float total_rainfall_mm,
                                                              float *effective_rainfall_mm)
{
    if (channel_id >= WATERING_CHANNELS_COUNT || !effective_rainfall_mm) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    /* Default infiltration efficiency */
    float infiltration_efficiency = 0.8f; /* 80% default */
    
    /* Check if custom soil is being used */
    if (custom_soil_db_exists(channel_id)) {
        custom_soil_entry_t custom_soil;
        if (custom_soil_db_read(channel_id, &custom_soil) == WATERING_SUCCESS) {
            /* Calculate infiltration efficiency based on soil properties */
            if (custom_soil.infiltration_rate < 5.0f) {
                infiltration_efficiency = 0.6f; /* Low infiltration - more runoff */
            } else if (custom_soil.infiltration_rate > 50.0f) {
                infiltration_efficiency = 0.95f; /* High infiltration - less runoff */
            } else {
                /* Linear interpolation */
                float normalized_rate = (custom_soil.infiltration_rate - 5.0f) / 45.0f;
                infiltration_efficiency = 0.6f + (normalized_rate * 0.35f);
            }
        }
    }
    
    /* Calculate effective rainfall */
    *effective_rainfall_mm = total_rainfall_mm * infiltration_efficiency;
    
    LOG_DBG("Effective rainfall calculation: total=%.1fmm, efficiency=%.2f, effective=%.1fmm",
            (double)total_rainfall_mm, (double)infiltration_efficiency, (double)*effective_rainfall_mm);
    
    return WATERING_SUCCESS;
}

watering_error_t rain_compensation_apply_to_duration(uint32_t original_duration_sec,
                                                    const rain_compensation_calculation_t *compensation_result,
                                                    uint32_t *adjusted_duration_sec)
{
    if (!compensation_result || !adjusted_duration_sec) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    if (compensation_result->skip_watering) {
        *adjusted_duration_sec = 0;
    } else {
        float reduction_factor = 1.0f - (compensation_result->reduction_percentage / 100.0f);
        *adjusted_duration_sec = (uint32_t)(original_duration_sec * reduction_factor);
    }
    
    LOG_DBG("Duration adjustment: original=%ds, reduction=%.1f%%, adjusted=%ds",
            original_duration_sec, (double)compensation_result->reduction_percentage, *adjusted_duration_sec);
    
    return WATERING_SUCCESS;
}

watering_error_t rain_compensation_apply_to_volume(uint32_t original_volume_ml,
                                                  const rain_compensation_calculation_t *compensation_result,
                                                  uint32_t *adjusted_volume_ml)
{
    if (!compensation_result || !adjusted_volume_ml) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    if (compensation_result->skip_watering) {
        *adjusted_volume_ml = 0;
    } else {
        float reduction_factor = 1.0f - (compensation_result->reduction_percentage / 100.0f);
        *adjusted_volume_ml = (uint32_t)(original_volume_ml * reduction_factor);
    }
    
    LOG_DBG("Volume adjustment: original=%dml, reduction=%.1f%%, adjusted=%dml",
            original_volume_ml, (double)compensation_result->reduction_percentage, *adjusted_volume_ml);
    
    return WATERING_SUCCESS;
}

watering_error_t rain_compensation_validate_config(const rain_compensation_config_t *config)
{
    if (!config) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    if (config->sensitivity < 0.0f || config->sensitivity > 1.0f) {
    LOG_ERR("Invalid sensitivity: %.2f (must be 0.0-1.0)", (double)config->sensitivity);
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    if (config->lookback_hours == 0 || config->lookback_hours > 168) { /* Max 1 week */
        LOG_ERR("Invalid lookback hours: %d (must be 1-168)", config->lookback_hours);
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    if (config->skip_threshold_mm < 0.0f || config->skip_threshold_mm > 100.0f) {
    LOG_ERR("Invalid skip threshold: %.2f (must be 0.0-100.0)", (double)config->skip_threshold_mm);
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    if (config->reduction_factor < 0.0f || config->reduction_factor > 1.0f) {
    LOG_ERR("Invalid reduction factor: %.2f (must be 0.0-1.0)", (double)config->reduction_factor);
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    return WATERING_SUCCESS;
}

watering_error_t rain_compensation_get_default_config(rain_compensation_config_t *config)
{
    if (!config) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    config->enabled = true;
    config->sensitivity = DEFAULT_SENSITIVITY;
    config->lookback_hours = DEFAULT_LOOKBACK_HOURS;
    config->skip_threshold_mm = DEFAULT_SKIP_THRESHOLD_MM;
    config->reduction_factor = DEFAULT_REDUCTION_FACTOR;
    
    return WATERING_SUCCESS;
}

uint8_t rain_compensation_calculate_confidence(uint8_t channel_id,
                                             const rain_compensation_config_t *config,
                                             uint8_t rainfall_data_quality)
{
    uint8_t confidence = MAX_CONFIDENCE_LEVEL;
    
    /* Reduce confidence based on data quality */
    if (rainfall_data_quality < 50) {
        confidence -= 30;
    } else if (rainfall_data_quality < 80) {
        confidence -= 15;
    }
    
    /* Reduce confidence for very short lookback periods */
    if (config->lookback_hours < 12) {
        confidence -= 20;
    } else if (config->lookback_hours < 24) {
        confidence -= 10;
    }
    
    /* Reduce confidence if custom soil data is not available */
    if (!custom_soil_db_exists(channel_id)) {
        confidence -= 5;
    }
    
    /* Ensure minimum confidence level */
    if (confidence < MIN_CONFIDENCE_LEVEL) {
        confidence = MIN_CONFIDENCE_LEVEL;
    }
    
    return confidence;
}

const char *rain_compensation_get_algorithm_name(rain_compensation_algorithm_t algorithm)
{
    if (algorithm < sizeof(algorithm_names) / sizeof(algorithm_names[0])) {
        return algorithm_names[algorithm];
    }
    return "Unknown";
}

watering_error_t rain_compensation_set_algorithm(rain_compensation_algorithm_t algorithm)
{
    if (algorithm >= sizeof(algorithm_names) / sizeof(algorithm_names[0])) {
        LOG_ERR("Invalid rain compensation algorithm: %d", algorithm);
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    current_algorithm = algorithm;
    LOG_INF("Rain compensation algorithm set to: %s", algorithm_names[algorithm]);
    
    return WATERING_SUCCESS;
}

rain_compensation_algorithm_t rain_compensation_get_algorithm(void)
{
    return current_algorithm;
}

watering_error_t rain_compensation_get_statistics(uint8_t channel_id,
                                                 uint32_t *total_calculations,
                                                 uint32_t *skip_count,
                                                 float *avg_reduction_pct)
{
    if (channel_id >= WATERING_CHANNELS_COUNT || !total_calculations || 
        !skip_count || !avg_reduction_pct) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    *total_calculations = channel_stats[channel_id].total_calculations;
    *skip_count = channel_stats[channel_id].skip_count;
    
    if (*total_calculations > 0) {
        *avg_reduction_pct = channel_stats[channel_id].total_reduction_pct / *total_calculations;
    } else {
        *avg_reduction_pct = 0.0f;
    }
    
    return WATERING_SUCCESS;
}

watering_error_t rain_compensation_reset_statistics(uint8_t channel_id)
{
    if (channel_id == 0xFF) {
        /* Reset all channels */
        memset(channel_stats, 0, sizeof(channel_stats));
        LOG_INF("Reset rain compensation statistics for all channels");
    } else if (channel_id < WATERING_CHANNELS_COUNT) {
        /* Reset specific channel */
        memset(&channel_stats[channel_id], 0, sizeof(rain_compensation_stats_t));
        LOG_INF("Reset rain compensation statistics for channel %d", channel_id);
    } else {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    return WATERING_SUCCESS;
}

void rain_compensation_log_calculation(uint8_t channel_id,
                                      const rain_compensation_config_t *config,
                                      const rain_compensation_calculation_t *result,
                                      const char *additional_info)
{
    if (!config || !result) {
        return;
    }
    
    LOG_INF("Rain compensation [%s] - Channel %d: %.1fmm rain -> %.1f%% reduction (skip=%s, confidence=%d%%)",
            additional_info ? additional_info : "Unknown",
            channel_id,
            (double)result->effective_rainfall_mm,
            (double)result->reduction_percentage,
            result->skip_watering ? "yes" : "no",
            result->confidence_level);
    
    if (result->calculation_status != WATERING_SUCCESS) {
        LOG_WRN("Rain compensation calculation had errors: %d", result->calculation_status);
    }
}