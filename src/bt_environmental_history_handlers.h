#ifndef BT_ENVIRONMENTAL_HISTORY_HANDLERS_H
#define BT_ENVIRONMENTAL_HISTORY_HANDLERS_H

#include <stdint.h>
#include <stdbool.h>
#include "environmental_history.h"
#include "watering.h"
#include "bt_gatt_structs.h"

/**
 * @file bt_environmental_history_handlers.h
 * @brief BLE interface for environmental historical data access
 * 
 * This module provides BLE characteristics for accessing environmental
 * historical data with fragmentation support for large data transfers.
 */

/* BLE data structures for environmental history */

/*
 * Environmental History Command (matches BLE API docs)
 * TOTAL SIZE: 20 bytes
 */
typedef struct {
    uint8_t  command;         /* 0x01=GET_DETAILED, 0x02=GET_HOURLY, 0x03=GET_DAILY, 0x04=GET_TRENDS, 0x05=CLEAR_HISTORY */
    uint32_t start_time;      /* Unix start timestamp */
    uint32_t end_time;        /* Unix end timestamp */
    uint8_t  data_type;       /* 0=detailed, 1=hourly, 2=daily */
    uint8_t  max_records;     /* Max records to return (1-100) */
    uint8_t  fragment_id;     /* Fragment index to request (0-based) */
    uint8_t  reserved[7];     /* Reserved */
} __attribute__((packed)) ble_history_request_t;

/*
 * Environmental History Response Header (matches BLE API docs)
 * TOTAL SIZE: 240 bytes (8B header + 232B data)
 */
typedef struct {
    uint8_t status;            /* 0=success, >0=error */
    uint8_t data_type;         /* 0=detailed, 1=hourly, 2=daily */
    uint8_t record_count;      /* Records in this fragment */
    uint8_t fragment_id;       /* Current fragment index */
    uint8_t total_fragments;   /* Total fragments for this request */
    uint8_t reserved[3];       /* Reserved */
    uint8_t data[232];         /* Packed records */
} __attribute__((packed)) ble_history_response_t;

/* Environmental history statistics structure */
typedef struct {
    uint16_t hourly_entries_used;     // Number of hourly entries used
    uint16_t daily_entries_used;      // Number of daily entries used
    uint16_t monthly_entries_used;    // Number of monthly entries used
    uint32_t oldest_hourly_timestamp; // Oldest hourly entry timestamp
    uint32_t oldest_daily_timestamp;  // Oldest daily entry timestamp
    uint32_t oldest_monthly_timestamp; // Oldest monthly entry timestamp
    uint32_t total_storage_bytes;     // Total storage bytes used
    uint8_t storage_utilization_pct;  // Storage utilization percentage
} __attribute__((packed)) ble_env_history_stats_t;

/* Environmental history reset request */
typedef struct {
    uint8_t reset_type;               // 0=all, 1=hourly, 2=daily, 3=monthly
    uint32_t confirmation_code;       // Confirmation code for safety
} __attribute__((packed)) ble_env_history_reset_t;

/**
 * @brief Handle environmental history data request
 * @param request Pointer to history request structure
 * @param response Pointer to response structure to fill
 * @return 0 on success, negative error code on failure
 */
int bt_env_history_request_handler(const ble_history_request_t *request, 
                                  ble_history_response_t *response);

/**
 * @brief Get next fragment of environmental history data
 * @param response Pointer to response structure to fill with next fragment
 * @return 0 on success, negative error code on failure
 */
/* No longer auto-iterates fragments; client must request specific fragment_id */
int bt_env_history_get_next_fragment(ble_history_response_t *response);

/**
 * @brief Get environmental history storage statistics
 * @param stats_response Pointer to statistics response structure to fill
 * @return 0 on success, negative error code on failure
 */
int bt_env_history_get_stats(ble_env_history_stats_t *stats_response);

/**
 * @brief Handle environmental history reset request
 * @param reset_request Pointer to reset request structure
 * @return 0 on success, negative error code on failure
 */
int bt_env_history_reset_request(const ble_env_history_reset_t *reset_request);

/**
 * @brief Check if a fragmented transfer is currently active
 * @return true if transfer is active, false otherwise
 */
bool bt_env_history_is_transfer_active(void);

/**
 * @brief Cancel any active fragmented transfer
 */
void bt_env_history_cancel_transfer(void);

#endif // BT_ENVIRONMENTAL_HISTORY_HANDLERS_H