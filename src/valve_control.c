#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/drivers/gpio.h>
#include <stdlib.h>  /* for abs() function */

#include "watering.h"
#include "watering_internal.h"
#include "flow_sensor.h"             /* reset_pulse_count prototype – NEW */
#include "bt_irrigation_service.h"   /* valve status notify – NEW */

/**
 * @file valve_control.c
 * @brief Implementation of irrigation valve control with master valve support
 * 
 * This file manages the hardware interface for valve control,
 * including intelligent master valve timing and overlapping task logic.
 */

// Define timeouts for operations
#define GPIO_INIT_TIMEOUT_MS 500
#define MAX_VALVE_INIT_RETRIES 2

/** GPIO device specifications for all valves, retrieved from devicetree */
static const struct gpio_dt_spec master_valve = GPIO_DT_SPEC_GET(DT_PATH(valves, master_valve), gpios);
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

/** Master valve configuration and state */
static master_valve_config_t master_config = {
    .enabled = true,
    .pre_start_delay_sec = 3,     // Open master 3 seconds before zone valve
    .post_stop_delay_sec = 2,     // Keep master open 2 seconds after zone valve closes
    .overlap_grace_sec = 5,       // 5-second grace period between consecutive tasks
    .auto_management = true,      // Automatically manage master valve
    .is_active = false
};

/** Master valve timer for delayed operations */
static struct k_work_delayable master_valve_work;

/** State tracking for master valve scheduling */
static struct {
    uint32_t next_task_start_time;     // When next task is scheduled to start
    bool has_pending_task;             // Whether there's a task waiting
    uint32_t current_task_end_time;    // When current task should end
} master_valve_schedule = {0};

/* Forward declarations for master valve functions */
static watering_error_t master_valve_open(void);
static watering_error_t master_valve_close(void);

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
 * @brief Master valve work handler for delayed operations
 */
static void master_valve_work_handler(struct k_work *work)
{
    struct k_work_delayable *delayable_work = k_work_delayable_from_work(work);
    ARG_UNUSED(delayable_work);
    
    // Check if we should close master valve due to no more pending tasks
    uint32_t now = k_uptime_get_32();
    
    if (master_config.is_active && 
        (!master_valve_schedule.has_pending_task || 
         now > master_valve_schedule.next_task_start_time + (master_config.overlap_grace_sec * 1000))) {
        
        printk("Master valve: Closing due to no pending tasks within grace period\n");
        master_valve_close();
    }
}

/**
 * @brief Open master valve with BLE notification
 */
static watering_error_t master_valve_open(void)
{
    if (!master_config.enabled || master_config.is_active) {
        return WATERING_SUCCESS; // Already open or disabled
    }
    
    if (!gpio_spec_ready(&master_config.valve)) {
        printk("Master valve GPIO not ready\n");
        return WATERING_ERROR_HARDWARE;
    }
    
    /* Master valve uses inverted logic: false = open (relay energized) */
    int ret = valve_set_state(&master_config.valve, false);
    if (ret != 0) {
        printk("Failed to activate master valve: %d\n", ret);
        return WATERING_ERROR_HARDWARE;
    }
    
    master_config.is_active = true;
    printk("Master valve OPENED\n");
    
    // Send BLE notification (use channel_id = 0xFF for master valve)
    bt_irrigation_valve_status_update(0xFF, true);
    
    return WATERING_SUCCESS;
}

/**
 * @brief Close master valve with BLE notification
 */
static watering_error_t master_valve_close(void)
{
    if (!master_config.enabled || !master_config.is_active) {
        return WATERING_SUCCESS; // Already closed or disabled
    }
    
    if (!gpio_spec_ready(&master_config.valve)) {
        printk("Master valve GPIO not ready\n");
        return WATERING_ERROR_HARDWARE;
    }
    
    /* Master valve uses inverted logic: true = closed (relay de-energized) */
    int ret = valve_set_state(&master_config.valve, true);
    if (ret != 0) {
        printk("Failed to deactivate master valve: %d\n", ret);
        return WATERING_ERROR_HARDWARE;
    }
    
    master_config.is_active = false;
    printk("Master valve CLOSED\n");
    
    // Send BLE notification (use channel_id = 0xFF for master valve)
    bt_irrigation_valve_status_update(0xFF, false);
    
    return WATERING_SUCCESS;
}

/**
 * @brief Initialize all valve hardware including master valve
 * 
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t valve_init(void) {
    // Initialize master valve configuration
    master_config.valve = master_valve;
    k_work_init_delayable(&master_valve_work, master_valve_work_handler);
    
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
    
    // Initialize master valve first
    printk("Initializing master valve... ");
    if (gpio_spec_ready(&master_config.valve)) {
        int ret = gpio_pin_configure_dt(&master_config.valve, GPIO_OUTPUT_INACTIVE);
        if (ret == 0) {
            /* Master valve uses inverted logic: true = closed (relay de-energized) */
            valve_set_state(&master_config.valve, true);
            master_config.is_active = false;
            printk("SUCCESS\n");
        } else {
            printk("FAILED (error %d)\n", ret);
        }
    } else {
        printk("SKIPPED (GPIO device not ready)\n");
    }
    
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
 * @brief Activate a specific watering channel's valve with master valve logic
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
    
    // Master valve logic: Open master valve with pre-start delay
    if (master_config.enabled && master_config.auto_management) {
        if (master_config.pre_start_delay_sec > 0) {
            // Open master valve BEFORE zone valve
            watering_error_t master_err = master_valve_open();
            if (master_err != WATERING_SUCCESS) {
                printk("Warning: Failed to open master valve: %d\n", master_err);
            } else {
                // Wait for pre-start delay
                k_sleep(K_MSEC(master_config.pre_start_delay_sec * 1000));
            }
        }
    }
    
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
    
    // Master valve logic: Open master valve AFTER zone valve if delay is negative
    if (master_config.enabled && master_config.auto_management) {
        if (master_config.pre_start_delay_sec <= 0) {
            // Delay is 0 or negative - open master valve now or after delay
            if (master_config.pre_start_delay_sec < 0) {
                k_sleep(K_MSEC(abs(master_config.pre_start_delay_sec) * 1000));
            }
            watering_error_t master_err = master_valve_open();
            if (master_err != WATERING_SUCCESS) {
                printk("Warning: Failed to open master valve: %d\n", master_err);
            }
        }
    }
    
    printk("Channel %d activated - sending BLE notification\n", channel_id);
    // --- BLE notify ---------------------------------------------------
    bt_irrigation_valve_status_update(channel_id, true);
    
    // If we were in idle state, transition to watering state
    if (system_state == WATERING_STATE_IDLE) {
        transition_to_state(WATERING_STATE_WATERING);
    }
    
    return WATERING_SUCCESS;
}

/**
 * @brief Deactivate a specific watering channel's valve with master valve logic
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
    
    // Master valve logic: Close master valve BEFORE zone valve if delay is negative
    if (master_config.enabled && master_config.auto_management) {
        if (master_config.post_stop_delay_sec < 0) {
            // Close master valve BEFORE zone valve
            watering_error_t master_err = master_valve_close();
            if (master_err != WATERING_SUCCESS) {
                printk("Warning: Failed to close master valve: %d\n", master_err);
            } else {
                // Wait for delay
                k_sleep(K_MSEC(abs(master_config.post_stop_delay_sec) * 1000));
            }
        }
    }
    
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
        printk("Channel %d deactivated - sending BLE notification\n", channel_id);
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
    
    // Master valve logic: Handle post-stop delay and overlapping tasks
    if (master_config.enabled && master_config.auto_management) {
        if (!any_active) {
            // No more active valves
            if (master_valve_schedule.has_pending_task) {
                uint32_t now = k_uptime_get_32();
                uint32_t time_until_next = master_valve_schedule.next_task_start_time - now;
                
                if (time_until_next <= (master_config.overlap_grace_sec * 1000)) {
                    // Next task is within grace period - keep master valve open
                    printk("Master valve: Keeping open for next task in %u ms\n", time_until_next);
                    k_work_schedule(&master_valve_work, K_MSEC(time_until_next + (master_config.overlap_grace_sec * 1000)));
                } else {
                    // Next task is too far away - close master valve after delay
                    if (master_config.post_stop_delay_sec > 0) {
                        k_work_schedule(&master_valve_work, K_MSEC(master_config.post_stop_delay_sec * 1000));
                    } else {
                        master_valve_close();
                    }
                }
            } else {
                // No pending tasks - close master valve after delay
                if (master_config.post_stop_delay_sec > 0) {
                    k_work_schedule(&master_valve_work, K_MSEC(master_config.post_stop_delay_sec * 1000));
                } else {
                    master_valve_close();
                }
            }
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
 * @brief Close all valves including master valve
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
    
    // Also close master valve
    if (master_config.enabled) {
        watering_error_t master_err = master_valve_close();
        if (master_err != WATERING_SUCCESS) {
            result = master_err;
        }
    }
    
    return result;
}

/**
 * @brief Set master valve configuration
 * 
 * @param config Pointer to master valve configuration
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t master_valve_set_config(const master_valve_config_t *config) {
    if (!config) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    // Update configuration
    master_config.enabled = config->enabled;
    master_config.pre_start_delay_sec = config->pre_start_delay_sec;
    master_config.post_stop_delay_sec = config->post_stop_delay_sec;
    master_config.overlap_grace_sec = config->overlap_grace_sec;
    master_config.auto_management = config->auto_management;
    
    printk("Master valve config updated: enabled=%d, pre_delay=%d, post_delay=%d, grace=%d\n",
           master_config.enabled, master_config.pre_start_delay_sec, 
           master_config.post_stop_delay_sec, master_config.overlap_grace_sec);
    
    return WATERING_SUCCESS;
}

/**
 * @brief Get master valve configuration
 * 
 * @param config Pointer to store master valve configuration
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t master_valve_get_config(master_valve_config_t *config) {
    if (!config) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    *config = master_config;
    return WATERING_SUCCESS;
}

/**
 * @brief Notify master valve system about upcoming task
 * 
 * This allows the master valve logic to prepare for overlapping tasks
 * 
 * @param start_time When the next task will start (k_uptime_get_32() format)
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t master_valve_notify_upcoming_task(uint32_t start_time) {
    master_valve_schedule.next_task_start_time = start_time;
    master_valve_schedule.has_pending_task = true;
    
    printk("Master valve: Notified of upcoming task at %u\n", start_time);
    return WATERING_SUCCESS;
}

/**
 * @brief Clear pending task notification
 * 
 * Called when a scheduled task is cancelled or completed
 */
void master_valve_clear_pending_task(void) {
    master_valve_schedule.has_pending_task = false;
    master_valve_schedule.next_task_start_time = 0;
}

/**
 * @brief Manually open master valve (for BLE control)
 * 
 * This function allows manual control of the master valve via BLE.
 * Only works when auto_management is disabled.
 * 
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t master_valve_manual_open(void) {
    if (!master_config.enabled) {
        printk("Master valve is disabled\n");
        return WATERING_ERROR_CONFIG;
    }
    
    if (master_config.auto_management) {
        printk("Master valve is in automatic mode - manual control disabled\n");
        return WATERING_ERROR_BUSY;
    }
    
    return master_valve_open();
}

/**
 * @brief Manually close master valve (for BLE control)
 * 
 * This function allows manual control of the master valve via BLE.
 * Only works when auto_management is disabled.
 * 
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t master_valve_manual_close(void) {
    if (!master_config.enabled) {
        printk("Master valve is disabled\n");
        return WATERING_ERROR_CONFIG;
    }
    
    if (master_config.auto_management) {
        printk("Master valve is in automatic mode - manual control disabled\n");
        return WATERING_ERROR_BUSY;
    }
    
    return master_valve_close();
}

/**
 * @brief Get current master valve state
 * 
 * @return true if master valve is open, false if closed
 */
bool master_valve_is_open(void) {
    return master_config.enabled && master_config.is_active;
}
