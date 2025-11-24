/**
 * @file soil_enhanced_db.c
 * @brief Enhanced soil database implementation for irrigation system
 * Generated from soil_db_new.csv - 15 soil types with hydraulic properties
 */

#include "soil_enhanced_db.inc"

const soil_enhanced_data_t soil_enhanced_database[SOIL_ENHANCED_TYPES_COUNT] = {
    { // [0] Sand (Coarse sand)
        .soil_id = 0,
        .soil_type = "Sand",
        .texture = "Coarse sand",
        .fc_pctvol_x100 = 1600,
        .pwp_pctvol_x100 = 600,
        .awc_mm_per_m = 100,
        .infil_mm_h = 25,
        .p_raw_x1000 = 300
    },
    { // [1] LoamySand (Loamy sand)
        .soil_id = 1,
        .soil_type = "LoamySand",
        .texture = "Loamy sand",
        .fc_pctvol_x100 = 1800,
        .pwp_pctvol_x100 = 800,
        .awc_mm_per_m = 100,
        .infil_mm_h = 20,
        .p_raw_x1000 = 350
    },
    { // [2] SandyLoam (Sandy loam)
        .soil_id = 2,
        .soil_type = "SandyLoam",
        .texture = "Sandy loam",
        .fc_pctvol_x100 = 2300,
        .pwp_pctvol_x100 = 1000,
        .awc_mm_per_m = 130,
        .infil_mm_h = 15,
        .p_raw_x1000 = 400
    },
    { // [3] Loam (Loam)
        .soil_id = 3,
        .soil_type = "Loam",
        .texture = "Loam",
        .fc_pctvol_x100 = 3500,
        .pwp_pctvol_x100 = 1700,
        .awc_mm_per_m = 180,
        .infil_mm_h = 11,
        .p_raw_x1000 = 500
    },
    { // [4] SiltLoam (Silt loam)
        .soil_id = 4,
        .soil_type = "SiltLoam",
        .texture = "Silt loam",
        .fc_pctvol_x100 = 3900,
        .pwp_pctvol_x100 = 1900,
        .awc_mm_per_m = 200,
        .infil_mm_h = 9,
        .p_raw_x1000 = 500
    },
    { // [5] SandyClayLoam (Sandy clay loam)
        .soil_id = 5,
        .soil_type = "SandyClayLoam",
        .texture = "Sandy clay loam",
        .fc_pctvol_x100 = 3300,
        .pwp_pctvol_x100 = 1800,
        .awc_mm_per_m = 150,
        .infil_mm_h = 8,
        .p_raw_x1000 = 450
    },
    { // [6] ClayLoam (Clay loam)
        .soil_id = 6,
        .soil_type = "ClayLoam",
        .texture = "Clay loam",
        .fc_pctvol_x100 = 3700,
        .pwp_pctvol_x100 = 2100,
        .awc_mm_per_m = 160,
        .infil_mm_h = 6,
        .p_raw_x1000 = 500
    },
    { // [7] SiltyClayLoam (Silty clay loam)
        .soil_id = 7,
        .soil_type = "SiltyClayLoam",
        .texture = "Silty clay loam",
        .fc_pctvol_x100 = 4100,
        .pwp_pctvol_x100 = 2700,
        .awc_mm_per_m = 140,
        .infil_mm_h = 5,
        .p_raw_x1000 = 550
    },
    { // [8] SandyClay (Sandy clay)
        .soil_id = 8,
        .soil_type = "SandyClay",
        .texture = "Sandy clay",
        .fc_pctvol_x100 = 3800,
        .pwp_pctvol_x100 = 2500,
        .awc_mm_per_m = 130,
        .infil_mm_h = 5,
        .p_raw_x1000 = 550
    },
    { // [9] SiltyClay (Silty clay)
        .soil_id = 9,
        .soil_type = "SiltyClay",
        .texture = "Silty clay",
        .fc_pctvol_x100 = 4400,
        .pwp_pctvol_x100 = 3000,
        .awc_mm_per_m = 140,
        .infil_mm_h = 4,
        .p_raw_x1000 = 550
    },
    { // [10] Clay (Heavy clay)
        .soil_id = 10,
        .soil_type = "Clay",
        .texture = "Heavy clay",
        .fc_pctvol_x100 = 4800,
        .pwp_pctvol_x100 = 3100,
        .awc_mm_per_m = 170,
        .infil_mm_h = 3,
        .p_raw_x1000 = 600
    },
    { // [11] PeatOrganic (High organic peat)
        .soil_id = 11,
        .soil_type = "PeatOrganic",
        .texture = "High organic peat",
        .fc_pctvol_x100 = 6500,
        .pwp_pctvol_x100 = 4000,
        .awc_mm_per_m = 250,
        .infil_mm_h = 6,
        .p_raw_x1000 = 600
    },
    { // [12] GravellyLoam (Stony/gravelly loam)
        .soil_id = 12,
        .soil_type = "GravellyLoam",
        .texture = "Stony/gravelly loam",
        .fc_pctvol_x100 = 3000,
        .pwp_pctvol_x100 = 1900,
        .awc_mm_per_m = 110,
        .infil_mm_h = 18,
        .p_raw_x1000 = 400
    },
    { // [13] PottingMix (Container soilless mix)
        .soil_id = 13,
        .soil_type = "PottingMix",
        .texture = "Container soilless mix",
        .fc_pctvol_x100 = 6000,
        .pwp_pctvol_x100 = 4000,
        .awc_mm_per_m = 200,
        .infil_mm_h = 16,
        .p_raw_x1000 = 500
    },
    { // [14] Hydroponic (No soil (solution))
        .soil_id = 14,
        .soil_type = "Hydroponic",
        .texture = "No soil (solution)",
        .fc_pctvol_x100 = 0,
        .pwp_pctvol_x100 = 0,
        .awc_mm_per_m = 0,
        .infil_mm_h = 0,
        .p_raw_x1000 = 250
    }
};
