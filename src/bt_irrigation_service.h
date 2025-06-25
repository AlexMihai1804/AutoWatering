#ifndef CONFIG_BT_MAX_PAIRED
#define CONFIG_BT_MAX_PAIRED 1
#endif
#ifndef CONFIG_BT_MAX_CONN
#define CONFIG_BT_MAX_CONN 1
#endif

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gatt.h>
#include "watering.h"
#include "rtc.h"  // Add RTC header for time synchronization

/**
 * @brief Initialize the Bluetooth irrigation service
 * 
 * @return 0 on success, negative error code on failure
 */
int bt_irrigation_service_init(void);

/**
 * @brief Update valve status via Bluetooth
 * 
 * @param channel_id Channel ID
 * @param state Valve state (0=off, 1=on)
 * @return 0 on success, negative error code on failure
 */
int bt_irrigation_valve_status_update(uint8_t channel_id, bool state);

/**
 * @brief Update flow data via Bluetooth
 * 
 * @param pulses Current pulse count
 * @return 0 on success, negative error code on failure
 */
int bt_irrigation_flow_update(uint32_t pulses);

/**
 * @brief Update system status via Bluetooth
 * 
 * @param status Current system status
 * @return 0 on success, negative error code on failure
 */
int bt_irrigation_system_status_update(watering_status_t status);

/**
 * @brief Update channel configuration via Bluetooth
 * 
 * @param channel_id Channel ID
 * @return 0 on success, negative error code on failure
 */
int bt_irrigation_channel_config_update(uint8_t channel_id);

/**
 * @brief Update pending tasks count via Bluetooth
 * 
 * @param count Number of pending tasks
 * @return 0 on success, negative error code on failure
 */
int bt_irrigation_queue_status_update(uint8_t count);

/**
 * @brief Update schedule configuration via Bluetooth
 * 
 * @param channel_id Channel ID
 * @return 0 on success, negative error code on failure
 */
int bt_irrigation_schedule_update(uint8_t channel_id);

/**
 * @brief Update system configuration via Bluetooth
 * 
 * @return 0 on success, negative error code on failure
 */
int bt_irrigation_config_update(void);

/**
 * @brief Update statistics via Bluetooth
 * 
 * @param channel_id Channel ID
 * @return 0 on success, negative error code on failure
 */
int bt_irrigation_statistics_update(uint8_t channel_id);

/**
 * @brief Update RTC time via Bluetooth
 * 
 * @param datetime Structure with new date and time
 * @return 0 on success, negative error code on failure
 */
int bt_irrigation_rtc_update(rtc_datetime_t *datetime);

/**
 * @brief Notify Bluetooth client about an alarm
 * 
 * @param alarm_code Alarm code
 * @param alarm_data Additional alarm data
 * @return 0 on success, negative error code on failure
 */
int bt_irrigation_alarm_notify(uint8_t alarm_code, uint16_t alarm_data);

/**
 * @brief Start flow sensor calibration session
 * 
 * @param start 1 to start, 0 to stop
 * @param volume_ml Water volume in ml for calibration (when stopping)
 * @return 0 on success, negative error code on failure
 */
int bt_irrigation_start_flow_calibration(uint8_t start, uint32_t volume_ml);

/**
 * @brief Update irrigation history
 * 
 * @param channel_id Channel ID
 * @param entry_index History entry index (0 = most recent)
 * @return 0 on success, negative error code on failure
 */
int bt_irrigation_history_update(uint8_t channel_id, uint8_t entry_index);

/**
 * @brief Update system diagnostics
 * 
 * @return 0 on success, negative error code on failure
 */
int bt_irrigation_diagnostics_update(void);

/**
 * @brief Execute a direct command on a channel
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
 * @return 0 on success, negative error code on failure
 */
int bt_irrigation_queue_status_notify(void);
