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
 * @brief Rain integration configuration (DEPRECATED)
 * 
 * This structure is kept for API compatibility only.
 * Rain compensation settings are now configured per-channel via
 * watering_channel_t.rain_compensation structure.
 * 
 * @deprecated Use per-channel rain_compensation settings instead.
 */
typedef struct {
    float rain_sensitivity_pct;    /**< Rain sensitivity (0-100%) - DEPRECATED */
    float skip_threshold_mm;       /**< mm threshold to skip irrigation - DEPRECATED */
    float effective_rain_factor;   /**< Soil infiltration efficiency (0.0-1.0) - DEPRECATED */
    uint32_t lookback_hours;       /**< Hours to look back for rain data - DEPRECATED */
    bool integration_enabled;      /**< Enable/disable integration - DEPRECATED */
} rain_integration_config_t;

/**
 * @brief Default rain integration configuration (DEPRECATED)
 * @deprecated Use per-channel rain_compensation settings instead.
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
 * @brief Set rain integration configuration (DEPRECATED)
 * 
 * @deprecated Rain compensation is now per-channel only. This function does nothing.
 * Configure rain compensation via watering_channel_t.rain_compensation instead.
 * 
 * @param config New configuration (ignored)
 * @return WATERING_SUCCESS always
 */
watering_error_t rain_integration_set_config(const rain_integration_config_t *config);

/**
 * @brief Get current rain integration configuration (DEPRECATED)
 * 
 * @deprecated Rain compensation is now per-channel only. Returns default values.
 * Read rain compensation via watering_channel_t.rain_compensation instead.
 * 
 * @param config Buffer to store default configuration values
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t rain_integration_get_config(rain_integration_config_t *config);

/**
 * @brief Set rain sensitivity percentage (DEPRECATED)
 * 
 * @deprecated Use per-channel watering_channel_t.rain_compensation.sensitivity instead.
 * 
 * @param sensitivity_pct Sensitivity percentage (ignored)
 * @return WATERING_SUCCESS always
 */
watering_error_t rain_integration_set_sensitivity(float sensitivity_pct);

/**
 * @brief Get current rain sensitivity percentage (DEPRECATED)
 * 
 * @deprecated Use per-channel watering_channel_t.rain_compensation.sensitivity instead.
 * 
 * @return Default sensitivity percentage (75.0)
 */
float rain_integration_get_sensitivity(void);

/**
 * @brief Set skip threshold for irrigation (DEPRECATED)
 * 
 * @deprecated Use per-channel watering_channel_t.rain_compensation.skip_threshold_mm instead.
 * 
 * @param threshold_mm Rainfall threshold (ignored)
 * @return WATERING_SUCCESS always
 */
watering_error_t rain_integration_set_skip_threshold(float threshold_mm);

/**
 * @brief Get current skip threshold (DEPRECATED)
 * 
 * @deprecated Use per-channel watering_channel_t.rain_compensation.skip_threshold_mm instead.
 * 
 * @return Default skip threshold (5.0 mm)
 */
float rain_integration_get_skip_threshold(void);

/**
 * @brief Set effective rain factor (soil infiltration efficiency) (DEPRECATED)
 * 
 * @deprecated Use per-channel watering_channel_t.rain_compensation.reduction_factor instead.
 * 
 * @param factor Infiltration efficiency (ignored)
 * @return WATERING_SUCCESS always
 */
watering_error_t rain_integration_set_effective_rain_factor(float factor);

/**
 * @brief Get current effective rain factor (DEPRECATED)
 * 
 * @deprecated Use per-channel watering_channel_t.rain_compensation.reduction_factor instead.
 * 
 * @return Default effective rain factor (0.8)
 */
float rain_integration_get_effective_rain_factor(void);

/**
 * @brief Set lookback period for rain analysis (DEPRECATED)
 * 
 * @deprecated Use per-channel watering_channel_t.rain_compensation.lookback_hours instead.
 * 
 * @param hours Number of hours (ignored)
 * @return WATERING_SUCCESS always
 */
watering_error_t rain_integration_set_lookback_hours(uint32_t hours);

/**
 * @brief Get current lookback period (DEPRECATED)
 * 
 * @deprecated Use per-channel watering_channel_t.rain_compensation.lookback_hours instead.
 * 
 * @return Default lookback period (48 hours)
 */
uint32_t rain_integration_get_lookback_hours(void);

/**
 * @brief Enable or disable rain integration (DEPRECATED)
 * 
 * @deprecated Use per-channel watering_channel_t.rain_compensation.enabled instead.
 * 
 * @param enabled Enable flag (ignored)
 * @return WATERING_SUCCESS always
 */
watering_error_t rain_integration_set_enabled(bool enabled);

/**
 * @brief Check if rain integration is enabled
 * 
 * @return true if any channel has rain compensation enabled, false otherwise
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
 * @brief Save rain integration configuration to NVS (DEPRECATED)
 * 
 * @deprecated Rain compensation settings are saved per-channel as part of watering config.
 * 
 * @return WATERING_SUCCESS always (no-op)
 */
watering_error_t rain_integration_save_config(void);

/**
 * @brief Load rain integration configuration from NVS (DEPRECATED)
 * 
 * @deprecated Rain compensation settings are loaded per-channel as part of watering config.
 * 
 * @return WATERING_SUCCESS always (no-op)
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
 * @brief Reset rain integration configuration to defaults (DEPRECATED)
 * 
 * @deprecated Rain compensation settings are per-channel. Reset individual channels instead.
 * 
 * @return WATERING_SUCCESS always (no-op)
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