#ifndef FLOW_SENSOR_H
#define FLOW_SENSOR_H
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>

/**
 * @file flow_sensor.h
 * @brief Interface for the water flow sensor
 * 
 * This header defines the public API for interfacing with the flow sensor
 * that measures water consumption.
 */

/**
 * @brief Initialize the flow sensor hardware
 * 
 * Sets up GPIO and interrupt handlers for the flow sensor pulse detection.
 */
void flow_sensor_init(void);

/**
 * @brief Get the current pulse count from the flow sensor
 * 
 * @return Number of pulses detected since last reset
 */
uint32_t get_pulse_count(void);

/**
 * @brief Reset the pulse counter to zero
 */
void reset_pulse_count(void);

#endif // FLOW_SENSOR_H
