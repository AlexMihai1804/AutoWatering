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

/**
 * @brief Structure to track the currently active watering task
 */
struct watering_task_state_t {
    watering_task_t *current_active_task;  /**< Currently executing task or NULL */
    uint32_t watering_start_time;          /**< Timestamp when watering started */
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

/**
 * @brief Initialize task management system
 */
void tasks_init(void);

/**
 * @brief Start execution of a watering task
 * 
 * @param task Task to start executing
 */
void watering_start_task(watering_task_t *task);

/**
 * @brief Stop the currently active watering task
 * 
 * @return true if a task was stopped, false if no active task
 */
bool watering_stop_current_task(void);

/**
 * @brief Initialize flow monitoring system
 */
void flow_monitor_init(void);

/**
 * @brief Check for flow anomalies and update system status
 */
void check_flow_anomalies(void);

/**
 * @brief Initialize the configuration system
 */
void config_init(void);

#endif // WATERING_INTERNAL_H
