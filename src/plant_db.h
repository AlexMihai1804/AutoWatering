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
#include "plant_db.inc"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Search for a plant species by name
 * 
 * @param species_name Name of the plant species to search for
 * @return Pointer to plant data if found, NULL otherwise
 */
const plant_data_t *plant_db_find_species(const char *species_name);

/**
 * @brief Get plant data by index
 * 
 * @param index Index in the plant database (0 to PLANT_SPECIES_COUNT-1)
 * @return Pointer to plant data if valid index, NULL otherwise
 */
const plant_data_t *plant_db_get_by_index(uint16_t index);

/**
 * @brief Get all plants in a specific category
 * 
 * @param category Category name to search for
 * @param results Array to store pointers to matching plants
 * @param max_results Maximum number of results to return
 * @return Number of plants found in the category
 */
uint16_t plant_db_get_by_category(const char *category, 
                                  const plant_data_t **results, 
                                  uint16_t max_results);

/**
 * @brief Get crop coefficient for a plant at a specific growth stage
 * 
 * @param plant_data Pointer to plant data
 * @param growth_stage Growth stage (0=initial, 1=mid, 2=end)
 * @return Crop coefficient value (actual value, not x1000)
 */
float plant_db_get_crop_coefficient(const plant_data_t *plant_data, uint8_t growth_stage);

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

#ifdef __cplusplus
}
#endif

#endif // PLANT_DB_H
