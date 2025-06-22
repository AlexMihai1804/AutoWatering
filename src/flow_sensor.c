#include "flow_sensor.h"
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/atomic.h>      /* NEW */
#include "bt_irrigation_service.h"   /* for bt_irrigation_flow_update */

/**
 * @file flow_sensor.c
 * @brief Implementation of flow sensor pulse counting
 * 
 * This file implements the interface to a pulse-based flow sensor,
 * counting pulses to measure water flow in real-time.
 */

/** Devicetree node label for water flow sensor */
#define FLOW_SENSOR_NODE        DT_NODELABEL(water_flow_sensor)
/** child named “flow_key” directly under the sensor node */
#define FLOW_SENSOR_GPIO_NODE   DT_CHILD(FLOW_SENSOR_NODE, flow_key)
#define SENSOR_CONFIG_NODE DT_NODELABEL(sensor_config)

/* pick flow_calibration from DT or default to 450 */
#define FLOW_CALIB_DT DT_PROP_OR(SENSOR_CONFIG_NODE, flow_calibration, 450)

/** GPIO specification for flow sensor from devicetree */
static const struct gpio_dt_spec flow_sensor =
    GPIO_DT_SPEC_GET(FLOW_SENSOR_GPIO_NODE, gpios);

/* ---------- data ---------------------------------------------------------- */
static atomic_t pulse_count = ATOMIC_INIT(0);   /* REPLACES volatile + mutex */
static uint32_t last_notified          = 0;   /* last value sent over BLE   */
static uint32_t last_flow_notify_time  = 0;   /* k_uptime at last BLE notif */

/** Timestamp of last interrupt for debounce */
static uint32_t last_interrupt_time = 0;

/** Minimum milliseconds between pulses (debounce) - get from devicetree if available */
#if DT_NODE_HAS_PROP(SENSOR_CONFIG_NODE, debounce_ms)
  #define DEBOUNCE_MS DT_PROP(SENSOR_CONFIG_NODE, debounce_ms)
#else
  #define DEBOUNCE_MS 2
#endif

/** GPIO interrupt callback structure */
static struct gpio_callback flow_sensor_cb;

/* Notification-throttling parameters (tune as needed) */
#define FLOW_NOTIFY_PULSE_STEP        10     /* min. extra pulses before notify */
#define FLOW_NOTIFY_MIN_INTERVAL_MS  500     /* max. 2 Hz notification rate   */

/* ---------- BLE work handler (no mutex needed) ---------------------------- */
static void flow_update_work_handler(struct k_work *work)
{
    uint32_t cnt = atomic_get(&pulse_count);
    uint32_t now = k_uptime_get_32();

    /* send only if enough new pulses OR time interval elapsed */
    if ((cnt - last_notified >= FLOW_NOTIFY_PULSE_STEP) ||
        (now - last_flow_notify_time >= FLOW_NOTIFY_MIN_INTERVAL_MS)) {

        last_notified         = cnt;
        last_flow_notify_time = now;
        /* ignore -ENOTCONN etc. */
        bt_irrigation_flow_update(cnt);
    }
}
static struct k_work flow_update_work;
/* ----------------------------------------------------------- */

/**
 * @brief Interrupt handler for flow sensor pulses
 * 
 * @param dev GPIO device that triggered the interrupt
 * @param cb Pointer to callback data
 * @param pins Bitmask of pins that triggered the interrupt
 */
static void flow_sensor_callback(const struct device *dev,
                                 struct gpio_callback *cb, uint32_t pins)
{
    uint32_t now = k_uptime_get_32();

    if ((now - last_interrupt_time) > DEBOUNCE_MS) {
        last_interrupt_time = now;
        atomic_inc(&pulse_count);

        /* schedule work only if not already pending */
        if (!k_work_is_pending(&flow_update_work)) {
            k_work_submit(&flow_update_work);
        }
    }
}

/**
 * @brief Initialize the flow sensor hardware and interrupts
 * 
 * @return 0 on success, negative error code on failure
 */
int flow_sensor_init(void) {
    int ret;
    static bool initialized = false;

    if (initialized) {
        return 0;
    }

    /* Verify GPIO device is ready */
    if (!device_is_ready(flow_sensor.port)) {
        printk("ERROR: GPIO device not ready!\n");
        return -ENODEV;
    }

    /* Configure GPIO pin for sensor input */
    ret = gpio_pin_configure_dt(&flow_sensor, GPIO_INPUT | GPIO_PULL_UP);
    if (ret < 0) {
        printk("ERROR: Failed to configure GPIO pin: %d\n", ret);
        return ret;
    }
    
    /* Enable interrupt on rising edge */
    ret = gpio_pin_interrupt_configure_dt(&flow_sensor, GPIO_INT_EDGE_RISING);
    if (ret < 0) {
        printk("ERROR: Failed to configure GPIO interrupt: %d\n", ret);
        return ret;
    }
    
    /* Register interrupt callback */
    gpio_init_callback(&flow_sensor_cb, flow_sensor_callback, BIT(flow_sensor.pin));
    
    ret = gpio_add_callback(flow_sensor.port, &flow_sensor_cb);
    if (ret < 0) {
        printk("ERROR: Failed to add GPIO callback: %d\n", ret);
        return ret;
    }
    
    /* init work item for BLE notifications */
    k_work_init(&flow_update_work, flow_update_work_handler);
    initialized = true;
    return 0;
}

/**
 * @brief Get the current flow sensor pulse count
 * 
 * @return Number of pulses counted since last reset
 */
uint32_t get_pulse_count(void)
{
    return atomic_get(&pulse_count);          /* no locking required */
}

/**
 * @brief Reset the flow sensor pulse counter to zero
 */
void reset_pulse_count(void)
{
    atomic_set(&pulse_count, 0);
}
