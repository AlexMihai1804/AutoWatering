/**
 * @file plant_db.inc
 * @brief Plant database definitions for irrigation system
 * Generated from plant_db.csv
 */

#ifndef PLANT_DB_INC
#define PLANT_DB_INC

#include <stdint.h>

typedef struct {
    const char *category;
    const char *species;
    uint16_t kc_i_x1000;         // Initial crop coefficient × 1000
    uint16_t kc_mid_x1000;       // Mid-season crop coefficient × 1000
    uint16_t kc_end_x1000;       // End-season crop coefficient × 1000
    uint16_t root_m_x1000;       // Root depth in meters × 1000
    uint8_t raw_pct;             // RAW depletion percentage
    uint8_t mm_min;              // Minimum irrigation mm
    uint16_t deficit_resist_x1000; // Deficit resistance × 1000
} plant_data_t;

#define PLANT_SPECIES_COUNT 407
#define PLANT_CATEGORIES_COUNT 9

extern const plant_data_t plant_database[PLANT_SPECIES_COUNT];

// Category indices for quick lookup
#define CATEGORY_VEGETABLES 0
#define CATEGORY_HERBS 1
#define CATEGORY_FLOWERS 2
#define CATEGORY_SHRUBS 3
#define CATEGORY_TREES 4
#define CATEGORY_FRUITS 5
#define CATEGORY_BERRIES 6
#define CATEGORY_GRAINS 7
#define CATEGORY_SUCCULENTS 8

#endif // PLANT_DB_INC
