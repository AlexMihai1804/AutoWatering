/**
 * @file soil_enhanced_db.inc
 * @brief Enhanced soil database definitions for irrigation system
 * Generated from soil_db_new.csv - 15 soil types with hydraulic properties
 */

#ifndef SOIL_ENHANCED_DB_INC
#define SOIL_ENHANCED_DB_INC

#include <stdint.h>

typedef struct {
    uint8_t soil_id;
    const char *soil_type;
    const char *texture;
    
    // Hydraulic Properties (scaled for embedded storage)
    uint16_t fc_pctvol_x100;           // Field capacity % × 100
    uint16_t pwp_pctvol_x100;          // Permanent wilting point % × 100
    uint16_t awc_mm_per_m;             // Available water capacity mm/m
    uint16_t infil_mm_h;               // Infiltration rate mm/h
    uint16_t p_raw_x1000;              // Default depletion fraction × 1000
} soil_enhanced_data_t;

#define SOIL_ENHANCED_TYPES_COUNT 15

extern const soil_enhanced_data_t soil_enhanced_database[SOIL_ENHANCED_TYPES_COUNT];

// Soil type indices for quick lookup
#define SOIL_SAND 0
#define SOIL_LOAMYSAND 1
#define SOIL_SANDYLOAM 2
#define SOIL_LOAM 3
#define SOIL_SILTLOAM 4
#define SOIL_SANDYCLAYLOAM 5
#define SOIL_CLAYLOAM 6
#define SOIL_SILTYCLAYLOAM 7
#define SOIL_SANDYCLAY 8
#define SOIL_SILTYCLAY 9
#define SOIL_CLAY 10
#define SOIL_PEATORGANIC 11
#define SOIL_GRAVELLYLOAM 12
#define SOIL_POTTINGMIX 13
#define SOIL_HYDROPONIC 14

#endif // SOIL_ENHANCED_DB_INC
