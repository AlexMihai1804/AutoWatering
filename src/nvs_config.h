#ifndef NVS_CONFIG_H
#define NVS_CONFIG_H

/**
 * @file nvs_config.h
 * @brief Declaraţii pentru accesul în NVS (Non-Volatile Storage)
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <zephyr/devicetree.h>            // for DT_ALIAS, DT_REG_*
#include <zephyr/storage/flash_map.h>
#include <zephyr/fs/nvs.h>
#include "rain_config.h"
#include "watering_enhanced.h"

/* Forward declarations for enhanced growing environment types */
/* water_balance_t is defined in fao56_calc.h */
struct watering_channel_t;

#define NVS_SECTOR_SIZE    4096u
#define NVS_DT_NODE        DT_ALIAS(nvs_storage)
#define NVS_OFFSET         DT_REG_ADDR(NVS_DT_NODE)
#define NVS_SIZE           DT_REG_SIZE(NVS_DT_NODE)
/* calculate automatically from DT */
#define NVS_SECTOR_COUNT   (NVS_SIZE / NVS_SECTOR_SIZE)

/* ——— Timezone Configuration ——————————————————————————— */
typedef struct {
    int16_t utc_offset_minutes;  /**< UTC offset in minutes (e.g., 120 for UTC+2) */
    uint8_t dst_enabled;         /**< 1 if DST is enabled, 0 otherwise */
    uint8_t dst_start_month;     /**< DST start month (1-12) */
    uint8_t dst_start_week;      /**< DST start week of month (1-5, 5=last) */
    uint8_t dst_start_dow;       /**< DST start day of week (0=Sunday, 1=Monday, etc.) */
    uint8_t dst_end_month;       /**< DST end month (1-12) */
    uint8_t dst_end_week;        /**< DST end week of month (1-5, 5=last) */
    uint8_t dst_end_dow;         /**< DST end day of week (0=Sunday, 1=Monday, etc.) */
    int16_t dst_offset_minutes;  /**< DST offset in minutes (usually 60) */
} __attribute__((packed)) timezone_config_t;

/* Default timezone configuration now defaults to UTC with no DST. */
#define DEFAULT_TIMEZONE_CONFIG { \
    .utc_offset_minutes = 0, \
    .dst_enabled = 0, \
    .dst_start_month = 0, \
    .dst_start_week = 0, \
    .dst_start_dow = 0, \
    .dst_end_month = 0, \
    .dst_end_week = 0, \
    .dst_end_dow = 0, \
    .dst_offset_minutes = 0 \
}

/* ——— Onboarding State Management ————————————————————— */

/**
 * @brief Onboarding state structure for tracking configuration completeness
 * 
 * This structure tracks which configuration parameters have been set by the user
 * versus those using default values, enabling proper onboarding flow management.
 */
typedef struct {
    // Channel configuration flags (8 channels × 8 bits each)
    uint64_t channel_config_flags;     /**< Bitfield for channel settings */
    
    // System configuration flags
    uint32_t system_config_flags;      /**< Bitfield for system settings */
    
    // Schedule configuration flags  
    uint8_t schedule_config_flags;     /**< Bitfield for schedule settings (8 channels) */
    
    // Overall completion status
    uint8_t onboarding_completion_pct; /**< 0-100% completion */
    
    // Timestamps
    uint32_t last_update_time;         /**< Last state update */
    uint32_t onboarding_start_time;    /**< When onboarding began */
} __attribute__((packed)) onboarding_state_t;

// Channel configuration flag bits
#define CHANNEL_FLAG_PLANT_TYPE_SET     (1 << 0)
#define CHANNEL_FLAG_SOIL_TYPE_SET      (1 << 1)
#define CHANNEL_FLAG_IRRIGATION_METHOD_SET (1 << 2)
#define CHANNEL_FLAG_COVERAGE_SET       (1 << 3)
#define CHANNEL_FLAG_SUN_EXPOSURE_SET   (1 << 4)
#define CHANNEL_FLAG_NAME_SET           (1 << 5)
#define CHANNEL_FLAG_WATER_FACTOR_SET   (1 << 6)
#define CHANNEL_FLAG_ENABLED            (1 << 7)

// System configuration flag bits
#define SYSTEM_FLAG_TIMEZONE_SET        (1 << 0) /* Timezone/DST configuration persisted */
#define SYSTEM_FLAG_FLOW_CALIBRATED     (1 << 1)
#define SYSTEM_FLAG_MASTER_VALVE_SET    (1 << 2)
#define SYSTEM_FLAG_RTC_CONFIGURED      (1 << 3)
#define SYSTEM_FLAG_RAIN_SENSOR_SET     (1 << 4)
#define SYSTEM_FLAG_POWER_MODE_SET      (1 << 5)
#define SYSTEM_FLAG_LOCATION_SET        (1 << 6)
#define SYSTEM_FLAG_INITIAL_SETUP_DONE  (1 << 7)

/* Enhanced channel configuration with flags will be defined after enhanced_channel_config_t */

/**
 * @brief Enhanced system configuration with onboarding flags
 */
typedef struct {
    // Existing system config fields
    uint8_t power_mode;
    uint32_t flow_calibration;
    // master_valve_config_t master_valve;  // Will be added when available
    timezone_config_t timezone;
    
    // New: configuration flags
    uint32_t config_flags;
    uint32_t last_modified_time;
} __attribute__((packed)) system_config_with_flags_t;

/* Default onboarding state */
#define DEFAULT_ONBOARDING_STATE { \
    .channel_config_flags = 0,        /* No channels configured */ \
    .system_config_flags = 0,         /* No system settings configured */ \
    .schedule_config_flags = 0,       /* No schedules configured */ \
    .onboarding_completion_pct = 0,   /* 0% complete */ \
    .last_update_time = 0,            /* Never updated */ \
    .onboarding_start_time = 0        /* Not started */ \
}

/* ——— Enhanced Growing Environment Configuration ————————— */

/**
 * @brief Enhanced channel configuration for growing environment parameters
 * 
 * This structure contains the new growing environment parameters that extend
 * the basic channel configuration with scientific irrigation capabilities.
 */
typedef struct {
    /* Enhanced growing environment configuration */
    uint16_t plant_db_index;           /**< Index into plant_full_database (0-based, UINT16_MAX = not set) */
    uint8_t soil_db_index;             /**< Index into soil_enhanced_database (0-based, UINT8_MAX = not set) */
    uint8_t irrigation_method_index;   /**< Index into irrigation_methods_database (0-based, UINT8_MAX = not set) */
    
    /* Coverage specification */
    bool use_area_based;               /**< True = area-based calculation, false = plant count-based */
    union {
        float area_m2;                 /**< Area in square meters (for area-based) */
        uint16_t plant_count;          /**< Number of plants (for plant-count-based) */
    } coverage;
    
    /* Automatic mode settings */
    uint8_t auto_mode;                 /**< watering_mode_t: WATERING_AUTOMATIC_QUALITY or WATERING_AUTOMATIC_ECO */
    float max_volume_limit_l;          /**< Maximum irrigation volume limit (liters) */
    bool enable_cycle_soak;            /**< Enable cycle and soak for clay soils */
    
    /* Plant lifecycle tracking */
    uint32_t planting_date_unix;       /**< When plants were established (Unix timestamp) */
    uint16_t days_after_planting;      /**< Calculated field - days since planting */
    
    /* Environmental overrides */
    float latitude_deg;                /**< Location latitude for solar calculations */
    uint8_t sun_exposure_pct;          /**< Site-specific sun exposure (0-100%) */
    
    /* Runtime state */
    uint32_t last_calculation_time;    /**< Last automatic calculation timestamp */
} __attribute__((packed)) enhanced_channel_config_t;

/**
 * @brief Enhanced channel configuration with onboarding flags
 */
typedef struct {
    enhanced_channel_config_t config;  /**< Existing configuration */
    uint8_t config_flags;              /**< Configuration state flags */
    uint32_t last_modified_time;       /**< When last modified */
} __attribute__((packed)) enhanced_channel_config_with_flags_t;

/**
 * @brief Water balance state for persistent storage
 * 
 * This structure stores the current water balance state that needs to persist
 * across system reboots to maintain accurate deficit tracking.
 */
typedef struct {
    float rwz_awc_mm;                  /**< Root zone available water capacity */
    float wetting_awc_mm;              /**< Wetted zone AWC (adjusted for irrigation method) */
    float raw_mm;                      /**< Readily available water */
    float current_deficit_mm;          /**< Current water deficit */
    float effective_rain_mm;           /**< Effective precipitation */
    bool irrigation_needed;            /**< Trigger flag */
    uint32_t last_update_time;         /**< Last update timestamp */
    uint8_t data_quality;              /**< Data quality indicator (0-100%) */
} __attribute__((packed)) water_balance_config_t;

/**
 * @brief Automatic calculation system state
 * 
 * Global state for the automatic irrigation calculation system.
 */
typedef struct {
    bool system_enabled;               /**< Global enable/disable for automatic calculations */
    uint32_t calculation_interval_sec; /**< Calculation interval in seconds (default 3600 = 1 hour) */
    uint32_t last_global_calculation;  /**< Last global calculation timestamp */
    uint8_t calculation_failures;      /**< Count of consecutive calculation failures */
    uint8_t sensor_failures;           /**< Count of consecutive sensor failures */
    bool fallback_mode;                /**< True if running in fallback mode due to sensor failures */
} __attribute__((packed)) automatic_calc_state_t;

/* Default enhanced channel configuration */
#define DEFAULT_ENHANCED_CHANNEL_CONFIG { \
    .plant_db_index = UINT16_MAX,         /* Not set */ \
    .soil_db_index = UINT8_MAX,           /* Not set */ \
    .irrigation_method_index = UINT8_MAX, /* Not set */ \
    .use_area_based = true,               /* Default to area-based */ \
    .coverage = { .area_m2 = 1.0f },      /* 1 square meter */ \
    .auto_mode = 0,                       /* WATERING_BY_DURATION (disabled) */ \
    .max_volume_limit_l = 10.0f,          /* 10 liter default limit */ \
    .enable_cycle_soak = false,           /* Disabled by default */ \
    .planting_date_unix = 0,              /* Not set */ \
    .days_after_planting = 0,             /* Not calculated */ \
    .latitude_deg = 45.0f,                /* Default latitude (Romania) */ \
    .sun_exposure_pct = 75,               /* 75% sun exposure */ \
    .last_calculation_time = 0            /* Never calculated */ \
}

/* Default water balance configuration */
#define DEFAULT_WATER_BALANCE_CONFIG { \
    .rwz_awc_mm = 100.0f,                 /* 100mm default AWC */ \
    .wetting_awc_mm = 100.0f,             /* Same as root zone initially */ \
    .raw_mm = 50.0f,                      /* 50% depletion threshold */ \
    .current_deficit_mm = 0.0f,           /* No deficit initially */ \
    .effective_rain_mm = 0.0f,            /* No recent rain */ \
    .irrigation_needed = false,           /* No irrigation needed */ \
    .last_update_time = 0,                /* Never updated */ \
    .data_quality = 50                    /* Medium quality (no sensors) */ \
}

/* Default automatic calculation state */
#define DEFAULT_AUTOMATIC_CALC_STATE { \
    .system_enabled = false,              /* Disabled by default */ \
    .calculation_interval_sec = 3600,     /* 1 hour */ \
    .last_global_calculation = 0,         /* Never calculated */ \
    .calculation_failures = 0,            /* No failures */ \
    .sensor_failures = 0,                 /* No sensor failures */ \
    .fallback_mode = false                /* Normal mode */ \
}

/* ——— Rain History Configuration ————————————————————————— */

/**
 * @brief Compressed rain history header for NVS storage
 */
typedef struct {
    uint16_t entry_count;         /**< Number of entries in compressed data */
    uint32_t oldest_timestamp;    /**< Oldest entry timestamp */
    uint32_t newest_timestamp;    /**< Newest entry timestamp */
    uint32_t checksum;            /**< Data integrity checksum */
    uint16_t compressed_size;     /**< Size of compressed data in bytes */
    uint8_t reserved[4];          /**< Reserved for future use */
} __attribute__((packed)) rain_history_header_t;


/* ——— API public ——————————————————————————————————————— */
int  nvs_config_init(void);
bool nvs_config_is_ready(void);

int  nvs_config_read (uint16_t id, void *data, size_t len);
int  nvs_config_write(uint16_t id, const void *data, size_t len);
int  nvs_config_delete(uint16_t id);

/* Blob read/write aliases for compatibility */
#define nvs_config_read_blob(id, data, len)  nvs_config_read(id, data, len)
#define nvs_config_write_blob(id, data, len) nvs_config_write(id, data, len)

/* ——— convenienţe de nivel înalt ————————————————————— */
int nvs_save_watering_config (const void *cfg,  size_t sz);
int nvs_load_watering_config (void *cfg,        size_t sz);

int nvs_save_channel_config  (uint8_t ch, const void *cfg, size_t sz);
int nvs_load_channel_config  (uint8_t ch,       void *cfg, size_t sz);

int nvs_save_config_reset_log(uint8_t channel_id, const config_reset_log_t *log);
int nvs_load_config_reset_log(uint8_t channel_id, config_reset_log_t *log);
int nvs_clear_config_reset_log(uint8_t channel_id);

int nvs_save_flow_calibration(uint32_t cal);
int nvs_load_flow_calibration(uint32_t *cal);

int nvs_save_days_since_start(uint16_t days);
int nvs_load_days_since_start(uint16_t *days);

int nvs_save_channel_name (uint8_t ch, const char *name);
int nvs_load_channel_name (uint8_t ch, char *name_buf, size_t buf_sz);

/* Timezone configuration functions */
int nvs_save_timezone_config(const timezone_config_t *config);
int nvs_load_timezone_config(timezone_config_t *config);

/* Enhanced Growing Environment Configuration Functions */
int nvs_save_enhanced_channel_config(uint8_t ch, const enhanced_channel_config_t *config);
int nvs_load_enhanced_channel_config(uint8_t ch, enhanced_channel_config_t *config);

int nvs_save_water_balance_config(uint8_t ch, const water_balance_config_t *balance);
int nvs_load_water_balance_config(uint8_t ch, water_balance_config_t *balance);

int nvs_save_automatic_calc_state(const automatic_calc_state_t *state);
int nvs_load_automatic_calc_state(automatic_calc_state_t *state);

/* ——— Enhanced Channel Management Functions ————————————————— */
/* Note: These functions are declared in watering.h where the full struct definition is available */

int nvs_validate_enhanced_config(const enhanced_channel_config_t *config);

/* ——— Bulk Configuration Management Functions ————————————————— */
int nvs_save_all_channel_configs(void);
int nvs_load_all_channel_configs(void);
int nvs_reset_enhanced_configs(void);

/* ——— Configuration Backup and Maintenance Functions ————————————— */
int nvs_backup_configuration(void);
int nvs_get_storage_usage(size_t *used_bytes, size_t *total_bytes);

/* ——— Rain History NVS Functions ————————————————————————————— */
int nvs_save_rain_config(const rain_nvs_config_t *config);
int nvs_load_rain_config(rain_nvs_config_t *config);

int nvs_save_rain_state(const rain_nvs_state_t *state);
int nvs_load_rain_state(rain_nvs_state_t *state);

int nvs_save_rain_hourly_data(const void *hourly_data, uint16_t entry_count, size_t data_size);
int nvs_load_rain_hourly_data(void *hourly_data, uint16_t max_entries, uint16_t *actual_count);

int nvs_save_rain_daily_data(const void *daily_data, uint16_t entry_count, size_t data_size);
int nvs_load_rain_daily_data(void *daily_data, uint16_t max_entries, uint16_t *actual_count);

int nvs_clear_rain_history(void);
int nvs_get_rain_storage_usage(size_t *used_bytes, size_t *total_bytes);

/* ——— Onboarding State NVS Functions ————————————————————————————— */
int nvs_save_onboarding_state(const onboarding_state_t *state);
int nvs_load_onboarding_state(onboarding_state_t *state);
int nvs_save_channel_flags(uint8_t channel_id, uint8_t flags);
int nvs_load_channel_flags(uint8_t channel_id, uint8_t *flags);
int nvs_save_system_flags(uint32_t flags);
int nvs_load_system_flags(uint32_t *flags);
int nvs_clear_onboarding_data(void);

#endif /* NVS_CONFIG_H */
