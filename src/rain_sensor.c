#include "rain_sensor.h"
#include "rain_config.h"
#include "nvs_config.h"
#include <zephyr/kernel.h>
#include "rain_history.h"
#include "timezone.h"
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>
#include <zephyr/devicetree.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

LOG_MODULE_REGISTER(rain_sensor, LOG_LEVEL_DBG);

/* Log throttling (avoid spamming when there is simply no rain) */
static uint32_t last_no_pulses_log_time_s;
static uint32_t last_disconnected_log_time_s;

/* Device tree definitions for rain sensor */
#define RAIN_SENSOR_NODE DT_NODELABEL(rain_key)

#if DT_NODE_EXISTS(RAIN_SENSOR_NODE)
#define RAIN_SENSOR_GPIO_NODE DT_GPIO_CTLR(RAIN_SENSOR_NODE, gpios)
#define RAIN_SENSOR_GPIO_PIN DT_GPIO_PIN(RAIN_SENSOR_NODE, gpios)
#define RAIN_SENSOR_GPIO_FLAGS DT_GPIO_FLAGS(RAIN_SENSOR_NODE, gpios)
#else
/* Fallback to P0.31 if device tree node not found */
#define RAIN_SENSOR_GPIO_NODE DT_NODELABEL(gpio0)
#define RAIN_SENSOR_GPIO_PIN 31
#define RAIN_SENSOR_GPIO_FLAGS (GPIO_INPUT | GPIO_PULL_UP)
#endif

/* Rain sensor state structure */
static struct {
    const struct device *gpio_dev;
    struct gpio_callback gpio_cb;
    
    /* Configuration */
    rain_sensor_config_t config;
    
    /* Pulse counting (atomic for thread safety) */
    atomic_t total_pulses;
    atomic_t last_pulse_time;
    
    /* Hourly tracking */
    float current_hour_mm;
    uint32_t hour_start_time;
    uint32_t last_hour_pulses;
    
    /* Rate calculation */
    float hourly_rate_mm;
    uint32_t rate_calc_time;
    
    /* Status */
    rain_sensor_status_t status;
    uint8_t data_quality;
    bool initialized;
    
    /* Debouncing */
    uint32_t last_interrupt_time;
    
    /* Mutex for non-atomic operations */
    struct k_mutex mutex;
} rain_sensor_state = {
    .config = {
        .mm_per_pulse = RAIN_SENSOR_DEFAULT_MM_PER_PULSE,
        .debounce_ms = RAIN_SENSOR_DEFAULT_DEBOUNCE_MS,
        .sensor_enabled = true,
        .integration_enabled = true
    },
    .status = RAIN_SENSOR_STATUS_INACTIVE,
    .data_quality = 100,
    .initialized = false
};

static rain_error_code_t last_error = RAIN_ERROR_NONE;
static uint32_t error_count = 0;
static uint32_t last_error_time = 0;

/* Forward declarations */
static void rain_sensor_gpio_callback(const struct device *dev, 
                                     struct gpio_callback *cb, 
                                     uint32_t pins);
static void rain_sensor_update_status(void);
static void rain_sensor_calculate_rate(void);
static void rain_sensor_handle_error(rain_error_code_t error);
static bool rain_sensor_validate_pulse_timing(uint32_t current_time);
static bool rain_sensor_validate_pulse_enhanced(uint32_t current_time);
static void rain_sensor_health_check(void);

/**
 * @brief GPIO interrupt callback for rain sensor pulses
 */
static void rain_sensor_gpio_callback(const struct device *dev, 
                                     struct gpio_callback *cb, 
                                     uint32_t pins)
{
    uint32_t current_time = k_uptime_get_32();
    
    /* Debouncing check */
    if (current_time - rain_sensor_state.last_interrupt_time < 
        rain_sensor_state.config.debounce_ms) {
        return;
    }
    
    /* Only count if sensor is enabled */
    if (!rain_sensor_state.config.sensor_enabled) {
        return;
    }
    
    /* Enhanced pulse validation with outlier detection */
    if (!rain_sensor_validate_pulse_enhanced(current_time)) {
        rain_sensor_handle_error(RAIN_ERROR_EXCESSIVE_RATE);
        return;
    }
    
    rain_sensor_state.last_interrupt_time = current_time;
    
    /* Increment pulse counter atomically */
    atomic_inc(&rain_sensor_state.total_pulses);
    atomic_set(&rain_sensor_state.last_pulse_time, current_time / 1000);
    
    /* Update status to active */
    rain_sensor_state.status = RAIN_SENSOR_STATUS_ACTIVE;
    
    /* Clear any previous errors on successful pulse */
    if (last_error != RAIN_ERROR_NONE) {
        last_error = RAIN_ERROR_NONE;
        LOG_INF("Rain sensor error cleared");
    }
    
    LOG_DBG("Rain pulse detected, total: %lu",
            (unsigned long)atomic_get(&rain_sensor_state.total_pulses));
}

/**
 * @brief Update sensor status based on recent activity
 */
static void rain_sensor_update_status(void)
{
    uint32_t current_time = k_uptime_get_32() / 1000;
    uint32_t last_pulse = atomic_get(&rain_sensor_state.last_pulse_time);
    
    if (!rain_sensor_state.config.sensor_enabled) {
        rain_sensor_state.status = RAIN_SENSOR_STATUS_INACTIVE;
        rain_sensor_state.data_quality = 0;
        return;
    }
    
    /* Check for sensor errors */
        if ((double)rain_sensor_state.hourly_rate_mm > (double)RAIN_SENSOR_MAX_RATE_MM_H) {
        rain_sensor_state.status = RAIN_SENSOR_STATUS_ERROR;
        rain_sensor_state.data_quality = 25;
            LOG_ERR("Rain sensor error: excessive rate %.1f mm/h", (double)rain_sensor_state.hourly_rate_mm);
        return;
    }
    
    /* Consider sensor active if pulse within last 5 minutes */
    if (last_pulse > 0 && (current_time - last_pulse) < 300) {
        rain_sensor_state.status = RAIN_SENSOR_STATUS_ACTIVE;
        rain_sensor_state.data_quality = 100;
    } else {
        rain_sensor_state.status = RAIN_SENSOR_STATUS_INACTIVE;
        
        /* Reduce data quality based on inactivity duration */
        uint32_t inactive_time = current_time - last_pulse;
        if (inactive_time > 86400) { /* 24 hours */
            rain_sensor_state.data_quality = 60;
        } else if (inactive_time > 43200) { /* 12 hours */
            rain_sensor_state.data_quality = 80;
        } else {
            rain_sensor_state.data_quality = 90;
        }
    }
    
    /* Additional info (rate-limited): no pulses since boot can simply mean no rainfall. */
    if (last_pulse == 0 && current_time > 3600) { /* after 1 hour of operation */
        if ((current_time - last_no_pulses_log_time_s) >= 3600U) { /* at most once per hour */
            last_no_pulses_log_time_s = current_time;
            LOG_INF("Rain sensor: No pulses detected since startup");
        }

        if (rain_sensor_state.data_quality > 70) {
            rain_sensor_state.data_quality = 70;
        }
    }
}

/**
 * @brief Calculate current rainfall rate
 */
static void rain_sensor_calculate_rate(void)
{
    uint32_t current_time = k_uptime_get_32() / 1000;
    uint32_t current_pulses = atomic_get(&rain_sensor_state.total_pulses);
    
    k_mutex_lock(&rain_sensor_state.mutex, K_FOREVER);
    
    /* Calculate rate based on pulses in last hour */
    uint32_t time_diff = current_time - rain_sensor_state.rate_calc_time;
    if (time_diff >= 3600) { /* Update every hour */
        uint32_t pulse_diff = current_pulses - rain_sensor_state.last_hour_pulses;
        float rainfall_mm = pulse_diff * rain_sensor_state.config.mm_per_pulse;
        
        rain_sensor_state.hourly_rate_mm = rainfall_mm;
        rain_sensor_state.rate_calc_time = current_time;
        rain_sensor_state.last_hour_pulses = current_pulses;
        
    /* Validate rate for error detection */
        if ((double)rain_sensor_state.hourly_rate_mm > (double)RAIN_SENSOR_MAX_RATE_MM_H) {
        LOG_WRN("Excessive rainfall rate detected: %.2f mm/h", 
            (double)rain_sensor_state.hourly_rate_mm);
            rain_sensor_state.status = RAIN_SENSOR_STATUS_ERROR;
            rain_sensor_state.data_quality = 50;
        }
    }
    
    k_mutex_unlock(&rain_sensor_state.mutex);
}

int rain_sensor_init(void)
{
    int ret;
    
    if (rain_sensor_state.initialized) {
        return 0;
    }
    
    LOG_INF("Initializing rain sensor on P0.%d", RAIN_SENSOR_GPIO_PIN);
    
    /* Initialize mutex */
    k_mutex_init(&rain_sensor_state.mutex);
    
    /* Get GPIO device */
    rain_sensor_state.gpio_dev = DEVICE_DT_GET(RAIN_SENSOR_GPIO_NODE);
    if (!device_is_ready(rain_sensor_state.gpio_dev)) {
        LOG_ERR("GPIO device not ready");
        return -ENODEV;
    }
    
    /* Configure GPIO pin */
    ret = gpio_pin_configure(rain_sensor_state.gpio_dev, 
                            RAIN_SENSOR_GPIO_PIN, 
                            RAIN_SENSOR_GPIO_FLAGS);
    if (ret < 0) {
        LOG_ERR("Failed to configure GPIO pin: %d", ret);
        return ret;
    }
    
    /* Setup GPIO interrupt */
    gpio_init_callback(&rain_sensor_state.gpio_cb, 
                      rain_sensor_gpio_callback, 
                      BIT(RAIN_SENSOR_GPIO_PIN));
    
    ret = gpio_add_callback(rain_sensor_state.gpio_dev, 
                           &rain_sensor_state.gpio_cb);
    if (ret < 0) {
        LOG_ERR("Failed to add GPIO callback: %d", ret);
        return ret;
    }
    
    /* Enable interrupt on falling edge (pulse detection) */
    ret = gpio_pin_interrupt_configure(rain_sensor_state.gpio_dev,
                                      RAIN_SENSOR_GPIO_PIN,
                                      GPIO_INT_EDGE_FALLING);
    if (ret < 0) {
        LOG_ERR("Failed to configure GPIO interrupt: %d", ret);
        return ret;
    }
    
    /* Load configuration from NVS */
    rain_sensor_load_config();
    
    /* Initialize timing */
    uint32_t current_time = k_uptime_get_32() / 1000;
    rain_sensor_state.hour_start_time = 0U; /* initialize when RTC time becomes available */
    rain_sensor_state.rate_calc_time = current_time;
    
    /* Initialize atomic variables */
    atomic_set(&rain_sensor_state.total_pulses, 0);
    atomic_set(&rain_sensor_state.last_pulse_time, 0);
    
    rain_sensor_state.initialized = true;
    
    LOG_INF("Rain sensor initialized successfully");
    LOG_INF("Calibration: %.2f mm/pulse, Debounce: %u ms", 
            (double)rain_sensor_state.config.mm_per_pulse,
            rain_sensor_state.config.debounce_ms);
    
    return 0;
}

int rain_sensor_deinit(void)
{
    if (!rain_sensor_state.initialized) {
        return 0;
    }
    
    /* Disable GPIO interrupt */
    gpio_pin_interrupt_configure(rain_sensor_state.gpio_dev,
                                RAIN_SENSOR_GPIO_PIN,
                                GPIO_INT_DISABLE);
    
    /* Remove callback */
    gpio_remove_callback(rain_sensor_state.gpio_dev, 
                        &rain_sensor_state.gpio_cb);
    
    rain_sensor_state.initialized = false;
    
    LOG_INF("Rain sensor deinitialized");
    return 0;
}

int rain_sensor_set_calibration(float mm_per_pulse)
{
    if (mm_per_pulse < RAIN_SENSOR_MIN_CALIBRATION || 
        mm_per_pulse > RAIN_SENSOR_MAX_CALIBRATION) {
    LOG_ERR("Invalid calibration value: %.3f", (double)mm_per_pulse);
        return -EINVAL;
    }
    
    k_mutex_lock(&rain_sensor_state.mutex, K_FOREVER);
    rain_sensor_state.config.mm_per_pulse = mm_per_pulse;
    k_mutex_unlock(&rain_sensor_state.mutex);
    
    LOG_INF("Rain sensor calibration set to %.3f mm/pulse", (double)mm_per_pulse);
    return 0;
}

float rain_sensor_get_calibration(void)
{
    return rain_sensor_state.config.mm_per_pulse;
}

int rain_sensor_set_debounce(uint16_t debounce_ms)
{
    if (debounce_ms < 10 || debounce_ms > 1000) {
        LOG_ERR("Invalid debounce value: %u ms", debounce_ms);
        return -EINVAL;
    }
    
    k_mutex_lock(&rain_sensor_state.mutex, K_FOREVER);
    rain_sensor_state.config.debounce_ms = debounce_ms;
    k_mutex_unlock(&rain_sensor_state.mutex);
    
    LOG_INF("Rain sensor debounce set to %u ms", debounce_ms);
    return 0;
}

uint16_t rain_sensor_get_debounce(void)
{
    return rain_sensor_state.config.debounce_ms;
}

uint32_t rain_sensor_get_pulse_count(void)
{
    return atomic_get(&rain_sensor_state.total_pulses);
}

float rain_sensor_get_current_rainfall_mm(void)
{
    uint32_t pulses = atomic_get(&rain_sensor_state.total_pulses);
    return pulses * rain_sensor_state.config.mm_per_pulse;
}

float rain_sensor_get_hourly_rate_mm(void)
{
    rain_sensor_calculate_rate();
    return rain_sensor_state.hourly_rate_mm;
}

float rain_sensor_get_current_hour_mm(void)
{
    return rain_sensor_state.current_hour_mm;
}

uint32_t rain_sensor_get_last_pulse_time(void)
{
    return atomic_get(&rain_sensor_state.last_pulse_time);
}

void rain_sensor_reset_counters(void)
{
    k_mutex_lock(&rain_sensor_state.mutex, K_FOREVER);
    
    atomic_set(&rain_sensor_state.total_pulses, 0);
    atomic_set(&rain_sensor_state.last_pulse_time, 0);
    
    rain_sensor_state.current_hour_mm = 0.0f;
    rain_sensor_state.hourly_rate_mm = 0.0f;
    rain_sensor_state.last_hour_pulses = 0;
    
    uint32_t current_time = k_uptime_get_32() / 1000;
    rain_sensor_state.hour_start_time = 0U; /* re-init on next update */
    rain_sensor_state.rate_calc_time = current_time;
    
    k_mutex_unlock(&rain_sensor_state.mutex);
    
    LOG_INF("Rain sensor counters reset");
}

bool rain_sensor_is_active(void)
{
    rain_sensor_update_status();
    return rain_sensor_state.status == RAIN_SENSOR_STATUS_ACTIVE;
}

rain_sensor_status_t rain_sensor_get_status(void)
{
    rain_sensor_update_status();
    return rain_sensor_state.status;
}

int rain_sensor_get_data(rain_sensor_data_t *data)
{
    if (!data) {
        return -EINVAL;
    }
    
    rain_sensor_update_status();
    rain_sensor_calculate_rate();
    
    k_mutex_lock(&rain_sensor_state.mutex, K_FOREVER);
    
    data->total_pulses = atomic_get(&rain_sensor_state.total_pulses);
    data->last_pulse_time = atomic_get(&rain_sensor_state.last_pulse_time);
    data->current_hour_mm = rain_sensor_state.current_hour_mm;
    data->hourly_rate_mm = rain_sensor_state.hourly_rate_mm;
    data->status = rain_sensor_state.status;
    data->data_quality = rain_sensor_state.data_quality;
    
    k_mutex_unlock(&rain_sensor_state.mutex);
    
    return 0;
}

int rain_sensor_set_enabled(bool enabled)
{
    k_mutex_lock(&rain_sensor_state.mutex, K_FOREVER);
    rain_sensor_state.config.sensor_enabled = enabled;
    k_mutex_unlock(&rain_sensor_state.mutex);
    
    LOG_INF("Rain sensor %s", enabled ? "enabled" : "disabled");
    return 0;
}

bool rain_sensor_is_enabled(void)
{
    return rain_sensor_state.config.sensor_enabled;
}

int rain_sensor_set_integration_enabled(bool enabled)
{
    k_mutex_lock(&rain_sensor_state.mutex, K_FOREVER);
    rain_sensor_state.config.integration_enabled = enabled;
    k_mutex_unlock(&rain_sensor_state.mutex);
    
    LOG_INF("Rain sensor irrigation integration %s", 
            enabled ? "enabled" : "disabled");
    return 0;
}

bool rain_sensor_is_integration_enabled(void)
{
    return rain_sensor_state.config.integration_enabled;
}

void rain_sensor_update_hourly(void)
{
    uint32_t current_pulses = atomic_get(&rain_sensor_state.total_pulses);
    uint32_t current_unix = timezone_get_unix_utc();

    rain_sensor_update_status();
    
    k_mutex_lock(&rain_sensor_state.mutex, K_FOREVER);

    if (current_unix != 0U) {
        uint32_t current_hour_epoch = (current_unix / 3600U) * 3600U;

        if (rain_sensor_state.hour_start_time == 0U) {
            rain_sensor_state.hour_start_time = current_hour_epoch;
            rain_sensor_state.last_hour_pulses = current_pulses;
        }

        if (current_hour_epoch < rain_sensor_state.hour_start_time) {
            LOG_WRN("RTC hour moved backwards (%u -> %u), resetting rain hour tracking",
                    rain_sensor_state.hour_start_time, current_hour_epoch);
            rain_sensor_state.hour_start_time = current_hour_epoch;
            rain_sensor_state.last_hour_pulses = current_pulses;
            rain_sensor_state.current_hour_mm = 0.0f;
        }

        /* Check if we've moved to a new hour (or multiple hours due to delayed calls) */
        if (current_hour_epoch > rain_sensor_state.hour_start_time) {
            uint32_t completed_hour_epoch = rain_sensor_state.hour_start_time;
            uint32_t hour_pulses = current_pulses - rain_sensor_state.last_hour_pulses;
            float completed_hour_mm = hour_pulses * rain_sensor_state.config.mm_per_pulse;
            uint8_t pulse_count = (hour_pulses > UINT8_MAX) ? UINT8_MAX : (uint8_t)hour_pulses;
            uint8_t quality = rain_sensor_state.data_quality;

                /* Avoid float formatting in logs (often disabled); print mm with 2 decimals via fixed-point. */
                uint32_t mm_x100 = (completed_hour_mm <= 0.0f) ? 0U : (uint32_t)(completed_hour_mm * 100.0f + 0.5f);
                LOG_INF("Hour completed (%u): %u.%02u mm rainfall",
                    completed_hour_epoch, mm_x100 / 100U, mm_x100 % 100U);

            /* Persist to rain history (store even 0.0mm to keep a continuous timeline) */
            watering_error_t hist_ret = rain_history_record_hourly_full(
                completed_hour_epoch, completed_hour_mm, pulse_count, quality);
            if (hist_ret != WATERING_SUCCESS) {
                LOG_WRN("Failed to record rain history for hour %u: %d", completed_hour_epoch, hist_ret);
            }

            /* Reset for new hour */
            rain_sensor_state.hour_start_time = current_hour_epoch;
            rain_sensor_state.last_hour_pulses = current_pulses;
            rain_sensor_state.current_hour_mm = 0.0f;

            /* Save state to NVS periodically (best-effort; not used for history queries) */
            rain_nvs_state_t state;
            state.total_pulses = current_pulses;
            state.last_pulse_time = atomic_get(&rain_sensor_state.last_pulse_time);
            state.current_hour_mm = rain_sensor_state.current_hour_mm;
            state.today_total_mm = 0.0f; /* Will be calculated from history */
            state.hour_start_time = rain_sensor_state.hour_start_time;
            state.day_start_time = (current_unix / 86400U) * 86400U;
            memset(state.reserved, 0, sizeof(state.reserved));
            (void)rain_state_save(&state);
        } else {
            /* Update current hour rainfall */
            uint32_t hour_pulses = current_pulses - rain_sensor_state.last_hour_pulses;
            rain_sensor_state.current_hour_mm = hour_pulses * rain_sensor_state.config.mm_per_pulse;
        }
    } else {
        /* RTC not available: keep updating current hour estimate without recording history */
        uint32_t hour_pulses = current_pulses - rain_sensor_state.last_hour_pulses;
        rain_sensor_state.current_hour_mm = hour_pulses * rain_sensor_state.config.mm_per_pulse;
    }
    
    k_mutex_unlock(&rain_sensor_state.mutex);
    
    /* Update status and rate calculations */
    rain_sensor_update_status();
    rain_sensor_calculate_rate();
    
    /* Check for error recovery */
    rain_sensor_health_check();
    
    /* Do not treat "no rain" as a hardware disconnect.
     * (A tipping bucket can legitimately have 0 pulses for days.)
     */
}

void rain_sensor_debug_info(void)
{
    rain_sensor_data_t data;
    rain_sensor_get_data(&data);
    
    printk("=== Rain Sensor Debug Info ===\n");
    printk("Initialized: %s\n", rain_sensor_state.initialized ? "Yes" : "No");
    printk("Enabled: %s\n", rain_sensor_state.config.sensor_enabled ? "Yes" : "No");
    printk("Integration: %s\n", rain_sensor_state.config.integration_enabled ? "Yes" : "No");
    printk("Calibration: %.3f mm/pulse\n", (double)rain_sensor_state.config.mm_per_pulse);
    printk("Debounce: %u ms\n", rain_sensor_state.config.debounce_ms);
    printk("Total pulses: %u\n", data.total_pulses);
    printk("Current rainfall: %.2f mm\n", (double)rain_sensor_get_current_rainfall_mm());
    printk("Current hour: %.2f mm\n", (double)data.current_hour_mm);
    printk("Hourly rate: %.2f mm/h\n", (double)data.hourly_rate_mm);
    printk("Last pulse: %u s ago\n",
            data.last_pulse_time > 0 ?
            (k_uptime_get_32() / 1000) - data.last_pulse_time : 0);
    printk("Status: %s\n",
            data.status == RAIN_SENSOR_STATUS_ACTIVE ? "Active" :
            data.status == RAIN_SENSOR_STATUS_INACTIVE ? "Inactive" : "Error");
    printk("Data quality: %u%%\n", data.data_quality);
    printk("==============================\n");
}

int rain_sensor_validate_config(const rain_sensor_config_t *config)
{
    if (!config) {
        return -EINVAL;
    }
    
    if (config->mm_per_pulse < RAIN_SENSOR_MIN_CALIBRATION ||
        config->mm_per_pulse > RAIN_SENSOR_MAX_CALIBRATION) {
        return -EINVAL;
    }
    
    if (config->debounce_ms < 10 || config->debounce_ms > 1000) {
        return -EINVAL;
    }
    
    return 0;
}

void rain_sensor_get_default_config(rain_sensor_config_t *config)
{
    if (!config) {
        return;
    }
    
    config->mm_per_pulse = RAIN_SENSOR_DEFAULT_MM_PER_PULSE;
    config->debounce_ms = RAIN_SENSOR_DEFAULT_DEBOUNCE_MS;
    config->sensor_enabled = true;
    config->integration_enabled = true;
}

/**
 * @brief Handle rain sensor errors
 */
static void rain_sensor_handle_error(rain_error_code_t error)
{
    last_error = error;
    error_count++;
    last_error_time = k_uptime_get_32() / 1000;
    
    switch (error) {
        case RAIN_ERROR_SENSOR_DISCONNECTED:
            /* Rate-limit to avoid log spam. */
            if ((last_error_time - last_disconnected_log_time_s) >= 1800U) {
                last_disconnected_log_time_s = last_error_time;
                LOG_ERR("Rain sensor disconnected - no pulses detected");
            }
            rain_sensor_state.status = RAIN_SENSOR_STATUS_ERROR;
            rain_sensor_state.data_quality = 0;
            break;
            
        case RAIN_ERROR_CALIBRATION_INVALID:
            LOG_ERR("Rain sensor calibration invalid");
            rain_sensor_state.data_quality = 25;
            break;
            
        case RAIN_ERROR_EXCESSIVE_RATE:
            LOG_ERR("Rain sensor excessive pulse rate detected");
            rain_sensor_state.status = RAIN_SENSOR_STATUS_ERROR;
            rain_sensor_state.data_quality = 30;
            break;
            
        case RAIN_ERROR_GPIO_FAILURE:
            LOG_ERR("Rain sensor GPIO failure");
            rain_sensor_state.status = RAIN_SENSOR_STATUS_ERROR;
            rain_sensor_state.data_quality = 0;
            break;
            
        case RAIN_ERROR_CONFIG_CORRUPT:
            LOG_ERR("Rain sensor configuration corrupted");
            rain_sensor_get_default_config(&rain_sensor_state.config);
            break;
            
        default:
            LOG_ERR("Unknown rain sensor error: %d", error);
            break;
    }
}

/**
 * @brief Validate pulse timing to detect sensor errors
 */
static bool rain_sensor_validate_pulse_timing(uint32_t current_time)
{
    static uint32_t pulse_times[10];
    static uint8_t pulse_index = 0;
    static uint8_t pulse_count = 0;
    
    /* Store pulse time */
    pulse_times[pulse_index] = current_time;
    pulse_index = (pulse_index + 1) % 10;
    if (pulse_count < 10) {
        pulse_count++;
    }
    
    /* Need at least 5 pulses to validate */
    if (pulse_count < 5) {
        return true;
    }
    
    /* Calculate average interval between pulses */
    uint32_t total_interval = 0;
    uint8_t intervals = 0;
    
    for (int i = 1; i < pulse_count; i++) {
        uint8_t prev_idx = (pulse_index - i - 1 + 10) % 10;
        uint8_t curr_idx = (pulse_index - i + 10) % 10;
        
        if (pulse_times[curr_idx] > pulse_times[prev_idx]) {
            total_interval += pulse_times[curr_idx] - pulse_times[prev_idx];
            intervals++;
        }
    }
    
    if (intervals == 0) {
        return true;
    }
    
    uint32_t avg_interval_ms = total_interval / intervals;
    
    /* Check for excessive rate (>100 mm/h is physically impossible) */
    float rate_mm_h = (3600000.0f / avg_interval_ms) * rain_sensor_state.config.mm_per_pulse;
    
    if ((double)rate_mm_h > (double)RAIN_SENSOR_MAX_RATE_MM_H) {
        LOG_WRN("Excessive rain rate detected: %.1f mm/h", (double)rate_mm_h);
        return false;
    }
    
    return true;
}

/**
 * @brief Check for sensor recovery from error conditions
 */
static void __attribute__((unused)) rain_sensor_recovery_check(void)
{
    uint32_t current_time = k_uptime_get_32() / 1000;
    
    /* Check for sensor disconnection recovery */
    if (last_error == RAIN_ERROR_SENSOR_DISCONNECTED) {
        uint32_t last_pulse = atomic_get(&rain_sensor_state.last_pulse_time);
        if (last_pulse > 0 && (current_time - last_pulse) < 300) {
            LOG_INF("Rain sensor reconnected");
            last_error = RAIN_ERROR_NONE;
            rain_sensor_state.status = RAIN_SENSOR_STATUS_ACTIVE;
            rain_sensor_state.data_quality = 100;
        }
    }
    
    /* Check for excessive rate recovery */
    if (last_error == RAIN_ERROR_EXCESSIVE_RATE) {
        if ((current_time - last_error_time) > 300) { /* 5 minutes */
            LOG_INF("Rain sensor rate normalized");
            last_error = RAIN_ERROR_NONE;
            rain_sensor_state.status = RAIN_SENSOR_STATUS_INACTIVE;
            rain_sensor_state.data_quality = 80;
        }
    }
}

/**
 * @brief Get error information for diagnostics
 */
rain_error_code_t rain_sensor_get_last_error(void)
{
    return last_error;
}

/**
 * @brief Get error count for diagnostics
 */
uint32_t rain_sensor_get_error_count(void)
{
    return error_count;
}

/**
 * @brief Get time of last error
 */
uint32_t rain_sensor_get_last_error_time(void)
{
    return last_error_time;
}

/**
 * @brief Clear error state (for testing/recovery)
 */
void rain_sensor_clear_errors(void)
{
    last_error = RAIN_ERROR_NONE;
    last_error_time = 0;
    LOG_INF("Rain sensor errors cleared");
}

int rain_sensor_save_config(void)
{
    rain_nvs_config_t nvs_config;
    
    k_mutex_lock(&rain_sensor_state.mutex, K_FOREVER);
    
    nvs_config.mm_per_pulse = rain_sensor_state.config.mm_per_pulse;
    nvs_config.debounce_ms = rain_sensor_state.config.debounce_ms;
    nvs_config.sensor_enabled = rain_sensor_state.config.sensor_enabled ? 1 : 0;
    nvs_config.integration_enabled = rain_sensor_state.config.integration_enabled ? 1 : 0;
    nvs_config.rain_sensitivity_pct = 75.0f; // Default for now
    nvs_config.skip_threshold_mm = 5.0f;     // Default for now
    nvs_config.last_reset_time = k_uptime_get_32() / 1000;
    memset(nvs_config.reserved, 0, sizeof(nvs_config.reserved));
    
    k_mutex_unlock(&rain_sensor_state.mutex);
    
    return rain_config_save(&nvs_config);
}

int rain_sensor_load_config(void)
{
    rain_nvs_config_t nvs_config;
    int ret = rain_config_load(&nvs_config);
    
    k_mutex_lock(&rain_sensor_state.mutex, K_FOREVER);
    
    rain_sensor_state.config.mm_per_pulse = nvs_config.mm_per_pulse;
    rain_sensor_state.config.debounce_ms = nvs_config.debounce_ms;
    rain_sensor_state.config.sensor_enabled = nvs_config.sensor_enabled != 0;
    rain_sensor_state.config.integration_enabled = nvs_config.integration_enabled != 0;
    
    k_mutex_unlock(&rain_sensor_state.mutex);
    
    return ret;
} 

#define RAIN_ERROR_LOG_SIZE 10
static rain_error_log_t error_log[RAIN_ERROR_LOG_SIZE];
static uint8_t error_log_index = 0;
static uint8_t error_log_count = 0;

/* Outlier detection parameters */
#define RAIN_OUTLIER_THRESHOLD_MULTIPLIER 3.0f
#define RAIN_MIN_SAMPLES_FOR_OUTLIER_DETECTION 20
#define RAIN_PULSE_INTERVAL_HISTORY_SIZE 50

static struct {
    uint32_t pulse_intervals[RAIN_PULSE_INTERVAL_HISTORY_SIZE];
    uint8_t interval_index;
    uint8_t interval_count;
    float mean_interval;
    float std_deviation;
    bool outlier_detection_enabled;
} outlier_detector = {
    .outlier_detection_enabled = true
};

/* Sensor health monitoring */
typedef struct {
    uint32_t total_pulses_lifetime;
    uint32_t valid_pulses;
    uint32_t invalid_pulses;
    uint32_t outlier_pulses;
    uint32_t last_health_check;
    uint32_t consecutive_errors;
    uint32_t max_consecutive_errors;
    float pulse_accuracy_percentage;
    bool sensor_health_critical;
} rain_sensor_health_t;

static rain_sensor_health_t sensor_health = {0};

/**
 * @brief Log error with detailed information
 */
static void rain_sensor_log_error(rain_error_code_t error_code, const char *description)
{
    rain_error_log_t *log_entry = &error_log[error_log_index];
    
    log_entry->error_code = error_code;
    log_entry->timestamp = k_uptime_get_32() / 1000;
    log_entry->pulse_count_at_error = atomic_get(&rain_sensor_state.total_pulses);
    log_entry->rate_at_error = rain_sensor_state.hourly_rate_mm;
    strncpy(log_entry->description, description, sizeof(log_entry->description) - 1);
    log_entry->description[sizeof(log_entry->description) - 1] = '\0';
    
    error_log_index = (error_log_index + 1) % RAIN_ERROR_LOG_SIZE;
    if (error_log_count < RAIN_ERROR_LOG_SIZE) {
        error_log_count++;
    }
    
    LOG_ERR("Rain sensor error logged: %s (code: %d)", description, error_code);
}

/**
 * @brief Update outlier detection statistics
 */
static void update_outlier_statistics(uint32_t interval_ms)
{
    if (!outlier_detector.outlier_detection_enabled) {
        return;
    }
    
    /* Store interval */
    outlier_detector.pulse_intervals[outlier_detector.interval_index] = interval_ms;
    outlier_detector.interval_index = (outlier_detector.interval_index + 1) % RAIN_PULSE_INTERVAL_HISTORY_SIZE;
    if (outlier_detector.interval_count < RAIN_PULSE_INTERVAL_HISTORY_SIZE) {
        outlier_detector.interval_count++;
    }
    
    /* Calculate mean */
    uint64_t sum = 0;
    for (int i = 0; i < outlier_detector.interval_count; i++) {
        sum += outlier_detector.pulse_intervals[i];
    }
    outlier_detector.mean_interval = (float)sum / outlier_detector.interval_count;
    
    /* Calculate standard deviation */
    if (outlier_detector.interval_count >= RAIN_MIN_SAMPLES_FOR_OUTLIER_DETECTION) {
        float variance_sum = 0.0f;
        for (int i = 0; i < outlier_detector.interval_count; i++) {
            float diff = outlier_detector.pulse_intervals[i] - outlier_detector.mean_interval;
            variance_sum += diff * diff;
        }
        outlier_detector.std_deviation = sqrtf(variance_sum / outlier_detector.interval_count);
    }
}

/**
 * @brief Detect outlier pulses using statistical analysis
 */
static bool is_pulse_outlier(uint32_t interval_ms)
{
    if (!outlier_detector.outlier_detection_enabled || 
        outlier_detector.interval_count < RAIN_MIN_SAMPLES_FOR_OUTLIER_DETECTION) {
        return false;
    }
    
    float z_score = fabsf((float)interval_ms - outlier_detector.mean_interval) / outlier_detector.std_deviation;
    
    if (z_score > RAIN_OUTLIER_THRESHOLD_MULTIPLIER) {
        LOG_WRN("Outlier pulse detected: interval=%ums, mean=%.1fms, z-score=%.2f", 
                interval_ms, (double)outlier_detector.mean_interval, (double)z_score);
        sensor_health.outlier_pulses++;
        return true;
    }
    
    return false;
}

/**
 * @brief Enhanced pulse validation with outlier detection
 */
static bool rain_sensor_validate_pulse_enhanced(uint32_t current_time)
{
    static uint32_t last_pulse_time = 0;
    bool is_valid = true;
    
    sensor_health.total_pulses_lifetime++;
    
    /* Basic timing validation */
    if (!rain_sensor_validate_pulse_timing(current_time)) {
        sensor_health.invalid_pulses++;
        sensor_health.consecutive_errors++;
        rain_sensor_log_error(RAIN_ERROR_EXCESSIVE_RATE, "Pulse timing validation failed");
        is_valid = false;
    }
    
    /* Outlier detection */
    if (last_pulse_time > 0) {
        uint32_t interval_ms = current_time - last_pulse_time;
        
        update_outlier_statistics(interval_ms);
        
        if (is_pulse_outlier(interval_ms)) {
            /* Don't reject outliers completely, but flag them */
            LOG_WRN("Outlier pulse detected but accepted");
        }
    }
    
    /* Debounce validation */
    if (last_pulse_time > 0 && (current_time - last_pulse_time) < rain_sensor_state.config.debounce_ms) {
        sensor_health.invalid_pulses++;
        sensor_health.consecutive_errors++;
        rain_sensor_log_error(RAIN_ERROR_EXCESSIVE_RATE, "Pulse failed debounce validation");
        is_valid = false;
    }
    
    /* Update health statistics */
    if (is_valid) {
        sensor_health.valid_pulses++;
        sensor_health.consecutive_errors = 0;
    } else {
        if (sensor_health.consecutive_errors > sensor_health.max_consecutive_errors) {
            sensor_health.max_consecutive_errors = sensor_health.consecutive_errors;
        }
    }
    
    /* Check for critical health condition */
    if (sensor_health.consecutive_errors > 10) {
        sensor_health.sensor_health_critical = true;
        rain_sensor_log_error(RAIN_ERROR_SENSOR_DISCONNECTED, "Critical: Too many consecutive errors");
    }
    
    /* Update pulse accuracy */
    if (sensor_health.total_pulses_lifetime > 0) {
        sensor_health.pulse_accuracy_percentage = 
            (float)sensor_health.valid_pulses / sensor_health.total_pulses_lifetime * 100.0f;
    }
    
    last_pulse_time = current_time;
    return is_valid;
}

/**
 * @brief Comprehensive sensor health check
 */
static void rain_sensor_health_check(void)
{
    uint32_t current_time = k_uptime_get_32() / 1000;
    
    /* Perform health check every 5 minutes */
    if ((current_time - sensor_health.last_health_check) < 300) {
        return;
    }
    
    sensor_health.last_health_check = current_time;
    
    /* Don't flag a "disconnect" purely because there are no pulses yet.
     * If the sensor ever produced pulses, you can add a stricter heuristic later.
     */
    
    /* Check pulse accuracy */
    if (sensor_health.pulse_accuracy_percentage < 70.0f && sensor_health.total_pulses_lifetime > 50) {
        rain_sensor_log_error(RAIN_ERROR_SENSOR_DISCONNECTED, "Low pulse accuracy detected");
    LOG_WRN("Rain sensor pulse accuracy: %.1f%% (valid: %u, invalid: %u)", 
        (double)sensor_health.pulse_accuracy_percentage, 
                sensor_health.valid_pulses, sensor_health.invalid_pulses);
    }
    
    /* Check for excessive outliers */
    if (sensor_health.total_pulses_lifetime > 100) {
        float outlier_percentage = (float)sensor_health.outlier_pulses / sensor_health.total_pulses_lifetime * 100.0f;
        if (outlier_percentage > 20.0f) {
            rain_sensor_log_error(RAIN_ERROR_CALIBRATION_INVALID, "Excessive outlier pulses detected");
            LOG_WRN("Rain sensor outlier percentage: %.1f%%", (double)outlier_percentage);
        }
    }
    
    /* Check configuration validity */
    if (rain_sensor_state.config.mm_per_pulse < RAIN_SENSOR_MIN_CALIBRATION || 
        rain_sensor_state.config.mm_per_pulse > RAIN_SENSOR_MAX_CALIBRATION) {
        rain_sensor_log_error(RAIN_ERROR_CALIBRATION_INVALID, "Calibration value out of range");
    }
    
    /* Reset critical health flag if conditions improve */
    if (sensor_health.sensor_health_critical && sensor_health.consecutive_errors == 0) {
        uint32_t time_since_last_error = current_time - last_error_time;
        if (time_since_last_error > 1800) { /* 30 minutes */
            sensor_health.sensor_health_critical = false;
            LOG_INF("Rain sensor health status improved");
        }
    }
}

/**
 * @brief Data validation for rainfall measurements
 */
static bool __attribute__((unused)) rain_sensor_validate_data(float rainfall_mm, float rate_mm_h)
{
    /* Check for reasonable rainfall values */
    if (rainfall_mm < 0.0f || rainfall_mm > 1000.0f) {
        rain_sensor_log_error(RAIN_ERROR_CALIBRATION_INVALID, "Invalid rainfall amount");
        return false;
    }
    
    /* Check for reasonable rate values */
    if (rate_mm_h < 0.0f || rate_mm_h > RAIN_SENSOR_MAX_RATE_MM_H) {
        rain_sensor_log_error(RAIN_ERROR_EXCESSIVE_RATE, "Invalid rainfall rate");
        return false;
    }
    
    /* Check for sudden rate changes (possible sensor malfunction) */
    static float last_rate = 0.0f;
    if (last_rate > 0.0f && rate_mm_h > 0.0f) {
        float rate_change_ratio = rate_mm_h / last_rate;
        if (rate_change_ratio > 10.0f || rate_change_ratio < 0.1f) {
            LOG_WRN("Sudden rainfall rate change detected: %.2f -> %.2f mm/h", (double)last_rate, (double)rate_mm_h);
            /* Don't reject, but log for analysis */
        }
    }
    last_rate = rate_mm_h;
    
    return true;
}

/**
 * @brief Get comprehensive diagnostic information
 */
int rain_sensor_get_diagnostics(char *buffer, uint16_t buffer_size)
{
    if (!buffer || buffer_size < 500) {
        return -1;
    }
    
    int written = 0;
    uint32_t current_time = k_uptime_get_32() / 1000;
    
    written += snprintf(buffer + written, buffer_size - written,
                       "=== Rain Sensor Diagnostics ===\n");
    
    /* Basic status */
    written += snprintf(buffer + written, buffer_size - written,
                       "Status: %s\n", 
                       rain_sensor_state.status == RAIN_SENSOR_STATUS_ACTIVE ? "Active" :
                       rain_sensor_state.status == RAIN_SENSOR_STATUS_INACTIVE ? "Inactive" : "Error");
    
    written += snprintf(buffer + written, buffer_size - written,
                       "Enabled: %s\n", rain_sensor_state.config.sensor_enabled ? "Yes" : "No");
    
    written += snprintf(buffer + written, buffer_size - written,
                       "Data Quality: %u%%\n", rain_sensor_state.data_quality);
    
    /* Health statistics */
    written += snprintf(buffer + written, buffer_size - written,
                       "Health Critical: %s\n", sensor_health.sensor_health_critical ? "YES" : "No");
    
    written += snprintf(buffer + written, buffer_size - written,
                       "Pulse Accuracy: %.1f%%\n", (double)sensor_health.pulse_accuracy_percentage);
    
    written += snprintf(buffer + written, buffer_size - written,
                       "Total Pulses: %u (Valid: %u, Invalid: %u, Outliers: %u)\n",
                       sensor_health.total_pulses_lifetime, sensor_health.valid_pulses,
                       sensor_health.invalid_pulses, sensor_health.outlier_pulses);
    
    written += snprintf(buffer + written, buffer_size - written,
                       "Consecutive Errors: %u (Max: %u)\n",
                       sensor_health.consecutive_errors, sensor_health.max_consecutive_errors);
    
    /* Error information */
    written += snprintf(buffer + written, buffer_size - written,
                       "Last Error: %d (%us ago)\n", last_error, 
                       last_error_time > 0 ? current_time - last_error_time : 0);
    
    written += snprintf(buffer + written, buffer_size - written,
                       "Total Errors: %u\n", error_count);
    
    /* Configuration */
    written += snprintf(buffer + written, buffer_size - written,
                       "Calibration: %.3f mm/pulse\n", (double)rain_sensor_state.config.mm_per_pulse);
    
    written += snprintf(buffer + written, buffer_size - written,
                       "Debounce: %u ms\n", rain_sensor_state.config.debounce_ms);
    
    /* Outlier detection */
    if (outlier_detector.interval_count >= RAIN_MIN_SAMPLES_FOR_OUTLIER_DETECTION) {
    written += snprintf(buffer + written, buffer_size - written,
               "Pulse Statistics: Mean=%.1fms, StdDev=%.1fms\n",
               (double)outlier_detector.mean_interval, (double)outlier_detector.std_deviation);
    }
    
    /* Recent errors */
    if (error_log_count > 0) {
        written += snprintf(buffer + written, buffer_size - written,
                           "Recent Errors:\n");
        
        for (int i = 0; i < error_log_count && i < 5; i++) {
            int idx = (error_log_index - 1 - i + RAIN_ERROR_LOG_SIZE) % RAIN_ERROR_LOG_SIZE;
            rain_error_log_t *entry = &error_log[idx];
            written += snprintf(buffer + written, buffer_size - written,
                               "  %us ago: %s\n", 
                               current_time - entry->timestamp, entry->description);
        }
    }
    
    written += snprintf(buffer + written, buffer_size - written,
                       "===============================\n");
    
    return written;
}

/**
 * @brief Get error log for detailed analysis
 */
int rain_sensor_get_error_log(rain_error_log_t *log_buffer, uint8_t max_entries)
{
    if (!log_buffer || max_entries == 0) {
        return -EINVAL;
    }
    
    uint8_t count = 0;
    for (int i = 0; i < error_log_count && i < max_entries; i++) {
        int idx = (error_log_index - 1 - i + RAIN_ERROR_LOG_SIZE) % RAIN_ERROR_LOG_SIZE;
        log_buffer[count++] = error_log[idx];
    }
    
    return count;
}

/**
 * @brief Reset error statistics and health monitoring
 */
void rain_sensor_reset_diagnostics(void)
{
    memset(&sensor_health, 0, sizeof(sensor_health));
    memset(&error_log, 0, sizeof(error_log));
    error_log_index = 0;
    error_log_count = 0;
    error_count = 0;
    last_error = RAIN_ERROR_NONE;
    last_error_time = 0;
    
    /* Reset outlier detection */
    memset(&outlier_detector.pulse_intervals, 0, sizeof(outlier_detector.pulse_intervals));
    outlier_detector.interval_index = 0;
    outlier_detector.interval_count = 0;
    outlier_detector.mean_interval = 0.0f;
    outlier_detector.std_deviation = 0.0f;
    
    LOG_INF("Rain sensor diagnostics reset");
}

/**
 * @brief Enable or disable outlier detection
 */
void rain_sensor_set_outlier_detection(bool enabled)
{
    outlier_detector.outlier_detection_enabled = enabled;
    LOG_INF("Rain sensor outlier detection %s", enabled ? "enabled" : "disabled");
}

/**
 * @brief Get sensor health status
 */
bool rain_sensor_is_health_critical(void)
{
    return sensor_health.sensor_health_critical;
}

/**
 * @brief Get pulse accuracy percentage
 */
float rain_sensor_get_pulse_accuracy(void)
{
    return sensor_health.pulse_accuracy_percentage;
}
/**
 * 
@brief Periodic diagnostics for rain sensor health monitoring
 */
void rain_sensor_periodic_diagnostics(void)
{
    if (!rain_sensor_state.initialized) {
        return;
    }
    
    /* Update sensor health statistics */
    rain_sensor_health_check();
    
    /* Check for critical health conditions */
    if (sensor_health.sensor_health_critical) {
        uint32_t acc_x10 = (uint32_t)(sensor_health.pulse_accuracy_percentage * 10.0f);
        LOG_WRN("Rain sensor health critical - accuracy: %u.%u%%, errors: %u",
            acc_x10 / 10U, acc_x10 % 10U,
            sensor_health.consecutive_errors);
    }
    
    /* Log periodic health summary without floats to avoid formatter issues */
    uint32_t acc_x10 = (uint32_t)(sensor_health.pulse_accuracy_percentage * 10.0f);
    LOG_INF("Rain sensor health: accuracy=%u.%u%%, pulses=%u, errors=%u",
            acc_x10 / 10U, acc_x10 % 10U,
            sensor_health.total_pulses_lifetime,
            sensor_health.consecutive_errors);
}
