#ifndef RTC_H
#define RTC_H

#include <stdbool.h>
#include <stdint.h>

/**
 * @file rtc.h
 * @brief Interface for RTC (Real-Time Clock) functionality
 * 
 * This header defines functions for interfacing with the DS3231 RTC module
 * to obtain accurate date and time information.
 */

typedef struct {
    uint8_t second;     /**< Second (0-59) */
    uint8_t minute;     /**< Minute (0-59) */
    uint8_t hour;       /**< Hour (0-23) */
    uint8_t day;        /**< Day of month (1-31) */
    uint8_t month;      /**< Month (1-12) */
    uint16_t year;      /**< Year (e.g. 2023) */
    uint8_t day_of_week; /**< Day of week (0=Sunday, 1=Monday, etc) */
} rtc_datetime_t;

/**
 * @brief Initialize the RTC device
 * 
 * @return 0 on success, negative error code on failure
 */
int rtc_init(void);

/**
 * @brief Get current time from RTC
 * 
 * @param datetime Pointer to datetime structure to fill
 * @return 0 on success, negative error code on failure
 */
int rtc_datetime_get(rtc_datetime_t *datetime);

/**
 * @brief Set time on RTC
 * 
 * @param datetime Pointer to datetime structure with new time values
 * @return 0 on success, negative error code on failure
 */
int rtc_datetime_set(const rtc_datetime_t *datetime);

/**
 * @brief Check if RTC communication is working
 * 
 * @return true if RTC is responding, false otherwise
 */
bool rtc_is_available(void);

/**
 * @brief Display current time from RTC (utility function for debugging)
 */
void rtc_print_time(void);

#endif // RTC_H
