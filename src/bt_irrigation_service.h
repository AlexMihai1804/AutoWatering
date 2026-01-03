#ifndef BT_IRRIGATION_SERVICE_H_
#define BT_IRRIGATION_SERVICE_H_

#include <zephyr/autoconf.h>

#ifdef CONFIG_BT

#ifndef CONFIG_BT_MAX_PAIRED
#define CONFIG_BT_MAX_PAIRED 1
#endif
#ifndef CONFIG_BT_MAX_CONN
#define CONFIG_BT_MAX_CONN 1
#endif

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>
#include "watering.h"
#include "rtc.h"  // Add RTC header for time synchronization

/**
 * @file bt_irrigation_service.h
 * @brief Bluetooth Low Energy service for AutoWatering irrigation system
 * 
 * This service provides comprehensive BLE communication capabilities with enhanced
 * reliability features including notification throttling and buffer management.
 * 
 * Version: 1.2.0 (July 2025)
 * 
 * Key Features:
 * - 15-characteristic GATT service with complete functionality
 * - Notification throttling system prevents buffer overflow errors  
 * - Simple direct notification system (500ms delay between notifications)
 * - Background task update thread for reliable real-time monitoring
 * - Enhanced buffer pool management for stable operation
 * 
 * GATT Service Characteristics:
 * 1. Valve Control - Read/Write/Notify valve operations
 * 2. Flow Sensor - Read/Notify flow rate data
 * 3. System Status - Read/Notify system state
 * 4. Channel Configuration - Read/Write/Notify channel settings
 * 5. Schedule Configuration - Read/Write/Notify scheduling settings
 * 6. System Configuration - Read/Write/Notify system settings
 * 7. Task Queue - Read/Write/Notify task management
 * 8. Statistics - Read/Write/Notify usage statistics
 * 9. RTC - Read/Write/Notify real-time clock
 * 10. Alarm - Read/Write/Notify alarm notifications
 * 11. Calibration - Read/Write/Notify flow sensor calibration
 * 12. History - Read/Write/Notify watering history
 * 13. Diagnostics - Read/Notify system diagnostics
 * 14. Growing Environment - Read/Write/Notify environmental settings
 * 15. Current Task - Read/Write/Notify real-time task monitoring
 * 16. Lifecycle Config - Read/Write/Notify plant lifecycle settings
 * 17. Auto Calc Status - Read/Notify automatic calculation results
 * 
 * Recent Improvements:
 * - Simplified notification system with direct calls (no queues)
 * - Fixed "Unable to allocate buffer within timeout" errors
 * - Implemented 500ms minimum delay between notifications
 * - Added background task monitoring thread
 * - Enhanced error handling and graceful degradation
 */

/**
 * @brief Initialize the Bluetooth irrigation service
 * 
 * Initializes the complete BLE service including:
 * - GATT service registration with all 17 characteristics
 * - Direct notification system setup (500ms minimum delay)
 * - Background task update thread for current task monitoring
 * - Default value initialization for all characteristics
 * - System configuration with defaults (750 pulses/liter, normal power mode)
 * - Connection callback registration
 * - Bluetooth stack initialization
 * - Advertising configuration and startup
 * 
 * @return 0 on success, negative error code on failure
 */
int bt_irrigation_service_init(void);

/**
 * @brief Resume factory wipe if one was interrupted by reboot
 * 
 * Should be called after BLE service init. If a wipe was in progress,
 * this will re-queue the wipe work to continue from where it left off.
 * 
 * @return 0 if no resume needed, 1 if resume queued, negative on error
 */
int bt_irrigation_resume_wipe_if_needed(void);

/**
 * @brief Update valve status via Bluetooth
 * 
 * Sends a direct notification about valve state changes. Uses the simplified
 * notification system with 500ms minimum delay between notifications to prevent
 * buffer overflow during rapid state changes.
 * 
 * @param channel_id Channel ID (0-7)
 * @param state Valve state (false=off, true=on)
 * @return 0 on success, negative error code on failure
 */
int bt_irrigation_valve_status_update(uint8_t channel_id, bool state);

/**
 * @brief Update flow data via Bluetooth
 * 
 * Provides real-time flow sensor data with automatic rate limiting (500ms minimum
 * delay) to prevent BLE buffer overflow during high-frequency updates.
 * 
 * @param flow_rate_or_pulses Stabilized flow rate in pulses per second or raw pulse count
 * @return 0 on success, negative error code on failure
 */
int bt_irrigation_flow_update(uint32_t flow_rate_or_pulses);

/**
 * @brief Update system status via Bluetooth
 * 
 * Notifies clients of system state changes using the direct notification system.
 * 
 * @param status Current system status (watering_status_t enum)
 * @return 0 on success, negative error code on failure
 */
int bt_irrigation_system_status_update(watering_status_t status);

/**
 * @brief Update channel configuration via Bluetooth
 * 
 * Sends channel configuration updates using the direct notification system.
 * 
 * @param channel_id Channel ID (0-7)
 * @return 0 on success, negative error code on failure
 */
int bt_irrigation_channel_config_update(uint8_t channel_id);

/**
 * @brief Update pending tasks count via Bluetooth
 * 
 * Notifies clients about changes in the task queue using the direct notification
 * system with automatic rate limiting.
 * 
 * @param pending_count Number of pending tasks in queue
 * @return 0 on success, negative error code on failure
 */
int bt_irrigation_queue_status_update(uint8_t pending_count);

/**
 * @brief Update current task status via Bluetooth
 * 
 * Provides real-time task monitoring with the direct notification system. This function
 * is called frequently during task execution and benefits from the 500ms rate limiting
 * to prevent buffer overflow. A background task update thread also monitors and updates
 * this characteristic automatically.
 * 
 * @param channel_id Channel ID (0xFF if no active task)
 * @param start_time Task start time in seconds since epoch
 * @param mode Watering mode (0=duration, 1=volume)
 * @param target_value Target duration in seconds or volume in ml
 * @param current_value Current duration in seconds or volume in ml
 * @param total_volume Total volume dispensed in ml
 * @return 0 on success, negative error code on failure
 */
int bt_irrigation_current_task_update(uint8_t channel_id, uint32_t start_time, 
                                    uint8_t mode, uint32_t target_value, 
                                    uint32_t current_value, uint32_t total_volume);

/**
 * @brief Send current task notification via Bluetooth
 * 
 * Reads current task status from watering system and sends notification
 * to connected clients. Used for periodic updates and immediate notifications
 * when task status changes.
 * 
 * @return 0 on success, negative error code on failure
 */
int bt_irrigation_current_task_notify(void);

/**
 * @brief Update schedule configuration via Bluetooth
 * 
 * Sends schedule configuration updates using the direct notification system.
 * Retrieves current schedule settings from the watering system and sends
 * them to connected BLE clients.
 * 
 * @param channel_id Channel ID (0-7)
 * @return 0 on success, negative error code on failure
 */
int bt_irrigation_schedule_update(uint8_t channel_id);

/**
 * @brief Update system configuration via Bluetooth
 * 
 * Sends system configuration updates using the direct notification system.
 * Retrieves current system settings from the watering system and sends
 * them to connected BLE clients.
 * 
 * @return 0 on success, negative error code on failure
 */
int bt_irrigation_config_update(void);

/**
 * @brief Update statistics via Bluetooth
 * 
 * Sends usage statistics updates using the direct notification system.
 * Retrieves current irrigation statistics from the watering system and
 * sends them to connected BLE clients.
 * 
 * @param channel_id Channel ID (0-7)
 * @return 0 on success, negative error code on failure
 */
int bt_irrigation_statistics_update(uint8_t channel_id);

/**
 * @brief Update RTC time via Bluetooth
 * 
 * Updates the RTC characteristic with new date/time information and sends
 * a notification to connected clients using the direct notification system.
 * 
 * @param datetime Structure with new date and time
 * @return 0 on success, negative error code on failure
 */
int bt_irrigation_rtc_update(rtc_datetime_t *datetime);

/**
 * @brief Notify Bluetooth client about an alarm
 * 
 * Uses the direct notification system to ensure alarm notifications are
 * delivered reliably even during high system activity. Updates the alarm
 * characteristic with the alarm code, data, and current timestamp.
 * 
 * @param alarm_code Alarm code
 * @param alarm_data Additional alarm data
 * @return 0 on success, negative error code on failure
 */
int bt_irrigation_alarm_notify(uint8_t alarm_code, uint16_t alarm_data);

/**
 * @brief Clear a specific alarm or all alarms
 * 
 * Sends an alarm clear notification using the direct notification system.
 * Sets alarm_data to 0 and updates the timestamp.
 * 
 * @param alarm_code Alarm code to clear (0 = clear all alarms)
 * @return 0 on success, negative error code on failure
 */
int bt_irrigation_alarm_clear(uint8_t alarm_code);

/**
 * @brief Send calibration notification via Bluetooth
 * 
 * Reads current calibration status and sends notification to connected
 * clients. Used for reporting calibration progress and completion.
 * 
 * @return 0 on success, negative error code on failure
 */
int bt_irrigation_calibration_notify(void);

/**
 * @brief Send timezone configuration notification via Bluetooth
 * 
 * Reads current timezone configuration and sends notification to connected
 * clients. Used for reporting timezone changes and DST status updates.
 * 
 * @return 0 on success, negative error code on failure
 */
int bt_irrigation_timezone_notify(void);

/**
 * @brief Start flow sensor calibration session
 * 
 * Controls flow sensor calibration process via BLE. Starts or stops calibration
 * and calculates the pulses per liter when stopped with a known volume.
 * 
 * @param start 1 to start, 0 to stop
 * @param volume_ml Water volume in ml for calibration (when stopping)
 * @return 0 on success, negative error code on failure
 */
int bt_irrigation_start_flow_calibration(uint8_t start, uint32_t volume_ml);

/**
 * @brief Update irrigation history
 * 
 * Sends irrigation history updates via BLE. Retrieves history entries from
 * the watering system and sends them to connected BLE clients.
 * 
 * @param channel_id Channel ID (0-7)
 * @param entry_index History entry index (0 = most recent)
 * @return 0 on success, negative error code on failure
 */
int bt_irrigation_history_update(uint8_t channel_id, uint8_t entry_index);

/**
 * @brief Get detailed history events via Bluetooth
 * 
 * Retrieves detailed irrigation history events and sends them via BLE.
 * Supports filtering by timestamp range and entry index.
 * 
 * @param channel_id Channel ID or 0xFF for all
 * @param start_timestamp Start timestamp filter (0 for no filter)
 * @param end_timestamp End timestamp filter (0 for no filter)
 * @param entry_index Entry index to retrieve
 * @return 0 on success, negative error code on failure
 */
int bt_irrigation_history_get_detailed(uint8_t channel_id, uint32_t start_timestamp, 
                                     uint32_t end_timestamp, uint8_t entry_index);

/**
 * @brief Get daily statistics via Bluetooth
 * 
 * Retrieves daily irrigation statistics and sends them via BLE.
 * Provides aggregated data for a specific day.
 * 
 * @param channel_id Channel ID or 0xFF for all
 * @param entry_index Entry index to retrieve
 * @return 0 on success, negative error code on failure
 */
int bt_irrigation_history_get_daily(uint8_t channel_id, uint8_t entry_index);

/**
 * @brief Get monthly statistics via Bluetooth
 * 
 * Retrieves monthly irrigation statistics and sends them via BLE.
 * Provides aggregated data for a specific month.
 * 
 * @param channel_id Channel ID or 0xFF for all
 * @param entry_index Entry index to retrieve
 * @return 0 on success, negative error code on failure
 */
int bt_irrigation_history_get_monthly(uint8_t channel_id, uint8_t entry_index);

/**
 * @brief Get annual statistics via Bluetooth
 * 
 * Retrieves annual irrigation statistics and sends them via BLE.
 * Provides aggregated data for a specific year.
 * 
 * @param channel_id Channel ID or 0xFF for all
 * @param entry_index Entry index to retrieve
 * @return 0 on success, negative error code on failure
 */
int bt_irrigation_history_get_annual(uint8_t channel_id, uint8_t entry_index);

/**
 * @brief Notify when new history event is recorded
 * 
 * Sends a notification when a new irrigation event is recorded in history.
 * Records the event in the watering system and notifies BLE clients.
 * 
 * @param channel_id Channel ID
 * @param event_type Event type (0=start, 1=complete, 2=abort, 3=error)
 * @param timestamp Event timestamp
 * @param value Event value (volume, duration, etc.)
 * @return 0 on success, negative error code on failure
 */
int bt_irrigation_history_notify_event(uint8_t channel_id, uint8_t event_type, uint32_t timestamp, uint32_t value);

/**
 * @brief Update growing environment configuration via Bluetooth
 * 
 * Sends growing environment configuration updates via BLE using the direct
 * notification system. Retrieves current environmental settings from the
 * watering system and sends them to connected BLE clients.
 * 
 * @param channel_id Channel ID
 * @return 0 on success, negative error code on failure
 */
int bt_irrigation_growing_env_update(uint8_t channel_id);

/**
 * @brief Send automatic calculation status notification via Bluetooth
 * 
 * Sends current automatic irrigation calculation status and results via BLE.
 * Provides real-time information about water balance calculations, ET0 values,
 * and irrigation requirements for automatic mode channels.
 * 
 * @return 0 on success, negative error code on failure
 */
int bt_irrigation_auto_calc_status_notify(void);

/**
 * @brief Notify BLE clients of growing environment changes
 * 
 * Triggers BLE notifications when growing environment parameters change,
 * including plant, soil, and irrigation method selections.
 * 
 * @return 0 on success, negative error code on failure
 */
int bt_irrigation_growing_env_notify(void);

/**
 * @brief Execute a direct command on a channel
 * 
 * Executes direct valve commands via BLE. Supports open, close, and pulse
 * operations with appropriate status updates.
 * 
 * @param channel_id Channel ID
 * @param command Command code (0=close, 1=open, 2=pulse)
 * @param param Additional parameter (e.g., pulse duration in seconds)
 * @return 0 on success, negative error code on failure
 */
int bt_irrigation_direct_command(uint8_t channel_id, uint8_t command, uint16_t param);

/**
 * @brief Force send task queue notification (for important changes)
 * 
 * Sends an immediate task queue status notification using the direct
 * notification system, bypassing rate limiting for important updates.
 * Retrieves current queue status from the watering system.
 * 
 * @return 0 on success, negative error code on failure
 */
int bt_irrigation_queue_status_notify(void);

/**
 * @brief Record watering error in history and notify via BLE
 * 
 * Records an error event in the irrigation history and sends a notification
 * via BLE. Also triggers alarm notifications for the error.
 * 
 * @param channel_id Channel ID
 * @param error_code Error code to record
 * @return 0 on success, negative error code on failure
 */
int bt_irrigation_record_error(uint8_t channel_id, uint8_t error_code);

/**
 * @brief Update statistics and history based on current flow data
 * 
 * Updates irrigation statistics and history data based on flow sensor readings
 * and sends notifications via BLE. Calls the watering system to update statistics
 * and then notifies BLE clients.
 * 
 * @param channel_id Channel ID
 * @param volume_ml Volume in milliliters
 * @return 0 on success, negative error code on failure
 */
int bt_irrigation_update_statistics_from_flow(uint8_t channel_id, uint32_t volume_ml);

/**
 * @brief Update statistics for a channel and notify clients
 * 
 * Updates the statistics for a specific channel with new watering data
 * and sends notification to connected BLE clients. This is the core function
 * for statistics tracking, called after watering events complete.
 * 
 * @param channel_id Channel ID (0-7)
 * @param volume_ml Volume watered in milliliters
 * @param timestamp Timestamp of the watering event in seconds
 * @return 0 on success, negative error code on failure
 */
int bt_irrigation_update_statistics(uint8_t channel_id, uint32_t volume_ml, uint32_t timestamp);

/**
 * @brief Periodic function to update daily/monthly/annual aggregations
 * 
 * Updates historical data aggregations (daily, monthly, annual statistics)
 * and sends notifications via BLE. Processes all channels and updates
 * their respective aggregated statistics.
 * 
 * @return 0 on success, negative error code on failure
 */
int bt_irrigation_update_history_aggregations(void);

/**
 * @brief Debug and test BLE notification system
 * 
 * Force enables all BLE notifications and tests the notification system.
 * Useful for debugging notification issues and verifying client connectivity.
 * 
 * @return 0 on success, negative error code on failure
 */
int bt_irrigation_debug_notifications(void);

/**
 * @brief Periodically update rain data and notify BLE client.
 */
void bt_irrigation_rain_periodic_update(void);

/**
 * @brief Test channel configuration notification
 * 
 * Forces a channel configuration notification regardless of client subscription state.
 * Used for debugging and testing the BLE notification system.
 * 
 * @param channel_id Channel ID to test notification for
 * @return 0 on success, negative error code on failure
 */
int bt_irrigation_test_channel_notification(uint8_t channel_id);

/**
 * @brief Force enable all BLE notifications
 * 
 * Bypasses normal CCC (Client Characteristic Configuration) setup and forces
 * all notifications to be enabled. Useful for debugging client connectivity issues.
 * 
 * @return 0 on success, negative error code on failure
 */
int bt_irrigation_force_enable_notifications(void);

#else /* !CONFIG_BT */

/* Bluetooth disabled - provide no-op function declarations */
#include <stdint.h>
#include <stdbool.h>
#include "watering.h"
#include "rtc.h"  // Include RTC header for rtc_datetime_t type

int bt_irrigation_service_init(void);
int bt_irrigation_queue_status_notify(void);
int bt_irrigation_alarm_clear(uint8_t alarm_code);
int bt_irrigation_valve_status_update(uint8_t channel_id, bool is_open);
int bt_irrigation_flow_update(uint32_t flow_rate);
int bt_irrigation_update_statistics_from_flow(uint8_t channel_id, uint32_t volume_ml);
int bt_irrigation_queue_status_update(uint8_t task_id);
int bt_irrigation_alarm_notify(uint8_t alarm_code, uint16_t alarm_data);
int bt_irrigation_system_status_update(watering_status_t status);
int bt_irrigation_channel_config_update(uint8_t channel_id);
int bt_irrigation_current_task_update(uint8_t channel_id, uint32_t start_time,
                                    uint8_t mode, uint32_t target_value,
                                    uint32_t current_value, uint32_t total_volume);
int bt_irrigation_schedule_update(uint8_t channel_id);
int bt_irrigation_config_update(void);
int bt_irrigation_statistics_update(uint8_t channel_id);
int bt_irrigation_rtc_update(rtc_datetime_t *datetime);
int bt_irrigation_start_flow_calibration(uint8_t start, uint32_t volume_ml);
int bt_irrigation_history_update(uint8_t channel_id, uint8_t entry_index);
int bt_irrigation_history_get_detailed(uint8_t channel_id, uint32_t start_timestamp,
                                     uint32_t end_timestamp, uint8_t entry_index);
int bt_irrigation_history_get_daily(uint8_t channel_id, uint8_t entry_index);
int bt_irrigation_history_get_monthly(uint8_t channel_id, uint8_t entry_index);
int bt_irrigation_history_get_annual(uint8_t channel_id, uint8_t entry_index);
int bt_irrigation_history_notify_event(uint8_t channel_id, uint8_t event_type,
                                      uint32_t timestamp, uint32_t value);
int bt_irrigation_direct_command(uint8_t channel_id, uint8_t command, uint16_t param);
int bt_irrigation_record_error(uint8_t channel_id, uint8_t error_code);
int bt_irrigation_update_history_aggregations(void);
int bt_irrigation_auto_calc_status_notify(void);

/* Diagnostics functions */
/**
 * @brief Send diagnostics notification
 * 
 * Sends current system diagnostics via BLE notification if enabled.
 * 
 * @return 0 on success, negative error code on failure
 */
int bt_irrigation_diagnostics_notify(void);

/**
 * @brief Update diagnostics data and notify
 * 
 * Updates diagnostics data with current system state and sends notification if enabled.
 * 
 * @param error_count Total error count since boot
 * @param last_error Code of the most recent error (0 if no errors)
 * @param valve_status Valve status bitmap (bit 0 = channel 0, etc.)
 * @return 0 on success, negative error code on failure
 */
int bt_irrigation_diagnostics_update(uint16_t error_count, uint8_t last_error, uint8_t valve_status);

/* Environmental data notification functions */
/**
 * @brief Send environmental data notification
 * @return 0 on success, negative error code on failure
 */
int bt_irrigation_environmental_data_notify(void);

/**
 * @brief Send compensation status notification
 * @param channel_id Channel ID to send compensation status for
 * @return 0 on success, negative error code on failure
 */
int bt_irrigation_compensation_status_notify(uint8_t channel_id);

/**
 * @brief Send hydraulic status notification
 * @param channel_id Channel ID or 0xFF to use cached selection
 * @return 0 on success, negative error code on failure
 */
int bt_irrigation_hydraulic_status_notify(uint8_t channel_id);

/**
 * @brief Send interval mode phase change notification
 * @param channel_id Channel ID in interval mode
 * @param is_watering True if currently watering, false if pausing
 * @param phase_remaining_sec Seconds remaining in current phase
 * @return 0 on success, negative error code on failure
 */
int bt_irrigation_interval_mode_phase_notify(uint8_t channel_id, bool is_watering, uint32_t phase_remaining_sec);

/* Timezone configuration functions */
/**
 * @brief Read timezone configuration from BLE characteristic
 * 
 * Handles read requests for the timezone configuration characteristic,
 * returning current timezone settings including UTC offset and DST rules.
 * 
 * @param conn BLE connection handle
 * @param attr GATT attribute
 * @param buf Buffer to write data to
 * @param len Length of buffer
 * @param offset Read offset
 * @return Number of bytes read, or negative error code
 */
size_t read_timezone(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                      void *buf, uint16_t len, uint16_t offset);

/**
 * @brief Write timezone configuration to BLE characteristic
 * 
 * Handles write requests for the timezone configuration characteristic,
 * updating timezone settings and storing them in NVS.
 * 
 * @param conn BLE connection handle
 * @param attr GATT attribute
 * @param buf Buffer containing new timezone data
 * @param len Length of data
 * @param offset Write offset (must be 0)
 * @param flags Write flags
 * @return Number of bytes written, or negative error code
 */
size_t write_timezone(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                       const void *buf, uint16_t len, uint16_t offset, uint8_t flags);

/**
 * @brief Timezone CCC changed callback
 * 
 * Called when client enables/disables notifications for timezone characteristic.
 * 
 * @param attr GATT attribute
 * @param value CCC value (notifications enabled/disabled)
 */
void timezone_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value);

/* Debugging and diagnostic functions */
void bt_irrigation_debug_notification_status(void);

/* Rain sensor BLE notification functions */

/**
 * @brief Send rain configuration notification
 * 
 * Notifies connected clients about rain sensor configuration changes.
 */
void bt_irrigation_rain_config_notify(void);

/**
 * @brief Send rain data notification
 * 
 * Notifies connected clients about current rain sensor data.
 */
void bt_irrigation_rain_data_notify(void);

/**
 * @brief Send rain sensor pulse notification
 * 
 * Called when rain is detected to notify clients of new pulse data.
 * 
 * @param pulse_count Current total pulse count
 * @param current_rate_mm_h Current rainfall rate in mm/h
 */
void bt_irrigation_rain_pulse_notify(uint32_t pulse_count, float current_rate_mm_h);

/**
 * @brief Send rain integration status notification
 * 
 * Notifies clients about rain-based irrigation adjustments.
 * 
 * @param channel_id Channel being affected
 * @param reduction_pct Percentage reduction in irrigation
 * @param skip_irrigation Whether irrigation is being skipped
 */
void bt_irrigation_rain_integration_notify(uint8_t channel_id, float reduction_pct, bool skip_irrigation);

/**
 * @brief Send full rain integration status notification
 * 
 * Sends the complete rain integration status structure to connected clients.
 * Used for initial synchronization and periodic full updates.
 * 
 * @return 0 on success, negative error code on failure
 */
int bt_irrigation_rain_integration_status_notify(void);

/**
 * @brief Periodic rain data update
 * 
 * Called periodically to update rain sensor data notifications.
 */
void bt_irrigation_rain_periodic_update(void);

/* Onboarding functions */
/**
 * @brief Notify onboarding status update
 * 
 * Sends notification to connected clients about onboarding progress changes.
 * Includes completion percentages for channels, system, and schedules.
 * 
 * @return 0 on success, negative error code on failure
 */
int bt_irrigation_onboarding_status_notify(void);

/**
 * @brief Notify reset control status update
 * 
 * Sends notification to connected clients about reset operation status.
 * Includes confirmation codes and operation results.
 * 
 * @return 0 on success, negative error code on failure
 */
int bt_irrigation_reset_control_notify(void);

/**
 * @brief Notify channel compensation config update
 * 
 * Sends notification to connected clients about per-channel rain and
 * temperature compensation settings. This allows apps to configure
 * compensation thresholds per-channel instead of relying on global defaults.
 * 
 * NOTE: Compensation only applies to TIME and VOLUME watering modes.
 * FAO-56 modes already incorporate weather data in their calculations.
 * 
 * @param channel_id Channel that was updated (0-7)
 * @return 0 on success, negative error code on failure
 */
int bt_irrigation_channel_comp_config_notify(uint8_t channel_id);

#endif /* CONFIG_BT */
#endif /* BT_IRRIGATION_SERVICE_H_ */
