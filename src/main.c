#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include "flow_sensor.h"
#include "watering.h"

int main(void) {
    printk("Initializing automatic irrigation system...\n");

    /* Initialize flow sensor */
    flow_sensor_init();

    /* Initialize watering module */
    watering_init();

    /* Load persistent configurations */
    if (watering_load_config() != 0) {
        /* If no saved configurations exist, set default values */
        printk("No saved configurations found, using default values\n");
        watering_set_flow_calibration(750); // 750 pulses per liter

        /* Save default configurations for next boot */
        watering_save_config();
    }

    printk("Flow sensor calibrated to %d pulses per liter\n", watering_get_flow_calibration());

    /* Simple valve test - activate each channel for a few seconds */
    for (int i = 0; i < WATERING_CHANNELS_COUNT; i++) {
        printk("Testing channel %d...\n", i + 1);
        watering_channel_on(i);
        k_sleep(K_SECONDS(2));
        watering_channel_off(i);
        k_sleep(K_MSEC(500));
    }

    /* Create demonstration task */
    watering_task_t test_task;
    test_task.channel = watering_get_channel(0); // First channel

    /* Configure for volumetric watering */
    test_task.channel->watering_event.watering_mode = WATERING_BY_VOLUME;
    test_task.channel->watering_event.watering.by_volume.volume_liters = 2; // 2 liters
    test_task.by_volume.volume_liters = 2;

    /* Add task to queue */
    int result = watering_add_task(&test_task);
    if (result == 0) {
        printk("Demonstration task added for channel 1 (2 liters)\n");
    } else {
        printk("Error adding demonstration task: %d\n", result);
    }

    /* Start dedicated watering tasks */
    if (watering_start_tasks() != 0) {
        printk("Error starting watering tasks!\n");
        return -1;
    }

    printk("Watering system now running in dedicated tasks\n");
    printk("Main application can perform other operations or enter sleep mode\n");

    /* In the main application, monitor system status */
    while (1) {
        watering_status_t status = watering_get_status();

        if (status == WATERING_STATUS_FAULT) {
            printk("CRITICAL ERROR: System blocked! Manual intervention required.\n");
            printk("Waiting for problem resolution and manual reset...\n");

            // We can try automatic reset after an interval
            k_sleep(K_SECONDS(300)); // 5 minutes
            watering_reset_fault();
        }

        /* Periodically save configuration */
        watering_save_config();

        k_sleep(K_SECONDS(60));
        printk("Main thread: system status: %d\n", status);
    }

    return 0; // Will never reach here, but necessary for compliance
}
