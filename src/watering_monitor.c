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

/**
 * @brief Check for flow anomalies and update system status
 * 
 * This function detects two main anomalies:
 * 1. No flow when a valve is open (may indicate empty tank or clogged line)
 * 2. Unexpected flow when all valves are closed (may indicate leak or valve failure)
 */
void check_flow_anomalies(void) {
    uint32_t now = k_uptime_get_32();
    
    // Only check periodically to allow flow to stabilize
    if ((now - last_flow_check_time) < FLOW_CHECK_THRESHOLD_MS) {
        return;
    }
    last_flow_check_time = now;
    
    // Skip checks if system is in fault state
    if (system_status == WATERING_STATUS_FAULT) {
        return;
    }
    
    // Check for no-flow condition when a valve is open
    if (watering_task_state.current_active_task != NULL) {
        uint32_t pulses = get_pulse_count();
        
        // If no pulses after 5 seconds of watering, we may have a problem
        if (pulses == 0 && k_uptime_get_32() - watering_task_state.watering_start_time > 5000) {
            printk("ALERT: No water flow detected with valve open!\n");
            flow_error_attempts++;
            
            // If we've had multiple consecutive errors, enter fault state
            if (flow_error_attempts >= MAX_FLOW_ERROR_ATTEMPTS) {
                printk("CRITICAL ERROR: Maximum attempts reached. Entering fault state!\n");
                system_status = WATERING_STATUS_FAULT;
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
        } else if (system_status == WATERING_STATUS_UNEXPECTED_FLOW) {
            // If flow has stopped, return to normal status
            if (pulses < UNEXPECTED_FLOW_THRESHOLD / 2) {
                system_status = WATERING_STATUS_OK;
                printk("Unexpected flow resolved, normal operation\n");
            }
        }
    }
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
            // Convert pulses to liters for reporting
            float liters = (float) pulses / watering_get_flow_calibration();
            printk("Flow sensor pulses: %u (%.2f liters)\n", pulses, (double) liters);
            
            // Report on any abnormal system status
            if (system_status != WATERING_STATUS_OK) {
                switch (system_status) {
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
                    default:
                        break;
                }
            }
        }
        k_sleep(K_SECONDS(1));
    }
    
    printk("Flow sensor monitoring task stopped\n");
}

/**
 * @brief Initialize the flow monitoring subsystem
 * 
 * Sets up the monitoring thread and resets status variables
 */
void flow_monitor_init(void) {
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
    } else {
        printk("Error starting flow monitoring task\n");
    }
}

/**
 * @brief Reset the system from fault state
 * 
 * @return 0 if successfully reset, negative error if not in fault state
 */
int watering_reset_fault(void) {
    if (system_status == WATERING_STATUS_FAULT) {
        printk("Resetting system from fault state\n");
        system_status = WATERING_STATUS_OK;
        flow_error_attempts = 0;
        return 0;
    }
    return -1;  // Not in fault state
}
