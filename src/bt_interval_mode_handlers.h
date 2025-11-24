#ifndef BT_INTERVAL_MODE_HANDLERS_H
#define BT_INTERVAL_MODE_HANDLERS_H

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gatt.h>
#include "bt_gatt_structs_enhanced.h"
#include "interval_task_integration.h"

/**
 * @file bt_interval_mode_handlers.h
 * @brief BLE interface handlers for configurable interval mode
 * 
 * This module provides BLE GATT characteristic handlers for configuring
 * and monitoring interval-based watering mode via Bluetooth.
 */

/* Interval mode configuration structure for BLE */
struct interval_mode_config_data {
    uint8_t channel_id;             /* Channel ID (0-7) */
    uint8_t enabled;                /* 0=disabled, 1=enabled */
    uint16_t watering_minutes;      /* Watering duration in minutes (0-60) */
    uint8_t watering_seconds;       /* Watering duration in seconds (0-59) */
    uint16_t pause_minutes;         /* Pause duration in minutes (0-60) */
    uint8_t pause_seconds;          /* Pause duration in seconds (0-59) */
    uint8_t configured;             /* Whether interval settings are configured */
    uint32_t last_update;           /* Last configuration update timestamp */
    uint8_t reserved[4];            /* Reserved for future use */
} __attribute__((packed));

/* Interval mode status structure for BLE */
struct interval_mode_status_data {
    uint8_t channel_id;             /* Channel ID (0xFF if no active task) */
    uint8_t is_active;              /* Whether interval mode is currently active */
    uint8_t current_state;          /* Current interval state (interval_state_t) */
    uint8_t currently_watering;     /* 1=watering phase, 0=pause phase */
    uint32_t phase_remaining_sec;   /* Seconds remaining in current phase */
    uint32_t cycles_completed;      /* Number of complete cycles */
    uint32_t total_elapsed_sec;     /* Total elapsed time in seconds */
    uint32_t total_volume_ml;       /* Total volume dispensed in ml */
    uint8_t progress_percent;       /* Overall progress percentage (0-100) */
    uint32_t cycles_remaining;      /* Estimated remaining cycles */
    uint32_t next_phase_time;       /* Time until next phase change (seconds) */
    uint32_t estimated_completion;  /* Estimated completion time (timestamp) */
    uint8_t reserved[4];            /* Reserved for future use */
} __attribute__((packed));

/* Interval timing validation request structure */
struct interval_timing_validation_data {
    uint16_t watering_minutes;      /* Watering duration in minutes */
    uint8_t watering_seconds;       /* Watering duration in seconds */
    uint16_t pause_minutes;         /* Pause duration in minutes */
    uint8_t pause_seconds;          /* Pause duration in seconds */
    uint8_t validation_result;      /* Validation result (0=valid, error code if invalid) */
    uint32_t total_cycle_seconds;   /* Total cycle duration in seconds */
    char description[64];           /* Human-readable description */
    uint8_t reserved[4];            /* Reserved for future use */
} __attribute__((packed));

/**
 * @brief Read interval mode configuration for a channel
 * @param conn BLE connection
 * @param attr GATT attribute
 * @param buf Buffer to write response
 * @param len Buffer length
 * @param offset Read offset
 * @return Number of bytes written or negative error code
 */
ssize_t bt_interval_config_read(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                               void *buf, uint16_t len, uint16_t offset);

/**
 * @brief Write interval mode configuration for a channel
 * @param conn BLE connection
 * @param attr GATT attribute
 * @param buf Buffer containing new configuration
 * @param len Buffer length
 * @param offset Write offset
 * @param flags Write flags
 * @return Number of bytes written or negative error code
 */
ssize_t bt_interval_config_write(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                const void *buf, uint16_t len, uint16_t offset, uint8_t flags);

/**
 * @brief Read current interval mode status
 * @param conn BLE connection
 * @param attr GATT attribute
 * @param buf Buffer to write response
 * @param len Buffer length
 * @param offset Read offset
 * @return Number of bytes written or negative error code
 */
ssize_t bt_interval_status_read(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                               void *buf, uint16_t len, uint16_t offset);

/**
 * @brief Validate interval timing configuration
 * @param conn BLE connection
 * @param attr GATT attribute
 * @param buf Buffer containing timing to validate
 * @param len Buffer length
 * @param offset Write offset
 * @param flags Write flags
 * @return Number of bytes written or negative error code
 */
ssize_t bt_interval_timing_validate(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                   const void *buf, uint16_t len, uint16_t offset, uint8_t flags);

/**
 * @brief Enhanced current task status read with interval mode support
 * @param conn BLE connection
 * @param attr GATT attribute
 * @param buf Buffer to write response
 * @param len Buffer length
 * @param offset Read offset
 * @return Number of bytes written or negative error code
 */
ssize_t bt_enhanced_task_status_read(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                    void *buf, uint16_t len, uint16_t offset);

/**
 * @brief Update enhanced channel configuration with interval mode settings
 * @param conn BLE connection
 * @param attr GATT attribute
 * @param buf Buffer containing new configuration
 * @param len Buffer length
 * @param offset Write offset
 * @param flags Write flags
 * @return Number of bytes written or negative error code
 */
ssize_t bt_enhanced_channel_config_write(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                         const void *buf, uint16_t len, uint16_t offset, uint8_t flags);

/**
 * @brief Read enhanced channel configuration with interval mode settings
 * @param conn BLE connection
 * @param attr GATT attribute
 * @param buf Buffer to write response
 * @param len Buffer length
 * @param offset Read offset
 * @return Number of bytes written or negative error code
 */
ssize_t bt_enhanced_channel_config_read(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                       void *buf, uint16_t len, uint16_t offset);

/**
 * @brief Send interval mode status notification
 * @param channel_id Channel ID for notification
 * @return 0 on success, negative error code on failure
 */
int bt_interval_mode_notify_status(uint8_t channel_id);

/**
 * @brief Send interval phase change notification
 * @param channel_id Channel ID
 * @param new_phase New phase (watering/pausing)
 * @param phase_remaining_sec Seconds remaining in new phase
 * @return 0 on success, negative error code on failure
 */
int bt_interval_mode_notify_phase_change(uint8_t channel_id, bool new_phase, uint32_t phase_remaining_sec);

/**
 * @brief Send interval mode configuration update notification
 * @param channel_id Channel ID that was updated
 * @return 0 on success, negative error code on failure
 */
int bt_interval_mode_notify_config_update(uint8_t channel_id);

/**
 * @brief Initialize interval mode BLE handlers
 * @return 0 on success, negative error code on failure
 */
int bt_interval_mode_handlers_init(void);

/**
 * @brief Get interval mode configuration for BLE response
 * @param channel_id Channel ID
 * @param config_data Pointer to store configuration data
 * @return 0 on success, negative error code on failure
 */
int bt_interval_mode_get_config(uint8_t channel_id, struct interval_mode_config_data *config_data);

/**
 * @brief Set interval mode configuration from BLE request
 * @param config_data Pointer to configuration data
 * @return 0 on success, negative error code on failure
 */
int bt_interval_mode_set_config(const struct interval_mode_config_data *config_data);

/**
 * @brief Get interval mode status for BLE response
 * @param status_data Pointer to store status data
 * @return 0 on success, negative error code on failure
 */
int bt_interval_mode_get_status(struct interval_mode_status_data *status_data);

/**
 * @brief Validate interval timing and provide feedback
 * @param validation_data Pointer to validation data (input/output)
 * @return 0 on success, negative error code on failure
 */
int bt_interval_mode_validate_timing(struct interval_timing_validation_data *validation_data);

/**
 * @brief Update enhanced task status with interval mode information
 * @param status_data Pointer to store enhanced task status
 * @return 0 on success, negative error code on failure
 */
int bt_interval_mode_get_enhanced_task_status(struct enhanced_task_status_data *status_data);

/**
 * @brief Handle interval mode real-time notifications
 * @return 0 on success, negative error code on failure
 */
int bt_interval_mode_handle_notifications(void);

/**
 * @brief Check if interval mode notifications are enabled
 * @param conn BLE connection to check
 * @return true if notifications are enabled, false otherwise
 */
bool bt_interval_mode_notifications_enabled(struct bt_conn *conn);

/**
 * @brief Enable/disable interval mode notifications
 * @param conn BLE connection
 * @param enabled Whether to enable notifications
 * @return 0 on success, negative error code on failure
 */
int bt_interval_mode_set_notifications(struct bt_conn *conn, bool enabled);

#endif // BT_INTERVAL_MODE_HANDLERS_H