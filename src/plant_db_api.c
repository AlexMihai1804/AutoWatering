/**
 * @file plant_db_api.c
 * @brief Plant database API implementation for irrigation system
 * 
 * This file provides access functions to the plant database that is
 * generated from plant_db.csv by the Python build tools.
 */

#include "plant_db.h"
#include <string.h>
#include <stddef.h>
#include <stdbool.h>

/**
 * @brief Search for a plant species by name
 */
const plant_data_t *plant_db_find_species(const char *species_name) {
    if (!species_name) {
        return NULL;
    }
    
    for (uint16_t i = 0; i < PLANT_SPECIES_COUNT; i++) {
        if (strcmp(plant_database[i].species, species_name) == 0) {
            return &plant_database[i];
        }
    }
    
    return NULL;
}

/**
 * @brief Get plant data by index
 */
const plant_data_t *plant_db_get_by_index(uint16_t index) {
    if (index >= PLANT_SPECIES_COUNT) {
        return NULL;
    }
    
    return &plant_database[index];
}

/**
 * @brief Get all plants in a specific category
 */
uint16_t plant_db_get_by_category(const char *category, 
                                  const plant_data_t **results, 
                                  uint16_t max_results) {
    if (!category || !results || max_results == 0) {
        return 0;
    }
    
    uint16_t count = 0;
    
    for (uint16_t i = 0; i < PLANT_SPECIES_COUNT && count < max_results; i++) {
        if (strcmp(plant_database[i].category, category) == 0) {
            results[count] = &plant_database[i];
            count++;
        }
    }
    
    return count;
}

/**
 * @brief Get crop coefficient for a plant at a specific growth stage
 */
float plant_db_get_crop_coefficient(const plant_data_t *plant_data, uint8_t growth_stage) {
    if (!plant_data) {
        return 1.0f; // Default coefficient
    }
    
    switch (growth_stage) {
        case 0: // Initial stage
            return (float)plant_data->kc_i_x1000 / 1000.0f;
        case 1: // Mid-season stage
            return (float)plant_data->kc_mid_x1000 / 1000.0f;
        case 2: // End-season stage
            return (float)plant_data->kc_end_x1000 / 1000.0f;
        default:
            return (float)plant_data->kc_mid_x1000 / 1000.0f; // Default to mid-season
    }
}

/**
 * @brief Get water requirement factor for a plant species
 */
float plant_db_get_water_factor(const char *species_name, uint8_t growth_stage) {
    const plant_data_t *plant_data = plant_db_find_species(species_name);
    if (!plant_data) {
        return 1.0f; // Default factor if species not found
    }
    
    return plant_db_get_crop_coefficient(plant_data, growth_stage);
}

/**
 * @brief Check if a plant species exists in the database
 */
bool plant_db_species_exists(const char *species_name) {
    return plant_db_find_species(species_name) != NULL;
}

/**
 * @brief Get the total number of plant species in the database
 */
uint16_t plant_db_get_species_count(void) {
    return PLANT_SPECIES_COUNT;
}

/**
 * @brief Get plant by partial name match (case-insensitive)
 */
const plant_data_t *plant_db_find_species_partial(const char *partial_name) {
    if (!partial_name) {
        return NULL;
    }
    
    for (uint16_t i = 0; i < PLANT_SPECIES_COUNT; i++) {
        // Simple case-insensitive partial match
        const char *species = plant_database[i].species;
        const char *haystack = species;
        const char *needle = partial_name;
        
        while (*haystack && *needle) {
            char h = (*haystack >= 'A' && *haystack <= 'Z') ? *haystack + 32 : *haystack;
            char n = (*needle >= 'A' && *needle <= 'Z') ? *needle + 32 : *needle;
            
            if (h == n) {
                needle++;
                if (*needle == '\0') {
                    return &plant_database[i];
                }
            } else {
                needle = partial_name;
            }
            haystack++;
        }
    }
    
    return NULL;
}

/**
 * @brief Get recommended minimum irrigation amount for a plant
 */
uint8_t plant_db_get_min_irrigation_mm(const plant_data_t *plant_data) {
    if (!plant_data) {
        return 10; // Default minimum
    }
    
    return plant_data->mm_min;
}

/**
 * @brief Get root depth for a plant species
 */
float plant_db_get_root_depth_meters(const plant_data_t *plant_data) {
    if (!plant_data) {
        return 0.5f; // Default root depth
    }
    
    return (float)plant_data->root_m_x1000 / 1000.0f;
}

/**
 * @brief Get deficit resistance factor for a plant
 */
float plant_db_get_deficit_resistance(const plant_data_t *plant_data) {
    if (!plant_data) {
        return 1.0f; // Default resistance
    }
    
    return (float)plant_data->deficit_resist_x1000 / 1000.0f;
}
