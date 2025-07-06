#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/pm/pm.h>
#include <zephyr/logging/log.h>
#include "flow_sensor.h"
#include "watering.h"
#include "watering_internal.h"
#include "rtc.h"
#include "bt_irrigation_service.h"   /* NEW */
#include "watering_history.h"        /* Add history integration */

LOG_MODULE_DECLARE(watering, CONFIG_LOG_DEFAULT_LEVEL);

/**
 * @file watering_tasks.c
 * @brief Implementation of watering task management and scheduling
 * 
 * This file manages the execution of watering tasks including scheduling,
 * prioritization, and flow monitoring.
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
    TASK_STATE_IDLE,      /**< No active task */
    TASK_STATE_RUNNING,   /**< Task is currently running */
    TASK_STATE_COMPLETED  /**< Task has completed but not cleaned up */
} task_state_t;

/** Current state of task execution system */
static task_state_t current_task_state = TASK_STATE_IDLE;

/** Flow sensor calibration - pulses per liter */
static uint32_t pulses_per_liter = DEFAULT_PULSES_PER_LITER;

/** Stack sizes for watering threads */
#define WATERING_STACK_SIZE 2048
#define SCHEDULER_STACK_SIZE 1024

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

/** Mutex for protecting task state */
K_MUTEX_DEFINE(watering_state_mutex);

/** Current time tracking for scheduler */
static uint8_t current_hour = 0;
static uint8_t current_minute = 0;
static uint8_t current_day_of_week = 0;
uint16_t days_since_start = 0;  // Changed from static to global

/** Last read time to detect day changes */
static uint8_t last_day = 0;

/** RTC error count for tracking reliability */
static uint8_t rtc_error_count = 0;
#define MAX_RTC_ERRORS 5

/** System time tracking for RTC fallback */
static uint32_t last_time_update = 0;

/* ------------------------------------------------------------------ */
/* Static storage for the currently running task (avoids dangling ptr) */
static watering_task_t active_task_storage;          /* NEW */
/* ------------------------------------------------------------------ */

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
    
    current_task_state = TASK_STATE_IDLE;
    watering_tasks_running = false;
    exit_tasks = false;
    rtc_error_count = 0;
    
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
    
    // Validate watering mode parameters
    if (task->channel->watering_event.watering_mode == WATERING_BY_VOLUME) {
        if (task->by_volume.volume_liters == 0) {
            return WATERING_ERROR_INVALID_PARAM;
        }
    }
    
    if (k_msgq_put(&watering_tasks_queue, task, K_NO_WAIT) != 0) {
        LOG_ERROR("Watering queue is full", WATERING_ERROR_QUEUE_FULL);
        watering_increment_error_tasks();  /* Track queue overflow errors */
        return WATERING_ERROR_QUEUE_FULL;
    }
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
    
    watering_error_t ret = watering_channel_on(channel_id);
    if (ret != WATERING_SUCCESS) {
        LOG_ERROR("Error activating channel for task", ret);
        return ret;
    }
    
    k_mutex_lock(&watering_state_mutex, K_FOREVER);

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
    current_task_state = TASK_STATE_RUNNING;
    
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
    k_mutex_lock(&watering_state_mutex, K_FOREVER);
    
    if (watering_task_state.current_active_task == NULL) {
        k_mutex_unlock(&watering_state_mutex);
        return false;  // No active task
    }
    
    uint8_t channel_id = watering_task_state.current_active_task->channel - watering_channels;
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
    uint16_t total_volume_ml = get_pulse_count() * 1000 / pulses_per_liter; // Convert to ml
    
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
    current_task_state = TASK_STATE_IDLE;
    
    k_mutex_unlock(&watering_state_mutex);
    
    /* Record task completion in history */
    #ifdef CONFIG_BT
    watering_history_record_task_complete(channel_id, actual_value, total_volume_ml, WATERING_SUCCESS_COMPLETE);
    
    // Notify BLE clients about history event
    bt_irrigation_history_notify_event(channel_id, WATERING_EVENT_COMPLETE, 
                                      k_uptime_get_32() / 1000, total_volume_ml);
    #endif
    
    /* Increment completed tasks counter */
    watering_increment_completed_tasks_count();
    
    /* Notify BLE clients about task completion */
    #ifdef CONFIG_BT
    bt_irrigation_current_task_update(0xFF, 0, 0, 0, 0, 0); // No active task
    #endif
    
    return true;
}

/**
 * @brief Check active tasks for completion or issues
 * 
 * @return 1 if tasks are active, 0 if idle, negative error code on failure
 */
int watering_check_tasks(void) {
    if (k_mutex_lock(&watering_state_mutex, K_NO_WAIT) != 0) {
        return 0;   /* skip this cycle if busy */
    }
    
    __attribute__((unused)) uint32_t start_time = k_uptime_get_32();
    watering_error_t flow_check_result = check_flow_anomalies();
    
    if (flow_check_result != WATERING_SUCCESS && flow_check_result != WATERING_ERROR_BUSY) {
        watering_increment_error_tasks();  /* Track flow anomaly errors */
        k_mutex_unlock(&watering_state_mutex);
        return -flow_check_result;
    }

    if (system_status == WATERING_STATUS_FAULT) {
        k_mutex_unlock(&watering_state_mutex);
        return -WATERING_ERROR_BUSY;
    }
    
    __attribute__((unused)) uint32_t task_start_time = k_uptime_get_32();
    
    if (watering_task_state.current_active_task != NULL) {
        watering_channel_t *channel = watering_task_state.current_active_task->channel;
        watering_event_t *event = &channel->watering_event;
        
        bool task_complete = false;
        uint32_t current_time = k_uptime_get_32();
        uint32_t elapsed_ms = current_time - watering_task_state.watering_start_time;
        
        if (event->watering_mode == WATERING_BY_DURATION) {
            uint32_t duration_ms = event->watering.by_duration.duration_minutes * 60000;
            if (elapsed_ms >= duration_ms) {
                task_complete = true;
                printk("Duration task complete after %u ms\n", elapsed_ms);
            }
        } else if (event->watering_mode == WATERING_BY_VOLUME) {
            uint32_t pulses = get_pulse_count();
            uint32_t pulses_per_liter;
            watering_get_flow_calibration(&pulses_per_liter);
            
            uint32_t target_volume_ml = event->watering.by_volume.volume_liters * 1000;
            uint32_t pulses_target = (target_volume_ml * pulses_per_liter) / 1000;
            
            if (pulses >= pulses_target) {
                task_complete = true;
                printk("Volume task complete: %u pulses\n", pulses);
            }
            
            if (elapsed_ms > 30 * 60000) {
                printk("Volume task timed out (safety limit)\n");
                task_complete = true;
            }
        }
        
        if (task_complete) {
            uint8_t channel_id = watering_task_state.current_active_task->channel - watering_channels;
            watering_channel_off(channel_id);
            
            // Save completed task information for BLE reporting
            last_completed_task.task = watering_task_state.current_active_task;
            last_completed_task.start_time = watering_task_state.watering_start_time;
            last_completed_task.completion_time = k_uptime_get_32();
            last_completed_task.valid = true;
            
            watering_task_state.current_active_task = NULL;
            watering_task_state.task_in_progress = false;
            current_task_state = TASK_STATE_COMPLETED;
            
            k_mutex_unlock(&watering_state_mutex);
            
            /* Increment completed tasks counter */
            watering_increment_completed_tasks_count();
            
            /* Notify BLE clients about task completion */
            #ifdef CONFIG_BT
            bt_irrigation_current_task_update(0xFF, 0, 0, 0, 0, 0); // No active task
            #endif
            
            return 1;
        }
    }
    
    if (current_task_state != TASK_STATE_RUNNING) {
        int result = watering_process_next_task();
        
        if (result < 0) {
            k_mutex_unlock(&watering_state_mutex);
            return result;
        }
    }
    
    k_mutex_unlock(&watering_state_mutex);
    return (watering_task_state.current_active_task != NULL) ? 1 : 0;
}

/**
 * @brief Clean up completed tasks and release resources
 * 
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_cleanup_tasks(void) {
    k_mutex_lock(&watering_state_mutex, K_FOREVER);
    
    if (current_task_state == TASK_STATE_COMPLETED && watering_task_state.current_active_task != NULL) {
        watering_task_state.current_active_task = NULL;
        watering_task_state.task_in_progress = false;
        current_task_state = TASK_STATE_IDLE;
        
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
    
    k_mutex_lock(&watering_state_mutex, K_FOREVER);
    pulses_per_liter = new_pulses_per_liter;
    k_mutex_unlock(&watering_state_mutex);
    
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
    
    k_mutex_lock(&watering_state_mutex, K_FOREVER);
    *pulses_per_liter_out = pulses_per_liter;
    k_mutex_unlock(&watering_state_mutex);
    
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
        current_hour = now.hour;
        current_minute = now.minute;
        current_day_of_week = now.day_of_week;
        last_day = now.day;
        printk("Current time from RTC: %02d:%02d, day %d\n", 
               current_hour, current_minute, current_day_of_week);
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
        
        if (rtc_status == 0) {
            if (rtc_datetime_get(&now) == 0) {
                current_hour = now.hour;
                current_minute = now.minute;
                current_day_of_week = now.day_of_week;
                rtc_read_success = true;
                
                if (rtc_error_count > 0) {
                    rtc_error_count--;
                }
                
                if (now.day != last_day) {
                    days_since_start++;
                    last_day = now.day;
                    watering_save_config();
                    printk("Day changed, days since start: %d\n", days_since_start);
                }
            } else {
                handle_rtc_failure();
            }
        } else {
            update_system_time();
            rtc_read_success = true;
        }
        
        if (rtc_read_success) {
            watering_scheduler_run();
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
    printk("Running watering scheduler [time %02d:%02d, day %d]\n", 
           current_hour, current_minute, current_day_of_week);
    
    if (system_status == WATERING_STATUS_FAULT || system_status == WATERING_STATUS_RTC_ERROR) {
        return WATERING_ERROR_BUSY;
    }
    
    if (current_hour > 23 || current_minute > 59 || current_day_of_week > 6) {
        LOG_ERROR("Invalid time values in scheduler", WATERING_ERROR_INVALID_PARAM);
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    for (int i = 0; i < WATERING_CHANNELS_COUNT; i++) {
        watering_channel_t *channel = &watering_channels[i];
        watering_event_t *event = &channel->watering_event;
        
        if (!event->auto_enabled) {
            continue;
        }
        
        bool should_run = false;
        if (event->start_time.hour == current_hour && event->start_time.minute == current_minute) {
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
            }
        }
        
        if (should_run) {
            watering_task_t new_task;
            new_task.channel = channel;
            new_task.trigger_type = WATERING_TRIGGER_SCHEDULED;  // Scheduled tasks
            
            if (event->watering_mode == WATERING_BY_DURATION) {
                new_task.by_time.start_time = k_uptime_get_32();
            } else {
                new_task.by_volume.volume_liters = event->watering.by_volume.volume_liters;
            }
            
            watering_error_t result = watering_add_task(&new_task);
            if (result == WATERING_SUCCESS) {
                channel->last_watering_time = k_uptime_get_32();
                printk("Watering schedule added for channel %d (added to task queue)\n", i + 1);
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
    
    k_mutex_lock(&watering_state_mutex, K_FOREVER);
    
    while (k_msgq_get(&watering_tasks_queue, &dummy_task, K_NO_WAIT) == 0) {
        count++;
    }
    
    if (count > 0) {
        k_msgq_purge(&watering_tasks_queue);
    }
    
    printk("%d tasks removed from queue\n", count);
    
    k_mutex_unlock(&watering_state_mutex);
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
    
    k_mutex_lock(&watering_state_mutex, K_FOREVER);
    
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
    
    /* We can't easily peek into the message queue without removing items,
     * so for now return a simple indication that there is a next task.
     * In a real implementation, we might maintain a separate queue
     * data structure that allows peeking. */
    
    /* For the BLE interface, we just need to know if there are pending tasks,
     * which we can get from the queue count */
    return (watering_task_t *)1; /* Non-NULL indicates there is a pending task */
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
 * @return true if a task was paused, false if no task was running or task cannot be paused
 */
bool watering_pause_current_task(void) {
    k_mutex_lock(&watering_state_mutex, K_FOREVER);
    
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
    k_mutex_lock(&watering_state_mutex, K_FOREVER);
    
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
    k_mutex_lock(&watering_state_mutex, K_FOREVER);
    bool paused = watering_task_state.task_paused;
    k_mutex_unlock(&watering_state_mutex);
    return paused;
}
