#ifndef INTERVAL_MODE_CONTROLLER_H
#define INTERVAL_MODE_CONTROLLER_H

#include "watering_enhanced.h"
#include "interval_timing.h"

/**
 * @file interval_mode_controller.h
 * @brief Interval mode state machine and controller for AutoWatering system
 * 
 * This module provides the state machine and controller for interval-based
 * watering with configurable watering and pause phases.
 */

/* Interval mode states */
typedef enum {
    INTERVAL_STATE_IDLE,              // Not running
    INTERVAL_STATE_WATERING,          // Currently watering
    INTERVAL_STATE_PAUSING,           // Currently in pause phase
    INTERVAL_STATE_COMPLETED,         // All cycles completed
    INTERVAL_STATE_ERROR              // Error occurred
} interval_state_t;

/* Interval mode controller structure */
typedef struct {
    interval_state_t state;           // Current state
    interval_config_t *config;        // Interval configuration
    uint8_t channel_id;               // Channel being controlled
    uint32_t task_start_time;         // When task started
    uint32_t phase_start_time;        // When current phase started
    uint32_t total_target;            // Total target (duration or volume)
    uint32_t total_elapsed;           // Total elapsed time
    uint32_t total_volume;            // Total volume dispensed
    uint32_t cycles_completed;        // Number of complete cycles
    uint32_t current_cycle_volume;    // Volume in current cycle
    bool is_volume_based;             // True for volume-based, false for duration-based
    float flow_rate_ml_sec;           // Current flow rate (for volume calculations)
    uint32_t last_update_time;        // Last state update time
    watering_error_t last_error;      // Last error that occurred
} interval_controller_t;

/**
 * @brief Initialize interval mode controller
 * @param controller Pointer to controller structure
 * @param channel_id Channel ID
 * @param config Pointer to interval configuration
 * @param total_target Total target (duration in seconds or volume in ml)
 * @param is_volume_based True for volume-based mode, false for duration-based
 * @return 0 on success, negative error code on failure
 */
int interval_controller_init(interval_controller_t *controller,
                            uint8_t channel_id,
                            interval_config_t *config,
                            uint32_t total_target,
                            bool is_volume_based);

/**
 * @brief Start interval mode execution
 * @param controller Pointer to controller structure
 * @return 0 on success, negative error code on failure
 */
int interval_controller_start(interval_controller_t *controller);

/**
 * @brief Update interval mode state machine
 * @param controller Pointer to controller structure
 * @param current_volume Current total volume dispensed (ml)
 * @param flow_rate_ml_sec Current flow rate (ml/sec)
 * @return 0 on success, negative error code on failure
 */
int interval_controller_update(interval_controller_t *controller,
                              uint32_t current_volume,
                              float flow_rate_ml_sec);

/**
 * @brief Stop interval mode execution
 * @param controller Pointer to controller structure
 * @param reason Reason for stopping
 * @return 0 on success, negative error code on failure
 */
int interval_controller_stop(interval_controller_t *controller,
                            const char *reason);

/**
 * @brief Check if interval mode is complete
 * @param controller Pointer to controller structure
 * @param is_complete Pointer to store completion status
 * @return 0 on success, negative error code on failure
 */
int interval_controller_is_complete(const interval_controller_t *controller,
                                   bool *is_complete);

/**
 * @brief Get current interval mode status
 * @param controller Pointer to controller structure
 * @param status Pointer to store enhanced task status
 * @return 0 on success, negative error code on failure
 */
int interval_controller_get_status(const interval_controller_t *controller,
                                  enhanced_task_status_t *status);

/**
 * @brief Get remaining time in current phase
 * @param controller Pointer to controller structure
 * @param remaining_sec Pointer to store remaining seconds
 * @return 0 on success, negative error code on failure
 */
int interval_controller_get_phase_remaining(const interval_controller_t *controller,
                                           uint32_t *remaining_sec);

/**
 * @brief Get total remaining time for task
 * @param controller Pointer to controller structure
 * @param remaining_sec Pointer to store remaining seconds
 * @return 0 on success, negative error code on failure
 */
int interval_controller_get_total_remaining(const interval_controller_t *controller,
                                           uint32_t *remaining_sec);

/**
 * @brief Check if currently in watering phase
 * @param controller Pointer to controller structure
 * @param is_watering Pointer to store watering status
 * @return 0 on success, negative error code on failure
 */
int interval_controller_is_watering(const interval_controller_t *controller,
                                   bool *is_watering);

/**
 * @brief Check if currently in pause phase
 * @param controller Pointer to controller structure
 * @param is_pausing Pointer to store pause status
 * @return 0 on success, negative error code on failure
 */
int interval_controller_is_pausing(const interval_controller_t *controller,
                                  bool *is_pausing);

/**
 * @brief Get progress information
 * @param controller Pointer to controller structure
 * @param progress_percent Pointer to store progress percentage (0-100)
 * @param cycles_remaining Pointer to store remaining cycles
 * @return 0 on success, negative error code on failure
 */
int interval_controller_get_progress(const interval_controller_t *controller,
                                    uint8_t *progress_percent,
                                    uint32_t *cycles_remaining);

/**
 * @brief Handle state transition
 * @param controller Pointer to controller structure
 * @param new_state New state to transition to
 * @return 0 on success, negative error code on failure
 */
int interval_controller_transition_state(interval_controller_t *controller,
                                        interval_state_t new_state);

/**
 * @brief Calculate next phase switch time
 * @param controller Pointer to controller structure
 * @param next_switch_time Pointer to store next switch time
 * @return 0 on success, negative error code on failure
 */
int interval_controller_get_next_switch_time(const interval_controller_t *controller,
                                            uint32_t *next_switch_time);

/**
 * @brief Reset controller for new task
 * @param controller Pointer to controller structure
 * @return 0 on success, negative error code on failure
 */
int interval_controller_reset(interval_controller_t *controller);

/**
 * @brief Validate controller state
 * @param controller Pointer to controller structure
 * @return 0 if valid, negative error code if invalid
 */
int interval_controller_validate(const interval_controller_t *controller);

/**
 * @brief Get human-readable state description
 * @param state Interval state
 * @return String description of state
 */
const char* interval_controller_state_to_string(interval_state_t state);

/**
 * @brief Handle error condition
 * @param controller Pointer to controller structure
 * @param error Error code that occurred
 * @param error_message Error message
 * @return 0 on success, negative error code on failure
 */
int interval_controller_handle_error(interval_controller_t *controller,
                                    watering_error_t error,
                                    const char *error_message);

/**
 * @brief Calculate estimated completion time
 * @param controller Pointer to controller structure
 * @param estimated_completion Pointer to store estimated completion time
 * @return 0 on success, negative error code on failure
 */
int interval_controller_get_estimated_completion(const interval_controller_t *controller,
                                                uint32_t *estimated_completion);

/**
 * @brief Update flow rate for volume calculations
 * @param controller Pointer to controller structure
 * @param flow_rate_ml_sec New flow rate in ml/sec
 * @return 0 on success, negative error code on failure
 */
int interval_controller_update_flow_rate(interval_controller_t *controller,
                                         float flow_rate_ml_sec);

/**
 * @brief Check if phase switch is needed
 * @param controller Pointer to controller structure
 * @param should_switch Pointer to store switch decision
 * @param next_state Pointer to store next state if switch is needed
 * @return 0 on success, negative error code on failure
 */
int interval_controller_should_switch_phase(const interval_controller_t *controller,
                                           bool *should_switch,
                                           interval_state_t *next_state);

#endif // INTERVAL_MODE_CONTROLLER_H