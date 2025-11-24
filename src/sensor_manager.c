#include "sensor_manager.h"
#include <zephyr/logging/log.h>
#include <zephyr/kernel.h>
#include <string.h>
#include "rain_sensor.h"
#include "rain_integration.h"
#include "rain_history.h"
#include "flow_sensor.h"

LOG_MODULE_REGISTER(sensor_manager, LOG_LEVEL_DBG);

/* Global sensor manager instance */
sensor_manager_t g_sensor_manager;

/* Default configuration */
static const sensor_manager_config_t default_config = {
    .auto_recovery_enabled = true,
    .recovery_timeout_ms = 5000,
    .max_recovery_attempts = 3,
    .health_check_interval_ms = 60000, // 1 minute
    .reading_timeout_ms = 1000
};

/* Internal helper functions */
static int update_sensor_status(sensor_type_t type, sensor_health_t health, 
                               sensor_error_t error, const char *message);
static int attempt_bme280_recovery(void);
static bool is_sensor_response_timeout(sensor_type_t type);

int sensor_manager_init(const sensor_manager_config_t *config)
{
    /* Initialize mutex */
    int ret = k_mutex_init(&g_sensor_manager.sensor_mutex);
    if (ret < 0) {
        LOG_ERR("Failed to initialize sensor mutex: %d", ret);
        return ret;
    }

    /* Lock mutex for initialization */
    ret = k_mutex_lock(&g_sensor_manager.sensor_mutex, K_MSEC(1000));
    if (ret < 0) {
        LOG_ERR("Failed to lock sensor mutex: %d", ret);
        return ret;
    }

    /* Clear manager structure */
    memset(&g_sensor_manager, 0, sizeof(sensor_manager_t));

    /* Set configuration */
    if (config) {
        g_sensor_manager.config = *config;
    } else {
        g_sensor_manager.config = default_config;
    }

    /* Initialize environmental data processor */
    ret = env_data_processor_init(&g_sensor_manager.env_processor);
    if (ret < 0) {
        LOG_ERR("Failed to initialize environmental data processor: %d", ret);
        k_mutex_unlock(&g_sensor_manager.sensor_mutex);
        return ret;
    }

    /* Initialize sensor status structures */
    for (int i = 0; i < SENSOR_TYPE_COUNT; i++) {
        g_sensor_manager.sensor_status[i].type = (sensor_type_t)i;
        g_sensor_manager.sensor_status[i].health = SENSOR_HEALTH_UNKNOWN;
        g_sensor_manager.sensor_status[i].last_error = SENSOR_ERROR_NONE;
        g_sensor_manager.sensor_status[i].enabled = true;
        g_sensor_manager.sensor_status[i].initialized = false;
        strcpy(g_sensor_manager.sensor_status[i].status_message, "Not initialized");
    }

    g_sensor_manager.last_health_check = k_uptime_get_32();
    g_sensor_manager.initialized = true;

    k_mutex_unlock(&g_sensor_manager.sensor_mutex);

    LOG_INF("Sensor manager initialized successfully");
    return 0;
}

int sensor_manager_init_bme280(const struct device *i2c_dev, uint8_t addr)
{
    if (!g_sensor_manager.initialized) {
        LOG_ERR("Sensor manager not initialized");
        return -ENODEV;
    }

    int ret = k_mutex_lock(&g_sensor_manager.sensor_mutex, K_MSEC(1000));
    if (ret < 0) {
        LOG_ERR("Failed to lock sensor mutex: %d", ret);
        return ret;
    }

    /* Initialize BME280 device */
    ret = bme280_init(&g_sensor_manager.bme280, i2c_dev, addr);
    if (ret < 0) {
        LOG_ERR("BME280 initialization failed: %d", ret);
        update_sensor_status(SENSOR_TYPE_BME280, SENSOR_HEALTH_FAILED, 
                           SENSOR_ERROR_INITIALIZATION, "Initialization failed");
        k_mutex_unlock(&g_sensor_manager.sensor_mutex);
        return ret;
    }

    /* Update sensor status */
    update_sensor_status(SENSOR_TYPE_BME280, SENSOR_HEALTH_OK, 
                        SENSOR_ERROR_NONE, "Initialized successfully");
    g_sensor_manager.sensor_status[SENSOR_TYPE_BME280].initialized = true;

    k_mutex_unlock(&g_sensor_manager.sensor_mutex);

    LOG_INF("BME280 sensor initialized at address 0x%02X", addr);
    return 0;
}

int sensor_manager_configure_bme280(const bme280_config_t *config)
{
    if (!g_sensor_manager.initialized) {
        return -ENODEV;
    }

    if (!config) {
        return -EINVAL;
    }

    int ret = k_mutex_lock(&g_sensor_manager.sensor_mutex, K_MSEC(1000));
    if (ret < 0) {
        return ret;
    }

    /* Check if BME280 is initialized */
    if (!g_sensor_manager.sensor_status[SENSOR_TYPE_BME280].initialized) {
        LOG_ERR("BME280 not initialized");
        k_mutex_unlock(&g_sensor_manager.sensor_mutex);
        return -ENODEV;
    }

    /* Configure BME280 */
    ret = bme280_configure(&g_sensor_manager.bme280, config);
    if (ret < 0) {
        LOG_ERR("BME280 configuration failed: %d", ret);
        update_sensor_status(SENSOR_TYPE_BME280, SENSOR_HEALTH_ERROR, 
                           SENSOR_ERROR_CALIBRATION, "Configuration failed");
        k_mutex_unlock(&g_sensor_manager.sensor_mutex);
        return ret;
    }

    /* Update sensor status */
    update_sensor_status(SENSOR_TYPE_BME280, SENSOR_HEALTH_OK, 
                        SENSOR_ERROR_NONE, "Configured successfully");

    k_mutex_unlock(&g_sensor_manager.sensor_mutex);

    LOG_DBG("BME280 sensor configured successfully");
    return 0;
}

int sensor_manager_read_environmental_data(bme280_environmental_data_t *data)
{
    if (!g_sensor_manager.initialized || !data) {
        return -EINVAL;
    }

    int ret = k_mutex_lock(&g_sensor_manager.sensor_mutex, K_MSEC(1000));
    if (ret < 0) {
        return ret;
    }

    /* Check if BME280 is enabled and initialized */
    if (!g_sensor_manager.sensor_status[SENSOR_TYPE_BME280].enabled ||
        !g_sensor_manager.sensor_status[SENSOR_TYPE_BME280].initialized) {
        k_mutex_unlock(&g_sensor_manager.sensor_mutex);
        return -ENODEV;
    }

    /* Read raw data from BME280 */
    bme280_reading_t reading;
    ret = bme280_read_data(&g_sensor_manager.bme280, &reading);
    if (ret < 0) {
        LOG_ERR("Failed to read BME280 data: %d", ret);
        
        /* Update error status */
        sensor_error_t error_type = (ret == -EAGAIN) ? SENSOR_ERROR_TIMEOUT : SENSOR_ERROR_COMMUNICATION;
        update_sensor_status(SENSOR_TYPE_BME280, SENSOR_HEALTH_ERROR, error_type, "Read failed");
        g_sensor_manager.sensor_status[SENSOR_TYPE_BME280].error_count++;
        g_sensor_manager.sensor_status[SENSOR_TYPE_BME280].last_error_time = k_uptime_get_32();
        
        /* Attempt recovery if enabled */
        if (g_sensor_manager.config.auto_recovery_enabled) {
            LOG_INF("Attempting BME280 recovery");
            attempt_bme280_recovery();
        }
        
        k_mutex_unlock(&g_sensor_manager.sensor_mutex);
        return ret;
    }

    /* Process the reading through environmental data processor */
    ret = env_data_process_reading(&g_sensor_manager.env_processor, &reading);
    if (ret < 0) {
        LOG_ERR("Failed to process environmental data: %d", ret);
        update_sensor_status(SENSOR_TYPE_BME280, SENSOR_HEALTH_WARNING, 
                           SENSOR_ERROR_INVALID_DATA, "Data processing failed");
        k_mutex_unlock(&g_sensor_manager.sensor_mutex);
        return ret;
    }

    /* Get processed data */
    ret = env_data_get_current(&g_sensor_manager.env_processor, data);
    if (ret < 0) {
        LOG_ERR("Failed to get current environmental data: %d", ret);
        k_mutex_unlock(&g_sensor_manager.sensor_mutex);
        return ret;
    }

    /* Update success status */
    g_sensor_manager.sensor_status[SENSOR_TYPE_BME280].success_count++;
    g_sensor_manager.sensor_status[SENSOR_TYPE_BME280].last_reading_time = k_uptime_get_32();
    update_sensor_status(SENSOR_TYPE_BME280, SENSOR_HEALTH_OK, 
                        SENSOR_ERROR_NONE, "Reading successful");

    k_mutex_unlock(&g_sensor_manager.sensor_mutex);

    LOG_DBG("Environmental data read successfully");
    return 0;
}

int sensor_manager_trigger_bme280_measurement(void)
{
    if (!g_sensor_manager.initialized) {
        return -ENODEV;
    }

    int ret = k_mutex_lock(&g_sensor_manager.sensor_mutex, K_MSEC(1000));
    if (ret < 0) {
        return ret;
    }

    /* Check if BME280 is enabled and initialized */
    if (!g_sensor_manager.sensor_status[SENSOR_TYPE_BME280].enabled ||
        !g_sensor_manager.sensor_status[SENSOR_TYPE_BME280].initialized) {
        k_mutex_unlock(&g_sensor_manager.sensor_mutex);
        return -ENODEV;
    }

    /* Trigger measurement */
    ret = bme280_trigger_measurement(&g_sensor_manager.bme280);
    if (ret < 0) {
        LOG_ERR("Failed to trigger BME280 measurement: %d", ret);
        update_sensor_status(SENSOR_TYPE_BME280, SENSOR_HEALTH_ERROR, 
                           SENSOR_ERROR_COMMUNICATION, "Trigger failed");
        k_mutex_unlock(&g_sensor_manager.sensor_mutex);
        return ret;
    }

    k_mutex_unlock(&g_sensor_manager.sensor_mutex);

    LOG_DBG("BME280 measurement triggered");
    return 0;
}

int sensor_manager_get_sensor_status(sensor_type_t type, sensor_status_t *status)
{
    if (!g_sensor_manager.initialized || !status || type >= SENSOR_TYPE_COUNT) {
        return -EINVAL;
    }

    int ret = k_mutex_lock(&g_sensor_manager.sensor_mutex, K_MSEC(1000));
    if (ret < 0) {
        return ret;
    }

    *status = g_sensor_manager.sensor_status[type];

    k_mutex_unlock(&g_sensor_manager.sensor_mutex);
    return 0;
}

int sensor_manager_get_all_sensor_status(sensor_status_t statuses[SENSOR_TYPE_COUNT])
{
    if (!g_sensor_manager.initialized || !statuses) {
        return -EINVAL;
    }

    int ret = k_mutex_lock(&g_sensor_manager.sensor_mutex, K_MSEC(1000));
    if (ret < 0) {
        return ret;
    }

    for (int i = 0; i < SENSOR_TYPE_COUNT; i++) {
        statuses[i] = g_sensor_manager.sensor_status[i];
    }

    k_mutex_unlock(&g_sensor_manager.sensor_mutex);
    return 0;
}

int sensor_manager_set_sensor_enabled(sensor_type_t type, bool enabled)
{
    if (!g_sensor_manager.initialized || type >= SENSOR_TYPE_COUNT) {
        return -EINVAL;
    }

    int ret = k_mutex_lock(&g_sensor_manager.sensor_mutex, K_MSEC(1000));
    if (ret < 0) {
        return ret;
    }

    g_sensor_manager.sensor_status[type].enabled = enabled;
    
    const char *status_msg = enabled ? "Enabled" : "Disabled";
    update_sensor_status(type, enabled ? SENSOR_HEALTH_OK : SENSOR_HEALTH_UNKNOWN, 
                        SENSOR_ERROR_NONE, status_msg);

    k_mutex_unlock(&g_sensor_manager.sensor_mutex);

    LOG_INF("Sensor %s %s", sensor_manager_type_to_string(type), status_msg);
    return 0;
}

int sensor_manager_health_check(void)
{
    if (!g_sensor_manager.initialized) {
        return -ENODEV;
    }

    int ret = k_mutex_lock(&g_sensor_manager.sensor_mutex, K_MSEC(1000));
    if (ret < 0) {
        return ret;
    }

    uint32_t current_time = k_uptime_get_32();
    
    /* Check each sensor */
    for (int i = 0; i < SENSOR_TYPE_COUNT; i++) {
        sensor_status_t *status = &g_sensor_manager.sensor_status[i];
        
        if (!status->enabled) {
            continue;
        }

        /* Check for timeout */
        if (is_sensor_response_timeout((sensor_type_t)i)) {
            update_sensor_status((sensor_type_t)i, SENSOR_HEALTH_ERROR, 
                               SENSOR_ERROR_TIMEOUT, "Response timeout");
        }

        /* Check error rate */
        uint32_t total_operations = status->success_count + status->error_count;
        if (total_operations > 10) {
            float error_rate = (float)status->error_count / total_operations;
            if (error_rate > 0.5f) { // More than 50% errors
                update_sensor_status((sensor_type_t)i, SENSOR_HEALTH_WARNING, 
                                   status->last_error, "High error rate");
            }
        }
    }

    g_sensor_manager.last_health_check = current_time;

    k_mutex_unlock(&g_sensor_manager.sensor_mutex);

    LOG_DBG("Sensor health check completed");
    return 0;
}

int sensor_manager_recover_sensor(sensor_type_t type)
{
    if (!g_sensor_manager.initialized || type >= SENSOR_TYPE_COUNT) {
        return -EINVAL;
    }

    int ret = k_mutex_lock(&g_sensor_manager.sensor_mutex, K_MSEC(1000));
    if (ret < 0) {
        return ret;
    }

    LOG_INF("Attempting to recover sensor: %s", sensor_manager_type_to_string(type));

    switch (type) {
        case SENSOR_TYPE_BME280:
            ret = attempt_bme280_recovery();
            break;
        case SENSOR_TYPE_RAIN:
        {
            ret = rain_sensor_init();
            if (ret != 0) {
                update_sensor_status(type, SENSOR_HEALTH_FAILED,
                                   SENSOR_ERROR_INITIALIZATION, "Rain sensor init failed");
                break;
            }

            watering_error_t wret = rain_integration_init();
            if (wret != WATERING_SUCCESS) {
                ret = -EIO;
                update_sensor_status(type, SENSOR_HEALTH_ERROR,
                                   SENSOR_ERROR_COMMUNICATION, "Rain integration init failed");
                break;
            }

            wret = rain_history_init();
            if (wret != WATERING_SUCCESS) {
                ret = -EIO;
                update_sensor_status(type, SENSOR_HEALTH_WARNING,
                                   SENSOR_ERROR_INVALID_DATA, "Rain history init failed");
                break;
            }

            rain_sensor_clear_errors();
            rain_sensor_reset_counters();

            sensor_status_t *status = &g_sensor_manager.sensor_status[type];
            status->error_count = 0;
            status->success_count++;
            status->initialized = true;
            status->last_reading_time = k_uptime_get_32();

            update_sensor_status(type, SENSOR_HEALTH_OK, SENSOR_ERROR_NONE, "Rain sensor recovered");
            ret = 0;
            break;
        }
        case SENSOR_TYPE_FLOW:
        {
            ret = flow_sensor_init();
            if (ret != 0) {
                update_sensor_status(type, SENSOR_HEALTH_FAILED,
                                   SENSOR_ERROR_INITIALIZATION, "Flow sensor init failed");
                break;
            }

            reset_pulse_count();

            sensor_status_t *status = &g_sensor_manager.sensor_status[type];
            status->error_count = 0;
            status->success_count++;
            status->initialized = true;
            status->last_reading_time = k_uptime_get_32();

            update_sensor_status(type, SENSOR_HEALTH_OK, SENSOR_ERROR_NONE, "Flow sensor recovered");
            ret = 0;
            break;
        }
        default:
            ret = -ENOTSUP;
            break;
    }

    k_mutex_unlock(&g_sensor_manager.sensor_mutex);

    if (ret == 0) {
        LOG_INF("Sensor recovery successful: %s", sensor_manager_type_to_string(type));
    } else {
        LOG_ERR("Sensor recovery failed: %s (%d)", sensor_manager_type_to_string(type), ret);
    }

    return ret;
}

sensor_health_t sensor_manager_get_overall_health(void)
{
    if (!g_sensor_manager.initialized) {
        return SENSOR_HEALTH_UNKNOWN;
    }

    sensor_health_t overall_health = SENSOR_HEALTH_OK;

    int ret = k_mutex_lock(&g_sensor_manager.sensor_mutex, K_MSEC(1000));
    if (ret < 0) {
        return SENSOR_HEALTH_UNKNOWN;
    }

    /* Check all enabled sensors */
    for (int i = 0; i < SENSOR_TYPE_COUNT; i++) {
        if (!g_sensor_manager.sensor_status[i].enabled) {
            continue;
        }

        sensor_health_t sensor_health = g_sensor_manager.sensor_status[i].health;
        
        /* Overall health is the worst individual sensor health */
        if (sensor_health > overall_health) {
            overall_health = sensor_health;
        }
    }

    k_mutex_unlock(&g_sensor_manager.sensor_mutex);
    return overall_health;
}

bool sensor_manager_is_data_fresh(sensor_type_t type, uint32_t max_age_ms)
{
    if (!g_sensor_manager.initialized || type >= SENSOR_TYPE_COUNT) {
        return false;
    }

    int ret = k_mutex_lock(&g_sensor_manager.sensor_mutex, K_MSEC(100));
    if (ret < 0) {
        return false;
    }

    uint32_t current_time = k_uptime_get_32();
    uint32_t last_reading = g_sensor_manager.sensor_status[type].last_reading_time;
    bool is_fresh = (current_time - last_reading) <= max_age_ms;

    k_mutex_unlock(&g_sensor_manager.sensor_mutex);
    return is_fresh;
}

/* String conversion functions */

const char* sensor_manager_error_to_string(sensor_error_t error)
{
    switch (error) {
        case SENSOR_ERROR_NONE: return "No error";
        case SENSOR_ERROR_COMMUNICATION: return "Communication error";
        case SENSOR_ERROR_TIMEOUT: return "Timeout";
        case SENSOR_ERROR_INVALID_DATA: return "Invalid data";
        case SENSOR_ERROR_CALIBRATION: return "Calibration error";
        case SENSOR_ERROR_HARDWARE: return "Hardware failure";
        case SENSOR_ERROR_POWER: return "Power issue";
        case SENSOR_ERROR_INITIALIZATION: return "Initialization failure";
        default: return "Unknown error";
    }
}

const char* sensor_manager_health_to_string(sensor_health_t health)
{
    switch (health) {
        case SENSOR_HEALTH_OK: return "OK";
        case SENSOR_HEALTH_WARNING: return "Warning";
        case SENSOR_HEALTH_ERROR: return "Error";
        case SENSOR_HEALTH_FAILED: return "Failed";
        case SENSOR_HEALTH_UNKNOWN: return "Unknown";
        default: return "Invalid";
    }
}

const char* sensor_manager_type_to_string(sensor_type_t type)
{
    switch (type) {
        case SENSOR_TYPE_BME280: return "BME280";
        case SENSOR_TYPE_RAIN: return "Rain";
        case SENSOR_TYPE_FLOW: return "Flow";
        default: return "Unknown";
    }
}

int sensor_manager_reset_error_counters(sensor_type_t type)
{
    if (!g_sensor_manager.initialized) {
        return -ENODEV;
    }

    int ret = k_mutex_lock(&g_sensor_manager.sensor_mutex, K_MSEC(1000));
    if (ret < 0) {
        return ret;
    }

    if (type == SENSOR_TYPE_COUNT) {
        /* Reset all sensors */
        for (int i = 0; i < SENSOR_TYPE_COUNT; i++) {
            g_sensor_manager.sensor_status[i].error_count = 0;
            g_sensor_manager.sensor_status[i].success_count = 0;
        }
        LOG_INF("All sensor error counters reset");
    } else if (type < SENSOR_TYPE_COUNT) {
        /* Reset specific sensor */
        g_sensor_manager.sensor_status[type].error_count = 0;
        g_sensor_manager.sensor_status[type].success_count = 0;
        LOG_INF("Error counters reset for sensor: %s", sensor_manager_type_to_string(type));
    } else {
        k_mutex_unlock(&g_sensor_manager.sensor_mutex);
        return -EINVAL;
    }

    k_mutex_unlock(&g_sensor_manager.sensor_mutex);
    return 0;
}

int sensor_manager_set_config(const sensor_manager_config_t *config)
{
    if (!g_sensor_manager.initialized || !config) {
        return -EINVAL;
    }

    int ret = k_mutex_lock(&g_sensor_manager.sensor_mutex, K_MSEC(1000));
    if (ret < 0) {
        return ret;
    }

    g_sensor_manager.config = *config;

    k_mutex_unlock(&g_sensor_manager.sensor_mutex);

    LOG_INF("Sensor manager configuration updated");
    return 0;
}

int sensor_manager_get_config(sensor_manager_config_t *config)
{
    if (!g_sensor_manager.initialized || !config) {
        return -EINVAL;
    }

    int ret = k_mutex_lock(&g_sensor_manager.sensor_mutex, K_MSEC(1000));
    if (ret < 0) {
        return ret;
    }

    *config = g_sensor_manager.config;

    k_mutex_unlock(&g_sensor_manager.sensor_mutex);
    return 0;
}

int sensor_manager_shutdown(void)
{
    if (!g_sensor_manager.initialized) {
        return -ENODEV;
    }

    int ret = k_mutex_lock(&g_sensor_manager.sensor_mutex, K_MSEC(1000));
    if (ret < 0) {
        return ret;
    }

    /* Disable all sensors */
    for (int i = 0; i < SENSOR_TYPE_COUNT; i++) {
        g_sensor_manager.sensor_status[i].enabled = false;
        update_sensor_status((sensor_type_t)i, SENSOR_HEALTH_UNKNOWN, 
                           SENSOR_ERROR_NONE, "Shutdown");
    }

    g_sensor_manager.initialized = false;

    k_mutex_unlock(&g_sensor_manager.sensor_mutex);

    LOG_INF("Sensor manager shutdown completed");
    return 0;
}

/* Internal helper functions */

static int update_sensor_status(sensor_type_t type, sensor_health_t health, 
                               sensor_error_t error, const char *message)
{
    if (type >= SENSOR_TYPE_COUNT) {
        return -EINVAL;
    }

    sensor_status_t *status = &g_sensor_manager.sensor_status[type];
    status->health = health;
    status->last_error = error;
    
    if (message) {
        strncpy(status->status_message, message, sizeof(status->status_message) - 1);
        status->status_message[sizeof(status->status_message) - 1] = '\0';
    }

    return 0;
}

static int attempt_bme280_recovery(void)
{
    LOG_INF("Attempting BME280 recovery");

    bme280_config_t previous_cfg = g_sensor_manager.bme280.config;
    bme280_config_t defaults;
    if (bme280_get_config(&defaults) != 0) {
        defaults = previous_cfg;
    }

    int ret = bme280_init(&g_sensor_manager.bme280, NULL, 0);
    if (ret < 0) {
        LOG_ERR("BME280 rebind failed: %d", ret);
        update_sensor_status(SENSOR_TYPE_BME280, SENSOR_HEALTH_FAILED,
                             SENSOR_ERROR_HARDWARE, "Recovery failed");
        return ret;
    }

    if (previous_cfg.measurement_interval == 0) {
        previous_cfg.measurement_interval = defaults.measurement_interval;
    }

    ret = bme280_configure(&g_sensor_manager.bme280, &previous_cfg);
    if (ret < 0) {
        LOG_WRN("BME280 reconfiguration after recovery failed: %d", ret);
        update_sensor_status(SENSOR_TYPE_BME280, SENSOR_HEALTH_WARNING,
                             SENSOR_ERROR_CALIBRATION, "Recovery partial");
        return ret;
    }

    update_sensor_status(SENSOR_TYPE_BME280, SENSOR_HEALTH_OK,
                         SENSOR_ERROR_NONE, "Recovery successful");

    LOG_INF("BME280 recovery completed successfully");
    return 0;
}

static bool is_sensor_response_timeout(sensor_type_t type)
{
    if (type >= SENSOR_TYPE_COUNT) {
        return false;
    }

    sensor_status_t *status = &g_sensor_manager.sensor_status[type];
    
    if (!status->enabled || !status->initialized) {
        return false;
    }

    uint32_t current_time = k_uptime_get_32();
    uint32_t time_since_last_reading = current_time - status->last_reading_time;
    
    /* Consider timeout if no reading for more than configured timeout */
    return (time_since_last_reading > g_sensor_manager.config.reading_timeout_ms);
}
