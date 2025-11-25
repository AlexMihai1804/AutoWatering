#include "timezone.h"
#include <errno.h>
#include <zephyr/kernel.h>

/* In-memory timezone configuration persisted via NVS when available. */
static bool tz_initialized;
static timezone_config_t current_config;
static uint32_t last_good_utc;
static uint32_t last_good_uptime_ms;
static bool have_fallback_time;

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
    timezone_config_t stored = default_config;
    have_fallback_time = false;
    last_good_utc = 0;
    last_good_uptime_ms = 0;

    if (nvs_config_is_ready()) {
        int ret = nvs_load_timezone_config(&stored);
        if (ret < 0) {
            stored = default_config;
        }
    }

    current_config = stored;
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
    if (nvs_config_is_ready()) {
        int ret = nvs_save_timezone_config(&current_config);
        if (ret < 0) {
            return ret;
        }
    }
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
        if (have_fallback_time) {
            uint32_t delta_ms = k_uptime_get_32() - last_good_uptime_ms;
            return last_good_utc + (delta_ms / 1000);
        }
        return 0;
    }
    uint32_t ts = timezone_rtc_to_unix_utc(&now);
    last_good_utc = ts;
    last_good_uptime_ms = k_uptime_get_32();
    have_fallback_time = true;
    return ts;
}

bool timezone_is_dst_active(uint32_t utc_timestamp)
{
    return timezone_get_total_offset(utc_timestamp) != current_config.utc_offset_minutes;
}

/* Calculate the nth weekday of a month (week 1-4, 5=last). DOW: 0=Sun..6=Sat. */
static uint32_t calc_weekday_in_month_ts(uint16_t year, uint8_t month, uint8_t week, uint8_t dow)
{
    rtc_datetime_t first = {
        .year = year,
        .month = month,
        .day = 1,
        .hour = 0,
        .minute = 0,
        .second = 0,
    };

    /* Day-of-week for first day (0=Sun) */
    timezone_unix_to_rtc_utc(timezone_rtc_to_unix_utc(&first), &first);
    uint8_t first_dow = first.day_of_week;

    uint8_t days_in_month = get_days_in_month(month, year);
    uint8_t day = 1 + ((dow + 7 - first_dow) % 7); /* first occurrence of dow */

    if (week == 5) {
        while (day + 7 <= days_in_month) {
            day += 7;
        }
    } else if (week > 1) {
        day += (week - 1) * 7;
        if (day > days_in_month) {
            day -= 7; /* clamp to last valid occurrence */
        }
    }

    rtc_datetime_t target = {
        .year = year,
        .month = month,
        .day = day,
        .hour = 2,      /* Assume 02:00 local transition */
        .minute = 0,
        .second = 0,
    };
    return timezone_rtc_to_unix_utc(&target);
}

/* Determine total offset (base + DST) for a given UTC timestamp. */
int16_t timezone_get_total_offset(uint32_t utc_timestamp)
{
    if (!tz_initialized) {
        timezone_init();
    }

    int16_t base_offset = current_config.utc_offset_minutes;
    if (!current_config.dst_enabled) {
        return base_offset;
    }

    /* Convert UTC to local using base offset only to evaluate rules */
    uint32_t local_ts = utc_timestamp + (base_offset * 60);

    /* Build DST start/end boundaries in local time for the year */
    rtc_datetime_t local_dt;
    timezone_unix_to_rtc_utc(local_ts, &local_dt); /* local_ts interpreted as UTC struct (OK for date math) */
    uint16_t year = local_dt.year;

    uint32_t dst_start_local = calc_weekday_in_month_ts(
        year,
        current_config.dst_start_month,
        current_config.dst_start_week,
        current_config.dst_start_dow
    );
    uint32_t dst_end_local = calc_weekday_in_month_ts(
        year,
        current_config.dst_end_month,
        current_config.dst_end_week,
        current_config.dst_end_dow
    );

    /* If end is before start (southern hemisphere), treat wrap-around */
    bool in_dst;
    if (dst_start_local <= dst_end_local) {
        in_dst = (local_ts >= dst_start_local) && (local_ts < dst_end_local);
    } else {
        in_dst = (local_ts >= dst_start_local) || (local_ts < dst_end_local);
    }

    return in_dst ? (base_offset + current_config.dst_offset_minutes) : base_offset;
}

uint32_t timezone_utc_to_local(uint32_t utc_timestamp)
{
    int16_t offset = timezone_get_total_offset(utc_timestamp);
    return utc_timestamp + (offset * 60);
}

/* Convert local wall-clock timestamp to UTC by applying the offset inferred from local time. */
uint32_t timezone_local_to_utc(uint32_t local_timestamp)
{
    if (!tz_initialized) {
        timezone_init();
    }

    int16_t base_offset = current_config.utc_offset_minutes;
    int16_t dst_offset = current_config.dst_enabled ? current_config.dst_offset_minutes : 0;

    /* Try with DST applied first (common case when clocks are advanced). */
    int16_t assumed_offset = base_offset + dst_offset;
    uint32_t utc_guess = local_timestamp - (assumed_offset * 60);
    if (timezone_get_total_offset(utc_guess) == assumed_offset) {
        return utc_guess;
    }

    /* Fallback to base offset. */
    assumed_offset = base_offset;
    utc_guess = local_timestamp - (assumed_offset * 60);
    return utc_guess;
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
