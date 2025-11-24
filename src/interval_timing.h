#ifndef INTERVAL_TIMING_H
#define INTERVAL_TIMING_H

#include "watering_enhanced.h"

/**
 * @file interval_timing.h
 * @brief Configurable interval timing system for AutoWatering
 * 
 * This module provides functions for managing configurable interval-based
 * watering with separate minutes and seconds fields for watering and pause
 * durations.
 */

/* Interval timing validation limits */
#define INTERVAL_MIN_DURATION_SEC       1       // Minimum duration (1 second)
#define INTERVAL_MAX_DURATION_SEC       3600    // Maximum duration (60 minutes)
#define INTERVAL_MAX_MINUTES            60      // Maximum minutes value
#define INTERVAL_MAX_SECONDS            59      // Maximum seconds value

/* Interval timing defaults */
#define INTERVAL_DEFAULT_WATERING_MIN   5       // Default watering: 5 minutes
#define INTERVAL_DEFAULT_WATERING_SEC   0       // Default watering: 0 seconds
#define INTERVAL_DEFAULT_PAUSE_MIN      2       // Default pause: 2 minutes
#define INTERVAL_DEFAULT_PAUSE_SEC      0       // Default pause: 0 seconds

/**
 * @brief Initialize interval configuration with defaults
 * @param config Pointer to interval configuration structure
 * @return 0 on success, negative error code on failure
 */
int interval_timing_init_config(interval_config_t *config);

/**
 * @brief Validate interval timing configuration
 * @param config Pointer to interval configuration to validate
 * @return 0 if valid, negative error code if invalid
 */
int interval_timing_validate_config(const interval_config_t *config);

/**
 * @brief Set watering duration in minutes and seconds
 * @param config Pointer to interval configuration
 * @param minutes Watering duration in minutes (0-60)
 * @param seconds Watering duration in seconds (0-59)
 * @return 0 on success, negative error code on failure
 */
int interval_timing_set_watering_duration(interval_config_t *config,
                                          uint16_t minutes,
                                          uint8_t seconds);

/**
 * @brief Set pause duration in minutes and seconds
 * @param config Pointer to interval configuration
 * @param minutes Pause duration in minutes (0-60)
 * @param seconds Pause duration in seconds (0-59)
 * @return 0 on success, negative error code on failure
 */
int interval_timing_set_pause_duration(interval_config_t *config,
                                      uint16_t minutes,
                                      uint8_t seconds);

/**
 * @brief Get watering duration in minutes and seconds
 * @param config Pointer to interval configuration
 * @param minutes Pointer to store minutes value
 * @param seconds Pointer to store seconds value
 * @return 0 on success, negative error code on failure
 */
int interval_timing_get_watering_duration(const interval_config_t *config,
                                          uint16_t *minutes,
                                          uint8_t *seconds);

/**
 * @brief Get pause duration in minutes and seconds
 * @param config Pointer to interval configuration
 * @param minutes Pointer to store minutes value
 * @param seconds Pointer to store seconds value
 * @return 0 on success, negative error code on failure
 */
int interval_timing_get_pause_duration(const interval_config_t *config,
                                      uint16_t *minutes,
                                      uint8_t *seconds);

/**
 * @brief Convert minutes and seconds to total seconds
 * @param minutes Minutes value
 * @param seconds Seconds value
 * @param total_seconds Pointer to store total seconds
 * @return 0 on success, negative error code on failure
 */
int interval_timing_convert_to_seconds(uint16_t minutes,
                                      uint8_t seconds,
                                      uint32_t *total_seconds);

/**
 * @brief Convert total seconds to minutes and seconds
 * @param total_seconds Total seconds value
 * @param minutes Pointer to store minutes value
 * @param seconds Pointer to store seconds value
 * @return 0 on success, negative error code on failure
 */
int interval_timing_convert_from_seconds(uint32_t total_seconds,
                                        uint16_t *minutes,
                                        uint8_t *seconds);

/**
 * @brief Calculate total cycle duration (watering + pause)
 * @param config Pointer to interval configuration
 * @param cycle_duration_sec Pointer to store cycle duration in seconds
 * @return 0 on success, negative error code on failure
 */
int interval_timing_get_cycle_duration(const interval_config_t *config,
                                      uint32_t *cycle_duration_sec);

/**
 * @brief Calculate number of complete cycles for target duration
 * @param config Pointer to interval configuration
 * @param target_duration_sec Target total duration in seconds
 * @param cycle_count Pointer to store number of cycles
 * @param remaining_sec Pointer to store remaining seconds after complete cycles
 * @return 0 on success, negative error code on failure
 */
int interval_timing_calculate_cycles(const interval_config_t *config,
                                    uint32_t target_duration_sec,
                                    uint32_t *cycle_count,
                                    uint32_t *remaining_sec);

/**
 * @brief Calculate number of complete cycles for target volume
 * @param config Pointer to interval configuration
 * @param target_volume_ml Target total volume in milliliters
 * @param flow_rate_ml_sec Flow rate in ml/second
 * @param cycle_count Pointer to store number of cycles
 * @param remaining_ml Pointer to store remaining volume after complete cycles
 * @return 0 on success, negative error code on failure
 */
int interval_timing_calculate_cycles_for_volume(const interval_config_t *config,
                                               uint32_t target_volume_ml,
                                               float flow_rate_ml_sec,
                                               uint32_t *cycle_count,
                                               uint32_t *remaining_ml);

/**
 * @brief Update interval configuration and mark as configured
 * @param config Pointer to interval configuration
 * @param watering_min Watering duration in minutes
 * @param watering_sec Watering duration in seconds
 * @param pause_min Pause duration in minutes
 * @param pause_sec Pause duration in seconds
 * @return 0 on success, negative error code on failure
 */
int interval_timing_update_config(interval_config_t *config,
                                 uint16_t watering_min,
                                 uint8_t watering_sec,
                                 uint16_t pause_min,
                                 uint8_t pause_sec);

/**
 * @brief Clear interval configuration and mark as not configured
 * @param config Pointer to interval configuration
 * @return 0 on success, negative error code on failure
 */
int interval_timing_clear_config(interval_config_t *config);

/**
 * @brief Check if interval configuration is valid and complete
 * @param config Pointer to interval configuration
 * @param is_configured Pointer to store configuration status
 * @return 0 on success, negative error code on failure
 */
int interval_timing_is_configured(const interval_config_t *config,
                                 bool *is_configured);

/**
 * @brief Get human-readable description of interval timing
 * @param config Pointer to interval configuration
 * @param description Buffer to store description (min 128 chars)
 * @return 0 on success, negative error code on failure
 */
int interval_timing_get_description(const interval_config_t *config,
                                   char *description);

/**
 * @brief Validate individual timing values
 * @param minutes Minutes value to validate
 * @param seconds Seconds value to validate
 * @return 0 if valid, negative error code if invalid
 */
int interval_timing_validate_values(uint16_t minutes, uint8_t seconds);

/**
 * @brief Calculate remaining time in current phase
 * @param config Pointer to interval configuration
 * @param phase_start_time When current phase started (timestamp)
 * @param currently_watering Whether currently in watering phase
 * @param remaining_sec Pointer to store remaining seconds
 * @return 0 on success, negative error code on failure
 */
int interval_timing_get_phase_remaining(const interval_config_t *config,
                                       uint32_t phase_start_time,
                                       bool currently_watering,
                                       uint32_t *remaining_sec);

/**
 * @brief Update phase timing information
 * @param config Pointer to interval configuration
 * @param currently_watering Current phase state
 * @param phase_start_time When current phase started
 * @return 0 on success, negative error code on failure
 */
int interval_timing_update_phase(interval_config_t *config,
                                bool currently_watering,
                                uint32_t phase_start_time);

/**
 * @brief Check if it's time to switch phases
 * @param config Pointer to interval configuration
 * @param should_switch Pointer to store switch decision
 * @return 0 on success, negative error code on failure
 */
int interval_timing_should_switch_phase(const interval_config_t *config,
                                       bool *should_switch);

/**
 * @brief Reset interval timing state for new task
 * @param config Pointer to interval configuration
 * @return 0 on success, negative error code on failure
 */
int interval_timing_reset_state(interval_config_t *config);

#endif // INTERVAL_TIMING_H