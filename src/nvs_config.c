#include "nvs_config.h"
#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>    // for DT_MTD_FROM_FIXED_PARTITION, DEVICE_DT_GET
#include <zephyr/drivers/flash.h>
#include <string.h>   /* ← NEW */
#include <errno.h>    /* for EINVAL */

/* ——— instanţă NVS —————————————————————————————————————— */
static struct nvs_fs fs;
static bool nvs_ready;

/* ——— iniţializare ————————————————————————————————————— */
int nvs_config_init(void)
{
    const struct device *flash_dev =
        DEVICE_DT_GET(DT_MTD_FROM_FIXED_PARTITION(NVS_DT_NODE));
    if (!device_is_ready(flash_dev)) {
        printk("NVS flash device not ready\n");
        return -ENODEV;
    }

    /* assign device before mounting */
    fs.offset       = NVS_OFFSET;
    fs.sector_size  = NVS_SECTOR_SIZE;
    fs.sector_count = NVS_SECTOR_COUNT;
    fs.flash_device = flash_dev;          /* <—— fix esenţial */

    int rc = nvs_mount(&fs);
    if (rc == 0) {
        nvs_ready = true;
    } else {
        printk("NVS mount failed (%d)\n", rc);
    }
    return rc;
}

bool nvs_config_is_ready(void)
{
    return nvs_ready;
}

/* ——— RAW read/write/delete ————————————————————————————— */
int nvs_config_read(uint16_t id, void *data, size_t len)
{
    return nvs_ready ? nvs_read (&fs, id, data, len) : -ENODEV;
}

int nvs_config_write(uint16_t id, const void *data, size_t len)
{
    return nvs_ready ? nvs_write(&fs, id, data, len) : -ENODEV;
}

int nvs_config_delete(uint16_t id)
{
    return nvs_ready ? nvs_delete(&fs, id) : -ENODEV;
}

/* ——— IDs logice ——————————————————————————————————————— */
enum {
    ID_WATERING_CFG     =  1,
    ID_CHANNEL_CFG_BASE = 100,   /* +ch (0-7) */
    ID_FLOW_CALIB       = 200,
    ID_DAYS_SINCE_START = 201,
    ID_CHANNEL_NAME_BASE= 300,   /* +ch (0-7)  ← NEW */
};

/* ——— nivel înalt —————————————————————————————————————— */
int nvs_save_watering_config(const void *cfg, size_t sz)
{
    return nvs_config_write(ID_WATERING_CFG, cfg, sz);
}

int nvs_load_watering_config(void *cfg, size_t sz)
{
    return nvs_config_read(ID_WATERING_CFG, cfg, sz);
}

int nvs_save_channel_config(uint8_t ch, const void *cfg, size_t sz)
{
    return nvs_config_write(ID_CHANNEL_CFG_BASE + ch, cfg, sz);
}

int nvs_load_channel_config(uint8_t ch, void *cfg, size_t sz)
{
    return nvs_config_read(ID_CHANNEL_CFG_BASE + ch, cfg, sz);
}

int nvs_save_flow_calibration(uint32_t cal)
{
    return nvs_config_write(ID_FLOW_CALIB, &cal, sizeof(cal));
}

int nvs_load_flow_calibration(uint32_t *cal)
{
    return nvs_config_read(ID_FLOW_CALIB, cal, sizeof(*cal));
}

int nvs_save_days_since_start(uint16_t days)
{
    return nvs_config_write(ID_DAYS_SINCE_START, &days, sizeof(days));
}

int nvs_load_days_since_start(uint16_t *days)
{
    return nvs_config_read(ID_DAYS_SINCE_START, days, sizeof(*days));
}

int nvs_save_channel_name(uint8_t ch, const char *name)
{
    if (!name) {
        /* Save empty string */
        return nvs_config_write(ID_CHANNEL_NAME_BASE + ch, "", 1);
    }
    size_t len = strnlen(name, 63) + 1;   /* include '\0', max 64 chars */
    return nvs_config_write(ID_CHANNEL_NAME_BASE + ch, name, len);
}

int nvs_load_channel_name(uint8_t ch, char *buf, size_t sz)
{
    if (!buf || sz == 0) return -EINVAL;
    
    /* Initialize buffer to empty string */
    buf[0] = '\0';
    
    int ret = nvs_config_read(ID_CHANNEL_NAME_BASE + ch, buf, sz - 1);  /* leave space for '\0' */
    if (ret >= 0) {
        if (ret < sz - 1) {
            buf[ret] = '\0';  /* ensure NUL termination */
        } else {
            buf[sz - 1] = '\0';  /* truncate if needed */
        }
    }
    return ret;
}