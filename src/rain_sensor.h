#ifndef RAIN_SENSOR_H
#define RAIN_SENSOR_H

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <stdbool.h>
#include <stdint.h>

/**
 * @file rain_sensor.h
 * @brief Interface for the tipping bucket rain sensor
 * 
 * This header defines the public API for interfacing with the rain sensor
 * that measures precipitation using pulse counting on pin P0.31.
 * 
 * Features:
 * - Tipping bucket rain gauge support
 * - Configurable mm per pulse calibration
 * - Hardware debouncing with configurable timing
 * - Thread-safe pulse counting with atomic operations
 * - Real-time rainfall rate calculations
 * - Integration with automatic irrigation system
 */

/** Default calibration value for typical tipping bucket (0.2mm per pulse) */
#define RAIN_SENSOR_DEFAULT_MM_PER_PULSE    0.2f

/** Default debounce time in milliseconds for rain sensor */
#define RAIN_SENSOR_DEFAULT_DEBOUNCE_MS     50

/** Maximum reasonable rainfall rate (mm/hour) for validation */
#define RAIN_SENSOR_MAX_RATE_MM_H           100.0f

/** Minimum valid calibration value (mm per pulse) */
#define RAIN_SENSOR_MIN_CALIBRATION         0.1f

/** Maximum valid calibration value (mm per pulse) */
#define RAIN_SENSOR_MAX_CALIBRATION         10.0f

/**
 * @brief Rain sensor status enumeration
 */
typedef enum {
    RAIN_SENSOR_STATUS_INACTIVE = 0,    /**< No recent activity */
    RAIN_SENSOR_STATUS_ACTIVE = 1,      /**< Currently detecting rain */
    RAIN_SENSOR_STATUS_ERROR = 2        /**< Sensor error detected */
} rain_sensor_status_t;

/**
 * @brief Rain sensor configuration structure
 */
typedef struct {
    float mm_per_pulse;                 /**< Calibration: mm per pulse */
    uint16_t debounce_ms;               /**< Debounce time in milliseconds */
    bool sensor_enabled;                /**< Enable/disable sensor */
    bool integration_enabled;           /**< Enable irrigation integration */
} rain_sensor_config_t;

/**
 * @brief Rain sensor data structure
 */
typedef struct {
    uint32_t total_pulses;              /**< Total pulse count since reset */
    uint32_t last_pulse_time;           /**< Timestamp of last pulse */
    float current_hour_mm;              /**< Current hour rainfall */
    float hourly_rate_mm;               /**< Current rainfall rate mm/h */
    rain_sensor_status_t status;        /**< Current sensor status */
    uint8_t data_quality;               /**< Data quality 0-100% */
} rain_sensor_data_t;

/**
 * @brief Initialize the rain sensor hardware
 * 
 * Sets up GPIO and interrupt handlers for the rain sensor pulse detection
 * on pin P0.31. Loads configuration from NVS and initializes counters.
 * 
 * @return 0 on success, negative error code on failure
 */
int rain_sensor_init(void);

/**
 * @brief Deinitialize the rain sensor
 * 
 * Disables interrupts and cleans up resources.
 * 
 * @return 0 on success, negative error code on failure
 */
int rain_sensor_deinit(void);

/**
 * @brief Set the rain sensor calibration value
 * 
 * @param mm_per_pulse Calibration value in mm per pulse (0.1-10.0 range)
 * @return 0 on success, negative error code on failure
 */
int rain_sensor_set_calibration(float mm_per_pulse);

/**
 * @brief Get the current rain sensor calibration value
 * 
 * @return Calibration value in mm per pulse
 */
float rain_sensor_get_calibration(void);

/**
 * @brief Set the debounce time for rain sensor
 * 
 * @param debounce_ms Debounce time in milliseconds (10-1000 range)
 * @return 0 on success, negative error code on failure
 */
int rain_sensor_set_debounce(uint16_t debounce_ms);

/**
 * @brief Get the current debounce time
 * 
 * @return Debounce time in milliseconds
 */
uint16_t rain_sensor_get_debounce(void);

/**
 * @brief Get the current pulse count from the rain sensor
 * 
 * @return Number of pulses detected since last reset
 */
uint32_t rain_sensor_get_pulse_count(void);

/**
 * @brief Get the current rainfall in millimeters
 * 
 * @return Current rainfall in mm (pulse count × calibration)
 */
float rain_sensor_get_current_rainfall_mm(void);

/**
 * @brief Get the current hourly rainfall rate
 * 
 * @return Current rainfall rate in mm/hour
 */
float rain_sensor_get_hourly_rate_mm(void);

/**
 * @brief Get the rainfall for the current hour
 * 
 * @return Rainfall in mm for the current hour
 */
float rain_sensor_get_current_hour_mm(void);

/**
 * @brief Get the timestamp of the last pulse
 * 
 * @return Timestamp of last pulse in seconds since epoch
 */
uint32_t rain_sensor_get_last_pulse_time(void);

/**
 * @brief Reset the pulse counter and rainfall totals
 * 
 * Resets all counters to zero and updates the reset timestamp.
 */
void rain_sensor_reset_counters(void);

/**
 * @brief Check if the rain sensor is currently active
 * 
 * @return true if sensor detected rain recently, false otherwise
 */
bool rain_sensor_is_active(void);

/**
 * @brief Get the current sensor status
 * 
 * @return Current sensor status (inactive/active/error)
 */
rain_sensor_status_t rain_sensor_get_status(void);

/**
 * @brief Get comprehensive rain sensor data
 * 
 * @param data Pointer to structure to fill with current data
 * @return 0 on success, negative error code on failure
 */
int rain_sensor_get_data(rain_sensor_data_t *data);

/**
 * @brief Enable or disable the rain sensor
 * 
 * @param enabled true to enable, false to disable
 * @return 0 on success, negative error code on failure
 */
int rain_sensor_set_enabled(bool enabled);

/**
 * @brief Check if the rain sensor is enabled
 * 
 * @return true if enabled, false if disabled
 */
bool rain_sensor_is_enabled(void);

/**
 * @brief Enable or disable irrigation integration
 * 
 * @param enabled true to enable integration, false to disable
 * @return 0 on success, negative error code on failure
 */
int rain_sensor_set_integration_enabled(bool enabled);

/**
 * @brief Check if irrigation integration is enabled
 * 
 * @return true if integration enabled, false otherwise
 */
bool rain_sensor_is_integration_enabled(void);

/**
 * @brief Update hourly rainfall calculations
 * 
 * Called periodically to update hourly totals and rates.
 * Should be called from a timer or periodic task.
 */
void rain_sensor_update_hourly(void);

/**
 * @brief Print rain sensor debug information
 * 
 * Outputs current sensor state, configuration, and statistics
 * to the console for debugging purposes.
 */
void rain_sensor_debug_info(void);

/**
 * @brief Validate rain sensor configuration
 * 
 * @param config Configuration to validate
 * @return 0 if valid, negative error code if invalid
 */
int rain_sensor_validate_config(const rain_sensor_config_t *config);

/**
 * @brief Get default rain sensor configuration
 * 
 * @param config Pointer to structure to fill with defaults
 */
void rain_sensor_get_default_config(rain_sensor_config_t *config);

/**
 * @brief Save current configuration to NVS
 * 
 * @return 0 on success, negative error code on failure
 */
int rain_sensor_save_config(void);

/**
 * @brief Load configuration from NVS
 * 
 * @return 0 on success, negative error code on failure
 */
int rain_sensor_load_config(void);

/**
 * @brief Error handling and diagnostics functions
 */

/**
 * @brief Rain sensor error codes
 */
typedef enum {
    RAIN_ERROR_NONE = 0,
    RAIN_ERROR_SENSOR_DISCONNECTED = 1,
    RAIN_ERROR_CALIBRATION_INVALID = 2,
    RAIN_ERROR_EXCESSIVE_RATE = 3,
    RAIN_ERROR_GPIO_FAILURE = 4,
    RAIN_ERROR_CONFIG_CORRUPT = 5
} rain_error_code_t;

/**
 * @brief Get the last error that occurred
 * 
 * @return Error code of the last error
 */
rain_error_code_t rain_sensor_get_last_error(void);

/**
 * @brief Get the total number of errors since startup
 * 
 * @return Total error count
 */
uint32_t rain_sensor_get_error_count(void);

/**
 * @brief Get the timestamp of the last error
 * 
 * @return Timestamp of last error in seconds since epoch
 */
uint32_t rain_sensor_get_last_error_time(void);

/**
 * @brief Clear all error states (for recovery/testing)
 */
void rain_sensor_clear_errors(void);

/* Enhanced diagnostics and error handling functions */

/**
 * @brief Error log entry structure for detailed error tracking
 */
typedef struct {
    uint8_t error_code;           /**< Error code */
    uint32_t timestamp;           /**< When error occurred */
    uint32_t pulse_count_at_error; /**< Pulse count when error occurred */
    float rate_at_error;          /**< Rainfall rate when error occurred */
    char description[64];         /**< Error description */
} rain_error_log_t;

/**
 * @brief Get comprehensive diagnostic information
 * 
 * @param buffer Buffer to store diagnostic information
 * @param buffer_size Size of the buffer
 * @return Number of bytes written, or negative error code
 */
int rain_sensor_get_diagnostics(char *buffer, uint16_t buffer_size);

/**
 * @brief Get detailed error log for analysis
 * 
 * @param log_buffer Buffer to store error log entries
 * @param max_entries Maximum number of entries to retrieve
 * @return Number of entries copied, or negative error code
 */
int rain_sensor_get_error_log(rain_error_log_t *log_buffer, uint8_t max_entries);

/**
 * @brief Reset all diagnostic data and error statistics
 */
void rain_sensor_reset_diagnostics(void);

/**
 * @brief Enable or disable outlier detection
 * 
 * @param enabled true to enable outlier detection, false to disable
 */
void rain_sensor_set_outlier_detection(bool enabled);

/**
 * @brief Check if sensor health is in critical condition
 * 
 * @return true if health is critical, false otherwise
 */
bool rain_sensor_is_health_critical(void);

/**
 * @brief Get pulse accuracy percentage
 * 
 * @return Pulse accuracy as percentage (0-100%)
 */
float rain_sensor_get_pulse_accuracy(void);

/**
 * @brief Report health status through BLE notifications
 */
void rain_sensor_report_health_ble(void);

/**
 * @brief Periodic diagnostic and maintenance function
 * 
 * Should be called periodically (every 10 minutes) for health monitoring.
 */
void rain_sensor_periodic_diagnostics(void);

/**
 * @brief Get error description string
 * 
 * @param error_code Error code to get description for
 * @return Human-readable error description
 */
const char* rain_sensor_get_error_description(uint8_t error_code);

/**
 * @brief Export diagnostic data for external analysis
 * 
 * @param buffer Buffer to store diagnostic data in CSV format
 * @param buffer_size Size of the buffer
 * @return Number of bytes written, or negative error code
 */
int rain_sensor_export_diagnostic_data(char *buffer, uint16_t buffer_size);

#endif // RAIN_SENSOR_H