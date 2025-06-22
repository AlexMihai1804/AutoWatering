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
    
    // Set default channel names
    for (int i = 0; i < WATERING_CHANNELS_COUNT; i++) {
        snprintf(watering_channels[i].name, sizeof(watering_channels[i].name), 
                "Channel %d", i + 1);
                
        // Default schedule: disabled
        watering_channels[i].watering_event.auto_enabled = false;
        
        // Default to daily schedule at noon
        watering_channels[i].watering_event.schedule_type = SCHEDULE_DAILY;
        watering_channels[i].watering_event.schedule.daily.days_of_week = 0x7F; // All days
        watering_channels[i].watering_event.start_time.hour = 12;
        watering_channels[i].watering_event.start_time.minute = 0;
        
        // Default to 5 minute duration watering
        watering_channels[i].watering_event.watering_mode = WATERING_BY_DURATION;
        watering_channels[i].watering_event.watering.by_duration.duration_minutes = 5;
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
    int ret = 0;
    
    // Don't try to save if we're in default-only mode
    if (using_default_settings) {
        printk("Using default configuration only - save not performed\n");
        return WATERING_SUCCESS;
    }
    
    k_mutex_lock(&config_mutex, K_FOREVER);
    
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
    }
    
    // Save days_since_start to persistent storage
    ret = nvs_save_days_since_start(days_since_start);
    if (ret < 0) {
        LOG_ERROR("Error saving days_since_start", ret);
        // Non-fatal error, continue
    }
    
    k_mutex_unlock(&config_mutex);
    printk("Configurations successfully saved (version %d)\n", WATERING_CONFIG_VERSION);
    return WATERING_SUCCESS;
}

/**
 * @brief Callback for loading header from settings
 */
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
