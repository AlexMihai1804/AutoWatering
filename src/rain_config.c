#include "rain_config.h"
#include "nvs_config.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>

LOG_MODULE_REGISTER(rain_config, LOG_LEVEL_INF);

/* Validation constants */
#define MIN_MM_PER_PULSE        0.1f
#define MAX_MM_PER_PULSE        10.0f
#define MIN_DEBOUNCE_MS         10
#define MAX_DEBOUNCE_MS         1000
#define MIN_SENSITIVITY_PCT     0.0f
#define MAX_SENSITIVITY_PCT     100.0f
#define MIN_SKIP_THRESHOLD_MM   0.0f
#define MAX_SKIP_THRESHOLD_MM   50.0f

int rain_config_save(const rain_nvs_config_t *config)
{
    if (!config) {
        LOG_ERR("Invalid config pointer");
        return -EINVAL;
    }
    
    int ret = rain_config_validate(config);
    if (ret != 0) {
        LOG_ERR("Invalid configuration, not saving");
        return ret;
    }
    
    /* Use nvs_save_rain_config which also sets the onboarding flag */
    ret = nvs_save_rain_config(config);
    if (ret < 0) {
        LOG_ERR("Failed to save rain config to NVS: %d", ret);
        return ret;
    }
    
    LOG_INF("Rain sensor configuration saved to NVS");
    LOG_DBG("Calibration: %.3f mm/pulse, Debounce: %u ms", 
            (double)config->mm_per_pulse, config->debounce_ms);
    LOG_DBG("Sensitivity: %.1f%%, Skip threshold: %.1f mm", 
            (double)config->rain_sensitivity_pct, (double)config->skip_threshold_mm);
    
    return 0;
}

int rain_config_load(rain_nvs_config_t *config)
{
    if (!config) {
        LOG_ERR("Invalid config pointer");
        return -EINVAL;
    }
    
    int ret = nvs_config_read(NVS_RAIN_CONFIG_ID, config, sizeof(rain_nvs_config_t));
    if (ret < 0) {
        LOG_WRN("Failed to load rain config from NVS: %d, using defaults", ret);
        rain_config_get_default(config);
        return ret;
    }
    
    /* Validate loaded configuration */
    ret = rain_config_validate(config);
    if (ret != 0) {
        LOG_WRN("Loaded rain config is invalid, using defaults");
        rain_config_get_default(config);
        return ret;
    }
    
    LOG_INF("Rain sensor configuration loaded from NVS");
    LOG_DBG("Calibration: %.3f mm/pulse, Debounce: %u ms", 
            (double)config->mm_per_pulse, config->debounce_ms);
    
    return 0;
}

int rain_state_save(const rain_nvs_state_t *state)
{
    if (!state) {
        LOG_ERR("Invalid state pointer");
        return -EINVAL;
    }
    
    int ret = nvs_config_write(NVS_RAIN_STATE_ID, state, sizeof(rain_nvs_state_t));
    if (ret < 0) {
        LOG_ERR("Failed to save rain state to NVS: %d", ret);
        return ret;
    }
    
    LOG_DBG("Rain sensor state saved to NVS");
    LOG_DBG("Total pulses: %u, Current hour: %.2f mm", 
            state->total_pulses, (double)state->current_hour_mm);
    
    return 0;
}

int rain_state_load(rain_nvs_state_t *state)
{
    if (!state) {
        LOG_ERR("Invalid state pointer");
        return -EINVAL;
    }
    
    int ret = nvs_config_read(NVS_RAIN_STATE_ID, state, sizeof(rain_nvs_state_t));
    if (ret < 0) {
        LOG_WRN("Failed to load rain state from NVS: %d, using defaults", ret);
        rain_state_get_default(state);
        return ret;
    }
    
    LOG_INF("Rain sensor state loaded from NVS");
    LOG_DBG("Total pulses: %u, Today total: %.2f mm", 
            state->total_pulses, (double)state->today_total_mm);
    
    return 0;
}

int rain_config_validate(const rain_nvs_config_t *config)
{
    if (!config) {
        return -EINVAL;
    }
    
    /* Validate mm per pulse */
    if (config->mm_per_pulse < MIN_MM_PER_PULSE || 
        config->mm_per_pulse > MAX_MM_PER_PULSE) {
    LOG_ERR("Invalid mm_per_pulse: %.3f (range: %.1f-%.1f)", 
        (double)config->mm_per_pulse, (double)MIN_MM_PER_PULSE, (double)MAX_MM_PER_PULSE);
        return -EINVAL;
    }
    
    /* Validate debounce time */
    if (config->debounce_ms < MIN_DEBOUNCE_MS || 
        config->debounce_ms > MAX_DEBOUNCE_MS) {
        LOG_ERR("Invalid debounce_ms: %u (range: %u-%u)", 
                config->debounce_ms, MIN_DEBOUNCE_MS, MAX_DEBOUNCE_MS);
        return -EINVAL;
    }
    
    /* Validate sensitivity percentage */
    if (config->rain_sensitivity_pct < MIN_SENSITIVITY_PCT || 
        config->rain_sensitivity_pct > MAX_SENSITIVITY_PCT) {
    LOG_ERR("Invalid rain_sensitivity_pct: %.1f (range: %.1f-%.1f)", 
        (double)config->rain_sensitivity_pct, (double)MIN_SENSITIVITY_PCT, (double)MAX_SENSITIVITY_PCT);
        return -EINVAL;
    }
    
    /* Validate skip threshold */
    if (config->skip_threshold_mm < MIN_SKIP_THRESHOLD_MM || 
        config->skip_threshold_mm > MAX_SKIP_THRESHOLD_MM) {
    LOG_ERR("Invalid skip_threshold_mm: %.1f (range: %.1f-%.1f)", 
        (double)config->skip_threshold_mm, (double)MIN_SKIP_THRESHOLD_MM, (double)MAX_SKIP_THRESHOLD_MM);
        return -EINVAL;
    }
    
    /* Validate boolean fields */
    if (config->sensor_enabled > 1) {
        LOG_ERR("Invalid sensor_enabled: %u (must be 0 or 1)", config->sensor_enabled);
        return -EINVAL;
    }
    
    if (config->integration_enabled > 1) {
        LOG_ERR("Invalid integration_enabled: %u (must be 0 or 1)", config->integration_enabled);
        return -EINVAL;
    }
    
    return 0;
}

void rain_config_get_default(rain_nvs_config_t *config)
{
    if (!config) {
        return;
    }
    
    static const rain_nvs_config_t default_config = DEFAULT_RAIN_CONFIG;
    memcpy(config, &default_config, sizeof(rain_nvs_config_t));
    
    LOG_DBG("Using default rain sensor configuration");
}

void rain_state_get_default(rain_nvs_state_t *state)
{
    if (!state) {
        return;
    }
    
    static const rain_nvs_state_t default_state = DEFAULT_RAIN_STATE;
    memcpy(state, &default_state, sizeof(rain_nvs_state_t));
    
    /* Set current time for hour and day start */
    uint32_t current_time = k_uptime_get_32() / 1000;
    state->hour_start_time = current_time;
    state->day_start_time = current_time;
    
    LOG_DBG("Using default rain sensor state");
}

int rain_config_reset(void)
{
    rain_nvs_config_t default_config;
    rain_config_get_default(&default_config);
    
    int ret = rain_config_save(&default_config);
    if (ret != 0) {
        LOG_ERR("Failed to reset rain config: %d", ret);
        return ret;
    }
    
    LOG_INF("Rain sensor configuration reset to defaults");
    return 0;
}

int rain_state_reset(void)
{
    rain_nvs_state_t default_state;
    rain_state_get_default(&default_state);
    
    int ret = rain_state_save(&default_state);
    if (ret != 0) {
        LOG_ERR("Failed to reset rain state: %d", ret);
        return ret;
    }
    
    LOG_INF("Rain sensor state reset (counters cleared)");
    return 0;
}