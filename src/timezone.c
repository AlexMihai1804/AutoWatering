#include "timezone.h"
#include "nvs_config.h"
#include "rtc.h"
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <string.h>
#include <stdlib.h>  /* for abs() function */

/* Current timezone configuration */
static timezone_config_t current_tz_config;
static bool tz_initialized = false;

/* Days in month lookup table */
static const uint8_t days_in_month[] = {
    31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
};

/* Check if year is leap year */
static bool is_leap_year(uint16_t year) {
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

/* Get days in month considering leap year */
static uint8_t get_days_in_month(uint8_t month, uint16_t year) {
    if (month == 2 && is_leap_year(year)) {
        return 29;
    }
    return days_in_month[month - 1];
}

/* Calculate day of year (1-366) */
static uint16_t get_day_of_year(uint8_t day, uint8_t month, uint16_t year) {
    uint16_t day_of_year = day;
    for (uint8_t m = 1; m < month; m++) {
        day_of_year += get_days_in_month(m, year);
    }
    return day_of_year;
}

/* Get the date of the Nth occurrence of a weekday in a month */
static uint8_t get_nth_weekday_date(uint8_t month, uint16_t year, uint8_t week, uint8_t dow) {
    /* Find first day of month */
    rtc_datetime_t first_day = {
        .year = year,
        .month = month,
        .day = 1,
        .hour = 0,
        .minute = 0,
        .second = 0
    };
    
    /* Calculate day of week for first day of month */
    uint32_t unix_first = timezone_rtc_to_unix_utc(&first_day);
    uint8_t first_dow = (unix_first / 86400 + 4) % 7; /* Unix epoch was Thursday */
    
    /* Calculate days to add to get to the desired weekday */
    int8_t days_to_add = dow - first_dow;
    if (days_to_add < 0) {
        days_to_add += 7;
    }
    
    /* Calculate the date */
    uint8_t target_date = 1 + days_to_add + (week - 1) * 7;
    
    /* Handle "last week" case (week = 5) */
    if (week == 5) {
        uint8_t max_days = get_days_in_month(month, year);
        while (target_date > max_days) {
            target_date -= 7;
        }
    }
    
    return target_date;
}

int timezone_init(void) {
    int ret = nvs_load_timezone_config(&current_tz_config);
    if (ret >= 0) {
        tz_initialized = true;
        printk("Timezone initialized: UTC%+d:%02d, DST %s\n",
               current_tz_config.utc_offset_minutes / 60,
               abs(current_tz_config.utc_offset_minutes % 60),
               current_tz_config.dst_enabled ? "enabled" : "disabled");
        return 0;
    }
    
    printk("Failed to load timezone config: %d\n", ret);
    return ret;
}

int timezone_set_config(const timezone_config_t *config) {
    if (!config) {
        return -EINVAL;
    }
    
    current_tz_config = *config;
    int ret = nvs_save_timezone_config(config);
    if (ret >= 0) {
        tz_initialized = true;
    }
    return ret;
}

int timezone_get_config(timezone_config_t *config) {
    if (!config) {
        return -EINVAL;
    }
    
    if (!tz_initialized) {
        int ret = timezone_init();
        if (ret < 0) {
            return ret;
        }
    }
    
    *config = current_tz_config;
    return 0;
}

uint32_t timezone_rtc_to_unix_utc(const rtc_datetime_t *datetime) {
    if (!datetime) {
        return 0;
    }
    
    /* Calculate days since Unix epoch (1970-01-01) */
    uint32_t days = 0;
    
    /* Add days for complete years */
    for (uint16_t year = 1970; year < datetime->year; year++) {
        days += is_leap_year(year) ? 366 : 365;
    }
    
    /* Add days for complete months in current year */
    for (uint8_t month = 1; month < datetime->month; month++) {
        days += get_days_in_month(month, datetime->year);
    }
    
    /* Add days in current month */
    days += datetime->day - 1;
    
    /* Convert to seconds and add time */
    uint32_t seconds = days * 86400UL + 
                      datetime->hour * 3600UL + 
                      datetime->minute * 60UL + 
                      datetime->second;
    
    return seconds;
}

int timezone_unix_to_rtc_utc(uint32_t timestamp, rtc_datetime_t *datetime) {
    if (!datetime) {
        return -EINVAL;
    }
    
    /* Extract time components */
    datetime->second = timestamp % 60;
    timestamp /= 60;
    datetime->minute = timestamp % 60;
    timestamp /= 60;
    datetime->hour = timestamp % 24;
    timestamp /= 24;
    
    /* Calculate day of week (Unix epoch was Thursday = 4) */
    datetime->day_of_week = (timestamp + 4) % 7;
    
    /* Calculate year, month, day */
    uint16_t year = 1970;
    uint32_t days = timestamp;
    
    /* Find year */
    while (true) {
        uint32_t days_in_year = is_leap_year(year) ? 366 : 365;
        if (days < days_in_year) {
            break;
        }
        days -= days_in_year;
        year++;
    }
    datetime->year = year;
    
    /* Find month and day */
    uint8_t month = 1;
    while (month <= 12) {
        uint8_t days_in_this_month = get_days_in_month(month, year);
        if (days < days_in_this_month) {
            break;
        }
        days -= days_in_this_month;
        month++;
    }
    datetime->month = month;
    datetime->day = days + 1;
    
    return 0;
}

uint32_t timezone_get_unix_utc(void) {
    rtc_datetime_t now;
    if (rtc_datetime_get(&now) != 0) {
        return 0; /* RTC unavailable */
    }
    return timezone_rtc_to_unix_utc(&now);
}

bool timezone_is_dst_active(uint32_t utc_timestamp) {
    if (!tz_initialized || !current_tz_config.dst_enabled) {
        return false;
    }
    
    rtc_datetime_t dt;
    if (timezone_unix_to_rtc_utc(utc_timestamp, &dt) != 0) {
        return false;
    }
    
    /* Calculate DST start and end dates for this year */
    uint8_t dst_start_date = get_nth_weekday_date(current_tz_config.dst_start_month, 
                                                 dt.year,
                                                 current_tz_config.dst_start_week,
                                                 current_tz_config.dst_start_dow);
    
    uint8_t dst_end_date = get_nth_weekday_date(current_tz_config.dst_end_month,
                                               dt.year, 
                                               current_tz_config.dst_end_week,
                                               current_tz_config.dst_end_dow);
    
    /* Check if current date is in DST period */
    if (current_tz_config.dst_start_month < current_tz_config.dst_end_month) {
        /* DST doesn't cross year boundary (Northern Hemisphere) */
        if (dt.month > current_tz_config.dst_start_month && 
            dt.month < current_tz_config.dst_end_month) {
            return true;
        } else if (dt.month == current_tz_config.dst_start_month) {
            return dt.day >= dst_start_date;
        } else if (dt.month == current_tz_config.dst_end_month) {
            return dt.day < dst_end_date;
        }
    } else {
        /* DST crosses year boundary (Southern Hemisphere) */
        if (dt.month > current_tz_config.dst_start_month || 
            dt.month < current_tz_config.dst_end_month) {
            return true;
        } else if (dt.month == current_tz_config.dst_start_month) {
            return dt.day >= dst_start_date;
        } else if (dt.month == current_tz_config.dst_end_month) {
            return dt.day < dst_end_date;
        }
    }
    
    return false;
}

int16_t timezone_get_total_offset(uint32_t utc_timestamp) {
    if (!tz_initialized) {
        return 0;
    }
    
    int16_t offset = current_tz_config.utc_offset_minutes;
    if (timezone_is_dst_active(utc_timestamp)) {
        offset += current_tz_config.dst_offset_minutes;
    }
    return offset;
}

uint32_t timezone_utc_to_local(uint32_t utc_timestamp) {
    int16_t offset = timezone_get_total_offset(utc_timestamp);
    return utc_timestamp + (offset * 60);
}

uint32_t timezone_local_to_utc(uint32_t local_timestamp) {
    /* This is approximate - we apply current offset */
    /* For exact conversion, we'd need to iterate and check DST transitions */
    int16_t offset = timezone_get_total_offset(local_timestamp);
    return local_timestamp - (offset * 60);
}

uint32_t timezone_get_unix_local(void) {
    uint32_t utc = timezone_get_unix_utc();
    if (utc == 0) {
        return 0;
    }
    return timezone_utc_to_local(utc);
}

int timezone_unix_to_rtc_local(uint32_t timestamp, rtc_datetime_t *datetime) {
    uint32_t local_timestamp = timezone_utc_to_local(timestamp);
    return timezone_unix_to_rtc_utc(local_timestamp, datetime);
}
