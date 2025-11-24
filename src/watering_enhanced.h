#ifndef WATERING_ENHANCED_H
#define WATERING_ENHANCED_H

#include <stdbool.h>
#include <stdint.h>
#include "watering.h"

/**
 * @file watering_enhanced.h
 * @brief Enhanced data structures and interfaces for advanced irrigation modes
 * 
 * This header defines enhanced data structures for:
 * - Configurable interval-based watering with minutes/seconds timing
 * - Custom soil types per channel
 * - Rain and temperature compensation systems
 * - BME280 environmental sensor integration
 * - Configuration status tracking and reset management
 * - Multi-resolution environmental history storage
 */

/* Enhanced watering modes including interval mode */
typedef enum {
    WATERING_BY_INTERVAL = 4      // New: Interval mode with configurable pauses
} enhanced_watering_mode_t;

/* Enhanced task states including pause phase */
typedef enum {
    TASK_STATE_IDLE,
    TASK_STATE_WATERING,
    TASK_STATE_PAUSING,           // New: Pause phase in interval mode
    TASK_STATE_COMPLETED,
    TASK_STATE_ERROR
} enhanced_task_state_t;

/* Configurable interval timing structure with minutes/seconds fields */
typedef struct {
    uint16_t watering_minutes;        // Watering duration in minutes (0-60)
    uint8_t watering_seconds;         // Watering duration in seconds (0-59)
    uint16_t pause_minutes;           // Pause duration in minutes (0-60)
    uint8_t pause_seconds;            // Pause duration in seconds (0-59)
    uint32_t total_target;            // Total target (duration or volume)
    uint32_t cycles_completed;        // Number of complete cycles
    bool currently_watering;          // Current phase state
    uint32_t phase_start_time;        // When current phase started
    uint32_t phase_remaining_sec;     // Seconds remaining in current phase
    bool configured;                  // Whether interval settings are configured
} interval_config_t;

/* Custom soil configuration per channel */
typedef struct {
    bool use_custom_soil;              // True if using custom parameters
    union {
        soil_type_t standard_type;     // Standard soil from database
        struct {
            char name[32];             // Custom soil name
            float field_capacity;      // Field capacity (%)
            float wilting_point;       // Wilting point (%)
            float infiltration_rate;   // Infiltration rate (mm/hr)
            float bulk_density;        // Bulk density (g/cm³)
            float organic_matter;      // Organic matter content (%)
        } custom;
    };
} soil_configuration_t;

/* Rain compensation configuration */
typedef struct {
    bool enabled;                      // Enable/disable rain compensation
    float sensitivity;                 // Sensitivity factor (0.0-1.0)
    uint16_t lookback_hours;          // Hours to look back for rain data
    float skip_threshold_mm;          // Rain threshold to skip watering
    float reduction_factor;           // Factor for duration/volume reduction
} rain_compensation_config_t;

/* Rain compensation calculation results */
typedef struct {
    float recent_rainfall_mm;         // Recent rainfall amount
    float reduction_percentage;       // Calculated reduction percentage
    bool skip_watering;              // Whether to skip this watering
    uint32_t calculation_timestamp;   // When calculation was performed
} rain_compensation_result_t;

/* Temperature compensation configuration */
typedef struct {
    bool enabled;                     // Enable/disable temperature compensation
    float base_temperature;           // Base temperature for calculations (°C)
    float sensitivity;                // Temperature sensitivity factor
    float min_factor;                 // Minimum compensation factor
    float max_factor;                 // Maximum compensation factor
} temperature_compensation_config_t;

/* Temperature compensation calculation results */
typedef struct {
    float current_temperature;        // Current temperature reading
    float compensation_factor;        // Calculated compensation factor
    float adjusted_requirement;       // Adjusted water requirement
    uint32_t calculation_timestamp;   // When calculation was performed
} temperature_compensation_result_t;

/* BME280 sensor reading structure */
typedef struct {
    float temperature;                // Temperature in °C
    float humidity;                   // Relative humidity in %
    float pressure;                   // Atmospheric pressure in hPa
    uint32_t timestamp;               // Measurement timestamp
    bool valid;                       // Data validity flag
} bme280_reading_t;

/* BME280 sensor configuration */
typedef struct {
    uint16_t measurement_interval;    // Measurement interval in seconds
    bool initialized;                 // Sensor initialization status
    bool enabled;                     // Sensor enable/disable
} bme280_config_t;

/* BME280 environmental data processing structure */
typedef struct {
    bme280_reading_t current;         // Current sensor reading
    bme280_reading_t daily_min;       // Daily minimum values
    bme280_reading_t daily_max;       // Daily maximum values
    bme280_reading_t daily_avg;       // Daily average values
    uint16_t readings_count;          // Number of readings today
    uint32_t last_update;             // Last update timestamp
} bme280_environmental_data_t;

/* Configuration group types for reset management */
typedef enum {
    CONFIG_GROUP_BASIC = 0,           // Plant, soil, irrigation method
    CONFIG_GROUP_GROWING_ENV = 1,     // Coverage, sun exposure, water factor
    CONFIG_GROUP_COMPENSATION = 2,    // Rain/temperature compensation
    CONFIG_GROUP_CUSTOM_SOIL = 3,     // Custom soil parameters
    CONFIG_GROUP_INTERVAL = 4,        // Interval watering settings
    CONFIG_GROUP_ALL = 0xFF           // Reset all groups
} config_group_t;

/* Configuration status tracking per channel */
typedef struct {
    bool basic_configured;            // Plant, soil, irrigation method set
    bool growing_env_configured;      // Coverage, sun exposure, water factor set
    bool compensation_configured;     // Rain/temperature compensation set
    bool custom_soil_configured;      // Custom soil parameters set
    bool interval_configured;         // Interval watering settings configured
    uint8_t configuration_score;      // Overall configuration completeness (0-100)
    uint32_t last_reset_timestamp;    // Last time any group was reset
    uint8_t reset_count;              // Number of resets performed
} channel_config_status_t;

/* Configuration reset log entry */
typedef struct {
    config_group_t group;             // Which group was reset
    uint32_t timestamp;               // When reset occurred
    uint8_t channel_id;               // Which channel was reset
    char reason[32];                  // Optional reason for reset
} config_reset_log_entry_t;

/* Configuration reset log management */
typedef struct {
    config_reset_log_entry_t entries[16]; // Last 16 reset operations
    uint8_t head;                     // Ring buffer head pointer
    uint8_t count;                    // Number of entries
} config_reset_log_t;

/* Hourly environmental history entry (30 days retention) */
typedef struct {
    uint32_t timestamp;               // Hour timestamp
    bme280_reading_t environmental;   // Environmental data
    float rainfall_mm;                // Rainfall in this hour
    uint8_t watering_events;          // Number of watering events
    uint32_t total_volume_ml;         // Total volume watered
    uint16_t active_channels;         // Bitmap of active channels
} hourly_history_entry_t;

/* Daily aggregated environmental history (12 months retention) */
typedef struct {
    uint32_t date;                    // Date (YYYYMMDD format)
    struct {
        float min, max, avg;          // Temperature statistics
    } temperature;
    struct {
        float min, max, avg;          // Humidity statistics
    } humidity;
    struct {
        float min, max, avg;          // Pressure statistics
    } pressure;
    float total_rainfall_mm;          // Total daily rainfall
    uint16_t watering_events;         // Total watering events
    uint32_t total_volume_ml;         // Total volume watered
    uint16_t sample_count;            // Number of hourly samples aggregated
    uint8_t active_channels_bitmap;   // Channels that were active
} daily_history_entry_t;

/* Monthly aggregated environmental history (5 years retention) */
typedef struct {
    uint16_t year_month;              // YYYYMM format
    struct {
        float min, max, avg;          // Monthly temperature statistics
    } temperature;
    struct {
        float min, max, avg;          // Monthly humidity statistics
    } humidity;
    struct {
        float min, max, avg;          // Monthly pressure statistics
    } pressure;
    float total_rainfall_mm;          // Total monthly rainfall
    uint32_t watering_events;         // Total watering events
    uint64_t total_volume_ml;         // Total volume watered
    uint8_t days_active;              // Number of days with activity
} monthly_history_entry_t;

/* Multi-resolution environmental history storage */
typedef struct {
    // Hourly data (30 days × 24 hours = 720 entries)
    hourly_history_entry_t hourly[720];
    uint16_t hourly_head;             // Ring buffer head pointer
    uint16_t hourly_count;            // Number of valid entries
    
    // Daily data (12 months × 31 days = 372 entries)
    daily_history_entry_t daily[372];
    uint16_t daily_head;              // Ring buffer head pointer
    uint16_t daily_count;             // Number of valid entries
    
    // Monthly data (5 years × 12 months = 60 entries)
    monthly_history_entry_t monthly[60];
    uint8_t monthly_head;             // Ring buffer head pointer
    uint8_t monthly_count;            // Number of valid entries
    
    uint32_t last_hourly_update;      // Last hourly aggregation
    uint32_t last_daily_update;       // Last daily aggregation
    uint32_t last_monthly_update;     // Last monthly aggregation
} environmental_history_t;

/* Enhanced task status with interval mode support */
typedef struct {
    enhanced_task_state_t state;
    enhanced_watering_mode_t mode;
    interval_config_t interval;       // Only used for interval mode
    uint32_t remaining_time;          // Time remaining in current phase
    uint32_t total_elapsed;           // Total elapsed time
    uint32_t total_volume;            // Total volume dispensed
} enhanced_task_status_t;

/* Enhanced channel configuration with all new features */
typedef struct {
    // Existing fields (from original watering_channel_t)
    plant_info_t plant;
    soil_configuration_t soil;        // Enhanced with custom soil support
    irrigation_method_t irrigation_method;
    channel_coverage_t coverage;
    
    // New compensation settings
    rain_compensation_config_t rain_compensation;
    temperature_compensation_config_t temp_compensation;
    
    // Interval mode configuration
    interval_config_t interval_config;
    
    // Configuration status and reset management
    channel_config_status_t config_status;
    config_reset_log_t reset_log;     // Reset operation history
    
    // Runtime compensation results
    rain_compensation_result_t last_rain_compensation;
    temperature_compensation_result_t last_temp_compensation;
    
    uint32_t last_config_update;      // Last configuration update timestamp
} enhanced_watering_channel_t;

/* Custom soil database entry for NVS storage */
typedef struct {
    uint8_t channel_id;               // Channel this applies to
    char name[32];                    // Custom soil name
    float field_capacity;             // Field capacity percentage
    float wilting_point;              // Wilting point percentage
    float infiltration_rate;          // Infiltration rate mm/hr
    float bulk_density;               // Bulk density g/cm³
    float organic_matter;             // Organic matter percentage
    uint32_t created_timestamp;       // When this was created
    uint32_t modified_timestamp;      // Last modification time
    uint32_t crc32;                   // Data integrity check
} custom_soil_entry_t;

/* Enhanced error codes for new features */
typedef enum {
    // Existing error codes from watering_error_t...
    WATERING_ERROR_BME280_INIT = -20,        // BME280 initialization failed
    WATERING_ERROR_BME280_READ = -21,        // BME280 reading failed
    WATERING_ERROR_CUSTOM_SOIL_INVALID = -22, // Invalid custom soil parameters
    WATERING_ERROR_COMPENSATION_CALC = -23,   // Compensation calculation failed
    WATERING_ERROR_INTERVAL_CONFIG = -24,     // Invalid interval configuration
    WATERING_ERROR_HISTORY_STORAGE = -25,     // History storage operation failed
    WATERING_ERROR_ENV_DATA_CORRUPT = -26,    // Environmental data corruption
    WATERING_ERROR_INTERVAL_MODE_FAILURE = -27, // Interval mode controller failure
    WATERING_ERROR_COMPENSATION_DISABLED = -28, // Compensation system disabled due to errors
    WATERING_ERROR_SENSOR_DEGRADED = -29,    // Sensor operating in degraded mode
    WATERING_ERROR_CONFIG_RESET_FAILED = -30, // Configuration reset operation failed
    WATERING_ERROR_RECOVERY_FAILED = -31,    // Error recovery attempt failed
} enhanced_watering_error_t;

/* Error recovery strategy types */
typedef enum {
    RECOVERY_STRATEGY_NONE = 0,              // No recovery action
    RECOVERY_STRATEGY_RETRY = 1,             // Retry the failed operation
    RECOVERY_STRATEGY_FALLBACK = 2,          // Use fallback/default values
    RECOVERY_STRATEGY_DISABLE = 3,           // Disable the failing component
    RECOVERY_STRATEGY_RESET = 4,             // Reset the component/system
    RECOVERY_STRATEGY_GRACEFUL_DEGRADE = 5,  // Continue with reduced functionality
} error_recovery_strategy_t;

/* Error recovery context information */
typedef struct {
    enhanced_watering_error_t error_code;    // The error that occurred
    uint8_t retry_count;                     // Number of retry attempts made
    uint8_t max_retries;                     // Maximum retry attempts allowed
    error_recovery_strategy_t strategy;      // Recovery strategy to use
    uint32_t last_error_time;                // Timestamp of last error occurrence
    uint32_t recovery_timeout_ms;            // Timeout for recovery operations
    bool recovery_in_progress;               // Whether recovery is currently active
    char error_context[64];                  // Additional error context information
} error_recovery_context_t;

/* System error recovery state */
typedef struct {
    error_recovery_context_t bme280_recovery;     // BME280 sensor error recovery
    error_recovery_context_t compensation_recovery; // Compensation system recovery
    error_recovery_context_t interval_recovery;   // Interval mode recovery
    error_recovery_context_t storage_recovery;    // Storage system recovery
    uint32_t global_error_count;                  // Total system error count
    uint32_t successful_recoveries;               // Number of successful recoveries
    uint32_t failed_recoveries;                   // Number of failed recoveries
    bool system_degraded;                         // System operating in degraded mode
} system_error_recovery_state_t;

/* Helper functions for interval timing */
static inline uint32_t interval_get_watering_duration_sec(const interval_config_t *config) {
    return (config->watering_minutes * 60) + config->watering_seconds;
}

static inline uint32_t interval_get_pause_duration_sec(const interval_config_t *config) {
    return (config->pause_minutes * 60) + config->pause_seconds;
}

static inline bool interval_is_valid_config(const interval_config_t *config) {
    uint32_t watering_sec = interval_get_watering_duration_sec(config);
    uint32_t pause_sec = interval_get_pause_duration_sec(config);
    return (watering_sec >= 1 && watering_sec <= 3600 && 
            pause_sec >= 1 && pause_sec <= 3600);
}

/* Configuration management function declarations */
watering_error_t channel_reset_config_group(uint8_t channel_id, config_group_t group, const char *reason);
watering_error_t channel_get_config_status(uint8_t channel_id, channel_config_status_t *status);
watering_error_t channel_validate_config_completeness(uint8_t channel_id, bool *can_water);
uint8_t channel_calculate_config_score(const enhanced_watering_channel_t *channel);

/* Enhanced system status to support new operational modes */
typedef enum {
    // Base system status (compatible with existing watering_status_t)
    ENHANCED_STATUS_OK = 0,                    // System operating normally
    ENHANCED_STATUS_NO_FLOW = 1,               // No flow detected when valve is open
    ENHANCED_STATUS_UNEXPECTED_FLOW = 2,       // Flow detected when all valves closed
    ENHANCED_STATUS_FAULT = 3,                 // System in fault state requiring manual reset
    ENHANCED_STATUS_RTC_ERROR = 4,             // RTC failure detected
    ENHANCED_STATUS_LOW_POWER = 5,             // System in low power mode
    
    // New enhanced status indicators for advanced features
    ENHANCED_STATUS_INTERVAL_WATERING = 10,    // Currently in watering phase of interval mode
    ENHANCED_STATUS_INTERVAL_PAUSING = 11,     // Currently in pause phase of interval mode
    ENHANCED_STATUS_RAIN_COMPENSATION_ACTIVE = 12,  // Rain compensation is reducing watering
    ENHANCED_STATUS_TEMP_COMPENSATION_ACTIVE = 13,  // Temperature compensation is adjusting watering
    ENHANCED_STATUS_BME280_ERROR = 14,         // BME280 environmental sensor failure
    ENHANCED_STATUS_CUSTOM_SOIL_ACTIVE = 15,   // Using custom soil parameters
    ENHANCED_STATUS_CONFIG_INCOMPLETE = 16,    // Channel configuration incomplete
    ENHANCED_STATUS_DEGRADED_MODE = 17,        // Operating with reduced functionality
} enhanced_system_status_t;

/* Compensation system status indicators */
typedef struct {
    bool rain_compensation_active;        // Rain compensation currently applied
    bool temp_compensation_active;        // Temperature compensation currently applied
    float rain_reduction_percentage;      // Current rain reduction (0-100%)
    float temp_adjustment_factor;         // Current temperature factor (0.5-2.0)
    uint32_t last_compensation_update;    // When compensation was last calculated
} compensation_status_t;

/* Environmental sensor health status */
typedef struct {
    bool bme280_initialized;              // BME280 sensor initialized successfully
    bool bme280_responding;               // BME280 responding to commands
    bool bme280_data_valid;               // BME280 providing valid data
    uint8_t bme280_data_quality;          // Data quality score (0-100)
    uint32_t last_successful_reading;     // Timestamp of last successful reading
    uint32_t consecutive_failures;        // Number of consecutive read failures
    bool rain_sensor_active;              // Rain sensor operational
    uint32_t environmental_data_age_sec;  // Age of current environmental data
} environmental_sensor_status_t;

/* Enhanced system status structure with detailed operational information */
typedef struct {
    enhanced_system_status_t primary_status;     // Primary system status
    enhanced_task_state_t current_task_phase;    // Current task phase (if active)
    compensation_status_t compensation;          // Compensation system status
    environmental_sensor_status_t sensors;       // Environmental sensor health
    uint8_t active_channels_bitmap;              // Bitmap of channels with active tasks
    uint8_t interval_mode_channels_bitmap;       // Bitmap of channels using interval mode
    uint8_t config_incomplete_channels_bitmap;   // Bitmap of channels with incomplete config
    uint32_t status_update_timestamp;            // When this status was last updated
} enhanced_system_status_info_t;

/* Configuration validation functions */
watering_error_t validate_interval_configuration(const interval_config_t *config);
uint8_t calculate_configuration_score(const enhanced_watering_channel_t *channel);
bool can_channel_perform_automatic_watering(uint8_t channel_id, const enhanced_watering_channel_t *channel);

/* Enhanced system status functions */
watering_error_t enhanced_system_get_status(enhanced_system_status_info_t *status_info);
enhanced_system_status_t enhanced_system_determine_primary_status(void);
watering_error_t enhanced_system_update_compensation_status(compensation_status_t *comp_status);
watering_error_t enhanced_system_update_sensor_status(environmental_sensor_status_t *sensor_status);
bool enhanced_system_is_interval_mode_active(uint8_t *active_channels_bitmap);
bool enhanced_system_has_incomplete_config(uint8_t *incomplete_channels_bitmap);

/* Enhanced error handling and recovery functions */
watering_error_t enhanced_error_recovery_init(void);
watering_error_t enhanced_error_handle_bme280_failure(enhanced_watering_error_t error_code, const char *context);
watering_error_t enhanced_error_handle_compensation_failure(enhanced_watering_error_t error_code, const char *context);
watering_error_t enhanced_error_handle_interval_mode_failure(enhanced_watering_error_t error_code, const char *context);
watering_error_t enhanced_error_handle_storage_failure(enhanced_watering_error_t error_code, const char *context);
watering_error_t enhanced_error_attempt_recovery(error_recovery_context_t *recovery_ctx);
watering_error_t enhanced_error_get_recovery_state(system_error_recovery_state_t *recovery_state);
bool enhanced_error_should_retry(const error_recovery_context_t *recovery_ctx);
void enhanced_error_reset_recovery_context(error_recovery_context_t *recovery_ctx);
const char* enhanced_error_code_to_string(enhanced_watering_error_t error_code);
const char* enhanced_error_recovery_strategy_to_string(error_recovery_strategy_t strategy);

#endif // WATERING_ENHANCED_H
