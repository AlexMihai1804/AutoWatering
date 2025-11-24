#ifndef BT_CUSTOM_SOIL_HANDLERS_H
#define BT_CUSTOM_SOIL_HANDLERS_H

/**
 * @file bt_custom_soil_handlers.h
 * @brief BLE handlers for custom soil configuration
 * 
 * This module provides BLE GATT characteristic handlers for managing
 * custom soil configurations through the Bluetooth interface.
 */

#include <zephyr/bluetooth/gatt.h>
#include "bt_gatt_structs_enhanced.h"
#include "watering_enhanced.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize custom soil BLE handlers
 * 
 * @return 0 on success, negative error code on failure
 */
int bt_custom_soil_handlers_init(void);

/**
 * @brief Read handler for custom soil configuration characteristic
 * 
 * @param conn BLE connection
 * @param attr GATT attribute
 * @param buf Buffer to write response
 * @param len Buffer length
 * @param offset Read offset
 * @return Number of bytes written or negative error code
 */
ssize_t bt_custom_soil_config_read(struct bt_conn *conn,
                                  const struct bt_gatt_attr *attr,
                                  void *buf, uint16_t len, uint16_t offset);

/**
 * @brief Write handler for custom soil configuration characteristic
 * 
 * @param conn BLE connection
 * @param attr GATT attribute
 * @param buf Buffer containing write data
 * @param len Data length
 * @param offset Write offset
 * @param flags Write flags
 * @return Number of bytes written or negative error code
 */
ssize_t bt_custom_soil_config_write(struct bt_conn *conn,
                                   const struct bt_gatt_attr *attr,
                                   const void *buf, uint16_t len,
                                   uint16_t offset, uint8_t flags);

/**
 * @brief Read handler for configuration reset characteristic
 * 
 * @param conn BLE connection
 * @param attr GATT attribute
 * @param buf Buffer to write response
 * @param len Buffer length
 * @param offset Read offset
 * @return Number of bytes written or negative error code
 */
ssize_t bt_config_reset_read(struct bt_conn *conn,
                            const struct bt_gatt_attr *attr,
                            void *buf, uint16_t len, uint16_t offset);

/**
 * @brief Write handler for configuration reset characteristic
 * 
 * @param conn BLE connection
 * @param attr GATT attribute
 * @param buf Buffer containing write data
 * @param len Data length
 * @param offset Write offset
 * @param flags Write flags
 * @return Number of bytes written or negative error code
 */
ssize_t bt_config_reset_write(struct bt_conn *conn,
                             const struct bt_gatt_attr *attr,
                             const void *buf, uint16_t len,
                             uint16_t offset, uint8_t flags);

/**
 * @brief Read handler for configuration status characteristic
 * 
 * @param conn BLE connection
 * @param attr GATT attribute
 * @param buf Buffer to write response
 * @param len Buffer length
 * @param offset Read offset
 * @return Number of bytes written or negative error code
 */
ssize_t bt_config_status_read(struct bt_conn *conn,
                             const struct bt_gatt_attr *attr,
                             void *buf, uint16_t len, uint16_t offset);

/**
 * @brief Write handler for configuration status query characteristic
 * 
 * @param conn BLE connection
 * @param attr GATT attribute
 * @param buf Buffer containing write data
 * @param len Data length
 * @param offset Write offset
 * @param flags Write flags
 * @return Number of bytes written or negative error code
 */
ssize_t bt_config_status_write(struct bt_conn *conn,
                              const struct bt_gatt_attr *attr,
                              const void *buf, uint16_t len,
                              uint16_t offset, uint8_t flags);

/**
 * @brief Update enhanced channel configuration with custom soil support
 * 
 * This function updates the existing channel configuration characteristic
 * to include custom soil parameters and other enhanced features.
 * 
 * @param channel_id Channel ID to update
 * @return 0 on success, negative error code on failure
 */
int bt_enhanced_channel_config_update(uint8_t channel_id);

/**
 * @brief Send custom soil configuration notification
 * 
 * @param channel_id Channel ID
 * @param operation Operation performed (create, update, delete)
 * @param result Operation result
 * @return 0 on success, negative error code on failure
 */
int bt_custom_soil_config_notify(uint8_t channel_id, uint8_t operation, watering_error_t result);

/**
 * @brief Send configuration reset notification
 * 
 * @param channel_id Channel ID that was reset
 * @param group Configuration group that was reset
 * @param result Reset operation result
 * @return 0 on success, negative error code on failure
 */
int bt_config_reset_notify(uint8_t channel_id, config_group_t group, watering_error_t result);

/**
 * @brief Send configuration status notification
 * 
 * @param channel_id Channel ID
 * @return 0 on success, negative error code on failure
 */
int bt_config_status_notify(uint8_t channel_id);

/**
 * @brief Convert channel configuration to enhanced BLE format
 * 
 * @param channel_id Channel ID
 * @param ble_config Pointer to BLE configuration structure to fill
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t bt_convert_to_enhanced_ble_config(uint8_t channel_id,
                                                  struct enhanced_channel_config_data *ble_config);

/**
 * @brief Convert enhanced BLE format to channel configuration
 * 
 * @param ble_config Pointer to BLE configuration structure
 * @param channel_id Channel ID to update
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t bt_convert_from_enhanced_ble_config(const struct enhanced_channel_config_data *ble_config,
                                                    uint8_t channel_id);

/**
 * @brief Validate enhanced channel configuration from BLE
 * 
 * @param ble_config Pointer to BLE configuration structure to validate
 * @return WATERING_SUCCESS if valid, error code otherwise
 */
watering_error_t bt_validate_enhanced_ble_config(const struct enhanced_channel_config_data *ble_config);

/**
 * @brief Get custom soil configuration for BLE response
 * 
 * @param channel_id Channel ID
 * @param soil_config Pointer to custom soil configuration structure to fill
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t bt_get_custom_soil_for_ble(uint8_t channel_id,
                                           struct custom_soil_config_data *soil_config);

/**
 * @brief Process custom soil configuration from BLE
 * 
 * @param soil_config Pointer to custom soil configuration from BLE
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t bt_process_custom_soil_from_ble(const struct custom_soil_config_data *soil_config);

#ifdef __cplusplus
}
#endif

#endif // BT_CUSTOM_SOIL_HANDLERS_H