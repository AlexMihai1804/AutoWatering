#include "environmental_data.h"
#include <zephyr/logging/log.h>
#include <zephyr/kernel.h>
#include <math.h>
#include <string.h>

LOG_MODULE_REGISTER(environmental_data, LOG_LEVEL_DBG);

/* Internal helper functions */
static void update_min_max_avg(env_data_stats_t *stats, float value);
/* The standard deviation helper is currently unused in production path but
 * kept for future statistical enhancements. Mark it maybe_unused to silence
 * -Wunused-function without removing code.
 */
static float calculate_standard_deviation(const env_data_stats_t *stats) __attribute__((unused));
static bool is_value_in_range(float value, float min, float max);

int env_data_processor_init(env_data_processor_t *processor)
{
    if (!processor) {
        LOG_ERR("Invalid processor pointer");
        return -EINVAL;
    }

    /* Clear processor structure */
    memset(processor, 0, sizeof(env_data_processor_t));

    /* Initialize statistics structures */
    processor->temp_stats.min_value = INFINITY;
    processor->temp_stats.max_value = -INFINITY;
    processor->humidity_stats.min_value = INFINITY;
    processor->humidity_stats.max_value = -INFINITY;
    processor->pressure_stats.min_value = INFINITY;
    processor->pressure_stats.max_value = -INFINITY;

    processor->last_daily_reset = k_uptime_get_32();
    processor->initialized = true;

    LOG_INF("Environmental data processor initialized");
    return 0;
}

int env_data_process_reading(env_data_processor_t *processor, const bme280_reading_t *reading)
{
    if (!processor || !reading) {
        LOG_ERR("Invalid parameters");
        return -EINVAL;
    }

    if (!processor->initialized) {
        LOG_ERR("Processor not initialized");
        return -ENODEV;
    }

    /* Validate the reading */
    env_data_validation_t validation;
    int ret = env_data_validate_reading(reading, &processor->last_reading, &validation);
    if (ret < 0) {
        LOG_ERR("Reading validation failed: %d", ret);
        return ret;
    }

    /* Check if we should reset daily statistics */
    uint32_t current_time = k_uptime_get_32();
    uint32_t time_since_reset = current_time - processor->last_daily_reset;
    if (time_since_reset > (24 * 60 * 60 * 1000)) { // 24 hours in milliseconds
        env_data_reset_daily_stats(processor);
    }

    /* Apply smoothing if we have a previous reading */
    bme280_reading_t processed_reading = *reading;
    if (processor->last_reading.valid) {
        /* Apply light smoothing (alpha = 0.8 for minimal smoothing) */
        env_data_apply_smoothing(reading, &processor->last_reading, 0.8f, &processed_reading);
    }

    /* Update daily statistics */
    if (validation.temperature_valid) {
        env_data_update_daily_stats(processor, &processed_reading);
    }

    /* Update current data structure */
    processor->current_data.current = processed_reading;
    processor->current_data.readings_count++;
    processor->current_data.last_update = current_time;

    /* Update min/max/avg for the day */
    if (validation.temperature_valid) {
        if (processed_reading.temperature < processor->current_data.daily_min.temperature ||
            !processor->current_data.daily_min.valid) {
            processor->current_data.daily_min = processed_reading;
        }
        if (processed_reading.temperature > processor->current_data.daily_max.temperature ||
            !processor->current_data.daily_max.valid) {
            processor->current_data.daily_max = processed_reading;
        }
    }

    /* Update daily average (simple running average) */
    if (processor->current_data.readings_count > 0) {
        float count = (float)processor->current_data.readings_count;
        processor->current_data.daily_avg.temperature = 
            (processor->current_data.daily_avg.temperature * (count - 1) + processed_reading.temperature) / count;
        processor->current_data.daily_avg.humidity = 
            (processor->current_data.daily_avg.humidity * (count - 1) + processed_reading.humidity) / count;
        processor->current_data.daily_avg.pressure = 
            (processor->current_data.daily_avg.pressure * (count - 1) + processed_reading.pressure) / count;
        processor->current_data.daily_avg.valid = true;
        processor->current_data.daily_avg.timestamp = current_time;
    }

    /* Store this reading as the last reading for next iteration */
    processor->last_reading = processed_reading;
    processor->readings_today++;

    LOG_DBG("Processed environmental reading: T=%.2f degC, H=%.2f%%, P=%.2f hPa, Q=%s",
            (double)processed_reading.temperature, (double)processed_reading.humidity,
            (double)processed_reading.pressure,
            env_data_quality_to_string(validation.overall_quality));

    return 0;
}

int env_data_validate_reading(const bme280_reading_t *reading, 
                             const bme280_reading_t *last_reading,
                             env_data_validation_t *validation)
{
    if (!reading || !validation) {
        return -EINVAL;
    }

    /* Initialize validation structure */
    memset(validation, 0, sizeof(env_data_validation_t));

    /* Validate temperature */
    validation->temperature_valid = is_value_in_range(reading->temperature, 
                                                     ENV_DATA_TEMP_MIN_C, 
                                                     ENV_DATA_TEMP_MAX_C);

    /* Validate humidity */
    validation->humidity_valid = is_value_in_range(reading->humidity, 
                                                  ENV_DATA_HUMIDITY_MIN, 
                                                  ENV_DATA_HUMIDITY_MAX);

    /* Validate pressure */
    validation->pressure_valid = is_value_in_range(reading->pressure, 
                                                  ENV_DATA_PRESSURE_MIN_HPA, 
                                                  ENV_DATA_PRESSURE_MAX_HPA);

    /* Check for outliers if we have a previous reading */
    bool is_outlier = false;
    if (last_reading && last_reading->valid) {
        is_outlier = env_data_is_outlier(reading, last_reading);
    }

    /* Calculate overall quality */
    uint8_t quality_score = env_data_calculate_quality_score(reading, validation);
    
    if (quality_score >= 90) {
        validation->overall_quality = ENV_DATA_QUALITY_EXCELLENT;
        strcpy(validation->quality_notes, "Excellent data quality");
    } else if (quality_score >= 75) {
        validation->overall_quality = ENV_DATA_QUALITY_GOOD;
        strcpy(validation->quality_notes, "Good data quality");
    } else if (quality_score >= 50) {
        validation->overall_quality = ENV_DATA_QUALITY_FAIR;
        strcpy(validation->quality_notes, "Fair data quality");
    } else if (quality_score >= 25) {
        validation->overall_quality = ENV_DATA_QUALITY_POOR;
        strcpy(validation->quality_notes, "Poor data quality");
    } else {
        validation->overall_quality = ENV_DATA_QUALITY_INVALID;
        strcpy(validation->quality_notes, "Invalid data");
    }

    /* Add specific quality notes */
    if (is_outlier) {
        strcat(validation->quality_notes, " (outlier detected)");
    }
    if (!validation->temperature_valid) {
        strcat(validation->quality_notes, " (temp invalid)");
    }
    if (!validation->humidity_valid) {
        strcat(validation->quality_notes, " (humidity invalid)");
    }
    if (!validation->pressure_valid) {
        strcat(validation->quality_notes, " (pressure invalid)");
    }
    return 0;
}

uint8_t env_data_calculate_quality_score(const bme280_reading_t *reading,
                                        const env_data_validation_t *validation)
{
    if (!reading || !validation) {
        return 0;
    }

    uint8_t score = 0;

    /* Base score for valid readings */
    if (validation->temperature_valid) score += 34;
    if (validation->humidity_valid) score += 33;
    if (validation->pressure_valid) score += 33;

    /* Penalty for very recent timestamp (might indicate stale data) */
    uint32_t current_time = k_uptime_get_32();
    uint32_t age_ms = current_time - reading->timestamp;
    if (age_ms > 300000) { // 5 minutes
        score = (score * 80) / 100; // 20% penalty
    }

    /* Cap at 100 */
    if (score > 100) {
        score = 100;
    }

    return score;
}

int env_data_update_daily_stats(env_data_processor_t *processor, const bme280_reading_t *reading)
{
    if (!processor || !reading) {
        return -EINVAL;
    }

    /* Update temperature statistics */
    env_data_update_moving_average(&processor->temp_stats, reading->temperature);
    
    /* Update humidity statistics */
    env_data_update_moving_average(&processor->humidity_stats, reading->humidity);
    
    /* Update pressure statistics */
    env_data_update_moving_average(&processor->pressure_stats, reading->pressure);
    
    return 0;
}

int env_data_reset_daily_stats(env_data_processor_t *processor)
{
    if (!processor) {
        return -EINVAL;
    }

    /* Reset all statistics */
    memset(&processor->temp_stats, 0, sizeof(env_data_stats_t));
    memset(&processor->humidity_stats, 0, sizeof(env_data_stats_t));
    memset(&processor->pressure_stats, 0, sizeof(env_data_stats_t));

    /* Reset min/max values */
    processor->temp_stats.min_value = INFINITY;
    processor->temp_stats.max_value = -INFINITY;
    processor->humidity_stats.min_value = INFINITY;
    processor->humidity_stats.max_value = -INFINITY;
    processor->pressure_stats.min_value = INFINITY;
    processor->pressure_stats.max_value = -INFINITY;

    /* Reset daily counters */
    processor->readings_today = 0;
    processor->last_daily_reset = k_uptime_get_32();

    /* Reset daily data */
    memset(&processor->current_data.daily_min, 0, sizeof(bme280_reading_t));
    memset(&processor->current_data.daily_max, 0, sizeof(bme280_reading_t));
    memset(&processor->current_data.daily_avg, 0, sizeof(bme280_reading_t));
    processor->current_data.readings_count = 0;

    LOG_DBG("Daily environmental statistics reset");
    return 0;
}

int env_data_get_current(const env_data_processor_t *processor, bme280_environmental_data_t *data)
{
    if (!processor || !data) {
        return -EINVAL;
    }

    if (!processor->initialized) {
        return -ENODEV;
    }

    /* Copy current data */
    *data = processor->current_data;

    return 0;
}

bool env_data_is_outlier(const bme280_reading_t *current, const bme280_reading_t *previous)
{
    if (!current || !previous || !previous->valid) {
        return false;
    }

    /* Check temperature change */
    float temp_change = fabsf(current->temperature - previous->temperature);
    if (temp_change > ENV_DATA_TEMP_MAX_CHANGE) {
        return true;
    }

    /* Check humidity change */
    float humidity_change = fabsf(current->humidity - previous->humidity);
    if (humidity_change > ENV_DATA_HUMIDITY_MAX_CHANGE) {
        return true;
    }

    /* Check pressure change */
    float pressure_change = fabsf(current->pressure - previous->pressure);
    if (pressure_change > ENV_DATA_PRESSURE_MAX_CHANGE) {
        return true;
    }

    return false;
}

int env_data_apply_smoothing(const bme280_reading_t *current,
                            const bme280_reading_t *previous,
                            float alpha,
                            bme280_reading_t *smoothed)
{
    if (!current || !previous || !smoothed) {
        return -EINVAL;
    }

    if (alpha < 0.0f || alpha > 1.0f) {
        return -EINVAL;
    }

    /* Apply exponential smoothing: smoothed = alpha * current + (1 - alpha) * previous */
    *smoothed = *current; // Copy all fields first

    if (previous->valid) {
        smoothed->temperature = alpha * current->temperature + (1.0f - alpha) * previous->temperature;
        smoothed->humidity = alpha * current->humidity + (1.0f - alpha) * previous->humidity;
        smoothed->pressure = alpha * current->pressure + (1.0f - alpha) * previous->pressure;
    }

    return 0;
}

int env_data_update_moving_average(env_data_stats_t *stats, float new_value)
{
    if (!stats) {
        return -EINVAL;
    }

    /* Update min/max */
    update_min_max_avg(stats, new_value);

    /* Update sample count */
    stats->sample_count++;
    stats->last_update = k_uptime_get_32();

    return 0;
}

int env_data_detect_sensor_failure(const env_data_processor_t *processor,
                                  bool *failure_detected,
                                  char *failure_reason)
{
    if (!processor || !failure_detected || !failure_reason) {
        return -EINVAL;
    }

    *failure_detected = false;
    strcpy(failure_reason, "No failure detected");

    if (!processor->initialized) {
        *failure_detected = true;
        strcpy(failure_reason, "Processor not initialized");
        return 0;
    }

    /* Check if data is too old */
    if (env_data_is_stale(processor, 600)) { // 10 minutes
        *failure_detected = true;
        strcpy(failure_reason, "Data is stale (>10 minutes old)");
        return 0;
    }

    /* Check if we have any readings today */
    if (processor->readings_today == 0) {
        uint32_t time_since_reset = k_uptime_get_32() - processor->last_daily_reset;
        if (time_since_reset > (60 * 60 * 1000)) { // 1 hour
            *failure_detected = true;
            strcpy(failure_reason, "No readings for over 1 hour");
            return 0;
        }
    }

    /* Check for stuck readings (all values identical) */
    if (processor->temp_stats.sample_count > 10) {
        float temp_range = processor->temp_stats.max_value - processor->temp_stats.min_value;
        if (temp_range < 0.1f) { // Less than 0.1°C variation
            *failure_detected = true;
            strcpy(failure_reason, "Temperature readings appear stuck");
            return 0;
        }
    }

    return 0;
}

const char* env_data_quality_to_string(env_data_quality_t quality)
{
    switch (quality) {
        case ENV_DATA_QUALITY_EXCELLENT: return "Excellent";
        case ENV_DATA_QUALITY_GOOD: return "Good";
        case ENV_DATA_QUALITY_FAIR: return "Fair";
        case ENV_DATA_QUALITY_POOR: return "Poor";
        case ENV_DATA_QUALITY_INVALID: return "Invalid";
        default: return "Unknown";
    }
}

bool env_data_is_stale(const env_data_processor_t *processor, uint32_t max_age_sec)
{
    if (!processor || !processor->initialized) {
        return true;
    }

    uint32_t current_time = k_uptime_get_32();
    uint32_t age_ms = current_time - processor->current_data.last_update;
    uint32_t max_age_ms = max_age_sec * 1000;

    return (age_ms > max_age_ms);
}

/* Internal helper functions */

static void update_min_max_avg(env_data_stats_t *stats, float value)
{
    /* Update min */
    if (value < stats->min_value || stats->sample_count == 0) {
        stats->min_value = value;
    }

    /* Update max */
    if (value > stats->max_value || stats->sample_count == 0) {
        stats->max_value = value;
    }

    /* Update running average */
    if (stats->sample_count == 0) {
        stats->avg_value = value;
    } else {
        stats->avg_value = (stats->avg_value * stats->sample_count + value) / (stats->sample_count + 1);
    }
}

static float calculate_standard_deviation(const env_data_stats_t *stats)
{
    /* For now, return a simple estimate based on range */
    /* In a full implementation, we would track sum of squares */
    if (stats->sample_count < 2) {
        return 0.0f;
    }

    float range = stats->max_value - stats->min_value;
    return range / 4.0f; // Rough estimate: std dev ≈ range/4 for normal distribution
}

static bool is_value_in_range(float value, float min, float max)
{
    return (value >= min && value <= max && !isnan(value) && !isinf(value));
}
/* Global 
environmental data processor instance */
static env_data_processor_t g_env_processor;
static bool g_env_processor_initialized = false;

int environmental_data_get_current(bme280_environmental_data_t *data)
{
    if (!data) {
        return -EINVAL;
    }

    /* Initialize processor if not already done */
    if (!g_env_processor_initialized) {
        int ret = env_data_processor_init(&g_env_processor);
        if (ret != 0) {
            LOG_ERR("Failed to initialize environmental data processor: %d", ret);
            return ret;
        }
        g_env_processor_initialized = true;
    }

    /* Get current environmental data */
    return env_data_get_current(&g_env_processor, data);
}

int environmental_data_process_bme280_reading(const bme280_reading_t *reading)
{
    if (!reading) {
        return -EINVAL;
    }

    /* Initialize processor if not already done */
    if (!g_env_processor_initialized) {
        int ret = env_data_processor_init(&g_env_processor);
        if (ret != 0) {
            LOG_ERR("Failed to initialize environmental data processor: %d", ret);
            return ret;
        }
        g_env_processor_initialized = true;
    }

    /* Process the BME280 reading */
    return env_data_process_reading(&g_env_processor, reading);
}

/**
 * @brief Initialize the environmental data system
 * 
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t environmental_data_init(void)
{
    if (g_env_processor_initialized) {
        LOG_INF("Environmental data already initialized");
        return WATERING_SUCCESS;
    }

    int ret = env_data_processor_init(&g_env_processor);
    if (ret != 0) {
        LOG_ERR("Failed to initialize environmental data processor: %d", ret);
        return WATERING_ERROR_BME280_INIT;
    }
    
    g_env_processor_initialized = true;
    LOG_INF("Environmental data system initialized");
    return WATERING_SUCCESS;
}
