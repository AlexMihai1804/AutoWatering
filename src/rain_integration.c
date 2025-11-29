#include "rain_integration.h"
#include "rain_history.h"
#include "rain_sensor.h"
#include "rain_config.h"
#include "nvs_config.h"
#include "watering.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

LOG_MODULE_REGISTER(rain_integration, LOG_LEVEL_INF);

/* Internal state structure - no global config, uses per-channel settings only */
static struct {
    bool initialized;
    struct k_mutex mutex;
    uint32_t last_calculation_time;
    rain_irrigation_impact_t last_impact[WATERING_CHANNELS_COUNT];
} rain_integration_state = {
    .initialized = false,
    .last_calculation_time = 0
};

/* Forward declarations */
static float calculate_reduction_curve(float rainfall_mm, float sensitivity_pct);
static float get_soil_infiltration_factor(uint8_t channel_id);
static uint8_t calculate_confidence_level(float rainfall_mm, uint32_t data_age);
void bt_irrigation_rain_config_notify(void);

/**
 * @brief Calculate irrigation reduction using exponential curve
 */
static float calculate_reduction_curve(float rainfall_mm, float sensitivity_pct)
{
    if (rainfall_mm <= 0.0f) {
        return 0.0f;
    }
    
    /* Exponential reduction curve: reduction = (1 - e^(-k*rainfall)) * sensitivity */
    float k = 0.2f; /* Curve steepness factor */
    float reduction = (1.0f - expf(-k * rainfall_mm)) * (sensitivity_pct / 100.0f);
    
    /* Cap at 100% reduction */
    if (reduction > 1.0f) {
        reduction = 1.0f;
    }
    
    return reduction * 100.0f; /* Return as percentage */
}

/**
 * @brief Get soil infiltration factor based on channel configuration
 */
static float get_soil_infiltration_factor(uint8_t channel_id)
{
    /* Use per-channel reduction_factor if available, otherwise default */
    watering_channel_t *channel = NULL;
    if (watering_get_channel(channel_id, &channel) == WATERING_SUCCESS && channel != NULL) {
        if (channel->rain_compensation.enabled && channel->rain_compensation.reduction_factor > 0.0f) {
            return channel->rain_compensation.reduction_factor;
        }
    }
    /* Default infiltration efficiency */
    return 0.8f;
}

/**
 * @brief Calculate confidence level based on data quality and age
 */
static uint8_t calculate_confidence_level(float rainfall_mm, uint32_t data_age)
{
    uint8_t confidence = 100;
    
    /* Reduce confidence for older data */
    if (data_age > 24 * 3600) { /* Older than 24 hours */
        confidence -= 20;
    } else if (data_age > 12 * 3600) { /* Older than 12 hours */
        confidence -= 10;
    }
    
    /* Reduce confidence for very small amounts (measurement uncertainty) */
    if (rainfall_mm < 0.5f) {
        confidence -= 15;
    }
    
    /* Ensure minimum confidence */
    if (confidence < 50) {
        confidence = 50;
    }
    
    return confidence;
}

watering_error_t rain_integration_init(void)
{
    if (rain_integration_state.initialized) {
        return WATERING_SUCCESS;
    }
    
    LOG_INF("Initializing rain integration system (per-channel config only)");
    
    /* Initialize mutex */
    k_mutex_init(&rain_integration_state.mutex);
    
    /* Initialize impact cache */
    memset(rain_integration_state.last_impact, 0, sizeof(rain_integration_state.last_impact));
    
    rain_integration_state.initialized = true;
    
    LOG_INF("Rain integration system initialized - using per-channel settings");
    
    return WATERING_SUCCESS;
}

watering_error_t rain_integration_deinit(void)
{
    if (!rain_integration_state.initialized) {
        return WATERING_SUCCESS;
    }
    
    rain_integration_state.initialized = false;
    
    LOG_INF("Rain integration system deinitialized");
    return WATERING_SUCCESS;
}

rain_irrigation_impact_t rain_integration_calculate_impact(uint8_t channel_id)
{
    rain_irrigation_impact_t impact = {0};
    
    if (!rain_integration_state.initialized || channel_id >= WATERING_CHANNELS_COUNT) {
        impact.confidence_level = 0;
        return impact;
    }
    
    k_mutex_lock(&rain_integration_state.mutex, K_FOREVER);
    
    /* Get channel-specific rain compensation settings */
    watering_channel_t *channel = NULL;
    watering_mode_t watering_mode = WATERING_BY_DURATION; /* default */
    
    if (watering_get_channel(channel_id, &channel) != WATERING_SUCCESS || channel == NULL) {
        k_mutex_unlock(&rain_integration_state.mutex);
        impact.confidence_level = 0;
        return impact;
    }
    
    /* Check if rain compensation is enabled for this channel */
    if (!channel->rain_compensation.enabled) {
        k_mutex_unlock(&rain_integration_state.mutex);
        impact.confidence_level = 100;
        return impact;
    }
    
    /* Use per-channel settings */
    float channel_skip_threshold = channel->rain_compensation.skip_threshold_mm;
    float channel_sensitivity = channel->rain_compensation.sensitivity * 100.0f; /* convert 0-1 to % */
    uint16_t channel_lookback = channel->rain_compensation.lookback_hours;
    watering_mode = channel->watering_event.watering_mode;
    
    /* Get recent rainfall data using channel-specific lookback */
    float recent_rainfall = rain_history_get_recent_total(channel_lookback);
    
    /* Calculate effective rainfall based on soil infiltration */
    float soil_factor = get_soil_infiltration_factor(channel_id);
    float effective_rainfall = recent_rainfall * soil_factor;
    
    /* Calculate irrigation reduction percentage using channel sensitivity */
    float reduction_pct = calculate_reduction_curve(effective_rainfall, channel_sensitivity);
    
    /* Determine if irrigation should be skipped completely:
     * Skip is ONLY applicable for TIME and VOLUME modes, NOT for FAO-56/AUTO modes.
     * FAO-56 modes handle rain compensation through their own calculations. */
    bool skip_irrigation = false;
    if (watering_mode == WATERING_BY_DURATION || watering_mode == WATERING_BY_VOLUME) {
        skip_irrigation = (recent_rainfall >= channel_skip_threshold);
    }
    /* For WATERING_AUTOMATIC_QUALITY and WATERING_AUTOMATIC_ECO: no skip, FAO-56 handles it */
    
    /* Calculate confidence level */
    uint32_t current_time = k_uptime_get_32() / 1000;
    uint32_t data_age = current_time - rain_integration_state.last_calculation_time;
    uint8_t confidence = calculate_confidence_level(recent_rainfall, data_age);
    
    /* Fill impact structure */
    impact.recent_rainfall_mm = recent_rainfall;
    impact.effective_rainfall_mm = effective_rainfall;
    impact.irrigation_reduction_pct = reduction_pct;
    impact.skip_irrigation = skip_irrigation;
    impact.calculation_time = current_time;
    impact.confidence_level = confidence;
    
    /* Cache the result */
    memcpy(&rain_integration_state.last_impact[channel_id], &impact, sizeof(impact));
    rain_integration_state.last_calculation_time = current_time;
    
    k_mutex_unlock(&rain_integration_state.mutex);
    
    LOG_DBG("Rain impact for channel %u: %.2f mm recent, %.2f mm effective, %.1f%% reduction, skip=%s",
            channel_id, (double)recent_rainfall, (double)effective_rainfall, (double)reduction_pct,
            skip_irrigation ? "yes" : "no");
    
    return impact;
}

watering_error_t rain_integration_adjust_task(uint8_t channel_id, watering_task_t *task)
{
    if (!rain_integration_state.initialized || !task || channel_id >= WATERING_CHANNELS_COUNT) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    /* Check if channel has rain compensation enabled */
    watering_channel_t *channel = NULL;
    if (watering_get_channel(channel_id, &channel) != WATERING_SUCCESS || channel == NULL) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    if (!channel->rain_compensation.enabled) {
        return WATERING_SUCCESS; /* No adjustment needed - compensation disabled for this channel */
    }
    
    /* Calculate rain impact */
    rain_irrigation_impact_t impact = rain_integration_calculate_impact(channel_id);
    
    /* Skip irrigation if threshold exceeded */
    if (impact.skip_irrigation) {
    LOG_INF("Skipping irrigation for channel %u due to recent rainfall (%.2f mm)",
        channel_id, (double)impact.recent_rainfall_mm);
        return WATERING_ERROR_BUSY; /* Use busy error to indicate skip */
    }
    
    /* Adjust task based on reduction percentage */
    if (impact.irrigation_reduction_pct > 0.0f) {
        float reduction_factor = 1.0f - (impact.irrigation_reduction_pct / 100.0f);
        
        if (task->channel->watering_event.watering_mode == WATERING_BY_DURATION) {
            uint32_t original_duration = task->by_time.start_time;
            task->by_time.start_time = (uint32_t)(original_duration * reduction_factor);
            
        LOG_INF("Reduced irrigation duration for channel %u: %u -> %u seconds (%.1f%% reduction)",
            channel_id, original_duration, task->by_time.start_time, (double)impact.irrigation_reduction_pct);
        } else if (task->channel->watering_event.watering_mode == WATERING_BY_VOLUME) {
            uint32_t original_volume = task->by_volume.volume_liters;
            task->by_volume.volume_liters = (uint32_t)(original_volume * reduction_factor);
            
        LOG_INF("Reduced irrigation volume for channel %u: %u -> %u ml (%.1f%% reduction)",
            channel_id, original_volume, task->by_volume.volume_liters, (double)impact.irrigation_reduction_pct);
        }
    }
    
    return WATERING_SUCCESS;
}

bool rain_integration_should_skip_irrigation(uint8_t channel_id)
{
    if (!rain_integration_state.initialized || channel_id >= WATERING_CHANNELS_COUNT) {
        return false;
    }
    
    /* Check if channel has rain compensation enabled */
    watering_channel_t *channel = NULL;
    if (watering_get_channel(channel_id, &channel) != WATERING_SUCCESS || channel == NULL) {
        return false;
    }
    
    if (!channel->rain_compensation.enabled) {
        return false;
    }
    
    rain_irrigation_impact_t impact = rain_integration_calculate_impact(channel_id);
    return impact.skip_irrigation;
}

float rain_integration_get_reduction_percentage(uint8_t channel_id)
{
    if (!rain_integration_state.initialized || channel_id >= WATERING_CHANNELS_COUNT) {
        return 0.0f;
    }
    
    /* Check if channel has rain compensation enabled */
    watering_channel_t *channel = NULL;
    if (watering_get_channel(channel_id, &channel) != WATERING_SUCCESS || channel == NULL) {
        return 0.0f;
    }
    
    if (!channel->rain_compensation.enabled) {
        return 0.0f;
    }
    
    rain_irrigation_impact_t impact = rain_integration_calculate_impact(channel_id);
    return impact.irrigation_reduction_pct;
}

/* ========== DEPRECATED GLOBAL CONFIG FUNCTIONS ========== */
/* These functions are kept for API compatibility but do nothing.
 * Rain compensation settings are now per-channel only.
 * Use watering_channel_t.rain_compensation instead. */

watering_error_t rain_integration_set_config(const rain_integration_config_t *config)
{
    (void)config;
    LOG_WRN("rain_integration_set_config() is deprecated - use per-channel settings");
    return WATERING_SUCCESS;
}

watering_error_t rain_integration_get_config(rain_integration_config_t *config)
{
    if (!config) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    /* Return default values for backwards compatibility */
    config->rain_sensitivity_pct = 75.0f;
    config->skip_threshold_mm = 5.0f;
    config->effective_rain_factor = 0.8f;
    config->lookback_hours = 48;
    config->integration_enabled = true; /* Always "enabled" - actual enable is per-channel */
    
    LOG_WRN("rain_integration_get_config() is deprecated - use per-channel settings");
    return WATERING_SUCCESS;
}

watering_error_t rain_integration_set_sensitivity(float sensitivity_pct)
{
    (void)sensitivity_pct;
    LOG_WRN("rain_integration_set_sensitivity() is deprecated - use per-channel settings");
    return WATERING_SUCCESS;
}

float rain_integration_get_sensitivity(void)
{
    LOG_WRN("rain_integration_get_sensitivity() is deprecated - use per-channel settings");
    return 75.0f; /* Default value */
}

watering_error_t rain_integration_set_skip_threshold(float threshold_mm)
{
    (void)threshold_mm;
    LOG_WRN("rain_integration_set_skip_threshold() is deprecated - use per-channel settings");
    return WATERING_SUCCESS;
}

float rain_integration_get_skip_threshold(void)
{
    LOG_WRN("rain_integration_get_skip_threshold() is deprecated - use per-channel settings");
    return 5.0f; /* Default value */
}

watering_error_t rain_integration_set_enabled(bool enabled)
{
    (void)enabled;
    LOG_WRN("rain_integration_set_enabled() is deprecated - use per-channel settings");
    return WATERING_SUCCESS;
}

bool rain_integration_is_enabled(void)
{
    if (!rain_integration_state.initialized) {
        return false;
    }
    
    /* Return true if any channel has rain compensation enabled */
    for (uint8_t i = 0; i < WATERING_CHANNELS_COUNT; i++) {
        watering_channel_t *channel = NULL;
        if (watering_get_channel(i, &channel) == WATERING_SUCCESS && channel != NULL) {
            if (channel->rain_compensation.enabled) {
                return true;
            }
        }
    }
    return false;
}

float rain_integration_calculate_effective_rainfall(float rainfall_mm, uint8_t channel_id)
{
    if (!rain_integration_state.initialized || channel_id >= WATERING_CHANNELS_COUNT) {
        return rainfall_mm; /* Return original if not initialized */
    }
    
    float soil_factor = get_soil_infiltration_factor(channel_id);
    return rainfall_mm * soil_factor;
}

/* Deprecated - global config no longer used */
watering_error_t rain_integration_save_config(void)
{
    LOG_WRN("rain_integration_save_config() is deprecated - rain config is per-channel only");
    return WATERING_SUCCESS;
}

/* Deprecated - global config no longer used */
watering_error_t rain_integration_load_config(void)
{
    LOG_WRN("rain_integration_load_config() is deprecated - rain config is per-channel only");
    return WATERING_SUCCESS;
}

watering_error_t rain_integration_validate_config(const rain_integration_config_t *config)
{
    if (!config) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    if (config->rain_sensitivity_pct < 0.0f || config->rain_sensitivity_pct > 100.0f) {
    LOG_ERR("Invalid rain sensitivity: %.1f%% (range: 0-100%%)", (double)config->rain_sensitivity_pct);
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    if (config->skip_threshold_mm < 0.0f || config->skip_threshold_mm > 100.0f) {
    LOG_ERR("Invalid skip threshold: %.1f mm (range: 0-100mm)", (double)config->skip_threshold_mm);
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    if (config->effective_rain_factor < 0.0f || config->effective_rain_factor > 1.0f) {
    LOG_ERR("Invalid effective rain factor: %.2f (range: 0.0-1.0)", (double)config->effective_rain_factor);
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    if (config->lookback_hours < 1 || config->lookback_hours > 168) { /* Max 1 week */
        LOG_ERR("Invalid lookback hours: %u (range: 1-168)", config->lookback_hours);
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    return WATERING_SUCCESS;
}

watering_error_t rain_integration_reset_config(void)
{
    LOG_WRN("rain_integration_reset_config() is deprecated - rain config is per-channel only");
    return WATERING_SUCCESS;
}

void rain_integration_debug_info(void)
{
    if (!rain_integration_state.initialized) {
        printk("Rain integration not initialized\n");
        return;
    }
    
    printk("=== Rain Integration Debug Info (Per-Channel Mode) ===\n");
    printk("Initialized: Yes\n");
    
    /* Show recent rainfall data */
    float recent_24h = rain_history_get_last_24h();
    float recent_48h = rain_history_get_recent_total(48);
    printk("Recent rainfall (24h): %.2f mm\n", (double)recent_24h);
    printk("Recent rainfall (48h): %.2f mm\n", (double)recent_48h);
    
    /* Show per-channel settings and impact */
    for (int i = 0; i < WATERING_CHANNELS_COUNT; i++) {
        watering_channel_t *channel = NULL;
        if (watering_get_channel(i, &channel) == WATERING_SUCCESS && channel != NULL) {
            if (channel->rain_compensation.enabled) {
                printk("Channel %d: ENABLED - sensitivity=%.1f, threshold=%.1fmm, lookback=%uh\n",
                       i, (double)channel->rain_compensation.sensitivity,
                       (double)channel->rain_compensation.skip_threshold_mm,
                       channel->rain_compensation.lookback_hours);
                rain_irrigation_impact_t impact = rain_integration_calculate_impact(i);
                printk("  -> %.1f%% reduction, skip=%s\n",
                       (double)impact.irrigation_reduction_pct, impact.skip_irrigation ? "yes" : "no");
            } else {
                printk("Channel %d: DISABLED\n", i);
            }
        }
    }
    
    printk("===================================\n");
}

rain_irrigation_impact_t rain_integration_test_calculation(float rainfall_mm, uint8_t channel_id)
{
    rain_irrigation_impact_t impact = {0};
    
    if (!rain_integration_state.initialized || channel_id >= WATERING_CHANNELS_COUNT) {
        return impact;
    }
    
    /* Get channel-specific settings */
    watering_channel_t *channel = NULL;
    float channel_sensitivity = 75.0f; /* default */
    float channel_skip_threshold = 5.0f; /* default */
    
    if (watering_get_channel(channel_id, &channel) == WATERING_SUCCESS && channel != NULL) {
        if (channel->rain_compensation.enabled) {
            channel_sensitivity = channel->rain_compensation.sensitivity * 100.0f;
            channel_skip_threshold = channel->rain_compensation.skip_threshold_mm;
        }
    }
    
    /* Simulate calculation with provided rainfall */
    float soil_factor = get_soil_infiltration_factor(channel_id);
    float effective_rainfall = rainfall_mm * soil_factor;
    
    float reduction_pct = calculate_reduction_curve(effective_rainfall, channel_sensitivity);
    
    bool skip_irrigation = (rainfall_mm >= channel_skip_threshold);
    
    impact.recent_rainfall_mm = rainfall_mm;
    impact.effective_rainfall_mm = effective_rainfall;
    impact.irrigation_reduction_pct = reduction_pct;
    impact.skip_irrigation = skip_irrigation;
    impact.calculation_time = k_uptime_get_32() / 1000;
    impact.confidence_level = 100; /* Test data is always high confidence */
    
    return impact;
}/**
 
* @brief Enhanced error handling and diagnostics for rain integration
 */

/* Integration error tracking */
typedef enum {
    RAIN_INTEGRATION_ERROR_NONE = 0,
    RAIN_INTEGRATION_ERROR_SENSOR_UNAVAILABLE = 1,
    RAIN_INTEGRATION_ERROR_HISTORY_CORRUPT = 2,
    RAIN_INTEGRATION_ERROR_CONFIG_INVALID = 3,
    RAIN_INTEGRATION_ERROR_CALCULATION_FAILED = 4
} rain_integration_error_t;

static struct {
    rain_integration_error_t last_error;
    uint32_t error_count;
    uint32_t last_error_time;
    uint32_t successful_calculations;
    uint32_t failed_calculations;
    float calculation_success_rate;
} integration_diagnostics = {0};

/**
 * @brief Validate rain integration configuration
 */
static bool validate_integration_config(const rain_integration_config_t *config)
{
    if (!config) {
        return false;
    }
    
    /* Check sensitivity range */
    if (config->rain_sensitivity_pct < 0.0f || config->rain_sensitivity_pct > 100.0f) {
        LOG_ERR("Invalid rain sensitivity: %.1f%% (valid range: 0-100%%)", 
                (double)config->rain_sensitivity_pct);
        return false;
    }
    
    /* Check skip threshold */
    if (config->skip_threshold_mm < 0.0f || config->skip_threshold_mm > 100.0f) {
        LOG_ERR("Invalid skip threshold: %.1f mm (valid range: 0-100mm)", 
                (double)config->skip_threshold_mm);
        return false;
    }
    
    /* Check effective rain factor */
    if (config->effective_rain_factor < 0.0f || config->effective_rain_factor > 1.0f) {
        LOG_ERR("Invalid effective rain factor: %.2f (valid range: 0.0-1.0)", 
                (double)config->effective_rain_factor);
        return false;
    }
    
    /* Check lookback hours */
    if (config->lookback_hours < 1 || config->lookback_hours > 168) {
        LOG_ERR("Invalid lookback hours: %u (valid range: 1-168)", config->lookback_hours);
        return false;
    }
    
    return true;
}

/**
 * @brief Log integration error with diagnostics
 */
static void log_integration_error(rain_integration_error_t error, const char *description)
{
    integration_diagnostics.last_error = error;
    integration_diagnostics.error_count++;
    integration_diagnostics.last_error_time = k_uptime_get_32() / 1000;
    integration_diagnostics.failed_calculations++;
    
    /* Update success rate */
    uint32_t total_calculations = integration_diagnostics.successful_calculations + 
                                 integration_diagnostics.failed_calculations;
    if (total_calculations > 0) {
        integration_diagnostics.calculation_success_rate = 
            (float)integration_diagnostics.successful_calculations / total_calculations * 100.0f;
    }
    
    LOG_ERR("Rain integration error: %s", description);
    
    /* Report through BLE if available */
    #ifdef CONFIG_BT
    bt_irrigation_rain_config_notify();
    #endif
}

/**
 * @brief Enhanced impact calculation with error handling
 */
rain_irrigation_impact_t rain_integration_calculate_impact_enhanced(uint8_t channel_id)
{
    rain_irrigation_impact_t impact = {0};
    
    if (!rain_integration_state.initialized || channel_id >= WATERING_CHANNELS_COUNT) {
        log_integration_error(RAIN_INTEGRATION_ERROR_CONFIG_INVALID, "Invalid parameters");
        impact.confidence_level = 0;
        return impact;
    }
    
    /* Get channel-specific settings */
    watering_channel_t *channel = NULL;
    if (watering_get_channel(channel_id, &channel) != WATERING_SUCCESS || channel == NULL) {
        log_integration_error(RAIN_INTEGRATION_ERROR_CONFIG_INVALID, "Channel not found");
        impact.confidence_level = 0;
        return impact;
    }
    
    /* Check if rain compensation is enabled for this channel */
    if (!channel->rain_compensation.enabled) {
        impact.confidence_level = 100;
        integration_diagnostics.successful_calculations++;
        return impact;
    }
    
    /* Check if rain sensor is available */
    if (!rain_sensor_is_active()) {
        log_integration_error(RAIN_INTEGRATION_ERROR_SENSOR_UNAVAILABLE, "Rain sensor not active");
        impact.confidence_level = 0;
        return impact;
    }
    
    /* Check for critical sensor health */
    if (rain_sensor_is_health_critical()) {
        log_integration_error(RAIN_INTEGRATION_ERROR_SENSOR_UNAVAILABLE, "Rain sensor health critical");
        impact.confidence_level = 25; /* Low confidence but still provide some data */
    }
    
    k_mutex_lock(&rain_integration_state.mutex, K_FOREVER);
    
    /* Use per-channel settings */
    float channel_sensitivity = channel->rain_compensation.sensitivity * 100.0f;
    float channel_skip_threshold = channel->rain_compensation.skip_threshold_mm;
    uint16_t channel_lookback = channel->rain_compensation.lookback_hours;
    
    /* Get recent rainfall data with validation */
    float recent_rainfall = rain_history_get_recent_total(channel_lookback);
    
    /* Validate rainfall data */
    if (recent_rainfall < 0.0f || recent_rainfall > 500.0f) {
        log_integration_error(RAIN_INTEGRATION_ERROR_HISTORY_CORRUPT, "Invalid rainfall data");
        k_mutex_unlock(&rain_integration_state.mutex);
        impact.confidence_level = 0;
        return impact;
    }
    
    /* Calculate effective rainfall based on soil infiltration */
    float soil_factor = get_soil_infiltration_factor(channel_id);
    if (soil_factor <= 0.0f || soil_factor > 1.0f) {
    LOG_WRN("Invalid soil factor %.2f for channel %u, using default", (double)soil_factor, channel_id);
        soil_factor = 0.8f; /* Default infiltration factor */
    }
    
    float effective_rainfall = recent_rainfall * soil_factor;
    
    /* Calculate irrigation reduction percentage */
    float reduction_pct = calculate_reduction_curve(effective_rainfall, channel_sensitivity);
    
    /* Validate reduction percentage */
    if (reduction_pct < 0.0f || reduction_pct > 100.0f) {
        log_integration_error(RAIN_INTEGRATION_ERROR_CALCULATION_FAILED, "Invalid reduction calculation");
        k_mutex_unlock(&rain_integration_state.mutex);
        impact.confidence_level = 0;
        return impact;
    }
    
    /* Determine if irrigation should be skipped completely */
    bool skip_irrigation = (recent_rainfall >= channel_skip_threshold);
    
    /* Calculate confidence level based on data quality */
    uint32_t current_time = k_uptime_get_32() / 1000;
    uint32_t data_age = current_time - rain_integration_state.last_calculation_time;
    uint8_t confidence = calculate_confidence_level(recent_rainfall, data_age);
    
    /* Adjust confidence based on sensor health */
    float sensor_accuracy = rain_sensor_get_pulse_accuracy();
    if (sensor_accuracy < 90.0f) {
        confidence = (uint8_t)(confidence * (sensor_accuracy / 100.0f));
    }
    
    /* Fill impact structure */
    impact.recent_rainfall_mm = recent_rainfall;
    impact.effective_rainfall_mm = effective_rainfall;
    impact.irrigation_reduction_pct = reduction_pct;
    impact.skip_irrigation = skip_irrigation;
    impact.calculation_time = current_time;
    impact.confidence_level = confidence;
    
    /* Cache the result */
    memcpy(&rain_integration_state.last_impact[channel_id], &impact, sizeof(impact));
    rain_integration_state.last_calculation_time = current_time;
    
    k_mutex_unlock(&rain_integration_state.mutex);
    
    /* Update success statistics */
    integration_diagnostics.successful_calculations++;
    uint32_t total_calculations = integration_diagnostics.successful_calculations + 
                                 integration_diagnostics.failed_calculations;
    if (total_calculations > 0) {
        integration_diagnostics.calculation_success_rate = 
            (float)integration_diagnostics.successful_calculations / total_calculations * 100.0f;
    }
    
    LOG_DBG("Rain impact for channel %u: %.2f mm recent, %.2f mm effective, %.1f%% reduction, skip=%s, confidence=%u%%",
            channel_id, (double)recent_rainfall, (double)effective_rainfall, (double)reduction_pct,
            skip_irrigation ? "yes" : "no", confidence);
    
    return impact;
}

/**
 * @brief Get integration diagnostic information
 */
int rain_integration_get_diagnostics(char *buffer, uint16_t buffer_size)
{
    if (!buffer || buffer_size < 300) {
        return -1;
    }
    
    int written = 0;
    uint32_t current_time = k_uptime_get_32() / 1000;
    
    written += snprintf(buffer + written, buffer_size - written,
                       "=== Rain Integration Diagnostics (Per-Channel Mode) ===\n");
    
    written += snprintf(buffer + written, buffer_size - written,
                       "Initialized: %s\n", rain_integration_state.initialized ? "Yes" : "No");
    
    /* Count channels with rain compensation enabled */
    int enabled_channels = 0;
    for (uint8_t i = 0; i < WATERING_CHANNELS_COUNT; i++) {
        watering_channel_t *channel = NULL;
        if (watering_get_channel(i, &channel) == WATERING_SUCCESS && channel != NULL) {
            if (channel->rain_compensation.enabled) {
                enabled_channels++;
            }
        }
    }
    
    written += snprintf(buffer + written, buffer_size - written,
                       "Channels with rain compensation: %d/%d\n", enabled_channels, WATERING_CHANNELS_COUNT);
    
    written += snprintf(buffer + written, buffer_size - written,
                       "Last Error: %d (%us ago)\n", integration_diagnostics.last_error,
                       integration_diagnostics.last_error_time > 0 ? 
                       current_time - integration_diagnostics.last_error_time : 0);
    
    written += snprintf(buffer + written, buffer_size - written,
                       "Total Errors: %u\n", integration_diagnostics.error_count);
    
    written += snprintf(buffer + written, buffer_size - written,
                       "Calculations: %u successful, %u failed (%.1f%% success rate)\n",
                       integration_diagnostics.successful_calculations,
                       integration_diagnostics.failed_calculations,
                       (double)integration_diagnostics.calculation_success_rate);
    
    written += snprintf(buffer + written, buffer_size - written,
                       "====================================\n");
    
    return written;
}

/**
 * @brief Reset integration diagnostics
 */
void rain_integration_reset_diagnostics(void)
{
    memset(&integration_diagnostics, 0, sizeof(integration_diagnostics));
    LOG_INF("Rain integration diagnostics reset");
}

/**
 * @brief Periodic health check for rain integration
 */
void rain_integration_periodic_health_check(void)
{
    static uint32_t last_health_check = 0;
    uint32_t current_time = k_uptime_get_32() / 1000;
    
    /* Run health check every 15 minutes */
    if ((current_time - last_health_check) < 900) {
        return;
    }
    
    last_health_check = current_time;
    
    if (!rain_integration_state.initialized) {
        return;
    }
    
    /* Check calculation success rate */
    if (integration_diagnostics.calculation_success_rate < 80.0f && 
        (integration_diagnostics.successful_calculations + integration_diagnostics.failed_calculations) > 10) {
    LOG_WRN("Rain integration calculation success rate low: %.1f%%", 
        (double)integration_diagnostics.calculation_success_rate);
    }
    
    /* Check sensor availability if any channel has rain compensation enabled */
    bool any_enabled = rain_integration_is_enabled();
    if (any_enabled && !rain_sensor_is_active()) {
        LOG_WRN("Rain compensation enabled on some channels but sensor not active");
    }
    
    LOG_DBG("Rain integration health check completed - success rate: %.1f%%",
            (double)integration_diagnostics.calculation_success_rate);
}