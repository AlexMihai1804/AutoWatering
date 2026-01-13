/**
 * @file bt_pack_handlers.h
 * @brief BLE handlers for plant pack management
 * 
 * Provides BLE GATT characteristic handlers for installing, listing,
 * and managing custom plants and packs on external flash storage.
 * 
 * Supports multi-part transfers for large pack installations using
 * a chunked protocol with START/DATA/COMMIT/ABORT operations.
 */

#ifndef BT_PACK_HANDLERS_H
#define BT_PACK_HANDLERS_H

#include <zephyr/bluetooth/gatt.h>
#include "pack_storage.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Configuration
 * ============================================================================ */

/** Maximum plants per pack transfer (64 plants * 156 bytes = 9984 bytes) */
#define PACK_TRANSFER_MAX_PLANTS    64

/** Transfer buffer size (64 plants * 156 bytes = 9984 bytes) */
#define PACK_TRANSFER_BUFFER_SIZE   (PACK_TRANSFER_MAX_PLANTS * sizeof(pack_plant_v1_t))

/** Maximum chunk size per BLE write (MTU dependent, typical 244 bytes) */
#define PACK_TRANSFER_CHUNK_SIZE    240

/** Transfer timeout in seconds */
#define PACK_TRANSFER_TIMEOUT_SEC   120

/* ============================================================================
 * Transfer Protocol
 * ============================================================================ */

/**
 * @brief Pack transfer opcodes
 */
typedef enum {
    PACK_XFER_OP_START = 0x01,   /**< Start new pack transfer */
    PACK_XFER_OP_DATA = 0x02,    /**< Data chunk */
    PACK_XFER_OP_COMMIT = 0x03,  /**< Commit (finalize) transfer */
    PACK_XFER_OP_ABORT = 0x04,   /**< Abort current transfer */
    PACK_XFER_OP_STATUS = 0x05,  /**< Query transfer status */
} pack_transfer_opcode_t;

/**
 * @brief Transfer state
 */
typedef enum {
    PACK_XFER_STATE_IDLE = 0,       /**< No transfer in progress */
    PACK_XFER_STATE_RECEIVING = 1,  /**< Receiving data chunks */
    PACK_XFER_STATE_COMPLETE = 2,   /**< Transfer complete, ready to commit */
    PACK_XFER_STATE_ERROR = 3,      /**< Transfer error occurred */
} pack_transfer_state_t;

/**
 * @brief Start transfer request
 * 
 * Format: [opcode(1)][pack_id(2)][version(2)][plant_count(2)][total_size(4)][crc32(4)][name(32)]
 */
typedef struct __attribute__((packed)) {
    uint8_t opcode;             /**< PACK_XFER_OP_START */
    uint16_t pack_id;           /**< Pack ID */
    uint16_t version;           /**< Pack version */
    uint16_t plant_count;       /**< Number of plants in pack */
    uint32_t total_size;        /**< Total payload size in bytes */
    uint32_t crc32;             /**< CRC32 of entire payload */
    char name[32];              /**< Pack name */
} bt_pack_xfer_start_t;

#define BT_PACK_XFER_START_SIZE 47

/**
 * @brief Data chunk header
 * 
 * Format: [opcode(1)][offset(4)][length(2)][data(N)]
 */
typedef struct __attribute__((packed)) {
    uint8_t opcode;             /**< PACK_XFER_OP_DATA */
    uint32_t offset;            /**< Byte offset in transfer */
    uint16_t length;            /**< Chunk data length */
    /* Followed by data bytes */
} bt_pack_xfer_data_header_t;

#define BT_PACK_XFER_DATA_HEADER_SIZE 7

/**
 * @brief Commit/Abort request
 * 
 * Format: [opcode(1)]
 */
typedef struct __attribute__((packed)) {
    uint8_t opcode;             /**< PACK_XFER_OP_COMMIT or PACK_XFER_OP_ABORT */
} bt_pack_xfer_control_t;

/**
 * @brief Transfer status response (notification/read)
 */
typedef struct __attribute__((packed)) {
    uint8_t state;              /**< pack_transfer_state_t */
    uint8_t progress_pct;       /**< Transfer progress 0-100% */
    uint16_t pack_id;           /**< Current pack ID (0 if idle) */
    uint32_t bytes_received;    /**< Bytes received so far */
    uint32_t bytes_expected;    /**< Total bytes expected */
    uint8_t last_error;         /**< Last error code */
    uint8_t reserved[3];
} bt_pack_xfer_status_t;

#define BT_PACK_XFER_STATUS_SIZE 16

/* ============================================================================
 * BLE Pack Structures (wire format)
 * ============================================================================ */

/**
 * @brief Plant install request (write to Pack Plant characteristic)
 * 
 * Write the full pack_plant_v1_t structure (156 bytes) to install a plant.
 */
typedef struct __attribute__((packed)) {
    pack_plant_v1_t plant;  /**< Plant data to install */
} bt_pack_plant_install_t;

/**
 * @brief Plant list request (write to trigger list)
 * 
 * Streaming mode: set max_count=0 to stream all matching plants via notifications.
 * Filter values:
 *   0xFF = CUSTOM_ONLY (default, app has CSV for built-in)
 *   0xFE = ALL (custom + built-in, for API users without CSV)
 *   0x00 = BUILTIN_ONLY (pack_id=0 plants only)
 *   0x01-0xFD = specific pack filter
 */
typedef struct __attribute__((packed)) {
    uint16_t offset;        /**< Pagination offset (0 for streaming) */
    uint8_t max_count;      /**< Max entries (1-10), or 0 = STREAM ALL via notifications */
    uint8_t filter_pack_id; /**< Filter: 0xFF=custom, 0xFE=all, 0x00=builtin, other=pack */
} bt_pack_plant_list_req_t;

/** Streaming mode trigger - set max_count to this value */
#define BT_PACK_STREAM_MODE         0

/** Filter values for plant list streaming */
#define PACK_FILTER_CUSTOM_ONLY     0xFF  /**< Only custom plants (default) */
#define PACK_FILTER_ALL             0xFE  /**< Custom + built-in plants */
#define PACK_FILTER_BUILTIN_ONLY    0x00  /**< Only built-in plants (pack 0) */

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
 * @brief Plant list response (read after list request, or notification in stream mode)
 * 
 * In streaming mode, firmware sends multiple notifications:
 *   - First notification has flags=0x80 (STARTING)
 *   - Middle notifications have flags=0x00 (NORMAL)
 *   - Last notification has flags=0x01 (COMPLETE)
 *   - On error, flags=0x02 (ERROR)
 */
typedef struct __attribute__((packed)) {
    uint16_t total_count;   /**< Total plants matching filter */
    uint8_t returned_count; /**< Number of entries in this notification (0-10) */
    uint8_t flags;          /**< Stream flags (see BT_PACK_STREAM_FLAG_*) */
    bt_pack_plant_list_entry_t entries[10]; /**< Up to 10 entries per notification */
} bt_pack_plant_list_resp_t;

/** Stream flag values */
#define BT_PACK_STREAM_FLAG_NORMAL      0x00  /**< More notifications coming */
#define BT_PACK_STREAM_FLAG_COMPLETE    0x01  /**< Stream finished successfully */
#define BT_PACK_STREAM_FLAG_ERROR       0x02  /**< Stream error, aborted */
#define BT_PACK_STREAM_FLAG_STARTING    0x80  /**< First notification of stream */

#define BT_PACK_PLANT_LIST_RESP_SIZE (4 + 10 * sizeof(bt_pack_plant_list_entry_t))

/**
 * @brief Plant delete request
 */
typedef struct __attribute__((packed)) {
    uint16_t plant_id;      /**< Plant ID to delete */
} bt_pack_plant_delete_t;

/**
 * @brief Pack storage stats response
 * 
 * Size: 26 bytes
 * 
 * Plant count breakdown:
 * - builtin_count: ROM plants (223, constant)
 * - plant_count: Total in flash storage (builtin provisioned + custom)
 * - custom_plant_count: Custom only (pack_id != 0), for sync logic
 */
typedef struct __attribute__((packed)) {
    uint32_t total_bytes;       /**< Total storage capacity */
    uint32_t used_bytes;        /**< Used storage */
    uint32_t free_bytes;        /**< Free storage */
    uint16_t plant_count;       /**< Total plants in flash storage */
    uint16_t custom_plant_count;/**< Custom plants only (pack_id != 0) */
    uint16_t pack_count;        /**< Number of packs */
    uint16_t builtin_count;     /**< Built-in plant count in ROM (223) */
    uint8_t status;             /**< 0=OK, 1=not mounted, 2=error */
    uint8_t reserved;
    uint32_t change_counter;    /**< Increments on install/delete (cache invalidation) */
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
 * Pack List Structures (for listing installed packs)
 * ============================================================================ */

/**
 * @brief Pack list request (write to Pack List characteristic)
 * 
 * Opcode 0x01: List packs
 * Opcode 0x02: Get pack content (plant IDs)
 */
typedef struct __attribute__((packed)) {
    uint8_t opcode;         /**< 0x01=list packs, 0x02=get content */
    uint16_t offset;        /**< Pagination offset (for list) or pack_id (for content) */
    uint8_t reserved;
} bt_pack_list_req_t;

#define BT_PACK_LIST_OP_LIST     0x01
#define BT_PACK_LIST_OP_CONTENT  0x02

/**
 * @brief Pack list entry (in response)
 */
typedef struct __attribute__((packed)) {
    uint16_t pack_id;       /**< Pack ID */
    uint16_t version;       /**< Pack version */
    uint16_t plant_count;   /**< Number of plants in pack */
    char name[24];          /**< Pack name (truncated) */
} bt_pack_list_entry_t;

/**
 * @brief Pack list response (read after list request)
 */
typedef struct __attribute__((packed)) {
    uint16_t total_count;   /**< Total packs available (including builtin) */
    uint8_t returned_count; /**< Number of entries in this response */
    uint8_t include_builtin;/**< 1 if builtin pack 0 is included */
    bt_pack_list_entry_t entries[4]; /**< Up to 4 entries per read */
} bt_pack_list_resp_t;

#define BT_PACK_LIST_RESP_SIZE (4 + 4 * sizeof(bt_pack_list_entry_t))

/**
 * @brief Pack content response (plant IDs in a pack)
 */
typedef struct __attribute__((packed)) {
    uint16_t pack_id;       /**< Pack ID */
    uint16_t version;       /**< Pack version */
    uint16_t total_plants;  /**< Total plants in pack */
    uint8_t returned_count; /**< Number of plant IDs in this response */
    uint8_t offset;         /**< Current offset */
    uint16_t plant_ids[16]; /**< Up to 16 plant IDs per read */
} bt_pack_content_resp_t;

#define BT_PACK_CONTENT_RESP_SIZE (8 + 16 * sizeof(uint16_t))

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

/* Pack List handlers - list installed packs and their contents */
ssize_t bt_pack_list_read(struct bt_conn *conn,
                          const struct bt_gatt_attr *attr,
                          void *buf, uint16_t len, uint16_t offset);

ssize_t bt_pack_list_write(struct bt_conn *conn,
                           const struct bt_gatt_attr *attr,
                           const void *buf, uint16_t len,
                           uint16_t offset, uint8_t flags);

/* ============================================================================
 * Pack Transfer Handlers
 * ============================================================================ */

/**
 * @brief Handle pack transfer write (START/DATA/COMMIT/ABORT)
 */
ssize_t bt_pack_xfer_write(struct bt_conn *conn,
                           const struct bt_gatt_attr *attr,
                           const void *buf, uint16_t len,
                           uint16_t offset, uint8_t flags);

/**
 * @brief Read pack transfer status
 */
ssize_t bt_pack_xfer_read(struct bt_conn *conn,
                          const struct bt_gatt_attr *attr,
                          void *buf, uint16_t len, uint16_t offset);

/**
 * @brief Get current transfer state
 */
pack_transfer_state_t bt_pack_get_transfer_state(void);

/**
 * @brief Abort any ongoing transfer
 */
void bt_pack_abort_transfer(void);

#ifdef __cplusplus
}
#endif

#endif /* BT_PACK_HANDLERS_H */
