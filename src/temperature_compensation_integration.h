#ifndef TEMPERATURE_COMPENSATION_INTEGRATION_H
#define TEMPERATURE_COMPENSATION_INTEGRATION_H

#include "temperature_compensation.h"
#include "fao56_calc.h"
#include "watering_enhanced.h"

/**
 * @file temperature_compensation_integration.h
 * @brief Integration of temperature compensation with growing environment mode
 * 
 * This module integrates temperature compensation calculations with the
 * FAO-56 based automatic watering modes, adjusting water requirements
 * based on current temperature conditions.
 */

/**
 * @brief Apply temperature compensation to FAO-56 irrigation calculation
 * @param channel_id Channel ID for the calculation
 * @param env Environmental data including current temperature
 * @param base_result Base irrigation calculation result from FAO-56
 * @param compensated_result Pointer to store temperature-compensated result
 * @return 0 on success, negative error code on failure
 */
int temp_comp_apply_to_fao56(uint8_t channel_id,
                            const environmental_data_t *env,
                            const irrigation_calculation_t *base_result,
                            irrigation_calculation_t *compensated_result);

/**
 * @brief Calculate temperature-compensated ET0 for FAO-56 calculations
 * @param config Temperature compensation configuration
 * @param env Environmental data
 * @param latitude_rad Latitude in radians
 * @param day_of_year Day of year (1-365)
 * @param compensated_et0 Pointer to store compensated ET0 result
 * @return 0 on success, negative error code on failure
 */
int temp_comp_calculate_compensated_et0(const temperature_compensation_config_t *config,
                                       const environmental_data_t *env,
                                       float latitude_rad,
                                       uint16_t day_of_year,
                                       float *compensated_et0);

/**
 * @brief Apply temperature compensation to automatic quality mode
 * @param channel_id Channel ID
 * @param env Environmental data
 * @param base_result Base quality mode calculation result
 * @param compensated_result Pointer to store compensated result
 * @return 0 on success, negative error code on failure
 */
int temp_comp_apply_to_quality_mode(uint8_t channel_id,
                                   const environmental_data_t *env,
                                   const irrigation_calculation_t *base_result,
                                   irrigation_calculation_t *compensated_result);

/**
 * @brief Apply temperature compensation to automatic eco mode
 * @param channel_id Channel ID
 * @param env Environmental data
 * @param base_result Base eco mode calculation result
 * @param compensated_result Pointer to store compensated result
 * @return 0 on success, negative error code on failure
 */
int temp_comp_apply_to_eco_mode(uint8_t channel_id,
                               const environmental_data_t *env,
                               const irrigation_calculation_t *base_result,
                               irrigation_calculation_t *compensated_result);

/**
 * @brief Get temperature compensation status for a channel
 * @param channel_id Channel ID
 * @param result Pointer to store temperature compensation result
 * @return 0 on success, negative error code on failure
 */
int temp_comp_get_channel_status(uint8_t channel_id,
                                temperature_compensation_result_t *result);

/**
 * @brief Update temperature compensation configuration for a channel
 * @param channel_id Channel ID
 * @param config New temperature compensation configuration
 * @return 0 on success, negative error code on failure
 */
int temp_comp_update_channel_config(uint8_t channel_id,
                                   const temperature_compensation_config_t *config);

/**
 * @brief Check if temperature compensation should be applied
 * @param channel_id Channel ID
 * @param mode Watering mode
 * @param should_apply Pointer to store result
 * @return 0 on success, negative error code on failure
 */
int temp_comp_should_apply(uint8_t channel_id,
                          watering_mode_t mode,
                          bool *should_apply);

/**
 * @brief Apply temperature compensation with fallback handling
 * @param channel_id Channel ID
 * @param env Environmental data (may have invalid temperature)
 * @param base_result Base calculation result
 * @param compensated_result Pointer to store result
 * @return 0 on success, negative error code on failure
 */
int temp_comp_apply_with_fallback(uint8_t channel_id,
                                 const environmental_data_t *env,
                                 const irrigation_calculation_t *base_result,
                                 irrigation_calculation_t *compensated_result);

/**
 * @brief Log temperature compensation application
 * @param channel_id Channel ID
 * @param compensation_result Temperature compensation result
 * @param base_volume Original volume before compensation
 * @param final_volume Final volume after compensation
 */
void temp_comp_log_application(uint8_t channel_id,
                              const temperature_compensation_result_t *compensation_result,
                              uint32_t base_volume,
                              uint32_t final_volume);

/**
 * @brief Validate environmental data for temperature compensation
 * @param env Environmental data to validate
 * @param is_valid Pointer to store validation result
 * @param fallback_temp Pointer to store fallback temperature if needed
 * @return 0 on success, negative error code on failure
 */
int temp_comp_validate_environmental_data(const environmental_data_t *env,
                                         bool *is_valid,
                                         float *fallback_temp);

/**
 * @brief Calculate temperature compensation with trend analysis
 * @param channel_id Channel ID
 * @param current_temp Current temperature
 * @param recent_temps Array of recent temperature readings
 * @param temp_count Number of recent temperature readings
 * @param result Pointer to store compensation result with trend
 * @return 0 on success, negative error code on failure
 */
int temp_comp_calculate_with_trend(uint8_t channel_id,
                                  float current_temp,
                                  const float *recent_temps,
                                  uint8_t temp_count,
                                  temperature_compensation_result_t *result);

/**
 * @brief Initialize temperature compensation integration system
 * @return WATERING_ERROR_NONE on success, error code on failure
 */
watering_error_t temperature_compensation_integration_init(void);

#endif // TEMPERATURE_COMPENSATION_INTEGRATION_H