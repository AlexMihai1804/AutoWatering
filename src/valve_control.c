#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/drivers/gpio.h>

#include "watering.h"
#include "watering_internal.h"
#include "flow_sensor.h"             /* reset_pulse_count prototype – NEW */
#include "bt_irrigation_service.h"   /* valve status notify – NEW */

/**
 * @file valve_control.c
 * @brief Implementation of irrigation valve control
 * 
 * This file manages the hardware interface for valve control
 * and their activation/deactivation operations.
 */

// Define timeouts for operations
#define GPIO_INIT_TIMEOUT_MS 500
#define MAX_VALVE_INIT_RETRIES 2

/** GPIO device specifications for all valves, retrieved from devicetree */
static const struct gpio_dt_spec valve1 = GPIO_DT_SPEC_GET(DT_PATH(valves, valve_1), gpios);
static const struct gpio_dt_spec valve2 = GPIO_DT_SPEC_GET(DT_PATH(valves, valve_2), gpios);
static const struct gpio_dt_spec valve3 = GPIO_DT_SPEC_GET(DT_PATH(valves, valve_3), gpios);
static const struct gpio_dt_spec valve4 = GPIO_DT_SPEC_GET(DT_PATH(valves, valve_4), gpios);
static const struct gpio_dt_spec valve5 = GPIO_DT_SPEC_GET(DT_PATH(valves, valve_5), gpios);
static const struct gpio_dt_spec valve6 = GPIO_DT_SPEC_GET(DT_PATH(valves, valve_6), gpios);
static const struct gpio_dt_spec valve7 = GPIO_DT_SPEC_GET(DT_PATH(valves, valve_7), gpios);
static const struct gpio_dt_spec valve8 = GPIO_DT_SPEC_GET(DT_PATH(valves, valve_8), gpios);

/** Maximum number of valves that can be active simultaneously */
#define MAX_SIMULTANEOUS_VALVES 1

/** Counter for active valves */
static int active_valves_count = 0;

// Flags
static int valves_ready = 0;   /* count of valves configured */

/**
 * @brief Check if another valve can be safely activated
 * 
 * @return true if activation is safe, false if max valves reached
 */
static bool is_valve_activation_safe(void) {
    return (active_valves_count < MAX_SIMULTANEOUS_VALVES);
}

/**
 * @brief Safe GPIO configuration with timeout
 * 
 * @param valve GPIO specification
 * @param flags GPIO configuration flags
 * @return 0 on success, error code on failure
 */
__attribute__((unused))
static int safe_gpio_configure(const struct gpio_dt_spec *valve, gpio_flags_t flags) {
    return gpio_pin_configure_dt(valve, flags);
}

/* ---------- helpers --------------------------------------------------- */
static inline bool gpio_spec_ready(const struct gpio_dt_spec *spec)
{
    return (spec && spec->port && device_is_ready(spec->port));
}

/* honours GPIO_ACTIVE_LOW, returns -ENODEV if spec invalid -------------- */
static inline int valve_set_state(const struct gpio_dt_spec *valve,
                                  bool active)
{
    if (!valve || !valve->port) {
        return -ENODEV;
    }
    const bool active_low = (valve->dt_flags & GPIO_ACTIVE_LOW);
    int level = active ? !active_low : active_low;
    return gpio_pin_set_dt(valve, level);
}

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
    valves_ready = 0;
    
    printk("Starting valve initialization...\n");
    
    // Use safer approach that initializes all valves, but with protections
    printk("Using progressive, sequential valve initialization\n");
    
    // Initialize all valves one by one
    const struct gpio_dt_spec *valves[] = {
        &valve1, &valve2, &valve3, &valve4, 
        &valve5, &valve6, &valve7, &valve8
    };
    
    for (int i = 0; i < WATERING_CHANNELS_COUNT; i++) {
        printk("Initializing valve %d... ", i + 1);
        
        // Skip valves with invalid ports
        if (!valves[i] || !valves[i]->port) {
            printk("SKIPPED (invalid GPIO definition)\n");
            continue;
        }
        
        // Try to initialize with simple checks
        if (device_is_ready(valves[i]->port)) {
            // Basic configuration without complex error checking
            int ret = gpio_pin_configure_dt(valves[i], GPIO_OUTPUT_INACTIVE);
            if (ret == 0) {
                /* ensure valve is logically OFF irrespective of polarity */
                valve_set_state(valves[i], false);                 // <- replaced
                watering_channels[i].is_active = false;
                valves_ready++;
                printk("SUCCESS\n");
            } else {
                printk("FAILED (error %d)\n", ret);
            }
        } else {
            printk("SKIPPED (GPIO device not ready)\n");
        }
        
        // Brief pause between valves to avoid overloading the system
        k_sleep(K_MSEC(50));
    }
    
    printk("%d out of %d valves successfully initialized\n", 
           valves_ready, WATERING_CHANNELS_COUNT);
    
    if (valves_ready == WATERING_CHANNELS_COUNT) {
        printk("All valves available\n");
    } else {
        printk("%d valves available, %d failed to initialise\n",
               valves_ready, WATERING_CHANNELS_COUNT - valves_ready);
    }
    
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
    
    // Check if we can safely activate another valve
    if (!is_valve_activation_safe()) {
        printk("Max valve activation limit reached, delaying activation\n");
        return WATERING_ERROR_BUSY;
    }
    
    watering_channel_t *channel = &watering_channels[channel_id];
    if (!gpio_spec_ready(&channel->valve)) {
        printk("GPIO port for channel %d not ready\n", channel_id + 1);
        return WATERING_ERROR_HARDWARE;
    }
    
    // Check system status
    if (system_status == WATERING_STATUS_FAULT) {
        return WATERING_ERROR_BUSY;
    }
    
    printk("Activating channel %d (%s) on GPIO pin %d\n", 
           channel_id + 1, channel->name, channel->valve.pin);
    
    // Add timeout protection for the GPIO operation
    uint32_t start = k_uptime_get_32();
    int ret;
    /* use polarity-aware helper instead of raw gpio */
    while ((ret = valve_set_state(&channel->valve, true)) != 0) {   // <- replaced
        if (k_uptime_get_32() - start > 200) {
            printk("GPIO activation timed out\n");
            return WATERING_ERROR_HARDWARE;
        }
        k_busy_wait(10000);  // 10ms
    }
    
    channel->is_active = true;
    active_valves_count++;
    // --- BLE notify ---------------------------------------------------
    bt_irrigation_valve_status_update(channel_id, true);
    
    // If we were in idle state, transition to watering state
    if (system_state == WATERING_STATE_IDLE) {
        transition_to_state(WATERING_STATE_WATERING);
    }
    
    return WATERING_SUCCESS;
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
    if (!gpio_spec_ready(&channel->valve)) {
        printk("GPIO port for channel %d not ready for deactivation\n",
               channel_id + 1);
        return WATERING_ERROR_HARDWARE;
    }
    
    printk("Deactivating channel %d on GPIO pin %d\n", 
           channel_id + 1, channel->valve.pin);
    
    // Add timeout protection
    uint32_t start = k_uptime_get_32();
    int ret;
    while ((ret = valve_set_state(&channel->valve, false)) != 0) {  // <- replaced
        if (k_uptime_get_32() - start > 200) {
            printk("GPIO deactivation timed out\n");
            break; // Continue anyway to update internal state
        }
        k_busy_wait(10000);  // 10ms
    }
    
    if (channel->is_active) {
        channel->is_active = false;
        active_valves_count--;
        /* BLE notify on close */
        bt_irrigation_valve_status_update(channel_id, false);
    }
    
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

    /* NEW: when ALL valves are now closed, clear flow counter so
     * leakage detection starts from zero.
     */
    if (!any_active) {
        reset_pulse_count();
    }

    return WATERING_SUCCESS;
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
