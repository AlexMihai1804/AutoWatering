#ifndef CONFIGURATION_STATUS_H
#define CONFIGURATION_STATUS_H

#include <stdint.h>
#include <stdbool.h>
#include "watering_enhanced.h"
#include "watering.h"

/**
 * @file configuration_status.h
 * @brief Comprehensive configuration status tracking system
 * 
 * This module provides configuration status assessment for all configuration
 * groups with completeness scoring and persistent flag management.
 */

/* Configuration validation error types */
typedef enum {
    CONFIG_VALIDATION_ERROR_NONE = 0,
    CONFIG_VALIDATION_ERROR_NOT_CONFIGURED,
    CONFIG_VALIDATION_ERROR_INCOMPLETE,
    CONFIG_VALIDATION_ERROR_INVALID_PARAMETERS
} config_validation_error_t;

/* System configuration health levels */
typedef enum {
    CONFIG_HEALTH_POOR = 0,      // < 40% configured
    CONFIG_HEALTH_FAIR = 1,      // 40-59% configured
    CONFIG_HEALTH_GOOD = 2,      // 60-79% configured
    CONFIG_HEALTH_EXCELLENT = 3  // 80%+ configured
} config_health_level_t;

/* Missing configuration items structure */
typedef struct {
    bool missing_basic;                    // Basic configuration missing
    bool missing_growing_env;              // Growing environment missing
    bool missing_compensation;             // Compensation settings missing
    bool missing_custom_soil;              // Custom soil missing
    bool missing_interval;                 // Interval settings missing
    
    char basic_details[64];                // Details about missing basic items
    char growing_env_details[64];          // Details about missing growing env items
    char compensation_details[64];         // Details about missing compensation items
    char custom_soil_details[64];          // Details about missing custom soil items
    char interval_details[64];             // Details about missing interval items
    
    uint8_t total_missing_count;           // Total number of missing groups
} config_missing_items_t;

/* Configuration validation result */
typedef struct {
    uint8_t channel_id;                    // Channel being validated
    bool can_water;                        // Whether automatic watering is allowed
    config_validation_error_t validation_error; // Validation error type
    uint8_t configuration_score;           // Configuration completeness score (0-100)
    char error_message[128];               // Detailed error message
    char recommendations[128];             // Recommendations for improvement
} config_validation_result_t;

/* System-wide configuration overview */
typedef struct {
    uint8_t channel_scores[WATERING_CHANNELS_COUNT]; // Individual channel scores
    uint8_t overall_system_score;          // Average system score
    uint8_t channels_ready_for_auto_watering; // Channels that can perform auto watering
    uint8_t fully_configured_channels;     // Channels with 100% configuration
    
    // Group statistics
    uint8_t channels_with_basic;           // Channels with basic configuration
    uint8_t channels_with_growing_env;     // Channels with growing environment
    uint8_t channels_with_compensation;    // Channels with compensation
    uint8_t channels_with_custom_soil;     // Channels with custom soil
    uint8_t channels_with_interval;        // Channels with interval settings
    
    config_health_level_t system_health;  // Overall system health level
} config_system_overview_t;

/**
 * @brief Assess configuration status for a specific channel
 * @param channel_id Channel ID to assess (0-7)
 * @param channel Pointer to enhanced channel configuration
 * @param status Pointer to status structure to fill
 * @return 0 on success, negative error code on failure
 */
int config_status_assess_channel(uint8_t channel_id, const enhanced_watering_channel_t *channel,
                                channel_config_status_t *status);

/**
 * @brief Calculate configuration completeness score
 * @param status Pointer to configuration status
 * @return Configuration score (0-100)
 */
uint8_t config_status_calculate_score(const channel_config_status_t *status);

/**
 * @brief Check if channel can perform automatic watering
 * @param status Pointer to configuration status
 * @return true if automatic watering is allowed, false otherwise
 */
bool config_status_can_perform_automatic_watering(const channel_config_status_t *status);

/**
 * @brief Get detailed information about missing configuration items
 * @param status Pointer to configuration status
 * @param missing_items Pointer to structure to fill with missing items
 * @return 0 on success, negative error code on failure
 */
int config_status_get_missing_items(const channel_config_status_t *status,
                                   config_missing_items_t *missing_items);

/**
 * @brief Update configuration flags for a specific group
 * @param channel_id Channel ID (0-7)
 * @param group Configuration group to update
 * @param configured Whether the group is now configured
 * @return 0 on success, negative error code on failure
 */
int config_status_update_flags(uint8_t channel_id, config_group_t group, bool configured);

/**
 * @brief Retrieve configuration reset log for a channel
 * @param channel_id Channel ID (0-7)
 * @param log Pointer to store reset log entries
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t config_status_get_reset_log(uint8_t channel_id, config_reset_log_t *log);

/**
 * @brief Get system-wide configuration overview
 * @param overview Pointer to overview structure to fill
 * @return 0 on success, negative error code on failure
 */
int config_status_get_system_overview(config_system_overview_t *overview);

/**
 * @brief Validate channel configuration for watering operations
 * @param channel_id Channel ID to validate (0-7)
 * @param result Pointer to validation result structure to fill
 * @return 0 on success, negative error code on failure
 */
int config_status_validate_for_watering(uint8_t channel_id, config_validation_result_t *result);

/**
 * @brief Reset all configuration flags for a channel
 * @param channel_id Channel ID (0-7)
 * @return 0 on success, negative error code on failure
 */
int config_status_reset_channel_flags(uint8_t channel_id);

/**
 * @brief Initialize the configuration status system
 * @return WATERING_ERROR_NONE on success, error code on failure
 */
watering_error_t configuration_status_init(void);

#endif // CONFIGURATION_STATUS_H
