#include "bt_interval_mode_handlers.h"
#include "interval_task_integration.h"
#include "watering.h"
#include <zephyr/logging/log.h>
#include <zephyr/kernel.h>
#include <stdio.h>
#include <string.h>

LOG_MODULE_REGISTER(bt_interval_handlers, LOG_LEVEL_DBG);

/* External references */
extern watering_channel_t watering_channels[WATERING_CHANNELS_COUNT];

/* Notification state tracking */
static bool interval_notifications_enabled = false;
static struct bt_conn *notification_conn = NULL;

int bt_interval_mode_handlers_init(void)
{
    interval_notifications_enabled = false;
    notification_conn = NULL;
    
    LOG_INF("Interval mode BLE handlers initialized");
    return 0;
}

ssize_t bt_interval_config_read(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                               void *buf, uint16_t len, uint16_t offset)
{
    if (!buf || len < sizeof(struct interval_mode_config_data)) {
        LOG_ERR("Invalid buffer for interval config read");
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }

    // For now, return configuration for channel 0
    // In a full implementation, channel ID would be determined from context
    struct interval_mode_config_data config_data;
    int ret = bt_interval_mode_get_config(0, &config_data);
    if (ret != 0) {
        LOG_ERR("Failed to get interval config: %d", ret);
        return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
    }

    return bt_gatt_attr_read(conn, attr, buf, len, offset, &config_data, sizeof(config_data));
}

ssize_t bt_interval_config_write(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
    if (!buf || len != sizeof(struct interval_mode_config_data)) {
        LOG_ERR("Invalid buffer for interval config write");
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }

    if (offset != 0) {
        LOG_ERR("Invalid offset for interval config write");
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }

    const struct interval_mode_config_data *config_data = 
        (const struct interval_mode_config_data *)buf;

    int ret = bt_interval_mode_set_config(config_data);
    if (ret != 0) {
        LOG_ERR("Failed to set interval config: %d", ret);
        return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
    }

    // Send notification of configuration update
    bt_interval_mode_notify_config_update(config_data->channel_id);

    LOG_INF("Updated interval config for channel %u", config_data->channel_id);
    return len;
}

ssize_t bt_interval_status_read(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                               void *buf, uint16_t len, uint16_t offset)
{
    if (!buf || len < sizeof(struct interval_mode_status_data)) {
        LOG_ERR("Invalid buffer for interval status read");
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }

    struct interval_mode_status_data status_data;
    int ret = bt_interval_mode_get_status(&status_data);
    if (ret != 0) {
        LOG_ERR("Failed to get interval status: %d", ret);
        return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
    }

    return bt_gatt_attr_read(conn, attr, buf, len, offset, &status_data, sizeof(status_data));
}

ssize_t bt_interval_timing_validate(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                   const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
    if (!buf || len != sizeof(struct interval_timing_validation_data)) {
        LOG_ERR("Invalid buffer for interval timing validation");
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }

    if (offset != 0) {
        LOG_ERR("Invalid offset for interval timing validation");
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }

    struct interval_timing_validation_data *validation_data = 
        (struct interval_timing_validation_data *)buf;

    int ret = bt_interval_mode_validate_timing(validation_data);
    if (ret != 0) {
        LOG_ERR("Failed to validate interval timing: %d", ret);
        return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
    }

    LOG_DBG("Validated interval timing: %u:%02u water, %u:%02u pause, result=%u",
            validation_data->watering_minutes, validation_data->watering_seconds,
            validation_data->pause_minutes, validation_data->pause_seconds,
            validation_data->validation_result);

    return len;
}

ssize_t bt_enhanced_task_status_read(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                    void *buf, uint16_t len, uint16_t offset)
{
    if (!buf || len < sizeof(struct enhanced_task_status_data)) {
        LOG_ERR("Invalid buffer for enhanced task status read");
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }

    struct enhanced_task_status_data status_data;
    int ret = bt_interval_mode_get_enhanced_task_status(&status_data);
    if (ret != 0) {
        LOG_ERR("Failed to get enhanced task status: %d", ret);
        return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
    }

    return bt_gatt_attr_read(conn, attr, buf, len, offset, &status_data, sizeof(status_data));
}

int bt_interval_mode_get_config(uint8_t channel_id, struct interval_mode_config_data *config_data)
{
    if (!config_data || channel_id >= WATERING_CHANNELS_COUNT) {
        LOG_ERR("Invalid parameters for get config");
        return -EINVAL;
    }

    watering_channel_t *channel = &watering_channels[channel_id];
    
    memset(config_data, 0, sizeof(struct interval_mode_config_data));
    
    config_data->channel_id = channel_id;
    config_data->enabled = channel->interval_config.configured ? 1 : 0;
    config_data->watering_minutes = channel->interval_config.watering_minutes;
    config_data->watering_seconds = channel->interval_config.watering_seconds;
    config_data->pause_minutes = channel->interval_config.pause_minutes;
    config_data->pause_seconds = channel->interval_config.pause_seconds;
    config_data->configured = channel->interval_config.configured ? 1 : 0;
    /* watering_channel_t doesn't track last_config_update explicitly; use last reset timestamp if available */
    uint32_t last = channel->config_status.last_reset_timestamp;
    if (last == 0) {
        last = k_uptime_get_32();
    }
    config_data->last_update = last;

    return 0;
}

int bt_interval_mode_set_config(const struct interval_mode_config_data *config_data)
{
    if (!config_data || config_data->channel_id >= WATERING_CHANNELS_COUNT) {
        LOG_ERR("Invalid parameters for set config");
        return -EINVAL;
    }

    watering_channel_t *channel = &watering_channels[config_data->channel_id];
    
    // Update interval configuration via adapter to interval_config_t API
    interval_config_t tmp_cfg;
    memset(&tmp_cfg, 0, sizeof(tmp_cfg));
    tmp_cfg.watering_minutes = channel->interval_config.watering_minutes;
    tmp_cfg.watering_seconds = channel->interval_config.watering_seconds;
    tmp_cfg.pause_minutes = channel->interval_config.pause_minutes;
    tmp_cfg.pause_seconds = channel->interval_config.pause_seconds;
    tmp_cfg.total_target = 0;
    tmp_cfg.cycles_completed = 0;
    tmp_cfg.currently_watering = false;
    tmp_cfg.phase_start_time = 0;
    tmp_cfg.phase_remaining_sec = 0;
    tmp_cfg.configured = channel->interval_config.configured;

    int ret = interval_timing_update_config(&tmp_cfg,
                                           config_data->watering_minutes,
                                           config_data->watering_seconds,
                                           config_data->pause_minutes,
                                           config_data->pause_seconds);
    if (ret != 0) {
        LOG_ERR("Failed to update interval timing config: %d", ret);
        return ret;
    }

    // Copy updated values back to channel's interval_config structure
    channel->interval_config.watering_minutes = tmp_cfg.watering_minutes;
    channel->interval_config.watering_seconds = tmp_cfg.watering_seconds;
    channel->interval_config.pause_minutes = tmp_cfg.pause_minutes;
    channel->interval_config.pause_seconds = tmp_cfg.pause_seconds;
    /* In this firmware, 'configured' is the runtime enable gate for interval execution.
     * Treat config_data->enabled as authoritative ON/OFF, while still validating/storing durations. */
    channel->interval_config.configured = (config_data->enabled != 0) ? tmp_cfg.configured : false;
    channel->interval_config.phase_start_time = (uint64_t)tmp_cfg.phase_start_time;

    // Update configuration status
    channel->config_status.interval_configured = (config_data->enabled != 0);
    // Track last change time using existing reset timestamp field for reporting
    channel->config_status.last_reset_timestamp = k_uptime_get_32();

    LOG_INF("Set interval config for channel %u: %u:%02u water, %u:%02u pause, enabled=%u",
            config_data->channel_id, config_data->watering_minutes, config_data->watering_seconds,
            config_data->pause_minutes, config_data->pause_seconds, config_data->enabled);

    /* Persist configuration (debounced) */
    (void)watering_save_channel_config_priority(config_data->channel_id, true);

    return 0;
}

int bt_interval_mode_get_status(struct interval_mode_status_data *status_data)
{
    if (!status_data) {
        LOG_ERR("Invalid status_data pointer");
        return -EINVAL;
    }

    memset(status_data, 0, sizeof(struct interval_mode_status_data));

    // Check if interval mode is currently active
    bool is_interval_mode;
    int ret = interval_task_is_interval_mode(&is_interval_mode);
    if (ret != 0) {
        LOG_ERR("Failed to check interval mode status");
        return ret;
    }

    if (!is_interval_mode) {
        // No interval mode active
        status_data->channel_id = 0xFF;
        status_data->is_active = 0;
        return 0;
    }

    // Get enhanced task status
    enhanced_task_status_t task_status;
    ret = interval_task_get_status(&task_status);
    if (ret != 0) {
        LOG_ERR("Failed to get task status");
        return ret;
    }

    // Fill status data
    status_data->channel_id = 0; // Would need to be determined from active task
    status_data->is_active = 1;
    status_data->current_state = (uint8_t)task_status.state;
    status_data->currently_watering = task_status.interval.currently_watering ? 1 : 0;
    status_data->phase_remaining_sec = task_status.interval.phase_remaining_sec;
    status_data->cycles_completed = task_status.interval.cycles_completed;
    status_data->total_elapsed_sec = task_status.total_elapsed / 1000;
    status_data->total_volume_ml = task_status.total_volume;

    // Get progress information
    uint8_t progress_percent;
    uint32_t cycles_remaining;
    ret = interval_task_get_progress(&progress_percent, &cycles_remaining);
    if (ret == 0) {
        status_data->progress_percent = progress_percent;
        status_data->cycles_remaining = cycles_remaining;
    }

    // Get next phase time
    uint32_t next_phase_time_sec;
    ret = interval_task_get_next_phase_time(&next_phase_time_sec);
    if (ret == 0) {
        status_data->next_phase_time = next_phase_time_sec;
    }

    return 0;
}

int bt_interval_mode_validate_timing(struct interval_timing_validation_data *validation_data)
{
    if (!validation_data) {
        LOG_ERR("Invalid validation_data pointer");
        return -EINVAL;
    }

    // Validate the timing values
    int ret = interval_timing_validate_values(validation_data->watering_minutes,
                                             validation_data->watering_seconds);
    if (ret != 0) {
        validation_data->validation_result = (uint8_t)(-ret);
        snprintf(validation_data->description, sizeof(validation_data->description),
                "Invalid watering duration: %u:%02u",
                validation_data->watering_minutes, validation_data->watering_seconds);
        return 0; // Return success but with validation error in data
    }

    ret = interval_timing_validate_values(validation_data->pause_minutes,
                                         validation_data->pause_seconds);
    if (ret != 0) {
        validation_data->validation_result = (uint8_t)(-ret);
        snprintf(validation_data->description, sizeof(validation_data->description),
                "Invalid pause duration: %u:%02u",
                validation_data->pause_minutes, validation_data->pause_seconds);
        return 0; // Return success but with validation error in data
    }

    // Calculate total cycle duration
    uint32_t watering_sec, pause_sec;
    ret = interval_timing_convert_to_seconds(validation_data->watering_minutes,
                                            validation_data->watering_seconds,
                                            &watering_sec);
    if (ret != 0) {
        validation_data->validation_result = (uint8_t)(-ret);
        snprintf(validation_data->description, sizeof(validation_data->description),
                "Failed to convert watering duration");
        return 0;
    }

    ret = interval_timing_convert_to_seconds(validation_data->pause_minutes,
                                            validation_data->pause_seconds,
                                            &pause_sec);
    if (ret != 0) {
        validation_data->validation_result = (uint8_t)(-ret);
        snprintf(validation_data->description, sizeof(validation_data->description),
                "Failed to convert pause duration");
        return 0;
    }

    validation_data->total_cycle_seconds = watering_sec + pause_sec;
    validation_data->validation_result = 0; // Valid

    // Generate description
    snprintf(validation_data->description, sizeof(validation_data->description),
            "Valid: %u:%02u water, %u:%02u pause, cycle: %u:%02u",
            validation_data->watering_minutes, validation_data->watering_seconds,
            validation_data->pause_minutes, validation_data->pause_seconds,
            validation_data->total_cycle_seconds / 60, validation_data->total_cycle_seconds % 60);

    return 0;
}

int bt_interval_mode_get_enhanced_task_status(struct enhanced_task_status_data *status_data)
{
    if (!status_data) {
        LOG_ERR("Invalid status_data pointer");
        return -EINVAL;
    }

    memset(status_data, 0, sizeof(struct enhanced_task_status_data));

    // Get basic task status
    enhanced_task_status_t task_status;
    int ret = interval_task_get_status(&task_status);
    if (ret != 0) {
        // No active task
        status_data->channel_id = 0xFF;
        status_data->task_state = (uint8_t)TASK_STATE_IDLE;
        status_data->task_mode = (uint8_t)WATERING_BY_DURATION;
        return 0;
    }

    // Fill basic task information
    status_data->channel_id = 0; // Would need to be determined from active task
    status_data->task_state = (uint8_t)task_status.state;
    status_data->task_mode = (uint8_t)task_status.mode;
    status_data->remaining_time = task_status.remaining_time;
    status_data->total_elapsed = task_status.total_elapsed / 1000;
    status_data->total_volume = task_status.total_volume;

    // Check if interval mode is active
    bool is_interval_mode;
    ret = interval_task_is_interval_mode(&is_interval_mode);
    if (ret == 0 && is_interval_mode) {
        status_data->is_interval_mode = 1;
        status_data->currently_watering = task_status.interval.currently_watering ? 1 : 0;
        status_data->phase_remaining_sec = task_status.interval.phase_remaining_sec;
        status_data->cycles_completed = task_status.interval.cycles_completed;
        
        // Get interval configuration
        status_data->watering_minutes = task_status.interval.watering_minutes;
        status_data->watering_seconds = task_status.interval.watering_seconds;
        status_data->pause_minutes = task_status.interval.pause_minutes;
        status_data->pause_seconds = task_status.interval.pause_seconds;

        // Get timing information
        uint32_t next_phase_time_sec;
        ret = interval_task_get_next_phase_time(&next_phase_time_sec);
        if (ret == 0) {
            status_data->next_phase_time = k_uptime_get_32() + (next_phase_time_sec * 1000);
        }
    }

    // Add timestamps
    status_data->task_start_time = k_uptime_get_32() - (task_status.total_elapsed);
    status_data->phase_start_time = status_data->task_start_time; // Simplified

    return 0;
}

int bt_interval_mode_notify_status(uint8_t channel_id)
{
    if (!interval_notifications_enabled || !notification_conn) {
        return 0; // Notifications not enabled
    }

    struct interval_mode_status_data status_data;
    int ret = bt_interval_mode_get_status(&status_data);
    if (ret != 0) {
        LOG_ERR("Failed to get status for notification");
        return ret;
    }

    // In a full implementation, this would send a BLE notification
    LOG_DBG("Interval mode status notification for channel %u", channel_id);
    return 0;
}

int bt_interval_mode_notify_phase_change(uint8_t channel_id, bool new_phase, uint32_t phase_remaining_sec)
{
    if (!interval_notifications_enabled || !notification_conn) {
        return 0; // Notifications not enabled
    }

    LOG_INF("Interval phase change notification: channel=%u, watering=%s, remaining=%u sec",
            channel_id, new_phase ? "true" : "false", phase_remaining_sec);

    // Send status notification with updated phase information
    return bt_interval_mode_notify_status(channel_id);
}

int bt_interval_mode_notify_config_update(uint8_t channel_id)
{
    if (!interval_notifications_enabled || !notification_conn) {
        return 0; // Notifications not enabled
    }

    LOG_INF("Interval config update notification for channel %u", channel_id);
    
    // In a full implementation, this would send a BLE notification
    return 0;
}

bool bt_interval_mode_notifications_enabled(struct bt_conn *conn)
{
    return interval_notifications_enabled && (notification_conn == conn);
}

int bt_interval_mode_set_notifications(struct bt_conn *conn, bool enabled)
{
    if (enabled) {
        interval_notifications_enabled = true;
        notification_conn = conn;
        LOG_INF("Enabled interval mode notifications");
    } else {
        interval_notifications_enabled = false;
        notification_conn = NULL;
        LOG_INF("Disabled interval mode notifications");
    }

    return 0;
}

int bt_interval_mode_handle_notifications(void)
{
    if (!interval_notifications_enabled) {
        return 0;
    }

    // Check if we need to send any notifications
    bool is_interval_mode;
    int ret = interval_task_is_interval_mode(&is_interval_mode);
    if (ret != 0 || !is_interval_mode) {
        return 0;
    }

    // Send periodic status updates during interval mode
    static uint32_t last_notification_time = 0;
    uint32_t current_time = k_uptime_get_32();
    
    if (current_time - last_notification_time >= 2000) { // Every 2 seconds
        bt_interval_mode_notify_status(0); // Channel 0 for now
        last_notification_time = current_time;
    }

    return 0;
}