/**
 * @file soil_table.c
 * @brief Soil type data table for irrigation system
 * Generated from soil_table.csv
 */

#include "soil_table.inc"

const soil_data_t soil_types[SOIL_TYPES_COUNT] = {
    { // [0] 0
        .soil_type = "0",
        .texture = "Clay",
        .fc_pctvol_x100 = 4700,
        .pwp_pctvol_x100 = 3000,
        .awc_mm_per_m = 170,
        .infil_mm_h = 3,
        .p_raw_x1000 = 550
    },
    { // [1] 1
        .soil_type = "1",
        .texture = "Sandy",
        .fc_pctvol_x100 = 1500,
        .pwp_pctvol_x100 = 600,
        .awc_mm_per_m = 60,
        .infil_mm_h = 22,
        .p_raw_x1000 = 300
    },
    { // [2] 2
        .soil_type = "2",
        .texture = "Loamy",
        .fc_pctvol_x100 = 3500,
        .pwp_pctvol_x100 = 1700,
        .awc_mm_per_m = 140,
        .infil_mm_h = 11,
        .p_raw_x1000 = 450
    },
    { // [3] 3
        .soil_type = "3",
        .texture = "Silty",
        .fc_pctvol_x100 = 4000,
        .pwp_pctvol_x100 = 2000,
        .awc_mm_per_m = 160,
        .infil_mm_h = 9,
        .p_raw_x1000 = 450
    },
    { // [4] 4
        .soil_type = "4",
        .texture = "Rocky",
        .fc_pctvol_x100 = 1800,
        .pwp_pctvol_x100 = 800,
        .awc_mm_per_m = 75,
        .infil_mm_h = 30,
        .p_raw_x1000 = 350
    },
    { // [5] 5
        .soil_type = "5",
        .texture = "Peaty",
        .fc_pctvol_x100 = 5500,
        .pwp_pctvol_x100 = 3500,
        .awc_mm_per_m = 200,
        .infil_mm_h = 6,
        .p_raw_x1000 = 600
    },
    { // [6] 6
        .soil_type = "6",
        .texture = "PottingMix",
        .fc_pctvol_x100 = 6000,
        .pwp_pctvol_x100 = 4000,
        .awc_mm_per_m = 200,
        .infil_mm_h = 16,
        .p_raw_x1000 = 500
    },
    { // [7] 7
        .soil_type = "7",
        .texture = "Hydroponic",
        .fc_pctvol_x100 = 0,
        .pwp_pctvol_x100 = 0,
        .awc_mm_per_m = 0,
        .infil_mm_h = 0,
        .p_raw_x1000 = 250
    }
};
