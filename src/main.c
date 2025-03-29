#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/drivers/watchdog.h>

#include "flow_sensor.h"
#include "watering.h"
#include "watering_internal.h"  // Add this include to access DEFAULT_PULSES_PER_LITER
#include "rtc.h"

/**
 * @file main.c
 * @brief Main application for the automatic watering system
 * 
 * This file contains the entry point for the application and handles
 * system initialization, demo setup, and the main monitoring loop.
 */

// Timeout for initialization in milliseconds
#define INIT_TIMEOUT_MS 5000

// System status check interval in seconds
#define STATUS_CHECK_INTERVAL_S 60

// Configuration save interval in seconds
#define CONFIG_SAVE_INTERVAL_S 3600

// Watchdog configuration
#define WDT_TIMEOUT_MS 5000

#if DT_HAS_CHOSEN(zephyr_watchdog) || DT_HAS_ALIAS(watchdog0)
static const struct device *wdt_dev;
static struct wdt_timeout_cfg wdt_config;

/**
 * @brief Configure and start watchdog timer
 * 
 * @return 0 on success, negative error code on failure
 */
static int setup_watchdog(void) {
    // Use runtime binding instead of devicetree references
    wdt_dev = device_get_binding("WDT_0");
    
    if (!wdt_dev || !device_is_ready(wdt_dev)) {
        printk("Watchdog device not ready or not available\n");
        return -ENODEV;
    }
    
    wdt_config.window.min = 0;
    wdt_config.window.max = WDT_TIMEOUT_MS;
    wdt_config.callback = NULL;  // Reset on timeout
    
    int wdt_channel_id = wdt_install_timeout(wdt_dev, &wdt_config);
    if (wdt_channel_id < 0) {
        printk("Watchdog install error: %d\n", wdt_channel_id);
        return wdt_channel_id;
    }
    
    int ret = wdt_setup(wdt_dev, WDT_OPT_PAUSE_HALTED_BY_DBG);
    if (ret < 0) {
        printk("Watchdog setup error: %d\n", ret);
        return ret;
    }
    
    printk("Watchdog initialized with %d ms timeout\n", WDT_TIMEOUT_MS);
    return 0;
}
#else
// Stub function when no watchdog is available
static inline int setup_watchdog(void) {
    printk("No watchdog device available in device tree\n");
    return 0;
}
#endif

/**
 * @brief Initialize system hardware components
 * 
 * @return WATERING_SUCCESS on success, error code on failure
 */
static watering_error_t initialize_hardware(void) {
    watering_error_t err;
    int retry_count = 0;
    const int max_retries = 3;
    
    // Retry initialization loop for better resilience
    while (retry_count < max_retries) {
        // Initialize flow sensor hardware
        flow_sensor_init();
        
        // Initialize watering system
        err = watering_init();
        if (err == WATERING_SUCCESS) {
            break;
        }
        
        printk("Failed to initialize watering system: %d (attempt %d/%d)\n", 
               err, retry_count + 1, max_retries);
        k_sleep(K_MSEC(500));
        retry_count++;
    }
    
    if (err != WATERING_SUCCESS) {
        printk("Failed to initialize watering system after %d attempts\n", max_retries);
        return err;
    }
    
    // Try to initialize RTC but don't rely on it
    printk("Attempting to initialize RTC...\n");
    
    // Initialize RTC access - don't error out if RTC isn't available
    int rtc_status = rtc_init();
    if (rtc_status != 0) {
        printk("RTC initialization failed - scheduling will use system time only\n");
    } else {
        // Only access RTC functions if initialization was successful
        rtc_datetime_t now;
        if (rtc_datetime_get(&now) == 0) {
            printk("RTC time: %02d:%02d:%02d, Date: %02d/%02d/%04d (day %d)\n",
                now.hour, now.minute, now.second,
                now.day, now.month, now.year,
                now.day_of_week);
        } else {
            printk("Failed to read current time from RTC\n");
        }
    }
    
    return WATERING_SUCCESS;
}

/**
 * @brief Load or create system configuration
 * 
 * @return WATERING_SUCCESS on success, error code on failure
 */
static watering_error_t initialize_configuration(void) {
    watering_error_t err;
    uint32_t calibration;
    
    // Load existing configuration or create new one
    err = watering_load_config();
    if (err != WATERING_SUCCESS) {
        printk("No saved configurations found, using default values\n");
        watering_set_flow_calibration(DEFAULT_PULSES_PER_LITER);
        err = watering_save_config();
        if (err != WATERING_SUCCESS) {
            printk("Warning: Failed to save default configuration: %d\n", err);
        }
    }
    
    // Display current calibration
    if (watering_get_flow_calibration(&calibration) == WATERING_SUCCESS) {
        printk("Flow sensor calibrated to %d pulses per liter\n", calibration);
    }
    
    return WATERING_SUCCESS;
}

/**
 * @brief Run a test cycle of all valves
 * 
 * @return WATERING_SUCCESS on success, error code on failure
 */
static watering_error_t run_valve_test(void) {
    printk("Running valve test sequence...\n");
    watering_error_t err;
    
    for (int i = 0; i < WATERING_CHANNELS_COUNT; i++) {
        printk("Testing channel %d...\n", i + 1);
        err = watering_channel_on(i);
        if (err != WATERING_SUCCESS) {
            printk("Error activating channel %d: %d\n", i + 1, err);
            continue;
        }
        
        k_sleep(K_SECONDS(2));
        
        err = watering_channel_off(i);
        if (err != WATERING_SUCCESS) {
            printk("Error deactivating channel %d: %d\n", i + 1, err);
        }
        
        k_sleep(K_MSEC(500));
    }
    
    return WATERING_SUCCESS;
}

/**
 * @brief Create a demonstration task for testing
 * 
 * @return WATERING_SUCCESS on success, error code on failure
 */
static watering_error_t create_demo_task(void) {
    watering_task_t test_task;
    watering_channel_t *channel;
    watering_error_t err;
    
    // Get first channel
    err = watering_get_channel(0, &channel);
    if (err != WATERING_SUCCESS) {
        printk("Error getting channel: %d\n", err);
        return err;
    }
    
    // Configure demo task
    test_task.channel = channel;
    test_task.channel->watering_event.watering_mode = WATERING_BY_VOLUME;
    test_task.channel->watering_event.watering.by_volume.volume_liters = 2;
    test_task.by_volume.volume_liters = 2;
    
    // Add task to queue
    err = watering_add_task(&test_task);
    if (err == WATERING_SUCCESS) {
        printk("Demonstration task added for channel 1 (2 liters)\n");
    } else {
        printk("Error adding demonstration task: %d\n", err);
    }
    
    return err;
}

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
    
    // Setup watchdog timer
    setup_watchdog();
    
    // Initialize hardware components
    if (initialize_hardware() != WATERING_SUCCESS) {
        printk("Critical initialization error - attempting recovery\n");
        k_sleep(K_SECONDS(1));
        sys_reboot(SYS_REBOOT_WARM);
    }
    
    // Load or create configuration
    initialize_configuration();
    
    // Run a test sequence to verify all valves are working
    run_valve_test();
    
    // Create a demonstration task for channel 1
    create_demo_task();
    
    // Start background tasks for watering operations
    watering_error_t err = watering_start_tasks();
    if (err != WATERING_SUCCESS) {
        printk("Error starting watering tasks: %d\n", err);
        return -1;
    }
    
    printk("Watering system now running in dedicated tasks\n");
    printk("Main application entering monitoring loop\n");
    
    // Configuration save timer
    uint32_t last_save_time = k_uptime_get_32();
    
    // Power monitoring variables
    uint32_t power_check_time = 0;
    power_mode_t current_mode = POWER_MODE_NORMAL;
    
    // Main monitoring loop
    while (1) {
        watering_status_t status;
        watering_state_t state;
        
        if (watering_get_status(&status) != WATERING_SUCCESS || 
            watering_get_state(&state) != WATERING_SUCCESS) {
            printk("Error reading system state\n");
        } else {
            // Handle fault state by periodically trying to reset
            if (status == WATERING_STATUS_FAULT) {
                printk("CRITICAL ERROR: System blocked! Manual intervention required.\n");
                printk("Waiting for problem resolution and manual reset...\n");
                k_sleep(K_SECONDS(300));  // Wait 5 minutes before trying reset
                watering_reset_fault();
            }
            
            // Log current system state
            printk("Main thread: system status: %d, state: %d\n", status, state);
        }
        
        // Periodically save configuration
        uint32_t now = k_uptime_get_32();
        if ((now - last_save_time) > (CONFIG_SAVE_INTERVAL_S * 1000)) {
            watering_save_config();
            last_save_time = now;
        }
        
        // Check power state and update mode if needed (demo)
        // In a real system, this would monitor battery voltage or external power state
        if ((now - power_check_time) > (300 * 1000)) { // Every 5 minutes
            power_check_time = now;
            
            // Demo power mode rotation (would be based on real power readings)
            if (current_mode == POWER_MODE_NORMAL) {
                watering_set_power_mode(POWER_MODE_ENERGY_SAVING);
                current_mode = POWER_MODE_ENERGY_SAVING;
                printk("Switched to energy saving mode (demo)\n");
            } else if (current_mode == POWER_MODE_ENERGY_SAVING) {
                watering_set_power_mode(POWER_MODE_NORMAL);
                current_mode = POWER_MODE_NORMAL;
                printk("Switched to normal power mode (demo)\n");
            }
        }
        
        // Feed watchdog to prevent reset
        #if DT_HAS_CHOSEN(zephyr_watchdog) || DT_HAS_ALIAS(watchdog0)
        if (wdt_dev && device_is_ready(wdt_dev)) {
            wdt_feed(wdt_dev, 0);
        }
        #endif
        
        // Sleep before next check
        k_sleep(K_SECONDS(STATUS_CHECK_INTERVAL_S));
    }
    
    // This point is never reached, but for completeness
    return 0;
}
