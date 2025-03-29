#include "flow_sensor.h"
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/printk.h>

/**
 * @file flow_sensor.c
 * @brief Implementation of flow sensor pulse counting
 * 
 * This file implements the interface to a pulse-based flow sensor,
 * counting pulses to measure water flow in real-time.
 */

/** Devicetree node label for water flow sensor */
#define FLOW_SENSOR_NODE DT_NODELABEL(water_flow_sensor)

/** GPIO specification for flow sensor from devicetree */
static const struct gpio_dt_spec flow_sensor = GPIO_DT_SPEC_GET(FLOW_SENSOR_NODE, gpios);

/** Current pulse count from flow sensor */
volatile uint32_t pulse_count = 0;

/** Timestamp of last interrupt for debounce */
static uint32_t last_interrupt_time = 0;

/** Minimum milliseconds between pulses (debounce) */
#define DEBOUNCE_MS 2

/** GPIO interrupt callback structure */
static struct gpio_callback flow_sensor_cb;

/** Mutex for protecting access to pulse counter */
static K_MUTEX_DEFINE(pulse_count_mutex);

/**
 * @brief Interrupt handler for flow sensor pulses
 * 
 * @param dev GPIO device that triggered the interrupt
 * @param cb Pointer to callback data
 * @param pins Bitmask of pins that triggered the interrupt
 */
static void flow_sensor_callback(const struct device *dev, struct gpio_callback *cb, uint32_t pins) {
    uint32_t now = k_uptime_get_32();
    
    // Simple debouncing - ignore pulses that come too quickly
    if ((now - last_interrupt_time) > DEBOUNCE_MS) {
        last_interrupt_time = now;
        k_mutex_lock(&pulse_count_mutex, K_NO_WAIT);
        pulse_count++;
        k_mutex_unlock(&pulse_count_mutex);
    }
}

/**
 * @brief Initialize the flow sensor hardware and interrupts
 */
void flow_sensor_init(void) {
    int ret;
    static bool initialized = false;
    
    // Only initialize once
    if (initialized) {
        return;
    }
    
    // Check if GPIO device is ready
    if (!device_is_ready(flow_sensor.port)) {
        printk("GPIO device for sensor is not ready!\n");
        return;
    }
    
    // Configure GPIO pin as input
    ret = gpio_pin_configure_dt(&flow_sensor, GPIO_INPUT);
    if (ret < 0) {
        printk("Error configuring pin: %d\n", ret);
        return;
    }
    
    // Configure GPIO interrupt on rising edge
    ret = gpio_pin_interrupt_configure_dt(&flow_sensor, GPIO_INT_EDGE_RISING);
    if (ret < 0) {
        printk("Error configuring interrupt: %d\n", ret);
        return;
    }
    
    // Set up callback for GPIO interrupt
    gpio_init_callback(&flow_sensor_cb, flow_sensor_callback, BIT(flow_sensor.pin));
    gpio_add_callback(flow_sensor.port, &flow_sensor_cb);
    
    initialized = true;
    printk("Flow sensor started on pin %d\n", flow_sensor.pin);
}

/**
 * @brief Get the current flow sensor pulse count
 * 
 * @return Number of pulses counted since last reset
 */
uint32_t get_pulse_count(void) {
    uint32_t count;
    
    k_mutex_lock(&pulse_count_mutex, K_FOREVER);
    count = pulse_count;
    k_mutex_unlock(&pulse_count_mutex);
    
    return count;
}

/**
 * @brief Reset the flow sensor pulse counter to zero
 */
void reset_pulse_count(void) {
    k_mutex_lock(&pulse_count_mutex, K_FOREVER);
    pulse_count = 0;
    k_mutex_unlock(&pulse_count_mutex);
}
