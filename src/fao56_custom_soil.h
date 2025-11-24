#ifndef FAO56_CUSTOM_SOIL_H
#define FAO56_CUSTOM_SOIL_H

/**
 * @file fao56_custom_soil.h
 * @brief FAO-56 calculations with custom soil support
 * 
 * This module extends the FAO-56 calculation engine to support custom soil
 * parameters on a per-channel basis, falling back to standard soil database
 * entries when custom parameters are not available or invalid.
 */

#include "fao56_calc.h"
#include "custom_soil_db.h"
#include "watering_enhanced.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Get soil data for FAO-56 calculations with custom soil support
 * 
 * This function retrieves soil data for a channel, using custom soil parameters
 * if available and valid, otherwise falling back to standard soil database.
 * 
 * @param channel_id Channel ID (0-7)
 * @param soil_config Soil configuration from channel
 * @param soil_data Pointer to store soil data for calculations
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t fao56_get_soil_data_with_custom(uint8_t channel_id,
                                                const soil_configuration_t *soil_config,
                                                soil_enhanced_data_t *soil_data);

/**
 * @brief Calculate water balance with custom soil support
 * 
 * Enhanced version of calc_water_balance that supports custom soil parameters.
 * 
 * @param channel_id Channel ID for custom soil lookup
 * @param plant Plant database entry
 * @param soil_config Soil configuration (standard or custom)
 * @param method Irrigation method database entry
 * @param env Environmental data
 * @param root_depth_current_m Current root depth (m)
 * @param balance Water balance structure to update
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t fao56_calc_water_balance_with_custom_soil(uint8_t channel_id,
                                                         const plant_full_data_t *plant,
                                                         const soil_configuration_t *soil_config,
                                                         const irrigation_method_data_t *method,
                                                         const environmental_data_t *env,
                                                         float root_depth_current_m,
                                                         water_balance_t *balance);

/**
 * @brief Calculate effective precipitation with custom soil support
 * 
 * @param rainfall_mm Total rainfall (mm)
 * @param soil_config Soil configuration (standard or custom)
 * @param irrigation_method Irrigation method for runoff assessment
 * @return Effective precipitation (mm)
 */
float fao56_calc_effective_precipitation_with_custom_soil(float rainfall_mm,
                                                        const soil_configuration_t *soil_config,
                                                        const irrigation_method_data_t *irrigation_method);

/**
 * @brief Calculate cycle and soak with custom soil support
 * 
 * @param method Irrigation method database entry
 * @param soil_config Soil configuration (standard or custom)
 * @param application_rate_mm_h Planned application rate (mm/h)
 * @param result Calculation results to update with cycle information
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t fao56_calc_cycle_and_soak_with_custom_soil(const irrigation_method_data_t *method,
                                                          const soil_configuration_t *soil_config,
                                                          float application_rate_mm_h,
                                                          irrigation_calculation_t *result);

/**
 * @brief Calculate localized wetting pattern with custom soil support
 * 
 * @param method Irrigation method database entry
 * @param soil_config Soil configuration (standard or custom)
 * @param emitter_spacing_m Spacing between emitters (m)
 * @param wetted_diameter_m Calculated wetted diameter (m)
 * @param wetted_depth_m Calculated wetted depth (m)
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t fao56_calc_localized_wetting_pattern_with_custom_soil(const irrigation_method_data_t *method,
                                                                     const soil_configuration_t *soil_config,
                                                                     float emitter_spacing_m,
                                                                     float *wetted_diameter_m,
                                                                     float *wetted_depth_m);

/**
 * @brief Adjust irrigation volume for partial wetting with custom soil support
 * 
 * @param base_volume_mm Base irrigation volume for full coverage (mm)
 * @param method Irrigation method database entry
 * @param plant Plant database entry
 * @param soil_config Soil configuration (standard or custom)
 * @return Adjusted irrigation volume (mm)
 */
float fao56_adjust_volume_for_partial_wetting_with_custom_soil(float base_volume_mm,
                                                             const irrigation_method_data_t *method,
                                                             const plant_full_data_t *plant,
                                                             const soil_configuration_t *soil_config);

/**
 * @brief Check irrigation trigger with custom soil support
 * 
 * @param balance Current water balance state
 * @param plant Plant database entry
 * @param soil_config Soil configuration (standard or custom)
 * @param stress_factor Environmental stress factor (0.8-1.2, default 1.0)
 * @return True if irrigation is needed, false otherwise
 */
bool fao56_check_irrigation_trigger_mad_with_custom_soil(const water_balance_t *balance,
                                                       const plant_full_data_t *plant,
                                                       const soil_configuration_t *soil_config,
                                                       float stress_factor);

/**
 * @brief Calculate irrigation requirement with custom soil support
 * 
 * Enhanced version of the main FAO-56 calculation function that supports
 * custom soil parameters on a per-channel basis.
 * 
 * @param channel_id Channel ID
 * @param env Environmental data
 * @param result Calculation result structure to be filled
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t fao56_calculate_irrigation_requirement_with_custom_soil(uint8_t channel_id,
                                                                       const environmental_data_t *env,
                                                                       irrigation_calculation_t *result);

/**
 * @brief Validate custom soil parameters for FAO-56 calculations
 * 
 * This function performs additional validation specific to FAO-56 calculations
 * beyond the basic parameter validation in custom_soil_db.
 * 
 * @param custom_soil Custom soil entry to validate
 * @return WATERING_SUCCESS if valid for FAO-56, error code otherwise
 */
watering_error_t fao56_validate_custom_soil_for_calculations(const custom_soil_entry_t *custom_soil);

/**
 * @brief Get effective AWC with wetting fraction for custom soil
 * 
 * @param soil_config Soil configuration (standard or custom)
 * @param method Irrigation method database entry
 * @param plant Plant database entry
 * @param root_depth_m Current root depth (m)
 * @return Effective AWC considering wetting fraction (mm)
 */
float fao56_calc_effective_awc_with_wetting_fraction_custom_soil(const soil_configuration_t *soil_config,
                                                               const irrigation_method_data_t *method,
                                                               const plant_full_data_t *plant,
                                                               float root_depth_m);

/**
 * @brief Log custom soil usage for debugging and monitoring
 * 
 * @param channel_id Channel ID using custom soil
 * @param custom_soil Custom soil entry being used
 * @param calculation_type Type of calculation using custom soil
 */
void fao56_log_custom_soil_usage(uint8_t channel_id,
                               const custom_soil_entry_t *custom_soil,
                               const char *calculation_type);

#ifdef __cplusplus
}
#endif

#endif // FAO56_CUSTOM_SOIL_H