#ifndef ENVIRONMENTAL_DATA_H
#define ENVIRONMENTAL_DATA_H

#include "watering_enhanced.h"

/**
 * @file environmental_data.h
 * @brief Environmental data processing and validation for BME280 sensor
 * 
 * This module provides functions for processing, validating, and aggregating
 * environmental sensor data from the BME280 sensor.
 */

/* Data quality thresholds */
#define ENV_DATA_TEMP_MIN_C         -40.0f
#define ENV_DATA_TEMP_MAX_C         85.0f
#define ENV_DATA_HUMIDITY_MIN       0.0f
#define ENV_DATA_HUMIDITY_MAX       100.0f
#define ENV_DATA_PRESSURE_MIN_HPA   300.0f
#define ENV_DATA_PRESSURE_MAX_HPA   1100.0f

/* Outlier detection parameters */
#define ENV_DATA_TEMP_MAX_CHANGE    10.0f   // Max temperature change per reading (°C)
#define ENV_DATA_HUMIDITY_MAX_CHANGE 20.0f  // Max humidity change per reading (%)
#define ENV_DATA_PRESSURE_MAX_CHANGE 50.0f  // Max pressure change per reading (hPa)

/* Data quality levels */
typedef enum {
    ENV_DATA_QUALITY_INVALID = 0,    // Data is invalid or corrupted
    ENV_DATA_QUALITY_POOR = 25,      // Data has significant issues
    ENV_DATA_QUALITY_FAIR = 50,      // Data is acceptable but not ideal
    ENV_DATA_QUALITY_GOOD = 75,      // Data is good quality
    ENV_DATA_QUALITY_EXCELLENT = 100 // Data is excellent quality
} env_data_quality_t;

/* Environmental data validation result */
typedef struct {
    bool temperature_valid;          // Temperature reading is valid
    bool humidity_valid;             // Humidity reading is valid
    bool pressure_valid;             // Pressure reading is valid
    env_data_quality_t overall_quality; // Overall data quality assessment
    char quality_notes[64];          // Human-readable quality notes
} env_data_validation_t;

/* Environmental data statistics */
typedef struct {
    float min_value;                 // Minimum value in dataset
    float max_value;                 // Maximum value in dataset
    float avg_value;                 // Average value in dataset
    float std_deviation;             // Standard deviation
    uint16_t sample_count;           // Number of samples
    uint32_t last_update;            // Last update timestamp
} env_data_stats_t;

/* Environmental data processor state */
typedef struct {
    bme280_environmental_data_t current_data;   // Current processed data
    bme280_reading_t last_reading;       // Last raw reading for outlier detection
    env_data_stats_t temp_stats;         // Temperature statistics
    env_data_stats_t humidity_stats;     // Humidity statistics
    env_data_stats_t pressure_stats;     // Pressure statistics
    uint32_t readings_today;             // Number of readings today
    uint32_t last_daily_reset;           // Last daily statistics reset
    bool initialized;                    // Processor initialization status
} env_data_processor_t;

/**
 * @brief Initialize environmental data processor
 * @param processor Pointer to processor structure
 * @return 0 on success, negative error code on failure
 */
int env_data_processor_init(env_data_processor_t *processor);

/**
 * @brief Process new BME280 reading
 * @param processor Pointer to processor structure
 * @param reading Pointer to new BME280 reading
 * @return 0 on success, negative error code on failure
 */
int env_data_process_reading(env_data_processor_t *processor, const bme280_reading_t *reading);

/**
 * @brief Validate BME280 reading data
 * @param reading Pointer to BME280 reading to validate
 * @param last_reading Pointer to previous reading for outlier detection (can be NULL)
 * @param validation Pointer to validation result structure
 * @return 0 on success, negative error code on failure
 */
int env_data_validate_reading(const bme280_reading_t *reading, 
                             const bme280_reading_t *last_reading,
                             env_data_validation_t *validation);

/**
 * @brief Calculate data quality score
 * @param reading Pointer to BME280 reading
 * @param validation Pointer to validation result
 * @return Quality score (0-100)
 */
uint8_t env_data_calculate_quality_score(const bme280_reading_t *reading,
                                        const env_data_validation_t *validation);

/**
 * @brief Update daily statistics with new reading
 * @param processor Pointer to processor structure
 * @param reading Pointer to validated BME280 reading
 * @return 0 on success, negative error code on failure
 */
int env_data_update_daily_stats(env_data_processor_t *processor, const bme280_reading_t *reading);

/**
 * @brief Reset daily statistics (called at midnight)
 * @param processor Pointer to processor structure
 * @return 0 on success, negative error code on failure
 */
int env_data_reset_daily_stats(env_data_processor_t *processor);

/**
 * @brief Get current environmental data
 * @param processor Pointer to processor structure
 * @param data Pointer to environmental data structure to fill
 * @return 0 on success, negative error code on failure
 */
int env_data_get_current(const env_data_processor_t *processor, bme280_environmental_data_t *data);

/**
 * @brief Check if reading is an outlier compared to previous reading
 * @param current Pointer to current reading
 * @param previous Pointer to previous reading
 * @return true if reading is an outlier, false otherwise
 */
bool env_data_is_outlier(const bme280_reading_t *current, const bme280_reading_t *previous);

/**
 * @brief Apply smoothing filter to reading
 * @param current Pointer to current reading
 * @param previous Pointer to previous reading
 * @param alpha Smoothing factor (0.0-1.0, higher = less smoothing)
 * @param smoothed Pointer to smoothed reading output
 * @return 0 on success, negative error code on failure
 */
int env_data_apply_smoothing(const bme280_reading_t *current,
                            const bme280_reading_t *previous,
                            float alpha,
                            bme280_reading_t *smoothed);

/**
 * @brief Calculate moving average for statistics
 * @param stats Pointer to statistics structure
 * @param new_value New value to add to average
 * @return 0 on success, negative error code on failure
 */
int env_data_update_moving_average(env_data_stats_t *stats, float new_value);

/**
 * @brief Detect sensor failure conditions
 * @param processor Pointer to processor structure
 * @param failure_detected Pointer to boolean for failure detection result
 * @param failure_reason Pointer to string buffer for failure reason (min 64 chars)
 * @return 0 on success, negative error code on failure
 */
int env_data_detect_sensor_failure(const env_data_processor_t *processor,
                                  bool *failure_detected,
                                  char *failure_reason);

/**
 * @brief Get data quality assessment string
 * @param quality Quality level
 * @return Human-readable quality string
 */
const char* env_data_quality_to_string(env_data_quality_t quality);

/**
 * @brief Check if environmental data is stale
 * @param processor Pointer to processor structure
 * @param max_age_sec Maximum age in seconds before data is considered stale
 * @return true if data is stale, false otherwise
 */
bool env_data_is_stale(const env_data_processor_t *processor, uint32_t max_age_sec);

/**
 * @brief Get current environmental data (global instance)
 * @param data Pointer to environmental data structure to fill
 * @return 0 on success, negative error code on failure
 */
int environmental_data_get_current(bme280_environmental_data_t *data);

/**
 * @brief Process BME280 reading (global instance)
 * @param reading Pointer to BME280 reading to process
 * @return 0 on success, negative error code on failure
 */
int environmental_data_process_bme280_reading(const bme280_reading_t *reading);

/**
 * @brief Initialize environmental data system
 * @return WATERING_ERROR_NONE on success, error code on failure
 */
watering_error_t environmental_data_init(void);

#endif // ENVIRONMENTAL_DATA_H
