#ifndef BT_IRRIGATION_SERVICE_H
#define BT_IRRIGATION_SERVICE_H

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gatt.h>
#include "watering.h"

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

#endif // BT_IRRIGATION_SERVICE_H
