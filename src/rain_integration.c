#include "rain_integration.h"
#include "rain_history.h"
#include "rain_sensor.h"
#include "rain_config.h"
#include "nvs_config.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

LOG_MODULE_REGISTER(rain_integration, LOG_LEVEL_INF);

/* NVS storage ID for rain integration config */
#define NVS_RAIN_INTEGRATION_CONFIG_ID  0x0184

/* Internal state structure */
static struct {
    rain_integration_config_t config;
    bool initialized;
    struct k_mutex mutex;
    uint32_t last_calculation_time;
    rain_irrigation_impact_t last_impact[WATERING_CHANNELS_COUNT];
} rain_integration_state = {
    .config = DEFAULT_RAIN_INTEGRATION_CONFIG,
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
    /* For now, use the global effective rain factor */
    /* In full implementation, this would look up soil type from channel config */
    return rain_integration_state.config.effective_rain_factor;
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
    
    LOG_INF("Initializing rain integration system");
    
    /* Initialize mutex */
    k_mutex_init(&rain_integration_state.mutex);
    
    /* Load configuration from NVS */
    watering_error_t ret = rain_integration_load_config();
    if (ret != WATERING_SUCCESS) {
        LOG_WRN("Failed to load rain integration config, using defaults");
    }
    
    /* Initialize impact cache */
    memset(rain_integration_state.last_impact, 0, sizeof(rain_integration_state.last_impact));
    
    rain_integration_state.initialized = true;
    
    LOG_INF("Rain integration system initialized");
    LOG_INF("Sensitivity: %.1f%%, Skip threshold: %.1f mm, Lookback: %u hours",
            (double)rain_integration_state.config.rain_sensitivity_pct,
            (double)rain_integration_state.config.skip_threshold_mm,
            rain_integration_state.config.lookback_hours);
    
    return WATERING_SUCCESS;
}

watering_error_t rain_integration_deinit(void)
{
    if (!rain_integration_state.initialized) {
        return WATERING_SUCCESS;
    }
    
    /* Save configuration to NVS */
    rain_integration_save_config();
    
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
    
    if (!rain_integration_state.config.integration_enabled) {
        impact.confidence_level = 100;
        return impact;
    }
    
    k_mutex_lock(&rain_integration_state.mutex, K_FOREVER);
    
    /* Get recent rainfall data */
    float recent_rainfall = rain_history_get_recent_total(rain_integration_state.config.lookback_hours);
    
    /* Calculate effective rainfall based on soil infiltration */
    float soil_factor = get_soil_infiltration_factor(channel_id);
    float effective_rainfall = recent_rainfall * soil_factor;
    
    /* Calculate irrigation reduction percentage */
    float reduction_pct = calculate_reduction_curve(effective_rainfall, 
                                                   rain_integration_state.config.rain_sensitivity_pct);
    
    /* Determine if irrigation should be skipped completely */
    bool skip_irrigation = (recent_rainfall >= rain_integration_state.config.skip_threshold_mm);
    
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
    
    if (!rain_integration_state.config.integration_enabled) {
        return WATERING_SUCCESS; /* No adjustment needed */
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
    
    if (!rain_integration_state.config.integration_enabled) {
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
    
    if (!rain_integration_state.config.integration_enabled) {
        return 0.0f;
    }
    
    rain_irrigation_impact_t impact = rain_integration_calculate_impact(channel_id);
    return impact.irrigation_reduction_pct;
}

watering_error_t rain_integration_set_config(const rain_integration_config_t *config)
{
    if (!rain_integration_state.initialized || !config) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    watering_error_t ret = rain_integration_validate_config(config);
    if (ret != WATERING_SUCCESS) {
        return ret;
    }
    
    k_mutex_lock(&rain_integration_state.mutex, K_FOREVER);
    memcpy(&rain_integration_state.config, config, sizeof(rain_integration_config_t));
    k_mutex_unlock(&rain_integration_state.mutex);
    
    LOG_INF("Rain integration configuration updated");
    return WATERING_SUCCESS;
}

watering_error_t rain_integration_get_config(rain_integration_config_t *config)
{
    if (!rain_integration_state.initialized || !config) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    k_mutex_lock(&rain_integration_state.mutex, K_FOREVER);
    memcpy(config, &rain_integration_state.config, sizeof(rain_integration_config_t));
    k_mutex_unlock(&rain_integration_state.mutex);
    
    return WATERING_SUCCESS;
}

watering_error_t rain_integration_set_sensitivity(float sensitivity_pct)
{
    if (!rain_integration_state.initialized) {
        return WATERING_ERROR_NOT_INITIALIZED;
    }
    
    if (sensitivity_pct < 0.0f || sensitivity_pct > 100.0f) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    k_mutex_lock(&rain_integration_state.mutex, K_FOREVER);
    rain_integration_state.config.rain_sensitivity_pct = sensitivity_pct;
    k_mutex_unlock(&rain_integration_state.mutex);
    
    LOG_INF("Rain sensitivity set to %.1f%%", (double)sensitivity_pct);
    return WATERING_SUCCESS;
}

float rain_integration_get_sensitivity(void)
{
    if (!rain_integration_state.initialized) {
        return 0.0f;
    }
    
    return rain_integration_state.config.rain_sensitivity_pct;
}

watering_error_t rain_integration_set_skip_threshold(float threshold_mm)
{
    if (!rain_integration_state.initialized) {
        return WATERING_ERROR_NOT_INITIALIZED;
    }
    
    if (threshold_mm < 0.0f || threshold_mm > 100.0f) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    k_mutex_lock(&rain_integration_state.mutex, K_FOREVER);
    rain_integration_state.config.skip_threshold_mm = threshold_mm;
    k_mutex_unlock(&rain_integration_state.mutex);
    
    LOG_INF("Rain skip threshold set to %.1f mm", (double)threshold_mm);
    return WATERING_SUCCESS;
}

float rain_integration_get_skip_threshold(void)
{
    if (!rain_integration_state.initialized) {
        return 0.0f;
    }
    
    return rain_integration_state.config.skip_threshold_mm;
}

watering_error_t rain_integration_set_enabled(bool enabled)
{
    if (!rain_integration_state.initialized) {
        return WATERING_ERROR_NOT_INITIALIZED;
    }
    
    k_mutex_lock(&rain_integration_state.mutex, K_FOREVER);
    rain_integration_state.config.integration_enabled = enabled;
    k_mutex_unlock(&rain_integration_state.mutex);
    
    LOG_INF("Rain integration %s", enabled ? "enabled" : "disabled");
    return WATERING_SUCCESS;
}

bool rain_integration_is_enabled(void)
{
    if (!rain_integration_state.initialized) {
        return false;
    }
    
    return rain_integration_state.config.integration_enabled;
}

float rain_integration_calculate_effective_rainfall(float rainfall_mm, uint8_t channel_id)
{
    if (!rain_integration_state.initialized || channel_id >= WATERING_CHANNELS_COUNT) {
        return rainfall_mm; /* Return original if not initialized */
    }
    
    float soil_factor = get_soil_infiltration_factor(channel_id);
    return rainfall_mm * soil_factor;
}

watering_error_t rain_integration_save_config(void)
{
    if (!rain_integration_state.initialized) {
        return WATERING_ERROR_NOT_INITIALIZED;
    }
    
    int ret = nvs_config_write(NVS_RAIN_INTEGRATION_CONFIG_ID, 
                              &rain_integration_state.config, 
                              sizeof(rain_integration_config_t));
    if (ret != 0) {
        LOG_ERR("Failed to save rain integration config to NVS: %d", ret);
        return WATERING_ERROR_STORAGE;
    }
    
    LOG_INF("Rain integration configuration saved to NVS");
    return WATERING_SUCCESS;
}

watering_error_t rain_integration_load_config(void)
{
    if (!rain_integration_state.initialized) {
        return WATERING_ERROR_NOT_INITIALIZED;
    }
    
    int ret = nvs_config_read(NVS_RAIN_INTEGRATION_CONFIG_ID, 
                             &rain_integration_state.config, 
                             sizeof(rain_integration_config_t));
    if (ret != 0) {
        LOG_WRN("Failed to load rain integration config from NVS: %d, using defaults", ret);
        rain_integration_state.config = (rain_integration_config_t)DEFAULT_RAIN_INTEGRATION_CONFIG;
        return WATERING_ERROR_STORAGE;
    }
    
    /* Validate loaded configuration */
    ret = rain_integration_validate_config(&rain_integration_state.config);
    if (ret != WATERING_SUCCESS) {
        LOG_WRN("Loaded rain integration config is invalid, using defaults");
        rain_integration_state.config = (rain_integration_config_t)DEFAULT_RAIN_INTEGRATION_CONFIG;
        return ret;
    }
    
    LOG_INF("Rain integration configuration loaded from NVS");
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
    if (!rain_integration_state.initialized) {
        return WATERING_ERROR_NOT_INITIALIZED;
    }
    
    k_mutex_lock(&rain_integration_state.mutex, K_FOREVER);
    rain_integration_state.config = (rain_integration_config_t)DEFAULT_RAIN_INTEGRATION_CONFIG;
    k_mutex_unlock(&rain_integration_state.mutex);
    
    LOG_INF("Rain integration configuration reset to defaults");
    return WATERING_SUCCESS;
}

void rain_integration_debug_info(void)
{
    if (!rain_integration_state.initialized) {
        printk("Rain integration not initialized\n");
        return;
    }
    
    printk("=== Rain Integration Debug Info ===\n");
    printk("Initialized: Yes\n");
    printk("Integration enabled: %s\n", rain_integration_state.config.integration_enabled ? "Yes" : "No");
    printk("Rain sensitivity: %.1f%%\n", (double)rain_integration_state.config.rain_sensitivity_pct);
    printk("Skip threshold: %.1f mm\n", (double)rain_integration_state.config.skip_threshold_mm);
    printk("Effective rain factor: %.2f\n", (double)rain_integration_state.config.effective_rain_factor);
    printk("Lookback hours: %u\n", rain_integration_state.config.lookback_hours);
    
    /* Show recent rainfall data */
    float recent_24h = rain_history_get_last_24h();
    float recent_48h = rain_history_get_recent_total(48);
    printk("Recent rainfall (24h): %.2f mm\n", (double)recent_24h);
    printk("Recent rainfall (48h): %.2f mm\n", (double)recent_48h);
    
    /* Show impact for each channel */
    for (int i = 0; i < WATERING_CHANNELS_COUNT; i++) {
        rain_irrigation_impact_t impact = rain_integration_calculate_impact(i);
    printk("Channel %d: %.1f%% reduction, skip=%s\n", 
           i, (double)impact.irrigation_reduction_pct, impact.skip_irrigation ? "yes" : "no");
    }
    
    printk("===================================\n");
}

rain_irrigation_impact_t rain_integration_test_calculation(float rainfall_mm, uint8_t channel_id)
{
    rain_irrigation_impact_t impact = {0};
    
    if (!rain_integration_state.initialized || channel_id >= WATERING_CHANNELS_COUNT) {
        return impact;
    }
    
    /* Simulate calculation with provided rainfall */
    float soil_factor = get_soil_infiltration_factor(channel_id);
    float effective_rainfall = rainfall_mm * soil_factor;
    
    float reduction_pct = calculate_reduction_curve(effective_rainfall, 
                                                   rain_integration_state.config.rain_sensitivity_pct);
    
    bool skip_irrigation = (rainfall_mm >= rain_integration_state.config.skip_threshold_mm);
    
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
    
    if (!rain_integration_state.config.integration_enabled) {
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
    
    /* Get recent rainfall data with validation */
    float recent_rainfall = rain_history_get_recent_total(rain_integration_state.config.lookback_hours);
    
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
    float reduction_pct = calculate_reduction_curve(effective_rainfall, 
                                                   rain_integration_state.config.rain_sensitivity_pct);
    
    /* Validate reduction percentage */
    if (reduction_pct < 0.0f || reduction_pct > 100.0f) {
        log_integration_error(RAIN_INTEGRATION_ERROR_CALCULATION_FAILED, "Invalid reduction calculation");
        k_mutex_unlock(&rain_integration_state.mutex);
        impact.confidence_level = 0;
        return impact;
    }
    
    /* Determine if irrigation should be skipped completely */
    bool skip_irrigation = (recent_rainfall >= rain_integration_state.config.skip_threshold_mm);
    
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
                       "=== Rain Integration Diagnostics ===\n");
    
    written += snprintf(buffer + written, buffer_size - written,
                       "Initialized: %s\n", rain_integration_state.initialized ? "Yes" : "No");
    
    written += snprintf(buffer + written, buffer_size - written,
                       "Enabled: %s\n", rain_integration_state.config.integration_enabled ? "Yes" : "No");
    
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
                       "Configuration: %.1f%% sensitivity, %.1f mm threshold, %u hours lookback\n",
                       (double)rain_integration_state.config.rain_sensitivity_pct,
                       (double)rain_integration_state.config.skip_threshold_mm,
                       rain_integration_state.config.lookback_hours);
    
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
    
    /* Validate current configuration */
    if (!validate_integration_config(&rain_integration_state.config)) {
        log_integration_error(RAIN_INTEGRATION_ERROR_CONFIG_INVALID, "Configuration validation failed");
        /* Reset to defaults */
        rain_integration_reset_config();
    }
    
    /* Check calculation success rate */
    if (integration_diagnostics.calculation_success_rate < 80.0f && 
        (integration_diagnostics.successful_calculations + integration_diagnostics.failed_calculations) > 10) {
    LOG_WRN("Rain integration calculation success rate low: %.1f%%", 
        (double)integration_diagnostics.calculation_success_rate);
    }
    
    /* Check sensor availability */
    if (rain_integration_state.config.integration_enabled && !rain_sensor_is_active()) {
        LOG_WRN("Rain integration enabled but sensor not active");
    }
    
    LOG_DBG("Rain integration health check completed - success rate: %.1f%%",
            (double)integration_diagnostics.calculation_success_rate);
}