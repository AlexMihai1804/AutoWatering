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

#endif /* NVS_CONFIG_H */
