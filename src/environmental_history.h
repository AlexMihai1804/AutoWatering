#ifndef ENVIRONMENTAL_HISTORY_H
#define ENVIRONMENTAL_HISTORY_H

#include <stdint.h>
#include <stdbool.h>
#include "watering_enhanced.h"
#include "watering.h"

/**
 * @file environmental_history.h
 * @brief Multi-resolution environmental history storage system
 * 
 * This module implements a comprehensive multi-resolution history storage system
 * for environmental data with automatic aggregation and ring buffer management.
 * 
 * Storage Hierarchy:
 * - Hourly data: 30 days retention (720 entries)
 * - Daily data: 12 months retention (372 entries) 
 * - Monthly data: 5 years retention (60 entries)
 */

/* Storage configuration constants */
#define ENV_HISTORY_HOURLY_ENTRIES      720    // 30 days × 24 hours
#define ENV_HISTORY_DAILY_ENTRIES       372    // 12 months × 31 days
#define ENV_HISTORY_MONTHLY_ENTRIES     60     // 5 years × 12 months

/* Aggregation timing constants */
#define ENV_HISTORY_HOURLY_INTERVAL_SEC 3600   // 1 hour
#define ENV_HISTORY_DAILY_INTERVAL_SEC  86400  // 24 hours
#define ENV_HISTORY_MONTHLY_INTERVAL_SEC 2592000 // 30 days (approx)

/* Ring buffer management thresholds */
#define ENV_HISTORY_CLEANUP_THRESHOLD   90     // Cleanup when 90% full
#define ENV_HISTORY_CLEANUP_TARGET      70     // Clean to 70% capacity

/* History data types for queries */
typedef enum {
    ENV_HISTORY_TYPE_HOURLY = 0,
    ENV_HISTORY_TYPE_DAILY = 1,
    ENV_HISTORY_TYPE_MONTHLY = 2
} env_history_data_type_t;

/* History query parameters */
typedef struct {
    env_history_data_type_t data_type;
    uint32_t start_timestamp;
    uint32_t end_timestamp;
    uint8_t channel_filter;           // 0xFF for all channels
    uint16_t max_entries;             // Maximum entries to return
} env_history_query_t;

/* History query result */
typedef struct {
    env_history_data_type_t data_type;
    uint16_t entry_count;
    uint32_t total_entries_available;
    union {
        hourly_history_entry_t *hourly_entries;
        daily_history_entry_t *daily_entries;
        monthly_history_entry_t *monthly_entries;
    };
} env_history_result_t;

/* History storage statistics */
typedef struct {
    uint16_t hourly_entries_used;
    uint16_t daily_entries_used;
    uint16_t monthly_entries_used;
    uint32_t oldest_hourly_timestamp;
    uint32_t oldest_daily_timestamp;
    uint32_t oldest_monthly_timestamp;
    uint32_t total_storage_bytes;
    uint8_t storage_utilization_pct;
} env_history_stats_t;

/* History aggregation status */
typedef struct {
    uint32_t last_hourly_aggregation;
    uint32_t last_daily_aggregation;
    uint32_t last_monthly_aggregation;
    bool hourly_aggregation_pending;
    bool daily_aggregation_pending;
    bool monthly_aggregation_pending;
    uint16_t aggregation_errors;
} env_history_aggregation_status_t;

/**
 * @brief Initialize environmental history storage system
 * @return 0 on success, negative error code on failure
 */
int env_history_init(void);

/**
 * @brief Deinitialize environmental history storage system
 * @return 0 on success, negative error code on failure
 */
int env_history_deinit(void);

/**
 * @brief Add new hourly environmental data entry
 * @param entry Pointer to hourly history entry
 * @return 0 on success, negative error code on failure
 */
int env_history_add_hourly_entry(const hourly_history_entry_t *entry);

/**
 * @brief Get environmental history storage instance
 * @return Pointer to environmental history storage (read-only)
 */
const environmental_history_t* env_history_get_storage(void);

/**
 * @brief Perform hourly data aggregation
 * @param current_timestamp Current system timestamp
 * @return 0 on success, negative error code on failure
 */
int env_history_aggregate_hourly(uint32_t current_timestamp);

/**
 * @brief Perform daily data aggregation
 * @param current_timestamp Current system timestamp
 * @return 0 on success, negative error code on failure
 */
int env_history_aggregate_daily(uint32_t current_timestamp);

/**
 * @brief Perform monthly data aggregation
 * @param current_timestamp Current system timestamp
 * @return 0 on success, negative error code on failure
 */
int env_history_aggregate_monthly(uint32_t current_timestamp);

/**
 * @brief Perform automatic aggregation based on time intervals
 * @param current_timestamp Current system timestamp
 * @return 0 on success, negative error code on failure
 */
int env_history_auto_aggregate(uint32_t current_timestamp);

/**
 * @brief Query environmental history data
 * @param query Pointer to query parameters
 * @param result Pointer to result structure (caller must free data)
 * @return 0 on success, negative error code on failure
 */
int env_history_query(const env_history_query_t *query, env_history_result_t *result);

/**
 * @brief Get hourly history entries within time range
 * @param start_timestamp Start time (inclusive)
 * @param end_timestamp End time (inclusive)
 * @param entries Pointer to array to fill (caller allocated)
 * @param max_entries Maximum entries to return
 * @param actual_count Pointer to store actual number of entries returned
 * @return 0 on success, negative error code on failure
 */
int env_history_get_hourly_range(uint32_t start_timestamp, 
                                uint32_t end_timestamp,
                                hourly_history_entry_t *entries,
                                uint16_t max_entries,
                                uint16_t *actual_count);

/**
 * @brief Get daily history entries within time range
 * @param start_timestamp Start time (inclusive)
 * @param end_timestamp End time (inclusive)
 * @param entries Pointer to array to fill (caller allocated)
 * @param max_entries Maximum entries to return
 * @param actual_count Pointer to store actual number of entries returned
 * @return 0 on success, negative error code on failure
 */
int env_history_get_daily_range(uint32_t start_timestamp,
                               uint32_t end_timestamp,
                               daily_history_entry_t *entries,
                               uint16_t max_entries,
                               uint16_t *actual_count);

/**
 * @brief Get monthly history entries within time range
 * @param start_timestamp Start time (inclusive)
 * @param end_timestamp End time (inclusive)
 * @param entries Pointer to array to fill (caller allocated)
 * @param max_entries Maximum entries to return
 * @param actual_count Pointer to store actual number of entries returned
 * @return 0 on success, negative error code on failure
 */
int env_history_get_monthly_range(uint32_t start_timestamp,
                                 uint32_t end_timestamp,
                                 monthly_history_entry_t *entries,
                                 uint16_t max_entries,
                                 uint16_t *actual_count);

/**
 * @brief Get storage statistics
 * @param stats Pointer to statistics structure to fill
 * @return 0 on success, negative error code on failure
 */
int env_history_get_stats(env_history_stats_t *stats);

/**
 * @brief Get aggregation status
 * @param status Pointer to aggregation status structure to fill
 * @return 0 on success, negative error code on failure
 */
int env_history_get_aggregation_status(env_history_aggregation_status_t *status);

/**
 * @brief Cleanup old entries when storage is full
 * @return 0 on success, negative error code on failure
 */
int env_history_cleanup_old_entries(void);

/**
 * @brief Reset all environmental history data
 * @return 0 on success, negative error code on failure
 */
int env_history_reset_all(void);

/**
 * @brief Save environmental history to NVS
 * @return 0 on success, negative error code on failure
 */
int env_history_save_to_nvs(void);

/**
 * @brief Load environmental history from NVS
 * @return 0 on success, negative error code on failure
 */
int env_history_load_from_nvs(void);

/**
 * @brief Calculate storage utilization percentage
 * @return Storage utilization percentage (0-100)
 */
uint8_t env_history_calculate_utilization(void);

/**
 * @brief Check if aggregation is needed
 * @param current_timestamp Current system timestamp
 * @param hourly_needed Pointer to store if hourly aggregation is needed
 * @param daily_needed Pointer to store if daily aggregation is needed
 * @param monthly_needed Pointer to store if monthly aggregation is needed
 * @return 0 on success, negative error code on failure
 */
int env_history_check_aggregation_needed(uint32_t current_timestamp,
                                        bool *hourly_needed,
                                        bool *daily_needed,
                                        bool *monthly_needed);

/**
 * @brief Get the most recent entry of specified type
 * @param data_type Type of data to retrieve
 * @param entry Pointer to store the entry (caller allocated)
 * @return 0 on success, negative error code on failure
 */
int env_history_get_latest_entry(env_history_data_type_t data_type, void *entry);

/**
 * @brief Get the oldest entry of specified type
 * @param data_type Type of data to retrieve
 * @param entry Pointer to store the entry (caller allocated)
 * @return 0 on success, negative error code on failure
 */
int env_history_get_oldest_entry(env_history_data_type_t data_type, void *entry);

/**
 * @brief Validate history data integrity
 * @param repair_if_needed Whether to attempt repair of corrupted data
 * @return 0 if data is valid, negative error code if corruption detected
 */
int env_history_validate_integrity(bool repair_if_needed);

/**
 * @brief Get ring buffer head position for specified data type
 * @param data_type Type of data
 * @return Head position, or -1 on error
 */
int env_history_get_head_position(env_history_data_type_t data_type);

/**
 * @brief Get ring buffer count for specified data type
 * @param data_type Type of data
 * @return Entry count, or -1 on error
 */
int env_history_get_entry_count(env_history_data_type_t data_type);

/**
 * @brief Initialize environmental history system
 * @return WATERING_ERROR_NONE on success, error code on failure
 */
watering_error_t environmental_history_init(void);

#endif // ENVIRONMENTAL_HISTORY_H