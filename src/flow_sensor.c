#include "flow_sensor.h"
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#define FLOW_SENSOR_NODE DT_NODELABEL(water_flow_sensor)
static const struct gpio_dt_spec flow_sensor = GPIO_DT_SPEC_GET(FLOW_SENSOR_NODE, gpios);

volatile uint32_t pulse_count = 0;
static uint32_t last_interrupt_time = 0;
#define DEBOUNCE_MS 2

static struct gpio_callback flow_sensor_cb;
static K_MUTEX_DEFINE(pulse_count_mutex);

static void flow_sensor_callback(const struct device *dev, struct gpio_callback *cb, uint32_t pins) {
    uint32_t now = k_uptime_get_32();
    if ((now - last_interrupt_time) > DEBOUNCE_MS) {
        last_interrupt_time = now;
        k_mutex_lock(&pulse_count_mutex, K_NO_WAIT); // Don't block in IRQ context
        pulse_count++;
        k_mutex_unlock(&pulse_count_mutex);
    }
}

void flow_sensor_init(void) {
    int ret;

    if (!device_is_ready(flow_sensor.port)) {
        printk("GPIO device for sensor is not ready!\n");
        return;
    }

    ret = gpio_pin_configure_dt(&flow_sensor, GPIO_INPUT);
    if (ret < 0) {
        printk("Error configuring pin: %d\n", ret);
        return;
    }

    ret = gpio_pin_interrupt_configure_dt(&flow_sensor, GPIO_INT_EDGE_RISING);
    if (ret < 0) {
        printk("Error configuring interrupt: %d\n", ret);
        return;
    }

    gpio_init_callback(&flow_sensor_cb, flow_sensor_callback, BIT(flow_sensor.pin));
    gpio_add_callback(flow_sensor.port, &flow_sensor_cb);

    printk("Flow sensor started\n");
}

uint32_t get_pulse_count(void) {
    uint32_t count;
    k_mutex_lock(&pulse_count_mutex, K_FOREVER);
    count = pulse_count;
    k_mutex_unlock(&pulse_count_mutex);
    return count;
}

void reset_pulse_count(void) {
    k_mutex_lock(&pulse_count_mutex, K_FOREVER);
    pulse_count = 0;
    k_mutex_unlock(&pulse_count_mutex);
}
