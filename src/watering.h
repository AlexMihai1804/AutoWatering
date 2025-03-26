#ifndef WATERING_H
#define WATERING_H

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/drivers/gpio.h>

/* Number of watering channels */
#define WATERING_CHANNELS_COUNT 8

/* Schedule types */
typedef enum { SCHEDULE_DAILY, SCHEDULE_PERIODIC } schedule_type_t;

/* Watering measurement modes */
typedef enum watering_mode { WATERING_BY_DURATION, WATERING_BY_VOLUME } watering_mode_t;

/* Watering event structure */
typedef struct watering_event_t {
    schedule_type_t schedule_type;
    watering_mode_t watering_mode;
    union {
        struct {
            uint8_t days_of_week; // Bitmask: bit 0 = Sunday, 1 = Monday, etc.
        } daily;
        struct {
            uint8_t interval_days;
        } periodic;
    } schedule;
    union {
        struct {
            uint8_t duration_minutes;
        } by_duration;
        struct {
            uint16_t volume_liters;
        } by_volume;
    } watering;
    struct {
        uint8_t hour;
        uint8_t minute;
    } start_time;
    bool auto_enabled;
} watering_event_t;

/* Watering channel structure */
typedef struct {
    watering_event_t watering_event;
    uint32_t last_watering_time;
    char name[64];
    /* GPIO specification from Device Tree for valve control */
    struct gpio_dt_spec valve;
} watering_channel_t;

/* Structure for a watering task in queue */
typedef struct {
    watering_channel_t *channel;
    union {
        struct {
            uint32_t start_time;
        } by_time;
        struct {
            uint32_t volume_liters;
        } by_volume;
    };
} watering_task_t;

/* Status codes for anomalies */
typedef enum {
    WATERING_STATUS_OK = 0,
    WATERING_STATUS_NO_FLOW = 1, /* No flow with valves open */
    WATERING_STATUS_UNEXPECTED_FLOW = 2, /* Flow detected with valves closed */
    WATERING_STATUS_FAULT = 3 /* Permanent error state */
} watering_status_t;

/**
 * @brief Initialize the watering module.
 */
void watering_init(void);

/**
 * @brief Start dedicated watering system tasks
 * This creates threads for task monitoring and scheduling
 *
 * @return int 0 on success, error code on failure
 */
int watering_start_tasks(void);

/**
 * @brief Stop dedicated watering system tasks
 */
void watering_stop_tasks(void);

/**
 * @brief Get reference to a watering channel
 *
 * @param channel_id Channel ID (0-7)
 * @return watering_channel_t* Pointer to the watering channel, NULL if invalid
 */
watering_channel_t *watering_get_channel(uint8_t channel_id);

/**
 * @brief Activate a watering channel
 *
 * @param channel_id Channel ID (0-7)
 * @return int 0 on success, error code on failure
 */
int watering_channel_on(uint8_t channel_id);

/**
 * @brief Deactivate a watering channel
 *
 * @param channel_id Channel ID (0-7)
 * @return int 0 on success, error code on failure
 */
int watering_channel_off(uint8_t channel_id);

/**
 * @brief Add a task to the watering queue
 *
 * @param task The watering task
 * @return int 0 on success, -1 on error
 */
int watering_add_task(watering_task_t *task);

/**
 * @brief Process the next task from the queue
 *
 * @return int 0 if no tasks, 1 if a task was processed
 */
int watering_process_next_task(void);

/**
 * @brief Check schedules and add tasks to queue if needed
 * This function should be called periodically, e.g., once a minute
 */
void watering_scheduler_run(void);

/**
 * @brief Check and process watering tasks
 *
 * This function should be called periodically to:
 * 1. Check if there's an active task and monitor its progress
 * 2. If no active task, start the next one from the queue
 *
 * @return int 1 if a task is active, 0 otherwise
 */
int watering_check_tasks(void);

/**
 * @brief Free resources when a task is done
 * Should be called periodically to clean up completed tasks
 */
void watering_cleanup_tasks(void);

/**
 * @brief Set flow sensor calibration
 *
 * @param pulses_per_liter Number of pulses generated per liter of water
 */
void watering_set_flow_calibration(uint32_t pulses_per_liter);

/**
 * @brief Get current calibration value for flow sensor
 *
 * @return uint32_t Current number of pulses per liter
 */
uint32_t watering_get_flow_calibration(void);

/**
 * @brief Save configurations to persistent memory
 *
 * @return int 0 on success, error code on failure
 */
int watering_save_config(void);

/**
 * @brief Load configurations from persistent memory
 *
 * @return int 0 on success, error code on failure
 */
int watering_load_config(void);

/**
 * @brief Get current status of the watering system
 *
 * @return watering_status_t Status code of the system
 */
watering_status_t watering_get_status(void);

/**
 * @brief Reset system status after fixing errors
 *
 * @return int 0 if reset was successful, otherwise error code
 */
int watering_reset_fault(void);

#endif // WATERING_H
