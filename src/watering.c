#include <zephyr/autoconf.h>
#include "watering.h"
#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/logging/log.h>
#include <stdio.h>  // Add this for snprintf
#include <string.h> // Add this for strncpy, memset
#include "flow_sensor.h"
#include "watering_internal.h"
#include "watering_history.h"          /* Add history integration */
#ifdef CONFIG_BT
#include "bt_irrigation_service.h"     /* + BLE status update */
#endif
#include "plant_db.h"                  /* Add plant database */
#include "fao56_calc.h"                /* Add FAO-56 calculations */
#include "rain_sensor.h"               /* Rain sensor integration */
#include "rain_integration.h"          /* Rain integration system */
#include "rain_history.h"              /* Rain history management */
#include "onboarding_state.h"          /* Onboarding state management */
#include "timezone.h"                  /* For UTC timestamping */
#include "nvs_config.h"                /* For lock persistence */

#ifdef CONFIG_BT
int bt_irrigation_hydraulic_status_notify(uint8_t channel_id);
#endif

LOG_MODULE_DECLARE(watering, CONFIG_LOG_DEFAULT_LEVEL);

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

static hydraulic_lock_state_t global_hydraulic_lock = {
    .level = HYDRAULIC_LOCK_NONE,
    .reason = HYDRAULIC_LOCK_REASON_NONE,
    .locked_at_epoch = 0,
    .retry_after_epoch = 0
};

static uint8_t manual_override_channel = 0xFF;
static uint32_t manual_override_until_ms = 0;

/** Mutex for protecting system state */
K_MUTEX_DEFINE(system_state_mutex);

/** Global tracking for completed tasks */
static int completed_tasks_count = 0;
static K_MUTEX_DEFINE(completed_tasks_mutex);

typedef struct {
    bool active;                     /**< Snapshot validity flag */
    watering_event_t snapshot;       /**< Original event configuration */
} watering_event_backup_t;

static watering_event_backup_t watering_event_backups[WATERING_CHANNELS_COUNT];

void watering_snapshot_event(uint8_t channel_id)
{
    if (channel_id >= WATERING_CHANNELS_COUNT) {
        return;
    }

    watering_event_backup_t *backup = &watering_event_backups[channel_id];
    if (backup->active) {
        return; /* Existing snapshot covers current override */
    }

    backup->snapshot = watering_channels[channel_id].watering_event;
    backup->active = true;
}

void watering_restore_event(uint8_t channel_id)
{
    if (channel_id >= WATERING_CHANNELS_COUNT) {
        return;
    }

    watering_event_backup_t *backup = &watering_event_backups[channel_id];
    if (!backup->active) {
        return;
    }

    watering_channels[channel_id].watering_event = backup->snapshot;
    backup->active = false;
}

static bool manual_override_active_for_channel(uint8_t channel_id)
{
    uint32_t now_ms = k_uptime_get_32();

    if (watering_task_state.manual_override_active &&
        watering_task_state.current_active_task &&
        watering_task_state.current_active_task->channel) {
        uint8_t active_id = watering_task_state.current_active_task->channel - watering_channels;
        if (active_id == channel_id) {
            return true;
        }
    }

    if (manual_override_until_ms == 0) {
        return false;
    }

    if (now_ms >= manual_override_until_ms) {
        manual_override_until_ms = 0;
        manual_override_channel = 0xFF;
        return false;
    }

    return (manual_override_channel == 0xFF || manual_override_channel == channel_id);
}

bool watering_hydraulic_is_global_locked(void)
{
    return (global_hydraulic_lock.level != HYDRAULIC_LOCK_NONE);
}

void watering_get_global_hydraulic_lock(hydraulic_lock_state_t *out_lock)
{
    if (!out_lock) {
        return;
    }
    *out_lock = global_hydraulic_lock;
}

bool watering_hydraulic_is_channel_locked(uint8_t channel_id)
{
    if (channel_id >= WATERING_CHANNELS_COUNT) {
        return false;
    }
    return (watering_channels[channel_id].hydraulic_lock.level != HYDRAULIC_LOCK_NONE);
}

bool watering_hydraulic_manual_override_active(uint8_t channel_id)
{
    if (channel_id >= WATERING_CHANNELS_COUNT) {
        return false;
    }
    return manual_override_active_for_channel(channel_id);
}

void watering_hydraulic_set_manual_override(uint8_t channel_id, uint32_t duration_ms)
{
    manual_override_channel = channel_id;
    manual_override_until_ms = k_uptime_get_32() + duration_ms;
#ifdef CONFIG_BT
    bt_irrigation_hydraulic_status_notify(channel_id);
#endif
}

void watering_hydraulic_clear_manual_override(void)
{
    uint8_t previous_channel = manual_override_channel;
    manual_override_channel = 0xFF;
    manual_override_until_ms = 0;
#ifdef CONFIG_BT
    if (previous_channel < WATERING_CHANNELS_COUNT) {
        bt_irrigation_hydraulic_status_notify(previous_channel);
    } else {
        bt_irrigation_hydraulic_status_notify(0xFF);
    }
#endif
}

void watering_hydraulic_set_global_lock(hydraulic_lock_level_t level, hydraulic_lock_reason_t reason)
{
    if (level == HYDRAULIC_LOCK_NONE) {
        watering_hydraulic_clear_global_lock();
        return;
    }

    uint32_t now_epoch = timezone_get_unix_utc();
    global_hydraulic_lock.level = level;
    global_hydraulic_lock.reason = reason;
    global_hydraulic_lock.locked_at_epoch = now_epoch;
    global_hydraulic_lock.retry_after_epoch =
        (level == HYDRAULIC_LOCK_SOFT && now_epoch > 0) ? (now_epoch + HYDRAULIC_SOFT_LOCK_RETRY_SEC) : 0;

    if (system_status != WATERING_STATUS_LOCKED) {
        system_status = WATERING_STATUS_LOCKED;
#ifdef CONFIG_BT
        bt_irrigation_system_status_update(system_status);
#endif
    }

    nvs_save_hydraulic_global_lock(&global_hydraulic_lock);
#ifdef CONFIG_BT
    bt_irrigation_hydraulic_status_notify(0xFF);
#endif
}

void watering_hydraulic_clear_global_lock(void)
{
    global_hydraulic_lock.level = HYDRAULIC_LOCK_NONE;
    global_hydraulic_lock.reason = HYDRAULIC_LOCK_REASON_NONE;
    global_hydraulic_lock.locked_at_epoch = 0;
    global_hydraulic_lock.retry_after_epoch = 0;

    if (system_status == WATERING_STATUS_LOCKED) {
        system_status = WATERING_STATUS_OK;
        bt_irrigation_system_status_update(system_status);
    }

    nvs_save_hydraulic_global_lock(&global_hydraulic_lock);
    bt_irrigation_hydraulic_status_notify(0xFF);
}

void watering_hydraulic_set_channel_lock(uint8_t channel_id, hydraulic_lock_level_t level, hydraulic_lock_reason_t reason)
{
    if (channel_id >= WATERING_CHANNELS_COUNT) {
        return;
    }

    if (level == HYDRAULIC_LOCK_NONE) {
        watering_hydraulic_clear_channel_lock(channel_id);
        return;
    }

    uint32_t now_epoch = timezone_get_unix_utc();
    watering_channels[channel_id].hydraulic_lock.level = level;
    watering_channels[channel_id].hydraulic_lock.reason = reason;
    watering_channels[channel_id].hydraulic_lock.locked_at_epoch = now_epoch;
    watering_channels[channel_id].hydraulic_lock.retry_after_epoch =
        (level == HYDRAULIC_LOCK_SOFT && now_epoch > 0) ?
            (now_epoch + ((reason == HYDRAULIC_LOCK_REASON_NO_FLOW) ?
                          HYDRAULIC_NO_FLOW_RETRY_COOLDOWN_SEC :
                          HYDRAULIC_SOFT_LOCK_RETRY_SEC)) : 0;

    nvs_save_complete_channel_config(channel_id, &watering_channels[channel_id]);
    bt_irrigation_hydraulic_status_notify(channel_id);
}

void watering_hydraulic_clear_channel_lock(uint8_t channel_id)
{
    if (channel_id >= WATERING_CHANNELS_COUNT) {
        return;
    }

    watering_channels[channel_id].hydraulic_lock.level = HYDRAULIC_LOCK_NONE;
    watering_channels[channel_id].hydraulic_lock.reason = HYDRAULIC_LOCK_REASON_NONE;
    watering_channels[channel_id].hydraulic_lock.locked_at_epoch = 0;
    watering_channels[channel_id].hydraulic_lock.retry_after_epoch = 0;

    nvs_save_complete_channel_config(channel_id, &watering_channels[channel_id]);
    bt_irrigation_hydraulic_status_notify(channel_id);
}

void watering_hydraulic_check_retry(void)
{
    uint32_t now_epoch = timezone_get_unix_utc();
    if (now_epoch == 0) {
        return;
    }

    if (global_hydraulic_lock.level == HYDRAULIC_LOCK_SOFT &&
        global_hydraulic_lock.retry_after_epoch > 0 &&
        now_epoch >= global_hydraulic_lock.retry_after_epoch) {
        watering_hydraulic_clear_global_lock();
    }

    for (uint8_t i = 0; i < WATERING_CHANNELS_COUNT; i++) {
        hydraulic_lock_state_t *lock = &watering_channels[i].hydraulic_lock;
        if (lock->level == HYDRAULIC_LOCK_SOFT &&
            lock->retry_after_epoch > 0 &&
            now_epoch >= lock->retry_after_epoch) {
            watering_hydraulic_clear_channel_lock(i);
        }
    }
}

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

    hydraulic_lock_state_t persisted_lock;
    int lock_ret = nvs_load_hydraulic_global_lock(&persisted_lock);
    if (lock_ret >= 0 && persisted_lock.level != HYDRAULIC_LOCK_NONE) {
        uint32_t now_epoch = timezone_get_unix_utc();
        if (persisted_lock.level == HYDRAULIC_LOCK_SOFT &&
            persisted_lock.retry_after_epoch > 0 &&
            now_epoch > 0 &&
            now_epoch >= persisted_lock.retry_after_epoch) {
            nvs_save_hydraulic_global_lock(&global_hydraulic_lock);
        } else {
            global_hydraulic_lock = persisted_lock;
            system_status = WATERING_STATUS_LOCKED;
            bt_irrigation_system_status_update(system_status);
        }
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
    
    /* Initialize onboarding state management */
    int onboarding_err = onboarding_state_init();
    if (onboarding_err != 0) {
        printk("Onboarding state initialization failed: %d - continuing\n", onboarding_err);
    }
    /* Backfill schedule flags from loaded channel configs (auto_enabled) */
    for (int ch = 0; ch < WATERING_CHANNELS_COUNT; ch++) {
        if (watering_channels[ch].watering_event.auto_enabled) {
            onboarding_update_schedule_flag(ch, true);
        }
    }
    
    /* always start flow-monitoring */
    flow_monitor_init();
    
    /* Initialize rain sensor system */
    err = rain_sensor_init();
    if (err != WATERING_SUCCESS) {
        printk("Rain sensor initialization failed: %d - continuing without rain integration\n", err);
    } else {
        printk("Rain sensor initialized successfully\n");
        
        /* Initialize rain integration */
        err = rain_integration_init();
        if (err != WATERING_SUCCESS) {
            printk("Rain integration initialization failed: %d\n", err);
        }
        
        /* Initialize rain history */
        err = rain_history_init();
        if (err != WATERING_SUCCESS) {
            printk("Rain history initialization failed: %d\n", err);
        }
    }
    
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
        event->schedule_type != SCHEDULE_PERIODIC &&
        event->schedule_type != SCHEDULE_AUTO) {
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
    } else if (event->schedule_type == SCHEDULE_PERIODIC) {
        // Periodic must have interval > 0
        if (event->schedule.periodic.interval_days == 0) {
            return WATERING_ERROR_INVALID_PARAM;
        }
    }
    // SCHEDULE_AUTO has no schedule-specific validation here
    // (plant/soil/date requirements checked in watering_channel_auto_mode_valid)

    return WATERING_SUCCESS;
}

/**
 * @brief Check if a channel has valid configuration for AUTO (FAO-56) scheduling mode
 * 
 * AUTO mode requires plant_db_index, soil_db_index, and planting_date_unix to be configured.
 * This function validates that all prerequisites are met before enabling SCHEDULE_AUTO.
 * 
 * @param channel Pointer to the watering channel to validate
 * @return true if channel can use AUTO mode, false if prerequisites are missing
 */
bool watering_channel_auto_mode_valid(const watering_channel_t *channel)
{
    if (!channel) {
        return false;
    }

    /* Plant is valid if either:
     * 1. custom_plant_id > 0 (custom plant from pack storage), or
     * 2. plant_db_index < UINT16_MAX (ROM database plant)
     */
    bool has_custom_plant = (channel->custom_plant_id > 0);
    bool has_rom_plant = (channel->plant_db_index != UINT16_MAX);
    bool missing_plant = !(has_custom_plant || has_rom_plant);
    
    bool missing_soil = (channel->soil_db_index == UINT8_MAX);
    bool missing_date = (channel->planting_date_unix == 0);
    bool missing_coverage = false;

    // Check coverage is configured (need area or plant count for volume calculation)
    if (channel->use_area_based) {
        missing_coverage = (channel->coverage.area_m2 <= 0.0f);
    } else {
        missing_coverage = (channel->coverage.plant_count == 0);
    }

    if (missing_plant || missing_soil || missing_date || missing_coverage) {
        uint8_t channel_id = 0xFF;
        if (channel >= watering_channels &&
            channel < (watering_channels + WATERING_CHANNELS_COUNT)) {
            channel_id = (uint8_t)(channel - watering_channels);
        }

        uint32_t now_ms = k_uptime_get_32();
        if (channel_id < WATERING_CHANNELS_COUNT) {
            static uint32_t last_log_ms[WATERING_CHANNELS_COUNT];
            if (now_ms - last_log_ms[channel_id] > 60000U) {
                LOG_WRN("AUTO config missing ch=%u: plant_rom=%u custom=%u soil=%u date=%u coverage=%u area=%.2f count=%u use_area=%u",
                        channel_id,
                        channel->plant_db_index,
                        channel->custom_plant_id,
                        channel->soil_db_index,
                        channel->planting_date_unix,
                        missing_coverage ? 1U : 0U,
                        (double)channel->coverage.area_m2,
                        channel->coverage.plant_count,
                        channel->use_area_based ? 1U : 0U);
                last_log_ms[channel_id] = now_ms;
            }
        } else {
            static uint32_t last_unknown_log_ms;
            if (now_ms - last_unknown_log_ms > 60000U) {
                LOG_WRN("AUTO config missing for unknown channel: plant_rom=%u custom=%u soil=%u date=%u coverage=%u area=%.2f count=%u use_area=%u",
                        channel->plant_db_index,
                        channel->custom_plant_id,
                        channel->soil_db_index,
                        channel->planting_date_unix,
                        missing_coverage ? 1U : 0U,
                        (double)channel->coverage.area_m2,
                        channel->coverage.plant_count,
                        channel->use_area_based ? 1U : 0U);
                last_unknown_log_ms = now_ms;
            }
        }

        return false;
    }

    return true;
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
    
    // Legacy plant_type assignment removed
    
    // Update onboarding flag
    onboarding_update_channel_flag(channel_id, CHANNEL_FLAG_PLANT_TYPE_SET, true);
    
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
    
    *plant_type = watering_channels[channel_id].plant_info.main_type;
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
    
    // Update onboarding flag
    onboarding_update_channel_flag(channel_id, CHANNEL_FLAG_SOIL_TYPE_SET, true);
    
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
    
    // Update onboarding flag
    onboarding_update_channel_flag(channel_id, CHANNEL_FLAG_IRRIGATION_METHOD_SET, true);
    
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
    
    watering_channels[channel_id].use_area_based = true;
    watering_channels[channel_id].coverage.area_m2 = area_m2;
    
    // Update onboarding flag
    onboarding_update_channel_flag(channel_id, CHANNEL_FLAG_COVERAGE_SET, true);
    
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
    
    watering_channels[channel_id].use_area_based = false;
    watering_channels[channel_id].coverage.plant_count = count;
    
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
    
    // Get coverage information
    coverage->use_area = watering_channels[channel_id].use_area_based;
    if (coverage->use_area) {
        coverage->area.area_m2 = watering_channels[channel_id].coverage.area_m2;
    } else {
        coverage->plants.count = watering_channels[channel_id].coverage.plant_count;
    }
    
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
    
    // Update onboarding flag
    onboarding_update_channel_flag(channel_id, CHANNEL_FLAG_SUN_EXPOSURE_SET, true);
    
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
    
    // Update onboarding flag - water need factor has been set
    onboarding_update_channel_flag(channel_id, CHANNEL_FLAG_WATER_FACTOR_SET, true);
    
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
    // If custom configuration is provided, use it
    if (custom_config != NULL) {
        if (custom_config->custom_name[0] != '\0') {
            // Try to find the specific species in the database
            const plant_full_data_t *plant_data = plant_db_find_species(custom_config->custom_name);
            if (plant_data != NULL) {
                // Use mid-season coefficient as the base water factor
                return plant_db_get_crop_coefficient(plant_data, 1);
            }
        }
        // If species not found, use the custom water factor
        return custom_config->water_need_factor;
    }
    
    // Default factors based on plant type categories
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
            return 1.0f;  // Default factor
            
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
    const soil_enhanced_data_t *soil = fao56_get_channel_soil(channel_id, channel);
    if (!soil) {
        return WATERING_ERROR_INVALID_DATA;
    }
    
    if (plant_type) *plant_type = channel->plant_info.main_type;
    if (soil_type) *soil_type = channel->soil_type;
    if (irrigation_method) *irrigation_method = channel->irrigation_method;
    if (coverage) {
        // Get coverage information
        coverage->use_area = channel->use_area_based;
        if (coverage->use_area) {
            coverage->area.area_m2 = channel->coverage.area_m2;
        } else {
            coverage->plants.count = channel->coverage.plant_count;
        }
    }
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
    // Legacy plant_type assignment removed
    channel->soil_type = soil_type;
    channel->irrigation_method = irrigation_method;
    // Set coverage information
    channel->use_area_based = coverage->use_area;
    if (coverage->use_area) {
        channel->coverage.area_m2 = coverage->area.area_m2;
    } else {
        channel->coverage.plant_count = coverage->plants.count;
    }
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

/* ------------------------------------------------------------------ */
/* Automatic irrigation processing functions                          */
/* ------------------------------------------------------------------ */

/**
 * @brief Check if a channel is configured for automatic irrigation
 */
bool watering_is_automatic_mode(uint8_t channel_id) {
    if (channel_id >= WATERING_CHANNELS_COUNT) {
        return false;
    }
    
    watering_mode_t mode = watering_channels[channel_id].watering_event.watering_mode;
    return (mode == WATERING_AUTOMATIC_QUALITY || mode == WATERING_AUTOMATIC_ECO);
}

/**
 * @brief Process automatic irrigation for a channel based on its mode
 */
watering_error_t watering_process_automatic_irrigation(
    uint8_t channel_id,
    const water_balance_t *balance,
    const irrigation_method_data_t *method,
    const plant_full_data_t *plant,
    irrigation_calculation_t *result
) {
    if (channel_id >= WATERING_CHANNELS_COUNT) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    if (!balance || !method || !result) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    watering_channel_t *channel = &watering_channels[channel_id];
    const soil_enhanced_data_t *soil = fao56_get_channel_soil(channel_id, channel);
    if (!soil) {
        return WATERING_ERROR_INVALID_DATA;
    }
    watering_mode_t mode = channel->watering_event.watering_mode;
    
    // Determine mode name for logging
    const char *mode_name = "Unknown";
    watering_error_t err = WATERING_SUCCESS;
    
    // Get coverage information
    float area_m2 = 0.0f;
    uint16_t plant_count = 0;
    
    if (channel->use_area_based) {
        area_m2 = channel->coverage.area_m2;
    } else {
        plant_count = channel->coverage.plant_count;
    }

    float application_rate_mm_h = 0.0f;
    if (channel->hydraulic.nominal_flow_ml_min > 0) {
        float flow_l_min = channel->hydraulic.nominal_flow_ml_min / 1000.0f;
        float area_for_rate = area_m2;
        if (!channel->use_area_based) {
            float row_spacing_m = 0.0f;
            float plant_spacing_m = 0.0f;
            float area_per_plant = 0.0f;
            if (plant) {
                row_spacing_m = plant->spacing_row_m_x1000 / 1000.0f;
                plant_spacing_m = plant->spacing_plant_m_x1000 / 1000.0f;
            }
            if (row_spacing_m > 0.0f && plant_spacing_m > 0.0f) {
                area_per_plant = row_spacing_m * plant_spacing_m;
            } else if (plant && plant->default_density_plants_m2_x100 > 0) {
                float density = plant->default_density_plants_m2_x100 / 100.0f;
                area_per_plant = (density > 0.0f) ? (1.0f / density) : 0.0f;
            } else {
                area_per_plant = 0.5f;
            }
            area_for_rate = area_per_plant * plant_count;
        }
        if (area_for_rate > 0.0f) {
            application_rate_mm_h = (flow_l_min * 60.0f) / area_for_rate;
        }
    }
    
    // Process based on automatic mode
    switch (mode) {
        case WATERING_AUTOMATIC_QUALITY:
            mode_name = "Quality";
            err = apply_quality_irrigation_mode(balance, method, soil, plant, 
                                              area_m2, plant_count,
                                              application_rate_mm_h,
                                              channel->max_volume_limit_l, result);
            break;
            
        case WATERING_AUTOMATIC_ECO:
            mode_name = "Eco";
            err = apply_eco_irrigation_mode(balance, method, soil, plant,
                                          area_m2, plant_count,
                                          application_rate_mm_h,
                                          channel->max_volume_limit_l, result);
            break;
            
        default:
            LOG_ERR("Channel %d is not in automatic irrigation mode", channel_id);
            return WATERING_ERROR_INVALID_PARAM;
    }
    
    if (err != WATERING_SUCCESS) {
        LOG_ERR("Automatic irrigation calculation failed for channel %d: %d", channel_id, err);
        return err;
    }
    
    // Log the results
    LOG_INF("Channel %d %s mode result: %.1f L (%.2f mm net, %.2f mm gross)",
            channel_id, mode_name, (double)result->volume_liters,
            (double)result->net_irrigation_mm, (double)result->gross_irrigation_mm);
    
    if (result->volume_limited) {
        LOG_WRN("Channel %d volume was limited by max constraint (%.1f L)", 
                channel_id, (double)channel->max_volume_limit_l);
    }
    
    return WATERING_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Maximum volume limit functions                                     */
/* ------------------------------------------------------------------ */

/**
 * @brief Set maximum volume limit for a channel
 * 
 * @param channel_id Channel ID
 * @param max_volume_l Maximum volume limit in liters (0 = no limit)
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_set_max_volume_limit(uint8_t channel_id, float max_volume_l)
{
    if (channel_id >= WATERING_CHANNELS_COUNT) {
        return WATERING_ERROR_INVALID_PARAM;
    }

    if (max_volume_l < 0.0f) {
        LOG_ERR("Invalid maximum volume limit: %.1f", (double)max_volume_l);
        return WATERING_ERROR_INVALID_PARAM;
    }

    watering_channel_t *channel = &watering_channels[channel_id];
    channel->max_volume_limit_l = max_volume_l;

    LOG_INF("Channel %d maximum volume limit set to %.1f L", channel_id, (double)max_volume_l);
    return WATERING_SUCCESS;
}

/**
 * @brief Get maximum volume limit for a channel
 * 
 * @param channel_id Channel ID
 * @param max_volume_l Pointer to store maximum volume limit
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_get_max_volume_limit(uint8_t channel_id, float *max_volume_l)
{
    if (channel_id >= WATERING_CHANNELS_COUNT || max_volume_l == NULL) {
        return WATERING_ERROR_INVALID_PARAM;
    }

    watering_channel_t *channel = &watering_channels[channel_id];
    *max_volume_l = channel->max_volume_limit_l;

    return WATERING_SUCCESS;
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
    // Legacy plant_type assignment removed
    
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
    // Legacy plant_type assignment removed
    
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
    // Legacy plant_type assignment removed
    
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
    // Legacy plant_type assignment removed
    
    watering_save_config();
    printk("Channel %d plant info updated (main type: %d)\n", channel_id, plant_info->main_type);
    return WATERING_SUCCESS;
}

/**
 * @brief Get channel statistics for BLE API
 * 
 * This function retrieves cumulative statistics for a channel by querying
 * the history system for daily statistics over recent periods.
 */
watering_error_t watering_get_channel_statistics(uint8_t channel_id,
                                                uint32_t *total_volume_ml,
                                                uint32_t *last_volume_ml,
                                                uint32_t *watering_count) {
    if (channel_id >= WATERING_CHANNELS_COUNT) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    if (!total_volume_ml || !last_volume_ml || !watering_count) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    // Initialize values
    *total_volume_ml = 0;
    *last_volume_ml = 0;
    *watering_count = 0;
    
    // For now, use basic channel data since history system is not fully implemented
    watering_channel_t *channel;
    watering_error_t err = watering_get_channel(channel_id, &channel);
    if (err != WATERING_SUCCESS) {
        return err;
    }
    
    // Use basic estimates when history system is not available
    // This is a fallback until full history system is implemented
    *total_volume_ml = 0; // No historical data available
    *last_volume_ml = 0;  // No last volume data
    *watering_count = 0;  // No count data
    
    // If we have a recent watering time, we can estimate some activity
    if (channel->last_watering_time > 0) {
        // Estimate based on configured watering parameters
        if (channel->watering_event.watering_mode == WATERING_BY_VOLUME) {
            *last_volume_ml = channel->watering_event.watering.by_volume.volume_liters * 1000; // Convert to ml
        } else {
            // For duration mode, estimate based on typical flow rate
            uint32_t duration_minutes = channel->watering_event.watering.by_duration.duration_minutes;
            *last_volume_ml = duration_minutes * 100; // Estimate 100ml/minute
        }
        *watering_count = 1; // At least one watering happened
        
        // Estimate total volume as multiple of last volume (rough approximation)
        *total_volume_ml = *last_volume_ml * *watering_count;
    }
    
    return WATERING_SUCCESS;
}

/**
 * @brief Update channel statistics after watering event
 * 
 * This function should be called after each watering event to update
 * the statistics tracking system.
 */
watering_error_t watering_update_channel_statistics(uint8_t channel_id,
                                                   uint32_t volume_ml,
                                                   uint32_t timestamp) {
    if (channel_id >= WATERING_CHANNELS_COUNT) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    watering_channel_t *channel;
    watering_error_t err = watering_get_channel(channel_id, &channel);
    if (err != WATERING_SUCCESS) {
        return err;
    }
    
    // Update channel's last watering time
    channel->last_watering_time = timestamp;
    
    // Log the watering event to history system
    watering_history_record_task_complete(channel_id, volume_ml, volume_ml, WATERING_SUCCESS_COMPLETE);
    
    // Save configuration to persist last watering time
    watering_save_config();
    
    return WATERING_SUCCESS;
}

/**
 * @brief Reset channel statistics
 * 
 * This function resets all statistics for a channel.
 */
watering_error_t watering_reset_channel_statistics(uint8_t channel_id) {
    if (channel_id >= WATERING_CHANNELS_COUNT) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    // Reset last watering time
    watering_channel_t *channel;
    watering_error_t err = watering_get_channel(channel_id, &channel);
    if (err != WATERING_SUCCESS) {
        return err;
    }
    
    channel->last_watering_time = 0;
    
    // Clear history for this channel - record a reset event
    watering_history_record_task_error(channel_id, 255); // Use error code 255 to indicate "reset"
    
    // Save configuration
    watering_save_config();
    
    return WATERING_SUCCESS;
}

/**
 * @brief Get the number of completed tasks
 */
int watering_get_completed_tasks_count(void) {
    int count = 0;
    
    if (k_mutex_lock(&completed_tasks_mutex, K_MSEC(100)) == 0) {
        count = completed_tasks_count;
        k_mutex_unlock(&completed_tasks_mutex);
    }
    
    return count;
}

/**
 * @brief Increment the completed tasks counter
 * This function should be called whenever a task completes successfully
 */
void watering_increment_completed_tasks_count(void) {
    if (k_mutex_lock(&completed_tasks_mutex, K_MSEC(100)) == 0) {
        completed_tasks_count++;
        k_mutex_unlock(&completed_tasks_mutex);
        LOG_DBG("Completed tasks count incremented to %d", completed_tasks_count);
    }
}
/* Duplicate function removed - keeping original implementation above */

/* Duplicate functions removed - keeping original implementations above *//**

 * @brief Set the planting date for a channel
 * 
 * @param channel_id Channel ID (0-7)
 * @param planting_date_unix Unix timestamp of when plants were established
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_set_planting_date(uint8_t channel_id, uint32_t planting_date_unix)
{
    if (channel_id >= WATERING_CHANNELS_COUNT) {
        return WATERING_ERROR_INVALID_PARAM;
    }

    watering_channel_t *channel = &watering_channels[channel_id];
    channel->planting_date_unix = planting_date_unix;
    
    // Immediately update days after planting
    watering_update_days_after_planting(channel_id);
    
    // Save configuration
    watering_save_config();
    
    LOG_INF("Channel %d planting date set to %u (Unix timestamp)", channel_id, planting_date_unix);
    return WATERING_SUCCESS;
}

/**
 * @brief Get the planting date for a channel
 * 
 * @param channel_id Channel ID (0-7)
 * @param planting_date_unix Pointer to store the planting date Unix timestamp
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_get_planting_date(uint8_t channel_id, uint32_t *planting_date_unix)
{
    if (channel_id >= WATERING_CHANNELS_COUNT || planting_date_unix == NULL) {
        return WATERING_ERROR_INVALID_PARAM;
    }

    *planting_date_unix = watering_channels[channel_id].planting_date_unix;
    return WATERING_SUCCESS;
}

/**
 * @brief Update days after planting for a channel
 * 
 * This function calculates the current days after planting based on the 
 * planting date and current time. Should be called periodically or when
 * planting date is updated.
 * 
 * @param channel_id Channel ID (0-7)
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_update_days_after_planting(uint8_t channel_id)
{
    if (channel_id >= WATERING_CHANNELS_COUNT) {
        return WATERING_ERROR_INVALID_PARAM;
    }

    watering_channel_t *channel = &watering_channels[channel_id];
    
    // If no planting date is set, days after planting is 0
    if (channel->planting_date_unix == 0) {
        channel->days_after_planting = 0;
        return WATERING_SUCCESS;
    }
    
    // Get current time
    uint32_t current_time = k_uptime_get_32() / 1000; // Convert to seconds
    
    // Calculate days difference
    if (current_time >= channel->planting_date_unix) {
        uint32_t seconds_diff = current_time - channel->planting_date_unix;
        channel->days_after_planting = (uint16_t)(seconds_diff / (24 * 60 * 60)); // Convert to days
    } else {
        // Planting date is in the future - set to 0
        channel->days_after_planting = 0;
    }
    
    LOG_DBG("Channel %d: %d days after planting", channel_id, channel->days_after_planting);
    return WATERING_SUCCESS;
}

/**
 * @brief Get days after planting for a channel
 * 
 * @param channel_id Channel ID (0-7)
 * @param days_after_planting Pointer to store the days after planting
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_get_days_after_planting(uint8_t channel_id, uint16_t *days_after_planting)
{
    if (channel_id >= WATERING_CHANNELS_COUNT || days_after_planting == NULL) {
        return WATERING_ERROR_INVALID_PARAM;
    }

    // Update the calculation first
    watering_error_t err = watering_update_days_after_planting(channel_id);
    if (err != WATERING_SUCCESS) {
        return err;
    }
    
    *days_after_planting = watering_channels[channel_id].days_after_planting;
    return WATERING_SUCCESS;
}

/**
 * @brief Update days after planting for all channels
 * 
 * This function should be called periodically (e.g., daily) to keep the
 * days after planting calculations current for all channels.
 * 
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_update_all_days_after_planting(void)
{
    watering_error_t result = WATERING_SUCCESS;
    
    for (uint8_t i = 0; i < WATERING_CHANNELS_COUNT; i++) {
        watering_error_t err = watering_update_days_after_planting(i);
        if (err != WATERING_SUCCESS) {
            LOG_WRN("Failed to update days after planting for channel %d: %d", i, err);
            result = err; // Keep track of last error, but continue processing
        }
    }
    
    return result;
}/*
*
 * @brief Set the latitude for a channel (for solar radiation calculations)
 * 
 * @param channel_id Channel ID (0-7)
 * @param latitude_deg Latitude in degrees (-90 to +90)
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_set_latitude(uint8_t channel_id, float latitude_deg)
{
    if (channel_id >= WATERING_CHANNELS_COUNT) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    // Validate latitude range
    if (latitude_deg < -90.0f || latitude_deg > 90.0f) {
        LOG_ERR("Invalid latitude: %.2f (must be -90 to +90)", (double)latitude_deg);
        return WATERING_ERROR_INVALID_PARAM;
    }

    watering_channel_t *channel = &watering_channels[channel_id];
    channel->latitude_deg = latitude_deg;
    
    // Save configuration
    watering_save_config();

    /* Mark latitude flag for onboarding */
    onboarding_update_channel_extended_flag(channel_id, CHANNEL_EXT_FLAG_LATITUDE_SET, true);
    
    LOG_INF("Channel %d latitude set to %.2f degrees", channel_id, (double)latitude_deg);
    return WATERING_SUCCESS;
}

/**
 * @brief Get the latitude for a channel
 * 
 * @param channel_id Channel ID (0-7)
 * @param latitude_deg Pointer to store the latitude in degrees
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_get_latitude(uint8_t channel_id, float *latitude_deg)
{
    if (channel_id >= WATERING_CHANNELS_COUNT || latitude_deg == NULL) {
        return WATERING_ERROR_INVALID_PARAM;
    }

    *latitude_deg = watering_channels[channel_id].latitude_deg;
    return WATERING_SUCCESS;
}

/**
 * @brief Set the sun exposure percentage for a channel
 * 
 * @param channel_id Channel ID (0-7)
 * @param sun_exposure_pct Site-specific sun exposure percentage (0-100%)
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_set_sun_exposure(uint8_t channel_id, uint8_t sun_exposure_pct)
{
    if (channel_id >= WATERING_CHANNELS_COUNT) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    // Validate sun exposure range
    if (sun_exposure_pct > 100) {
        LOG_ERR("Invalid sun exposure: %d%% (must be 0-100%%)", sun_exposure_pct);
        return WATERING_ERROR_INVALID_PARAM;
    }

    watering_channel_t *channel = &watering_channels[channel_id];
    channel->sun_exposure_pct = sun_exposure_pct;
    
    // Save configuration
    watering_save_config();
    
    LOG_INF("Channel %d sun exposure set to %d%%", channel_id, sun_exposure_pct);
    return WATERING_SUCCESS;
}

/**
 * @brief Get the sun exposure percentage for a channel
 * 
 * @param channel_id Channel ID (0-7)
 * @param sun_exposure_pct Pointer to store the sun exposure percentage
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_get_sun_exposure(uint8_t channel_id, uint8_t *sun_exposure_pct)
{
    if (channel_id >= WATERING_CHANNELS_COUNT || sun_exposure_pct == NULL) {
        return WATERING_ERROR_INVALID_PARAM;
    }

    *sun_exposure_pct = watering_channels[channel_id].sun_exposure_pct;
    return WATERING_SUCCESS;
}

/**
 * @brief Set comprehensive environmental configuration for a channel
 * 
 * @param channel_id Channel ID (0-7)
 * @param latitude_deg Latitude in degrees (-90 to +90)
 * @param sun_exposure_pct Site-specific sun exposure percentage (0-100%)
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_set_environmental_config(uint8_t channel_id, 
                                                  float latitude_deg, 
                                                  uint8_t sun_exposure_pct)
{
    if (channel_id >= WATERING_CHANNELS_COUNT) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    // Validate parameters
    if (latitude_deg < -90.0f || latitude_deg > 90.0f) {
        LOG_ERR("Invalid latitude: %.2f (must be -90 to +90)", (double)latitude_deg);
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    if (sun_exposure_pct > 100) {
        LOG_ERR("Invalid sun exposure: %d%% (must be 0-100%%)", sun_exposure_pct);
        return WATERING_ERROR_INVALID_PARAM;
    }

    watering_channel_t *channel = &watering_channels[channel_id];
    channel->latitude_deg = latitude_deg;
    channel->sun_exposure_pct = sun_exposure_pct;
    
    // Save configuration
    watering_save_config();
    
    LOG_INF("Channel %d environmental config: latitude=%.2f°, sun exposure=%d%%", 
            channel_id, (double)latitude_deg, sun_exposure_pct);
    return WATERING_SUCCESS;
}

/**
 * @brief Get comprehensive environmental configuration for a channel
 * 
 * @param channel_id Channel ID (0-7)
 * @param latitude_deg Pointer to store latitude in degrees (can be NULL)
 * @param sun_exposure_pct Pointer to store sun exposure percentage (can be NULL)
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_get_environmental_config(uint8_t channel_id, 
                                                  float *latitude_deg, 
                                                  uint8_t *sun_exposure_pct)
{
    if (channel_id >= WATERING_CHANNELS_COUNT) {
        return WATERING_ERROR_INVALID_PARAM;
    }

    watering_channel_t *channel = &watering_channels[channel_id];
    
    if (latitude_deg != NULL) {
        *latitude_deg = channel->latitude_deg;
    }
    
    if (sun_exposure_pct != NULL) {
        *sun_exposure_pct = channel->sun_exposure_pct;
    }
    
    return WATERING_SUCCESS;
}/**
 * @
brief Run automatic irrigation calculations for all channels
 * 
 * This function processes all channels configured for automatic irrigation
 * and schedules irrigation tasks based on FAO-56 calculations.
 * 
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_run_automatic_calculations(void)
{
    watering_error_t result = WATERING_SUCCESS;
    uint8_t processed_channels = 0;
    
    LOG_DBG("Running automatic irrigation calculations");
    
    // Check if system is in a state that allows automatic calculations
    if (system_status == WATERING_STATUS_FAULT ||
        system_status == WATERING_STATUS_RTC_ERROR ||
        system_status == WATERING_STATUS_LOCKED) {
        LOG_WRN("Skipping automatic calculations due to system status: %d", system_status);
        return WATERING_ERROR_BUSY;
    }
    
    // Process each channel
    for (uint8_t channel_id = 0; channel_id < WATERING_CHANNELS_COUNT; channel_id++) {
        watering_channel_t *channel = &watering_channels[channel_id];
        
        // Skip channels not in automatic mode
        if (channel->watering_event.watering_mode != WATERING_AUTOMATIC_QUALITY &&
            channel->watering_event.watering_mode != WATERING_AUTOMATIC_ECO) {
            continue;
        }
        
        // Skip if channel has an active task
        if (watering_task_state.current_active_task != NULL &&
            watering_task_state.current_active_task->channel == channel) {
            LOG_DBG("Channel %d has active task, skipping automatic calculation", channel_id);
            continue;
        }
        
        // Update days after planting
        watering_error_t err = watering_update_days_after_planting(channel_id);
        if (err != WATERING_SUCCESS) {
            LOG_WRN("Failed to update days after planting for channel %d: %d", channel_id, err);
            result = err;
            continue;
        }
        
        // Get environmental data
        environmental_data_t env_data;
        err = env_sensors_read(&env_data);
        if (err != WATERING_SUCCESS) {
            LOG_WRN("Failed to read environmental data for channel %d: %d", channel_id, err);
            // Continue with fallback data
            memset(&env_data, 0, sizeof(env_data));
            env_data.air_temp_mean_c = 20.0f;  // Default temperature
            env_data.rel_humidity_pct = 60.0f; // Default humidity
            env_data.temp_valid = false;
            env_data.humidity_valid = false;
        }
        
        // Perform FAO-56 calculations
        irrigation_calculation_t calc_result;
        err = fao56_calculate_irrigation_requirement(channel_id, &env_data, &calc_result);
        if (err != WATERING_SUCCESS) {
            LOG_WRN("FAO-56 calculation failed for channel %d: %d", channel_id, err);
            result = err;
            continue;
        }
        
        // Check if irrigation is needed
        if (calc_result.volume_liters <= 0.1f) {
            LOG_DBG("Channel %d: No irrigation needed (calculated volume: %.3f L)", 
                    channel_id, (double)calc_result.volume_liters);
            continue;
        }
        
        // Create irrigation task
        watering_task_t auto_task;
        memset(&auto_task, 0, sizeof(auto_task));
        
        auto_task.channel = channel;
        auto_task.trigger_type = WATERING_TRIGGER_SCHEDULED; // Automatic calculations are scheduled

        /* Temporarily override scheduling parameters for this run */
        watering_snapshot_event(channel_id);
        channel->watering_event.watering_mode = WATERING_BY_VOLUME;
        auto_task.by_volume.volume_liters = (uint16_t)(calc_result.volume_liters + 0.5f); // Round to nearest liter
        
        // Ensure minimum volume
        if (auto_task.by_volume.volume_liters < 1) {
            auto_task.by_volume.volume_liters = 1;
        }

        channel->watering_event.watering.by_volume.volume_liters = auto_task.by_volume.volume_liters;
        
        // Add task to queue
        err = watering_add_task(&auto_task);
        if (err != WATERING_SUCCESS) {
            watering_restore_event(channel_id);
            LOG_ERR("Failed to add automatic irrigation task for channel %d: %d", channel_id, err);
            result = err;
            continue;
        }
        
        const char *auto_mode_str = "Automatic";
        if (channel->auto_mode == WATERING_AUTOMATIC_QUALITY) {
            auto_mode_str = "Quality";
        } else if (channel->auto_mode == WATERING_AUTOMATIC_ECO) {
            auto_mode_str = "Eco";
        }

        LOG_INF("Channel %d: Scheduled automatic irrigation (%.1f L, %s mode)",
                channel_id,
                (double)calc_result.volume_liters,
                auto_mode_str);
        
        processed_channels++;
    }
    
    if (processed_channels > 0) {
        LOG_INF("Automatic calculations complete: %d channels processed", processed_channels);
        
        // Notify BLE clients of automatic calculation completion
        bt_irrigation_auto_calc_status_notify();
    } else {
        LOG_DBG("No channels required automatic irrigation");
    }
    
    return result;
}

/**
 * @brief Set automatic calculation interval
 * 
 * @param interval_hours Interval between automatic calculations in hours (1-24)
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_set_auto_calc_interval(uint8_t interval_hours)
{
    if (interval_hours < 1 || interval_hours > 24) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    // Update the interval in watering_tasks.c
    extern uint32_t auto_calc_interval_ms;
    auto_calc_interval_ms = interval_hours * 3600000; // Convert to milliseconds
    
    LOG_INF("Automatic calculation interval set to %d hours", interval_hours);
    return WATERING_SUCCESS;
}

/**
 * @brief Enable or disable automatic calculations
 * 
 * @param enabled True to enable, false to disable
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_set_auto_calc_enabled(bool enabled)
{
    // Update the flag in watering_tasks.c
    extern bool auto_calc_enabled;
    auto_calc_enabled = enabled;
    
    LOG_INF("Automatic calculations %s", enabled ? "enabled" : "disabled");
    return WATERING_SUCCESS;
}

/**
 * @brief Get automatic calculation status
 * 
 * @param enabled Pointer to store enabled status
 * @param interval_hours Pointer to store interval in hours
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_get_auto_calc_status(bool *enabled, uint8_t *interval_hours)
{
    if (enabled == NULL || interval_hours == NULL) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    extern bool auto_calc_enabled;
    extern uint32_t auto_calc_interval_ms;
    
    *enabled = auto_calc_enabled;
    *interval_hours = (uint8_t)(auto_calc_interval_ms / 3600000);
    
    return WATERING_SUCCESS;
}/**

 * @brief Get comprehensive system status including rain sensor information
 * 
 * @param status_buffer Buffer to store status information
 * @param buffer_size Size of the status buffer
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_get_system_status_detailed(char *status_buffer, uint16_t buffer_size)
{
    if (!status_buffer || buffer_size < 200) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    int written = 0;
    
    /* Basic system status */
    watering_status_t status;
    watering_error_t ret = watering_get_status(&status);
    if (ret == WATERING_SUCCESS) {
        const char *status_str = "Unknown";
        switch (status) {
            case WATERING_STATUS_OK: status_str = "OK"; break;
            case WATERING_STATUS_FAULT: status_str = "FAULT"; break;
            case WATERING_STATUS_NO_FLOW: status_str = "NO_FLOW"; break;
            case WATERING_STATUS_UNEXPECTED_FLOW: status_str = "UNEXPECTED_FLOW"; break;
            case WATERING_STATUS_RTC_ERROR: status_str = "RTC_ERROR"; break;
            case WATERING_STATUS_LOW_POWER: status_str = "LOW_POWER"; break;
            case WATERING_STATUS_LOCKED: status_str = "LOCKED"; break;
        }
        written += snprintf(status_buffer + written, buffer_size - written,
                           "System Status: %s\n", status_str);
    }
    
    /* Current task status */
    watering_task_t *current_task = watering_get_current_task();
    if (current_task) {
        uint8_t channel_id = current_task->channel - watering_channels;
        written += snprintf(status_buffer + written, buffer_size - written,
                           "Active Task: Channel %d (%s)\n", 
                           channel_id + 1, current_task->channel->name);
    } else {
        written += snprintf(status_buffer + written, buffer_size - written,
                           "Active Task: None\n");
    }
    
    /* Queue status */
    uint8_t pending_count;
    bool queue_active;
    ret = watering_get_queue_status(&pending_count, &queue_active);
    if (ret == WATERING_SUCCESS) {
        written += snprintf(status_buffer + written, buffer_size - written,
                           "Task Queue: %d pending\n", pending_count);
    }
    
    /* Rain sensor status */
    if (rain_sensor_is_active()) {
        float recent_rainfall = rain_history_get_last_24h();
        bool integration_enabled = rain_integration_is_enabled();
    written += snprintf(status_buffer + written, buffer_size - written,
               "Rain Sensor: Active (%.2fmm/24h, integration %s)\n",
               (double)recent_rainfall, integration_enabled ? "ON" : "OFF");
    } else {
        written += snprintf(status_buffer + written, buffer_size - written,
                           "Rain Sensor: Inactive/Error\n");
    }
    
    /* Flow sensor status */
    uint32_t flow_pulses = get_pulse_count();
    written += snprintf(status_buffer + written, buffer_size - written,
                       "Flow Sensor: %u pulses\n", flow_pulses);
    
    /* Power mode */
    const char *power_mode_str = "Unknown";
    switch (current_power_mode) {
        case POWER_MODE_NORMAL: power_mode_str = "Normal"; break;
        case POWER_MODE_ENERGY_SAVING: power_mode_str = "Energy Saving"; break;
        case POWER_MODE_ULTRA_LOW_POWER: power_mode_str = "Ultra Low Power"; break;
    }
    written += snprintf(status_buffer + written, buffer_size - written,
                       "Power Mode: %s\n", power_mode_str);
    
    return WATERING_SUCCESS;
}

/**
 * @brief Get rain sensor integration status for monitoring
 * 
 * @param integration_status Buffer to store integration status
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_get_rain_integration_status(rain_integration_status_t *integration_status)
{
    if (!integration_status) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    memset(integration_status, 0, sizeof(rain_integration_status_t));
    
    /* Rain sensor status */
    integration_status->sensor_active = rain_sensor_is_active();
    integration_status->integration_enabled = rain_integration_is_enabled();
    
    if (integration_status->sensor_active) {
        integration_status->last_pulse_time = rain_sensor_get_last_pulse_time();
        integration_status->calibration_mm_per_pulse = rain_sensor_get_calibration();
    }
    
    /* Recent rainfall data */
    integration_status->rainfall_last_hour = rain_history_get_current_hour();
    integration_status->rainfall_last_24h = rain_history_get_last_24h();
    integration_status->rainfall_last_48h = rain_history_get_recent_total(48);
    
    /* Integration configuration */
    if (integration_status->integration_enabled) {
        integration_status->sensitivity_pct = rain_integration_get_sensitivity();
        integration_status->skip_threshold_mm = rain_integration_get_skip_threshold();
        
        /* Calculate impact for each channel */
        for (int i = 0; i < WATERING_CHANNELS_COUNT; i++) {
            rain_irrigation_impact_t impact = rain_integration_calculate_impact(i);
            integration_status->channel_reduction_pct[i] = impact.irrigation_reduction_pct;
            integration_status->channel_skip_irrigation[i] = impact.skip_irrigation;
        }
    }
    
    /* History statistics */
    rain_history_stats_t stats;
    watering_error_t ret = rain_history_get_stats(&stats);
    if (ret == WATERING_SUCCESS) {
        integration_status->hourly_entries = stats.hourly_entries;
        integration_status->daily_entries = stats.daily_entries;
        integration_status->storage_usage_bytes = stats.total_storage_bytes;
    }
    
    return WATERING_SUCCESS;
}
