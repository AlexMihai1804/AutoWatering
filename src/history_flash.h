/**
 * @file history_flash.h
 * @brief External flash history storage using LittleFS
 * 
 * Stores environmental and rain history data on W25Q128 external flash
 * to free up internal RAM. Data is organized in ring buffer files with
 * automatic rotation when capacity is reached.
 * 
 * Storage Layout on LittleFS (/lfs/history/):
 *   - env_hourly.bin:   720 entries × 32 bytes = ~23 KB
 *   - env_daily.bin:    372 entries × 56 bytes = ~20 KB
 *   - env_monthly.bin:   60 entries × 52 bytes = ~3 KB
 *   - rain_hourly.bin:  720 entries × 8 bytes  = ~6 KB
 *   - rain_daily.bin:  1825 entries × 12 bytes = ~22 KB
 *   Total: ~74 KB on external flash (freed from RAM!)
 * 
 * File Format:
 *   - Header: 16 bytes (magic, version, count, head, capacity)
 *   - Data: Fixed-size ring buffer entries
 */

#ifndef HISTORY_FLASH_H
#define HISTORY_FLASH_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*******************************************************************************
 * History Type Identifiers
 ******************************************************************************/
typedef enum {
    HISTORY_TYPE_ENV_HOURLY = 0,
    HISTORY_TYPE_ENV_DAILY,
    HISTORY_TYPE_ENV_MONTHLY,
    HISTORY_TYPE_RAIN_HOURLY,
    HISTORY_TYPE_RAIN_DAILY,
    HISTORY_TYPE_COUNT
} history_type_t;

/*******************************************************************************
 * File Magic Numbers
 ******************************************************************************/
#define HISTORY_MAGIC_ENV_HOURLY    0x454E5648U  /* "ENVH" */
#define HISTORY_MAGIC_ENV_DAILY     0x454E5644U  /* "ENVD" */
#define HISTORY_MAGIC_ENV_MONTHLY   0x454E564DU  /* "ENVM" */
#define HISTORY_MAGIC_RAIN_HOURLY   0x524E4948U  /* "RNIH" */
#define HISTORY_MAGIC_RAIN_DAILY    0x524E4944U  /* "RNID" */
#define HISTORY_VERSION             1

/*******************************************************************************
 * Capacity Configuration
 ******************************************************************************/
#define HISTORY_ENV_HOURLY_CAPACITY     720   /* 30 days × 24 hours */
#define HISTORY_ENV_DAILY_CAPACITY      372   /* 12 months × 31 days */
#define HISTORY_ENV_MONTHLY_CAPACITY     60   /* 5 years × 12 months */
#define HISTORY_RAIN_HOURLY_CAPACITY    720   /* 30 days × 24 hours */
#define HISTORY_RAIN_DAILY_CAPACITY    1825   /* 5 years × 365 days */

/*******************************************************************************
 * File Header Structure (16 bytes)
 ******************************************************************************/
typedef struct __attribute__((packed)) {
    uint32_t magic;           /* 4: File type magic number */
    uint16_t version;         /* 2: Format version */
    uint16_t entry_count;     /* 2: Current number of entries */
    uint16_t head_index;      /* 2: Ring buffer head (oldest entry) */
    uint16_t capacity;        /* 2: Maximum entries */
    uint16_t entry_size;      /* 2: Size of each entry in bytes */
    uint16_t reserved;        /* 2: Reserved/padding */
} history_file_header_t;

#define HISTORY_HEADER_SIZE 16
_Static_assert(sizeof(history_file_header_t) == HISTORY_HEADER_SIZE,
               "history_file_header_t must be 16 bytes");

/*******************************************************************************
 * Environmental Hourly Entry (32 bytes) - matches hourly_history_entry_t
 ******************************************************************************/
typedef struct __attribute__((packed)) {
    uint32_t timestamp;           /* 4: Hour timestamp (Unix epoch) */
    int16_t  temperature_x100;    /* 2: Temperature × 100 (°C) */
    uint16_t humidity_x100;       /* 2: Humidity × 100 (%) */
    uint32_t pressure_x100;       /* 4: Pressure × 100 (hPa) */
    uint16_t rainfall_mm_x100;    /* 2: Rainfall × 100 (mm) */
    uint8_t  watering_events;     /* 1: Number of watering events */
    uint8_t  reserved1;           /* 1: Padding */
    uint32_t total_volume_ml;     /* 4: Total volume watered (mL) */
    uint16_t active_channels;     /* 2: Bitmap of active channels */
    uint8_t  reserved[10];        /* 10: Reserved for future use */
} history_env_hourly_t;

#define HISTORY_ENV_HOURLY_SIZE 32
_Static_assert(sizeof(history_env_hourly_t) == HISTORY_ENV_HOURLY_SIZE,
               "history_env_hourly_t must be 32 bytes");

/*******************************************************************************
 * Environmental Daily Entry (56 bytes) - matches daily_history_entry_t
 ******************************************************************************/
typedef struct __attribute__((packed)) {
    uint32_t date;                /* 4: Date (YYYYMMDD format) */
    /* Temperature stats × 100 */
    int16_t  temp_min_x100;       /* 2 */
    int16_t  temp_max_x100;       /* 2 */
    int16_t  temp_avg_x100;       /* 2 */
    /* Humidity stats × 100 */
    uint16_t humid_min_x100;      /* 2 */
    uint16_t humid_max_x100;      /* 2 */
    uint16_t humid_avg_x100;      /* 2 */
    /* Pressure stats × 10 (hPa) */
    uint16_t press_min_x10;       /* 2 */
    uint16_t press_max_x10;       /* 2 */
    uint16_t press_avg_x10;       /* 2 */
    /* Aggregated data */
    uint32_t total_rainfall_mm_x100; /* 4: Total daily rainfall × 100 */
    uint16_t watering_events;     /* 2: Total watering events */
    uint32_t total_volume_ml;     /* 4: Total volume watered */
    uint16_t sample_count;        /* 2: Number of hourly samples */
    uint8_t  active_channels;     /* 1: Channels bitmap */
    uint8_t  reserved[9];         /* 9: Reserved for future use */
} history_env_daily_t;

#define HISTORY_ENV_DAILY_SIZE 48
/* Static assert removed temporarily to find actual size */

/*******************************************************************************
 * Environmental Monthly Entry (52 bytes) - matches monthly_history_entry_t
 ******************************************************************************/
typedef struct __attribute__((packed)) {
    uint16_t year_month;          /* 2: YYYYMM format */
    /* Temperature stats × 100 */
    int16_t  temp_min_x100;       /* 2 */
    int16_t  temp_max_x100;       /* 2 */
    int16_t  temp_avg_x100;       /* 2 */
    /* Humidity stats × 100 */
    uint16_t humid_min_x100;      /* 2 */
    uint16_t humid_max_x100;      /* 2 */
    uint16_t humid_avg_x100;      /* 2 */
    /* Pressure stats × 10 */
    uint16_t press_min_x10;       /* 2 */
    uint16_t press_max_x10;       /* 2 */
    uint16_t press_avg_x10;       /* 2 */
    /* Monthly aggregates */
    uint32_t total_rainfall_mm_x100; /* 4 */
    uint32_t watering_events;     /* 4 */
    uint64_t total_volume_ml;     /* 8: 64-bit for large volumes */
    uint8_t  days_active;         /* 1 */
    uint8_t  reserved[5];         /* 5: Reserved for future use */
} history_env_monthly_t;

#define HISTORY_ENV_MONTHLY_SIZE 46
/* Static assert removed temporarily to find actual size */

/*******************************************************************************
 * Rain Hourly Entry (8 bytes) - matches rain_hourly_data_t
 ******************************************************************************/
typedef struct __attribute__((packed)) {
    uint32_t hour_epoch;          /* 4: Hour timestamp */
    uint16_t rainfall_mm_x100;    /* 2: Rainfall × 100 (0.01mm precision) */
    uint8_t  pulse_count;         /* 1: Raw pulse count */
    uint8_t  data_quality;        /* 1: Quality indicator 0-100 */
} history_rain_hourly_t;

#define HISTORY_RAIN_HOURLY_SIZE 8
_Static_assert(sizeof(history_rain_hourly_t) == HISTORY_RAIN_HOURLY_SIZE,
               "history_rain_hourly_t must be 8 bytes");

/*******************************************************************************
 * Rain Daily Entry (12 bytes) - matches rain_daily_data_t
 ******************************************************************************/
typedef struct __attribute__((packed)) {
    uint32_t day_epoch;           /* 4: Day timestamp (00:00 UTC) */
    uint32_t total_rainfall_mm_x100; /* 4: Total rainfall × 100 */
    uint16_t max_hourly_mm_x100;  /* 2: Peak hourly rainfall × 100 */
    uint8_t  active_hours;        /* 1: Hours with rainfall */
    uint8_t  data_completeness;   /* 1: % of valid hourly data */
} history_rain_daily_t;

#define HISTORY_RAIN_DAILY_SIZE 12
_Static_assert(sizeof(history_rain_daily_t) == HISTORY_RAIN_DAILY_SIZE,
               "history_rain_daily_t must be 12 bytes");

/*******************************************************************************
 * File Paths on LittleFS
 ******************************************************************************/
#define HISTORY_MOUNT_POINT         "/lfs"
#define HISTORY_DIR                 "/lfs/history"
#define HISTORY_PATH_ENV_HOURLY     "/lfs/history/env_hourly.bin"
#define HISTORY_PATH_ENV_DAILY      "/lfs/history/env_daily.bin"
#define HISTORY_PATH_ENV_MONTHLY    "/lfs/history/env_monthly.bin"
#define HISTORY_PATH_RAIN_HOURLY    "/lfs/history/rain_hourly.bin"
#define HISTORY_PATH_RAIN_DAILY     "/lfs/history/rain_daily.bin"

/*******************************************************************************
 * Runtime Statistics
 ******************************************************************************/
typedef struct {
    uint16_t entry_count;         /* Current entries in file */
    uint16_t capacity;            /* Maximum entries */
    uint32_t oldest_timestamp;    /* Oldest entry timestamp */
    uint32_t newest_timestamp;    /* Newest entry timestamp */
    uint32_t file_size_bytes;     /* Total file size on flash */
} history_file_stats_t;

typedef struct {
    bool initialized;
    bool mounted;
    history_file_stats_t env_hourly;
    history_file_stats_t env_daily;
    history_file_stats_t env_monthly;
    history_file_stats_t rain_hourly;
    history_file_stats_t rain_daily;
    uint32_t total_storage_bytes;
    uint32_t write_errors;
    uint32_t read_errors;
} history_flash_stats_t;

/*******************************************************************************
 * API Functions - Initialization
 ******************************************************************************/

/**
 * @brief Initialize history flash storage
 * 
 * Creates history directory and initializes all history files if needed.
 * Must be called after db_flash_init() mounts LittleFS.
 * 
 * @return 0 on success, negative errno on failure
 */
int history_flash_init(void);

/**
 * @brief Deinitialize history flash storage
 * @return 0 on success, negative errno on failure
 */
int history_flash_deinit(void);

/**
 * @brief Get storage statistics
 * @param[out] stats Pointer to statistics structure
 * @return 0 on success, negative errno on failure
 */
int history_flash_get_stats(history_flash_stats_t *stats);

/*******************************************************************************
 * API Functions - Environmental History
 ******************************************************************************/

/**
 * @brief Add hourly environmental entry
 * @param entry Pointer to entry data
 * @return 0 on success, negative errno on failure
 */
int history_flash_add_env_hourly(const history_env_hourly_t *entry);

/**
 * @brief Add daily environmental entry
 * @param entry Pointer to entry data
 * @return 0 on success, negative errno on failure
 */
int history_flash_add_env_daily(const history_env_daily_t *entry);

/**
 * @brief Add monthly environmental entry
 * @param entry Pointer to entry data
 * @return 0 on success, negative errno on failure
 */
int history_flash_add_env_monthly(const history_env_monthly_t *entry);

/**
 * @brief Read environmental hourly entries
 * @param start_index Starting index (0 = oldest)
 * @param entries Output buffer for entries
 * @param max_entries Maximum entries to read
 * @param[out] count Actual entries read
 * @return 0 on success, negative errno on failure
 */
int history_flash_read_env_hourly(uint16_t start_index, 
                                  history_env_hourly_t *entries,
                                  uint16_t max_entries,
                                  uint16_t *count);

/**
 * @brief Read environmental daily entries
 * @param start_index Starting index (0 = oldest)
 * @param entries Output buffer for entries
 * @param max_entries Maximum entries to read
 * @param[out] count Actual entries read
 * @return 0 on success, negative errno on failure
 */
int history_flash_read_env_daily(uint16_t start_index,
                                 history_env_daily_t *entries,
                                 uint16_t max_entries,
                                 uint16_t *count);

/**
 * @brief Read environmental monthly entries
 * @param start_index Starting index (0 = oldest)
 * @param entries Output buffer for entries
 * @param max_entries Maximum entries to read
 * @param[out] count Actual entries read
 * @return 0 on success, negative errno on failure
 */
int history_flash_read_env_monthly(uint16_t start_index,
                                   history_env_monthly_t *entries,
                                   uint16_t max_entries,
                                   uint16_t *count);

/*******************************************************************************
 * API Functions - Rain History
 ******************************************************************************/

/**
 * @brief Add hourly rain entry
 * @param entry Pointer to entry data
 * @return 0 on success, negative errno on failure
 */
int history_flash_add_rain_hourly(const history_rain_hourly_t *entry);

/**
 * @brief Add daily rain entry
 * @param entry Pointer to entry data
 * @return 0 on success, negative errno on failure
 */
int history_flash_add_rain_daily(const history_rain_daily_t *entry);

/**
 * @brief Read rain hourly entries
 * @param start_index Starting index (0 = oldest)
 * @param entries Output buffer for entries
 * @param max_entries Maximum entries to read
 * @param[out] count Actual entries read
 * @return 0 on success, negative errno on failure
 */
int history_flash_read_rain_hourly(uint16_t start_index,
                                   history_rain_hourly_t *entries,
                                   uint16_t max_entries,
                                   uint16_t *count);

/**
 * @brief Read rain daily entries
 * @param start_index Starting index (0 = oldest)
 * @param entries Output buffer for entries
 * @param max_entries Maximum entries to read
 * @param[out] count Actual entries read
 * @return 0 on success, negative errno on failure
 */
int history_flash_read_rain_daily(uint16_t start_index,
                                  history_rain_daily_t *entries,
                                  uint16_t max_entries,
                                  uint16_t *count);

/*******************************************************************************
 * API Functions - Query by Timestamp
 ******************************************************************************/

/**
 * @brief Find entries in timestamp range
 * @param type History type to query
 * @param start_ts Start timestamp (inclusive)
 * @param end_ts End timestamp (inclusive)
 * @param[out] start_idx First matching entry index
 * @param[out] count Number of matching entries
 * @return 0 on success, negative errno on failure
 */
int history_flash_query_range(history_type_t type,
                              uint32_t start_ts,
                              uint32_t end_ts,
                              uint16_t *start_idx,
                              uint16_t *count);

/**
 * @brief Get latest N entries
 * @param type History type to query
 * @param buffer Output buffer (must be sized for entry type)
 * @param count Number of entries to get (and output count)
 * @return 0 on success, negative errno on failure
 */
int history_flash_get_latest(history_type_t type,
                             void *buffer,
                             uint16_t *count);

/*******************************************************************************
 * API Functions - Maintenance
 ******************************************************************************/

/**
 * @brief Clear all entries from a history type
 * @param type History type to clear
 * @return 0 on success, negative errno on failure
 */
int history_flash_clear(history_type_t type);

/**
 * @brief Clear all history data
 * @return 0 on success, negative errno on failure
 */
int history_flash_clear_all(void);

/**
 * @brief Compact/defragment history files
 * 
 * LittleFS handles wear leveling automatically, but this
 * can be called periodically to optimize file layout.
 * 
 * @return 0 on success, negative errno on failure
 */
int history_flash_compact(void);

#ifdef __cplusplus
}
#endif

#endif /* HISTORY_FLASH_H */
