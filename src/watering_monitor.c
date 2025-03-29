#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include "flow_sensor.h"
#include "watering.h"
#include "watering_internal.h"

/**
 * @file watering_monitor.c
 * @brief Implementation of flow monitoring and anomaly detection
 * 
 * This file implements the flow monitoring system that detects problems
 * with water flow, including no-flow conditions when a valve is open
 * and unexpected flow when all valves are closed.
 */

/** Stack size for flow monitor thread */
#define FLOW_MONITOR_STACK_SIZE 1024

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

/**
 * @brief Check for flow anomalies and update system status
 * 
 * This function detects two main anomalies:
 * 1. No flow when a valve is open (may indicate empty tank or clogged line)
 * 2. Unexpected flow when all valves are closed (may indicate leak or valve failure)
 * 
 * @return WATERING_SUCCESS on success or WATERING_ERROR_BUSY if in fault state
 */
watering_error_t check_flow_anomalies(void) {
    uint32_t now = k_uptime_get_32();
    
    k_mutex_lock(&flow_monitor_mutex, K_NO_WAIT);
    
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
    
    // Check for no-flow condition when a valve is open
    if (watering_task_state.current_active_task != NULL) {
        uint32_t pulses = get_pulse_count();
        
        // If no pulses after 5 seconds of watering, we may have a problem
        if (pulses == 0 && k_uptime_get_32() - watering_task_state.watering_start_time > 5000) {
            // Consider making this timeout configurable or longer for low-pressure systems
            printk("ALERT: No water flow detected with valve open!\n");
            flow_error_attempts++;
            
            // If we've had multiple consecutive errors, enter fault state
            if (flow_error_attempts >= MAX_FLOW_ERROR_ATTEMPTS) {
                printk("CRITICAL ERROR: Maximum attempts reached. Entering fault state!\n");
                system_status = WATERING_STATUS_FAULT;
                transition_to_state(WATERING_STATE_ERROR_RECOVERY);
                watering_stop_current_task();
            } else {
                printk("Retrying watering (%d/%d)...\n", flow_error_attempts, MAX_FLOW_ERROR_ATTEMPTS);
                system_status = WATERING_STATUS_NO_FLOW;
                watering_stop_current_task();
            }
        } else if (pulses > 0) {
            // Reset error counter if flow is detected
            flow_error_attempts = 0;
            if (system_status == WATERING_STATUS_NO_FLOW) {
                system_status = WATERING_STATUS_OK;
                printk("Water flow detected, normal operation\n");
            }
        }
    } else {
        // Check for unexpected flow when no valves are open
        uint32_t pulses = get_pulse_count();
        if (pulses > UNEXPECTED_FLOW_THRESHOLD) {
            printk("ALERT: Water flow detected with all valves closed! (%d pulses)\n", pulses);
            system_status = WATERING_STATUS_UNEXPECTED_FLOW;
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
                printk("Unexpected flow resolved, normal operation\n");
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
    
    while (!exit_tasks) {
        uint32_t pulses = get_pulse_count();
        
        if (pulses > 0) {
            uint32_t calibration;
            if (watering_get_flow_calibration(&calibration) == WATERING_SUCCESS) {
                // Convert pulses to liters for reporting
                float liters = (float) pulses / calibration;
                printk("Flow sensor pulses: %u (%.2f liters)\n", pulses, (double) liters);
            }
            
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
                        printk("WARNING: Unexpected flow detected!\n");
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
        
        // Adjust sleep duration based on power mode
        uint32_t sleep_time = 1000; // Default 1 second
        switch (current_power_mode) {
            case POWER_MODE_ENERGY_SAVING:
                sleep_time = 5000; // 5 seconds in energy saving mode
                break;
            case POWER_MODE_ULTRA_LOW_POWER:
                sleep_time = 30000; // 30 seconds in ultra-low power mode
                break;
            default:
                break;
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
    flow_error_attempts = 0;
    last_flow_check_time = 0;
    exit_tasks = false;
    
    // Create and start the flow monitoring thread
    k_tid_t flow_tid =
            k_thread_create(&flow_monitor_data, flow_monitor_stack, K_THREAD_STACK_SIZEOF(flow_monitor_stack),
                            flow_monitor_fn, NULL, NULL, NULL, K_PRIO_PREEMPT(6), 0, K_NO_WAIT);
    
    if (flow_tid != NULL) {
        k_thread_name_set(flow_tid, "flow_monitor");
        printk("Flow monitoring task started\n");
        k_mutex_unlock(&flow_monitor_mutex);
        return WATERING_SUCCESS;
    } else {
        printk("Error starting flow monitoring task\n");
        k_mutex_unlock(&flow_monitor_mutex);
        return WATERING_ERROR_CONFIG;
    }
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
        flow_error_attempts = 0;
        
        // Try to recover the system state
        attempt_error_recovery(WATERING_SUCCESS);
        
        k_mutex_unlock(&flow_monitor_mutex);
        return WATERING_SUCCESS;
    }
    
    k_mutex_unlock(&flow_monitor_mutex);
    return WATERING_ERROR_INVALID_PARAM;  // Not in fault state
}
