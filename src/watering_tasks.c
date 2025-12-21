#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/pm/pm.h>
#include <zephyr/logging/log.h>
#include "flow_sensor.h"
#include "watering.h"
#include "watering_internal.h"
#include "rtc.h"
#include "timezone.h"               /* Add timezone support for local time scheduling */
#include "bt_irrigation_service.h"   /* NEW */
#include "watering_history.h"        /* Add history integration */
#include "rain_integration.h"       /* Rain sensor integration */
#include "rain_sensor.h"            /* Rain sensor status */
#include "rain_history.h"           /* Rain history data */
#include "interval_task_integration.h" /* Interval mode integration */
#include "environmental_data.h"     /* Temperature source for anti-freeze */
#include "environmental_history.h"  /* Environmental history aggregation */
#include "bme280_driver.h"          /* Direct BME reads for freeze lockout */
#include "fao56_calc.h"             /* FAO-56 calculations for AUTO mode */

LOG_MODULE_DECLARE(watering, CONFIG_LOG_DEFAULT_LEVEL);

/* --- Anti-freeze safety cutoff configuration -------------------------- */
#define FREEZE_LOCK_TEMP_C        2.0f   /* block tasks at/under this temp */
#define FREEZE_CLEAR_TEMP_C       4.0f   /* require this temp to clear lock */
#define FREEZE_DATA_MAX_AGE_MS    (10 * 60 * 1000) /* 10 minutes stale window */
#define FREEZE_ALARM_CODE         3      /* BLE alarm code for freeze */
#define ALARM_REFRESH_MS          60000  /* resend active alarms every 60s */

/**
 * @file watering_tasks.c
 * @brief Implementation of watering task management and scheduling
 * 
 * This file manages the execution of watering tasks including scheduling,
 * prioritization, and flow monitoring.
 * 
 * PERFORMANCE IMPROVEMENTS:
 * - Replaced K_FOREVER mutex locks with timeouts to prevent system freezes
 * - Optimized mutex acquisition for responsive channel switching
 * - Added graceful degradation when mutexes are busy
 */

/** Message queue for pending watering tasks */
K_MSGQ_DEFINE(watering_tasks_queue, sizeof(watering_task_t), 10, 4);

/** Current state of task execution */
struct watering_task_state_t watering_task_state = {NULL, 0, false, false, 0, 0};

/** Global state of last completed task for BLE reporting */
struct last_completed_task_t last_completed_task = {NULL, 0, 0, false};

/** Error task tracking */
static uint16_t error_task_count = 0;

/** Flow pulse count at task start */
uint32_t initial_pulse_count = 0;        /* was static, now global */

/**
 * @brief Task execution state enumeration
 */
typedef enum {
    W_TASK_STATE_IDLE,      /**< No active task */
    W_TASK_STATE_RUNNING,   /**< Task is currently running */
    W_TASK_STATE_COMPLETED  /**< Task has completed but not cleaned up */
} task_state_t;

/** Current state of task execution system */
static task_state_t current_task_state = W_TASK_STATE_IDLE;

/** Stack sizes for watering threads */
#define WATERING_STACK_SIZE 4096
#define SCHEDULER_STACK_SIZE 4096

/** Thread stacks */
K_THREAD_STACK_DEFINE(watering_task_stack, WATERING_STACK_SIZE);
K_THREAD_STACK_DEFINE(scheduler_task_stack, SCHEDULER_STACK_SIZE);

/** Thread control structures */
static struct k_thread watering_task_data;
static struct k_thread scheduler_task_data;

/** Flag to indicate if task threads are running */
static bool watering_tasks_running = false;

/** Flag to signal threads to exit */
static bool exit_tasks = false;

/** Anti-freeze state */
static bool freeze_lockout_active = false;
static uint32_t last_freeze_alarm_ms = 0;

/* Emit BLE/system alarm with optional log */
static void raise_alarm(uint8_t alarm_code, uint16_t alarm_data, const char *logmsg)
{
    if (logmsg) {
        printk("%s\n", logmsg);
    }
#ifdef CONFIG_BT
    bt_irrigation_alarm_notify(alarm_code, alarm_data);
#endif
}

/* Update system status and notify BLE */
static void update_system_status(watering_status_t status)
{
    system_status = status;
#ifdef CONFIG_BT
    bt_irrigation_system_status_update(system_status);
#endif
}

/* Check temperature and enforce anti-freeze lockout */
static bool check_freeze_lockout(float *temperature_out)
{
    bme280_environmental_data_t env = {0};
    int ret = environmental_data_get_current(&env);
    uint32_t now = k_uptime_get_32();

    bool data_valid = (ret == 0) && env.current.valid;
    uint32_t last_ts = env.current.timestamp ? env.current.timestamp : env.last_update;
    bool stale = data_valid && (now - last_ts > FREEZE_DATA_MAX_AGE_MS);

    /* If data is missing/stale, try an on-demand BME280 read to refresh */
    if (!data_valid || stale) {
        bme280_reading_t fresh = {0};
        int bme_ret = bme280_system_read_data(&fresh);
        if (bme_ret == -EAGAIN) {
            bme280_system_trigger_measurement();
            k_msleep(120);
            bme_ret = bme280_system_read_data(&fresh);
        }
        if (bme_ret == 0 && fresh.valid) {
            /* Feed the environmental processor with the new reading */
            environmental_data_process_bme280_reading(&fresh);
            environmental_data_get_current(&env);
            data_valid = env.current.valid;
            last_ts = env.current.timestamp ? env.current.timestamp : env.last_update;
            stale = data_valid && (now - last_ts > FREEZE_DATA_MAX_AGE_MS);
        } else if (bme_ret == -ENODEV || bme_ret == -EACCES || bme_ret == -EBUSY) {
            /* Sensor unavailable: fail open with a conservative warm default to avoid permanent lockout */
            env.current.valid = true;
            env.current.temperature = FREEZE_CLEAR_TEMP_C + 1.0f; /* 1C above clear threshold */
            env.current.timestamp = now;
            data_valid = true;
            stale = false;
        }
    }

    /* If data is stale but warm, allow tasks (warn instead of hard lockout) */
    if (!data_valid || stale) {
        float temp_c_stale = env.current.temperature;
        bool warm_enough = data_valid && !isnan(temp_c_stale) && (temp_c_stale >= FREEZE_CLEAR_TEMP_C);
        if (warm_enough) {
            /* Clear any existing lockout if stale-but-warm */
            if (freeze_lockout_active && system_status == WATERING_STATUS_FREEZE_LOCKOUT) {
                update_system_status(WATERING_STATUS_OK);
            }
            freeze_lockout_active = false;
            /* Keep going to allow task creation without returning early */
        } else {
            if (!freeze_lockout_active || (now - last_freeze_alarm_ms) > ALARM_REFRESH_MS) {
                raise_alarm(FREEZE_ALARM_CODE, 0xFFFF, "ALERT: Environmental data unavailable/stale - freeze lockout");
                last_freeze_alarm_ms = now;
            }
            freeze_lockout_active = true;
            if (system_status != WATERING_STATUS_FAULT) {
                update_system_status(WATERING_STATUS_FREEZE_LOCKOUT);
            }
            return true;
        }
    }

    float temp_c = env.current.temperature;
    if (temperature_out) {
        *temperature_out = temp_c;
    }

    if (temp_c <= FREEZE_LOCK_TEMP_C) {
        freeze_lockout_active = true;
        if (!last_freeze_alarm_ms || (now - last_freeze_alarm_ms) > ALARM_REFRESH_MS) {
            raise_alarm(FREEZE_ALARM_CODE, (int16_t)(temp_c * 10), "ALERT: Temperature below freeze cutoff");
            last_freeze_alarm_ms = now;
        }
        if (system_status != WATERING_STATUS_FAULT) {
            update_system_status(WATERING_STATUS_FREEZE_LOCKOUT);
        }
        return true;
    }

    /* Clear lockout when safe temperature is sustained */
    if (freeze_lockout_active && temp_c >= FREEZE_CLEAR_TEMP_C) {
        freeze_lockout_active = false;
        if (system_status == WATERING_STATUS_FREEZE_LOCKOUT) {
            update_system_status(WATERING_STATUS_OK);
        }
        raise_alarm(FREEZE_ALARM_CODE, 0, "INFO: Freeze lockout cleared");
    }

    return freeze_lockout_active;
}

/** Mutex for protecting task state */
K_MUTEX_DEFINE(watering_state_mutex);

/** Current time tracking for scheduler */
static uint8_t current_hour = 0;
static uint8_t current_minute = 0;
static uint8_t current_day_of_week = 0;
uint16_t days_since_start = 0;  // Changed from static to global

/** Last read time to detect day changes */
static uint8_t last_day = 0;

/** AUTO mode julian day tracking (prevents multiple runs per day) */
static uint16_t current_julian_day = 0;

/** Automatic calculation scheduling */
static uint32_t last_auto_calc_time = 0;
uint32_t auto_calc_interval_ms = 3600000; // Default: 1 hour (non-static for external access)
bool auto_calc_enabled = true; // Non-static for external access

/** RTC error count for tracking reliability */
static uint8_t rtc_error_count = 0;
#define MAX_RTC_ERRORS 5

/** System time tracking for RTC fallback */
static uint32_t last_time_update = 0;

/* ------------------------------------------------------------------ */
/* Static storage for the currently running task (avoids dangling ptr) */
static watering_task_t active_task_storage;          /* NEW */
/* ------------------------------------------------------------------ */
static bool get_channel_id_safe(const watering_channel_t *channel, uint8_t *channel_id_out)
{
    if (!channel || !channel_id_out) {
        return false;
    }

    uintptr_t base = (uintptr_t)watering_channels;
    uintptr_t end = (uintptr_t)(watering_channels + WATERING_CHANNELS_COUNT);
    uintptr_t ptr = (uintptr_t)channel;

    if (ptr < base || ptr >= end) {
        return false;
    }

    size_t offset = (size_t)(ptr - base);
    if (offset % sizeof(watering_channel_t) != 0) {
        return false;
    }

    uint8_t id = (uint8_t)(offset / sizeof(watering_channel_t));
    if (id >= WATERING_CHANNELS_COUNT) {
        return false;
    }

    *channel_id_out = id;
    return true;
}

/**
 * @brief Helper function to convert watering error codes to standard error codes
 */
static inline int watering_to_system_error(watering_error_t err) {
    // The watering error codes are already negative, so we just return the value
    return err;
}

/**
 * @brief Initialize the task management system
 */
watering_error_t tasks_init(void) {
    watering_task_state.current_active_task = NULL;
    watering_task_state.watering_start_time = 0;
    watering_task_state.task_in_progress = false;
    watering_task_state.task_paused = false;
    watering_task_state.pause_start_time = 0;
    watering_task_state.total_paused_time = 0;
    
    // Initialize last completed task state
    last_completed_task.task = NULL;
    last_completed_task.start_time = 0;
    last_completed_task.completion_time = 0;
    last_completed_task.valid = false;
    
    current_task_state = W_TASK_STATE_IDLE;
    watering_tasks_running = false;
    exit_tasks = false;
    rtc_error_count = 0;
    
    /* Initialize interval task integration */
    interval_task_integration_init();

    return WATERING_SUCCESS;
}

/**
 * @brief Add a watering task to the task queue
 * 
 * @param task Task to be added to the queue
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_add_task(watering_task_t *task) {
    if (task == NULL || task->channel == NULL) {
        return WATERING_ERROR_INVALID_PARAM;
    }

    uint8_t channel_id = task->channel - watering_channels;
    if (channel_id >= WATERING_CHANNELS_COUNT) {
        return WATERING_ERROR_INVALID_PARAM;
    }

    /* Anti-freeze safety: block enqueue when too cold or data unavailable */
    float temp_c = 0.0f;
    if (check_freeze_lockout(&temp_c)) {
        printk("Skipping task enqueue for channel %u due to freeze lockout (T=%.1fC)\n",
               channel_id, (double)temp_c);
#ifdef CONFIG_BT
        watering_history_record_task_skip(channel_id, WATERING_SKIP_REASON_FREEZE, temp_c);
#endif
        return WATERING_ERROR_BUSY;
    }
    
    // Validate watering mode parameters
    if (task->channel->watering_event.watering_mode == WATERING_BY_VOLUME) {
        if (task->by_volume.volume_liters == 0) {
            return WATERING_ERROR_INVALID_PARAM;
        }
    }
    
    /* Rain integration check - skip irrigation if recent rainfall is significant */
    if (rain_integration_is_enabled() && rain_integration_should_skip_irrigation(channel_id)) {
    float recent_rainfall = rain_history_get_last_24h();
    printk("Skipping irrigation for channel %s due to recent rainfall (%.2f mm)\n", 
           task->channel->name, (double)recent_rainfall);
        
        /* Log the skipped task for history tracking */
        #ifdef CONFIG_BT
        watering_history_record_task_skip(channel_id, WATERING_SKIP_REASON_RAIN, recent_rainfall);
        #endif
        watering_restore_event(channel_id);
        return WATERING_ERROR_BUSY; /* Use busy to indicate task was skipped */
    }
    
    if (k_msgq_put(&watering_tasks_queue, task, K_NO_WAIT) != 0) {
        LOG_ERROR("Watering queue is full", WATERING_ERROR_QUEUE_FULL);
        watering_increment_error_tasks();  /* Track queue overflow errors */
        watering_restore_event(channel_id);
        return WATERING_ERROR_QUEUE_FULL;
    }
    
    // Notify master valve about upcoming task for intelligent scheduling
    uint32_t task_start_time = k_uptime_get_32() + 1000; // Assuming task starts soon (1 second)
    master_valve_notify_upcoming_task(task_start_time);
    
    /* BLE notify – 0xFF ⇒ calculează intern */
    bt_irrigation_queue_status_update(0xFF);
    
    printk("Added watering task for channel %s\n", task->channel->name);
    return WATERING_SUCCESS;
}

/**
 * @brief Process the next task in the queue
 * 
 * @return 1 if task was processed, 0 if no tasks, negative error code on failure
 */
int watering_process_next_task(void) {
    if (!system_initialized) {
        return -WATERING_ERROR_NOT_INITIALIZED;
    }
    
    if (system_status == WATERING_STATUS_FAULT) {
        return -WATERING_ERROR_BUSY;
    }
    
    watering_task_t task;
    if (k_msgq_get(&watering_tasks_queue, &task, K_NO_WAIT) != 0) {
        return 0;  // No tasks in queue
    }

    if (task.channel == NULL) {
        return -WATERING_ERROR_INVALID_PARAM;
    }

    /* NEW: start task via helper so state & timers are set ------------------ */
    watering_error_t ret = watering_start_task(&task);
    if (ret != WATERING_SUCCESS) {
        LOG_ERROR("Failed to start watering task", ret);
        return -ret;
    }

    return 1;   /* one task consumed and started */
}

/**
 * @brief Start execution of a watering task
 * 
 * @param task Task to be executed
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_start_task(watering_task_t *task)
{
    if (task == NULL || task->channel == NULL) {
        return WATERING_ERROR_INVALID_PARAM;
    }

    /* Copy the task to our permanent storage BEFORE opening the valve   */
    active_task_storage = *task;                     /* NEW – deep copy  */
    task = &active_task_storage;                     /* point to storage */

    uint8_t channel_id = task->channel - watering_channels;
    if (channel_id >= WATERING_CHANNELS_COUNT) {
        return WATERING_ERROR_INVALID_PARAM;
    }

    /* Re-check anti-freeze just before start */
    float temp_c = 0.0f;
    if (check_freeze_lockout(&temp_c)) {
        printk("Blocked watering start for channel %u due to freeze lockout (T=%.1fC)\n",
               channel_id, (double)temp_c);
#ifdef CONFIG_BT
        watering_history_record_task_skip(channel_id, WATERING_SKIP_REASON_FREEZE, temp_c);
#endif
        return WATERING_ERROR_BUSY;
    }
    
    /* Apply rain-based adjustments to the task before starting */
    if (rain_integration_is_enabled()) {
        rain_irrigation_impact_t impact = rain_integration_calculate_impact(channel_id);
        
        /* Log rain impact information */
    printk("Rain impact for channel %d: %.2f mm recent, %.1f%% reduction, skip=%s\n",
           channel_id, (double)impact.recent_rainfall_mm, (double)impact.irrigation_reduction_pct,
               impact.skip_irrigation ? "yes" : "no");
        
        /* Skip irrigation if threshold exceeded (double-check) */
        if (impact.skip_irrigation) {
            printk("Skipping irrigation for channel %d due to rain threshold\n", channel_id);
            watering_restore_event(channel_id);
            return WATERING_ERROR_BUSY;
        }
        
        /* Apply reduction to task parameters */
        if (impact.irrigation_reduction_pct > 0.0f) {
            float reduction_factor = 1.0f - (impact.irrigation_reduction_pct / 100.0f);
            watering_snapshot_event(channel_id);
            
            if (task->channel->watering_event.watering_mode == WATERING_BY_DURATION) {
                uint32_t original_duration = task->channel->watering_event.watering.by_duration.duration_minutes;
                uint32_t adjusted_duration = (uint32_t)(original_duration * reduction_factor);
                
                /* Ensure minimum duration of 1 minute */
                if (adjusted_duration < 1) {
                    adjusted_duration = 1;
                }
                
                task->channel->watering_event.watering.by_duration.duration_minutes = adjusted_duration;
                
          printk("Rain-adjusted duration for channel %d: %u -> %u minutes (%.1f%% reduction)\n",
              channel_id, original_duration, adjusted_duration, (double)impact.irrigation_reduction_pct);
                       
            } else if (task->channel->watering_event.watering_mode == WATERING_BY_VOLUME) {
                uint32_t original_volume = task->by_volume.volume_liters;
                uint32_t adjusted_volume = (uint32_t)(original_volume * reduction_factor);
                
                /* Ensure minimum volume of 100ml */
                if (adjusted_volume < 1) {
                    adjusted_volume = 1;
                }
                
                task->by_volume.volume_liters = adjusted_volume;
                
          printk("Rain-adjusted volume for channel %d: %u -> %u liters (%.1f%% reduction)\n",
              channel_id, original_volume, adjusted_volume, (double)impact.irrigation_reduction_pct);
            }
        }
    }
    
    /* Initialize interval task system for this task */
    int interval_ret = interval_task_start(task);
    if (interval_ret != 0) {
        LOG_WRN("Failed to start interval task: %d", interval_ret);
        // Continue with standard watering as fallback
    }

    watering_error_t ret = watering_channel_on(channel_id);
    if (ret != WATERING_SUCCESS) {
        LOG_ERROR("Error activating channel for task", ret);
        watering_restore_event(channel_id);
        return ret;
    }
    
    // Use timeout instead of K_FOREVER to prevent system freeze
    if (k_mutex_lock(&watering_state_mutex, K_MSEC(100)) != 0) {
        printk("Failed to get mutex for task start - system busy\n");
        watering_restore_event(channel_id);
        return WATERING_ERROR_BUSY;
    }

    /* --------- NEW: baseline for flow detection ---------------- */
    reset_pulse_count();           /* always start from 0                */
    initial_pulse_count = 0;       /* value after reset – kept for ref   */

    // Clear any previous completed task when starting a new one
    last_completed_task.valid = false;
    
    watering_task_state.watering_start_time = k_uptime_get_32();
    watering_task_state.task_in_progress = true;
    
    printk("TASK DEBUG: watering_start_time set to %u\n", watering_task_state.watering_start_time);
    
    if (task->channel->watering_event.watering_mode == WATERING_BY_VOLUME) {
        reset_pulse_count();
        initial_pulse_count = 0;
        printk("Started volumetric watering for channel %d: %d liters\n", 
               channel_id + 1, task->by_volume.volume_liters);
    } else {
        printk("Started timed watering for channel %d: %d minutes\n", 
               channel_id + 1, task->channel->watering_event.watering.by_duration.duration_minutes);
    }
    
    watering_task_state.current_active_task = task;
    current_task_state = W_TASK_STATE_RUNNING;
    
    k_mutex_unlock(&watering_state_mutex);
    
    /* Record task start in history */
    #ifdef CONFIG_BT
    watering_mode_t mode = task->channel->watering_event.watering_mode;
    uint16_t target_value;
    if (mode == WATERING_BY_DURATION) {
        target_value = task->channel->watering_event.watering.by_duration.duration_minutes;
    } else {
        target_value = task->channel->watering_event.watering.by_volume.volume_liters;
    }
    
    // Record in history system
    printk("Recording task start in history: channel=%d, mode=%d, target=%d, trigger=%d\n", 
           channel_id, mode, target_value, task->trigger_type);
    watering_history_record_task_start(channel_id, mode, target_value, task->trigger_type);
    
    // Notify BLE clients about new history event
    bt_irrigation_history_notify_event(channel_id, WATERING_EVENT_START, 
                                      watering_task_state.watering_start_time / 1000, 0);
    #endif
    
    /* Notify BLE clients about task start */
    #ifdef CONFIG_BT
    bt_irrigation_current_task_update(channel_id, 
                                     watering_task_state.watering_start_time / 1000,
                                     (uint8_t)task->channel->watering_event.watering_mode,
                                     (task->channel->watering_event.watering_mode == WATERING_BY_DURATION) ? 
                                         task->channel->watering_event.watering.by_duration.duration_minutes * 60 : 
                                         task->channel->watering_event.watering.by_volume.volume_liters * 1000,
                                     0, 0);
    #endif
    
    return WATERING_SUCCESS;
}

/**
 * @brief Stop the currently running task
 * 
 * @return true if a task was stopped, false if no active task
 */
bool watering_stop_current_task(void) {
    // Use timeout instead of K_FOREVER to prevent system freeze
    if (k_mutex_lock(&watering_state_mutex, K_MSEC(100)) != 0) {
        printk("Failed to get mutex for task stop - system busy\n");
        return false;
    }
    
    if (watering_task_state.current_active_task == NULL) {
        k_mutex_unlock(&watering_state_mutex);
        return false;  // No active task
    }
    
    /* Stop interval task system */
    interval_task_stop("Task stopped manually or completed");

    uint8_t channel_id = 0;
    if (!get_channel_id_safe(watering_task_state.current_active_task->channel, &channel_id)) {
        printk("ERROR: Active task has invalid channel pointer; forcing valve shutdown\n");
        watering_task_state.current_active_task = NULL;
        watering_task_state.task_in_progress = false;
        watering_task_state.task_paused = false;
        watering_task_state.pause_start_time = 0;
        watering_task_state.total_paused_time = 0;
        current_task_state = W_TASK_STATE_IDLE;
        k_mutex_unlock(&watering_state_mutex);

        valve_close_all();

        #ifdef CONFIG_BT
        bt_irrigation_current_task_update(0xFF, 0, 0, 0, 0, 0); // No active task
        #endif

        return true;
    }
    watering_channel_off(channel_id);
    
    // Calculate effective duration excluding paused time
    uint32_t total_duration_ms = k_uptime_get_32() - watering_task_state.watering_start_time;
    uint32_t current_pause_time = 0;
    
    // If currently paused, add current pause period
    if (watering_task_state.task_paused) {
        current_pause_time = k_uptime_get_32() - watering_task_state.pause_start_time;
    }
    
    uint32_t duration_ms = total_duration_ms - watering_task_state.total_paused_time - current_pause_time;
    printk("Stopping watering for channel %d after %d seconds\n", 
           channel_id + 1, duration_ms / 1000);
    
    // Save completed task information for BLE reporting
    last_completed_task.task = watering_task_state.current_active_task;
    last_completed_task.start_time = watering_task_state.watering_start_time;
    last_completed_task.completion_time = k_uptime_get_32();
    last_completed_task.valid = true;
    
    /* Calculate actual values for history recording */
    uint16_t actual_value;
    uint32_t calibration = get_flow_calibration();
    if (calibration == 0) {
        calibration = DEFAULT_PULSES_PER_LITER;
    }
    uint16_t total_volume_ml = get_pulse_count() * 1000 / calibration; // Convert to ml
    
    if (watering_task_state.current_active_task->channel->watering_event.watering_mode == WATERING_BY_DURATION) {
        actual_value = duration_ms / (60 * 1000); // Convert to minutes
    } else {
        actual_value = total_volume_ml / 1000; // Convert to liters
    }
    
    watering_task_state.current_active_task = NULL;
    watering_task_state.task_in_progress = false;
    watering_task_state.task_paused = false;
    watering_task_state.pause_start_time = 0;
    watering_task_state.total_paused_time = 0;
    current_task_state = W_TASK_STATE_IDLE;
    
    k_mutex_unlock(&watering_state_mutex);
    
    /* AUTO mode: Reduce deficit after successful irrigation */
    watering_channel_t *stop_channel = NULL;
    if (watering_get_channel(channel_id, &stop_channel) == WATERING_SUCCESS && stop_channel &&
        stop_channel->watering_event.schedule_type == SCHEDULE_AUTO && total_volume_ml > 0) {
        float volume_liters = (float)total_volume_ml / 1000.0f;
        watering_error_t deficit_err = fao56_reduce_deficit_after_irrigation(channel_id, volume_liters);
        if (deficit_err != WATERING_SUCCESS) {
            printk("AUTO mode: Failed to reduce deficit for channel %d: %d\n", channel_id + 1, deficit_err);
        } else {
            printk("AUTO mode: Channel %d deficit reduced after %.1f L irrigation\n", 
                   channel_id + 1, (double)volume_liters);
        }
    }
    
    /* Record task completion in history */
    #ifdef CONFIG_BT
    watering_history_record_task_complete(channel_id, actual_value, total_volume_ml, WATERING_SUCCESS_COMPLETE);
    
    // Notify BLE clients about history event using RTC timestamp
    bt_irrigation_history_notify_event(channel_id, WATERING_EVENT_COMPLETE, 
                                      timezone_get_unix_utc(), total_volume_ml);
    #endif
    
    /* Increment completed tasks counter */
    watering_increment_completed_tasks_count();
    
    /* Notify BLE clients about task completion */
    #ifdef CONFIG_BT
    bt_irrigation_current_task_update(0xFF, 0, 0, 0, 0, 0); // No active task
    #endif
    
    watering_restore_event(channel_id);

    return true;
}

/**
 * @brief Check active tasks for completion or issues
 * 
 * @return 1 if tasks are active, 0 if idle, negative error code on failure
 */
int watering_check_tasks(void) {
    bool should_fetch_next = false;

    /* Update freeze status periodically so alarms clear/refresh */
    check_freeze_lockout(NULL);

    /* Run flow checks outside the task mutex so stop logic can acquire it. */
    watering_error_t flow_check_result = check_flow_anomalies();
    if (flow_check_result != WATERING_SUCCESS && flow_check_result != WATERING_ERROR_BUSY) {
        watering_increment_error_tasks();  /* Track flow anomaly errors */
        return -flow_check_result;
    }

    // Use timeout instead of K_NO_WAIT for better responsiveness while avoiding hangs
    if (k_mutex_lock(&watering_state_mutex, K_MSEC(50)) != 0) {
        return 0;   /* skip this cycle if busy but don't wait too long */
    }
    
    __attribute__((unused)) uint32_t start_time = k_uptime_get_32();

    if (system_status == WATERING_STATUS_FAULT) {
        k_mutex_unlock(&watering_state_mutex);
        return -WATERING_ERROR_BUSY;
    }
    
    __attribute__((unused)) uint32_t task_start_time = k_uptime_get_32();
    
    if (watering_task_state.current_active_task != NULL) {
        watering_channel_t *channel = watering_task_state.current_active_task->channel;
        watering_event_t *event = &channel->watering_event;
        uint8_t channel_id = channel - watering_channels;
        
        bool task_complete = false;
        uint32_t current_time = k_uptime_get_32();
        uint32_t elapsed_ms = current_time - watering_task_state.watering_start_time;
        
        /* Update interval task system */
        uint32_t current_volume = 0;
        float flow_rate = 0.0f;
        
        if (event->watering_mode == WATERING_BY_VOLUME) {
            uint32_t pulses = get_pulse_count();
            uint32_t pulses_per_liter;
            watering_get_flow_calibration(&pulses_per_liter);
            if (pulses_per_liter > 0) {
                current_volume = (pulses * 1000) / pulses_per_liter; // ml
            }
            // Flow rate calculation would go here if available
        }
        
        interval_task_update(current_volume, flow_rate);
        
        /* Check if we need to toggle valve for Cycle & Soak */
        if (!watering_task_state.task_paused) { // Only if not manually paused
            bool should_open_valve;
            interval_task_get_valve_control(&should_open_valve);
            
            // Check current valve state (we assume if not paused, it matches our expectation, but let's enforce)
            // Note: We don't have a direct "is_valve_open" check easily available without reading GPIO, 
            // but we can track state. For now, just enforce it.
            if (should_open_valve) {
                watering_channel_on(channel_id);
            } else {
                watering_channel_off(channel_id);
            }
        }

        /* Check completion via interval system */
        bool interval_complete = false;
        interval_task_is_complete(&interval_complete);
        
        if (interval_complete) {
            task_complete = true;
            printk("Task completed (Interval/Standard logic)\n");
        }
        
        /* Fallback / Safety checks */
        if (event->watering_mode == WATERING_BY_DURATION) {
            uint32_t duration_ms = event->watering.by_duration.duration_minutes * 60000;
            if (elapsed_ms >= duration_ms && !task_complete) {
                task_complete = true;
                printk("Duration task complete after %u ms (Fallback)\n", elapsed_ms);
            }
        } else if (event->watering_mode == WATERING_BY_VOLUME) {
            uint32_t pulses = get_pulse_count();
            uint32_t pulses_per_liter;
            watering_get_flow_calibration(&pulses_per_liter);
            
            uint32_t target_volume_ml = event->watering.by_volume.volume_liters * 1000;
            uint32_t pulses_target = (target_volume_ml * pulses_per_liter) / 1000;
            
            if (pulses >= pulses_target && !task_complete) {
                task_complete = true;
                printk("Volume task complete: %u pulses (Fallback)\n", pulses);
            }
            
            if (elapsed_ms > 30 * 60000) {
                printk("Volume task timed out (safety limit)\n");
                task_complete = true;
            }
        }
        
        if (task_complete) {
            uint8_t channel_id = watering_task_state.current_active_task->channel - watering_channels;
            watering_channel_off(channel_id);
            
            /* Stop interval task system */
            interval_task_stop("Task completed");

            // Save completed task information for BLE reporting
            last_completed_task.task = watering_task_state.current_active_task;
            last_completed_task.start_time = watering_task_state.watering_start_time;
            last_completed_task.completion_time = k_uptime_get_32();
            last_completed_task.valid = true;
            
            watering_task_state.current_active_task = NULL;
            watering_task_state.task_in_progress = false;
            current_task_state = W_TASK_STATE_COMPLETED;
            
            k_mutex_unlock(&watering_state_mutex);
            
            /* Increment completed tasks counter */
            watering_increment_completed_tasks_count();
            
            /* Notify BLE clients about task completion */
            #ifdef CONFIG_BT
            bt_irrigation_current_task_update(0xFF, 0, 0, 0, 0, 0); // No active task
            #endif
            watering_restore_event(channel_id);
            
            return 1;
        }
    } else if (current_task_state != W_TASK_STATE_RUNNING) {
        should_fetch_next = true;
    }

    k_mutex_unlock(&watering_state_mutex);

    if (should_fetch_next) {
        int result = watering_process_next_task();
        if (result < 0) {
            return result;
        }
        if (result > 0) {
            return 1;
        }
    }

    return (watering_task_state.current_active_task != NULL) ? 1 : 0;
}

/**
 * @brief Clean up completed tasks and release resources
 * 
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_cleanup_tasks(void) {
    // Use timeout instead of K_FOREVER to prevent system freeze
    if (k_mutex_lock(&watering_state_mutex, K_MSEC(100)) != 0) {
        return WATERING_SUCCESS; // Skip cleanup if busy, try again later
    }
    
    if (current_task_state == W_TASK_STATE_COMPLETED) {
        /* Ensure state is fully reset even if the active task pointer was already cleared */
        watering_task_state.current_active_task = NULL;
        watering_task_state.task_in_progress = false;
        current_task_state = W_TASK_STATE_IDLE;
        
        /* Notify BLE clients about task completion */
        #ifdef CONFIG_BT
        bt_irrigation_current_task_update(0xFF, 0, 0, 0, 0, 0); // No active task
        #endif
    }
    
    k_mutex_unlock(&watering_state_mutex);
    return WATERING_SUCCESS;
}

/**
 * @brief Update the flow sensor calibration
 * 
 * @param new_pulses_per_liter New calibration value in pulses per liter
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_set_flow_calibration(uint32_t new_pulses_per_liter) {
    if (new_pulses_per_liter == 0) {
        return WATERING_ERROR_INVALID_PARAM;
    }

    /* Persist and update the authoritative calibration in flow_sensor */
    int ret = set_flow_calibration(new_pulses_per_liter);
    if (ret < 0) {
        return WATERING_ERROR_CONFIG;
    }

    printk("Flow sensor calibration updated: %d pulses per liter\n", new_pulses_per_liter);
    return WATERING_SUCCESS;
}

/**
 * @brief Get the current flow sensor calibration
 * 
 * @param pulses_per_liter_out Pointer to store the calibration value
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_get_flow_calibration(uint32_t *pulses_per_liter_out) {
    if (pulses_per_liter_out == NULL) {
        return WATERING_ERROR_INVALID_PARAM;
    }

    /* Always read the authoritative value from flow_sensor (persisted in NVS) */
    *pulses_per_liter_out = get_flow_calibration();
    if (*pulses_per_liter_out == 0) {
        *pulses_per_liter_out = DEFAULT_PULSES_PER_LITER;
    }

    return WATERING_SUCCESS;
}

/**
 * @brief Handle RTC failures
 * 
 * @return WATERING_SUCCESS if recovered, error code otherwise
 */
static watering_error_t handle_rtc_failure(void) {
    rtc_error_count++;
    printk("RTC error detected, count: %d/%d\n", rtc_error_count, MAX_RTC_ERRORS);
    
    if (rtc_error_count >= MAX_RTC_ERRORS) {
        printk("Maximum RTC errors reached, entering RTC failure mode\n");
        system_status = WATERING_STATUS_RTC_ERROR;
        
        k_sleep(K_MSEC(100));
        if (rtc_init() == 0 && rtc_is_available()) {
            printk("Final RTC recovery attempt successful\n");
            rtc_error_count = MAX_RTC_ERRORS - 1;
            return WATERING_SUCCESS;
        }
        
        return WATERING_ERROR_RTC_FAILURE;
    }
    
    if (rtc_init() == 0 && rtc_is_available()) {
        printk("RTC recovery successful\n");
        rtc_error_count = 0;
        return WATERING_SUCCESS;
    }
    
    return WATERING_ERROR_RTC_FAILURE;
}

/**
 * @brief Update system time when RTC is not available
 */
static void update_system_time(void) {
    uint32_t now = k_uptime_get_32();
    
    if (now >= last_time_update) {
        uint32_t elapsed_ms = now - last_time_update;
        uint32_t elapsed_minutes = elapsed_ms / 60000;
        
        if (elapsed_minutes > 0) {
            current_minute += elapsed_minutes;
            
            while (current_minute >= 60) {
                current_minute -= 60;
                current_hour++;
                
                if (current_hour >= 24) {
                    current_hour = 0;
                    
                    current_day_of_week = (current_day_of_week + 1) % 7;
                    
                    days_since_start++;
                    watering_save_config();
                    
                    last_day = (last_day % 31) + 1;
                    printk("Day changed (system time), days since start: %d\n", days_since_start);
                }
            }
            
            last_time_update = now - (elapsed_ms % 60000);
        }
    } else {
        last_time_update = now;
    }
}

/**
 * @brief Thread function for watering task processing
 */
static void watering_task_fn(void *p1, void *p2, void *p3) {
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);
    
    printk("Watering processing task started\n");
    
    while (!exit_tasks) {
        watering_check_tasks();
        watering_cleanup_tasks();

        uint32_t sleep_ms = 500;
        switch (current_power_mode) {
            case POWER_MODE_NORMAL:
                sleep_ms = 500;
                break;
            case POWER_MODE_ENERGY_SAVING:
                sleep_ms = 2000;
                break;
            case POWER_MODE_ULTRA_LOW_POWER:
                sleep_ms = 600000;
                break;
        }
        k_sleep(K_MSEC(sleep_ms));
    }
    
    printk("Watering processing task stopped\n");
}

/**
 * @brief Thread function for schedule checking
 */
static void scheduler_task_fn(void *p1, void *p2, void *p3) {
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);
    
    printk("Watering scheduler task started\n");
    
    int rtc_status = rtc_init();
    if (rtc_status != 0) {
        printk("ERROR: Failed to initialize RTC. Will use system time instead.\n");
        system_status = WATERING_STATUS_RTC_ERROR;
    } else {
        printk("RTC initialized successfully\n");
    }
    
    rtc_datetime_t now;
    if (rtc_status == 0 && rtc_datetime_get(&now) == 0) {
        /* TIMEZONE FIX: Convert RTC time (UTC) to local time for scheduling */
        uint32_t utc_timestamp = timezone_rtc_to_unix_utc(&now);
        rtc_datetime_t local_time;
        
        /* Convert UTC to local time using timezone configuration */
        if (timezone_unix_to_rtc_local(utc_timestamp, &local_time) == 0) {
            current_hour = local_time.hour;
            current_minute = local_time.minute;
            current_day_of_week = local_time.day_of_week;
            last_day = local_time.day;
            printk("Current time from RTC (LOCAL): %02d:%02d, day %d [UTC was %02d:%02d]\n", 
                   current_hour, current_minute, current_day_of_week, now.hour, now.minute);
        } else {
            /* Fallback to UTC if timezone conversion fails */
            current_hour = now.hour;
            current_minute = now.minute;
            current_day_of_week = now.day_of_week;
            last_day = now.day;
            printk("Current time from RTC (UTC fallback): %02d:%02d, day %d\n", 
                   current_hour, current_minute, current_day_of_week);
        }
    } else {
        printk("Using system time as fallback\n");
        current_hour = 12;
        current_minute = 0;
        current_day_of_week = 1;
        last_day = 1;
        
        last_time_update = k_uptime_get_32();
    }
    
    while (!exit_tasks) {
        bool rtc_read_success = false;
        uint32_t utc_timestamp_for_history = 0U;
        
        if (rtc_status == 0) {
            if (rtc_datetime_get(&now) == 0) {
                /* TIMEZONE FIX: Convert RTC time (UTC) to local time for scheduling */
                uint32_t utc_timestamp = timezone_rtc_to_unix_utc(&now);
                utc_timestamp_for_history = utc_timestamp;
                rtc_datetime_t local_time;
                
                /* Convert UTC to local time using timezone configuration */
                if (timezone_unix_to_rtc_local(utc_timestamp, &local_time) == 0) {
                    current_hour = local_time.hour;
                    current_minute = local_time.minute;
                    current_day_of_week = local_time.day_of_week;
                } else {
                    /* Fallback to UTC if timezone conversion fails */
                    current_hour = now.hour;
                    current_minute = now.minute;
                    current_day_of_week = now.day_of_week;
                }
                rtc_read_success = true;
                
                if (rtc_error_count > 0) {
                    rtc_error_count--;
                }
                
                /* Calculate current julian day (day of year) for AUTO mode tracking */
                /* Simple formula: days elapsed in current year */
                static const uint16_t days_before_month[] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
                if (now.month >= 1 && now.month <= 12) {
                    current_julian_day = days_before_month[now.month - 1] + now.day;
                    /* Leap year adjustment for months after February */
                    if (now.month > 2 && (now.year % 4 == 0) && 
                        ((now.year % 100 != 0) || (now.year % 400 == 0))) {
                        current_julian_day++;
                    }
                }
                
                if (now.day != last_day) {
                    days_since_start++;
                    last_day = now.day;
                    
                    /* Reset AUTO check flags for all channels on day change */
                    for (int ch = 0; ch < WATERING_CHANNELS_COUNT; ch++) {
                        watering_channels[ch].auto_check_ran_today = false;
                    }
                    
                    watering_save_config();
                    printk("Day changed, days since start: %d, julian day: %d\n", 
                           days_since_start, current_julian_day);
                }
            } else {
                handle_rtc_failure();
            }
        } else {
            update_system_time();
            rtc_read_success = true;
            utc_timestamp_for_history = timezone_get_unix_utc();
        }
        
        if (rtc_read_success) {
            watering_scheduler_run();

            /* Aggregate hourly/daily environmental history (once per scheduler tick) */
            if (utc_timestamp_for_history != 0U && env_history_get_storage() != NULL) {
                int env_rc = env_history_auto_aggregate(utc_timestamp_for_history);
                if (env_rc != 0 && env_rc != (-WATERING_ERROR_NOT_INITIALIZED)) {
                    LOG_WRN("Environmental history auto-aggregate failed: %d", env_rc);
                }
            }
             
            // Run automatic irrigation calculations periodically
            uint32_t current_time = k_uptime_get_32();
            if (auto_calc_enabled && 
                (current_time - last_auto_calc_time) >= auto_calc_interval_ms) {
                
                watering_error_t calc_result = watering_run_automatic_calculations();
                if (calc_result == WATERING_SUCCESS) {
                    last_auto_calc_time = current_time;
                } else if (calc_result != WATERING_ERROR_BUSY) {
                    // Log error but don't stop scheduler
                    printk("Automatic calculation failed: %d\n", calc_result);
                }
            }
        }

        uint32_t sleep_time = 60;
        switch (current_power_mode) {
            case POWER_MODE_NORMAL:
                sleep_time = 60;
                break;
            case POWER_MODE_ENERGY_SAVING:
                sleep_time = 120;
                break;
            case POWER_MODE_ULTRA_LOW_POWER:
                sleep_time = 300;
                break;
        }
        
        k_sleep(K_SECONDS(sleep_time));
    }
    
    printk("Watering scheduler task stopped\n");
}

/**
 * @brief Run the watering scheduler to check for scheduled tasks
 * 
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_scheduler_run(void) {
    /* CRITICAL FIX: Add timeout protection to prevent scheduler hanging */
    uint32_t scheduler_start_time = k_uptime_get_32();
    
    printk("Running watering scheduler [time %02d:%02d, day %d]\n", 
           current_hour, current_minute, current_day_of_week);

    /* Global anti-freeze block: do not schedule while lockout active */
    float temp_c = 0.0f;
    if (check_freeze_lockout(&temp_c)) {
        printk("Scheduler: freeze lockout active (T=%.1fC) - skipping all channels\n", (double)temp_c);
        return WATERING_ERROR_BUSY;
    }
    
    if (system_status == WATERING_STATUS_FAULT || system_status == WATERING_STATUS_RTC_ERROR) {
        return WATERING_ERROR_BUSY;
    }
    
    if (current_hour > 23 || current_minute > 59 || current_day_of_week > 6) {
        LOG_ERROR("Invalid time values in scheduler", WATERING_ERROR_INVALID_PARAM);
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    /* CRITICAL FIX: Add timeout check to prevent infinite loops */
    for (int i = 0; i < WATERING_CHANNELS_COUNT; i++) {
        /* Safety check: prevent scheduler from running too long */
        if (k_uptime_get_32() - scheduler_start_time > 1000) {
            printk("Scheduler timeout after %d channels - aborting\n", i);
            break;
        }
        
        watering_channel_t *channel = &watering_channels[i];
        watering_event_t *event = &channel->watering_event;
        
        if (!event->auto_enabled) {
            continue;
        }
        
        bool should_run = false;
        bool is_auto_mode = false;
        fao56_auto_decision_t auto_decision = {0};
        
        /* TIMEZONE FIX: Both times are now in LOCAL TIME for proper scheduling */
        /* current_hour/current_minute = RTC time converted to local timezone */
        /* event->start_time.hour/minute = user-configured local time (or fallback for solar timing) */
        
        /* Calculate effective start time (handles solar timing if enabled) */
        uint8_t effective_hour = event->start_time.hour;
        uint8_t effective_minute = event->start_time.minute;
        
        if (event->use_solar_timing) {
            /* Get timezone offset in hours (rounded from minutes) */
            int16_t tz_offset_min = timezone_get_total_offset(timezone_get_unix_utc());
            int8_t tz_offset_hours = (int8_t)(tz_offset_min / 60);
            
            watering_error_t solar_ret = fao56_get_effective_start_time(
                event, 
                channel->latitude_deg, 
                channel->longitude_deg,
                current_julian_day,
                tz_offset_hours,
                &effective_hour, 
                &effective_minute
            );
            
            if (solar_ret == WATERING_ERROR_SOLAR_FALLBACK) {
                printk("Solar timing: Channel %d using fallback time %02u:%02u\n",
                       i + 1, effective_hour, effective_minute);
            }
        }
        
        if (effective_hour == current_hour && effective_minute == current_minute) {
            if (event->schedule_type == SCHEDULE_DAILY) {
                if (event->schedule.daily.days_of_week & (1 << current_day_of_week)) {
                    should_run = true;
                }
            } else if (event->schedule_type == SCHEDULE_PERIODIC) {
                if (event->schedule.periodic.interval_days > 0 && 
                    days_since_start > 0 && 
                    (days_since_start % event->schedule.periodic.interval_days) == 0) {
                    should_run = true;
                }
            } else if (event->schedule_type == SCHEDULE_AUTO) {
                /* AUTO mode: FAO-56 based smart scheduling */
                is_auto_mode = true;
                
                /* Prevent running multiple times per day using julian day tracking */
                if (channel->last_auto_check_julian_day == current_julian_day && 
                    channel->auto_check_ran_today) {
                    printk("AUTO mode: Channel %d already checked today (julian=%u)\n", 
                           i + 1, current_julian_day);
                    continue;
                }
                
                /* Validate AUTO mode prerequisites */
                if (!watering_channel_auto_mode_valid(channel)) {
                    printk("AUTO mode: Channel %d missing required config (plant/soil/date)\n", i + 1);
                    continue;
                }
                
                /* Handle multi-day offline gap: apply missed ETc accumulation */
                if (channel->last_auto_check_julian_day > 0) {
                    int16_t days_diff;
                    if (current_julian_day >= channel->last_auto_check_julian_day) {
                        days_diff = current_julian_day - channel->last_auto_check_julian_day;
                    } else {
                        /* Year wrapped: assume 365 days in year for simplicity */
                        days_diff = (365 - channel->last_auto_check_julian_day) + current_julian_day;
                    }
                    if (days_diff > 1 && days_diff <= 366) {
                        /* More than 1 day gap - apply conservative ETc for missed days
                         * (subtract 1 because today's check will handle the current day) */
                        uint16_t days_missed = (uint16_t)(days_diff - 1);
                        watering_error_t gap_err = fao56_apply_missed_days_deficit(i, days_missed);
                        if (gap_err == WATERING_SUCCESS) {
                            printk("AUTO mode: Channel %d applied %u missed days deficit\n", 
                                   i + 1, days_missed);
                        }
                    }
                }
                
                /* Run daily deficit update and get irrigation decision */
                watering_error_t auto_err = fao56_daily_update_deficit(i, &auto_decision);
                if (auto_err != WATERING_SUCCESS) {
                    printk("AUTO mode: Deficit calculation failed for channel %d: %d\n", 
                           i + 1, auto_err);
                    continue;
                }
                
                /* Mark that we've done the daily check */
                channel->last_auto_check_julian_day = current_julian_day;
                channel->auto_check_ran_today = true;
                
                /* Only run if FAO-56 says we need water */
                if (auto_decision.should_water && auto_decision.volume_liters > 0) {
                    should_run = true;
                    printk("AUTO mode: Channel %d NEEDS WATER - deficit=%.1f mm, volume=%.1f L\n",
                           i + 1, (double)auto_decision.current_deficit_mm, 
                           (double)auto_decision.volume_liters);
                } else {
                    printk("AUTO mode: Channel %d SKIP - soil has adequate moisture (deficit=%.1f mm)\n",
                           i + 1, (double)auto_decision.current_deficit_mm);
                }
            }
        }
        
        if (should_run) {
            /* CRITICAL FIX: Add safety check to prevent system overload during scheduling */
            if (system_status == WATERING_STATUS_FAULT) {
                printk("System in fault state - skipping scheduled task for channel %d\n", i + 1);
                continue;
            }
            
            /* Check if we already have too many tasks in the queue */
            if (k_msgq_num_used_get(&watering_tasks_queue) >= 2) {
                printk("Task queue full - skipping scheduled task for channel %d\n", i + 1);
                continue;
            }
            
            watering_task_t new_task;
            new_task.channel = channel;
            new_task.trigger_type = WATERING_TRIGGER_SCHEDULED;  // Scheduled tasks
            
            if (is_auto_mode) {
                /* AUTO mode: Always use volume-based watering with FAO-56 calculated volume */
                new_task.by_volume.volume_liters = (uint32_t)auto_decision.volume_liters;
                /* Temporarily override watering mode to volume for this task */
                channel->watering_event.watering_mode = WATERING_BY_VOLUME;
            } else if (event->watering_mode == WATERING_BY_DURATION) {
                new_task.by_time.start_time = k_uptime_get_32();
            } else {
                new_task.by_volume.volume_liters = event->watering.by_volume.volume_liters;
            }
            
            watering_error_t result = watering_add_task(&new_task);
            if (result == WATERING_SUCCESS) {
                /* Use RTC timestamp for persistent last watering time tracking */
                channel->last_watering_time = timezone_get_unix_utc();
                if (is_auto_mode) {
                    printk("AUTO watering scheduled for channel %d: %.1f L\n", 
                           i + 1, (double)auto_decision.volume_liters);
                } else {
                    printk("Watering schedule added for channel %d (added to task queue)\n", i + 1);
                }
            } else {
                printk("Failed to add scheduled task for channel %d: error %d\n", i + 1, result);
            }
        }
    }
    
    return WATERING_SUCCESS;
}

/**
 * @brief Start the background tasks for watering operations
 * 
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_start_tasks(void) {
    if (!system_initialized) {
        return WATERING_ERROR_NOT_INITIALIZED;
    }
    
    if (watering_tasks_running) {
        printk("Watering tasks already running\n");
        return WATERING_SUCCESS;
    }
    
    exit_tasks = false;
    
    k_tid_t watering_tid =
        k_thread_create(&watering_task_data, watering_task_stack, K_THREAD_STACK_SIZEOF(watering_task_stack),
                        watering_task_fn, NULL, NULL, NULL, K_PRIO_PREEMPT(5), 0, K_NO_WAIT);
    if (watering_tid == NULL) {
        LOG_ERROR("Error creating watering processing task", WATERING_ERROR_CONFIG);
        return WATERING_ERROR_CONFIG;
    }
    
    k_tid_t scheduler_tid =
        k_thread_create(&scheduler_task_data, scheduler_task_stack, K_THREAD_STACK_SIZEOF(scheduler_task_stack),
                        scheduler_task_fn, NULL, NULL, NULL, 
                        K_PRIO_PREEMPT(7), 0, K_NO_WAIT);
    if (scheduler_tid == NULL) {
        LOG_ERROR("Error creating scheduler task", WATERING_ERROR_CONFIG);
        exit_tasks = true;
        k_thread_abort(watering_tid);
        return WATERING_ERROR_CONFIG;
    }
    
    k_thread_name_set(watering_tid, "watering_task");
    k_thread_name_set(scheduler_tid, "scheduler_task");
    watering_tasks_running = true;
    printk("Watering tasks successfully started\n");
    return WATERING_SUCCESS;
}

/**
 * @brief Stop all background watering tasks
 * 
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_stop_tasks(void) {
    if (!watering_tasks_running) {
        return WATERING_SUCCESS;
    }
    
    printk("Stopping watering tasks...\n");
    exit_tasks = true;
    k_sleep(K_SECONDS(1));
    
    for (int i = 0; i < WATERING_CHANNELS_COUNT; i++) {
        watering_channel_off(i);
    }

    for (uint8_t channel = 0; channel < WATERING_CHANNELS_COUNT; channel++) {
        watering_restore_event(channel);
    }
    
    watering_tasks_running = false;
    printk("Watering tasks stopped\n");
    return WATERING_SUCCESS;
}

/* ------------------------------------------------------------------------
 * Duplicate definition removed – a unică implementare se află acum în
 * watering.c.  Menţinem codul comentat ca referinţă dar nu îl mai compilăm.
 * --------------------------------------------------------------------- */
#if 0
/**
 * @brief Validate watering event configuration
 * 
 * @param event Pointer to watering event to validate
 * @return WATERING_SUCCESS if valid, error code if invalid
 */
watering_error_t watering_validate_event_config(const watering_event_t *event) {
    if (event == NULL) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    if (event->schedule_type != SCHEDULE_DAILY && event->schedule_type != SCHEDULE_PERIODIC) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    if (event->watering_mode != WATERING_BY_DURATION && event->watering_mode != WATERING_BY_VOLUME) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    if (event->schedule_type == SCHEDULE_DAILY) {
        if (event->schedule.daily.days_of_week == 0) {
            return WATERING_ERROR_INVALID_PARAM;
        }
    } else {
        if (event->schedule.periodic.interval_days == 0) {
            return WATERING_ERROR_INVALID_PARAM;
        }
    }
    
    if (event->watering_mode == WATERING_BY_DURATION) {
        if (event->watering.by_duration.duration_minutes == 0) {
            return WATERING_ERROR_INVALID_PARAM;
        }
    } else {
        if (event->watering.by_volume.volume_liters == 0) {
            return WATERING_ERROR_INVALID_PARAM;
        }
    }
    
    if (event->start_time.hour > 23 || event->start_time.minute > 59) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    return WATERING_SUCCESS;
}
#endif /* duplicate removed */
/**
 * @brief Add a duration-based watering task for a specific channel
 * 
 * @param channel_id Channel ID
 * @param minutes Duration in minutes
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_add_duration_task(uint8_t channel_id, uint16_t minutes) {
    watering_task_t new_task;
    watering_channel_t *channel;
    watering_error_t err;
    
    if (channel_id >= WATERING_CHANNELS_COUNT || minutes == 0) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    err = watering_get_channel(channel_id, &channel);
    if (err != WATERING_SUCCESS) {
        printk("Error getting channel %d: %d\n", channel_id, err);
        return err;
    }
    
    new_task.channel = channel;
    new_task.trigger_type = WATERING_TRIGGER_MANUAL;  // Tasks created via Bluetooth are manual
    new_task.channel->watering_event.watering_mode = WATERING_BY_DURATION;
    new_task.channel->watering_event.watering.by_duration.duration_minutes = minutes;
    new_task.by_time.start_time = k_uptime_get_32();
    
    printk("Adding %d minute watering task for channel %d with trigger type %d (MANUAL)\n", 
           minutes, channel_id + 1, new_task.trigger_type);
    return watering_add_task(&new_task);
}

/**
 * @brief Add a volume-based watering task for a specific channel
 * 
 * @param channel_id Channel ID
 * @param liters Volume in liters
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_add_volume_task(uint8_t channel_id, uint16_t liters) {
    watering_task_t new_task;
    watering_channel_t *channel;
    watering_error_t err;
    
    if (channel_id >= WATERING_CHANNELS_COUNT || liters == 0) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    err = watering_get_channel(channel_id, &channel);
    if (err != WATERING_SUCCESS) {
        printk("Error getting channel %d: %d\n", channel_id, err);
        return err;
    }
    
    new_task.channel = channel;
    new_task.trigger_type = WATERING_TRIGGER_MANUAL;  // Tasks created via Bluetooth are manual
    new_task.channel->watering_event.watering_mode = WATERING_BY_VOLUME;
    new_task.channel->watering_event.watering.by_volume.volume_liters = liters;
    new_task.by_volume.volume_liters = liters;
    
    printk("Adding %d liter watering task for channel %d with trigger type %d (MANUAL)\n", 
           liters, channel_id + 1, new_task.trigger_type);
    return watering_add_task(&new_task);
}

/**
 * @brief Clear the pending task queue
 * 
 * @return Number of tasks removed
 */
int watering_clear_task_queue(void) {
    int count = 0;
    watering_task_t dummy_task;
    
    // Use timeout instead of K_FOREVER to prevent system freeze
    if (k_mutex_lock(&watering_state_mutex, K_MSEC(100)) != 0) {
        return 0; // Return 0 if we can't get the mutex quickly
    }
    
    while (k_msgq_get(&watering_tasks_queue, &dummy_task, K_NO_WAIT) == 0) {
        count++;
    }
    
    if (count > 0) {
        k_msgq_purge(&watering_tasks_queue);
    }
    
    printk("%d tasks removed from queue\n", count);
    
    k_mutex_unlock(&watering_state_mutex);

    if (count > 0) {
        for (uint8_t channel = 0; channel < WATERING_CHANNELS_COUNT; channel++) {
            watering_restore_event(channel);
        }
    }

    return count;
}

/**
 * @brief Get the number of pending tasks
 * 
 * @return Number of pending tasks
 */
int watering_get_pending_tasks_count(void) {
    return k_msgq_num_used_get(&watering_tasks_queue);
}



/**
 * @brief Get information about pending tasks
 * 
 * @param tasks_info Buffer for task information (watering_task_info_t[])
 * @param max_tasks Maximum buffer size
 * @return Number of tasks copied to buffer
 */
int watering_get_pending_tasks_info(void *tasks_info, int max_tasks) {
    if (!tasks_info || max_tasks <= 0) {
        return 0;
    }
    
    // Use timeout instead of K_FOREVER to prevent system freeze
    if (k_mutex_lock(&watering_state_mutex, K_MSEC(100)) != 0) {
        return 0; // Return 0 if we can't get the mutex quickly
    }
    
    int task_count = 0;
    watering_task_info_t *info_array = (watering_task_info_t *)tasks_info;
    
    // For now, we only have one active task maximum since we use a simple message queue
    // In a real implementation, we'd iterate through the message queue or task list
    
    // Check if there's a current active task
    if (watering_task_state.current_active_task != NULL && task_count < max_tasks) {
        watering_task_t *current_task = watering_task_state.current_active_task;
        watering_channel_t *channel = current_task->channel;
        
        if (channel) {
            info_array[task_count].channel_id = channel - watering_channels;
            info_array[task_count].task_type = channel->watering_event.watering_mode;
            
            if (channel->watering_event.watering_mode == WATERING_BY_DURATION) {
                info_array[task_count].target_value = channel->watering_event.watering.by_duration.duration_minutes;
            } else {
                info_array[task_count].target_value = channel->watering_event.watering.by_volume.volume_liters;
            }
            
            info_array[task_count].start_time = watering_task_state.watering_start_time;
            info_array[task_count].is_active = true;
            info_array[task_count].is_paused = watering_task_state.task_paused;
            
            task_count++;
        }
    }
    
    // NOTE: Zephyr's k_msgq doesn't provide a way to inspect queued items without
    // removing them from the queue. In a production system, you might:
    // 1. Maintain a separate list of pending tasks
    // 2. Use a custom queue implementation with inspection capability
    // 3. Keep a counter of queued items
    // For now, we only report the active task which is sufficient for most use cases
    
    k_mutex_unlock(&watering_state_mutex);
    
    return task_count;
}

/**
 * @brief Run a test cycle of all valves
 * 
 * @return WATERING_SUCCESS on success, error code on failure
 */
__attribute__((unused))
static watering_error_t run_valve_test(void) {
    printk("Running valve test sequence...\n");
    watering_error_t err;
    
    for (int i = 0; i < WATERING_CHANNELS_COUNT; i++) {
        printk("Testing channel %d...\n", i + 1);
        err = watering_channel_on(i);
        if (err != WATERING_SUCCESS) {
            printk("Error activating channel %d: %d\n", i + 1, err);
            continue;
        }
        
        k_sleep(K_SECONDS(1));
        
        err = watering_channel_off(i);
        if (err != WATERING_SUCCESS) {
            printk("Error deactivating channel %d: %d\n", i + 1, err);
        }
        
        k_sleep(K_MSEC(200));
    }
    
    return WATERING_SUCCESS;
}

/**
 * @brief Get the number of running tasks
 * 
 * @return Number of running tasks (0 or 1 since only one task can run at a time)
 */
int watering_get_running_tasks_count(void) {
    return (watering_task_state.task_in_progress && watering_task_state.current_active_task) ? 1 : 0;
}

/**
 * @brief Get the number of completed tasks
 * 
 * @return Number of completed tasks (for now just returns 1 if last task is valid)
 */
// Note: watering_get_completed_tasks_count() is implemented in watering.c

/**
 * @brief Increment error task count
 * 
 * This function should be called when a task encounters an error
 */
void watering_increment_error_tasks(void) {
    if (error_task_count < UINT16_MAX) {
        error_task_count++;
        LOG_WRN("Error task count incremented to %u", error_task_count);
    }
}

/**
 * @brief Get the number of error tasks
 * 
 * @return Number of error tasks
 */
int watering_get_error_tasks_count(void) {
    return error_task_count;
}

/**
 * @brief Get the next scheduled task
 * 
 * @return Pointer to next task or NULL if no tasks pending
 */
watering_task_t *watering_get_next_task(void) {
    /* Check if there are any messages in the queue */
    if (k_msgq_num_used_get(&watering_tasks_queue) == 0) {
        return NULL; /* No tasks pending */
    }

    static watering_task_t next_task;
    if (k_msgq_peek(&watering_tasks_queue, &next_task) != 0) {
        return NULL;
    }
    return &next_task;
}

watering_error_t watering_peek_next_task(watering_task_t *task_out)
{
    if (task_out == NULL) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    if (k_msgq_num_used_get(&watering_tasks_queue) == 0) {
        return WATERING_ERROR_INVALID_DATA;
    }
    if (k_msgq_peek(&watering_tasks_queue, task_out) != 0) {
        return WATERING_ERROR_BUSY;
    }
    return WATERING_SUCCESS;
}

/**
 * @brief Get the current running task
 * 
 * @return Pointer to current task or NULL if no task running
 */
watering_task_t *watering_get_current_task(void) {
    return watering_task_state.current_active_task;
}

/**
 * @brief Clear all tasks from the system
 * 
 * @return WATERING_SUCCESS on success
 */
watering_error_t watering_clear_all_tasks(void) {
    // Stop current task if running
    watering_stop_current_task();
    
    // Clear the task queue
    watering_clear_task_queue();
    
    return WATERING_SUCCESS;
}

/**
 * @brief Clear error tasks
 * 
 * @return WATERING_SUCCESS on success
 */
watering_error_t watering_clear_error_tasks(void) {
    error_task_count = 0;
    LOG_INF("Error tasks cleared");
    return WATERING_SUCCESS;
}

/**
 * @brief Pause all tasks
 * 
 * @return WATERING_SUCCESS on success
 */
watering_error_t watering_pause_all_tasks(void) {
    /* Pause current task if running */
    if (watering_task_state.task_in_progress) {
        watering_pause_current_task();
        LOG_INF("All tasks paused");
    } else {
        LOG_INF("No tasks to pause");
    }
    return WATERING_SUCCESS;
}

/**
 * @brief Pause the currently running task
 * 
 * @return true if a task was paused, false if no task is running or task cannot be paused
 */
bool watering_pause_current_task(void) {
    // Use timeout instead of K_FOREVER to prevent system freeze
    if (k_mutex_lock(&watering_state_mutex, K_MSEC(100)) != 0) {
        return false; // Can't pause if mutex is busy
    }
    
    // Check if there's an active task
    if (watering_task_state.current_active_task == NULL || !watering_task_state.task_in_progress) {
        k_mutex_unlock(&watering_state_mutex);
        return false;
    }
    
    // Check if task is already paused
    if (watering_task_state.task_paused) {
        k_mutex_unlock(&watering_state_mutex);
        return false;
    }
    
    // Pause the task
    watering_task_state.task_paused = true;
    watering_task_state.pause_start_time = k_uptime_get_32();
    
    // Get channel and close valve
    watering_channel_t *channel = watering_task_state.current_active_task->channel;
    uint8_t channel_id = channel - watering_channels;
    
    // Close valve to stop water flow
    watering_error_t valve_err = watering_channel_off(channel_id);
    if (valve_err != WATERING_SUCCESS) {
        printk("Warning: Failed to close valve during pause for channel %d\n", channel_id);
    }
    
    // Update system state
    watering_error_t state_err = transition_to_state(WATERING_STATE_PAUSED);
    if (state_err != WATERING_SUCCESS) {
        printk("Warning: Failed to transition to paused state\n");
    }
    
    k_mutex_unlock(&watering_state_mutex);
    
    printk("Task paused for channel %d\n", channel_id);
    return true;
}

/**
 * @brief Resume the currently paused task
 * 
 * @return true if a task was resumed, false if no task was paused or task cannot be resumed
 */
bool watering_resume_current_task(void) {
    // Use timeout instead of K_FOREVER to prevent system freeze
    if (k_mutex_lock(&watering_state_mutex, K_MSEC(100)) != 0) {
        return false; // Can't resume if mutex is busy
    }
    
    // Check if there's a paused task
    if (watering_task_state.current_active_task == NULL || !watering_task_state.task_paused) {
        k_mutex_unlock(&watering_state_mutex);
        return false;
    }
    
    // Calculate how long we were paused and add to total
    uint32_t pause_duration = k_uptime_get_32() - watering_task_state.pause_start_time;
    watering_task_state.total_paused_time += pause_duration;
    
    // Resume the task
    watering_task_state.task_paused = false;
    watering_task_state.pause_start_time = 0;
    
    // Get channel and reopen valve
    watering_channel_t *channel = watering_task_state.current_active_task->channel;
    uint8_t channel_id = channel - watering_channels;
    
    // Reopen valve to resume water flow
    watering_error_t valve_err = watering_channel_on(channel_id);
    if (valve_err != WATERING_SUCCESS) {
        printk("Error: Failed to reopen valve during resume for channel %d\n", channel_id);
        k_mutex_unlock(&watering_state_mutex);
        return false;
    }
    
    // Update system state
    watering_error_t state_err = transition_to_state(WATERING_STATE_WATERING);
    if (state_err != WATERING_SUCCESS) {
        printk("Warning: Failed to transition to watering state\n");
    }
    
    k_mutex_unlock(&watering_state_mutex);
    
    printk("Task resumed for channel %d (paused for %u ms)\n", channel_id, pause_duration);
    return true;
}

/**
 * @brief Check if the current task is paused
 * 
 * @return true if current task is paused, false otherwise
 */
bool watering_is_current_task_paused(void) {
    // Use timeout instead of K_FOREVER to prevent system freeze
    if (k_mutex_lock(&watering_state_mutex, K_MSEC(100)) != 0) {
        return false; // Return false if we can't check quickly
    }
    bool paused = watering_task_state.task_paused;
    k_mutex_unlock(&watering_state_mutex);
    return paused;
}
