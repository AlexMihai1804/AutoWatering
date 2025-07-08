#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/printk.h>
#include <stdio.h>  // Add this for snprintf
#include "watering.h"
#include "watering_internal.h"
#include "nvs_config.h"  // Changed to use direct NVS

/**
 * @file watering_config.c
 * @brief Implementation of configuration storage and retrieval
 * 
 * This file manages persistent storage of system configuration including
 * channel settings and calibration values using Zephyr's settings subsystem.
 */

/**
 * @brief Configuration data header for versioning
 */
typedef struct {
    uint8_t version;      /**< Configuration version number */
    uint32_t timestamp;   /**< Last save timestamp */
} config_header_t;

/** Current configuration header */
static config_header_t config_header = {
    .version = WATERING_CONFIG_VERSION,
    .timestamp = 0
};

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
    
    // Always set default values first to ensure we have something to work with
    using_default_settings = true;
    
    // Initialize NVS for configuration storage
    rc = nvs_config_init();
    if (rc != 0) {
        printk("Failed to initialize NVS: %d\n", rc);
        printk("Using default configuration values\n");
        load_default_config();
        return WATERING_SUCCESS;  // Continue with default settings
    }
    
    // Load all configuration values
    rc = watering_load_config();
    if (rc != WATERING_SUCCESS) {
        printk("Failed to load configuration: %d\n", rc);
        printk("Using default configuration values\n");
        load_default_config();
    } else {
        // Mark that we're not using defaults anymore
        using_default_settings = false;
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
    watering_set_flow_calibration(DEFAULT_PULSES_PER_LITER);
    
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
        watering_channels[i].sun_percentage = 75;                 // Default to 75% sun exposure
        
        // Default to area-based coverage with 1 square meter
        watering_channels[i].coverage.use_area = true;
        watering_channels[i].coverage.area.area_m2 = 1.0f;
        
        // Initialize custom plant configuration
        memset(&watering_channels[i].custom_plant, 0, sizeof(watering_channels[i].custom_plant));
        watering_channels[i].custom_plant.water_need_factor = 1.0f;
        watering_channels[i].custom_plant.irrigation_freq = 3;
        watering_channels[i].custom_plant.prefer_area_based = true;
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
    
    // Update configuration header
    config_header.version = WATERING_CONFIG_VERSION;
    config_header.timestamp = k_uptime_get_32();
    
    // Save header using direct NVS
    ret = nvs_save_watering_config(&config_header, sizeof(config_header));
    if (ret < 0) {
        LOG_ERROR("Error saving configuration header", ret);
        k_mutex_unlock(&config_mutex);
        return WATERING_ERROR_STORAGE;
    }
    
    // Save flow sensor calibration
    uint32_t calibration;
    watering_get_flow_calibration(&calibration);
    ret = nvs_save_flow_calibration(calibration);
    if (ret < 0) {
        LOG_ERROR("Error saving calibration", ret);
        k_mutex_unlock(&config_mutex);
        return WATERING_ERROR_STORAGE;
    }
    
    // Save each channel's configuration
    for (int i = 0; i < WATERING_CHANNELS_COUNT; i++) {
        ret = nvs_save_channel_config(i, &watering_channels[i].watering_event, 
                                     sizeof(watering_event_t));
        if (ret < 0) {
            LOG_ERROR("Error saving channel configuration", ret);
            k_mutex_unlock(&config_mutex);
            return WATERING_ERROR_STORAGE;
        }

        ret = nvs_save_channel_name(i, watering_channels[i].name);   /* NEW */
        if (ret < 0) {
            printk("🔧 ERROR: Failed to save channel %d name \"%s\" (ret=%d)\n", 
                   i, watering_channels[i].name, ret);
            // non-fatal, continue
        } else {
            printk("🔧 SUCCESS: Channel %d name saved: \"%s\" (ret=%d)\n", 
                   i, watering_channels[i].name, ret);
        }
        
        // Save new plant and growing environment fields
        // Create a structure to save the extended channel data
        struct {
            plant_type_t plant_type;
            plant_info_t plant_info;
            soil_type_t soil_type;
            irrigation_method_t irrigation_method;
            channel_coverage_t coverage;
            uint8_t sun_percentage;
            custom_plant_config_t custom_plant;
        } channel_env_data;
        
        channel_env_data.plant_type = watering_channels[i].plant_type;
        channel_env_data.plant_info = watering_channels[i].plant_info;
        channel_env_data.soil_type = watering_channels[i].soil_type;
        channel_env_data.irrigation_method = watering_channels[i].irrigation_method;
        channel_env_data.coverage = watering_channels[i].coverage;
        channel_env_data.sun_percentage = watering_channels[i].sun_percentage;
        channel_env_data.custom_plant = watering_channels[i].custom_plant;
        
        // Save extended data using channel-specific ID
        ret = nvs_config_write(400 + i, &channel_env_data, sizeof(channel_env_data));
        if (ret < 0) {
            LOG_ERROR("Error saving channel environment data", ret);
            // non-fatal, continue
        }
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
        printk("Configurations successfully saved (version %d)\n",
               WATERING_CONFIG_VERSION);
        last_save_log_time = now_log;
    }
    
    // Mark that we're no longer using default settings after successful save
    printk("🔧 SAVE: Marking using_default_settings = false (was %s)\n", 
           using_default_settings ? "true" : "false");
    using_default_settings = false;

    return WATERING_SUCCESS;
}

/**
 * @brief Callback for loading header from settings
 */
__attribute__((unused))
static int header_load_cb(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg, void *param) {
    config_header_t *header = (config_header_t *) param;
    int rc;
    
    // Validate data length
    if (len != sizeof(*header)) {
        return -EINVAL;
    }
    
    // Read data from storage
    rc = read_cb(cb_arg, header, len);
    if (rc >= 0) {
        printk("Configuration header loaded: version %d, timestamp %u\n", 
              header->version, header->timestamp);
    }
    
    return rc;
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
    config_header_t loaded_header = {0};
    uint32_t saved_calibration;
    int loaded_configs = 0;
    
    k_mutex_lock(&config_mutex, K_FOREVER);
    
    // Try to load the configuration header
    ret = nvs_load_watering_config(&loaded_header, sizeof(loaded_header));
    if (ret < 0 && ret != -ENOENT) {
        printk("Error loading configuration header: %d\n", ret);
        printk("Using default configuration\n");
        k_mutex_unlock(&config_mutex);
        return load_default_config();
    }
    
    if (ret >= 0) {
        // Check version compatibility
        if (loaded_header.version > WATERING_CONFIG_VERSION) {
            printk("WARNING: Saved configuration version (%d) is newer than current version (%d)\n",
                   loaded_header.version, WATERING_CONFIG_VERSION);
            printk("Configuration might not be fully compatible\n");
        }
        config_header = loaded_header;
    }
    
    // Load flow sensor calibration
    ret = nvs_load_flow_calibration(&saved_calibration);
    if (ret >= 0) {
        watering_set_flow_calibration(saved_calibration);
        loaded_configs++;
    } else if (ret != -ENOENT) {
        LOG_ERROR("Error reading calibration", ret);
    }
    
    // Load each channel's configuration
    for (int i = 0; i < WATERING_CHANNELS_COUNT; i++) {
        // Load channel configuration
        ret = nvs_load_channel_config(i, &watering_channels[i].watering_event, 
                               sizeof(watering_channels[i].watering_event));
        if (ret >= 0) {
            printk("Channel %d configuration loaded\n", i + 1);
            loaded_configs++;
        } else if (ret != -ENOENT) {
            LOG_ERROR("Error reading channel configuration", ret);
        }

        ret = nvs_load_channel_name(i,
                    watering_channels[i].name,
                    sizeof(watering_channels[i].name));              /* NEW */
        if (ret >= 0) {
            printk("Channel %d name loaded: \"%s\" (len=%d)\n", 
                   i, watering_channels[i].name, ret);
        } else if (ret == -ENOENT) {
            printk("Channel %d name not found in NVS, keeping default: \"%s\"\n", 
                   i, watering_channels[i].name);
        } else {
            LOG_ERROR("Error reading channel name", ret);
        }
        
        // Load new plant and growing environment fields
        struct {
            plant_type_t plant_type;
            plant_info_t plant_info;
            soil_type_t soil_type;
            irrigation_method_t irrigation_method;
            channel_coverage_t coverage;
            uint8_t sun_percentage;
            custom_plant_config_t custom_plant;
        } channel_env_data;
        
        ret = nvs_config_read(400 + i, &channel_env_data, sizeof(channel_env_data));
        if (ret >= 0) {
            watering_channels[i].plant_type = channel_env_data.plant_type;
            watering_channels[i].plant_info = channel_env_data.plant_info;
            watering_channels[i].soil_type = channel_env_data.soil_type;
            watering_channels[i].irrigation_method = channel_env_data.irrigation_method;
            watering_channels[i].coverage = channel_env_data.coverage;
            watering_channels[i].sun_percentage = channel_env_data.sun_percentage;
            watering_channels[i].custom_plant = channel_env_data.custom_plant;
            printk("Channel %d environment data loaded (plant_type=%d, specific=%d)\n", 
                   i + 1, watering_channels[i].plant_type, 
                   watering_channels[i].plant_info.specific.vegetable);
        } else if (ret == -ENOENT) {
            /* Use default values already set in load_default_config */
        } else {
            LOG_ERROR("Error reading channel environment data", ret);
        }
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
    
    printk("%d configurations loaded from persistent memory (version %d)\n", 
          loaded_configs, config_header.version);
    
    k_mutex_unlock(&config_mutex);
    
    if (loaded_configs > 0) {
        using_default_settings = false;
        return WATERING_SUCCESS;
    } else {
        printk("No configurations found, loading defaults\n");
        return load_default_config();
    }
}
