#ifndef RAIN_COMPENSATION_H
#define RAIN_COMPENSATION_H

/**
 * @file rain_compensation.h
 * @brief Rain compensation calculation engine for advanced irrigation modes
 * 
 * This module provides comprehensive rain compensation calculations that
 * integrate with the enhanced watering system to automatically adjust
 * irrigation based on recent precipitation data.
 */

#include <stdint.h>
#include <stdbool.h>
#include "watering_enhanced.h"
#include "watering.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Rain compensation calculation parameters
 */
typedef struct {
    float recent_rainfall_mm;           /**< Recent rainfall amount (mm) */
    float effective_rainfall_mm;        /**< Effective rainfall after soil infiltration (mm) */
    float base_water_requirement_mm;    /**< Base water requirement before compensation (mm) */
    float adjusted_requirement_mm;      /**< Adjusted requirement after compensation (mm) */
    float reduction_percentage;         /**< Percentage reduction applied (0-100%) */
    bool skip_watering;                 /**< Whether to skip watering entirely */
    uint32_t calculation_timestamp;     /**< When calculation was performed */
    uint8_t confidence_level;           /**< Confidence in calculation (0-100%) */
    watering_error_t calculation_status; /**< Status of the calculation */
} rain_compensation_calculation_t;

/**
 * @brief Rain compensation algorithm types
 */
typedef enum {
    RAIN_COMP_ALGORITHM_SIMPLE,         /**< Simple threshold-based algorithm */
    RAIN_COMP_ALGORITHM_PROPORTIONAL,   /**< Proportional reduction algorithm */
    RAIN_COMP_ALGORITHM_EXPONENTIAL,    /**< Exponential decay algorithm */
    RAIN_COMP_ALGORITHM_ADAPTIVE        /**< Adaptive algorithm based on soil and plant */
} rain_compensation_algorithm_t;

/**
 * @brief Initialize the rain compensation calculation engine
 * 
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t rain_compensation_init(void);

/**
 * @brief Calculate rain compensation for a specific channel
 * 
 * This is the main entry point for rain compensation calculations.
 * 
 * @param channel_id Channel ID (0-7)
 * @param config Rain compensation configuration for the channel
 * @param base_requirement_mm Base water requirement before compensation (mm)
 * @param result Pointer to store calculation results
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t rain_compensation_calculate(uint8_t channel_id,
                                           const rain_compensation_config_t *config,
                                           float base_requirement_mm,
                                           rain_compensation_calculation_t *result);

/**
 * @brief Calculate rain compensation using simple threshold algorithm
 * 
 * This algorithm uses a simple threshold approach where irrigation is
 * skipped if recent rainfall exceeds the threshold, otherwise no reduction.
 * 
 * @param channel_id Channel ID (0-7)
 * @param config Rain compensation configuration
 * @param base_requirement_mm Base water requirement (mm)
 * @param result Pointer to store calculation results
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t rain_compensation_calculate_simple(uint8_t channel_id,
                                                   const rain_compensation_config_t *config,
                                                   float base_requirement_mm,
                                                   rain_compensation_calculation_t *result);

/**
 * @brief Calculate rain compensation using proportional algorithm
 * 
 * This algorithm applies proportional reduction based on the ratio of
 * recent rainfall to the skip threshold.
 * 
 * @param channel_id Channel ID (0-7)
 * @param config Rain compensation configuration
 * @param base_requirement_mm Base water requirement (mm)
 * @param result Pointer to store calculation results
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t rain_compensation_calculate_proportional(uint8_t channel_id,
                                                         const rain_compensation_config_t *config,
                                                         float base_requirement_mm,
                                                         rain_compensation_calculation_t *result);

/**
 * @brief Calculate rain compensation using exponential decay algorithm
 * 
 * This algorithm applies exponential decay based on time since rainfall,
 * with more recent rain having greater impact.
 * 
 * @param channel_id Channel ID (0-7)
 * @param config Rain compensation configuration
 * @param base_requirement_mm Base water requirement (mm)
 * @param result Pointer to store calculation results
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t rain_compensation_calculate_exponential(uint8_t channel_id,
                                                        const rain_compensation_config_t *config,
                                                        float base_requirement_mm,
                                                        rain_compensation_calculation_t *result);

/**
 * @brief Calculate rain compensation using adaptive algorithm
 * 
 * This algorithm adapts the compensation based on soil type, plant type,
 * and environmental conditions for optimal water management.
 * 
 * @param channel_id Channel ID (0-7)
 * @param config Rain compensation configuration
 * @param base_requirement_mm Base water requirement (mm)
 * @param result Pointer to store calculation results
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t rain_compensation_calculate_adaptive(uint8_t channel_id,
                                                     const rain_compensation_config_t *config,
                                                     float base_requirement_mm,
                                                     rain_compensation_calculation_t *result);

/**
 * @brief Get recent rainfall data for compensation calculations
 * 
 * @param channel_id Channel ID (0-7)
 * @param lookback_hours Hours to look back for rainfall data
 * @param total_rainfall_mm Pointer to store total rainfall amount
 * @param effective_rainfall_mm Pointer to store effective rainfall amount
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t rain_compensation_get_recent_rainfall(uint8_t channel_id,
                                                      uint16_t lookback_hours,
                                                      float *total_rainfall_mm,
                                                      float *effective_rainfall_mm);

/**
 * @brief Calculate effective rainfall based on soil infiltration
 * 
 * @param channel_id Channel ID for soil type lookup
 * @param total_rainfall_mm Total rainfall amount (mm)
 * @param effective_rainfall_mm Pointer to store effective rainfall (mm)
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t rain_compensation_calculate_effective_rainfall(uint8_t channel_id,
                                                              float total_rainfall_mm,
                                                              float *effective_rainfall_mm);

/**
 * @brief Apply rain compensation to watering duration
 * 
 * @param original_duration_sec Original watering duration (seconds)
 * @param compensation_result Rain compensation calculation result
 * @param adjusted_duration_sec Pointer to store adjusted duration (seconds)
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t rain_compensation_apply_to_duration(uint32_t original_duration_sec,
                                                    const rain_compensation_calculation_t *compensation_result,
                                                    uint32_t *adjusted_duration_sec);

/**
 * @brief Apply rain compensation to watering volume
 * 
 * @param original_volume_ml Original watering volume (ml)
 * @param compensation_result Rain compensation calculation result
 * @param adjusted_volume_ml Pointer to store adjusted volume (ml)
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t rain_compensation_apply_to_volume(uint32_t original_volume_ml,
                                                  const rain_compensation_calculation_t *compensation_result,
                                                  uint32_t *adjusted_volume_ml);

/**
 * @brief Validate rain compensation configuration
 * 
 * @param config Rain compensation configuration to validate
 * @return WATERING_SUCCESS if valid, error code if invalid
 */
watering_error_t rain_compensation_validate_config(const rain_compensation_config_t *config);

/**
 * @brief Get default rain compensation configuration
 * 
 * @param config Pointer to store default configuration
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t rain_compensation_get_default_config(rain_compensation_config_t *config);

/**
 * @brief Calculate confidence level for rain compensation
 * 
 * The confidence level indicates how reliable the compensation calculation is
 * based on data quality, sensor status, and calculation parameters.
 * 
 * @param channel_id Channel ID (0-7)
 * @param config Rain compensation configuration
 * @param rainfall_data_quality Quality of rainfall data (0-100%)
 * @return Confidence level (0-100%)
 */
uint8_t rain_compensation_calculate_confidence(uint8_t channel_id,
                                             const rain_compensation_config_t *config,
                                             uint8_t rainfall_data_quality);

/**
 * @brief Get rain compensation algorithm name
 * 
 * @param algorithm Algorithm type
 * @return Algorithm name string
 */
const char *rain_compensation_get_algorithm_name(rain_compensation_algorithm_t algorithm);

/**
 * @brief Set rain compensation algorithm for calculations
 * 
 * @param algorithm Algorithm to use for calculations
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t rain_compensation_set_algorithm(rain_compensation_algorithm_t algorithm);

/**
 * @brief Get current rain compensation algorithm
 * 
 * @return Current algorithm type
 */
rain_compensation_algorithm_t rain_compensation_get_algorithm(void);

/**
 * @brief Test rain compensation calculation with simulated data
 * 
 * This function is useful for testing and validation of compensation algorithms.
 * 
 * @param channel_id Channel ID (0-7)
 * @param simulated_rainfall_mm Simulated rainfall amount (mm)
 * @param config Rain compensation configuration to test
 * @param base_requirement_mm Base water requirement (mm)
 * @param result Pointer to store test results
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t rain_compensation_test_calculation(uint8_t channel_id,
                                                   float simulated_rainfall_mm,
                                                   const rain_compensation_config_t *config,
                                                   float base_requirement_mm,
                                                   rain_compensation_calculation_t *result);

/**
 * @brief Get rain compensation statistics for monitoring
 * 
 * @param channel_id Channel ID (0-7)
 * @param total_calculations Pointer to store total number of calculations
 * @param skip_count Pointer to store number of times watering was skipped
 * @param avg_reduction_pct Pointer to store average reduction percentage
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t rain_compensation_get_statistics(uint8_t channel_id,
                                                 uint32_t *total_calculations,
                                                 uint32_t *skip_count,
                                                 float *avg_reduction_pct);

/**
 * @brief Reset rain compensation statistics
 * 
 * @param channel_id Channel ID (0-7), or 0xFF for all channels
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t rain_compensation_reset_statistics(uint8_t channel_id);

/**
 * @brief Log rain compensation calculation for debugging
 * 
 * @param channel_id Channel ID
 * @param config Configuration used
 * @param result Calculation result
 * @param additional_info Additional context information
 */
void rain_compensation_log_calculation(uint8_t channel_id,
                                      const rain_compensation_config_t *config,
                                      const rain_compensation_calculation_t *result,
                                      const char *additional_info);

#ifdef __cplusplus
}
#endif

#endif // RAIN_COMPENSATION_H