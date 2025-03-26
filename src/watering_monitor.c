#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include "flow_sensor.h"
#include "watering.h"
#include "watering_internal.h"

/* Stack definition for flow monitor task */
#define FLOW_MONITOR_STACK_SIZE 1024
K_THREAD_STACK_DEFINE(flow_monitor_stack, FLOW_MONITOR_STACK_SIZE);

/* Thread structure */
static struct k_thread flow_monitor_data;

/* Task state indicator */
static bool exit_tasks = false;

/* Variables for anomaly detection */
static uint8_t flow_error_attempts = 0;
static uint32_t last_flow_check_time = 0;

/**
 * @brief Check if there are flow anomalies
 */
void check_flow_anomalies(void) {
    uint32_t now = k_uptime_get_32();

    // Check only at regular intervals
    if ((now - last_flow_check_time) < FLOW_CHECK_THRESHOLD_MS) {
        return;
    }

    last_flow_check_time = now;

    // If we're already in fault state, don't check
    if (system_status == WATERING_STATUS_FAULT) {
        return;
    }

    // Check 1: Valve open but no water flow
    if (watering_task_state.current_active_task != NULL) {
        uint32_t pulses = get_pulse_count();

        // If valve is open but we don't detect flow
        if (pulses == 0 &&
            k_uptime_get_32() - watering_task_state.watering_start_time > 5000) { // Allow 5 seconds for flow to start
            printk("ALERT: No water flow detected with valve open!\n");

            // Increment attempts counter
            flow_error_attempts++;

            if (flow_error_attempts >= MAX_FLOW_ERROR_ATTEMPTS) {
                printk("CRITICAL ERROR: Maximum attempts reached. Entering fault state!\n");
                system_status = WATERING_STATUS_FAULT;

                // Stop all valves and cancel current task
                watering_stop_current_task();
            } else {
                printk("Retrying watering (%d/%d)...\n", flow_error_attempts, MAX_FLOW_ERROR_ATTEMPTS);
                system_status = WATERING_STATUS_NO_FLOW;

                // Stop current task and we'll try again
                watering_stop_current_task();
            }
        } else if (pulses > 0) {
            // If we detect flow, reset error counter
            flow_error_attempts = 0;
            if (system_status == WATERING_STATUS_NO_FLOW) {
                system_status = WATERING_STATUS_OK;
                printk("Water flow detected, normal operation\n");
            }
        }
    } else {
        // Check 2: All valves closed but water flowing
        uint32_t pulses = get_pulse_count();
        if (pulses > UNEXPECTED_FLOW_THRESHOLD) {
            printk("ALERT: Water flow detected with all valves closed! (%d pulses)\n", pulses);
            system_status = WATERING_STATUS_UNEXPECTED_FLOW;

            // Reset counter to check if it persists
            reset_pulse_count();
        } else if (system_status == WATERING_STATUS_UNEXPECTED_FLOW) {
            // Check if the problem has been resolved
            if (pulses < UNEXPECTED_FLOW_THRESHOLD / 2) {
                system_status = WATERING_STATUS_OK;
                printk("Unexpected flow resolved, normal operation\n");
            }
        }
    }
}

/**
 * @brief Task for monitoring the flow sensor
 */
static void flow_monitor_fn(void *p1, void *p2, void *p3) {
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    printk("Flow sensor monitoring task started\n");

    while (!exit_tasks) {
        /* Read and display flow sensor pulses */
        uint32_t pulses = get_pulse_count();
        if (pulses > 0) {
            float liters = (float) pulses / watering_get_flow_calibration();
            printk("Flow sensor pulses: %u (%.2f liters)\n", pulses, (double) liters);

            // Display system status
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

        /* Wait one second */
        k_sleep(K_SECONDS(1));
    }

    printk("Flow sensor monitoring task stopped\n");
}

/**
 * @brief Initialize flow monitoring
 */
void flow_monitor_init(void) {
    // Reset anomaly detection variables
    system_status = WATERING_STATUS_OK;
    flow_error_attempts = 0;
    last_flow_check_time = 0;
    exit_tasks = false;

    /* Start flow monitor task */
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
 * @brief Reset system status after fixing errors
 *
 * @return int 0 if reset was successful, otherwise error code
 */
int watering_reset_fault(void) {
    if (system_status == WATERING_STATUS_FAULT) {
        printk("Resetting system from fault state\n");
        system_status = WATERING_STATUS_OK;
        flow_error_attempts = 0;
        return 0;
    }

    return -1; // Not in fault state
}
