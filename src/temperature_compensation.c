#include "temperature_compensation.h"
#include <zephyr/logging/log.h>
#include <math.h>
#include <string.h>
#include <stdio.h>

LOG_MODULE_REGISTER(temp_compensation, LOG_LEVEL_DBG);

int temp_compensation_init_config(temperature_compensation_config_t *config)
{
    if (!config) {
        LOG_ERR("Invalid config pointer");
        return -EINVAL;
    }

    memset(config, 0, sizeof(temperature_compensation_config_t));
    
    config->enabled = false;
    config->base_temperature = TEMP_COMP_DEFAULT_BASE_TEMP;
    config->sensitivity = TEMP_COMP_DEFAULT_SENSITIVITY;
    config->min_factor = TEMP_COMP_DEFAULT_MIN_FACTOR;
    config->max_factor = TEMP_COMP_DEFAULT_MAX_FACTOR;

    LOG_DBG("Temperature compensation config initialized with defaults");
    return 0;
}

int temp_compensation_validate_config(const temperature_compensation_config_t *config)
{
    if (!config) {
        LOG_ERR("Invalid config pointer");
        return -EINVAL;
    }

    if (config->base_temperature < TEMP_COMP_MIN_TEMP_C || 
        config->base_temperature > TEMP_COMP_MAX_TEMP_C) {
    LOG_ERR("Invalid base temperature: %.1f°C", (double)config->base_temperature);
        return -EINVAL;
    }

    if (config->sensitivity < TEMP_COMP_MIN_SENSITIVITY || 
        config->sensitivity > TEMP_COMP_MAX_SENSITIVITY) {
    LOG_ERR("Invalid sensitivity: %.3f", (double)config->sensitivity);
        return -EINVAL;
    }

    if (config->min_factor <= 0.0f || config->min_factor >= config->max_factor) {
    LOG_ERR("Invalid factor range: min=%.2f, max=%.2f", 
        (double)config->min_factor, (double)config->max_factor);
        return -EINVAL;
    }

    if (config->max_factor > 5.0f) {
    LOG_ERR("Maximum factor too high: %.2f", (double)config->max_factor);
        return -EINVAL;
    }

    return 0;
}

int temp_compensation_calculate(const temperature_compensation_config_t *config,
                               float current_temp,
                               temperature_compensation_result_t *result)
{
    if (!config || !result) {
        LOG_ERR("Invalid parameters");
        return -EINVAL;
    }

    // Validate configuration
    int ret = temp_compensation_validate_config(config);
    if (ret != 0) {
        return ret;
    }

    // Check if compensation is enabled
    if (!config->enabled) {
        result->current_temperature = current_temp;
        result->compensation_factor = 1.0f;
        result->adjusted_requirement = 0.0f; // Will be set by caller
        result->calculation_timestamp = k_uptime_get_32();
        LOG_DBG("Temperature compensation disabled, factor=1.0");
        return 0;
    }

    // Validate temperature reading
    if (!temp_compensation_is_temp_valid(current_temp)) {
    LOG_ERR("Invalid temperature reading: %.1f°C", (double)current_temp);
        return -EINVAL;
    }

    // Calculate temperature difference from base
    float temp_diff = current_temp - config->base_temperature;
    
    // Calculate compensation factor
    float factor = temp_compensation_get_factor(config, temp_diff);

    // Fill result structure
    result->current_temperature = current_temp;
    result->compensation_factor = factor;
    result->adjusted_requirement = 0.0f; // Will be set by caller
    result->calculation_timestamp = k_uptime_get_32();

    LOG_DBG("Temperature compensation: %.1f°C, diff=%.1f°C, factor=%.3f",
            (double)current_temp, (double)temp_diff, (double)factor);

    return 0;
}

int temp_compensation_apply(uint32_t base_requirement,
                           float compensation_factor,
                           uint32_t *compensated_requirement)
{
    if (!compensated_requirement) {
        LOG_ERR("Invalid compensated_requirement pointer");
        return -EINVAL;
    }

    if (compensation_factor <= 0.0f) {
    LOG_ERR("Invalid compensation factor: %.3f", (double)compensation_factor);
        return -EINVAL;
    }

    // Apply compensation factor
    float compensated = (float)base_requirement * compensation_factor;
    
    // Ensure result is within reasonable bounds
    if (compensated < 1.0f) {
        compensated = 1.0f;
    } else if (compensated > UINT32_MAX) {
        compensated = UINT32_MAX;
    }

    *compensated_requirement = (uint32_t)compensated;

    LOG_DBG("Applied temperature compensation: %u -> %u (factor=%.3f)",
            base_requirement, *compensated_requirement, (double)compensation_factor);

    return 0;
}

int temp_compensation_calculate_et0(const temperature_compensation_config_t *config,
                                   float current_temp,
                                   float base_et0,
                                   float *compensated_et0)
{
    if (!config || !compensated_et0) {
        LOG_ERR("Invalid parameters");
        return -EINVAL;
    }

    if (base_et0 < 0.0f) {
    LOG_ERR("Invalid base ET0: %.3f", (double)base_et0);
        return -EINVAL;
    }

    // Calculate temperature compensation result
    temperature_compensation_result_t result;
    int ret = temp_compensation_calculate(config, current_temp, &result);
    if (ret != 0) {
        return ret;
    }

    // Apply compensation to ET0
    *compensated_et0 = base_et0 * result.compensation_factor;

    LOG_DBG("ET0 temperature compensation: %.3f -> %.3f mm/day (factor=%.3f)",
            (double)base_et0, (double)*compensated_et0, (double)result.compensation_factor);

    return 0;
}

float temp_compensation_get_factor(const temperature_compensation_config_t *config,
                                  float temp_diff)
{
    if (!config) {
        return 1.0f;
    }

    // Calculate linear compensation factor
    // factor = 1.0 + (temp_diff * sensitivity)
    float factor = 1.0f + (temp_diff * config->sensitivity);

    // Clamp to configured limits
    if (factor < config->min_factor) {
        factor = config->min_factor;
    } else if (factor > config->max_factor) {
        factor = config->max_factor;
    }

    return factor;
}

bool temp_compensation_is_temp_valid(float temperature)
{
    return (temperature >= TEMP_COMP_MIN_TEMP_C && 
            temperature <= TEMP_COMP_MAX_TEMP_C &&
            !isnan(temperature) && 
            !isinf(temperature));
}

int temp_compensation_get_description(float compensation_factor, char *description)
{
    if (!description) {
        LOG_ERR("Invalid description pointer");
        return -EINVAL;
    }

    if (compensation_factor < 0.95f) {
        double pct = (double)(compensation_factor * 100.0f);
        snprintf(description, 64, "Reduced watering (%.0f%% of normal)", pct);
    } else if (compensation_factor > 1.05f) {
        double pct = (double)(compensation_factor * 100.0f);
        snprintf(description, 64, "Increased watering (%.0f%% of normal)", pct);
    } else {
        snprintf(description, 64, "Normal watering (no temperature adjustment)");
    }

    return 0;
}

int temp_compensation_calculate_trend(const float *temps, uint8_t count, float *trend_factor)
{
    if (!temps || !trend_factor || count < 2) {
        LOG_ERR("Invalid parameters for trend calculation");
        return -EINVAL;
    }

    // Calculate simple linear trend over recent readings
    float sum_x = 0.0f, sum_y = 0.0f, sum_xy = 0.0f, sum_x2 = 0.0f;
    
    for (uint8_t i = 0; i < count; i++) {
        if (!temp_compensation_is_temp_valid(temps[i])) {
            LOG_WRN("Invalid temperature in trend calculation: %.1f°C", (double)temps[i]);
            continue;
        }
        
        float x = (float)i;
        float y = temps[i];
        
        sum_x += x;
        sum_y += y;
        sum_xy += x * y;
        sum_x2 += x * x;
    }

    // Calculate slope (trend)
    float n = (float)count;
    float slope = (n * sum_xy - sum_x * sum_y) / (n * sum_x2 - sum_x * sum_x);
    
    // Convert slope to trend factor (small adjustment based on trend)
    // Positive slope (warming) = slight increase in watering
    // Negative slope (cooling) = slight decrease in watering
    *trend_factor = 1.0f + (slope * 0.01f); // Very small trend influence
    
    // Clamp trend factor to reasonable bounds
    if (*trend_factor < 0.9f) {
        *trend_factor = 0.9f;
    } else if (*trend_factor > 1.1f) {
        *trend_factor = 1.1f;
    }

    LOG_DBG("Temperature trend: slope=%.3f°C/reading, factor=%.3f", (double)slope, (double)*trend_factor);
    return 0;
}

int temp_compensation_update_config(temperature_compensation_config_t *config,
                                   float base_temp,
                                   float sensitivity,
                                   float min_factor,
                                   float max_factor)
{
    if (!config) {
        LOG_ERR("Invalid config pointer");
        return -EINVAL;
    }

    // Create temporary config for validation
    temperature_compensation_config_t temp_config = *config;
    temp_config.base_temperature = base_temp;
    temp_config.sensitivity = sensitivity;
    temp_config.min_factor = min_factor;
    temp_config.max_factor = max_factor;

    // Validate new configuration
    int ret = temp_compensation_validate_config(&temp_config);
    if (ret != 0) {
        LOG_ERR("Invalid temperature compensation configuration");
        return ret;
    }

    // Update configuration
    config->base_temperature = base_temp;
    config->sensitivity = sensitivity;
    config->min_factor = min_factor;
    config->max_factor = max_factor;

    LOG_INF("Temperature compensation config updated: base=%.1f°C, sens=%.3f, range=[%.2f,%.2f]",
            (double)base_temp, (double)sensitivity, (double)min_factor, (double)max_factor);

    return 0;
}

/**
 * @brief Initialize the temperature compensation system
 * 
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t temperature_compensation_init(void)
{
    LOG_INF("Temperature compensation system initialized");
    return WATERING_SUCCESS;
}