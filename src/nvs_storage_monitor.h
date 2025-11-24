/**
 * @file nvs_storage_monitor.h
 * @brief NVS storage monitoring and cleanup interface
 * 
 * This header provides storage monitoring functionality for:
 * - Usage tracking and capacity monitoring
 * - Automatic cleanup and data rotation
 * - Storage health monitoring
 * - Error tracking and reporting
 */

#ifndef NVS_STORAGE_MONITOR_H
#define NVS_STORAGE_MONITOR_H

#include "watering_enhanced.h"

/* Storage monitoring state structure */
typedef struct {
    uint32_t total_capacity_bytes;      // Total NVS partition capacity
    uint32_t used_bytes;                // Currently used bytes
    uint32_t free_bytes;                // Available free bytes
    uint8_t usage_percentage;           // Usage as percentage (0-100)
    uint32_t last_cleanup_time;         // Timestamp of last cleanup
    uint32_t cleanup_count;             // Number of cleanups performed
    uint32_t write_errors;              // Number of write errors encountered
    uint32_t read_errors;               // Number of read errors encountered
    bool health_check_active;           // Whether health check is running
    bool cleanup_in_progress;           // Whether cleanup is in progress
} storage_monitor_state_t;

/**
 * @brief Initialize NVS storage monitoring system
 * 
 * This function sets up the storage monitoring system and starts
 * periodic health checks. It calculates initial usage statistics
 * and prepares the system for automatic cleanup operations.
 * 
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t nvs_storage_monitor_init(void);

/**
 * @brief Get current storage usage information
 * 
 * This function provides comprehensive storage usage statistics
 * including capacity, usage percentage, and error counts.
 * 
 * @param usage_info Pointer to structure to fill with usage information
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t nvs_storage_get_usage(storage_monitor_state_t *usage_info);

/**
 * @brief Trigger immediate storage cleanup if needed
 * 
 * This function performs immediate cleanup of old data if storage
 * usage exceeds warning thresholds. It uses data rotation algorithms
 * to remove the oldest, least critical data first.
 * 
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t nvs_storage_trigger_cleanup(void);

/**
 * @brief Check storage health and trigger cleanup if needed
 * 
 * This function performs a comprehensive health check including:
 * - Usage percentage monitoring
 * - Error rate tracking
 * - Automatic cleanup triggering
 * - Health status reporting
 * 
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t nvs_storage_health_check(void);

/**
 * @brief Get storage health status
 * 
 * This function evaluates the overall health of the storage system
 * based on usage levels, error rates, and operational status.
 * 
 * @param is_healthy Pointer to store health status (true = healthy)
 * @param health_message Pointer to store health message (optional)
 * @param message_size Size of health message buffer
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t nvs_storage_get_health_status(bool *is_healthy, 
                                                char *health_message, 
                                                size_t message_size);

/**
 * @brief Record storage operation error for monitoring
 * 
 * This function records storage errors for health monitoring
 * and trend analysis. It helps identify storage degradation
 * and trigger appropriate responses.
 * 
 * @param is_write_error True for write error, false for read error
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t nvs_storage_record_error(bool is_write_error);

/**
 * @brief Force storage cleanup to specific usage target
 * 
 * This function performs aggressive cleanup to reach a specific
 * usage target. It should be used carefully as it may remove
 * more data than normal cleanup operations.
 * 
 * @param target_usage_percent Target usage percentage (0-100)
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t nvs_storage_force_cleanup(uint8_t target_usage_percent);

/**
 * @brief Get storage cleanup statistics
 * 
 * This function provides statistics about cleanup operations
 * including frequency, effectiveness, and data removed.
 * 
 * @param cleanup_count Pointer to store number of cleanups performed
 * @param last_cleanup_time Pointer to store timestamp of last cleanup
 * @param bytes_cleaned Pointer to store total bytes cleaned (optional)
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t nvs_storage_get_cleanup_stats(uint32_t *cleanup_count,
                                                uint32_t *last_cleanup_time,
                                                uint32_t *bytes_cleaned);

/**
 * @brief Reset storage error counters
 * 
 * This function resets the error counters for fresh monitoring.
 * Useful after resolving storage issues or for testing.
 * 
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t nvs_storage_reset_error_counters(void);

/**
 * @brief Check if storage cleanup is recommended
 * 
 * This function evaluates current storage conditions and
 * recommends whether cleanup should be performed.
 * 
 * @param cleanup_recommended Pointer to store recommendation flag
 * @param urgency_level Pointer to store urgency level (0-10, optional)
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t nvs_storage_check_cleanup_needed(bool *cleanup_recommended,
                                                   uint8_t *urgency_level);

/**
 * @brief Get detailed storage breakdown by data type
 * 
 * This function provides a breakdown of storage usage by
 * different data types (system config, channels, history, etc.).
 * 
 * @param system_config_bytes Pointer to store system config usage
 * @param channel_config_bytes Pointer to store channel config usage
 * @param history_bytes Pointer to store history data usage
 * @param other_bytes Pointer to store other data usage
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t nvs_storage_get_usage_breakdown(uint32_t *system_config_bytes,
                                                  uint32_t *channel_config_bytes,
                                                  uint32_t *history_bytes,
                                                  uint32_t *other_bytes);

#endif // NVS_STORAGE_MONITOR_H