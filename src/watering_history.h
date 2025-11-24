#ifndef WATERING_HISTORY_H
#define WATERING_HISTORY_H

#include <stdint.h>
#include <stdbool.h>
#include "watering.h"
#include <math.h>

/**
 * @file watering_history.h
 * @brief AutoWatering – History subsystem
 * Hardware: nRF52840 (SoftDevice s140)
 * Flash: 144 KB NVS dedicat
 * Constrângeri: max 8 canale de udare
 * 
 * RETENȚIE:
 * - 30 evenimente detaliate / canal  (struct HistoryEventSlim = 15 B)
 * - 90 statistici zilnice            (struct DailyStats      = 16 B)
 * - 36 statistici lunare             (struct MonthlyStatsRaw = 12 B, compresie Heatshrink)
 * - 10 statistici anuale             (struct AnnualStats     = 20 B)
 * - GC: high-watermark 90 %, low 70 %  (rulează în thread maintenance)
 * 
 * BLE – History Service (UUID 0x181A)
 * 0x2A80 ServiceRevision    R  uint8[2] {major,minor}
 * 0x2A81 Capabilities       R  uint32   (bit0 Export, 1 Purge, 2 Insights, …)
 * 0xEF01 HistoryCtrl        RW opCode + TLV params   (maxLen 20 B)
 * 0xEF02 HistoryData        N / Ind / Read   TLV stream, frame-seq, EoT=0xFFFF
 * 0xEF03 Insights           R / N  weekly_ml[8] uint32, leak[8] uint8, efficiency_pct uint8
 * 0xEF04 HistorySettings    RW (LESC) { detailed_cnt, daily_days, monthly_months, annual_years }
 */

// Storage configuration limits
#define DETAILED_EVENTS_PER_CHANNEL 30    // 30 evenimente detaliate per canal
#define DAILY_STATS_DAYS            90    // 90 statistici zilnice
#define MONTHLY_STATS_MONTHS        36    // 36 statistici lunare
#define ANNUAL_STATS_YEARS          10    // 10 statistici anuale
#define MAX_CHANNELS                8     // Maximum 8 canale de udare

// Storage size calculations
#define TOTAL_HISTORY_STORAGE_KB    144
#define FLASH_SECTOR_SIZE_KB        4
#define REQUIRED_FLASH_SECTORS      36

// GC thresholds
#define GC_HIGH_WATERMARK_PCT       90
#define GC_LOW_WATERMARK_PCT        70

/* ---- Utilitare TLV ------------------------------------------------------*/
typedef struct __attribute__((packed)) {
    uint8_t  type;     /* TLV type  */
    uint8_t  len;      /* nr. octeți în value[] */
    uint8_t  value[];  /* len bytes */
} tlv_t;

/* ---- Eveniment detaliat (15 B) -----------------------------------------*/
typedef struct __attribute__((packed)) {
    uint8_t dt_delta;        /* 1‒255 s față de evenimentul anterior; 0 ⇒ timestamp absolut într-un TLV separat */
    union {
        struct {
            uint8_t mode    :1;  /* 0=volume-based, 1=time-based               */
            uint8_t trigger :2;  /* 0=manual 1=schedule 2=remote 3=sensor      */
            uint8_t success :2;  /* 0=ok 1=warn 2=fail 3=skipped               */
            uint8_t err     :3;  /* 0=fără eroare, 1-7 coduri predefinite      */
        };
        uint8_t packed;
    } flags;                  /* 1 B */
    uint16_t target_ml;       /* ml țintiți (sau durată țintă s dacă mode=1) */
    uint16_t actual_ml;       /* ml reali (sau durată reală s)               */
    uint16_t avg_flow_ml_s;   /* debit mediu ml/s                            */
    uint8_t  reserved[3];     /* păstrează structura la EXACT 15 B           */
} history_event_t;            /* 15 B */

/* ---- Stat Zilnic (16 B) ------------------------------------------------*/
typedef struct __attribute__((packed)) {
    uint32_t day_epoch;       /* 00:00 UTC al zilei                          */
    uint32_t total_ml;        /* volum total ml                              */
    uint16_t sessions_ok;     /* sesiuni reușite                             */
    uint16_t sessions_err;    /* sesiuni cu eroare                           */
    uint8_t  max_channel;     /* canal cu volum maxim                        */
    uint8_t  success_rate;    /* 0‒100 %                                     */
    uint8_t  reserved[2];
} daily_stats_t;              /* 16 B */

/* ---- Stat Lunar (RAW, 12 B → ~11 B comprimat) --------------------------*/
typedef struct __attribute__((packed)) {
    uint16_t year;            /* ex. 2025                                    */
    uint8_t  month;           /* 1‒12                                        */
    uint32_t total_ml;        /* volum total ml                              */
    uint16_t active_days;     /* zile în care s-a udat                       */
    uint8_t  peak_channel;    /* canal cu volum maxim                        */
    uint8_t  reserved;        /* padding                                     */
} monthly_stats_raw_t;        /* 12 B */

/* ---- Stat Anual (20 B) -------------------------------------------------*/
typedef struct __attribute__((packed)) {
    uint16_t year;            /* ex. 2025                                    */
    uint32_t total_ml;        /* volum total ml                              */
    uint32_t sessions;        /* nr. total sesiuni                           */
    uint32_t errors;          /* nr. total erori                             */
    uint16_t max_month_ml;    /* volum maxim/lună                            */
    uint16_t min_month_ml;    /* volum minim/lună                            */
    uint8_t  peak_channel;    /* canal cel mai activ                         */
    uint8_t  reserved[3];
} annual_stats_t;             /* 20 B */

/* ---- Header cadru HistoryData -----------------------------------------*/
typedef struct __attribute__((packed)) {
    uint16_t seq;    /* 0‒0xFFFE incremental; 0xFFFF ⇒ End-of-Transfer     */
    uint16_t len;    /* număr octeți în payload[]                          */
    uint8_t  payload[];
} history_frame_t;

/* ---- Enum TLV types pentru HistoryCtrl --------------------------------*/
enum {
    HT_CHANNEL_ID   = 0x00,  /* uint8  */
    HT_RANGE_START  = 0x01,  /* uint32 epoch */
    HT_RANGE_END    = 0x02,  /* uint32 epoch */
    HT_PAGE_INDEX   = 0x03,  /* uint16 */
    HT_BEFORE_EPOCH = 0x04   /* uint32 epoch */
};

/* ---- Enum OpCodes HistoryCtrl -----------------------------------------*/
enum {
    HC_QUERY_RANGE   = 0x01,
    HC_QUERY_PAGE    = 0x02,
    HC_EXPORT_START  = 0x10,
    HC_EXPORT_ACK    = 0x11,
    HC_EXPORT_FINISH = 0x12,
    HC_RESET_HISTORY = 0x20,  // Reset istoric pentru canal specific sau toate
    HC_RESET_CHANNEL = 0x21,  // Reset configurație canal (fără istoric)
    HC_RESET_ALL     = 0x22,  // Reset complet (istoric + configurație)
    HC_FACTORY_RESET = 0xFF   // Factory reset complet
};

/* ---- Setări History ----------------------------------------------------*/
typedef struct __attribute__((packed)) {
    uint8_t detailed_cnt;     /* evenimente detaliate per canal             */
    uint8_t daily_days;       /* zile de statistici zilnice                 */
    uint8_t monthly_months;   /* luni de statistici lunare                  */
    uint8_t annual_years;     /* ani de statistici anuale                   */
} history_settings_t;         /* 4 B */

/* ---- Insights (33 B) --------------------------------------------------*/
typedef struct __attribute__((packed)) {
    uint32_t weekly_ml[8];    /* volum săptămânal per canal (32 B)          */
    uint8_t  leak[8];         /* indicator scurgere per canal (8 B)         */
    uint8_t  efficiency_pct;  /* eficiență generală % (1 B)                 */
} insights_t;                 /* 33 B */
/* ---- Compatibilitate cu structuri vechi (deprecated) ------------------*/
typedef history_event_t watering_event_detailed_t;  // backward compatibility
typedef enum {
    WATERING_SUCCESS_COMPLETE = 0,
    WATERING_SUCCESS_PARTIAL = 1,
    WATERING_SUCCESS_FAILED = 2
} watering_success_status_t;

typedef enum {
    WATERING_EVENT_START = 0,
    WATERING_EVENT_COMPLETE = 1,
    WATERING_EVENT_ABORT = 2,
    WATERING_EVENT_ERROR = 3
} watering_event_type_t;

/* ---- Stat Lunar (alias pentru compatibilitate) ------------------------*/
typedef monthly_stats_raw_t monthly_stats_t;

/* ---- Management intern --------------------------------------------------*/
typedef struct {
    uint32_t oldest_detailed_timestamp;
    uint16_t oldest_daily_day;
    uint16_t oldest_monthly_month;
    uint16_t oldest_annual_year;
    uint16_t detailed_count;
    uint16_t daily_count;
    uint16_t monthly_count;
    uint16_t annual_count;
    bool rotation_needed[4];
} history_rotation_t;

/* ---- Storage requirements -----------------------------------------------*/
typedef struct {
    uint32_t detailed_events_size;
    uint32_t daily_stats_size;
    uint32_t monthly_stats_size;
    uint32_t annual_stats_size;
    uint32_t total_storage_kb;
} storage_requirements_t;

/* ---- Channel comparison -------------------------------------------------*/
typedef struct {
    uint8_t channel_id;
    float efficiency_vs_average;
    uint8_t ranking_volume;
    uint8_t ranking_frequency;
    uint8_t consistency_score;
    uint8_t optimization_suggestions;
} channel_comparison_t;

/* ---- History cache ------------------------------------------------------*/
typedef struct {
    daily_stats_t last_30_days[8][30];
    monthly_stats_t last_12_months[8][12];
    bool cache_valid[2];
    uint32_t last_cache_update;
} history_cache_t;

// ========================================================================
// FUNCȚII API HISTORY
// ========================================================================

// Core history management functions
watering_error_t watering_history_init(void);
watering_error_t watering_history_deinit(void);

// Event recording functions
watering_error_t watering_history_add_event(const history_event_t *event);
watering_error_t watering_history_record_task_start(uint8_t channel_id, 
                                                   watering_mode_t mode,
                                                   uint16_t target_value,
                                                   watering_trigger_type_t trigger);
watering_error_t watering_history_record_task_complete(uint8_t channel_id,
                                                      uint16_t actual_value,
                                                      uint16_t total_volume_ml,
                                                      watering_success_status_t status);
watering_error_t watering_history_record_task_error(uint8_t channel_id,
                                                   uint8_t error_code);
watering_error_t watering_history_record_task_skip(uint8_t channel_id,
                                                   watering_skip_reason_t reason,
                                                   float rain_amount_mm);

// TLV helper functions
int tlv_encode_uint8(uint8_t *buffer, uint8_t type, uint8_t value);
int tlv_encode_uint16(uint8_t *buffer, uint8_t type, uint16_t value);
int tlv_encode_uint32(uint8_t *buffer, uint8_t type, uint32_t value);
int tlv_decode_uint8(const uint8_t *buffer, uint8_t type, uint8_t *value);
int tlv_decode_uint16(const uint8_t *buffer, uint8_t type, uint16_t *value);
int tlv_decode_uint32(const uint8_t *buffer, uint8_t type, uint32_t *value);

// BLE History Service functions
watering_error_t history_service_init(void);
watering_error_t history_ctrl_handler(const uint8_t *data, uint16_t len);
watering_error_t history_data_send_frame(uint16_t seq, const uint8_t *payload, uint16_t len);
watering_error_t history_insights_update(const insights_t *insights);
watering_error_t history_settings_get(history_settings_t *settings);
watering_error_t history_settings_set(const history_settings_t *settings);

// Query functions
watering_error_t watering_history_query_range(uint8_t channel_id,
                                              uint32_t start_epoch,
                                              uint32_t end_epoch,
                                              history_event_t *results,
                                              uint16_t *count);

watering_error_t watering_history_query_page(uint8_t channel_id,
                                             uint16_t page_index,
                                             history_event_t *results,
                                             uint16_t *count,
                                             uint32_t *timestamps);

// Statistics aggregation
watering_error_t watering_history_aggregate_daily(uint16_t day_index, uint16_t year);
watering_error_t watering_history_aggregate_monthly(uint8_t month, uint16_t year);
watering_error_t watering_history_aggregate_annual(uint16_t year);

// Quick access functions
watering_error_t watering_history_get_daily_stats(uint8_t channel_id,
                                                  uint16_t start_day,
                                                  uint16_t end_day,
                                                  uint16_t year,
                                                  daily_stats_t *results,
                                                  uint16_t *count);

watering_error_t watering_history_get_monthly_stats(uint8_t channel_id,
                                                    uint8_t start_month,
                                                    uint8_t end_month,
                                                    uint16_t year,
                                                    monthly_stats_t *results,
                                                    uint16_t *count);
watering_error_t watering_history_count_events(uint8_t channel_id,
                                               uint32_t start_epoch,
                                               uint32_t end_epoch,
                                               uint16_t *count);

watering_error_t watering_history_get_annual_stats(uint8_t channel_id,
                                                   uint16_t start_year,
                                                   uint16_t end_year,
                                                   annual_stats_t *results,
                                                   uint16_t *count);

// Maintenance and GC functions
watering_error_t watering_history_gc_trigger(void);
watering_error_t watering_history_rotate_old_data(void);
watering_error_t watering_history_cleanup_expired(void);
watering_error_t watering_history_get_storage_info(storage_requirements_t *info);

// Cache management
watering_error_t watering_history_update_cache(void);
watering_error_t watering_history_invalidate_cache(void);

// Compression functions (Heatshrink)
int heatshrink_compress_monthly(const monthly_stats_raw_t *input, uint8_t *output, uint16_t *output_len);
int heatshrink_decompress_monthly(const uint8_t *input, uint16_t input_len, monthly_stats_raw_t *output);

// Integration hooks
void watering_history_on_task_start(uint8_t channel_id, watering_mode_t mode, 
                                   uint16_t target_value, bool is_scheduled);
void watering_history_on_task_complete(uint8_t channel_id, uint16_t actual_value,
                                      uint16_t total_volume_ml, bool success);
void watering_history_on_task_error(uint8_t channel_id, uint8_t error_code);

// Maintenance functions
void watering_history_daily_maintenance(void);
void watering_history_monthly_maintenance(void);
void watering_history_annual_maintenance(void);

// Legacy compatibility functions
watering_error_t watering_history_get_recent_daily_volumes(uint8_t channel_id,
                                                          uint16_t days_back,
                                                          uint16_t *volumes_ml,
                                                          uint16_t *count);

watering_error_t watering_history_get_monthly_trends(uint8_t channel_id,
                                                     uint16_t months_back,
                                                     monthly_stats_t *monthly_data,
                                                     uint16_t *count);

watering_error_t watering_history_get_annual_overview(uint8_t channel_id,
                                                     uint16_t years_back,
                                                     annual_stats_t *annual_data,
                                                     uint16_t *count);

watering_error_t watering_history_compare_channels(uint32_t period_days,
                                                   channel_comparison_t *comparison,
                                                   uint8_t *channel_count);

watering_error_t watering_history_get_channel_efficiency(uint8_t channel_id,
                                                        uint32_t period_days,
                                                        float *efficiency_score);

watering_error_t watering_history_export_csv(uint8_t channel_id,
                                             uint32_t start_timestamp,
                                             uint32_t end_timestamp,
                                             char *output_buffer,
                                             uint16_t buffer_size);

// Reset functions
watering_error_t watering_history_reset_channel_history(uint8_t channel_id);
watering_error_t watering_history_reset_all_history(void);
watering_error_t watering_history_reset_channel_config(uint8_t channel_id);
watering_error_t watering_history_reset_channel_complete(uint8_t channel_id);
watering_error_t watering_history_factory_reset(void);

#endif // WATERING_HISTORY_H
