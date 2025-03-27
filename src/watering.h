#ifndef WATERING_H
#define WATERING_H
#include <stdbool.h>
#include <stdint.h>
#include <zephyr/drivers/gpio.h>
/**
 * @file watering.h
 * @brief Main interface for the automatic irrigation system
 * 
 * This header defines the public API and data structures for controlling 
 * a multi-channel irrigation system with flow monitoring capabilities.
 */

/** Number of available watering channels in the system */
#define WATERING_CHANNELS_COUNT 8

/**
 * @brief Schedule type for automatic watering
 * 
 * Defines how watering events are scheduled over time.
 */
typedef enum { 
    SCHEDULE_DAILY,    /**< Schedule on specific days of the week */
    SCHEDULE_PERIODIC  /**< Schedule every N days */
} schedule_type_t;

/**
 * @brief Watering mode that determines how irrigation is measured
 */
typedef enum watering_mode { 
    WATERING_BY_DURATION, /**< Water for a specified time period */
    WATERING_BY_VOLUME    /**< Water until a specified volume is dispensed */
} watering_mode_t;

/**
 * @brief Complete definition of a watering event including scheduling and quantity
 */
typedef struct watering_event_t {
    schedule_type_t schedule_type;  /**< Type of schedule (daily or periodic) */
    watering_mode_t watering_mode;  /**< Mode of watering (duration or volume) */

    /** Schedule-specific parameters */
    union {
        struct {
            uint8_t days_of_week;   /**< Bitmask of days (bit 0=Sunday, 1=Monday, etc.) */
        } daily;

        struct {
            uint8_t interval_days;  /**< Number of days between watering events */
        } periodic;
    } schedule;

    /** Watering quantity-specific parameters */
    union {
        struct {
            uint8_t duration_minutes;  /**< Duration in minutes for time-based watering */
        } by_duration;

        struct {
            uint16_t volume_liters;    /**< Volume in liters for volume-based watering */
        } by_volume;
    } watering;

    /** Time to start watering event */
    struct {
        uint8_t hour;    /**< Hour of day (0-23) */
        uint8_t minute;  /**< Minute of hour (0-59) */
    } start_time;

    bool auto_enabled;  /**< Whether this event is enabled for automatic scheduling */
} watering_event_t;

/**
 * @brief Definition of a watering channel including its configuration and hardware
 */
typedef struct {
    watering_event_t watering_event;  /**< Configuration for automatic scheduling */
    uint32_t last_watering_time;      /**< Timestamp of last watering event */
    char name[64];                    /**< User-friendly name for the channel */
    struct gpio_dt_spec valve;        /**< GPIO specification for the valve control */
} watering_channel_t;

/**
 * @brief A watering task represents a single watering operation to be executed
 */
typedef struct {
    watering_channel_t *channel;  /**< Channel to be watered */

    /** Task-specific parameters depending on watering mode */
    union {
        struct {
            uint32_t start_time;  /**< Start time for duration-based watering */
        } by_time;

        struct {
            uint32_t volume_liters;  /**< Target volume for volume-based watering */
        } by_volume;
    };
} watering_task_t;

/**
 * @brief System status codes for the watering system
 */
typedef enum {
    WATERING_STATUS_OK = 0,            /**< System operating normally */
    WATERING_STATUS_NO_FLOW = 1,       /**< No flow detected when valve is open */
    WATERING_STATUS_UNEXPECTED_FLOW = 2,  /**< Flow detected when all valves closed */
    WATERING_STATUS_FAULT = 3          /**< System in fault state requiring manual reset */
} watering_status_t;

/**
 * @brief Initialize the watering system
 * 
 * Sets up all channels, GPIO pins, and internal state.
 */
void watering_init(void);

/**
 * @brief Start background tasks for watering operations
 * 
 * @return 0 on success, negative error code on failure
 */
int watering_start_tasks(void);

/**
 * @brief Stop all background watering tasks
 */
void watering_stop_tasks(void);

/**
 * @brief Get a pointer to the specified watering channel
 * 
 * @param channel_id Channel ID (0-based index)
 * @return Pointer to the channel or NULL if invalid ID
 */
watering_channel_t *watering_get_channel(uint8_t channel_id);

/**
 * @brief Turn on a specific watering channel
 * 
 * @param channel_id Channel ID (0-based index)
 * @return 0 on success, negative error code on failure
 */
int watering_channel_on(uint8_t channel_id);

/**
 * @brief Turn off a specific watering channel
 * 
 * @param channel_id Channel ID (0-based index)
 * @return 0 on success, negative error code on failure
 */
int watering_channel_off(uint8_t channel_id);

/**
 * @brief Add a watering task to the execution queue
 * 
 * @param task Pointer to task definition
 * @return 0 on success, negative error code on failure
 */
int watering_add_task(watering_task_t *task);

/**
 * @brief Process the next task in the queue
 * 
 * @return 1 if a task was processed, 0 if no tasks available, negative on error
 */
int watering_process_next_task(void);

/**
 * @brief Execute the scheduler to check for automatic watering events
 * 
 * Checks all channels for scheduled events that should run at current time.
 */
void watering_scheduler_run(void);

/**
 * @brief Check active tasks for completion or issues
 * 
 * @return 1 if tasks are active, 0 if idle, negative on error
 */
int watering_check_tasks(void);

/**
 * @brief Clean up completed tasks
 */
void watering_cleanup_tasks(void);

/**
 * @brief Set the flow sensor calibration value
 * 
 * @param pulses_per_liter Number of sensor pulses per liter of water
 */
void watering_set_flow_calibration(uint32_t pulses_per_liter);

/**
 * @brief Get the current flow sensor calibration value
 * 
 * @return Pulses per liter calibration value
 */
uint32_t watering_get_flow_calibration(void);

/**
 * @brief Save system configuration to persistent storage
 * 
 * @return 0 on success, negative error code on failure
 */
int watering_save_config(void);

/**
 * @brief Load system configuration from persistent storage
 * 
 * @return 0 on success, negative error code on failure
 */
int watering_load_config(void);

/**
 * @brief Get the current system status
 * 
 * @return Current status code
 */
watering_status_t watering_get_status(void);

/**
 * @brief Reset the system from a fault state
 * 
 * @return 0 if reset successful, negative error code if not in fault state
 */
int watering_reset_fault(void);
#endif // WATERING_H
