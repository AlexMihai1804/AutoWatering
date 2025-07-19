#ifndef NVS_CONFIG_H
#define NVS_CONFIG_H

/**
 * @file nvs_config.h
 * @brief Declaraţii pentru accesul în NVS (Non-Volatile Storage)
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <zephyr/devicetree.h>            // for DT_ALIAS, DT_REG_*
#include <zephyr/storage/flash_map.h>
#include <zephyr/fs/nvs.h>

#define NVS_SECTOR_SIZE    4096u
#define NVS_DT_NODE        DT_ALIAS(nvs_storage)
#define NVS_OFFSET         DT_REG_ADDR(NVS_DT_NODE)
#define NVS_SIZE           DT_REG_SIZE(NVS_DT_NODE)
/* calculate automatically from DT */
#define NVS_SECTOR_COUNT   (NVS_SIZE / NVS_SECTOR_SIZE)

/* ——— Timezone Configuration ——————————————————————————— */
typedef struct {
    int16_t utc_offset_minutes;  /**< UTC offset in minutes (e.g., 120 for UTC+2) */
    uint8_t dst_enabled;         /**< 1 if DST is enabled, 0 otherwise */
    uint8_t dst_start_month;     /**< DST start month (1-12) */
    uint8_t dst_start_week;      /**< DST start week of month (1-5, 5=last) */
    uint8_t dst_start_dow;       /**< DST start day of week (0=Sunday, 1=Monday, etc.) */
    uint8_t dst_end_month;       /**< DST end month (1-12) */
    uint8_t dst_end_week;        /**< DST end week of month (1-5, 5=last) */
    uint8_t dst_end_dow;         /**< DST end day of week (0=Sunday, 1=Monday, etc.) */
    int16_t dst_offset_minutes;  /**< DST offset in minutes (usually 60) */
    uint8_t reserved[5];         /**< Reserved for future use */
} __attribute__((packed)) timezone_config_t;

/* Default timezone configuration for Romania (UTC+2, DST enabled) */
#define DEFAULT_TIMEZONE_CONFIG { \
    .utc_offset_minutes = 120,    /* UTC+2 */ \
    .dst_enabled = 1,             /* DST enabled */ \
    .dst_start_month = 3,         /* March */ \
    .dst_start_week = 5,          /* Last week */ \
    .dst_start_dow = 0,           /* Sunday */ \
    .dst_end_month = 10,          /* October */ \
    .dst_end_week = 5,            /* Last week */ \
    .dst_end_dow = 0,             /* Sunday */ \
    .dst_offset_minutes = 60,     /* +1 hour */ \
    .reserved = {0} \
}

/* ——— API public ——————————————————————————————————————— */
int  nvs_config_init(void);
bool nvs_config_is_ready(void);

int  nvs_config_read (uint16_t id, void *data, size_t len);
int  nvs_config_write(uint16_t id, const void *data, size_t len);
int  nvs_config_delete(uint16_t id);

/* ——— convenienţe de nivel înalt ————————————————————— */
int nvs_save_watering_config (const void *cfg,  size_t sz);
int nvs_load_watering_config (void *cfg,        size_t sz);

int nvs_save_channel_config  (uint8_t ch, const void *cfg, size_t sz);
int nvs_load_channel_config  (uint8_t ch,       void *cfg, size_t sz);

int nvs_save_flow_calibration(uint32_t cal);
int nvs_load_flow_calibration(uint32_t *cal);

int nvs_save_days_since_start(uint16_t days);
int nvs_load_days_since_start(uint16_t *days);

int nvs_save_channel_name (uint8_t ch, const char *name);
int nvs_load_channel_name (uint8_t ch, char *name_buf, size_t buf_sz);

/* ——— Timezone configuration functions ————————————————— */
int nvs_save_timezone_config(const timezone_config_t *tz_config);
int nvs_load_timezone_config(timezone_config_t *tz_config);

#endif /* NVS_CONFIG_H */
