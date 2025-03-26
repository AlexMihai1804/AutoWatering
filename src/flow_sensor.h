#ifndef FLOW_SENSOR_H
#define FLOW_SENSOR_H

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>

/**
 * @brief Initialize the flow sensor.
 */
void flow_sensor_init(void);

/**
 * @brief Returns the number of measured pulses.
 */
uint32_t get_pulse_count(void);

/**
 * @brief Reset the pulse counter.
 */
void reset_pulse_count(void);

#endif // FLOW_SENSOR_H
