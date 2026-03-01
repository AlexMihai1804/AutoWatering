#ifndef UCP_UART_H
#define UCP_UART_H

#include <zephyr/autoconf.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Event origin is included in EVENT_CHANGE payloads. */
typedef enum {
    UCP_ORIGIN_UART = 1,
    UCP_ORIGIN_BLE = 2,
    UCP_ORIGIN_SCHEDULER = 3,
    UCP_ORIGIN_SENSOR = 4,
    UCP_ORIGIN_LOCAL_FW = 5,
    UCP_ORIGIN_BOOT_RECOVERY = 6,
} ucp_origin_t;

/* v1 dense topic IDs (bitset-friendly for SUB_REQ). */
typedef enum {
    UCP_TOPIC_SYSTEM_STATUS   = 1,
    UCP_TOPIC_CONFIG_CHANGED  = 2,
    UCP_TOPIC_RTC_TZ_CHANGED  = 3,
    UCP_TOPIC_TASK_CURRENT    = 4,
    UCP_TOPIC_ALARM           = 5,
    UCP_TOPIC_OTA_TRANSITION  = 6,
    UCP_TOPIC_PEER_POWER      = 7,
    UCP_TOPIC_DIAGNOSTICS     = 8,
    UCP_TOPIC_CHANNEL_STATE   = 9,
    UCP_TOPIC_CHANNEL_CONFIG  = 10,
    UCP_TOPIC_SCHEDULE_CONFIG = 11,
    UCP_TOPIC_FLOW_LIVE       = 12,
    UCP_TOPIC_RAIN_LIVE       = 13,
    UCP_TOPIC_ENV_LIVE        = 14,
    UCP_TOPIC_HISTORY_READY   = 15,
    UCP_TOPIC_PACKS_CHANGED   = 16,
    UCP_TOPIC_SYSTEM_CONFIG   = 17,
    UCP_TOPIC_STATISTICS      = 18,
    UCP_TOPIC_AUTO_CALC_STATUS = 19,
    UCP_TOPIC_HYDRAULIC_STATUS = 20,
    UCP_TOPIC_COMP_STATUS     = 21,
    UCP_TOPIC_COMP_CONFIG     = 22,
    UCP_TOPIC_ONBOARDING_STATUS = 23,
    UCP_TOPIC_RESET_PROGRESS  = 24,
    UCP_TOPIC_CUSTOM_SOIL     = 25,
    UCP_TOPIC_SOIL_MOISTURE   = 26,
    UCP_TOPIC_INTERVAL_MODE   = 27,
    UCP_TOPIC_CHANNEL_CFG_STATUS = 28,
    UCP_TOPIC_RAIN_INTEGRATION = 29,
    UCP_TOPIC_BULK_SNAPSHOT   = 30,
    UCP_TOPIC_FLOW_CALIBRATION = 31,
} ucp_topic_id_t;

#ifdef CONFIG_AUTOWATERING_UCP_UART

int ucp_uart_init(void);
bool ucp_uart_is_ready(void);
void ucp_uart_request_notify_rescan(uint32_t topic_mask);

/* Optional publish helpers for future hooks from non-UCP code paths. */
int ucp_uart_publish_timezone_changed(const uint8_t *delta_tlv, size_t delta_len, ucp_origin_t origin);
int ucp_uart_publish_rtc_changed(const uint8_t *delta_tlv, size_t delta_len, ucp_origin_t origin);
int ucp_uart_publish_system_status_changed(const uint8_t *delta_tlv, size_t delta_len, ucp_origin_t origin);
int ucp_uart_publish_alarm(uint8_t alarm_code, uint16_t alarm_data, bool cleared, ucp_origin_t origin);
int ucp_uart_publish_valve_state(uint8_t channel_id, bool is_open, bool is_master, ucp_origin_t origin);

#else

static inline int ucp_uart_init(void) { return 0; }
static inline bool ucp_uart_is_ready(void) { return false; }
static inline void ucp_uart_request_notify_rescan(uint32_t topic_mask)
{
    (void)topic_mask;
}
static inline int ucp_uart_publish_timezone_changed(const uint8_t *delta_tlv, size_t delta_len, ucp_origin_t origin)
{
    (void)delta_tlv; (void)delta_len; (void)origin;
    return -ENOTSUP;
}
static inline int ucp_uart_publish_rtc_changed(const uint8_t *delta_tlv, size_t delta_len, ucp_origin_t origin)
{
    (void)delta_tlv; (void)delta_len; (void)origin;
    return -ENOTSUP;
}
static inline int ucp_uart_publish_system_status_changed(const uint8_t *delta_tlv, size_t delta_len, ucp_origin_t origin)
{
    (void)delta_tlv; (void)delta_len; (void)origin;
    return -ENOTSUP;
}
static inline int ucp_uart_publish_alarm(uint8_t alarm_code, uint16_t alarm_data, bool cleared, ucp_origin_t origin)
{
    (void)alarm_code; (void)alarm_data; (void)cleared; (void)origin;
    return -ENOTSUP;
}
static inline int ucp_uart_publish_valve_state(uint8_t channel_id, bool is_open, bool is_master, ucp_origin_t origin)
{
    (void)channel_id; (void)is_open; (void)is_master; (void)origin;
    return -ENOTSUP;
}

#endif /* CONFIG_AUTOWATERING_UCP_UART */

#endif /* UCP_UART_H */
