#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <stdio.h> /* ensure snprintf prototype visible on all build paths */
#include "flow_sensor.h"
#include "watering.h"
#include "watering_internal.h"
#include "watering_history.h"
#include "bt_irrigation_service.h"   /* + BLE notifications */
#include "rain_sensor.h"             /* Rain sensor monitoring */
#include "rain_integration.h"        /* Rain integration status */
#include "rain_history.h"            /* Rain history monitoring */
/* stdio already included above for snprintf */

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

/** Counter for consecutive flow errors */
static uint8_t flow_error_attempts = 0;

/** Timestamp of last flow check */
static uint32_t last_flow_check_time = 0;

/** Mutex for protecting flow monitor state */
K_MUTEX_DEFINE(flow_monitor_mutex);

/* --- new constants ---------------------------------------------------- */
#define NO_FLOW_STALL_TIMEOUT_MS   3000   /* 3 s without new pulses ⇒ error */

/* --- new state vars --------------------------------------------------- */
static uint32_t last_task_pulses      = 0;
static uint32_t last_pulse_update_ts  = 0;
static watering_task_t *watched_task  = NULL;


size_t flow_monitor_get_unused_stack(void)
{
    /* Stack diagnostics rely on CONFIG_THREAD_STACK_INFO; return 0 otherwise */
    return 0;
}

/* --- alarm codes used by bt_irrigation_alarm_notify ------------------- */
#define ALARM_NO_FLOW          1
#define ALARM_UNEXPECTED_FLOW  2

/* Helper used throughout this file */
static inline void ble_status_update(void)
{
    bt_irrigation_system_status_update(system_status);
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
    
    // CRITICAL FIX: Use non-blocking mutex to prevent deadlock
    if (k_mutex_lock(&flow_monitor_mutex, K_NO_WAIT) != 0) {
        // If we can't get the mutex immediately, skip this check
        // It's better to miss a check than to hang the system
        return WATERING_SUCCESS;
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
        watched_task          = cur_task_ptr;
        last_task_pulses      = get_pulse_count();
        last_pulse_update_ts  = now;
    }

    // Get pulse count and flow rate with timeout protection
    uint32_t pulses = get_pulse_count();
    uint32_t flow_rate = get_flow_rate();  /* Get stabilized flow rate */
    
    /* Debug info every 10 seconds when task is active */
    static uint32_t last_debug_time = 0;
    if (task_active && (now - last_debug_time > 10000)) {
        printk("Flow monitor: pulses=%u, rate=%u pps, task_pulses=%u\n", 
               pulses, flow_rate, last_task_pulses);
        last_debug_time = now;
    }
    
    // Check for no-flow condition when a valve is open
    if (task_active && !task_paused) {
        /* update watchdog when flow increases */
        if (pulses > last_task_pulses) {
            last_task_pulses     = pulses;
            last_pulse_update_ts = now;
        }

        /* existing “never started” 5 s logic stays unchanged */
        bool never_started = (pulses == 0 && now - start_time > 5000);

        /* new: no new pulses for a while => stalled flow */
        bool stalled_flow = (pulses == last_task_pulses) &&
                            (now - last_pulse_update_ts > NO_FLOW_STALL_TIMEOUT_MS);

        if (never_started || stalled_flow) {
            printk("ALERT: No water flow detected with valve open!\n");
            flow_error_attempts++;

            /* raise first-time alarm (kept until cleared) */
            if (system_status != WATERING_STATUS_NO_FLOW) {
                bt_irrigation_alarm_notify(ALARM_NO_FLOW, flow_error_attempts);
            }

            /* NEW: re-queue current task for a retry --------------------- */
            if (flow_error_attempts < MAX_FLOW_ERROR_ATTEMPTS &&
                watering_task_state.current_active_task) {

                watering_task_t retry_task =
                        *watering_task_state.current_active_task;

                watering_stop_current_task();

                /* push the task back into the queue ---------------------- */
                watering_error_t qret = watering_add_task(&retry_task);

                system_status = WATERING_STATUS_NO_FLOW;
                bt_irrigation_alarm_notify(ALARM_NO_FLOW, flow_error_attempts); /* NEW */
                ble_status_update();

                /* --- escalate to FAULT if queue full or any error ------- */
                if (qret != WATERING_SUCCESS) {
                    printk("Retry enqueue failed (%d) -> entering FAULT\n", qret);
                    system_status = WATERING_STATUS_FAULT;
                    bt_irrigation_alarm_notify(ALARM_NO_FLOW, flow_error_attempts); /* NEW */
                    ble_status_update();
                    transition_to_state(WATERING_STATE_ERROR_RECOVERY);
                }

            } else {
                /* exceeded attempts – go to FAULT ----------------------- */
                printk("CRITICAL ERROR: Maximum attempts reached. "
                       "Entering fault state!\n");
                system_status = WATERING_STATUS_FAULT;
                bt_irrigation_alarm_notify(ALARM_NO_FLOW, flow_error_attempts);     /* NEW */
                ble_status_update();                  /* NEW */
                transition_to_state(WATERING_STATE_ERROR_RECOVERY);
                watering_stop_current_task();
            }
        } else if (pulses > 0) {
            // Reset error counter if flow is detected
            flow_error_attempts = 0;
            if (system_status == WATERING_STATUS_NO_FLOW) {
                system_status = WATERING_STATUS_OK;
                bt_irrigation_alarm_notify(ALARM_NO_FLOW, 0);  /* clear alarm */
                ble_status_update();                  /* NEW */
                printk("Water flow detected, normal operation\n");
            }
        }
    } else {
        // Check for unexpected flow when no valves are open
        if (pulses > UNEXPECTED_FLOW_THRESHOLD) {
            printk("ALERT: Water flow detected with all valves closed! (%d pulses)\n", pulses);
            system_status = WATERING_STATUS_UNEXPECTED_FLOW;
            bt_irrigation_alarm_notify(ALARM_UNEXPECTED_FLOW, pulses);   /* NEW */
            ble_status_update();                      /* NEW */
            reset_pulse_count();
            
            // Attempt to recover by making sure all valves are closed
            for (int i = 0; i < WATERING_CHANNELS_COUNT; i++) {
                if (watering_channels[i].is_active) {
                    printk("Forcing channel %d valve to close\n", i + 1);
                    watering_channel_off(i);
                }
            }
        } else if (system_status == WATERING_STATUS_UNEXPECTED_FLOW) {
            // If flow has stopped, return to normal status
            if (pulses < UNEXPECTED_FLOW_THRESHOLD / 2) {
                system_status = WATERING_STATUS_OK;
                bt_irrigation_alarm_notify(ALARM_UNEXPECTED_FLOW, 0);    /* clear alarm */
                ble_status_update();                  /* NEW */
                printk("Unexpected flow resolved, normal operation\n");
            }
        }
    }
    
    /* Rain sensor status monitoring */
    static uint32_t last_rain_check = 0;
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
            /* Check rain integration system health */
            if (rain_integration_is_enabled()) {
                /* Perform periodic maintenance on rain history system */
                watering_error_t rain_maintenance_result = rain_history_maintenance();
                if (rain_maintenance_result != WATERING_SUCCESS) {
                    printk("WARNING: Rain history maintenance failed: %d\n", rain_maintenance_result);
                }
            }
            
            /* Run comprehensive rain sensor diagnostics */
            rain_sensor_periodic_diagnostics();
            
            /* Run rain integration health check */
            rain_integration_periodic_health_check();
            
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

    /* DEBUG: Disable flow monitor thread to isolate lockups during BLE init */
    printk("Flow monitoring task DISABLED for debug\n");
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
    /* Clear all errors unconditionally when explicitly requested */
    system_status = WATERING_STATUS_OK;
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
    
    /* Check rain integration configuration */
    rain_integration_config_t config;
    ret = rain_integration_get_config(&config);
    if (ret != WATERING_SUCCESS) {
        printk("Rain integration config check failed: %d - resetting to defaults\n", ret);
        rain_integration_reset_config();
    }
    
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
    
    /* Rain integration status */
    if (rain_integration_is_enabled()) {
    float sensitivity = rain_integration_get_sensitivity();
    float threshold = rain_integration_get_skip_threshold();
        written += snprintf(status_buffer + written, buffer_size - written,
               "Rain integration: Enabled (%.1f%% sensitivity, %.1fmm threshold)\n",
               (double)sensitivity, (double)threshold);
    } else {
        written += snprintf(status_buffer + written, buffer_size - written,
                           "Rain integration: Disabled\n");
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
