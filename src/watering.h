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
 * @brief Standardized error codes for watering system
 */
typedef enum {
    WATERING_SUCCESS = 0,            /**< Operation completed successfully */
    WATERING_ERROR_INVALID_PARAM = -1,  /**< Invalid parameter provided */
    WATERING_ERROR_NOT_INITIALIZED = -2,  /**< System not initialized */
    WATERING_ERROR_HARDWARE = -3,    /**< Hardware failure or not ready */
    WATERING_ERROR_BUSY = -4,        /**< System is busy/in use */
    WATERING_ERROR_QUEUE_FULL = -5,  /**< Task queue is full */
    WATERING_ERROR_TIMEOUT = -6,     /**< Operation timed out */
    WATERING_ERROR_CONFIG = -7,      /**< Configuration error */
    WATERING_ERROR_RTC_FAILURE = -8, /**< RTC failure */
    WATERING_ERROR_STORAGE = -9,     /**< Storage/persistence error */
} watering_error_t;

/**
 * @brief Configuration version for compatibility and migrations
 */
#define WATERING_CONFIG_VERSION 1

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
 * @brief Watering system state machine states
 */
typedef enum {
    WATERING_STATE_IDLE,           /**< No active watering */
    WATERING_STATE_WATERING,       /**< Actively watering */
    WATERING_STATE_PAUSED,         /**< Watering temporarily paused */
    WATERING_STATE_ERROR_RECOVERY, /**< Error recovery in progress */
} watering_state_t;

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
    bool is_active;                   /**< Whether this channel is currently active */
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
    WATERING_STATUS_FAULT = 3,         /**< System in fault state requiring manual reset */
    WATERING_STATUS_RTC_ERROR = 4,     /**< RTC failure detected */
    WATERING_STATUS_LOW_POWER = 5      /**< System in low power mode */
} watering_status_t;

/**
 * @brief Power mode configuration for the system
 */
typedef enum {
    POWER_MODE_NORMAL,          /**< Normal operation mode */
    POWER_MODE_ENERGY_SAVING,   /**< Energy-saving mode with reduced polling */
    POWER_MODE_ULTRA_LOW_POWER  /**< Ultra-low power mode with minimal activity */
} power_mode_t;

/**
 * @brief Initialize the watering system
 * 
 * Sets up all channels, GPIO pins, and internal state.
 * 
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_init(void);

/**
 * @brief Start background tasks for watering operations
 * 
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_start_tasks(void);

/**
 * @brief Stop all background watering tasks
 * 
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_stop_tasks(void);

/**
 * @brief Get a pointer to the specified watering channel
 * 
 * @param channel_id Channel ID (0-based index)
 * @param channel Pointer to store the channel reference
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_get_channel(uint8_t channel_id, watering_channel_t **channel);

/**
 * @brief Turn on a specific watering channel
 * 
 * @param channel_id Channel ID (0-based index)
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_channel_on(uint8_t channel_id);

/**
 * @brief Turn off a specific watering channel
 * 
 * @param channel_id Channel ID (0-based index)
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_channel_off(uint8_t channel_id);

/**
 * @brief Add a watering task to the execution queue
 * 
 * @param task Pointer to task definition
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_add_task(watering_task_t *task);

/**
 * @brief Process the next task in the queue
 * 
 * @return 1 if a task was processed, 0 if no tasks available, negative error code on failure
 */
int watering_process_next_task(void);

/**
 * @brief Execute the scheduler to check for automatic watering events
 * 
 * Checks all channels for scheduled events that should run at current time.
 * 
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_scheduler_run(void);

/**
 * @brief Check active tasks for completion or issues
 * 
 * @return 1 if tasks are active, 0 if idle, negative error code on failure
 */
int watering_check_tasks(void);

/**
 * @brief Clean up completed tasks and release resources
 * 
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_cleanup_tasks(void);

/**
 * @brief Set the flow sensor calibration value
 * 
 * @param pulses_per_liter Number of sensor pulses per liter of water
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_set_flow_calibration(uint32_t pulses_per_liter);

/**
 * @brief Get the current flow sensor calibration value
 * 
 * @param pulses_per_liter Pointer to store the calibration value
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_get_flow_calibration(uint32_t *pulses_per_liter);

/**
 * @brief Save system configuration to persistent storage
 * 
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_save_config(void);

/**
 * @brief Load system configuration from persistent storage
 * 
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_load_config(void);

/**
 * @brief Validate watering event configuration
 * 
 * @param event Pointer to watering event to validate
 * @return WATERING_SUCCESS if valid, error code if invalid
 */
watering_error_t watering_validate_event_config(const watering_event_t *event);

/**
 * @brief Get the current system status
 * 
 * @param status Pointer to store the status
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_get_status(watering_status_t *status);

/**
 * @brief Get the current system state
 * 
 * @param state Pointer to store the state
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_get_state(watering_state_t *state);

/**
 * @brief Reset the system from a fault state
 * 
 * @return WATERING_SUCCESS if reset successful, error code on failure
 */
watering_error_t watering_reset_fault(void);

/**
 * @brief Set the system power mode
 * 
 * @param mode New power mode to set
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_set_power_mode(power_mode_t mode);

/**
 * @brief Get the current power mode
 * 
 * @param mode Pointer to store the power mode
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_get_power_mode(power_mode_t *mode);

/**
 * @brief Add a duration-based watering task for a specific channel
 * 
 * @param channel_id Channel ID (0-based index)
 * @param minutes Watering duration in minutes
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_add_duration_task(uint8_t channel_id, uint16_t minutes);

/**
 * @brief Add a volume-based watering task for a specific channel
 * 
 * @param channel_id Channel ID (0-based index)
 * @param liters Water volume in liters
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_add_volume_task(uint8_t channel_id, uint16_t liters);

/**
 * @brief Cancel all tasks and clear the task queue
 * 
 * @return Number of tasks canceled
 */
int watering_cancel_all_tasks(void);

/**
 * @brief Get the status of the task queue
 * 
 * @param pending_count Pointer where the number of pending tasks will be stored
 * @param active Flag indicating if there is an active task
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_get_queue_status(uint8_t *pending_count, bool *active);

#endif // WATERING_H
