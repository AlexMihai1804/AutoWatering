#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/printk.h>
#include <stdio.h>  // Add this for snprintf
#include "watering.h"
#include "watering_internal.h"
#include "nvs_config.h"  // Changed to use direct NVS
#include "flow_sensor.h" // For in-memory calibration updates on load

/**
 * @file watering_config.c
 * @brief Implementation of configuration storage and retrieval
 * 
 * This file manages persistent storage of system configuration including
 * channel settings and calibration values using Zephyr's settings subsystem.
 */

/** Last save timestamp for logging throttling */
static uint32_t last_save_timestamp = 0;

/** Mutex for protecting configuration operations */
K_MUTEX_DEFINE(config_mutex);

/* Flag to indicate if we're using default settings */
bool using_default_settings = false;

/**
 * @brief Initialize the configuration subsystem
 * 
 * Sets up NVS storage for persistent configuration
 * 
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t config_init(void) {
    int rc;
    
    printk("Initializing configuration storage...\n");
    
    /* Always load defaults first so any missing NVS entries don't leave
     * zero-initialized channel structs (which later get persisted and
     * incorrectly trip onboarding flags for all channels).
     */
    load_default_config();
    
    // Initialize NVS for configuration storage
    rc = nvs_config_init();
    if (rc != 0) {
        printk("Failed to initialize NVS: %d\n", rc);
        printk("Using default configuration values\n");
        return WATERING_SUCCESS;  // Continue with default settings
    }
    
    // Load all configuration values
    rc = watering_load_config();
    if (rc != WATERING_SUCCESS) {
        printk("Failed to load configuration: %d\n", rc);
        printk("Keeping default configuration values\n");
    }
    
    return WATERING_SUCCESS;
}

/**
 * @brief Load default configuration values
 * 
 * Used when settings subsystem is unavailable
 * 
 * @return WATERING_SUCCESS on success
 */
watering_error_t load_default_config(void) {
    printk("Loading default configuration values\n");
    
    // Set flow sensor calibration to default
    set_flow_calibration_in_memory(DEFAULT_PULSES_PER_LITER);
    
    // Set default channel names and configuration
    for (int i = 0; i < WATERING_CHANNELS_COUNT; i++) {
        snprintf(watering_channels[i].name, sizeof(watering_channels[i].name), 
                "Channel %d", i + 1);
                
        // Default schedule: disabled but with reasonable example values
        watering_channels[i].watering_event.auto_enabled = false;

        /* Example DAILY schedule – Monday to Friday (0x3E) at 07:00 for 5 minutes (but disabled) */
        watering_channels[i].watering_event.schedule_type = SCHEDULE_DAILY;
        watering_channels[i].watering_event.schedule.daily.days_of_week = 0x3E; /* Mon-Fri (bits 1-5) */
        watering_channels[i].watering_event.start_time.hour   = 7;              /* 07:00 */
        watering_channels[i].watering_event.start_time.minute = 0;

        /* 5 minutes duration (but auto_enabled = false so schedule is inactive) */
        watering_channels[i].watering_event.watering_mode = WATERING_BY_DURATION;
        watering_channels[i].watering_event.watering.by_duration.duration_minutes = 5;
        
        // Initialize new plant and growing environment fields with defaults
        watering_channels[i].plant_type = PLANT_TYPE_VEGETABLES;  // Default to vegetables
        
        // Initialize plant_info structure
        watering_channels[i].plant_info.main_type = PLANT_TYPE_VEGETABLES;
        watering_channels[i].plant_info.specific.vegetable = VEGETABLE_TOMATOES; // Default to tomatoes
        
        watering_channels[i].soil_type = SOIL_TYPE_LOAMY;         // Default to loamy soil
        watering_channels[i].irrigation_method = IRRIGATION_DRIP; // Default to drip irrigation
        watering_channels[i].sun_percentage = 75;                 // Default to 75% sun exposure (legacy)
        watering_channels[i].sun_exposure_pct = 75;               // Default to 75% sun exposure (new field)
        
        // Default to area-based coverage with 1 square meter
        watering_channels[i].use_area_based = true;
        watering_channels[i].coverage.area_m2 = 1.0f;

        /* Enhanced defaults aligned with DEFAULT_ENHANCED_CHANNEL_CONFIG */
        watering_channels[i].auto_mode = WATERING_BY_DURATION; /* disabled/manual */
        watering_channels[i].max_volume_limit_l = 10.0f;
        watering_channels[i].enable_cycle_soak = false;
        watering_channels[i].planting_date_unix = 0;
        watering_channels[i].days_after_planting = 0;
        watering_channels[i].latitude_deg = 0.0f;
        watering_channels[i].longitude_deg = 0.0f;
        watering_channels[i].last_calculation_time = 0;
        watering_channels[i].last_auto_check_julian_day = 0;
        watering_channels[i].auto_check_ran_today = false;
        
        // Initialize custom plant configuration
        memset(&watering_channels[i].custom_plant, 0, sizeof(watering_channels[i].custom_plant));
        watering_channels[i].custom_plant.water_need_factor = 1.0f;
        watering_channels[i].custom_plant.irrigation_freq = 3;
        watering_channels[i].custom_plant.prefer_area_based = true;
        
        // Initialize database index fields to sentinel values (not configured)
        // These sentinel values prevent onboarding flags from being set during system config save
        watering_channels[i].plant_db_index = UINT16_MAX;
        watering_channels[i].soil_db_index = UINT8_MAX;
        watering_channels[i].irrigation_method_index = UINT8_MAX;

        /* Hydraulic monitoring defaults */
        watering_channels[i].hydraulic.nominal_flow_ml_min = 0;
        watering_channels[i].hydraulic.ramp_up_time_sec = 0;
        watering_channels[i].hydraulic.profile_type = PROFILE_AUTO;
        watering_channels[i].hydraulic.tolerance_high_percent = 30;
        watering_channels[i].hydraulic.tolerance_low_percent = 40;
        watering_channels[i].hydraulic.is_calibrated = false;
        watering_channels[i].hydraulic.monitoring_enabled = true;
        watering_channels[i].hydraulic.learning_runs = 0;
        watering_channels[i].hydraulic.stable_runs = 0;
        watering_channels[i].hydraulic.estimated = false;

        watering_channels[i].hydraulic_lock.level = HYDRAULIC_LOCK_NONE;
        watering_channels[i].hydraulic_lock.reason = HYDRAULIC_LOCK_REASON_NONE;
        watering_channels[i].hydraulic_lock.locked_at_epoch = 0;
        watering_channels[i].hydraulic_lock.retry_after_epoch = 0;

        watering_channels[i].hydraulic_anomaly.no_flow_runs = 0;
        watering_channels[i].hydraulic_anomaly.high_flow_runs = 0;
        watering_channels[i].hydraulic_anomaly.unexpected_flow_runs = 0;
        watering_channels[i].hydraulic_anomaly.last_anomaly_epoch = 0;
    }
    
    // Default days counter
    days_since_start = 0;
    
    // Set flag to indicate we're using defaults
    using_default_settings = true;
    
    return WATERING_SUCCESS;
}

/**
 * @brief Save all system configuration to persistent storage
 * 
 * Saves flow sensor calibration and all channel configurations
 * 
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_save_config(void) {
    return watering_save_config_priority(false);
}

watering_error_t watering_save_config_priority(bool is_priority) {
    int ret = 0;
    
    // Allow saves even when starting with default settings
    // The check for using_default_settings prevented initial saves from working
    // Once we save successfully, we'll no longer be using defaults
    if (using_default_settings) {
        printk("🔧 SAVE: First time saving configuration - transitioning from defaults\n");
    } else {
        printk("🔧 SAVE: Saving configuration (not using defaults)\n");
    }
    
    // REMOVED THROTTLING - Allow rapid access to history data
    // User complained about slow response when changing channels
    
    // Use a timeout for the mutex to prevent system freeze
    if (k_mutex_lock(&config_mutex, K_MSEC(500)) != 0) {
        printk("Config save failed: mutex timeout\n");
        return WATERING_ERROR_TIMEOUT;
    }
    
    // Update save timestamp
    last_save_timestamp = k_uptime_get_32();
    
    // Save flow sensor calibration
    uint32_t calibration;
    watering_get_flow_calibration(&calibration);
    ret = nvs_save_flow_calibration(calibration);
    if (ret < 0) {
        LOG_ERROR("Error saving calibration", ret);
        k_mutex_unlock(&config_mutex);
        return WATERING_ERROR_STORAGE;
    }
    
    // Save each channel's configuration (COMPLETE CHANNEL with enhanced parameters)
    for (int i = 0; i < WATERING_CHANNELS_COUNT; i++) {
        ret = nvs_save_complete_channel_config(i, &watering_channels[i]);
        if (ret < 0) {
            LOG_ERROR("Error saving enhanced channel configuration", ret);
            k_mutex_unlock(&config_mutex);
            return WATERING_ERROR_STORAGE;
        }
        
        printk("🔧 SUCCESS: Enhanced channel %d configuration saved\n", i);
    }
    
    // Save days_since_start to persistent storage
    ret = nvs_save_days_since_start(days_since_start);
    if (ret < 0) {
        LOG_ERROR("Error saving days_since_start", ret);
        // Non-fatal error, continue
    }
    
    k_mutex_unlock(&config_mutex);
    /* --------- NEW: avoid duplicate log spam ------------------------ */
    static uint32_t last_save_log_time = 0;          /* ms since boot */
    uint32_t now_log = k_uptime_get_32();
    if (now_log - last_save_log_time > 1000) {           /* 1 s debounce */
        printk("Configurations successfully saved\n");
        last_save_log_time = now_log;
    }
    
    // Mark that we're no longer using default settings after successful save
    printk("🔧 SAVE: Marking using_default_settings = false (was %s)\n", 
           using_default_settings ? "true" : "false");
    using_default_settings = false;

    return WATERING_SUCCESS;
}

watering_error_t watering_save_channel_config_priority(uint8_t channel_id, bool is_priority) {
    ARG_UNUSED(is_priority);

    if (channel_id >= WATERING_CHANNELS_COUNT) {
        return WATERING_ERROR_INVALID_PARAM;
    }

    if (using_default_settings) {
        printk("🔧 SAVE: First time saving configuration - transitioning from defaults\n");
    } else {
        printk("🔧 SAVE: Saving configuration (single channel)\n");
    }

    if (k_mutex_lock(&config_mutex, K_MSEC(500)) != 0) {
        printk("Config save failed: mutex timeout\n");
        return WATERING_ERROR_TIMEOUT;
    }

    last_save_timestamp = k_uptime_get_32();

    int ret = nvs_save_complete_channel_config(channel_id, &watering_channels[channel_id]);
    if (ret < 0) {
        LOG_ERROR("Error saving enhanced channel configuration", ret);
        k_mutex_unlock(&config_mutex);
        return WATERING_ERROR_STORAGE;
    }

    printk("🔧 SUCCESS: Enhanced channel %u configuration saved\n", channel_id);

    k_mutex_unlock(&config_mutex);

    printk("🔧 SAVE: Marking using_default_settings = false (was %s)\n",
           using_default_settings ? "true" : "false");
    using_default_settings = false;

    return WATERING_SUCCESS;
}



/**
 * @brief Callback for loading calibration value from settings
 */
__attribute__((unused))
static int calibration_load_cb(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg, void *param) {
    uint32_t *calibration = (uint32_t *) param;
    int rc;
    
    // Validate data length
    if (len != sizeof(*calibration)) {
        return -EINVAL;
    }
    
    // Read data from storage
    rc = read_cb(cb_arg, calibration, len);
    if (rc >= 0 && *calibration > 0) {
        printk("Calibration loaded: %d pulses per liter\n", *calibration);
        watering_set_flow_calibration(*calibration);
    }
    
    return rc;
}

/**
 * @brief Callback for loading channel configuration from settings
 */
__attribute__((unused))
static int channel_config_load_cb(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg, void *param) {
    watering_event_t *event = (watering_event_t *) param;
    int rc;
    
    // Validate data length
    if (len != sizeof(*event)) {
        return -EINVAL;
    }
    
    // Read data from storage
    rc = read_cb(cb_arg, event, len);
    if (rc >= 0) {
        // Validate loaded configuration
        if (watering_validate_event_config(event) != WATERING_SUCCESS) {
            printk("Warning: Invalid configuration loaded, using defaults\n");
            return -EINVAL;
        }
    }
    
    return rc;
}

/**
 * @brief Callback for loading channel name from settings
 */
__attribute__((unused))
static int channel_name_load_cb(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg, void *param) {
    char *channel_name = (char *) param;
    int rc;
    
    // Ensure we don't overflow the buffer
    size_t max_len = 64; // Maximum name length
    if (len > max_len) {
        len = max_len;
    }
    
    // Read data from storage
    rc = read_cb(cb_arg, channel_name, len);
    if (rc >= 0) {
        // Ensure null-termination
        channel_name[len < max_len ? len : max_len - 1] = '\0';
    }
    
    return rc;
}

/**
 * @brief Callback for loading days counter from settings
 */
__attribute__((unused))
static int days_since_load_cb(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg, void *param) {
    uint16_t *days = (uint16_t *) param;
    int rc;
    
    if (len != sizeof(*days)) {
        return -EINVAL;
    }
    
    rc = read_cb(cb_arg, days, len);
    if (rc >= 0) {
        printk("Loaded days since start: %d\n", *days);
    }
    
    return rc;
}

/**
 * @brief Load all system configuration from persistent storage
 * 
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_load_config(void) {
    int ret;
    uint32_t saved_calibration;
    int loaded_configs = 0;
    
    k_mutex_lock(&config_mutex, K_FOREVER);
    
    // Load flow sensor calibration
    ret = nvs_load_flow_calibration(&saved_calibration);
    if (ret >= 0) {
        int cal_ret = set_flow_calibration_in_memory(saved_calibration);
        if (cal_ret == 0) {
            loaded_configs++;
        } else {
            LOG_ERROR("Error applying calibration from storage", cal_ret);
        }
    } else if (ret != -ENOENT) {
        LOG_ERROR("Error reading calibration", ret);
    }
    
    // Load each channel's configuration (COMPLETE CHANNEL with enhanced parameters)
    for (int i = 0; i < WATERING_CHANNELS_COUNT; i++) {
        // Try to load the complete enhanced channel configuration  
        ret = nvs_load_complete_channel_config(i, &watering_channels[i]);
        if (ret >= 0) {
            printk("Channel %d configuration loaded\n", i + 1);
            loaded_configs++;
        } else if (ret == -ENOENT) {
            printk("Channel %d no configuration found, using defaults\n", i + 1);
        } else {
            LOG_ERROR("Error reading channel configuration", ret);
        }
        
        /* Enhanced configuration includes all channel parameters including
         * growing environment settings, water balance state, and automatic
         * irrigation parameters. */
    }
    
    // Load days_since_start counter
    uint16_t days;
    ret = nvs_load_days_since_start(&days);
    if (ret >= 0) {
        days_since_start = days;
        printk("Days since start loaded: %d\n", days_since_start);
    } else if (ret != -ENOENT) {
        LOG_ERROR("Error reading days_since_start", ret);
    }
    
    // Load automatic calculation system state
    automatic_calc_state_t calc_state;
    ret = nvs_load_automatic_calc_state(&calc_state);
    if (ret >= 0) {
        printk("Automatic calculation state loaded (enabled: %s)\n", 
               calc_state.system_enabled ? "yes" : "no");
        loaded_configs++;
    } else if (ret != -ENOENT) {
        LOG_ERROR("Error reading automatic calculation state", ret);
    }
    
    printk("%d configurations loaded from persistent memory\n", loaded_configs);
    
    k_mutex_unlock(&config_mutex);
    
    if (loaded_configs > 0) {
        using_default_settings = false;
        return WATERING_SUCCESS;
    } else {
        printk("No configurations found, loading defaults\n");
        return load_default_config();
    }
}
