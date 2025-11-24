/**
 * @file plant_full_db.inc
 * @brief Enhanced plant database definitions
 * Generated from plants_full.csv - 223 species
 */

#ifndef PLANT_FULL_DB_INC
#define PLANT_FULL_DB_INC

#include <stdint.h>

typedef struct {
    const char *subtype_enum;
    const char *common_name_en;
    const char *scientific_name;
    uint16_t kc_ini_x1000;
    uint16_t kc_mid_x1000;
    uint16_t kc_end_x1000;
    uint16_t kc_dev_x1000;
    uint16_t root_depth_min_m_x1000;
    uint16_t root_depth_max_m_x1000;
    uint16_t stage_days_mid;
    uint16_t depletion_fraction_p_x1000;
    uint16_t spacing_row_m_x1000;
    uint16_t spacing_plant_m_x1000;
    uint16_t default_density_plants_m2_x100;
    uint16_t canopy_cover_max_frac_x1000;
    uint8_t stage_days_ini;
    uint8_t stage_days_dev;
    uint8_t stage_days_end;
    int8_t  frost_tolerance_c;
    uint8_t temp_opt_min_c;
    uint8_t temp_opt_max_c;
    uint8_t growth_cycle;
    uint8_t typ_irrig_method_id;
} plant_full_data_t;

#define PLANT_FULL_SPECIES_COUNT 223
#define PLANT_FULL_CATEGORIES_COUNT 10
#define PLANT_FULL_DATA_T_EXPECTED_SIZE 44
_Static_assert(sizeof(plant_full_data_t) == PLANT_FULL_DATA_T_EXPECTED_SIZE, "Unexpected plant_full_data_t size");

#define PLANT_KC_INI(p) ((p)->kc_ini_x1000 / 1000.0f)
#define PLANT_KC_MID(p) ((p)->kc_mid_x1000 / 1000.0f)
#define PLANT_KC_END(p) ((p)->kc_end_x1000 / 1000.0f)
#define PLANT_KC_DEV(p) ((p)->kc_dev_x1000 / 1000.0f)
#define PLANT_ROOT_MIN_M(p) ((p)->root_depth_min_m_x1000 / 1000.0f)
#define PLANT_ROOT_MAX_M(p) ((p)->root_depth_max_m_x1000 / 1000.0f)
#define PLANT_DEPL_FRACTION(p) ((p)->depletion_fraction_p_x1000 / 1000.0f)
#define PLANT_ROW_SPACING_M(p) ((p)->spacing_row_m_x1000 / 1000.0f)
#define PLANT_PLANT_SPACING_M(p) ((p)->spacing_plant_m_x1000 / 1000.0f)
#define PLANT_DENSITY_P_M2(p) ((p)->default_density_plants_m2_x100 / 100.0f)
#define PLANT_CANOPY_MAX_FRAC(p) ((p)->canopy_cover_max_frac_x1000 / 1000.0f)

extern const plant_full_data_t plant_full_database[PLANT_FULL_SPECIES_COUNT];
// Category indices
#define CATEGORY_AGRICULTURE 0
#define CATEGORY_GARDENING 1
#define CATEGORY_LANDSCAPING 2
#define CATEGORY_INDOOR 3
#define CATEGORY_SUCCULENT 4
#define CATEGORY_FRUIT 5
#define CATEGORY_VEGETABLE 6
#define CATEGORY_HERB 7
#define CATEGORY_LAWN 8
#define CATEGORY_SHRUB 9

#endif // PLANT_FULL_DB_INC
