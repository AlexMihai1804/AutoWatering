#include "bme280_driver.h"

#include <zephyr/devicetree.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/logging/log.h>
#include <zephyr/pm/device.h>
#include <zephyr/pm/device_runtime.h>
#include <zephyr/sys/util.h>
#include <string.h>

LOG_MODULE_REGISTER(bme280_driver, LOG_LEVEL_INF);

#define BME280_DEFAULT_INTERVAL_SEC 60U

static bme280_device_t g_bme280_singleton;
static bme280_device_t *g_bme280_ref;
static bool g_bme280_initialized;

static inline bme280_config_t bme280_default_config(void)
{
    bme280_config_t cfg = {
        .measurement_interval = BME280_DEFAULT_INTERVAL_SEC,
        .initialized = true,
        .enabled = true,
    };
    return cfg;
}

static const struct device *resolve_sensor_device(void)
{
#if DT_NODE_HAS_STATUS(DT_NODELABEL(bme280), okay)
    return DEVICE_DT_GET(DT_NODELABEL(bme280));
#else
    return NULL;
#endif
}

static int bind_sensor(bme280_device_t *dev)
{
    const struct device *sensor = resolve_sensor_device();

    if (!sensor) {
        LOG_ERR("Missing devicetree node labelled 'bme280'");
        return -ENODEV;
    }

    if (!device_is_ready(sensor)) {
        LOG_ERR("BME280 device %s not ready", sensor->name);
        return -EBUSY;
    }

    dev->sensor_dev = sensor;
    return 0;
}

static void reset_reading(bme280_reading_t *reading)
{
    if (reading) {
        memset(reading, 0, sizeof(*reading));
        reading->valid = false;
    }
}

int bme280_init(bme280_device_t *dev, const struct device *i2c_dev, uint8_t addr)
{
    ARG_UNUSED(i2c_dev);
    ARG_UNUSED(addr);

    if (!dev) {
        return -EINVAL;
    }

    memset(dev, 0, sizeof(*dev));

    int ret = bind_sensor(dev);
    if (ret < 0) {
        return ret;
    }

    dev->config = bme280_default_config();
    dev->initialized = true;
    dev->last_measurement = 0;

    int pm_ret = pm_device_runtime_enable(dev->sensor_dev);
    if (pm_ret < 0 && pm_ret != -ENOTSUP) {
        LOG_WRN("Runtime PM enable failed for %s (%d)", dev->sensor_dev->name, pm_ret);
    }

    g_bme280_ref = dev;
    g_bme280_initialized = true;

    LOG_INF("BME280 ready via Zephyr sensor driver (%s)", dev->sensor_dev->name);
    return 0;
}

int bme280_configure(bme280_device_t *dev, const bme280_config_t *config)
{
    if (!dev || !config) {
        return -EINVAL;
    }

    if (!dev->initialized) {
        return -EACCES;
    }

    bme280_config_t sanitized = *config;
    if (sanitized.measurement_interval == 0) {
        sanitized.measurement_interval = dev->config.measurement_interval ?
                                         dev->config.measurement_interval :
                                         BME280_DEFAULT_INTERVAL_SEC;
    }

    sanitized.initialized = true;
    dev->config = sanitized;

    /* Keep singleton view in sync */
    if (g_bme280_ref == dev) {
        g_bme280_ref->config = sanitized;
    }

    return 0;
}

static int fetch_sample(const bme280_device_t *dev)
{
    int ret = pm_device_runtime_get(dev->sensor_dev);
    if (ret < 0) {
        LOG_ERR("Failed to resume BME280 device for fetch (%d)", ret);
        return ret;
    }

    ret = sensor_sample_fetch(dev->sensor_dev);
    if (ret < 0) {
        LOG_ERR("sensor_sample_fetch failed (%d)", ret);
    }

    int put_ret = pm_device_runtime_put(dev->sensor_dev);
    if (put_ret < 0) {
        LOG_WRN("Failed to release BME280 runtime PM handle (%d)", put_ret);
        if (ret >= 0) {
            ret = put_ret;
        }
    }

    return ret;
}

int bme280_read_data(bme280_device_t *dev, bme280_reading_t *reading)
{
    if (!dev || !reading) {
        return -EINVAL;
    }

    if (!dev->initialized) {
        reset_reading(reading);
        return -EACCES;
    }

    if (!dev->config.enabled) {
        reset_reading(reading);
        return -EACCES;
    }

    int ret = fetch_sample(dev);
    if (ret < 0) {
        reset_reading(reading);
        return ret;
    }

    struct sensor_value temperature;
    struct sensor_value humidity;
    struct sensor_value pressure;

    ret = sensor_channel_get(dev->sensor_dev, SENSOR_CHAN_AMBIENT_TEMP, &temperature);
    if (ret < 0) {
        LOG_ERR("Failed to read BME280 temperature channel (%d)", ret);
        reset_reading(reading);
        return ret;
    }

    ret = sensor_channel_get(dev->sensor_dev, SENSOR_CHAN_HUMIDITY, &humidity);
    if (ret < 0) {
        LOG_ERR("Failed to read BME280 humidity channel (%d)", ret);
        reset_reading(reading);
        return ret;
    }

    ret = sensor_channel_get(dev->sensor_dev, SENSOR_CHAN_PRESS, &pressure);
    if (ret < 0) {
        LOG_ERR("Failed to read BME280 pressure channel (%d)", ret);
        reset_reading(reading);
        return ret;
    }

    double temp_c = sensor_value_to_double(&temperature);
    double humidity_pct = sensor_value_to_double(&humidity);
    double pressure_kpa = sensor_value_to_double(&pressure);

    reading->temperature = (float)temp_c;
    reading->humidity = (float)humidity_pct;
    reading->pressure = (float)(pressure_kpa * 10.0); /* convert kPa to hPa */
    reading->timestamp = k_uptime_get_32();
    reading->valid = true;

    dev->last_measurement = reading->timestamp;

    LOG_DBG("BME280 reading: T=%.2f C, H=%.2f %%, P=%.2f hPa",
            temp_c, humidity_pct, pressure_kpa * 10.0);

    return 0;
}

int bme280_trigger_measurement(bme280_device_t *dev)
{
    if (!dev) {
        return -EINVAL;
    }

    if (!dev->initialized || !dev->config.enabled) {
        return -EACCES;
    }

    int ret = fetch_sample(dev);
    if (ret == 0) {
        dev->last_measurement = k_uptime_get_32();
    }
    return ret;
}

int bme280_get_config(bme280_config_t *config)
{
    if (!config) {
        return -EINVAL;
    }

    *config = bme280_default_config();
    return 0;
}

int bme280_system_init(const struct device *i2c_dev, uint8_t addr)
{
    if (g_bme280_initialized && g_bme280_ref) {
        return 0;
    }

    return bme280_init(&g_bme280_singleton, i2c_dev, addr);
}

int bme280_system_get_config(bme280_config_t *config)
{
    if (!config) {
        return -EINVAL;
    }

    if (!g_bme280_initialized || !g_bme280_ref) {
        *config = bme280_default_config();
        config->initialized = false;
        config->enabled = false;
        return 0;
    }

    *config = g_bme280_ref->config;
    return 0;
}

int bme280_system_read_data(bme280_reading_t *reading)
{
    if (!g_bme280_initialized || !g_bme280_ref) {
        return -ENODEV;
    }
    return bme280_read_data(g_bme280_ref, reading);
}

int bme280_system_trigger_measurement(void)
{
    if (!g_bme280_initialized || !g_bme280_ref) {
        return -ENODEV;
    }
    return bme280_trigger_measurement(g_bme280_ref);
}
