#include "configuration_status.h"
#include "watering_log.h"
#include "nvs_config.h"
#include "watering.h"
#include "custom_soil_db.h"
#include "temperature_compensation.h"
#include <string.h>
#include <time.h>
#include <stdio.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(configuration_status, LOG_LEVEL_INF);

#define CONFIG_RESET_LOG_CAPACITY 16U

static config_reset_log_t g_reset_logs[WATERING_CHANNELS_COUNT];
static bool g_reset_logs_loaded[WATERING_CHANNELS_COUNT];

static config_reset_log_t *config_status_load_reset_log(uint8_t channel_id);
static void config_status_save_reset_log(uint8_t channel_id);
static void config_status_reset_log_add_entry(uint8_t channel_id,
                                             config_group_t group,
                                             const char *reason,
                                             uint32_t timestamp);
static void reset_basic_configuration(watering_channel_t *channel);
static void reset_growing_environment_configuration(watering_channel_t *channel);
static void reset_compensation_configuration(watering_channel_t *channel);
static void reset_custom_soil_configuration(uint8_t channel_id, watering_channel_t *channel);
static void reset_interval_configuration(watering_channel_t *channel);

static const char *RESET_REASON_FALLBACK = "User request";

static config_reset_log_t *config_status_load_reset_log(uint8_t channel_id)
{
    if (channel_id >= WATERING_CHANNELS_COUNT) {
        return NULL;
    }

    if (!g_reset_logs_loaded[channel_id]) {
        if (nvs_load_config_reset_log(channel_id, &g_reset_logs[channel_id]) < 0) {
            memset(&g_reset_logs[channel_id], 0, sizeof(config_reset_log_t));
        } else {
            if (g_reset_logs[channel_id].head >= CONFIG_RESET_LOG_CAPACITY) {
                g_reset_logs[channel_id].head %= CONFIG_RESET_LOG_CAPACITY;
            }
            if (g_reset_logs[channel_id].count > CONFIG_RESET_LOG_CAPACITY) {
                g_reset_logs[channel_id].count = CONFIG_RESET_LOG_CAPACITY;
            }
        }
        g_reset_logs_loaded[channel_id] = true;
    }

    return &g_reset_logs[channel_id];
}

static void config_status_save_reset_log(uint8_t channel_id)
{
    config_reset_log_t *log = config_status_load_reset_log(channel_id);
    if (!log) {
        return;
    }

    int ret = nvs_save_config_reset_log(channel_id, log);
    if (ret < 0) {
        LOG_WRN("Failed to persist reset log for channel %u: %d", channel_id, ret);
    }
}

static void config_status_reset_log_add_entry(uint8_t channel_id,
                                             config_group_t group,
                                             const char *reason,
                                             uint32_t timestamp)
{
    config_reset_log_t *log = config_status_load_reset_log(channel_id);
    if (!log) {
        return;
    }

    uint8_t index = log->head % CONFIG_RESET_LOG_CAPACITY;
    config_reset_log_entry_t *entry = &log->entries[index];

    entry->group = group;
    entry->timestamp = timestamp;
    entry->channel_id = channel_id;

    const char *effective_reason = (reason && reason[0] != '\0') ? reason : RESET_REASON_FALLBACK;
    strncpy(entry->reason, effective_reason, sizeof(entry->reason) - 1);
    entry->reason[sizeof(entry->reason) - 1] = '\0';

    log->head = (log->head + 1U) % CONFIG_RESET_LOG_CAPACITY;
    if (log->count < CONFIG_RESET_LOG_CAPACITY) {
        log->count++;
    }

    config_status_save_reset_log(channel_id);
}

watering_error_t config_status_get_reset_log(uint8_t channel_id, config_reset_log_t *log)
{
    if (channel_id >= WATERING_CHANNELS_COUNT || !log) {
        return WATERING_ERROR_INVALID_PARAM;
    }

    config_reset_log_t *stored = config_status_load_reset_log(channel_id);
    if (!stored) {
        return WATERING_ERROR_INVALID_PARAM;
    }

    *log = *stored;
    return WATERING_SUCCESS;
}

/**
 * @file configuration_status.c
 * @brief Comprehensive configuration status tracking implementation
 * 
 * This module implements configuration status assessment for all configuration
 * groups with completeness scoring and persistent flag management.
 */

/* Configuration group weights for scoring */
#define CONFIG_WEIGHT_BASIC             25      // 25% weight
#define CONFIG_WEIGHT_GROWING_ENV       25      // 25% weight
#define CONFIG_WEIGHT_COMPENSATION      20      // 20% weight
#define CONFIG_WEIGHT_CUSTOM_SOIL       15      // 15% weight
#define CONFIG_WEIGHT_INTERVAL          15      // 15% weight

/* Internal helper functions */
static int config_status_save_to_nvs(uint8_t channel_id, const channel_config_status_t *status);
static int config_status_load_from_nvs(uint8_t channel_id, channel_config_status_t *status);
static bool validate_basic_configuration(const enhanced_watering_channel_t *channel);
static bool validate_growing_environment_configuration(const enhanced_watering_channel_t *channel);
static bool validate_compensation_configuration(const enhanced_watering_channel_t *channel);
static bool validate_custom_soil_configuration(const enhanced_watering_channel_t *channel);
static bool validate_interval_channel_configuration(const enhanced_watering_channel_t *channel);

int config_status_assess_channel(uint8_t channel_id, const enhanced_watering_channel_t *channel,
                                channel_config_status_t *status)
{
    if (channel_id >= WATERING_CHANNELS_COUNT || !channel || !status) {
        return -WATERING_ERROR_INVALID_PARAM;
    }

    LOG_DBG("Assessing configuration status for channel %d", channel_id);

    // Initialize status structure
    memset(status, 0, sizeof(channel_config_status_t));

    // Assess each configuration group
    status->basic_configured = validate_basic_configuration(channel);
    status->growing_env_configured = validate_growing_environment_configuration(channel);
    status->compensation_configured = validate_compensation_configuration(channel);
    status->custom_soil_configured = validate_custom_soil_configuration(channel);
    status->interval_configured = validate_interval_channel_configuration(channel);

    // Calculate configuration score
    status->configuration_score = config_status_calculate_score(status);

    // Preserve reset metadata from persisted state if available
    channel_config_status_t persisted;
    if (config_status_load_from_nvs(channel_id, &persisted) == 0) {
        status->last_reset_timestamp = persisted.last_reset_timestamp;
        status->reset_count = persisted.reset_count;
    } else {
        status->last_reset_timestamp = 0;
        status->reset_count = 0;
    }

    // Save status to NVS
    int result = config_status_save_to_nvs(channel_id, status);
    if (result != 0) {
        LOG_WRN("Failed to save configuration status to NVS: %d", result);
    }

    LOG_INF("Channel %d configuration status: basic=%d, env=%d, comp=%d, soil=%d, interval=%d, score=%d%%",
            channel_id, status->basic_configured, status->growing_env_configured,
            status->compensation_configured, status->custom_soil_configured,
            status->interval_configured, status->configuration_score);

    return 0;
}

uint8_t config_status_calculate_score(const channel_config_status_t *status)
{
    if (!status) {
        return 0;
    }

    uint8_t total_score = 0;

    // Add weighted scores for each configuration group
    if (status->basic_configured) {
        total_score += CONFIG_WEIGHT_BASIC;
    }
    
    if (status->growing_env_configured) {
        total_score += CONFIG_WEIGHT_GROWING_ENV;
    }
    
    if (status->compensation_configured) {
        total_score += CONFIG_WEIGHT_COMPENSATION;
    }
    
    if (status->custom_soil_configured) {
        total_score += CONFIG_WEIGHT_CUSTOM_SOIL;
    }
    
    if (status->interval_configured) {
        total_score += CONFIG_WEIGHT_INTERVAL;
    }

    return total_score;
}

bool config_status_can_perform_automatic_watering(const channel_config_status_t *status)
{
    if (!status) {
        return false;
    }

    // Minimum requirements for automatic watering:
    // - Basic configuration (plant, soil, irrigation method)
    // - Growing environment configuration (coverage, sun exposure, water factor)
    return status->basic_configured && status->growing_env_configured;
}

int config_status_get_missing_items(const channel_config_status_t *status,
                                   config_missing_items_t *missing_items)
{
    if (!status || !missing_items) {
        return -WATERING_ERROR_INVALID_PARAM;
    }

    memset(missing_items, 0, sizeof(config_missing_items_t));

    // Check basic configuration items
    if (!status->basic_configured) {
        missing_items->missing_basic = true;
        strncpy(missing_items->basic_details, 
                "Plant type, soil type, or irrigation method not configured", 
                sizeof(missing_items->basic_details) - 1);
    }

    // Check growing environment items
    if (!status->growing_env_configured) {
        missing_items->missing_growing_env = true;
        strncpy(missing_items->growing_env_details,
                "Coverage area, sun exposure, or water factor not configured",
                sizeof(missing_items->growing_env_details) - 1);
    }

    // Check compensation items
    if (!status->compensation_configured) {
        missing_items->missing_compensation = true;
        strncpy(missing_items->compensation_details,
                "Rain or temperature compensation settings not configured",
                sizeof(missing_items->compensation_details) - 1);
    }

    // Check custom soil items
    if (!status->custom_soil_configured) {
        missing_items->missing_custom_soil = true;
        strncpy(missing_items->custom_soil_details,
                "Custom soil parameters not configured (optional)",
                sizeof(missing_items->custom_soil_details) - 1);
    }

    // Check interval items
    if (!status->interval_configured) {
        missing_items->missing_interval = true;
        strncpy(missing_items->interval_details,
                "Interval watering timing not configured (optional)",
                sizeof(missing_items->interval_details) - 1);
    }

    // Count total missing items
    missing_items->total_missing_count = 0;
    if (missing_items->missing_basic) missing_items->total_missing_count++;
    if (missing_items->missing_growing_env) missing_items->total_missing_count++;
    if (missing_items->missing_compensation) missing_items->total_missing_count++;
    if (missing_items->missing_custom_soil) missing_items->total_missing_count++;
    if (missing_items->missing_interval) missing_items->total_missing_count++;

    return 0;
}

int config_status_update_flags(uint8_t channel_id, config_group_t group, bool configured)
{
    if (channel_id >= WATERING_CHANNELS_COUNT) {
        return -WATERING_ERROR_INVALID_PARAM;
    }

    LOG_DBG("Updating configuration flag for channel %d, group %d: %s", 
            channel_id, group, configured ? "configured" : "not configured");

    // Load current status
    channel_config_status_t status;
    int result = config_status_load_from_nvs(channel_id, &status);
    if (result != 0) {
        // Initialize with defaults if load fails
        memset(&status, 0, sizeof(channel_config_status_t));
    }

    // Update specific flag
    switch (group) {
        case CONFIG_GROUP_BASIC:
            status.basic_configured = configured;
            break;
        case CONFIG_GROUP_GROWING_ENV:
            status.growing_env_configured = configured;
            break;
        case CONFIG_GROUP_COMPENSATION:
            status.compensation_configured = configured;
            break;
        case CONFIG_GROUP_CUSTOM_SOIL:
            status.custom_soil_configured = configured;
            break;
        case CONFIG_GROUP_INTERVAL:
            status.interval_configured = configured;
            break;
        default:
            return -WATERING_ERROR_INVALID_PARAM;
    }

    // Recalculate score
    status.configuration_score = config_status_calculate_score(&status);

    // Save updated status
    result = config_status_save_to_nvs(channel_id, &status);
    if (result != 0) {
        LOG_ERR("Failed to save updated configuration status: %d", result);
        return result;
    }

    LOG_DBG("Configuration flag updated, new score: %d%%", status.configuration_score);
    return 0;
}

watering_error_t channel_validate_config_completeness(uint8_t channel_id, bool *can_water)
{
    if (!can_water || channel_id >= WATERING_CHANNELS_COUNT) {
        return WATERING_ERROR_INVALID_PARAM;
    }

    watering_channel_t *channel;
    watering_error_t err = watering_get_channel(channel_id, &channel);
    if (err != WATERING_SUCCESS) {
        return err;
    }

    channel_config_status_t status;
    watering_error_t assess_err = config_status_assess_channel(
        channel_id, (enhanced_watering_channel_t *)channel, &status);
    if (assess_err != WATERING_SUCCESS) {
        return assess_err;
    }

    *can_water = config_status_can_perform_automatic_watering(&status);
    return WATERING_SUCCESS;
}

int config_status_get_system_overview(config_system_overview_t *overview)
{
    if (!overview) {
        return -WATERING_ERROR_INVALID_PARAM;
    }

    memset(overview, 0, sizeof(config_system_overview_t));

    // Assess all channels
    for (uint8_t channel_id = 0; channel_id < WATERING_CHANNELS_COUNT; channel_id++) {
        channel_config_status_t status;
        int result = config_status_load_from_nvs(channel_id, &status);
        
        if (result == 0) {
            overview->channel_scores[channel_id] = status.configuration_score;
            overview->channels_ready_for_auto_watering += 
                config_status_can_perform_automatic_watering(&status) ? 1 : 0;
            
            // Update group statistics
            if (status.basic_configured) overview->channels_with_basic++;
            if (status.growing_env_configured) overview->channels_with_growing_env++;
            if (status.compensation_configured) overview->channels_with_compensation++;
            if (status.custom_soil_configured) overview->channels_with_custom_soil++;
            if (status.interval_configured) overview->channels_with_interval++;
            
            // Track fully configured channels
            if (status.configuration_score == 100) {
                overview->fully_configured_channels++;
            }
            
            // Update overall score
            overview->overall_system_score += status.configuration_score;
        } else {
            // Channel not configured
            overview->channel_scores[channel_id] = 0;
        }
    }

    // Calculate average system score
    overview->overall_system_score /= WATERING_CHANNELS_COUNT;

    // Calculate configuration health
    if (overview->overall_system_score >= 80) {
        overview->system_health = CONFIG_HEALTH_EXCELLENT;
    } else if (overview->overall_system_score >= 60) {
        overview->system_health = CONFIG_HEALTH_GOOD;
    } else if (overview->overall_system_score >= 40) {
        overview->system_health = CONFIG_HEALTH_FAIR;
    } else {
        overview->system_health = CONFIG_HEALTH_POOR;
    }

    LOG_INF("System configuration overview: %d%% overall, %d channels ready for auto watering",
            overview->overall_system_score, overview->channels_ready_for_auto_watering);

    return 0;
}

int config_status_validate_for_watering(uint8_t channel_id, config_validation_result_t *result)
{
    if (channel_id >= WATERING_CHANNELS_COUNT || !result) {
        return -WATERING_ERROR_INVALID_PARAM;
    }

    memset(result, 0, sizeof(config_validation_result_t));
    result->channel_id = channel_id;

    // Load configuration status
    channel_config_status_t status;
    int load_result = config_status_load_from_nvs(channel_id, &status);
    if (load_result != 0) {
        result->can_water = false;
        result->validation_error = CONFIG_VALIDATION_ERROR_NOT_CONFIGURED;
        strncpy(result->error_message, "Channel configuration not found",
                sizeof(result->error_message) - 1);
        return 0;
    }

    // Check minimum requirements
    if (!config_status_can_perform_automatic_watering(&status)) {
        result->can_water = false;
        result->validation_error = CONFIG_VALIDATION_ERROR_INCOMPLETE;
        
        if (!status.basic_configured && !status.growing_env_configured) {
            strncpy(result->error_message, 
                    "Basic configuration and growing environment required",
                    sizeof(result->error_message) - 1);
        } else if (!status.basic_configured) {
            strncpy(result->error_message, 
                    "Basic configuration (plant, soil, irrigation method) required",
                    sizeof(result->error_message) - 1);
        } else if (!status.growing_env_configured) {
            strncpy(result->error_message, 
                    "Growing environment (coverage, sun exposure, water factor) required",
                    sizeof(result->error_message) - 1);
        }
        
        return 0;
    }

    // Validation passed
    result->can_water = true;
    result->validation_error = CONFIG_VALIDATION_ERROR_NONE;
    result->configuration_score = status.configuration_score;
    
    // Add recommendations for optional features
    if (!status.compensation_configured) {
        strncpy(result->recommendations, 
                "Consider configuring rain/temperature compensation for better efficiency",
                sizeof(result->recommendations) - 1);
    } else if (!status.interval_configured) {
        strncpy(result->recommendations,
                "Consider configuring interval watering for advanced irrigation patterns",
                sizeof(result->recommendations) - 1);
    }

    LOG_DBG("Channel %d validation: can_water=%d, score=%d%%", 
            channel_id, result->can_water, result->configuration_score);

    return 0;
}

int config_status_reset_channel_flags(uint8_t channel_id)
{
    if (channel_id >= WATERING_CHANNELS_COUNT) {
        return -WATERING_ERROR_INVALID_PARAM;
    }

    LOG_INF("Resetting all configuration flags for channel %d", channel_id);

    channel_config_status_t status;
    memset(&status, 0, sizeof(status));
    status.last_reset_timestamp = (uint32_t)time(NULL);

    channel_config_status_t existing_status;
    if (config_status_load_from_nvs(channel_id, &existing_status) == 0) {
        status.reset_count = existing_status.reset_count + 1;
    } else {
        status.reset_count = 1;
    }

    status.configuration_score = config_status_calculate_score(&status);

    int result = config_status_save_to_nvs(channel_id, &status);
    if (result != 0) {
        LOG_ERR("Failed to save reset configuration status: %d", result);
        return result;
    }

    config_status_reset_log_add_entry(channel_id, CONFIG_GROUP_ALL,
                                      "Configuration flags reset",
                                      status.last_reset_timestamp);

    watering_channel_t *channel;
    if (watering_get_channel(channel_id, &channel) == WATERING_SUCCESS) {
        channel->config_status.basic_configured = false;
        channel->config_status.growing_env_configured = false;
        channel->config_status.compensation_configured = false;
        channel->config_status.custom_soil_configured = false;
        channel->config_status.interval_configured = false;
        channel->config_status.configuration_score = status.configuration_score;
        channel->config_status.last_reset_timestamp = status.last_reset_timestamp;
    }

    LOG_INF("Channel %d configuration flags reset (reset count: %d)", 
            channel_id, status.reset_count);

    return 0;
}

static void reset_basic_configuration(watering_channel_t *channel)
{
    if (!channel) {
        return;
    }

    channel->plant_info.main_type = PLANT_TYPE_OTHER;
    memset(&channel->plant_info.specific, 0, sizeof(channel->plant_info.specific));
    channel->plant_type = PLANT_TYPE_OTHER;
    channel->soil_type = SOIL_TYPE_LOAMY;
    channel->irrigation_method = IRRIGATION_DRIP;
}

static void reset_growing_environment_configuration(watering_channel_t *channel)
{
    if (!channel) {
        return;
    }

    channel->use_area_based = true;
    channel->coverage.area_m2 = 0.0f;
    channel->coverage.plant_count = 0;
    channel->sun_percentage = 0;
    channel->max_volume_limit_l = 0.0f;
    channel->enable_cycle_soak = false;
}

static void reset_compensation_configuration(watering_channel_t *channel)
{
    if (!channel) {
        return;
    }

    channel->rain_compensation.enabled = false;
    channel->rain_compensation.sensitivity = 0.0f;
    channel->rain_compensation.lookback_hours = 0;
    channel->rain_compensation.skip_threshold_mm = 0.0f;
    channel->rain_compensation.reduction_factor = 0.0f;

    channel->last_rain_compensation.reduction_percentage = 0.0f;
    channel->last_rain_compensation.skip_watering = false;

    channel->temp_compensation.enabled = false;
    channel->temp_compensation.base_temperature = TEMP_COMP_DEFAULT_BASE_TEMP;
    channel->temp_compensation.sensitivity = TEMP_COMP_DEFAULT_SENSITIVITY;
    channel->temp_compensation.min_factor = TEMP_COMP_DEFAULT_MIN_FACTOR;
    channel->temp_compensation.max_factor = TEMP_COMP_DEFAULT_MAX_FACTOR;

    channel->last_temp_compensation.compensation_factor = 1.0f;
    channel->last_temp_compensation.adjusted_requirement = 0.0f;
}

static void reset_custom_soil_configuration(uint8_t channel_id, watering_channel_t *channel)
{
    if (!channel) {
        return;
    }

    if (custom_soil_db_delete(channel_id) != WATERING_SUCCESS) {
        LOG_WRN("Custom soil delete failed for channel %u (may not exist)", channel_id);
    }

    channel->soil_config.use_custom_soil = false;
    memset(&channel->soil_config.custom, 0, sizeof(channel->soil_config.custom));
}

static void reset_interval_configuration(watering_channel_t *channel)
{
    if (!channel) {
        return;
    }

    channel->interval_config.configured = false;
    channel->interval_config.watering_minutes = 0;
    channel->interval_config.watering_seconds = 0;
    channel->interval_config.pause_minutes = 0;
    channel->interval_config.pause_seconds = 0;
    channel->interval_config.phase_start_time = 0;

    memset(&channel->interval_config_shadow, 0, sizeof(channel->interval_config_shadow));
    channel->interval_config_shadow.configured = false;
}

watering_error_t channel_reset_config_group(uint8_t channel_id, config_group_t group, const char *reason)
{
    if (channel_id >= WATERING_CHANNELS_COUNT) {
        return WATERING_ERROR_INVALID_PARAM;
    }

    watering_channel_t *channel;
    watering_error_t err = watering_get_channel(channel_id, &channel);
    if (err != WATERING_SUCCESS) {
        return err;
    }

    bool reset_all = (group == CONFIG_GROUP_ALL);
    bool handled = false;
    uint32_t timestamp = (uint32_t)time(NULL);

    channel_config_status_t persisted_status;
    if (config_status_load_from_nvs(channel_id, &persisted_status) != 0) {
        memset(&persisted_status, 0, sizeof(persisted_status));
    }

    if (reset_all || group == CONFIG_GROUP_BASIC) {
        reset_basic_configuration(channel);
        persisted_status.basic_configured = false;
        channel->config_status.basic_configured = false;
        handled = true;
    }

    if (reset_all || group == CONFIG_GROUP_GROWING_ENV) {
        reset_growing_environment_configuration(channel);
        persisted_status.growing_env_configured = false;
        channel->config_status.growing_env_configured = false;
        handled = true;
    }

    if (reset_all || group == CONFIG_GROUP_COMPENSATION) {
        reset_compensation_configuration(channel);
        persisted_status.compensation_configured = false;
        channel->config_status.compensation_configured = false;
        handled = true;
    }

    if (reset_all || group == CONFIG_GROUP_CUSTOM_SOIL) {
        reset_custom_soil_configuration(channel_id, channel);
        persisted_status.custom_soil_configured = false;
        channel->config_status.custom_soil_configured = false;
        handled = true;
    }

    if (reset_all || group == CONFIG_GROUP_INTERVAL) {
        reset_interval_configuration(channel);
        persisted_status.interval_configured = false;
        channel->config_status.interval_configured = false;
        handled = true;
    }

    if (!handled) {
        return WATERING_ERROR_INVALID_PARAM;
    }

    persisted_status.last_reset_timestamp = timestamp;
    persisted_status.reset_count += 1;
    persisted_status.configuration_score = config_status_calculate_score(&persisted_status);

    int save_result = config_status_save_to_nvs(channel_id, &persisted_status);
    if (save_result != 0) {
        LOG_WRN("Failed to persist configuration status for channel %u: %d", channel_id, save_result);
    }

    channel->config_status.basic_configured = persisted_status.basic_configured;
    channel->config_status.growing_env_configured = persisted_status.growing_env_configured;
    channel->config_status.compensation_configured = persisted_status.compensation_configured;
    channel->config_status.custom_soil_configured = persisted_status.custom_soil_configured;
    channel->config_status.interval_configured = persisted_status.interval_configured;
    channel->config_status.configuration_score = persisted_status.configuration_score;
    channel->config_status.last_reset_timestamp = persisted_status.last_reset_timestamp;

    watering_error_t save_err = watering_save_config();
    if (save_err != WATERING_SUCCESS) {
        LOG_WRN("Failed to persist channel %u configuration after reset: %d", channel_id, save_err);
    }

    config_status_reset_log_add_entry(channel_id, group, reason, timestamp);

    LOG_INF("Channel %u configuration reset for group %d", channel_id, group);

    return WATERING_SUCCESS;
}

/* Internal helper function implementations */

static int config_status_save_to_nvs(uint8_t channel_id, const channel_config_status_t *status)
{
    if (channel_id >= WATERING_CHANNELS_COUNT || !status) {
        return -WATERING_ERROR_INVALID_PARAM;
    }

    // Use numeric ID instead of string key
    uint16_t nvs_id = 930 + channel_id;  // ID_CONFIG_STATUS_BASE + channel_id

    return nvs_config_write(nvs_id, status, sizeof(channel_config_status_t));
}

static int config_status_load_from_nvs(uint8_t channel_id, channel_config_status_t *status)
{
    if (channel_id >= WATERING_CHANNELS_COUNT || !status) {
        return -WATERING_ERROR_INVALID_PARAM;
    }

    // Use numeric ID instead of string key
    uint16_t nvs_id = 930 + channel_id;  // ID_CONFIG_STATUS_BASE + channel_id

    return nvs_config_read(nvs_id, status, sizeof(channel_config_status_t));
}

static bool validate_basic_configuration(const enhanced_watering_channel_t *channel)
{
    if (!channel) {
        return false;
    }

    // Check if plant type is configured (all types are valid including OTHER)
    // No validation needed as enum ensures valid values

    // Check if soil type is configured (either standard or custom)
    if (!channel->soil.use_custom_soil) {
        // For standard soil, check if a valid type is selected (first enum value is valid)
        // All enum values are valid, no need to check for "unknown"
    } else {
        if (strlen(channel->soil.custom.name) == 0 ||
            channel->soil.custom.field_capacity <= 0.0f ||
            channel->soil.custom.wilting_point <= 0.0f) {
            return false;
        }
    }

    // Check if irrigation method is configured
    // All enum values are valid irrigation methods

    return true;
}

static bool validate_growing_environment_configuration(const enhanced_watering_channel_t *channel)
{
    if (!channel) {
        return false;
    }

    // Check if coverage is configured
    if (channel->coverage.use_area) {
        if (channel->coverage.area.area_m2 <= 0.0f) {
            return false;
        }
    } else {
        if (channel->coverage.plants.count == 0) {
            return false;
        }
    }

    // Check if sun exposure is configured (0-100%)
    // Sun exposure is optional - skip validation

    // Check if water factor is configured (should be > 0)
    // Water factor is optional - skip validation

    return true;
}

static bool validate_compensation_configuration(const enhanced_watering_channel_t *channel)
{
    if (!channel) {
        return false;
    }

    // Check if at least one compensation method is configured
    bool rain_configured = channel->rain_compensation.enabled &&
                          channel->rain_compensation.sensitivity > 0.0f &&
                          channel->rain_compensation.lookback_hours > 0;

    bool temp_configured = channel->temp_compensation.enabled &&
                          channel->temp_compensation.base_temperature > -50.0f &&
                          channel->temp_compensation.base_temperature < 60.0f &&
                          channel->temp_compensation.sensitivity > 0.0f;

    return rain_configured || temp_configured;
}

static bool validate_custom_soil_configuration(const enhanced_watering_channel_t *channel)
{
    if (!channel) {
        return false;
    }

    // Custom soil is optional, but if enabled, it should be properly configured
    if (!channel->soil.use_custom_soil) {
        return false;  // Not using custom soil
    }

    // Validate custom soil parameters
    if (strlen(channel->soil.custom.name) == 0) {
        return false;
    }

    if (channel->soil.custom.field_capacity <= 0.0f || 
        channel->soil.custom.field_capacity > 100.0f) {
        return false;
    }

    if (channel->soil.custom.wilting_point <= 0.0f || 
        channel->soil.custom.wilting_point >= channel->soil.custom.field_capacity) {
        return false;
    }

    if (channel->soil.custom.infiltration_rate <= 0.0f) {
        return false;
    }

    return true;
}

static bool validate_interval_channel_configuration(const enhanced_watering_channel_t *channel)
{
    if (!channel) {
        return false;
    }

    // Interval configuration is optional
    if (!channel->interval_config.configured) {
        return false;
    }

    // Validate interval timing
    uint32_t watering_sec = (channel->interval_config.watering_minutes * 60) + 
                           channel->interval_config.watering_seconds;
    uint32_t pause_sec = (channel->interval_config.pause_minutes * 60) + 
                        channel->interval_config.pause_seconds;

    if (watering_sec < 1 || watering_sec > 3600) {
        return false;
    }

    if (pause_sec < 1 || pause_sec > 3600) {
        return false;
    }

    return true;
}

/**
 * @brief Initialize the configuration status system
 * 
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t configuration_status_init(void)
{
    LOG_INF("Configuration status system initialized");
    return WATERING_SUCCESS;
}
