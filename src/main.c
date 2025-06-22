#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/devicetree.h>
#include <zephyr/pm/pm.h>
#include "flow_sensor.h"
#include "watering.h"
#include "watering_internal.h"
#include "rtc.h"
#include "bt_irrigation_service.h"
#include "usb_descriptors.h"
#include "nvs_config.h"
bool critical_section_active = false;
#define INIT_TIMEOUT_MS 5000
#define STATUS_CHECK_INTERVAL_S 30
#define CONFIG_SAVE_INTERVAL_S 3600
#define USB_GLOBAL_TIMEOUT_MS 10000
#define USB_MAX_RETRIES 3
#define USB_RETRY_DELAY_MS 1000
#define ENABLE_BLUETOOTH true
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

static int setup_usb_cdc_acm(void) {
    if (!ENABLE_USB) {
        return -ENODEV;
    }
    cdc_dev = DEVICE_DT_GET_OR_NULL(DT_NODELABEL(cdc_acm_uart0));
    if (!cdc_dev || !device_is_ready(cdc_dev)) {
        cdc_dev = device_get_binding("CDC_ACM_0");
    }
    if (!cdc_dev || !device_is_ready(cdc_dev)) {
        printk("CDC ACM device not ready\n");
        return -ENODEV;
    }
    printk("CDC ACM ready (USB was started at boot)\n");
    usb_functional = true;
    return 0;
}

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
    printk("\n\n==============================\n");
    printk("AutoWatering System v2.4\n");
    printk("SERIAL PORT FIX BUILD\n");
    printk("==============================\n\n");
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
        printk("Flow sensor initialization failed: %d – continuing\n", ret);
    } else {
        printk("Flow sensor initialization successful\n");
    }
    ret = initialize_component("RTC", rtc_init);
    if (ret != 0) {
        printk("WARNING: RTC init failed (%d) – using uptime fallback\n", ret);
    } else {
        set_default_rtc_time();
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
    printk("Boot completed in %u ms (%.2f s)\n",
           boot_time_ms, boot_time_ms / 1000.0f);
    if (ENABLE_BLUETOOTH) {
        printk("Initializing Bluetooth irrigation service...\n");
        int ble_err = bt_irrigation_service_init();
        if (ble_err != 0) {
            printk("Error initializing BLE service: %d\n", ble_err);
        }
    }
    while (1) {
        k_sleep(K_SECONDS(60));
    }
    return 0;
}
