/**
 * @file bt_custom_soil_handlers.c
 * @brief BLE handlers for custom soil configuration implementation
 */

#include "bt_custom_soil_handlers.h"
#include "custom_soil_db.h"
#include "watering.h"
#include "configuration_status.h"
#include "onboarding_state.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>
#include <string.h>

LOG_MODULE_REGISTER(bt_custom_soil, LOG_LEVEL_DBG);

/* Static storage for BLE responses */
static struct custom_soil_config_data custom_soil_response;
static struct config_reset_response_data reset_response;
static struct config_status_response_data status_response;
static struct enhanced_channel_config_data enhanced_config_response;

/* Notification state tracking */
static bool custom_soil_notifications_enabled = false;
static bool config_reset_notifications_enabled = false;
static bool config_status_notifications_enabled = false;

/* Custom UUIDs for the configuration service */
#define BT_UUID_CUSTOM_CONFIG_SERVICE_VAL \
    BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x9abc, 0xdef123456780)
#define BT_UUID_CUSTOM_SOIL_CONFIG_VAL \
    BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x9abc, 0xdef123456781)
#define BT_UUID_CUSTOM_CONFIG_RESET_VAL \
    BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x9abc, 0xdef123456782)
#define BT_UUID_CUSTOM_CONFIG_STATUS_VAL \
    BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x9abc, 0xdef123456783)

static struct bt_uuid_128 custom_config_service_uuid = BT_UUID_INIT_128(BT_UUID_CUSTOM_CONFIG_SERVICE_VAL);
static struct bt_uuid_128 custom_soil_config_uuid = BT_UUID_INIT_128(BT_UUID_CUSTOM_SOIL_CONFIG_VAL);
static struct bt_uuid_128 custom_config_reset_uuid = BT_UUID_INIT_128(BT_UUID_CUSTOM_CONFIG_RESET_VAL);
static struct bt_uuid_128 custom_config_status_uuid = BT_UUID_INIT_128(BT_UUID_CUSTOM_CONFIG_STATUS_VAL);

/* Attribute indices inside the custom configuration service */
#define CUSTOM_CFG_ATTR_SOIL_VALUE   2
#define CUSTOM_CFG_ATTR_RESET_VALUE  5
#define CUSTOM_CFG_ATTR_STATUS_VALUE 8

static void custom_soil_config_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value);
static void custom_config_reset_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value);
static void custom_config_status_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value);

BT_GATT_SERVICE_DEFINE(custom_config_svc,
    BT_GATT_PRIMARY_SERVICE(&custom_config_service_uuid.uuid),

    BT_GATT_CHARACTERISTIC(&custom_soil_config_uuid.uuid,
                           BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE | BT_GATT_CHRC_NOTIFY,
                           BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT,
                           bt_custom_soil_config_read, bt_custom_soil_config_write,
                           &custom_soil_response),
    BT_GATT_CCC(custom_soil_config_ccc_changed, BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT),

    BT_GATT_CHARACTERISTIC(&custom_config_reset_uuid.uuid,
                           BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
                           BT_GATT_PERM_READ_ENCRYPT,
                           bt_config_reset_read, NULL, &reset_response),
    BT_GATT_CCC(custom_config_reset_ccc_changed, BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT),

    BT_GATT_CHARACTERISTIC(&custom_config_status_uuid.uuid,
                           BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE | BT_GATT_CHRC_NOTIFY,
                           BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT,
                           bt_config_status_read, bt_config_status_write, &status_response),
    BT_GATT_CCC(custom_config_status_ccc_changed, BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT)
);

int bt_custom_soil_handlers_init(void)
{
    LOG_INF("Initializing custom soil BLE handlers");
    
    /* Clear response structures */
    memset(&custom_soil_response, 0, sizeof(custom_soil_response));
    memset(&reset_response, 0, sizeof(reset_response));
    memset(&status_response, 0, sizeof(status_response));
    memset(&enhanced_config_response, 0, sizeof(enhanced_config_response));
    
    LOG_INF("Custom soil BLE handlers initialized");
    return 0;
}

static void custom_soil_config_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
    custom_soil_notifications_enabled = (value == BT_GATT_CCC_NOTIFY);
    LOG_INF("Custom soil config notifications %s",
            custom_soil_notifications_enabled ? "enabled" : "disabled");

    if (custom_soil_notifications_enabled) {
        (void)bt_gatt_notify(NULL,
                             &custom_config_svc.attrs[CUSTOM_CFG_ATTR_SOIL_VALUE],
                             &custom_soil_response,
                             sizeof(custom_soil_response));
    }
}

static void custom_config_reset_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
    config_reset_notifications_enabled = (value == BT_GATT_CCC_NOTIFY);
    LOG_INF("Config reset notifications %s",
            config_reset_notifications_enabled ? "enabled" : "disabled");

    if (config_reset_notifications_enabled) {
        (void)bt_gatt_notify(NULL,
                             &custom_config_svc.attrs[CUSTOM_CFG_ATTR_RESET_VALUE],
                             &reset_response,
                             sizeof(reset_response));
    }
}

static void custom_config_status_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
    config_status_notifications_enabled = (value == BT_GATT_CCC_NOTIFY);
    LOG_INF("Config status notifications %s",
            config_status_notifications_enabled ? "enabled" : "disabled");

    if (config_status_notifications_enabled) {
        (void)bt_gatt_notify(NULL,
                             &custom_config_svc.attrs[CUSTOM_CFG_ATTR_STATUS_VALUE],
                             &status_response,
                             sizeof(status_response));
    }
}

ssize_t bt_custom_soil_config_read(struct bt_conn *conn,
                                  const struct bt_gatt_attr *attr,
                                  void *buf, uint16_t len, uint16_t offset)
{
    LOG_DBG("Custom soil config read request, offset=%d, len=%d", offset, len);
    
    /* Return the last response or empty structure */
    return bt_gatt_attr_read(conn, attr, buf, len, offset,
                            &custom_soil_response, sizeof(custom_soil_response));
}

ssize_t bt_custom_soil_config_write(struct bt_conn *conn,
                                   const struct bt_gatt_attr *attr,
                                   const void *buf, uint16_t len,
                                   uint16_t offset, uint8_t flags)
{
    if (offset != 0) {
        LOG_ERR("Custom soil config write with non-zero offset not supported");
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }
    
    if (len != sizeof(struct custom_soil_config_data)) {
        LOG_ERR("Invalid custom soil config data length: %d, expected %zu", 
                len, sizeof(struct custom_soil_config_data));
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }
    
    const struct custom_soil_config_data *soil_config = 
        (const struct custom_soil_config_data *)buf;
    
    LOG_INF("Custom soil config write: channel=%d, operation=%d, name='%s'",
            soil_config->channel_id, soil_config->operation, soil_config->name);
    
    /* Validate channel ID */
    if (soil_config->channel_id >= WATERING_CHANNELS_COUNT) {
        LOG_ERR("Invalid channel ID: %d", soil_config->channel_id);
        return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
    }
    
    /* Process the custom soil configuration */
    watering_error_t result = bt_process_custom_soil_from_ble(soil_config);
    
    /* Prepare response */
    memcpy(&custom_soil_response, soil_config, sizeof(custom_soil_response));
    custom_soil_response.status = (uint8_t)result;
    
    /* Send notification if enabled */
    if (custom_soil_notifications_enabled) {
        bt_custom_soil_config_notify(soil_config->channel_id, 
                                    soil_config->operation, result);
    }
    
    LOG_INF("Custom soil config operation completed with result: %d", result);
    return len;
}

ssize_t bt_config_reset_read(struct bt_conn *conn,
                            const struct bt_gatt_attr *attr,
                            void *buf, uint16_t len, uint16_t offset)
{
    LOG_DBG("Config reset read request, offset=%d, len=%d", offset, len);
    
    /* Return the last reset response */
    return bt_gatt_attr_read(conn, attr, buf, len, offset,
                            &reset_response, sizeof(reset_response));
}

ssize_t bt_config_reset_write(struct bt_conn *conn,
                             const struct bt_gatt_attr *attr,
                             const void *buf, uint16_t len,
                             uint16_t offset, uint8_t flags)
{
    if (offset != 0) {
        LOG_ERR("Config reset write with non-zero offset not supported");
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }
    
    if (len != sizeof(struct config_reset_request_data)) {
        LOG_ERR("Invalid config reset data length: %d, expected %zu", 
                len, sizeof(struct config_reset_request_data));
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }
    
    const struct config_reset_request_data *reset_request = 
        (const struct config_reset_request_data *)buf;
    
    LOG_INF("Config reset request: channel=%d, group=%d, reason='%s'",
            reset_request->channel_id, reset_request->group, reset_request->reason);
    
    /* Validate channel ID (0xFF means all channels) */
    if (reset_request->channel_id != 0xFF && 
        reset_request->channel_id >= WATERING_CHANNELS_COUNT) {
        LOG_ERR("Invalid channel ID: %d", reset_request->channel_id);
        return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
    }
    
    /* Validate group */
    if (reset_request->group > CONFIG_GROUP_ALL) {
        LOG_ERR("Invalid config group: %d", reset_request->group);
        return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
    }
    
    watering_error_t result = WATERING_SUCCESS;
    
    /* Perform reset operation */
    if (reset_request->channel_id == 0xFF) {
        /* Reset all channels */
        for (uint8_t ch = 0; ch < WATERING_CHANNELS_COUNT; ch++) {
            watering_error_t ch_result = channel_reset_config_group(
                ch, (config_group_t)reset_request->group, reset_request->reason);
            if (ch_result != WATERING_SUCCESS) {
                LOG_ERR("Failed to reset channel %d: %d", ch, ch_result);
                result = ch_result; /* Keep last error */
            }
        }
    } else {
        /* Reset specific channel */
        result = channel_reset_config_group(reset_request->channel_id,
                                          (config_group_t)reset_request->group,
                                          reset_request->reason);
    }
    
    /* Prepare response */
    reset_response.result = (uint8_t)result;
    reset_response.channel_id = reset_request->channel_id;
    reset_response.group = reset_request->group;
    
    /* Get updated configuration status for the response */
    if (reset_request->channel_id != 0xFF) {
        channel_config_status_t status;
        if (channel_get_config_status(reset_request->channel_id, &status) == WATERING_SUCCESS) {
            reset_response.new_basic_complete = status.basic_configured;
            reset_response.new_growing_env_complete = status.growing_env_configured;
            reset_response.new_compensation_complete = status.compensation_configured;
            reset_response.new_custom_soil_complete = status.custom_soil_configured;
            reset_response.new_interval_complete = status.interval_configured;
            reset_response.new_config_score = status.configuration_score;
        }
    }
    
    /* Send notification if enabled */
    if (config_reset_notifications_enabled) {
        bt_config_reset_notify(reset_request->channel_id, 
                              (config_group_t)reset_request->group, result);
    }
    
    LOG_INF("Config reset operation completed with result: %d", result);
    return len;
}

ssize_t bt_config_status_read(struct bt_conn *conn,
                             const struct bt_gatt_attr *attr,
                             void *buf, uint16_t len, uint16_t offset)
{
    LOG_DBG("Config status read request, offset=%d, len=%d", offset, len);
    
    /* Return the last status response */
    return bt_gatt_attr_read(conn, attr, buf, len, offset,
                            &status_response, sizeof(status_response));
}

ssize_t bt_config_status_write(struct bt_conn *conn,
                              const struct bt_gatt_attr *attr,
                              const void *buf, uint16_t len,
                              uint16_t offset, uint8_t flags)
{
    if (offset != 0) {
        LOG_ERR("Config status write with non-zero offset not supported");
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }
    
    if (len != sizeof(struct config_status_request_data)) {
        LOG_ERR("Invalid config status request length: %d, expected %zu", 
                len, sizeof(struct config_status_request_data));
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }
    
    const struct config_status_request_data *status_request = 
        (const struct config_status_request_data *)buf;
    
    LOG_DBG("Config status query: channel=%d, include_reset_log=%d",
            status_request->channel_id, status_request->include_reset_log);
    
    /* Validate channel ID (0xFF means query all channels - for now just return first) */
    uint8_t query_channel = status_request->channel_id;
    if (query_channel == 0xFF) {
        query_channel = 0; /* Default to first channel for now */
    }
    
    if (query_channel >= WATERING_CHANNELS_COUNT) {
        LOG_ERR("Invalid channel ID: %d", query_channel);
        return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
    }
    
    /* Get configuration status */
    channel_config_status_t status;
    watering_error_t result = channel_get_config_status(query_channel, &status);
    
    if (result == WATERING_SUCCESS) {
        /* Fill response structure */
        status_response.channel_id = query_channel;
        status_response.basic_complete = status.basic_configured;
        status_response.growing_env_complete = status.growing_env_configured;
        status_response.compensation_complete = status.compensation_configured;
        status_response.custom_soil_complete = status.custom_soil_configured;
        status_response.interval_complete = status.interval_configured;
        status_response.config_score = status.configuration_score;
        
        /* Check if automatic watering is allowed */
        bool can_water;
        channel_validate_config_completeness(query_channel, &can_water);
        status_response.can_auto_water = can_water ? 1 : 0;
        
        status_response.last_reset_timestamp = status.last_reset_timestamp;
        status_response.reset_count = status.reset_count;
        status_response.last_reset_group = 0xFF;
        memset(status_response.last_reset_reason, 0, sizeof(status_response.last_reset_reason));
        
        if (status_request->include_reset_log) {
            config_reset_log_t reset_log;
            if (config_status_get_reset_log(query_channel, &reset_log) == WATERING_SUCCESS &&
                reset_log.count > 0) {
                uint8_t capacity = ARRAY_SIZE(reset_log.entries);
                uint8_t latest_index = (reset_log.head + capacity - 1U) % capacity;
                const config_reset_log_entry_t *entry = &reset_log.entries[latest_index];

                status_response.last_reset_group = (uint8_t)entry->group;
                strncpy(status_response.last_reset_reason, entry->reason,
                        sizeof(status_response.last_reset_reason) - 1);
                status_response.last_reset_reason[sizeof(status_response.last_reset_reason) - 1] = '\0';
            }
        }
    } else {
        LOG_ERR("Failed to get config status for channel %d: %d", query_channel, result);
        memset(&status_response, 0, sizeof(status_response));
        status_response.channel_id = query_channel;
    }
    
    /* Send notification if enabled */
    if (config_status_notifications_enabled) {
        bt_config_status_notify(query_channel);
    }
    
    LOG_DBG("Config status query completed for channel %d", query_channel);
    return len;
}

watering_error_t bt_process_custom_soil_from_ble(const struct custom_soil_config_data *soil_config)
{
    if (!soil_config) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    watering_error_t result = WATERING_SUCCESS;
    
    switch (soil_config->operation) {
        case 0: /* Read operation - no action needed, data already in response */
            LOG_DBG("Custom soil read operation for channel %d", soil_config->channel_id);
            result = bt_get_custom_soil_for_ble(soil_config->channel_id, 
                                              (struct custom_soil_config_data *)&custom_soil_response);
            break;
            
        case 1: /* Create operation */
            LOG_INF("Creating custom soil '%s' for channel %d", 
                    soil_config->name, soil_config->channel_id);
            result = custom_soil_db_create(soil_config->channel_id,
                                         soil_config->name,
                                         soil_config->field_capacity,
                                         soil_config->wilting_point,
                                         soil_config->infiltration_rate,
                                         soil_config->bulk_density,
                                         soil_config->organic_matter);
            break;
            
        case 2: /* Update operation */
            LOG_INF("Updating custom soil '%s' for channel %d", 
                    soil_config->name, soil_config->channel_id);
            result = custom_soil_db_update(soil_config->channel_id,
                                         soil_config->name,
                                         soil_config->field_capacity,
                                         soil_config->wilting_point,
                                         soil_config->infiltration_rate,
                                         soil_config->bulk_density,
                                         soil_config->organic_matter);
            break;
            
        case 3: /* Delete operation */
            LOG_INF("Deleting custom soil for channel %d", soil_config->channel_id);
            result = custom_soil_db_delete(soil_config->channel_id);
            break;
            
        default:
            LOG_ERR("Invalid custom soil operation: %d", soil_config->operation);
            result = WATERING_ERROR_INVALID_PARAM;
            break;
    }
    
    return result;
}

watering_error_t bt_get_custom_soil_for_ble(uint8_t channel_id,
                                           struct custom_soil_config_data *soil_config)
{
    if (!soil_config) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    /* Clear the structure */
    memset(soil_config, 0, sizeof(*soil_config));
    soil_config->channel_id = channel_id;
    
    /* Try to read custom soil from database */
    custom_soil_entry_t custom_soil;
    watering_error_t result = custom_soil_db_read(channel_id, &custom_soil);
    
    if (result == WATERING_SUCCESS) {
        /* Fill BLE structure with custom soil data */
        strncpy(soil_config->name, custom_soil.name, sizeof(soil_config->name) - 1);
        soil_config->field_capacity = custom_soil.field_capacity;
        soil_config->wilting_point = custom_soil.wilting_point;
        soil_config->infiltration_rate = custom_soil.infiltration_rate;
        soil_config->bulk_density = custom_soil.bulk_density;
        soil_config->organic_matter = custom_soil.organic_matter;
        soil_config->created_timestamp = custom_soil.created_timestamp;
        soil_config->modified_timestamp = custom_soil.modified_timestamp;
        soil_config->crc32 = custom_soil.crc32;
        soil_config->status = 0; /* Success */
    } else {
        /* No custom soil found or error */
        soil_config->status = (uint8_t)result;
    }
    
    return result;
}

int bt_custom_soil_config_notify(uint8_t channel_id, uint8_t operation, watering_error_t result)
{
    if (!custom_soil_notifications_enabled) {
        return 0; /* Notifications not enabled */
    }

    LOG_DBG("Sending custom soil config notification: channel=%d, op=%d, result=%d",
            channel_id, operation, result);

    /* Update response with current data */
    bt_get_custom_soil_for_ble(channel_id, &custom_soil_response);
    custom_soil_response.operation = operation;
    custom_soil_response.status = (uint8_t)result;
    
    int err = bt_gatt_notify(NULL,
                             &custom_config_svc.attrs[CUSTOM_CFG_ATTR_SOIL_VALUE],
                             &custom_soil_response,
                             sizeof(custom_soil_response));
    if (err < 0) {
        LOG_WRN("Failed to send custom soil notification: %d", err);
        return err;
    }
    
    return 0;
}

int bt_config_reset_notify(uint8_t channel_id, config_group_t group, watering_error_t result)
{
    if (!config_reset_notifications_enabled) {
        return 0; /* Notifications not enabled */
    }
    
    LOG_DBG("Sending config reset notification: channel=%d, group=%d, result=%d",
            channel_id, group, result);

    int err = bt_gatt_notify(NULL,
                             &custom_config_svc.attrs[CUSTOM_CFG_ATTR_RESET_VALUE],
                             &reset_response,
                             sizeof(reset_response));
    if (err < 0) {
        LOG_WRN("Failed to send config reset notification: %d", err);
        return err;
    }
    
    return 0;
}

int bt_config_status_notify(uint8_t channel_id)
{
    if (!config_status_notifications_enabled) {
        return 0; /* Notifications not enabled */
    }
    
    LOG_DBG("Sending config status notification: channel=%d", channel_id);

    int err = bt_gatt_notify(NULL,
                             &custom_config_svc.attrs[CUSTOM_CFG_ATTR_STATUS_VALUE],
                             &status_response,
                             sizeof(status_response));
    if (err < 0) {
        LOG_WRN("Failed to send config status notification: %d", err);
        return err;
    }
    
    return 0;
}

watering_error_t bt_convert_to_enhanced_ble_config(uint8_t channel_id,
                                                  struct enhanced_channel_config_data *ble_config)
{
    if (channel_id >= WATERING_CHANNELS_COUNT || !ble_config) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    /* Clear the structure */
    memset(ble_config, 0, sizeof(*ble_config));
    
    /* Get current channel configuration */
    /* This would need to be implemented to read from the actual channel configuration */
    /* For now, just fill basic fields */
    ble_config->channel_id = channel_id;
    
    /* Get channel name */
    char channel_name[64];
    if (watering_get_channel_name(channel_id, channel_name, sizeof(channel_name)) == WATERING_SUCCESS) {
        strncpy(ble_config->name, channel_name, sizeof(ble_config->name) - 1);
    }
    
    /* Check if custom soil exists */
    if (custom_soil_db_exists(channel_id)) {
        custom_soil_entry_t custom_soil;
        if (custom_soil_db_read(channel_id, &custom_soil) == WATERING_SUCCESS) {
            ble_config->use_custom_soil = 1;
            strncpy(ble_config->custom_soil_name, custom_soil.name, 
                   sizeof(ble_config->custom_soil_name) - 1);
            ble_config->custom_field_capacity = custom_soil.field_capacity;
            ble_config->custom_wilting_point = custom_soil.wilting_point;
            ble_config->custom_infiltration_rate = custom_soil.infiltration_rate;
            ble_config->custom_bulk_density = custom_soil.bulk_density;
            ble_config->custom_organic_matter = custom_soil.organic_matter;
            ble_config->custom_soil_created = custom_soil.created_timestamp;
            ble_config->custom_soil_modified = custom_soil.modified_timestamp;
        }
    }
    
    /* Get configuration status */
    channel_config_status_t status;
    if (channel_get_config_status(channel_id, &status) == WATERING_SUCCESS) {
        ble_config->config_basic_complete = status.basic_configured;
        ble_config->config_growing_env_complete = status.growing_env_configured;
        ble_config->config_compensation_complete = status.compensation_configured;
        ble_config->config_custom_soil_complete = status.custom_soil_configured;
        ble_config->config_interval_complete = status.interval_configured;
        ble_config->config_score = status.configuration_score;
        ble_config->last_config_update = status.last_reset_timestamp;
    }
    
    return WATERING_SUCCESS;
}

watering_error_t bt_convert_from_enhanced_ble_config(const struct enhanced_channel_config_data *ble_config,
                                                    uint8_t channel_id)
{
    if (!ble_config || channel_id >= WATERING_CHANNELS_COUNT) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    /* Validate the configuration */
    watering_error_t result = bt_validate_enhanced_ble_config(ble_config);
    if (result != WATERING_SUCCESS) {
        return result;
    }
    
    /* Update channel name if provided */
    if (strlen(ble_config->name) > 0) {
        watering_set_channel_name(channel_id, ble_config->name);
    }
    
    /* Handle custom soil configuration */
    if (ble_config->use_custom_soil) {
        if (strlen(ble_config->custom_soil_name) > 0) {
            /* Create or update custom soil */
            result = custom_soil_db_create(channel_id,
                                         ble_config->custom_soil_name,
                                         ble_config->custom_field_capacity,
                                         ble_config->custom_wilting_point,
                                         ble_config->custom_infiltration_rate,
                                         ble_config->custom_bulk_density,
                                         ble_config->custom_organic_matter);
            if (result != WATERING_SUCCESS) {
                LOG_ERR("Failed to create custom soil from BLE config: %d", result);
                return result;
            }
        }
    } else {
        /* Remove custom soil if it exists */
        custom_soil_db_delete(channel_id);
    }

    /* Update remaining channel configuration fields */
    watering_channel_t *channel = NULL;
    result = watering_get_channel(channel_id, &channel);
    if (result != WATERING_SUCCESS || !channel) {
        LOG_ERR("Failed to access channel %u for enhanced config update: %d", channel_id, result);
        return result != WATERING_SUCCESS ? result : WATERING_ERROR_INVALID_PARAM;
    }

    /* Update plant information */
    plant_info_t plant_info = {0};
    plant_info.main_type = (plant_type_t)ble_config->plant_type;
    if (plant_info.main_type == PLANT_TYPE_OTHER) {
        plant_info.specific.custom = channel->custom_plant;
    }

    result = watering_set_plant_info(channel_id, &plant_info);
    if (result != WATERING_SUCCESS) {
        LOG_ERR("Failed to set plant info from BLE config: %d", result);
        return result;
    }

    /* Prepare coverage structure */
    channel_coverage_t coverage = {0};
    if (ble_config->coverage_type == 0) {
        coverage.use_area = true;
        coverage.area.area_m2 = ble_config->coverage.area_m2;
    } else {
        coverage.use_area = false;
        coverage.plants.count = ble_config->coverage.plant_count;
    }

    /* Use existing custom plant configuration if needed */
    const custom_plant_config_t *custom_cfg = NULL;
    if (plant_info.main_type == PLANT_TYPE_OTHER) {
        custom_cfg = &channel->custom_plant;
    }

    result = watering_set_channel_environment(
        channel_id,
        plant_info.main_type,
        (soil_type_t)ble_config->soil_type,
        (irrigation_method_t)ble_config->irrigation_method,
        &coverage,
        ble_config->sun_percentage,
        custom_cfg);
    if (result != WATERING_SUCCESS) {
        LOG_ERR("Failed to set channel environment from BLE config: %d", result);
        return result;
    }

    /* Rain compensation configuration */
    channel->rain_compensation.enabled = ble_config->rain_compensation_enabled != 0;
    channel->rain_compensation.sensitivity = ble_config->rain_sensitivity;
    channel->rain_compensation.lookback_hours = ble_config->rain_lookback_hours;
    channel->rain_compensation.skip_threshold_mm = ble_config->rain_skip_threshold_mm;
    channel->rain_compensation.reduction_factor = ble_config->rain_reduction_factor;

    /* Temperature compensation configuration */
    channel->temp_compensation.enabled = ble_config->temp_compensation_enabled != 0;
    channel->temp_compensation.base_temperature = ble_config->temp_base_temperature;
    channel->temp_compensation.sensitivity = ble_config->temp_sensitivity;
    channel->temp_compensation.min_factor = ble_config->temp_min_factor;
    channel->temp_compensation.max_factor = ble_config->temp_max_factor;

    /* Interval mode configuration */
    channel->interval_config.configured = ble_config->interval_mode_enabled != 0;
    channel->interval_config.watering_minutes = ble_config->interval_watering_minutes;
    channel->interval_config.watering_seconds = ble_config->interval_watering_seconds;
    channel->interval_config.pause_minutes = ble_config->interval_pause_minutes;
    channel->interval_config.pause_seconds = ble_config->interval_pause_seconds;
    channel->interval_config.phase_start_time = 0;

    channel->interval_config_shadow.watering_minutes = channel->interval_config.watering_minutes;
    channel->interval_config_shadow.watering_seconds = channel->interval_config.watering_seconds;
    channel->interval_config_shadow.pause_minutes = channel->interval_config.pause_minutes;
    channel->interval_config_shadow.pause_seconds = channel->interval_config.pause_seconds;
    channel->interval_config_shadow.total_target = 0;
    channel->interval_config_shadow.cycles_completed = 0;
    channel->interval_config_shadow.currently_watering = false;
    channel->interval_config_shadow.phase_start_time = 0;
    channel->interval_config_shadow.phase_remaining_sec = 0;
    channel->interval_config_shadow.configured = channel->interval_config.configured;

    /* Persist changes */
    result = watering_save_config();
    if (result != WATERING_SUCCESS) {
        LOG_WRN("Failed to persist enhanced BLE config for channel %u: %d", channel_id, result);
    }
    
    /* Update extended onboarding flags for advanced configurations */
    if (channel->rain_compensation.enabled) {
        onboarding_update_channel_extended_flag(channel_id, CHANNEL_EXT_FLAG_RAIN_COMP_SET, true);
    }
    if (channel->temp_compensation.enabled) {
        onboarding_update_channel_extended_flag(channel_id, CHANNEL_EXT_FLAG_TEMP_COMP_SET, true);
    }
    if (channel->enable_cycle_soak) {
        onboarding_update_channel_extended_flag(channel_id, CHANNEL_EXT_FLAG_CYCLE_SOAK_SET, true);
    }
    
    /* Check if FAO-56 is now ready after this configuration */
    onboarding_check_fao56_ready(channel_id);
    
    return result;
}

watering_error_t bt_validate_enhanced_ble_config(const struct enhanced_channel_config_data *ble_config)
{
    if (!ble_config) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    /* Validate channel ID */
    if (ble_config->channel_id >= WATERING_CHANNELS_COUNT) {
        LOG_ERR("Invalid channel ID in BLE config: %d", ble_config->channel_id);
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    /* Validate custom soil parameters if custom soil is enabled */
    if (ble_config->use_custom_soil) {
        watering_error_t result = custom_soil_db_validate_parameters(
            ble_config->custom_field_capacity,
            ble_config->custom_wilting_point,
            ble_config->custom_infiltration_rate,
            ble_config->custom_bulk_density,
            ble_config->custom_organic_matter
        );
        
        if (result != WATERING_SUCCESS) {
            LOG_ERR("Invalid custom soil parameters in BLE config");
            return result;
        }
        
        if (strlen(ble_config->custom_soil_name) == 0) {
            LOG_ERR("Custom soil name is required when custom soil is enabled");
            return WATERING_ERROR_CUSTOM_SOIL_INVALID;
        }
    }
    
    /* Validate rain compensation parameters */
    if (ble_config->rain_compensation_enabled) {
        if (ble_config->rain_sensitivity < 0.0f || ble_config->rain_sensitivity > 1.0f) {
            LOG_ERR("Invalid rain sensitivity: %.2f", (double)ble_config->rain_sensitivity);
            return WATERING_ERROR_INVALID_PARAM;
        }
        
        if (ble_config->rain_skip_threshold_mm < 0.0f || ble_config->rain_skip_threshold_mm > 100.0f) {
            LOG_ERR("Invalid rain skip threshold: %.2f", (double)ble_config->rain_skip_threshold_mm);
            return WATERING_ERROR_INVALID_PARAM;
        }
    }
    
    /* Validate temperature compensation parameters */
    if (ble_config->temp_compensation_enabled) {
        if (ble_config->temp_base_temperature < -10.0f || ble_config->temp_base_temperature > 50.0f) {
            LOG_ERR("Invalid base temperature: %.2f", (double)ble_config->temp_base_temperature);
            return WATERING_ERROR_INVALID_PARAM;
        }
        
        if (ble_config->temp_min_factor < 0.1f || ble_config->temp_min_factor > 2.0f) {
            LOG_ERR("Invalid temperature min factor: %.2f", (double)ble_config->temp_min_factor);
            return WATERING_ERROR_INVALID_PARAM;
        }
        
        if (ble_config->temp_max_factor < 0.1f || ble_config->temp_max_factor > 2.0f) {
            LOG_ERR("Invalid temperature max factor: %.2f", (double)ble_config->temp_max_factor);
            return WATERING_ERROR_INVALID_PARAM;
        }
    }
    
    /* Validate interval mode parameters */
    if (ble_config->interval_mode_enabled) {
        uint32_t watering_sec = (ble_config->interval_watering_minutes * 60) + 
                               ble_config->interval_watering_seconds;
        uint32_t pause_sec = (ble_config->interval_pause_minutes * 60) + 
                            ble_config->interval_pause_seconds;
        
        if (watering_sec < 1 || watering_sec > 3600) {
            LOG_ERR("Invalid interval watering duration: %d seconds", watering_sec);
            return WATERING_ERROR_INTERVAL_CONFIG;
        }
        
        if (pause_sec < 1 || pause_sec > 3600) {
            LOG_ERR("Invalid interval pause duration: %d seconds", pause_sec);
            return WATERING_ERROR_INTERVAL_CONFIG;
        }
    }
    
    return WATERING_SUCCESS;
}
