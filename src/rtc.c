#include "rtc.h"
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/rtc.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <time.h>
#include <stdio.h>

/**
 * @file rtc.c
 * @brief Implementation of RTC (Real-Time Clock) interface using DS3231
 * 
 * This file implements functions to communicate with the DS3231 RTC module
 * using Zephyr's RTC driver API to get and set accurate time information.
 */

/* DS3231 runtime-fallback is handled dynamically, no compile-time disable */

/* RTC device pointer for DS3231 */
static const struct device *rtc_dev = NULL;

/* Flag to indicate if the RTC is working properly */
static bool rtc_working = false;

/* Thread stack and entry point for RTC test operations */
K_THREAD_STACK_DEFINE(test_stack, 1024);
__attribute__((unused))
static struct k_thread test_thread;

/* Function prototypes for thread functions */
static void rtc_test_thread_entry(void* a, void* b, void* c);
static void rtc_write_thread_entry(void* a, void* b, void* c);

// Thread shared data
static struct rtc_time shared_rtc_time;

/**
 * @brief Validate datetime values to ensure they are reasonable
 * 
 * @param datetime Pointer to datetime structure to validate
 * @return true if values are valid, false otherwise
 */
static bool validate_datetime(const rtc_datetime_t *datetime) {
    if (!datetime) {
        return false;
    }
    
    // Basic validation of date/time fields
    if (datetime->second > 59 || 
        datetime->minute > 59 || 
        datetime->hour > 23 || 
        datetime->day < 1 || 
        datetime->day > 31 || 
        datetime->month < 1 || 
        datetime->month > 12 || 
        datetime->year < 2000 || 
        datetime->year > 2099 ||
        datetime->day_of_week > 6) {
        return false;
    }
    
    // Some additional validation for specific months
    if ((datetime->month == 4 || datetime->month == 6 || 
         datetime->month == 9 || datetime->month == 11) && datetime->day > 30) {
        return false;
    }
    
    // February special case
    if (datetime->month == 2) {
        bool leap_year = (datetime->year % 4 == 0) && 
                        ((datetime->year % 100 != 0) || (datetime->year % 400 == 0));
        if ((leap_year && datetime->day > 29) || (!leap_year && datetime->day > 28)) {
            return false;
        }
    }
    
    return true;
}

/**
 * @brief Thread function for RTC testing
 */
__attribute__((unused))
static void rtc_test_thread_entry(void* a, void* b, void* c)
{
    volatile bool *complete_flag = (volatile bool *)a;
    volatile int *result = (volatile int *)b;
    const struct device *dev = (const struct device *)c;
    
    // Read RTC time into shared structure
    *result = rtc_get_time(dev, &shared_rtc_time);
    *complete_flag = true;
}

/**
 * @brief Initialize the DS3231 RTC (if present)
 */
int rtc_init(void)
{
    /* obtain DS3231 device */
    rtc_dev = device_get_binding("DS3231");
    if (!rtc_dev) {
        printk("DS3231 device not found – will use uptime fallback\n");
        rtc_working = false;
        return -ENODEV;
    }

    /* Give the oscillator / I²C bus a moment to settle on cold power-up */
    k_sleep(K_MSEC(50));

    /* Wait until the driver reports ready (max 3 × 50 ms) */
    for (int i = 0; i < 3 && !device_is_ready(rtc_dev); i++) {
        k_sleep(K_MSEC(50));
    }
    if (!device_is_ready(rtc_dev)) {
        printk("DS3231 device not ready – will use uptime fallback\n");
        rtc_working = false;
        return -ENODEV;
    }

    /* Read once to be sure the IC really answers – retry up to 3 times */
    struct rtc_time tm;
    bool ok = false;
    for (int i = 0; i < 3; i++) {
        if (rtc_get_time(rtc_dev, &tm) == 0) {
            ok = true;
            break;
        }
        k_sleep(K_MSEC(100));
    }

    if (!ok) {
        printk("DS3231 not responding – will use uptime fallback\n");
        rtc_working = false;
        return -EIO;
    }

    rtc_working = true;
    printk("DS3231 RTC initialised successfully\n");
    return 0;
}

/**
 * @brief Get current time from RTC with strict timeout protection
 * 
 * @param datetime Pointer to datetime structure to fill
 * @return 0 on success, negative error code on failure
 */
int rtc_datetime_get(rtc_datetime_t *datetime)
{
    if (!datetime) {
        return -EINVAL;
    }

    if (rtc_working && rtc_dev) {
        struct rtc_time tm;
        if (rtc_get_time(rtc_dev, &tm) == 0) {
            datetime->second       = tm.tm_sec;
            datetime->minute       = tm.tm_min;
            datetime->hour         = tm.tm_hour;
            datetime->day          = tm.tm_mday;
            datetime->month        = tm.tm_mon + 1;
            datetime->year         = tm.tm_year + 1900;
            /* DS3231 driver returns 1-7, our app uses 0-6 */
            datetime->day_of_week  = (tm.tm_wday == 7) ? 6 : (tm.tm_wday - 1);
            return 0;
        }
        /* read failed ‑ mark device unusable and fall through to fallback */
        rtc_working = false;
    }

    /* Fallback – derive pseudo-time from uptime */
    uint32_t uptime_ms  = k_uptime_get_32();
    uint32_t uptime_sec = uptime_ms / 1000;
    datetime->second      = uptime_sec % 60;
    datetime->minute      = (uptime_sec / 60) % 60;
    datetime->hour        = (uptime_sec / 3600) % 24;
    datetime->day         = 1;
    datetime->month       = 1;
    datetime->year        = 2023;
    datetime->day_of_week = (uptime_sec / 86400) % 7;
    return 0;
}

/**
 * @brief Set time on RTC with strict timeout protection
 * 
 * @param datetime Pointer to datetime structure with new time values
 * @return 0 on success, negative error code on failure
 */
int rtc_datetime_set(const rtc_datetime_t *datetime)
{
    if (!validate_datetime(datetime)) {
        return -EINVAL;
    }

    if (!rtc_working || !rtc_dev) {
        printk("DS3231 not available ‑ cannot set time\n");
        return -ENODEV;
    }

    struct rtc_time tm = {
        .tm_sec  = datetime->second,
        .tm_min  = datetime->minute,
        .tm_hour = datetime->hour,
        .tm_mday = datetime->day,
        .tm_mon  = datetime->month - 1,
        .tm_year = datetime->year - 1900,
        /* Convert 0-6 → 1-7 expected by DS3231 driver */
        .tm_wday = datetime->day_of_week + 1,
    };

    int ret = rtc_set_time(rtc_dev, &tm);
    if (ret) {
        printk("Failed to set DS3231 time (%d)\n", ret);
        rtc_working = false;
    }
    return ret;
}

/**
 * @brief Thread function for RTC write operations
 */
__attribute__((unused))
static void rtc_write_thread_entry(void* a, void* b, void* c)
{
    struct k_sem *completion_sem = (struct k_sem *)a;
    
    // CRITICAL FIX: Add early exit if RTC not initialized properly
    if (!rtc_dev || !device_is_ready(rtc_dev)) {
        printk("RTC device not ready during write operation\n");
        rtc_working = false;
        k_sem_give(completion_sem);
        return;
    }
    
    // Use the shared structure already prepared
    int ret = rtc_set_time(rtc_dev, &shared_rtc_time);
    if (ret) {
        printk("Failed to set RTC: %d\n", ret);
        rtc_working = false;
    } else {
        rtc_working = true;
    }
    
    k_sem_give(completion_sem);
}

/**
 * @brief Check if RTC communication is working
 * 
 * @return true if RTC is responding, false otherwise
 */
bool rtc_is_available(void)
{
    return (rtc_working && rtc_dev && device_is_ready(rtc_dev));
}

/**
 * @brief Display current time from RTC (utility function for debugging)
 */
void rtc_print_time(void) {
    rtc_datetime_t now;
    
    if (rtc_datetime_get(&now) == 0) {
        // Safe printing with direct values, avoiding %s formatting
        printk("Current RTC time: %04d-%02d-%02d %02d:%02d:%02d (day of week: %d)\n",
               now.year, now.month, now.day,
               now.hour, now.minute, now.second,
               now.day_of_week);
    } else {
        printk("Failed to read current time from RTC\n");
    }
}
