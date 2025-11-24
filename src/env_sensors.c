/* NOTE: This file was previously a full simulation provider.
 * It has been refactored to use real sensor backends when available.
 * Simulation code is retained behind CONFIG_ENV_SENSORS_SIM so that
 * test environments can still generate deterministic data without
 * contaminating production builds with artificial values.
 */

#include "env_sensors.h"
#include "bme280_driver.h"          /* Real environmental sensor */
#include "rain_history.h"            /* Real rainfall aggregation */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>
#include <math.h>
#include <string.h>

#ifdef CONFIG_ZTEST
#include "sensor_emulators.h"
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Use a float-precision PI to avoid implicit float->double promotions in float math
#ifndef M_PI_F
#define M_PI_F 3.14159265358979323846f
#endif

LOG_MODULE_REGISTER(env_sensors, LOG_LEVEL_INF);

/* --------------------------------------------------------------- */
/* Configuration switches                                          */
/* --------------------------------------------------------------- */
#ifdef CONFIG_ENV_SENSORS_SIM
#define ENV_SENSORS_USE_SIMULATION 1
#endif

/**
 * @file env_sensors.c
 * @brief Environmental sensor provider (production + optional simulation)
 *
 * Production mode (default):
 *  - Reads temperature / humidity / pressure from BME280 via bme280_system_read_data()
 *  - Rainfall sourced from rain_history (24h aggregation)
 *  - Wind / solar / soil moisture currently not instrumented -> marked invalid
 *  - No artificial data generation: if a sensor is unavailable, flags become false
 *
 * Simulation mode (CONFIG_ENV_SENSORS_SIM):
 *  - Retains legacy pseudo-realistic generators for development & tests
 *
 * Goal: eliminate silent injection de date artificiale în build-urile de producție.
 */

// Static configuration and state
static env_sensor_config_t sensor_config;
static env_sensor_status_t sensor_status;
static bool system_initialized = false;

#ifndef ENV_SENSORS_USE_SIMULATION
static bme280_reading_t last_bme_reading;
static bool last_bme_valid;
static uint32_t last_bme_timestamp;
#endif

/* Unified default configuration (conditional fields differ by build mode) */
static const env_sensor_config_t default_config = {
    .enable_temp_sensor = true,
    .enable_humidity_sensor = true,
    .enable_pressure_sensor = true,
    /* solar / wind / soil flags removed from public config; only rain retained */
    .enable_rain_sensor = true,
    .temp_interval_min = 15,
    .humidity_interval_min = 15,
    .rain_interval_min = 60,
    /* soil_interval_min removed */
    .temp_offset_c = 0.0f,
    .humidity_offset_pct = 0.0f,
    .rain_calibration_factor = 1.0f,
    /* soil_moisture_offset_pct removed */
    .min_data_quality = 80,
    .max_sensor_age_min = 120,
};

/* Simulation state (only used when ENV_SENSORS_USE_SIMULATION) */
#ifdef ENV_SENSORS_USE_SIMULATION
static uint32_t simulation_start_time;
static uint16_t simulation_day_offset = 0;  // Days since simulation start
#endif

// No soil sensor forward declarations (soil subsystem removed)

/* -------------------------------------------------------------------------- */
/* Simulation-only helpers & configuration                                   */
/* -------------------------------------------------------------------------- */
#ifdef ENV_SENSORS_USE_SIMULATION
/**
 * @brief Generate realistic temperature based on time of day and season
 */
static float generate_temperature(uint32_t timestamp, bool is_min, bool is_max) {
    // Base temperature varies by season (simplified Northern Hemisphere)
    uint16_t day_of_year = (timestamp / 86400 + simulation_day_offset) % 365;
    float seasonal_temp = 20.0f + 15.0f * sinf(2.0f * M_PI_F * (day_of_year - 80) / 365.0f);
    
    // Daily temperature variation
    uint32_t seconds_in_day = timestamp % 86400;
    float hour_of_day = seconds_in_day / 3600.0f;
    
    // Temperature peaks around 2 PM (14:00), minimum around 6 AM
    float daily_variation = 8.0f * sinf(2.0f * M_PI_F * (hour_of_day - 6.0f) / 24.0f);
    
    float base_temp = seasonal_temp + daily_variation;
    
    if (is_min) {
        return base_temp - 5.0f;  // Daily minimum
    } else if (is_max) {
        return base_temp + 5.0f;  // Daily maximum
    } else {
        return base_temp;         // Current/mean temperature
    }
}

/**
 * @brief Generate realistic humidity based on temperature and season
 */
static float generate_humidity(float temperature, uint32_t timestamp) {
    // Base humidity varies seasonally (higher in summer)
    uint16_t day_of_year = (timestamp / 86400 + simulation_day_offset) % 365;
    float seasonal_humidity = 60.0f + 20.0f * sinf(2.0f * M_PI_F * (day_of_year - 80) / 365.0f);
    
    // Humidity inversely related to temperature
    float temp_effect = -0.5f * (temperature - 20.0f);
    
    // Daily variation (higher at night)
    uint32_t seconds_in_day = timestamp % 86400;
    float hour_of_day = seconds_in_day / 3600.0f;
    float daily_variation = 10.0f * cosf(2.0f * M_PI_F * (hour_of_day - 14.0f) / 24.0f);
    
    float humidity = seasonal_humidity + temp_effect + daily_variation;
    
    // Clamp to realistic range
    if (humidity < 20.0f) humidity = 20.0f;
    if (humidity > 95.0f) humidity = 95.0f;
    
    return humidity;
}

/**
 * @brief Generate realistic atmospheric pressure
 */
static float generate_pressure(uint32_t timestamp) {
    // Standard atmospheric pressure with small variations
    float base_pressure = 1013.25f;
    
    // Simulate weather system variations
    float weather_variation = 20.0f * sinf(2.0f * M_PI_F * timestamp / (7.0f * 86400.0f)); // 7-day cycle
    
    // Small random-like variation based on timestamp
    float micro_variation = 5.0f * sinf(timestamp * 0.001f);
    
    return base_pressure + weather_variation + micro_variation;
}


/**
 * @brief Generate realistic rainfall data
 */
static float generate_rainfall(uint32_t timestamp) {
    // Simulate occasional rain events
    uint32_t rain_seed = timestamp / 86400;  // Daily rain probability
    
    // Simple pseudo-random based on timestamp
    float rain_probability = fmodf(sinf(rain_seed * 12.9898f) * 43758.5453f, 1.0f);
    
    if (rain_probability < 0.0f) rain_probability = -rain_probability;
    
    // 20% chance of rain on any given day
    if (rain_probability < 0.2f) {
        // Rain amount varies from light to heavy
        float rain_intensity = fmodf(sinf(rain_seed * 78.233f) * 43758.5453f, 1.0f);
        if (rain_intensity < 0.0f) rain_intensity = -rain_intensity;
        
        // Light rain (2-5mm) to heavy rain (15-30mm)
        return 2.0f + rain_intensity * 28.0f;
    }
    
    return 0.0f;  // No rain
}

/* Solar and wind generators removed (no fabrication policy). */

#endif /* ENV_SENSORS_USE_SIMULATION */

/* -------------------------------------------------------------------------- */
/* Public API (always compiled). Simulation vs production handled inside.    */
/* -------------------------------------------------------------------------- */

watering_error_t env_sensors_init(void) {
    LOG_INF("Initializing environmental sensor system (%s)",
        #ifdef ENV_SENSORS_USE_SIMULATION
        "simulation"
        #else
        "production"
        #endif
    );
    
    // Initialize configuration with defaults
    memcpy(&sensor_config, &default_config, sizeof(env_sensor_config_t));
    
    // Initialize status structure
    memset(&sensor_status, 0, sizeof(env_sensor_status_t));
    
    // Mark sensors online according to the active configuration
#ifdef CONFIG_ZTEST
    // In test mode, check emulator status
    sensor_status.temp_sensor_online = sensor_config.enable_temp_sensor && sensor_emulator_get_temperature_online();
    sensor_status.humidity_sensor_online = sensor_config.enable_humidity_sensor && sensor_emulator_get_humidity_online();
    sensor_status.pressure_sensor_online = sensor_config.enable_pressure_sensor && sensor_emulator_get_pressure_online();
#else
    sensor_status.temp_sensor_online = sensor_config.enable_temp_sensor;
    sensor_status.humidity_sensor_online = sensor_config.enable_humidity_sensor;
    sensor_status.pressure_sensor_online = sensor_config.enable_pressure_sensor;
#endif
    sensor_status.rain_sensor_online = sensor_config.enable_rain_sensor;
    
    // Initialize timestamps
    uint32_t current_time = k_uptime_get_32() / 1000;  // Convert to seconds
    sensor_status.last_temp_reading = current_time;
    sensor_status.last_humidity_reading = current_time;
    sensor_status.last_rain_reading = current_time;
    sensor_status.last_full_reading = current_time;
    
    // Initial overall health set to 100%
    sensor_status.overall_health = 100;
    
    // Initialize simulation parameters (if enabled)
#ifdef ENV_SENSORS_USE_SIMULATION
    simulation_start_time = current_time;
#endif
    
#ifndef ENV_SENSORS_USE_SIMULATION
    memset(&last_bme_reading, 0, sizeof(last_bme_reading));
    last_bme_valid = false;
    last_bme_timestamp = 0;
#endif

    // Soil moisture sensors removed – no initialization required
    
    system_initialized = true;
    
    LOG_INF("Environmental sensor system initialized successfully");
    return WATERING_SUCCESS;
}

watering_error_t env_sensors_read(environmental_data_t *data) {
    if (!system_initialized) {
        LOG_ERR("Environmental sensor system not initialized");
        return WATERING_ERROR_NOT_INITIALIZED;
    }
    
    if (data == NULL) {
        LOG_ERR("Invalid data pointer");
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    // Clear the data structure
    memset(data, 0, sizeof(environmental_data_t));
    
    uint32_t current_time = k_uptime_get_32() / 1000;  // Convert to seconds
    data->timestamp = current_time;
    data->measurement_interval_min = 15;  // Default measurement interval

#ifdef CONFIG_ZTEST
    // In test mode, update sensor status from emulators
    sensor_status.temp_sensor_online = sensor_config.enable_temp_sensor && sensor_emulator_get_temperature_online();
    sensor_status.humidity_sensor_online = sensor_config.enable_humidity_sensor && sensor_emulator_get_humidity_online();
    sensor_status.pressure_sensor_online = sensor_config.enable_pressure_sensor && sensor_emulator_get_pressure_online();
#endif
    
    // --------------------------------------------------------------------
    // Temperature / Humidity / Pressure from real sensor (production)
    // --------------------------------------------------------------------
#ifndef ENV_SENSORS_USE_SIMULATION
    bool temp_due = false;
    bool humidity_due = false;
    bool pressure_due = false;

    if (sensor_config.enable_temp_sensor) {
        uint32_t temp_interval_s = (uint32_t)sensor_config.temp_interval_min * 60U;
        temp_due = !last_bme_valid || temp_interval_s == 0U ||
                   (current_time - sensor_status.last_temp_reading) >= temp_interval_s;
    }

    if (sensor_config.enable_humidity_sensor) {
        uint32_t humidity_interval_s = (uint32_t)sensor_config.humidity_interval_min * 60U;
        humidity_due = !last_bme_valid || humidity_interval_s == 0U ||
                       (current_time - sensor_status.last_humidity_reading) >= humidity_interval_s;
    }

    if (sensor_config.enable_pressure_sensor) {
        uint32_t pressure_interval_s = (uint32_t)sensor_config.temp_interval_min * 60U;
        pressure_due = !last_bme_valid || pressure_interval_s == 0U ||
                       (current_time - last_bme_timestamp) >= pressure_interval_s;
    }

    bool need_bme_sample = temp_due || humidity_due || pressure_due;
    if (need_bme_sample) {
        bme280_reading_t bme = {0};
        int ret = bme280_system_read_data(&bme);
        if (ret == -EAGAIN) {
            /* Trigger & retry once */
            bme280_system_trigger_measurement();
            k_msleep(120);
            ret = bme280_system_read_data(&bme);
        }

        if (ret == 0 && bme.valid) {
            last_bme_reading = bme;
            last_bme_valid = true;
            last_bme_timestamp = current_time;

            if (sensor_config.enable_temp_sensor) {
                sensor_status.last_temp_reading = current_time;
            }
            if (sensor_config.enable_humidity_sensor) {
                sensor_status.last_humidity_reading = current_time;
            }
        } else if (ret < 0) {
            if (sensor_config.enable_temp_sensor) {
                sensor_status.temp_error_count++;
            }
            if (sensor_config.enable_humidity_sensor) {
                sensor_status.humidity_error_count++;
            }
        }
    }
#endif /* !ENV_SENSORS_USE_SIMULATION */

#ifdef ENV_SENSORS_USE_SIMULATION
    // Generate temperature data (simulation path)
    if (sensor_config.enable_temp_sensor && sensor_status.temp_sensor_online) {
#ifdef CONFIG_ZTEST
        // In test mode, use emulated sensor values if available
        float emulated_temp = sensor_emulator_get_temperature();
        if (!isnan(emulated_temp)) {
            data->air_temp_mean_c = emulated_temp + sensor_config.temp_offset_c;
            data->air_temp_min_c = emulated_temp + sensor_config.temp_offset_c - 2.0f;
            data->air_temp_max_c = emulated_temp + sensor_config.temp_offset_c + 2.0f;
        } else {
            data->air_temp_mean_c = generate_temperature(current_time, false, false) + sensor_config.temp_offset_c;
            data->air_temp_min_c = generate_temperature(current_time, true, false) + sensor_config.temp_offset_c;
            data->air_temp_max_c = generate_temperature(current_time, false, true) + sensor_config.temp_offset_c;
        }
#else
        data->air_temp_mean_c = generate_temperature(current_time, false, false) + sensor_config.temp_offset_c;
        data->air_temp_min_c = generate_temperature(current_time, true, false) + sensor_config.temp_offset_c;
        data->air_temp_max_c = generate_temperature(current_time, false, true) + sensor_config.temp_offset_c;
#endif
        data->temp_valid = true;
        sensor_status.last_temp_reading = current_time;
    } else {
        data->temp_valid = false;
    }
#else /* production path */
    if (sensor_config.enable_temp_sensor &&
        sensor_status.temp_sensor_online &&
        last_bme_valid) {
        data->air_temp_mean_c = last_bme_reading.temperature + sensor_config.temp_offset_c;
        data->air_temp_min_c = data->air_temp_mean_c; // Until daily aggregation implemented
        data->air_temp_max_c = data->air_temp_mean_c;
        data->temp_valid = true;
    } else {
        data->temp_valid = false;
    }
#endif
    
    // Generate humidity data
#ifdef ENV_SENSORS_USE_SIMULATION
    if (sensor_config.enable_humidity_sensor && sensor_status.humidity_sensor_online) {
#ifdef CONFIG_ZTEST
        // In test mode, use emulated sensor values if available
        float emulated_humidity = sensor_emulator_get_humidity();
        if (!isnan(emulated_humidity)) {
            data->rel_humidity_pct = emulated_humidity + sensor_config.humidity_offset_pct;
        } else {
            data->rel_humidity_pct = generate_humidity(data->air_temp_mean_c, current_time) + sensor_config.humidity_offset_pct;
        }
#else
        data->rel_humidity_pct = generate_humidity(data->air_temp_mean_c, current_time) + sensor_config.humidity_offset_pct;
#endif
        // Clamp humidity to valid range
        if (data->rel_humidity_pct < 0.0f) data->rel_humidity_pct = 0.0f;
        if (data->rel_humidity_pct > 100.0f) data->rel_humidity_pct = 100.0f;
        data->humidity_valid = true;
        sensor_status.last_humidity_reading = current_time;
    } else {
        data->humidity_valid = false;
    }
#else
    if (sensor_config.enable_humidity_sensor &&
        sensor_status.humidity_sensor_online &&
        last_bme_valid) {
        data->rel_humidity_pct = last_bme_reading.humidity + sensor_config.humidity_offset_pct;
        if (data->rel_humidity_pct < 0.0f) data->rel_humidity_pct = 0.0f;
        if (data->rel_humidity_pct > 100.0f) data->rel_humidity_pct = 100.0f;
        data->humidity_valid = true;
    } else {
        data->humidity_valid = false;
    }
#endif
    
    // Generate pressure data
#ifdef ENV_SENSORS_USE_SIMULATION
    if (sensor_config.enable_pressure_sensor && sensor_status.pressure_sensor_online) {
#ifdef CONFIG_ZTEST
        // In test mode, use emulated sensor values if available
        float emulated_pressure = sensor_emulator_get_pressure();
        if (!isnan(emulated_pressure)) {
            data->atmos_pressure_hpa = emulated_pressure;
        } else {
            data->atmos_pressure_hpa = generate_pressure(current_time);
        }
#else
        data->atmos_pressure_hpa = generate_pressure(current_time);
#endif
        data->pressure_valid = true;
    } else {
        data->pressure_valid = false;
    }
#else
    if (sensor_config.enable_pressure_sensor &&
        sensor_status.pressure_sensor_online &&
        last_bme_valid) {
        data->atmos_pressure_hpa = last_bme_reading.pressure;
        data->pressure_valid = true;
    } else {
        data->pressure_valid = false;
    }
#endif
    
    // Solar & wind removed (no fabrication, fields no longer exist)

    // Rainfall: production uses aggregation (last 24h) from history
#ifdef ENV_SENSORS_USE_SIMULATION
    if (sensor_config.enable_rain_sensor && sensor_status.rain_sensor_online) {
        data->rain_mm_24h = generate_rainfall(current_time) * sensor_config.rain_calibration_factor;
        data->rain_valid = true;
        sensor_status.last_rain_reading = current_time;
    } else {
        data->rain_valid = false;
    }
#else
    if (sensor_config.enable_rain_sensor) {
        data->rain_mm_24h = rain_history_get_last_24h() * sensor_config.rain_calibration_factor;
        data->rain_valid = true; /* Assume history valid if system initialized */
        sensor_status.last_rain_reading = current_time;
    } else {
        data->rain_valid = false;
    }
#endif
    
    // Soil sensors removed

    // Calculate overall data quality
    uint8_t valid_sensors = 0;
    uint8_t total_enabled_sensors = 0;
    
    if (sensor_config.enable_temp_sensor) {
        total_enabled_sensors++;
        if (data->temp_valid) valid_sensors++;
    }
    if (sensor_config.enable_humidity_sensor) {
        total_enabled_sensors++;
        if (data->humidity_valid) valid_sensors++;
    }
    if (sensor_config.enable_pressure_sensor) {
        total_enabled_sensors++;
        if (data->pressure_valid) valid_sensors++;
    }
    /* solar removed */
    if (sensor_config.enable_rain_sensor) {
        total_enabled_sensors++;
        if (data->rain_valid) valid_sensors++;
    }
    /* wind removed */
    // Soil sensors removed; do not count toward quality
    
    if (total_enabled_sensors > 0) {
        data->data_quality = (valid_sensors * 100) / total_enabled_sensors;
    } else {
        data->data_quality = 0;
    }
    
    // Mark derived values as not calculated yet
    data->derived_values_calculated = false;
    
    sensor_status.last_full_reading = current_time;

#ifndef ENV_SENSORS_USE_SIMULATION
    uint32_t max_age_s = (uint32_t)sensor_config.max_sensor_age_min * 60U;
    if (max_age_s > 0U) {
        if (data->temp_valid) {
            uint32_t age = current_time - sensor_status.last_temp_reading;
            if (age > max_age_s) {
                data->temp_valid = false;
            }
        }
        if (data->humidity_valid) {
            uint32_t age = current_time - sensor_status.last_humidity_reading;
            if (age > max_age_s) {
                data->humidity_valid = false;
            }
        }
        if (data->pressure_valid) {
            uint32_t age = current_time - last_bme_timestamp;
            if (age > max_age_s) {
                data->pressure_valid = false;
            }
        }
    }
#endif

    uint16_t measurement_interval = 0;
    if (sensor_config.enable_temp_sensor) {
        measurement_interval = sensor_config.temp_interval_min;
    }
    if (sensor_config.enable_humidity_sensor) {
        measurement_interval = (measurement_interval == 0) ?
            sensor_config.humidity_interval_min :
            MIN(measurement_interval, sensor_config.humidity_interval_min);
    }
    if (sensor_config.enable_rain_sensor) {
        measurement_interval = (measurement_interval == 0) ?
            sensor_config.rain_interval_min :
            MIN(measurement_interval, sensor_config.rain_interval_min);
    }
    data->measurement_interval_min = measurement_interval;
    
    LOG_DBG("Environmental data read (%s): T=%.1f°C valid=%d, RH=%.1f%% valid=%d, P=%.1fhPa valid=%d, Rain=%.2fmm valid=%d, Quality=%d%%",
#ifdef ENV_SENSORS_USE_SIMULATION
            "sim",
#else
            "prod",
#endif
            (double)data->air_temp_mean_c, data->temp_valid,
            (double)data->rel_humidity_pct, data->humidity_valid,
            (double)data->atmos_pressure_hpa, data->pressure_valid,
            (double)data->rain_mm_24h, data->rain_valid,
            data->data_quality);
    
    return WATERING_SUCCESS;
}

watering_error_t env_sensors_get_status(env_sensor_status_t *status) {
    if (!system_initialized) {
        return WATERING_ERROR_NOT_INITIALIZED;
    }
    
    if (status == NULL) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    // Copy current status
    memcpy(status, &sensor_status, sizeof(env_sensor_status_t));
    
    return WATERING_SUCCESS;
}

watering_error_t env_sensors_configure(const env_sensor_config_t *config) {
    if (!system_initialized) {
        return WATERING_ERROR_NOT_INITIALIZED;
    }
    
    if (config == NULL) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    LOG_INF("Updating environmental sensor configuration");
    
    // Update configuration
    memcpy(&sensor_config, config, sizeof(env_sensor_config_t));
    
    // Update sensor online status based on new configuration
#ifdef CONFIG_ZTEST
    // In test mode, check emulator status
    sensor_status.temp_sensor_online = sensor_config.enable_temp_sensor && sensor_emulator_get_temperature_online();
    sensor_status.humidity_sensor_online = sensor_config.enable_humidity_sensor && sensor_emulator_get_humidity_online();
    sensor_status.pressure_sensor_online = sensor_config.enable_pressure_sensor && sensor_emulator_get_pressure_online();
#else
    sensor_status.temp_sensor_online = sensor_config.enable_temp_sensor;
    sensor_status.humidity_sensor_online = sensor_config.enable_humidity_sensor;
    sensor_status.pressure_sensor_online = sensor_config.enable_pressure_sensor;
#endif
    sensor_status.rain_sensor_online = sensor_config.enable_rain_sensor;
    
    LOG_INF("Environmental sensor configuration updated successfully");
    return WATERING_SUCCESS;
}

watering_error_t env_sensors_calibrate(void) {
    if (!system_initialized) {
        return WATERING_ERROR_NOT_INITIALIZED;
    }
    
    LOG_INF("Performing environmental sensor calibration (simulation pathway)");
    
    // In a real implementation, this would perform sensor calibration
    // Simulation builds simply reset error counters and report success
    
    // Reset error counters as part of calibration
    sensor_status.temp_error_count = 0;
    sensor_status.humidity_error_count = 0;
    sensor_status.rain_error_count = 0;
    /* soil_error_count removed */
    
    LOG_INF("Environmental sensor calibration completed successfully");
    return WATERING_SUCCESS;
}

watering_error_t env_sensors_calculate_derived(environmental_data_t *data) {
    if (data == NULL) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    // Only calculate if we have valid temperature and humidity
    if (!data->temp_valid || !data->humidity_valid) {
        data->derived_values_calculated = false;
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    float temp_c = data->air_temp_mean_c;
    float rh_pct = data->rel_humidity_pct;
    
    // Calculate saturation vapor pressure using Tetens formula
    // es = 0.6108 * exp(17.27 * T / (T + 237.3))
    data->saturation_vapor_pressure_kpa = 0.6108f * expf(17.27f * temp_c / (temp_c + 237.3f));
    
    // Calculate actual vapor pressure
    // ea = es * RH / 100
    data->vapor_pressure_kpa = data->saturation_vapor_pressure_kpa * rh_pct / 100.0f;
    
    // Calculate dewpoint temperature
    // Td = 237.3 * ln(ea/0.6108) / (17.27 - ln(ea/0.6108))
    if (data->vapor_pressure_kpa > 0.0f) {
        float ln_ea = logf(data->vapor_pressure_kpa / 0.6108f);
        data->dewpoint_temp_c = 237.3f * ln_ea / (17.27f - ln_ea);
    } else {
        data->dewpoint_temp_c = temp_c - 20.0f;  // Fallback estimate
    }
    
    data->derived_values_calculated = true;
    
    LOG_DBG("Calculated derived values: es=%.3fkPa, ea=%.3fkPa, Td=%.1f°C",
            (double)data->saturation_vapor_pressure_kpa, (double)data->vapor_pressure_kpa, (double)data->dewpoint_temp_c);
    
    return WATERING_SUCCESS;
}

watering_error_t env_sensors_generate_fallback(environmental_data_t *data, 
                                               float latitude_deg, 
                                               uint16_t day_of_year) {
    if (data == NULL) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    LOG_WRN("Generating fallback environmental data");
    
    // Clear the data structure
    memset(data, 0, sizeof(environmental_data_t));
    
    uint32_t current_time = k_uptime_get_32() / 1000;
    data->timestamp = current_time;
    data->measurement_interval_min = 60;  // Fallback data updated hourly
    
    // Generate conservative fallback values based on season and location
    
    // Temperature: seasonal variation based on latitude and day of year
    float seasonal_temp_base = 20.0f;  // Moderate base temperature
    if (fabsf(latitude_deg) > 40.0f) {
        // Higher latitudes have more seasonal variation
    seasonal_temp_base = 15.0f + 10.0f * cosf(2.0f * M_PI_F * (day_of_year - 172) / 365.0f);
    } else {
        // Lower latitudes have less seasonal variation
    seasonal_temp_base = 22.0f + 5.0f * cosf(2.0f * M_PI_F * (day_of_year - 172) / 365.0f);
    }
    
    data->air_temp_mean_c = seasonal_temp_base;
    data->air_temp_min_c = seasonal_temp_base - 8.0f;
    data->air_temp_max_c = seasonal_temp_base + 8.0f;
    data->temp_valid = true;
    
    // Humidity: moderate values
    data->rel_humidity_pct = 65.0f;  // Conservative moderate humidity
    data->humidity_valid = true;
    
    // Pressure: standard atmospheric pressure
    data->atmos_pressure_hpa = 1013.25f;
    data->pressure_valid = true;
    
    // Solar & wind omitted (no sensors; do not fabricate)
    
    // Rain: assume no recent rainfall (conservative for irrigation)
    data->rain_mm_24h = 0.0f;
    data->rain_valid = true;
    
    // Soil removed
    
    // Set data quality to indicate this is fallback data
    data->data_quality = 60;  // Lower quality for fallback data
    
    // Calculate derived values
    env_sensors_calculate_derived(data);
    
    LOG_INF("Generated fallback environmental data: T=%.1f°C, RH=%.1f%%",
            (double)data->air_temp_mean_c, (double)data->rel_humidity_pct);
    
    return WATERING_SUCCESS;
}

watering_error_t env_sensors_validate_data(environmental_data_t *data) {
    if (data == NULL) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    bool validation_passed = true;
    
    // Validate temperature readings
    if (data->temp_valid) {
        if (data->air_temp_mean_c < -50.0f || data->air_temp_mean_c > 70.0f) {
            LOG_WRN("Temperature out of range: %.1f°C", (double)data->air_temp_mean_c);
            data->temp_valid = false;
            validation_passed = false;
        }
        
        if (data->air_temp_min_c > data->air_temp_max_c) {
        LOG_WRN("Invalid temperature range: min=%.1f°C > max=%.1f°C", 
            (double)data->air_temp_min_c, (double)data->air_temp_max_c);
            data->temp_valid = false;
            validation_passed = false;
        }
    }
    
    // Validate humidity
    if (data->humidity_valid) {
        if (data->rel_humidity_pct < 0.0f || data->rel_humidity_pct > 100.0f) {
            LOG_WRN("Humidity out of range: %.1f%%", (double)data->rel_humidity_pct);
            data->humidity_valid = false;
            validation_passed = false;
        }
    }
    
    // Validate pressure
    if (data->pressure_valid) {
        if (data->atmos_pressure_hpa < 800.0f || data->atmos_pressure_hpa > 1200.0f) {
            LOG_WRN("Pressure out of range: %.1fhPa", (double)data->atmos_pressure_hpa);
            data->pressure_valid = false;
            validation_passed = false;
        }
    }
    
    /* solar removed */
    
    // Validate rainfall
    if (data->rain_valid) {
        if (data->rain_mm_24h < 0.0f || data->rain_mm_24h > 500.0f) {
            LOG_WRN("Rainfall out of range: %.1fmm", (double)data->rain_mm_24h);
            data->rain_valid = false;
            validation_passed = false;
        }
    }
    
    /* soil removed */
    
    // Recalculate data quality after validation
    uint8_t valid_sensors = 0;
    uint8_t total_sensors = 0;
    
    if (data->temp_valid) valid_sensors++;
    total_sensors++;
    
    if (data->humidity_valid) valid_sensors++;
    total_sensors++;
    
    if (data->pressure_valid) valid_sensors++;
    total_sensors++;
    
    if (data->rain_valid) valid_sensors++;
    total_sensors++;
    /* solar & wind removed */
    
    // Soil sensors removed; exclude from quality calculation
    
    data->data_quality = (valid_sensors * 100) / total_sensors;
    
    return validation_passed ? WATERING_SUCCESS : WATERING_ERROR_CONFIG;
}

watering_error_t env_sensors_reset_errors(void) {
    if (!system_initialized) {
        return WATERING_ERROR_NOT_INITIALIZED;
    }
    
    LOG_INF("Resetting environmental sensor error counters");
    
    sensor_status.temp_error_count = 0;
    sensor_status.humidity_error_count = 0;
    sensor_status.rain_error_count = 0;
    /* soil error counter removed */
    
    // Reset overall health to 100%
    sensor_status.overall_health = 100;
    
    return WATERING_SUCCESS;
}

watering_error_t env_sensors_set_low_power(bool enable) {
    if (!system_initialized) {
        return WATERING_ERROR_NOT_INITIALIZED;
    }
    
    LOG_INF("Setting environmental sensors to %s power mode", enable ? "low" : "normal");
    
    if (enable) {
        // In low power mode, reduce measurement frequency
        sensor_config.temp_interval_min = 60;      // Reduce to hourly
        sensor_config.humidity_interval_min = 60;  // Reduce to hourly
    sensor_config.rain_interval_min = 120;     // Reduce to 2 hours
    /* soil & wind removed */
        
        LOG_INF("Environmental sensors configured for low power operation");
    } else {
        // Restore normal operation intervals
        sensor_config.temp_interval_min = 15;      // Every 15 minutes
        sensor_config.humidity_interval_min = 15;  // Every 15 minutes
    sensor_config.rain_interval_min = 60;      // Every hour
    /* soil & wind removed */
        
        LOG_INF("Environmental sensors restored to normal operation");
    }
    
    return WATERING_SUCCESS;
}

/* Soil sensor implementation removed (feature eliminated). */
