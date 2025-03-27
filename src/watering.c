#include "watering.h"
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include "flow_sensor.h"
#include "watering_internal.h"

/**
 * @file watering.c
 * @brief Implementation of the core watering control system
 * 
 * This file implements the main interfaces for controlling watering valves
 * and managing the watering channels.
 */

/** GPIO device specifications for all valves, retrieved from devicetree */
static const struct gpio_dt_spec valve1 = GPIO_DT_SPEC_GET(DT_PATH(valves, valve1), gpios);
static const struct gpio_dt_spec valve2 = GPIO_DT_SPEC_GET(DT_PATH(valves, valve2), gpios);
static const struct gpio_dt_spec valve3 = GPIO_DT_SPEC_GET(DT_PATH(valves, valve3), gpios);
static const struct gpio_dt_spec valve4 = GPIO_DT_SPEC_GET(DT_PATH(valves, valve4), gpios);
static const struct gpio_dt_spec valve5 = GPIO_DT_SPEC_GET(DT_PATH(valves, valve5), gpios);
static const struct gpio_dt_spec valve6 = GPIO_DT_SPEC_GET(DT_PATH(valves, valve6), gpios);
static const struct gpio_dt_spec valve7 = GPIO_DT_SPEC_GET(DT_PATH(valves, valve7), gpios);
static const struct gpio_dt_spec valve8 = GPIO_DT_SPEC_GET(DT_PATH(valves, valve8), gpios);

/** Global array holding all watering channel configurations */
watering_channel_t watering_channels[WATERING_CHANNELS_COUNT];

/** Current system status/state */
watering_status_t system_status = WATERING_STATUS_OK;

void watering_init(void) {
    // Assign GPIO specifications to each channel
    watering_channels[0].valve = valve1;
    watering_channels[1].valve = valve2;
    watering_channels[2].valve = valve3;
    watering_channels[3].valve = valve4;
    watering_channels[4].valve = valve5;
    watering_channels[5].valve = valve6;
    watering_channels[6].valve = valve7;
    watering_channels[7].valve = valve8;
    
    // Initialize each channel's GPIO pins and default name
    for (int i = 0; i < WATERING_CHANNELS_COUNT; i++) {
        snprintf(watering_channels[i].name, sizeof(watering_channels[i].name), "Channel %d", i + 1);
        if (!device_is_ready(watering_channels[i].valve.port)) {
            printk("GPIO device for valve %d not ready\n", i + 1);
            continue;
        }
        gpio_pin_configure_dt(&watering_channels[i].valve, GPIO_OUTPUT_INACTIVE);
        watering_channel_off(i);
    }
    int num_channels = WATERING_CHANNELS_COUNT;
    printk("Watering module initialized with %d channels.\n", num_channels);
    
    // Initialize subsystems
    tasks_init();
    config_init();
    flow_monitor_init();
}

/**
 * @brief Activate a specific watering channel's valve
 * 
 * @param channel_id Channel to activate (0-based index)
 * @return 0 on success, negative error code on failure
 */
int watering_channel_on(uint8_t channel_id) {
    // Validate channel ID
    if (channel_id >= WATERING_CHANNELS_COUNT) {
        return -1;  // Invalid channel ID
    }
    
    watering_channel_t *channel = &watering_channels[channel_id];
    if (!device_is_ready(channel->valve.port)) {
        return -2;  // GPIO device not ready
    }
    
    printk("Activating channel %d (%s)\n", channel_id + 1, channel->name);
    return gpio_pin_set_dt(&channel->valve, 1);
}

/**
 * @brief Deactivate a specific watering channel's valve
 * 
 * @param channel_id Channel to deactivate (0-based index)
 * @return 0 on success, negative error code on failure
 */
int watering_channel_off(uint8_t channel_id) {
    // Validate channel ID
    if (channel_id >= WATERING_CHANNELS_COUNT) {
        return -1;  // Invalid channel ID
    }
    
    watering_channel_t *channel = &watering_channels[channel_id];
    if (!device_is_ready(channel->valve.port)) {
        return -2;  // GPIO device not ready
    }
    
    printk("Deactivating channel %d\n", channel_id + 1);
    return gpio_pin_set_dt(&channel->valve, 0);
}

/**
 * @brief Get reference to a specific watering channel
 * 
 * @param channel_id Channel ID to retrieve (0-based index)
 * @return Pointer to the channel structure or NULL if invalid
 */
watering_channel_t *watering_get_channel(uint8_t channel_id) {
    if (channel_id >= WATERING_CHANNELS_COUNT) {
        return NULL;  // Invalid channel ID
    }
    return &watering_channels[channel_id];
}

/**
 * @brief Get the current watering system status
 * 
 * @return Current system status code
 */
watering_status_t watering_get_status(void) {
    return system_status;
}
