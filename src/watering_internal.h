#ifndef WATERING_INTERNAL_H
#define WATERING_INTERNAL_H
#include "watering.h"

/**
 * @file watering_internal.h
 * @brief Internal definitions and shared data for the watering system
 * 
 * This header contains definitions and declarations that are shared between
 * multiple source files but are not part of the public API.
 */

/** Global array of all watering channels */
extern watering_channel_t watering_channels[WATERING_CHANNELS_COUNT];

/** Current system status */
extern watering_status_t system_status;

/** Current system state */
extern watering_state_t system_state;

/** Current power mode */
extern power_mode_t current_power_mode;

/** System initialized flag */
extern bool system_initialized;

/** Current time tracking for scheduler */
extern uint16_t days_since_start;

/**
 * @brief Structure to track the currently active watering task
 */
struct watering_task_state_t {
    watering_task_t *current_active_task;  /**< Currently executing task or NULL */
    uint32_t watering_start_time;          /**< Timestamp when watering started */
    bool task_in_progress;                 /**< Flag indicating task is in progress */
};

/** Global state of active watering tasks */
extern struct watering_task_state_t watering_task_state;

/** Default flow sensor calibration (pulses per liter) */
#define DEFAULT_PULSES_PER_LITER 750

/** Minimum time between flow checks in milliseconds */
#define FLOW_CHECK_THRESHOLD_MS 5000

/** Maximum number of flow error attempts before entering fault state */
#define MAX_FLOW_ERROR_ATTEMPTS 3

/** Threshold of pulses that indicates unexpected flow */
#define UNEXPECTED_FLOW_THRESHOLD 10

/** Timeout duration for state transition in milliseconds */
#define STATE_TRANSITION_TIMEOUT_MS 10000

/** Error logging helper macro with file and line info */
#define LOG_ERROR(msg, err) log_error_with_info(msg, err, __FILE__, __LINE__)

/**
 * @brief Initialize task management system
 * 
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t tasks_init(void);

/**
 * @brief Start execution of a watering task
 * 
 * @param task Task to start executing
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_start_task(watering_task_t *task);

/**
 * @brief Stop the currently active watering task
 * 
 * @return true if a task was stopped, false if no active task
 */
bool watering_stop_current_task(void);

/**
 * @brief Initialize flow monitoring system
 * 
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t flow_monitor_init(void);

/**
 * @brief Check for flow anomalies and update system status
 * 
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t check_flow_anomalies(void);

/**
 * @brief Initialize the configuration system
 * 
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t config_init(void);

/**
 * @brief Log error with file and line information
 * 
 * @param message Error message
 * @param error_code Error code
 * @param file File where error occurred
 * @param line Line where error occurred
 */
void log_error_with_info(const char *message, int error_code, const char *file, int line);

/**
 * @brief Attempt recovery from system errors
 * 
 * @param error_code Error code to recover from
 * @return WATERING_SUCCESS if recovery successful, error code if not
 */
watering_error_t attempt_error_recovery(watering_error_t error_code);

/**
 * @brief Transition system to a new state
 * 
 * @param new_state State to transition to
 * @return WATERING_SUCCESS if transition successful, error code if not
 */
watering_error_t transition_to_state(watering_state_t new_state);

/**
 * @brief Update system timing based on power mode
 * 
 * @param mode Current power mode
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t update_power_timings(power_mode_t mode);

/**
 * @brief Clean up resources for graceful shutdown
 */
void cleanup_resources(void);

/**
 * @brief Initialize all valve hardware
 * 
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t valve_init(void);

/**
 * @brief Close all valves (safety function)
 * 
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t valve_close_all(void);

/**
 * @brief Clear the pending task queue
 * 
 * @return Number of tasks removed
 */
int watering_clear_task_queue(void);

/**
 * @brief Get the number of pending tasks
 * 
 * @return Number of pending tasks
 */
int watering_get_pending_tasks_count(void);

/**
 * @brief Get information about pending tasks
 * 
 * @param tasks_info Buffer for task information
 * @param max_tasks Maximum buffer size
 * @return Number of tasks copied to buffer
 */
int watering_get_pending_tasks_info(void *tasks_info, int max_tasks);

#endif // WATERING_INTERNAL_H
