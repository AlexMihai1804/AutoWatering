#ifndef SENSOR_MANAGER_H
#define SENSOR_MANAGER_H

#include "bme280_driver.h"
#include "environmental_data.h"
#include "watering_enhanced.h"

/**
 * @file sensor_manager.h
 * @brief Sensor management system integrating BME280 with existing sensors
 * 
 * This module provides centralized management of all environmental sensors
 * including BME280, rain sensor, and flow sensor with health monitoring
 * and error recovery capabilities.
 */

/* Sensor types */
typedef enum {
    SENSOR_TYPE_BME280,          // Environmental sensor (temperature, humidity, pressure)
    SENSOR_TYPE_RAIN,            // Rain sensor
    SENSOR_TYPE_FLOW,            // Flow sensor
    SENSOR_TYPE_COUNT            // Number of sensor types
} sensor_type_t;

/* Sensor health status */
typedef enum {
    SENSOR_HEALTH_OK,            // Sensor operating normally
    SENSOR_HEALTH_WARNING,       // Sensor has minor issues
    SENSOR_HEALTH_ERROR,         // Sensor has errors but may recover
    SENSOR_HEALTH_FAILED,        // Sensor has failed completely
    SENSOR_HEALTH_UNKNOWN        // Sensor status unknown
} sensor_health_t;

/* Sensor error types */
typedef enum {
    SENSOR_ERROR_NONE = 0,
    SENSOR_ERROR_COMMUNICATION,  // I2C/communication error
    SENSOR_ERROR_TIMEOUT,        // Sensor response timeout
    SENSOR_ERROR_INVALID_DATA,   // Invalid sensor data
    SENSOR_ERROR_CALIBRATION,    // Calibration error
    SENSOR_ERROR_HARDWARE,       // Hardware failure
    SENSOR_ERROR_POWER,          // Power supply issue
    SENSOR_ERROR_INITIALIZATION  // Initialization failure
} sensor_error_t;

/* Sensor status information */
typedef struct {
    sensor_type_t type;          // Sensor type
    sensor_health_t health;      // Current health status
    sensor_error_t last_error;   // Last error encountered
    uint32_t error_count;        // Total error count
    uint32_t success_count;      // Total successful readings
    uint32_t last_reading_time;  // Timestamp of last successful reading
    uint32_t last_error_time;    // Timestamp of last error
    bool enabled;                // Whether sensor is enabled
    bool initialized;            // Whether sensor is initialized
    char status_message[64];     // Human-readable status message
} sensor_status_t;

/* Sensor manager configuration */
typedef struct {
    bool auto_recovery_enabled;  // Enable automatic error recovery
    uint32_t recovery_timeout_ms; // Timeout for recovery attempts
    uint32_t max_recovery_attempts; // Maximum recovery attempts
    uint32_t health_check_interval_ms; // Health check interval
    uint32_t reading_timeout_ms; // Timeout for sensor readings
} sensor_manager_config_t;

/* Sensor manager state */
typedef struct {
    bme280_device_t bme280;      // BME280 device instance
    env_data_processor_t env_processor; // Environmental data processor
    sensor_status_t sensor_status[SENSOR_TYPE_COUNT]; // Status for each sensor
    sensor_manager_config_t config; // Manager configuration
    uint32_t last_health_check;  // Last health check timestamp
    bool initialized;            // Manager initialization status
    struct k_mutex sensor_mutex; // Mutex for thread safety
} sensor_manager_t;

/* Global sensor manager instance */
extern sensor_manager_t g_sensor_manager;

/**
 * @brief Initialize sensor manager
 * @param config Pointer to configuration structure (NULL for defaults)
 * @return 0 on success, negative error code on failure
 */
int sensor_manager_init(const sensor_manager_config_t *config);

/**
 * @brief Initialize BME280 sensor
 * @param i2c_dev I2C device pointer
 * @param addr I2C address
 * @return 0 on success, negative error code on failure
 */
int sensor_manager_init_bme280(const struct device *i2c_dev, uint8_t addr);

/**
 * @brief Configure BME280 sensor
 * @param config Pointer to BME280 configuration
 * @return 0 on success, negative error code on failure
 */
int sensor_manager_configure_bme280(const bme280_config_t *config);

/**
 * @brief Read environmental data from BME280
 * @param data Pointer to environmental data structure to fill
 * @return 0 on success, negative error code on failure
 */
int sensor_manager_read_environmental_data(bme280_environmental_data_t *data);

/**
 * @brief Trigger BME280 measurement
 * @return 0 on success, negative error code on failure
 */
int sensor_manager_trigger_bme280_measurement(void);

/**
 * @brief Get sensor status
 * @param type Sensor type
 * @param status Pointer to status structure to fill
 * @return 0 on success, negative error code on failure
 */
int sensor_manager_get_sensor_status(sensor_type_t type, sensor_status_t *status);

/**
 * @brief Get all sensor statuses
 * @param statuses Array of status structures (must have SENSOR_TYPE_COUNT elements)
 * @return 0 on success, negative error code on failure
 */
int sensor_manager_get_all_sensor_status(sensor_status_t statuses[SENSOR_TYPE_COUNT]);

/**
 * @brief Enable/disable sensor
 * @param type Sensor type
 * @param enabled True to enable, false to disable
 * @return 0 on success, negative error code on failure
 */
int sensor_manager_set_sensor_enabled(sensor_type_t type, bool enabled);

/**
 * @brief Perform health check on all sensors
 * @return 0 on success, negative error code on failure
 */
int sensor_manager_health_check(void);

/**
 * @brief Attempt to recover failed sensor
 * @param type Sensor type to recover
 * @return 0 on success, negative error code on failure
 */
int sensor_manager_recover_sensor(sensor_type_t type);

/**
 * @brief Get overall system sensor health
 * @return Overall health status
 */
sensor_health_t sensor_manager_get_overall_health(void);

/**
 * @brief Check if sensor data is available and fresh
 * @param type Sensor type
 * @param max_age_ms Maximum age in milliseconds
 * @return true if data is available and fresh, false otherwise
 */
bool sensor_manager_is_data_fresh(sensor_type_t type, uint32_t max_age_ms);

/**
 * @brief Get sensor error string
 * @param error Sensor error code
 * @return Human-readable error string
 */
const char* sensor_manager_error_to_string(sensor_error_t error);

/**
 * @brief Get sensor health string
 * @param health Sensor health status
 * @return Human-readable health string
 */
const char* sensor_manager_health_to_string(sensor_health_t health);

/**
 * @brief Get sensor type string
 * @param type Sensor type
 * @return Human-readable sensor type string
 */
const char* sensor_manager_type_to_string(sensor_type_t type);

/**
 * @brief Reset sensor error counters
 * @param type Sensor type (SENSOR_TYPE_COUNT for all sensors)
 * @return 0 on success, negative error code on failure
 */
int sensor_manager_reset_error_counters(sensor_type_t type);

/**
 * @brief Set sensor manager configuration
 * @param config Pointer to new configuration
 * @return 0 on success, negative error code on failure
 */
int sensor_manager_set_config(const sensor_manager_config_t *config);

/**
 * @brief Get current sensor manager configuration
 * @param config Pointer to configuration structure to fill
 * @return 0 on success, negative error code on failure
 */
int sensor_manager_get_config(sensor_manager_config_t *config);

/**
 * @brief Shutdown sensor manager and all sensors
 * @return 0 on success, negative error code on failure
 */
int sensor_manager_shutdown(void);

#endif // SENSOR_MANAGER_H
