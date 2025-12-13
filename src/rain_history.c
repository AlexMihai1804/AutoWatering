#include "rain_history.h"
#include "rain_config.h"
#include "nvs_config.h"
#include "rtc.h"
#include "timezone.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <stdio.h>
#include "watering.h"
#include <errno.h>

#ifdef CONFIG_HISTORY_EXTERNAL_FLASH
#include "history_flash.h"
#endif

LOG_MODULE_REGISTER(rain_history, LOG_LEVEL_INF);

/* External NVS function declarations */
extern int nvs_save_rain_hourly_data(const void *hourly_data, uint16_t entry_count, size_t data_size);
extern int nvs_load_rain_hourly_data(void *hourly_data, uint16_t max_entries, uint16_t *actual_count);
extern int nvs_save_rain_daily_data(const void *daily_data, uint16_t entry_count, size_t data_size);
extern int nvs_load_rain_daily_data(void *daily_data, uint16_t max_entries, uint16_t *actual_count);
extern int nvs_clear_rain_history(void);
extern int nvs_get_rain_storage_usage(size_t *used_bytes, size_t *total_bytes);

/* Internal storage structures */

/* Global state variable definition */
struct rain_history_state_s rain_history_state = {
    .initialized = false,
    .hourly_count = 0,
    .daily_count = 0,
    .hourly_write_index = 0,
    .daily_write_index = 0,
    .last_hourly_save = 0,
    .command_active = false,
    .requesting_conn = NULL,
    .current_command = 0,
    .start_timestamp = 0,
    .end_timestamp = 0,
    .max_entries = 0,
    .data_type = 0,
    .current_entry = 0,
    .total_entries = 0,
    .current_fragment = 0,
    .total_fragments = 0,
    .fragment_buffer = NULL
};

/* Forward declarations */
static uint32_t get_hour_epoch(uint32_t timestamp);
static uint32_t get_day_epoch(uint32_t timestamp);
#ifndef CONFIG_HISTORY_EXTERNAL_FLASH
static watering_error_t rotate_hourly_data(void);
static watering_error_t rotate_daily_data(void);
static int find_hourly_index(uint32_t hour_epoch);
static int find_daily_index(uint32_t day_epoch);
#endif

/**
 * @brief Get hour epoch (timestamp rounded to hour boundary)
 */
static uint32_t get_hour_epoch(uint32_t timestamp)
{
    return (timestamp / 3600) * 3600;
}

/**
 * @brief Get day epoch (timestamp rounded to day boundary)
 */
static uint32_t get_day_epoch(uint32_t timestamp)
{
    return (timestamp / 86400) * 86400;
}

/**
 * @brief Find index of hourly data entry
 */
#ifndef CONFIG_HISTORY_EXTERNAL_FLASH
static int find_hourly_index(uint32_t hour_epoch)
{
    for (int i = 0; i < rain_history_state.hourly_count; i++) {
        if (rain_history_state.hourly_data[i].hour_epoch == hour_epoch) {
            return i;
        }
    }
    return -1;
}
#endif

/**
 * @brief Find index of daily data entry
 */
#ifndef CONFIG_HISTORY_EXTERNAL_FLASH
static int find_daily_index(uint32_t day_epoch)
{
    for (int i = 0; i < rain_history_state.daily_count; i++) {
        if (rain_history_state.daily_data[i].day_epoch == day_epoch) {
            return i;
        }
    }
    return -1;
}
#endif

/**
 * @brief Rotate hourly data when storage is full
 */
#ifndef CONFIG_HISTORY_EXTERNAL_FLASH
static watering_error_t rotate_hourly_data(void)
{
    if (rain_history_state.hourly_count < RAIN_HOURLY_ENTRIES) {
        return WATERING_SUCCESS;
    }
    
    /* Find oldest entry */
    uint32_t oldest_time = UINT32_MAX;
    int oldest_index = 0;
    
    for (int i = 0; i < RAIN_HOURLY_ENTRIES; i++) {
        if (rain_history_state.hourly_data[i].hour_epoch < oldest_time) {
            oldest_time = rain_history_state.hourly_data[i].hour_epoch;
            oldest_index = i;
        }
    }
    
    /* Use the oldest entry slot for new data */
    rain_history_state.hourly_write_index = oldest_index;
    
    LOG_DBG("Rotated hourly data, oldest entry at index %d", oldest_index);
    return WATERING_SUCCESS;
}
#endif

/**
 * @brief Rotate daily data when storage is full
 */
#ifndef CONFIG_HISTORY_EXTERNAL_FLASH
static watering_error_t rotate_daily_data(void)
{
    if (rain_history_state.daily_count < RAIN_DAILY_ENTRIES) {
        return WATERING_SUCCESS;
    }
    
    /* Find oldest entry */
    uint32_t oldest_time = UINT32_MAX;
    int oldest_index = 0;
    
    for (int i = 0; i < RAIN_DAILY_ENTRIES; i++) {
        if (rain_history_state.daily_data[i].day_epoch < oldest_time) {
            oldest_time = rain_history_state.daily_data[i].day_epoch;
            oldest_index = i;
        }
    }
    
    /* Use the oldest entry slot for new data */
    rain_history_state.daily_write_index = oldest_index;
    
    LOG_DBG("Rotated daily data, oldest entry at index %d", oldest_index);
    return WATERING_SUCCESS;
}
#endif

watering_error_t rain_history_init(void)
{
    if (rain_history_state.initialized) {
        return WATERING_SUCCESS;
    }
    
    LOG_INF("Initializing rain history system");
    
    /* Initialize mutex */
    k_mutex_init(&rain_history_state.mutex);
    
#ifdef CONFIG_HISTORY_EXTERNAL_FLASH
    /* Initialize external flash storage for rain history */
    int ret = history_flash_init();
    if (ret != 0) {
        LOG_ERR("Failed to initialize rain history flash storage: %d", ret);
        return WATERING_ERROR_STORAGE;
    }
    
    /* Get stats from flash */
    history_flash_stats_t flash_stats;
    if (history_flash_get_stats(&flash_stats) == 0) {
        rain_history_state.hourly_count = flash_stats.rain_hourly.entry_count;
        rain_history_state.daily_count = flash_stats.rain_daily.entry_count;
    }
#else
    /* Clear data arrays */
    memset(rain_history_state.hourly_data, 0, sizeof(rain_history_state.hourly_data));
    memset(rain_history_state.daily_data, 0, sizeof(rain_history_state.daily_data));
    
    /* Load existing data from NVS */
    watering_error_t ret = rain_history_load_from_nvs();
    if (ret != WATERING_SUCCESS) {
        LOG_WRN("Failed to load rain history from NVS: %d", ret);
        /* Continue with empty history */
    }
#endif
    
    rain_history_state.initialized = true;
    
    LOG_INF("Rain history system initialized");
    LOG_INF("Hourly entries: %u/%u, Daily entries: %u/%u",
            rain_history_state.hourly_count, RAIN_HOURLY_ENTRIES,
            rain_history_state.daily_count, RAIN_DAILY_ENTRIES);
    
    return WATERING_SUCCESS;
}

watering_error_t rain_history_deinit(void)
{
    if (!rain_history_state.initialized) {
        return WATERING_SUCCESS;
    }
    
    /* Save current data to NVS */
    rain_history_save_to_nvs();
    
    rain_history_state.initialized = false;
    
    LOG_INF("Rain history system deinitialized");
    return WATERING_SUCCESS;
}

watering_error_t rain_history_record_hourly(float rainfall_mm)
{
    uint32_t current_time = timezone_get_unix_utc();
    if (current_time == 0U) {
        LOG_WRN("Cannot record rain history: RTC time not available");
        return WATERING_ERROR_RTC_FAILURE;
    }
    uint32_t hour_epoch = get_hour_epoch(current_time);
    
    return rain_history_record_hourly_full(hour_epoch, rainfall_mm, 0, RAIN_QUALITY_GOOD);
}

watering_error_t rain_history_record_hourly_full(uint32_t hour_epoch,
                                                 float rainfall_mm,
                                                 uint8_t pulse_count,
                                                 uint8_t data_quality)
{
    if (!rain_history_state.initialized) {
        return WATERING_ERROR_NOT_INITIALIZED;
    }
    
    if (rainfall_mm < 0.0f || rainfall_mm > 1000.0f) {
        LOG_ERR("Invalid rainfall amount: %.2f mm", (double)rainfall_mm);
        return WATERING_ERROR_INVALID_PARAM;
    }
    
#ifdef CONFIG_HISTORY_EXTERNAL_FLASH
    /* Use external flash storage */
    history_rain_hourly_t flash_entry = {
        .hour_epoch = hour_epoch,
        .rainfall_mm_x100 = (uint16_t)(rainfall_mm * 100.0f),
        .pulse_count = pulse_count,
        .data_quality = data_quality
    };
    
    int ret = history_flash_add_rain_hourly(&flash_entry);
    if (ret < 0) {
        LOG_ERR("Failed to add rain hourly to flash: %d", ret);
        return WATERING_ERROR_STORAGE;
    }
    
    LOG_DBG("Added hourly entry to flash for epoch %u: %.2f mm", hour_epoch, (double)rainfall_mm);
    return WATERING_SUCCESS;
#else
    k_mutex_lock(&rain_history_state.mutex, K_FOREVER);
    
    /* Check if entry already exists for this hour */
    int existing_index = find_hourly_index(hour_epoch);
    if (existing_index >= 0) {
        /* Update existing entry */
        rain_history_state.hourly_data[existing_index].rainfall_mm_x100 = 
            (uint16_t)(rainfall_mm * 100.0f);
        rain_history_state.hourly_data[existing_index].pulse_count = pulse_count;
        rain_history_state.hourly_data[existing_index].data_quality = data_quality;
        
        LOG_DBG("Updated hourly entry for epoch %u: %.2f mm", hour_epoch, (double)rainfall_mm);
    } else {
        /* Add new entry */
        if (rain_history_state.hourly_count >= RAIN_HOURLY_ENTRIES) {
            rotate_hourly_data();
        } else {
            rain_history_state.hourly_write_index = rain_history_state.hourly_count;
            rain_history_state.hourly_count++;
        }
        
        rain_hourly_data_t *entry = &rain_history_state.hourly_data[rain_history_state.hourly_write_index];
        entry->hour_epoch = hour_epoch;
        entry->rainfall_mm_x100 = (uint16_t)(rainfall_mm * 100.0f);
        entry->pulse_count = pulse_count;
        entry->data_quality = data_quality;
        
        LOG_DBG("Added hourly entry for epoch %u: %.2f mm (index %u)", 
            hour_epoch, (double)rainfall_mm, rain_history_state.hourly_write_index);
        
        /* Move to next index for circular buffer */
        rain_history_state.hourly_write_index = 
            (rain_history_state.hourly_write_index + 1) % RAIN_HOURLY_ENTRIES;
    }
    
    k_mutex_unlock(&rain_history_state.mutex);
    
    /* Save to NVS periodically (every 6 hours) */
    uint32_t current_time = k_uptime_get_32() / 1000;
    if (current_time - rain_history_state.last_hourly_save > 21600) {
        rain_history_save_to_nvs();
        rain_history_state.last_hourly_save = current_time;
    }
    
    return WATERING_SUCCESS;
#endif /* CONFIG_HISTORY_EXTERNAL_FLASH */
}

watering_error_t rain_history_aggregate_daily(void)
{
    if (!rain_history_state.initialized) {
        return WATERING_ERROR_NOT_INITIALIZED;
    }
    
#ifdef CONFIG_HISTORY_EXTERNAL_FLASH
    /* Flash-based aggregation - read recent hourly data and aggregate */
    uint32_t current_time = timezone_get_unix_utc();
    if (current_time == 0U) {
        LOG_WRN("Skipping daily rain aggregation: RTC time not available");
        return WATERING_SUCCESS;
    }
    uint32_t today_epoch = get_day_epoch(current_time);
    uint32_t yesterday_epoch = today_epoch - 86400;

    /* Avoid duplicating yesterday's summary on every maintenance call */
    history_rain_daily_t last_daily = {0};
    uint16_t last_daily_count = 1;
    int daily_chk = history_flash_get_latest(HISTORY_TYPE_RAIN_DAILY, &last_daily, &last_daily_count);
    if (daily_chk == 0 && last_daily_count == 1U && last_daily.day_epoch == yesterday_epoch) {
        return WATERING_SUCCESS;
    }

    /* Read hourly entries from flash */
    history_rain_hourly_t hourly_buffer[48]; /* Last 2 days max */
    uint16_t hourly_count = (uint16_t)(sizeof(hourly_buffer) / sizeof(hourly_buffer[0]));
    
    int ret = history_flash_get_latest(HISTORY_TYPE_RAIN_HOURLY, hourly_buffer, &hourly_count);
    if (ret < 0) {
        LOG_ERR("Failed to read rain hourly from flash: %d", ret);
        return WATERING_ERROR_STORAGE;
    }
    
    /* Aggregate yesterday's data */
    float total_rainfall = 0.0f;
    float max_hourly = 0.0f;
    uint8_t active_hours = 0;
    uint8_t valid_hours = 0;
    
    for (int i = 0; i < hourly_count; i++) {
        if (hourly_buffer[i].hour_epoch >= yesterday_epoch && 
            hourly_buffer[i].hour_epoch < today_epoch) {
            float hourly_mm = hourly_buffer[i].rainfall_mm_x100 / 100.0f;
            total_rainfall += hourly_mm;
            
            if (hourly_mm > max_hourly) {
                max_hourly = hourly_mm;
            }
            
            if (hourly_mm > 0.0f) {
                active_hours++;
            }
            
            if (hourly_buffer[i].data_quality >= RAIN_QUALITY_FAIR) {
                valid_hours++;
            }
        }
    }
    
    /* Only create daily entry if we have some data */
    if (valid_hours > 0) {
        history_rain_daily_t daily_entry = {
            .day_epoch = yesterday_epoch,
            .total_rainfall_mm_x100 = (uint32_t)(total_rainfall * 100.0f),
            .max_hourly_mm_x100 = (uint16_t)(max_hourly * 100.0f),
            .active_hours = active_hours,
            .data_completeness = (valid_hours * 100) / 24
        };
        
        ret = history_flash_add_rain_daily(&daily_entry);
        if (ret < 0) {
            LOG_ERR("Failed to add rain daily to flash: %d", ret);
            return WATERING_ERROR_STORAGE;
        }
        
        LOG_INF("Daily aggregation for %u: %.2f mm total, %.2f mm max hourly, %u active hours",
            yesterday_epoch, (double)total_rainfall, (double)max_hourly, active_hours);
    }
    
    return WATERING_SUCCESS;
#else
    uint32_t current_time = timezone_get_unix_utc();
    if (current_time == 0U) {
        LOG_WRN("Skipping daily rain aggregation: RTC time not available");
        return WATERING_SUCCESS;
    }
    uint32_t today_epoch = get_day_epoch(current_time);
    uint32_t yesterday_epoch = today_epoch - 86400;
    
    k_mutex_lock(&rain_history_state.mutex, K_FOREVER);
    
    /* Aggregate yesterday's data */
    float total_rainfall = 0.0f;
    float max_hourly = 0.0f;
    uint8_t active_hours = 0;
    uint8_t valid_hours = 0;
    
    for (int i = 0; i < rain_history_state.hourly_count; i++) {
        rain_hourly_data_t *hourly = &rain_history_state.hourly_data[i];
        
        if (hourly->hour_epoch >= yesterday_epoch && hourly->hour_epoch < today_epoch) {
            float hourly_mm = hourly->rainfall_mm_x100 / 100.0f;
            total_rainfall += hourly_mm;
            
            if (hourly_mm > max_hourly) {
                max_hourly = hourly_mm;
            }
            
            if (hourly_mm > 0.0f) {
                active_hours++;
            }
            
            if (hourly->data_quality >= RAIN_QUALITY_FAIR) {
                valid_hours++;
            }
        }
    }
    
    /* Only create daily entry if we have some data */
    if (valid_hours > 0) {
        /* Check if daily entry already exists */
        int existing_index = find_daily_index(yesterday_epoch);
        if (existing_index >= 0) {
            /* Update existing entry */
            rain_daily_data_t *daily = &rain_history_state.daily_data[existing_index];
            daily->total_rainfall_mm_x100 = (uint32_t)(total_rainfall * 100.0f);
            daily->max_hourly_mm_x100 = (uint16_t)(max_hourly * 100.0f);
            daily->active_hours = active_hours;
            daily->data_completeness = (valid_hours * 100) / 24;
            
            LOG_DBG("Updated daily entry for %u: %.2f mm", yesterday_epoch, (double)total_rainfall);
        } else {
            /* Add new daily entry */
            if (rain_history_state.daily_count >= RAIN_DAILY_ENTRIES) {
                rotate_daily_data();
            } else {
                rain_history_state.daily_write_index = rain_history_state.daily_count;
                rain_history_state.daily_count++;
            }
            
            rain_daily_data_t *daily = &rain_history_state.daily_data[rain_history_state.daily_write_index];
            daily->day_epoch = yesterday_epoch;
            daily->total_rainfall_mm_x100 = (uint32_t)(total_rainfall * 100.0f);
            daily->max_hourly_mm_x100 = (uint16_t)(max_hourly * 100.0f);
            daily->active_hours = active_hours;
            daily->data_completeness = (valid_hours * 100) / 24;
            
            LOG_INF("Daily aggregation for %u: %.2f mm total, %.2f mm max hourly, %u active hours",
                yesterday_epoch, (double)total_rainfall, (double)max_hourly, active_hours);
            
            /* Move to next index for circular buffer */
            rain_history_state.daily_write_index = 
                (rain_history_state.daily_write_index + 1) % RAIN_DAILY_ENTRIES;
        }
    }
    
    k_mutex_unlock(&rain_history_state.mutex);
    
    return WATERING_SUCCESS;
#endif /* CONFIG_HISTORY_EXTERNAL_FLASH */
}

watering_error_t rain_history_get_hourly(uint32_t start_hour, 
                                         uint32_t end_hour,
                                         rain_hourly_data_t *data, 
                                         uint16_t max_entries,
                                         uint16_t *count)
{
    if (!rain_history_state.initialized || !data || !count) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    *count = 0;
    
#ifdef CONFIG_HISTORY_EXTERNAL_FLASH
    /* Read from external flash in small chunks to avoid stack overflow */
    history_flash_stats_t flash_stats;
    int stats_ret = history_flash_get_stats(&flash_stats);
    if (stats_ret < 0) {
        LOG_ERR("Failed to get rain history stats from flash: %d", stats_ret);
        return WATERING_ERROR_STORAGE;
    }

    uint16_t total_entries = flash_stats.rain_hourly.entry_count;
    if (total_entries == 0 || max_entries == 0) {
        return WATERING_SUCCESS;
    }

    history_rain_hourly_t flash_chunk[32];
    uint16_t offset = 0;

    while (offset < total_entries && *count < max_entries) {
        uint16_t remaining = (uint16_t)(total_entries - offset);
        uint16_t chunk_size = remaining > (uint16_t)(sizeof(flash_chunk) / sizeof(flash_chunk[0]))
                                  ? (uint16_t)(sizeof(flash_chunk) / sizeof(flash_chunk[0]))
                                  : remaining;
        uint16_t read_count = 0;

        int ret = history_flash_read_rain_hourly(offset, flash_chunk, chunk_size, &read_count);
        if (ret < 0) {
            LOG_ERR("Failed to read rain hourly from flash: %d", ret);
            return WATERING_ERROR_STORAGE;
        }
        if (read_count == 0U) {
            break;
        }

        for (uint16_t i = 0; i < read_count && *count < max_entries; i++) {
            if (flash_chunk[i].hour_epoch >= start_hour && flash_chunk[i].hour_epoch <= end_hour) {
                data[*count].hour_epoch = flash_chunk[i].hour_epoch;
                data[*count].rainfall_mm_x100 = flash_chunk[i].rainfall_mm_x100;
                data[*count].pulse_count = flash_chunk[i].pulse_count;
                data[*count].data_quality = flash_chunk[i].data_quality;
                (*count)++;
            }
        }

        offset = (uint16_t)(offset + read_count);
    }
    
    LOG_DBG("Retrieved %u hourly entries from flash for range %u-%u", *count, start_hour, end_hour);
    return WATERING_SUCCESS;
#else
    k_mutex_lock(&rain_history_state.mutex, K_FOREVER);
    
    for (int i = 0; i < rain_history_state.hourly_count && *count < max_entries; i++) {
        rain_hourly_data_t *hourly = &rain_history_state.hourly_data[i];
        
        if (hourly->hour_epoch >= start_hour && hourly->hour_epoch <= end_hour) {
            memcpy(&data[*count], hourly, sizeof(rain_hourly_data_t));
            (*count)++;
        }
    }
    
    k_mutex_unlock(&rain_history_state.mutex);
    
    LOG_DBG("Retrieved %u hourly entries for range %u-%u", *count, start_hour, end_hour);
    return WATERING_SUCCESS;
#endif /* CONFIG_HISTORY_EXTERNAL_FLASH */
}

watering_error_t rain_history_get_daily(uint32_t start_day, 
                                        uint32_t end_day,
                                        rain_daily_data_t *data, 
                                        uint16_t max_entries,
                                        uint16_t *count)
{
    if (!rain_history_state.initialized || !data || !count) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    *count = 0;
    
#ifdef CONFIG_HISTORY_EXTERNAL_FLASH
    /* Read from external flash in small chunks to avoid stack overflow */
    history_flash_stats_t flash_stats;
    int stats_ret = history_flash_get_stats(&flash_stats);
    if (stats_ret < 0) {
        LOG_ERR("Failed to get rain history stats from flash: %d", stats_ret);
        return WATERING_ERROR_STORAGE;
    }

    uint16_t total_entries = flash_stats.rain_daily.entry_count;
    if (total_entries == 0 || max_entries == 0) {
        return WATERING_SUCCESS;
    }

    history_rain_daily_t flash_chunk[32];
    uint16_t offset = 0;

    while (offset < total_entries && *count < max_entries) {
        uint16_t remaining = (uint16_t)(total_entries - offset);
        uint16_t chunk_size = remaining > (uint16_t)(sizeof(flash_chunk) / sizeof(flash_chunk[0]))
                                  ? (uint16_t)(sizeof(flash_chunk) / sizeof(flash_chunk[0]))
                                  : remaining;
        uint16_t read_count = 0;

        int ret = history_flash_read_rain_daily(offset, flash_chunk, chunk_size, &read_count);
        if (ret < 0) {
            LOG_ERR("Failed to read rain daily from flash: %d", ret);
            return WATERING_ERROR_STORAGE;
        }
        if (read_count == 0U) {
            break;
        }

        for (uint16_t i = 0; i < read_count && *count < max_entries; i++) {
            if (flash_chunk[i].day_epoch >= start_day && flash_chunk[i].day_epoch <= end_day) {
                data[*count].day_epoch = flash_chunk[i].day_epoch;
                data[*count].total_rainfall_mm_x100 = flash_chunk[i].total_rainfall_mm_x100;
                data[*count].max_hourly_mm_x100 = flash_chunk[i].max_hourly_mm_x100;
                data[*count].active_hours = flash_chunk[i].active_hours;
                data[*count].data_completeness = flash_chunk[i].data_completeness;
                (*count)++;
            }
        }

        offset = (uint16_t)(offset + read_count);
    }
    
    LOG_DBG("Retrieved %u daily entries from flash for range %u-%u", *count, start_day, end_day);
    return WATERING_SUCCESS;
#else
    k_mutex_lock(&rain_history_state.mutex, K_FOREVER);
    
    for (int i = 0; i < rain_history_state.daily_count && *count < max_entries; i++) {
        rain_daily_data_t *daily = &rain_history_state.daily_data[i];
        
        if (daily->day_epoch >= start_day && daily->day_epoch <= end_day) {
            memcpy(&data[*count], daily, sizeof(rain_daily_data_t));
            (*count)++;
        }
    }
    
    k_mutex_unlock(&rain_history_state.mutex);
    
    LOG_DBG("Retrieved %u daily entries for range %u-%u", *count, start_day, end_day);
    return WATERING_SUCCESS;
#endif /* CONFIG_HISTORY_EXTERNAL_FLASH */
}

float rain_history_get_recent_total(uint32_t hours_back)
{
    if (!rain_history_state.initialized) {
        return 0.0f;
    }
    
    uint32_t current_time = timezone_get_unix_utc();
    if (current_time == 0U) {
        return 0.0f;
    }
    uint32_t start_time = current_time - (hours_back * 3600);
    
    float total_rainfall = 0.0f;
    
#ifdef CONFIG_HISTORY_EXTERNAL_FLASH
    /* Read from external flash in small chunks to avoid stack overflow */
    history_flash_stats_t flash_stats;
    int stats_ret = history_flash_get_stats(&flash_stats);
    if (stats_ret < 0) {
        LOG_ERR("Failed to get rain history stats from flash: %d", stats_ret);
        return 0.0f;
    }

    uint16_t total_entries = flash_stats.rain_hourly.entry_count;
    history_rain_hourly_t flash_chunk[32];
    uint16_t offset = 0;

    while (offset < total_entries) {
        uint16_t remaining = (uint16_t)(total_entries - offset);
        uint16_t chunk_size = remaining > (uint16_t)(sizeof(flash_chunk) / sizeof(flash_chunk[0]))
                                  ? (uint16_t)(sizeof(flash_chunk) / sizeof(flash_chunk[0]))
                                  : remaining;
        uint16_t read_count = 0;

        int ret = history_flash_read_rain_hourly(offset, flash_chunk, chunk_size, &read_count);
        if (ret < 0) {
            LOG_ERR("Failed to read rain hourly from flash: %d", ret);
            return 0.0f;
        }
        if (read_count == 0U) {
            break;
        }

        for (uint16_t i = 0; i < read_count; i++) {
            if (flash_chunk[i].hour_epoch >= start_time && flash_chunk[i].hour_epoch <= current_time) {
                total_rainfall += flash_chunk[i].rainfall_mm_x100 / 100.0f;
            }
        }

        offset = (uint16_t)(offset + read_count);
    }
#else
    k_mutex_lock(&rain_history_state.mutex, K_FOREVER);
    
    for (int i = 0; i < rain_history_state.hourly_count; i++) {
        rain_hourly_data_t *hourly = &rain_history_state.hourly_data[i];
        
        if (hourly->hour_epoch >= start_time && hourly->hour_epoch <= current_time) {
            total_rainfall += hourly->rainfall_mm_x100 / 100.0f;
        }
    }
    
    k_mutex_unlock(&rain_history_state.mutex);
#endif /* CONFIG_HISTORY_EXTERNAL_FLASH */
    
    return total_rainfall;
}

bool rain_history_significant_rain_detected(uint32_t hours_back, float threshold_mm)
{
    float total_rainfall = rain_history_get_recent_total(hours_back);
    return total_rainfall >= threshold_mm;
}

float rain_history_get_last_24h(void)
{
    return rain_history_get_recent_total(24);
}

float rain_history_get_today(void)
{
    uint32_t current_time = timezone_get_unix_utc();
    if (current_time == 0U) {
        return 0.0f;
    }
    uint32_t today_start = get_day_epoch(current_time);
    
    float total_rainfall = 0.0f;
    
#ifdef CONFIG_HISTORY_EXTERNAL_FLASH
    /* Read just the most recent entries (today is always within last 48 hours) */
    history_rain_hourly_t flash_buffer[48];
    uint16_t flash_count = (uint16_t)(sizeof(flash_buffer) / sizeof(flash_buffer[0]));

    int ret = history_flash_get_latest(HISTORY_TYPE_RAIN_HOURLY, flash_buffer, &flash_count);
    if (ret < 0) {
        LOG_ERR("Failed to read rain hourly from flash: %d", ret);
        return 0.0f;
    }

    for (uint16_t i = 0; i < flash_count; i++) {
        if (flash_buffer[i].hour_epoch >= today_start) {
            total_rainfall += flash_buffer[i].rainfall_mm_x100 / 100.0f;
        }
    }
#else
    k_mutex_lock(&rain_history_state.mutex, K_FOREVER);
    
    for (int i = 0; i < rain_history_state.hourly_count; i++) {
        rain_hourly_data_t *hourly = &rain_history_state.hourly_data[i];
        
        if (hourly->hour_epoch >= today_start) {
            total_rainfall += hourly->rainfall_mm_x100 / 100.0f;
        }
    }
    
    k_mutex_unlock(&rain_history_state.mutex);
#endif /* CONFIG_HISTORY_EXTERNAL_FLASH */
    
    return total_rainfall;
}

float rain_history_get_current_hour(void)
{
    uint32_t current_time = timezone_get_unix_utc();
    if (current_time == 0U) {
        return 0.0f;
    }
    uint32_t current_hour = get_hour_epoch(current_time);
    
    float rainfall = 0.0f;
    
#ifdef CONFIG_HISTORY_EXTERNAL_FLASH
    /* Read from external flash */
    history_rain_hourly_t flash_buffer[24]; /* Just recent entries */
    uint16_t flash_count = (uint16_t)(sizeof(flash_buffer) / sizeof(flash_buffer[0]));
    
    int ret = history_flash_get_latest(HISTORY_TYPE_RAIN_HOURLY, flash_buffer, &flash_count);
    if (ret >= 0) {
        for (uint16_t i = 0; i < flash_count; i++) {
            if (flash_buffer[i].hour_epoch == current_hour) {
                rainfall = flash_buffer[i].rainfall_mm_x100 / 100.0f;
                break;
            }
        }
    }
#else
    k_mutex_lock(&rain_history_state.mutex, K_FOREVER);
    
    int index = find_hourly_index(current_hour);
    
    if (index >= 0) {
        rainfall = rain_history_state.hourly_data[index].rainfall_mm_x100 / 100.0f;
    }
    
    k_mutex_unlock(&rain_history_state.mutex);
#endif /* CONFIG_HISTORY_EXTERNAL_FLASH */
    
    return rainfall;
}

watering_error_t rain_history_save_to_nvs(void)
{
    if (!rain_history_state.initialized) {
        return WATERING_ERROR_NOT_INITIALIZED;
    }
    
#ifdef CONFIG_HISTORY_EXTERNAL_FLASH
    /* Flash storage handles persistence automatically */
    LOG_DBG("Rain history using external flash - NVS save not needed");
    return WATERING_SUCCESS;
#else
    watering_error_t result = WATERING_SUCCESS;
    
    k_mutex_lock(&rain_history_state.mutex, K_FOREVER);
    
    /* Save hourly data if we have any */
    if (rain_history_state.hourly_count > 0) {
        size_t hourly_data_size = rain_history_state.hourly_count * sizeof(rain_hourly_data_t);
        int ret = nvs_save_rain_hourly_data(rain_history_state.hourly_data,
                                           rain_history_state.hourly_count,
                                           hourly_data_size);
        if (ret < 0) {
            LOG_ERR("Failed to save hourly rain data to NVS: %d", ret);
            result = WATERING_ERROR_STORAGE;
        } else {
            LOG_INF("Saved %u hourly rain entries to NVS", rain_history_state.hourly_count);
        }
    }
    
    /* Save daily data if we have any */
    if (rain_history_state.daily_count > 0) {
        size_t daily_data_size = rain_history_state.daily_count * sizeof(rain_daily_data_t);
        int ret = nvs_save_rain_daily_data(rain_history_state.daily_data,
                                          rain_history_state.daily_count,
                                          daily_data_size);
        if (ret < 0) {
            LOG_ERR("Failed to save daily rain data to NVS: %d", ret);
            result = WATERING_ERROR_STORAGE;
        } else {
            LOG_INF("Saved %u daily rain entries to NVS", rain_history_state.daily_count);
        }
    }
    
    k_mutex_unlock(&rain_history_state.mutex);
    
    return result;
#endif /* CONFIG_HISTORY_EXTERNAL_FLASH */
}

watering_error_t rain_history_load_from_nvs(void)
{
    if (!rain_history_state.initialized) {
        return WATERING_ERROR_NOT_INITIALIZED;
    }
    
#ifdef CONFIG_HISTORY_EXTERNAL_FLASH
    /* Flash storage handles persistence automatically */
    LOG_DBG("Rain history using external flash - NVS load not needed");
    return WATERING_SUCCESS;
#else
    watering_error_t result = WATERING_SUCCESS;
    
    k_mutex_lock(&rain_history_state.mutex, K_FOREVER);
    
    /* Load hourly data */
    uint16_t loaded_hourly_count = 0;
    int ret = nvs_load_rain_hourly_data(rain_history_state.hourly_data,
                                       RAIN_HOURLY_ENTRIES,
                                       &loaded_hourly_count);
    if (ret >= 0) {
        rain_history_state.hourly_count = loaded_hourly_count;
        LOG_INF("Loaded %u hourly rain entries from NVS", loaded_hourly_count);
    } else if (ret != -ENOENT) {
        LOG_WRN("Failed to load hourly rain data from NVS: %d", ret);
        result = WATERING_ERROR_STORAGE;
    }
    
    /* Load daily data */
    uint16_t loaded_daily_count = 0;
    ret = nvs_load_rain_daily_data(rain_history_state.daily_data,
                                  RAIN_DAILY_ENTRIES,
                                  &loaded_daily_count);
    if (ret >= 0) {
        rain_history_state.daily_count = loaded_daily_count;
        LOG_INF("Loaded %u daily rain entries from NVS", loaded_daily_count);
    } else if (ret != -ENOENT) {
        LOG_WRN("Failed to load daily rain data from NVS: %d", ret);
        result = WATERING_ERROR_STORAGE;
    }
    
    /* Update write indices for circular buffers */
    if (rain_history_state.hourly_count > 0) {
        rain_history_state.hourly_write_index = rain_history_state.hourly_count % RAIN_HOURLY_ENTRIES;
    }
    
    if (rain_history_state.daily_count > 0) {
        rain_history_state.daily_write_index = rain_history_state.daily_count % RAIN_DAILY_ENTRIES;
    }
    
    k_mutex_unlock(&rain_history_state.mutex);
    
    return result;
#endif /* CONFIG_HISTORY_EXTERNAL_FLASH */
}

watering_error_t rain_history_get_stats(rain_history_stats_t *stats)
{
    if (!rain_history_state.initialized || !stats) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
#ifdef CONFIG_HISTORY_EXTERNAL_FLASH
    /* Get stats from flash */
    history_flash_stats_t flash_stats;
    int ret = history_flash_get_stats(&flash_stats);
    if (ret < 0) {
        LOG_ERR("Failed to get rain history stats from flash: %d", ret);
        return WATERING_ERROR_STORAGE;
    }
    
    stats->hourly_entries = flash_stats.rain_hourly.entry_count;
    stats->daily_entries = flash_stats.rain_daily.entry_count;
    stats->oldest_hourly = flash_stats.rain_hourly.oldest_timestamp;
    stats->newest_hourly = flash_stats.rain_hourly.newest_timestamp;
    stats->oldest_daily = flash_stats.rain_daily.oldest_timestamp;
    stats->newest_daily = flash_stats.rain_daily.newest_timestamp;
    stats->total_storage_bytes = flash_stats.rain_hourly.file_size_bytes + flash_stats.rain_daily.file_size_bytes;
    
    return WATERING_SUCCESS;
#else
    k_mutex_lock(&rain_history_state.mutex, K_FOREVER);
    
    stats->hourly_entries = rain_history_state.hourly_count;
    stats->daily_entries = rain_history_state.daily_count;
    
    /* Find oldest and newest timestamps */
    stats->oldest_hourly = UINT32_MAX;
    stats->newest_hourly = 0;
    for (int i = 0; i < rain_history_state.hourly_count; i++) {
        uint32_t epoch = rain_history_state.hourly_data[i].hour_epoch;
        if (epoch < stats->oldest_hourly) stats->oldest_hourly = epoch;
        if (epoch > stats->newest_hourly) stats->newest_hourly = epoch;
    }
    
    stats->oldest_daily = UINT32_MAX;
    stats->newest_daily = 0;
    for (int i = 0; i < rain_history_state.daily_count; i++) {
        uint32_t epoch = rain_history_state.daily_data[i].day_epoch;
        if (epoch < stats->oldest_daily) stats->oldest_daily = epoch;
        if (epoch > stats->newest_daily) stats->newest_daily = epoch;
    }
    
    stats->total_storage_bytes = 
        (rain_history_state.hourly_count * sizeof(rain_hourly_data_t)) +
        (rain_history_state.daily_count * sizeof(rain_daily_data_t));
    
    k_mutex_unlock(&rain_history_state.mutex);
    
    return WATERING_SUCCESS;
#endif /* CONFIG_HISTORY_EXTERNAL_FLASH */
}

watering_error_t rain_history_maintenance(void)
{
    return rain_history_periodic_maintenance();
}

void rain_history_debug_info(void)
{
    if (!rain_history_state.initialized) {
        printk("Rain history not initialized\n");
        return;
    }
    
    rain_history_stats_t stats;
    rain_history_get_stats(&stats);
    
    printk("=== Rain History Debug Info ===\n");
    printk("Initialized: Yes\n");
    printk("Hourly entries: %u/%u\n", stats.hourly_entries, RAIN_HOURLY_ENTRIES);
    printk("Daily entries: %u/%u\n", stats.daily_entries, RAIN_DAILY_ENTRIES);
    printk("Storage used: %u bytes\n", stats.total_storage_bytes);
    printk("Last 24h rainfall: %.2f mm\n", (double)rain_history_get_last_24h());
    printk("Today's rainfall: %.2f mm\n", (double)rain_history_get_today());
    printk("Current hour: %.2f mm\n", (double)rain_history_get_current_hour());
    printk("===============================\n");
}

watering_error_t rain_history_clear_all(void)
{
    if (!rain_history_state.initialized) {
        return WATERING_ERROR_NOT_INITIALIZED;
    }
    
#ifdef CONFIG_HISTORY_EXTERNAL_FLASH
    /* Clear flash storage */
    int ret = history_flash_clear(HISTORY_TYPE_RAIN_HOURLY);
    if (ret < 0) {
        LOG_ERR("Failed to clear rain hourly flash: %d", ret);
        return WATERING_ERROR_STORAGE;
    }
    
    ret = history_flash_clear(HISTORY_TYPE_RAIN_DAILY);
    if (ret < 0) {
        LOG_ERR("Failed to clear rain daily flash: %d", ret);
        return WATERING_ERROR_STORAGE;
    }
    
    /* Reset counters */
    k_mutex_lock(&rain_history_state.mutex, K_FOREVER);
    rain_history_state.hourly_count = 0;
    rain_history_state.daily_count = 0;
    rain_history_state.hourly_write_index = 0;
    rain_history_state.daily_write_index = 0;
    k_mutex_unlock(&rain_history_state.mutex);
    
    LOG_INF("All rain history data cleared from flash");
    return WATERING_SUCCESS;
#else
    k_mutex_lock(&rain_history_state.mutex, K_FOREVER);
    
    /* Clear in-memory data */
    memset(rain_history_state.hourly_data, 0, sizeof(rain_history_state.hourly_data));
    memset(rain_history_state.daily_data, 0, sizeof(rain_history_state.daily_data));
    
    rain_history_state.hourly_count = 0;
    rain_history_state.daily_count = 0;
    rain_history_state.hourly_write_index = 0;
    rain_history_state.daily_write_index = 0;
    
    k_mutex_unlock(&rain_history_state.mutex);
    
    /* Clear NVS data */
    int ret = nvs_clear_rain_history();
    if (ret < 0) {
        LOG_ERR("Failed to clear rain history from NVS: %d", ret);
        return WATERING_ERROR_STORAGE;
    }
    
    LOG_INF("All rain history data cleared");
    return WATERING_SUCCESS;
#endif /* CONFIG_HISTORY_EXTERNAL_FLASH */
}

watering_error_t rain_history_clear_hourly_older_than(uint32_t older_than_epoch)
{
    if (!rain_history_state.initialized) {
        return WATERING_ERROR_NOT_INITIALIZED;
    }
    
#ifdef CONFIG_HISTORY_EXTERNAL_FLASH
    /* Flash storage handles rotation automatically, just log */
    LOG_DBG("Rain hourly cleanup requested for epoch < %u (flash handles automatically)", older_than_epoch);
    return WATERING_SUCCESS;
#else
    k_mutex_lock(&rain_history_state.mutex, K_FOREVER);
    
    uint16_t removed_count = 0;
    
    /* Remove entries older than specified time */
    for (int i = 0; i < rain_history_state.hourly_count; i++) {
        if (rain_history_state.hourly_data[i].hour_epoch < older_than_epoch) {
            /* Shift remaining entries down */
            for (int j = i; j < rain_history_state.hourly_count - 1; j++) {
                rain_history_state.hourly_data[j] = rain_history_state.hourly_data[j + 1];
            }
            rain_history_state.hourly_count--;
            removed_count++;
            i--; /* Check the same index again since we shifted */
        }
    }
    
    /* Update write index */
    rain_history_state.hourly_write_index = rain_history_state.hourly_count % RAIN_HOURLY_ENTRIES;
    
    k_mutex_unlock(&rain_history_state.mutex);
    
    if (removed_count > 0) {
        LOG_INF("Removed %u hourly entries older than %u", removed_count, older_than_epoch);
        /* Save updated data to NVS */
        rain_history_save_to_nvs();
    }
    
    return WATERING_SUCCESS;
#endif /* CONFIG_HISTORY_EXTERNAL_FLASH */
}

watering_error_t rain_history_clear_daily_older_than(uint32_t older_than_epoch)
{
    if (!rain_history_state.initialized) {
        return WATERING_ERROR_NOT_INITIALIZED;
    }
    
#ifdef CONFIG_HISTORY_EXTERNAL_FLASH
    /* Flash storage handles rotation automatically, just log */
    LOG_DBG("Rain daily cleanup requested for epoch < %u (flash handles automatically)", older_than_epoch);
    return WATERING_SUCCESS;
#else
    k_mutex_lock(&rain_history_state.mutex, K_FOREVER);
    
    uint16_t removed_count = 0;
    
    /* Remove entries older than specified time */
    for (int i = 0; i < rain_history_state.daily_count; i++) {
        if (rain_history_state.daily_data[i].day_epoch < older_than_epoch) {
            /* Shift remaining entries down */
            for (int j = i; j < rain_history_state.daily_count - 1; j++) {
                rain_history_state.daily_data[j] = rain_history_state.daily_data[j + 1];
            }
            rain_history_state.daily_count--;
            removed_count++;
            i--; /* Check the same index again since we shifted */
        }
    }
    
    /* Update write index */
    rain_history_state.daily_write_index = rain_history_state.daily_count % RAIN_DAILY_ENTRIES;
    
    k_mutex_unlock(&rain_history_state.mutex);
    
    if (removed_count > 0) {
        LOG_INF("Removed %u daily entries older than %u", removed_count, older_than_epoch);
        /* Save updated data to NVS */
        rain_history_save_to_nvs();
    }
    
    return WATERING_SUCCESS;
#endif /* CONFIG_HISTORY_EXTERNAL_FLASH */
}

watering_error_t rain_history_get_storage_usage(uint32_t *used_bytes, uint32_t *total_bytes)
{
    if (!used_bytes || !total_bytes) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    size_t nvs_used, nvs_total;
    int ret = nvs_get_rain_storage_usage(&nvs_used, &nvs_total);
    if (ret < 0) {
        return WATERING_ERROR_STORAGE;
    }
    
    *used_bytes = (uint32_t)nvs_used;
    *total_bytes = (uint32_t)nvs_total;
    
    return WATERING_SUCCESS;
}

watering_error_t rain_history_validate_data(void)
{
    if (!rain_history_state.initialized) {
        return WATERING_ERROR_NOT_INITIALIZED;
    }
    
#ifdef CONFIG_HISTORY_EXTERNAL_FLASH
    /* Flash storage validates on read, just return success */
    LOG_DBG("Rain history using external flash - validation handled by flash layer");
    return WATERING_SUCCESS;
#else
    k_mutex_lock(&rain_history_state.mutex, K_FOREVER);
    
    uint16_t validation_errors = 0;
    uint32_t now_unix = timezone_get_unix_utc();
    
    /* Validate hourly data */
    for (int i = 0; i < rain_history_state.hourly_count; i++) {
        rain_hourly_data_t *entry = &rain_history_state.hourly_data[i];
        
        /* Check for reasonable timestamp */
        if (entry->hour_epoch == 0 || (now_unix != 0U && entry->hour_epoch > (now_unix + 86400U))) {
            LOG_WRN("Invalid hourly timestamp at index %d: %u", i, entry->hour_epoch);
            validation_errors++;
        }
        
        /* Check for reasonable rainfall values (max 1000mm/hour) */
        if (entry->rainfall_mm_x100 > 100000) {
            LOG_WRN("Excessive hourly rainfall at index %d: %u", i, entry->rainfall_mm_x100);
            validation_errors++;
        }
        
        /* Check data quality */
        if (entry->data_quality > 100) {
            LOG_WRN("Invalid data quality at hourly index %d: %u", i, entry->data_quality);
            validation_errors++;
        }
    }
    
    /* Validate daily data */
    for (int i = 0; i < rain_history_state.daily_count; i++) {
        rain_daily_data_t *entry = &rain_history_state.daily_data[i];
        
        /* Check for reasonable timestamp */
        if (entry->day_epoch == 0 || (now_unix != 0U && entry->day_epoch > (now_unix + 86400U))) {
            LOG_WRN("Invalid daily timestamp at index %d: %u", i, entry->day_epoch);
            validation_errors++;
        }
        
        /* Check for reasonable rainfall values (max 2000mm/day) */
        if (entry->total_rainfall_mm_x100 > 200000) {
            LOG_WRN("Excessive daily rainfall at index %d: %u", i, entry->total_rainfall_mm_x100);
            validation_errors++;
        }
        
        /* Check active hours */
        if (entry->active_hours > 24) {
            LOG_WRN("Invalid active hours at daily index %d: %u", i, entry->active_hours);
            validation_errors++;
        }
        
        /* Check data completeness */
        if (entry->data_completeness > 100) {
            LOG_WRN("Invalid data completeness at daily index %d: %u", i, entry->data_completeness);
            validation_errors++;
        }
    }
    
    k_mutex_unlock(&rain_history_state.mutex);
    
    if (validation_errors > 0) {
        LOG_ERR("Rain history validation found %u errors", validation_errors);
        return WATERING_ERROR_INVALID_DATA;
    }
    
    LOG_DBG("Rain history validation passed");
    return WATERING_SUCCESS;
#endif /* CONFIG_HISTORY_EXTERNAL_FLASH */
}

watering_error_t rain_history_export_csv(uint32_t start_time,
                                        uint32_t end_time,
                                        char *buffer,
                                        uint16_t buffer_size,
                                        uint16_t *bytes_written)
{
    if (!buffer || !bytes_written || buffer_size < 100) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    if (!rain_history_state.initialized) {
        return WATERING_ERROR_NOT_INITIALIZED;
    }
    
    *bytes_written = 0;
    
    /* Write CSV header */
    int written = snprintf(buffer, buffer_size,
                          "timestamp,type,rainfall_mm,pulse_count,data_quality\n");
    if (written >= buffer_size) {
        return WATERING_ERROR_BUFFER_FULL;
    }
    
    *bytes_written = written;
    
#ifdef CONFIG_HISTORY_EXTERNAL_FLASH
    history_flash_stats_t flash_stats;
    int stats_ret = history_flash_get_stats(&flash_stats);
    if (stats_ret < 0) {
        LOG_ERR("Failed to get rain history stats from flash: %d", stats_ret);
        return WATERING_ERROR_STORAGE;
    }

    /* Read hourly data from flash in small chunks */
    {
        uint16_t total_entries = flash_stats.rain_hourly.entry_count;
        history_rain_hourly_t hourly_chunk[32];
        uint16_t offset = 0;

        while (offset < total_entries) {
            uint16_t remaining = (uint16_t)(total_entries - offset);
            uint16_t chunk_size = remaining > (uint16_t)(sizeof(hourly_chunk) / sizeof(hourly_chunk[0]))
                                      ? (uint16_t)(sizeof(hourly_chunk) / sizeof(hourly_chunk[0]))
                                      : remaining;
            uint16_t read_count = 0;

            int ret = history_flash_read_rain_hourly(offset, hourly_chunk, chunk_size, &read_count);
            if (ret < 0) {
                LOG_ERR("Failed to read rain hourly from flash: %d", ret);
                return WATERING_ERROR_STORAGE;
            }
            if (read_count == 0U) {
                break;
            }

            for (uint16_t i = 0; i < read_count; i++) {
                if (hourly_chunk[i].hour_epoch < start_time || hourly_chunk[i].hour_epoch > end_time) {
                    continue;
                }

                written = snprintf(buffer + *bytes_written, buffer_size - *bytes_written,
                                   "%u,hourly,%.2f,%u,%u\n",
                                   hourly_chunk[i].hour_epoch,
                                   (double)(hourly_chunk[i].rainfall_mm_x100 / 100.0f),
                                   hourly_chunk[i].pulse_count,
                                   hourly_chunk[i].data_quality);

                if (*bytes_written + written >= buffer_size) {
                    return WATERING_ERROR_BUFFER_FULL;
                }

                *bytes_written += written;
            }

            offset = (uint16_t)(offset + read_count);
        }
    }

    /* Read daily data from flash in small chunks */
    {
        uint16_t total_entries = flash_stats.rain_daily.entry_count;
        history_rain_daily_t daily_chunk[32];
        uint16_t offset = 0;

        while (offset < total_entries) {
            uint16_t remaining = (uint16_t)(total_entries - offset);
            uint16_t chunk_size = remaining > (uint16_t)(sizeof(daily_chunk) / sizeof(daily_chunk[0]))
                                      ? (uint16_t)(sizeof(daily_chunk) / sizeof(daily_chunk[0]))
                                      : remaining;
            uint16_t read_count = 0;

            int ret = history_flash_read_rain_daily(offset, daily_chunk, chunk_size, &read_count);
            if (ret < 0) {
                LOG_ERR("Failed to read rain daily from flash: %d", ret);
                return WATERING_ERROR_STORAGE;
            }
            if (read_count == 0U) {
                break;
            }

            for (uint16_t i = 0; i < read_count; i++) {
                if (daily_chunk[i].day_epoch < start_time || daily_chunk[i].day_epoch > end_time) {
                    continue;
                }

                written = snprintf(buffer + *bytes_written, buffer_size - *bytes_written,
                                   "%u,daily,%.2f,%u,%u\n",
                                   daily_chunk[i].day_epoch,
                                   (double)(daily_chunk[i].total_rainfall_mm_x100 / 100.0f),
                                   daily_chunk[i].active_hours,
                                   daily_chunk[i].data_completeness);

                if (*bytes_written + written >= buffer_size) {
                    return WATERING_ERROR_BUFFER_FULL;
                }

                *bytes_written += written;
            }

            offset = (uint16_t)(offset + read_count);
        }
    }
#else
    k_mutex_lock(&rain_history_state.mutex, K_FOREVER);
    
    /* Export hourly data */
    for (int i = 0; i < rain_history_state.hourly_count; i++) {
        rain_hourly_data_t *entry = &rain_history_state.hourly_data[i];
        
        if (entry->hour_epoch >= start_time && entry->hour_epoch <= end_time) {
            written = snprintf(buffer + *bytes_written, buffer_size - *bytes_written,
                              "%u,hourly,%.2f,%u,%u\n",
                              entry->hour_epoch,
                              (double)(entry->rainfall_mm_x100 / 100.0f),
                              entry->pulse_count,
                              entry->data_quality);
            
            if (*bytes_written + written >= buffer_size) {
                k_mutex_unlock(&rain_history_state.mutex);
                return WATERING_ERROR_BUFFER_FULL;
            }
            
            *bytes_written += written;
        }
    }
    
    /* Export daily data */
    for (int i = 0; i < rain_history_state.daily_count; i++) {
        rain_daily_data_t *entry = &rain_history_state.daily_data[i];
        
        if (entry->day_epoch >= start_time && entry->day_epoch <= end_time) {
            written = snprintf(buffer + *bytes_written, buffer_size - *bytes_written,
                              "%u,daily,%.2f,%u,%u\n",
                              entry->day_epoch,
                              (double)(entry->total_rainfall_mm_x100 / 100.0f),
                              entry->active_hours,
                              entry->data_completeness);
            
            if (*bytes_written + written >= buffer_size) {
                k_mutex_unlock(&rain_history_state.mutex);
                return WATERING_ERROR_BUFFER_FULL;
            }
            
            *bytes_written += written;
        }
    }
    
    k_mutex_unlock(&rain_history_state.mutex);
#endif /* CONFIG_HISTORY_EXTERNAL_FLASH */
    
    LOG_DBG("Exported rain history CSV: %u bytes", *bytes_written);
    return WATERING_SUCCESS;
}

watering_error_t rain_history_monitor_storage(void)
{
    if (!rain_history_state.initialized) {
        return WATERING_ERROR_NOT_INITIALIZED;
    }
    
    uint32_t used_bytes, total_bytes;
    watering_error_t ret = rain_history_get_storage_usage(&used_bytes, &total_bytes);
    if (ret != WATERING_SUCCESS) {
        return ret;
    }
    
    uint32_t usage_percent = (used_bytes * 100) / total_bytes;
    
    LOG_DBG("Rain history storage usage: %u/%u bytes (%u%%)", 
            used_bytes, total_bytes, usage_percent);
    
    /* Trigger cleanup if storage usage exceeds 80% */
    if (usage_percent > 80) {
        LOG_WRN("Rain history storage usage high (%u%%), triggering cleanup", usage_percent);
        
        /* Remove old hourly data (older than 25 days to keep some margin) */
        uint32_t current_time = timezone_get_unix_utc();
        if (current_time == 0U) {
            LOG_WRN("Skipping rain history cleanup: RTC time not available");
            return WATERING_SUCCESS;
        }
        uint32_t cleanup_threshold = current_time - (25 * 24 * 3600); /* 25 days ago */
        
        ret = rain_history_clear_hourly_older_than(cleanup_threshold);
        if (ret != WATERING_SUCCESS) {
            LOG_ERR("Failed to cleanup old hourly data: %d", ret);
            return ret;
        }
        
        /* Remove old daily data (older than 4 years to keep some margin) */
        cleanup_threshold = current_time - (4 * 365 * 24 * 3600); /* 4 years ago */
        
        ret = rain_history_clear_daily_older_than(cleanup_threshold);
        if (ret != WATERING_SUCCESS) {
            LOG_ERR("Failed to cleanup old daily data: %d", ret);
            return ret;
        }
        
        /* Save cleaned data */
        ret = rain_history_save_to_nvs();
        if (ret != WATERING_SUCCESS) {
            LOG_ERR("Failed to save cleaned rain history: %d", ret);
            return ret;
        }
        
        LOG_INF("Rain history cleanup completed");
    }
    
    return WATERING_SUCCESS;
}

watering_error_t rain_history_handle_nvs_error(int nvs_error)
{
    switch (nvs_error) {
        case 0:
            return WATERING_SUCCESS;
            
        case -ENOENT:
            LOG_DBG("Rain history data not found in NVS (first run)");
            return WATERING_SUCCESS;
            
        case -ENOMEM:
            LOG_ERR("Insufficient memory for rain history NVS operation");
            return WATERING_ERROR_NO_MEMORY;
            
            
        case -EINVAL:
            LOG_ERR("Invalid parameters for rain history NVS operation");
            return WATERING_ERROR_INVALID_PARAM;
            
        case -EILSEQ:
            LOG_ERR("Rain history data corruption detected in NVS");
            /* Clear corrupted data */
            nvs_clear_rain_history();
            return WATERING_ERROR_DATA_CORRUPT;
            
        case -ENODATA:
            LOG_WRN("Incomplete rain history data in NVS");
            return WATERING_ERROR_INVALID_DATA;
            
        default:
            LOG_ERR("Unknown NVS error for rain history: %d", nvs_error);
            return WATERING_ERROR_STORAGE;
    }
}

watering_error_t rain_history_periodic_maintenance(void)
{
    if (!rain_history_state.initialized) {
        return WATERING_ERROR_NOT_INITIALIZED;
    }
    
    watering_error_t ret;
    
    /* Perform daily aggregation */
    ret = rain_history_aggregate_daily();
    if (ret != WATERING_SUCCESS) {
        LOG_ERR("Failed to aggregate daily rain data: %d", ret);
        return ret;
    }
    
    /* Monitor and cleanup storage if needed */
    ret = rain_history_monitor_storage();
    if (ret != WATERING_SUCCESS) {
        LOG_ERR("Failed to monitor rain history storage: %d", ret);
        return ret;
    }
    
    /* Validate data integrity */
    ret = rain_history_validate_data();
    if (ret != WATERING_SUCCESS) {
        LOG_ERR("Rain history data validation failed: %d", ret);
        /* Don't return error - just log it */
    }
    
    /* Save to NVS periodically */
    ret = rain_history_save_to_nvs();
    if (ret != WATERING_SUCCESS) {
        LOG_ERR("Failed to save rain history to NVS: %d", ret);
        return ret;
    }
    
    LOG_DBG("Rain history periodic maintenance completed");
    return WATERING_SUCCESS;
}
