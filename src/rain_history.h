#ifndef RAIN_HISTORY_H
#define RAIN_HISTORY_H

#include <stdint.h>
#include <stdbool.h>
#include "watering.h"

/**
 * @file rain_history.h
 * @brief Rain history management system
 * 
 * This module provides multi-level rain data storage with:
 * - Hourly data for 30 days (720 entries)
 * - Daily summaries for 5 years (1825 entries)
 * - Automatic data aggregation and rotation
 * - NVS persistence with compression
 */

/* Storage configuration */
#define RAIN_HOURLY_ENTRIES     720     /* 30 days × 24 hours */
#define RAIN_DAILY_ENTRIES      1825    /* 5 years × 365 days */

/* Data quality indicators */
#define RAIN_QUALITY_EXCELLENT  100
#define RAIN_QUALITY_GOOD       80
#define RAIN_QUALITY_FAIR       60
#define RAIN_QUALITY_POOR       40
#define RAIN_QUALITY_INVALID    0

/**
 * @brief Hourly rain data structure (8 bytes)
 */
typedef struct __attribute__((packed)) {
    uint32_t hour_epoch;        /**< Hour timestamp (Unix epoch) */
    uint16_t rainfall_mm_x100;  /**< Rainfall in mm × 100 (0.01mm precision) */
    uint8_t pulse_count;        /**< Raw pulse count for validation */
    uint8_t data_quality;       /**< Data quality indicator (0-100%) */
} rain_hourly_data_t;

/**
 * @brief Daily rain summary structure (12 bytes)
 */
typedef struct __attribute__((packed)) {
    uint32_t day_epoch;         /**< Day timestamp (00:00 UTC) */
    uint32_t total_rainfall_mm_x100; /**< Total daily rainfall × 100 */
    uint16_t max_hourly_mm_x100; /**< Peak hourly rainfall × 100 */
    uint8_t active_hours;       /**< Hours with rainfall */
    uint8_t data_completeness;  /**< Percentage of valid hourly data */
} rain_daily_data_t;

/**
 * @brief Rain history statistics
 */
typedef struct {
    uint16_t hourly_entries;    /**< Number of hourly entries stored */
    uint16_t daily_entries;     /**< Number of daily entries stored */
    uint32_t oldest_hourly;     /**< Oldest hourly timestamp */
    uint32_t newest_hourly;     /**< Newest hourly timestamp */
    uint32_t oldest_daily;      /**< Oldest daily timestamp */
    uint32_t newest_daily;      /**< Newest daily timestamp */
    uint32_t total_storage_bytes; /**< Total storage used */
} rain_history_stats_t;

/**
 * @brief Initialize the rain history system
 * 
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t rain_history_init(void);

/**
 * @brief Deinitialize the rain history system
 * 
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t rain_history_deinit(void);

/**
 * @brief Record hourly rainfall data
 * 
 * @param rainfall_mm Rainfall amount in millimeters
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t rain_history_record_hourly(float rainfall_mm);

/**
 * @brief Record hourly rainfall with full data
 * 
 * @param hour_epoch Hour timestamp
 * @param rainfall_mm Rainfall amount in millimeters
 * @param pulse_count Raw pulse count
 * @param data_quality Data quality (0-100%)
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t rain_history_record_hourly_full(uint32_t hour_epoch,
                                                 float rainfall_mm,
                                                 uint8_t pulse_count,
                                                 uint8_t data_quality);

/**
 * @brief Aggregate daily rainfall data
 * 
 * Called automatically to create daily summaries from hourly data.
 * 
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t rain_history_aggregate_daily(void);

/**
 * @brief Get hourly rainfall data for a time range
 * 
 * @param start_hour Start hour timestamp
 * @param end_hour End hour timestamp
 * @param data Buffer to store retrieved data
 * @param max_entries Maximum entries to retrieve
 * @param count Pointer to store actual number of entries retrieved
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t rain_history_get_hourly(uint32_t start_hour, 
                                         uint32_t end_hour,
                                         rain_hourly_data_t *data, 
                                         uint16_t max_entries,
                                         uint16_t *count);

/**
 * @brief Get daily rainfall data for a time range
 * 
 * @param start_day Start day timestamp
 * @param end_day End day timestamp
 * @param data Buffer to store retrieved data
 * @param max_entries Maximum entries to retrieve
 * @param count Pointer to store actual number of entries retrieved
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t rain_history_get_daily(uint32_t start_day, 
                                        uint32_t end_day,
                                        rain_daily_data_t *data, 
                                        uint16_t max_entries,
                                        uint16_t *count);

/**
 * @brief Get recent rainfall total
 * 
 * @param hours_back Number of hours to look back
 * @return Total rainfall in mm for the specified period
 */
float rain_history_get_recent_total(uint32_t hours_back);

/**
 * @brief Check if significant rain was detected recently
 * 
 * @param hours_back Number of hours to look back
 * @param threshold_mm Minimum rainfall threshold in mm
 * @return true if significant rain detected, false otherwise
 */
bool rain_history_significant_rain_detected(uint32_t hours_back, 
                                           float threshold_mm);

/**
 * @brief Get rainfall for the last 24 hours
 * 
 * @return Total rainfall in mm for the last 24 hours
 */
float rain_history_get_last_24h(void);

/**
 * @brief Get rainfall for today
 * 
 * @return Total rainfall in mm for today (since midnight)
 */
float rain_history_get_today(void);

/**
 * @brief Get rainfall for the current hour
 * 
 * @return Rainfall in mm for the current hour
 */
float rain_history_get_current_hour(void);

/**
 * @brief Get history statistics
 * 
 * @param stats Pointer to structure to fill with statistics
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t rain_history_get_stats(rain_history_stats_t *stats);

/**
 * @brief Perform maintenance tasks
 * 
 * Called periodically to:
 * - Rotate old data
 * - Compress historical data
 * - Update aggregations
 * 
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t rain_history_maintenance(void);

/**
 * @brief Clear all rain history data
 * 
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t rain_history_clear_all(void);

/**
 * @brief Clear hourly data older than specified time
 * 
 * @param older_than_epoch Timestamp - data older than this will be cleared
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t rain_history_clear_hourly_older_than(uint32_t older_than_epoch);

/**
 * @brief Clear daily data older than specified time
 * 
 * @param older_than_epoch Timestamp - data older than this will be cleared
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t rain_history_clear_daily_older_than(uint32_t older_than_epoch);

/**
 * @brief Save history data to NVS
 * 
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t rain_history_save_to_nvs(void);

/**
 * @brief Load history data from NVS
 * 
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t rain_history_load_from_nvs(void);

/**
 * @brief Get storage usage information
 * 
 * @param used_bytes Pointer to store used bytes
 * @param total_bytes Pointer to store total allocated bytes
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t rain_history_get_storage_usage(uint32_t *used_bytes, 
                                               uint32_t *total_bytes);

/**
 * @brief Validate history data integrity
 * 
 * @return WATERING_SUCCESS if data is valid, error code if corrupted
 */
watering_error_t rain_history_validate_data(void);

/**
 * @brief Export history data to CSV format
 * 
 * @param start_time Start timestamp for export
 * @param end_time End timestamp for export
 * @param buffer Buffer to store CSV data
 * @param buffer_size Size of the buffer
 * @param bytes_written Pointer to store number of bytes written
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t rain_history_export_csv(uint32_t start_time,
                                        uint32_t end_time,
                                        char *buffer,
                                        uint16_t buffer_size,
                                        uint16_t *bytes_written);

/**
 * @brief Monitor storage usage and trigger cleanup if needed
 * 
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t rain_history_monitor_storage(void);

/**
 * @brief Handle NVS error codes and convert to watering errors
 * 
 * @param nvs_error NVS error code
 * @return Corresponding watering error code
 */
watering_error_t rain_history_handle_nvs_error(int nvs_error);

/**
 * @brief Perform comprehensive periodic maintenance
 * 
 * Includes aggregation, storage monitoring, validation, and NVS save.
 * 
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t rain_history_periodic_maintenance(void);

/**
 * @brief Debug function to print history information
 */
void rain_history_debug_info(void);

/* Make state structure available to other modules */
extern struct rain_history_state_s {
    bool initialized;
    uint16_t hourly_count;
    uint16_t daily_count;
    uint16_t hourly_write_index;
    uint16_t daily_write_index;
    uint32_t last_hourly_save;
    rain_hourly_data_t hourly_data[RAIN_HOURLY_ENTRIES];
    rain_daily_data_t daily_data[RAIN_DAILY_ENTRIES];
    struct k_mutex mutex;

    /* BLE command state */
    bool command_active;
    struct bt_conn *requesting_conn;
    uint8_t current_command;
    uint32_t start_timestamp;
    uint32_t end_timestamp;
    uint16_t max_entries;
    uint8_t data_type;
    uint16_t current_entry;
    uint16_t total_entries;
    uint8_t current_fragment;
    uint8_t total_fragments;
    uint8_t *fragment_buffer;
} rain_history_state;


#endif // RAIN_HISTORY_H