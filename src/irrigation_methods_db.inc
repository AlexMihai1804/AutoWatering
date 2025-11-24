/**
 * @file irrigation_methods_db.inc
 * @brief Irrigation methods database definitions for irrigation system
 * Generated from irrigation_methods.csv - 15 irrigation methods with efficiency and application parameters
 */

#ifndef IRRIGATION_METHODS_DB_INC
#define IRRIGATION_METHODS_DB_INC

#include <stdint.h>

typedef struct {
    uint8_t method_id;
    const char *method_name;
    const char *code_enum;
    
    // Efficiency Parameters
    uint8_t efficiency_pct;            // Application efficiency %
    uint16_t wetting_fraction_x1000;   // Wetted area fraction × 1000
    uint8_t distribution_uniformity_pct; // Distribution uniformity %
    
    // Application Characteristics
    uint8_t depth_typical_min_mm;      // Minimum application depth
    uint8_t depth_typical_max_mm;      // Maximum application depth
    uint8_t application_rate_min_mm_h; // Minimum application rate
    uint8_t application_rate_max_mm_h; // Maximum application rate
} irrigation_method_data_t;

#define IRRIGATION_METHODS_COUNT 15

extern const irrigation_method_data_t irrigation_methods_database[IRRIGATION_METHODS_COUNT];

// Irrigation method indices for quick lookup
#define IRRIG_SURFACE_FLOOD 0
#define IRRIG_SURFACE_BORDER 1
#define IRRIG_SURFACE_FURROW 2
#define IRRIG_SPRINKLER_SET 3
#define IRRIG_SPRINKLER_PIVOT 4
#define IRRIG_SPRINKLER_LEPA 5
#define IRRIG_MICRO_SPRAY 6
#define IRRIG_DRIP_SURFACE 7
#define IRRIG_DRIP_SUBSURFACE 8
#define IRRIG_DRIP_TAPE 9
#define IRRIG_BUBBLER 10
#define IRRIG_SUBIRRIGATION 11
#define IRRIG_WICK_BED 12
#define IRRIG_HYDROPONIC_RECIRC 13
#define IRRIG_AEROPONIC 14

#endif // IRRIGATION_METHODS_DB_INC
