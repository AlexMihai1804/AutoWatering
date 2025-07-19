#ifndef TIMEZONE_H
#define TIMEZONE_H

/**
 * @file timezone.h
 * @brief Timezone and DST handling functions
 */

#include <stdint.h>
#include <stdbool.h>
#include "nvs_config.h"
#include "rtc.h"

/**
 * @brief Initialize timezone subsystem and load configuration
 * 
 * @return 0 on success, negative error code on failure
 */
int timezone_init(void);

/**
 * @brief Set timezone configuration
 * 
 * @param config Pointer to timezone configuration
 * @return 0 on success, negative error code on failure
 */
int timezone_set_config(const timezone_config_t *config);

/**
 * @brief Get current timezone configuration
 * 
 * @param config Pointer to store timezone configuration
 * @return 0 on success, negative error code on failure
 */
int timezone_get_config(timezone_config_t *config);

/**
 * @brief Convert RTC datetime (UTC) to Unix timestamp (UTC)
 * 
 * @param datetime RTC datetime structure (assumed to be UTC)
 * @return Unix timestamp in seconds since epoch (UTC)
 */
uint32_t timezone_rtc_to_unix_utc(const rtc_datetime_t *datetime);

/**
 * @brief Convert Unix timestamp (UTC) to RTC datetime (UTC)
 * 
 * @param timestamp Unix timestamp in seconds since epoch (UTC)
 * @param datetime Pointer to store RTC datetime structure
 * @return 0 on success, negative error code on failure
 */
int timezone_unix_to_rtc_utc(uint32_t timestamp, rtc_datetime_t *datetime);

/**
 * @brief Get current Unix timestamp in UTC
 * 
 * @return Unix timestamp in seconds since epoch (UTC), 0 if RTC unavailable
 */
uint32_t timezone_get_unix_utc(void);

/**
 * @brief Get current Unix timestamp in local time (with timezone and DST)
 * 
 * @return Unix timestamp in seconds since epoch (local time)
 */
uint32_t timezone_get_unix_local(void);

/**
 * @brief Convert UTC timestamp to local timestamp (applying timezone and DST)
 * 
 * @param utc_timestamp UTC timestamp in seconds since epoch
 * @return Local timestamp in seconds since epoch
 */
uint32_t timezone_utc_to_local(uint32_t utc_timestamp);

/**
 * @brief Convert local timestamp to UTC timestamp (removing timezone and DST)
 * 
 * @param local_timestamp Local timestamp in seconds since epoch
 * @return UTC timestamp in seconds since epoch
 */
uint32_t timezone_local_to_utc(uint32_t local_timestamp);

/**
 * @brief Check if DST is active for a given UTC timestamp
 * 
 * @param utc_timestamp UTC timestamp in seconds since epoch
 * @return true if DST is active, false otherwise
 */
bool timezone_is_dst_active(uint32_t utc_timestamp);

/**
 * @brief Get total offset (timezone + DST) for a given UTC timestamp
 * 
 * @param utc_timestamp UTC timestamp in seconds since epoch
 * @return Total offset in minutes
 */
int16_t timezone_get_total_offset(uint32_t utc_timestamp);

/**
 * @brief Convert Unix timestamp to RTC datetime with local time
 * 
 * @param timestamp Unix timestamp in seconds since epoch (UTC)
 * @param datetime Pointer to store local RTC datetime structure
 * @return 0 on success, negative error code on failure
 */
int timezone_unix_to_rtc_local(uint32_t timestamp, rtc_datetime_t *datetime);

#endif /* TIMEZONE_H */
