#ifndef BME280_DRIVER_H
#define BME280_DRIVER_H

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include "watering_enhanced.h"

/**
 * @file bme280_driver.h
 * @brief Thin wrapper around Zephyr's BME280 sensor driver.
 *
 * The legacy project maintained its own register-level BME680/BME280 driver.
 * This header now exposes a lightweight wrapper that forwards reads to
 * Zephyr's `sensor` API and keeps only the configuration that is still
 * relevant for the BME280 part.
 */

typedef struct {
    const struct device *sensor_dev;  // Bound Zephyr device instance
    bme280_config_t config;           // Cached configuration
    bool initialized;                 // Driver state flag
    uint32_t last_measurement;        // Timestamp of last successful sample
} bme280_device_t;

int bme280_init(bme280_device_t *dev, const struct device *i2c_dev, uint8_t addr);
int bme280_configure(bme280_device_t *dev, const bme280_config_t *config);
int bme280_read_data(bme280_device_t *dev, bme280_reading_t *reading);
int bme280_trigger_measurement(bme280_device_t *dev);
int bme280_get_config(bme280_config_t *config);

int bme280_system_init(const struct device *i2c_dev, uint8_t addr);
int bme280_system_get_config(bme280_config_t *config);
int bme280_system_read_data(bme280_reading_t *reading);
int bme280_system_trigger_measurement(void);

#endif /* BME280_DRIVER_H */
