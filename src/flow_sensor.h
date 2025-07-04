#ifndef FLOW_SENSOR_H
#define FLOW_SENSOR_H
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>   /* needed by implementation */

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
 * 
 * @return 0 on success, negative error code on failure
 */
int flow_sensor_init(void);

/**
 * @brief Get the current pulse count from the flow sensor
 * 
 * @return Number of pulses detected since last reset
 */
uint32_t get_pulse_count(void);

/**
 * @brief Get the current smoothed flow rate
 * 
 * @return Smoothed flow rate in pulses per second
 */
uint32_t get_flow_rate(void);

/**
 * @brief Reset the pulse counter to zero
 */
void reset_pulse_count(void);

/**
 * @brief Print flow sensor debug information
 */
void flow_sensor_debug_info(void);

#endif // FLOW_SENSOR_H
