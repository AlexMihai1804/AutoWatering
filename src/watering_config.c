#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/printk.h>
#include <stdio.h>  // Add this for snprintf
#include "watering.h"
#include "watering_internal.h"

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

/**
 * @brief Initialize the configuration subsystem
 * 
 * Sets up Zephyr's settings subsystem for persistent storage
 * 
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t config_init(void) {
    int rc = settings_subsys_init();
    if (rc) {
        LOG_ERROR("Error initializing settings subsystem", rc);
        return WATERING_ERROR_STORAGE;
    } 
    
    printk("Settings subsystem initialized\n");
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
    
    k_mutex_lock(&config_mutex, K_FOREVER);
    
    // Update configuration header
    config_header.version = WATERING_CONFIG_VERSION;
    config_header.timestamp = k_uptime_get_32();
    
    // Save header
    ret = settings_save_one("watering/header", &config_header, sizeof(config_header));
    if (ret) {
        LOG_ERROR("Error saving configuration header", ret);
        k_mutex_unlock(&config_mutex);
        return WATERING_ERROR_STORAGE;
    }
    
    // Save flow sensor calibration
    uint32_t calibration;
    watering_get_flow_calibration(&calibration);
    ret = settings_save_one("watering/calibration", &calibration, sizeof(calibration));
    if (ret) {
        LOG_ERROR("Error saving calibration", ret);
        k_mutex_unlock(&config_mutex);
        return WATERING_ERROR_STORAGE;
    }
    
    // Save each channel's configuration
    for (int i = 0; i < WATERING_CHANNELS_COUNT; i++) {
        char path[32];
        snprintf(path, sizeof(path), "watering/channel/%d", i);
        ret = settings_save_one(path, &watering_channels[i].watering_event, sizeof(watering_event_t));
        if (ret) {
            LOG_ERROR("Error saving channel configuration", ret);
            k_mutex_unlock(&config_mutex);
            return WATERING_ERROR_STORAGE;
        }
        
        // Save channel name separately
        snprintf(path, sizeof(path), "watering/name/%d", i);
        ret = settings_save_one(path, watering_channels[i].name, sizeof(watering_channels[i].name));
        if (ret) {
            LOG_ERROR("Error saving channel name", ret);
            // Non-fatal error, continue
        }
    }
    
    // Save days_since_start to persistent storage
    ret = settings_save_one("watering/days_since", &days_since_start, sizeof(days_since_start));
    if (ret) {
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
    
    // Load the configuration header first
    ret = settings_load_subtree_direct("watering/header", header_load_cb, &loaded_header);
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
    ret = settings_load_subtree_direct("watering/calibration", calibration_load_cb, &saved_calibration);
    if (ret >= 0) {
        watering_set_flow_calibration(saved_calibration);
        loaded_configs++;
    } else if (ret < 0 && ret != -ENOENT) {
        LOG_ERROR("Error reading calibration", ret);
    }
    
    // Load each channel's configuration
    for (int i = 0; i < WATERING_CHANNELS_COUNT; i++) {
        char path[32];
        
        // Load channel configuration
        snprintf(path, sizeof(path), "watering/channel/%d", i);
        ret = settings_load_subtree_direct(path, channel_config_load_cb, &watering_channels[i].watering_event);
        if (ret >= 0) {
            printk("Channel %d configuration loaded\n", i + 1);
            loaded_configs++;
            
            // Load channel name
            snprintf(path, sizeof(path), "watering/name/%d", i);
            ret = settings_load_subtree_direct(path, channel_name_load_cb, watering_channels[i].name);
            if (ret >= 0) {
                printk("Channel %d name loaded: %s\n", i + 1, watering_channels[i].name);
            } else {
                // If name isn't loaded, ensure we have a valid one
                snprintf(watering_channels[i].name, sizeof(watering_channels[i].name), "Channel %d", i + 1);
            }
        } else if (ret < 0 && ret != -ENOENT) {
            LOG_ERROR("Error reading channel configuration", ret);
        }
    }
    
    // Load days_since_start counter
    ret = settings_load_subtree_direct("watering/days_since", days_since_load_cb, &days_since_start);
    if (ret >= 0) {
        printk("Days since start loaded: %d\n", days_since_start);
    } else if (ret < 0 && ret != -ENOENT) {
        LOG_ERROR("Error reading days_since_start", ret);
    }
    
    printk("%d configurations loaded from persistent memory (version %d)\n", 
          loaded_configs, config_header.version);
    
    k_mutex_unlock(&config_mutex);
    return loaded_configs > 0 ? WATERING_SUCCESS : WATERING_ERROR_NOT_FOUND;
}
