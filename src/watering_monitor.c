#include <zephyr/autoconf.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <stdio.h> /* ensure snprintf prototype visible on all build paths */
#include <string.h>
#include <zephyr/fs/fs.h>
#include "flow_sensor.h"
#include "watering.h"
#include "watering_internal.h"
#include "watering_history.h"
#include "bt_irrigation_service.h"   /* + BLE notifications */
#include "rain_sensor.h"             /* Rain sensor monitoring */
#include "rain_integration.h"        /* Rain integration status */
#include "rain_history.h"            /* Rain history monitoring */
#include "fao56_calc.h"              /* FAO-56 rain deficit updates */
#include "env_sensors.h"             /* Environmental data for rain effectiveness */
#include "timezone.h"
/* stdio already included above for snprintf */

#ifdef CONFIG_BT
int bt_irrigation_hydraulic_status_notify(uint8_t channel_id);
#endif

/**
 * @file watering_monitor.c
 * @brief Implementation of flow monitoring and anomaly detection
 * 
 * This file implements the flow monitoring system that detects problems
 * with water flow, including no-flow conditions when a valve is open
 * and unexpected flow when all valves are closed.
 */

/* Error codes for task error reporting */
#define TASK_ERROR_NO_FLOW      1  /**< No flow detected when valve is open */
#define TASK_ERROR_UNEXPECTED_FLOW 2  /**< Flow detected when valves are closed */

/** Stack size for flow monitor thread */
#define FLOW_MONITOR_STACK_SIZE 2048

/** Thread stack for flow monitor */
K_THREAD_STACK_DEFINE(flow_monitor_stack, FLOW_MONITOR_STACK_SIZE);

/** Thread control structure */
static struct k_thread flow_monitor_data;

/** Flag to signal monitor thread to exit */
static bool exit_tasks = false;
static bool monitor_started = false;

/** Counter for consecutive flow errors */
static uint8_t flow_error_attempts = 0;

/** Timestamp of last flow check */
static uint32_t last_flow_check_time = 0;

/** Mutex for protecting flow monitor state */
K_MUTEX_DEFINE(flow_monitor_mutex);

/* --- new constants ---------------------------------------------------- */
#define NO_FLOW_STALL_TIMEOUT_MS   3000   /* 3 s without new pulses ⇒ error */
#define NO_FLOW_RETRY_COOLDOWN_MS  5000   /* 5 s cooldown before retry to let system settle */
#define FLOW_STARTUP_GRACE_MS      8000   /* 8 s grace period after task starts for flow to begin
                                           * (accounts for master valve pre-delay + water pressure build-up) */
#define HYDRAULIC_RING_SECONDS             60
#define HYDRAULIC_LEARNING_MIN_RUNS         2
#define HYDRAULIC_LEARNING_MAX_RUNS         4
#define HYDRAULIC_LEARNING_MAX_RUNS_EXT     6
#define HYDRAULIC_STABLE_WINDOW_S           3
#define HYDRAULIC_STABLE_VARIATION_PCT      5
#define HYDRAULIC_MEASURE_WINDOW_S         30
#define HYDRAULIC_LEARNING_TIMEOUT_S       60
#define HYDRAULIC_UNEXPECTED_FLOW_WINDOW_S 30
#define HYDRAULIC_UNEXPECTED_FLOW_PULSES   10
#define HYDRAULIC_UNEXPECTED_FLOW_PERSIST_S 30
#define HYDRAULIC_POST_CLOSE_IGNORE_MS   2000
#define HYDRAULIC_HIGH_FLOW_HOLD_S          5
#define HYDRAULIC_LOW_FLOW_HOLD_S          30
#define HYDRAULIC_ABS_HIGH_FLOW_ML_MIN  20000
#define HYDRAULIC_MIN_NO_FLOW_ML_MIN      200
#define HYDRAULIC_MAINLINE_LEAK_PULSES     3
#define HYDRAULIC_LOG_PATH "/lfs/history/hydraulic_events.bin"
#define HYDRAULIC_LOG_MAX_BYTES 4096

typedef enum {
    HYDRAULIC_LOG_ACTION_WARN = 0,
    HYDRAULIC_LOG_ACTION_CHANNEL_LOCK = 1,
    HYDRAULIC_LOG_ACTION_GLOBAL_LOCK = 2
} hydraulic_log_action_t;

/* --- new state vars --------------------------------------------------- */
static uint32_t last_task_pulses      = 0;
static uint32_t last_pulse_update_ts  = 0;
static watering_task_t *watched_task  = NULL;
static uint32_t retry_cooldown_until  = 0; /* timestamp until which we skip flow checks after a retry */
static uint16_t pulse_history[HYDRAULIC_RING_SECONDS] = {0};
static uint8_t pulse_history_index = 0;
static uint32_t pulse_history_last_count = 0;
static uint32_t pulse_history_last_ts = 0;
static uint16_t flow_1s_history[HYDRAULIC_STABLE_WINDOW_S] = {0};
static uint8_t flow_1s_index = 0;
static uint8_t high_flow_consecutive = 0;
static uint8_t low_flow_consecutive = 0;
static uint8_t unexpected_flow_consecutive = 0;
static uint32_t last_valve_closed_ms = 0;
static bool static_test_active = false;

typedef enum {
    LEARNING_IDLE = 0,
    LEARNING_WAIT_STABLE,
    LEARNING_MEASURE
} learning_phase_t;

static struct {
    learning_phase_t phase;
    uint8_t channel_id;
    uint32_t start_ms;
    uint32_t stable_detected_ms;
    uint32_t measure_start_ms;
    uint32_t measure_start_pulses;
    uint8_t stable_windows;
} learning_ctx = {0};


size_t flow_monitor_get_unused_stack(void)
{
    /* Stack diagnostics rely on CONFIG_THREAD_STACK_INFO; return 0 otherwise */
    return 0;
}

/* --- alarm codes used by bt_irrigation_alarm_notify ------------------- */
#define ALARM_NO_FLOW          1
#define ALARM_UNEXPECTED_FLOW  2
#define ALARM_HIGH_FLOW        4
#define ALARM_LOW_FLOW         5
#define ALARM_MAINLINE_LEAK    6
#define ALARM_CHANNEL_LOCK     7
#define ALARM_GLOBAL_LOCK      8

/* Helper used throughout this file */
static inline void ble_status_update(void)
{
    bt_irrigation_system_status_update(system_status);
}

static uint16_t clamp_u16(uint16_t value, uint16_t min, uint16_t max)
{
    if (value < min) {
        return min;
    }
    if (value > max) {
        return max;
    }
    return value;
}

static uint32_t calc_flow_ml_min(uint32_t pulses, uint32_t window_s, uint32_t pulses_per_liter)
{
    if (pulses_per_liter == 0 || window_s == 0) {
        return 0;
    }
    return (uint32_t)(((uint64_t)pulses * 60000ULL) / ((uint64_t)pulses_per_liter * window_s));
}

static uint32_t pulse_sum_last_seconds(uint8_t seconds)
{
    if (seconds == 0 || seconds > HYDRAULIC_RING_SECONDS) {
        return 0;
    }

    uint32_t sum = 0;
    uint8_t idx = pulse_history_index;
    for (uint8_t i = 0; i < seconds; i++) {
        if (idx == 0) {
            idx = HYDRAULIC_RING_SECONDS;
        }
        idx--;
        sum += pulse_history[idx];
    }
    return sum;
}

static void hydraulic_reset_runtime_state(uint32_t current_pulses, uint32_t now_ms)
{
    memset(pulse_history, 0, sizeof(pulse_history));
    pulse_history_index = 0;
    pulse_history_last_count = current_pulses;
    pulse_history_last_ts = now_ms;
    memset(flow_1s_history, 0, sizeof(flow_1s_history));
    flow_1s_index = 0;
    high_flow_consecutive = 0;
    low_flow_consecutive = 0;
    unexpected_flow_consecutive = 0;
}

static void hydraulic_update_pulse_history(uint32_t current_pulses, uint32_t now_ms,
                                           uint32_t pulses_per_liter)
{
    if (pulse_history_last_ts == 0) {
        pulse_history_last_ts = now_ms;
        pulse_history_last_count = current_pulses;
        return;
    }

    uint32_t elapsed_ms = now_ms - pulse_history_last_ts;
    if (elapsed_ms < 1000) {
        return;
    }

    uint32_t steps = elapsed_ms / 1000;
    uint32_t pulse_diff = current_pulses - pulse_history_last_count;
    for (uint32_t i = 0; i < steps; i++) {
        pulse_history[pulse_history_index] = (i == 0) ? (uint16_t)pulse_diff : 0;
        pulse_history_index = (pulse_history_index + 1) % HYDRAULIC_RING_SECONDS;
    }

    uint16_t flow_1s_ml_min = (uint16_t)calc_flow_ml_min(pulse_diff, 1, pulses_per_liter);
    flow_1s_history[flow_1s_index] = flow_1s_ml_min;
    flow_1s_index = (flow_1s_index + 1) % HYDRAULIC_STABLE_WINDOW_S;

    pulse_history_last_count = current_pulses;
    pulse_history_last_ts += steps * 1000;
}

static hydraulic_profile_t hydraulic_profile_from_channel(const watering_channel_t *channel)
{
    if (!channel) {
        return PROFILE_AUTO;
    }

    switch (channel->irrigation_method) {
        case IRRIGATION_DRIP:
        case IRRIGATION_SOAKER_HOSE:
        case IRRIGATION_SUBSURFACE:
            return PROFILE_DRIP;
        case IRRIGATION_SPRINKLER:
        case IRRIGATION_MICRO_SPRAY:
        case IRRIGATION_FLOOD:
            return PROFILE_SPRAY;
        default:
            return PROFILE_AUTO;
    }
}

static hydraulic_profile_t hydraulic_resolve_profile(const watering_channel_t *channel)
{
    if (!channel) {
        return PROFILE_AUTO;
    }

    if (channel->hydraulic.profile_type != PROFILE_AUTO) {
        return channel->hydraulic.profile_type;
    }

    return hydraulic_profile_from_channel(channel);
}

static uint16_t hydraulic_start_ignore_sec(const watering_channel_t *channel)
{
    if (!channel) {
        return 20;
    }

    uint16_t ramp_up = channel->hydraulic.ramp_up_time_sec;
    hydraulic_profile_t profile = hydraulic_resolve_profile(channel);

    if (profile == PROFILE_DRIP || ramp_up > 15) {
        return clamp_u16((uint16_t)(ramp_up + 15), 30, 90);
    }

    if (profile == PROFILE_SPRAY || ramp_up < 5) {
        return clamp_u16((uint16_t)(ramp_up + 5), 8, 20);
    }

    return clamp_u16((uint16_t)(ramp_up + 8), 12, 25);
}

static void hydraulic_get_tolerances(const watering_channel_t *channel,
                                     uint8_t *high_pct,
                                     uint8_t *low_pct)
{
    if (!channel || !high_pct || !low_pct) {
        return;
    }

    if (channel->hydraulic.tolerance_high_percent > 0) {
        *high_pct = channel->hydraulic.tolerance_high_percent;
    } else {
        *high_pct = (hydraulic_resolve_profile(channel) == PROFILE_DRIP) ? 30 : 20;
    }

    if (channel->hydraulic.tolerance_low_percent > 0) {
        *low_pct = channel->hydraulic.tolerance_low_percent;
    } else {
        *low_pct = (hydraulic_resolve_profile(channel) == PROFILE_DRIP) ? 40 : 20;
    }
}

static bool hydraulic_learning_active(const watering_channel_t *channel)
{
    if (!channel || !channel->hydraulic.monitoring_enabled) {
        return false;
    }
    if (channel->hydraulic.stable_runs >= HYDRAULIC_LEARNING_MIN_RUNS) {
        return false;
    }
    return (channel->hydraulic.learning_runs < HYDRAULIC_LEARNING_MAX_RUNS_EXT);
}

static bool hydraulic_flow_stable(void)
{
    uint16_t min = flow_1s_history[0];
    uint16_t max = flow_1s_history[0];

    for (uint8_t i = 1; i < HYDRAULIC_STABLE_WINDOW_S; i++) {
        if (flow_1s_history[i] < min) {
            min = flow_1s_history[i];
        }
        if (flow_1s_history[i] > max) {
            max = flow_1s_history[i];
        }
    }

    if (max == 0) {
        return false;
    }

    return ((max - min) * 100U <= (max * HYDRAULIC_STABLE_VARIATION_PCT));
}

static void hydraulic_learning_reset(uint8_t channel_id, uint32_t now_ms)
{
    learning_ctx.phase = LEARNING_WAIT_STABLE;
    learning_ctx.channel_id = channel_id;
    learning_ctx.start_ms = now_ms;
    learning_ctx.stable_detected_ms = 0;
    learning_ctx.measure_start_ms = 0;
    learning_ctx.measure_start_pulses = 0;
    learning_ctx.stable_windows = 0;
}

static void hydraulic_learning_update(watering_channel_t *channel, uint32_t now_ms,
                                      uint32_t pulses, uint32_t pulses_per_liter)
{
    if (!channel || !hydraulic_learning_active(channel)) {
        return;
    }

    uint8_t channel_id = channel - watering_channels;
    if (learning_ctx.channel_id != channel_id) {
        hydraulic_learning_reset(channel_id, now_ms);
    }

    if (learning_ctx.phase == LEARNING_IDLE) {
        return;
    }

    bool updated = false;

    if (learning_ctx.phase == LEARNING_WAIT_STABLE) {
        uint32_t flow_3s = calc_flow_ml_min(pulse_sum_last_seconds(3), 3, pulses_per_liter);

        if (flow_3s > 0 && hydraulic_flow_stable()) {
            learning_ctx.stable_windows++;
        } else {
            learning_ctx.stable_windows = 0;
        }

        if (learning_ctx.stable_windows >= HYDRAULIC_STABLE_WINDOW_S) {
            uint32_t ramp_sec = (now_ms - learning_ctx.start_ms) / 1000;
            uint16_t ramp_u16 = (uint16_t)ramp_sec;
            if (channel->hydraulic.ramp_up_time_sec != ramp_u16) {
                channel->hydraulic.ramp_up_time_sec = ramp_u16;
                updated = true;
            }
            if (channel->hydraulic.profile_type == PROFILE_AUTO) {
                if (ramp_sec < 5) {
                    channel->hydraulic.profile_type = PROFILE_SPRAY;
                    updated = true;
                } else if (ramp_sec > 15) {
                    channel->hydraulic.profile_type = PROFILE_DRIP;
                    updated = true;
                }
            }

            learning_ctx.phase = LEARNING_MEASURE;
            learning_ctx.measure_start_ms = now_ms;
            learning_ctx.measure_start_pulses = pulses;
            return;
        }

        if ((now_ms - learning_ctx.start_ms) >= (HYDRAULIC_LEARNING_TIMEOUT_S * 1000)) {
            channel->hydraulic.learning_runs++;
            channel->hydraulic.estimated = true;
            learning_ctx.phase = LEARNING_IDLE;
            updated = true;
        }
    } else if (learning_ctx.phase == LEARNING_MEASURE) {
        if ((now_ms - learning_ctx.measure_start_ms) >= (HYDRAULIC_MEASURE_WINDOW_S * 1000)) {
            uint32_t pulse_window = pulses - learning_ctx.measure_start_pulses;
            uint32_t nominal = calc_flow_ml_min(pulse_window, HYDRAULIC_MEASURE_WINDOW_S, pulses_per_liter);

            if (nominal > 0) {
                if (channel->hydraulic.nominal_flow_ml_min == 0) {
                    channel->hydraulic.nominal_flow_ml_min = nominal;
                } else {
                    channel->hydraulic.nominal_flow_ml_min =
                        (uint32_t)((channel->hydraulic.nominal_flow_ml_min * 9 + nominal) / 10);
                }
                channel->hydraulic.stable_runs++;
                channel->hydraulic.is_calibrated =
                    (channel->hydraulic.stable_runs >= HYDRAULIC_LEARNING_MIN_RUNS);
                channel->hydraulic.estimated = false;
                updated = true;
            }

            channel->hydraulic.learning_runs++;
            learning_ctx.phase = LEARNING_IDLE;
            updated = true;
        }
    }

    if (updated) {
        bt_irrigation_hydraulic_status_notify(channel_id);
    }
}

typedef struct __attribute__((packed)) {
    uint32_t timestamp;
    uint8_t channel_id;
    uint8_t alarm_code;
    uint16_t flow_ml_min;
    uint16_t limit_ml_min;
    uint8_t action;
    uint8_t confidence;
} hydraulic_log_entry_t;

static void hydraulic_log_event(uint8_t alarm_code, uint8_t channel_id,
                                uint16_t flow_ml_min, uint16_t limit_ml_min,
                                hydraulic_log_action_t action, uint8_t confidence)
{
#ifdef CONFIG_HISTORY_EXTERNAL_FLASH
    struct fs_dirent entry_stat;
    if (fs_stat(HYDRAULIC_LOG_PATH, &entry_stat) == 0 &&
        entry_stat.size >= HYDRAULIC_LOG_MAX_BYTES) {
        struct fs_file_t trunc_file;
        fs_file_t_init(&trunc_file);
        if (fs_open(&trunc_file, HYDRAULIC_LOG_PATH, FS_O_WRITE | FS_O_TRUNC) == 0) {
            fs_close(&trunc_file);
        }
    }

    struct fs_file_t file;
    fs_file_t_init(&file);
    if (fs_open(&file, HYDRAULIC_LOG_PATH, FS_O_WRITE | FS_O_CREATE) != 0) {
        return;
    }

    fs_seek(&file, 0, FS_SEEK_END);

    hydraulic_log_entry_t entry = {
        .timestamp = timezone_get_unix_utc(),
        .channel_id = channel_id,
        .alarm_code = alarm_code,
        .flow_ml_min = flow_ml_min,
        .limit_ml_min = limit_ml_min,
        .action = (uint8_t)action,
        .confidence = confidence
    };

    fs_write(&file, &entry, sizeof(entry));
    fs_close(&file);
#else
    (void)alarm_code;
    (void)channel_id;
    (void)flow_ml_min;
    (void)limit_ml_min;
    (void)action;
    (void)confidence;
#endif
}

/**
 * @brief Check for flow anomalies and update system status
 * 
 * This function detects two main anomalies:
 * 1. No flow when a valve is open (may indicate empty tank or clogged line)
 * 2. Unexpected flow when all valves are closed (may indicate leak or valve failure)
 * 
 * @return WATERING_SUCCESS on success or WATERING_ERROR_BUSY if in fault state
 */
watering_error_t check_flow_anomalies(void)
{
    uint32_t now = k_uptime_get_32();
    
    /* Prefer a short wait instead of skipping checks entirely so we still detect leaks */
    if (k_mutex_lock(&flow_monitor_mutex, K_MSEC(10)) != 0) {
        return WATERING_SUCCESS; /* give up this cycle but try again quickly */
    }
    
    // Only check periodically to allow flow to stabilize
    if ((now - last_flow_check_time) < FLOW_CHECK_THRESHOLD_MS) {
        k_mutex_unlock(&flow_monitor_mutex);
        return WATERING_SUCCESS;
    }
    
    last_flow_check_time = now;
    
    // Skip checks if system is in fault state
    if (system_status == WATERING_STATUS_FAULT) {
        k_mutex_unlock(&flow_monitor_mutex);
        return WATERING_ERROR_BUSY;
    }
    
    // CRITICAL FIX: Get a snapshot of the watering task state to minimize
    // time spent in critical sections
    bool task_active = false;
    bool task_paused = false;
    uint32_t start_time = 0;

    watering_task_t *cur_task_ptr = watering_task_state.current_active_task;
    if (cur_task_ptr != NULL) {
        task_active = true;
        task_paused = watering_task_state.task_paused;
        start_time  = watering_task_state.watering_start_time;
    }

    /* -------- detect task change -> reset stall watchdog -------------- */
    if (cur_task_ptr != watched_task) {
        if (watched_task != NULL && cur_task_ptr == NULL) {
            last_valve_closed_ms = now;
        }
        watched_task          = cur_task_ptr;
        last_task_pulses      = get_pulse_count();
        last_pulse_update_ts  = now;
        hydraulic_reset_runtime_state(last_task_pulses, now);
        flow_error_attempts = 0;
        retry_cooldown_until = 0;
        if (cur_task_ptr && cur_task_ptr->channel) {
            hydraulic_learning_reset((uint8_t)(cur_task_ptr->channel - watering_channels), now);
        } else {
            learning_ctx.phase = LEARNING_IDLE;
        }
    }

    // Get pulse count and flow rate with timeout protection
    uint32_t pulses = get_pulse_count();
    uint32_t flow_rate = get_flow_rate();  /* Get stabilized flow rate */
    
    uint32_t pulses_per_liter = get_flow_calibration();
    if (pulses_per_liter == 0) {
        pulses_per_liter = DEFAULT_PULSES_PER_LITER;
    }

    hydraulic_update_pulse_history(pulses, now, pulses_per_liter);

    uint32_t flow_5s = calc_flow_ml_min(pulse_sum_last_seconds(5), 5, pulses_per_liter);
    uint32_t flow_30s = calc_flow_ml_min(pulse_sum_last_seconds(30), 30, pulses_per_liter);
    uint32_t flow_60s = calc_flow_ml_min(pulse_sum_last_seconds(60), 60, pulses_per_liter);

    /* Debug info every 10 seconds when task is active */
    static uint32_t last_debug_time = 0;
    if (task_active && (now - last_debug_time > 10000)) {
        printk("Flow monitor: pulses=%u, rate=%u pps, flow5=%u, flow30=%u, flow60=%u\n",
               pulses, flow_rate, flow_5s, flow_30s, flow_60s);
        last_debug_time = now;
    }

    if (task_active && !task_paused) {
        watering_channel_t *channel = cur_task_ptr ? cur_task_ptr->channel : NULL;
        if (!channel) {
            k_mutex_unlock(&flow_monitor_mutex);
            return WATERING_SUCCESS;
        }

        uint8_t channel_id = channel - watering_channels;
        if (channel_id >= WATERING_CHANNELS_COUNT) {
            k_mutex_unlock(&flow_monitor_mutex);
            return WATERING_SUCCESS;
        }

        hydraulic_learning_update(channel, now, pulses, pulses_per_liter);

        uint16_t start_ignore_sec = hydraulic_start_ignore_sec(channel);
        bool ignore_window = (now - start_time) < ((uint32_t)start_ignore_sec * 1000U);

        if (pulses > last_task_pulses) {
            last_task_pulses = pulses;
            last_pulse_update_ts = now;
        }

        uint8_t high_pct = 0;
        uint8_t low_pct = 0;
        hydraulic_get_tolerances(channel, &high_pct, &low_pct);
        uint32_t nominal = channel->hydraulic.nominal_flow_ml_min;
        uint32_t high_limit = (nominal > 0) ?
                              (nominal + (nominal * high_pct) / 100) :
                              HYDRAULIC_ABS_HIGH_FLOW_ML_MIN;
        uint32_t low_limit = 0;
        if (nominal > 0) {
            uint32_t low_calc = (nominal * low_pct) / 100;
            low_limit = (nominal > low_calc) ? (nominal - low_calc) : 0;
            if (low_limit < HYDRAULIC_MIN_NO_FLOW_ML_MIN) {
                low_limit = HYDRAULIC_MIN_NO_FLOW_ML_MIN;
            }
        }

        if (!ignore_window && flow_5s > high_limit) {
            high_flow_consecutive++;
        } else {
            high_flow_consecutive = 0;
        }

        if (high_flow_consecutive >= HYDRAULIC_HIGH_FLOW_HOLD_S) {
            bt_irrigation_alarm_notify(ALARM_HIGH_FLOW, channel_id);
            channel->hydraulic_anomaly.high_flow_runs++;
            channel->hydraulic_anomaly.last_anomaly_epoch = timezone_get_unix_utc();
            k_mutex_unlock(&flow_monitor_mutex);
            valve_close_all();
            k_sleep(K_MSEC(HYDRAULIC_POST_CLOSE_IGNORE_MS));
            uint32_t pulses_after = get_pulse_count();
            k_mutex_lock(&flow_monitor_mutex, K_FOREVER);

            bool still_flowing = (pulses_after > pulses + 2);
            if (still_flowing) {
                watering_hydraulic_set_global_lock(HYDRAULIC_LOCK_HARD, HYDRAULIC_LOCK_REASON_HIGH_FLOW);
                bt_irrigation_alarm_notify(ALARM_GLOBAL_LOCK, channel_id);
                hydraulic_log_event(ALARM_HIGH_FLOW, channel_id,
                                    (uint16_t)flow_5s, (uint16_t)high_limit,
                                    HYDRAULIC_LOG_ACTION_GLOBAL_LOCK, 95);
            } else {
                watering_hydraulic_set_channel_lock(channel_id, HYDRAULIC_LOCK_HARD, HYDRAULIC_LOCK_REASON_HIGH_FLOW);
                bt_irrigation_alarm_notify(ALARM_CHANNEL_LOCK, channel_id);
                hydraulic_log_event(ALARM_HIGH_FLOW, channel_id,
                                    (uint16_t)flow_5s, (uint16_t)high_limit,
                                    HYDRAULIC_LOG_ACTION_CHANNEL_LOCK, 90);
            }

            k_mutex_unlock(&flow_monitor_mutex);
            watering_stop_current_task();
            return WATERING_SUCCESS;
        }

        bool never_started = (!ignore_window && pulses == 0);
        bool stalled_flow = (!ignore_window &&
                             pulses == last_task_pulses &&
                             (now - last_pulse_update_ts > NO_FLOW_STALL_TIMEOUT_MS));

        if ((never_started || stalled_flow) && (retry_cooldown_until == 0 || now >= retry_cooldown_until)) {
            printk("ALERT: No water flow detected with valve open! (attempt %d/%d)\n",
                   flow_error_attempts + 1, MAX_FLOW_ERROR_ATTEMPTS);
            flow_error_attempts++;

            if (system_status != WATERING_STATUS_NO_FLOW) {
                bt_irrigation_alarm_notify(ALARM_NO_FLOW, flow_error_attempts);
            }
            system_status = WATERING_STATUS_NO_FLOW;
            ble_status_update();

            if (flow_error_attempts < MAX_FLOW_ERROR_ATTEMPTS) {
                printk("NO_FLOW: TOGGLE - Closing all valves (ch=%d + master)\n", channel_id + 1);
                valve_close_all();
                last_task_pulses = 0;
                last_pulse_update_ts = 0;
                reset_pulse_count();

                printk("NO_FLOW: Waiting %d ms before reopening...\n", NO_FLOW_RETRY_COOLDOWN_MS);
                k_mutex_unlock(&flow_monitor_mutex);
                k_sleep(K_MSEC(NO_FLOW_RETRY_COOLDOWN_MS));
                k_mutex_lock(&flow_monitor_mutex, K_FOREVER);

                printk("NO_FLOW: TOGGLE - Reopening channel %d valve\n", channel_id + 1);
                watering_error_t reopen_err = watering_channel_on(channel_id);
                if (reopen_err != WATERING_SUCCESS) {
                    valve_close_all();
                } else {
                    watering_task_state.watering_start_time = k_uptime_get_32();
                    last_pulse_update_ts = k_uptime_get_32();
                    watched_task = watering_task_state.current_active_task;
                }
            } else {
                channel->hydraulic_anomaly.no_flow_runs++;
                channel->hydraulic_anomaly.last_anomaly_epoch = timezone_get_unix_utc();
                if (channel->hydraulic_anomaly.no_flow_runs >= 3) {
                    watering_hydraulic_set_channel_lock(channel_id, HYDRAULIC_LOCK_HARD,
                                                        HYDRAULIC_LOCK_REASON_NO_FLOW);
                    bt_irrigation_alarm_notify(ALARM_CHANNEL_LOCK, channel_id);
                    hydraulic_log_event(ALARM_NO_FLOW, channel_id,
                                        (uint16_t)flow_30s, (uint16_t)low_limit,
                                        HYDRAULIC_LOG_ACTION_CHANNEL_LOCK, 85);
                } else {
                    watering_hydraulic_set_channel_lock(channel_id, HYDRAULIC_LOCK_SOFT,
                                                        HYDRAULIC_LOCK_REASON_NO_FLOW);
                    hydraulic_log_event(ALARM_NO_FLOW, channel_id,
                                        (uint16_t)flow_30s, (uint16_t)low_limit,
                                        HYDRAULIC_LOG_ACTION_WARN, 70);
                }
                k_mutex_unlock(&flow_monitor_mutex);
                watering_stop_current_task();
                return WATERING_SUCCESS;
            }
        } else if (pulses > 0) {
            flow_error_attempts = 0;
            retry_cooldown_until = 0;
            channel->hydraulic_anomaly.no_flow_runs = 0;
            if (system_status == WATERING_STATUS_NO_FLOW) {
                system_status = WATERING_STATUS_OK;
                bt_irrigation_alarm_notify(ALARM_NO_FLOW, 0);
                ble_status_update();
            }
        }

        if (!ignore_window && nominal > 0 && !hydraulic_learning_active(channel)) {
            if (flow_30s > 0 && flow_30s < low_limit) {
                low_flow_consecutive++;
                if (low_flow_consecutive >= HYDRAULIC_LOW_FLOW_HOLD_S) {
                    uint32_t now_epoch = timezone_get_unix_utc();
                    if (now_epoch == 0 ||
                        now_epoch - channel->hydraulic_anomaly.last_anomaly_epoch > 3600) {
                        bt_irrigation_alarm_notify(ALARM_LOW_FLOW, channel_id);
                        hydraulic_log_event(ALARM_LOW_FLOW, channel_id,
                                            (uint16_t)flow_30s, (uint16_t)low_limit,
                                            HYDRAULIC_LOG_ACTION_WARN, 60);
                        channel->hydraulic_anomaly.last_anomaly_epoch = now_epoch;
                        bt_irrigation_hydraulic_status_notify(channel_id);
                    }
                    low_flow_consecutive = 0;
                }
            } else {
                low_flow_consecutive = 0;
            }
        } else {
            low_flow_consecutive = 0;
        }
    } else {
        if (cur_task_ptr == NULL && watched_task != NULL) {
            last_valve_closed_ms = now;
        }

        if (!static_test_active &&
            (now - last_valve_closed_ms) > HYDRAULIC_POST_CLOSE_IGNORE_MS) {
            uint32_t pulses_30s = pulse_sum_last_seconds(HYDRAULIC_UNEXPECTED_FLOW_WINDOW_S);
            if (pulses_30s > HYDRAULIC_UNEXPECTED_FLOW_PULSES) {
                unexpected_flow_consecutive++;
                if (system_status != WATERING_STATUS_UNEXPECTED_FLOW &&
                    system_status != WATERING_STATUS_LOCKED) {
                    system_status = WATERING_STATUS_UNEXPECTED_FLOW;
                    ble_status_update();
                }
                if (unexpected_flow_consecutive >= HYDRAULIC_UNEXPECTED_FLOW_PERSIST_S) {
                    watering_hydraulic_set_global_lock(HYDRAULIC_LOCK_HARD,
                                                       HYDRAULIC_LOCK_REASON_UNEXPECTED_FLOW);
                    bt_irrigation_alarm_notify(ALARM_UNEXPECTED_FLOW, pulses_30s);
                    bt_irrigation_alarm_notify(ALARM_GLOBAL_LOCK, 0);
                    hydraulic_log_event(ALARM_UNEXPECTED_FLOW, 0xFF,
                                        (uint16_t)flow_30s, 0,
                                        HYDRAULIC_LOG_ACTION_GLOBAL_LOCK, 90);
                }
            } else {
                unexpected_flow_consecutive = 0;
                if (system_status == WATERING_STATUS_UNEXPECTED_FLOW) {
                    system_status = WATERING_STATUS_OK;
                    bt_irrigation_alarm_notify(ALARM_UNEXPECTED_FLOW, 0);
                    ble_status_update();
                }
            }
        }
    }
    
    /* Rain sensor status monitoring */
    static uint32_t last_rain_check = 0;
    static uint32_t last_rain_pulses_applied = 0;
    if ((now - last_rain_check) > 30000) { /* Check every 30 seconds */
        last_rain_check = now;
        
        /* Check rain sensor connectivity and status */
        if (rain_sensor_is_active()) {
            /* Rain sensor is working - check for recent activity */
            uint32_t last_pulse_time = rain_sensor_get_last_pulse_time();
            uint32_t time_since_pulse = now - last_pulse_time;
            
            /* If no pulses for more than 7 days during rain season, sensor might be disconnected */
            if (time_since_pulse > (7 * 24 * 3600 * 1000)) {
                static bool rain_sensor_warning_logged = false;
                if (!rain_sensor_warning_logged) {
                    printk("WARNING: Rain sensor inactive for %u days - check connection\n", 
                           time_since_pulse / (24 * 3600 * 1000));
                    rain_sensor_warning_logged = true;
                }
            } else {
                /* Reset warning flag if sensor shows activity */
                static bool rain_sensor_warning_logged = false;
                rain_sensor_warning_logged = false;
            }
        } else {
            /* Rain sensor initialization failed or not connected */
            static bool rain_init_warning_logged = false;
            if (!rain_init_warning_logged) {
                printk("WARNING: Rain sensor not initialized - rain integration disabled\n");
                rain_init_warning_logged = true;
            }
        }
        
        /* Run rain-related checks only when sensor is initialized/enabled */
        if (rain_sensor_is_enabled()) {
            /* Update hourly tracking and persist completed hours to history */
            rain_sensor_update_hourly();

            /* Apply incremental rain events to FAO-56 deficit tracking */
            uint32_t current_pulses = rain_sensor_get_pulse_count();
            if (rain_sensor_is_integration_enabled()) {
                if (current_pulses < last_rain_pulses_applied) {
                    last_rain_pulses_applied = current_pulses;
                }

                uint32_t delta_pulses = current_pulses - last_rain_pulses_applied;
                if (delta_pulses > 0) {
                    float delta_mm = (float)delta_pulses * rain_sensor_get_calibration();
                    if (delta_mm > 0.0f) {
                        environmental_data_t env_data = {0};
                        float air_temp_c = 20.0f;
                        if (env_sensors_read(&env_data) == WATERING_SUCCESS && env_data.temp_valid) {
                            air_temp_c = env_data.air_temp_mean_c;
                        }
                        (void)fao56_apply_rainfall_increment(delta_mm, air_temp_c);
                    }
                    last_rain_pulses_applied = current_pulses;
                }
            } else {
                last_rain_pulses_applied = current_pulses;
            }

            /* Check rain integration system health */
            /* Perform periodic maintenance on rain history system (independent of integration) */
            watering_error_t rain_maintenance_result = rain_history_maintenance();
            if (rain_maintenance_result != WATERING_SUCCESS) {
                printk("WARNING: Rain history maintenance failed: %d\n", rain_maintenance_result);
            }
            
            /* Run comprehensive rain sensor diagnostics */
            rain_sensor_periodic_diagnostics();
            
            /* Run rain integration health check */
            if (rain_integration_is_enabled()) {
                rain_integration_periodic_health_check();
            }
            
            /* Check for critical health conditions */
            if (rain_sensor_is_health_critical()) {
                printk("CRITICAL: Rain sensor health is critical - check sensor connection\n");
                /* Could trigger system alert or notification here */
            }
        }
    }
    
    k_mutex_unlock(&flow_monitor_mutex);
    return WATERING_SUCCESS;
}

watering_error_t hydraulic_run_static_test(void)
{
    if (static_test_active) {
        return WATERING_ERROR_BUSY;
    }

    if (watering_task_state.task_in_progress || watering_task_state.current_active_task) {
        return WATERING_ERROR_BUSY;
    }

    static_test_active = true;

    watering_error_t err = master_valve_force_open();
    if (err != WATERING_SUCCESS) {
        static_test_active = false;
        return err;
    }

    k_sleep(K_SECONDS(10));

    err = master_valve_force_close();
    if (err != WATERING_SUCCESS) {
        static_test_active = false;
        return err;
    }

    k_sleep(K_SECONDS(5));

    uint32_t baseline = get_pulse_count();
    uint32_t leak_pulses = 0;

    for (int i = 0; i < 60; i++) {
        if (watering_task_state.task_in_progress || watering_task_state.current_active_task) {
            static_test_active = false;
            return WATERING_ERROR_BUSY;
        }
        k_sleep(K_SECONDS(1));
        leak_pulses = get_pulse_count() - baseline;
    }

    static_test_active = false;
    hydraulic_reset_runtime_state(get_pulse_count(), k_uptime_get_32());
    last_valve_closed_ms = k_uptime_get_32();

    if (leak_pulses > HYDRAULIC_MAINLINE_LEAK_PULSES) {
        watering_hydraulic_set_global_lock(HYDRAULIC_LOCK_HARD,
                                           HYDRAULIC_LOCK_REASON_MAINLINE_LEAK);
        bt_irrigation_alarm_notify(ALARM_MAINLINE_LEAK, leak_pulses);
        bt_irrigation_alarm_notify(ALARM_GLOBAL_LOCK, 0);
        hydraulic_log_event(ALARM_MAINLINE_LEAK, 0xFF,
                            0, 0, HYDRAULIC_LOG_ACTION_GLOBAL_LOCK, 95);
        return WATERING_ERROR_HARDWARE;
    }

    return WATERING_SUCCESS;
}

/**
 * @brief Thread function for flow monitoring
 * 
 * Periodically reports flow information and system status
 */
static void flow_monitor_fn(void *p1, void *p2, void *p3) {
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);
    
    printk("Flow sensor monitoring task started\n");
    
    // Timer pentru notificările periodice de progres
    static uint32_t last_progress_notification_time = 0;
    static uint32_t last_significant_progress = 0xFFFFFFFF; // Pentru detectarea schimbărilor semnificative
    static watering_task_t *last_monitored_task = NULL;     // Pentru detectarea completion-ului
    static bool last_task_in_progress = false;              // Pentru detectarea tranziției
    
    while (!exit_tasks) {
        uint32_t pulses = get_pulse_count();
        uint32_t now_ms = k_uptime_get_32();

        // Verifică dacă există un task activ pentru notificări de progres
        watering_task_t *current_task = watering_get_current_task();
        bool should_send_progress_update = false;
        bool task_just_completed = false;
        
        // Detectează completion-ul task-ului
        if (last_task_in_progress && (!watering_task_state.task_in_progress || current_task == NULL)) {
            task_just_completed = true;
        }
        
        // Actualizează starea monitorizată
        last_monitored_task = current_task;
        last_task_in_progress = watering_task_state.task_in_progress;
        
        // Trimite notificări de progres la fiecare 2 secunde dacă există task activ
        if (current_task && watering_task_state.task_in_progress && !watering_task_state.task_paused) {
            // Calculează progresul curent
            uint32_t elapsed_ms = now_ms - watering_task_state.watering_start_time;
            
            // Scade timpul pauzat din timpul total
            if (watering_task_state.total_paused_time > 0) {
                elapsed_ms -= watering_task_state.total_paused_time;
            }
            
            uint32_t current_progress = 0;
            
            if (current_task->channel->watering_event.watering_mode == WATERING_BY_DURATION) {
                // Pentru mode duration: progres = timp scurs efectiv / timp total * 100
                uint32_t target_ms = current_task->channel->watering_event.watering.by_duration.duration_minutes * 60000;
                if (target_ms > 0) {
                    current_progress = (elapsed_ms * 100) / target_ms;
                    if (current_progress > 100) current_progress = 100;
                }
            } else {
                // Pentru mode volume: progres = volum scurs / volum total * 100
                uint32_t pulses_per_liter;
                if (watering_get_flow_calibration(&pulses_per_liter) == WATERING_SUCCESS && pulses_per_liter > 0) {
                    uint32_t target_pulses = current_task->channel->watering_event.watering.by_volume.volume_liters * pulses_per_liter;
                    if (target_pulses > 0) {
                        current_progress = (pulses * 100) / target_pulses;
                        if (current_progress > 100) current_progress = 100;
                    }
                }
            }
            
            // Trimite notificare dacă:
            // 1. Au trecut cel puțin 200ms de la ultima notificare (5Hz când activ)
            // 2. Progresul s-a schimbat cu cel puțin 1% față de ultima notificare (ultra sensibil)
            bool time_elapsed = (now_ms - last_progress_notification_time >= 200);
            bool significant_change = (last_significant_progress == 0xFFFFFFFF) || 
                                     (current_progress >= last_significant_progress + 1) ||
                                     (current_progress <= last_significant_progress - 1);
            
            if (time_elapsed && (significant_change || (now_ms - last_progress_notification_time >= 1000))) {
                should_send_progress_update = true;
                last_progress_notification_time = now_ms;
                last_significant_progress = current_progress;
            }
        } else {
            // Resetează timerul când nu există task activ
            last_significant_progress = 0xFFFFFFFF;
        }
        
        // Trimite notificare immediaă la completion task
        if (task_just_completed) {
            should_send_progress_update = true;
            last_progress_notification_time = now_ms;
            last_significant_progress = 100; // 100% complete
        }

        /* Report abnormal statuses when pulses observed */
        if (pulses > 0) {
            // Report on any abnormal system status
            watering_status_t current_status;
            if (watering_get_status(&current_status) == WATERING_SUCCESS && 
                current_status != WATERING_STATUS_OK) {
                switch (current_status) {
                    case WATERING_STATUS_NO_FLOW:
                        printk("WARNING: No flow detected, attempts: %d/%d\n", flow_error_attempts,
                               MAX_FLOW_ERROR_ATTEMPTS);
                        break;
                    case WATERING_STATUS_UNEXPECTED_FLOW:
                        printk("Unexpected flow detected!\n");
                        break;
                    case WATERING_STATUS_FAULT:
                        printk("ERROR: System in fault state! Manual intervention needed.\n");
                        break;
                    case WATERING_STATUS_RTC_ERROR:
                        printk("ERROR: RTC failure! Time-based scheduling unavailable.\n");
                        break;
                    case WATERING_STATUS_LOW_POWER:
                        printk("NOTICE: System in low power mode.\n");
                        break;
                    case WATERING_STATUS_LOCKED:
                        printk("ALERT: System locked by hydraulic safety.\n");
                        break;
                    default:
                        break;
                }
            }
        }
        
        // Trimite notificarea de progres dacă este necesară
        if (should_send_progress_update) {
            #ifdef CONFIG_BT
            // Apelează notificarea pentru task-ul curent
            int notify_result = bt_irrigation_current_task_notify();
            if (notify_result != 0) {
                printk("❌ Failed to send progress notification: %d\n", notify_result);
            }
            #endif
        }
        
        // Adjust sleep duration based on power mode and task activity
        uint32_t sleep_time = 200; // Default 200ms for 5Hz ultra responsive when task active
        
        // If no active task, use longer sleep times to save power
        if (!current_task || !watering_task_state.task_in_progress) {
            sleep_time = 1000; // 1 second when idle
        }
        
        switch (current_power_mode) {
            case POWER_MODE_ENERGY_SAVING:
                sleep_time = current_task ? 1000 : 5000; // 1s active, 5s idle
                break;
            case POWER_MODE_ULTRA_LOW_POWER:
                sleep_time = current_task ? 5000 : 30000; // 5s active, 30s idle
                break;
            default:
                break;
        }
        
        /* Periodic rain sensor health monitoring */
        static uint32_t last_rain_health_check = 0;
        if ((now_ms - last_rain_health_check) > 300000) { /* Every 5 minutes */
            last_rain_health_check = now_ms;
            watering_error_t rain_health = check_rain_sensor_health();
            if (rain_health != WATERING_SUCCESS) {
                printk("Rain sensor health check failed: %d\n", rain_health);
            }
        }
        
        k_sleep(K_MSEC(sleep_time));
    }
    
    printk("Flow sensor monitoring task stopped\n");
}

/**
 * @brief Initialize the flow monitoring subsystem
 * 
 * Sets up the monitoring thread and resets status variables
 * 
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t flow_monitor_init(void) {
    k_mutex_lock(&flow_monitor_mutex, K_FOREVER);
    
    system_status = WATERING_STATUS_OK;
    ble_status_update();                              /* NEW */
    flow_error_attempts = 0;
    last_flow_check_time = 0;
    exit_tasks = false;
    learning_ctx.phase = LEARNING_IDLE;
    hydraulic_reset_runtime_state(get_pulse_count(), k_uptime_get_32());

    /* Start background monitoring thread once */
    if (!monitor_started) {
        k_tid_t tid = k_thread_create(&flow_monitor_data,
                                      flow_monitor_stack,
                                      K_THREAD_STACK_SIZEOF(flow_monitor_stack),
                                      flow_monitor_fn,
                                      NULL, NULL, NULL,
                                      K_PRIO_PREEMPT(6), 0, K_NO_WAIT);
        if (tid) {
            k_thread_name_set(tid, "flow_monitor");
            monitor_started = true;
            printk("Flow monitoring task started\n");
        } else {
            printk("ERROR: Failed to start flow monitoring task\n");
            k_mutex_unlock(&flow_monitor_mutex);
            return WATERING_ERROR_CONFIG;
        }
    }

    k_mutex_unlock(&flow_monitor_mutex);
    return WATERING_SUCCESS;
}

/**
 * @brief Reset the system from fault state
 * 
 * @return WATERING_SUCCESS if successfully reset, error code if not in fault state
 */
watering_error_t watering_reset_fault(void) {
    k_mutex_lock(&flow_monitor_mutex, K_FOREVER);
    
    watering_status_t current_status;
    if (watering_get_status(&current_status) != WATERING_SUCCESS) {
        k_mutex_unlock(&flow_monitor_mutex);
        return WATERING_ERROR_BUSY;
    }
    
    if (current_status == WATERING_STATUS_FAULT) {
        printk("Resetting system from fault state\n");
        system_status = WATERING_STATUS_OK;
        ble_status_update();                          /* NEW */
        flow_error_attempts = 0;
        retry_cooldown_until = 0;  /* Clear retry cooldown */
        
        // Try to recover the system state
        attempt_error_recovery(WATERING_SUCCESS);
        
        k_mutex_unlock(&flow_monitor_mutex);
        return WATERING_SUCCESS;
    }
    
    k_mutex_unlock(&flow_monitor_mutex);
    return WATERING_ERROR_INVALID_PARAM;  // Not in fault state
}

/* ---------- NEW: public helper to clear flow error counters -------- */
void flow_monitor_clear_errors(void)
{
    k_mutex_lock(&flow_monitor_mutex, K_FOREVER);
    flow_error_attempts   = 0;
    last_flow_check_time  = 0;
    last_task_pulses      = 0;
    retry_cooldown_until  = 0;  /* Clear retry cooldown */
    watched_task          = NULL;
    learning_ctx.phase    = LEARNING_IDLE;
    high_flow_consecutive = 0;
    low_flow_consecutive  = 0;
    unexpected_flow_consecutive = 0;
    hydraulic_reset_runtime_state(get_pulse_count(), k_uptime_get_32());
    /* Only clear flow-related flags; keep other fault states intact */
    if (system_status == WATERING_STATUS_NO_FLOW ||
        system_status == WATERING_STATUS_UNEXPECTED_FLOW) {
        system_status = WATERING_STATUS_OK;
    }
    k_mutex_unlock(&flow_monitor_mutex);
}
/**
 * @brief Rain sensor error recovery function
 * 
 * Attempts to recover from rain sensor errors and reinitialize the system
 * 
 * @return WATERING_SUCCESS on successful recovery, error code on failure
 */
watering_error_t rain_sensor_error_recovery(void)
{
    static uint8_t rain_recovery_attempts = 0;
    static uint32_t last_recovery_attempt = 0;
    uint32_t now = k_uptime_get_32();
    
    /* Limit recovery attempts to prevent system overload */
    if (rain_recovery_attempts >= 3) {
        if ((now - last_recovery_attempt) < 300000) { /* 5 minutes */
            return WATERING_ERROR_BUSY; /* Too many recent attempts */
        } else {
            rain_recovery_attempts = 0; /* Reset after cooldown period */
        }
    }
    
    printk("Attempting rain sensor error recovery (attempt %d/3)\n", rain_recovery_attempts + 1);
    last_recovery_attempt = now;
    rain_recovery_attempts++;
    
    /* Try to reinitialize rain sensor */
    watering_error_t ret = rain_sensor_init();
    if (ret != WATERING_SUCCESS) {
        printk("Rain sensor reinitialization failed: %d\n", ret);
        return ret;
    }
    
    /* Try to reinitialize rain integration */
    ret = rain_integration_init();
    if (ret != WATERING_SUCCESS) {
        printk("Rain integration reinitialization failed: %d\n", ret);
        return ret;
    }
    
    /* Try to reinitialize rain history */
    ret = rain_history_init();
    if (ret != WATERING_SUCCESS) {
        printk("Rain history reinitialization failed: %d\n", ret);
        return ret;
    }
    
    printk("Rain sensor error recovery successful\n");
    rain_recovery_attempts = 0; /* Reset on success */
    return WATERING_SUCCESS;
}

/**
 * @brief Check rain sensor system health and perform recovery if needed
 * 
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t check_rain_sensor_health(void)
{
    static uint32_t last_health_check = 0;
    uint32_t now = k_uptime_get_32();
    
    /* Check every 5 minutes */
    if ((now - last_health_check) < 300000) {
        return WATERING_SUCCESS;
    }
    
    last_health_check = now;
    
    /* Check if rain sensor is responsive */
    if (!rain_sensor_is_active()) {
        printk("Rain sensor health check failed - attempting recovery\n");
        return rain_sensor_error_recovery();
    }
    
    /* Check rain history system integrity */
    watering_error_t ret = rain_history_validate_data();
    if (ret != WATERING_SUCCESS) {
        printk("Rain history data validation failed: %d\n", ret);
        /* Try to recover by clearing corrupted data */
        rain_history_clear_all();
    }
    
    /* Rain integration config is now per-channel - no global config to check */
    
    return WATERING_SUCCESS;
}

/**
 * @brief Get rain sensor system status for health monitoring
 * 
 * @param status_buffer Buffer to store status string
 * @param buffer_size Size of the status buffer
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t get_rain_sensor_status(char *status_buffer, uint16_t buffer_size)
{
    if (!status_buffer || buffer_size < 100) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    int written = 0;
    
    /* Rain sensor status */
    if (rain_sensor_is_active()) {
        uint32_t last_pulse = rain_sensor_get_last_pulse_time();
        uint32_t time_since = (k_uptime_get_32() - last_pulse) / 1000;
        written += snprintf(status_buffer + written, buffer_size - written,
                           "Rain sensor: Active (last pulse %us ago)\n", time_since);
    } else {
        written += snprintf(status_buffer + written, buffer_size - written,
                           "Rain sensor: Inactive/Error\n");
    }
    
    /* Rain integration status - now per-channel */
    if (rain_integration_is_enabled()) {
        written += snprintf(status_buffer + written, buffer_size - written,
               "Rain integration: Enabled on some channels (per-channel config)\n");
    } else {
        written += snprintf(status_buffer + written, buffer_size - written,
                           "Rain integration: No channels have rain compensation enabled\n");
    }
    
    /* Recent rainfall data */
    float recent_24h = rain_history_get_last_24h();
    float recent_48h = rain_history_get_recent_total(48);
    written += snprintf(status_buffer + written, buffer_size - written,
                       "Recent rainfall: 24h=%.2fmm, 48h=%.2fmm\n",
                       (double)recent_24h, (double)recent_48h);
    
    /* Rain history status */
    rain_history_stats_t stats;
    watering_error_t ret = rain_history_get_stats(&stats);
    if (ret == WATERING_SUCCESS) {
        written += snprintf(status_buffer + written, buffer_size - written,
                           "Rain history: %u hourly, %u daily entries\n",
                           stats.hourly_entries, stats.daily_entries);
    }
    
    return WATERING_SUCCESS;
}
