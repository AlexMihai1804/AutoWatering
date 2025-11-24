// Primary include guard replaced: relying solely on #pragma once due to intermittent
// toolchain false 'unterminated #ifndef' diagnostics. Original traditional guard
// is commented out (minimal change, easy revert) to remove preprocessor state that
// was sporadically misparsed. Audit reference: build instability investigation.
#pragma once
//#ifndef ENV_SENSORS_H
//#define ENV_SENSORS_H 1

#include <stdbool.h>
#include <stdint.h>
#include "watering.h"

/**
 * @file env_sensors.h
 * @brief Environmental sensor interface for automatic irrigation calculations
 * 
 * This header defines the environmental data structures and sensor interface
 * for collecting weather and soil conditions needed for FAO-56 based
 * evapotranspiration calculations.
 */

/**
 * @brief Environmental sensor data structure
 * 
 * Contains all environmental measurements needed for automatic irrigation
 * calculations using FAO-56 methodology. Includes validity flags and
 * data quality assessment.
 */
typedef struct {
    // Temperature measurements (°C)
    float air_temp_mean_c;          /**< Mean air temperature over measurement period */
    float air_temp_min_c;           /**< Minimum air temperature in 24h period */
    float air_temp_max_c;           /**< Maximum air temperature in 24h period */
    bool temp_valid;                /**< True if temperature readings are valid */
    
    // Humidity and atmospheric pressure
    float rel_humidity_pct;         /**< Relative humidity percentage (0-100%) */
    float atmos_pressure_hpa;       /**< Atmospheric pressure in hectopascals */
    bool humidity_valid;            /**< True if humidity reading is valid */
    bool pressure_valid;            /**< True if pressure reading is valid */
    
    // Precipitation (real sensor aggregation only)
    float rain_mm_24h;              /**< Precipitation in last 24 hours (mm) */
    bool rain_valid;                /**< True if rainfall measurement is valid */
    
    // Data quality and metadata
    uint32_t timestamp;             /**< Unix timestamp when data was collected */
    uint8_t data_quality;           /**< Overall data quality confidence (0-100%) */
    uint16_t measurement_interval_min; /**< Measurement interval in minutes */
    
    // Calculated/derived values (filled by processing functions)
    float dewpoint_temp_c;          /**< Calculated dewpoint temperature */
    float vapor_pressure_kpa;       /**< Actual vapor pressure (kPa) */
    float saturation_vapor_pressure_kpa; /**< Saturation vapor pressure (kPa) */
    bool derived_values_calculated; /**< True if derived values have been calculated */
    
} environmental_data_t;

/**
 * @brief Environmental sensor configuration
 * 
 * Configuration parameters for environmental sensor operation
 */
typedef struct {
    // Sensor enable flags
    bool enable_temp_sensor;        /**< Enable temperature sensor */
    bool enable_humidity_sensor;    /**< Enable humidity sensor */
    bool enable_pressure_sensor;    /**< Enable pressure sensor */
    bool enable_rain_sensor;        /**< Enable rain gauge */
    
    // Measurement intervals
    uint16_t temp_interval_min;     /**< Temperature measurement interval (minutes) */
    uint16_t humidity_interval_min; /**< Humidity measurement interval (minutes) */
    uint16_t rain_interval_min;     /**< Rain measurement interval (minutes) */
    uint16_t _reserved_interval_min; /**< Reserved (formerly soil) */
    
    // Calibration factors
    float temp_offset_c;            /**< Temperature calibration offset */
    float humidity_offset_pct;      /**< Humidity calibration offset */
    float rain_calibration_factor;  /**< Rain gauge calibration multiplier */
    float _reserved_offset;         /**< Reserved (formerly soil moisture offset) */
    
    // Data quality thresholds
    uint8_t min_data_quality;       /**< Minimum acceptable data quality (0-100%) */
    uint16_t max_sensor_age_min;    /**< Maximum age of sensor data before considered stale (minutes) */
    
} env_sensor_config_t;

/**
 * @brief Environmental sensor status information
 */
typedef struct {
    // Sensor operational status
    bool temp_sensor_online;        /**< Temperature sensor operational */
    bool humidity_sensor_online;    /**< Humidity sensor operational */
    bool pressure_sensor_online;    /**< Pressure sensor operational */
    bool rain_sensor_online;        /**< Rain sensor operational */
    
    // Last successful readings
    uint32_t last_temp_reading;     /**< Timestamp of last temperature reading */
    uint32_t last_humidity_reading; /**< Timestamp of last humidity reading */
    uint32_t last_rain_reading;     /**< Timestamp of last rain reading */
    uint32_t _reserved_last_soil_reading; /**< Reserved */
    
    // Error counters
    uint16_t temp_error_count;      /**< Temperature sensor error count */
    uint16_t humidity_error_count;  /**< Humidity sensor error count */
    uint16_t rain_error_count;      /**< Rain sensor error count */
    uint16_t _reserved_soil_error_count; /**< Reserved */
    
    // Overall system status
    uint8_t overall_health;         /**< Overall sensor system health (0-100%) */
    uint32_t last_full_reading;     /**< Timestamp of last complete sensor reading */
    
} env_sensor_status_t;

/**
 * @brief Initialize environmental sensor system
 * 
 * Initializes all configured environmental sensors and prepares the system
 * for data collection.
 * 
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t env_sensors_init(void);

/**
 * @brief Read current environmental data from all sensors
 * 
 * Collects current readings from all available environmental sensors and
 * populates the environmental_data_t structure with current conditions.
 * 
 * @param data Pointer to environmental_data_t structure to populate
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t env_sensors_read(environmental_data_t *data);

/**
 * @brief Get environmental sensor system status
 * 
 * Returns the current operational status of all environmental sensors
 * including error counts and last reading timestamps.
 * 
 * @param status Pointer to env_sensor_status_t structure to populate
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t env_sensors_get_status(env_sensor_status_t *status);

/**
 * @brief Configure environmental sensor system
 * 
 * Updates the configuration of the environmental sensor system including
 * which sensors are enabled and their measurement intervals.
 * 
 * @param config Pointer to new configuration
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t env_sensors_configure(const env_sensor_config_t *config);

/**
 * @brief Calibrate environmental sensors
 * 
 * Performs calibration procedures for environmental sensors that support
 * calibration. This may include zero-point calibration for some sensors.
 * 
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t env_sensors_calibrate(void);

/**
 * @brief Calculate derived environmental values
 * 
 * Calculates derived values like dewpoint temperature and vapor pressures
 * from basic sensor readings. Updates the derived values in the provided
 * environmental data structure.
 * 
 * @param data Pointer to environmental_data_t structure to update
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t env_sensors_calculate_derived(environmental_data_t *data);

/**
 * @brief Generate fallback environmental data
 * 
 * Generates reasonable fallback environmental data when sensors are not
 * available or have failed. Uses historical data, seasonal averages, or
 * conservative estimates.
 * 
 * @param data Pointer to environmental_data_t structure to populate
 * @param latitude_deg Site latitude in degrees for seasonal calculations
 * @param day_of_year Current day of year (1-365)
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t env_sensors_generate_fallback(environmental_data_t *data, 
                                               float latitude_deg, 
                                               uint16_t day_of_year);

/**
 * @brief Validate environmental data quality
 * 
 * Validates environmental sensor readings for reasonableness and consistency.
 * Updates data quality flags and overall confidence score.
 * 
 * @param data Pointer to environmental_data_t structure to validate
 * @return WATERING_SUCCESS if data is valid, error code if validation fails
 */
watering_error_t env_sensors_validate_data(environmental_data_t *data);

/**
 * @brief Reset environmental sensor error counters
 * 
 * Resets all sensor error counters and clears any persistent error states.
 * Used for system maintenance and troubleshooting.
 * 
 * @return WATERING_SUCCESS on success
 */
watering_error_t env_sensors_reset_errors(void);

/**
 * @brief Put environmental sensors into low power mode
 * 
 * Configures environmental sensors for low power operation when the system
 * is in power saving mode. May reduce measurement frequency or disable
 * non-critical sensors.
 * 
 * @param enable True to enable low power mode, false to return to normal
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t env_sensors_set_low_power(bool enable);

/**
 * @brief Read soil moisture sensor for specific channel
 * 
 * Reads volumetric water content from soil moisture sensor associated with
 * a specific irrigation channel. This allows per-channel soil monitoring.
 * 
 * @param channel_id Channel ID (0-7) to read soil sensor for
 * @param vwc_pct Pointer to store volumetric water content percentage
 * @param soil_temp_c Pointer to store soil temperature in Celsius
 * @return WATERING_SUCCESS on success, error code on failure
 */

/**
 * @brief Calculate soil water deficit for irrigation decisions
 * 
 * Calculates current soil water deficit based on sensor readings and soil
 * characteristics. Integrates with water balance calculations for automatic
 * irrigation triggering.
 * 
 * @param channel_id Channel ID to calculate deficit for
 * @param soil_type_id Index into soil database for soil characteristics
 * @param root_depth_m Current root depth in meters
 * @param deficit_mm Pointer to store calculated water deficit in mm
 * @return WATERING_SUCCESS on success, error code on failure
 */

/**
 * @brief Calibrate soil moisture sensors
 * 
 * Performs calibration of soil moisture sensors including dry and wet point
 * calibration. Should be performed with sensors in known moisture conditions.
 * 
 * @param channel_id Channel ID to calibrate (0-7), or 255 for all channels
 * @param calibration_type 0=dry point, 1=wet point, 2=field capacity
 * @return WATERING_SUCCESS on success, error code on failure
 */
/* Deprecated soil sensor APIs fully removed (no hardware planned). */

/* NOTE: All soil sensor public APIs removed (read, deficit, calibrate, status).
 * System now relies solely on modeled water balance (ET-based) without direct
 * soil moisture hardware. Remaining deprecated fields stay in structs for
 * ABI stability and will always report inert values (0 / false). */

//#endif /* ENV_SENSORS_H (disabled, using pragma once only) */
