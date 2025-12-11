#include "environmental_history.h"
#include "watering_log.h"
#include "nvs_config.h"
#include "environmental_data.h"
#include "rain_history.h"
#include "watering_history.h"
#ifdef CONFIG_HISTORY_EXTERNAL_FLASH
#include "history_flash.h"
#endif
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>
#include <string.h>
#include <time.h>

LOG_MODULE_REGISTER(environmental_history, LOG_LEVEL_DBG);

/**
 * @file environmental_history.c
 * @brief Multi-resolution environmental history storage system implementation
 * 
 * This module implements a comprehensive multi-resolution history storage system
 * with automatic aggregation and ring buffer management for environmental data.
 */

/* Global environmental history storage */
static environmental_history_t g_env_history;
static bool g_env_history_initialized = false;

/* NVS keys for persistent storage (must be uint16_t IDs expected by nvs_config_* API) */
#define NVS_KEY_ENV_HISTORY_HOURLY    0x6101
#define NVS_KEY_ENV_HISTORY_DAILY     0x6102
#define NVS_KEY_ENV_HISTORY_MONTHLY   0x6103
#define NVS_KEY_ENV_HISTORY_META      0x6104

/* Internal helper functions */
static int env_history_add_to_ring_buffer(void *buffer, size_t entry_size, 
                                         uint16_t max_entries, uint16_t *head, 
                                         uint16_t *count, const void *new_entry);
static int env_history_get_from_ring_buffer(const void *buffer, size_t entry_size,
                                           uint16_t max_entries, uint16_t head,
                                           uint16_t count, uint16_t index, void *entry);
static uint32_t env_history_timestamp_to_hour(uint32_t timestamp);
static uint32_t env_history_timestamp_to_day(uint32_t timestamp);
static uint32_t env_history_timestamp_to_month(uint32_t timestamp);
static int env_history_find_hourly_entries_for_day(uint32_t day_timestamp,
                                                   hourly_history_entry_t *entries,
                                                   uint16_t *count);
static int env_history_find_daily_entries_for_month(uint32_t month_timestamp,
                                                    daily_history_entry_t *entries,
                                                    uint16_t *count);

int env_history_init(void)
{
    if (g_env_history_initialized) {
        return 0; // Already initialized
    }

    // Initialize the environmental history structure
    memset(&g_env_history, 0, sizeof(environmental_history_t));
    
#ifdef CONFIG_HISTORY_EXTERNAL_FLASH
    // Initialize flash storage for history
    int flash_result = history_flash_init();
    if (flash_result != 0) {
        LOG_ERR("Failed to initialize history flash storage: %d", flash_result);
        return flash_result;
    }
    
    // Get counts from flash storage
    history_flash_stats_t flash_stats;
    if (history_flash_get_stats(&flash_stats) == 0) {
        g_env_history.hourly_count = flash_stats.env_hourly.entry_count;
        g_env_history.daily_count = flash_stats.env_daily.entry_count;
        g_env_history.monthly_count = flash_stats.env_monthly.entry_count;
    }
    LOG_INF("Environmental history using external flash storage");
#else
    // Initialize ring buffer pointers
    g_env_history.hourly_head = 0;
    g_env_history.hourly_count = 0;
    g_env_history.daily_head = 0;
    g_env_history.daily_count = 0;
    g_env_history.monthly_head = 0;
    g_env_history.monthly_count = 0;
    
    // Try to load existing data from NVS
    int result = env_history_load_from_nvs();
    if (result != 0) {
        LOG_WRN("Failed to load environmental history from NVS: %d", result);
        // Continue with empty history - this is not a fatal error
    }
#endif
    
    // Initialize timestamps
    g_env_history.last_hourly_update = 0;
    g_env_history.last_daily_update = 0;
    g_env_history.last_monthly_update = 0;

    g_env_history_initialized = true;
    LOG_INF("Environmental history storage initialized");
    
    return 0;
}

int env_history_deinit(void)
{
    if (!g_env_history_initialized) {
        return 0;
    }

    // Save current data to NVS before deinitializing
    int result = env_history_save_to_nvs();
    if (result != 0) {
        LOG_ERR("Failed to save environmental history to NVS: %d", result);
    }

    g_env_history_initialized = false;
    LOG_INF("Environmental history storage deinitialized");
    
    return 0;
}

int env_history_add_hourly_entry(const hourly_history_entry_t *entry)
{
    if (!g_env_history_initialized) {
        return -WATERING_ERROR_NOT_INITIALIZED;
    }

    if (!entry) {
        return -WATERING_ERROR_INVALID_PARAM;
    }

#ifdef CONFIG_HISTORY_EXTERNAL_FLASH
    // Convert to flash format and write to external flash
    history_env_hourly_t flash_entry = {
        .timestamp = entry->timestamp,
        .temperature_x100 = (int16_t)(entry->environmental.temperature * 100),
        .humidity_x100 = (uint16_t)(entry->environmental.humidity * 100),
        .pressure_x100 = (uint32_t)(entry->environmental.pressure * 100),
        .rainfall_mm_x100 = (uint16_t)(entry->rainfall_mm * 100),
        .watering_events = entry->watering_events,
        .total_volume_ml = entry->total_volume_ml,
        .active_channels = entry->active_channels,
    };
    
    int result = history_flash_add_env_hourly(&flash_entry);
    if (result == 0) {
        g_env_history.hourly_count++;
        g_env_history.last_hourly_update = entry->timestamp;
        LOG_DBG("Added hourly entry to flash at timestamp %u", entry->timestamp);
    }
    return result;
#else
    // Add entry to hourly ring buffer
    int result = env_history_add_to_ring_buffer(
        g_env_history.hourly,
        sizeof(hourly_history_entry_t),
        ENV_HISTORY_HOURLY_ENTRIES,
        &g_env_history.hourly_head,
        &g_env_history.hourly_count,
        entry
    );

    if (result == 0) {
        g_env_history.last_hourly_update = entry->timestamp;
        LOG_DBG("Added hourly environmental entry at timestamp %u", entry->timestamp);
    }

    return result;
#endif
}

const environmental_history_t* env_history_get_storage(void)
{
    if (!g_env_history_initialized) {
        return NULL;
    }
    return &g_env_history;
}

int env_history_aggregate_hourly(uint32_t current_timestamp)
{
    if (!g_env_history_initialized) {
        return -WATERING_ERROR_NOT_INITIALIZED;
    }

    uint32_t current_hour_index = env_history_timestamp_to_hour(current_timestamp);

    if (current_hour_index == 0) {
        /* Not enough elapsed time to aggregate a full hour */
        return 0;
    }

    uint32_t target_hour_index = current_hour_index - 1;

    hourly_history_entry_t last_entry = {0};
    bool have_last_entry = false;

#ifdef CONFIG_HISTORY_EXTERNAL_FLASH
    /* For flash storage, read last entry from flash */
    if (g_env_history.hourly_count > 0) {
        history_env_hourly_t flash_entry;
        uint16_t read_count = 0;
        if (history_flash_read_env_hourly(g_env_history.hourly_count - 1, 
                                          &flash_entry, 1, &read_count) == 0 && read_count > 0) {
            last_entry.timestamp = flash_entry.timestamp;
            last_entry.environmental.temperature = flash_entry.temperature_x100 / 100.0f;
            last_entry.environmental.humidity = flash_entry.humidity_x100 / 100.0f;
            last_entry.environmental.pressure = flash_entry.pressure_x100 / 100.0f;
            last_entry.environmental.valid = true;
            have_last_entry = true;
        }
    }
#else
    if (g_env_history.hourly_count > 0) {
        uint16_t last_idx = g_env_history.hourly_count - 1U;
        if (env_history_get_from_ring_buffer(
                g_env_history.hourly,
                sizeof(hourly_history_entry_t),
                ENV_HISTORY_HOURLY_ENTRIES,
                g_env_history.hourly_head,
                g_env_history.hourly_count,
                last_idx,
                &last_entry) == 0) {
            have_last_entry = true;
        }
    }
#endif
    
    if (!have_last_entry && g_env_history.last_hourly_update != 0U) {
        last_entry.timestamp = g_env_history.last_hourly_update;
        have_last_entry = true;
    }

    uint32_t start_hour_index;
    if (have_last_entry) {
        uint32_t last_hour_index = env_history_timestamp_to_hour(last_entry.timestamp);
        if (last_hour_index >= target_hour_index) {
            return 0;
        }
        start_hour_index = last_hour_index + 1U;
    } else {
        start_hour_index = target_hour_index;
    }

    if (start_hour_index > target_hour_index) {
        return 0;
    }

    bme280_environmental_data_t env_snapshot;
    bool have_env_snapshot = (environmental_data_get_current(&env_snapshot) == 0);

    bool added_entries = false;

    for (uint32_t hour_idx = start_hour_index; hour_idx <= target_hour_index; ++hour_idx) {
        uint32_t hour_start = hour_idx * ENV_HISTORY_HOURLY_INTERVAL_SEC;
        uint32_t hour_end = hour_start + ENV_HISTORY_HOURLY_INTERVAL_SEC;

        hourly_history_entry_t entry;
        memset(&entry, 0, sizeof(entry));
        entry.timestamp = hour_start;

        bme280_reading_t reading = {0};
        bool reading_valid = false;

        if (have_env_snapshot && env_snapshot.current.valid) {
            reading = env_snapshot.current;
            reading_valid = true;
        } else if (have_last_entry && last_entry.environmental.valid) {
            reading = last_entry.environmental;
            reading_valid = true;
        }

        reading.timestamp = hour_start;
        reading.valid = reading_valid;
        entry.environmental = reading;

        rain_hourly_data_t rain_sample;
        uint16_t rain_count = 1;
        if (rain_history_get_hourly(hour_start, hour_start, &rain_sample, 1, &rain_count) == WATERING_SUCCESS &&
            rain_count > 0) {
            entry.rainfall_mm = rain_sample.rainfall_mm_x100 / 100.0f;
        }

        uint8_t events_total = 0;
        uint32_t volume_total = 0;
        uint16_t active_mask = 0;

        for (uint8_t channel = 0; channel < WATERING_CHANNELS_COUNT; ++channel) {
            history_event_t events[DETAILED_EVENTS_PER_CHANNEL];
            uint16_t event_count = ARRAY_SIZE(events);
            watering_error_t history_err = watering_history_query_range(
                channel,
                hour_start,
                hour_end,
                events,
                &event_count);

            if (history_err != WATERING_SUCCESS) {
                continue;
            }

            for (uint16_t i = 0; i < event_count; ++i) {
                const history_event_t *evt = &events[i];
                bool has_volume = (evt->flags.mode == 0) && (evt->actual_ml > 0);
                bool duration_event = (evt->flags.mode != 0) && (evt->actual_ml > 0);

                if (!has_volume && !duration_event) {
                    continue;
                }

                if (events_total < UINT8_MAX) {
                    events_total++;
                }

                if (has_volume) {
                    volume_total += evt->actual_ml;
                }

                active_mask |= BIT(channel);
            }
        }

        entry.watering_events = events_total;
        entry.total_volume_ml = volume_total;
        entry.active_channels = active_mask;

        if (env_history_add_hourly_entry(&entry) == 0) {
            added_entries = true;
            last_entry = entry;
            have_last_entry = true;
        }
    }

    if (added_entries) {
#ifndef CONFIG_HISTORY_EXTERNAL_FLASH
        int persist_result = env_history_save_to_nvs();
        if (persist_result != 0) {
            LOG_WRN("Failed to persist environmental history to NVS: %d", persist_result);
        } else {
            LOG_DBG("Hourly aggregation processed up to hour index %u (timestamp %u)",
                    target_hour_index, target_hour_index * ENV_HISTORY_HOURLY_INTERVAL_SEC);
        }
#else
        /* Flash storage auto-persists, just log */
        LOG_DBG("Hourly aggregation to flash processed up to hour index %u", target_hour_index);
#endif
    }
    return 0;
}

int env_history_aggregate_daily(uint32_t current_timestamp)
{
    if (!g_env_history_initialized) {
        return -WATERING_ERROR_NOT_INITIALIZED;
    }

    // Check if daily aggregation is needed
    uint32_t current_day = env_history_timestamp_to_day(current_timestamp);
    uint32_t last_day = env_history_timestamp_to_day(g_env_history.last_daily_update);
    
    if (current_day <= last_day) {
        return 0; // No aggregation needed
    }

    // Find all hourly entries for the previous day
    hourly_history_entry_t hourly_entries[24];
    uint16_t hourly_count = 0;
    
    int result = env_history_find_hourly_entries_for_day(last_day, hourly_entries, &hourly_count);
    if (result != 0) {
        LOG_ERR("Failed to find hourly entries for daily aggregation: %d", result);
        return result;
    }

    if (hourly_count == 0) {
        return 0; // No data to aggregate
    }

    // Create daily aggregated entry
    daily_history_entry_t daily_entry;
    memset(&daily_entry, 0, sizeof(daily_history_entry_t));
    
    daily_entry.date = last_day;
    daily_entry.temperature.min = 999.0f;
    daily_entry.temperature.max = -999.0f;
    daily_entry.humidity.min = 999.0f;
    daily_entry.humidity.max = -999.0f;
    daily_entry.pressure.min = 9999.0f;
    daily_entry.pressure.max = 0.0f;
    
    float temp_sum = 0.0f, humidity_sum = 0.0f, pressure_sum = 0.0f;
    
    // Aggregate hourly data into daily statistics
    for (uint16_t i = 0; i < hourly_count; i++) {
        const hourly_history_entry_t *hourly = &hourly_entries[i];
        
        // Temperature aggregation
        if (hourly->environmental.temperature < daily_entry.temperature.min) {
            daily_entry.temperature.min = hourly->environmental.temperature;
        }
        if (hourly->environmental.temperature > daily_entry.temperature.max) {
            daily_entry.temperature.max = hourly->environmental.temperature;
        }
        temp_sum += hourly->environmental.temperature;
        
        // Humidity aggregation
        if (hourly->environmental.humidity < daily_entry.humidity.min) {
            daily_entry.humidity.min = hourly->environmental.humidity;
        }
        if (hourly->environmental.humidity > daily_entry.humidity.max) {
            daily_entry.humidity.max = hourly->environmental.humidity;
        }
        humidity_sum += hourly->environmental.humidity;
        
        // Pressure aggregation
        if (hourly->environmental.pressure < daily_entry.pressure.min) {
            daily_entry.pressure.min = hourly->environmental.pressure;
        }
        if (hourly->environmental.pressure > daily_entry.pressure.max) {
            daily_entry.pressure.max = hourly->environmental.pressure;
        }
        pressure_sum += hourly->environmental.pressure;
        
        // Sum other values
        daily_entry.total_rainfall_mm += hourly->rainfall_mm;
        daily_entry.watering_events += hourly->watering_events;
        daily_entry.total_volume_ml += hourly->total_volume_ml;
        daily_entry.active_channels_bitmap |= (uint8_t)(hourly->active_channels & 0xFF);
    }

    daily_entry.sample_count = hourly_count;
    
    // Calculate averages
    daily_entry.temperature.avg = temp_sum / hourly_count;
    daily_entry.humidity.avg = humidity_sum / hourly_count;
    daily_entry.pressure.avg = pressure_sum / hourly_count;

#ifdef CONFIG_HISTORY_EXTERNAL_FLASH
    // Convert to flash format and write
    history_env_daily_t flash_entry;
    flash_entry.date = daily_entry.date;
    flash_entry.temp_min_x100 = (int16_t)(daily_entry.temperature.min * 100);
    flash_entry.temp_max_x100 = (int16_t)(daily_entry.temperature.max * 100);
    flash_entry.temp_avg_x100 = (int16_t)(daily_entry.temperature.avg * 100);
    flash_entry.humid_min_x100 = (uint16_t)(daily_entry.humidity.min * 100);
    flash_entry.humid_max_x100 = (uint16_t)(daily_entry.humidity.max * 100);
    flash_entry.humid_avg_x100 = (uint16_t)(daily_entry.humidity.avg * 100);
    flash_entry.press_min_x10 = (uint16_t)(daily_entry.pressure.min * 10);
    flash_entry.press_max_x10 = (uint16_t)(daily_entry.pressure.max * 10);
    flash_entry.press_avg_x10 = (uint16_t)(daily_entry.pressure.avg * 10);
    flash_entry.total_rainfall_mm_x100 = (uint32_t)(daily_entry.total_rainfall_mm * 100);
    flash_entry.watering_events = daily_entry.watering_events;
    flash_entry.total_volume_ml = daily_entry.total_volume_ml;
    flash_entry.sample_count = daily_entry.sample_count;
    flash_entry.active_channels = daily_entry.active_channels_bitmap;
    
    result = history_flash_add_env_daily(&flash_entry);
#else
    // Add daily entry to ring buffer
    result = env_history_add_to_ring_buffer(
        g_env_history.daily,
        sizeof(daily_history_entry_t),
        ENV_HISTORY_DAILY_ENTRIES,
        &g_env_history.daily_head,
        &g_env_history.daily_count,
        &daily_entry
    );
#endif

    if (result == 0) {
        g_env_history.last_daily_update = current_timestamp;
        LOG_INF("Performed daily aggregation for day %u, %d hourly entries processed", 
                last_day, hourly_count);
    }

    return result;
}

int env_history_aggregate_monthly(uint32_t current_timestamp)
{
    if (!g_env_history_initialized) {
        return -WATERING_ERROR_NOT_INITIALIZED;
    }

    // Check if monthly aggregation is needed
    uint32_t current_month = env_history_timestamp_to_month(current_timestamp);
    uint32_t last_month = env_history_timestamp_to_month(g_env_history.last_monthly_update);
    
    if (current_month <= last_month) {
        return 0; // No aggregation needed
    }

    // Find all daily entries for the previous month
    daily_history_entry_t daily_entries[31];
    uint16_t daily_count = 0;
    
    int result = env_history_find_daily_entries_for_month(last_month, daily_entries, &daily_count);
    if (result != 0) {
        LOG_ERR("Failed to find daily entries for monthly aggregation: %d", result);
        return result;
    }

    if (daily_count == 0) {
        return 0; // No data to aggregate
    }

    // Create monthly aggregated entry
    monthly_history_entry_t monthly_entry;
    memset(&monthly_entry, 0, sizeof(monthly_history_entry_t));
    
    monthly_entry.year_month = (uint16_t)last_month;
    monthly_entry.temperature.min = 999.0f;
    monthly_entry.temperature.max = -999.0f;
    monthly_entry.humidity.min = 999.0f;
    monthly_entry.humidity.max = -999.0f;
    monthly_entry.pressure.min = 9999.0f;
    monthly_entry.pressure.max = 0.0f;
    
    float temp_sum = 0.0f, humidity_sum = 0.0f, pressure_sum = 0.0f;
    
    // Aggregate daily data into monthly statistics
    for (uint16_t i = 0; i < daily_count; i++) {
        const daily_history_entry_t *daily = &daily_entries[i];
        
        // Temperature aggregation
        if (daily->temperature.min < monthly_entry.temperature.min) {
            monthly_entry.temperature.min = daily->temperature.min;
        }
        if (daily->temperature.max > monthly_entry.temperature.max) {
            monthly_entry.temperature.max = daily->temperature.max;
        }
        temp_sum += daily->temperature.avg;
        
        // Humidity aggregation
        if (daily->humidity.min < monthly_entry.humidity.min) {
            monthly_entry.humidity.min = daily->humidity.min;
        }
        if (daily->humidity.max > monthly_entry.humidity.max) {
            monthly_entry.humidity.max = daily->humidity.max;
        }
        humidity_sum += daily->humidity.avg;
        
        // Pressure aggregation
        if (daily->pressure.min < monthly_entry.pressure.min) {
            monthly_entry.pressure.min = daily->pressure.min;
        }
        if (daily->pressure.max > monthly_entry.pressure.max) {
            monthly_entry.pressure.max = daily->pressure.max;
        }
        pressure_sum += daily->pressure.avg;
        
        // Sum other values
        monthly_entry.total_rainfall_mm += daily->total_rainfall_mm;
        monthly_entry.watering_events += daily->watering_events;
        monthly_entry.total_volume_ml += daily->total_volume_ml;
        monthly_entry.days_active++;
    }
    
    // Calculate averages
    monthly_entry.temperature.avg = temp_sum / daily_count;
    monthly_entry.humidity.avg = humidity_sum / daily_count;
    monthly_entry.pressure.avg = pressure_sum / daily_count;

#ifdef CONFIG_HISTORY_EXTERNAL_FLASH
    // Convert to flash format and write
    history_env_monthly_t flash_entry;
    flash_entry.year_month = monthly_entry.year_month;
    flash_entry.temp_min_x100 = (int16_t)(monthly_entry.temperature.min * 100);
    flash_entry.temp_max_x100 = (int16_t)(monthly_entry.temperature.max * 100);
    flash_entry.temp_avg_x100 = (int16_t)(monthly_entry.temperature.avg * 100);
    flash_entry.humid_min_x100 = (uint16_t)(monthly_entry.humidity.min * 100);
    flash_entry.humid_max_x100 = (uint16_t)(monthly_entry.humidity.max * 100);
    flash_entry.humid_avg_x100 = (uint16_t)(monthly_entry.humidity.avg * 100);
    flash_entry.press_min_x10 = (uint16_t)(monthly_entry.pressure.min * 10);
    flash_entry.press_max_x10 = (uint16_t)(monthly_entry.pressure.max * 10);
    flash_entry.press_avg_x10 = (uint16_t)(monthly_entry.pressure.avg * 10);
    flash_entry.total_rainfall_mm_x100 = (uint32_t)(monthly_entry.total_rainfall_mm * 100);
    flash_entry.watering_events = monthly_entry.watering_events;
    flash_entry.total_volume_ml = monthly_entry.total_volume_ml;
    flash_entry.days_active = monthly_entry.days_active;
    
    result = history_flash_add_env_monthly(&flash_entry);
#else
    // Add monthly entry to ring buffer
    uint16_t monthly_head_16 = g_env_history.monthly_head;
    uint16_t monthly_count_16 = g_env_history.monthly_count;
    
    result = env_history_add_to_ring_buffer(
        g_env_history.monthly,
        sizeof(monthly_history_entry_t),
        ENV_HISTORY_MONTHLY_ENTRIES,
        &monthly_head_16,
        &monthly_count_16,
        &monthly_entry
    );
    
    // Update the actual values
    g_env_history.monthly_head = (uint8_t)monthly_head_16;
    g_env_history.monthly_count = (uint8_t)monthly_count_16;
#endif

    if (result == 0) {
        g_env_history.last_monthly_update = current_timestamp;
        LOG_INF("Performed monthly aggregation for month %u, %d daily entries processed", 
                last_month, daily_count);
    }

    return result;
}

static __attribute__((unused)) watering_error_t _history_auto_aggregate(uint32_t current_timestamp)
{
    if (!g_env_history_initialized) {
        return -WATERING_ERROR_NOT_INITIALIZED;
    }

    int result = 0;
    
    // Check and perform hourly aggregation
    result = env_history_aggregate_hourly(current_timestamp);
    if (result != 0) {
        LOG_ERR("Hourly aggregation failed: %d", result);
        return result;
    }
    
    // Check and perform daily aggregation
    result = env_history_aggregate_daily(current_timestamp);
    if (result != 0) {
        LOG_ERR("Daily aggregation failed: %d", result);
        return result;
    }
    
    // Check and perform monthly aggregation
    result = env_history_aggregate_monthly(current_timestamp);
    if (result != 0) {
        LOG_ERR("Monthly aggregation failed: %d", result);
        return result;
    }
    
    return 0;
}

int env_history_query(const env_history_query_t *query, env_history_result_t *result)
{
    if (!g_env_history_initialized) {
        return -WATERING_ERROR_NOT_INITIALIZED;
    }
    
    if (!query || !result) {
        return -WATERING_ERROR_INVALID_PARAM;
    }
    
    memset(result, 0, sizeof(env_history_result_t));
    result->data_type = query->data_type;
    
    switch (query->data_type) {
        case ENV_HISTORY_TYPE_HOURLY:
            return env_history_get_hourly_range(query->start_timestamp, 
                                              query->end_timestamp,
                                              (hourly_history_entry_t*)result->hourly_entries,
                                              query->max_entries,
                                              &result->entry_count);
        
        case ENV_HISTORY_TYPE_DAILY:
            return env_history_get_daily_range(query->start_timestamp,
                                             query->end_timestamp,
                                             (daily_history_entry_t*)result->daily_entries,
                                             query->max_entries,
                                             &result->entry_count);
        
        case ENV_HISTORY_TYPE_MONTHLY:
            return env_history_get_monthly_range(query->start_timestamp,
                                                query->end_timestamp,
                                                (monthly_history_entry_t*)result->monthly_entries,
                                                query->max_entries,
                                                &result->entry_count);
        
        default:
            return -WATERING_ERROR_INVALID_PARAM;
    }
}

int env_history_get_hourly_range(uint32_t start_timestamp, 
                                uint32_t end_timestamp,
                                hourly_history_entry_t *entries,
                                uint16_t max_entries,
                                uint16_t *actual_count)
{
    if (!g_env_history_initialized) {
        return -WATERING_ERROR_NOT_INITIALIZED;
    }
    
    if (!entries || !actual_count) {
        return -WATERING_ERROR_INVALID_PARAM;
    }
    
    *actual_count = 0;

#ifdef CONFIG_HISTORY_EXTERNAL_FLASH
    /* Read from flash and filter by timestamp */
    history_env_hourly_t flash_entries[24]; /* Read in chunks */
    uint16_t total_count = g_env_history.hourly_count;
    uint16_t offset = 0;
    
    while (offset < total_count && *actual_count < max_entries) {
        uint16_t chunk_size = MIN(24, total_count - offset);
        uint16_t read_count = 0;
        
        int rc = history_flash_read_env_hourly(offset, flash_entries, chunk_size, &read_count);
        if (rc != 0 || read_count == 0) {
            break;
        }
        
        for (uint16_t i = 0; i < read_count && *actual_count < max_entries; i++) {
            if (flash_entries[i].timestamp >= start_timestamp && 
                flash_entries[i].timestamp <= end_timestamp) {
                /* Convert flash format to runtime format */
                entries[*actual_count].timestamp = flash_entries[i].timestamp;
                entries[*actual_count].environmental.temperature = flash_entries[i].temperature_x100 / 100.0f;
                entries[*actual_count].environmental.humidity = flash_entries[i].humidity_x100 / 100.0f;
                entries[*actual_count].environmental.pressure = flash_entries[i].pressure_x100 / 100.0f;
                entries[*actual_count].environmental.valid = true;
                entries[*actual_count].rainfall_mm = flash_entries[i].rainfall_mm_x100 / 100.0f;
                entries[*actual_count].watering_events = flash_entries[i].watering_events;
                entries[*actual_count].total_volume_ml = flash_entries[i].total_volume_ml;
                entries[*actual_count].active_channels = flash_entries[i].active_channels;
                (*actual_count)++;
            }
        }
        offset += read_count;
    }
#else
    // Search through hourly ring buffer
    for (uint16_t i = 0; i < g_env_history.hourly_count && *actual_count < max_entries; i++) {
        hourly_history_entry_t entry;
        int result = env_history_get_from_ring_buffer(
            g_env_history.hourly,
            sizeof(hourly_history_entry_t),
            ENV_HISTORY_HOURLY_ENTRIES,
            g_env_history.hourly_head,
            g_env_history.hourly_count,
            i,
            &entry
        );
        
        if (result == 0 && entry.timestamp >= start_timestamp && entry.timestamp <= end_timestamp) {
            entries[*actual_count] = entry;
            (*actual_count)++;
        }
    }
#endif
    
    return 0;
}

int env_history_get_daily_range(uint32_t start_timestamp,
                               uint32_t end_timestamp,
                               daily_history_entry_t *entries,
                               uint16_t max_entries,
                               uint16_t *actual_count)
{
    if (!g_env_history_initialized) {
        return -WATERING_ERROR_NOT_INITIALIZED;
    }
    
    if (!entries || !actual_count) {
        return -WATERING_ERROR_INVALID_PARAM;
    }
    
    *actual_count = 0;

#ifdef CONFIG_HISTORY_EXTERNAL_FLASH
    /* Read from flash and filter by timestamp */
    history_env_daily_t flash_entries[16]; /* Read in chunks */
    uint16_t total_count = g_env_history.daily_count;
    uint16_t offset = 0;
    
    while (offset < total_count && *actual_count < max_entries) {
        uint16_t chunk_size = MIN(16, total_count - offset);
        uint16_t read_count = 0;
        
        int rc = history_flash_read_env_daily(offset, flash_entries, chunk_size, &read_count);
        if (rc != 0 || read_count == 0) {
            break;
        }
        
        for (uint16_t i = 0; i < read_count && *actual_count < max_entries; i++) {
            if (flash_entries[i].date >= start_timestamp && 
                flash_entries[i].date <= end_timestamp) {
                /* Convert flash format to runtime format */
                entries[*actual_count].date = flash_entries[i].date;
                entries[*actual_count].temperature.min = flash_entries[i].temp_min_x100 / 100.0f;
                entries[*actual_count].temperature.max = flash_entries[i].temp_max_x100 / 100.0f;
                entries[*actual_count].temperature.avg = flash_entries[i].temp_avg_x100 / 100.0f;
                entries[*actual_count].humidity.min = flash_entries[i].humid_min_x100 / 100.0f;
                entries[*actual_count].humidity.max = flash_entries[i].humid_max_x100 / 100.0f;
                entries[*actual_count].humidity.avg = flash_entries[i].humid_avg_x100 / 100.0f;
                entries[*actual_count].pressure.min = flash_entries[i].press_min_x10 / 10.0f;
                entries[*actual_count].pressure.max = flash_entries[i].press_max_x10 / 10.0f;
                entries[*actual_count].pressure.avg = flash_entries[i].press_avg_x10 / 10.0f;
                entries[*actual_count].total_rainfall_mm = flash_entries[i].total_rainfall_mm_x100 / 100.0f;
                entries[*actual_count].watering_events = flash_entries[i].watering_events;
                entries[*actual_count].total_volume_ml = flash_entries[i].total_volume_ml;
                entries[*actual_count].sample_count = flash_entries[i].sample_count;
                entries[*actual_count].active_channels_bitmap = flash_entries[i].active_channels;
                (*actual_count)++;
            }
        }
        offset += read_count;
    }
#else
    // Search through daily ring buffer
    for (uint16_t i = 0; i < g_env_history.daily_count && *actual_count < max_entries; i++) {
        daily_history_entry_t entry;
        int result = env_history_get_from_ring_buffer(
            g_env_history.daily,
            sizeof(daily_history_entry_t),
            ENV_HISTORY_DAILY_ENTRIES,
            g_env_history.daily_head,
            g_env_history.daily_count,
            i,
            &entry
        );
        
        if (result == 0 && entry.date >= start_timestamp && entry.date <= end_timestamp) {
            entries[*actual_count] = entry;
            (*actual_count)++;
        }
    }
#endif
    
    return 0;
}

int env_history_get_monthly_range(uint32_t start_timestamp,
                                 uint32_t end_timestamp,
                                 monthly_history_entry_t *entries,
                                 uint16_t max_entries,
                                 uint16_t *actual_count)
{
    if (!g_env_history_initialized) {
        return -WATERING_ERROR_NOT_INITIALIZED;
    }
    
    if (!entries || !actual_count) {
        return -WATERING_ERROR_INVALID_PARAM;
    }
    
    *actual_count = 0;

#ifdef CONFIG_HISTORY_EXTERNAL_FLASH
    /* Read from flash and filter */
    history_env_monthly_t flash_entries[12];
    uint16_t total_count = g_env_history.monthly_count;
    uint16_t offset = 0;
    
    while (offset < total_count && *actual_count < max_entries) {
        uint16_t chunk_size = MIN(12, total_count - offset);
        uint16_t read_count = 0;
        
        int rc = history_flash_read_env_monthly(offset, flash_entries, chunk_size, &read_count);
        if (rc != 0 || read_count == 0) {
            break;
        }
        
        for (uint16_t i = 0; i < read_count && *actual_count < max_entries; i++) {
            if (flash_entries[i].year_month >= start_timestamp && 
                flash_entries[i].year_month <= end_timestamp) {
                entries[*actual_count].year_month = flash_entries[i].year_month;
                entries[*actual_count].temperature.min = flash_entries[i].temp_min_x100 / 100.0f;
                entries[*actual_count].temperature.max = flash_entries[i].temp_max_x100 / 100.0f;
                entries[*actual_count].temperature.avg = flash_entries[i].temp_avg_x100 / 100.0f;
                entries[*actual_count].humidity.min = flash_entries[i].humid_min_x100 / 100.0f;
                entries[*actual_count].humidity.max = flash_entries[i].humid_max_x100 / 100.0f;
                entries[*actual_count].humidity.avg = flash_entries[i].humid_avg_x100 / 100.0f;
                entries[*actual_count].pressure.min = flash_entries[i].press_min_x10 / 10.0f;
                entries[*actual_count].pressure.max = flash_entries[i].press_max_x10 / 10.0f;
                entries[*actual_count].pressure.avg = flash_entries[i].press_avg_x10 / 10.0f;
                entries[*actual_count].total_rainfall_mm = flash_entries[i].total_rainfall_mm_x100 / 100.0f;
                entries[*actual_count].watering_events = flash_entries[i].watering_events;
                entries[*actual_count].total_volume_ml = flash_entries[i].total_volume_ml;
                entries[*actual_count].days_active = flash_entries[i].days_active;
                (*actual_count)++;
            }
        }
        offset += read_count;
    }
#else
    // Search through monthly ring buffer
    for (uint8_t i = 0; i < g_env_history.monthly_count && *actual_count < max_entries; i++) {
        monthly_history_entry_t entry;
        int result = env_history_get_from_ring_buffer(
            g_env_history.monthly,
            sizeof(monthly_history_entry_t),
            ENV_HISTORY_MONTHLY_ENTRIES,
            g_env_history.monthly_head,
            g_env_history.monthly_count,
            i,
            &entry
        );
        
        if (result == 0 && entry.year_month >= start_timestamp && entry.year_month <= end_timestamp) {
            entries[*actual_count] = entry;
            (*actual_count)++;
        }
    }
#endif
    
    return 0;
}

int env_history_get_stats(env_history_stats_t *stats)
{
    if (!g_env_history_initialized) {
        return -WATERING_ERROR_NOT_INITIALIZED;
    }
    
    if (!stats) {
        return -WATERING_ERROR_INVALID_PARAM;
    }
    
    memset(stats, 0, sizeof(env_history_stats_t));
    
#ifdef CONFIG_HISTORY_EXTERNAL_FLASH
    history_flash_stats_t flash_stats;
    if (history_flash_get_stats(&flash_stats) == 0) {
        stats->hourly_entries_used = flash_stats.env_hourly.entry_count;
        stats->daily_entries_used = flash_stats.env_daily.entry_count;
        stats->monthly_entries_used = flash_stats.env_monthly.entry_count;
    }
#else
    stats->hourly_entries_used = g_env_history.hourly_count;
    stats->daily_entries_used = g_env_history.daily_count;
    stats->monthly_entries_used = g_env_history.monthly_count;
#endif
    
    // Calculate oldest timestamps
    if (stats->hourly_entries_used > 0) {
        hourly_history_entry_t oldest_hourly;
        if (env_history_get_oldest_entry(ENV_HISTORY_TYPE_HOURLY, &oldest_hourly) == 0) {
            stats->oldest_hourly_timestamp = oldest_hourly.timestamp;
        }
    }
    
    if (stats->daily_entries_used > 0) {
        daily_history_entry_t oldest_daily;
        if (env_history_get_oldest_entry(ENV_HISTORY_TYPE_DAILY, &oldest_daily) == 0) {
            stats->oldest_daily_timestamp = oldest_daily.date;
        }
    }
    
    if (stats->monthly_entries_used > 0) {
        monthly_history_entry_t oldest_monthly;
        if (env_history_get_oldest_entry(ENV_HISTORY_TYPE_MONTHLY, &oldest_monthly) == 0) {
            stats->oldest_monthly_timestamp = oldest_monthly.year_month;
        }
    }
    
    // Calculate total storage bytes
#ifdef CONFIG_HISTORY_EXTERNAL_FLASH
    stats->total_storage_bytes = (stats->hourly_entries_used * HISTORY_ENV_HOURLY_SIZE) +
                                 (stats->daily_entries_used * HISTORY_ENV_DAILY_SIZE) +
                                 (stats->monthly_entries_used * HISTORY_ENV_MONTHLY_SIZE);
#else
    stats->total_storage_bytes = sizeof(environmental_history_t);
#endif
    
    // Calculate utilization percentage
    stats->storage_utilization_pct = env_history_calculate_utilization();
    
    return 0;
}

int env_history_get_aggregation_status(env_history_aggregation_status_t *status)
{
    if (!g_env_history_initialized) {
        return -WATERING_ERROR_NOT_INITIALIZED;
    }
    
    if (!status) {
        return -WATERING_ERROR_INVALID_PARAM;
    }
    
    memset(status, 0, sizeof(env_history_aggregation_status_t));
    
    status->last_hourly_aggregation = g_env_history.last_hourly_update;
    status->last_daily_aggregation = g_env_history.last_daily_update;
    status->last_monthly_aggregation = g_env_history.last_monthly_update;
    
    uint32_t current_time = (uint32_t)time(NULL);
    uint32_t current_hour_index = env_history_timestamp_to_hour(current_time);
    uint32_t current_day_index = env_history_timestamp_to_day(current_time);
    uint32_t current_month_index = env_history_timestamp_to_month(current_time);

    uint32_t last_hour_index = env_history_timestamp_to_hour(g_env_history.last_hourly_update);
    uint32_t last_day_index = env_history_timestamp_to_day(g_env_history.last_daily_update);
    uint32_t last_month_index = env_history_timestamp_to_month(g_env_history.last_monthly_update);

    status->hourly_aggregation_pending = (current_hour_index > last_hour_index);
    status->daily_aggregation_pending = (current_day_index > last_day_index);
    status->monthly_aggregation_pending = (current_month_index > last_month_index);
    
    return 0;
}

int env_history_cleanup_old_entries(void)
{
    if (!g_env_history_initialized) {
        return -WATERING_ERROR_NOT_INITIALIZED;
    }
    
    // Check if cleanup is needed
    uint8_t utilization = env_history_calculate_utilization();
    if (utilization < ENV_HISTORY_CLEANUP_THRESHOLD) {
        return 0; // No cleanup needed
    }
    
    LOG_INF("Starting environmental history cleanup, utilization: %d%%", utilization);
    
#ifdef CONFIG_HISTORY_EXTERNAL_FLASH
    // External flash uses ring buffers that auto-overwrite oldest entries
    // No explicit cleanup needed - LittleFS manages file size automatically
    LOG_DBG("External flash uses auto-rotating ring buffers");
#else
    // Calculate how many entries to remove to reach target utilization
    uint16_t hourly_to_remove = (g_env_history.hourly_count * 
                                (utilization - ENV_HISTORY_CLEANUP_TARGET)) / 100;
    uint16_t daily_to_remove = (g_env_history.daily_count * 
                               (utilization - ENV_HISTORY_CLEANUP_TARGET)) / 100;
    uint8_t monthly_to_remove = (g_env_history.monthly_count * 
                                (utilization - ENV_HISTORY_CLEANUP_TARGET)) / 100;
    
    // Remove oldest entries
    if (hourly_to_remove > 0 && g_env_history.hourly_count > hourly_to_remove) {
        g_env_history.hourly_count -= hourly_to_remove;
        LOG_DBG("Removed %d hourly entries", hourly_to_remove);
    }
    
    if (daily_to_remove > 0 && g_env_history.daily_count > daily_to_remove) {
        g_env_history.daily_count -= daily_to_remove;
        LOG_DBG("Removed %d daily entries", daily_to_remove);
    }
    
    if (monthly_to_remove > 0 && g_env_history.monthly_count > monthly_to_remove) {
        g_env_history.monthly_count -= monthly_to_remove;
        LOG_DBG("Removed %d monthly entries", monthly_to_remove);
    }
#endif
    
    LOG_INF("Environmental history cleanup completed");
    return 0;
}

int env_history_reset_all(void)
{
    if (!g_env_history_initialized) {
        return -WATERING_ERROR_NOT_INITIALIZED;
    }
    
#ifdef CONFIG_HISTORY_EXTERNAL_FLASH
    // Reset external flash history
    int ret = history_flash_clear(HISTORY_TYPE_ENV_HOURLY);
    if (ret == 0) ret = history_flash_clear(HISTORY_TYPE_ENV_DAILY);
    if (ret == 0) ret = history_flash_clear(HISTORY_TYPE_ENV_MONTHLY);
    if (ret != 0) {
        LOG_ERR("Failed to reset external flash history: %d", ret);
        return ret;
    }
#else
    // Clear all data
    memset(&g_env_history, 0, sizeof(environmental_history_t));
    
    // Reset ring buffer pointers
    g_env_history.hourly_head = 0;
    g_env_history.hourly_count = 0;
    g_env_history.daily_head = 0;
    g_env_history.daily_count = 0;
    g_env_history.monthly_head = 0;
    g_env_history.monthly_count = 0;
#endif
    
    // Reset timestamps
    g_env_history.last_hourly_update = 0;
    g_env_history.last_daily_update = 0;
    g_env_history.last_monthly_update = 0;
    
    LOG_INF("Environmental history reset completed");
    return 0;
}

int env_history_save_to_nvs(void)
{
    if (!g_env_history_initialized) {
        return -WATERING_ERROR_NOT_INITIALIZED;
    }
    
#ifdef CONFIG_HISTORY_EXTERNAL_FLASH
    // Data is automatically persisted in external flash, no NVS needed
    LOG_DBG("Environmental history uses external flash, NVS save skipped");
    return 0;
#else
    // Save hourly data
    int result = nvs_config_write_blob(NVS_KEY_ENV_HISTORY_HOURLY, 
                                      g_env_history.hourly, 
                                      sizeof(g_env_history.hourly));
    if (result != 0) {
        LOG_ERR("Failed to save hourly history to NVS: %d", result);
        return result;
    }
    
    // Save daily data
    result = nvs_config_write_blob(NVS_KEY_ENV_HISTORY_DAILY,
                                  g_env_history.daily,
                                  sizeof(g_env_history.daily));
    if (result != 0) {
        LOG_ERR("Failed to save daily history to NVS: %d", result);
        return result;
    }
    
    // Save monthly data
    result = nvs_config_write_blob(NVS_KEY_ENV_HISTORY_MONTHLY,
                                  g_env_history.monthly,
                                  sizeof(g_env_history.monthly));
    if (result != 0) {
        LOG_ERR("Failed to save monthly history to NVS: %d", result);
        return result;
    }
    
    // Save metadata (pointers and counts)
    struct {
        uint16_t hourly_head, hourly_count;
        uint16_t daily_head, daily_count;
        uint8_t monthly_head, monthly_count;
        uint32_t last_hourly_update, last_daily_update, last_monthly_update;
    } metadata = {
        .hourly_head = g_env_history.hourly_head,
        .hourly_count = g_env_history.hourly_count,
        .daily_head = g_env_history.daily_head,
        .daily_count = g_env_history.daily_count,
        .monthly_head = g_env_history.monthly_head,
        .monthly_count = g_env_history.monthly_count,
        .last_hourly_update = g_env_history.last_hourly_update,
        .last_daily_update = g_env_history.last_daily_update,
        .last_monthly_update = g_env_history.last_monthly_update
    };
    
    result = nvs_config_write_blob(NVS_KEY_ENV_HISTORY_META, &metadata, sizeof(metadata));
    if (result != 0) {
        LOG_ERR("Failed to save history metadata to NVS: %d", result);
        return result;
    }
    
    LOG_DBG("Environmental history saved to NVS");
    return 0;
#endif
}

int env_history_load_from_nvs(void)
{
    if (!g_env_history_initialized) {
        return -WATERING_ERROR_NOT_INITIALIZED;
    }
    
#ifdef CONFIG_HISTORY_EXTERNAL_FLASH
    // Data is stored in external flash, no NVS load needed
    LOG_DBG("Environmental history uses external flash, NVS load skipped");
    return 0;
#else
    // Load metadata first
    struct {
        uint16_t hourly_head, hourly_count;
        uint16_t daily_head, daily_count;
        uint8_t monthly_head, monthly_count;
        uint32_t last_hourly_update, last_daily_update, last_monthly_update;
    } metadata;
    
    size_t metadata_size = sizeof(metadata);
    int result = nvs_config_read_blob(NVS_KEY_ENV_HISTORY_META, &metadata, metadata_size);
    if (result != 0) {
        LOG_WRN("Failed to load history metadata from NVS: %d", result);
        return result;
    }
    
    // Load hourly data
    size_t hourly_size = sizeof(g_env_history.hourly);
    result = nvs_config_read_blob(NVS_KEY_ENV_HISTORY_HOURLY, 
                                 g_env_history.hourly, hourly_size);
    if (result != 0) {
        LOG_WRN("Failed to load hourly history from NVS: %d", result);
        return result;
    }
    
    // Load daily data
    size_t daily_size = sizeof(g_env_history.daily);
    result = nvs_config_read_blob(NVS_KEY_ENV_HISTORY_DAILY,
                                 g_env_history.daily, daily_size);
    if (result != 0) {
        LOG_WRN("Failed to load daily history from NVS: %d", result);
        return result;
    }
    
    // Load monthly data
    size_t monthly_size = sizeof(g_env_history.monthly);
    result = nvs_config_read_blob(NVS_KEY_ENV_HISTORY_MONTHLY,
                                 g_env_history.monthly, monthly_size);
    if (result != 0) {
        LOG_WRN("Failed to load monthly history from NVS: %d", result);
        return result;
    }
    
    // Restore metadata
    g_env_history.hourly_head = metadata.hourly_head;
    g_env_history.hourly_count = metadata.hourly_count;
    g_env_history.daily_head = metadata.daily_head;
    g_env_history.daily_count = metadata.daily_count;
    g_env_history.monthly_head = metadata.monthly_head;
    g_env_history.monthly_count = metadata.monthly_count;
    g_env_history.last_hourly_update = metadata.last_hourly_update;
    g_env_history.last_daily_update = metadata.last_daily_update;
    g_env_history.last_monthly_update = metadata.last_monthly_update;
    
    LOG_INF("Environmental history loaded from NVS: %d hourly, %d daily, %d monthly entries",
            g_env_history.hourly_count, g_env_history.daily_count, g_env_history.monthly_count);
    
    return 0;
#endif
}

uint8_t env_history_calculate_utilization(void)
{
    if (!g_env_history_initialized) {
        return 0;
    }
    
#ifdef CONFIG_HISTORY_EXTERNAL_FLASH
    history_flash_stats_t stats;
    if (history_flash_get_stats(&stats) != 0) {
        return 0;
    }
    uint32_t total_used = stats.env_hourly.entry_count + stats.env_daily.entry_count + stats.env_monthly.entry_count;
    uint32_t total_capacity = ENV_HISTORY_HOURLY_ENTRIES + ENV_HISTORY_DAILY_ENTRIES + ENV_HISTORY_MONTHLY_ENTRIES;
    return (uint8_t)((total_used * 100) / total_capacity);
#else
    // Calculate utilization based on entry counts
    uint32_t total_used = g_env_history.hourly_count + g_env_history.daily_count + g_env_history.monthly_count;
    uint32_t total_capacity = ENV_HISTORY_HOURLY_ENTRIES + ENV_HISTORY_DAILY_ENTRIES + ENV_HISTORY_MONTHLY_ENTRIES;
    
    return (uint8_t)((total_used * 100) / total_capacity);
#endif
}

int env_history_check_aggregation_needed(uint32_t current_timestamp,
                                        bool *hourly_needed,
                                        bool *daily_needed,
                                        bool *monthly_needed)
{
    if (!g_env_history_initialized) {
        return -WATERING_ERROR_NOT_INITIALIZED;
    }
    
    if (!hourly_needed || !daily_needed || !monthly_needed) {
        return -WATERING_ERROR_INVALID_PARAM;
    }
    
    uint32_t current_hour_index = env_history_timestamp_to_hour(current_timestamp);
    uint32_t current_day_index = env_history_timestamp_to_day(current_timestamp);
    uint32_t current_month_index = env_history_timestamp_to_month(current_timestamp);

    uint32_t last_hour_index = env_history_timestamp_to_hour(g_env_history.last_hourly_update);
    uint32_t last_day_index = env_history_timestamp_to_day(g_env_history.last_daily_update);
    uint32_t last_month_index = env_history_timestamp_to_month(g_env_history.last_monthly_update);

    *hourly_needed = (current_hour_index > last_hour_index);
    *daily_needed = (current_day_index > last_day_index);
    *monthly_needed = (current_month_index > last_month_index);
    
    return 0;
}

int env_history_get_latest_entry(env_history_data_type_t data_type, void *entry)
{
    if (!g_env_history_initialized) {
        return -WATERING_ERROR_NOT_INITIALIZED;
    }
    
    if (!entry) {
        return -WATERING_ERROR_INVALID_PARAM;
    }
    
#ifdef CONFIG_HISTORY_EXTERNAL_FLASH
    uint16_t count = 1;
    int ret;
    
    switch (data_type) {
        case ENV_HISTORY_TYPE_HOURLY: {
            history_env_hourly_t flash_entry;
            ret = history_flash_get_latest(HISTORY_TYPE_ENV_HOURLY, &flash_entry, &count);
            if (ret == 0 && count > 0) {
                hourly_history_entry_t *out = (hourly_history_entry_t *)entry;
                out->timestamp = flash_entry.timestamp;
                out->environmental.temperature = flash_entry.temperature_x100 / 100.0f;
                out->environmental.humidity = flash_entry.humidity_x100 / 100.0f;
                out->environmental.pressure = flash_entry.pressure_x100 / 100.0f;
                out->environmental.valid = true;
                out->rainfall_mm = flash_entry.rainfall_mm_x100 / 100.0f;
                out->watering_events = flash_entry.watering_events;
                out->total_volume_ml = flash_entry.total_volume_ml;
                out->active_channels = flash_entry.active_channels;
                return 0;
            }
            return (count == 0) ? -WATERING_ERROR_NO_MEMORY : ret;
        }
        case ENV_HISTORY_TYPE_DAILY: {
            history_env_daily_t flash_entry;
            ret = history_flash_get_latest(HISTORY_TYPE_ENV_DAILY, &flash_entry, &count);
            if (ret == 0 && count > 0) {
                daily_history_entry_t *out = (daily_history_entry_t *)entry;
                out->date = flash_entry.date;
                out->temperature.min = flash_entry.temp_min_x100 / 100.0f;
                out->temperature.max = flash_entry.temp_max_x100 / 100.0f;
                out->temperature.avg = flash_entry.temp_avg_x100 / 100.0f;
                out->humidity.min = flash_entry.humid_min_x100 / 100.0f;
                out->humidity.max = flash_entry.humid_max_x100 / 100.0f;
                out->humidity.avg = flash_entry.humid_avg_x100 / 100.0f;
                out->pressure.min = flash_entry.press_min_x10 / 10.0f;
                out->pressure.max = flash_entry.press_max_x10 / 10.0f;
                out->pressure.avg = flash_entry.press_avg_x10 / 10.0f;
                out->total_rainfall_mm = flash_entry.total_rainfall_mm_x100 / 100.0f;
                out->watering_events = flash_entry.watering_events;
                out->total_volume_ml = flash_entry.total_volume_ml;
                out->sample_count = flash_entry.sample_count;
                out->active_channels_bitmap = flash_entry.active_channels;
                return 0;
            }
            return (count == 0) ? -WATERING_ERROR_NO_MEMORY : ret;
        }
        case ENV_HISTORY_TYPE_MONTHLY: {
            history_env_monthly_t flash_entry;
            ret = history_flash_get_latest(HISTORY_TYPE_ENV_MONTHLY, &flash_entry, &count);
            if (ret == 0 && count > 0) {
                monthly_history_entry_t *out = (monthly_history_entry_t *)entry;
                out->year_month = flash_entry.year_month;
                out->temperature.min = flash_entry.temp_min_x100 / 100.0f;
                out->temperature.max = flash_entry.temp_max_x100 / 100.0f;
                out->temperature.avg = flash_entry.temp_avg_x100 / 100.0f;
                out->humidity.min = flash_entry.humid_min_x100 / 100.0f;
                out->humidity.max = flash_entry.humid_max_x100 / 100.0f;
                out->humidity.avg = flash_entry.humid_avg_x100 / 100.0f;
                out->pressure.min = flash_entry.press_min_x10 / 10.0f;
                out->pressure.max = flash_entry.press_max_x10 / 10.0f;
                out->pressure.avg = flash_entry.press_avg_x10 / 10.0f;
                out->total_rainfall_mm = flash_entry.total_rainfall_mm_x100 / 100.0f;
                out->watering_events = flash_entry.watering_events;
                out->total_volume_ml = flash_entry.total_volume_ml;
                out->days_active = flash_entry.days_active;
                return 0;
            }
            return (count == 0) ? -WATERING_ERROR_NO_MEMORY : ret;
        }
        default:
            return -WATERING_ERROR_INVALID_PARAM;
    }
#else
    switch (data_type) {
        case ENV_HISTORY_TYPE_HOURLY:
            if (g_env_history.hourly_count == 0) {
                return -WATERING_ERROR_NO_MEMORY;
            }
            return env_history_get_from_ring_buffer(
                g_env_history.hourly,
                sizeof(hourly_history_entry_t),
                ENV_HISTORY_HOURLY_ENTRIES,
                g_env_history.hourly_head,
                g_env_history.hourly_count,
                g_env_history.hourly_count - 1,
                entry
            );
            
        case ENV_HISTORY_TYPE_DAILY:
            if (g_env_history.daily_count == 0) {
                return -WATERING_ERROR_NO_MEMORY;
            }
            return env_history_get_from_ring_buffer(
                g_env_history.daily,
                sizeof(daily_history_entry_t),
                ENV_HISTORY_DAILY_ENTRIES,
                g_env_history.daily_head,
                g_env_history.daily_count,
                g_env_history.daily_count - 1,
                entry
            );
            
        case ENV_HISTORY_TYPE_MONTHLY:
            if (g_env_history.monthly_count == 0) {
                return -WATERING_ERROR_NO_MEMORY;
            }
            return env_history_get_from_ring_buffer(
                g_env_history.monthly,
                sizeof(monthly_history_entry_t),
                ENV_HISTORY_MONTHLY_ENTRIES,
                g_env_history.monthly_head,
                g_env_history.monthly_count,
                g_env_history.monthly_count - 1,
                entry
            );
            
        default:
            return -WATERING_ERROR_INVALID_PARAM;
    }
#endif
}

int env_history_get_oldest_entry(env_history_data_type_t data_type, void *entry)
{
    if (!g_env_history_initialized) {
        return -WATERING_ERROR_NOT_INITIALIZED;
    }
    
    if (!entry) {
        return -WATERING_ERROR_INVALID_PARAM;
    }
    
#ifdef CONFIG_HISTORY_EXTERNAL_FLASH
    uint16_t count = 0;
    int ret;
    
    switch (data_type) {
        case ENV_HISTORY_TYPE_HOURLY: {
            history_env_hourly_t flash_entry;
            ret = history_flash_read_env_hourly(0, &flash_entry, 1, &count);
            if (ret == 0 && count > 0) {
                hourly_history_entry_t *out = (hourly_history_entry_t *)entry;
                out->timestamp = flash_entry.timestamp;
                out->environmental.temperature = flash_entry.temperature_x100 / 100.0f;
                out->environmental.humidity = flash_entry.humidity_x100 / 100.0f;
                out->environmental.pressure = flash_entry.pressure_x100 / 100.0f;
                out->environmental.valid = true;
                out->rainfall_mm = flash_entry.rainfall_mm_x100 / 100.0f;
                out->watering_events = flash_entry.watering_events;
                out->total_volume_ml = flash_entry.total_volume_ml;
                out->active_channels = flash_entry.active_channels;
                return 0;
            }
            return (count == 0) ? -WATERING_ERROR_NO_MEMORY : ret;
        }
        case ENV_HISTORY_TYPE_DAILY: {
            history_env_daily_t flash_entry;
            ret = history_flash_read_env_daily(0, &flash_entry, 1, &count);
            if (ret == 0 && count > 0) {
                daily_history_entry_t *out = (daily_history_entry_t *)entry;
                out->date = flash_entry.date;
                out->temperature.min = flash_entry.temp_min_x100 / 100.0f;
                out->temperature.max = flash_entry.temp_max_x100 / 100.0f;
                out->temperature.avg = flash_entry.temp_avg_x100 / 100.0f;
                out->humidity.min = flash_entry.humid_min_x100 / 100.0f;
                out->humidity.max = flash_entry.humid_max_x100 / 100.0f;
                out->humidity.avg = flash_entry.humid_avg_x100 / 100.0f;
                out->pressure.min = flash_entry.press_min_x10 / 10.0f;
                out->pressure.max = flash_entry.press_max_x10 / 10.0f;
                out->pressure.avg = flash_entry.press_avg_x10 / 10.0f;
                out->total_rainfall_mm = flash_entry.total_rainfall_mm_x100 / 100.0f;
                out->watering_events = flash_entry.watering_events;
                out->total_volume_ml = flash_entry.total_volume_ml;
                out->sample_count = flash_entry.sample_count;
                out->active_channels_bitmap = flash_entry.active_channels;
                return 0;
            }
            return (count == 0) ? -WATERING_ERROR_NO_MEMORY : ret;
        }
        case ENV_HISTORY_TYPE_MONTHLY: {
            history_env_monthly_t flash_entry;
            ret = history_flash_read_env_monthly(0, &flash_entry, 1, &count);
            if (ret == 0 && count > 0) {
                monthly_history_entry_t *out = (monthly_history_entry_t *)entry;
                out->year_month = flash_entry.year_month;
                out->temperature.min = flash_entry.temp_min_x100 / 100.0f;
                out->temperature.max = flash_entry.temp_max_x100 / 100.0f;
                out->temperature.avg = flash_entry.temp_avg_x100 / 100.0f;
                out->humidity.min = flash_entry.humid_min_x100 / 100.0f;
                out->humidity.max = flash_entry.humid_max_x100 / 100.0f;
                out->humidity.avg = flash_entry.humid_avg_x100 / 100.0f;
                out->pressure.min = flash_entry.press_min_x10 / 10.0f;
                out->pressure.max = flash_entry.press_max_x10 / 10.0f;
                out->pressure.avg = flash_entry.press_avg_x10 / 10.0f;
                out->total_rainfall_mm = flash_entry.total_rainfall_mm_x100 / 100.0f;
                out->watering_events = flash_entry.watering_events;
                out->total_volume_ml = flash_entry.total_volume_ml;
                out->days_active = flash_entry.days_active;
                return 0;
            }
            return (count == 0) ? -WATERING_ERROR_NO_MEMORY : ret;
        }
        default:
            return -WATERING_ERROR_INVALID_PARAM;
    }
#else
    switch (data_type) {
        case ENV_HISTORY_TYPE_HOURLY:
            if (g_env_history.hourly_count == 0) {
                return -WATERING_ERROR_NO_MEMORY;
            }
            return env_history_get_from_ring_buffer(
                g_env_history.hourly,
                sizeof(hourly_history_entry_t),
                ENV_HISTORY_HOURLY_ENTRIES,
                g_env_history.hourly_head,
                g_env_history.hourly_count,
                0,
                entry
            );
            
        case ENV_HISTORY_TYPE_DAILY:
            if (g_env_history.daily_count == 0) {
                return -WATERING_ERROR_NO_MEMORY;
            }
            return env_history_get_from_ring_buffer(
                g_env_history.daily,
                sizeof(daily_history_entry_t),
                ENV_HISTORY_DAILY_ENTRIES,
                g_env_history.daily_head,
                g_env_history.daily_count,
                0,
                entry
            );
            
        case ENV_HISTORY_TYPE_MONTHLY:
            if (g_env_history.monthly_count == 0) {
                return -WATERING_ERROR_NO_MEMORY;
            }
            return env_history_get_from_ring_buffer(
                g_env_history.monthly,
                sizeof(monthly_history_entry_t),
                ENV_HISTORY_MONTHLY_ENTRIES,
                g_env_history.monthly_head,
                g_env_history.monthly_count,
                0,
                entry
            );
            
        default:
            return -WATERING_ERROR_INVALID_PARAM;
    }
#endif
}

int env_history_validate_integrity(bool repair_if_needed)
{
    if (!g_env_history_initialized) {
        return -WATERING_ERROR_NOT_INITIALIZED;
    }
    
    bool corruption_detected = false;
    
#ifdef CONFIG_HISTORY_EXTERNAL_FLASH
    // External flash validation is handled by LittleFS checksums
    // Just verify we can read stats
    history_flash_stats_t stats;
    if (history_flash_get_stats(&stats) != 0) {
        LOG_ERR("External flash history access error");
        corruption_detected = true;
    }
    (void)repair_if_needed; // LittleFS handles repairs internally
#else
    // Validate ring buffer pointers
    if (g_env_history.hourly_head >= ENV_HISTORY_HOURLY_ENTRIES ||
        g_env_history.hourly_count > ENV_HISTORY_HOURLY_ENTRIES) {
        LOG_ERR("Hourly ring buffer corruption detected");
        corruption_detected = true;
        
        if (repair_if_needed) {
            g_env_history.hourly_head = 0;
            g_env_history.hourly_count = 0;
            LOG_INF("Repaired hourly ring buffer");
        }
    }
    
    if (g_env_history.daily_head >= ENV_HISTORY_DAILY_ENTRIES ||
        g_env_history.daily_count > ENV_HISTORY_DAILY_ENTRIES) {
        LOG_ERR("Daily ring buffer corruption detected");
        corruption_detected = true;
        
        if (repair_if_needed) {
            g_env_history.daily_head = 0;
            g_env_history.daily_count = 0;
            LOG_INF("Repaired daily ring buffer");
        }
    }
    
    if (g_env_history.monthly_head >= ENV_HISTORY_MONTHLY_ENTRIES ||
        g_env_history.monthly_count > ENV_HISTORY_MONTHLY_ENTRIES) {
        LOG_ERR("Monthly ring buffer corruption detected");
        corruption_detected = true;
        
        if (repair_if_needed) {
            g_env_history.monthly_head = 0;
            g_env_history.monthly_count = 0;
            LOG_INF("Repaired monthly ring buffer");
        }
    }
    
    // Validate timestamp consistency
    if (g_env_history.last_hourly_update > g_env_history.last_daily_update + ENV_HISTORY_DAILY_INTERVAL_SEC ||
        g_env_history.last_daily_update > g_env_history.last_monthly_update + ENV_HISTORY_MONTHLY_INTERVAL_SEC) {
        LOG_WRN("Timestamp inconsistency detected in environmental history");
        // This is not necessarily corruption, just a warning
    }
#endif
    
    return corruption_detected ? -WATERING_ERROR_ENV_DATA_CORRUPT : 0;
}

int env_history_get_head_position(env_history_data_type_t data_type)
{
    if (!g_env_history_initialized) {
        return -1;
    }
    
#ifdef CONFIG_HISTORY_EXTERNAL_FLASH
    // External flash uses file-based ring buffers, head position is managed internally
    (void)data_type;
    return 0; // Always 0 for external flash
#else
    switch (data_type) {
        case ENV_HISTORY_TYPE_HOURLY:
            return g_env_history.hourly_head;
        case ENV_HISTORY_TYPE_DAILY:
            return g_env_history.daily_head;
        case ENV_HISTORY_TYPE_MONTHLY:
            return g_env_history.monthly_head;
        default:
            return -1;
    }
#endif
}

int env_history_get_entry_count(env_history_data_type_t data_type)
{
    if (!g_env_history_initialized) {
        return -1;
    }
    
#ifdef CONFIG_HISTORY_EXTERNAL_FLASH
    history_flash_stats_t stats;
    if (history_flash_get_stats(&stats) != 0) {
        return -1;
    }
    switch (data_type) {
        case ENV_HISTORY_TYPE_HOURLY:
            return stats.env_hourly.entry_count;
        case ENV_HISTORY_TYPE_DAILY:
            return stats.env_daily.entry_count;
        case ENV_HISTORY_TYPE_MONTHLY:
            return stats.env_monthly.entry_count;
        default:
            return -1;
    }
#else
    switch (data_type) {
        case ENV_HISTORY_TYPE_HOURLY:
            return g_env_history.hourly_count;
        case ENV_HISTORY_TYPE_DAILY:
            return g_env_history.daily_count;
        case ENV_HISTORY_TYPE_MONTHLY:
            return g_env_history.monthly_count;
        default:
            return -1;
    }
#endif
}

/* Internal helper function implementations */

static int env_history_add_to_ring_buffer(void *buffer, size_t entry_size, 
                                         uint16_t max_entries, uint16_t *head, 
                                         uint16_t *count, const void *new_entry)
{
    if (!buffer || !head || !count || !new_entry) {
        return -WATERING_ERROR_INVALID_PARAM;
    }
    
    // Calculate position to write
    uint8_t *byte_buffer = (uint8_t*)buffer;
    uint16_t write_pos = *head;
    
    // Copy new entry to buffer
    memcpy(byte_buffer + (write_pos * entry_size), new_entry, entry_size);
    
    // Update head pointer (wrap around if necessary)
    *head = (*head + 1) % max_entries;
    
    // Update count (don't exceed maximum)
    if (*count < max_entries) {
        (*count)++;
    }
    
    return 0;
}

static int env_history_get_from_ring_buffer(const void *buffer, size_t entry_size,
                                           uint16_t max_entries, uint16_t head,
                                           uint16_t count, uint16_t index, void *entry)
{
    if (!buffer || !entry || index >= count) {
        return -WATERING_ERROR_INVALID_PARAM;
    }
    
    // Calculate actual position in ring buffer
    uint16_t actual_pos;
    if (count < max_entries) {
        // Buffer not full yet, simple indexing
        actual_pos = index;
    } else {
        // Buffer is full, calculate position relative to head
        actual_pos = (head + index) % max_entries;
    }
    
    const uint8_t *byte_buffer = (const uint8_t*)buffer;
    memcpy(entry, byte_buffer + (actual_pos * entry_size), entry_size);
    
    return 0;
}

static uint32_t env_history_timestamp_to_hour(uint32_t timestamp)
{
    return timestamp / ENV_HISTORY_HOURLY_INTERVAL_SEC;
}

static uint32_t env_history_timestamp_to_day(uint32_t timestamp)
{
    return timestamp / ENV_HISTORY_DAILY_INTERVAL_SEC;
}

static uint32_t env_history_timestamp_to_month(uint32_t timestamp)
{
    return timestamp / ENV_HISTORY_MONTHLY_INTERVAL_SEC;
}

static int env_history_find_hourly_entries_for_day(uint32_t day_timestamp,
                                                   hourly_history_entry_t *entries,
                                                   uint16_t *count)
{
    if (!entries || !count) {
        return -WATERING_ERROR_INVALID_PARAM;
    }
    
    *count = 0;
    uint32_t day_start = day_timestamp * ENV_HISTORY_DAILY_INTERVAL_SEC;
    uint32_t day_end = day_start + ENV_HISTORY_DAILY_INTERVAL_SEC - 1;
    
#ifdef CONFIG_HISTORY_EXTERNAL_FLASH
    // Use env_history_get_hourly_range for flash storage
    return env_history_get_hourly_range(day_start, day_end, entries, 24, count);
#else
    // Search through hourly entries
    for (uint16_t i = 0; i < g_env_history.hourly_count && *count < 24; i++) {
        hourly_history_entry_t entry;
        int result = env_history_get_from_ring_buffer(
            g_env_history.hourly,
            sizeof(hourly_history_entry_t),
            ENV_HISTORY_HOURLY_ENTRIES,
            g_env_history.hourly_head,
            g_env_history.hourly_count,
            i,
            &entry
        );
        
        if (result == 0 && entry.timestamp >= day_start && entry.timestamp <= day_end) {
            entries[*count] = entry;
            (*count)++;
        }
    }
    
    return 0;
#endif
}

static int env_history_find_daily_entries_for_month(uint32_t month_timestamp,
                                                    daily_history_entry_t *entries,
                                                    uint16_t *count)
{
    if (!entries || !count) {
        return -WATERING_ERROR_INVALID_PARAM;
    }
    
    *count = 0;
    uint32_t month_start = month_timestamp * ENV_HISTORY_MONTHLY_INTERVAL_SEC;
    uint32_t month_end = month_start + ENV_HISTORY_MONTHLY_INTERVAL_SEC - 1;
    
#ifdef CONFIG_HISTORY_EXTERNAL_FLASH
    // Use env_history_get_daily_range for flash storage
    return env_history_get_daily_range(month_start, month_end, entries, 31, count);
#else
    // Search through daily entries
    for (uint16_t i = 0; i < g_env_history.daily_count && *count < 31; i++) {
        daily_history_entry_t entry;
        int result = env_history_get_from_ring_buffer(
            g_env_history.daily,
            sizeof(daily_history_entry_t),
            ENV_HISTORY_DAILY_ENTRIES,
            g_env_history.daily_head,
            g_env_history.daily_count,
            i,
            &entry
        );
        
        if (result == 0 && entry.date >= month_start && entry.date <= month_end) {
            entries[*count] = entry;
            (*count)++;
        }
    }
    
    return 0;
#endif
}

/**
 * @brief Initialize the environmental history system
 * 
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t environmental_history_init(void)
{
    int ret = env_history_init();
    if (ret != 0) {
        LOG_ERR("Failed to initialize environmental history: %d", ret);
        return WATERING_ERROR_BME280_INIT;
    }
    
    LOG_INF("Environmental history system initialized");
    return WATERING_SUCCESS;
}
