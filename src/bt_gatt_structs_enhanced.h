#ifndef BT_GATT_STRUCTS_ENHANCED_H
#define BT_GATT_STRUCTS_ENHANCED_H

/**
 * @file bt_gatt_structs_enhanced.h
 * @brief Enhanced BLE GATT structures with custom soil support
 * 
 * This file extends the existing BLE GATT structures to support
 * custom soil parameters and other advanced irrigation features.
 */

#include <stdint.h>
#include <zephyr/sys/util.h>
#include "bt_gatt_structs.h"
#include "watering_enhanced.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Enhanced channel configuration structure with custom soil support */
struct enhanced_channel_config_data {
    /* Basic channel information (compatible with existing structure) */
    uint8_t channel_id;             /* Channel ID (0-7) */
    char    name[64];               /* Channel name */
    uint8_t auto_enabled;           /* 1=automatic schedule active, 0=disabled */
    
    /* Plant and growing environment fields */
    uint8_t plant_type;             /* Plant type or 255 for custom */
    uint8_t soil_type;              /* Standard soil type or 255 for custom */
    uint8_t irrigation_method;      /* Irrigation method */
    uint8_t coverage_type;          /* 0=area in mÂ², 1=plant count */
    union {
        float area_m2;              /* Area in square meters */
        uint16_t plant_count;       /* Number of individual plants */
    } coverage;
    uint8_t sun_percentage;         /* Percentage of direct sunlight (0-100%) */
    
    /* Custom soil parameters (new fields) */
    uint8_t use_custom_soil;        /* 0=use standard soil, 1=use custom parameters */
    char custom_soil_name[32];      /* Custom soil name */
    float custom_field_capacity;   /* Field capacity percentage (0.0-100.0) */
    float custom_wilting_point;     /* Wilting point percentage (0.0-100.0) */
    float custom_infiltration_rate; /* Infiltration rate (mm/hr) */
    float custom_bulk_density;      /* Bulk density (g/cmÂ³) */
    float custom_organic_matter;    /* Organic matter percentage (0.0-100.0) */
    
    /* Rain compensation settings */
    uint8_t rain_compensation_enabled;  /* 0=disabled, 1=enabled */
    float rain_sensitivity;             /* Sensitivity factor (0.0-1.0) */
    uint16_t rain_lookback_hours;       /* Hours to look back for rain data */
    float rain_skip_threshold_mm;       /* Rain threshold to skip watering */
    float rain_reduction_factor;        /* Factor for duration/volume reduction */
    
    /* Temperature compensation settings */
    uint8_t temp_compensation_enabled;  /* 0=disabled, 1=enabled */
    float temp_base_temperature;        /* Base temperature for calculations (ÂdegC) */
    float temp_sensitivity;             /* Temperature sensitivity factor */
    float temp_min_factor;              /* Minimum compensation factor */
    float temp_max_factor;              /* Maximum compensation factor */
    
    /* Interval mode settings */
    uint8_t interval_mode_enabled;      /* 0=disabled, 1=enabled */
    uint16_t interval_watering_minutes; /* Watering duration in minutes */
    uint8_t interval_watering_seconds;  /* Watering duration in seconds */
    uint16_t interval_pause_minutes;    /* Pause duration in minutes */
    uint8_t interval_pause_seconds;     /* Pause duration in seconds */
    
    /* Configuration status flags */
    uint8_t config_basic_complete;      /* Basic configuration complete flag */
    uint8_t config_growing_env_complete; /* Growing environment complete flag */
    uint8_t config_compensation_complete; /* Compensation settings complete flag */
    uint8_t config_custom_soil_complete; /* Custom soil complete flag */
    uint8_t config_interval_complete;   /* Interval settings complete flag */
    uint8_t config_score;               /* Overall configuration score (0-100) */
    
    /* Timestamps */
    uint32_t last_config_update;        /* Last configuration update timestamp */
    uint32_t custom_soil_created;       /* When custom soil was created */
    uint32_t custom_soil_modified;      /* When custom soil was last modified */
    
    /* Reserved for future expansion */
    uint8_t reserved[8];
} __attribute__((packed));

/* Packed size verification: must stay in sync with documentation.
 * Manual sum (packed, no padding):
 * 1 (channel_id)
 * +64 (name)
 * +1 (auto_enabled)
 * +1 (plant_type)
 * +1 (soil_type)
 * +1 (irrigation_method)
 * +1 (coverage_type)
 * +4 (coverage union)
 * +1 (sun_percentage)
 * +1 (use_custom_soil)
 * +32 (custom_soil_name)
 * +4*5 (five custom soil floats)
 * +1 (rain_compensation_enabled)
 * +4 (rain_sensitivity)
 * +2 (rain_lookback_hours)
 * +4 (rain_skip_threshold_mm)
 * +4 (rain_reduction_factor)
 * +1 (temp_compensation_enabled)
 * +4*4 (four temp compensation floats)
 * +1 (interval_mode_enabled)
 * +2 (interval_watering_minutes)
 * +1 (interval_watering_seconds)
 * +2 (interval_pause_minutes)
 * +1 (interval_pause_seconds)
 * +6 (six config_* flags)
 * +1 (config_score)
 * +4*3 (timestamps)
 * +8 (reserved)
 * = 193 bytes total. */
/* Enforce packed size exactly (see manual sum above) */
BUILD_ASSERT(sizeof(struct enhanced_channel_config_data) == 193,
            "enhanced_channel_config_data unexpected size");

/* Custom soil configuration structure for BLE transfer */
struct custom_soil_config_data {
    uint8_t channel_id;             /* Channel ID (0-7) */
    uint8_t operation;              /* 0=read, 1=create, 2=update, 3=delete */
    char name[32];                  /* Custom soil name */
    float field_capacity;           /* Field capacity percentage */
    float wilting_point;            /* Wilting point percentage */
    float infiltration_rate;        /* Infiltration rate (mm/hr) */
    float bulk_density;             /* Bulk density (g/cmÂ³) */
    float organic_matter;           /* Organic matter percentage */
    uint32_t created_timestamp;     /* Creation timestamp */
    uint32_t modified_timestamp;    /* Last modification timestamp */
    uint32_t crc32;                 /* Data integrity check */
    uint8_t status;                 /* Operation result status */
    uint8_t reserved[3];            /* Reserved for alignment */
} __attribute__((packed));

/* Configuration reset request structure */
struct config_reset_request_data {
    uint8_t channel_id;             /* Channel ID (0-7, 0xFF for all) */
    uint8_t group;                  /* Configuration group to reset (config_group_t) */
    char reason[32];                /* Optional reason for reset */
    uint32_t timestamp;             /* Reset timestamp */
    uint8_t reserved[4];            /* Reserved for future use */
} __attribute__((packed));

/* Configuration reset response structure */
struct config_reset_response_data {
    uint8_t result;                 /* Reset operation result (watering_error_t) */
    uint8_t channel_id;             /* Channel that was reset */
    uint8_t group;                  /* Group that was reset */
    uint8_t new_basic_complete;     /* Updated basic configuration flag */
    uint8_t new_growing_env_complete; /* Updated growing environment flag */
    uint8_t new_compensation_complete; /* Updated compensation flag */
    uint8_t new_custom_soil_complete; /* Updated custom soil flag */
    uint8_t new_interval_complete;  /* Updated interval flag */
    uint8_t new_config_score;       /* Updated configuration score */
    uint8_t reserved[7];            /* Reserved for future use */
} __attribute__((packed));

/* Configuration status query request structure */
struct config_status_request_data {
    uint8_t channel_id;             /* Channel ID to query (0xFF for all) */
    uint8_t include_reset_log;      /* Whether to include reset history */
    uint8_t reserved[6];            /* Reserved for future use */
} __attribute__((packed));

/* Configuration status response structure */
struct config_status_response_data {
    uint8_t channel_id;             /* Channel ID */
    uint8_t basic_complete;         /* Basic configuration complete flag */
    uint8_t growing_env_complete;   /* Growing environment complete flag */
    uint8_t compensation_complete;  /* Compensation settings complete flag */
    uint8_t custom_soil_complete;   /* Custom soil complete flag */
    uint8_t interval_complete;      /* Interval settings complete flag */
    uint8_t config_score;           /* Configuration completeness score (0-100) */
    uint8_t can_auto_water;         /* Whether automatic watering is allowed */
    uint32_t last_reset_timestamp;  /* Last reset timestamp */
    uint8_t reset_count;            /* Number of resets performed */
    uint8_t last_reset_group;       /* Last reset group */
    char last_reset_reason[32];     /* Last reset reason */
    uint8_t reserved[4];            /* Reserved for future use */
} __attribute__((packed));

/* Enhanced current task status with interval mode support */
struct enhanced_task_status_data {
    uint8_t channel_id;             /* Channel ID (0xFF if no active task) */
    uint8_t task_state;             /* enhanced_task_state_t */
    uint8_t task_mode;              /* enhanced_watering_mode_t */
    uint32_t remaining_time;        /* Time remaining in current phase (seconds) */
    uint32_t total_elapsed;         /* Total elapsed time (seconds) */
    uint32_t total_volume;          /* Total volume dispensed (ml) */
    
    /* Interval mode specific fields */
    uint8_t is_interval_mode;       /* Whether task is using interval mode */
    uint8_t currently_watering;     /* Current phase: 1=watering, 0=pausing */
    uint32_t phase_remaining_sec;   /* Seconds remaining in current phase */
    uint32_t cycles_completed;      /* Number of complete cycles */
    uint16_t watering_minutes;      /* Configured watering duration (minutes) */
    uint8_t watering_seconds;       /* Configured watering duration (seconds) */
    uint16_t pause_minutes;         /* Configured pause duration (minutes) */
    uint8_t pause_seconds;          /* Configured pause duration (seconds) */
    
    /* Compensation results */
    float rain_reduction_percentage; /* Rain compensation reduction */
    uint8_t rain_skip_watering;     /* Whether rain caused skip */
    float temp_compensation_factor; /* Temperature compensation factor */
    float temp_adjusted_requirement; /* Temperature adjusted requirement */
    
    /* Timestamps */
    uint32_t task_start_time;       /* When task started */
    uint32_t phase_start_time;      /* When current phase started */
    uint32_t next_phase_time;       /* When next phase will start */
    
    uint8_t reserved[4];            /* Reserved for future use */
} __attribute__((packed));

/* Enforce packed size exactly (see breakdown comment below) */
BUILD_ASSERT(sizeof(struct enhanced_task_status_data) == 60,
            "enhanced_task_status_data unexpected size");

/* Packed size verification (no padding):
 * 3 x u8 = 3
 * + 3 x u32 = 12 (15)
 * + 2 x u8 = 2 (17)
 * + 2 x u32 = 8 (25)
 * + u16 = 2 (27)
 * + u8 =1 (28)
 * + u16 =2 (30)
 * + u8 =1 (31)
 * + 2 x float = 8 (39)
 * + u8 =1 (40)
 * + 2 x float = 8 (48)
 * + 3 x u32 = 12 (60)
 * + reserved[4] = 4 (64) <-- NOTE reserved is declared as 4 bytes already in total calc below
 * Re-evaluated explicit field order gives:
 * channel_id(1) + task_state(1) + task_mode(1) =3
 * remaining_time(4)=7, total_elapsed(4)=11, total_volume(4)=15,
 * is_interval_mode(1)=16, currently_watering(1)=17,
 * phase_remaining_sec(4)=21, cycles_completed(4)=25,
 * watering_minutes(2)=27, watering_seconds(1)=28,
 * pause_minutes(2)=30, pause_seconds(1)=31,
 * rain_reduction_percentage(4)=35, rain_skip_watering(1)=36,
 * temp_compensation_factor(4)=40, temp_adjusted_requirement(4)=44,
 * task_start_time(4)=48, phase_start_time(4)=52, next_phase_time(4)=56,
 * reserved[4]=4 => 60 total. */

/* Environmental data structure for BLE */
struct environmental_data_ble {
    float temperature;              /* Current temperature (degC) */
    float humidity;                 /* Current humidity (%) */
    float pressure;                 /* Current pressure (hPa) */
    uint32_t timestamp;             /* Measurement timestamp */
    uint8_t sensor_status;          /* Sensor health status */
    uint16_t measurement_interval;  /* Current measurement interval (seconds) */
    uint8_t data_quality;           /* Data quality indicator (0-100) */
    uint8_t reserved[4];            /* Reserved for future expansion */
} __attribute__((packed));

/* Compensation status structure for BLE */
struct compensation_status_data {
    uint8_t channel_id;             /* Channel ID */
    
    /* Rain compensation status */
    uint8_t rain_compensation_active; /* Whether rain compensation is active */
    float recent_rainfall_mm;       /* Recent rainfall amount */
    float rain_reduction_percentage; /* Current reduction percentage */
    uint8_t rain_skip_watering;     /* Whether rain caused skip */
    uint32_t rain_calculation_time; /* When rain compensation was calculated */
    
    /* Temperature compensation status */
    uint8_t temp_compensation_active; /* Whether temperature compensation is active */
    float current_temperature;      /* Current temperature reading */
    float temp_compensation_factor; /* Current compensation factor */
    float temp_adjusted_requirement; /* Adjusted water requirement */
    uint32_t temp_calculation_time; /* When temperature compensation was calculated */
    
    /* Overall status */
    uint8_t any_compensation_active; /* Whether any compensation is active */
    uint8_t reserved[7];            /* Reserved for future use */
} __attribute__((packed));

/* Hydraulic status structure for BLE */
struct hydraulic_status_data {
    uint8_t channel_id;             /* Channel ID (0-7) */
    uint8_t profile_type;           /* hydraulic_profile_t */
    uint8_t lock_level;             /* hydraulic_lock_level_t (channel) */
    uint8_t lock_reason;            /* hydraulic_lock_reason_t (channel) */
    uint32_t nominal_flow_ml_min;   /* Learned nominal flow */
    uint16_t ramp_up_time_sec;      /* Learned ramp-up time */
    uint8_t tolerance_high_percent; /* High flow tolerance */
    uint8_t tolerance_low_percent;  /* Low flow tolerance */
    uint8_t is_calibrated;          /* 1 if stable runs met */
    uint8_t monitoring_enabled;     /* 1 if monitoring enabled */
    uint8_t learning_runs;          /* Total learning runs */
    uint8_t stable_runs;            /* Stable learning runs */
    uint8_t estimated;              /* 1 if nominal is estimated */
    uint8_t manual_override_active; /* 1 if manual override active */
    uint16_t reserved0;             /* Reserved */
    uint32_t lock_at_epoch;         /* Channel lock timestamp */
    uint32_t retry_after_epoch;     /* Channel retry after timestamp */
    uint8_t no_flow_runs;           /* Persistent NO_FLOW count */
    uint8_t high_flow_runs;         /* Persistent HIGH_FLOW count */
    uint8_t unexpected_flow_runs;   /* Persistent UNEXPECTED_FLOW count */
    uint8_t reserved1;              /* Reserved */
    uint32_t last_anomaly_epoch;    /* Last anomaly timestamp */
    uint8_t global_lock_level;      /* hydraulic_lock_level_t (global) */
    uint8_t global_lock_reason;     /* hydraulic_lock_reason_t (global) */
    uint16_t reserved2;             /* Reserved */
    uint32_t global_lock_at_epoch;  /* Global lock timestamp */
    uint32_t global_retry_after_epoch; /* Global retry after timestamp */
} __attribute__((packed));

/* Enhanced system configuration structure with BME280 and compensation support */
struct enhanced_system_config_data {
    /* Basic system configuration (compatible with existing structure) */
    uint8_t version;                /* Configuration version (read-only) */
    uint8_t power_mode;             /* 0=Normal, 1=Energy-Saving, 2=Ultra-Low */
    uint32_t flow_calibration;      /* Pulses per liter */
    uint8_t max_active_valves;      /* Always 1 (read-only) */
    uint8_t num_channels;           /* Number of channels (read-only) */
    
    /* Master valve configuration */
    uint8_t master_valve_enabled;   /* 0=disabled, 1=enabled */
    int16_t master_valve_pre_delay; /* Pre-start delay in seconds */
    int16_t master_valve_post_delay; /* Post-stop delay in seconds */
    uint8_t master_valve_overlap_grace; /* Grace period for overlapping tasks */
    uint8_t master_valve_auto_mgmt; /* 0=manual, 1=automatic management */
    uint8_t master_valve_current_state; /* Current state: 0=closed, 1=open */
    
    /* BME280 environmental sensor configuration */
    uint8_t bme280_enabled;         /* 0=disabled, 1=enabled */
    uint16_t bme280_measurement_interval; /* Measurement interval in seconds */
    uint8_t bme280_sensor_status;   /* Current sensor status (read-only) */
    
    /* Global temperature compensation system settings (rain is per-channel only) */
    uint8_t _reserved_rain_enabled; /* DEPRECATED: Was global_rain_compensation_enabled, now unused */
    uint8_t global_temp_compensation_enabled; /* Global temperature compensation enable */
    float _reserved_rain_sensitivity;  /* DEPRECATED: Was global_rain_sensitivity, now unused */
    float global_temp_sensitivity;   /* Default temperature sensitivity */
    uint16_t _reserved_rain_lookback; /* DEPRECATED: Was global_rain_lookback_hours, now unused */
    float _reserved_rain_threshold; /* DEPRECATED: Was global_rain_skip_threshold, now unused */
    float global_temp_base_temperature; /* Default base temperature (°C) */
    
    /* System status indicators */
    uint8_t interval_mode_active_channels; /* Bitmap of channels using interval mode */
    uint8_t compensation_active_channels; /* Bitmap of channels with active compensation */
    uint8_t incomplete_config_channels; /* Bitmap of channels with incomplete config */
    uint8_t environmental_data_quality; /* Overall environmental data quality (0-100) */
    
    /* Timestamps */
    uint32_t last_config_update;    /* Last configuration update timestamp */
    uint32_t last_sensor_reading;   /* Last environmental sensor reading timestamp */
    
    /* Reserved for future expansion */
    uint8_t reserved[4];
} __attribute__((packed));

BUILD_ASSERT(sizeof(struct enhanced_system_config_data) == 56, "enhanced_system_config_data must be 56 bytes");

/* Compile-time size guards for enhanced BLE structs (documentation alignment) */
BUILD_ASSERT(sizeof(struct custom_soil_config_data) == 1 + 1 + 32 + 4*5 + 4 + 4 + 4 + 1 + 3,
            "custom_soil_config_data unexpected size");
BUILD_ASSERT(sizeof(struct config_reset_request_data) == 1 + 1 + 32 + 4 + 4,
            "config_reset_request_data unexpected size");
BUILD_ASSERT(sizeof(struct config_reset_response_data) == 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 7,
            "config_reset_response_data unexpected size");
BUILD_ASSERT(sizeof(struct config_status_request_data) == 1 + 1 + 6,
            "config_status_request_data unexpected size");
BUILD_ASSERT(sizeof(struct config_status_response_data) == 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 4 + 1 + 1 + 32 + 4,
            "config_status_response_data unexpected size");
BUILD_ASSERT(sizeof(struct environmental_data_ble) == 4*3 + 4 + 1 + 2 + 1 + 4,
            "environmental_data_ble unexpected size");
BUILD_ASSERT(sizeof(struct compensation_status_data) == 1 + 1 + 4 + 4 + 1 + 4 + 1 + 4 + 4 + 4 + 4 + 1 + 7,
            "compensation_status_data unexpected size");
BUILD_ASSERT(sizeof(struct hydraulic_status_data) == 48,
            "hydraulic_status_data unexpected size");

#ifdef __cplusplus
}
#endif

#endif // BT_GATT_STRUCTS_ENHANCED_H

