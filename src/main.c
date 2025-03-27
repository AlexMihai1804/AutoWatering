#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include "flow_sensor.h"
#include "watering.h"

/**
 * @file main.c
 * @brief Main application for the automatic watering system
 * 
 * This file contains the entry point for the application and handles
 * system initialization, demo setup, and the main monitoring loop.
 */

/**
 * @brief Application entry point
 * 
 * Initializes hardware and software components, runs a test sequence,
 * and starts the watering system.
 * 
 * @return 0 on success, negative error code on failure
 */
int main(void) {
    printk("Initializing automatic irrigation system...\n");
    
    // Initialize hardware components
    flow_sensor_init();
    watering_init();
    
    // Load or create configuration
    if (watering_load_config() != 0) {
        printk("No saved configurations found, using default values\n");
        watering_set_flow_calibration(750); // Default calibration
        watering_save_config();
    }
    printk("Flow sensor calibrated to %d pulses per liter\n", watering_get_flow_calibration());
    
    // Run a test sequence to verify all valves are working
    for (int i = 0; i < WATERING_CHANNELS_COUNT; i++) {
        printk("Testing channel %d...\n", i + 1);
        watering_channel_on(i);
        k_sleep(K_SECONDS(2));
        watering_channel_off(i);
        k_sleep(K_MSEC(500));
    }
    
    // Create a demonstration task for channel 1
    watering_task_t test_task;
    test_task.channel = watering_get_channel(0);
    test_task.channel->watering_event.watering_mode = WATERING_BY_VOLUME;
    test_task.channel->watering_event.watering.by_volume.volume_liters = 2;
    test_task.by_volume.volume_liters = 2;
    
    int result = watering_add_task(&test_task);
    if (result == 0) {
        printk("Demonstration task added for channel 1 (2 liters)\n");
    } else {
        printk("Error adding demonstration task: %d\n", result);
    }
    
    // Start background tasks for watering operations
    if (watering_start_tasks() != 0) {
        printk("Error starting watering tasks!\n");
        return -1;
    }
    
    printk("Watering system now running in dedicated tasks\n");
    printk("Main application can perform other operations or enter sleep mode\n");
    
    // Main monitoring loop
    while (1) {
        watering_status_t status = watering_get_status();
        
        // Handle fault state by periodically trying to reset
        if (status == WATERING_STATUS_FAULT) {
            printk("CRITICAL ERROR: System blocked! Manual intervention required.\n");
            printk("Waiting for problem resolution and manual reset...\n");
            k_sleep(K_SECONDS(300));  // Wait 5 minutes before trying reset
            watering_reset_fault();
        }
        
        // Periodically save configuration
        watering_save_config();
        
        // Sleep for 1 minute before next check
        k_sleep(K_SECONDS(60));
        printk("Main thread: system status: %d\n", status);
    }
    
    return 0;
}
