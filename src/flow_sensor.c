#include "flow_sensor.h"
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/logging/log.h>     /* Add logging support */
#include "bt_irrigation_service.h"   /* for bt_irrigation_flow_update */
#include "watering_internal.h"       /* for active task information */
#include "nvs_config.h"              /* for persistent storage */

LOG_MODULE_REGISTER(flow_sensor, CONFIG_LOG_DEFAULT_LEVEL);

/**
 * @file flow_sensor.c
 * @brief Implementation of flow sensor pulse counting
 * 
 * This file implements the interface to a pulse-based flow sensor,
 * counting pulses to measure water flow in real-time.
 */

/* NVS key for flow calibration */
#define NVS_KEY_FLOW_CALIBRATION  1000

/** Devicetree node label for water flow sensor */
#define FLOW_SENSOR_NODE        DT_NODELABEL(water_flow_sensor)
/** child named “flow_key” directly under the sensor node */
#define FLOW_SENSOR_GPIO_NODE   DT_CHILD(FLOW_SENSOR_NODE, flow_key)
#define SENSOR_CONFIG_NODE DT_NODELABEL(sensor_config)

/* pick flow_calibration from DT or default to 450 */
#define FLOW_CALIB_DT DT_PROP_OR(SENSOR_CONFIG_NODE, flow_calibration, 450)

/* Current flow calibration value (can be updated via BLE) */
static uint32_t current_flow_calibration = FLOW_CALIB_DT;

/** GPIO specification for flow sensor from devicetree */
static const struct gpio_dt_spec flow_sensor =
    GPIO_DT_SPEC_GET(FLOW_SENSOR_GPIO_NODE, gpios);

/* ---------- data ---------------------------------------------------------- */
static atomic_t pulse_count = ATOMIC_INIT(0);   /* REPLACES volatile + mutex */
static uint32_t last_notified          = 0;   /* last value sent over BLE   */
static uint32_t last_flow_notify_time  = 0;   /* k_uptime at last BLE notif */

/** Timestamp of last interrupt for debounce */
static uint32_t last_interrupt_time = 0;

/** Minimum milliseconds between pulses (debounce) - increased for stability */
#if DT_NODE_HAS_PROP(SENSOR_CONFIG_NODE, debounce_ms)
  #define DEBOUNCE_MS DT_PROP(SENSOR_CONFIG_NODE, debounce_ms)
#else
  #define DEBOUNCE_MS 5  /* Increased from 2ms to 5ms for better noise rejection */
#endif

/** GPIO interrupt callback structure */
static struct gpio_callback flow_sensor_cb;

/* Notification-throttling parameters for stable flow readings */
#define FLOW_NOTIFY_PULSE_STEP        1      /* min. extra pulses before notify - ultra responsive */
#define FLOW_NOTIFY_MIN_INTERVAL_MS  50      /* 20 Hz notification rate - very high frequency */
#define FLOW_RATE_WINDOW_MS          500     /* 0.5 second window for flow rate calculation - ultra fast response */
#define MIN_PULSES_FOR_RATE         1        /* minimum pulses before calculating rate - ultra low threshold */

/* Flow rate smoothing variables */
static uint32_t flow_rate_samples[2] = {0};  /* circular buffer for 2 samples - minimal smoothing */
static uint8_t sample_index = 0;
static uint32_t smoothed_flow_rate = 0;

/* Calculate smoothed flow rate from recent samples */
static uint32_t calculate_smoothed_flow_rate(uint32_t current_rate) {
    flow_rate_samples[sample_index] = current_rate;
    sample_index = (sample_index + 1) % 2;  /* Updated for 2 samples */
    
    uint32_t sum = 0;
    uint8_t valid_samples = 0;
    
    for (int i = 0; i < 2; i++) {  /* Updated for 2 samples */
        if (flow_rate_samples[i] > 0) {
            sum += flow_rate_samples[i];
            valid_samples++;
        }
    }
    
    return valid_samples > 0 ? sum / valid_samples : 0;
}

/* ---------- BLE work handler with flow rate stabilization ---------------- */
static void flow_update_work_handler(struct k_work *work)
{
    uint32_t cnt = atomic_get(&pulse_count);
    uint32_t now = k_uptime_get_32();
    static uint32_t last_rate_calc_time = 0;
    static uint32_t last_rate_calc_pulses = 0;

    /* Calculate flow rate on a short window so low flows are still reported */
    if ((now - last_rate_calc_time) >= 2000) {  /* 2 second window */
        uint32_t pulse_diff = cnt - last_rate_calc_pulses;
        uint32_t time_diff_ms = now - last_rate_calc_time;
        
        if (pulse_diff >= 1 && time_diff_ms > 0) {  /* accept very low flow */
            /* Calculate pulses per second, then smooth it */
            uint32_t current_rate = (pulse_diff * 1000) / time_diff_ms;
            smoothed_flow_rate = calculate_smoothed_flow_rate(current_rate);
            /* Only log significant flow rates to avoid spam */
            if (current_rate > 10) {  /* Only log if >10 pps */
                LOG_DBG("Flow rate calculated: %u pps (from %u pulses in %u ms)", 
                       current_rate, pulse_diff, time_diff_ms);
            }
        } else if (pulse_diff == 0) {
            /* No flow detected */
            smoothed_flow_rate = calculate_smoothed_flow_rate(0);
        }
        
        last_rate_calc_time = now;
        last_rate_calc_pulses = cnt;
    }

    /* Send notifications on moderate changes or periodic heartbeat */
    bool significant_change = (cnt - last_notified >= 10);      /* Every 10 pulses */
    bool time_interval_reached = (now - last_flow_notify_time >= 5000);  /* Every 5 seconds */
    
    if (significant_change || time_interval_reached) {
        last_notified = cnt;
        last_flow_notify_time = now;
        
        /* Send smoothed flow rate for stable readings */
        /* Only log when there's actual flow activity */
        if (smoothed_flow_rate > 0) {
            LOG_DBG("BLE update: sending smoothed rate %u pps (total pulses: %u)", smoothed_flow_rate, cnt);
        }
        bt_irrigation_flow_update(smoothed_flow_rate);
        
        /* Update statistics if there's an active task - but much less frequently */
        if (watering_task_state.task_in_progress && watering_task_state.current_active_task) {
            uint8_t channel_id = watering_task_state.current_active_task->channel - watering_channels;
            if (channel_id < WATERING_CHANNELS_COUNT) {
                /* Calculate volume from pulse count (assuming calibration) */
                uint32_t calib = get_flow_calibration();
                if (calib == 0) {
                    calib = FLOW_CALIB_DT;
                }
                uint32_t volume_ml = cnt * 1000 / calib; // Convert pulses to ml
                bt_irrigation_update_statistics_from_flow(channel_id, volume_ml);
            }
        }
    }
}
static struct k_work flow_update_work;

/* Periodic timer for forcing BLE updates */
static struct k_timer periodic_ble_timer;

/* Timer handler to force periodic BLE updates */
static void periodic_ble_timer_handler(struct k_timer *timer)
{
    if (!k_work_is_pending(&flow_update_work)) {
        k_work_submit(&flow_update_work);
        /* Only log if there's actual flow activity */
        if (smoothed_flow_rate > 0) {
            LOG_DBG("Periodic BLE update triggered (flow active: %u pps)", smoothed_flow_rate);
        }
    }
}

/* ----------------------------------------------------------- */

/**
 * @brief Interrupt handler for flow sensor pulses with enhanced debouncing
 * 
 * @param dev GPIO device that triggered the interrupt
 * @param cb Pointer to callback data
 * @param pins Bitmask of pins that triggered the interrupt
 */
static void flow_sensor_callback(const struct device *dev,
                                 struct gpio_callback *cb, uint32_t pins)
{
    uint32_t now = k_uptime_get_32();
    static uint32_t pulse_count_at_last_work = 0;

    /* Enhanced debouncing - require longer gap between pulses */
    if ((now - last_interrupt_time) > DEBOUNCE_MS) {
        last_interrupt_time = now;
        atomic_inc(&pulse_count);

    /* Submit work on small batches to keep BLE updates responsive */
    uint32_t current_count = atomic_get(&pulse_count);
    static uint32_t last_work_submit_time = 0;
    if ((current_count - pulse_count_at_last_work) >= 5 || (now - last_work_submit_time) >= 3000) {  /* Every 5 pulses or 3s */
        if (!k_work_is_pending(&flow_update_work)) {
            pulse_count_at_last_work = current_count;
            last_work_submit_time = now;
            k_work_submit(&flow_update_work);
            /* Only log every 100th pulse to avoid spam */
            if (current_count % 100 == 0) {
                LOG_DBG("Flow pulse: %u (submitted work)", current_count);
            }
        }
    } else {
        /* Only log every 100th pulse to avoid spam */
        if (current_count % 100 == 0) {
            LOG_DBG("Flow pulse: %u", current_count);
        }
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

    /* Moderate periodic timer to ensure slow flows still report */
    k_timer_init(&periodic_ble_timer, periodic_ble_timer_handler, NULL);
    k_timer_start(&periodic_ble_timer, K_SECONDS(5), K_SECONDS(5));
    LOG_INF("Flow sensor: periodic timer enabled (5s heartbeat)");
    
    /* Load flow calibration from persistent storage */
    uint32_t saved_calibration;
    int nvs_ret = nvs_config_read(NVS_KEY_FLOW_CALIBRATION, &saved_calibration, sizeof(saved_calibration));
    if (nvs_ret == sizeof(saved_calibration)) {
        /* Validate loaded calibration */
        if (saved_calibration >= 100 && saved_calibration <= 10000) {
            current_flow_calibration = saved_calibration;
            LOG_INF("Flow calibration loaded from persistent storage: %u pulses/L", current_flow_calibration);
        } else {
            LOG_WRN("Invalid calibration loaded (%u), using default %u", saved_calibration, DEFAULT_PULSES_PER_LITER);
        }
    } else {
        LOG_INF("No saved calibration found, using default: %u pulses/L", DEFAULT_PULSES_PER_LITER);
    }
    
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
 * @brief Get the current smoothed flow rate
 * 
 * @return Smoothed flow rate in pulses per second
 */
uint32_t get_flow_rate(void)
{
    return smoothed_flow_rate;
}

/**
 * @brief Reset the flow sensor pulse counter to zero
 */
void reset_pulse_count(void)
{
    atomic_set(&pulse_count, 0);
    /* Also reset flow rate calculation */
    smoothed_flow_rate = 0;
    sample_index = 0;
    for (int i = 0; i < 2; i++) {  /* Updated for 2 samples */
        flow_rate_samples[i] = 0;
    }
}

/**
 * @brief Print flow sensor debug information
 */
void flow_sensor_debug_info(void)
{
    uint32_t current_pulse_count = atomic_get(&pulse_count);
    
    printk("=== Flow Sensor Debug Info ===\n");
    printk("Total pulses: %u\n", current_pulse_count);
    printk("Smoothed flow rate: %u pps\n", smoothed_flow_rate);
    printk("Flow rate samples: ");
    for (int i = 0; i < 2; i++) {  /* Updated for 2 samples */
        printk("%u ", flow_rate_samples[i]);
    }
    printk("\n");
    printk("Sample index: %u\n", sample_index);
    printk("Last notified: %u\n", last_notified);
    printk("Flow calibration: %u pulses/L\n", current_flow_calibration);
    printk("=============================\n");
}

/**
 * @brief Get the current flow calibration value
 * 
 * @return Calibration value in pulses per liter
 */
uint32_t get_flow_calibration(void)
{
    return current_flow_calibration;
}

/**
 * @brief Set the flow calibration value
 * 
 * @param pulses_per_liter Calibration value in pulses per liter
 * @return 0 on success, negative error code on failure
 */
int set_flow_calibration(uint32_t pulses_per_liter)
{
    /* Validate calibration range */
    if (pulses_per_liter < 100 || pulses_per_liter > 10000) {
        printk("Flow calibration out of range: %u (valid: 100-10000)\n", pulses_per_liter);
        return -EINVAL;
    }
    
    current_flow_calibration = pulses_per_liter;
    printk("Flow calibration updated: %u pulses/L\n", current_flow_calibration);
    
    /* Save to persistent storage (also updates onboarding flag) */
    int nvs_ret = nvs_save_flow_calibration(current_flow_calibration);
    if (nvs_ret < 0) {
        LOG_ERR("Failed to save flow calibration to NVS: %d", nvs_ret);
        return nvs_ret;
    }
    
    LOG_INF("Flow calibration saved to persistent storage: %u pulses/L", current_flow_calibration);
    
    return 0;
}
