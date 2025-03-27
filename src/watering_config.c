#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/printk.h>
#include "watering.h"
#include "watering_internal.h"

/**
 * @file watering_config.c
 * @brief Implementation of configuration storage and retrieval
 * 
 * This file manages persistent storage of system configuration including
 * channel settings and calibration values using Zephyr's settings subsystem.
 */

extern uint32_t watering_get_flow_calibration(void);
extern void watering_set_flow_calibration(uint32_t new_pulses_per_liter);

/**
 * @brief Initialize the configuration subsystem
 * 
 * Sets up Zephyr's settings subsystem for persistent storage
 */
void config_init(void) {
    int rc = settings_subsys_init();
    if (rc) {
        printk("Error initializing settings subsystem: %d\n", rc);
    } else {
        printk("Settings subsystem initialized\n");
    }
}

/**
 * @brief Save all system configuration to persistent storage
 * 
 * Saves flow sensor calibration and all channel configurations
 * 
 * @return 0 on success, negative error code on failure
 */
int watering_save_config(void) {
    int ret = 0;
    
    // Save flow sensor calibration
    uint32_t calibration = watering_get_flow_calibration();
    ret = settings_save_one("watering/calibration", &calibration, sizeof(calibration));
    if (ret) {
        printk("Error saving calibration: %d\n", ret);
        return ret;
    }
    
    // Save each channel's configuration
    for (int i = 0; i < WATERING_CHANNELS_COUNT; i++) {
        char path[32];
        snprintf(path, sizeof(path), "watering/channel/%d", i);
        ret = settings_save_one(path, &watering_channels[i].watering_event, sizeof(watering_event_t));
        if (ret) {
            printk("Error saving channel %d configuration: %d\n", i + 1, ret);
            return ret;
        }
    }
    
    printk("Configurations successfully saved\n");
    return 0;
}

/**
 * @brief Callback for loading calibration value from settings
 * 
 * @param name Settings key name
 * @param len Length of data
 * @param read_cb Callback to read the value
 * @param cb_arg Callback argument
 * @param param Pointer to calibration value
 * @return 0 on success, negative error code on failure
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
    if (rc >= 0) {
        printk("Calibration loaded: %d pulses per liter\n", *calibration);
    }
    
    return rc;
}

/**
 * @brief Callback for loading channel configuration from settings
 * 
 * @param name Settings key name
 * @param len Length of data
 * @param read_cb Callback to read the value
 * @param cb_arg Callback argument
 * @param param Pointer to watering event structure
 * @return 0 on success, negative error code on failure
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
    return rc;
}

/**
 * @brief Load all system configuration from persistent storage
 * 
 * @return 0 on success, -ENOENT if no configurations found
 */
int watering_load_config(void) {
    int ret;
    uint32_t saved_calibration;
    int loaded_configs = 0;
    
    // Load flow sensor calibration
    ret = settings_load_subtree_direct("watering/calibration", calibration_load_cb, &saved_calibration);
    if (ret >= 0) {
        watering_set_flow_calibration(saved_calibration);
        loaded_configs++;
    } else if (ret < 0 && ret != -ENOENT) {
        printk("Error reading calibration: %d\n", ret);
    }
    
    // Load each channel's configuration
    for (int i = 0; i < WATERING_CHANNELS_COUNT; i++) {
        char path[32];
        snprintf(path, sizeof(path), "watering/channel/%d", i);
        ret = settings_load_subtree_direct(path, channel_config_load_cb, &watering_channels[i].watering_event);
        if (ret >= 0) {
            printk("Channel %d configuration loaded\n", i + 1);
            loaded_configs++;
        } else if (ret < 0 && ret != -ENOENT) {
            printk("Error reading channel %d configuration: %d\n", i + 1, ret);
        }
    }
    
    printk("%d configurations loaded from persistent memory\n", loaded_configs);
    return loaded_configs > 0 ? 0 : -ENOENT;
}
