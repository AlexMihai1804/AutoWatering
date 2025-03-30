#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include "flow_sensor.h"
#include "watering.h"
#include "watering_internal.h"
#include "rtc.h" // Add RTC header

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
struct watering_task_state_t watering_task_state = {NULL, 0, false};

/** Flow pulse count at task start */
static uint32_t initial_pulse_count = 0;

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

/** Buffer for active task data */
static watering_task_t active_task_buffer;

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

/**
 * @brief Initialize the task management system
 */
watering_error_t tasks_init(void) {
    watering_task_state.current_active_task = NULL;
    watering_task_state.watering_start_time = 0;
    watering_task_state.task_in_progress = false;
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
        return WATERING_ERROR_QUEUE_FULL;
    }
    
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
    
    uint8_t channel_id = task.channel - watering_channels;
    printk("Processing watering task for channel %d\n", channel_id + 1);
    
    watering_error_t ret = watering_channel_on(channel_id);
    if (ret != WATERING_SUCCESS) {
        LOG_ERROR("Failed to activate channel for task", ret);
        return -ret;
    }
    
    return 1;
}

/**
 * @brief Start execution of a watering task
 * 
 * @param task Task to be executed
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_start_task(watering_task_t *task) {
    if (task == NULL || task->channel == NULL) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
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
    
    watering_task_state.watering_start_time = k_uptime_get_32();
    watering_task_state.task_in_progress = true;
    
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
    
    uint32_t duration_ms = k_uptime_get_32() - watering_task_state.watering_start_time;
    printk("Stopping watering for channel %d after %d seconds\n", 
           channel_id + 1, duration_ms / 1000);
    
    watering_task_state.current_active_task = NULL;
    watering_task_state.task_in_progress = false;
    current_task_state = TASK_STATE_IDLE;
    
    k_mutex_unlock(&watering_state_mutex);
    return true;
}

/**
 * @brief Check active tasks for completion or issues
 * 
 * @return 1 if tasks are active, 0 if idle, negative error code on failure
 */
int watering_check_tasks(void) {
    watering_error_t flow_check_result = check_flow_anomalies();
    if (flow_check_result != WATERING_SUCCESS && flow_check_result != WATERING_ERROR_BUSY) {
        return -flow_check_result;
    }
    
    k_mutex_lock(&watering_state_mutex, K_FOREVER);
    
    if (system_status == WATERING_STATUS_FAULT) {
        k_mutex_unlock(&watering_state_mutex);
        return -WATERING_ERROR_BUSY;
    }
    
    if (watering_task_state.current_active_task != NULL) {
        watering_event_t *event = &watering_task_state.current_active_task->channel->watering_event;
        uint8_t channel_id = watering_task_state.current_active_task->channel - watering_channels;
        if (event->watering_mode == WATERING_BY_DURATION) {
            uint32_t elapsed_ms = k_uptime_get_32() - watering_task_state.watering_start_time;
            uint32_t duration_ms = (uint32_t) event->watering.by_duration.duration_minutes * 60 * 1000;
            if (elapsed_ms >= duration_ms) {
                printk("Watering duration reached for channel %d: %d minutes\n", 
                       channel_id + 1, event->watering.by_duration.duration_minutes);
                watering_stop_current_task();
                current_task_state = TASK_STATE_COMPLETED;
            } else {
                k_mutex_unlock(&watering_state_mutex);
                return 1;  // Task still in progress
            }
        } else if (event->watering_mode == WATERING_BY_VOLUME) {
            uint32_t current_pulses = get_pulse_count();
            uint32_t total_pulses = initial_pulse_count + current_pulses;
            uint32_t required_pulses = 
                (uint32_t) watering_task_state.current_active_task->by_volume.volume_liters * pulses_per_liter;
            if (total_pulses >= required_pulses) {
                printk("Watering volume reached for channel %d: %d liters (pulses: %d/%d)\n", 
                       channel_id + 1, watering_task_state.current_active_task->by_volume.volume_liters, 
                       total_pulses, required_pulses);
                watering_stop_current_task();
                current_task_state = TASK_STATE_COMPLETED;
            } else {
                // Update initial pulse count BUT DON'T reset pulse counter to avoid missing pulses
                initial_pulse_count += current_pulses;
                reset_pulse_count();  // Reset for next reading
                float volume_dispensed = (float) total_pulses / pulses_per_liter;
                float target_volume = watering_task_state.current_active_task->by_volume.volume_liters;
                int progress_percent = (int) (volume_dispensed * 100 / target_volume);
                if (progress_percent % 10 == 0) {
                    printk("Channel %d: Watering progress %d%% (%.1f / %d liters)\n", 
                           channel_id + 1, progress_percent, (double) volume_dispensed,
                           watering_task_state.current_active_task->by_volume.volume_liters);
                }
                k_mutex_unlock(&watering_state_mutex);
                return 1;
            }
        }
    }
    
    if (current_task_state != TASK_STATE_RUNNING) {
        if (system_status != WATERING_STATUS_OK && system_status != WATERING_STATUS_FAULT) {
            k_mutex_unlock(&watering_state_mutex);
            return 0;
        }
        
        // Get next task from queue
        watering_task_t next_task;
        if (k_msgq_get(&watering_tasks_queue, &next_task, K_NO_WAIT) == 0) {
            // Make a local copy of the task
            memcpy(&active_task_buffer, &next_task, sizeof(watering_task_t));
            watering_task_state.current_active_task = &active_task_buffer;
            
            // Start the task
            k_mutex_unlock(&watering_state_mutex);
            watering_start_task(watering_task_state.current_active_task);
            return 1;
        }

        if (current_task_state == TASK_STATE_COMPLETED) {
            current_task_state = TASK_STATE_IDLE;
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
        
        // Try to initialize one final time with extended timeout
        k_sleep(K_MSEC(100));
        if (rtc_init() == 0 && rtc_is_available()) {
            printk("Final RTC recovery attempt successful\n");
            rtc_error_count = MAX_RTC_ERRORS - 1;
            return WATERING_SUCCESS;
        }
        
        return WATERING_ERROR_RTC_FAILURE;
    }
    
    // Try to re-initialize RTC
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
    
    // Calculate actual elapsed time and update accordingly
    if (now >= last_time_update) {  // Handle uint32_t overflow
        uint32_t elapsed_ms = now - last_time_update;
        uint32_t elapsed_minutes = elapsed_ms / 60000;  // Convert ms to minutes using the intended interval
        
        if (elapsed_minutes > 0) {
            // Add the actual number of minutes that passed
            current_minute += elapsed_minutes;
            
            // Handle minute overflow
            while (current_minute >= 60) {
                current_minute -= 60;
                current_hour++;
                
                if (current_hour >= 24) {
                    current_hour = 0;
                    
                    // Update day of week (0=Sunday, 6=Saturday)
                    current_day_of_week = (current_day_of_week + 1) % 7;
                    
                    // Update the days counter
                    days_since_start++;
                    watering_save_config();
                    
                    // Update the last_day to detect day change
                    last_day = (last_day % 31) + 1;
                    printk("Day changed (system time), days since start: %d\n", days_since_start);
                }
            }
            
            // Update only when we've processed the elapsed time
            last_time_update = now - (elapsed_ms % 60000);
        }
    } else {
        // Handle timer overflow
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
        
        // Sleep duration based on power mode
        uint32_t sleep_time = 500;
        switch (current_power_mode) {
            case POWER_MODE_NORMAL:
                sleep_time = 500;
                break;
            case POWER_MODE_ENERGY_SAVING:
                sleep_time = 1000;
                break;
            case POWER_MODE_ULTRA_LOW_POWER:
                sleep_time = 2000;
                break;
        }
        
        k_sleep(K_MSEC(sleep_time));
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
    
    // Initialize RTC access
    int rtc_status = rtc_init();
    if (rtc_status != 0) {
        printk("ERROR: Failed to initialize RTC. Will use system time instead.\n");
        system_status = WATERING_STATUS_RTC_ERROR;
    } else {
        printk("RTC initialized successfully\n");
    }
    
    // Initial time read
    rtc_datetime_t now;
    if (rtc_status == 0 && rtc_datetime_get(&now) == 0) {
        current_hour = now.hour;
        current_minute = now.minute;
        current_day_of_week = now.day_of_week;
        last_day = now.day;
        printk("Current time from RTC: %02d:%02d, day %d\n", 
               current_hour, current_minute, current_day_of_week);
    } else {
        // If RTC isn't available, use system time as fallback
        printk("Using system time as fallback\n");
        // Set simple values for initial running
        current_hour = 12;
        current_minute = 0;
        current_day_of_week = 1;
        last_day = 1;
        
        // Initialize the last update time
        last_time_update = k_uptime_get_32();
    }
    
    while (!exit_tasks) {
        bool rtc_read_success = false;
        
        // Check if RTC is available and update time
        if (rtc_status == 0) {
            if (rtc_datetime_get(&now) == 0) {
                current_hour = now.hour;
                current_minute = now.minute;
                current_day_of_week = now.day_of_week;
                rtc_read_success = true;
                
                // Reset error count on successful read
                if (rtc_error_count > 0) {
                    rtc_error_count--;
                }
                
                // Detect day change to increment days_since_start
                if (now.day != last_day) {
                    days_since_start++;
                    last_day = now.day;
                    // Save days_since_start immediately to prevent loss on power failure
                    watering_save_config();
                    printk("Day changed, days since start: %d\n", days_since_start);
                }
            } else {
                // Handle RTC failure
                handle_rtc_failure();
            }
        } else {
            // RTC not available, use internal time tracking
            update_system_time();
            rtc_read_success = true; // We'll use our system time
        }
        
        // Only run scheduler if we have valid time data
        if (rtc_read_success) {
            watering_scheduler_run();
        }
        
        // Sleep duration based on power mode
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
    
    // Validate time values
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
                    days_since_start > 0 &&  // Avoid triggering on first day
                    (days_since_start % event->schedule.periodic.interval_days) == 0) {
                    should_run = true;
                }
            }
        }
        
        if (should_run) {
            watering_task_t new_task;
            new_task.channel = channel;
            
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
    
    // Create watering task thread
    k_tid_t watering_tid =
        k_thread_create(&watering_task_data, watering_task_stack, K_THREAD_STACK_SIZEOF(watering_task_stack),
                        watering_task_fn, NULL, NULL, NULL, K_PRIO_PREEMPT(5), 0, K_NO_WAIT);
    if (watering_tid == NULL) {
        LOG_ERROR("Error creating watering processing task", WATERING_ERROR_CONFIG);
        return WATERING_ERROR_CONFIG;
    }
    
    // Create scheduler thread
    k_tid_t scheduler_tid =
        k_thread_create(&scheduler_task_data, scheduler_task_stack, K_THREAD_STACK_SIZEOF(scheduler_task_stack),
                        scheduler_task_fn, NULL, NULL, NULL, K_PRIO_PREEMPT(7), 0, K_NO_WAIT);
    if (scheduler_tid == NULL) {
        LOG_ERROR("Error creating scheduler task", WATERING_ERROR_CONFIG);
        // Cleanup the watering thread
        exit_tasks = true;
        k_thread_abort(watering_tid);
        return WATERING_ERROR_CONFIG;
    }
    
    // Set thread names for debugging
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
    
    // Ensure all channels are turned off
    for (int i = 0; i < WATERING_CHANNELS_COUNT; i++) {
        watering_channel_off(i);
    }
    
    watering_tasks_running = false;
    printk("Watering tasks stopped\n");
    return WATERING_SUCCESS;
}

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
    
    // Validate schedule type
    if (event->schedule_type != SCHEDULE_DAILY && event->schedule_type != SCHEDULE_PERIODIC) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    // Validate watering mode
    if (event->watering_mode != WATERING_BY_DURATION && event->watering_mode != WATERING_BY_VOLUME) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    // Validate schedule parameters
    if (event->schedule_type == SCHEDULE_DAILY) {
        // At least one day of week should be selected
        if (event->schedule.daily.days_of_week == 0) {
            return WATERING_ERROR_INVALID_PARAM;
        }
    } else {
        // Periodic schedule should have interval > 0
        if (event->schedule.periodic.interval_days == 0) {
            return WATERING_ERROR_INVALID_PARAM;
        }
    }
    
    // Validate watering parameters
    if (event->watering_mode == WATERING_BY_DURATION) {
        if (event->watering.by_duration.duration_minutes == 0) {
            return WATERING_ERROR_INVALID_PARAM;
        }
    } else {
        if (event->watering.by_volume.volume_liters == 0) {
            return WATERING_ERROR_INVALID_PARAM;
        }
    }
    
    // Validate start time
    if (event->start_time.hour > 23 || event->start_time.minute > 59) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    return WATERING_SUCCESS;
}

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
    
    // Validate parameters
    if (channel_id >= WATERING_CHANNELS_COUNT || minutes == 0) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    // Get channel reference
    err = watering_get_channel(channel_id, &channel);
    if (err != WATERING_SUCCESS) {
        printk("Error getting channel %d: %d\n", channel_id, err);
        return err;
    }
    
    // Configure task
    new_task.channel = channel;
    new_task.channel->watering_event.watering_mode = WATERING_BY_DURATION;
    new_task.channel->watering_event.watering.by_duration.duration_minutes = minutes;
    new_task.by_time.start_time = k_uptime_get_32();
    
    // Add task to queue
    printk("Adding %d minute watering task for channel %d\n", 
           minutes, channel_id + 1);
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
    
    // Validate parameters
    if (channel_id >= WATERING_CHANNELS_COUNT || liters == 0) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    // Get channel reference
    err = watering_get_channel(channel_id, &channel);
    if (err != WATERING_SUCCESS) {
        printk("Error getting channel %d: %d\n", channel_id, err);
        return err;
    }
    
    // Configure task
    new_task.channel = channel;
    new_task.channel->watering_event.watering_mode = WATERING_BY_VOLUME;
    new_task.channel->watering_event.watering.by_volume.volume_liters = liters;
    new_task.by_volume.volume_liters = liters;
    
    // Add task to queue
    printk("Adding %d liter watering task for channel %d\n", 
           liters, channel_id + 1);
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
    
    // Lock to protect the queue
    k_mutex_lock(&watering_state_mutex, K_FOREVER);
    
    // Try to empty queue by extracting tasks until empty
    while (k_msgq_get(&watering_tasks_queue, &dummy_task, K_NO_WAIT) == 0) {
        count++;
    }
    
    // If necessary, we could rebuild the empty queue to fully reset its state
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
    // Use Zephyr function to get number of used messages in queue
    return k_msgq_num_used_get(&watering_tasks_queue);
}

/**
 * @brief Structure for pending task information
 * Used to store information about tasks
 */
typedef struct {
    uint8_t channel_id;
    uint8_t task_type;   // 0=duration, 1=volume
    uint16_t value;      // minutes or liters
} watering_task_info_t;

/**
 * @brief Get information about pending tasks
 * 
 * @param tasks_info Buffer for task information (watering_task_info_t[])
 * @param max_tasks Maximum buffer size
 * @return Number of tasks copied to buffer
 */
int watering_get_pending_tasks_info(void *tasks_info, int max_tasks) {
    // This implementation is more complex because we can't iterate through a message queue
    // without removing messages. An alternative approach would be to copy the queue to a temporary buffer
    // and then rebuild it.
    
    // For simplicity, we'll return 0 and leave a full implementation for the future
    // when the queue data structure can be modified to support iteration.
    printk("watering_get_pending_tasks_info function not yet fully implemented\n");
    
    // TODO: Full implementation when queue structure is modified to allow iteration
    return 0;
}
