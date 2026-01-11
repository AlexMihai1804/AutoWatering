/**
 * @file bt_pack_handlers.h
 * @brief BLE handlers for plant pack management
 * 
 * Provides BLE GATT characteristic handlers for installing, listing,
 * and managing custom plants and packs on external flash storage.
 */

#ifndef BT_PACK_HANDLERS_H
#define BT_PACK_HANDLERS_H

#include <zephyr/bluetooth/gatt.h>
#include "pack_storage.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * BLE Pack Structures (wire format)
 * ============================================================================ */

/**
 * @brief Plant install request (write to Pack Plant characteristic)
 * 
 * Write the full pack_plant_v1_t structure (120 bytes) to install a plant.
 */
typedef struct __attribute__((packed)) {
    pack_plant_v1_t plant;  /**< Plant data to install */
} bt_pack_plant_install_t;

/**
 * @brief Plant list request (write to trigger list)
 */
typedef struct __attribute__((packed)) {
    uint16_t offset;        /**< Pagination offset */
    uint8_t max_count;      /**< Max entries to return (1-16) */
    uint8_t filter_pack_id; /**< Filter by pack_id (0xFF = all) */
} bt_pack_plant_list_req_t;

/**
 * @brief Plant list response entry
 */
typedef struct __attribute__((packed)) {
    uint16_t plant_id;      /**< Plant ID */
    uint16_t pack_id;       /**< Owning pack ID */
    uint16_t version;       /**< Installed version */
    char name[16];          /**< Truncated common name */
} bt_pack_plant_list_entry_t;

/**
 * @brief Plant list response (read after list request)
 */
typedef struct __attribute__((packed)) {
    uint16_t total_count;   /**< Total plants available */
    uint8_t returned_count; /**< Number of entries in this response */
    uint8_t reserved;
    bt_pack_plant_list_entry_t entries[8]; /**< Up to 8 entries per read */
} bt_pack_plant_list_resp_t;

#define BT_PACK_PLANT_LIST_RESP_SIZE (4 + 8 * sizeof(bt_pack_plant_list_entry_t))

/**
 * @brief Plant delete request
 */
typedef struct __attribute__((packed)) {
    uint16_t plant_id;      /**< Plant ID to delete */
} bt_pack_plant_delete_t;

/**
 * @brief Pack storage stats response
 */
typedef struct __attribute__((packed)) {
    uint32_t total_bytes;   /**< Total storage capacity */
    uint32_t used_bytes;    /**< Used storage */
    uint32_t free_bytes;    /**< Free storage */
    uint16_t plant_count;   /**< Number of custom plants */
    uint16_t pack_count;    /**< Number of packs */
    uint16_t builtin_count; /**< Built-in plant count (223) */
    uint8_t status;         /**< 0=OK, 1=not mounted, 2=error */
    uint8_t reserved;
} bt_pack_stats_resp_t;

/**
 * @brief Operation result (notification after install/delete)
 */
typedef struct __attribute__((packed)) {
    uint8_t operation;      /**< 0=install, 1=delete, 2=list */
    uint8_t result;         /**< pack_result_t value */
    uint16_t plant_id;      /**< Affected plant ID */
    uint16_t version;       /**< Installed version (for install) */
    uint16_t reserved;
} bt_pack_op_result_t;

/* ============================================================================
 * API Functions
 * ============================================================================ */

/**
 * @brief Initialize pack BLE handlers
 * @return 0 on success, negative error code on failure
 */
int bt_pack_handlers_init(void);

/**
 * @brief Notify operation result to connected clients
 * @param result Operation result to notify
 */
void bt_pack_notify_result(const bt_pack_op_result_t *result);

/* Read/Write handlers */
ssize_t bt_pack_plant_read(struct bt_conn *conn,
                           const struct bt_gatt_attr *attr,
                           void *buf, uint16_t len, uint16_t offset);

ssize_t bt_pack_plant_write(struct bt_conn *conn,
                            const struct bt_gatt_attr *attr,
                            const void *buf, uint16_t len,
                            uint16_t offset, uint8_t flags);

ssize_t bt_pack_stats_read(struct bt_conn *conn,
                           const struct bt_gatt_attr *attr,
                           void *buf, uint16_t len, uint16_t offset);

#ifdef __cplusplus
}
#endif

#endif /* BT_PACK_HANDLERS_H */
