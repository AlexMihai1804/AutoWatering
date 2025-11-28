#include "temperature_compensation_integration.h"
#include "watering.h"
#include <zephyr/logging/log.h>
#include <string.h>
#include <stdio.h>

LOG_MODULE_REGISTER(temp_comp_integration, LOG_LEVEL_DBG);

/* External reference to watering channels */
extern watering_channel_t watering_channels[WATERING_CHANNELS_COUNT];

int temp_comp_apply_to_fao56(uint8_t channel_id,
                            const environmental_data_t *env,
                            const irrigation_calculation_t *base_result,
                            irrigation_calculation_t *compensated_result)
{
    if (channel_id >= WATERING_CHANNELS_COUNT || !env || !base_result || !compensated_result) {
        LOG_ERR("Invalid parameters for FAO-56 temperature compensation");
        return -EINVAL;
    }

    watering_channel_t *channel = &watering_channels[channel_id];
    
    // Check if temperature compensation is enabled for this channel
    if (!channel->temp_compensation.enabled) {
        LOG_DBG("Temperature compensation disabled for channel %u", channel_id);
        *compensated_result = *base_result;
        return 0;
    }

    // Validate environmental data
    bool env_valid;
    float fallback_temp;
    int ret = temp_comp_validate_environmental_data(env, &env_valid, &fallback_temp);
    if (ret != 0) {
        return ret;
    }

    float current_temp = env_valid ? env->air_temp_mean_c : fallback_temp;

    // Calculate temperature compensation (adapt inline config to enhanced type)
    temperature_compensation_config_t cfg = {
        .enabled = channel->temp_compensation.enabled,
        .base_temperature = channel->temp_compensation.base_temperature,
        .sensitivity = channel->temp_compensation.sensitivity,
        .min_factor = channel->temp_compensation.min_factor,
        .max_factor = channel->temp_compensation.max_factor,
    };
    temperature_compensation_result_t temp_result;
    ret = temp_compensation_calculate(&cfg, current_temp, &temp_result);
    if (ret != 0) {
        LOG_ERR("Temperature compensation calculation failed for channel %u: %d", channel_id, ret);
        return ret;
    }

    // Copy base result and apply compensation
    *compensated_result = *base_result;
    
    // Apply compensation to volume
    uint32_t compensated_volume;
    ret = temp_compensation_apply((uint32_t)(base_result->volume_liters * 1000), 
                                 temp_result.compensation_factor, 
                                 &compensated_volume);
    if (ret != 0) {
        LOG_ERR("Failed to apply temperature compensation to volume");
        return ret;
    }
    
    compensated_result->volume_liters = (float)compensated_volume / 1000.0f;
    
    // Apply compensation to per-plant volume if applicable
    if (base_result->volume_per_plant_liters > 0.0f) {
        uint32_t compensated_per_plant;
        ret = temp_compensation_apply((uint32_t)(base_result->volume_per_plant_liters * 1000),
                                     temp_result.compensation_factor,
                                     &compensated_per_plant);
        if (ret != 0) {
            LOG_ERR("Failed to apply temperature compensation to per-plant volume");
            return ret;
        }
        compensated_result->volume_per_plant_liters = (float)compensated_per_plant / 1000.0f;
    }

    // Apply compensation to net and gross irrigation amounts
    compensated_result->net_irrigation_mm *= temp_result.compensation_factor;
    compensated_result->gross_irrigation_mm *= temp_result.compensation_factor;

    // Store compensation summary in channel
    channel->last_temp_compensation.compensation_factor = temp_result.compensation_factor;
    channel->last_temp_compensation.adjusted_requirement = compensated_result->volume_liters;

    // Log the compensation application
    temp_comp_log_application(channel_id, &temp_result, 
                             (uint32_t)(base_result->volume_liters * 1000),
                             (uint32_t)(compensated_result->volume_liters * 1000));

    LOG_DBG("Applied temperature compensation to channel %u: %.1f°C, factor=%.3f, volume=%.2fL->%.2fL",
            channel_id, (double)current_temp, (double)temp_result.compensation_factor,
            (double)base_result->volume_liters, (double)compensated_result->volume_liters);

    return 0;
}

int temp_comp_calculate_compensated_et0(const temperature_compensation_config_t *config,
                                       const environmental_data_t *env,
                                       float latitude_rad,
                                       uint16_t day_of_year,
                                       float *compensated_et0)
{
    if (!config || !env || !compensated_et0) {
        LOG_ERR("Invalid parameters for compensated ET0 calculation");
        return -EINVAL;
    }

    // Calculate base ET0 using Penman-Monteith or Hargreaves-Samani
    float base_et0;
    if (env->rel_humidity_pct > 0.0f && env->atmos_pressure_hpa > 0.0f) {
        // Use Penman-Monteith if full meteorological data is available
        base_et0 = calc_et0_penman_monteith(env, latitude_rad, day_of_year);
    } else {
        // Fall back to Hargreaves-Samani with temperature only
        base_et0 = calc_et0_hargreaves_samani(env, latitude_rad, day_of_year);
    }

    if (base_et0 <= 0.0f) {
    LOG_ERR("Invalid base ET0 calculation: %.3f", (double)base_et0);
        return -EINVAL;
    }

    // Apply temperature compensation to ET0
    int ret = temp_compensation_calculate_et0(config, env->air_temp_mean_c, 
                                             base_et0, compensated_et0);
    if (ret != 0) {
        LOG_ERR("Failed to apply temperature compensation to ET0");
        return ret;
    }

    LOG_DBG("Compensated ET0: %.3f -> %.3f mm/day (temp=%.1f°C)",
            (double)base_et0, (double)*compensated_et0, (double)env->air_temp_mean_c);

    return 0;
}

int temp_comp_apply_to_quality_mode(uint8_t channel_id,
                                   const environmental_data_t *env,
                                   const irrigation_calculation_t *base_result,
                                   irrigation_calculation_t *compensated_result)
{
    if (channel_id >= WATERING_CHANNELS_COUNT) {
        LOG_ERR("Invalid channel ID: %u", channel_id);
        return -EINVAL;
    }

    /* FAO-56 Quality mode already incorporates temperature in ET0 calculations.
     * Do NOT apply additional temperature compensation - just copy the base result.
     */
    LOG_DBG("Quality mode (FAO-56): skipping temp compensation - already in ET0 calc");
    *compensated_result = *base_result;
    return 0;
}

int temp_comp_apply_to_eco_mode(uint8_t channel_id,
                               const environmental_data_t *env,
                               const irrigation_calculation_t *base_result,
                               irrigation_calculation_t *compensated_result)
{
    if (channel_id >= WATERING_CHANNELS_COUNT) {
        LOG_ERR("Invalid channel ID: %u", channel_id);
        return -EINVAL;
    }

    /* FAO-56 Eco mode already incorporates temperature in ET0 calculations.
     * Do NOT apply additional temperature compensation - just copy the base result.
     */
    LOG_DBG("Eco mode (FAO-56): skipping temp compensation - already in ET0 calc");
    *compensated_result = *base_result;
    return 0;
}

int temp_comp_get_channel_status(uint8_t channel_id,
                                temperature_compensation_result_t *result)
{
    if (channel_id >= WATERING_CHANNELS_COUNT || !result) {
        LOG_ERR("Invalid parameters for channel status");
        return -EINVAL;
    }

    watering_channel_t *channel = &watering_channels[channel_id];
    // Adapt minimal stored info into full result structure
    result->current_temperature = 0.0f; // not stored in watering_channel_t
    result->compensation_factor = channel->last_temp_compensation.compensation_factor;
    result->adjusted_requirement = channel->last_temp_compensation.adjusted_requirement;
    result->calculation_timestamp = 0; // not tracked here

    return 0;
}

int temp_comp_update_channel_config(uint8_t channel_id,
                                   const temperature_compensation_config_t *config)
{
    if (channel_id >= WATERING_CHANNELS_COUNT || !config) {
        LOG_ERR("Invalid parameters for config update");
        return -EINVAL;
    }

    // Validate the configuration
    int ret = temp_compensation_validate_config(config);
    if (ret != 0) {
        LOG_ERR("Invalid temperature compensation configuration for channel %u", channel_id);
        return ret;
    }

    watering_channel_t *channel = &watering_channels[channel_id];
    // Copy fields individually due to different struct types
    channel->temp_compensation.enabled = config->enabled;
    channel->temp_compensation.base_temperature = config->base_temperature;
    channel->temp_compensation.sensitivity = config->sensitivity;
    channel->temp_compensation.min_factor = config->min_factor;
    channel->temp_compensation.max_factor = config->max_factor;

    // Update configuration status
    channel->config_status.compensation_configured = true;

    LOG_INF("Updated temperature compensation config for channel %u: enabled=%s, base=%.1f°C",
            channel_id, config->enabled ? "true" : "false", (double)config->base_temperature);

    return 0;
}

int temp_comp_should_apply(uint8_t channel_id,
                          watering_mode_t mode,
                          bool *should_apply)
{
    if (channel_id >= WATERING_CHANNELS_COUNT || !should_apply) {
        LOG_ERR("Invalid parameters for should_apply check");
        return -EINVAL;
    }

    watering_channel_t *channel = &watering_channels[channel_id];
    
    /* Temperature compensation only applies to TIME and VOLUME modes.
     * FAO-56 automatic modes (QUALITY/ECO) already incorporate temperature
     * in their ET0 calculations via Penman-Monteith or Hargreaves-Samani.
     * Applying additional compensation would double-count temperature impact.
     */
    *should_apply = channel->temp_compensation.enabled &&
                   (mode == WATERING_BY_DURATION || mode == WATERING_BY_VOLUME);

    LOG_DBG("Temperature compensation for channel %u, mode %d: %s",
            channel_id, (int)mode, *should_apply ? "apply" : "skip");

    return 0;
}

int temp_comp_apply_with_fallback(uint8_t channel_id,
                                 const environmental_data_t *env,
                                 const irrigation_calculation_t *base_result,
                                 irrigation_calculation_t *compensated_result)
{
    if (channel_id >= WATERING_CHANNELS_COUNT || !base_result || !compensated_result) {
        LOG_ERR("Invalid parameters for fallback compensation");
        return -EINVAL;
    }

    // Try normal temperature compensation first
    int ret = temp_comp_apply_to_fao56(channel_id, env, base_result, compensated_result);
    if (ret == 0) {
        return 0; // Success
    }

    LOG_WRN("Temperature compensation failed for channel %u, using fallback", channel_id);

    // Fallback: copy base result without compensation
    *compensated_result = *base_result;

    // Clear any invalid compensation summary
    watering_channel_t *channel = &watering_channels[channel_id];
    channel->last_temp_compensation.compensation_factor = 1.0f;
    channel->last_temp_compensation.adjusted_requirement = base_result->volume_liters;

    return 0; // Return success for fallback
}

void temp_comp_log_application(uint8_t channel_id,
                              const temperature_compensation_result_t *compensation_result,
                              uint32_t base_volume,
                              uint32_t final_volume)
{
    if (!compensation_result) {
        return;
    }

    char description[64];
    int ret = temp_compensation_get_description(compensation_result->compensation_factor, description);
    if (ret != 0) {
        snprintf(description, sizeof(description), "Factor: %.3f", (double)compensation_result->compensation_factor);
    }

    LOG_INF("Channel %u temperature compensation: %.1f°C -> %s (%u->%u ml)",
            channel_id, (double)compensation_result->current_temperature, description,
            base_volume, final_volume);
}

int temp_comp_validate_environmental_data(const environmental_data_t *env,
                                         bool *is_valid,
                                         float *fallback_temp)
{
    if (!env || !is_valid || !fallback_temp) {
        LOG_ERR("Invalid parameters for environmental data validation");
        return -EINVAL;
    }

    *is_valid = temp_compensation_is_temp_valid(env->air_temp_mean_c);
    
    if (!*is_valid) {
        // Use daily average as fallback if available  
        if (temp_compensation_is_temp_valid(env->air_temp_min_c)) {
            *fallback_temp = env->air_temp_min_c;
            LOG_WRN("Using minimum temperature as fallback: %.1f°C", (double)*fallback_temp);
        } else {
            // Use default base temperature as last resort
            *fallback_temp = TEMP_COMP_DEFAULT_BASE_TEMP;
            LOG_WRN("Using default temperature as fallback: %.1f°C", (double)*fallback_temp);
        }
    } else {
        *fallback_temp = env->air_temp_mean_c;
    }

    return 0;
}

int temp_comp_calculate_with_trend(uint8_t channel_id,
                                  float current_temp,
                                  const float *recent_temps,
                                  uint8_t temp_count,
                                  temperature_compensation_result_t *result)
{
    if (channel_id >= WATERING_CHANNELS_COUNT || !result) {
        LOG_ERR("Invalid parameters for trend calculation");
        return -EINVAL;
    }

    watering_channel_t *channel = &watering_channels[channel_id];
    
    // Calculate basic temperature compensation (adapt inline config)
    temperature_compensation_config_t cfg = {
        .enabled = channel->temp_compensation.enabled,
        .base_temperature = channel->temp_compensation.base_temperature,
        .sensitivity = channel->temp_compensation.sensitivity,
        .min_factor = channel->temp_compensation.min_factor,
        .max_factor = channel->temp_compensation.max_factor,
    };
    int ret = temp_compensation_calculate(&cfg, current_temp, result);
    if (ret != 0) {
        return ret;
    }

    // Apply trend analysis if we have enough data
    if (recent_temps && temp_count >= 3) {
        float trend_factor;
        ret = temp_compensation_calculate_trend(recent_temps, temp_count, &trend_factor);
        if (ret == 0) {
            // Apply trend factor to the compensation factor
            result->compensation_factor *= trend_factor;
            
            // Ensure the result is still within configured bounds
            if (result->compensation_factor < channel->temp_compensation.min_factor) {
                result->compensation_factor = channel->temp_compensation.min_factor;
            } else if (result->compensation_factor > channel->temp_compensation.max_factor) {
                result->compensation_factor = channel->temp_compensation.max_factor;
            }
            
        LOG_DBG("Applied temperature trend to channel %u: trend_factor=%.3f, final_factor=%.3f",
            channel_id, (double)trend_factor, (double)result->compensation_factor);
        } else {
            LOG_WRN("Temperature trend calculation failed, using basic compensation");
        }
    }

    return 0;
}

/**
 * @brief Initialize the temperature compensation integration system
 * 
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t temperature_compensation_integration_init(void)
{
    LOG_INF("Temperature compensation integration system initialized");
    return WATERING_SUCCESS;
}