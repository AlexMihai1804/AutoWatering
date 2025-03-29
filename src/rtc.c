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

/* RTC device pointer for DS3231 */
static const struct device *rtc_dev = NULL;

/**
 * @brief Initialize the RTC device
 * 
 * @return 0 on success, negative error code on failure
 */
int rtc_init(void) {
    static bool init_attempted = false;
    
    // Only attempt to initialize once
    if (init_attempted) {
        return (rtc_dev && device_is_ready(rtc_dev)) ? 0 : -ENODEV;
    }
    
    init_attempted = true;
    
    // Try to get the RTC device using the correct compatible string (maxim,ds3231-rtc)
    rtc_dev = DEVICE_DT_GET_ONE(maxim_ds3231_rtc);
    
    // Try alternate methods if the first attempt fails
    if (!rtc_dev || !device_is_ready(rtc_dev)) {
        // Try by alias (requires rtc0 to point to the RTC child node)
        rtc_dev = DEVICE_DT_GET_OR_NULL(DT_ALIAS(rtc0));
    }
    
    // Try binding by name as last resort
    if (!rtc_dev || !device_is_ready(rtc_dev)) {
        rtc_dev = device_get_binding("DS3231");
    }
    
    if (!rtc_dev || !device_is_ready(rtc_dev)) {
        rtc_dev = device_get_binding("RTC_0");
    }
    
    if (!rtc_dev || !device_is_ready(rtc_dev)) {
        printk("DS3231 device not ready or not available\n");
        return -ENODEV;
    }
    
    printk("DS3231 RTC initialized\n");
    return 0;
}

/**
 * @brief Get current time from RTC
 * 
 * @param datetime Pointer to datetime structure to fill
 * @return 0 on success, negative error code on failure
 */
int rtc_datetime_get(rtc_datetime_t *datetime) {
    if (!rtc_dev || !datetime) {
        return -EINVAL;
    }
    
    struct rtc_time rtc_date;
    int ret = rtc_get_time(rtc_dev, &rtc_date);
    if (ret) {
        printk("Failed to read RTC: %d\n", ret);
        return ret;
    }
    
    /* Convert rtc_time structure to our datetime format */
    datetime->second = rtc_date.tm_sec;
    datetime->minute = rtc_date.tm_min;
    datetime->hour = rtc_date.tm_hour;
    datetime->day = rtc_date.tm_mday;
    datetime->month = rtc_date.tm_mon + 1;      // tm_mon is 0-11, we use 1-12
    datetime->year = rtc_date.tm_year + 1900;   // tm_year is years since 1900
    datetime->day_of_week = rtc_date.tm_wday;   // 0-6, 0 = Sunday
    
    return 0;
}

/**
 * @brief Set time on RTC
 * 
 * @param datetime Pointer to datetime structure with new time values
 * @return 0 on success, negative error code on failure
 */
int rtc_datetime_set(const rtc_datetime_t *datetime) {
    if (!rtc_dev || !datetime) {
        return -EINVAL;
    }
    
    /* Validate datetime values */
    if (datetime->second > 59 || datetime->minute > 59 || datetime->hour > 23 ||
        datetime->day < 1 || datetime->day > 31 || datetime->month < 1 || 
        datetime->month > 12 || datetime->year < 2000 || datetime->year > 2099 ||
        datetime->day_of_week > 6) {
        return -EINVAL;
    }
    
    /* Convert our datetime format to rtc_time structure */
    struct rtc_time rtc_date = {
        .tm_sec = datetime->second,
        .tm_min = datetime->minute,
        .tm_hour = datetime->hour,
        .tm_mday = datetime->day,
        .tm_mon = datetime->month - 1,      // Convert 1-12 to 0-11
        .tm_year = datetime->year - 1900,   // Convert actual year to years since 1900
        .tm_wday = datetime->day_of_week,
        .tm_yday = 0,                      // Not used
        .tm_isdst = 0                      // Not used
    };
    
    /* Set time using RTC driver */
    int ret = rtc_set_time(rtc_dev, &rtc_date);
    if (ret) {
        printk("Failed to set RTC: %d\n", ret);
    }
    
    return ret;
}

/**
 * @brief Check if RTC communication is working
 * 
 * @return true if RTC is responding, false otherwise
 */
bool rtc_is_available(void) {
    if (!rtc_dev || !device_is_ready(rtc_dev)) {
        return false;
    }
    
    /* Try to read the RTC to verify it's responsive */
    struct rtc_time rtc_date;
    return (rtc_get_time(rtc_dev, &rtc_date) == 0);
}

/**
 * @brief Display current time from RTC (utility function for debugging)
 */
void rtc_print_time(void) {
    rtc_datetime_t now;
    
    if (rtc_datetime_get(&now) == 0) {
        printk("Current RTC time: %04d-%02d-%02d %02d:%02d:%02d (day of week: %d)\n",
               now.year, now.month, now.day,
               now.hour, now.minute, now.second,
               now.day_of_week);
    } else {
        printk("Failed to read current time from RTC\n");
    }
}
