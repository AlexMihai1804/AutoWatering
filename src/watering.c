#include "watering.h"
#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <stdio.h>  // Add this for snprintf
#include <string.h> // Add this for strncpy, memset
#include "flow_sensor.h"
#include "watering_internal.h"
#include "bt_irrigation_service.h"     /* + BLE status update */

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
 */
watering_error_t watering_init(void) {
    // Start with minimum logging level
    watering_log_init(WATERING_LOG_LEVEL_ERROR);

    // Initialize mutex
    static bool mutex_initialized = false;
    if (!mutex_initialized) {
        k_mutex_init(&system_state_mutex);
        mutex_initialized = true;
    }

    // Initialize task system
    watering_error_t err = tasks_init();
    if (err != WATERING_SUCCESS) {
        return err;
    }

    // Set up settings system
    err = config_init();
    if (err != WATERING_SUCCESS) {
        LOG_ERROR("Configuration subsystem init failed", err);
    }

    // Use ultra minimal valve initialization
    err = valve_init();
    if (err != WATERING_SUCCESS) {
        // Don't hang here, but report the error
        printk("Valve initialization failed but continuing: %d\n", err);
    }

    // Set default system state
    system_state = WATERING_STATE_IDLE;
    system_status = WATERING_STATUS_OK;
    current_power_mode = POWER_MODE_NORMAL;

    // Update system flags
    system_initialized = true;
    
    /* always start flow-monitoring */
    flow_monitor_init();
    
    // Ensure all valves are closed as a safety measure
    valve_close_all();

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
        bt_irrigation_system_status_update(system_status);   /* NEW */
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
    bt_irrigation_system_status_update(system_status);   /* NEW */
    
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

/**
 * @brief Cancel all tasks and clear the task queue
 * 
 * @return Number of tasks canceled
 */
int watering_cancel_all_tasks(void) {
    int removed = 0;
    
    // First, stop the current task if it exists
    if (watering_stop_current_task()) {
        removed = 1;
    }
    
    // Then, clear the pending task queue
    removed += watering_clear_task_queue();
    
    return removed;
}

/**
 * @brief Get the status of the task queue
 * 
 * @param pending_count Pointer where the number of pending tasks will be stored
 * @param active Flag indicating if there is an active task
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_get_queue_status(uint8_t *pending_count, bool *active) {
    if (pending_count == NULL || active == NULL) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    k_mutex_lock(&system_state_mutex, K_FOREVER);
    
    *pending_count = watering_get_pending_tasks_count();
    *active = (watering_task_state.current_active_task != NULL);
    
    k_mutex_unlock(&system_state_mutex);
    
    return WATERING_SUCCESS;
}

/**
 * @brief Validate the configuration of a watering event
 * 
 * This function checks the event parameters for correctness.
 * 
 * @param event Pointer to the watering event to validate
 * @return WATERING_SUCCESS if the configuration is valid, error code otherwise
 */
watering_error_t watering_validate_event_config(const watering_event_t *event) {
    if (event == NULL) {
        return WATERING_ERROR_INVALID_PARAM;
    }

    /* NEW: accept any values if the event is disabled */
    if (!event->auto_enabled) {
        return WATERING_SUCCESS;
    }

    // Validate schedule type
    if (event->schedule_type != SCHEDULE_DAILY && 
        event->schedule_type != SCHEDULE_PERIODIC) {
        return WATERING_ERROR_INVALID_PARAM;
    }

    // Validate time
    if (event->start_time.hour > 23 || 
        event->start_time.minute > 59) {
        return WATERING_ERROR_INVALID_PARAM;
    }

    // Validate watering mode
    if (event->watering_mode != WATERING_BY_DURATION && 
        event->watering_mode != WATERING_BY_VOLUME) {
        return WATERING_ERROR_INVALID_PARAM;
    }

    // Validate watering values
    if (event->watering_mode == WATERING_BY_DURATION) {
        if (event->watering.by_duration.duration_minutes == 0) {
            return WATERING_ERROR_INVALID_PARAM;
        }
    } else {
        if (event->watering.by_volume.volume_liters == 0) {
            return WATERING_ERROR_INVALID_PARAM;
        }
    }

    // Validate schedule-specific settings
    if (event->schedule_type == SCHEDULE_DAILY) {
        // At least one day must be selected
        if (event->schedule.daily.days_of_week == 0) {
            return WATERING_ERROR_INVALID_PARAM;
        }
    } else {
        // Periodic must have interval > 0
        if (event->schedule.periodic.interval_days == 0) {
            return WATERING_ERROR_INVALID_PARAM;
        }
    }

    return WATERING_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Clear run-time error flags & counters                              */
watering_error_t watering_clear_errors(void)
{
    k_mutex_lock(&system_state_mutex, K_FOREVER);

    /* return to a known good state */
    if (system_state == WATERING_STATE_ERROR_RECOVERY) {
        transition_to_state(WATERING_STATE_IDLE);
    }
    system_status = WATERING_STATUS_OK;
    k_mutex_unlock(&system_state_mutex);

    /* reset flow-monitor counters too */
    flow_monitor_clear_errors();

    /* inform BLE client (ignore if not connected) */
    bt_irrigation_system_status_update(system_status);

    printk("All error flags cleared, system back to OK\n");
    return WATERING_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Plant and growing environment configuration functions              */
/* ------------------------------------------------------------------ */

/**
 * @brief Set the plant type for a channel
 */
watering_error_t watering_set_plant_type(uint8_t channel_id, plant_type_t plant_type) {
    if (channel_id >= WATERING_CHANNELS_COUNT) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    if (plant_type > PLANT_TYPE_OTHER) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    watering_channels[channel_id].plant_type = plant_type;
    
    // Save configuration changes
    watering_save_config();
    
    printk("Channel %d plant type set to %d\n", channel_id, plant_type);
    return WATERING_SUCCESS;
}

/**
 * @brief Get the plant type for a channel
 */
watering_error_t watering_get_plant_type(uint8_t channel_id, plant_type_t *plant_type) {
    if (channel_id >= WATERING_CHANNELS_COUNT || plant_type == NULL) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    *plant_type = watering_channels[channel_id].plant_type;
    return WATERING_SUCCESS;
}

/**
 * @brief Set the soil type for a channel
 */
watering_error_t watering_set_soil_type(uint8_t channel_id, soil_type_t soil_type) {
    if (channel_id >= WATERING_CHANNELS_COUNT) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    if (soil_type > SOIL_TYPE_HYDROPONIC) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    watering_channels[channel_id].soil_type = soil_type;
    
    // Save configuration changes
    watering_save_config();
    
    printk("Channel %d soil type set to %d\n", channel_id, soil_type);
    return WATERING_SUCCESS;
}

/**
 * @brief Get the soil type for a channel
 */
watering_error_t watering_get_soil_type(uint8_t channel_id, soil_type_t *soil_type) {
    if (channel_id >= WATERING_CHANNELS_COUNT || soil_type == NULL) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    *soil_type = watering_channels[channel_id].soil_type;
    return WATERING_SUCCESS;
}

/**
 * @brief Set the irrigation method for a channel
 */
watering_error_t watering_set_irrigation_method(uint8_t channel_id, irrigation_method_t irrigation_method) {
    if (channel_id >= WATERING_CHANNELS_COUNT) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    if (irrigation_method > IRRIGATION_SUBSURFACE) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    watering_channels[channel_id].irrigation_method = irrigation_method;
    
    // Save configuration changes
    watering_save_config();
    
    printk("Channel %d irrigation method set to %d\n", channel_id, irrigation_method);
    return WATERING_SUCCESS;
}

/**
 * @brief Get the irrigation method for a channel
 */
watering_error_t watering_get_irrigation_method(uint8_t channel_id, irrigation_method_t *irrigation_method) {
    if (channel_id >= WATERING_CHANNELS_COUNT || irrigation_method == NULL) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    *irrigation_method = watering_channels[channel_id].irrigation_method;
    return WATERING_SUCCESS;
}

/**
 * @brief Set the coverage area for a channel
 */
watering_error_t watering_set_coverage_area(uint8_t channel_id, float area_m2) {
    if (channel_id >= WATERING_CHANNELS_COUNT) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    if (area_m2 < 0.0f || area_m2 > 10000.0f) { // Reasonable limit of 1 hectare
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    watering_channels[channel_id].coverage.use_area = true;
    watering_channels[channel_id].coverage.area.area_m2 = area_m2;
    
    // Save configuration changes
    watering_save_config();
    
    printk("Channel %d coverage area set to %.2f m²\n", channel_id, (double)area_m2);
    return WATERING_SUCCESS;
}

/**
 * @brief Set the plant count for a channel
 */
watering_error_t watering_set_plant_count(uint8_t channel_id, uint16_t count) {
    if (channel_id >= WATERING_CHANNELS_COUNT) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    if (count == 0 || count > 10000) { // Reasonable limits
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    watering_channels[channel_id].coverage.use_area = false;
    watering_channels[channel_id].coverage.plants.count = count;
    
    // Save configuration changes
    watering_save_config();
    
    printk("Channel %d plant count set to %d\n", channel_id, count);
    return WATERING_SUCCESS;
}

/**
 * @brief Get the coverage information for a channel
 */
watering_error_t watering_get_coverage(uint8_t channel_id, channel_coverage_t *coverage) {
    if (channel_id >= WATERING_CHANNELS_COUNT || coverage == NULL) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    *coverage = watering_channels[channel_id].coverage;
    return WATERING_SUCCESS;
}

/**
 * @brief Set the sun percentage for a channel
 */
watering_error_t watering_set_sun_percentage(uint8_t channel_id, uint8_t sun_percentage) {
    if (channel_id >= WATERING_CHANNELS_COUNT) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    if (sun_percentage > 100) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    watering_channels[channel_id].sun_percentage = sun_percentage;
    
    // Save configuration changes
    watering_save_config();
    
    printk("Channel %d sun percentage set to %d%%\n", channel_id, sun_percentage);
    return WATERING_SUCCESS;
}

/**
 * @brief Get the sun percentage for a channel
 */
watering_error_t watering_get_sun_percentage(uint8_t channel_id, uint8_t *sun_percentage) {
    if (channel_id >= WATERING_CHANNELS_COUNT || sun_percentage == NULL) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    *sun_percentage = watering_channels[channel_id].sun_percentage;
    return WATERING_SUCCESS;
}

/**
 * @brief Set custom plant configuration for a channel
 */
watering_error_t watering_set_custom_plant(uint8_t channel_id, const custom_plant_config_t *custom_config) {
    if (channel_id >= WATERING_CHANNELS_COUNT || custom_config == NULL) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    // Validate water need factor range
    if (custom_config->water_need_factor < 0.1f || custom_config->water_need_factor > 5.0f) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    // Validate irrigation frequency
    if (custom_config->irrigation_freq == 0 || custom_config->irrigation_freq > 30) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    // Copy the configuration
    watering_channels[channel_id].custom_plant = *custom_config;
    
    // Ensure custom name is null-terminated
    watering_channels[channel_id].custom_plant.custom_name[31] = '\0';
    
    // Save configuration changes
    watering_save_config();
    
    printk("Channel %d custom plant configured: %s (factor: %.1f)\n", 
           channel_id, custom_config->custom_name, (double)custom_config->water_need_factor);
    return WATERING_SUCCESS;
}

/**
 * @brief Get custom plant configuration for a channel
 */
watering_error_t watering_get_custom_plant(uint8_t channel_id, custom_plant_config_t *custom_config) {
    if (channel_id >= WATERING_CHANNELS_COUNT || custom_config == NULL) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    *custom_config = watering_channels[channel_id].custom_plant;
    return WATERING_SUCCESS;
}

/**
 * @brief Get the recommended coverage measurement type based on irrigation method
 */
bool watering_recommend_area_based_measurement(irrigation_method_t irrigation_method) {
    switch (irrigation_method) {
        case IRRIGATION_DRIP:
        case IRRIGATION_MICRO_SPRAY:
            // These methods target individual plants
            return false;  // Recommend plant count
            
        case IRRIGATION_SPRINKLER:
        case IRRIGATION_SOAKER_HOSE:
        case IRRIGATION_FLOOD:
        case IRRIGATION_SUBSURFACE:
            // These methods cover areas uniformly
            return true;   // Recommend area-based (m²)
            
        default:
            return true;   // Default to area-based for unknown methods
    }
}

/**
 * @brief Get water need factor for a specific plant type
 */
float watering_get_plant_water_factor(plant_type_t plant_type, const custom_plant_config_t *custom_config) {
    switch (plant_type) {
        case PLANT_TYPE_VEGETABLES:
            return 1.2f;  // Higher water needs for vegetables
            
        case PLANT_TYPE_HERBS:
            return 0.8f;  // Moderate water needs for herbs
            
        case PLANT_TYPE_FLOWERS:
            return 1.0f;  // Standard water needs for flowers
            
        case PLANT_TYPE_SHRUBS:
            return 0.7f;  // Lower water needs for established shrubs
            
        case PLANT_TYPE_TREES:
            return 0.9f;  // Moderate water needs for trees
            
        case PLANT_TYPE_LAWN:
            return 1.1f;  // Regular watering for lawn
            
        case PLANT_TYPE_SUCCULENTS:
            return 0.3f;  // Very low water needs for succulents
            
        case PLANT_TYPE_OTHER:
            // Use custom configuration if available
            if (custom_config != NULL) {
                return custom_config->water_need_factor;
            }
            return 1.0f;  // Default factor if no custom config
            
        default:
            return 1.0f;  // Default factor for unknown types
    }
}

/**
 * @brief Validate if coverage measurement type matches irrigation method recommendation
 */
bool watering_validate_coverage_method_match(irrigation_method_t irrigation_method, bool use_area_based) {
    bool recommended_area_based = watering_recommend_area_based_measurement(irrigation_method);
    return (use_area_based == recommended_area_based);
}

/**
 * @brief Get comprehensive channel environment information
 */
watering_error_t watering_get_channel_environment(uint8_t channel_id, 
                                                 plant_type_t *plant_type,
                                                 soil_type_t *soil_type,
                                                 irrigation_method_t *irrigation_method,
                                                 channel_coverage_t *coverage,
                                                 uint8_t *sun_percentage,
                                                 custom_plant_config_t *custom_config) {
    if (channel_id >= WATERING_CHANNELS_COUNT) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    watering_channel_t *channel = &watering_channels[channel_id];
    
    if (plant_type) *plant_type = channel->plant_type;
    if (soil_type) *soil_type = channel->soil_type;
    if (irrigation_method) *irrigation_method = channel->irrigation_method;
    if (coverage) *coverage = channel->coverage;
    if (sun_percentage) *sun_percentage = channel->sun_percentage;
    if (custom_config) *custom_config = channel->custom_plant;
    
    return WATERING_SUCCESS;
}

/**
 * @brief Set comprehensive channel environment configuration
 */
watering_error_t watering_set_channel_environment(uint8_t channel_id,
                                                 plant_type_t plant_type,
                                                 soil_type_t soil_type,
                                                 irrigation_method_t irrigation_method,
                                                 const channel_coverage_t *coverage,
                                                 uint8_t sun_percentage,
                                                 const custom_plant_config_t *custom_config) {
    if (channel_id >= WATERING_CHANNELS_COUNT) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    if (sun_percentage > 100) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    if (coverage == NULL) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    watering_channel_t *channel = &watering_channels[channel_id];
    
    // Set all environment parameters
    channel->plant_type = plant_type;
    channel->soil_type = soil_type;
    channel->irrigation_method = irrigation_method;
    channel->coverage = *coverage;
    channel->sun_percentage = sun_percentage;
    
    // Set custom plant config if provided and plant type is OTHER
    if (plant_type == PLANT_TYPE_OTHER && custom_config != NULL) {
        // Validate custom config
        if (custom_config->water_need_factor < 0.1f || custom_config->water_need_factor > 5.0f) {
            return WATERING_ERROR_INVALID_PARAM;
        }
        if (custom_config->irrigation_freq == 0 || custom_config->irrigation_freq > 30) {
            return WATERING_ERROR_INVALID_PARAM;
        }
        
        channel->custom_plant = *custom_config;
        // Ensure custom name is null-terminated
        channel->custom_plant.custom_name[31] = '\0';
    } else if (plant_type == PLANT_TYPE_OTHER) {
        // Clear custom config if plant type is OTHER but no config provided
        memset(&channel->custom_plant, 0, sizeof(custom_plant_config_t));
        strncpy(channel->custom_plant.custom_name, "Custom Plant", 31);
        channel->custom_plant.water_need_factor = 1.0f;
        channel->custom_plant.irrigation_freq = 3;
        channel->custom_plant.prefer_area_based = watering_recommend_area_based_measurement(irrigation_method);
    }
    
    // Save configuration changes
    watering_save_config();
    
    printk("Channel %d environment configured: plant=%d, soil=%d, irrigation=%d, sun=%d%%\n",
           channel_id, plant_type, soil_type, irrigation_method, sun_percentage);
    
    return WATERING_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Specific plant type management functions                           */
/* ------------------------------------------------------------------ */

/**
 * @brief Set specific vegetable type for a channel
 */
watering_error_t watering_set_vegetable_type(uint8_t channel_id, vegetable_type_t vegetable_type) {
    if (channel_id >= WATERING_CHANNELS_COUNT) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    if (vegetable_type > VEGETABLE_OTHER) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    // Set main type to vegetables and specific type
    watering_channels[channel_id].plant_info.main_type = PLANT_TYPE_VEGETABLES;
    watering_channels[channel_id].plant_info.specific.vegetable = vegetable_type;
    watering_channels[channel_id].plant_type = PLANT_TYPE_VEGETABLES; // For backward compatibility
    
    watering_save_config();
    printk("Channel %d vegetable type set to %d\n", channel_id, vegetable_type);
    return WATERING_SUCCESS;
}

/**
 * @brief Get specific vegetable type for a channel
 */
watering_error_t watering_get_vegetable_type(uint8_t channel_id, vegetable_type_t *vegetable_type) {
    if (channel_id >= WATERING_CHANNELS_COUNT || vegetable_type == NULL) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    if (watering_channels[channel_id].plant_info.main_type != PLANT_TYPE_VEGETABLES) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    *vegetable_type = watering_channels[channel_id].plant_info.specific.vegetable;
    return WATERING_SUCCESS;
}

/**
 * @brief Set specific herb type for a channel
 */
watering_error_t watering_set_herb_type(uint8_t channel_id, herb_type_t herb_type) {
    if (channel_id >= WATERING_CHANNELS_COUNT) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    if (herb_type > HERB_OTHER) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    watering_channels[channel_id].plant_info.main_type = PLANT_TYPE_HERBS;
    watering_channels[channel_id].plant_info.specific.herb = herb_type;
    watering_channels[channel_id].plant_type = PLANT_TYPE_HERBS;
    
    watering_save_config();
    printk("Channel %d herb type set to %d\n", channel_id, herb_type);
    return WATERING_SUCCESS;
}

/**
 * @brief Get specific herb type for a channel
 */
watering_error_t watering_get_herb_type(uint8_t channel_id, herb_type_t *herb_type) {
    if (channel_id >= WATERING_CHANNELS_COUNT || herb_type == NULL) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    if (watering_channels[channel_id].plant_info.main_type != PLANT_TYPE_HERBS) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    *herb_type = watering_channels[channel_id].plant_info.specific.herb;
    return WATERING_SUCCESS;
}

/**
 * @brief Set specific flower type for a channel
 */
watering_error_t watering_set_flower_type(uint8_t channel_id, flower_type_t flower_type) {
    if (channel_id >= WATERING_CHANNELS_COUNT) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    if (flower_type > FLOWER_OTHER) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    watering_channels[channel_id].plant_info.main_type = PLANT_TYPE_FLOWERS;
    watering_channels[channel_id].plant_info.specific.flower = flower_type;
    watering_channels[channel_id].plant_type = PLANT_TYPE_FLOWERS;
    
    watering_save_config();
    printk("Channel %d flower type set to %d\n", channel_id, flower_type);
    return WATERING_SUCCESS;
}

/**
 * @brief Get specific flower type for a channel
 */
watering_error_t watering_get_flower_type(uint8_t channel_id, flower_type_t *flower_type) {
    if (channel_id >= WATERING_CHANNELS_COUNT || flower_type == NULL) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    if (watering_channels[channel_id].plant_info.main_type != PLANT_TYPE_FLOWERS) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    *flower_type = watering_channels[channel_id].plant_info.specific.flower;
    return WATERING_SUCCESS;
}

/**
 * @brief Set specific tree type for a channel
 */
watering_error_t watering_set_tree_type(uint8_t channel_id, tree_type_t tree_type) {
    if (channel_id >= WATERING_CHANNELS_COUNT) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    if (tree_type > TREE_OTHER) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    watering_channels[channel_id].plant_info.main_type = PLANT_TYPE_TREES;
    watering_channels[channel_id].plant_info.specific.tree = tree_type;
    watering_channels[channel_id].plant_type = PLANT_TYPE_TREES;
    
    watering_save_config();
    printk("Channel %d tree type set to %d\n", channel_id, tree_type);
    return WATERING_SUCCESS;
}

/**
 * @brief Get specific tree type for a channel
 */
watering_error_t watering_get_tree_type(uint8_t channel_id, tree_type_t *tree_type) {
    if (channel_id >= WATERING_CHANNELS_COUNT || tree_type == NULL) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    if (watering_channels[channel_id].plant_info.main_type != PLANT_TYPE_TREES) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    *tree_type = watering_channels[channel_id].plant_info.specific.tree;
    return WATERING_SUCCESS;
}

/**
 * @brief Set specific lawn type for a channel
 */
watering_error_t watering_set_lawn_type(uint8_t channel_id, lawn_type_t lawn_type) {
    if (channel_id >= WATERING_CHANNELS_COUNT) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    if (lawn_type > LAWN_OTHER) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    watering_channels[channel_id].plant_info.main_type = PLANT_TYPE_LAWN;
    watering_channels[channel_id].plant_info.specific.lawn = lawn_type;
    watering_channels[channel_id].plant_type = PLANT_TYPE_LAWN;
    
    watering_save_config();
    printk("Channel %d lawn type set to %d\n", channel_id, lawn_type);
    return WATERING_SUCCESS;
}

/**
 * @brief Get specific lawn type for a channel
 */
watering_error_t watering_get_lawn_type(uint8_t channel_id, lawn_type_t *lawn_type) {
    if (channel_id >= WATERING_CHANNELS_COUNT || lawn_type == NULL) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    if (watering_channels[channel_id].plant_info.main_type != PLANT_TYPE_LAWN) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    *lawn_type = watering_channels[channel_id].plant_info.specific.lawn;
    return WATERING_SUCCESS;
}

/**
 * @brief Set specific succulent type for a channel
 */
watering_error_t watering_set_succulent_type(uint8_t channel_id, succulent_type_t succulent_type) {
    if (channel_id >= WATERING_CHANNELS_COUNT) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    if (succulent_type > SUCCULENT_OTHER) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    watering_channels[channel_id].plant_info.main_type = PLANT_TYPE_SUCCULENTS;
    watering_channels[channel_id].plant_info.specific.succulent = succulent_type;
    watering_channels[channel_id].plant_type = PLANT_TYPE_SUCCULENTS;
    
    watering_save_config();
    printk("Channel %d succulent type set to %d\n", channel_id, succulent_type);
    return WATERING_SUCCESS;
}

/**
 * @brief Get specific succulent type for a channel
 */
watering_error_t watering_get_succulent_type(uint8_t channel_id, succulent_type_t *succulent_type) {
    if (channel_id >= WATERING_CHANNELS_COUNT || succulent_type == NULL) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    if (watering_channels[channel_id].plant_info.main_type != PLANT_TYPE_SUCCULENTS) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    *succulent_type = watering_channels[channel_id].plant_info.specific.succulent;
    return WATERING_SUCCESS;
}

/**
 * @brief Set specific shrub type for a channel
 */
watering_error_t watering_set_shrub_type(uint8_t channel_id, shrub_type_t shrub_type) {
    if (channel_id >= WATERING_CHANNELS_COUNT) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    if (shrub_type > SHRUB_OTHER) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    watering_channels[channel_id].plant_info.main_type = PLANT_TYPE_SHRUBS;
    watering_channels[channel_id].plant_info.specific.shrub = shrub_type;
    watering_channels[channel_id].plant_type = PLANT_TYPE_SHRUBS;
    
    watering_save_config();
    printk("Channel %d shrub type set to %d\n", channel_id, shrub_type);
    return WATERING_SUCCESS;
}

/**
 * @brief Get specific shrub type for a channel
 */
watering_error_t watering_get_shrub_type(uint8_t channel_id, shrub_type_t *shrub_type) {
    if (channel_id >= WATERING_CHANNELS_COUNT || shrub_type == NULL) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    if (watering_channels[channel_id].plant_info.main_type != PLANT_TYPE_SHRUBS) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    *shrub_type = watering_channels[channel_id].plant_info.specific.shrub;
    return WATERING_SUCCESS;
}

/**
 * @brief Get complete plant information for a channel
 */
watering_error_t watering_get_plant_info(uint8_t channel_id, plant_info_t *plant_info) {
    if (channel_id >= WATERING_CHANNELS_COUNT || plant_info == NULL) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    *plant_info = watering_channels[channel_id].plant_info;
    return WATERING_SUCCESS;
}

/**
 * @brief Set complete plant information for a channel
 */
watering_error_t watering_set_plant_info(uint8_t channel_id, const plant_info_t *plant_info) {
    if (channel_id >= WATERING_CHANNELS_COUNT || plant_info == NULL) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    if (plant_info->main_type > PLANT_TYPE_OTHER) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    watering_channels[channel_id].plant_info = *plant_info;
    watering_channels[channel_id].plant_type = plant_info->main_type; // For backward compatibility
    
    watering_save_config();
    printk("Channel %d plant info updated (main type: %d)\n", channel_id, plant_info->main_type);
    return WATERING_SUCCESS;
}
