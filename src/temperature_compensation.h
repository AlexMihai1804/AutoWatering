#ifndef TEMPERATURE_COMPENSATION_H
#define TEMPERATURE_COMPENSATION_H

#include "watering_enhanced.h"

/**
 * @file temperature_compensation.h
 * @brief Temperature compensation calculation engine for AutoWatering system
 * 
 * This module provides temperature-based compensation calculations for
 * automatic watering modes, adjusting water requirements based on current
 * temperature conditions relative to a base temperature.
 */

/* Default temperature compensation parameters */
#define TEMP_COMP_DEFAULT_BASE_TEMP     20.0f   // Default base temperature (°C)
#define TEMP_COMP_DEFAULT_SENSITIVITY   0.05f   // Default sensitivity factor
#define TEMP_COMP_DEFAULT_MIN_FACTOR    0.5f    // Minimum compensation factor
#define TEMP_COMP_DEFAULT_MAX_FACTOR    2.0f    // Maximum compensation factor

/* Temperature compensation calculation parameters */
#define TEMP_COMP_MIN_TEMP_C           -10.0f   // Minimum valid temperature
#define TEMP_COMP_MAX_TEMP_C            50.0f   // Maximum valid temperature
#define TEMP_COMP_MIN_SENSITIVITY       0.01f   // Minimum sensitivity
#define TEMP_COMP_MAX_SENSITIVITY       0.20f   // Maximum sensitivity

/**
 * @brief Initialize temperature compensation configuration with defaults
 * @param config Pointer to temperature compensation configuration
 * @return 0 on success, negative error code on failure
 */
int temp_compensation_init_config(temperature_compensation_config_t *config);

/**
 * @brief Validate temperature compensation configuration
 * @param config Pointer to temperature compensation configuration
 * @return 0 if valid, negative error code if invalid
 */
int temp_compensation_validate_config(const temperature_compensation_config_t *config);

/**
 * @brief Calculate temperature compensation factor
 * @param config Pointer to temperature compensation configuration
 * @param current_temp Current temperature reading in °C
 * @param result Pointer to compensation result structure
 * @return 0 on success, negative error code on failure
 */
int temp_compensation_calculate(const temperature_compensation_config_t *config,
                               float current_temp,
                               temperature_compensation_result_t *result);

/**
 * @brief Apply temperature compensation to water requirement
 * @param base_requirement Base water requirement (duration in seconds or volume in ml)
 * @param compensation_factor Compensation factor from calculation
 * @param compensated_requirement Pointer to store compensated requirement
 * @return 0 on success, negative error code on failure
 */
int temp_compensation_apply(uint32_t base_requirement,
                           float compensation_factor,
                           uint32_t *compensated_requirement);

/**
 * @brief Calculate temperature compensation for FAO-56 based watering
 * @param config Pointer to temperature compensation configuration
 * @param current_temp Current temperature reading in °C
 * @param base_et0 Base reference evapotranspiration (mm/day)
 * @param compensated_et0 Pointer to store compensated ET0
 * @return 0 on success, negative error code on failure
 */
int temp_compensation_calculate_et0(const temperature_compensation_config_t *config,
                                   float current_temp,
                                   float base_et0,
                                   float *compensated_et0);

/**
 * @brief Get temperature compensation factor for given temperature difference
 * @param config Pointer to temperature compensation configuration
 * @param temp_diff Temperature difference from base (current - base)
 * @return Compensation factor (clamped to min/max limits)
 */
float temp_compensation_get_factor(const temperature_compensation_config_t *config,
                                  float temp_diff);

/**
 * @brief Check if temperature reading is valid for compensation
 * @param temperature Temperature reading to validate
 * @return true if valid, false if invalid
 */
bool temp_compensation_is_temp_valid(float temperature);

/**
 * @brief Get human-readable description of compensation effect
 * @param compensation_factor Compensation factor
 * @param description Buffer to store description (min 64 chars)
 * @return 0 on success, negative error code on failure
 */
int temp_compensation_get_description(float compensation_factor, char *description);

/**
 * @brief Calculate temperature trend for improved compensation
 * @param temps Array of recent temperature readings
 * @param count Number of temperature readings
 * @param trend_factor Pointer to store trend factor
 * @return 0 on success, negative error code on failure
 */
int temp_compensation_calculate_trend(const float *temps, uint8_t count, float *trend_factor);

/**
 * @brief Update compensation configuration with new parameters
 * @param config Pointer to temperature compensation configuration
 * @param base_temp New base temperature
 * @param sensitivity New sensitivity factor
 * @param min_factor New minimum factor
 * @param max_factor New maximum factor
 * @return 0 on success, negative error code on failure
 */
int temp_compensation_update_config(temperature_compensation_config_t *config,
                                   float base_temp,
                                   float sensitivity,
                                   float min_factor,
                                   float max_factor);

/**
 * @brief Initialize temperature compensation system
 * @return WATERING_ERROR_NONE on success, error code on failure
 */
watering_error_t temperature_compensation_init(void);

#endif // TEMPERATURE_COMPENSATION_H