/**
 * @file enhanced_system_status.h
 * @brief Enhanced system status interface for advanced irrigation modes
 * 
 * This header provides enhanced system status reporting that includes:
 * - Interval mode phase tracking
 * - Compensation system status indicators  
 * - Environmental sensor health monitoring
 * - Configuration completeness tracking
 */

#ifndef ENHANCED_SYSTEM_STATUS_H
#define ENHANCED_SYSTEM_STATUS_H

#include "watering_enhanced.h"

/**
 * @brief Initialize enhanced system status module
 * 
 * @return 0 on success, negative error code on failure
 */
int enhanced_system_status_init(void);

/**
 * @brief Get comprehensive enhanced system status information
 * 
 * This function provides a complete view of the system status including:
 * - Primary system status with enhanced operational modes
 * - Current task phase information for interval mode
 * - Compensation system status (rain/temperature)
 * - Environmental sensor health status
 * - Channel activity and configuration bitmaps
 * 
 * @param status_info Pointer to structure to fill with status information
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t enhanced_system_get_status(enhanced_system_status_info_t *status_info);

/**
 * @brief Determine the primary system status based on current conditions
 * 
 * This function evaluates all system conditions and returns the most
 * relevant status indicator based on priority:
 * 1. Critical errors (fault, RTC error, low power)
 * 2. Flow issues (no flow, unexpected flow)
 * 3. Sensor errors (BME280 failure)
 * 4. Active operations (interval mode phases)
 * 5. Compensation systems (rain/temperature active)
 * 6. Configuration issues (incomplete config)
 * 7. Normal operation (OK, custom soil, degraded mode)
 * 
 * @return Primary enhanced system status
 */
enhanced_system_status_t enhanced_system_determine_primary_status(void);

/**
 * @brief Update compensation system status information
 * 
 * This function checks the current state of rain and temperature
 * compensation systems and updates the status structure with:
 * - Active compensation flags
 * - Current reduction/adjustment percentages
 * - Last calculation timestamps
 * 
 * @param comp_status Pointer to compensation status structure to update
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t enhanced_system_update_compensation_status(compensation_status_t *comp_status);

/**
 * @brief Update environmental sensor health status
 * 
 * This function evaluates the health of environmental sensors including:
 * - BME280 initialization and response status
 * - Data validity and quality assessment
 * - Rain sensor operational status
 * - Data age and consecutive failure tracking
 * 
 * @param sensor_status Pointer to sensor status structure to update
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t enhanced_system_update_sensor_status(environmental_sensor_status_t *sensor_status);

/**
 * @brief Check if any channels are currently using interval mode
 * 
 * This function scans all channels to determine which ones are
 * currently configured for and actively using interval mode watering.
 * 
 * @param active_channels_bitmap Pointer to store bitmap of channels using interval mode
 * @return true if any channels are using interval mode, false otherwise
 */
bool enhanced_system_is_interval_mode_active(uint8_t *active_channels_bitmap);

/**
 * @brief Check which channels have incomplete configuration
 * 
 * This function evaluates the configuration completeness of all channels
 * and identifies which ones cannot perform automatic watering due to
 * missing configuration parameters.
 * 
 * @param incomplete_channels_bitmap Pointer to store bitmap of channels with incomplete config
 * @return true if any channels have incomplete configuration, false otherwise
 */
bool enhanced_system_has_incomplete_config(uint8_t *incomplete_channels_bitmap);

/**
 * @brief Convert enhanced system status to string representation
 * 
 * This function provides human-readable status descriptions for debugging
 * and logging purposes.
 * 
 * @param status Enhanced system status value
 * @return String representation of the status
 */
const char* enhanced_system_status_to_string(enhanced_system_status_t status);

/**
 * @brief Check if system status indicates an error condition
 * 
 * This function determines if the current status represents an error
 * condition that requires attention or intervention.
 * 
 * @param status Enhanced system status to check
 * @return true if status indicates an error, false otherwise
 */
bool enhanced_system_status_is_error(enhanced_system_status_t status);

/**
 * @brief Check if system status indicates active operation
 * 
 * This function determines if the current status represents active
 * watering or operational activity.
 * 
 * @param status Enhanced system status to check
 * @return true if status indicates active operation, false otherwise
 */
bool enhanced_system_status_is_active(enhanced_system_status_t status);

#endif // ENHANCED_SYSTEM_STATUS_H