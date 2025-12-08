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

/**
 * @brief Get the current flow calibration value
 * 
 * @return Calibration value in pulses per liter
 */
uint32_t get_flow_calibration(void);

/**
 * @brief Set the flow calibration value
 * 
 * @param pulses_per_liter Calibration value in pulses per liter
 * @return 0 on success, negative error code on failure
 */
int set_flow_calibration(uint32_t pulses_per_liter);

/**
 * @brief Set flow calibration without persisting or updating onboarding flags
 * 
 * Intended for boot/load paths so onboarding completion stays at 0% after reset.
 * Validation rules are identical to set_flow_calibration.
 *
 * @param pulses_per_liter Calibration value in pulses per liter
 * @return 0 on success, negative error code on failure
 */
int set_flow_calibration_in_memory(uint32_t pulses_per_liter);

#endif // FLOW_SENSOR_H
