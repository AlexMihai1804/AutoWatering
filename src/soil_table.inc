/**
 * @file soil_table.inc
 * @brief Soil type definitions for irrigation system
 * Generated from soil_table.csv
 */

#ifndef SOIL_TABLE_INC
#define SOIL_TABLE_INC

#include <stdint.h>

typedef struct {
    const char *soil_type;
    const char *texture;
    uint16_t fc_pctvol_x100;      // Field capacity % × 100
    uint16_t pwp_pctvol_x100;     // Permanent wilting point % × 100
    uint16_t awc_mm_per_m;        // Available water capacity mm/m
    uint16_t infil_mm_h;          // Infiltration rate mm/h
    uint16_t p_raw_x1000;         // RAW depletion factor × 1000
} soil_data_t;

#define SOIL_TYPES_COUNT 8

extern const soil_data_t soil_types[SOIL_TYPES_COUNT];

#endif // SOIL_TABLE_INC
