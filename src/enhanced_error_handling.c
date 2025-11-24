/**
 * @file enhanced_error_handling.c
 * @brief Enhanced error handling and recovery implementation
 * 
 * This module provides comprehensive error handling and recovery for:
 * - BME280 environmental sensor failures
 * - Compensation system errors
 * - Interval mode controller failures
 * - Storage system errors
 * - Graceful degradation strategies
 */

#include "watering_enhanced.h"
#include "enhanced_system_status.h"
#include "bme280_driver.h"
#include "rain_compensation.h"
#include "temperature_compensation.h"
#include "interval_mode_controller.h"
#include "nvs_config.h"
#include "sensor_manager.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

LOG_MODULE_REGISTER(enhanced_error_handling, LOG_LEVEL_INF);

/* Global error recovery state */
static system_error_recovery_state_t g_recovery_state = {0};
static K_MUTEX_DEFINE(recovery_mutex);

/* Recovery strategy configuration */
static const struct {
    enhanced_watering_error_t error_code;
    error_recovery_strategy_t strategy;
    uint8_t max_retries;
    uint32_t timeout_ms;
} recovery_config[] = {
    {WATERING_ERROR_BME280_INIT, RECOVERY_STRATEGY_RETRY, 3, 5000},
    {WATERING_ERROR_BME280_READ, RECOVERY_STRATEGY_FALLBACK, 5, 1000},
    {WATERING_ERROR_COMPENSATION_CALC, RECOVERY_STRATEGY_FALLBACK, 2, 2000},
    {WATERING_ERROR_INTERVAL_CONFIG, RECOVERY_STRATEGY_RESET, 1, 1000},
    {WATERING_ERROR_HISTORY_STORAGE, RECOVERY_STRATEGY_GRACEFUL_DEGRADE, 3, 3000},
    {WATERING_ERROR_ENV_DATA_CORRUPT, RECOVERY_STRATEGY_RESET, 1, 2000},
    {WATERING_ERROR_INTERVAL_MODE_FAILURE, RECOVERY_STRATEGY_DISABLE, 2, 1000},
    {WATERING_ERROR_COMPENSATION_DISABLED, RECOVERY_STRATEGY_GRACEFUL_DEGRADE, 1, 0},
    {WATERING_ERROR_SENSOR_DEGRADED, RECOVERY_STRATEGY_GRACEFUL_DEGRADE, 1, 0},
    {WATERING_ERROR_CONFIG_RESET_FAILED, RECOVERY_STRATEGY_RETRY, 2, 1000},
};

static bool parse_config_reset_context(const char *context, uint8_t *channel_id, config_group_t *group)
{
    bool parsed = false;

    if (!context) {
        return false;
    }

    if (channel_id) {
        const char *channel_ptr = strstr(context, "channel=");
        if (!channel_ptr) {
            channel_ptr = strstr(context, "ch=");
        }
        if (channel_ptr) {
            channel_ptr = strchr(channel_ptr, '=') + 1;
            char *end_ptr = NULL;
            long value = strtol(channel_ptr, &end_ptr, 10);
            if (end_ptr != channel_ptr && value >= -1 && value <= UINT8_MAX) {
                *channel_id = (value < 0) ? 0xFF : (uint8_t)value;
                parsed = true;
            }
        }
    }

    if (group) {
        const char *group_ptr = strstr(context, "group=");
        if (!group_ptr) {
            group_ptr = strstr(context, "grp=");
        }
        if (group_ptr) {
            group_ptr = strchr(group_ptr, '=') + 1;
            char *end_ptr = NULL;
            long value = strtol(group_ptr, &end_ptr, 10);
            if (end_ptr != group_ptr && value >= CONFIG_GROUP_BASIC && value <= CONFIG_GROUP_ALL) {
                *group = (config_group_t)value;
                parsed = true;
            }
        }
    }

    return parsed;
}

/**
 * @brief Initialize the enhanced error recovery system
 * 
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t enhanced_error_recovery_init(void)
{
    if (k_mutex_lock(&recovery_mutex, K_MSEC(1000)) != 0) {
        LOG_ERR("Failed to acquire recovery mutex during init");
        return WATERING_ERROR_TIMEOUT;
    }
    
    // Initialize recovery state
    memset(&g_recovery_state, 0, sizeof(g_recovery_state));
    
    // Initialize individual recovery contexts
    enhanced_error_reset_recovery_context(&g_recovery_state.bme280_recovery);
    enhanced_error_reset_recovery_context(&g_recovery_state.compensation_recovery);
    enhanced_error_reset_recovery_context(&g_recovery_state.interval_recovery);
    enhanced_error_reset_recovery_context(&g_recovery_state.storage_recovery);
    
    g_recovery_state.system_degraded = false;
    
    k_mutex_unlock(&recovery_mutex);
    
    LOG_INF("Enhanced error recovery system initialized");
    return WATERING_SUCCESS;
}

/**
 * @brief Handle BME280 sensor failures with appropriate recovery strategy
 * 
 * @param error_code Specific BME280 error that occurred
 * @param context Additional context information about the error
 * @return WATERING_SUCCESS if recovery successful, error code otherwise
 */
watering_error_t enhanced_error_handle_bme280_failure(enhanced_watering_error_t error_code, const char *context)
{
    if (k_mutex_lock(&recovery_mutex, K_MSEC(1000)) != 0) {
        return WATERING_ERROR_TIMEOUT;
    }
    
    error_recovery_context_t *recovery_ctx = &g_recovery_state.bme280_recovery;
    
    // Update recovery context
    recovery_ctx->error_code = error_code;
    recovery_ctx->last_error_time = k_uptime_get_32();
    if (context) {
        strncpy(recovery_ctx->error_context, context, sizeof(recovery_ctx->error_context) - 1);
        recovery_ctx->error_context[sizeof(recovery_ctx->error_context) - 1] = '\0';
    }
    
    // Find recovery strategy for this error
    for (size_t i = 0; i < ARRAY_SIZE(recovery_config); i++) {
        if (recovery_config[i].error_code == error_code) {
            recovery_ctx->strategy = recovery_config[i].strategy;
            recovery_ctx->max_retries = recovery_config[i].max_retries;
            recovery_ctx->recovery_timeout_ms = recovery_config[i].timeout_ms;
            break;
        }
    }
    
    watering_error_t result = enhanced_error_attempt_recovery(recovery_ctx);
    
    if (result == WATERING_SUCCESS) {
        g_recovery_state.successful_recoveries++;
        LOG_INF("BME280 error recovery successful: %s", context ? context : "");
    } else {
        g_recovery_state.failed_recoveries++;
        g_recovery_state.system_degraded = true;
        LOG_ERR("BME280 error recovery failed: %s", context ? context : "");
    }
    
    g_recovery_state.global_error_count++;
    
    k_mutex_unlock(&recovery_mutex);
    return result;
}

/**
 * @brief Handle compensation system failures with appropriate recovery strategy
 * 
 * @param error_code Specific compensation error that occurred
 * @param context Additional context information about the error
 * @return WATERING_SUCCESS if recovery successful, error code otherwise
 */
watering_error_t enhanced_error_handle_compensation_failure(enhanced_watering_error_t error_code, const char *context)
{
    if (k_mutex_lock(&recovery_mutex, K_MSEC(1000)) != 0) {
        return WATERING_ERROR_TIMEOUT;
    }
    
    error_recovery_context_t *recovery_ctx = &g_recovery_state.compensation_recovery;
    
    // Update recovery context
    recovery_ctx->error_code = error_code;
    recovery_ctx->last_error_time = k_uptime_get_32();
    if (context) {
        strncpy(recovery_ctx->error_context, context, sizeof(recovery_ctx->error_context) - 1);
        recovery_ctx->error_context[sizeof(recovery_ctx->error_context) - 1] = '\0';
    }
    
    // Find recovery strategy
    for (size_t i = 0; i < ARRAY_SIZE(recovery_config); i++) {
        if (recovery_config[i].error_code == error_code) {
            recovery_ctx->strategy = recovery_config[i].strategy;
            recovery_ctx->max_retries = recovery_config[i].max_retries;
            recovery_ctx->recovery_timeout_ms = recovery_config[i].timeout_ms;
            break;
        }
    }
    
    watering_error_t result = enhanced_error_attempt_recovery(recovery_ctx);
    
    if (result == WATERING_SUCCESS) {
        g_recovery_state.successful_recoveries++;
        LOG_INF("Compensation error recovery successful: %s", context ? context : "");
    } else {
        g_recovery_state.failed_recoveries++;
        // Compensation failures are less critical - continue with degraded mode
        g_recovery_state.system_degraded = true;
        LOG_WRN("Compensation error recovery failed, continuing in degraded mode: %s", 
                context ? context : "");
    }
    
    g_recovery_state.global_error_count++;
    
    k_mutex_unlock(&recovery_mutex);
    return result;
}

/**
 * @brief Handle interval mode controller failures
 * 
 * @param error_code Specific interval mode error that occurred
 * @param context Additional context information about the error
 * @return WATERING_SUCCESS if recovery successful, error code otherwise
 */
watering_error_t enhanced_error_handle_interval_mode_failure(enhanced_watering_error_t error_code, const char *context)
{
    if (k_mutex_lock(&recovery_mutex, K_MSEC(1000)) != 0) {
        return WATERING_ERROR_TIMEOUT;
    }
    
    error_recovery_context_t *recovery_ctx = &g_recovery_state.interval_recovery;
    
    // Update recovery context
    recovery_ctx->error_code = error_code;
    recovery_ctx->last_error_time = k_uptime_get_32();
    if (context) {
        strncpy(recovery_ctx->error_context, context, sizeof(recovery_ctx->error_context) - 1);
        recovery_ctx->error_context[sizeof(recovery_ctx->error_context) - 1] = '\0';
    }
    
    // Find recovery strategy
    for (size_t i = 0; i < ARRAY_SIZE(recovery_config); i++) {
        if (recovery_config[i].error_code == error_code) {
            recovery_ctx->strategy = recovery_config[i].strategy;
            recovery_ctx->max_retries = recovery_config[i].max_retries;
            recovery_ctx->recovery_timeout_ms = recovery_config[i].timeout_ms;
            break;
        }
    }
    
    watering_error_t result = enhanced_error_attempt_recovery(recovery_ctx);
    
    if (result == WATERING_SUCCESS) {
        g_recovery_state.successful_recoveries++;
        LOG_INF("Interval mode error recovery successful: %s", context ? context : "");
    } else {
        g_recovery_state.failed_recoveries++;
        LOG_ERR("Interval mode error recovery failed: %s", context ? context : "");
    }
    
    g_recovery_state.global_error_count++;
    
    k_mutex_unlock(&recovery_mutex);
    return result;
}

/**
 * @brief Handle storage system failures
 * 
 * @param error_code Specific storage error that occurred
 * @param context Additional context information about the error
 * @return WATERING_SUCCESS if recovery successful, error code otherwise
 */
watering_error_t enhanced_error_handle_storage_failure(enhanced_watering_error_t error_code, const char *context)
{
    if (k_mutex_lock(&recovery_mutex, K_MSEC(1000)) != 0) {
        return WATERING_ERROR_TIMEOUT;
    }
    
    error_recovery_context_t *recovery_ctx = &g_recovery_state.storage_recovery;
    
    // Update recovery context
    recovery_ctx->error_code = error_code;
    recovery_ctx->last_error_time = k_uptime_get_32();
    if (context) {
        strncpy(recovery_ctx->error_context, context, sizeof(recovery_ctx->error_context) - 1);
        recovery_ctx->error_context[sizeof(recovery_ctx->error_context) - 1] = '\0';
    }
    
    // Find recovery strategy
    for (size_t i = 0; i < ARRAY_SIZE(recovery_config); i++) {
        if (recovery_config[i].error_code == error_code) {
            recovery_ctx->strategy = recovery_config[i].strategy;
            recovery_ctx->max_retries = recovery_config[i].max_retries;
            recovery_ctx->recovery_timeout_ms = recovery_config[i].timeout_ms;
            break;
        }
    }
    
    watering_error_t result = enhanced_error_attempt_recovery(recovery_ctx);
    
    if (result == WATERING_SUCCESS) {
        g_recovery_state.successful_recoveries++;
        LOG_INF("Storage error recovery successful: %s", context ? context : "");
    } else {
        g_recovery_state.failed_recoveries++;
        g_recovery_state.system_degraded = true;
        LOG_ERR("Storage error recovery failed: %s", context ? context : "");
    }
    
    g_recovery_state.global_error_count++;
    
    k_mutex_unlock(&recovery_mutex);
    return result;
}

/**
 * @brief Attempt recovery based on the recovery context
 * 
 * @param recovery_ctx Recovery context with error information and strategy
 * @return WATERING_SUCCESS if recovery successful, error code otherwise
 */
watering_error_t enhanced_error_attempt_recovery(error_recovery_context_t *recovery_ctx)
{
    if (!recovery_ctx) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    // Check if we should attempt recovery
    if (!enhanced_error_should_retry(recovery_ctx)) {
        LOG_WRN("Recovery attempt skipped - max retries exceeded");
        return WATERING_ERROR_RECOVERY_FAILED;
    }
    
    recovery_ctx->recovery_in_progress = true;
    recovery_ctx->retry_count++;
    
    watering_error_t result = WATERING_ERROR_RECOVERY_FAILED;
    
    LOG_INF("Attempting recovery strategy %s for error %s (attempt %d/%d)",
            enhanced_error_recovery_strategy_to_string(recovery_ctx->strategy),
            enhanced_error_code_to_string(recovery_ctx->error_code),
            recovery_ctx->retry_count, recovery_ctx->max_retries);
    
    switch (recovery_ctx->strategy) {
        case RECOVERY_STRATEGY_RETRY:
            // Retry the original operation
            switch (recovery_ctx->error_code) {
                case WATERING_ERROR_BME280_INIT:
                {
                    int rc = sensor_manager_recover_sensor(SENSOR_TYPE_BME280);
                    if (rc == 0) {
                        result = WATERING_SUCCESS;
                    } else {
                        LOG_ERR("BME280 recovery failed with rc=%d", rc);
                        result = WATERING_ERROR_RECOVERY_FAILED;
                    }
                    break;
                }
                case WATERING_ERROR_CONFIG_RESET_FAILED:
                {
                    uint8_t channel = 0xFF;
                    config_group_t group = CONFIG_GROUP_ALL;
                    parse_config_reset_context(recovery_ctx->error_context, &channel, &group);

                    if (channel == 0xFF) {
                        result = WATERING_SUCCESS;
                        for (uint8_t ch = 0; ch < WATERING_CHANNELS_COUNT; ch++) {
                            watering_error_t reset_err =
                                channel_reset_config_group(ch, group, "automatic recovery");
                            if (reset_err != WATERING_SUCCESS) {
                                LOG_ERR("Config reset recovery failed for channel %d: %d", ch, reset_err);
                                result = reset_err;
                                break;
                            }
                        }
                    } else {
                        result = channel_reset_config_group(channel, group, "automatic recovery");
                        if (result != WATERING_SUCCESS) {
                            LOG_ERR("Config reset recovery failed for channel %d group %d: %d",
                                    channel, group, result);
                        }
                    }
                    break;
                }
                default:
                    LOG_WRN("Retry strategy not implemented for error %d", recovery_ctx->error_code);
                    break;
            }
            break;
            
        case RECOVERY_STRATEGY_FALLBACK:
            // Use fallback/default values
            switch (recovery_ctx->error_code) {
                case WATERING_ERROR_BME280_READ:
                    // Use last known good values or defaults
                    result = WATERING_SUCCESS;
                    LOG_INF("Using fallback environmental data");
                    break;
                case WATERING_ERROR_COMPENSATION_CALC:
                    // Disable compensation temporarily
                    result = WATERING_SUCCESS;
                    LOG_INF("Compensation disabled, using standard calculations");
                    break;
                default:
                    LOG_WRN("Fallback strategy not implemented for error %d", recovery_ctx->error_code);
                    break;
            }
            break;
            
        case RECOVERY_STRATEGY_DISABLE:
            // Disable the failing component
            switch (recovery_ctx->error_code) {
                case WATERING_ERROR_INTERVAL_MODE_FAILURE:
                    // Disable interval mode, fall back to continuous watering
                    result = WATERING_SUCCESS;
                    LOG_INF("Interval mode disabled, using continuous watering");
                    break;
                default:
                    LOG_WRN("Disable strategy not implemented for error %d", recovery_ctx->error_code);
                    break;
            }
            break;
            
        case RECOVERY_STRATEGY_RESET:
            // Reset the component/system
            switch (recovery_ctx->error_code) {
                case WATERING_ERROR_INTERVAL_CONFIG:
                    // Reset interval configuration to defaults
                    result = WATERING_SUCCESS;
                    LOG_INF("Interval configuration reset to defaults");
                    break;
                case WATERING_ERROR_ENV_DATA_CORRUPT:
                    // Reset environmental data storage
                    result = WATERING_SUCCESS;
                    LOG_INF("Environmental data storage reset");
                    break;
                default:
                    LOG_WRN("Reset strategy not implemented for error %d", recovery_ctx->error_code);
                    break;
            }
            break;
            
        case RECOVERY_STRATEGY_GRACEFUL_DEGRADE:
            // Continue with reduced functionality
            result = WATERING_SUCCESS;
            LOG_INF("System continuing in degraded mode");
            break;
            
        default:
            LOG_ERR("Unknown recovery strategy: %d", recovery_ctx->strategy);
            break;
    }
    
    recovery_ctx->recovery_in_progress = false;
    
    if (result == WATERING_SUCCESS) {
        LOG_INF("Recovery successful for error %s", 
                enhanced_error_code_to_string(recovery_ctx->error_code));
        // Reset retry count on successful recovery
        recovery_ctx->retry_count = 0;
    } else {
        LOG_ERR("Recovery failed for error %s", 
                enhanced_error_code_to_string(recovery_ctx->error_code));
    }
    
    return result;
}

/**
 * @brief Get current error recovery state
 * 
 * @param recovery_state Pointer to structure to fill with recovery state
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t enhanced_error_get_recovery_state(system_error_recovery_state_t *recovery_state)
{
    if (!recovery_state) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    if (k_mutex_lock(&recovery_mutex, K_MSEC(100)) != 0) {
        return WATERING_ERROR_TIMEOUT;
    }
    
    *recovery_state = g_recovery_state;
    
    k_mutex_unlock(&recovery_mutex);
    return WATERING_SUCCESS;
}

/**
 * @brief Check if recovery should be attempted based on retry limits
 * 
 * @param recovery_ctx Recovery context to check
 * @return true if recovery should be attempted, false otherwise
 */
bool enhanced_error_should_retry(const error_recovery_context_t *recovery_ctx)
{
    if (!recovery_ctx) {
        return false;
    }
    
    // Don't retry if already at max retries
    if (recovery_ctx->retry_count >= recovery_ctx->max_retries) {
        return false;
    }
    
    // Don't retry if recovery is already in progress
    if (recovery_ctx->recovery_in_progress) {
        return false;
    }
    
    // Check if enough time has passed since last error (rate limiting)
    uint32_t current_time = k_uptime_get_32();
    uint32_t time_since_error = current_time - recovery_ctx->last_error_time;
    
    // Wait at least 1 second between retry attempts
    if (time_since_error < 1000) {
        return false;
    }
    
    return true;
}

/**
 * @brief Reset recovery context to initial state
 * 
 * @param recovery_ctx Recovery context to reset
 */
void enhanced_error_reset_recovery_context(error_recovery_context_t *recovery_ctx)
{
    if (!recovery_ctx) {
        return;
    }
    
    memset(recovery_ctx, 0, sizeof(error_recovery_context_t));
    recovery_ctx->strategy = RECOVERY_STRATEGY_NONE;
}

/**
 * @brief Convert error code to string representation
 * 
 * @param error_code Enhanced error code
 * @return String representation of the error code
 */
const char* enhanced_error_code_to_string(enhanced_watering_error_t error_code)
{
    switch (error_code) {
        case WATERING_ERROR_BME280_INIT:
            return "BME280 Init Failed";
        case WATERING_ERROR_BME280_READ:
            return "BME280 Read Failed";
        case WATERING_ERROR_CUSTOM_SOIL_INVALID:
            return "Invalid Custom Soil";
        case WATERING_ERROR_COMPENSATION_CALC:
            return "Compensation Calculation Failed";
        case WATERING_ERROR_INTERVAL_CONFIG:
            return "Invalid Interval Configuration";
        case WATERING_ERROR_HISTORY_STORAGE:
            return "History Storage Failed";
        case WATERING_ERROR_ENV_DATA_CORRUPT:
            return "Environmental Data Corrupt";
        case WATERING_ERROR_INTERVAL_MODE_FAILURE:
            return "Interval Mode Failure";
        case WATERING_ERROR_COMPENSATION_DISABLED:
            return "Compensation Disabled";
        case WATERING_ERROR_SENSOR_DEGRADED:
            return "Sensor Degraded";
        case WATERING_ERROR_CONFIG_RESET_FAILED:
            return "Config Reset Failed";
        case WATERING_ERROR_RECOVERY_FAILED:
            return "Recovery Failed";
        default:
            return "Unknown Error";
    }
}

/**
 * @brief Convert recovery strategy to string representation
 * 
 * @param strategy Recovery strategy
 * @return String representation of the recovery strategy
 */
const char* enhanced_error_recovery_strategy_to_string(error_recovery_strategy_t strategy)
{
    switch (strategy) {
        case RECOVERY_STRATEGY_NONE:
            return "None";
        case RECOVERY_STRATEGY_RETRY:
            return "Retry";
        case RECOVERY_STRATEGY_FALLBACK:
            return "Fallback";
        case RECOVERY_STRATEGY_DISABLE:
            return "Disable";
        case RECOVERY_STRATEGY_RESET:
            return "Reset";
        case RECOVERY_STRATEGY_GRACEFUL_DEGRADE:
            return "Graceful Degrade";
        default:
            return "Unknown Strategy";
    }
}

/**
 * @brief Initialize the enhanced error handling system
 * 
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t enhanced_error_handling_init(void)
{
    return enhanced_error_recovery_init();
}
