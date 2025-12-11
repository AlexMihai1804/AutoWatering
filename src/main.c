#include <zephyr/kernel.h>
#include <zephyr/sys/reboot.h>
#include <hal/nrf_power.h>
#include <hal/nrf_wdt.h>
#include <zephyr/sys/printk.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/devicetree.h>
#include <zephyr/pm/pm.h>
#include <errno.h>

// Core system includes
#include "flow_sensor.h"
#include "rain_sensor.h"
#include "rain_history.h"
#include "rain_integration.h"
#include "watering.h"
#include "watering_internal.h"
#include "rtc.h"
#include "timezone.h"

// Enhanced features includes
#include "environmental_data.h"
#include "environmental_history.h"
#include "bme280_driver.h"
#include "sensor_manager.h"
#include "custom_soil_db.h"
#include "fao56_custom_soil.h"
#include "rain_compensation.h"
#include "temperature_compensation.h"
#include "temperature_compensation_integration.h"
#include "interval_timing.h"
#include "interval_mode_controller.h"
#include "interval_task_integration.h"
#include "configuration_status.h"
#include "enhanced_system_status.h"
#include "enhanced_error_handling.h"
#include "nvs_storage_monitor.h"
#include "onboarding_state.h"
#include "reset_controller.h"
#include "database_flash.h"
#include "history_flash.h"

#ifdef CONFIG_BT
#include "bt_irrigation_service.h"
#endif
#include "usb_descriptors.h"
#include "nvs_config.h"
#include "watering_history.h"
bool critical_section_active = false;
#define INIT_TIMEOUT_MS 5000
#define STATUS_CHECK_INTERVAL_S 30
#define CONFIG_SAVE_INTERVAL_S 3600
#define USB_GLOBAL_TIMEOUT_MS 10000
#define USB_MAX_RETRIES 3
#define USB_RETRY_DELAY_MS 1000
#define ENABLE_USB true
static bool usb_functional = false;
K_THREAD_STACK_DEFINE(init_thread_stack, 2048);
static struct k_thread init_thread_data;
K_SEM_DEFINE(init_complete_sem, 0, 1);
static volatile bool init_success = false;
static const struct device *cdc_dev;

static int setup_usb_cdc_acm(void);

static uint32_t boot_start_ms;

static int set_default_rtc_time(void);

/* WDT feed helper when bootloader watchdog is running */
static void wdt_feed_timer_handler(struct k_timer *timer)
{
    ARG_UNUSED(timer);

    if ((NRF_WDT->RUNSTATUS & WDT_RUNSTATUS_RUNSTATUS_Msk) == 0) {
        return;
    }

    uint32_t mask = NRF_WDT->RREN;
    for (int i = 0; i < 8; i++) {
        if (mask & (1u << i)) {
            NRF_WDT->RR[i] = NRF_WDT_RR_VALUE;
        }
    }
}

static K_TIMER_DEFINE(wdt_feed_timer, wdt_feed_timer_handler, NULL);

// Memory diagnostic function
static void print_memory_stats(void) {
    // Basic memory reporting using uptime and free heap (if available)
    uint32_t uptime = k_uptime_get_32();
    printk("=== Memory Statistics ===\n");
    printk("System uptime: %u ms\n", uptime);
    printk("========================\n");
}

static void print_stack_info(void) {
#ifdef CONFIG_THREAD_STACK_INFO
    struct k_thread *current = k_current_get();
    printk("Current thread: %s\n", k_thread_name_get(current));
    printk("Stack monitoring enabled\n");
#else
    printk("Stack monitoring not enabled\n");
#endif
}

static int setup_usb_cdc_acm(void) {
    if (!ENABLE_USB) {
        return -ENODEV;
    }

    int ret = usb_enable(NULL);
    if (ret != 0 && ret != -EALREADY) {
        printk("Failed to enable USB: %d\n", ret);
        return ret;
    }

    cdc_dev = DEVICE_DT_GET_OR_NULL(DT_CHOSEN(zephyr_console));
    if (!cdc_dev) {
        cdc_dev = DEVICE_DT_GET_OR_NULL(DT_NODELABEL(cdc_acm_uart0));
    }
    if (!cdc_dev || !device_is_ready(cdc_dev)) {
        cdc_dev = device_get_binding("CDC_ACM_0");
    }
    if (!cdc_dev || !device_is_ready(cdc_dev)) {
        printk("CDC ACM device not ready\n");
        return -ENODEV;
    }

    /* Fast-boot: do a single non-blocking DTR check instead of waiting for the host */
    uint32_t dtr = 0;
    (void)uart_line_ctrl_get(cdc_dev, UART_LINE_CTRL_DTR, &dtr);

    if (dtr == 0U) {
        printk("USB host not asserting DTR yet - skipping wait to speed up boot\n");
    } else {
        uart_line_ctrl_set(cdc_dev, UART_LINE_CTRL_DCD, 1);
        uart_line_ctrl_set(cdc_dev, UART_LINE_CTRL_DSR, 1);
    }

    printk("CDC ACM ready\n");
    usb_functional = true;
    return 0;
}

__attribute__((unused))
static void setup_usb(void) {
    printk("Initializing USB with minimal CDC ACM...\n");
    printk("USB disabled\n");
    k_sleep(K_MSEC(500));
    int ret = usb_enable(NULL);
    if (ret != 0) {
        printk("Failed to enable USB: %d\n", ret);
        return;
    }
    printk("USB enabled successfully\n");
    k_sleep(K_MSEC(1000));
    const struct device *cdc_dev = DEVICE_DT_GET_OR_NULL(DT_NODELABEL(cdc_acm_uart0));
    if (!cdc_dev) {
        cdc_dev = device_get_binding("CDC_ACM_0");
    }
    if (!cdc_dev) {
        cdc_dev = device_get_binding("CDC_ACM");
    }
    if (cdc_dev && device_is_ready(cdc_dev)) {
        printk("CDC ACM device found\n"); {
            uint32_t dtr = 0;
            while (uart_line_ctrl_get(cdc_dev, UART_LINE_CTRL_DTR, &dtr) == 0 && !dtr) {
                k_sleep(K_MSEC(10));
            }
        }
        uart_line_ctrl_set(cdc_dev, UART_LINE_CTRL_DCD, 1);
        uart_line_ctrl_set(cdc_dev, UART_LINE_CTRL_DSR, 1);
        const char *test_str = "\r\nSystem booting...\r\n";
        for (int i = 0; i < strlen(test_str); i++) {
            uart_poll_out(cdc_dev, test_str[i]);
        }
        printk("CDC ACM initialized - COM port should be available\n");
        usb_functional = true;
    } else {
        printk("CDC ACM device not found\n");
        usb_functional = false;
    }
}

static void init_thread_entry(void *p1, void *p2, void *p3) {
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);
    printk("Starting safe initialization process\n");
    printk("Initializing flow sensor...\n");
    flow_sensor_init();
    printk("Flow sensor initialized\n");
    printk("Safe initialization complete\n");
    init_success = true;
    k_sem_give(&init_complete_sem);
}

__attribute__((unused))
static watering_error_t initialize_hardware(void) {
    printk("Performing hardware diagnostics before initialization...\n");
    k_tid_t init_tid = k_thread_create(&init_thread_data,
                                       init_thread_stack,
                                       K_THREAD_STACK_SIZEOF(init_thread_stack),
                                       init_thread_entry,
                                       NULL, NULL, NULL,
                                       K_PRIO_PREEMPT(8), 0, K_NO_WAIT);
    k_thread_name_set(init_tid, "init_thread");
    if (k_sem_take(&init_complete_sem, K_MSEC(INIT_TIMEOUT_MS)) != 0) {
        printk("CRITICAL: Initialization thread timed out!\n");
        k_thread_abort(init_tid);
        return WATERING_ERROR_BUSY;
    }

    return init_success ? WATERING_SUCCESS : WATERING_ERROR_CONFIG;
}

static int set_default_rtc_time(void) {
    if (!rtc_is_available()) {
        printk("DS3231 RTC completely disabled to prevent system hangs\n");
        printk("ERROR: Failed to initialize RTC. Will use system time instead.\n");
        return 0;
    }
    rtc_datetime_t now;
    int ret;
    ret = rtc_datetime_get(&now);
    if (ret != 0) {
        printk("ERROR: Failed to read RTC. Using system time instead.\n");
        return ret;
    }
    bool is_default = (now.year <= 2000);
    printk("Current RTC values: %04d-%02d-%02d %02d:%02d:%02d (day %d)\n",
           now.year, now.month, now.day,
           now.hour, now.minute, now.second,
           now.day_of_week);
    if (is_default) {
        printk("RTC has default date, setting to 2023-12-10 12:00:00\n");
        rtc_datetime_t default_time = {
            .year = 2023,
            .month = 12,
            .day = 10,
            .hour = 12,
            .minute = 0,
            .second = 0,
            .day_of_week = 0
        };
        ret = rtc_datetime_set(&default_time);
        if (ret != 0) {
            printk("Failed to set default RTC time: %d\n", ret);
            return ret;
        }
        k_msleep(50);
        rtc_print_time();
    }
    return 0;
}

__attribute__((unused))
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
        k_sleep(K_SECONDS(1));
        err = watering_channel_off(i);
        if (err != WATERING_SUCCESS) {
            printk("Error deactivating channel %d: %d\n", i + 1, err);
        }
        k_sleep(K_MSEC(200));
    }
    return WATERING_SUCCESS;
}

__attribute__((unused))
static watering_error_t create_demo_task(void) {
    printk("Demo tasks disabled for debugging\n");
    return WATERING_SUCCESS;
}

__attribute__((unused))
static int initialize_component(const char *name, int (*init_func)(void)) {
    printk("Initializing %s...\n", name);
    int ret = init_func();
    if (ret != 0) {
        printk("ERROR: %s initialization failed: %d\n", name, ret);
        return ret;
    }
    printk("%s initialized successfully\n", name);
    return 0;
}

static int watering_init_wrapper(void) {
    watering_error_t err = watering_init();
    return (err == WATERING_SUCCESS) ? 0 : -1;
}

static int valve_init_wrapper(void) {
    watering_error_t err = valve_init();
    return (err == WATERING_SUCCESS) ? 0 : -1;
}

static int flow_sensor_init_wrapper(void) {
    int ret = flow_sensor_init();
    if (ret != 0) {
        printk("Flow sensor hardware init failed with error %d\n", ret);
        return ret;
    }
    return 0;
}

__attribute__((unused))
static int nvs_init_wrapper(void) {
    return nvs_config_init();
}

int main(void) {
    boot_start_ms = k_uptime_get_32();
    uint32_t resetreas = NRF_POWER->RESETREAS;
    NRF_POWER->RESETREAS = resetreas; /* clear so next boot is clean */

    printk("\n\n==============================\n");
    printk("AutoWatering System v2.4\n");
    printk("SERIAL PORT FIX BUILD\n");
    printk("==============================\n\n");
    printk("Reset reason bits: 0x%08lx\n", (unsigned long)resetreas);
    critical_section_active = true;
    printk("Starting USB init with port release safeguards...\n");
    int usb_ret = setup_usb_cdc_acm();
    if (usb_ret != 0) {
        printk("WARNING: USB init failed (%d), continuing without USB console\n", usb_ret);
    } else {
    printk("USB init complete\n");
    }
    
    int ret = nvs_config_init();
    if (ret != 0) {
        printk("FATAL: NVS initialization failed (%d), halting application\n", ret);
        k_sleep(K_FOREVER);
    }
    printk("NVS initialization successful\n");
    
    // Initialize onboarding state EARLY - must be before any nvs_save_* calls
    // that update onboarding flags (flow, timezone, rain sensor, etc.)
    printk("Initializing onboarding state system (early)...\n");
    ret = onboarding_state_init();
    if (ret != 0) {
        printk("Warning: Onboarding state system initialization failed: %d\n", ret);
    }
    
    k_sleep(K_MSEC(200));
    printk("Starting valve subsystem init...\n");
    ret = valve_init_wrapper();
    if (ret != 0) {
        printk("WARNING: Valve initialization encountered errors: %d\n", ret);
    } else {
        printk("Valve initialization successful\n");
    }
    k_sleep(K_MSEC(200));
    printk("Starting flow sensor init...\n");
    ret = flow_sensor_init_wrapper();
    if (ret != 0) {
        printk("Flow sensor initialization failed: %d - continuing\n", ret);
    } else {
        printk("Flow sensor initialization successful\n");
    }
    
    // Initialize rain sensor
    printk("Starting rain sensor init...\n");
    ret = rain_sensor_init();
    if (ret != 0) {
        printk("Rain sensor initialization failed: %d - continuing without rain data\n", ret);
    } else {
        printk("Rain sensor initialization successful\n");
    }
    
    ret = initialize_component("RTC", rtc_init);
    if (ret != 0) {
        printk("WARNING: RTC init failed (%d) - using uptime fallback\n", ret);
    } else {
        set_default_rtc_time();
    }
    
    // Initialize timezone helpers (persisted timezone/DST)
    ret = timezone_init();
    if (ret != 0) {
        printk("WARNING: Timezone init failed (%d)\n", ret);
    } else {
        printk("Timezone helpers ready (RTC+timezone config loaded)\n");
    }
    printk("Starting watering subsystem init...\n");
    ret = watering_init_wrapper();
    if (ret != 0) {
        printk("WARNING: Watering system initialization failed: %d\n", ret);
    } else {
        printk("Watering system initialization successful\n");
    }
    k_sleep(K_MSEC(200));
    printk("Starting watering tasks...\n");
    ret = watering_start_tasks();
    if (ret != WATERING_SUCCESS) {
        printk("ERROR: Failed to start watering tasks: %d\n", ret);
    } else {
        printk("Watering tasks started successfully\n");
    }
    critical_section_active = false;
    printk("System initialization complete\n");
    uint32_t boot_time_ms = k_uptime_get_32() - boot_start_ms;
    printk("Boot completed in %u ms\n", boot_time_ms);

    // Print memory statistics after initialization
    print_memory_stats();
    print_stack_info();

    // Initialize enhanced storage system
    printk("Initializing NVS storage monitor...\n");
    ret = nvs_storage_monitor_init();
    if (ret != 0) {
        printk("Warning: NVS storage monitor initialization failed: %d\n", ret);
    } else {
        printk("NVS storage monitor initialized successfully\n");
    }

    // Initialize configuration management system
    printk("Initializing configuration status system...\n");
    watering_error_t config_err = configuration_status_init();
    if (config_err != WATERING_SUCCESS) {
        printk("Warning: Configuration status system initialization failed: %d\n", config_err);
    }
    
    // NOTE: onboarding_state_init() is called early, right after nvs_config_init()
    // to ensure flag updates during boot are not lost
    
    // Initialize reset controller
    printk("Initializing reset controller...\n");
    ret = reset_controller_init();
    if (ret != 0) {
        printk("Warning: Reset controller initialization failed: %d\n", ret);
    } else {
        printk("Configuration status system initialized successfully\n");
    }

    // Initialize enhanced system status
    printk("Initializing enhanced system status...\n");
    ret = enhanced_system_status_init();
    if (ret != 0) {
        printk("Warning: Enhanced system status initialization failed: %d\n", ret);
    } else {
        printk("Enhanced system status initialized successfully\n");
    }

    // Initialize enhanced error handling
    printk("Initializing enhanced error handling...\n");
    ret = enhanced_error_handling_init();
    if (ret != 0) {
        printk("Warning: Enhanced error handling initialization failed: %d\n", ret);
    } else {
        printk("Enhanced error handling initialized successfully\n");
    }

    // Initialize environmental sensor system
    printk("Initializing sensor manager...\n");
    sensor_manager_config_t sensor_config = {
        .auto_recovery_enabled = true,
        .recovery_timeout_ms = 5000,
        .max_recovery_attempts = 3,
        .health_check_interval_ms = 30000,
        .reading_timeout_ms = 2000
    };
    ret = sensor_manager_init(&sensor_config);
    if (ret != 0) {
        printk("Warning: Sensor manager initialization failed: %d\n", ret);
    } else {
        printk("Sensor manager initialized successfully\n");
        
        // Try to initialize BME280 sensor
        const struct device *i2c_dev = DEVICE_DT_GET_OR_NULL(DT_NODELABEL(i2c0));
        if (i2c_dev && device_is_ready(i2c_dev)) {
            printk("Initializing BME280 environmental sensor...\n");
            /* Note: Address 0x76 is common for generic modules. 0x77 is for Adafruit. */
            /* The driver uses the address from Device Tree, this param is just for logging/compat */
            ret = sensor_manager_init_bme280(i2c_dev, 0x76);
            if (ret != 0) {
                printk("Warning: BME280 initialization failed: %d\n", ret);
            } else {
                printk("BME280 sensor initialized successfully\n");
            }
        } else {
            printk("Warning: I2C device not ready, skipping BME280 initialization\n");
        }
    }

    // Initialize environmental data system
    printk("Initializing environmental data system...\n");
    ret = environmental_data_init();
    if (ret != 0) {
        printk("Warning: Environmental data system initialization failed: %d\n", ret);
    } else {
        printk("Environmental data system initialized successfully\n");
    }

    // Initialize external flash database (LittleFS)
    printk("Initializing external flash database...\n");
    ret = db_flash_init();
    if (ret != 0) {
        printk("Warning: External flash database initialization failed: %d\n", ret);
    } else {
        printk("External flash database initialized successfully\n");
    }

    // Initialize history flash storage
    printk("Initializing history flash storage...\n");
    ret = history_flash_init();
    if (ret != 0) {
        printk("Warning: History flash storage initialization failed: %d\n", ret);
    } else {
        printk("History flash storage initialized successfully\n");
    }

    // Initialize environmental history
    printk("Initializing environmental history system...\n");
    ret = environmental_history_init();
    if (ret != 0) {
        printk("Warning: Environmental history system initialization failed: %d\n", ret);
    } else {
        printk("Environmental history system initialized successfully\n");
    }

    // Initialize custom soil database
    printk("Initializing custom soil database...\n");
    watering_error_t soil_err = custom_soil_db_init();
    if (soil_err != WATERING_SUCCESS) {
        printk("Warning: Custom soil database initialization failed: %d\n", soil_err);
    } else {
        printk("Custom soil database initialized successfully\n");
    }

    // Initialize compensation systems
    printk("Initializing rain compensation system...\n");
    watering_error_t rain_comp_err = rain_compensation_init();
    if (rain_comp_err != WATERING_SUCCESS) {
        printk("Warning: Rain compensation system initialization failed: %d\n", rain_comp_err);
    } else {
        printk("Rain compensation system initialized successfully\n");
    }

    printk("Initializing temperature compensation system...\n");
    watering_error_t temp_comp_err = temperature_compensation_init();
    if (temp_comp_err != WATERING_SUCCESS) {
        printk("Warning: Temperature compensation system initialization failed: %d\n", temp_comp_err);
    } else {
        printk("Temperature compensation system initialized successfully\n");
    }

    printk("Initializing temperature compensation integration...\n");
    ret = temperature_compensation_integration_init();
    if (ret != 0) {
        printk("Warning: Temperature compensation integration initialization failed: %d\n", ret);
    } else {
        printk("Temperature compensation integration initialized successfully\n");
    }

    // Initialize interval mode system
    printk("Initializing interval task integration...\n");
    ret = interval_task_integration_init();
    if (ret != 0) {
        printk("Warning: Interval task integration initialization failed: %d\n", ret);
    } else {
        printk("Interval task integration initialized successfully\n");
    }

    // Initialize history systems
    printk("Initializing watering history system...\n");
    watering_error_t hist_err = watering_history_init();
    if (hist_err != WATERING_SUCCESS) {
        printk("Warning: History system initialization failed: %d\n", hist_err);
    } else {
        printk("History system initialized successfully\n");
    }
    
    // Initialize rain history system
    printk("Initializing rain history system...\n");
    watering_error_t rain_hist_err = rain_history_init();
    if (rain_hist_err != WATERING_SUCCESS) {
        printk("Warning: Rain history system initialization failed: %d\n", rain_hist_err);
    } else {
        printk("Rain history system initialized successfully\n");
    }
    
    // Initialize rain integration system
    printk("Initializing rain integration system...\n");
    watering_error_t rain_int_err = rain_integration_init();
    if (rain_int_err != WATERING_SUCCESS) {
        printk("Warning: Rain integration system initialization failed: %d\n", rain_int_err);
    } else {
        printk("Rain integration system initialized successfully\n");
    }

#ifdef CONFIG_BT
    printk("Initializing Bluetooth irrigation service...\n");
    int ble_err = bt_irrigation_service_init();
    if (ble_err != 0) {
        printk("Error initializing BLE service: %d\n", ble_err);
    }
#endif
    
    // Detect if a hardware watchdog was started by the bootloader; feed it to prevent unexpected resets
    bool bootloader_wdt_active = (NRF_WDT->RUNSTATUS & WDT_RUNSTATUS_RUNSTATUS_Msk) != 0;
    if (bootloader_wdt_active) {
        printk("Bootloader watchdog detected - will feed via 100ms timer\n");
        k_timer_start(&wdt_feed_timer, K_MSEC(100), K_MSEC(100));
    }

    // Main loop with periodic memory monitoring and WDT feed (if bootloader started it)
    uint32_t loop_ticks = 0;
    while (1) {
        k_sleep(K_SECONDS(1));
        loop_ticks++;
        
        // Print memory stats every 10 minutes (600 seconds)
        if (loop_ticks % 600 == 0) {
            printk("=== Runtime Status (uptime: %u min) ===\n", loop_ticks / 60);
            print_memory_stats();
            print_stack_info();
        }
    }
    return 0;
}

/* Global fatal handler to log fault cause and reboot instead of silent hang */
void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf)
{
    printk("FATAL: reason=%u\n", reason);
#ifdef CONFIG_ARM
    if (esf) {
        printk("  PC=0x%08lx LR=0x%08lx\n",
               (unsigned long)esf->basic.pc,
               (unsigned long)esf->basic.lr);
    } else {
        printk("  no ESF available\n");
    }
#endif
    /* Give UART time to flush before reboot */
    k_sleep(K_MSEC(500));
    sys_reboot(SYS_REBOOT_COLD);
}
