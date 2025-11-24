/**
 * @file plant_db.h
 * @brief Plant database API for irrigation system
 * 
 * This file provides access to the plant database that contains
 * crop coefficients, water requirements, and other irrigation-related
 * parameters for different plant species.
 */

#ifndef PLANT_DB_H
#define PLANT_DB_H

#include <stdbool.h>
#include "plant_full_db.inc"
#include "soil_enhanced_db.inc"
#include "irrigation_methods_db.inc"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Search for a plant species by name
 * 
 * @param species_name Name of the plant species to search for
 * @return Pointer to plant data if found, NULL otherwise
 */
const plant_full_data_t *plant_db_find_species(const char *species_name);

/**
 * @brief Get plant data by index
 * 
 * @param index Index in the plant database (0 to PLANT_FULL_SPECIES_COUNT-1)
 * @return Pointer to plant data if valid index, NULL otherwise
 */
const plant_full_data_t *plant_db_get_by_index(uint16_t index);

/* Deprecated: plant_db_get_by_category() removed (data model has no 'category' field). */

/**
 * @brief Get crop coefficient for a plant at a specific growth stage
 * 
 * @param plant_data Pointer to plant data
 * @param growth_stage Growth stage (0=initial, 1=mid, 2=end)
 * @return Crop coefficient value (actual value, not x1000)
 */
float plant_db_get_crop_coefficient(const plant_full_data_t *plant_data, uint8_t growth_stage);

/**
 * @brief Get water requirement factor for a plant species
 * 
 * @param species_name Name of the plant species
 * @param growth_stage Growth stage (0=initial, 1=mid, 2=end)
 * @return Water requirement factor (1.0 = normal, >1.0 = more water, <1.0 = less water)
 */
float plant_db_get_water_factor(const char *species_name, uint8_t growth_stage);

/**
 * @brief Check if a plant species exists in the database
 * 
 * @param species_name Name of the plant species to check
 * @return true if species exists, false otherwise
 */
bool plant_db_species_exists(const char *species_name);

/**
 * @brief Get the total number of plant species in the database
 * 
 * @return Total number of plant species
 */
uint16_t plant_db_get_species_count(void);

/**
 * @brief Get soil data by index
 * 
 * @param index Index in the soil database (0 to SOIL_ENHANCED_TYPES_COUNT-1)
 * @return Pointer to soil data if valid index, NULL otherwise
 */
const soil_enhanced_data_t *soil_db_get_by_index(uint8_t index);

/**
 * @brief Get irrigation method data by index
 * 
 * @param index Index in the irrigation methods database (0 to IRRIGATION_METHODS_COUNT-1)
 * @return Pointer to irrigation method data if valid index, NULL otherwise
 */
const irrigation_method_data_t *irrigation_db_get_by_index(uint8_t index);

/**
 * @brief Get plant by partial name match (case-insensitive)
 * 
 * @param partial_name Partial name to search for
 * @return Pointer to plant data if found, NULL otherwise
 */
const plant_full_data_t *plant_db_find_species_partial(const char *partial_name);

/**
 * @brief Get recommended minimum irrigation amount for a plant
 * 
 * @param plant_data Pointer to plant data
 * @return Minimum irrigation amount in mm
 */
uint8_t plant_db_get_min_irrigation_mm(const plant_full_data_t *plant_data);

/**
 * @brief Get root depth for a plant species
 * 
 * @param plant_data Pointer to plant data
 * @return Root depth in meters
 */
float plant_db_get_root_depth_meters(const plant_full_data_t *plant_data);

/**
 * @brief Get deficit resistance factor for a plant
 * 
 * @param plant_data Pointer to plant data
 * @return Deficit resistance factor (0.0-1.0)
 */
float plant_db_get_deficit_resistance(const plant_full_data_t *plant_data);

#ifdef __cplusplus
}
#endif

#endif // PLANT_DB_H
