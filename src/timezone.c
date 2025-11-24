#include "timezone.h"
#include <errno.h>

/* In-memory timezone configuration. No persistence is performed anymore. */
static bool tz_initialized;
static timezone_config_t current_config;

static const timezone_config_t default_config = {
    .utc_offset_minutes = 0,
    .dst_enabled = 0,
    .dst_start_month = 0,
    .dst_start_week = 0,
    .dst_start_dow = 0,
    .dst_end_month = 0,
    .dst_end_week = 0,
    .dst_end_dow = 0,
    .dst_offset_minutes = 0,
};

static bool is_leap_year(uint16_t year)
{
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

static uint8_t get_days_in_month(uint8_t month, uint16_t year)
{
    static const uint8_t days_in_month[] = {
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
    };

    if (month == 2 && is_leap_year(year)) {
        return 29;
    }
    return days_in_month[month - 1];
}

int timezone_init(void)
{
    current_config = default_config;
    tz_initialized = true;
    return 0;
}

int timezone_set_config(const timezone_config_t *config)
{
    if (!config) {
        return -EINVAL;
    }

    current_config = *config;
    tz_initialized = true;
    return 0;
}

int timezone_get_config(timezone_config_t *config)
{
    if (!config) {
        return -EINVAL;
    }

    if (!tz_initialized) {
        timezone_init();
    }

    *config = current_config;
    return 0;
}

uint32_t timezone_rtc_to_unix_utc(const rtc_datetime_t *datetime)
{
    if (!datetime) {
        return 0;
    }

    uint32_t days = 0;

    for (uint16_t year = 1970; year < datetime->year; year++) {
        days += is_leap_year(year) ? 366 : 365;
    }

    for (uint8_t month = 1; month < datetime->month; month++) {
        days += get_days_in_month(month, datetime->year);
    }

    days += datetime->day - 1;

    uint32_t seconds = days * 86400UL +
                       datetime->hour * 3600UL +
                       datetime->minute * 60UL +
                       datetime->second;

    return seconds;
}

int timezone_unix_to_rtc_utc(uint32_t timestamp, rtc_datetime_t *datetime)
{
    if (!datetime) {
        return -EINVAL;
    }

    datetime->second = timestamp % 60;
    timestamp /= 60;
    datetime->minute = timestamp % 60;
    timestamp /= 60;
    datetime->hour = timestamp % 24;
    timestamp /= 24;

    datetime->day_of_week = (timestamp + 4) % 7;

    uint16_t year = 1970;
    uint32_t days = timestamp;

    while (true) {
        uint32_t days_in_year = is_leap_year(year) ? 366 : 365;
        if (days < days_in_year) {
            break;
        }
        days -= days_in_year;
        year++;
    }
    datetime->year = year;

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

uint32_t timezone_get_unix_utc(void)
{
    rtc_datetime_t now;
    if (rtc_datetime_get(&now) != 0) {
        return 0;
    }
    return timezone_rtc_to_unix_utc(&now);
}

bool timezone_is_dst_active(uint32_t utc_timestamp)
{
    (void)utc_timestamp;
    return false;
}

int16_t timezone_get_total_offset(uint32_t utc_timestamp)
{
    (void)utc_timestamp;

    if (!tz_initialized) {
        timezone_init();
    }

    return current_config.utc_offset_minutes;
}

uint32_t timezone_utc_to_local(uint32_t utc_timestamp)
{
    int16_t offset = timezone_get_total_offset(utc_timestamp);
    return utc_timestamp + (offset * 60);
}

uint32_t timezone_local_to_utc(uint32_t local_timestamp)
{
    int16_t offset = timezone_get_total_offset(local_timestamp);
    return local_timestamp - (offset * 60);
}

uint32_t timezone_get_unix_local(void)
{
    uint32_t utc = timezone_get_unix_utc();
    if (utc == 0) {
        return 0;
    }
    return timezone_utc_to_local(utc);
}

int timezone_unix_to_rtc_local(uint32_t timestamp, rtc_datetime_t *datetime)
{
    uint32_t local_timestamp = timezone_utc_to_local(timestamp);
    return timezone_unix_to_rtc_utc(local_timestamp, datetime);
}
