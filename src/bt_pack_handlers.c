/**
 * @file bt_pack_handlers.c
 * @brief BLE handlers for plant pack management implementation
 * 
 * Implements both single-plant operations and multi-part pack transfers.
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
 * Static Storage - Single Plant Operations
 * ============================================================================ */

static bt_pack_plant_list_resp_t list_response;
static bt_pack_stats_resp_t stats_response;
static bt_pack_op_result_t op_result;

/* Pack list static storage */
static bt_pack_list_resp_t pack_list_response;
static bt_pack_content_resp_t pack_content_response;
static uint8_t pack_list_opcode = BT_PACK_LIST_OP_LIST;
static uint16_t pack_list_param = 0;

static bool pack_notifications_enabled = false;
static uint16_t list_offset = 0;
static uint8_t list_filter_pack = 0xFF;

/* ============================================================================
 * Static Storage - Pack Transfer
 * ============================================================================ */

/** Transfer buffer for accumulating pack data */
static uint8_t transfer_buffer[PACK_TRANSFER_BUFFER_SIZE] __attribute__((aligned(4)));

/** Current transfer state */
static struct {
    pack_transfer_state_t state;
    uint16_t pack_id;
    uint16_t pack_version;
    uint16_t plant_count;
    uint32_t total_size;
    uint32_t expected_crc32;
    uint32_t bytes_received;
    uint32_t last_activity_time;
    uint8_t last_error;
    char pack_name[32];
} xfer_state;

static bool pack_xfer_notifications_enabled = false;
static bt_pack_xfer_status_t xfer_status;

/* ============================================================================
 * UUIDs - Pack service characteristics
 * ============================================================================ */

#define BT_UUID_PACK_PLANT_VAL \
    BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x9abc, 0xdef123456786)
#define BT_UUID_PACK_STATS_VAL \
    BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x9abc, 0xdef123456787)
#define BT_UUID_PACK_XFER_VAL \
    BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x9abc, 0xdef123456788)
#define BT_UUID_PACK_LIST_VAL \
    BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x9abc, 0xdef123456789)

static struct bt_uuid_128 pack_plant_uuid = BT_UUID_INIT_128(BT_UUID_PACK_PLANT_VAL);
static struct bt_uuid_128 pack_stats_uuid = BT_UUID_INIT_128(BT_UUID_PACK_STATS_VAL);
static struct bt_uuid_128 pack_xfer_uuid = BT_UUID_INIT_128(BT_UUID_PACK_XFER_VAL);
static struct bt_uuid_128 pack_list_uuid = BT_UUID_INIT_128(BT_UUID_PACK_LIST_VAL);

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

static void pack_xfer_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
    ARG_UNUSED(attr);
    pack_xfer_notifications_enabled = (value == BT_GATT_CCC_NOTIFY);
    LOG_INF("Pack transfer notifications %s", 
            pack_xfer_notifications_enabled ? "enabled" : "disabled");
}

/* ============================================================================
 * Transfer State Machine
 * ============================================================================ */

static void xfer_reset(void)
{
    memset(&xfer_state, 0, sizeof(xfer_state));
    xfer_state.state = PACK_XFER_STATE_IDLE;
}

static void xfer_update_status(void)
{
    xfer_status.state = (uint8_t)xfer_state.state;
    xfer_status.pack_id = xfer_state.pack_id;
    xfer_status.bytes_received = xfer_state.bytes_received;
    xfer_status.bytes_expected = xfer_state.total_size;
    xfer_status.last_error = xfer_state.last_error;
    
    if (xfer_state.total_size > 0) {
        xfer_status.progress_pct = (uint8_t)((xfer_state.bytes_received * 100) / xfer_state.total_size);
    } else {
        xfer_status.progress_pct = 0;
    }
}

static void xfer_notify_status(void)
{
    if (!pack_xfer_notifications_enabled) {
        return;
    }
    
    xfer_update_status();
    
    /* Will notify via the xfer characteristic - need attr pointer */
    /* For now just log */
    LOG_DBG("Transfer status: state=%d, progress=%d%%, bytes=%u/%u",
            xfer_status.state, xfer_status.progress_pct,
            xfer_status.bytes_received, xfer_status.bytes_expected);
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
        stats_response.change_counter = stats.change_counter;
        stats_response.status = 0;
    } else {
        stats_response.status = 2; /* Error */
    }
    
    stats_response.builtin_count = PLANT_FULL_SPECIES_COUNT;
    
    return bt_gatt_attr_read(conn, attr, buf, len, offset,
                             &stats_response, sizeof(stats_response));
}

/* ============================================================================
 * Pack List Read/Write Handlers
 * ============================================================================ */

ssize_t bt_pack_list_read(struct bt_conn *conn,
                          const struct bt_gatt_attr *attr,
                          void *buf, uint16_t len, uint16_t offset)
{
    ARG_UNUSED(conn);
    ARG_UNUSED(attr);
    
    if (pack_list_opcode == BT_PACK_LIST_OP_CONTENT) {
        /* Return pack content (plant IDs) */
        memset(&pack_content_response, 0, sizeof(pack_content_response));
        
        if (!pack_storage_is_ready()) {
            pack_content_response.pack_id = pack_list_param;
            return bt_gatt_attr_read(conn, attr, buf, len, offset,
                                     &pack_content_response, sizeof(pack_content_response));
        }
        
        /* Get pack info with plant IDs */
        pack_pack_v1_t pack_data;
        uint16_t plant_ids[64];
        pack_result_t result = pack_storage_get_pack(pack_list_param, &pack_data, 
                                                      plant_ids, 64);
        
        if (result != PACK_RESULT_SUCCESS) {
            /* Check if it's the builtin pack (pack_id=0) */
            if (pack_list_param == 0) {
                pack_content_response.pack_id = 0;
                pack_content_response.version = 1;
                pack_content_response.total_plants = PLANT_FULL_SPECIES_COUNT;
                pack_content_response.returned_count = 0; /* Too many to list */
                pack_content_response.offset = 0;
                LOG_INF("Builtin pack has %u plants (not enumerated)", 
                        PLANT_FULL_SPECIES_COUNT);
            } else {
                LOG_WRN("Pack %u not found: %d", pack_list_param, result);
                pack_content_response.pack_id = pack_list_param;
            }
            return bt_gatt_attr_read(conn, attr, buf, len, offset,
                                     &pack_content_response, sizeof(pack_content_response));
        }
        
        pack_content_response.pack_id = pack_list_param;
        pack_content_response.version = pack_data.version;
        pack_content_response.total_plants = pack_data.plant_count;
        pack_content_response.offset = 0;
        
        /* Copy up to 16 plant IDs */
        uint8_t copy_count = (pack_data.plant_count > 16) ? 16 : pack_data.plant_count;
        pack_content_response.returned_count = copy_count;
        for (uint8_t i = 0; i < copy_count; i++) {
            pack_content_response.plant_ids[i] = plant_ids[i];
        }
        
        size_t resp_size = 8 + copy_count * sizeof(uint16_t);
        return bt_gatt_attr_read(conn, attr, buf, len, offset,
                                 &pack_content_response, resp_size);
    }
    
    /* Default: Return pack list */
    memset(&pack_list_response, 0, sizeof(pack_list_response));
    
    if (!pack_storage_is_ready()) {
        /* Return just builtin pack */
        pack_list_response.total_count = 1;
        pack_list_response.returned_count = 1;
        pack_list_response.include_builtin = 1;
        pack_list_response.entries[0].pack_id = 0;
        pack_list_response.entries[0].version = 1;
        pack_list_response.entries[0].plant_count = PLANT_FULL_SPECIES_COUNT;
        strncpy(pack_list_response.entries[0].name, "Built-in Plants", 23);
        return bt_gatt_attr_read(conn, attr, buf, len, offset,
                                 &pack_list_response, 4 + sizeof(bt_pack_list_entry_t));
    }
    
    /* Get custom packs */
    pack_pack_list_entry_t entries[4];
    uint16_t count = 0;
    uint16_t req_offset = pack_list_param;
    
    /* First entry is always builtin pack if offset is 0 */
    uint8_t entry_idx = 0;
    if (req_offset == 0) {
        pack_list_response.include_builtin = 1;
        pack_list_response.entries[0].pack_id = 0;
        pack_list_response.entries[0].version = 1;
        pack_list_response.entries[0].plant_count = PLANT_FULL_SPECIES_COUNT;
        strncpy(pack_list_response.entries[0].name, "Built-in Plants", 23);
        entry_idx = 1;
    }
    
    /* Get custom packs (max 3 if builtin included, 4 otherwise) */
    uint16_t custom_offset = (req_offset > 0) ? (req_offset - 1) : 0;
    pack_result_t result = pack_storage_list_packs(entries, 4 - entry_idx, 
                                                    &count, custom_offset);
    
    if (result == PACK_RESULT_SUCCESS) {
        for (uint16_t i = 0; i < count && entry_idx < 4; i++, entry_idx++) {
            pack_list_response.entries[entry_idx].pack_id = entries[i].pack_id;
            pack_list_response.entries[entry_idx].version = entries[i].version;
            pack_list_response.entries[entry_idx].plant_count = entries[i].plant_count;
            strncpy(pack_list_response.entries[entry_idx].name, entries[i].name, 23);
            pack_list_response.entries[entry_idx].name[23] = '\0';
        }
    }
    
    pack_list_response.total_count = 1 + pack_storage_get_pack_count(); /* +1 for builtin */
    pack_list_response.returned_count = entry_idx;
    
    size_t resp_size = 4 + entry_idx * sizeof(bt_pack_list_entry_t);
    return bt_gatt_attr_read(conn, attr, buf, len, offset,
                             &pack_list_response, resp_size);
}

ssize_t bt_pack_list_write(struct bt_conn *conn,
                           const struct bt_gatt_attr *attr,
                           const void *buf, uint16_t len,
                           uint16_t offset, uint8_t flags)
{
    ARG_UNUSED(conn);
    ARG_UNUSED(attr);
    ARG_UNUSED(flags);
    
    if (offset != 0) {
        LOG_WRN("Pack list write with non-zero offset");
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }
    
    if (len < sizeof(bt_pack_list_req_t)) {
        LOG_WRN("Pack list write too short: %u", len);
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }
    
    const bt_pack_list_req_t *req = buf;
    pack_list_opcode = req->opcode;
    pack_list_param = req->offset;
    
    if (req->opcode == BT_PACK_LIST_OP_LIST) {
        LOG_INF("Pack list request: offset=%u", req->offset);
    } else if (req->opcode == BT_PACK_LIST_OP_CONTENT) {
        LOG_INF("Pack content request: pack_id=%u", req->offset);
    } else {
        LOG_WRN("Unknown pack list opcode: 0x%02x", req->opcode);
    }
    
    return len;
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
    
    /* Check for install request (156 bytes = pack_plant_v1_t) */
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
 * Transfer Handlers
 * ============================================================================ */

ssize_t bt_pack_xfer_read(struct bt_conn *conn,
                          const struct bt_gatt_attr *attr,
                          void *buf, uint16_t len, uint16_t offset)
{
    ARG_UNUSED(conn);
    ARG_UNUSED(attr);
    
    xfer_update_status();
    
    return bt_gatt_attr_read(conn, attr, buf, len, offset,
                             &xfer_status, sizeof(xfer_status));
}

static int handle_xfer_start(const bt_pack_xfer_start_t *req)
{
    /* Validate state */
    if (xfer_state.state == PACK_XFER_STATE_RECEIVING) {
        LOG_WRN("Transfer already in progress, aborting previous");
        xfer_reset();
    }
    
    /* Validate request */
    if (req->plant_count == 0 || req->plant_count > PACK_TRANSFER_MAX_PLANTS) {
        LOG_ERR("Invalid plant count: %u (max=%u)", req->plant_count, PACK_TRANSFER_MAX_PLANTS);
        xfer_state.last_error = PACK_RESULT_INVALID_DATA;
        return -EINVAL;
    }
    
    uint32_t expected_size = req->plant_count * sizeof(pack_plant_v1_t);
    if (req->total_size != expected_size || req->total_size > PACK_TRANSFER_BUFFER_SIZE) {
        LOG_ERR("Invalid total size: %u (expected=%u, max=%u)", 
                req->total_size, expected_size, PACK_TRANSFER_BUFFER_SIZE);
        xfer_state.last_error = PACK_RESULT_INVALID_DATA;
        return -EINVAL;
    }
    
    /* Initialize transfer state */
    xfer_state.state = PACK_XFER_STATE_RECEIVING;
    xfer_state.pack_id = req->pack_id;
    xfer_state.pack_version = req->version;
    xfer_state.plant_count = req->plant_count;
    xfer_state.total_size = req->total_size;
    xfer_state.expected_crc32 = req->crc32;
    xfer_state.bytes_received = 0;
    xfer_state.last_error = PACK_RESULT_SUCCESS;
    xfer_state.last_activity_time = k_uptime_get_32();
    
    strncpy(xfer_state.pack_name, req->name, sizeof(xfer_state.pack_name) - 1);
    xfer_state.pack_name[sizeof(xfer_state.pack_name) - 1] = '\0';
    
    memset(transfer_buffer, 0, sizeof(transfer_buffer));
    
    LOG_INF("Pack transfer started: pack_id=%u v%u, plants=%u, size=%u, name=%s",
            req->pack_id, req->version, req->plant_count, req->total_size, req->name);
    
    xfer_notify_status();
    return 0;
}

static int handle_xfer_data(const uint8_t *buf, uint16_t len)
{
    if (xfer_state.state != PACK_XFER_STATE_RECEIVING) {
        LOG_ERR("Not in receiving state");
        return -EINVAL;
    }
    
    /* Check timeout (120 seconds) */
    if ((k_uptime_get_32() - xfer_state.last_activity_time) > PACK_TRANSFER_TIMEOUT_SEC * 1000) {
        LOG_ERR("Transfer timeout");
        xfer_state.state = PACK_XFER_STATE_ERROR;
        xfer_state.last_error = PACK_RESULT_IO_ERROR; /* Use IO_ERROR for timeout */
        xfer_notify_status();
        return -ETIMEDOUT;
    }
    
    /* Parse header - note: we already skipped the opcode byte */
    if (len < (sizeof(bt_pack_xfer_data_header_t) - 1)) { /* -1 because opcode already stripped */
        LOG_ERR("Data chunk too small: %u", len);
        return -EINVAL;
    }
    
    /* Reconstruct header from remaining bytes (offset + length fields) */
    uint32_t offset;
    uint16_t chunk_len;
    memcpy(&offset, buf, sizeof(offset));
    memcpy(&chunk_len, buf + sizeof(offset), sizeof(chunk_len));
    const uint8_t *payload = buf + sizeof(offset) + sizeof(chunk_len);
    uint16_t payload_len = len - sizeof(offset) - sizeof(chunk_len);
    
    /* Validate offset and length */
    if (offset != xfer_state.bytes_received) {
        LOG_ERR("Offset mismatch: got %u, expected %u", offset, xfer_state.bytes_received);
        xfer_state.last_error = PACK_RESULT_INVALID_DATA;
        return -EINVAL;
    }
    
    if (chunk_len != payload_len) {
        LOG_ERR("Length mismatch: header says %u, actual %u", chunk_len, payload_len);
        xfer_state.last_error = PACK_RESULT_INVALID_DATA;
        return -EINVAL;
    }
    
    if (xfer_state.bytes_received + payload_len > xfer_state.total_size) {
        LOG_ERR("Too much data: would have %u, expected %u",
                xfer_state.bytes_received + payload_len, xfer_state.total_size);
        xfer_state.last_error = PACK_RESULT_INVALID_DATA;
        return -EINVAL;
    }
    
    /* Copy data to buffer */
    memcpy(transfer_buffer + xfer_state.bytes_received, payload, payload_len);
    xfer_state.bytes_received += payload_len;
    xfer_state.last_activity_time = k_uptime_get_32();
    
    LOG_DBG("Received chunk offset=%u, len=%u, total=%u/%u",
            offset, payload_len, 
            xfer_state.bytes_received, xfer_state.total_size);
    
    /* Notify progress */
    xfer_notify_status();
    
    return 0;
}

static int handle_xfer_commit(void)
{
    if (xfer_state.state != PACK_XFER_STATE_RECEIVING) {
        LOG_ERR("Not in receiving state");
        return -EINVAL;
    }
    
    if (xfer_state.bytes_received != xfer_state.total_size) {
        LOG_ERR("Incomplete transfer: %u/%u bytes",
                xfer_state.bytes_received, xfer_state.total_size);
        xfer_state.state = PACK_XFER_STATE_ERROR;
        xfer_state.last_error = PACK_RESULT_INVALID_DATA;
        xfer_notify_status();
        return -EINVAL;
    }
    
    /* Verify CRC32 */
    uint32_t calc_crc = pack_storage_crc32(transfer_buffer, xfer_state.bytes_received);
    if (calc_crc != xfer_state.expected_crc32) {
        LOG_ERR("CRC32 mismatch: calc=0x%08x, expected=0x%08x",
                calc_crc, xfer_state.expected_crc32);
        xfer_state.state = PACK_XFER_STATE_ERROR;
        xfer_state.last_error = PACK_RESULT_CRC_MISMATCH;
        xfer_notify_status();
        return -EINVAL;
    }
    
    LOG_INF("CRC32 verified, installing %u plants...", xfer_state.plant_count);
    
    /* Parse and install each plant */
    const pack_plant_v1_t *plants = (const pack_plant_v1_t *)transfer_buffer;
    uint16_t installed = 0;
    uint16_t updated = 0;
    uint16_t errors = 0;
    
    for (uint16_t i = 0; i < xfer_state.plant_count; i++) {
        pack_result_t result = pack_storage_install_plant(&plants[i]);
        
        if (result == PACK_RESULT_SUCCESS) {
            installed++;
        } else if (result == PACK_RESULT_UPDATED) {
            updated++;
        } else if (result == PACK_RESULT_ALREADY_CURRENT) {
            /* Already current, count as success */
            installed++;
        } else {
            LOG_ERR("Failed to install plant %u: %d", plants[i].plant_id, result);
            errors++;
        }
    }
    
    LOG_INF("Pack transfer complete: installed=%u, updated=%u, errors=%u",
            installed, updated, errors);
    
    if (errors > 0) {
        xfer_state.state = PACK_XFER_STATE_ERROR;
        xfer_state.last_error = PACK_RESULT_IO_ERROR;
    } else {
        xfer_state.state = PACK_XFER_STATE_COMPLETE;
        xfer_state.last_error = PACK_RESULT_SUCCESS;
    }
    
    xfer_notify_status();
    
    /* Reset after short delay (give client time to read status) */
    /* For now, reset immediately - client should read status first */
    
    return (errors > 0) ? -EIO : 0;
}

static int handle_xfer_abort(void)
{
    LOG_INF("Pack transfer aborted by client");
    xfer_reset();
    xfer_notify_status();
    return 0;
}

ssize_t bt_pack_xfer_write(struct bt_conn *conn,
                           const struct bt_gatt_attr *attr,
                           const void *buf, uint16_t len,
                           uint16_t offset, uint8_t flags)
{
    ARG_UNUSED(conn);
    ARG_UNUSED(attr);
    ARG_UNUSED(flags);
    
    if (offset != 0) {
        LOG_WRN("Pack xfer write with non-zero offset");
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }
    
    if (len < 1) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }
    
    const uint8_t *data = buf;
    uint8_t opcode = data[0];
    
    int result = 0;
    
    switch (opcode) {
    case PACK_XFER_OP_START:
        if (len != sizeof(bt_pack_xfer_start_t)) {
            LOG_ERR("Invalid START size: %u (expected %zu)", len, sizeof(bt_pack_xfer_start_t));
            return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
        }
        result = handle_xfer_start((const bt_pack_xfer_start_t *)buf);
        break;
        
    case PACK_XFER_OP_DATA:
        result = handle_xfer_data(data + 1, len - 1); /* Skip opcode byte */
        break;
        
    case PACK_XFER_OP_COMMIT:
        result = handle_xfer_commit();
        break;
        
    case PACK_XFER_OP_ABORT:
        result = handle_xfer_abort();
        break;
        
    case PACK_XFER_OP_STATUS:
        /* Just return current status on next read */
        xfer_update_status();
        break;
        
    default:
        LOG_ERR("Unknown transfer opcode: 0x%02x", opcode);
        return BT_GATT_ERR(BT_ATT_ERR_NOT_SUPPORTED);
    }
    
    if (result < 0) {
        /* Return len anyway - error info is in status/notification */
        LOG_WRN("Transfer operation failed: %d", result);
    }
    
    return len;
}

/* Public API */
pack_transfer_state_t bt_pack_get_transfer_state(void)
{
    return xfer_state.state;
}

void bt_pack_abort_transfer(void)
{
    handle_xfer_abort();
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
                           &stats_response),
    
    /* Pack List characteristic - list installed packs and their contents */
    BT_GATT_CHARACTERISTIC(&pack_list_uuid.uuid,
                           BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE,
                           BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT,
                           bt_pack_list_read, bt_pack_list_write,
                           &pack_list_response),
    
    /* Pack Transfer characteristic - multi-part pack installation */
    BT_GATT_CHARACTERISTIC(&pack_xfer_uuid.uuid,
                           BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE | BT_GATT_CHRC_NOTIFY,
                           BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT,
                           bt_pack_xfer_read, bt_pack_xfer_write,
                           &xfer_status),
    BT_GATT_CCC(pack_xfer_ccc_changed, BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT)
);

/* Attribute indices for notifications */
#define PACK_ATTR_PLANT_VALUE 2
#define PACK_ATTR_XFER_VALUE  10

/* Transfer notification attribute pointer */
static const struct bt_gatt_attr *pack_xfer_attr = NULL;

/* ============================================================================
 * Initialization
 * ============================================================================ */

int bt_pack_handlers_init(void)
{
    LOG_INF("Initializing pack BLE handlers");
    
    memset(&list_response, 0, sizeof(list_response));
    memset(&stats_response, 0, sizeof(stats_response));
    memset(&op_result, 0, sizeof(op_result));
    memset(&pack_list_response, 0, sizeof(pack_list_response));
    memset(&pack_content_response, 0, sizeof(pack_content_response));
    
    pack_notifications_enabled = false;
    pack_xfer_notifications_enabled = false;
    list_offset = 0;
    list_filter_pack = 0xFF;
    pack_list_opcode = BT_PACK_LIST_OP_LIST;
    pack_list_param = 0;
    
    /* Reset transfer state */
    xfer_reset();
    memset(&xfer_status, 0, sizeof(xfer_status));
    
    /* Get attribute pointers for notifications */
    pack_plant_attr = &pack_svc.attrs[PACK_ATTR_PLANT_VALUE];
    pack_xfer_attr = &pack_svc.attrs[PACK_ATTR_XFER_VALUE];
    
    LOG_INF("Pack BLE handlers initialized (service UUID: def123456800)");
    LOG_INF("  - Plant characteristic: install/delete/list single plants");
    LOG_INF("  - Stats characteristic: storage statistics");
    LOG_INF("  - List characteristic: list packs and pack contents");
    LOG_INF("  - Transfer characteristic: multi-part pack installation");
    return 0;
}
