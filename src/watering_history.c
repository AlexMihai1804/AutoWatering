#include "watering_history.h"
#include "watering_internal.h"
#include "watering.h"
#include "nvs_config.h"
#include "rtc.h"
#include "timezone.h"               /* Add timezone support for local time */
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <time.h>

/**
 * @file watering_history.c
 * @brief AutoWatering – History subsystem implementation
 * 
 * Backend NVS cu ring-buffer, GC, și compresie Heatshrink
 * pentru statistici lunare. Sistem optimizat pentru nRF52840
 * cu 144 KB NVS dedicat.
 */

LOG_MODULE_REGISTER(watering_history, CONFIG_LOG_DEFAULT_LEVEL);

// Storage keys pentru NVS
#define NVS_KEY_DETAILED_BASE           2000
#define NVS_KEY_DAILY_BASE              3000
#define NVS_KEY_MONTHLY_BASE            4000
#define NVS_KEY_ANNUAL_BASE             5000
#define NVS_KEY_ROTATION_INFO           6000
#define NVS_KEY_HISTORY_SETTINGS        6001
#define NVS_KEY_INSIGHTS_CACHE          6002

// Ring-buffer pentru fiecare tip de date
static history_event_t detailed_events[MAX_CHANNELS][DETAILED_EVENTS_PER_CHANNEL];
static daily_stats_t daily_stats[DAILY_STATS_DAYS];
static monthly_stats_raw_t monthly_stats[MONTHLY_STATS_MONTHS];
static annual_stats_t annual_stats[ANNUAL_STATS_YEARS];

// Management structures
static history_rotation_t rotation_info = {0};
static history_settings_t current_settings = {
    .detailed_cnt = DETAILED_EVENTS_PER_CHANNEL,
    .daily_days = DAILY_STATS_DAYS,
    .monthly_months = MONTHLY_STATS_MONTHS,
    .annual_years = ANNUAL_STATS_YEARS
};
static insights_t current_insights = {0};

// Thread-safety
K_MUTEX_DEFINE(history_mutex);

// GC thread
#define GC_THREAD_STACK_SIZE 2048
#define GC_THREAD_PRIORITY 5
K_THREAD_STACK_DEFINE(gc_thread_stack, GC_THREAD_STACK_SIZE);
static struct k_thread gc_thread_data;
static bool gc_thread_active = false;

// Internal helper functions
static watering_error_t save_rotation_info(void);
static watering_error_t load_rotation_info(void);
static watering_error_t gc_check_trigger(void);
static void gc_thread_main(void *a, void *b, void *c);
static uint32_t get_current_timestamp(void);
static uint16_t get_current_day_of_year(void);
static uint16_t get_current_year(void);
static uint8_t get_current_month(void);
static uint32_t calculate_storage_usage(void);

/**
 * @brief Initialize History subsystem
 */
watering_error_t watering_history_init(void) {
    // Use timeout instead of K_FOREVER to prevent system freeze
    if (k_mutex_lock(&history_mutex, K_MSEC(500)) != 0) {
        printk("History init failed: mutex timeout\n");
        return WATERING_ERROR_TIMEOUT;
    }
    
    // Initialize arrays
    memset(detailed_events, 0, sizeof(detailed_events));
    memset(daily_stats, 0, sizeof(daily_stats));
    memset(monthly_stats, 0, sizeof(monthly_stats));
    memset(annual_stats, 0, sizeof(annual_stats));
    
    // Load settings
    if (nvs_config_read(NVS_KEY_HISTORY_SETTINGS, &current_settings, sizeof(current_settings)) < 0) {
        LOG_INF("Using default history settings");
    }
    
    // Load rotation info from NVS
    watering_error_t err = load_rotation_info();
    if (err != WATERING_SUCCESS) {
        LOG_WRN("Failed to load rotation info, using defaults");
        memset(&rotation_info, 0, sizeof(rotation_info));
    }
    
    // Load existing data from NVS
    for (uint8_t ch = 0; ch < MAX_CHANNELS; ch++) {
        for (uint8_t i = 0; i < current_settings.detailed_cnt; i++) {
            uint16_t key = NVS_KEY_DETAILED_BASE + (ch * 100) + i;
            if (nvs_config_read(key, &detailed_events[ch][i], sizeof(history_event_t)) < 0) {
                // Not an error - might be empty
            }
        }
    }
    
    for (uint8_t i = 0; i < current_settings.daily_days; i++) {
        uint16_t key = NVS_KEY_DAILY_BASE + i;
        if (nvs_config_read(key, &daily_stats[i], sizeof(daily_stats_t)) < 0) {
            // Not an error - might be empty
        }
    }
    
    for (uint8_t i = 0; i < current_settings.monthly_months; i++) {
        uint16_t key = NVS_KEY_MONTHLY_BASE + i;
        if (nvs_config_read(key, &monthly_stats[i], sizeof(monthly_stats_raw_t)) < 0) {
            // Not an error - might be empty
        }
    }
    
    for (uint8_t i = 0; i < current_settings.annual_years; i++) {
        uint16_t key = NVS_KEY_ANNUAL_BASE + i;
        if (nvs_config_read(key, &annual_stats[i], sizeof(annual_stats_t)) < 0) {
            // Not an error - might be empty
        }
    }
    
    // Start GC thread
    if (!gc_thread_active) {
        k_thread_create(&gc_thread_data, gc_thread_stack,
                       K_THREAD_STACK_SIZEOF(gc_thread_stack),
                       gc_thread_main, NULL, NULL, NULL,
                       GC_THREAD_PRIORITY, 0, K_NO_WAIT);
        gc_thread_active = true;
    }
    
    k_mutex_unlock(&history_mutex);
    
    LOG_INF("History subsystem initialized successfully");
    return WATERING_SUCCESS;
}

/**
 * @brief Deinitialize History subsystem
 */
watering_error_t watering_history_deinit(void) {
    // Use timeout instead of K_FOREVER to prevent system freeze
    if (k_mutex_lock(&history_mutex, K_MSEC(100)) != 0) {
        printk("History deinit failed: mutex timeout\n");
        return WATERING_ERROR_TIMEOUT;
    }
    
    // Save current rotation info
    save_rotation_info();
    
    // Stop GC thread
    if (gc_thread_active) {
        k_thread_abort(&gc_thread_data);
        gc_thread_active = false;
    }
    
    k_mutex_unlock(&history_mutex);
    
    LOG_INF("History subsystem deinitialized");
    return WATERING_SUCCESS;
}

/**
 * @brief Add new event to history
 */
watering_error_t watering_history_add_event(const history_event_t *event) {
    if (!event) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    // Use timeout instead of K_FOREVER to prevent system freeze
    if (k_mutex_lock(&history_mutex, K_MSEC(100)) != 0) {
        // If we can't get the mutex quickly, drop the event rather than hanging
        printk("History add event failed: mutex timeout\n");
        return WATERING_ERROR_TIMEOUT;
    }
    
    // Extract channel from event (assuming it's encoded in reserved field)
    uint8_t channel = event->reserved[0];
    if (channel >= MAX_CHANNELS) {
        k_mutex_unlock(&history_mutex);
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    // Find next available slot in ring buffer
    uint8_t next_slot = 0;
    for (uint8_t i = 0; i < current_settings.detailed_cnt; i++) {
        if (detailed_events[channel][i].dt_delta == 0) {
            next_slot = i;
            break;
        }
        if (i == current_settings.detailed_cnt - 1) {
            // Ring buffer is full, overwrite oldest (slot 0)
            next_slot = 0;
            // Shift all events down
            for (uint8_t j = 0; j < current_settings.detailed_cnt - 1; j++) {
                detailed_events[channel][j] = detailed_events[channel][j + 1];
            }
            next_slot = current_settings.detailed_cnt - 1;
        }
    }
    
    // Add new event
    detailed_events[channel][next_slot] = *event;
    
    // Save to NVS
    uint16_t key = NVS_KEY_DETAILED_BASE + (channel * 100) + next_slot;
    if (nvs_config_write(key, event, sizeof(history_event_t)) < 0) {
        LOG_ERR("Failed to save event to NVS");
        k_mutex_unlock(&history_mutex);
        return WATERING_ERROR_STORAGE;
    }
    
    // Check if GC is needed
    gc_check_trigger();
    
    k_mutex_unlock(&history_mutex);
    return WATERING_SUCCESS;
}

/**
 * @brief Record task start
 */
watering_error_t watering_history_record_task_start(uint8_t channel_id, 
                                                   watering_mode_t mode,
                                                   uint16_t target_value,
                                                   watering_trigger_type_t trigger) {
    if (channel_id >= MAX_CHANNELS) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    history_event_t event = {0};
    event.dt_delta = 1; // Will be calculated properly in actual implementation
    event.flags.mode = (mode == WATERING_BY_DURATION) ? 1 : 0;
    event.flags.trigger = trigger;
    event.flags.success = 0; // OK for now
    event.flags.err = 0;
    event.target_ml = target_value;
    event.actual_ml = 0;
    event.avg_flow_ml_s = 0;
    event.reserved[0] = channel_id; // Store channel in reserved field
    
    return watering_history_add_event(&event);
}

/**
 * @brief Record task completion
 */
watering_error_t watering_history_record_task_complete(uint8_t channel_id,
                                                      uint16_t actual_value,
                                                      uint16_t total_volume_ml,
                                                      watering_success_status_t status) {
    if (channel_id >= MAX_CHANNELS) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    history_event_t event = {0};
    event.dt_delta = 1; // Will be calculated properly
    event.flags.success = status;
    event.actual_ml = actual_value;
    event.reserved[0] = channel_id;
    
    return watering_history_add_event(&event);
}

/**
 * @brief Record task error
 */
watering_error_t watering_history_record_task_error(uint8_t channel_id,
                                                   uint8_t error_code) {
    if (channel_id >= MAX_CHANNELS) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    history_event_t event = {0};
    event.dt_delta = 1;
    event.flags.success = 2; // failed
    event.flags.err = error_code;
    event.reserved[0] = channel_id;
    
    return watering_history_add_event(&event);
}

/**
 * @brief TLV encoding functions
 */
int tlv_encode_uint8(uint8_t *buffer, uint8_t type, uint8_t value) {
    if (!buffer) return -1;
    
    tlv_t *tlv = (tlv_t *)buffer;
    tlv->type = type;
    tlv->len = 1;
    tlv->value[0] = value;
    
    return 3; // type + len + value
}

int tlv_encode_uint16(uint8_t *buffer, uint8_t type, uint16_t value) {
    if (!buffer) return -1;
    
    tlv_t *tlv = (tlv_t *)buffer;
    tlv->type = type;
    tlv->len = 2;
    tlv->value[0] = value & 0xFF;
    tlv->value[1] = (value >> 8) & 0xFF;
    
    return 4; // type + len + 2 bytes value
}

int tlv_encode_uint32(uint8_t *buffer, uint8_t type, uint32_t value) {
    if (!buffer) return -1;
    
    tlv_t *tlv = (tlv_t *)buffer;
    tlv->type = type;
    tlv->len = 4;
    tlv->value[0] = value & 0xFF;
    tlv->value[1] = (value >> 8) & 0xFF;
    tlv->value[2] = (value >> 16) & 0xFF;
    tlv->value[3] = (value >> 24) & 0xFF;
    
    return 6; // type + len + 4 bytes value
}

int tlv_decode_uint8(const uint8_t *buffer, uint8_t type, uint8_t *value) {
    if (!buffer || !value) return -1;
    
    const tlv_t *tlv = (const tlv_t *)buffer;
    if (tlv->type != type || tlv->len != 1) return -1;
    
    *value = tlv->value[0];
    return 3;
}

int tlv_decode_uint16(const uint8_t *buffer, uint8_t type, uint16_t *value) {
    if (!buffer || !value) return -1;
    
    const tlv_t *tlv = (const tlv_t *)buffer;
    if (tlv->type != type || tlv->len != 2) return -1;
    
    *value = tlv->value[0] | (tlv->value[1] << 8);
    return 4;
}

int tlv_decode_uint32(const uint8_t *buffer, uint8_t type, uint32_t *value) {
    if (!buffer || !value) return -1;
    
    const tlv_t *tlv = (const tlv_t *)buffer;
    if (tlv->type != type || tlv->len != 4) return -1;
    
    *value = tlv->value[0] | (tlv->value[1] << 8) | 
             (tlv->value[2] << 16) | (tlv->value[3] << 24);
    return 6;
}

/**
 * @brief History control handler for BLE
 */
watering_error_t history_ctrl_handler(const uint8_t *data, uint16_t len) {
    if (!data || len < 1) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    uint8_t opcode = data[0];
    const uint8_t *tlv_data = data + 1;
    uint16_t tlv_len = len - 1;
    
    switch (opcode) {
        case HC_QUERY_RANGE: {
            uint8_t channel_id = 0;
            uint32_t start_epoch = 0, end_epoch = 0;
            
            // Parse TLV parameters
            const uint8_t *ptr = tlv_data;
            while (ptr < tlv_data + tlv_len) {
                const tlv_t *tlv = (const tlv_t *)ptr;
                
                switch (tlv->type) {
                    case HT_CHANNEL_ID:
                        if (tlv->len == 1) channel_id = tlv->value[0];
                        break;
                    case HT_RANGE_START:
                        if (tlv->len == 4) {
                            start_epoch = tlv->value[0] | (tlv->value[1] << 8) |
                                         (tlv->value[2] << 16) | (tlv->value[3] << 24);
                        }
                        break;
                    case HT_RANGE_END:
                        if (tlv->len == 4) {
                            end_epoch = tlv->value[0] | (tlv->value[1] << 8) |
                                       (tlv->value[2] << 16) | (tlv->value[3] << 24);
                        }
                        break;
                }
                
                ptr += 2 + tlv->len;
            }
            
            LOG_INF("Query range: ch=%d, start=%u, end=%u", channel_id, start_epoch, end_epoch);
            // Implementation would query and send data via history_data_send_frame
            break;
        }
        
        case HC_QUERY_PAGE: {
            uint8_t channel_id = 0;
            uint16_t page_index = 0;
            
            // Parse TLV parameters (similar to above)
            LOG_INF("Query page: ch=%d, page=%d", channel_id, page_index);
            break;
        }
        
        case HC_EXPORT_START: {
            uint32_t before_epoch = 0;
            // Parse and start export
            LOG_INF("Export start: before=%u", before_epoch);
            break;
        }
        
        case HC_EXPORT_ACK: {
            uint16_t seq = 0;
            // Parse ACK and continue export
            LOG_INF("Export ACK: seq=%d", seq);
            break;
        }
        
        case HC_EXPORT_FINISH: {
            LOG_INF("Export finish");
            break;
        }
        
        case HC_RESET_HISTORY: {
            uint8_t channel_id = 0xFF; // Default: toate canalele
            
            // Parse TLV parameters
            const uint8_t *ptr = tlv_data;
            while (ptr < tlv_data + tlv_len) {
                const tlv_t *tlv = (const tlv_t *)ptr;
                
                if (tlv->type == HT_CHANNEL_ID && tlv->len == 1) {
                    channel_id = tlv->value[0];
                }
                
                ptr += 2 + tlv->len;
            }
            
            if (channel_id == 0xFF) {
                LOG_INF("Reset history: all channels");
                return watering_history_reset_all_history();
            } else {
                LOG_INF("Reset history: channel %u", channel_id);
                return watering_history_reset_channel_history(channel_id);
            }
        }
        
        case HC_RESET_CHANNEL: {
            uint8_t channel_id = 0;
            
            // Parse TLV parameters pentru channel_id
            const uint8_t *ptr = tlv_data;
            while (ptr < tlv_data + tlv_len) {
                const tlv_t *tlv = (const tlv_t *)ptr;
                
                if (tlv->type == HT_CHANNEL_ID && tlv->len == 1) {
                    channel_id = tlv->value[0];
                    break;
                }
                
                ptr += 2 + tlv->len;
            }
            
            LOG_INF("Reset channel config: channel %u", channel_id);
            return watering_history_reset_channel_config(channel_id);
        }
        
        case HC_RESET_ALL: {
            uint8_t channel_id = 0;
            
            // Parse TLV parameters pentru channel_id
            const uint8_t *ptr = tlv_data;
            while (ptr < tlv_data + tlv_len) {
                const tlv_t *tlv = (const tlv_t *)ptr;
                
                if (tlv->type == HT_CHANNEL_ID && tlv->len == 1) {
                    channel_id = tlv->value[0];
                    break;
                }
                
                ptr += 2 + tlv->len;
            }
            
            LOG_INF("Reset complete: channel %u", channel_id);
            return watering_history_reset_channel_complete(channel_id);
        }
        
        case HC_FACTORY_RESET: {
            LOG_WRN("Factory reset requested - clearing all data!");
            return watering_history_factory_reset();
        }
        
        default:
            LOG_WRN("Unknown history control opcode: 0x%02x", opcode);
            return WATERING_ERROR_INVALID_PARAM;
    }
    
    return WATERING_SUCCESS;
}

/**
 * @brief Send history data frame via BLE
 */
watering_error_t history_data_send_frame(uint16_t seq, const uint8_t *payload, uint16_t len) {
    if (!payload && len > 0) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    // Create frame header
    history_frame_t frame = {
        .seq = seq,
        .len = len
    };
    
    // In actual implementation, this would be sent via BLE notification
    LOG_INF("Sending history frame: seq=%d, len=%d", seq, len);
    
    return WATERING_SUCCESS;
}

/**
 * @brief Update insights data
 */
watering_error_t history_insights_update(const insights_t *insights) {
    if (!insights) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    k_mutex_lock(&history_mutex, K_MSEC(100));
    
    current_insights = *insights;
    
    // Save to NVS
    if (nvs_config_write(NVS_KEY_INSIGHTS_CACHE, &current_insights, sizeof(current_insights)) < 0) {
        LOG_ERR("Failed to save insights to NVS");
        k_mutex_unlock(&history_mutex);
        return WATERING_ERROR_STORAGE;
    }
    
    k_mutex_unlock(&history_mutex);
    
    LOG_INF("Insights updated successfully");
    return WATERING_SUCCESS;
}

/**
 * @brief Get history settings
 */
watering_error_t history_settings_get(history_settings_t *settings) {
    if (!settings) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    k_mutex_lock(&history_mutex, K_MSEC(100));
    *settings = current_settings;
    k_mutex_unlock(&history_mutex);
    
    return WATERING_SUCCESS;
}

/**
 * @brief Set history settings
 */
watering_error_t history_settings_set(const history_settings_t *settings) {
    if (!settings) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    k_mutex_lock(&history_mutex, K_MSEC(100));
    
    current_settings = *settings;
    
    // Save to NVS
    if (nvs_config_write(NVS_KEY_HISTORY_SETTINGS, &current_settings, sizeof(current_settings)) < 0) {
        LOG_ERR("Failed to save history settings to NVS");
        k_mutex_unlock(&history_mutex);
        return WATERING_ERROR_STORAGE;
    }
    
    k_mutex_unlock(&history_mutex);
    
    LOG_INF("History settings updated successfully");
    return WATERING_SUCCESS;
}

/**
 * @brief GC thread main function
 */
static void gc_thread_main(void *a, void *b, void *c) {
    ARG_UNUSED(a);
    ARG_UNUSED(b);
    ARG_UNUSED(c);
    
    while (true) {
        k_sleep(K_MINUTES(30)); // Check every 30 minutes
        
        uint32_t usage = calculate_storage_usage();
        uint32_t threshold_high = (TOTAL_HISTORY_STORAGE_KB * 1024 * GC_HIGH_WATERMARK_PCT) / 100;
        
        if (usage > threshold_high) {
            LOG_INF("GC triggered: usage=%u KB, threshold=%u KB", usage/1024, threshold_high/1024);
            watering_history_gc_trigger();
        }
    }
}

/**
 * @brief Check if GC should be triggered
 */
static watering_error_t gc_check_trigger(void) {
    uint32_t usage = calculate_storage_usage();
    uint32_t threshold_high = (TOTAL_HISTORY_STORAGE_KB * 1024 * GC_HIGH_WATERMARK_PCT) / 100;
    
    if (usage > threshold_high) {
        LOG_INF("GC check triggered: usage=%u KB", usage/1024);
        return watering_history_gc_trigger();
    }
    
    return WATERING_SUCCESS;
}

/**
 * @brief Calculate current storage usage
 */
static uint32_t calculate_storage_usage(void) {
    uint32_t usage = 0;
    
    // Count detailed events
    for (uint8_t ch = 0; ch < MAX_CHANNELS; ch++) {
        for (uint8_t i = 0; i < current_settings.detailed_cnt; i++) {
            if (detailed_events[ch][i].dt_delta != 0) {
                usage += sizeof(history_event_t);
            }
        }
    }
    
    // Count other structures
    usage += sizeof(daily_stats);
    usage += sizeof(monthly_stats);
    usage += sizeof(annual_stats);
    
    return usage;
}

/**
 * @brief Trigger garbage collection
 */
watering_error_t watering_history_gc_trigger(void) {
    LOG_INF("GC started");
    
    // Implementation would perform garbage collection
    // For now, just log the action
    
    LOG_INF("GC completed");
    return WATERING_SUCCESS;
}

/**
 * @brief Helper functions for time
 */
static uint32_t get_current_timestamp(void) {
    // In actual implementation, this would get real timestamp
    return k_uptime_get_32() / 1000;
}

static uint16_t get_current_year(void) {
    rtc_datetime_t datetime;
    if (rtc_datetime_get(&datetime) == 0) {
        /* TIMEZONE FIX: Convert UTC to local time for user-facing date/time */
        uint32_t utc_timestamp = timezone_rtc_to_unix_utc(&datetime);
        rtc_datetime_t local_datetime;
        if (timezone_unix_to_rtc_local(utc_timestamp, &local_datetime) == 0) {
            return local_datetime.year;
        }
        /* Fallback to UTC if timezone conversion fails */
        return datetime.year;
    }
    return 2025; // Fallback if RTC unavailable
}

static uint8_t get_current_month(void) {
    rtc_datetime_t datetime;
    if (rtc_datetime_get(&datetime) == 0) {
        /* TIMEZONE FIX: Convert UTC to local time for user-facing date/time */
        uint32_t utc_timestamp = timezone_rtc_to_unix_utc(&datetime);
        rtc_datetime_t local_datetime;
        if (timezone_unix_to_rtc_local(utc_timestamp, &local_datetime) == 0) {
            return local_datetime.month;
        }
        /* Fallback to UTC if timezone conversion fails */
        return datetime.month;
    }
    return 7; // Fallback if RTC unavailable
}

static uint16_t get_current_day_of_year(void) {
    rtc_datetime_t datetime;
    if (rtc_datetime_get(&datetime) == 0) {
        /* TIMEZONE FIX: Convert UTC to local time for user-facing date/time */
        uint32_t utc_timestamp = timezone_rtc_to_unix_utc(&datetime);
        rtc_datetime_t local_datetime;
        if (timezone_unix_to_rtc_local(utc_timestamp, &local_datetime) == 0) {
            datetime = local_datetime; // Use local time for calculation
        }
        /* Continue with existing calculation using local time */
        // Calculate day of year from month and day
        static const uint16_t days_in_month[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
        uint16_t day_of_year = datetime.day;
        
        // Add days from previous months
        for (uint8_t i = 1; i < datetime.month; i++) {
            day_of_year += days_in_month[i];
        }
        
        // Add leap day if it's a leap year and we're past February
        if (datetime.month > 2 && ((datetime.year % 4 == 0 && datetime.year % 100 != 0) || 
                                   (datetime.year % 400 == 0))) {
            day_of_year++;
        }
        
        return day_of_year;
    }
    return 186; // Fallback for July 5th if RTC unavailable
}

/**
 * @brief Save/load rotation info
 */
static watering_error_t save_rotation_info(void) {
    return (nvs_config_write(NVS_KEY_ROTATION_INFO, &rotation_info, sizeof(rotation_info)) < 0) ? 
           WATERING_ERROR_STORAGE : WATERING_SUCCESS;
}

static watering_error_t load_rotation_info(void) {
    return (nvs_config_read(NVS_KEY_ROTATION_INFO, &rotation_info, sizeof(rotation_info)) < 0) ? 
           WATERING_ERROR_STORAGE : WATERING_SUCCESS;
}

// =============================================================================
// LEGACY COMPATIBILITY FUNCTIONS
// =============================================================================

void watering_history_on_task_start(uint8_t channel_id, watering_mode_t mode, 
                                   uint16_t target_value, bool is_scheduled) {
    watering_trigger_type_t trigger = is_scheduled ? WATERING_TRIGGER_SCHEDULED : WATERING_TRIGGER_MANUAL;
    watering_history_record_task_start(channel_id, mode, target_value, trigger);
}

void watering_history_on_task_complete(uint8_t channel_id, uint16_t actual_value,
                                      uint16_t total_volume_ml, bool success) {
    watering_success_status_t status = success ? WATERING_SUCCESS_COMPLETE : WATERING_SUCCESS_FAILED;
    watering_history_record_task_complete(channel_id, actual_value, total_volume_ml, status);
}

void watering_history_on_task_error(uint8_t channel_id, uint8_t error_code) {
    watering_history_record_task_error(channel_id, error_code);
}

// Placeholder implementations for other functions
watering_error_t watering_history_query_range(uint8_t channel_id, uint32_t start_epoch, uint32_t end_epoch, 
                                             history_event_t *results, uint16_t *count) {
    if (!results || !count || channel_id >= MAX_CHANNELS) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    // Use timeout instead of K_FOREVER to prevent system freeze
    if (k_mutex_lock(&history_mutex, K_MSEC(100)) != 0) {
        *count = 0;
        return WATERING_SUCCESS;
    }
    
    *count = 0;
    uint16_t max_results = 10; /* Limit results to avoid overflow */
    
    /* Search through detailed events for the channel within time range */
    for (uint8_t i = 0; i < current_settings.detailed_cnt && *count < max_results; i++) {
        history_event_t *event = &detailed_events[channel_id][i];
        
        /* Check if event exists and is within time range */
        if (event->dt_delta != 0) {
            /* For this implementation, we don't have proper timestamp conversion,
               so we'll return any existing events */
            results[*count] = *event;
            (*count)++;
        }
    }
    
    k_mutex_unlock(&history_mutex);
    
    LOG_INF("History range query: ch=%u, range=%u-%u, found=%u events", 
            channel_id, start_epoch, end_epoch, *count);
    return WATERING_SUCCESS;
}

watering_error_t watering_history_query_page(uint8_t channel_id, uint16_t page_index, 
                                            history_event_t *results, uint16_t *count) {
    if (!results || !count || channel_id >= MAX_CHANNELS) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    // Use timeout instead of K_FOREVER to prevent system freeze
    if (k_mutex_lock(&history_mutex, K_MSEC(100)) != 0) {
        // If we can't get the mutex quickly, return empty result instead of hanging
        *count = 0;
        return WATERING_SUCCESS;
    }
    
    *count = 0;
    const uint16_t events_per_page = 10;
    uint16_t start_index = page_index * events_per_page;
    uint16_t events_found = 0;
    
    // Count valid events first to determine pagination
    for (uint16_t i = 0; i < current_settings.detailed_cnt; i++) {
        if (detailed_events[channel_id][i].dt_delta != 0) {
            events_found++;
        }
    }
    
    // Check if page is valid
    if (start_index >= events_found) {
        k_mutex_unlock(&history_mutex);
        return WATERING_SUCCESS; // No events on this page
    }
    
    // Collect events for the requested page
    uint16_t current_event = 0;
    for (uint16_t i = 0; i < current_settings.detailed_cnt && *count < events_per_page; i++) {
        if (detailed_events[channel_id][i].dt_delta != 0) {
            if (current_event >= start_index) {
                results[*count] = detailed_events[channel_id][i];
                (*count)++;
            }
            current_event++;
        }
    }
    
    k_mutex_unlock(&history_mutex);
    
    LOG_DBG("Page query: ch=%u, page=%u, returned=%u events (total=%u)", 
            channel_id, page_index, *count, events_found);
    return WATERING_SUCCESS;
}

watering_error_t watering_history_aggregate_daily(uint16_t day_index, uint16_t year) {
    if (day_index >= DAILY_STATS_DAYS) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    k_mutex_lock(&history_mutex, K_MSEC(100));
    
    // Initialize daily stats entry if needed
    if (daily_stats[day_index].day_epoch == 0) {
        daily_stats[day_index].day_epoch = get_current_timestamp();
        daily_stats[day_index].sessions_ok = 0;
        daily_stats[day_index].sessions_err = 0;
        daily_stats[day_index].total_ml = 0;
        daily_stats[day_index].max_channel = 0;
        daily_stats[day_index].success_rate = 100;
    }
    
    // Aggregate events from detailed history for this day
    // This is a simplified implementation
    LOG_DBG("Daily aggregation for day %u, year %u", day_index, year);
    
    k_mutex_unlock(&history_mutex);
    return WATERING_SUCCESS;
}

watering_error_t watering_history_aggregate_monthly(uint8_t month, uint16_t year) {
    if (month == 0 || month > 12) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    k_mutex_lock(&history_mutex, K_MSEC(100));
    
    // Find or create monthly stats entry
    uint8_t month_index = (month - 1) % MONTHLY_STATS_MONTHS;
    
    if (monthly_stats[month_index].year == 0) {
        monthly_stats[month_index].year = year;
        monthly_stats[month_index].month = month;
        monthly_stats[month_index].total_ml = 0;
        monthly_stats[month_index].active_days = 0;
        monthly_stats[month_index].peak_channel = 0;
    }
    
    // Aggregate from daily stats
    LOG_DBG("Monthly aggregation for month %u, year %u", month, year);
    
    k_mutex_unlock(&history_mutex);
    return WATERING_SUCCESS;
}

watering_error_t watering_history_aggregate_annual(uint16_t year) {
    if (year < 2020 || year > 2050) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    k_mutex_lock(&history_mutex, K_MSEC(100));
    
    // Find or create annual stats entry
    uint8_t year_index = (year - 2020) % ANNUAL_STATS_YEARS;
    
    if (annual_stats[year_index].year == 0) {
        annual_stats[year_index].year = year;
        annual_stats[year_index].total_ml = 0;
        annual_stats[year_index].sessions = 0;
        annual_stats[year_index].errors = 0;
        annual_stats[year_index].max_month_ml = 0;
        annual_stats[year_index].min_month_ml = 0;
        annual_stats[year_index].peak_channel = 0;
    }
    
    // Aggregate from monthly stats
    LOG_DBG("Annual aggregation for year %u", year);
    
    k_mutex_unlock(&history_mutex);
    return WATERING_SUCCESS;
}

watering_error_t watering_history_get_daily_stats(uint8_t channel_id, uint16_t start_day, uint16_t end_day, 
                                                 uint16_t year, daily_stats_t *results, uint16_t *count) {
    if (!results || !count || channel_id >= MAX_CHANNELS) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    if (start_day > end_day || end_day >= DAILY_STATS_DAYS) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    k_mutex_lock(&history_mutex, K_MSEC(100));
    
    *count = 0;
    uint16_t max_results = 10; // Limit to prevent overflow
    
    // Copy daily stats for the requested range
    for (uint16_t day = start_day; day <= end_day && *count < max_results; day++) {
        if (daily_stats[day].day_epoch != 0) {
            // Check if this day has data for the requested channel
            // For now, we return all daily stats regardless of channel
            // In a full implementation, we'd maintain per-channel daily stats
            results[*count] = daily_stats[day];
            (*count)++;
        }
    }
    
    k_mutex_unlock(&history_mutex);
    
    LOG_DBG("Daily stats query: ch=%u, days=%u-%u, year=%u, found=%u", 
            channel_id, start_day, end_day, year, *count);
    return WATERING_SUCCESS;
}

watering_error_t watering_history_get_monthly_stats(uint8_t channel_id, uint8_t start_month, uint8_t end_month, 
                                                   uint16_t year, monthly_stats_raw_t *results, uint16_t *count) {
    if (!results || !count || channel_id >= MAX_CHANNELS) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    if (start_month == 0 || start_month > 12 || end_month == 0 || end_month > 12 || start_month > end_month) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    k_mutex_lock(&history_mutex, K_MSEC(100));
    
    *count = 0;
    uint16_t max_results = 12; // One year maximum
    
    // Copy monthly stats for the requested range
    for (uint8_t month = start_month; month <= end_month && *count < max_results; month++) {
        uint8_t month_index = (month - 1) % MONTHLY_STATS_MONTHS;
        
        if (monthly_stats[month_index].year == year && monthly_stats[month_index].month == month) {
            results[*count] = monthly_stats[month_index];
            (*count)++;
        }
    }
    
    k_mutex_unlock(&history_mutex);
    
    LOG_DBG("Monthly stats query: ch=%u, months=%u-%u, year=%u, found=%u", 
            channel_id, start_month, end_month, year, *count);
    return WATERING_SUCCESS;
}

watering_error_t watering_history_get_annual_stats(uint8_t channel_id, uint16_t start_year, uint16_t end_year, 
                                                  annual_stats_t *results, uint16_t *count) {
    if (!results || !count || channel_id >= MAX_CHANNELS) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    if (start_year < 2020 || end_year > 2050 || start_year > end_year) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    k_mutex_lock(&history_mutex, K_MSEC(100));
    
    *count = 0;
    uint16_t max_results = 10; // Limit to prevent overflow
    
    // Copy annual stats for the requested range
    for (uint16_t year = start_year; year <= end_year && *count < max_results; year++) {
        uint8_t year_index = (year - 2020) % ANNUAL_STATS_YEARS;
        
        if (annual_stats[year_index].year == year) {
            results[*count] = annual_stats[year_index];
            (*count)++;
        }
    }
    
    k_mutex_unlock(&history_mutex);
    
    LOG_DBG("Annual stats query: ch=%u, years=%u-%u, found=%u", 
            channel_id, start_year, end_year, *count);
    return WATERING_SUCCESS;
}

watering_error_t watering_history_rotate_old_data(void) {
    k_mutex_lock(&history_mutex, K_MSEC(100));
    
    LOG_INF("Starting data rotation for old history entries");
    
    // Simple rotation: move oldest entries to make space for new ones
    // In a real implementation, this would compress and archive old data
    
    // Rotate detailed events (move oldest entries)
    for (uint8_t ch = 0; ch < MAX_CHANNELS; ch++) {
        if (current_settings.detailed_cnt > DETAILED_EVENTS_PER_CHANNEL * 0.8) {
            // When we reach 80% capacity, remove oldest 20% of entries
            uint16_t remove_count = DETAILED_EVENTS_PER_CHANNEL * 0.2;
            
            for (uint16_t i = 0; i < DETAILED_EVENTS_PER_CHANNEL - remove_count; i++) {
                detailed_events[ch][i] = detailed_events[ch][i + remove_count];
            }
            
            // Clear the freed entries
            memset(&detailed_events[ch][DETAILED_EVENTS_PER_CHANNEL - remove_count], 0, 
                   remove_count * sizeof(history_event_t));
            
            current_settings.detailed_cnt -= remove_count;
        }
    }
    
    k_mutex_unlock(&history_mutex);
    
    LOG_INF("Data rotation completed");
    return WATERING_SUCCESS;
}

watering_error_t watering_history_cleanup_expired(void) {
    k_mutex_lock(&history_mutex, K_MSEC(100));
    
    LOG_INF("Cleaning up expired history entries");
    
    uint32_t current_time = get_current_timestamp();
    
    // Clean up old detailed events
    for (uint8_t ch = 0; ch < MAX_CHANNELS; ch++) {
        for (uint16_t i = 0; i < DETAILED_EVENTS_PER_CHANNEL; i++) {
            if (detailed_events[ch][i].dt_delta != 0) {
                // Check if event is older than expiry time
                // For simplicity, we'll just mark very old entries for removal
                if (detailed_events[ch][i].dt_delta > 7776000) { // ~3 months in seconds
                    memset(&detailed_events[ch][i], 0, sizeof(history_event_t));
                }
            }
        }
    }
    
    // Clean up old daily stats (keep only last 30 days)
    for (uint16_t i = 30; i < DAILY_STATS_DAYS; i++) {
        if (daily_stats[i].day_epoch != 0 && 
            (current_time - daily_stats[i].day_epoch) > (90 * 24 * 3600)) { // 90 days
            memset(&daily_stats[i], 0, sizeof(daily_stats_t));
        }
    }
    
    k_mutex_unlock(&history_mutex);
    
    LOG_INF("Cleanup completed");
    return WATERING_SUCCESS;
}

watering_error_t watering_history_get_storage_info(storage_requirements_t *info) {
    if (!info) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    k_mutex_lock(&history_mutex, K_MSEC(100));
    
    // Calculate storage usage in bytes, then convert to KB
    size_t detailed_size = MAX_CHANNELS * DETAILED_EVENTS_PER_CHANNEL * sizeof(history_event_t);
    size_t daily_size = DAILY_STATS_DAYS * sizeof(daily_stats_t);
    size_t monthly_size = MONTHLY_STATS_MONTHS * sizeof(monthly_stats_raw_t);
    size_t annual_size = ANNUAL_STATS_YEARS * sizeof(annual_stats_t);
    
    info->detailed_events_size = detailed_size / 1024; // Convert to KB
    info->daily_stats_size = daily_size / 1024;
    info->monthly_stats_size = monthly_size / 1024;
    info->annual_stats_size = annual_size / 1024;
    info->total_storage_kb = (detailed_size + daily_size + monthly_size + annual_size) / 1024;
    
    k_mutex_unlock(&history_mutex);
    
    return WATERING_SUCCESS;
}

watering_error_t watering_history_update_cache(void) {
    k_mutex_lock(&history_mutex, K_MSEC(100));
    
    LOG_DBG("Updating history cache");
    
    // Update internal cache/indexes
    // For now, this is a no-op since we don't have complex caching
    // In a full implementation, this would rebuild search indexes, etc.
    
    k_mutex_unlock(&history_mutex);
    
    return WATERING_SUCCESS;
}

watering_error_t watering_history_invalidate_cache(void) {
    k_mutex_lock(&history_mutex, K_MSEC(100));
    
    LOG_DBG("Invalidating history cache");
    
    // Clear any cached data
    // For now, this is a no-op since we don't have complex caching
    
    k_mutex_unlock(&history_mutex);
    
    return WATERING_SUCCESS;
}

watering_error_t watering_history_get_recent_daily_volumes(uint8_t channel_id, uint16_t days_back, 
                                                         uint16_t *volumes_ml, uint16_t *count) {
    if (!volumes_ml || !count || channel_id >= MAX_CHANNELS) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    k_mutex_lock(&history_mutex, K_MSEC(100));
    
    *count = 0;
    uint16_t max_days = (days_back > DAILY_STATS_DAYS) ? DAILY_STATS_DAYS : days_back;
    
    /* Iterate through daily stats and collect volumes for the channel */
    for (uint16_t i = 0; i < max_days && *count < days_back; i++) {
        /* Check if we have valid data for this day */
        if (daily_stats[i].day_epoch != 0) {
            /* For simplicity, use total_ml as the volume for this channel */
            /* In a real implementation, we'd need per-channel daily stats */
            volumes_ml[*count] = (uint16_t)(daily_stats[i].total_ml / 1000); /* Convert ml to liters */
            (*count)++;
        }
    }
    
    k_mutex_unlock(&history_mutex);
    
    LOG_DBG("Retrieved %u daily volumes for channel %u (requested %u days)", *count, channel_id, days_back);
    return WATERING_SUCCESS;
}

watering_error_t watering_history_get_monthly_trends(uint8_t channel_id, uint16_t months_back, 
                                                    monthly_stats_t *monthly_data, uint16_t *count) {
    if (!monthly_data || !count || channel_id >= MAX_CHANNELS) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    if (months_back == 0 || months_back > MONTHLY_STATS_MONTHS) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    k_mutex_lock(&history_mutex, K_MSEC(100));
    
    *count = 0;
    uint16_t current_year = get_current_year();
    uint8_t current_month = get_current_month();
    
    // Calculate monthly trends going backwards from current month
    for (uint16_t i = 0; i < months_back && *count < months_back; i++) {
        // Calculate which month we're looking at
        int16_t target_month = current_month - i;
        uint16_t target_year = current_year;
        
        if (target_month <= 0) {
            target_month += 12;
            target_year--;
        }
        
        // Find corresponding monthly stats
        uint8_t month_index = (target_month - 1) % MONTHLY_STATS_MONTHS;
        
        if (monthly_stats[month_index].year == target_year && 
            monthly_stats[month_index].month == target_month) {
            
            // Convert to monthly_stats_t format (assuming it exists)
            // For now, create a basic trend entry
            monthly_data[*count].year = target_year;
            monthly_data[*count].month = target_month;
            monthly_data[*count].total_ml = monthly_stats[month_index].total_ml;
            monthly_data[*count].active_days = monthly_stats[month_index].active_days;
            monthly_data[*count].peak_channel = monthly_stats[month_index].peak_channel;
            
            (*count)++;
        }
    }
    
    k_mutex_unlock(&history_mutex);
    
    LOG_DBG("Monthly trends for channel %u: found %u months (requested %u)", 
            channel_id, *count, months_back);
    return WATERING_SUCCESS;
}

watering_error_t watering_history_get_annual_overview(uint8_t channel_id, uint16_t years_back, 
                                                     annual_stats_t *annual_data, uint16_t *count) {
    if (!annual_data || !count || channel_id >= MAX_CHANNELS) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    if (years_back == 0 || years_back > ANNUAL_STATS_YEARS) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    k_mutex_lock(&history_mutex, K_MSEC(100));
    
    *count = 0;
    uint16_t current_year = get_current_year();
    
    // Get annual data going backwards from current year
    for (uint16_t i = 0; i < years_back && *count < years_back; i++) {
        uint16_t target_year = current_year - i;
        uint8_t year_index = (target_year - 2020) % ANNUAL_STATS_YEARS;
        
        if (annual_stats[year_index].year == target_year) {
            annual_data[*count] = annual_stats[year_index];
            (*count)++;
        }
    }
    
    k_mutex_unlock(&history_mutex);
    
    LOG_DBG("Annual overview for channel %u: found %u years (requested %u)", 
            channel_id, *count, years_back);
    return WATERING_SUCCESS;
}

watering_error_t watering_history_compare_channels(uint32_t period_days, channel_comparison_t *comparison, 
                                                  uint8_t *channel_count) {
    if (!comparison || !channel_count || period_days == 0) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    k_mutex_lock(&history_mutex, K_MSEC(100));
    
    *channel_count = 0;
    uint32_t current_time = get_current_timestamp();
    uint32_t start_period = current_time - (period_days * 24 * 3600);
    
    // First pass: collect data for all channels
    struct {
        uint32_t total_volume;
        uint16_t event_count;
        uint16_t success_count;
        float efficiency;
    } channel_data[MAX_CHANNELS] = {0};
    
    float total_efficiency = 0.0f;
    uint8_t active_channels = 0;
    
    // Analyze each channel
    for (uint8_t ch = 0; ch < MAX_CHANNELS; ch++) {
        uint32_t total_volume = 0;
        uint16_t event_count = 0;
        uint16_t success_count = 0;
        
        // Count events in period for this channel
        for (uint16_t i = 0; i < current_settings.detailed_cnt; i++) {
            history_event_t *event = &detailed_events[ch][i];
            
            if (event->dt_delta != 0) {
                // Estimate event time (simplified)
                uint32_t event_time = current_time - event->dt_delta;
                
                if (event_time >= start_period) {
                    total_volume += event->actual_ml;
                    event_count++;
                    
                    if (event->flags.success == WATERING_SUCCESS_COMPLETE) {
                        success_count++;
                    }
                }
            }
        }
        
        // Calculate efficiency for this channel
        if (event_count > 0) {
            float success_rate = (float)success_count / event_count;
            float activity_factor = (event_count > period_days) ? 1.0f : (float)event_count / period_days;
            channel_data[ch].efficiency = success_rate * activity_factor * 100.0f;
            channel_data[ch].total_volume = total_volume;
            channel_data[ch].event_count = event_count;
            channel_data[ch].success_count = success_count;
            
            total_efficiency += channel_data[ch].efficiency;
            active_channels++;
        }
    }
    
    // Calculate average efficiency
    float avg_efficiency = (active_channels > 0) ? total_efficiency / active_channels : 0.0f;
    
    // Create volume rankings
    uint8_t volume_rankings[MAX_CHANNELS];
    for (uint8_t i = 0; i < MAX_CHANNELS; i++) {
        volume_rankings[i] = 1; // Start with rank 1
        for (uint8_t j = 0; j < MAX_CHANNELS; j++) {
            if (j != i && channel_data[j].total_volume > channel_data[i].total_volume) {
                volume_rankings[i]++;
            }
        }
    }
    
    // Create frequency rankings
    uint8_t freq_rankings[MAX_CHANNELS];
    for (uint8_t i = 0; i < MAX_CHANNELS; i++) {
        freq_rankings[i] = 1; // Start with rank 1
        for (uint8_t j = 0; j < MAX_CHANNELS; j++) {
            if (j != i && channel_data[j].event_count > channel_data[i].event_count) {
                freq_rankings[i]++;
            }
        }
    }
    
    // Second pass: create comparison results for active channels
    for (uint8_t ch = 0; ch < MAX_CHANNELS && *channel_count < MAX_CHANNELS; ch++) {
        if (channel_data[ch].event_count > 0) {
            comparison[*channel_count].channel_id = ch;
            comparison[*channel_count].efficiency_vs_average = 
                (avg_efficiency > 0) ? (channel_data[ch].efficiency / avg_efficiency) : 1.0f;
            comparison[*channel_count].ranking_volume = volume_rankings[ch];
            comparison[*channel_count].ranking_frequency = freq_rankings[ch];
            
            // Calculate consistency score based on success rate and regularity
            uint8_t success_rate = (channel_data[ch].event_count > 0) ? 
                (channel_data[ch].success_count * 100 / channel_data[ch].event_count) : 0;
            comparison[*channel_count].consistency_score = success_rate;
            
            // Generate optimization suggestions (simplified)
            comparison[*channel_count].optimization_suggestions = 
                (success_rate < 80) ? 1 : (channel_data[ch].event_count < period_days / 7) ? 2 : 0;
            
            (*channel_count)++;
        }
    }
    
    k_mutex_unlock(&history_mutex);
    
    LOG_INF("Channel comparison for %u days: analyzed %u active channels", period_days, *channel_count);
    return WATERING_SUCCESS;
}

watering_error_t watering_history_get_channel_efficiency(uint8_t channel_id, uint32_t period_days, 
                                                        float *efficiency_score) {
    if (!efficiency_score || channel_id >= MAX_CHANNELS || period_days == 0) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    k_mutex_lock(&history_mutex, K_MSEC(100));
    
    uint32_t current_time = get_current_timestamp();
    uint32_t start_period = current_time - (period_days * 24 * 3600);
    
    uint32_t total_volume = 0;
    uint16_t total_events = 0;
    uint16_t successful_events = 0;
    uint32_t total_target_volume = 0;
    
    // Analyze events for this specific channel
    for (uint16_t i = 0; i < current_settings.detailed_cnt; i++) {
        history_event_t *event = &detailed_events[channel_id][i];
        
        if (event->dt_delta != 0) {
            // Estimate event time (simplified)
            uint32_t event_time = current_time - event->dt_delta;
            
            if (event_time >= start_period) {
                total_volume += event->actual_ml;
                total_target_volume += event->target_ml;
                total_events++;
                
                if (event->flags.success == WATERING_SUCCESS_COMPLETE) {
                    successful_events++;
                }
            }
        }
    }
    
    // Calculate efficiency score
    if (total_events == 0 || total_target_volume == 0) {
        *efficiency_score = 0.0f;
    } else {
        // Efficiency = (success_rate * volume_accuracy * activity_factor)
        float success_rate = (float)successful_events / total_events;
        float volume_accuracy = (float)total_volume / total_target_volume;
        
        // Clamp volume accuracy to reasonable range
        if (volume_accuracy > 1.2f) volume_accuracy = 1.2f - (volume_accuracy - 1.2f);
        if (volume_accuracy > 1.0f) volume_accuracy = 2.0f - volume_accuracy;
        
        float activity_factor = (total_events > period_days) ? 1.0f : (float)total_events / period_days;
        
        *efficiency_score = success_rate * volume_accuracy * activity_factor * 100.0f;
        
        // Clamp to 0-100 range
        if (*efficiency_score > 100.0f) *efficiency_score = 100.0f;
        if (*efficiency_score < 0.0f) *efficiency_score = 0.0f;
    }
    
    k_mutex_unlock(&history_mutex);
    
    LOG_DBG("Channel %u efficiency over %u days: %.1f%% (%u events, %u successful)", 
            channel_id, period_days, (double)*efficiency_score, total_events, successful_events);
    return WATERING_SUCCESS;
}

watering_error_t watering_history_export_csv(uint8_t channel_id, uint32_t start_timestamp, uint32_t end_timestamp, 
                                            char *output_buffer, uint16_t buffer_size) {
    if (!output_buffer || buffer_size == 0 || channel_id >= MAX_CHANNELS) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    if (start_timestamp >= end_timestamp) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    k_mutex_lock(&history_mutex, K_MSEC(100));
    
    uint16_t pos = 0;
    
    // Write CSV header
    int header_len = snprintf(output_buffer + pos, buffer_size - pos,
                             "timestamp,channel,mode,target_ml,actual_ml,flow_rate,success,trigger,error\n");
    
    if (header_len < 0 || header_len >= (buffer_size - pos)) {
        k_mutex_unlock(&history_mutex);
        return WATERING_ERROR_INVALID_PARAM;
    }
    pos += header_len;
    
    // Export events for the specified channel and time range
    for (uint16_t i = 0; i < current_settings.detailed_cnt && pos < (buffer_size - 100); i++) {
        history_event_t *event = &detailed_events[channel_id][i];
        
        if (event->dt_delta != 0) {
            // Calculate approximate timestamp
            uint32_t event_timestamp = get_current_timestamp() - event->dt_delta;
            
            if (event_timestamp >= start_timestamp && event_timestamp <= end_timestamp) {
                // Format event as CSV line
                const char *mode_str = event->flags.mode ? "duration" : "volume";
                const char *success_str = (event->flags.success == WATERING_SUCCESS_COMPLETE) ? "complete" : 
                                         (event->flags.success == WATERING_SUCCESS_FAILED) ? "failed" : "partial";
                const char *trigger_str = (event->flags.trigger == WATERING_TRIGGER_SCHEDULED) ? "scheduled" : 
                                         (event->flags.trigger == WATERING_TRIGGER_MANUAL) ? "manual" : "sensor";
                
                int line_len = snprintf(output_buffer + pos, buffer_size - pos,
                                       "%u,%u,%s,%u,%u,%u,%s,%s,%u\n",
                                       event_timestamp, channel_id, mode_str,
                                       event->target_ml, event->actual_ml, event->avg_flow_ml_s,
                                       success_str, trigger_str, event->flags.err);
                
                if (line_len < 0 || line_len >= (buffer_size - pos)) {
                    // Buffer full, truncate here
                    break;
                }
                pos += line_len;
            }
        }
    }
    
    // Null-terminate the output
    if (pos < buffer_size) {
        output_buffer[pos] = '\0';
    } else {
        output_buffer[buffer_size - 1] = '\0';
    }
    
    k_mutex_unlock(&history_mutex);
    
    LOG_INF("CSV export for channel %u: %u bytes written (range %u-%u)", 
            channel_id, pos, start_timestamp, end_timestamp);
    return WATERING_SUCCESS;
}

void watering_history_daily_maintenance(void) {
    LOG_INF("Running daily history maintenance");
    
    /* Update daily aggregations for current day */
    uint16_t current_day = get_current_day_of_year();
    uint16_t current_year = get_current_year();
    
    watering_history_aggregate_daily(current_day, current_year);
    
    /* Trigger garbage collection if needed */
    gc_check_trigger();
    
    LOG_INF("Daily maintenance completed");
}

void watering_history_monthly_maintenance(void) {
    LOG_INF("Running monthly history maintenance");
    
    /* Update monthly aggregations for current month */
    uint8_t current_month = get_current_month();
    uint16_t current_year = get_current_year();
    
    watering_history_aggregate_monthly(current_month, current_year);
    
    /* Rotate old data if storage is getting full */
    watering_history_rotate_old_data();
    
    LOG_INF("Monthly maintenance completed");
}

void watering_history_annual_maintenance(void) {
    LOG_INF("Running annual history maintenance");
    
    /* Update annual aggregations for current year */
    uint16_t current_year = get_current_year();
    
    watering_history_aggregate_annual(current_year);
    
    /* Cleanup expired data (older than retention period) */
    watering_history_cleanup_expired();
    
    /* Update storage optimization */
    watering_history_update_cache();
    
    LOG_INF("Annual maintenance completed");
}

watering_error_t history_service_init(void) {
    return WATERING_SUCCESS;
}

int heatshrink_compress_monthly(const monthly_stats_raw_t *input, uint8_t *output, uint16_t *output_len) {
    if (!input || !output || !output_len) {
        return -1;
    }
    
    /* Simple implementation: just copy data (no compression for now) */
    /* In a real implementation, this would use the Heatshrink compression library */
    size_t input_size = sizeof(monthly_stats_raw_t);
    
    if (*output_len < input_size) {
        return -1; /* Output buffer too small */
    }
    
    memcpy(output, input, input_size);
    *output_len = input_size;
    
    LOG_DBG("Monthly stats compressed: %zu bytes -> %u bytes", input_size, *output_len);
    return 0;
}

int heatshrink_decompress_monthly(const uint8_t *input, uint16_t input_len, monthly_stats_raw_t *output) {
    if (!input || !output || input_len == 0) {
        return -1;
    }
    
    /* Simple implementation: just copy data (no decompression for now) */
    /* In a real implementation, this would use the Heatshrink decompression library */
    size_t output_size = sizeof(monthly_stats_raw_t);
    
    if (input_len < output_size) {
        return -1; /* Input data too small */
    }
    
    memcpy(output, input, output_size);
    
    LOG_DBG("Monthly stats decompressed: %u bytes -> %zu bytes", input_len, output_size);
    return 0;
}

// =============================================================================
// RESET FUNCTIONS
// =============================================================================

/**
 * @brief Reset istoric pentru un canal specific
 */
watering_error_t watering_history_reset_channel_history(uint8_t channel_id) {
    if (channel_id >= MAX_CHANNELS) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    LOG_INF("Resetting history for channel %u", channel_id);
    
    if (k_mutex_lock(&history_mutex, K_MSEC(500)) != 0) {
        LOG_ERR("Failed to acquire mutex for history reset");
        return WATERING_ERROR_TIMEOUT;
    }
    
    // Clear detailed events for this channel
    memset(detailed_events[channel_id], 0, sizeof(detailed_events[channel_id]));
    
    // Clear daily stats entries that contain this channel's data
    for (uint16_t i = 0; i < DAILY_STATS_DAYS; i++) {
        if (daily_stats[i].day_epoch != 0) {
            // For simplicity, we clear the entire day if it contains this channel
            // In a more sophisticated implementation, we could remove only this channel's data
            daily_stats[i].total_ml = 0;
            daily_stats[i].sessions_ok = 0;
            daily_stats[i].sessions_err = 0;
            daily_stats[i].max_channel = 0;
            daily_stats[i].success_rate = 0;
        }
    }
    
    // Clear monthly stats (similar approach)
    for (uint16_t i = 0; i < MONTHLY_STATS_MONTHS; i++) {
        if (monthly_stats[i].year != 0) {
            monthly_stats[i].total_ml = 0;
            monthly_stats[i].active_days = 0;
            monthly_stats[i].peak_channel = 0;
        }
    }
    
    // Clear annual stats
    for (uint16_t i = 0; i < ANNUAL_STATS_YEARS; i++) {
        if (annual_stats[i].year != 0) {
            annual_stats[i].total_ml = 0;
            annual_stats[i].sessions = 0;
            annual_stats[i].errors = 0;
            annual_stats[i].max_month_ml = 0;
        }
    }
    
    // Clear NVS storage for this channel
    for (uint16_t i = 0; i < current_settings.detailed_cnt; i++) {
        uint16_t key = NVS_KEY_DETAILED_BASE + (channel_id * 100) + i;
        nvs_config_delete(key);
    }
    
    // Save updated daily/monthly/annual stats to NVS
    for (uint16_t i = 0; i < current_settings.daily_days; i++) {
        uint16_t key = NVS_KEY_DAILY_BASE + i;
        nvs_config_write(key, &daily_stats[i], sizeof(daily_stats_t));
    }
    
    for (uint16_t i = 0; i < current_settings.monthly_months; i++) {
        uint16_t key = NVS_KEY_MONTHLY_BASE + i;
        nvs_config_write(key, &monthly_stats[i], sizeof(monthly_stats_raw_t));
    }
    
    for (uint16_t i = 0; i < current_settings.annual_years; i++) {
        uint16_t key = NVS_KEY_ANNUAL_BASE + i;
        nvs_config_write(key, &annual_stats[i], sizeof(annual_stats_t));
    }
    
    k_mutex_unlock(&history_mutex);
    
    LOG_INF("History reset completed for channel %u", channel_id);
    return WATERING_SUCCESS;
}

/**
 * @brief Reset istoric pentru toate canalele
 */
watering_error_t watering_history_reset_all_history(void) {
    LOG_INF("Resetting history for all channels");
    
    if (k_mutex_lock(&history_mutex, K_MSEC(500)) != 0) {
        LOG_ERR("Failed to acquire mutex for full history reset");
        return WATERING_ERROR_TIMEOUT;
    }
    
    // Clear all detailed events
    memset(detailed_events, 0, sizeof(detailed_events));
    
    // Clear all daily stats
    memset(daily_stats, 0, sizeof(daily_stats));
    
    // Clear all monthly stats
    memset(monthly_stats, 0, sizeof(monthly_stats));
    
    // Clear all annual stats
    memset(annual_stats, 0, sizeof(annual_stats));
    
    // Clear all insights
    memset(&current_insights, 0, sizeof(current_insights));
    
    // Clear NVS storage for all history data
    for (uint8_t ch = 0; ch < MAX_CHANNELS; ch++) {
        for (uint16_t i = 0; i < current_settings.detailed_cnt; i++) {
            uint16_t key = NVS_KEY_DETAILED_BASE + (ch * 100) + i;
            nvs_config_delete(key);
        }
    }
    
    // Clear daily stats from NVS
    for (uint16_t i = 0; i < current_settings.daily_days; i++) {
        uint16_t key = NVS_KEY_DAILY_BASE + i;
        nvs_config_delete(key);
    }
    
    // Clear monthly stats from NVS
    for (uint16_t i = 0; i < current_settings.monthly_months; i++) {
        uint16_t key = NVS_KEY_MONTHLY_BASE + i;
        nvs_config_delete(key);
    }
    
    // Clear annual stats from NVS
    for (uint16_t i = 0; i < current_settings.annual_years; i++) {
        uint16_t key = NVS_KEY_ANNUAL_BASE + i;
        nvs_config_delete(key);
    }
    
    // Clear insights cache
    nvs_config_delete(NVS_KEY_INSIGHTS_CACHE);
    
    // Reset rotation info
    memset(&rotation_info, 0, sizeof(rotation_info));
    save_rotation_info();
    
    k_mutex_unlock(&history_mutex);
    
    LOG_INF("Complete history reset completed");
    return WATERING_SUCCESS;
}

/**
 * @brief Reset configurație canal (fără istoric)
 */
watering_error_t watering_history_reset_channel_config(uint8_t channel_id) {
    if (channel_id >= MAX_CHANNELS) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    LOG_INF("Resetting channel %u configuration", channel_id);
    
    // Folosim funcția existentă din watering.c
    watering_error_t err = watering_reset_channel_statistics(channel_id);
    if (err != WATERING_SUCCESS) {
        LOG_ERR("Failed to reset channel %u statistics: %d", channel_id, err);
        return err;
    }
    
    // Reset suplimentar pentru configurația canalului prin NVS
    // Ștergem configurația salvată pentru acest canal
    nvs_config_delete(100 + channel_id); // ID_CHANNEL_CFG_BASE + channel_id
    nvs_config_delete(300 + channel_id); // ID_CHANNEL_NAME_BASE + channel_id
    
    LOG_INF("Channel %u configuration reset completed", channel_id);
    return WATERING_SUCCESS;
}

/**
 * @brief Reset complet pentru un canal (istoric + configurație)
 */
watering_error_t watering_history_reset_channel_complete(uint8_t channel_id) {
    if (channel_id >= MAX_CHANNELS) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    LOG_INF("Complete reset for channel %u (history + configuration)", channel_id);
    
    // Reset istoric
    watering_error_t err = watering_history_reset_channel_history(channel_id);
    if (err != WATERING_SUCCESS) {
        LOG_ERR("Failed to reset channel %u history: %d", channel_id, err);
        return err;
    }
    
    // Reset configurație
    err = watering_history_reset_channel_config(channel_id);
    if (err != WATERING_SUCCESS) {
        LOG_ERR("Failed to reset channel %u config: %d", channel_id, err);
        return err;
    }
    
    LOG_INF("Complete reset for channel %u completed successfully", channel_id);
    return WATERING_SUCCESS;
}

/**
 * @brief Factory reset complet
 */
watering_error_t watering_history_factory_reset(void) {
    LOG_WRN("FACTORY RESET - All data will be lost!");
    
    // Reset complet istoric
    watering_error_t err = watering_history_reset_all_history();
    if (err != WATERING_SUCCESS) {
        LOG_ERR("Failed to reset history during factory reset: %d", err);
        return err;
    }
    
    // Reset configurație pentru toate canalele
    for (uint8_t ch = 0; ch < MAX_CHANNELS; ch++) {
        err = watering_history_reset_channel_config(ch);
        if (err != WATERING_SUCCESS) {
            LOG_ERR("Failed to reset channel %u config during factory reset: %d", ch, err);
            // Continue with other channels
        }
    }
    
    // Reset setări history
    memset(&current_settings, 0, sizeof(current_settings));
    current_settings.detailed_cnt = DETAILED_EVENTS_PER_CHANNEL;
    current_settings.daily_days = DAILY_STATS_DAYS;
    current_settings.monthly_months = MONTHLY_STATS_MONTHS;
    current_settings.annual_years = ANNUAL_STATS_YEARS;
    
    nvs_config_write(NVS_KEY_HISTORY_SETTINGS, &current_settings, sizeof(current_settings));
    
    // Reset configurația generală
    nvs_config_delete(1); // ID_WATERING_CFG
    nvs_config_delete(200); // ID_FLOW_CALIB
    nvs_config_delete(201); // ID_DAYS_SINCE_START
    
    LOG_WRN("FACTORY RESET COMPLETED - All data cleared!");
    return WATERING_SUCCESS;
}
