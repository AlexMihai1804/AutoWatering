#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include "flow_sensor.h"
#include "watering.h"
#include "watering_internal.h"

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
struct watering_task_state_t watering_task_state = {NULL, 0};

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
static uint16_t days_since_start = 0;

/**
 * @brief Initialize the task management system
 */
void tasks_init(void) {
    watering_task_state.current_active_task = NULL;
    watering_task_state.watering_start_time = 0;
    current_task_state = TASK_STATE_IDLE;
    watering_tasks_running = false;
    exit_tasks = false;
}

/**
 * @brief Add a watering task to the task queue
 * 
 * @param task Task to be added to the queue
 * @return 0 on success, -1 if queue is full
 */
int watering_add_task(watering_task_t *task) {
    if (k_msgq_put(&watering_tasks_queue, task, K_NO_WAIT) != 0) {
        printk("Error: Watering queue is full!\n");
        return -1;
    }
    return 0;
}

/**
 * @brief Process the next task in the queue
 * 
 * @return 1 if task was processed, 0 if no tasks, negative on error
 */
int watering_process_next_task(void) {
    watering_task_t task;
    if (k_msgq_get(&watering_tasks_queue, &task, K_NO_WAIT) != 0) {
        return 0;  // No tasks in queue
    }
    uint8_t channel_id = task.channel - watering_channels;
    printk("Processing watering task for channel %d\n", channel_id + 1);
    watering_channel_on(channel_id);
    return 1;
}

/**
 * @brief Start execution of a watering task
 * 
 * @param task Task to be executed
 */
void watering_start_task(watering_task_t *task) {
    uint8_t channel_id = task->channel - watering_channels;
    if (watering_channel_on(channel_id) != 0) {
        printk("Error activating channel for task\n");
        return;
    }
    watering_task_state.watering_start_time = k_uptime_get_32();
    
    k_mutex_lock(&watering_state_mutex, K_FOREVER);
    if (task->channel->watering_event.watering_mode == WATERING_BY_VOLUME) {
        reset_pulse_count();
        initial_pulse_count = 0;
        printk("Started volumetric watering for channel %d: %d liters\n", channel_id + 1,
               task->by_volume.volume_liters);
    } else {
        printk("Started timed watering for channel %d: %d minutes\n", channel_id + 1,
               task->channel->watering_event.watering.by_duration.duration_minutes);
    }
    watering_task_state.current_active_task = task;
    current_task_state = TASK_STATE_RUNNING;
    k_mutex_unlock(&watering_state_mutex);
}

/**
 * @brief Stop the currently running task
 * 
 * @return true if a task was stopped, false if no active task
 */
bool watering_stop_current_task(void) {
    if (watering_task_state.current_active_task == NULL) {
        return false;  // No active task
    }
    
    uint8_t channel_id = watering_task_state.current_active_task->channel - watering_channels;
    watering_channel_off(channel_id);
    uint32_t duration_ms = k_uptime_get_32() - watering_task_state.watering_start_time;
    printk("Stopping watering for channel %d after %d seconds\n", channel_id + 1, duration_ms / 1000);
    watering_task_state.current_active_task = NULL;
    current_task_state = TASK_STATE_IDLE;
    return true;
}

int watering_check_tasks(void) {
    check_flow_anomalies();
    k_mutex_lock(&watering_state_mutex, K_FOREVER);
    if (system_status == WATERING_STATUS_FAULT) {
        k_mutex_unlock(&watering_state_mutex);
        return 0;
    }
    if (watering_task_state.current_active_task != NULL) {
        watering_event_t *event = &watering_task_state.current_active_task->channel->watering_event;
        uint8_t channel_id = watering_task_state.current_active_task->channel - watering_channels;
        if (event->watering_mode == WATERING_BY_DURATION) {
            uint32_t elapsed_ms = k_uptime_get_32() - watering_task_state.watering_start_time;
            uint32_t duration_ms = (uint32_t) event->watering.by_duration.duration_minutes * 60 * 1000;
            if (elapsed_ms >= duration_ms) {
                printk("Watering duration reached for channel %d: %d minutes\n", channel_id + 1,
                       event->watering.by_duration.duration_minutes);
                watering_stop_current_task();
                current_task_state = TASK_STATE_COMPLETED;
            } else {
                k_mutex_unlock(&watering_state_mutex);
                return 1;
            }
        } else if (event->watering_mode == WATERING_BY_VOLUME) {
            uint32_t current_pulses = get_pulse_count();
            uint32_t total_pulses = initial_pulse_count + current_pulses;
            uint32_t required_pulses =
                    (uint32_t) watering_task_state.current_active_task->by_volume.volume_liters * pulses_per_liter;
            if (total_pulses >= required_pulses) {
                printk("Watering volume reached for channel %d: %d liters (pulses: %d/%d)\n", channel_id + 1,
                       watering_task_state.current_active_task->by_volume.volume_liters, total_pulses, required_pulses);
                watering_stop_current_task();
                current_task_state = TASK_STATE_COMPLETED;
            } else {
                initial_pulse_count += current_pulses;
                reset_pulse_count();
                float volume_dispensed = (float) total_pulses / pulses_per_liter;
                float target_volume = watering_task_state.current_active_task->by_volume.volume_liters;
                int progress_percent = (int) (volume_dispensed * 100 / target_volume);
                if (progress_percent % 10 == 0) {
                    printk("Channel %d: Watering progress %d%% (%.1f / %d liters)\n", channel_id + 1, progress_percent,
                           (double) volume_dispensed, watering_task_state.current_active_task->by_volume.volume_liters);
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
        watering_task_t next_task;
        if (k_msgq_get(&watering_tasks_queue, &next_task, K_NO_WAIT) == 0) {
            active_task_buffer = next_task;
            watering_task_state.current_active_task = &active_task_buffer;
            watering_start_task(watering_task_state.current_active_task);
            k_mutex_unlock(&watering_state_mutex);
            return 1;
        }

        if (current_task_state == TASK_STATE_COMPLETED) {
            current_task_state = TASK_STATE_IDLE;
        }
    }
    k_mutex_unlock(&watering_state_mutex);
    return (watering_task_state.current_active_task != NULL) ? 1 : 0;
}

void watering_cleanup_tasks(void) {
    if (current_task_state == TASK_STATE_COMPLETED && watering_task_state.current_active_task != NULL) {
        watering_task_state.current_active_task = NULL;
        current_task_state = TASK_STATE_IDLE;
    }
}

/**
 * @brief Update the flow sensor calibration
 * 
 * @param new_pulses_per_liter New calibration value in pulses per liter
 */
void watering_set_flow_calibration(uint32_t new_pulses_per_liter) {
    if (new_pulses_per_liter > 0) {
        pulses_per_liter = new_pulses_per_liter;
        printk("Flow sensor calibration updated: %d pulses per liter\n", pulses_per_liter);
    }
}

/**
 * @brief Get the current flow sensor calibration
 * 
 * @return Current calibration value in pulses per liter
 */
uint32_t watering_get_flow_calibration(void) { return pulses_per_liter; }

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
        k_sleep(K_MSEC(500));
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
    while (!exit_tasks) {
        watering_scheduler_run();
        k_sleep(K_SECONDS(60));
    }
    printk("Watering scheduler task stopped\n");
}

/**
 * @brief Run the watering scheduler to check for scheduled tasks
 */
void watering_scheduler_run(void) {
    printk("Running watering scheduler [time %02d:%02d, day %d]\n", current_hour, current_minute, current_day_of_week);
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
                if (days_since_start % event->schedule.periodic.interval_days == 0) {
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
            watering_add_task(&new_task);
            channel->last_watering_time = k_uptime_get_32();
            printk("Watering schedule added for channel %d\n", i + 1);
        }
    }
}

/**
 * @brief Start the background tasks for watering operations
 * 
 * @return 0 on success, negative error code on failure
 */
int watering_start_tasks(void) {
    if (watering_tasks_running) {
        printk("Watering tasks already running\n");
        return 0;
    }
    exit_tasks = false;
    
    // Create watering task thread
    k_tid_t watering_tid =
            k_thread_create(&watering_task_data, watering_task_stack, K_THREAD_STACK_SIZEOF(watering_task_stack),
                            watering_task_fn, NULL, NULL, NULL, K_PRIO_PREEMPT(5), 0, K_NO_WAIT);

    if (watering_tid == NULL) {
        printk("Error creating watering processing task\n");
        return -1;
    }
    
    // Create scheduler thread
    k_tid_t scheduler_tid =
            k_thread_create(&scheduler_task_data, scheduler_task_stack, K_THREAD_STACK_SIZEOF(scheduler_task_stack),
                            scheduler_task_fn, NULL, NULL, NULL, K_PRIO_PREEMPT(7), 0, K_NO_WAIT);
    if (scheduler_tid == NULL) {
        printk("Error creating scheduler task\n");
        return -2;
    }
    
    // Set thread names for debugging
    k_thread_name_set(watering_tid, "watering_task");
    k_thread_name_set(scheduler_tid, "scheduler_task");
    watering_tasks_running = true;
    printk("Watering tasks successfully started\n");
    return 0;
}

/**
 * @brief Stop all background watering tasks
 */
void watering_stop_tasks(void) {
    if (!watering_tasks_running) {
        return;
    }
    printk("Stopping watering tasks...\n");
    exit_tasks = true;
    k_sleep(K_SECONDS(1));
    watering_tasks_running = false;
    printk("Watering tasks stopped\n");
}
