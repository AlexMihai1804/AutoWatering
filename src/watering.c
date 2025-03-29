#include "watering.h"
#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <stdio.h>  // Add this for snprintf
#include "flow_sensor.h"
#include "watering_internal.h"

/**
 * @file watering.c
 * @brief Implementation of the core watering control system
 * 
 * This file implements the main interfaces for controlling watering valves
 * and managing the watering channels.
 */

/** Global array holding all watering channel configurations */
watering_channel_t watering_channels[WATERING_CHANNELS_COUNT];

/** Current system status/state */
watering_status_t system_status = WATERING_STATUS_OK;
watering_state_t system_state = WATERING_STATE_IDLE;
power_mode_t current_power_mode = POWER_MODE_NORMAL;
bool system_initialized = false;

/** Mutex for protecting system state */
K_MUTEX_DEFINE(system_state_mutex);

/**
 * @brief Log error with file and line information
 */
void log_error_with_info(const char *message, int error_code, const char *file, int line) {
    printk("ERROR [%s:%d]: %s (code: %d)\n", file, line, message, error_code);
}

/**
 * @brief Initialize the watering system
 */
watering_error_t watering_init(void) {
    if (system_initialized) {
        return WATERING_SUCCESS;  // Already initialized
    }
    
    k_mutex_lock(&system_state_mutex, K_FOREVER);
    
    // Initialize default channel names
    for (int i = 0; i < WATERING_CHANNELS_COUNT; i++) {
        snprintf(watering_channels[i].name, sizeof(watering_channels[i].name), "Channel %d", i + 1);
        watering_channels[i].is_active = false;
    }
    
    // Initialize subsystems
    watering_error_t err;
    
    // Initialize valve hardware
    err = valve_init();
    if (err != WATERING_SUCCESS) {
        LOG_ERROR("Failed to initialize valve hardware", err);
        k_mutex_unlock(&system_state_mutex);
        return err;
    }
    
    err = tasks_init();
    if (err != WATERING_SUCCESS) {
        LOG_ERROR("Failed to initialize tasks", err);
        k_mutex_unlock(&system_state_mutex);
        return err;
    }
    
    err = config_init();
    if (err != WATERING_SUCCESS) {
        LOG_ERROR("Failed to initialize configuration", err);
        // Non-critical error, continue initialization
    }
    
    err = flow_monitor_init();
    if (err != WATERING_SUCCESS) {
        LOG_ERROR("Failed to initialize flow monitoring", err);
        k_mutex_unlock(&system_state_mutex);
        return err;
    }
    
    system_initialized = true;
    system_state = WATERING_STATE_IDLE;
    system_status = WATERING_STATUS_OK;
    
    k_mutex_unlock(&system_state_mutex);
    return WATERING_SUCCESS;
}

/**
 * @brief Transition system to a new state
 */
watering_error_t transition_to_state(watering_state_t new_state) {
    k_mutex_lock(&system_state_mutex, K_FOREVER);
    
    // Check for valid state transitions
    bool transition_valid = false;
    
    switch (system_state) {
        case WATERING_STATE_IDLE:
            transition_valid = (new_state == WATERING_STATE_WATERING || 
                               new_state == WATERING_STATE_ERROR_RECOVERY);
            break;
        case WATERING_STATE_WATERING:
            transition_valid = (new_state == WATERING_STATE_IDLE || 
                               new_state == WATERING_STATE_PAUSED ||
                               new_state == WATERING_STATE_ERROR_RECOVERY);
            break;
        case WATERING_STATE_PAUSED:
            transition_valid = (new_state == WATERING_STATE_WATERING || 
                               new_state == WATERING_STATE_IDLE ||
                               new_state == WATERING_STATE_ERROR_RECOVERY);
            break;
        case WATERING_STATE_ERROR_RECOVERY:
            transition_valid = (new_state == WATERING_STATE_IDLE);
            break;
    }
    
    if (!transition_valid) {
        LOG_ERROR("Invalid state transition", new_state);
        k_mutex_unlock(&system_state_mutex);
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    printk("State transition: %d -> %d\n", system_state, new_state);
    system_state = new_state;
    
    k_mutex_unlock(&system_state_mutex);
    return WATERING_SUCCESS;
}

/**
 * @brief Attempt recovery from system errors
 */
watering_error_t attempt_error_recovery(watering_error_t error_code) {
    watering_error_t result = WATERING_ERROR_HARDWARE;
    
    // Transition to recovery state
    transition_to_state(WATERING_STATE_ERROR_RECOVERY);
    
    switch (error_code) {
        case WATERING_ERROR_HARDWARE:
            // Try turning all valves off and verifying hardware
            valve_close_all();
            
            // Check if hardware is now working
            for (int i = 0; i < WATERING_CHANNELS_COUNT; i++) {
                if (!device_is_ready(watering_channels[i].valve.port)) {
                    result = WATERING_ERROR_HARDWARE;
                    goto recovery_done;
                }
            }
            result = WATERING_SUCCESS;
            break;
            
        case WATERING_ERROR_RTC_FAILURE:
            // RTC failures may require external intervention
            printk("RTC failure requires manual intervention\n");
            result = WATERING_ERROR_RTC_FAILURE;
            break;
            
        default:
            // Try to reset to a known good state
            valve_close_all();
            result = WATERING_SUCCESS;
    }
    
recovery_done:
    if (result == WATERING_SUCCESS) {
        transition_to_state(WATERING_STATE_IDLE);
        system_status = WATERING_STATUS_OK;
        printk("Error recovery successful\n");
    } else {
        printk("Error recovery failed\n");
    }
    
    return result;
}

/**
 * @brief Get reference to a specific watering channel
 */
watering_error_t watering_get_channel(uint8_t channel_id, watering_channel_t **channel) {
    if (channel_id >= WATERING_CHANNELS_COUNT) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    if (channel == NULL) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    *channel = &watering_channels[channel_id];
    return WATERING_SUCCESS;
}

/**
 * @brief Get the current watering system status
 */
watering_error_t watering_get_status(watering_status_t *status) {
    if (status == NULL) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    k_mutex_lock(&system_state_mutex, K_FOREVER);
    *status = system_status;
    k_mutex_unlock(&system_state_mutex);
    
    return WATERING_SUCCESS;
}

/**
 * @brief Get the current watering system state
 */
watering_error_t watering_get_state(watering_state_t *state) {
    if (state == NULL) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    k_mutex_lock(&system_state_mutex, K_FOREVER);
    *state = system_state;
    k_mutex_unlock(&system_state_mutex);
    
    return WATERING_SUCCESS;
}

/**
 * @brief Set the system power mode
 */
watering_error_t watering_set_power_mode(power_mode_t mode) {
    // Validate power mode
    if (mode > POWER_MODE_ULTRA_LOW_POWER) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    k_mutex_lock(&system_state_mutex, K_FOREVER);
    
    // Don't change mode if we're in the middle of watering
    if (system_state == WATERING_STATE_WATERING) {
        k_mutex_unlock(&system_state_mutex);
        return WATERING_ERROR_BUSY;
    }
    
    printk("Changing power mode from %d to %d\n", current_power_mode, mode);
    current_power_mode = mode;
    
    // Apply power mode-specific settings
    update_power_timings(mode);
    
    if (mode == POWER_MODE_ULTRA_LOW_POWER) {
        system_status = WATERING_STATUS_LOW_POWER;
    } else if (system_status == WATERING_STATUS_LOW_POWER) {
        system_status = WATERING_STATUS_OK;
    }
    
    k_mutex_unlock(&system_state_mutex);
    return WATERING_SUCCESS;
}

/**
 * @brief Get the current power mode
 */
watering_error_t watering_get_power_mode(power_mode_t *mode) {
    if (mode == NULL) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    k_mutex_lock(&system_state_mutex, K_FOREVER);
    *mode = current_power_mode;
    k_mutex_unlock(&system_state_mutex);
    
    return WATERING_SUCCESS;
}

/**
 * @brief Update system timing based on power mode
 */
watering_error_t update_power_timings(power_mode_t mode) {
    // Implement different timing strategies based on power mode
    switch (mode) {
        case POWER_MODE_NORMAL:
            // Use standard polling intervals
            printk("Using normal power timings\n");
            break;
            
        case POWER_MODE_ENERGY_SAVING:
            // Reduce polling frequency to save energy
            printk("Using energy saving timings\n");
            break;
            
        case POWER_MODE_ULTRA_LOW_POWER:
            // Minimal polling, mostly sleep
            printk("Using ultra-low power timings\n");
            break;
            
        default:
            return WATERING_ERROR_INVALID_PARAM;
    }
    
    return WATERING_SUCCESS;
}

/**
 * @brief Clean up resources for graceful shutdown
 */
void cleanup_resources(void) {
    // Ensure all valves are closed
    valve_close_all();
    
    // Save configuration before shutdown
    watering_save_config();
    
    // Release any held resources
    printk("Resources cleaned up for shutdown\n");
}
