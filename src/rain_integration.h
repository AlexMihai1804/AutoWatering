#ifndef RAIN_INTEGRATION_H
#define RAIN_INTEGRATION_H

#include <stdint.h>
#include <stdbool.h>
#include "watering.h"

/**
 * @file rain_integration.h
 * @brief Rain sensor integration with automatic irrigation system
 * 
 * This module provides integration between rain sensor data and the
 * automatic irrigation system, allowing for intelligent watering
 * adjustments based on recent precipitation.
 */

/**
 * @brief Rain impact on irrigation structure
 */
typedef struct {
    float recent_rainfall_mm;      /**< Recent rainfall amount (mm) */
    float effective_rainfall_mm;   /**< Effective rainfall after soil infiltration */
    float irrigation_reduction_pct; /**< Percentage reduction in irrigation (0-100%) */
    bool skip_irrigation;          /**< Flag to completely skip irrigation */
    uint32_t calculation_time;     /**< When this calculation was performed */
    uint8_t confidence_level;      /**< Confidence in calculation (0-100%) */
} rain_irrigation_impact_t;

/**
 * @brief Rain integration configuration
 */
typedef struct {
    float rain_sensitivity_pct;    /**< Rain sensitivity (0-100%) */
    float skip_threshold_mm;       /**< mm threshold to skip irrigation */
    float effective_rain_factor;   /**< Soil infiltration efficiency (0.0-1.0) */
    uint32_t lookback_hours;       /**< Hours to look back for rain data */
    bool integration_enabled;      /**< Enable/disable integration */
} rain_integration_config_t;

/**
 * @brief Default rain integration configuration
 */
#define DEFAULT_RAIN_INTEGRATION_CONFIG { \
    .rain_sensitivity_pct = 75.0f,     /* 75% sensitivity */ \
    .skip_threshold_mm = 5.0f,         /* Skip after 5mm rain */ \
    .effective_rain_factor = 0.8f,     /* 80% infiltration efficiency */ \
    .lookback_hours = 48,              /* Look back 48 hours */ \
    .integration_enabled = true        /* Enabled by default */ \
}

/**
 * @brief Initialize rain integration system
 * 
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t rain_integration_init(void);

/**
 * @brief Deinitialize rain integration system
 * 
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t rain_integration_deinit(void);

/**
 * @brief Calculate rain impact on irrigation for a channel
 * 
 * @param channel_id Channel ID (0-7)
 * @return Rain impact structure with calculated adjustments
 */
rain_irrigation_impact_t rain_integration_calculate_impact(uint8_t channel_id);

/**
 * @brief Adjust watering task based on recent rainfall
 * 
 * @param channel_id Channel ID (0-7)
 * @param task Pointer to watering task to adjust
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t rain_integration_adjust_task(uint8_t channel_id, 
                                              watering_task_t *task);

/**
 * @brief Check if irrigation should be skipped due to rain
 * 
 * @param channel_id Channel ID (0-7)
 * @return true if irrigation should be skipped, false otherwise
 */
bool rain_integration_should_skip_irrigation(uint8_t channel_id);

/**
 * @brief Get recommended irrigation reduction percentage
 * 
 * @param channel_id Channel ID (0-7)
 * @return Reduction percentage (0-100%)
 */
float rain_integration_get_reduction_percentage(uint8_t channel_id);

/**
 * @brief Set rain integration configuration
 * 
 * @param config New configuration
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t rain_integration_set_config(const rain_integration_config_t *config);

/**
 * @brief Get current rain integration configuration
 * 
 * @param config Buffer to store current configuration
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t rain_integration_get_config(rain_integration_config_t *config);

/**
 * @brief Set rain sensitivity percentage
 * 
 * @param sensitivity_pct Sensitivity percentage (0-100%)
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t rain_integration_set_sensitivity(float sensitivity_pct);

/**
 * @brief Get current rain sensitivity percentage
 * 
 * @return Current sensitivity percentage
 */
float rain_integration_get_sensitivity(void);

/**
 * @brief Set skip threshold for irrigation
 * 
 * @param threshold_mm Rainfall threshold in mm to skip irrigation
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t rain_integration_set_skip_threshold(float threshold_mm);

/**
 * @brief Get current skip threshold
 * 
 * @return Current skip threshold in mm
 */
float rain_integration_get_skip_threshold(void);

/**
 * @brief Set effective rain factor (soil infiltration efficiency)
 * 
 * @param factor Infiltration efficiency (0.0-1.0)
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t rain_integration_set_effective_rain_factor(float factor);

/**
 * @brief Get current effective rain factor
 * 
 * @return Current effective rain factor
 */
float rain_integration_get_effective_rain_factor(void);

/**
 * @brief Set lookback period for rain analysis
 * 
 * @param hours Number of hours to look back
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t rain_integration_set_lookback_hours(uint32_t hours);

/**
 * @brief Get current lookback period
 * 
 * @return Current lookback period in hours
 */
uint32_t rain_integration_get_lookback_hours(void);

/**
 * @brief Enable or disable rain integration
 * 
 * @param enabled true to enable, false to disable
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t rain_integration_set_enabled(bool enabled);

/**
 * @brief Check if rain integration is enabled
 * 
 * @return true if enabled, false if disabled
 */
bool rain_integration_is_enabled(void);

/**
 * @brief Calculate effective rainfall based on soil type and irrigation method
 * 
 * @param rainfall_mm Raw rainfall amount
 * @param channel_id Channel ID for soil type lookup
 * @return Effective rainfall amount after infiltration losses
 */
float rain_integration_calculate_effective_rainfall(float rainfall_mm, uint8_t channel_id);

/**
 * @brief Get recent rainfall summary for display
 * 
 * @param summary Buffer to store rainfall summary string
 * @param buffer_size Size of the summary buffer
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t rain_integration_get_rainfall_summary(char *summary, uint16_t buffer_size);

/**
 * @brief Save rain integration configuration to NVS
 * 
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t rain_integration_save_config(void);

/**
 * @brief Load rain integration configuration from NVS
 * 
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t rain_integration_load_config(void);

/**
 * @brief Validate rain integration configuration
 * 
 * @param config Configuration to validate
 * @return WATERING_SUCCESS if valid, error code if invalid
 */
watering_error_t rain_integration_validate_config(const rain_integration_config_t *config);

/**
 * @brief Reset rain integration configuration to defaults
 * 
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t rain_integration_reset_config(void);

/**
 * @brief Debug function to print rain integration information
 */
void rain_integration_debug_info(void);

/**
 * @brief Test rain integration calculations with simulated data
 * 
 * @param rainfall_mm Simulated rainfall amount
 * @param channel_id Channel to test
 * @return Rain impact structure with test results
 */
rain_irrigation_impact_t rain_integration_test_calculation(float rainfall_mm, uint8_t channel_id);

/**
 * @brief Enhanced impact calculation with comprehensive error handling
 * 
 * @param channel_id Channel ID (0-7)
 * @return Rain impact structure with calculated adjustments and error handling
 */
rain_irrigation_impact_t rain_integration_calculate_impact_enhanced(uint8_t channel_id);

/**
 * @brief Get integration diagnostic information
 * 
 * @param buffer Buffer to store diagnostic information
 * @param buffer_size Size of the buffer
 * @return Number of bytes written, or negative error code
 */
int rain_integration_get_diagnostics(char *buffer, uint16_t buffer_size);

/**
 * @brief Reset integration diagnostic data
 */
void rain_integration_reset_diagnostics(void);

/**
 * @brief Periodic health check for rain integration system
 * 
 * Should be called periodically for system health monitoring.
 */
void rain_integration_periodic_health_check(void);

#endif // RAIN_INTEGRATION_H