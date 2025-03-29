#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/drivers/gpio.h>

#include "watering.h"
#include "watering_internal.h"

/**
 * @file valve_control.c
 * @brief Implementation of irrigation valve control
 * 
 * This file manages the hardware interface for valve control
 * and their activation/deactivation operations.
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

/**
 * @brief Initialize all valve hardware
 * 
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t valve_init(void) {
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
        // Print GPIO port information to debug multi-port usage
        printk("Channel %d: GPIO port %p, pin %d\n", 
               i + 1, 
               watering_channels[i].valve.port,
               watering_channels[i].valve.pin);
        
        if (!device_is_ready(watering_channels[i].valve.port)) {
            LOG_ERROR("GPIO device for valve not ready", i + 1);
            return WATERING_ERROR_HARDWARE;
        }
        
        int ret = gpio_pin_configure_dt(&watering_channels[i].valve, GPIO_OUTPUT_INACTIVE);
        if (ret != 0) {
            LOG_ERROR("Failed to configure valve GPIO", ret);
            return WATERING_ERROR_HARDWARE;
        }
        
        watering_channel_off(i);  // Ensure all valves are off initially
    }
    
    printk("Valve hardware initialized with %d channels.\n", WATERING_CHANNELS_COUNT);
    return WATERING_SUCCESS;
}

/**
 * @brief Activate a specific watering channel's valve
 */
watering_error_t watering_channel_on(uint8_t channel_id) {
    // Validate channel ID
    if (channel_id >= WATERING_CHANNELS_COUNT) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    // Verify system is initialized
    if (!system_initialized) {
        return WATERING_ERROR_NOT_INITIALIZED;
    }
    
    watering_channel_t *channel = &watering_channels[channel_id];
    if (!device_is_ready(channel->valve.port)) {
        printk("ERROR: GPIO port %p for channel %d not ready\n", 
               channel->valve.port, channel_id + 1);
        return WATERING_ERROR_HARDWARE;
    }
    
    // Check system status
    if (system_status == WATERING_STATUS_FAULT) {
        return WATERING_ERROR_BUSY;
    }
    
    printk("Activating channel %d (%s) on GPIO port %p pin %d\n", 
           channel_id + 1, channel->name, channel->valve.port, channel->valve.pin);
    int ret = gpio_pin_set_dt(&channel->valve, 1);
    
    if (ret == 0) {
        channel->is_active = true;
        
        // If we were in idle state, transition to watering state
        if (system_state == WATERING_STATE_IDLE) {
            transition_to_state(WATERING_STATE_WATERING);
        }
        
        return WATERING_SUCCESS;
    } else {
        LOG_ERROR("Failed to activate channel GPIO", ret);
        printk("Failed to activate GPIO: port %p, pin %d, error %d\n", 
               channel->valve.port, channel->valve.pin, ret);
        return WATERING_ERROR_HARDWARE;
    }
}

/**
 * @brief Deactivate a specific watering channel's valve
 */
watering_error_t watering_channel_off(uint8_t channel_id) {
    // Validate channel ID
    if (channel_id >= WATERING_CHANNELS_COUNT) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    watering_channel_t *channel = &watering_channels[channel_id];
    if (!device_is_ready(channel->valve.port)) {
        printk("ERROR: GPIO port %p for channel %d not ready\n", 
               channel->valve.port, channel_id + 1);
        return WATERING_ERROR_HARDWARE;
    }
    
    printk("Deactivating channel %d on GPIO port %p pin %d\n", 
           channel_id + 1, channel->valve.port, channel->valve.pin);
    int ret = gpio_pin_set_dt(&channel->valve, 0);
    
    if (ret == 0) {
        channel->is_active = false;
        
        // Check if any channels are still active
        bool any_active = false;
        for (int i = 0; i < WATERING_CHANNELS_COUNT; i++) {
            if (watering_channels[i].is_active) {
                any_active = true;
                break;
            }
        }
        
        // If no channels are active and we were in watering state, transition to idle
        if (!any_active && system_state == WATERING_STATE_WATERING) {
            transition_to_state(WATERING_STATE_IDLE);
        }
        
        return WATERING_SUCCESS;
    } else {
        LOG_ERROR("Failed to deactivate channel GPIO", ret);
        return WATERING_ERROR_HARDWARE;
    }
}

/**
 * @brief Close all valves
 * 
 * Safety function to ensure all valves are closed
 * 
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t valve_close_all(void) {
    watering_error_t result = WATERING_SUCCESS;
    
    for (int i = 0; i < WATERING_CHANNELS_COUNT; i++) {
        watering_error_t err = watering_channel_off(i);
        if (err != WATERING_SUCCESS) {
            result = err;  // Return last error but try all valves
        }
    }
    
    return result;
}
