#ifndef INTERVAL_TASK_INTEGRATION_H
#define INTERVAL_TASK_INTEGRATION_H

#include "watering_enhanced.h"
#include "interval_mode_controller.h"
#include "watering_internal.h"

/**
 * @file interval_task_integration.h
 * @brief Integration of interval mode with task execution system
 * 
 * This module integrates the interval mode controller with the existing
 * task execution system, providing seamless support for interval-based
 * watering alongside traditional continuous watering modes.
 */

/* Enhanced task state structure with interval support */
typedef struct {
    watering_task_t *current_active_task;    // Currently executing task or NULL
    uint32_t watering_start_time;            // Timestamp when watering started
    bool task_in_progress;                   // Flag indicating task is in progress
    bool task_paused;                        // Flag indicating task is paused
    uint32_t pause_start_time;               // Timestamp when pause started
    uint32_t total_paused_time;              // Total time spent paused (ms)
    
    // Interval mode extensions
    bool is_interval_mode;                   // True if using interval mode
    interval_controller_t interval_controller; // Interval mode controller
    enhanced_task_state_t current_phase;    // Current phase (watering/pausing)
    uint32_t phase_remaining_sec;            // Seconds remaining in current phase
    uint32_t cycles_completed;               // Number of complete cycles
    uint32_t next_phase_time;                // Time until next phase change
} enhanced_watering_task_state_t;

/**
 * @brief Initialize enhanced task execution system with interval support
 * @return 0 on success, negative error code on failure
 */
int interval_task_init(void);

/**
 * @brief Start a watering task with interval mode support
 * @param task Pointer to watering task
 * @return 0 on success, negative error code on failure
 */
int interval_task_start(watering_task_t *task);

/**
 * @brief Update task execution with interval mode support
 * @param current_volume Current total volume dispensed (ml)
 * @param flow_rate_ml_sec Current flow rate (ml/sec)
 * @return 0 on success, negative error code on failure
 */
int interval_task_update(uint32_t current_volume, float flow_rate_ml_sec);

/**
 * @brief Stop current task with interval mode support
 * @param reason Reason for stopping
 * @return 0 on success, negative error code on failure
 */
int interval_task_stop(const char *reason);

/**
 * @brief Check if current task is complete
 * @param is_complete Pointer to store completion status
 * @return 0 on success, negative error code on failure
 */
int interval_task_is_complete(bool *is_complete);

/**
 * @brief Get enhanced task status with interval mode information
 * @param status Pointer to store enhanced task status
 * @return 0 on success, negative error code on failure
 */
int interval_task_get_status(enhanced_task_status_t *status);

/**
 * @brief Check if task should use interval mode
 * @param task Pointer to watering task
 * @param should_use_interval Pointer to store result
 * @return 0 on success, negative error code on failure
 */
int interval_task_should_use_interval(const watering_task_t *task, bool *should_use_interval);

/**
 * @brief Get current phase information for interval mode
 * @param is_watering Pointer to store watering phase status
 * @param phase_remaining_sec Pointer to store remaining seconds in current phase
 * @return 0 on success, negative error code on failure
 */
int interval_task_get_phase_info(bool *is_watering, uint32_t *phase_remaining_sec);

/**
 * @brief Handle valve control for interval mode
 * @param should_open_valve Pointer to store valve control decision
 * @return 0 on success, negative error code on failure
 */
int interval_task_get_valve_control(bool *should_open_valve);

/**
 * @brief Update task progress with interval mode support
 * @param progress_percent Pointer to store progress percentage (0-100)
 * @param cycles_remaining Pointer to store remaining cycles
 * @return 0 on success, negative error code on failure
 */
int interval_task_get_progress(uint8_t *progress_percent, uint32_t *cycles_remaining);

/**
 * @brief Handle task completion with interval mode support
 * @param completion_reason Reason for completion
 * @return 0 on success, negative error code on failure
 */
int interval_task_handle_completion(const char *completion_reason);

/**
 * @brief Pause current task (interval mode aware)
 * @param reason Reason for pausing
 * @return 0 on success, negative error code on failure
 */
int interval_task_pause(const char *reason);

/**
 * @brief Resume paused task (interval mode aware)
 * @param reason Reason for resuming
 * @return 0 on success, negative error code on failure
 */
int interval_task_resume(const char *reason);

/**
 * @brief Get time until next phase change (interval mode only)
 * @param next_phase_time_sec Pointer to store time until next phase change
 * @return 0 on success, negative error code on failure
 */
int interval_task_get_next_phase_time(uint32_t *next_phase_time_sec);

/**
 * @brief Check if task is currently in interval mode
 * @param is_interval_mode Pointer to store interval mode status
 * @return 0 on success, negative error code on failure
 */
int interval_task_is_interval_mode(bool *is_interval_mode);

/**
 * @brief Get fallback behavior when interval mode is not configured
 * @param task Pointer to watering task
 * @param fallback_mode Pointer to store fallback watering mode
 * @return 0 on success, negative error code on failure
 */
int interval_task_get_fallback_mode(const watering_task_t *task, 
                                   enhanced_watering_mode_t *fallback_mode);

/**
 * @brief Handle interval mode errors and recovery
 * @param error Error code that occurred
 * @param error_message Error message
 * @return 0 on success, negative error code on failure
 */
int interval_task_handle_error(watering_error_t error, const char *error_message);

/**
 * @brief Validate interval configuration for task
 * @param task Pointer to watering task
 * @return 0 if valid, negative error code if invalid
 */
int interval_task_validate_config(const watering_task_t *task);

/**
 * @brief Calculate total task duration for interval mode
 * @param task Pointer to watering task
 * @param total_duration_sec Pointer to store total duration in seconds
 * @return 0 on success, negative error code on failure
 */
int interval_task_calculate_duration(const watering_task_t *task, uint32_t *total_duration_sec);

/**
 * @brief Update flow monitoring for interval mode
 * @param current_volume Current volume dispensed
 * @param expected_flow_during_watering Expected flow during watering phase
 * @param expected_flow_during_pause Expected flow during pause phase
 * @return 0 on success, negative error code on failure
 */
int interval_task_update_flow_monitoring(uint32_t current_volume,
                                        bool expected_flow_during_watering,
                                        bool expected_flow_during_pause);

/**
 * @brief Get enhanced task state for external systems
 * @param task_state Pointer to store enhanced task state
 * @return 0 on success, negative error code on failure
 */
int interval_task_get_enhanced_state(enhanced_watering_task_state_t *task_state);

/**
 * @brief Reset task state for new execution
 * @return 0 on success, negative error code on failure
 */
int interval_task_reset_state(void);

/**
 * @brief Check if system supports interval mode for given channel
 * @param channel_id Channel ID to check
 * @param is_supported Pointer to store support status
 * @return 0 on success, negative error code on failure
 */
int interval_task_is_supported(uint8_t channel_id, bool *is_supported);

/**
 * @brief Initialize interval task integration system
 * @return WATERING_ERROR_NONE on success, error code on failure
 */
watering_error_t interval_task_integration_init(void);

#endif // INTERVAL_TASK_INTEGRATION_H