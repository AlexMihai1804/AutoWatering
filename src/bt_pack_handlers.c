/**
 * @file bt_pack_handlers.c
 * @brief BLE handlers for plant pack management implementation
 */

#include "bt_pack_handlers.h"
#include "pack_storage.h"
#include "plant_db.h"
#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/logging/log.h>
#include <string.h>

LOG_MODULE_REGISTER(bt_pack, LOG_LEVEL_DBG);

/* ============================================================================
 * Static Storage
 * ============================================================================ */

static bt_pack_plant_list_resp_t list_response;
static bt_pack_stats_resp_t stats_response;
static bt_pack_op_result_t op_result;

static bool pack_notifications_enabled = false;
static uint16_t list_offset = 0;
static uint8_t list_filter_pack = 0xFF;

/* ============================================================================
 * UUIDs - Added to custom config service (0xdef123456786, 0xdef123456787)
 * ============================================================================ */

#define BT_UUID_PACK_PLANT_VAL \
    BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x9abc, 0xdef123456786)
#define BT_UUID_PACK_STATS_VAL \
    BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x9abc, 0xdef123456787)

static struct bt_uuid_128 pack_plant_uuid = BT_UUID_INIT_128(BT_UUID_PACK_PLANT_VAL);
static struct bt_uuid_128 pack_stats_uuid = BT_UUID_INIT_128(BT_UUID_PACK_STATS_VAL);

/* ============================================================================
 * CCC Callbacks
 * ============================================================================ */

static void pack_plant_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
    ARG_UNUSED(attr);
    pack_notifications_enabled = (value == BT_GATT_CCC_NOTIFY);
    LOG_INF("Pack plant notifications %s", 
            pack_notifications_enabled ? "enabled" : "disabled");
}

/* ============================================================================
 * Read Handlers
 * ============================================================================ */

ssize_t bt_pack_plant_read(struct bt_conn *conn,
                           const struct bt_gatt_attr *attr,
                           void *buf, uint16_t len, uint16_t offset)
{
    ARG_UNUSED(conn);
    ARG_UNUSED(attr);
    
    if (!pack_storage_is_ready()) {
        LOG_WRN("Pack storage not ready");
        list_response.total_count = 0;
        list_response.returned_count = 0;
        return bt_gatt_attr_read(conn, attr, buf, len, offset,
                                 &list_response, 4);
    }
    
    /* Populate list response */
    pack_plant_list_entry_t entries[8];
    uint16_t count = 0;
    
    pack_result_t result = pack_storage_list_plants(entries, 8, &count, list_offset);
    if (result != PACK_RESULT_SUCCESS) {
        LOG_ERR("Failed to list plants: %d", result);
        list_response.total_count = 0;
        list_response.returned_count = 0;
        return bt_gatt_attr_read(conn, attr, buf, len, offset,
                                 &list_response, 4);
    }
    
    list_response.total_count = pack_storage_get_plant_count();
    list_response.returned_count = (uint8_t)count;
    list_response.reserved = 0;
    
    /* Copy entries with truncated names */
    for (uint8_t i = 0; i < count && i < 8; i++) {
        list_response.entries[i].plant_id = entries[i].plant_id;
        list_response.entries[i].pack_id = entries[i].pack_id;
        list_response.entries[i].version = entries[i].version;
        strncpy(list_response.entries[i].name, entries[i].name, 15);
        list_response.entries[i].name[15] = '\0';
    }
    
    size_t resp_size = 4 + count * sizeof(bt_pack_plant_list_entry_t);
    return bt_gatt_attr_read(conn, attr, buf, len, offset,
                             &list_response, resp_size);
}

ssize_t bt_pack_stats_read(struct bt_conn *conn,
                           const struct bt_gatt_attr *attr,
                           void *buf, uint16_t len, uint16_t offset)
{
    ARG_UNUSED(conn);
    ARG_UNUSED(attr);
    
    memset(&stats_response, 0, sizeof(stats_response));
    
    if (!pack_storage_is_ready()) {
        stats_response.status = 1; /* Not mounted */
        stats_response.builtin_count = PLANT_FULL_SPECIES_COUNT;
        return bt_gatt_attr_read(conn, attr, buf, len, offset,
                                 &stats_response, sizeof(stats_response));
    }
    
    pack_storage_stats_t stats;
    pack_result_t result = pack_storage_get_stats(&stats);
    
    if (result == PACK_RESULT_SUCCESS) {
        stats_response.total_bytes = stats.total_bytes;
        stats_response.used_bytes = stats.used_bytes;
        stats_response.free_bytes = stats.free_bytes;
        stats_response.plant_count = stats.plant_count;
        stats_response.pack_count = stats.pack_count;
        stats_response.status = 0;
    } else {
        stats_response.status = 2; /* Error */
    }
    
    stats_response.builtin_count = PLANT_FULL_SPECIES_COUNT;
    
    return bt_gatt_attr_read(conn, attr, buf, len, offset,
                             &stats_response, sizeof(stats_response));
}

/* ============================================================================
 * Write Handlers
 * ============================================================================ */

ssize_t bt_pack_plant_write(struct bt_conn *conn,
                            const struct bt_gatt_attr *attr,
                            const void *buf, uint16_t len,
                            uint16_t offset, uint8_t flags)
{
    ARG_UNUSED(conn);
    ARG_UNUSED(attr);
    ARG_UNUSED(flags);
    
    if (offset != 0) {
        LOG_WRN("Pack plant write with non-zero offset");
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }
    
    const uint8_t *data = buf;
    
    /* Check for list request (4 bytes) */
    if (len == sizeof(bt_pack_plant_list_req_t)) {
        const bt_pack_plant_list_req_t *req = buf;
        list_offset = req->offset;
        list_filter_pack = req->filter_pack_id;
        LOG_INF("Pack plant list request: offset=%u, filter=%u",
                list_offset, list_filter_pack);
        return len;
    }
    
    /* Check for delete request (2 bytes) */
    if (len == sizeof(bt_pack_plant_delete_t)) {
        const bt_pack_plant_delete_t *req = buf;
        LOG_INF("Pack plant delete request: id=%u", req->plant_id);
        
        pack_result_t result = pack_storage_delete_plant(req->plant_id);
        
        /* Prepare notification */
        op_result.operation = 1; /* delete */
        op_result.result = (uint8_t)result;
        op_result.plant_id = req->plant_id;
        op_result.version = 0;
        op_result.reserved = 0;
        
        if (pack_notifications_enabled) {
            bt_pack_notify_result(&op_result);
        }
        
        if (result == PACK_RESULT_SUCCESS) {
            LOG_INF("Plant %u deleted", req->plant_id);
        } else {
            LOG_WRN("Failed to delete plant %u: %d", req->plant_id, result);
        }
        
        return len;
    }
    
    /* Check for install request (120 bytes = pack_plant_v1_t) */
    if (len == sizeof(pack_plant_v1_t)) {
        const pack_plant_v1_t *plant = buf;
        LOG_INF("Pack plant install: id=%u, pack=%u, name=%s",
                plant->plant_id, plant->pack_id, plant->common_name);
        
        pack_result_t result = pack_storage_install_plant(plant);
        
        /* Prepare notification */
        op_result.operation = 0; /* install */
        op_result.result = (uint8_t)result;
        op_result.plant_id = plant->plant_id;
        op_result.version = plant->version;
        op_result.reserved = 0;
        
        if (pack_notifications_enabled) {
            bt_pack_notify_result(&op_result);
        }
        
        if (result == PACK_RESULT_SUCCESS || result == PACK_RESULT_UPDATED) {
            LOG_INF("Plant %u installed (version %u)", plant->plant_id, plant->version);
            return len;
        } else if (result == PACK_RESULT_ALREADY_CURRENT) {
            LOG_INF("Plant %u already current version", plant->plant_id);
            return len;
        } else {
            LOG_ERR("Failed to install plant %u: %d", plant->plant_id, result);
            return len; /* Still return len, error is in notification */
        }
    }
    
    LOG_WRN("Invalid pack plant write length: %u", len);
    return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
}

/* ============================================================================
 * Notification
 * ============================================================================ */

/* Forward declaration - will be set after service registration */
static const struct bt_gatt_attr *pack_plant_attr = NULL;

void bt_pack_notify_result(const bt_pack_op_result_t *result)
{
    if (!pack_notifications_enabled || !pack_plant_attr) {
        return;
    }
    
    int err = bt_gatt_notify(NULL, pack_plant_attr, result, sizeof(*result));
    if (err) {
        LOG_WRN("Failed to notify pack result: %d", err);
    }
}

/* ============================================================================
 * Service Definition - Separate service for packs
 * ============================================================================ */

#define BT_UUID_PACK_SERVICE_VAL \
    BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x9abc, 0xdef123456800)

static struct bt_uuid_128 pack_service_uuid = BT_UUID_INIT_128(BT_UUID_PACK_SERVICE_VAL);

BT_GATT_SERVICE_DEFINE(pack_svc,
    BT_GATT_PRIMARY_SERVICE(&pack_service_uuid.uuid),
    
    /* Pack Plant characteristic - install/delete/list plants */
    BT_GATT_CHARACTERISTIC(&pack_plant_uuid.uuid,
                           BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE | BT_GATT_CHRC_NOTIFY,
                           BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT,
                           bt_pack_plant_read, bt_pack_plant_write,
                           &list_response),
    BT_GATT_CCC(pack_plant_ccc_changed, BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT),
    
    /* Pack Stats characteristic - storage statistics */
    BT_GATT_CHARACTERISTIC(&pack_stats_uuid.uuid,
                           BT_GATT_CHRC_READ,
                           BT_GATT_PERM_READ_ENCRYPT,
                           bt_pack_stats_read, NULL,
                           &stats_response)
);

/* Attribute index for notifications */
#define PACK_ATTR_PLANT_VALUE 2

/* ============================================================================
 * Initialization
 * ============================================================================ */

int bt_pack_handlers_init(void)
{
    LOG_INF("Initializing pack BLE handlers");
    
    memset(&list_response, 0, sizeof(list_response));
    memset(&stats_response, 0, sizeof(stats_response));
    memset(&op_result, 0, sizeof(op_result));
    
    pack_notifications_enabled = false;
    list_offset = 0;
    list_filter_pack = 0xFF;
    
    /* Get attribute pointer for notifications */
    pack_plant_attr = &pack_svc.attrs[PACK_ATTR_PLANT_VALUE];
    
    LOG_INF("Pack BLE handlers initialized (service UUID: def123456800)");
    return 0;
}
