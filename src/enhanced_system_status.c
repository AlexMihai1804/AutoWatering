/**
 * @file enhanced_system_status.c
 * @brief Enhanced system status implementation for advanced irrigation modes
 * 
 * This module provides enhanced system status reporting that includes:
 * - Interval mode phase tracking
 * - Compensation system status indicators
 * - Environmental sensor health monitoring
 * - Configuration completeness tracking
 */

#include "watering_enhanced.h"
#include "watering_internal.h"
#include "interval_task_integration.h"
#include "environmental_data.h"
#include "bme280_driver.h"
#include "rain_compensation.h"
#include "temperature_compensation.h"
#include "configuration_status.h"
#include "enhanced_system_status.h"
#include "rain_sensor.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(enhanced_system_status, LOG_LEVEL_INF);

/* Global enhanced system status information */
static enhanced_system_status_info_t g_enhanced_status = {0};
static K_MUTEX_DEFINE(status_mutex);
static bool enhanced_system_initialized = false;

/**
 * @brief Initialize enhanced system status module
 * 
 * @return 0 on success, negative error code on failure
 */
int enhanced_system_status_init(void)
{
    if (enhanced_system_initialized) {
        LOG_DBG("Enhanced system status already initialized");
        return 0;
    }

    LOG_INF("Initializing enhanced system status module");
    
    // Initialize module state
    memset(&g_enhanced_status, 0, sizeof(g_enhanced_status));
    enhanced_system_initialized = true;
    
    LOG_INF("Enhanced system status module initialized successfully");
    return 0;
}

/**
 * @brief Get comprehensive enhanced system status information
 * 
 * @param status_info Pointer to structure to fill with status information
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t enhanced_system_get_status(enhanced_system_status_info_t *status_info)
{
    if (!status_info) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    if (k_mutex_lock(&status_mutex, K_MSEC(100)) != 0) {
        LOG_ERR("Failed to acquire status mutex");
        return WATERING_ERROR_TIMEOUT;
    }
    
    // Update all status components
    g_enhanced_status.primary_status = enhanced_system_determine_primary_status();
    
    // Update current task phase
    enhanced_watering_task_state_t task_state;
    if (interval_task_get_enhanced_state(&task_state) == 0) {
        g_enhanced_status.current_task_phase = task_state.current_phase;
    } else {
        g_enhanced_status.current_task_phase = TASK_STATE_IDLE;
    }
    
    // Update compensation status
    enhanced_system_update_compensation_status(&g_enhanced_status.compensation);
    
    // Update sensor status
    enhanced_system_update_sensor_status(&g_enhanced_status.sensors);
    
    // Update channel bitmaps
    g_enhanced_status.interval_mode_channels_bitmap = 0;
    enhanced_system_is_interval_mode_active(&g_enhanced_status.interval_mode_channels_bitmap);
    
    g_enhanced_status.config_incomplete_channels_bitmap = 0;
    enhanced_system_has_incomplete_config(&g_enhanced_status.config_incomplete_channels_bitmap);
    
    // Update active channels bitmap (from existing system)
    g_enhanced_status.active_channels_bitmap = 0;
    if (watering_task_state.current_active_task && 
        watering_task_state.current_active_task->channel) {
        // Find which channel index this is
        for (int i = 0; i < WATERING_CHANNELS_COUNT; i++) {
            if (watering_task_state.current_active_task->channel == &watering_channels[i]) {
                g_enhanced_status.active_channels_bitmap |= (1 << i);
                break;
            }
        }
    }
    
    // Update timestamp
    g_enhanced_status.status_update_timestamp = k_uptime_get_32();
    
    // Copy to output
    *status_info = g_enhanced_status;
    
    k_mutex_unlock(&status_mutex);
    return WATERING_SUCCESS;
}

/**
 * @brief Determine the primary system status based on current conditions
 * 
 * @return Primary enhanced system status
 */
enhanced_system_status_t enhanced_system_determine_primary_status(void)
{
    // Check for critical errors first (highest priority)
    watering_status_t base_status;
    if (watering_get_status(&base_status) == WATERING_SUCCESS) {
        switch (base_status) {
            case WATERING_STATUS_FAULT:
                return ENHANCED_STATUS_FAULT;
            case WATERING_STATUS_RTC_ERROR:
                return ENHANCED_STATUS_RTC_ERROR;
            case WATERING_STATUS_LOW_POWER:
                return ENHANCED_STATUS_LOW_POWER;
            case WATERING_STATUS_NO_FLOW:
                return ENHANCED_STATUS_NO_FLOW;
            case WATERING_STATUS_UNEXPECTED_FLOW:
                return ENHANCED_STATUS_UNEXPECTED_FLOW;
            case WATERING_STATUS_LOCKED:
                return ENHANCED_STATUS_LOCKED;
            default:
                break;
        }
    }
    
    // Check for BME280 sensor errors
    bme280_reading_t current_reading;
    if (bme280_system_read_data(&current_reading) != 0 || !current_reading.valid) {
        // BME280 error, but system can still operate
        LOG_WRN("BME280 sensor error detected");
        return ENHANCED_STATUS_BME280_ERROR;
    }
    
    // Check for interval mode phases (active operations)
    enhanced_watering_task_state_t task_state;
    if (interval_task_get_enhanced_state(&task_state) == 0) {
        if (task_state.current_phase == TASK_STATE_WATERING && task_state.is_interval_mode) {
            return ENHANCED_STATUS_INTERVAL_WATERING;
        } else if (task_state.current_phase == TASK_STATE_PAUSING && task_state.is_interval_mode) {
            return ENHANCED_STATUS_INTERVAL_PAUSING;
        }
    }
    
    // Check for active compensation systems
    // Check rain compensation (use statistics function)
    uint32_t total_calculations, skip_count;
    float avg_reduction_pct;
    if (rain_compensation_get_statistics(0, &total_calculations, &skip_count, &avg_reduction_pct) == 0) {
        if (avg_reduction_pct > 0.1f) {
            return ENHANCED_STATUS_RAIN_COMPENSATION_ACTIVE;
        }
    }
    
    // Check temperature compensation (simplified check)
    temperature_compensation_config_t temp_config;
    if (temp_compensation_init_config(&temp_config) == 0) { 
        if (temp_config.base_temperature != 20.0f) {
            return ENHANCED_STATUS_TEMP_COMPENSATION_ACTIVE;
        }
    }
    
    // Check for incomplete configuration
    uint8_t incomplete_bitmap = 0;
    if (enhanced_system_has_incomplete_config(&incomplete_bitmap) && incomplete_bitmap != 0) {
        return ENHANCED_STATUS_CONFIG_INCOMPLETE;
    }
    
    // Check for custom soil usage
    for (int i = 0; i < WATERING_CHANNELS_COUNT; i++) {
        channel_config_status_t config_status;
        if (channel_get_config_status(i, &config_status) == WATERING_SUCCESS &&
            config_status.custom_soil_configured) {
            return ENHANCED_STATUS_CUSTOM_SOIL_ACTIVE;
        }
    }
    
    // Check for degraded mode (some sensors not working but system operational)
    environmental_sensor_status_t sensor_status;
    enhanced_system_update_sensor_status(&sensor_status);
    if (!sensor_status.bme280_responding || !sensor_status.rain_sensor_active) {
        return ENHANCED_STATUS_DEGRADED_MODE;
    }
    
    // Default to OK if no issues detected
    return ENHANCED_STATUS_OK;
}

/**
 * @brief Update compensation system status information
 * 
 * @param comp_status Pointer to compensation status structure to update
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t enhanced_system_update_compensation_status(compensation_status_t *comp_status)
{
    if (!comp_status) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    // Initialize status
    memset(comp_status, 0, sizeof(compensation_status_t));
    
    // Check rain compensation status
    uint32_t total_calculations, skip_count;
    float avg_reduction_pct;
    if (rain_compensation_get_statistics(0, &total_calculations, &skip_count, &avg_reduction_pct) == 0) {
        comp_status->rain_compensation_active = (avg_reduction_pct > 0.1f);
        comp_status->rain_reduction_percentage = avg_reduction_pct;
        comp_status->last_compensation_update = k_uptime_get_32();
    }
    
    // Check temperature compensation status
    temperature_compensation_config_t temp_config;
    if (temp_compensation_init_config(&temp_config) == 0) {
        comp_status->temp_compensation_active = (temp_config.base_temperature != 20.0f);
        comp_status->temp_adjustment_factor = 1.0f + (temp_config.base_temperature - 20.0f) * 0.05f;
        
        // Use current timestamp
        comp_status->last_compensation_update = k_uptime_get_32();
    }
    
    return WATERING_SUCCESS;
}

/**
 * @brief Update environmental sensor health status
 * 
 * @param sensor_status Pointer to sensor status structure to update
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t enhanced_system_update_sensor_status(environmental_sensor_status_t *sensor_status)
{
    if (!sensor_status) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    // Initialize status
    memset(sensor_status, 0, sizeof(environmental_sensor_status_t));
    
    // Check BME280 status
    bme280_config_t bme280_config;
    if (bme280_system_get_config(&bme280_config) == 0) {
        sensor_status->bme280_initialized = bme280_config.initialized;
        sensor_status->bme280_responding = bme280_config.enabled;
        
        // Check data validity
        bme280_reading_t current_reading;
        if (bme280_system_read_data(&current_reading) == 0) {
            sensor_status->bme280_data_valid = current_reading.valid;
            sensor_status->last_successful_reading = current_reading.timestamp;
            
            // Calculate data quality based on age and accuracy
            uint32_t current_time = k_uptime_get_32();
            uint32_t data_age = current_time - current_reading.timestamp;
            sensor_status->environmental_data_age_sec = data_age / 1000;
            
            if (current_reading.valid && data_age < 300000) { // Less than 5 minutes old
                sensor_status->bme280_data_quality = 100 - (data_age / 6000); // Decrease by 1% per minute
                if (sensor_status->bme280_data_quality > 100) {
                    sensor_status->bme280_data_quality = 100;
                }
            } else {
                sensor_status->bme280_data_quality = 0;
            }
        }
    }
    
    bool rain_enabled = rain_sensor_is_enabled();
    rain_sensor_status_t rain_status = rain_sensor_get_status();
    bool health_ok = !rain_sensor_is_health_critical();
    sensor_status->rain_sensor_active = rain_enabled &&
                                        (rain_status != RAIN_SENSOR_STATUS_ERROR) &&
                                        health_ok;
    
    return WATERING_SUCCESS;
}

/**
 * @brief Check if any channels are currently using interval mode
 * 
 * @param active_channels_bitmap Pointer to store bitmap of channels using interval mode
 * @return true if any channels are using interval mode, false otherwise
 */
bool enhanced_system_is_interval_mode_active(uint8_t *active_channels_bitmap)
{
    if (!active_channels_bitmap) {
        return false;
    }
    
    *active_channels_bitmap = 0;
    bool any_active = false;
    
    for (int i = 0; i < WATERING_CHANNELS_COUNT; i++) {
        // Check if channel has interval mode configured and is currently active
        enhanced_watering_task_state_t task_state;
        if (interval_task_get_enhanced_state(&task_state) == 0 && task_state.is_interval_mode) {
            *active_channels_bitmap |= (1 << i);
            any_active = true;
        }
    }
    
    return any_active;
}

/**
 * @brief Check which channels have incomplete configuration
 * 
 * @param incomplete_channels_bitmap Pointer to store bitmap of channels with incomplete config
 * @return true if any channels have incomplete configuration, false otherwise
 */
bool enhanced_system_has_incomplete_config(uint8_t *incomplete_channels_bitmap)
{
    if (!incomplete_channels_bitmap) {
        return false;
    }
    
    *incomplete_channels_bitmap = 0;
    bool any_incomplete = false;
    
    extern watering_channel_t watering_channels[WATERING_CHANNELS_COUNT];

    for (int i = 0; i < WATERING_CHANNELS_COUNT; i++) {
        channel_config_status_t config_status;
        if (channel_get_config_status(i, &config_status) == WATERING_SUCCESS) {
            /* Use cached config_status instead of calling can_channel_perform_automatic_watering
             * which would re-assess the channel and cause duplicate NVS writes */
            const watering_channel_t *channel = &watering_channels[i];
            bool can_water = config_status.basic_configured && 
                             channel->watering_event.auto_enabled;
            if (!can_water) {
                *incomplete_channels_bitmap |= (1 << i);
                any_incomplete = true;
            }
        }
    }
    
    return any_incomplete;
}

/**
 * @brief Convert enhanced system status to string representation
 * 
 * @param status Enhanced system status value
 * @return String representation of the status
 */
const char* enhanced_system_status_to_string(enhanced_system_status_t status)
{
    switch (status) {
        case ENHANCED_STATUS_OK:
            return "OK";
        case ENHANCED_STATUS_NO_FLOW:
            return "No Flow";
        case ENHANCED_STATUS_UNEXPECTED_FLOW:
            return "Unexpected Flow";
        case ENHANCED_STATUS_FAULT:
            return "System Fault";
        case ENHANCED_STATUS_RTC_ERROR:
            return "RTC Error";
        case ENHANCED_STATUS_LOW_POWER:
            return "Low Power";
        case ENHANCED_STATUS_LOCKED:
            return "Locked";
        case ENHANCED_STATUS_INTERVAL_WATERING:
            return "Interval Watering";
        case ENHANCED_STATUS_INTERVAL_PAUSING:
            return "Interval Pausing";
        case ENHANCED_STATUS_RAIN_COMPENSATION_ACTIVE:
            return "Rain Compensation Active";
        case ENHANCED_STATUS_TEMP_COMPENSATION_ACTIVE:
            return "Temperature Compensation Active";
        case ENHANCED_STATUS_BME280_ERROR:
            return "BME280 Sensor Error";
        case ENHANCED_STATUS_CUSTOM_SOIL_ACTIVE:
            return "Custom Soil Active";
        case ENHANCED_STATUS_CONFIG_INCOMPLETE:
            return "Configuration Incomplete";
        case ENHANCED_STATUS_DEGRADED_MODE:
            return "Degraded Mode";
        default:
            return "Unknown Status";
    }
}

/**
 * @brief Check if system status indicates an error condition
 * 
 * @param status Enhanced system status to check
 * @return true if status indicates an error, false otherwise
 */
bool enhanced_system_status_is_error(enhanced_system_status_t status)
{
    switch (status) {
        case ENHANCED_STATUS_NO_FLOW:
        case ENHANCED_STATUS_UNEXPECTED_FLOW:
        case ENHANCED_STATUS_FAULT:
        case ENHANCED_STATUS_RTC_ERROR:
        case ENHANCED_STATUS_BME280_ERROR:
        case ENHANCED_STATUS_CONFIG_INCOMPLETE:
        case ENHANCED_STATUS_LOCKED:
            return true;
        default:
            return false;
    }
}

/**
 * @brief Check if system status indicates active operation
 * 
 * @param status Enhanced system status to check
 * @return true if status indicates active operation, false otherwise
 */
bool enhanced_system_status_is_active(enhanced_system_status_t status)
{
    switch (status) {
        case ENHANCED_STATUS_INTERVAL_WATERING:
        case ENHANCED_STATUS_INTERVAL_PAUSING:
        case ENHANCED_STATUS_RAIN_COMPENSATION_ACTIVE:
        case ENHANCED_STATUS_TEMP_COMPENSATION_ACTIVE:
            return true;
        default:
            return false;
    }
}

/**
 * @brief Get the configuration status for a specific channel
 * 
 * @param channel_id Channel ID (0-based)
 * @param status Pointer to store the configuration status
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t channel_get_config_status(uint8_t channel_id, channel_config_status_t *status)
{
    if (channel_id >= WATERING_CHANNELS_COUNT || !status) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    // Use configuration_status.c functions to get proper status
    // Get channel reference 
    extern watering_channel_t watering_channels[WATERING_CHANNELS_COUNT];
    watering_channel_t *channel = &watering_channels[channel_id];
    
    watering_error_t result = config_status_assess_channel(channel_id, (enhanced_watering_channel_t*)channel, status);
    if (result != WATERING_SUCCESS) {
        LOG_ERR("Failed to get config status for channel %u: %d", channel_id, result);
        return result;
    }

    LOG_DBG("Channel %u config status: basic=%u, env=%u, comp=%u, custom=%u, interval=%u, score=%u",
            channel_id, status->basic_configured, status->growing_env_configured,
            status->compensation_configured, status->custom_soil_configured, 
            status->interval_configured, status->configuration_score);

    return WATERING_SUCCESS;
}

/**
 * @brief Check if a channel can perform automatic watering
 * 
 * @param channel Pointer to the enhanced watering channel
 * @return true if channel can perform automatic watering, false otherwise
 */
bool can_channel_perform_automatic_watering(uint8_t channel_id,
                                            const enhanced_watering_channel_t *enhanced_channel)
{
    if (!enhanced_channel || channel_id >= WATERING_CHANNELS_COUNT) {
        return false;
    }
    
    // Cast to get basic channel info
    const watering_channel_t *channel = (const watering_channel_t *)enhanced_channel;
    
    // Get channel configuration status
    channel_config_status_t config_status;
    watering_error_t result = config_status_assess_channel(channel_id, enhanced_channel, &config_status);
    if (result != WATERING_SUCCESS) {
        return false;
    }
    
    // Check if basic configuration is complete
    if (!config_status.basic_configured) {
        return false;
    }
    
    // Check if channel is enabled (use basic enabled field)
    if (!channel->watering_event.auto_enabled) {
        return false;
    }
    
    // Check if there are any critical errors
    enhanced_system_status_info_t status_info;
    if (enhanced_system_get_status(&status_info) == WATERING_SUCCESS) {
        if (enhanced_system_status_is_error(status_info.primary_status)) {
            return false;
        }
    }
    
    // Check if sensors are available (BME280 for environmental data)
    // This is a basic check - could be expanded based on specific requirements
    return true;
}
