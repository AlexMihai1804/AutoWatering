/**
 * @file irrigation_methods_db.c
 * @brief Irrigation methods database implementation for irrigation system
 * Generated from irrigation_methods.csv - 15 irrigation methods with efficiency and application parameters
 */

#include "irrigation_methods_db.inc"

const irrigation_method_data_t irrigation_methods_database[IRRIGATION_METHODS_COUNT] = {
    { // [0] Surface Flood (Level Basin)
        .method_id = 0,
        .method_name = "Surface Flood (Level Basin)",
        .code_enum = "IRRIG_SURFACE_FLOOD",
        .efficiency_pct = 50,
        .wetting_fraction_x1000 = 1000,
        .distribution_uniformity_pct = 60,
        .depth_typical_min_mm = 40,
        .depth_typical_max_mm = 80,
        .application_rate_min_mm_h = 15,
        .application_rate_max_mm_h = 25
    },
    { // [1] Border Strip
        .method_id = 1,
        .method_name = "Border Strip",
        .code_enum = "IRRIG_SURFACE_BORDER",
        .efficiency_pct = 55,
        .wetting_fraction_x1000 = 1000,
        .distribution_uniformity_pct = 65,
        .depth_typical_min_mm = 40,
        .depth_typical_max_mm = 70,
        .application_rate_min_mm_h = 10,
        .application_rate_max_mm_h = 20
    },
    { // [2] Furrow
        .method_id = 2,
        .method_name = "Furrow",
        .code_enum = "IRRIG_SURFACE_FURROW",
        .efficiency_pct = 60,
        .wetting_fraction_x1000 = 500,
        .distribution_uniformity_pct = 70,
        .depth_typical_min_mm = 30,
        .depth_typical_max_mm = 60,
        .application_rate_min_mm_h = 5,
        .application_rate_max_mm_h = 15
    },
    { // [3] Conventional Sprinkler (Hand/Set)
        .method_id = 3,
        .method_name = "Conventional Sprinkler (Hand/Set)",
        .code_enum = "IRRIG_SPRINKLER_SET",
        .efficiency_pct = 70,
        .wetting_fraction_x1000 = 1000,
        .distribution_uniformity_pct = 75,
        .depth_typical_min_mm = 15,
        .depth_typical_max_mm = 35,
        .application_rate_min_mm_h = 5,
        .application_rate_max_mm_h = 10
    },
    { // [4] Center Pivot Mid-Pressure
        .method_id = 4,
        .method_name = "Center Pivot Mid-Pressure",
        .code_enum = "IRRIG_SPRINKLER_PIVOT",
        .efficiency_pct = 80,
        .wetting_fraction_x1000 = 1000,
        .distribution_uniformity_pct = 85,
        .depth_typical_min_mm = 15,
        .depth_typical_max_mm = 30,
        .application_rate_min_mm_h = 6,
        .application_rate_max_mm_h = 12
    },
    { // [5] Low Energy Precision (LEPA/Drop)
        .method_id = 5,
        .method_name = "Low Energy Precision (LEPA/Drop)",
        .code_enum = "IRRIG_SPRINKLER_LEPA",
        .efficiency_pct = 90,
        .wetting_fraction_x1000 = 850,
        .distribution_uniformity_pct = 90,
        .depth_typical_min_mm = 10,
        .depth_typical_max_mm = 25,
        .application_rate_min_mm_h = 5,
        .application_rate_max_mm_h = 8
    },
    { // [6] Micro-sprayer/Microjet
        .method_id = 6,
        .method_name = "Micro-sprayer/Microjet",
        .code_enum = "IRRIG_MICRO_SPRAY",
        .efficiency_pct = 85,
        .wetting_fraction_x1000 = 600,
        .distribution_uniformity_pct = 85,
        .depth_typical_min_mm = 10,
        .depth_typical_max_mm = 25,
        .application_rate_min_mm_h = 2,
        .application_rate_max_mm_h = 6
    },
    { // [7] Drip Surface (Line+Emitters)
        .method_id = 7,
        .method_name = "Drip Surface (Line+Emitters)",
        .code_enum = "IRRIG_DRIP_SURFACE",
        .efficiency_pct = 90,
        .wetting_fraction_x1000 = 300,
        .distribution_uniformity_pct = 90,
        .depth_typical_min_mm = 8,
        .depth_typical_max_mm = 20,
        .application_rate_min_mm_h = 1,
        .application_rate_max_mm_h = 3
    },
    { // [8] Drip Subsurface (SDI)
        .method_id = 8,
        .method_name = "Drip Subsurface (SDI)",
        .code_enum = "IRRIG_DRIP_SUBSURFACE",
        .efficiency_pct = 92,
        .wetting_fraction_x1000 = 250,
        .distribution_uniformity_pct = 92,
        .depth_typical_min_mm = 8,
        .depth_typical_max_mm = 20,
        .application_rate_min_mm_h = 1,
        .application_rate_max_mm_h = 2
    },
    { // [9] Drip Inline Tape (Row Crops)
        .method_id = 9,
        .method_name = "Drip Inline Tape (Row Crops)",
        .code_enum = "IRRIG_DRIP_TAPE",
        .efficiency_pct = 88,
        .wetting_fraction_x1000 = 350,
        .distribution_uniformity_pct = 88,
        .depth_typical_min_mm = 10,
        .depth_typical_max_mm = 25,
        .application_rate_min_mm_h = 1,
        .application_rate_max_mm_h = 3
    },
    { // [10] Bubbler (Tree Basin)
        .method_id = 10,
        .method_name = "Bubbler (Tree Basin)",
        .code_enum = "IRRIG_BUBBLER",
        .efficiency_pct = 80,
        .wetting_fraction_x1000 = 100,
        .distribution_uniformity_pct = 80,
        .depth_typical_min_mm = 20,
        .depth_typical_max_mm = 40,
        .application_rate_min_mm_h = 20,
        .application_rate_max_mm_h = 40
    },
    { // [11] Sub-Irrigation (Controlled Water Table)
        .method_id = 11,
        .method_name = "Sub-Irrigation (Controlled Water Table)",
        .code_enum = "IRRIG_SUBIRRIGATION",
        .efficiency_pct = 90,
        .wetting_fraction_x1000 = 1000,
        .distribution_uniformity_pct = 85,
        .depth_typical_min_mm = 30,
        .depth_typical_max_mm = 60,
        .application_rate_min_mm_h = 1,
        .application_rate_max_mm_h = 6
    },
    { // [12] Wicking Bed (Capillary)
        .method_id = 12,
        .method_name = "Wicking Bed (Capillary)",
        .code_enum = "IRRIG_WICK_BED",
        .efficiency_pct = 85,
        .wetting_fraction_x1000 = 1000,
        .distribution_uniformity_pct = 85,
        .depth_typical_min_mm = 15,
        .depth_typical_max_mm = 30,
        .application_rate_min_mm_h = 1,
        .application_rate_max_mm_h = 6
    },
    { // [13] Hydroponic Recirculating (NFT/Drip Fertigation)
        .method_id = 13,
        .method_name = "Hydroponic Recirculating (NFT/Drip Fertigation)",
        .code_enum = "IRRIG_HYDROPONIC_RECIRC",
        .efficiency_pct = 95,
        .wetting_fraction_x1000 = 1000,
        .distribution_uniformity_pct = 95,
        .depth_typical_min_mm = 10,
        .depth_typical_max_mm = 20,
        .application_rate_min_mm_h = 1,
        .application_rate_max_mm_h = 6
    },
    { // [14] Mist / Aeroponic
        .method_id = 14,
        .method_name = "Mist / Aeroponic",
        .code_enum = "IRRIG_AEROPONIC",
        .efficiency_pct = 90,
        .wetting_fraction_x1000 = 1000,
        .distribution_uniformity_pct = 90,
        .depth_typical_min_mm = 10,
        .depth_typical_max_mm = 20,
        .application_rate_min_mm_h = 1,
        .application_rate_max_mm_h = 6
    }
};
