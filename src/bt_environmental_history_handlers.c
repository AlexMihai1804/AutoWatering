#include "bt_environmental_history_handlers.h"
#include "bt_gatt_structs_enhanced.h"
#include "environmental_history.h"
#include "watering_log.h"
#include "enhanced_error_handling.h"
#include <string.h>
#include <time.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(bt_env_history, LOG_LEVEL_DBG);

/**
 * @file bt_environmental_history_handlers.c
 * @brief BLE interface for environmental historical data access
 * 
 * This module implements BLE characteristics for accessing environmental
 * historical data with fragmentation support for large data transfers.
 */

/* BLE fragmentation constants per documentation */
#define BLE_FRAGMENT_MAX_SIZE           240     /* 8B header + 232B payload */
#define BLE_FRAGMENT_HEADER_SIZE        8
#define BLE_FRAGMENT_DATA_SIZE          (BLE_FRAGMENT_MAX_SIZE - BLE_FRAGMENT_HEADER_SIZE) /* 232 */

/* Global state for fragmented transfers */
/* Transfer cache for current query (paged by client via fragment_id) */
static struct {
    bool prepared;
    uint8_t cmd;                 /* command value from request */
    uint8_t api_data_type;       /* 0=detailed,1=hourly,2=daily */
    uint16_t total_fragments;    /* total fragments available */
    uint16_t total_records;      /* total records in request range */
    uint8_t buffer[8192];        /* packed records storage */
    size_t total_data_size;      /* bytes packed in buffer */
} g_transfer = {0};

/* Internal helper functions */
static int prepare_transfer_buffer(uint8_t api_data_type,
                                  const void *entries,
                                  uint16_t count);
static size_t pack_hourly_records(const hourly_history_entry_t *entries, uint16_t count,
                                  uint8_t *out, size_t out_cap);
static size_t pack_daily_records(const daily_history_entry_t *entries, uint16_t count,
                                 uint8_t *out, size_t out_cap);
/* Detailed view: derive from hourly entries (individual sensor readings) */
typedef struct {
    uint32_t ts;       /* Unix timestamp */
    int16_t  t_c_x100; /* temp C * 100 */
    uint16_t h_x100;   /* humidity % * 100 */
    uint32_t p_pa;     /* pressure Pa */
} __attribute__((packed)) detailed_record_t; /* 12B */
static size_t pack_detailed_records(const hourly_history_entry_t *entries, uint16_t count,
                                    uint8_t *out, size_t out_cap);
static void cleanup_transfer(void);

int bt_env_history_request_handler(const ble_history_request_t *request, 
                                  ble_history_response_t *response)
{
    if (!request || !response) {
        return -WATERING_ERROR_INVALID_PARAM;
    }

    /* Validate per documented command */
    LOG_DBG("Env history cmd=0x%02x type=%u start=%u end=%u max=%u frag=%u",
            request->command, request->data_type, request->start_time,
            request->end_time, request->max_records, request->fragment_id);

    /* Special handling: GET_TRENDS (0x04) produces data_type=0x03 (trends) */
    bool is_trends_cmd = (request->command == 0x04);
    if (!is_trends_cmd && request->data_type > 2) {
        memset(response, 0, sizeof(*response));
        response->status = 0x01; /* Invalid command/data_type */
        response->data_type = request->data_type;
        return 0;
    }
    if (is_trends_cmd) {
        /* Build a single trends record from last 24h hourly data */
        hourly_history_entry_t *hourly = k_malloc(sizeof(*hourly) * 48);
        uint16_t actual = 0;
        uint32_t end_ts = request->end_time ? request->end_time : (uint32_t)time(NULL);
        uint32_t start_ts = end_ts - 24 * 3600;
        memset(response, 0, sizeof(*response));
        response->data_type = 0x03; /* trends */
        if (!hourly) {
            response->status = 0x05; /* Storage error (OOM) */
            return 0;
        }
        int rc2 = env_history_get_hourly_range(start_ts, end_ts, hourly, 48, &actual);
        if (rc2 != 0 || actual < 2) {
            response->status = 0x03; /* No data */
            response->record_count = 0;
            response->fragment_id = 0;
            response->total_fragments = 0;
            k_free(hourly);
            return 0;
        }
        /* Compute metrics */
        float first_temp = hourly[0].environmental.temperature;
        float last_temp = hourly[actual - 1].environmental.temperature;
        float first_hum = hourly[0].environmental.humidity;
        float last_hum = hourly[actual - 1].environmental.humidity;
        float first_press = hourly[0].environmental.pressure; /* assume hPa */
        float last_press = hourly[actual - 1].environmental.pressure;
        float temp_min = first_temp, temp_max = first_temp;
        float hum_min = first_hum, hum_max = first_hum;
        for (uint16_t i = 1; i < actual; i++) {
            float t = hourly[i].environmental.temperature;
            float h = hourly[i].environmental.humidity;
            if (t < temp_min) temp_min = t;
            if (t > temp_max) temp_max = t;
            if (h < hum_min) hum_min = h;
            if (h > hum_max) hum_max = h;
        }
        float hours_span = (float)(hourly[actual - 1].timestamp - hourly[0].timestamp) / 3600.0f;
        if (hours_span < 0.5f) hours_span = (float)(actual - 1); /* fallback */
        float temp_slope = (last_temp - first_temp) / hours_span; /* C per hour */
        float hum_slope = (last_hum - first_hum) / hours_span; /* % per hour */
        float press_slope = ((last_press - first_press) * 100.0f) / hours_span; /* convert hPa diff to Pa per hour */

        /* Pack trends record (24 bytes) */
        struct __attribute__((packed)) trend_record {
            int16_t temp_change_24h_x100;
            int16_t humidity_change_24h_x100;
            int32_t pressure_change_24h;      /* Pa */
            int16_t temp_min_24h_x100;
            int16_t temp_max_24h_x100;
            uint16_t humidity_min_24h_x100;
            uint16_t humidity_max_24h_x100;
            int16_t temp_slope_c_per_hr_x100;
            int16_t humidity_slope_pct_per_hr_x100;
            int16_t pressure_slope_pa_per_hr;
            uint16_t sample_count;
        } tr;
        tr.temp_change_24h_x100 = (int16_t)((last_temp - first_temp) * 100.0f);
        tr.humidity_change_24h_x100 = (int16_t)((last_hum - first_hum) * 100.0f);
        tr.pressure_change_24h = (int32_t)((last_press - first_press) * 100.0f); /* hPa -> Pa */
        tr.temp_min_24h_x100 = (int16_t)(temp_min * 100.0f);
        tr.temp_max_24h_x100 = (int16_t)(temp_max * 100.0f);
        tr.humidity_min_24h_x100 = (uint16_t)(hum_min * 100.0f);
        tr.humidity_max_24h_x100 = (uint16_t)(hum_max * 100.0f);
        tr.temp_slope_c_per_hr_x100 = (int16_t)(temp_slope * 100.0f);
        tr.humidity_slope_pct_per_hr_x100 = (int16_t)(hum_slope * 100.0f);
        tr.pressure_slope_pa_per_hr = (int16_t)(press_slope); /* already Pa/hr */
        tr.sample_count = actual;
        memcpy(response->data, &tr, sizeof(tr));
        response->record_count = 1;
        response->fragment_id = 0;
        response->total_fragments = 1;
        response->status = 0;
        k_free(hourly);
        return 0;
    }
    if (request->start_time > request->end_time) {
        memset(response, 0, sizeof(*response));
        response->status = 0x02; /* Invalid time range */
        response->data_type = request->data_type;
        return 0;
    }

    /* CLEAR_HISTORY handled elsewhere; reflect as success here if requested */
    if (request->command == 0x05) {
        int rr = env_history_reset_all();
        memset(response, 0, sizeof(*response));
        response->status = (rr == 0) ? 0 : 0x05; /* Storage error */
        response->data_type = request->data_type;
        response->record_count = 0;
        response->fragment_id = 0;
        response->total_fragments = 0;
        return 0;
    }

    /* Gather raw entries according to type */
    int rc = 0;
    uint16_t max_req = (request->max_records == 0) ? 100 : request->max_records;
    if (max_req > 100) max_req = 100;

    hourly_history_entry_t *hourly = NULL;
    daily_history_entry_t *daily = NULL;
    uint16_t actual = 0;

    cleanup_transfer();
    g_transfer.cmd = request->command;
    g_transfer.api_data_type = request->data_type;

    if (request->data_type == 0 /* detailed */ || request->data_type == 1 /* hourly */) {
        hourly = k_malloc(sizeof(*hourly) * max_req);
        if (!hourly) {
            memset(response, 0, sizeof(*response));
            response->status = 0x05; /* Storage error (OOM) */
            response->data_type = request->data_type;
            return 0;
        }
        rc = env_history_get_hourly_range(request->start_time, request->end_time,
                                          hourly, max_req, &actual);
        if (rc != 0) {
            memset(response, 0, sizeof(*response));
            response->status = 0x05; /* Storage error */
            response->data_type = request->data_type;
            k_free(hourly);
            return 0;
        }
        if (actual == 0) {
            memset(response, 0, sizeof(*response));
            response->status = 0x03; /* No data */
            response->data_type = request->data_type;
            k_free(hourly);
            return 0;
        }
        rc = prepare_transfer_buffer(request->data_type, hourly, actual);
        k_free(hourly);
    } else if (request->data_type == 2 /* daily */) {
        daily = k_malloc(sizeof(*daily) * max_req);
        if (!daily) {
            memset(response, 0, sizeof(*response));
            response->status = 0x05; /* Storage error (OOM) */
            response->data_type = request->data_type;
            return 0;
        }
        rc = env_history_get_daily_range(request->start_time, request->end_time,
                                         daily, max_req, &actual);
        if (rc != 0) {
            memset(response, 0, sizeof(*response));
            response->status = 0x05; /* Storage error */
            response->data_type = request->data_type;
            k_free(daily);
            return 0;
        }
        if (actual == 0) {
            memset(response, 0, sizeof(*response));
            response->status = 0x03; /* No data */
            response->data_type = request->data_type;
            k_free(daily);
            return 0;
        }
        rc = prepare_transfer_buffer(request->data_type, daily, actual);
        k_free(daily);
    }
    if (rc != 0) {
        memset(response, 0, sizeof(*response));
        response->status = 0x05; /* Storage/pack error */
        response->data_type = request->data_type;
        return 0;
    }

    /* Compute which fragment to return (client provided fragment_id) */
    uint8_t frag = request->fragment_id;
    if (frag >= g_transfer.total_fragments) {
        memset(response, 0, sizeof(*response));
        response->status = 0x06; /* Invalid fragment */
        response->data_type = request->data_type;
        response->fragment_id = frag;
        response->total_fragments = g_transfer.total_fragments;
        return 0;
    }

    /* Fill response header in documented order */
    memset(response, 0, sizeof(*response));
    response->status = 0;
    response->data_type = request->data_type;
    response->fragment_id = frag;
    response->total_fragments = g_transfer.total_fragments;

    /* Copy up to 232 bytes for this fragment */
    size_t offset = (size_t)frag * BLE_FRAGMENT_DATA_SIZE;
    size_t remaining = (g_transfer.total_data_size > offset) ? (g_transfer.total_data_size - offset) : 0;
    size_t to_copy = remaining > BLE_FRAGMENT_DATA_SIZE ? BLE_FRAGMENT_DATA_SIZE : remaining;
    if (to_copy > 0) {
        memcpy(response->data, &g_transfer.buffer[offset], to_copy);
    }
    /* Compute record_count within this fragment by record size per type */
    uint8_t rec_size = (request->data_type == 0) ? 12 : (request->data_type == 1) ? 16 : 22;
    response->record_count = (uint8_t)(to_copy / rec_size);

    return 0;
}

int bt_env_history_get_next_fragment(ble_history_response_t *response)
{
    /* Deprecated flow – kept for compatibility; return invalid fragment */
    if (!response) return -WATERING_ERROR_INVALID_PARAM;
    memset(response, 0, sizeof(*response));
    response->status = 0x06; /* Invalid fragment */
    return 0;
}

int bt_env_history_get_stats(ble_env_history_stats_t *stats_response)
{
    if (!stats_response) {
        return -WATERING_ERROR_INVALID_PARAM;
    }

    env_history_stats_t stats;
    int result = env_history_get_stats(&stats);
    
    if (result != 0) {
        LOG_ERR("Failed to get environmental history stats: %d", result);
        return result;
    }

    // Convert to BLE response format
    memset(stats_response, 0, sizeof(ble_env_history_stats_t));
    stats_response->hourly_entries_used = stats.hourly_entries_used;
    stats_response->daily_entries_used = stats.daily_entries_used;
    stats_response->monthly_entries_used = stats.monthly_entries_used;
    stats_response->oldest_hourly_timestamp = stats.oldest_hourly_timestamp;
    stats_response->oldest_daily_timestamp = stats.oldest_daily_timestamp;
    stats_response->oldest_monthly_timestamp = stats.oldest_monthly_timestamp;
    stats_response->total_storage_bytes = stats.total_storage_bytes;
    stats_response->storage_utilization_pct = stats.storage_utilization_pct;

    LOG_DBG("Environmental history stats: %d hourly, %d daily, %d monthly entries, %d%% utilization",
            stats.hourly_entries_used, stats.daily_entries_used, 
            stats.monthly_entries_used, stats.storage_utilization_pct);

    return 0;
}

int bt_env_history_reset_request(const ble_env_history_reset_t *reset_request)
{
    if (!reset_request) {
        return -WATERING_ERROR_INVALID_PARAM;
    }

    LOG_WRN("Environmental history reset requested");

    // Cleanup any active transfer
    cleanup_transfer();

    // Perform reset
    int result = env_history_reset_all();
    
    if (result == 0) {
        LOG_INF("Environmental history reset completed");
    } else {
        LOG_ERR("Environmental history reset failed: %d", result);
    }

    return result;
}

bool bt_env_history_is_transfer_active(void)
{
    return g_transfer.prepared;
}

void bt_env_history_cancel_transfer(void)
{
    if (g_transfer.prepared) {
        LOG_INF("Cancelling prepared environmental history transfer");
        cleanup_transfer();
    }
}

/* Internal helper function implementations */

static int prepare_transfer_buffer(uint8_t api_data_type,
                                  const void *entries,
                                  uint16_t count)
{
    memset(&g_transfer, 0, sizeof(g_transfer));
    g_transfer.api_data_type = api_data_type;

    size_t written = 0;
    if (api_data_type == 0) {
        written = pack_detailed_records((const hourly_history_entry_t*)entries, count,
                                        g_transfer.buffer, sizeof(g_transfer.buffer));
    } else if (api_data_type == 1) {
        written = pack_hourly_records((const hourly_history_entry_t*)entries, count,
                                      g_transfer.buffer, sizeof(g_transfer.buffer));
    } else {
        written = pack_daily_records((const daily_history_entry_t*)entries, count,
                                     g_transfer.buffer, sizeof(g_transfer.buffer));
    }
    if (written == 0) {
        return -WATERING_ERROR_INVALID_DATA;
    }
    g_transfer.total_data_size = written;
    g_transfer.total_fragments = (uint16_t)((written + BLE_FRAGMENT_DATA_SIZE - 1) / BLE_FRAGMENT_DATA_SIZE);
    g_transfer.prepared = true;
    return 0;
}

static void cleanup_transfer(void)
{
    memset(&g_transfer, 0, sizeof(g_transfer));
}

/* Pack hourly summaries per doc: 16B per record */
static size_t pack_hourly_records(const hourly_history_entry_t *entries, uint16_t count,
                                  uint8_t *out, size_t out_cap)
{
    size_t off = 0;
    for (uint16_t i = 0; i < count; i++) {
        if (off + 16 > out_cap) break;
        uint32_t ts = entries[i].timestamp; /* hour start */
        /* We derive avg/min/max temperature from stored structure.
           If not available distinctly, use current as avg and same for min/max. */
        int16_t t_avg = (int16_t)((entries[i].environmental.temperature) * 100.0f);
        int16_t t_min = t_avg;
        int16_t t_max = t_avg;
        uint16_t h_avg = (uint16_t)((entries[i].environmental.humidity) * 100.0f);
        uint32_t p_avg = (uint32_t)((entries[i].environmental.pressure) * 100.0f); /* hPa -> Pa */

        memcpy(&out[off], &ts, 4); off += 4;
        memcpy(&out[off], &t_avg, 2); off += 2;
        memcpy(&out[off], &t_min, 2); off += 2;
        memcpy(&out[off], &t_max, 2); off += 2;
        memcpy(&out[off], &h_avg, 2); off += 2;
        memcpy(&out[off], &p_avg, 4); off += 4;
    }
    return off;
}

/* Pack daily summaries per doc: 22B per record (spec shows 20 fields + sample_count u16) */
static size_t pack_daily_records(const daily_history_entry_t *entries, uint16_t count,
                                 uint8_t *out, size_t out_cap)
{
    size_t off = 0;
    for (uint16_t i = 0; i < count; i++) {
        if (off + 22 > out_cap) break;
        /* day_timestamp: convert YYYYMMDD to Unix is expensive here; instead use date as-is in ts field if proper conversion helper is missing */
        uint32_t ts = entries[i].date; /* NOTE: Assumption – consumer maps YYYYMMDD; can be improved */
        int16_t t_avg = (int16_t)(entries[i].temperature.avg * 100.0f);
        int16_t t_min = (int16_t)(entries[i].temperature.min * 100.0f);
        int16_t t_max = (int16_t)(entries[i].temperature.max * 100.0f);
        uint16_t h_avg = (uint16_t)(entries[i].humidity.avg * 100.0f);
        uint16_t h_min = (uint16_t)(entries[i].humidity.min * 100.0f);
        uint16_t h_max = (uint16_t)(entries[i].humidity.max * 100.0f);
        uint32_t p_avg = (uint32_t)(entries[i].pressure.avg * 100.0f); /* hPa -> Pa */
        uint16_t samples = entries[i].sample_count;
        if (samples == 0) {
            samples = 24; /* Fallback when aggregation metadata unavailable */
        }

        memcpy(&out[off], &ts, 4); off += 4;
        memcpy(&out[off], &t_avg, 2); off += 2;
        memcpy(&out[off], &t_min, 2); off += 2;
        memcpy(&out[off], &t_max, 2); off += 2;
        memcpy(&out[off], &h_avg, 2); off += 2;
        memcpy(&out[off], &h_min, 2); off += 2;
        memcpy(&out[off], &h_max, 2); off += 2;
        memcpy(&out[off], &p_avg, 4); off += 4;
        memcpy(&out[off], &samples, 2); off += 2;
    }
    return off;
}

/* Pack detailed records (12B) from hourly entries */
static size_t pack_detailed_records(const hourly_history_entry_t *entries, uint16_t count,
                                    uint8_t *out, size_t out_cap)
{
    size_t off = 0;
    for (uint16_t i = 0; i < count; i++) {
        if (off + sizeof(detailed_record_t) > out_cap) break;
        detailed_record_t r;
        r.ts = entries[i].timestamp;
        r.t_c_x100 = (int16_t)(entries[i].environmental.temperature * 100.0f);
        r.h_x100 = (uint16_t)(entries[i].environmental.humidity * 100.0f);
        r.p_pa = (uint32_t)(entries[i].environmental.pressure * 100.0f); /* hPa -> Pa */
        memcpy(&out[off], &r, sizeof(r));
        off += sizeof(r);
    }
    return off;
}
