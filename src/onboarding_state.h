#ifndef ONBOARDING_STATE_H
#define ONBOARDING_STATE_H

/**
 * @file onboarding_state.h
 * @brief Onboarding state management for AutoWatering system
 * 
 * This module provides comprehensive state tracking for user onboarding,
 * including configuration completeness flags and progress calculation.
 */

#include <stdint.h>
#include <stdbool.h>
#include "nvs_config.h"

/**
 * @brief Initialize the onboarding state management system
 * 
 * @return 0 on success, negative error code on failure
 */
int onboarding_state_init(void);

/**
 * @brief Get the current onboarding state
 * 
 * @param state Pointer to store the current onboarding state
 * @return 0 on success, negative error code on failure
 */
int onboarding_get_state(onboarding_state_t *state);

/**
 * @brief Update a channel configuration flag
 * 
 * @param channel_id Channel ID (0-7)
 * @param flag Flag bit to update (CHANNEL_FLAG_*)
 * @param set True to set the flag, false to clear it
 * @return 0 on success, negative error code on failure
 */
int onboarding_update_channel_flag(uint8_t channel_id, uint8_t flag, bool set);

/**
 * @brief Update a system configuration flag
 * 
 * @param flag Flag bit to update (SYSTEM_FLAG_*)
 * @param set True to set the flag, false to clear it
 * @return 0 on success, negative error code on failure
 */
int onboarding_update_system_flag(uint32_t flag, bool set);

/**
 * @brief Calculate the overall onboarding completion percentage
 * 
 * @return Completion percentage (0-100)
 */
int onboarding_calculate_completion(void);

/**
 * @brief Check if onboarding is complete
 * 
 * @return True if onboarding is complete, false otherwise
 */
bool onboarding_is_complete(void);

/**
 * @brief Get channel configuration flags for a specific channel
 * 
 * @param channel_id Channel ID (0-7)
 * @return Channel configuration flags (8-bit value)
 */
uint8_t onboarding_get_channel_flags(uint8_t channel_id);

/**
 * @brief Get system configuration flags
 * 
 * @return System configuration flags (32-bit value)
 */
uint32_t onboarding_get_system_flags(void);

/**
 * @brief Get schedule configuration flags
 * 
 * @return Schedule configuration flags (8-bit value, one bit per channel)
 */
uint8_t onboarding_get_schedule_flags(void);

/**
 * @brief Update schedule configuration flag for a channel
 * 
 * @param channel_id Channel ID (0-7)
 * @param has_schedule True if channel has schedules configured
 * @return 0 on success, negative error code on failure
 */
int onboarding_update_schedule_flag(uint8_t channel_id, bool has_schedule);

/**
 * @brief Update an extended channel configuration flag
 * 
 * @param channel_id Channel ID (0-7)
 * @param flag Extended flag bit to update (CHANNEL_EXT_FLAG_*)
 * @param set True to set the flag, false to clear it
 * @return 0 on success, negative error code on failure
 */
int onboarding_update_channel_extended_flag(uint8_t channel_id, uint8_t flag, bool set);

/**
 * @brief Get extended channel configuration flags for a specific channel
 * 
 * @param channel_id Channel ID (0-7)
 * @return Extended channel configuration flags (8-bit value)
 */
uint8_t onboarding_get_channel_extended_flags(uint8_t channel_id);

/**
 * @brief Check and update FAO-56 readiness flag for a channel
 * 
 * Automatically checks if all FAO-56 requirements are met:
 * - Plant type set
 * - Soil type set  
 * - Irrigation method set
 * - Coverage (area) set
 * - Location (latitude) set
 * 
 * @param channel_id Channel ID (0-7)
 */
void onboarding_check_fao56_ready(uint8_t channel_id);

/**
 * @brief Reset all onboarding state to defaults
 * 
 * @return 0 on success, negative error code on failure
 */
int onboarding_reset_state(void);

#endif /* ONBOARDING_STATE_H */