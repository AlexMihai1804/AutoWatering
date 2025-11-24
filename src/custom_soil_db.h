#ifndef CUSTOM_SOIL_DB_H
#define CUSTOM_SOIL_DB_H

/**
 * @file custom_soil_db.h
 * @brief Custom soil database management for per-channel soil configurations
 * 
 * This module provides functionality to create, read, update, and delete
 * custom soil configurations that can be used instead of the standard
 * soil database entries. Each channel can have its own custom soil
 * parameters stored persistently in NVS.
 */

#include <stdbool.h>
#include <stdint.h>
#include "watering_enhanced.h"
#include "watering.h"
#include "soil_enhanced_db.inc"

// Type alias for compatibility
typedef soil_enhanced_data_t soil_data_t;

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the custom soil database system
 * 
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t custom_soil_db_init(void);

/**
 * @brief Create or update a custom soil configuration for a channel
 * 
 * @param channel_id Channel ID (0-7)
 * @param name Custom soil name (max 31 characters)
 * @param field_capacity Field capacity percentage (0.0-100.0)
 * @param wilting_point Wilting point percentage (0.0-100.0)
 * @param infiltration_rate Infiltration rate in mm/hr (0.1-1000.0)
 * @param bulk_density Bulk density in g/cm³ (0.5-2.5)
 * @param organic_matter Organic matter content percentage (0.0-100.0)
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t custom_soil_db_create(uint8_t channel_id,
                                      const char *name,
                                      float field_capacity,
                                      float wilting_point,
                                      float infiltration_rate,
                                      float bulk_density,
                                      float organic_matter);

/**
 * @brief Read custom soil configuration for a channel
 * 
 * @param channel_id Channel ID (0-7)
 * @param entry Pointer to store the custom soil entry
 * @return WATERING_SUCCESS on success, WATERING_ERROR_NOT_FOUND if no custom soil exists
 */
watering_error_t custom_soil_db_read(uint8_t channel_id, custom_soil_entry_t *entry);

/**
 * @brief Update an existing custom soil configuration
 * 
 * @param channel_id Channel ID (0-7)
 * @param name Updated soil name (max 31 characters)
 * @param field_capacity Updated field capacity percentage (0.0-100.0)
 * @param wilting_point Updated wilting point percentage (0.0-100.0)
 * @param infiltration_rate Updated infiltration rate in mm/hr (0.1-1000.0)
 * @param bulk_density Updated bulk density in g/cm³ (0.5-2.5)
 * @param organic_matter Updated organic matter content percentage (0.0-100.0)
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t custom_soil_db_update(uint8_t channel_id,
                                      const char *name,
                                      float field_capacity,
                                      float wilting_point,
                                      float infiltration_rate,
                                      float bulk_density,
                                      float organic_matter);

/**
 * @brief Delete custom soil configuration for a channel
 * 
 * @param channel_id Channel ID (0-7)
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t custom_soil_db_delete(uint8_t channel_id);

/**
 * @brief Check if a channel has custom soil configuration
 * 
 * @param channel_id Channel ID (0-7)
 * @return true if custom soil exists, false otherwise
 */
bool custom_soil_db_exists(uint8_t channel_id);

/**
 * @brief Validate custom soil parameters
 * 
 * @param field_capacity Field capacity percentage
 * @param wilting_point Wilting point percentage
 * @param infiltration_rate Infiltration rate in mm/hr
 * @param bulk_density Bulk density in g/cm³
 * @param organic_matter Organic matter content percentage
 * @return WATERING_SUCCESS if valid, WATERING_ERROR_CUSTOM_SOIL_INVALID if invalid
 */
watering_error_t custom_soil_db_validate_parameters(float field_capacity,
                                                   float wilting_point,
                                                   float infiltration_rate,
                                                   float bulk_density,
                                                   float organic_matter);

/**
 * @brief Get available water capacity for custom soil
 * 
 * @param entry Pointer to custom soil entry
 * @return Available water capacity in mm/m
 */
float custom_soil_db_get_awc(const custom_soil_entry_t *entry);

/**
 * @brief Get depletion fraction for custom soil (estimated)
 * 
 * @param entry Pointer to custom soil entry
 * @return Estimated depletion fraction (0.0-1.0)
 */
float custom_soil_db_get_depletion_fraction(const custom_soil_entry_t *entry);

/**
 * @brief Convert custom soil to standard soil_enhanced_data_t format
 * 
 * This function converts custom soil parameters to the format expected
 * by FAO-56 calculations and other system components.
 * 
 * @param entry Pointer to custom soil entry
 * @param soil_data Pointer to store converted soil data
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t custom_soil_db_to_enhanced_format(const custom_soil_entry_t *entry,
                                                  soil_enhanced_data_t *soil_data);

/**
 * @brief Get all custom soil configurations
 * 
 * @param entries Array to store custom soil entries (size 8)
 * @param count Pointer to store number of valid entries found
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t custom_soil_db_get_all(custom_soil_entry_t entries[8], uint8_t *count);

/**
 * @brief Clear all custom soil configurations
 * 
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t custom_soil_db_clear_all(void);

/**
 * @brief Get storage usage for custom soil database
 * 
 * @param used_bytes Pointer to store used bytes
 * @param total_bytes Pointer to store total allocated bytes
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t custom_soil_db_get_storage_usage(size_t *used_bytes, size_t *total_bytes);

#ifdef __cplusplus
}
#endif

#endif // CUSTOM_SOIL_DB_H