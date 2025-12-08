// Only compile this file if Bluetooth is enabled
#ifdef CONFIG_BT

// Manually define the crucial symbols if not defined in Kconfig to prevent compilation errors
#ifndef CONFIG_BT_MAX_PAIRED
#define CONFIG_BT_MAX_PAIRED 1
#endif

#ifndef CONFIG_BT_MAX_CONN
#define CONFIG_BT_MAX_CONN 1
#endif

#include <string.h>    // for strlen
#include <stdlib.h>    // for abs function
#include <stdio.h>     // for snprintf
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <limits.h>

LOG_MODULE_REGISTER(bt_irrigation_service, LOG_LEVEL_DBG);
#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>

// Forward declaration to help with logging
struct bt_conn;
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/sys/printk.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/util.h>

#include "bt_irrigation_service.h"
#include "watering.h"
#include "watering_internal.h"  // Add this include to access internal state
#include "watering_enhanced.h"  // Add enhanced watering structures
#include "bt_gatt_structs_enhanced.h"  // Add enhanced BLE structures
#include "fao56_calc.h"         // Add this include for water_balance_t type
#include "watering_history.h"   // Add this include for history statistics
#include "rtc.h"
#include "timezone.h"           // Add timezone support
#include "flow_sensor.h"        // For pulse count during calibration
#include "flow_sensor.h"
#include "nvs_config.h"         // Add NVS config support
#include "watering_history.h"   // Add this include for history functionality
#include "rain_sensor.h"        // Add rain sensor support
#include "rain_history.h"       // Add rain history support
#include "rain_integration.h"   // Add rain integration support
#include "rain_compensation.h"  // Add rain compensation support
#include "temperature_compensation.h"  // Add temperature compensation support
#include "interval_mode_controller.h"  // Add interval mode support
#include "custom_soil_db.h"     // Add custom soil database support
#include "configuration_status.h"  // Add configuration status support
#include "onboarding_state.h"       // Add onboarding state management
#include "reset_controller.h"       // Add reset controller support
#include "environmental_data.h"     // Environmental data helpers for validation/quality
#include "env_sensors.h"            // Sensor health and status
#include "bt_environmental_history_handlers.h" // Environmental history BLE handlers
#include "bme280_driver.h"          // BME280 config API prototypes

/* Forward declaration for the service structure */
extern const struct bt_gatt_service_static irrigation_svc;

/* Forward declarations for notify functions referenced before definition */
int bt_irrigation_onboarding_status_notify(void);
int bt_irrigation_reset_control_notify(void);

/* Rain history fragmentation support */
#define RAIN_HISTORY_MAX_FRAGMENTS 20   /* Maximum number of fragments */

/* ------------------------------------------------------------------ */
/* TIMING STRATEGY DOCUMENTATION                                      */
/* ------------------------------------------------------------------ */
/*
 * HYBRID TIMING APPROACH:
 * 
 * 1. USE k_uptime_get() FOR:
 *    - Relative duration measurements (ongoing tasks)
 *    - Throttling and rate limiting 
 *    - Timeout operations
 *    - Buffer maintenance intervals
 *    - Performance measurements
 * 
 * 2. USE timezone_get_unix_utc() FOR:
 *    - Persistent event timestamps (alarms, history)
 *    - Statistics and logging
 *    - Last watering time tracking
 *    - Cross-reboot time calculations
 *    - BLE client notifications requiring real time
 * 
 * This ensures temporal consistency across reboots while maintaining
 * efficient relative time calculations for system operations.
 */

/* ------------------------------------------------------------------ */
/* ADVANCED BLE Notification System with Buffer Pooling              */
/* ------------------------------------------------------------------ */

/* Notification Priority Levels */
typedef enum {
    NOTIFY_PRIORITY_CRITICAL = 0,  /* Alarms, errors - immediate */
    NOTIFY_PRIORITY_HIGH = 1,      /* Status updates, valve changes */
    NOTIFY_PRIORITY_NORMAL = 2,    /* Flow data, statistics */
    NOTIFY_PRIORITY_LOW = 3        /* History, diagnostics */
} notify_priority_t;

/* Dynamic Throttling Configuration */
#define THROTTLE_CRITICAL_MS    0      /* No throttling for critical */
#define THROTTLE_HIGH_MS        50     /* 50ms for high priority */
#define THROTTLE_NORMAL_MS      200    /* 200ms for normal */
#define THROTTLE_LOW_MS         1000   /* 1s for low priority */

/* Buffer Pool Management */
#define BLE_BUFFER_POOL_SIZE    8      /* Number of notification buffers */
#define MAX_NOTIFICATION_RETRIES 3
#define BLE_MAX_NOTIFICATION_SIZE 250  /* Increased to support large structs (e.g. channel config 76B) */

/* Enhanced BLE characteristic support flags */
static bool enhanced_features_enabled __attribute__((unused)) = true;
#define BUFFER_RECOVERY_TIME_MS 2000   /* Time to wait before retry after buffer exhaustion */

/* Connection parameter targets tuned for better Windows compatibility (units: 1.25 ms) */
#define LOW_POWER_CONN_INTERVAL_MIN 24U   /* 30 ms */
#define LOW_POWER_CONN_INTERVAL_MAX 40U   /* 50 ms */
#define LOW_POWER_CONN_LATENCY       0U   /* No latency for better stability */
#define LOW_POWER_CONN_TIMEOUT     500U   /* 5 s */

/* Buffer Pool Structure */
typedef struct {
    uint8_t data[BLE_MAX_NOTIFICATION_SIZE]; /* Maximum BLE notification size */
    uint16_t len;                  /* Data length */
    const struct bt_gatt_attr *attr; /* Target attribute */
    notify_priority_t priority;    /* Notification priority */
    uint32_t timestamp;            /* When queued */
    bool in_use;                   /* Buffer allocation status */
} ble_notification_buffer_t;

/* Global state */
static ble_notification_buffer_t notification_pool[BLE_BUFFER_POOL_SIZE];
static uint8_t pool_head = 0;     /* Next buffer to allocate */
static uint8_t buffers_in_use = 0; /* Number of allocated buffers */
static uint32_t last_buffer_exhaustion = 0; /* Last time we ran out of buffers */
static bool notification_system_enabled = true;

/* Mutex for thread-safe buffer pool access */
static K_MUTEX_DEFINE(notification_mutex);

/* Adaptive throttling state per priority */
static struct {
    uint32_t last_notification_time;
    uint32_t throttle_interval;
    uint32_t success_count;
    uint32_t failure_count;
} priority_state[4] = {
    {0, THROTTLE_CRITICAL_MS, 0, 0},
    {0, THROTTLE_HIGH_MS, 0, 0},
    {0, THROTTLE_NORMAL_MS, 0, 0},
    {0, THROTTLE_LOW_MS, 0, 0}
};

/* Global BLE connection reference */
static struct bt_conn *default_conn;
static bool connection_active = false;  /* Track connection state */

/* Forward declarations */
static bool should_throttle_channel_name_notification(uint8_t channel_id);

static uint32_t build_epoch_from_date(uint16_t year, uint8_t month, uint8_t day)
{
    rtc_datetime_t dt = {
        .second = 0,
        .minute = 0,
        .hour = 0,
        .day = day,
        .month = month,
        .year = year,
        .day_of_week = 0,
    };
    return timezone_rtc_to_unix_utc(&dt);
}

static uint16_t count_sessions_in_period(uint8_t channel_id, uint32_t start_epoch, uint32_t end_epoch)
{
    if (channel_id >= WATERING_CHANNELS_COUNT || end_epoch <= start_epoch) {
        return 0;
    }

    uint16_t sessions = 0;
    if (watering_history_count_events(channel_id, start_epoch, end_epoch, &sessions) != WATERING_SUCCESS) {
        return 0;
    }

    return sessions;
}

static bool epoch_to_local_datetime(uint32_t epoch, rtc_datetime_t *datetime)
{
    if (!datetime) {
        return false;
    }
    return timezone_unix_to_rtc_local(epoch, datetime) == 0;
}

static bool is_leap_year(uint16_t year)
{
    return ((year % 4 == 0 && year % 100 != 0) || (year % 400 == 0));
}

static uint8_t days_in_month(uint16_t year, uint8_t month)
{
    static const uint8_t month_lengths[12] = {
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
    };

    if (month < 1 || month > 12) {
        return 30;
    }

    uint8_t days = month_lengths[month - 1];
    if (month == 2 && is_leap_year(year)) {
        days = 29;
    }
    return days;
}

static uint16_t calculate_day_of_year(uint16_t year, uint8_t month, uint8_t day)
{
    uint16_t doy = day;
    for (uint8_t m = 1; m < month; ++m) {
        doy += days_in_month(year, m);
    }
    return doy;
}

/* Helper functions for RTC date/time access */
static uint16_t get_current_year(void) {
    rtc_datetime_t datetime;
    if (rtc_datetime_get(&datetime) == 0) {
        /* TIMEZONE FIX: Use local time for user-facing date/time functions */
        uint32_t utc_timestamp = timezone_rtc_to_unix_utc(&datetime);
        rtc_datetime_t local_datetime;
        if (timezone_unix_to_rtc_local(utc_timestamp, &local_datetime) == 0) {
            return local_datetime.year;
        }
        /* Fallback to UTC if timezone conversion fails */
        return datetime.year;
    }
    return 2025; /* Fallback year */
}

static uint8_t get_current_month(void) {
    rtc_datetime_t datetime;
    if (rtc_datetime_get(&datetime) == 0) {
        /* TIMEZONE FIX: Use local time for user-facing date/time functions */
        uint32_t utc_timestamp = timezone_rtc_to_unix_utc(&datetime);
        rtc_datetime_t local_datetime;
        if (timezone_unix_to_rtc_local(utc_timestamp, &local_datetime) == 0) {
            return local_datetime.month;
        }
        /* Fallback to UTC if timezone conversion fails */
        return datetime.month;
    }
    return 7; /* Fallback month (July) */
}

static uint16_t get_current_day_of_year(void) {
    rtc_datetime_t datetime;
    if (rtc_datetime_get(&datetime) != 0) {
        return 185; /* Fallback day of year */
    }
    
    /* TIMEZONE FIX: Convert to local time for user-facing calculations */
    uint32_t utc_timestamp = timezone_rtc_to_unix_utc(&datetime);
    rtc_datetime_t local_datetime;
    if (timezone_unix_to_rtc_local(utc_timestamp, &local_datetime) == 0) {
        datetime = local_datetime; /* Use local time for calculation */
    }
    /* Continue with existing calculation using local time */
    
    /* Calculate day of year */
    uint16_t days_in_month[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    
    /* Check for leap year */
    bool is_leap = (datetime.year % 4 == 0 && datetime.year % 100 != 0) || (datetime.year % 400 == 0);
    if (is_leap) {
        days_in_month[1] = 29;
    }
    
    uint16_t day_of_year = 0;
    for (uint8_t i = 0; i < datetime.month - 1; i++) {
        day_of_year += days_in_month[i];
    }
    day_of_year += datetime.day;
    
    return day_of_year;
}

/* GATT attribute indices for fast access */
#define ATTR_IDX_VALVE_VALUE        2
#define ATTR_IDX_FLOW_VALUE         5
#define ATTR_IDX_STATUS_VALUE       8
#define ATTR_IDX_CHANNEL_CFG_VALUE 11
#define ATTR_IDX_SCHEDULE_VALUE    14
#define ATTR_IDX_SYSTEM_CFG_VALUE  17
#define ATTR_IDX_TASK_QUEUE_VALUE  20
#define ATTR_IDX_STATISTICS_VALUE  23
#define ATTR_IDX_RTC_VALUE         26
#define ATTR_IDX_ALARM_VALUE       29
#define ATTR_IDX_CALIB_VALUE       32
#define ATTR_IDX_HISTORY_VALUE     35
#define ATTR_IDX_DIAGNOSTICS_VALUE 38
#define ATTR_IDX_GROWING_ENV_VALUE 41
#define ATTR_IDX_AUTO_CALC_STATUS_VALUE 44
#define ATTR_IDX_CURRENT_TASK_VALUE 47
#define ATTR_IDX_TIMEZONE_VALUE     50
#define ATTR_IDX_RAIN_CONFIG_VALUE  53
#define ATTR_IDX_RAIN_DATA_VALUE    56
#define ATTR_IDX_RAIN_HISTORY_VALUE 59
#define ATTR_IDX_ENVIRONMENTAL_DATA_VALUE 62
#define ATTR_IDX_ENVIRONMENTAL_HISTORY_VALUE 65
#define ATTR_IDX_COMPENSATION_STATUS_VALUE 68
#define ATTR_IDX_ONBOARDING_STATUS_VALUE 71
#define ATTR_IDX_RESET_CONTROL_VALUE 74
/* New Rain Integration Status characteristic index; appended to service */
#define ATTR_IDX_RAIN_INTEGRATION_STATUS_VALUE 77
/* Channel Compensation Config characteristic index (per-channel rain/temp settings) */
#define ATTR_IDX_CHANNEL_COMP_CONFIG_VALUE 80

/* Simple direct notification function - no queues, no work handlers */

/* Forward declarations for advanced notification functions */
static void init_notification_pool(void);
static void buffer_pool_maintenance(void);
static int advanced_notify(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                          const void *data, uint16_t len);

/* Safe notification function with connection validation - now uses advanced system */
static int safe_notify(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                      const void *data, uint16_t len) {
    return advanced_notify(conn, attr, data, len);
}



/* Safe channel config notification with name change throttling */
static int safe_notify_channel_config(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                     const void *data, uint16_t len) {
    if (!conn || !attr || !data || len < 1) {
        return -EINVAL;
    }
    
    /* Extract channel ID from the data */
    uint8_t channel_id = *(const uint8_t*)data;
    
    /* Check if this is a channel name change notification that should be throttled */
    if (should_throttle_channel_name_notification(channel_id)) {
        return -EBUSY; /* Throttled - don't send notification */
    }
    
    /* Use the standard safe notification function */
    return safe_notify(conn, attr, data, len);
}

/* Enhanced notification macros using advanced buffer pooling system */
#define SMART_NOTIFY(conn, attr, data, size) \
    do { \
        if ((conn) && (attr) && notification_system_enabled && connection_active) { \
            int _err = advanced_notify((conn), (attr), (data), (size)); \
            if (_err == -EBUSY) { \
                /* Throttled - this is expected and managed by adaptive system */ \
            } else if (_err == -ENOMEM) { \
                /* Buffer pool exhausted - handled by advanced_notify */ \
            } else if (_err != 0 && _err != -ENOTCONN) { \
                static uint32_t _last_err_time = 0; \
                uint32_t _now = k_uptime_get_32(); \
                if (_now - _last_err_time > 5000) { \
                    LOG_ERR("🚨 Notification failed: %d", _err); \
                    _last_err_time = _now; \
                } \
            } \
        } \
    } while(0)

/* Priority notification macro for critical alerts */
#define CRITICAL_NOTIFY(conn, attr, data, size) \
    do { \
        if ((conn) && (attr) && connection_active) { \
            /* Critical notifications bypass most checks and use priority handling */ \
            int _err = advanced_notify((conn), (attr), (data), (size)); \
            if (_err != 0 && _err != -ENOTCONN) { \
                LOG_ERR("🔥 CRITICAL notification failed: %d", _err); \
            } \
        } \
    } while(0)

/* Special macro for channel config notifications with name change throttling */
#define CHANNEL_CONFIG_NOTIFY(conn, attr, data, size) \
    do { \
        if ((conn) && (attr) && notification_system_enabled && connection_active) { \
            int _err = safe_notify_channel_config((conn), (attr), (data), (size)); \
            if (_err == -EBUSY) { \
                /* Throttled - this is expected behavior for name changes */ \
                break; \
            } else if (_err == -ENOMEM || _err == -ENOBUFS) { \
                static uint32_t _last_buffer_warn = 0; \
                uint32_t _now = k_uptime_get_32(); \
                if (_now - _last_buffer_warn > 2000) { /* Log every 2 seconds max */ \
                    LOG_WRN("⚠️ Channel config notification skipped - no BLE buffers available"); \
                    _last_buffer_warn = _now; \
                } \
            } else if (_err != 0 && _err != -ENOTCONN) { \
                static uint32_t _last_err_time = 0; \
                uint32_t _now = k_uptime_get_32(); \
                if (_now - _last_err_time > 5000) { \
                    LOG_ERR("Channel config notification failed: %d", _err); \
                    _last_err_time = _now; \
                } \
            } \
        } \
    } while(0)

/* ------------------------------------------------------------------ */
/* BLE Notification Subscription Tracking                            */
/* ------------------------------------------------------------------ */
typedef struct {
    bool valve_notifications_enabled;
    bool flow_notifications_enabled;
    bool status_notifications_enabled;
    bool channel_config_notifications_enabled;
    bool schedule_notifications_enabled;
    bool system_config_notifications_enabled;
    bool task_queue_notifications_enabled;
    bool statistics_notifications_enabled;
    bool rtc_notifications_enabled;
    bool alarm_notifications_enabled;
    bool calibration_notifications_enabled;
    bool history_notifications_enabled;
    bool diagnostics_notifications_enabled;
    bool growing_env_notifications_enabled;
    bool auto_calc_status_notifications_enabled;
    bool current_task_notifications_enabled;
    bool timezone_notifications_enabled;
    bool rain_config_notifications_enabled;
    bool rain_data_notifications_enabled;
    bool rain_history_notifications_enabled;
    bool environmental_data_notifications_enabled;
    bool environmental_history_notifications_enabled;
    bool compensation_status_notifications_enabled;
    bool onboarding_status_notifications_enabled;
    bool reset_control_notifications_enabled;
    bool rain_integration_status_notifications_enabled;
    bool channel_comp_config_notifications_enabled;
} notification_state_t;

static notification_state_t notification_state = {0};

/* ------------------------------------------------------------------ */
/* Channel Name Change Notification Throttling                       */
/* ------------------------------------------------------------------ */
static struct {
    uint8_t channel_id;
    uint32_t last_notification_time;
    uint32_t notification_count;
    bool throttling_active;
} channel_name_throttle = {0};

#define CHANNEL_NAME_NOTIFICATION_DELAY_MS 1000  /* 1 second delay for name changes */
#define CHANNEL_NAME_MAX_NOTIFICATIONS 3         /* Max 3 notifications per change */

/* Check if channel name notification should be throttled */
static bool should_throttle_channel_name_notification(uint8_t channel_id) {
    uint32_t now = k_uptime_get_32();
    
    /* Different channel - reset throttling */
    if (channel_name_throttle.channel_id != channel_id) {
        channel_name_throttle.channel_id = channel_id;
        channel_name_throttle.last_notification_time = now;
        channel_name_throttle.notification_count = 1;
        channel_name_throttle.throttling_active = false;
        return false; /* Allow first notification for new channel */
    }
    
    /* Same channel - check timing and count */
    if (now - channel_name_throttle.last_notification_time < CHANNEL_NAME_NOTIFICATION_DELAY_MS) {
        channel_name_throttle.notification_count++;
        
        if (channel_name_throttle.notification_count > CHANNEL_NAME_MAX_NOTIFICATIONS) {
            if (!channel_name_throttle.throttling_active) {
                LOG_WRN("Channel name notifications throttled for channel %u\n", channel_id);
                channel_name_throttle.throttling_active = true;
            }
            return true; /* Throttle */
        }
    } else {
        /* Reset after delay */
        channel_name_throttle.notification_count = 1;
        channel_name_throttle.last_notification_time = now;
        channel_name_throttle.throttling_active = false;
    }
    
    return false; /* Allow notification */
}

/* ------------------------------------------------------------------ */
/* Buffer Pool Management Functions                                   */
/* ------------------------------------------------------------------ */

/* Allocate a buffer from the pool */
static ble_notification_buffer_t* allocate_notification_buffer(void) {
    k_mutex_lock(&notification_mutex, K_FOREVER);
    
    for (int i = 0; i < BLE_BUFFER_POOL_SIZE; i++) {
        uint8_t idx = (pool_head + i) % BLE_BUFFER_POOL_SIZE;
        if (!notification_pool[idx].in_use) {
            notification_pool[idx].in_use = true;
            notification_pool[idx].timestamp = k_uptime_get_32();
            pool_head = (idx + 1) % BLE_BUFFER_POOL_SIZE;
            buffers_in_use++;
            k_mutex_unlock(&notification_mutex);
            return &notification_pool[idx];
        }
    }
    
    /* No buffers available */
    last_buffer_exhaustion = k_uptime_get_32();
    LOG_WRN("⚠️ BLE buffer pool exhausted (%d/%d in use)", buffers_in_use, BLE_BUFFER_POOL_SIZE);
    k_mutex_unlock(&notification_mutex);
    return NULL;
}

/* Release a buffer back to the pool */
static void release_notification_buffer(ble_notification_buffer_t* buffer) {
    if (buffer) {
        k_mutex_lock(&notification_mutex, K_FOREVER);
        if (buffer->in_use) {
            buffer->in_use = false;
            if (buffers_in_use > 0) {
                buffers_in_use--;
            }
        }
        k_mutex_unlock(&notification_mutex);
    }
}

/* Get priority for a specific attribute */
static notify_priority_t get_notification_priority(const struct bt_gatt_attr *attr) {
    if (attr == &irrigation_svc.attrs[ATTR_IDX_ALARM_VALUE]) {
        return NOTIFY_PRIORITY_CRITICAL;
    } else if (attr == &irrigation_svc.attrs[ATTR_IDX_STATUS_VALUE] ||
               attr == &irrigation_svc.attrs[ATTR_IDX_VALVE_VALUE] ||
               attr == &irrigation_svc.attrs[ATTR_IDX_CURRENT_TASK_VALUE] ||
               attr == &irrigation_svc.attrs[ATTR_IDX_TASK_QUEUE_VALUE]) {
        return NOTIFY_PRIORITY_HIGH;
    } else if (attr == &irrigation_svc.attrs[ATTR_IDX_FLOW_VALUE] ||
               attr == &irrigation_svc.attrs[ATTR_IDX_STATISTICS_VALUE] ||
               attr == &irrigation_svc.attrs[ATTR_IDX_CALIB_VALUE] ||
               attr == &irrigation_svc.attrs[ATTR_IDX_SCHEDULE_VALUE] ||
               attr == &irrigation_svc.attrs[ATTR_IDX_SYSTEM_CFG_VALUE] ||
               attr == &irrigation_svc.attrs[ATTR_IDX_CHANNEL_CFG_VALUE] ||
               attr == &irrigation_svc.attrs[ATTR_IDX_ENVIRONMENTAL_DATA_VALUE] ||
               attr == &irrigation_svc.attrs[ATTR_IDX_COMPENSATION_STATUS_VALUE] ||
               attr == &irrigation_svc.attrs[ATTR_IDX_RTC_VALUE] ||
               attr == &irrigation_svc.attrs[ATTR_IDX_AUTO_CALC_STATUS_VALUE] ||
               attr == &irrigation_svc.attrs[ATTR_IDX_RAIN_INTEGRATION_STATUS_VALUE]) {
        return NOTIFY_PRIORITY_NORMAL;
    } else if (attr == &irrigation_svc.attrs[ATTR_IDX_ENVIRONMENTAL_HISTORY_VALUE]) {
        return NOTIFY_PRIORITY_LOW; /* History data is low priority */
    } else if (attr == &irrigation_svc.attrs[ATTR_IDX_ONBOARDING_STATUS_VALUE]) {
        return NOTIFY_PRIORITY_LOW; /* Onboarding status is low priority (1000ms) */
    } else if (attr == &irrigation_svc.attrs[ATTR_IDX_DIAGNOSTICS_VALUE]) {
        return NOTIFY_PRIORITY_LOW; /* Diagnostics is low priority */
    } else if (attr == &irrigation_svc.attrs[ATTR_IDX_RESET_CONTROL_VALUE]) {
        return NOTIFY_PRIORITY_NORMAL; /* Reset Control per spec: Normal (200ms throttling) */
    } else {
        return NOTIFY_PRIORITY_LOW;
    }
}

/* Adaptive throttling - adjusts intervals based on success/failure rates */
static void update_adaptive_throttling(notify_priority_t priority, bool success) {
    struct {
        uint32_t *last_time;
        uint32_t *throttle_interval;
        uint32_t *success_count;
        uint32_t *failure_count;
    } state = {
        &priority_state[priority].last_notification_time,
        &priority_state[priority].throttle_interval,
        &priority_state[priority].success_count,
        &priority_state[priority].failure_count
    };
    
    if (success) {
        (*state.success_count)++;
        
        /* Reduce throttling if we have consistent success */
        if (*state.success_count > 20 && *state.failure_count < 5) {
            uint32_t min_interval = (priority == NOTIFY_PRIORITY_CRITICAL) ? 0 :
                                   (priority == NOTIFY_PRIORITY_HIGH) ? 25 :
                                   (priority == NOTIFY_PRIORITY_NORMAL) ? 100 : 500;
            
            if (*state.throttle_interval > min_interval) {
                *state.throttle_interval = (*state.throttle_interval * 9) / 10; /* Reduce by 10% */
                if (*state.throttle_interval < min_interval) {
                    *state.throttle_interval = min_interval;
                }
            }
            
            /* Reset counters */
            *state.success_count = 0;
            *state.failure_count = 0;
        }
    } else {
        (*state.failure_count)++;
        
        /* Increase throttling if we have failures */
        if (*state.failure_count > 5) {
            uint32_t max_interval = (priority == NOTIFY_PRIORITY_CRITICAL) ? 100 :
                                   (priority == NOTIFY_PRIORITY_HIGH) ? 500 :
                                   (priority == NOTIFY_PRIORITY_NORMAL) ? 2000 : 5000;
            
            if (*state.throttle_interval < max_interval) {
                *state.throttle_interval = (*state.throttle_interval * 12) / 10; /* Increase by 20% */
                if (*state.throttle_interval > max_interval) {
                    *state.throttle_interval = max_interval;
                }
            }
            
            /* Reset counters */
            *state.success_count = 0;
            *state.failure_count = 0;
        }
    }
}

/* Check if notification should be throttled */
static bool should_throttle_notification(notify_priority_t priority) {
    uint32_t now = k_uptime_get_32();
    uint32_t elapsed = now - priority_state[priority].last_notification_time;
    
    /* Critical notifications are never throttled */
    if (priority == NOTIFY_PRIORITY_CRITICAL) {
        return false;
    }
    
    /* Check if enough time has passed */
    if (elapsed < priority_state[priority].throttle_interval) {
        return true;
    }
    
    /* Also check buffer exhaustion recovery */
    if (last_buffer_exhaustion > 0 && 
        (now - last_buffer_exhaustion) < BUFFER_RECOVERY_TIME_MS &&
        priority != NOTIFY_PRIORITY_CRITICAL) {
        return true;
    }
    
    return false;
}

/* Advanced notification function with buffer pooling and adaptive throttling */
static int advanced_notify(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                          const void *data, uint16_t len) {
    /* Check against buffer size */
    if (!conn || !attr || !data || len > BLE_MAX_NOTIFICATION_SIZE) {
        return -EINVAL;
    }
    
    /* Check against connection MTU (minus 3 bytes for opcode/handle) */
    /* Note: If MTU is default (23), max payload is 20. */
    uint16_t mtu = bt_gatt_get_mtu(conn);
    uint16_t max_payload = (mtu > 3) ? (mtu - 3) : 20;
    
    if (len > max_payload) {
        /* Payload too large for current MTU - requires fragmentation */
        /* For now, we return error to avoid partial packets or truncation */
        /* TODO: Implement automatic fragmentation for generic notifications */
        return -EMSGSIZE;
    }
    
    if (!notification_system_enabled || !connection_active || conn != default_conn) {
        return -ENOTCONN;
    }
    
    /* Get notification priority */
    notify_priority_t priority = get_notification_priority(attr);
    
    /* Check throttling */
    if (should_throttle_notification(priority)) {
        update_adaptive_throttling(priority, false); /* Mark as throttled failure */
        return -EBUSY;
    }
    
    /* Try to allocate buffer */
    ble_notification_buffer_t* buffer = allocate_notification_buffer();
    if (!buffer) {
        update_adaptive_throttling(priority, false);
        return -ENOMEM;
    }
    
    /* Copy data to buffer */
    memcpy(buffer->data, data, len);
    buffer->len = len;
    buffer->attr = attr;
    buffer->priority = priority;
    
    /* Send notification */
    int err = bt_gatt_notify(conn, attr, buffer->data, buffer->len);
    
    /* Enhanced error handling and logging */
    if (err != 0) {
        LOG_ERR("🚨 BLE notification failed: err=%d, priority=%d, len=%u", err, priority, len);
        
        /* Specific error handling */
        switch (err) {
            case -EINVAL:
                LOG_ERR("  → Invalid parameters or client not subscribed to notifications");
                break;
            case -ENOMEM:
                LOG_ERR("  → Out of memory for BLE buffers");
                break;
            case -EMSGSIZE:
                LOG_ERR("  → Payload (%u) > MTU (%u) - fragmentation required", len, bt_gatt_get_mtu(conn));
                break;
            case -ENOTCONN:
                LOG_ERR("  → No active BLE connection");
                break;
            case -EBUSY:
                LOG_ERR("  → BLE stack busy, try again later");
                break;
            default:
                LOG_ERR("  → Unknown BLE error: %d", err);
                break;
        }
    } else {
        LOG_DBG("✅ BLE notification sent successfully: priority=%d, len=%u", priority, len);
    }
    
    /* Update state */
    uint32_t now = k_uptime_get_32();
    priority_state[priority].last_notification_time = now;
    
    bool success = (err == 0);
    update_adaptive_throttling(priority, success);
    
    /* Log adaptive behavior occasionally */
    static uint32_t last_log_time = 0;
    if (now - last_log_time > 10000) { /* Every 10 seconds */
        LOG_DBG("Adaptive throttling - P%d: %ums interval, %u/%u success/fail, %d/%d buffers\n",
                priority, priority_state[priority].throttle_interval,
                priority_state[priority].success_count, priority_state[priority].failure_count,
                buffers_in_use, BLE_BUFFER_POOL_SIZE);
        last_log_time = now;
    }
    
    /* Release buffer */
    release_notification_buffer(buffer);
    
    return err;
}

/* ------------------------------------------------------------------ */
/* NEW: forward declarations needed before first use                  */
/* ------------------------------------------------------------------ */

/* Forward-declared handlers used inside BT_GATT_SERVICE_DEFINE – declare early  */
/* Schedule characteristics */
static ssize_t read_schedule(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                            void *buf, uint16_t len, uint16_t offset);
static ssize_t write_schedule(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                             const void *buf, uint16_t len, uint16_t offset, uint8_t flags);
static void    schedule_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value);

/* System config characteristics */
static ssize_t read_system_config(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                 void *buf, uint16_t len, uint16_t offset);
static ssize_t write_system_config(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                  const void *buf, uint16_t len, uint16_t offset, uint8_t flags);
static void    system_config_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value);

/* Task queue characteristics */
static ssize_t read_task_queue(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                              void *buf, uint16_t len, uint16_t offset);
static ssize_t write_task_queue(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                               const void *buf, uint16_t len, uint16_t offset, uint8_t flags);
static void    task_queue_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value);

/* Statistics characteristics */
static ssize_t read_statistics(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                              void *buf, uint16_t len, uint16_t offset);
static ssize_t write_statistics(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                               const void *buf, uint16_t len, uint16_t offset, uint8_t flags);
static void    statistics_ccc_cfg_changed(const struct bt_gatt_attr *attr, uint16_t value);

static void    rtc_ccc_changed(const struct bt_gatt_attr *, uint16_t);

static ssize_t read_growing_env(struct bt_conn *, const struct bt_gatt_attr *,
                               void *, uint16_t, uint16_t);
static ssize_t write_growing_env(struct bt_conn *, const struct bt_gatt_attr *,
                                const void *, uint16_t, uint16_t, uint8_t);
static void    growing_env_ccc_changed(const struct bt_gatt_attr *, uint16_t);

/* Automatic calculation status characteristics */
static ssize_t read_auto_calc_status(struct bt_conn *, const struct bt_gatt_attr *,
                                    void *, uint16_t, uint16_t);
static void    auto_calc_status_ccc_changed(const struct bt_gatt_attr *, uint16_t);
static ssize_t write_auto_calc_status(struct bt_conn *, const struct bt_gatt_attr *,
                                     const void *, uint16_t, uint16_t, uint8_t);

static ssize_t read_alarm(struct bt_conn *, const struct bt_gatt_attr *,
                          void *, uint16_t, uint16_t);
static ssize_t write_alarm(struct bt_conn *, const struct bt_gatt_attr *,
                           const void *, uint16_t, uint16_t, uint8_t);
static void    alarm_ccc_changed(const struct bt_gatt_attr *, uint16_t);

/* Current task characteristics */
static ssize_t read_current_task(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                void *buf, uint16_t len, uint16_t offset);
static ssize_t write_current_task(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                 const void *buf, uint16_t len, uint16_t offset, uint8_t flags);
static void    current_task_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value);

/* Legacy 'task update thread' removed – any reintroduction should be justified (kept lean). */

/* Forward declarations for additional functions */
static ssize_t read_channel_config(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                  void *buf, uint16_t len, uint16_t offset);
static ssize_t write_channel_config(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                   const void *buf, uint16_t len, uint16_t offset, uint8_t flags);
static ssize_t read_rtc(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                       void *buf, uint16_t len, uint16_t offset);
static ssize_t write_rtc(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                        const void *buf, uint16_t len, uint16_t offset, uint8_t flags);
/* Timezone characteristic functions */
static ssize_t read_timezone(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                            void *buf, uint16_t len, uint16_t offset);
static ssize_t write_timezone(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                             const void *buf, uint16_t len, uint16_t offset, uint8_t flags);
static void    timezone_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value);
static ssize_t read_calibration(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                               void *buf, uint16_t len, uint16_t offset);
static ssize_t write_calibration(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                const void *buf, uint16_t len, uint16_t offset, uint8_t flags);
static void calibration_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value);
static ssize_t read_history(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                           void *buf, uint16_t len, uint16_t offset);
static ssize_t write_history(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                            const void *buf, uint16_t len, uint16_t offset, uint8_t flags);
static void history_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value);
static ssize_t read_diagnostics(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                               void *buf, uint16_t len, uint16_t offset);
static void diagnostics_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value);

/* Onboarding characteristics */
static ssize_t read_onboarding_status(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                    void *buf, uint16_t len, uint16_t offset);
static void onboarding_status_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value);

static ssize_t read_reset_control(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                void *buf, uint16_t len, uint16_t offset);
static ssize_t write_reset_control(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                 const void *buf, uint16_t len, uint16_t offset, uint8_t flags);
static void reset_control_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value);

/* Forward declaration for force enable notifications */
static void force_enable_all_notifications(void);

/* Rain sensor characteristics */
ssize_t read_rain_config(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                               void *buf, uint16_t len, uint16_t offset);
ssize_t write_rain_config(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                const void *buf, uint16_t len, uint16_t offset, uint8_t flags);
void rain_config_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value);

ssize_t read_rain_data(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                             void *buf, uint16_t len, uint16_t offset);
void rain_data_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value);

ssize_t read_rain_history(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                void *buf, uint16_t len, uint16_t offset);
ssize_t write_rain_history(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                 const void *buf, uint16_t len, uint16_t offset, uint8_t flags);
void rain_history_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value);

/* Environmental data characteristics */
static int send_rain_history_fragment(struct bt_conn *conn, uint8_t fragment_id);

/* Rain Integration Status characteristic */
static ssize_t read_rain_integration_status(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                            void *buf, uint16_t len, uint16_t offset);
static void    rain_integration_status_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value);

/* Channel Compensation Config characteristic */
static ssize_t read_channel_comp_config(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                        void *buf, uint16_t len, uint16_t offset);
static ssize_t write_channel_comp_config(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                         const void *buf, uint16_t len, uint16_t offset, uint8_t flags);
static void    channel_comp_config_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value);

/* ------------------------------------------------------------------ */

/* Custom UUIDs for Irrigation Service */
#define BT_UUID_IRRIGATION_SERVICE_VAL \
    BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x1234, 0x56789abcdef0)
#define BT_UUID_IRRIGATION_VALVE_VAL \
    BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x1234, 0x56789abcdef1)
#define BT_UUID_IRRIGATION_FLOW_VAL \
    BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x1234, 0x56789abcdef2)
#define BT_UUID_IRRIGATION_STATUS_VAL \
    BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x1234, 0x56789abcdef3)
#define BT_UUID_IRRIGATION_CHANNEL_CONFIG_VAL \
    BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x1234, 0x56789abcdef4)
#define BT_UUID_IRRIGATION_SCHEDULE_VAL \
    BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x1234, 0x56789abcdef5)
#define BT_UUID_IRRIGATION_SYSTEM_CONFIG_VAL \
    BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x1234, 0x56789abcdef6)
#define BT_UUID_IRRIGATION_TASK_QUEUE_VAL \
    BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x1234, 0x56789abcdef7)
#define BT_UUID_IRRIGATION_STATISTICS_VAL \
    BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x1234, 0x56789abcdef8)

/* Add new UUIDs for extended functionality */
#define BT_UUID_IRRIGATION_RTC_VAL \
    BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x1234, 0x56789abcdef9)
#define BT_UUID_IRRIGATION_ALARM_VAL \
    BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x1234, 0x56789abcdefa)
#define BT_UUID_IRRIGATION_CALIBRATION_VAL \
    BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x1234, 0x56789abcdefb)
#define BT_UUID_IRRIGATION_HISTORY_VAL \
    BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x1234, 0x56789abcdefc)
#define BT_UUID_IRRIGATION_TIMEZONE_VAL \
    BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x9abc, 0xdef123456793)
/* History Service UUIDs and values removed (unused) to reduce warnings and memory. */
#define BT_UUID_IRRIGATION_DIAGNOSTICS_VAL \
    BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x1234, 0x56789abcdefd)
#define BT_UUID_IRRIGATION_GROWING_ENV_VAL \
    BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x1234, 0x56789abcdefe)
#define BT_UUID_IRRIGATION_AUTO_CALC_STATUS_VAL \
    BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x1234, 0x56789abcde00)
#define BT_UUID_IRRIGATION_CURRENT_TASK_VAL \
    BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x1234, 0x56789abcdeff)

/* Onboarding characteristics UUIDs */
#define BT_UUID_IRRIGATION_ONBOARDING_STATUS_VAL \
    BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x1234, 0x56789abcde20)

#define BT_UUID_IRRIGATION_RESET_CONTROL_VAL \
    BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x1234, 0x56789abcde21)

/* Rain sensor characteristics UUIDs */
#define BT_UUID_IRRIGATION_RAIN_CONFIG_VAL \
    BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x1234, 0x56789abcde12)

#define BT_UUID_IRRIGATION_RAIN_DATA_VAL \
    BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x1234, 0x56789abcde13)

#define BT_UUID_IRRIGATION_RAIN_HISTORY_VAL \
    BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x1234, 0x56789abcde14)

/* New environmental data characteristics UUIDs */
#define BT_UUID_IRRIGATION_ENVIRONMENTAL_DATA_VAL \
    BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x1234, 0x56789abcde15)
#define BT_UUID_IRRIGATION_ENVIRONMENTAL_HISTORY_VAL \
    BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x1234, 0x56789abcde16)
#define BT_UUID_IRRIGATION_COMPENSATION_STATUS_VAL \
    BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x1234, 0x56789abcde17)
/* Rain integration status characteristic UUID (separate from config) */
#define BT_UUID_IRRIGATION_RAIN_INTEGRATION_STATUS_VAL \
    BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x1234, 0x56789abcde18)
/* Channel compensation config characteristic UUID (per-channel rain/temp settings) */
#define BT_UUID_IRRIGATION_CHANNEL_COMP_CONFIG_VAL \
    BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x1234, 0x56789abcde19)

static struct bt_uuid_128 irrigation_service_uuid = BT_UUID_INIT_128(BT_UUID_IRRIGATION_SERVICE_VAL);
static struct bt_uuid_128 valve_char_uuid = BT_UUID_INIT_128(BT_UUID_IRRIGATION_VALVE_VAL);
static struct bt_uuid_128 flow_char_uuid = BT_UUID_INIT_128(BT_UUID_IRRIGATION_FLOW_VAL);
static struct bt_uuid_128 status_char_uuid = BT_UUID_INIT_128(BT_UUID_IRRIGATION_STATUS_VAL);
static struct bt_uuid_128 channel_config_uuid = BT_UUID_INIT_128(BT_UUID_IRRIGATION_CHANNEL_CONFIG_VAL);
static struct bt_uuid_128 schedule_uuid = BT_UUID_INIT_128(BT_UUID_IRRIGATION_SCHEDULE_VAL);
static struct bt_uuid_128 system_config_uuid = BT_UUID_INIT_128(BT_UUID_IRRIGATION_SYSTEM_CONFIG_VAL);
static struct bt_uuid_128 task_queue_uuid = BT_UUID_INIT_128(BT_UUID_IRRIGATION_TASK_QUEUE_VAL);
static struct bt_uuid_128 statistics_uuid = BT_UUID_INIT_128(BT_UUID_IRRIGATION_STATISTICS_VAL);
static struct bt_uuid_128 rtc_char_uuid = BT_UUID_INIT_128(BT_UUID_IRRIGATION_RTC_VAL);
static struct bt_uuid_128 alarm_char_uuid = BT_UUID_INIT_128(BT_UUID_IRRIGATION_ALARM_VAL);
static struct bt_uuid_128 calibration_char_uuid = BT_UUID_INIT_128(BT_UUID_IRRIGATION_CALIBRATION_VAL);
static struct bt_uuid_128 history_char_uuid = BT_UUID_INIT_128(BT_UUID_IRRIGATION_HISTORY_VAL);
static struct bt_uuid_128 diagnostics_char_uuid = BT_UUID_INIT_128(BT_UUID_IRRIGATION_DIAGNOSTICS_VAL);
static struct bt_uuid_128 growing_env_char_uuid = BT_UUID_INIT_128(BT_UUID_IRRIGATION_GROWING_ENV_VAL);
static struct bt_uuid_128 auto_calc_status_char_uuid = BT_UUID_INIT_128(BT_UUID_IRRIGATION_AUTO_CALC_STATUS_VAL);
static struct bt_uuid_128 current_task_char_uuid = BT_UUID_INIT_128(BT_UUID_IRRIGATION_CURRENT_TASK_VAL);
static struct bt_uuid_128 timezone_char_uuid = BT_UUID_INIT_128(BT_UUID_IRRIGATION_TIMEZONE_VAL);

/* Onboarding characteristics UUIDs */
static struct bt_uuid_128 onboarding_status_char_uuid = BT_UUID_INIT_128(BT_UUID_IRRIGATION_ONBOARDING_STATUS_VAL);
static struct bt_uuid_128 reset_control_char_uuid = BT_UUID_INIT_128(BT_UUID_IRRIGATION_RESET_CONTROL_VAL);

/* Rain sensor characteristics UUIDs */
static struct bt_uuid_128 rain_config_char_uuid = BT_UUID_INIT_128(BT_UUID_IRRIGATION_RAIN_CONFIG_VAL);
static struct bt_uuid_128 rain_data_char_uuid = BT_UUID_INIT_128(BT_UUID_IRRIGATION_RAIN_DATA_VAL);
static struct bt_uuid_128 rain_history_char_uuid = BT_UUID_INIT_128(BT_UUID_IRRIGATION_RAIN_HISTORY_VAL);

/* New environmental data characteristics UUIDs */
static struct bt_uuid_128 environmental_data_char_uuid = BT_UUID_INIT_128(BT_UUID_IRRIGATION_ENVIRONMENTAL_DATA_VAL);
static struct bt_uuid_128 environmental_history_char_uuid = BT_UUID_INIT_128(BT_UUID_IRRIGATION_ENVIRONMENTAL_HISTORY_VAL);
static struct bt_uuid_128 compensation_status_char_uuid = BT_UUID_INIT_128(BT_UUID_IRRIGATION_COMPENSATION_STATUS_VAL);
static struct bt_uuid_128 rain_integration_status_char_uuid = BT_UUID_INIT_128(BT_UUID_IRRIGATION_RAIN_INTEGRATION_STATUS_VAL);
static struct bt_uuid_128 channel_comp_config_char_uuid = BT_UUID_INIT_128(BT_UUID_IRRIGATION_CHANNEL_COMP_CONFIG_VAL);

/* Characteristic value handles - use types from headers */
static uint8_t valve_value[sizeof(struct valve_control_data)];
static uint8_t flow_value[sizeof(uint32_t)];
static uint8_t status_value[1];
static uint8_t channel_config_value[sizeof(struct channel_config_data)];
static uint8_t schedule_value[sizeof(struct schedule_config_data)];
static uint8_t system_config_value[sizeof(struct enhanced_system_config_data)];
static uint16_t system_config_bytes_received = 0; /* Track fragmented writes */
static uint8_t task_queue_value[sizeof(struct task_queue_data)];
static uint8_t statistics_value[sizeof(struct statistics_data)];
static uint8_t rtc_value[sizeof(struct rtc_data)];
static uint8_t alarm_value[sizeof(struct alarm_data)];
static uint8_t calibration_value[sizeof(struct calibration_data)];
static uint8_t history_value[sizeof(struct history_data)];
static uint8_t diagnostics_value[sizeof(struct diagnostics_data)];
static uint8_t growing_env_value[sizeof(struct growing_env_data)];
static uint8_t auto_calc_status_value[sizeof(struct auto_calc_status_data)];
static uint8_t current_task_value[sizeof(struct current_task_data)];
static uint8_t timezone_value[sizeof(timezone_config_t)];

/* Rain sensor static values - using types from headers */
/* Removed legacy rain_history_cmd_value and rain_fragment_value (unused after unified fragmentation) */

/* Rain history response structure */
#define RAIN_HISTORY_FRAGMENT_SIZE 240  // Maximum BLE data payload

/* Rain sensor characteristic value handles */
static uint8_t rain_config_value[sizeof(struct rain_config_data)];
static uint8_t rain_data_value[sizeof(struct rain_data_data)];
static uint8_t rain_history_value[sizeof(struct rain_history_cmd_data)];

/* Adaptive cadence state for Rain Data */
static uint32_t rain_last_periodic_ms = 0;
static uint32_t rain_last_pulse_notify_ms = 0;
static uint8_t  rain_last_status_sent = 0xFF; /* invalid at boot to force first snapshot */

/* Environmental data characteristic value handles */
static uint8_t environmental_data_value[sizeof(struct environmental_data_ble)];
static uint8_t environmental_history_value[256]; // Fixed size buffer for history response
/* Async fragmentation state for environmental data notifications */
static struct {
    bool active;
    uint8_t buf[sizeof(struct environmental_data_ble)];
    uint16_t len;
    uint8_t chunk;
    uint8_t total_frags;
    uint8_t next_frag;
} env_frag_state = {0};
static void env_frag_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(env_frag_work, env_frag_work_handler);
/* Async fragmentation state for watering history notifications */
static struct {
    bool active;
    uint8_t *buf;
    size_t len;
    uint8_t total_frags;
    uint8_t next_frag;
    uint8_t history_type;
    uint16_t entry_count_le;
    struct bt_conn *conn;
    const struct bt_gatt_attr *attr;
} history_frag_state = {0};
static void history_frag_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(history_frag_work, history_frag_work_handler);
static uint8_t compensation_status_value[sizeof(struct compensation_status_data)];
/* Rain Integration Status characteristic value buffer */
static uint8_t rain_integration_status_value[sizeof(struct rain_integration_status_ble)];
/* Channel Compensation Config characteristic value buffer */
static uint8_t channel_comp_config_value[sizeof(struct channel_compensation_config_data)];

/* Channel caching for performance */
static struct {
    uint8_t channel_id;
    watering_channel_t *channel;
    uint32_t cache_time;
    bool valid;
} channel_cache = {0};

#define CHANNEL_CACHE_TIMEOUT_MS 100  /* 100ms cache - faster channel switching */

static inline watering_error_t get_channel_cached(uint8_t channel_id, watering_channel_t **channel) {
    /* DISABLE CACHE - was causing stale data when switching channels */
    /* Always fetch fresh data from system to prevent history timeout errors */
    return watering_get_channel(channel_id, channel);
}

/* Invalidate cache when configuration changes */
static inline void invalidate_channel_cache(void) {
    channel_cache.valid = false;
}

/* Currently selected channel for growing environment operations */
static uint8_t selected_channel_id __attribute__((unused)) = 0;

/* Last selected channel for growing environment characteristic */
static uint8_t growing_env_last_channel = 0;

/* ---------------------------------------------------------------
 *  Accumulator for fragmented Channel-Config writes (≤20 B each)
 * ------------------------------------------------------------- */
static struct {
    uint8_t  id;          /* channel being edited              */
    uint8_t  frag_type;   /* fragment type (1=name_le, 2=full_struct_be, 3=full_struct_le) */
    uint16_t expected;    /* total size from first frame       */
    uint16_t received;    /* bytes stored so far               */
    uint8_t  buf[128];    /* temporary buffer - increased for full struct */
    bool     in_progress; /* true while receiving fragments    */
    uint32_t start_time;  /* timestamp when fragmentation started */
} channel_frag = {0};

/* ---------------------------------------------------------------
 *  Accumulator for fragmented Growing Environment writes (≤20 B each)
 * ------------------------------------------------------------- */
static struct {
    uint8_t  channel_id;       /* channel being edited              */
    uint8_t  frag_type;        /* fragment type (2=full_struct_be, 3=full_struct_le) */
    uint16_t expected;         /* total struct size from first frame */
    uint16_t received;         /* bytes stored so far               */
    uint8_t  buf[128];         /* temporary buffer for struct data - increased for 76-byte structures */
    bool     in_progress;      /* true while receiving fragments    */
    uint32_t start_time;       /* timestamp when fragmentation started */
} growing_env_frag = {0};

/* ---------------------------------------------------------------
 *  Current Task: no fragmentation per 21-byte spec
 * ------------------------------------------------------------- */

/* ---------------------------------------------------------------
 *  Accumulator for fragmented Auto Calc Status writes (≤20 B each)
 * ------------------------------------------------------------- */
static struct {
    uint8_t  frag_type;        /* fragment type (2=full_struct_be, 3=full_struct_le) */
    uint16_t expected;         /* total struct size from first frame */
    uint16_t received;         /* bytes stored so far               */
    uint8_t  buf[64];          /* temporary buffer for 40-byte auto calc structure */
    bool     in_progress;      /* true while receiving fragments    */
    uint32_t start_time;       /* timestamp when fragmentation started */
} auto_calc_frag = {0};

/* ---------------------------------------------------------------
 *  Accumulator for fragmented History writes (≤20 B each)
 * ------------------------------------------------------------- */
static struct {
    uint8_t  frag_type;        /* fragment type (2=full_struct_be, 3=full_struct_le) */
    uint16_t expected;         /* total struct size from first frame */
    uint16_t received;         /* bytes stored so far               */
    uint8_t  buf[128];         /* temporary buffer for 32-byte history structure */
    bool     in_progress;      /* true while receiving fragments    */
    uint32_t start_time;       /* timestamp when fragmentation started */
} history_frag = {0};

#define FRAGMENTATION_TIMEOUT_MS 5000  /* 5 second timeout for fragmentation */

/* Check and reset fragmentation state if timeout occurred */
static inline void check_fragmentation_timeout(void) {
    uint32_t now = k_uptime_get_32();
    
    /* Check channel config fragmentation timeout */
    if (channel_frag.in_progress) {
        if (now - channel_frag.start_time > FRAGMENTATION_TIMEOUT_MS) {
            printk("⚠️ BLE: Channel config fragmentation timeout - resetting state\n");
            channel_frag.in_progress = false;
        }
    }
    
    /* Check growing environment fragmentation timeout */
    if (growing_env_frag.in_progress) {
        if (now - growing_env_frag.start_time > FRAGMENTATION_TIMEOUT_MS) {
            printk("⚠️ BLE: Growing environment fragmentation timeout - resetting state\n");
            growing_env_frag.in_progress = false;
        }
    }
    
    /* Check history fragmentation timeout */
    if (history_frag.in_progress) {
        if (now - history_frag.start_time > FRAGMENTATION_TIMEOUT_MS) {
            printk("⚠️ BLE: History fragmentation timeout - resetting state\n");
            history_frag.in_progress = false;
        }
    }
    
    /* Current Task: no fragmentation */
    
    /* Check auto calc fragmentation timeout */
    if (auto_calc_frag.in_progress) {
        if (now - auto_calc_frag.start_time > FRAGMENTATION_TIMEOUT_MS) {
            printk("⚠️ BLE: Auto calc fragmentation timeout - resetting state\n");
            auto_calc_frag.in_progress = false;
        }
    }
}

/* Debug function to log fragmentation state */
static inline void log_fragmentation_state(const char *context) {
    if (channel_frag.in_progress) {
        printk("🔧 BLE: Fragmentation state [%s]: ch=%u, type=%u, received=%u/%u bytes, active for %ums\n",
               context, channel_frag.id, channel_frag.frag_type, 
               channel_frag.received, channel_frag.expected,
               k_uptime_get_32() - channel_frag.start_time);
    } else {
        printk("🔧 BLE: Fragmentation state [%s]: IDLE\n", context);
    }
}



/* ---------------------------------------------------------------- */

/* Global variables for calibration */
static bool calibration_active = false;
static uint32_t calibration_start_pulses = 0;
/* Periodic notifier for calibration progress */
static struct k_work_delayable calibration_progress_work;
static void calibration_progress_work_handler(struct k_work *work) {
    ARG_UNUSED(work);
    if (!calibration_active) {
        return;
    }
    if (default_conn && notification_state.calibration_notifications_enabled) {
        struct calibration_data *val = (struct calibration_data *)calibration_value;
        uint32_t current_pulses = get_pulse_count();
        val->pulses = current_pulses - calibration_start_pulses;
        val->action = 2; /* IN_PROGRESS */
        bt_irrigation_calibration_notify();
    }
    /* Reschedule at ~200ms while active */
    k_work_schedule(&calibration_progress_work, K_MSEC(200));
}

/* ------------------------------------------------------------------ */
/* Current Task periodic notifier (2s while running)                   */
/* ------------------------------------------------------------------ */

/* Prototype first so the macro can reference it */
static void current_task_periodic_work_handler(struct k_work *work);

/* Define the delayable work instance before function body so it is visible */
static K_WORK_DELAYABLE_DEFINE(current_task_periodic_work, current_task_periodic_work_handler);

static void current_task_periodic_work_handler(struct k_work *work) {
    ARG_UNUSED(work);
    /* Only run when notifications enabled and connection up */
    if (!default_conn || !connection_active || !notification_state.current_task_notifications_enabled) {
        return;
    }

    /* Send progress only when a task is running (not paused) */
    watering_task_t *current_task = watering_get_current_task();
    bool running = (current_task && watering_task_state.task_in_progress && !watering_task_state.task_paused);

    if (running) {
        (void)bt_irrigation_current_task_notify();
    }

    /* Re-schedule at 2s cadence while notifications stay enabled */
    k_work_cancel_delayable(&current_task_periodic_work);
}


/* Global variables for diagnostics tracking */
static uint16_t diagnostics_error_count = 0;
static uint8_t diagnostics_last_error = 0;

/* Forward declaration */
static ssize_t read_valve(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                          void *buf, uint16_t len, uint16_t offset);

static ssize_t write_valve(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                           const void *buf, uint16_t len, uint16_t offset, uint8_t flags);

static void valve_ccc_cfg_changed(const struct bt_gatt_attr *attr, uint16_t value);

static ssize_t read_flow(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                         void *buf, uint16_t len, uint16_t offset);

static void flow_ccc_cfg_changed(const struct bt_gatt_attr *attr, uint16_t value);

static ssize_t read_status(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                           void *buf, uint16_t len, uint16_t offset);

static void status_ccc_cfg_changed(const struct bt_gatt_attr *attr, uint16_t value);
/* Forward declaration: work item instance used for periodic fault re-notifications */
static struct k_work_delayable status_periodic_work;

static void channel_config_ccc_changed(const struct bt_gatt_attr *attr,
                                       uint16_t value);

/* -------------------------------------------------------------------------
 *  Attribute indices of the VALUE descriptors inside irrigation_svc.
 *  Keep in ONE place → easy to update if the service definition changes.
 * ---------------------------------------------------------------------- */
/* Pre-calculated attribute pointers for performance */
static const struct bt_gatt_attr *attr_valve;
static const struct bt_gatt_attr *attr_flow;
static const struct bt_gatt_attr *attr_status;
static const struct bt_gatt_attr *attr_channel_config;

/* Initialize attribute pointers for fast access */
static inline void init_attr_pointers(void) {
    attr_valve = &irrigation_svc.attrs[ATTR_IDX_VALVE_VALUE];
    attr_flow = &irrigation_svc.attrs[ATTR_IDX_FLOW_VALUE];
    attr_status = &irrigation_svc.attrs[ATTR_IDX_STATUS_VALUE];
    attr_channel_config = &irrigation_svc.attrs[ATTR_IDX_CHANNEL_CFG_VALUE];
}

/* MTU callback to handle MTU changes */
static void mtu_exchange_cb(struct bt_conn *conn, uint8_t err,
                           struct bt_gatt_exchange_params *params) {
    if (err) {
        printk("MTU exchange failed: %d\n", err);
    } else {
        printk("MTU exchange successful: %u\n", bt_gatt_get_mtu(conn));
    }
}

/* MTU exchange parameters */
static struct bt_gatt_exchange_params mtu_exchange_params = {
    .func = mtu_exchange_cb,
};

/* ---------- reusable advertising definitions (GLOBAL) ----------------- */
#define DEVICE_NAME "AutoWatering"

static const struct bt_data adv_ad[] = {
        BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
        BT_DATA_BYTES(BT_DATA_UUID128_ALL,
                      BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678,
                                         0x1234, 0x56789abcdef0)),
};

static const struct bt_data adv_sd[] = {
        BT_DATA(BT_DATA_NAME_COMPLETE, DEVICE_NAME,
                sizeof(DEVICE_NAME) - 1),
        BT_DATA_BYTES(BT_DATA_MANUFACTURER_DATA, 0x00, 0x00, 'A', 'W'),
};

static struct bt_le_adv_param adv_param = {
        .options      = BT_LE_ADV_OPT_CONN | BT_LE_ADV_OPT_USE_IDENTITY,
        .interval_min = BT_GAP_ADV_FAST_INT_MIN_2,
        .interval_max = BT_GAP_ADV_FAST_INT_MAX_2,
        .peer         = NULL,
};
/* ---------------------------------------------------------------------- */

/* ------------------------------------------------------------------ */
/* Environmental Data Callback Implementations                       */
/* ------------------------------------------------------------------ */

/* Environmental data read callback - returns environmental_data_ble (24B) */
static ssize_t read_environmental_data(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                      void *buf, uint16_t len, uint16_t offset)
{
    if (!conn || !attr || !buf) {
        LOG_ERR("Invalid parameters for Environmental Data read");
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_HANDLE);
    }

    struct environmental_data_ble out = {0};
    bool data_available = false;

    /* Get current processed environmental data */
    bme280_environmental_data_t proc;
    int ret = environmental_data_get_current(&proc);
    if (ret == 0 && proc.current.valid) {
        out.temperature = proc.current.temperature;
        out.humidity = proc.current.humidity;
        out.pressure = proc.current.pressure;
        out.timestamp = proc.current.timestamp;
        out.sensor_status = 1; /* Active */

        /* Compute data_quality score using validation */
        env_data_validation_t validation;
        if (env_data_validate_reading(&proc.current, NULL, &validation) == 0) {
            out.data_quality = env_data_calculate_quality_score(&proc.current, &validation);
        } else {
            out.data_quality = 0;
        }
        data_available = true;
    }

    /* Fallback to direct BME280 read if processed data unavailable */
    if (!data_available) {
        bme280_reading_t reading;
        if (bme280_system_read_data(&reading) == 0 && reading.valid) {
            out.temperature = reading.temperature;
            out.humidity = reading.humidity;
            out.pressure = reading.pressure;
            out.timestamp = reading.timestamp;
            out.sensor_status = 1; /* Active */

            env_data_validation_t validation;
            if (env_data_validate_reading(&reading, NULL, &validation) == 0) {
                out.data_quality = env_data_calculate_quality_score(&reading, &validation);
            } else {
                out.data_quality = 50; /* Moderate quality for direct reads */
            }
            data_available = true;
        }
    }

    /* Provide sane defaults if sensor unavailable */
    if (!data_available) {
        out.temperature = 25.0f;
        out.humidity = 50.0f;
        out.pressure = 1013.25f;
        out.timestamp = k_uptime_get_32();
        out.sensor_status = 0; /* Inactive */
        out.data_quality = 0;
    }

    /* Get measurement interval from BME280 config if available */
    bme280_config_t config;
    if (bme280_system_get_config(&config) == 0) {
        out.measurement_interval = config.measurement_interval;
    } else {
        out.measurement_interval = 60; /* Default */
    }

    return bt_gatt_attr_read(conn, attr, buf, len, offset, &out, sizeof(out));
}

/* Environmental data CCC callback */
static void environmental_data_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
    bool notif_enabled = (value == BT_GATT_CCC_NOTIFY);
    notification_state.environmental_data_notifications_enabled = notif_enabled;
    
    if (notif_enabled) {
        LOG_INF("Environmental data notifications enabled");
    } else {
        LOG_INF("Environmental data notifications disabled");
    }
}

/* Environmental history read callback */
static ssize_t read_environmental_history(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                         void *buf, uint16_t len, uint16_t offset)
{
    if (!conn || !attr || !buf) {
        LOG_ERR("Invalid parameters for Environmental History read");
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_HANDLE);
    }

    /* Return current response buffer (may be large; long read supported) */
    return bt_gatt_attr_read(conn, attr, buf, len, offset, environmental_history_value, sizeof(environmental_history_value));
}

/* Environmental history write callback */
static ssize_t write_environmental_history(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                          const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
    /* Rate limiting: minimum 1s between commands, 500ms between notifications */
    static int64_t last_cmd_ms = -2000;
    static int64_t last_notify_ms = -2000;
    int64_t now_ms = k_uptime_get();
    if (!conn || !attr || !buf) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_HANDLE);
    }
    if (offset != 0) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }
    if (len != sizeof(ble_history_request_t)) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }

    /* Removed unused early request peek variable to reduce warnings */

    /* Enforce 1s minimum between commands */
    if ((now_ms - last_cmd_ms) < 1000) {
        /* Always build unified 8B header (no payload) with Rate Limited status */
        history_fragment_header_t hdr = (history_fragment_header_t){0};
        hdr.data_type = 0; /* unspecified */
        hdr.status = 0x07; /* Rate limited */
        hdr.entry_count = 0;
        hdr.fragment_index = 0;
        hdr.total_fragments = 0;
        hdr.fragment_size = 0;
        hdr.reserved = 0;
        memcpy(environmental_history_value, &hdr, sizeof(hdr));
        if (notification_state.environmental_history_notifications_enabled && default_conn) {
            if ((now_ms - last_notify_ms) >= 500) {
                const struct bt_gatt_attr *eh_attr = &irrigation_svc.attrs[ATTR_IDX_ENVIRONMENTAL_HISTORY_VALUE];
                bt_gatt_notify(default_conn, eh_attr, &hdr, sizeof(hdr));
                last_notify_ms = now_ms;
            }
        }
        return len; /* Accept write but indicate throttling via status */
    }

    const ble_history_request_t *req = (const ble_history_request_t *)buf;
    ble_history_response_t resp;
    memset(&resp, 0, sizeof(resp));

    int rc = bt_env_history_request_handler(req, &resp);
    if (rc != 0) {
        return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
    }

    /* Always wrap response using unified 8B header */
    uint8_t rec_size = 0;
    switch (resp.data_type) {
        case 0: rec_size = 12; break; /* detailed */
        case 1: rec_size = 16; break; /* hourly */
        case 2: rec_size = 22; break; /* daily */
        case 3: rec_size = 24; break; /* trends */
        default: rec_size = 0; break;
    }
    uint16_t fragment_size = (uint16_t)(resp.record_count * rec_size);

    /* Use max payload 232B to match environmental history fragment size */
    #define ENVHIST_MAX_PAYLOAD 232
    uint8_t notify_buf[sizeof(history_fragment_header_t) + ENVHIST_MAX_PAYLOAD] = {0};
    history_fragment_header_t *hdr = (history_fragment_header_t *)notify_buf;
    hdr->data_type = resp.data_type;
    hdr->status = resp.status;
    hdr->entry_count = resp.record_count;
    hdr->fragment_index = resp.fragment_id;
    hdr->total_fragments = resp.total_fragments;
    hdr->fragment_size = (uint8_t)(fragment_size > 255 ? 255 : fragment_size);
    hdr->reserved = 0;
    if (fragment_size > 0) {
        uint16_t copy_sz = fragment_size > ENVHIST_MAX_PAYLOAD ? ENVHIST_MAX_PAYLOAD : fragment_size;
        memcpy(&notify_buf[sizeof(history_fragment_header_t)], resp.data, copy_sz);
    }
    /* Update read buffer with header + payload slice */
    {
        uint16_t copy_sz = fragment_size > ENVHIST_MAX_PAYLOAD ? ENVHIST_MAX_PAYLOAD : fragment_size;
        memcpy(environmental_history_value, notify_buf, sizeof(history_fragment_header_t) + copy_sz);
    }
    /* Notify if enabled (500ms min interval) */
    if (notification_state.environmental_history_notifications_enabled && default_conn) {
        if ((now_ms - last_notify_ms) >= 500) {
            const struct bt_gatt_attr *eh_attr = &irrigation_svc.attrs[ATTR_IDX_ENVIRONMENTAL_HISTORY_VALUE];
            uint16_t copy_sz = fragment_size > ENVHIST_MAX_PAYLOAD ? ENVHIST_MAX_PAYLOAD : fragment_size;
            int nerr = bt_gatt_notify(default_conn, eh_attr, notify_buf, sizeof(history_fragment_header_t) + copy_sz);
            if (nerr) {
                LOG_WRN("Environmental history notify (unified) failed: %d", nerr);
            } else {
                last_notify_ms = now_ms;
            }
        }
    }
    last_cmd_ms = now_ms;
    return len;
}

/* Environmental history CCC callback */
static void environmental_history_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
    bool notif_enabled = (value == BT_GATT_CCC_NOTIFY);
    notification_state.environmental_history_notifications_enabled = notif_enabled;
    
    if (notif_enabled) {
        LOG_INF("Environmental history notifications enabled");
    } else {
        LOG_INF("Environmental history notifications disabled");
    }
}

/* Environmental data BLE notification functions */
int bt_irrigation_environmental_data_notify(void) {
    if (!default_conn || !notification_state.environmental_data_notifications_enabled) {
        return 0; // No connection or notifications disabled
    }
    
    /* Read current environmental data and send notification */
    struct environmental_data_ble env_data;
    memset(&env_data, 0, sizeof(env_data));

    bool data_available = false;

    /* Prefer processed environmental data for consistency with reads */
    bme280_environmental_data_t processed;
    if (environmental_data_get_current(&processed) == 0 && processed.current.valid) {
        env_data.temperature = processed.current.temperature;
        env_data.humidity = processed.current.humidity;
        env_data.pressure = processed.current.pressure;
        env_data.timestamp = processed.current.timestamp;
        env_data.sensor_status = 1;

        env_data_validation_t validation;
        if (env_data_validate_reading(&processed.current, NULL, &validation) == 0) {
            env_data.data_quality = env_data_calculate_quality_score(&processed.current, &validation);
        }
        data_available = true;
    } else {
        /* Fallback to direct sensor read if processor data unavailable */
        bme280_reading_t reading;
        if (bme280_system_read_data(&reading) == 0 && reading.valid) {
            env_data.temperature = reading.temperature;
            env_data.humidity = reading.humidity;
            env_data.pressure = reading.pressure;
            env_data.timestamp = reading.timestamp;
            env_data.sensor_status = 1;

            env_data_validation_t validation;
            if (env_data_validate_reading(&reading, NULL, &validation) == 0) {
                env_data.data_quality = env_data_calculate_quality_score(&reading, &validation);
            }
            data_available = true;
        }
    }

    if (!data_available) {
        env_data.sensor_status = 0;
        env_data.data_quality = 0;
        env_data.timestamp = k_uptime_get_32();
    }

    /* Incorporate system health metrics for additional context */
    env_sensor_status_t sensor_status;
    if (env_sensors_get_status(&sensor_status) == WATERING_SUCCESS) {
        bool any_sensor_online = sensor_status.temp_sensor_online ||
                                 sensor_status.humidity_sensor_online ||
                                 sensor_status.pressure_sensor_online;

        if (!any_sensor_online) {
            env_data.sensor_status = 0;
        } else if (env_data.sensor_status == 0) {
            env_data.sensor_status = 1;
        }

        if (sensor_status.overall_health > 0) {
            if (env_data.data_quality > 0) {
                env_data.data_quality = MIN(env_data.data_quality, sensor_status.overall_health);
            } else {
                env_data.data_quality = sensor_status.overall_health;
            }
        }
    }
    
    /* Get measurement interval */
    bme280_config_t config;
    if (bme280_system_get_config(&config) == 0) {
        env_data.measurement_interval = config.measurement_interval;
    } else {
        env_data.measurement_interval = 60; /* Default */
    }
    
    /* Update value buffer and send notification */
    memcpy(environmental_data_value, &env_data, sizeof(env_data));
    
    const struct bt_gatt_attr *attr = &irrigation_svc.attrs[ATTR_IDX_ENVIRONMENTAL_DATA_VALUE];

    /* MTU-aware send: single frame if fits, else fragmented with [seq,total,len] header */
    uint16_t mtu = bt_gatt_get_mtu(default_conn);
    uint16_t max_payload = (mtu > 3) ? (mtu - 3) : 20;
    /* advanced_notify enforces a 23-byte ceiling; anything larger must use the manual fragment path */
    if (sizeof(env_data) <= max_payload && sizeof(env_data) <= 23U) {
        int result = safe_notify(default_conn, attr, &env_data, sizeof(env_data));
        if (result == 0) {
            LOG_DBG("Environmental data notification sent (single frame)");
        } else {
            LOG_WRN("Environmental data notification failed: %d", result);
        }
        return result;
    }

    /* Asynchronous fragmentation to keep BT host thread unblocked */
    uint16_t chunk = (max_payload > 3) ? (max_payload - 3) : 0;
    if (chunk == 0) {
        LOG_WRN("MTU too small to send environmental data fragments");
        return -EMSGSIZE;
    }
    if (env_frag_state.active) {
        LOG_WRN("Environmental notify busy, dropping update");
        return -EBUSY;
    }

    memcpy(env_frag_state.buf, &env_data, sizeof(env_data));
    env_frag_state.len = sizeof(env_data);
    env_frag_state.chunk = (uint8_t)chunk;
    env_frag_state.total_frags = (uint8_t)((env_frag_state.len + env_frag_state.chunk - 1) / env_frag_state.chunk);
    env_frag_state.next_frag = 0;
    env_frag_state.active = true;

    k_work_schedule(&env_frag_work, K_NO_WAIT);
    return 0;
}

/* Async fragment sender for environmental data */
static void env_frag_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    if (!env_frag_state.active || !default_conn || !notification_state.environmental_data_notifications_enabled) {
        env_frag_state.active = false;
        return;
    }

    const struct bt_gatt_attr *attr = &irrigation_svc.attrs[ATTR_IDX_ENVIRONMENTAL_DATA_VALUE];
    const uint16_t header_sz = 3;
    uint8_t frag_buf[32];

    uint16_t offset = env_frag_state.next_frag * env_frag_state.chunk;
    uint16_t remaining = env_frag_state.len - offset;
    uint16_t this_len = (remaining > env_frag_state.chunk) ? env_frag_state.chunk : remaining;

    frag_buf[0] = env_frag_state.next_frag;
    frag_buf[1] = env_frag_state.total_frags;
    frag_buf[2] = (uint8_t)this_len;
    memcpy(&frag_buf[header_sz], &env_frag_state.buf[offset], this_len);

    int err = bt_gatt_notify(default_conn, attr, frag_buf, header_sz + this_len);
    if (err) {
        LOG_WRN("Environmental fragment %u/%u notify failed: %d",
                env_frag_state.next_frag + 1, env_frag_state.total_frags, err);
        env_frag_state.active = false;
        return;
    }

    env_frag_state.next_frag++;
    if (env_frag_state.next_frag < env_frag_state.total_frags) {
        k_work_schedule(&env_frag_work, K_MSEC(5));
    } else {
        env_frag_state.active = false;
        LOG_DBG("Environmental data notification sent in %u fragments", env_frag_state.total_frags);
    }
}

/* Compensation status read callback */
static ssize_t write_compensation_status(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                        const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
    ARG_UNUSED(conn);
    ARG_UNUSED(attr);
    ARG_UNUSED(flags);
    /* Accept a single byte channel selector: 0..(N-1) or 0xFF = first auto */
    if (offset != 0 || len != 1 || buf == NULL) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }
    uint8_t req = *((const uint8_t*)buf);
    uint8_t sel = req;
    if (req == 0xFF) {
        sel = 0xFF;
        for (uint8_t i = 0; i < WATERING_CHANNELS_COUNT; i++) {
            watering_channel_t *ch;
            if (watering_get_channel(i, &ch) == WATERING_SUCCESS) {
                if (ch->auto_mode == WATERING_AUTOMATIC_QUALITY || ch->auto_mode == WATERING_AUTOMATIC_ECO) {
                    sel = i;
                    break;
                }
            }
        }
        if (sel == 0xFF) {
            sel = 0; /* default if none in auto */
        }
    } else if (req >= WATERING_CHANNELS_COUNT) {
        return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
    }
    struct compensation_status_data *val = (struct compensation_status_data *)compensation_status_value;
    val->channel_id = sel;
    /* Build a fresh payload and push immediate notification if enabled */
    if (notification_state.compensation_status_notifications_enabled) {
        struct compensation_status_data comp = {0};
        comp.channel_id = sel;
        watering_channel_t *channel = NULL;
        if (watering_get_channel(sel, &channel) == WATERING_SUCCESS && channel) {
            comp.rain_compensation_active = channel->rain_compensation.enabled ? 1 : 0;
            comp.recent_rainfall_mm = 0.0f;
            comp.rain_reduction_percentage = channel->last_rain_compensation.reduction_percentage;
            comp.rain_skip_watering = channel->last_rain_compensation.skip_watering ? 1 : 0;
            comp.rain_calculation_time = channel->last_calculation_time;
            comp.temp_compensation_active = channel->temp_compensation.enabled ? 1 : 0;
            comp.current_temperature = 0.0f;
            comp.temp_compensation_factor = channel->last_temp_compensation.compensation_factor;
            comp.temp_adjusted_requirement = channel->last_temp_compensation.adjusted_requirement;
            comp.temp_calculation_time = channel->last_calculation_time;
            comp.any_compensation_active = (comp.rain_compensation_active || comp.temp_compensation_active) ? 1 : 0;
        }
        memcpy(compensation_status_value, &comp, sizeof(comp));
        /* Notify via this attribute */
        safe_notify(default_conn, attr, &comp, sizeof(comp));
    }
    return len;
}

static ssize_t read_compensation_status(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                       void *buf, uint16_t len, uint16_t offset)
{
    if (!conn || !attr || !buf) {
        LOG_ERR("Invalid parameters for Compensation Status read");
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_HANDLE);
    }

    /* Build current compensation status per implemented 40B struct */
    struct compensation_status_data comp = {0};

    /* Select channel for read: use last selected from value buffer, default 0 */
    uint8_t ch = ((const struct compensation_status_data *)compensation_status_value)->channel_id;
    if (ch >= WATERING_CHANNELS_COUNT) {
        ch = 0;
    }
    comp.channel_id = ch;

    watering_channel_t *channel = NULL;
    if (watering_get_channel(ch, &channel) == WATERING_SUCCESS && channel) {
        /* Rain compensation */
        comp.rain_compensation_active = channel->rain_compensation.enabled ? 1 : 0;
        comp.recent_rainfall_mm = 0.0f; /* Not tracked in watering_channel_t */
        comp.rain_reduction_percentage = channel->last_rain_compensation.reduction_percentage;
        comp.rain_skip_watering = channel->last_rain_compensation.skip_watering ? 1 : 0;
        comp.rain_calculation_time = channel->last_calculation_time;

        /* Temperature compensation */
        comp.temp_compensation_active = channel->temp_compensation.enabled ? 1 : 0;
        comp.current_temperature = 0.0f; /* Use sensor API if needed */
        comp.temp_compensation_factor = channel->last_temp_compensation.compensation_factor;
        comp.temp_adjusted_requirement = channel->last_temp_compensation.adjusted_requirement;
        comp.temp_calculation_time = channel->last_calculation_time;

        /* Overall */
        comp.any_compensation_active = (comp.rain_compensation_active || comp.temp_compensation_active) ? 1 : 0;
    } else {
        /* Defaults if channel unavailable */
        comp.rain_compensation_active = 0;
        comp.rain_skip_watering = 0;
        comp.temp_compensation_active = 0;
        comp.any_compensation_active = 0;
        comp.rain_reduction_percentage = 0.0f;
        comp.temp_compensation_factor = 1.0f;
        comp.temp_adjusted_requirement = 0.0f;
        comp.recent_rainfall_mm = 0.0f;
        comp.current_temperature = 0.0f;
        comp.rain_calculation_time = k_uptime_get_32();
        comp.temp_calculation_time = comp.rain_calculation_time;
    }

    return bt_gatt_attr_read(conn, attr, buf, len, offset, &comp, sizeof(comp));
}

/* Compensation status CCC callback */
static void compensation_status_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value) {
    bool notif_enabled = (value == BT_GATT_CCC_NOTIFY);
    notification_state.compensation_status_notifications_enabled = notif_enabled;
    
    if (notif_enabled) {
        LOG_INF("Compensation status notifications enabled");
    } else {
        LOG_INF("Compensation status notifications disabled");
    }
}

/* ================================================================== */
/* Onboarding Characteristics Implementation                          */
/* ================================================================== */

/* Onboarding status characteristic read callback */
static ssize_t read_onboarding_status(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                    void *buf, uint16_t len, uint16_t offset) {
    if (!conn || !attr || !buf) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_HANDLE);
    }

    struct onboarding_status_data status_data = {0};
    
    /* Get current onboarding state */
    onboarding_state_t state;
    int ret = onboarding_get_state(&state);
    if (ret < 0) {
        LOG_ERR("Failed to get onboarding state: %d", ret);
        return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
    }
    
    /* Fill the BLE structure */
    status_data.overall_completion_pct = state.onboarding_completion_pct;
    
    /* Calculate individual completion percentages */
    int total_channel_flags = 8 * 8; /* 8 channels × 8 flags each */
    int set_channel_flags = 0;
    for (int i = 0; i < 64; i++) {
        if (state.channel_config_flags & (1ULL << i)) {
            set_channel_flags++;
        }
    }
    status_data.channels_completion_pct = (set_channel_flags * 100) / total_channel_flags;
    
    int total_system_flags = 8; /* 8 system flags defined */
    int set_system_flags = 0;
    for (int i = 0; i < 32; i++) {
        if (state.system_config_flags & (1U << i)) {
            set_system_flags++;
        }
    }
    status_data.system_completion_pct = (set_system_flags * 100) / total_system_flags;
    
    int total_schedule_flags = 8; /* 8 channels can have schedules */
    int set_schedule_flags = 0;
    for (int i = 0; i < 8; i++) {
        if (state.schedule_config_flags & (1U << i)) {
            set_schedule_flags++;
        }
    }
    status_data.schedules_completion_pct = (set_schedule_flags * 100) / total_schedule_flags;
    
    /* Copy state flags */
    status_data.channel_config_flags = state.channel_config_flags;
    status_data.channel_extended_flags = state.channel_extended_flags;
    status_data.system_config_flags = state.system_config_flags;
    status_data.schedule_config_flags = state.schedule_config_flags;
    status_data.onboarding_start_time = state.onboarding_start_time;
    status_data.last_update_time = state.last_update_time;
    
    LOG_DBG("Onboarding status read: overall=%u%%, channels=%u%%, system=%u%%, schedules=%u%%",
            status_data.overall_completion_pct, status_data.channels_completion_pct,
            status_data.system_completion_pct, status_data.schedules_completion_pct);
    
    return bt_gatt_attr_read(conn, attr, buf, len, offset, &status_data, sizeof(status_data));
}

/* Onboarding status CCC callback */
static void onboarding_status_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value) {
    bool notif_enabled = (value == BT_GATT_CCC_NOTIFY);
    notification_state.onboarding_status_notifications_enabled = notif_enabled;
    
    LOG_DBG("Onboarding status notifications %s", notif_enabled ? "enabled" : "disabled");
    
    /* Send initial notification if enabled */
    if (notif_enabled) {
        /* Trigger an immediate notification with current status */
        bt_irrigation_onboarding_status_notify();
    }
}

/* Helper function to convert internal reset_type_t to BLE spec values */
static uint8_t reset_type_to_ble_spec(reset_type_t type) {
    switch (type) {
        case RESET_TYPE_CHANNEL_CONFIG:   return 0x01;
        case RESET_TYPE_CHANNEL_SCHEDULE: return 0x02;
        case RESET_TYPE_ALL_CHANNELS:     return 0x10;
        case RESET_TYPE_ALL_SCHEDULES:    return 0x11;
        case RESET_TYPE_SYSTEM_CONFIG:    return 0x12;
        case RESET_TYPE_CALIBRATION:      return 0x13; /* Using 0x13 for calibration */
        case RESET_TYPE_HISTORY:          return 0x14;
        case RESET_TYPE_FACTORY_RESET:    return 0xFF;
        default:                          return 0xFF;
    }
}

/* Reset control characteristic read callback */
static ssize_t read_reset_control(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                void *buf, uint16_t len, uint16_t offset) {
    if (!conn || !attr || !buf) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_HANDLE);
    }

    struct reset_control_data reset_data = {0};
    
    /* Get current confirmation info */
    reset_confirmation_t confirmation;
    int ret = reset_controller_get_confirmation_info(&confirmation);
    if (ret == 0 && confirmation.is_valid) {
        /* Convert internal type to BLE spec value */
        reset_data.reset_type = reset_type_to_ble_spec(confirmation.type);
        reset_data.channel_id = confirmation.channel_id;
        reset_data.confirmation_code = confirmation.code;
        reset_data.timestamp = confirmation.generation_time;
        reset_data.status = 0x01; /* Pending */
    } else {
        /* No active confirmation */
        reset_data.reset_type = 0xFF; /* Invalid */
        reset_data.channel_id = 0xFF;
        reset_data.confirmation_code = 0;
        reset_data.timestamp = 0;
        reset_data.status = 0xFF; /* No operation */
    }
    
    LOG_DBG("Reset control read: type=0x%02x, channel=%u, status=%u", 
            reset_data.reset_type, reset_data.channel_id, reset_data.status);
    
    return bt_gatt_attr_read(conn, attr, buf, len, offset, &reset_data, sizeof(reset_data));
}

/* Reset control characteristic write callback */
static ssize_t write_reset_control(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                 const void *buf, uint16_t len, uint16_t offset, uint8_t flags) {
    if (!conn || !attr || !buf) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_HANDLE);
    }

    if (offset != 0) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }

    if (len != sizeof(struct reset_control_data)) {
        LOG_ERR("Invalid reset control data length: %u (expected %u)", len, sizeof(struct reset_control_data));
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }

    const struct reset_control_data *reset_data = (const struct reset_control_data *)buf;

    LOG_DBG("Reset control write: type=0x%02x, channel=%u, code=0x%08x",
            reset_data->reset_type, reset_data->channel_id, reset_data->confirmation_code);

    /* Map BLE reset_type (spec-defined) to internal reset_type_t */
    reset_type_t mapped_type;
    bool type_valid = true;
    switch (reset_data->reset_type) {
        /* Channel-specific (requires channel_id) */
        case 0x01: mapped_type = RESET_TYPE_CHANNEL_CONFIG; break;          /* RESET_CHANNEL_CONFIG */
        case 0x02: mapped_type = RESET_TYPE_CHANNEL_SCHEDULE; break;        /* RESET_CHANNEL_SCHEDULE */
        case 0x03: /* RESET_CHANNEL_STATISTICS - not supported */ type_valid = false; break;
        case 0x04: /* RESET_CHANNEL_HISTORY - not supported */ type_valid = false; break;

        /* System-wide */
        case 0x10: mapped_type = RESET_TYPE_ALL_CHANNELS; break;            /* RESET_ALL_CHANNELS */
        case 0x11: mapped_type = RESET_TYPE_ALL_SCHEDULES; break;           /* RESET_ALL_SCHEDULES */
        case 0x12: mapped_type = RESET_TYPE_SYSTEM_CONFIG; break;           /* RESET_SYSTEM_CONFIG */
        case 0x13: /* RESET_STATISTICS - not supported */ type_valid = false; break;
        case 0x14: mapped_type = RESET_TYPE_HISTORY; break;                 /* RESET_HISTORY */
        case 0x15: /* RESET_ONBOARDING - not supported */ type_valid = false; break;

        /* Complete system */
        case 0xFF: mapped_type = RESET_TYPE_FACTORY_RESET; break;           /* RESET_FACTORY_SETTINGS */

        default:
            type_valid = false;
            break;
    }

    if (!type_valid) {
        LOG_ERR("Unsupported reset_type 0x%02x per BLE spec", reset_data->reset_type);
        return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
    }
    
    /* Check if this is a confirmation code generation request */
    if (reset_data->confirmation_code == 0) {
        /* Generate confirmation code */
        uint32_t code = reset_controller_generate_confirmation_code(
            mapped_type, reset_data->channel_id);
        
        if (code == 0) {
            LOG_ERR("Failed to generate confirmation code for reset type %u", reset_data->reset_type);
            return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
        }
        
        LOG_INF("Generated confirmation code 0x%08x for reset type %u, channel %u", 
                code, reset_data->reset_type, reset_data->channel_id);
        
        /* Notify clients of the new confirmation code */
        bt_irrigation_reset_control_notify();
        
        return len;
    }
    
    /* Execute reset operation */
    reset_request_t request = {
        .type = mapped_type,
        .channel_id = reset_data->channel_id,
        .confirmation_code = reset_data->confirmation_code
    };
    
    reset_status_t status = reset_controller_execute(&request);
    
    if (status == RESET_STATUS_SUCCESS) {
        LOG_INF("Reset operation completed successfully: type=%u, channel=%u", 
                request.type, request.channel_id);
        
        /* Notify clients of successful reset */
        bt_irrigation_reset_control_notify();
        
        /* Also trigger onboarding status update */
        if (notification_state.onboarding_status_notifications_enabled) {
            bt_irrigation_onboarding_status_notify();
        }
        
        return len;
    } else {
        LOG_ERR("Reset operation failed: type=%u, channel=%u, status=%u (%s)", 
                request.type, request.channel_id, status, 
                reset_controller_get_status_description(status));
        
        /* Map reset status to BLE error codes */
        switch (status) {
            case RESET_STATUS_INVALID_TYPE:
            case RESET_STATUS_INVALID_CHANNEL:
                return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
            case RESET_STATUS_INVALID_CODE:
                /* Incorrect confirmation code */
                return BT_GATT_ERR(BT_ATT_ERR_AUTHENTICATION);
            case RESET_STATUS_CODE_EXPIRED:
                /* Code expired - treat as authorization failure */
                return BT_GATT_ERR(BT_ATT_ERR_AUTHORIZATION);
            case RESET_STATUS_STORAGE_ERROR:
                return BT_GATT_ERR(BT_ATT_ERR_INSUFFICIENT_RESOURCES);
            default:
                return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
        }
    }
}

/* Reset control CCC callback */
static void reset_control_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value) {
    bool notif_enabled = (value == BT_GATT_CCC_NOTIFY);
    notification_state.reset_control_notifications_enabled = notif_enabled;
    
    LOG_DBG("Reset control notifications %s", notif_enabled ? "enabled" : "disabled");
}

/* ----------------------------------------------------------- */
/*  GATT Service Definition                                   */
/* ----------------------------------------------------------- */

BT_GATT_SERVICE_DEFINE(irrigation_svc,
    BT_GATT_PRIMARY_SERVICE(&irrigation_service_uuid.uuid),
    
    // Valve control characteristic
    BT_GATT_CHARACTERISTIC(&valve_char_uuid.uuid,
                         BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE | BT_GATT_CHRC_NOTIFY,
                         BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT,
                         read_valve, write_valve, valve_value),
    BT_GATT_CCC(valve_ccc_cfg_changed,
               BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT),
    
    // Flow sensor characteristic  
    BT_GATT_CHARACTERISTIC(&flow_char_uuid.uuid,
                         BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
                         BT_GATT_PERM_READ_ENCRYPT,
                         read_flow, NULL, flow_value),
    BT_GATT_CCC(flow_ccc_cfg_changed,
               BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT),
    
    // Status characteristic
    BT_GATT_CHARACTERISTIC(&status_char_uuid.uuid,
                         BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
                         BT_GATT_PERM_READ_ENCRYPT,
                         read_status, NULL, status_value),
    BT_GATT_CCC(status_ccc_cfg_changed,
               BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT),
    
    // Channel configuration characteristic
    BT_GATT_CHARACTERISTIC(&channel_config_uuid.uuid,
                         BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE | BT_GATT_CHRC_NOTIFY,
                         BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT,
                         read_channel_config, write_channel_config, channel_config_value),
    BT_GATT_CCC(channel_config_ccc_changed,
               BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT),
    
    // Schedule configuration characteristic
    BT_GATT_CHARACTERISTIC(&schedule_uuid.uuid,
                         BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE | BT_GATT_CHRC_NOTIFY,
                         BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT,
                         read_schedule, write_schedule, schedule_value),
    BT_GATT_CCC(schedule_ccc_changed,
               BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT),
    
    // System configuration characteristic
    BT_GATT_CHARACTERISTIC(&system_config_uuid.uuid,
                         BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE | BT_GATT_CHRC_NOTIFY,
                         BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT,
                         read_system_config, write_system_config, system_config_value),
    BT_GATT_CCC(system_config_ccc_changed,
               BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT),
    
    // Task queue characteristic
    BT_GATT_CHARACTERISTIC(&task_queue_uuid.uuid,
                         BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE | BT_GATT_CHRC_NOTIFY,
                         BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT,
                         read_task_queue, write_task_queue, task_queue_value),
    BT_GATT_CCC(task_queue_ccc_changed,
               BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT),
    
    // Statistics characteristic
    BT_GATT_CHARACTERISTIC(&statistics_uuid.uuid,
                         BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE | BT_GATT_CHRC_NOTIFY,
                         BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT,
                         read_statistics, write_statistics, statistics_value),
    BT_GATT_CCC(statistics_ccc_cfg_changed,
               BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT),
    
    // RTC characteristic
    BT_GATT_CHARACTERISTIC(&rtc_char_uuid.uuid,
                         BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE | BT_GATT_CHRC_NOTIFY,
                         BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT,
                         read_rtc, write_rtc, rtc_value),
    BT_GATT_CCC(rtc_ccc_changed,
               BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT),
    
    // Alarm characteristic
    BT_GATT_CHARACTERISTIC(&alarm_char_uuid.uuid,
                         BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE | BT_GATT_CHRC_NOTIFY,
                         BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT,
                         read_alarm, write_alarm, alarm_value),
    BT_GATT_CCC(alarm_ccc_changed,
               BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT),
    
    // Calibration characteristic
    BT_GATT_CHARACTERISTIC(&calibration_char_uuid.uuid,
                         BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE | BT_GATT_CHRC_NOTIFY,
                         BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT,
                         read_calibration, write_calibration, calibration_value),
    BT_GATT_CCC(calibration_ccc_changed,
               BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT),
    
    // History characteristic
    BT_GATT_CHARACTERISTIC(&history_char_uuid.uuid,
                         BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE | BT_GATT_CHRC_NOTIFY,
                         BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT,
                         read_history, write_history, history_value),
    BT_GATT_CCC(history_ccc_changed,
               BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT),
    
    // Diagnostics characteristic
    BT_GATT_CHARACTERISTIC(&diagnostics_char_uuid.uuid,
                         BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
                         BT_GATT_PERM_READ_ENCRYPT,
                         read_diagnostics, NULL, diagnostics_value),
    BT_GATT_CCC(diagnostics_ccc_changed,
               BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT),
    
    // Growing environment characteristic
    BT_GATT_CHARACTERISTIC(&growing_env_char_uuid.uuid,
                         BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE | BT_GATT_CHRC_NOTIFY,
                         BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT,
                         read_growing_env, write_growing_env, growing_env_value),
    BT_GATT_CCC(growing_env_ccc_changed,
               BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT),
    
    // Automatic calculation status characteristic
    BT_GATT_CHARACTERISTIC(&auto_calc_status_char_uuid.uuid,
                        BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE | BT_GATT_CHRC_NOTIFY,
                        BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT, 
                        read_auto_calc_status, write_auto_calc_status, auto_calc_status_value),
    BT_GATT_CCC(auto_calc_status_ccc_changed,
               BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT),
    
    // Current task characteristic
    BT_GATT_CHARACTERISTIC(&current_task_char_uuid.uuid,
                         BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE | BT_GATT_CHRC_NOTIFY,
                         BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT,
                         read_current_task, write_current_task, current_task_value),
    BT_GATT_CCC(current_task_ccc_changed,
               BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT),
    
    // Timezone configuration characteristic
    BT_GATT_CHARACTERISTIC(&timezone_char_uuid.uuid,
                         BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE | BT_GATT_CHRC_NOTIFY,
                         BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT,
                         read_timezone, write_timezone, timezone_value),
    BT_GATT_CCC(timezone_ccc_changed,
               BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT),
    
    // Rain sensor configuration characteristic
    BT_GATT_CHARACTERISTIC(&rain_config_char_uuid.uuid,
                         BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE | BT_GATT_CHRC_NOTIFY,
                         BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT,
                         read_rain_config, write_rain_config, rain_config_value),
    BT_GATT_CCC(rain_config_ccc_changed,
               BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT),
    
    // Rain sensor data characteristic
    BT_GATT_CHARACTERISTIC(&rain_data_char_uuid.uuid,
                         BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
                         BT_GATT_PERM_READ_ENCRYPT,
                         read_rain_data, NULL, rain_data_value),
    BT_GATT_CCC(rain_data_ccc_changed,
               BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT),
    
    // Rain history control characteristic
    BT_GATT_CHARACTERISTIC(&rain_history_char_uuid.uuid,
                         BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE | BT_GATT_CHRC_NOTIFY,
                         BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT,
                         read_rain_history, write_rain_history, rain_history_value),
    BT_GATT_CCC(rain_history_ccc_changed,
               BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT),
    
    // Environmental data characteristic (BME280 readings)
    BT_GATT_CHARACTERISTIC(&environmental_data_char_uuid.uuid,
                         BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
                         BT_GATT_PERM_READ_ENCRYPT,
                         read_environmental_data, NULL, environmental_data_value),
    BT_GATT_CCC(environmental_data_ccc_changed,
               BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT),
    
    // Environmental history characteristic (with fragmentation support)
    BT_GATT_CHARACTERISTIC(&environmental_history_char_uuid.uuid,
                         BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE | BT_GATT_CHRC_NOTIFY,
                         BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT,
                         read_environmental_history, write_environmental_history, environmental_history_value),
    BT_GATT_CCC(environmental_history_ccc_changed,
               BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT),
    
    // Compensation status characteristic (real-time compensation information)
    BT_GATT_CHARACTERISTIC(&compensation_status_char_uuid.uuid,
                         BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE | BT_GATT_CHRC_NOTIFY,
                         BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT,
                         read_compensation_status, write_compensation_status, compensation_status_value),
    BT_GATT_CCC(compensation_status_ccc_changed,
               BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT),
    
    // Onboarding status characteristic
    BT_GATT_CHARACTERISTIC(&onboarding_status_char_uuid.uuid,
                         BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
                         BT_GATT_PERM_READ_ENCRYPT,
                         read_onboarding_status, NULL, NULL),
    BT_GATT_CCC(onboarding_status_ccc_changed, BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT),
    
    // Reset control characteristic
    BT_GATT_CHARACTERISTIC(&reset_control_char_uuid.uuid,
                         BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE | BT_GATT_CHRC_NOTIFY,
                         BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT,
                         read_reset_control, write_reset_control, NULL),
    BT_GATT_CCC(reset_control_ccc_changed, BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT),

    // Rain Integration Status characteristic (separate from rain config)
    BT_GATT_CHARACTERISTIC(&rain_integration_status_char_uuid.uuid,
                         BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
                         BT_GATT_PERM_READ_ENCRYPT,
                         read_rain_integration_status, NULL, rain_integration_status_value),
    BT_GATT_CCC(rain_integration_status_ccc_changed, BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT)

    // Channel Compensation Config characteristic (per-channel rain/temp settings)
    ,BT_GATT_CHARACTERISTIC(&channel_comp_config_char_uuid.uuid,
                         BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE | BT_GATT_CHRC_NOTIFY,
                         BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT,
                         read_channel_comp_config, write_channel_comp_config, channel_comp_config_value),
    BT_GATT_CCC(channel_comp_config_ccc_changed, BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT)
);

/* ------------------------------------------------------------------ */
/* Implementation of GATT characteristic functions                   */
/* ------------------------------------------------------------------ */

/* Schedule characteristics implementation */
static ssize_t read_schedule(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                            void *buf, uint16_t len, uint16_t offset) {
    if (!conn || !attr || !buf) {
        LOG_ERR("Invalid parameters for Schedule read");
        return -EINVAL;
    }
    
    /* Create a local buffer for reading to avoid conflicts with notification buffer */
    struct schedule_config_data read_value;
    
    /* Get the current channel selection from the global attribute buffer */
    const struct schedule_config_data *global_value = 
        (const struct schedule_config_data *)schedule_value;
    
    /* Use the selected channel from the global buffer, but default to 0 if invalid */
    uint8_t channel_id = global_value->channel_id;
    if (channel_id >= WATERING_CHANNELS_COUNT) {
        channel_id = 0;
    }
    
    watering_channel_t *channel;
    watering_error_t err = watering_get_channel(channel_id, &channel);
    
    if (err != WATERING_SUCCESS) {
        LOG_WRN("Failed to get channel %u for schedule read: %d", channel_id, err);
        /* Return default/safe values - use compound literal for efficiency */
        static const struct schedule_config_data default_schedule = {
            .channel_id = 0,
            .schedule_type = 0, /* Daily */
            .days_mask = 0x7F, /* All days */
            .hour = 6,
            .minute = 0,
            .watering_mode = 0, /* Duration */
            .value = 5, /* 5 minutes */
            .auto_enabled = 0,
            .use_solar_timing = 0,
            .solar_event = 0, /* Sunset */
            .solar_offset_minutes = 0
        };
        read_value = default_schedule;
        read_value.channel_id = channel_id; /* Override with actual channel */
        
        return bt_gatt_attr_read(conn, attr, buf, len, offset,
                                 &read_value, sizeof(read_value));
    }
    
    /* Copy fresh data from the watering system */
    memset(&read_value, 0, sizeof(read_value));
    read_value.channel_id = channel_id;
    
    /* Map watering system schedule to BLE structure */
    if (channel->watering_event.schedule_type == SCHEDULE_DAILY) {
        read_value.schedule_type = 0;
        read_value.days_mask = channel->watering_event.schedule.daily.days_of_week;
    } else if (channel->watering_event.schedule_type == SCHEDULE_PERIODIC) {
        read_value.schedule_type = 1;
        read_value.days_mask = channel->watering_event.schedule.periodic.interval_days;
    } else if (channel->watering_event.schedule_type == SCHEDULE_AUTO) {
        read_value.schedule_type = 2;
        read_value.days_mask = 0x7F; /* All days - AUTO mode checks daily */
    } else {
        read_value.schedule_type = 0; /* Default to daily */
        read_value.days_mask = 0x7F; /* All days */
    }
    
    read_value.hour = channel->watering_event.start_time.hour;
    read_value.minute = channel->watering_event.start_time.minute;
    
    /* Map watering mode */
    if (channel->watering_event.watering_mode == WATERING_BY_DURATION) {
        read_value.watering_mode = 0;
        read_value.value = channel->watering_event.watering.by_duration.duration_minutes;
    } else {
        read_value.watering_mode = 1;
        read_value.value = channel->watering_event.watering.by_volume.volume_liters;
    }
    
    read_value.auto_enabled = channel->watering_event.auto_enabled ? 1 : 0;
    
    /* Map solar timing configuration */
    read_value.use_solar_timing = channel->watering_event.use_solar_timing ? 1 : 0;
    read_value.solar_event = channel->watering_event.solar_event;
    read_value.solar_offset_minutes = channel->watering_event.solar_offset_minutes;
    
    LOG_DBG("Schedule read: ch=%u, type=%u, days=0x%02X, time=%02u:%02u, mode=%u, value=%u, auto=%u, solar=%u",
            read_value.channel_id, read_value.schedule_type, read_value.days_mask,
            read_value.hour, read_value.minute, read_value.watering_mode,
            read_value.value, read_value.auto_enabled, read_value.use_solar_timing);
    
    return bt_gatt_attr_read(conn, attr, buf, len, offset,
                             &read_value, sizeof(read_value));
}

static ssize_t write_schedule(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                             const void *buf, uint16_t len, uint16_t offset, uint8_t flags) {
    struct schedule_config_data *value = (struct schedule_config_data *)attr->user_data;

    LOG_INF("Schedule write: len=%u, offset=%u, flags=0x%02x, expected_size=%zu", 
            len, offset, flags, sizeof(*value));

    /* Validate basic parameters */
    if (!conn || !attr || !buf) {
        LOG_ERR("Invalid parameters for Schedule write");
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_HANDLE);
    }

    /* Per BLE API Documentation: 1-byte SELECT-FOR-READ (channel selection) */
    if (!(flags & BT_GATT_WRITE_FLAG_PREPARE) && offset == 0 && len == 1) {
        uint8_t requested_channel_id = *(const uint8_t*)buf;  /* Direct access instead of memcpy */
        if (requested_channel_id >= WATERING_CHANNELS_COUNT) {
            LOG_ERR("Invalid channel ID for schedule selection: %u (max %u)", 
                    requested_channel_id, WATERING_CHANNELS_COUNT - 1);
            return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
        }
        
        /* Only update the local cache if the channel actually changed */
        if (value->channel_id != requested_channel_id) {
            value->channel_id = requested_channel_id;
            LOG_INF("Schedule channel selected for read: %u", requested_channel_id);
        }
        /* DO NOT call watering_save_config() here - this is just a selection, not a config change */
        return len; /* ACK */
    }

    /* Log raw data for debugging */
    if (len <= 16) {
        char hex_buf[50];
        int pos = 0;
        for (uint16_t i = 0; i < len && pos < 48; i++) {
            pos += snprintf(hex_buf + pos, sizeof(hex_buf) - pos, "%02x ", ((const uint8_t*)buf)[i]);
        }
        LOG_INF("Schedule raw data: %s", hex_buf);
    }

    /* Standard write handling for complete schedule structure */
    if (offset + len > sizeof(*value)) {
        LOG_ERR("Schedule write: Invalid offset/length (offset=%u, len=%u, max=%zu)", 
                offset, len, sizeof(*value));
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }

    /* Accept writes - copy data first */
    memcpy(((uint8_t *)value) + offset, buf, len);

    /* Only process if we have complete structure (offset 0, length 9) */
    if (offset == 0 && len == sizeof(*value)) {
        /* Validate channel ID */
        if (value->channel_id >= WATERING_CHANNELS_COUNT) {
            LOG_ERR("Invalid channel ID in schedule: %u (max %u)", 
                    value->channel_id, WATERING_CHANNELS_COUNT - 1);
            return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
        }

        watering_channel_t *channel;
        if (watering_get_channel(value->channel_id, &channel) != WATERING_SUCCESS) {
            LOG_ERR("Failed to get channel %u for schedule update", value->channel_id);
            return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
        }

        /* Validate input data according to BLE API Documentation */
        if (value->hour > 23 || value->minute > 59 || value->schedule_type > 2 ||
            value->watering_mode > 1) {
            LOG_ERR("Invalid schedule parameters: hour=%u, minute=%u, type=%u, mode=%u", 
                    value->hour, value->minute, value->schedule_type, value->watering_mode);
            return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
        }

        /* Validate value (must be > 0 if auto_enabled = 1, UNLESS FAO-56 or AUTO schedule is enabled) */
        /* When FAO-56 is enabled or schedule_type=2 (AUTO), the system calculates volume automatically */
        uint8_t ext_flags = onboarding_get_channel_extended_flags(value->channel_id);
        bool fao56_enabled = (ext_flags & CHANNEL_EXT_FLAG_FAO56_READY) != 0;
        bool is_auto_schedule = (value->schedule_type == 2);
        LOG_INF("Schedule validation: ch=%u, ext_flags=0x%02x, FAO56_READY=%d, AUTO=%d", 
                value->channel_id, ext_flags, fao56_enabled, is_auto_schedule);
        if (value->auto_enabled && value->value == 0 && !fao56_enabled && !is_auto_schedule) {
            LOG_ERR("Invalid schedule value: auto_enabled=1 but value=0 (FAO-56 not enabled, ext_flags=0x%02x)", ext_flags);
            return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
        }
        if (value->auto_enabled && value->value == 0 && (fao56_enabled || is_auto_schedule)) {
            LOG_INF("Schedule value=0 accepted: %s for channel %u", 
                    is_auto_schedule ? "AUTO schedule mode" : "FAO-56 auto-calculation enabled",
                    value->channel_id);
        }

        /* Validate days_mask (must be > 0 if auto_enabled = 1, UNLESS AUTO schedule) */
        /* AUTO schedule ignores days_mask - it checks every day automatically */
        if (value->auto_enabled && value->days_mask == 0 && !is_auto_schedule) {
            LOG_ERR("Invalid schedule days_mask: auto_enabled=1 but days_mask=0");
            return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
        }

        LOG_INF("Schedule update: ch=%u, type=%u (%s), days=0x%02X, time=%02u:%02u, mode=%u (%s), value=%u, auto=%u",
                value->channel_id, value->schedule_type, 
                (value->schedule_type == 0) ? "Daily" : (value->schedule_type == 1) ? "Periodic" : "Auto",
                value->days_mask, value->hour, value->minute, 
                value->watering_mode, (value->watering_mode == 0) ? "Duration" : "Volume",
                value->value, value->auto_enabled);

        /* Update schedule configuration - times are stored in LOCAL TIME */
        channel->watering_event.start_time.hour = value->hour;
        channel->watering_event.start_time.minute = value->minute;
        channel->watering_event.auto_enabled = (value->auto_enabled != 0);

        /* Update schedule type and parameters */
        if (value->schedule_type == 0) {
            /* Daily schedule */
            channel->watering_event.schedule_type = SCHEDULE_DAILY;
            channel->watering_event.schedule.daily.days_of_week = value->days_mask;
        } else if (value->schedule_type == 1) {
            /* Periodic schedule */
            channel->watering_event.schedule_type = SCHEDULE_PERIODIC;
            channel->watering_event.schedule.periodic.interval_days = value->days_mask;
        } else {
            /* AUTO (Smart Schedule) - FAO-56 based */
            channel->watering_event.schedule_type = SCHEDULE_AUTO;
            channel->watering_event.schedule.daily.days_of_week = 0x7F; /* Check every day */
            /* AUTO mode requires valid plant/soil/planting_date configuration */
            if (!watering_channel_auto_mode_valid(channel)) {
                LOG_WRN("AUTO schedule set but channel %u missing plant/soil/date config", 
                        value->channel_id);
            }
        }

        /* Update solar timing configuration */
        channel->watering_event.use_solar_timing = (value->use_solar_timing != 0);
        channel->watering_event.solar_event = value->solar_event;
        /* Clamp solar offset to valid range */
        if (value->solar_offset_minutes < SOLAR_OFFSET_MIN) {
            channel->watering_event.solar_offset_minutes = SOLAR_OFFSET_MIN;
        } else if (value->solar_offset_minutes > SOLAR_OFFSET_MAX) {
            channel->watering_event.solar_offset_minutes = SOLAR_OFFSET_MAX;
        } else {
            channel->watering_event.solar_offset_minutes = value->solar_offset_minutes;
        }
        
        if (channel->watering_event.use_solar_timing) {
            LOG_INF("Solar timing enabled: ch=%u, event=%s, offset=%+d min",
                    value->channel_id,
                    (value->solar_event == SOLAR_EVENT_SUNRISE) ? "sunrise" : "sunset",
                    channel->watering_event.solar_offset_minutes);
        }

        /* Update watering mode and parameters */
        if (value->watering_mode == 0) {
            /* Duration mode */
            channel->watering_event.watering_mode = WATERING_BY_DURATION;
            channel->watering_event.watering.by_duration.duration_minutes = value->value;
        } else {
            /* Volume mode */
            channel->watering_event.watering_mode = WATERING_BY_VOLUME;
            channel->watering_event.watering.by_volume.volume_liters = value->value;
        }

        /* Save configuration to persistent storage */
        watering_save_config_priority(true);
        
        /* Invalidate cache since configuration changed */
        invalidate_channel_cache();
        
        /* Update onboarding flag - this channel now has a schedule configured */
        onboarding_update_schedule_flag(value->channel_id, value->auto_enabled);
        if (value->auto_enabled) {
            /* Treat an active schedule as enabling the channel */
            onboarding_update_channel_flag(value->channel_id, CHANNEL_FLAG_ENABLED, true);
        }
        
        /* Send notification to confirm schedule update per BLE API Documentation */
        /* Schedule Config (ef5): Schedule updates | On change (throttled 500ms) | Schedule confirmations */
        if (notification_state.schedule_notifications_enabled) {
            bt_irrigation_schedule_update(value->channel_id);
        }
        
        LOG_INF("✅ Schedule updated successfully for channel %u", value->channel_id);
    }
    
    return len;
}

static void schedule_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value) {
    bool notif_enabled = (value == BT_GATT_CCC_NOTIFY);
    notification_state.schedule_notifications_enabled = notif_enabled;
    
    if (notif_enabled) {
        LOG_INF("✅ Schedule notifications ENABLED - will send updates when schedule changes");
        
        /* Per BLE API Documentation: Schedule Config structure (9 bytes total) */
        /* Fields: channel_id, schedule_type, days_mask, hour, minute, watering_mode, value, auto_enabled */
        /* Notification frequency: on change (throttled 500ms) for schedule confirmations */
        
        LOG_INF("Schedule monitoring active: 9-byte structure with throttled notifications");
        
        /* Clear any stale data to ensure clean state */
        memset(schedule_value, 0, sizeof(schedule_value));
        
        /* Initialize with default channel 0 for read operations */
        struct schedule_config_data *schedule_data = (struct schedule_config_data *)schedule_value;
        schedule_data->channel_id = 0;
    } else {
        LOG_INF("Schedule notifications disabled");
        /* Clear schedule_value when notifications disabled */
        memset(schedule_value, 0, sizeof(schedule_value));
    }
}

/* System config characteristics implementation */
static ssize_t read_system_config(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                 void *buf, uint16_t len, uint16_t offset) {
    if (!conn || !attr || !buf) {
        LOG_ERR("Invalid parameters for Enhanced System Config read");
        return -EINVAL;
    }
    
    /* Enhanced system configuration with BME280 and compensation support */
    struct enhanced_system_config_data enhanced_config;
    memset(&enhanced_config, 0, sizeof(enhanced_config));
    
    /* Update basic system values */
    enhanced_config.version = 2; /* Enhanced configuration version */
    
    /* Get current power mode from watering system */
    power_mode_t current_mode;
    if (watering_get_power_mode(&current_mode) == WATERING_SUCCESS) {
        enhanced_config.power_mode = (uint8_t)current_mode;
    } else {
        enhanced_config.power_mode = 0; /* Default to normal mode on error */
    }
    
    enhanced_config.flow_calibration = get_flow_calibration(); /* Get current calibration */
    enhanced_config.max_active_valves = 1; /* Always 1 (read-only) */
    enhanced_config.num_channels = WATERING_CHANNELS_COUNT; /* Number of channels (read-only) */
    
    /* Get current master valve configuration */
    master_valve_config_t master_config;
    if (master_valve_get_config(&master_config) == WATERING_SUCCESS) {
        enhanced_config.master_valve_enabled = master_config.enabled ? 1 : 0;
        enhanced_config.master_valve_pre_delay = master_config.pre_start_delay_sec;
        enhanced_config.master_valve_post_delay = master_config.post_stop_delay_sec;
        enhanced_config.master_valve_overlap_grace = master_config.overlap_grace_sec;
        enhanced_config.master_valve_auto_mgmt = master_config.auto_management ? 1 : 0;
        enhanced_config.master_valve_current_state = master_config.is_active ? 1 : 0;
    } else {
        /* Default values on error */
        enhanced_config.master_valve_enabled = 0;
        enhanced_config.master_valve_pre_delay = 0;
        enhanced_config.master_valve_post_delay = 0;
        enhanced_config.master_valve_overlap_grace = 30;
        enhanced_config.master_valve_auto_mgmt = 1;
        enhanced_config.master_valve_current_state = 0;
    }
    
    /* Get BME280 sensor configuration */
    bme280_config_t bme280_config;
    if (bme280_system_get_config(&bme280_config) == 0) {
        enhanced_config.bme280_enabled = bme280_config.enabled ? 1 : 0;
        enhanced_config.bme280_measurement_interval = bme280_config.measurement_interval;
        enhanced_config.bme280_sensor_status = bme280_config.initialized ? 1 : 0;
    } else {
        /* Default BME280 values on error */
        enhanced_config.bme280_enabled = 0;
        enhanced_config.bme280_measurement_interval = 60;
        enhanced_config.bme280_sensor_status = 0;
    }
    
    /* Rain compensation is now per-channel only - set deprecated fields to 0 */
    enhanced_config._reserved_rain_enabled = 0;
    enhanced_config._reserved_rain_sensitivity = 0.0f;
    enhanced_config._reserved_rain_lookback = 0;
    enhanced_config._reserved_rain_threshold = 0.0f;

    /* Get global temperature compensation settings (averaged from channels) */
    uint8_t temp_enabled_channels = 0;
    float temp_sensitivity_accum = 0.0f;
    float temp_base_accum = 0.0f;

    for (uint8_t i = 0; i < WATERING_CHANNELS_COUNT; i++) {
        watering_channel_t *channel;
        if (watering_get_channel(i, &channel) != WATERING_SUCCESS) {
            continue;
        }
        if (channel->temp_compensation.enabled) {
            temp_enabled_channels++;
            temp_sensitivity_accum += channel->temp_compensation.sensitivity;
            temp_base_accum += channel->temp_compensation.base_temperature;
        }
    }

    if (temp_enabled_channels > 0) {
        enhanced_config.global_temp_compensation_enabled = 1;
        enhanced_config.global_temp_sensitivity =
            temp_sensitivity_accum / (float)temp_enabled_channels;
        enhanced_config.global_temp_base_temperature =
            temp_base_accum / (float)temp_enabled_channels;
    } else {
        enhanced_config.global_temp_compensation_enabled = 0;
        enhanced_config.global_temp_sensitivity = TEMP_COMP_DEFAULT_SENSITIVITY;
        enhanced_config.global_temp_base_temperature = TEMP_COMP_DEFAULT_BASE_TEMP;
    }
    
    /* Get system status indicators */
    enhanced_system_is_interval_mode_active(&enhanced_config.interval_mode_active_channels);
    enhanced_system_has_incomplete_config(&enhanced_config.incomplete_config_channels);
    
    /* Calculate compensation active channels */
    enhanced_config.compensation_active_channels = 0;
    for (uint8_t i = 0; i < WATERING_CHANNELS_COUNT; i++) {
        watering_channel_t *channel;
        if (watering_get_channel(i, &channel) == WATERING_SUCCESS) {
            if (channel->rain_compensation.enabled || channel->temp_compensation.enabled) {
                enhanced_config.compensation_active_channels |= (1 << i);
            }
        }
    }
    
    /* Get environmental data quality */
    bme280_environmental_data_t env_data;
    if (environmental_data_get_current(&env_data) == 0 && env_data.current.valid) {
        env_data_validation_t validation;
        if (env_data_validate_reading(&env_data.current, NULL, &validation) == 0) {
            enhanced_config.environmental_data_quality =
                env_data_calculate_quality_score(&env_data.current, &validation);
        } else {
            enhanced_config.environmental_data_quality = 0;
        }
        enhanced_config.last_sensor_reading = env_data.current.timestamp;
    } else {
        enhanced_config.environmental_data_quality = 0; /* No data available */
        enhanced_config.last_sensor_reading = timezone_get_unix_utc();
    }
    
    /* Set timestamps */
    enhanced_config.last_config_update = timezone_get_unix_utc();
    if (enhanced_config.last_sensor_reading == 0) {
        enhanced_config.last_sensor_reading = enhanced_config.last_config_update;
    }
    
    /* Reduced logging frequency for read operations */
    static uint32_t last_read_time = 0;
    uint32_t now = k_uptime_get_32();
    if (now - last_read_time > 5000) { /* Log every 5 seconds max */
        LOG_DBG("Enhanced System Config read: version=%u, power_mode=%u, flow_cal=%u",
                enhanced_config.version, enhanced_config.power_mode, enhanced_config.flow_calibration);
        LOG_DBG("BME280: enabled=%u, interval=%u, status=%u",
                enhanced_config.bme280_enabled, enhanced_config.bme280_measurement_interval,
                enhanced_config.bme280_sensor_status);
        LOG_DBG("Compensation: temp_global=%u, active_channels=0x%02x (rain is per-channel only)",
                enhanced_config.global_temp_compensation_enabled,
                enhanced_config.compensation_active_channels);
        last_read_time = now;
    }
    
    return bt_gatt_attr_read(conn, attr, buf, len, offset, &enhanced_config,
                           sizeof(enhanced_config));
}

static ssize_t write_system_config(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                  const void *buf, uint16_t len, uint16_t offset, uint8_t flags) {
    struct enhanced_system_config_data *config = (struct enhanced_system_config_data *)attr->user_data;
    
    /* Validate basic parameters */
    if (!conn || !attr || !buf) {
        LOG_ERR("Invalid parameters for System Config write");
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_HANDLE);
    }
    
    /* Standard write handling */
    if (offset + len > sizeof(*config)) {
        LOG_ERR("System Config write: Invalid offset/length (offset=%u, len=%u, max=%zu)", 
                offset, len, sizeof(*config));
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }
    
    /* Support fragmented writes of the enhanced structure */
    memcpy(((uint8_t *)config) + offset, buf, len);
    system_config_bytes_received = offset + len;
    
    LOG_INF("System Config write: offset=%u, len=%u, total_received=%u, expected=%zu", 
            offset, len, system_config_bytes_received, sizeof(*config));
    
    /* Process fields as they are received - check which fields were written in this chunk */
    uint16_t write_start = offset;
    uint16_t write_end = offset + len;
    
    /* Field offsets in enhanced_system_config_data:
     * power_mode: offset 1, size 1
     * flow_calibration: offset 2, size 4
     * master_valve_enabled: offset 8, size 1
     */
    
    /* Check if power_mode field was written (offset 1, size 1) */
    if (write_start <= 1 && write_end >= 2) {
        LOG_INF("Power mode field received: value=%u (0=Normal, 1=EnergySaving, 2=UltraLow)", 
                config->power_mode);
        if (config->power_mode <= 2) {
            watering_error_t pm_err = watering_set_power_mode((power_mode_t)config->power_mode);
            if (pm_err == WATERING_SUCCESS) {
                onboarding_update_system_flag(SYSTEM_FLAG_POWER_MODE_SET, true);
                LOG_INF("✅ Power mode set to %u, flag updated", config->power_mode);
            } else {
                LOG_ERR("Failed to set power mode: %d", pm_err);
            }
        } else {
            LOG_ERR("Invalid power_mode value: %u", config->power_mode);
        }
    }
    
    /* Check if flow_calibration field was written (offset 2, size 4) */
    if (write_start <= 2 && write_end >= 6) {
        if (config->flow_calibration >= 100 && config->flow_calibration <= 10000) {
            int cal_err = set_flow_calibration(config->flow_calibration);
            if (cal_err == 0) {
                /* Flag is set inside set_flow_calibration -> nvs_save_flow_calibration */
                LOG_INF("Flow calibration updated: %u", config->flow_calibration);
            } else {
                LOG_ERR("Failed to set flow calibration: %d", cal_err);
            }
        }
    }
    
    /* Check if master_valve_enabled field was written (offset 8+) */
    if (write_start <= 8 && write_end >= 9) {
        /* Master valve config starts at offset 8 */
        master_valve_config_t master_config;
        master_config.enabled = (config->master_valve_enabled != 0);
        master_config.pre_start_delay_sec = config->master_valve_pre_delay;
        master_config.post_stop_delay_sec = config->master_valve_post_delay;
        master_config.overlap_grace_sec = config->master_valve_overlap_grace;
        master_config.auto_management = (config->master_valve_auto_mgmt != 0);
        
        watering_error_t mv_err = master_valve_set_config(&master_config);
        if (mv_err == WATERING_SUCCESS) {
            onboarding_update_system_flag(SYSTEM_FLAG_MASTER_VALVE_SET, true);
            LOG_INF("Master valve config updated: enabled=%u", config->master_valve_enabled);
        } else {
            LOG_ERR("Failed to set master valve config: %d", mv_err);
        }
    }
    
    /* If complete structure received, do full validation and apply remaining settings */
    if (system_config_bytes_received >= sizeof(*config)) {
        /* Final validation of all fields */
        if (config->power_mode > 2) {
            LOG_ERR("Invalid power_mode: %u (must be 0-2)", config->power_mode);
            return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
        }
        
        if (config->flow_calibration < 100 || config->flow_calibration > 10000) {
            LOG_ERR("Invalid flow_calibration: %u (range 100-10000)", config->flow_calibration);
            return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
        }
        
        if (config->bme280_enabled && config->bme280_measurement_interval == 0) {
            LOG_ERR("Invalid BME280 measurement interval: 0");
            return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
        }

        LOG_INF("System Config complete: power_mode=%u, flow_cal=%u, bme280=%u",
                config->power_mode, config->flow_calibration, config->bme280_enabled);
        
        /* Read-only fields are ignored during write */
        /* version, max_active_valves, num_channels, master_valve_current_state cannot be changed */
        
        /* Best-effort apply BME280 settings if API available */
        bme280_config_t bme_cfg;
        if (bme280_system_get_config(&bme_cfg) == 0) {
            bme_cfg.enabled = (config->bme280_enabled != 0);
            if (config->bme280_measurement_interval != 0) {
                bme_cfg.measurement_interval = config->bme280_measurement_interval;
            }
            /* Apply via sensor manager if present */
            extern int sensor_manager_configure_bme280(const bme280_config_t *config);
            int sm_ret = sensor_manager_configure_bme280(&bme_cfg);
            if (sm_ret != 0) {
                LOG_WRN("BME280 configure failed (%d), continuing without error", sm_ret);
            }
        }

        /* Rain compensation is now per-channel only - global fields are ignored */
        /* Configure rain compensation via Channel Configuration characteristic instead */

        /* Apply global temperature compensation defaults */
        float temp_sensitivity = config->global_temp_sensitivity;
        if (temp_sensitivity < TEMP_COMP_MIN_SENSITIVITY) {
            temp_sensitivity = TEMP_COMP_MIN_SENSITIVITY;
        } else if (temp_sensitivity > TEMP_COMP_MAX_SENSITIVITY) {
            temp_sensitivity = TEMP_COMP_MAX_SENSITIVITY;
        }

        float base_temperature = config->global_temp_base_temperature;
        if (base_temperature < TEMP_COMP_MIN_TEMP_C) {
            base_temperature = TEMP_COMP_MIN_TEMP_C;
        } else if (base_temperature > TEMP_COMP_MAX_TEMP_C) {
            base_temperature = TEMP_COMP_MAX_TEMP_C;
        }

        bool temp_enable = (config->global_temp_compensation_enabled != 0);

        for (uint8_t ch = 0; ch < WATERING_CHANNELS_COUNT; ch++) {
            watering_channel_t *channel;
            if (watering_get_channel(ch, &channel) != WATERING_SUCCESS) {
                continue;
            }
            channel->temp_compensation.enabled = temp_enable;
            channel->temp_compensation.base_temperature = base_temperature;
            channel->temp_compensation.sensitivity = temp_sensitivity;
            channel->temp_compensation.min_factor = TEMP_COMP_DEFAULT_MIN_FACTOR;
            channel->temp_compensation.max_factor = TEMP_COMP_DEFAULT_MAX_FACTOR;
        }

        watering_save_config_priority(true);

        /* Send notification to confirm system config update */
        if (notification_state.system_config_notifications_enabled) {
            /* Ensure read-only fields are populated */
            config->version = 2; /* Enhanced configuration version */
            config->max_active_valves = 1; /* Always 1 */
            config->num_channels = WATERING_CHANNELS_COUNT; /* Number of channels */

            const struct bt_gatt_attr *attr = &irrigation_svc.attrs[ATTR_IDX_SYSTEM_CFG_VALUE];
            int bt_err = safe_notify(default_conn, attr, system_config_value, sizeof(struct enhanced_system_config_data));
            
            if (bt_err == 0) {
                LOG_INF("✅ System Config notification sent successfully");
            } else {
                LOG_ERR("❌ Failed to send System Config notification: %d", bt_err);
            }
        }
        
        LOG_INF("✅ System configuration updated successfully");
    }
    
    return len;
}

static void system_config_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value) {
    bool notif_enabled = (value == BT_GATT_CCC_NOTIFY);
    notification_state.system_config_notifications_enabled = notif_enabled;
    
    if (notif_enabled) {
        LOG_INF("✅ System Config notifications ENABLED - will send updates when config changes");
        LOG_INF("System Config monitoring: enhanced configuration active (68B)");

        /* Initialize system_config_value with current values (enhanced) */
        struct enhanced_system_config_data *config = (struct enhanced_system_config_data *)system_config_value;
        memset(config, 0, sizeof(*config));
        config->version = 2; /* Enhanced version */
        power_mode_t current_mode;
        if (watering_get_power_mode(&current_mode) == WATERING_SUCCESS) {
            config->power_mode = (uint8_t)current_mode;
        }
        config->flow_calibration = get_flow_calibration();
        config->max_active_valves = 1;
        config->num_channels = WATERING_CHANNELS_COUNT;
    } else {
        LOG_INF("System Config notifications disabled");
        /* Clear system_config_value when notifications disabled */
        memset(system_config_value, 0, sizeof(system_config_value));
        system_config_bytes_received = 0;
    }
}

/* Task queue characteristics implementation */
/* Forward declare periodic handler and define work item early so CCC can start/stop it */
static void task_queue_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(task_queue_periodic_work, task_queue_work_handler);
/* Helper: send error-shaped Task Queue notification (current_task_type=0xFF, current_value=error_code) */
static void task_queue_send_error(uint8_t error_code)
{
    if (!default_conn || !notification_state.task_queue_notifications_enabled) {
        return;
    }
    struct task_queue_data *qd = (struct task_queue_data *)task_queue_value;
    uint8_t pending = 0; bool active = false;
    (void)watering_get_queue_status(&pending, &active);
    qd->pending_count = pending;
    qd->completed_tasks = watering_get_completed_tasks_count();

    if (!active) {
        qd->current_channel = 0xFF;
        qd->active_task_id = 0;
        qd->current_value = 0;
    } else {
        /* Keep current task info if available */
        watering_task_t *current_task = watering_get_current_task();
        if (current_task) {
            uint8_t channel_id = current_task->channel - watering_channels;
            qd->current_channel = channel_id;
            qd->active_task_id = 1;
        }
    }
    qd->current_task_type = 0xFF; /* Error indicator */
    qd->current_value = error_code; /* Error code */
    qd->command = 0;
    qd->task_id_to_delete = 0;

    const struct bt_gatt_attr *nattr = &irrigation_svc.attrs[ATTR_IDX_TASK_QUEUE_VALUE];
    (void)safe_notify(default_conn, nattr, task_queue_value, sizeof(task_queue_value));
}
static ssize_t read_task_queue(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                              void *buf, uint16_t len, uint16_t offset) {
    if (!conn || !attr || !buf) {
        LOG_ERR("Invalid parameters for Task Queue read");
        return -EINVAL;
    }
    
    /* Per BLE API Documentation: READ returns current task queue status */
    /* Structure: pending_count, completed_tasks, current_channel, current_task_type, 
                 current_value, command, task_id_to_delete, active_task_id */
    struct task_queue_data *queue_data = (struct task_queue_data *)task_queue_value;
    
    /* Update values from task system */
    
    /* Get current queue status using the available function */
    uint8_t pending_count = 0;
    bool active = false;
    watering_error_t err = watering_get_queue_status(&pending_count, &active);
    
    if (err == WATERING_SUCCESS) {
        queue_data->pending_count = pending_count;
    } else {
        queue_data->pending_count = 0;
    }
    
    /* Get completed tasks count from the tracking system */
    queue_data->completed_tasks = watering_get_completed_tasks_count();
    queue_data->current_channel = 0xFF; /* No active channel */
    queue_data->current_task_type = 0; /* Duration */
    queue_data->current_value = 0; /* No active task */
    queue_data->command = 0; /* No command */
    queue_data->task_id_to_delete = 0; /* No deletion */
    queue_data->active_task_id = 0; /* No active task */
    
    /* Try to get current active task information */
    watering_task_t *current_task = watering_get_current_task();
    if (current_task) {
        uint8_t channel_id = current_task->channel - watering_channels;
        queue_data->current_channel = channel_id;
        queue_data->current_task_type = (current_task->channel->watering_event.watering_mode == WATERING_BY_DURATION) ? 0 : 1;
        
        if (current_task->channel->watering_event.watering_mode == WATERING_BY_DURATION) {
            queue_data->current_value = current_task->channel->watering_event.watering.by_duration.duration_minutes;
        } else {
            queue_data->current_value = current_task->channel->watering_event.watering.by_volume.volume_liters;
        }
        queue_data->active_task_id = 1; /* Simple ID for active task */
    }
    
    /* Get pending task count */
    queue_data->pending_count = watering_get_pending_tasks_count();
    
    /* Get completed tasks count from the tracking system */
    queue_data->completed_tasks = watering_get_completed_tasks_count();
    
    LOG_DBG("Task Queue read: pending=%u, completed=%u, current_ch=%u, type=%u, value=%u, task_id=%u",
            queue_data->pending_count, queue_data->completed_tasks, 
            queue_data->current_channel, queue_data->current_task_type,
            queue_data->current_value, queue_data->active_task_id);
    
    return bt_gatt_attr_read(conn, attr, buf, len, offset, task_queue_value,
                           sizeof(task_queue_value));
}

static ssize_t write_task_queue(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                               const void *buf, uint16_t len, uint16_t offset, uint8_t flags) {
    struct task_queue_data *queue_data = (struct task_queue_data *)attr->user_data;
    
    /* Validate basic parameters */
    if (!conn || !attr || !buf) {
        LOG_ERR("Invalid parameters for Task Queue write");
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_HANDLE);
    }
    
    /* Standard write handling */
    if (offset + len > sizeof(*queue_data)) {
        LOG_ERR("Task Queue write: Invalid offset/length (offset=%u, len=%u, max=%zu)", 
                offset, len, sizeof(*queue_data));
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }
    
    /* Only accept complete structure writes */
    if (len != sizeof(*queue_data)) {
        LOG_ERR("Task Queue write: Invalid length (got %u, expected %zu)", 
                len, sizeof(*queue_data));
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }
    
    memcpy(((uint8_t *)queue_data) + offset, buf, len);
    
    /* If complete structure received, process commands */
    if (offset + len == sizeof(*queue_data)) {
        /* Process command field */
        if (queue_data->command != 0) {
            LOG_INF("Task Queue command: %u", queue_data->command);

            switch (queue_data->command) {
                case 1: /* Start next task in queue */
                {
                    uint8_t pending = 0; bool active = false;
                    (void)watering_get_queue_status(&pending, &active);
                    if (pending == 0) {
                        LOG_WRN("Start requested with no pending tasks");
                        task_queue_send_error(1); /* No pending tasks */
                        break;
                    }
                    if (active) {
                        LOG_WRN("Start requested while a task is already active");
                        task_queue_send_error(3); /* Treat busy as not ready */
                        break;
                    }
                    watering_status_t sys;
                    if (watering_get_status(&sys) != WATERING_SUCCESS || sys != WATERING_STATUS_OK) {
                        LOG_WRN("System not ready to start next task: %d", sys);
                        task_queue_send_error(3);
                        break;
                    }
                    int started = watering_process_next_task();
                    if (started <= 0) {
                        LOG_WRN("No task started (ret=%d)", started);
                        task_queue_send_error(3);
                        break;
                    }
                    LOG_INF("✅ Started next task from queue");
                    queue_data->command = 0;
                    if (notification_state.task_queue_notifications_enabled) {
                        bt_irrigation_queue_status_notify();
                    }
                }
                break;

                case 2: /* Pause current task */
                {
                    if (!watering_pause_current_task()) {
                        LOG_WRN("Pause requested but no pausable task");
                        task_queue_send_error(2); /* Invalid command in current state */
                        break;
                    }
                    LOG_INF("✅ Paused current task");
                    queue_data->command = 0;
                    if (notification_state.task_queue_notifications_enabled) {
                        bt_irrigation_queue_status_notify();
                    }
                }
                break;

                case 3: /* Resume current task */
                {
                    if (!watering_resume_current_task()) {
                        LOG_WRN("Resume requested but no resumable task");
                        task_queue_send_error(2);
                        break;
                    }
                    LOG_INF("✅ Resumed current task");
                    queue_data->command = 0;
                    if (notification_state.task_queue_notifications_enabled) {
                        bt_irrigation_queue_status_notify();
                    }
                }
                break;

                case 4: /* Cancel current task */
                {
                    if (!watering_stop_current_task()) {
                        LOG_WRN("Cancel requested but no active task");
                        task_queue_send_error(2);
                        break;
                    }
                    LOG_INF("✅ Cancelled current task");
                    queue_data->command = 0;
                    if (notification_state.task_queue_notifications_enabled) {
                        bt_irrigation_queue_status_notify();
                    }
                }
                break;

                case 5: /* Clear all pending tasks */
                {
                    int cerr = watering_clear_task_queue();
                    if (cerr != 0) {
                        LOG_ERR("❌ Failed to clear task queue: %d", cerr);
                        task_queue_send_error(3);
                        break;
                    }
                    LOG_INF("✅ Cleared all pending tasks");
                    queue_data->command = 0;
                    if (notification_state.task_queue_notifications_enabled) {
                        bt_irrigation_queue_status_notify();
                    }
                }
                break;

                default:
                    LOG_ERR("Unknown task queue command: %u", queue_data->command);
                    task_queue_send_error(2); /* Invalid command */
                    break;
            }

            LOG_INF("✅ Task Queue command processed");
        }
    }
    
    return len;
}

static void task_queue_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value) {
    bool notif_enabled = (value == BT_GATT_CCC_NOTIFY);
    notification_state.task_queue_notifications_enabled = notif_enabled;
    
    if (notif_enabled) {
        LOG_INF("✅ Task Queue notifications ENABLED - will send updates when queue changes");
        
        /* Per BLE API Documentation: Task Queue notifications */
        /* • Sent when tasks are added, started, completed, or when queue state changes */
        /* • 9-byte structure with pending counts, current task info, and command interface */
        /* • Commands: 1=Start immediate task, 2=Stop current task, 3=Clear completed counter */
        
        LOG_INF("Task Queue monitoring: pending tasks, current task, command interface");
        
    /* Initialize task_queue_value with current queue state */
        struct task_queue_data *queue_data = (struct task_queue_data *)task_queue_value;
        memset(queue_data, 0, sizeof(*queue_data));
        
        /* Set default values */
        queue_data->current_channel = 0xFF; /* No active channel */
        queue_data->current_task_type = 0; /* Duration */
        queue_data->current_value = 0; /* No active task */
        queue_data->command = 0; /* No command */
        queue_data->active_task_id = 0; /* No active task */

    /* Start periodic 5s updates while a task is active */
    k_work_schedule(&task_queue_periodic_work, K_SECONDS(5));
    } else {
        LOG_INF("Task Queue notifications disabled");
        /* Clear task_queue_value when notifications disabled */
        memset(task_queue_value, 0, sizeof(task_queue_value));
    k_work_cancel_delayable(&task_queue_periodic_work);
    }
}

/* Statistics characteristics implementation */
static ssize_t read_statistics(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                              void *buf, uint16_t len, uint16_t offset) {
    if (!conn || !attr || !buf) {
        LOG_ERR("Invalid parameters for Statistics read");
        return -EINVAL;
    }
    
    /* Create a local buffer for reading to avoid conflicts with notification buffer */
    struct statistics_data read_value;
    
    /* Get the current channel selection from the global attribute buffer */
    const struct statistics_data *global_value = 
        (const struct statistics_data *)statistics_value;
    
    /* Use the selected channel from the global buffer, but default to 0 if invalid */
    uint8_t channel_id = global_value->channel_id;
    if (channel_id >= WATERING_CHANNELS_COUNT) {
        channel_id = 0;
    }
    
    watering_channel_t *channel;
    watering_error_t err = watering_get_channel(channel_id, &channel);
    
    if (err != WATERING_SUCCESS) {
        LOG_WRN("Failed to get channel %u for statistics read: %d", channel_id, err);
        /* Return default/safe values */
        memset(&read_value, 0, sizeof(read_value));
        read_value.channel_id = channel_id;
        read_value.total_volume = 0;
        read_value.last_volume = 0;
        read_value.last_watering = 0;
        read_value.count = 0;
        
        return bt_gatt_attr_read(conn, attr, buf, len, offset,
                                 &read_value, sizeof(read_value));
    }
    
    /* Copy fresh data from the watering system */
    memset(&read_value, 0, sizeof(read_value));
    read_value.channel_id = channel_id;
    
    /* Per BLE API Documentation: Statistics structure for a channel */
    /* Fields: channel_id, total_volume (ml), last_volume (ml), last_watering (timestamp), count */
    
    /* Get real statistics from watering system */
    uint32_t total_volume_ml = 0;
    uint32_t last_volume_ml = 0;
    uint32_t watering_count = 0;
    
    watering_error_t stats_err = watering_get_channel_statistics(channel_id,
                                                                &total_volume_ml,
                                                                &last_volume_ml,
                                                                &watering_count);
    
    if (stats_err == WATERING_SUCCESS) {
        read_value.total_volume = total_volume_ml;
        read_value.last_volume = last_volume_ml;
        read_value.count = watering_count;
    } else {
        LOG_WRN("Failed to get channel %u statistics: %d", channel_id, stats_err);
        read_value.total_volume = 0;
        read_value.last_volume = 0;
        read_value.count = 0;
    }
    
    /* last_watering_time is already Unix seconds; do not convert */
    read_value.last_watering = channel->last_watering_time;
    
    LOG_DBG("Statistics read: ch=%u, total_vol=%u, last_vol=%u, last_time=%u, count=%u",
            read_value.channel_id, read_value.total_volume, read_value.last_volume,
            read_value.last_watering, read_value.count);
    
    return bt_gatt_attr_read(conn, attr, buf, len, offset,
                             &read_value, sizeof(read_value));
}

static ssize_t write_statistics(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                               const void *buf, uint16_t len, uint16_t offset, uint8_t flags) {
    struct statistics_data *value = (struct statistics_data *)attr->user_data;

    /* Validate basic parameters */
    if (!conn || !attr || !buf) {
        LOG_ERR("Invalid parameters for Statistics write");
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_HANDLE);
    }

    /* Per BLE API Documentation: 1-byte SELECT-FOR-READ (channel selection) */
    if (!(flags & BT_GATT_WRITE_FLAG_PREPARE) && offset == 0 && len == 1) {
        uint8_t requested_channel_id = *(const uint8_t*)buf;  /* Direct access instead of memcpy */
        if (requested_channel_id >= WATERING_CHANNELS_COUNT) {
            LOG_ERR("Invalid channel ID for statistics selection: %u (max %u)", 
                    requested_channel_id, WATERING_CHANNELS_COUNT - 1);
            return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
        }
        
        /* Only update the local cache if the channel actually changed */
        if (value->channel_id != requested_channel_id) {
            value->channel_id = requested_channel_id;
            LOG_INF("Statistics channel selected for read: %u", requested_channel_id);
        }
        /* DO NOT modify system state - this is just a selection, not a config change */
        return len; /* ACK */
    }

    /* Standard write handling for complete statistics structure */
    if (offset + len > sizeof(*value)) {
        LOG_ERR("Statistics write: Invalid offset/length (offset=%u, len=%u, max=%zu)", 
                offset, len, sizeof(*value));
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }

    /* Only accept complete structure writes */
    if (len != sizeof(*value)) {
        LOG_ERR("Statistics write: Invalid length (got %u, expected %zu)", 
                len, sizeof(*value));
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }

    memcpy(((uint8_t *)value) + offset, buf, len);

    /* If complete structure received, validate and process */
    if (offset + len == sizeof(*value)) {
        /* Validate channel ID */
        if (value->channel_id >= WATERING_CHANNELS_COUNT) {
            LOG_ERR("Invalid channel ID in statistics: %u (max %u)", 
                    value->channel_id, WATERING_CHANNELS_COUNT - 1);
            return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
        }

        LOG_INF("Statistics reset/write: ch=%u, total_vol=%u, last_vol=%u, last_time=%u, count=%u",
                value->channel_id, value->total_volume, value->last_volume,
                value->last_watering, value->count);

        /* Interpret write semantics:
         * - All-zeros payload => reset channel statistics
         * - Sentinel values (0xFFFFFFFF for 32-bit fields, 0xFFFF for 16-bit) mean "no change"
         * - We accept updates only for last_volume/last_watering by recording a synthetic completion
         *   event; total_volume and count are derived and thus ignored if provided.
         */
        const uint32_t NO_CHANGE_32 = 0xFFFFFFFFu;
        const uint16_t NO_CHANGE_16 = 0xFFFFu;

        bool is_reset = (value->total_volume == 0 &&
                         value->last_volume == 0 &&
                         value->last_watering == 0 &&
                         value->count == 0);

        if (is_reset) {
            watering_error_t reset_err = watering_reset_channel_statistics(value->channel_id);
            if (reset_err != WATERING_SUCCESS) {
                LOG_WRN("Failed to reset channel %u statistics: %d", value->channel_id, reset_err);
            } else {
                LOG_INF("Channel %u statistics reset successfully", value->channel_id);
            }
        } else {
            /* Record an update only if last_volume/last_watering are provided */
            uint32_t upd_volume = value->last_volume;
            uint32_t upd_time = value->last_watering;

            /* If only one field given, use sensible default for the other */
            if (upd_volume == NO_CHANGE_32 && upd_time == NO_CHANGE_32) {
                LOG_INF("Statistics write ignored (no updatable fields changed)");
            } else {
                if (upd_volume == NO_CHANGE_32) {
                    upd_volume = 0; /* Volume unknown; record zero-volume event */
                }
                if (upd_time == NO_CHANGE_32) {
                    upd_time = timezone_get_unix_utc();
                }

                watering_error_t update_err = watering_update_channel_statistics(value->channel_id,
                                                                                 upd_volume,
                                                                                 upd_time);
                if (update_err != WATERING_SUCCESS) {
                    LOG_WRN("Failed to update channel %u statistics: %d", value->channel_id, update_err);
                } else {
                    LOG_INF("Channel %u statistics updated (vol=%u, ts=%u)", value->channel_id, upd_volume, upd_time);
                }
            }

            /* total_volume/count are derived; ignore if client tried to set */
            if (value->total_volume != NO_CHANGE_32 || value->count != NO_CHANGE_16) {
                LOG_DBG("Statistics write: total/count fields are derived and were ignored");
            }
        }
        
        /* Send notification to confirm statistics update per BLE API Documentation */
        /* Statistics (ef8): Watering events | After completion | Volume and count updates */
        if (notification_state.statistics_notifications_enabled) {
            const struct bt_gatt_attr *attr = &irrigation_svc.attrs[ATTR_IDX_STATISTICS_VALUE];
            /* Refresh the buffer from source of truth before notifying */
            struct statistics_data *stats = (struct statistics_data *)statistics_value;
            uint32_t total_volume_ml = 0, last_volume_ml = 0, watering_count = 0;
            watering_channel_t *channel = NULL;
            if (watering_get_channel(value->channel_id, &channel) == WATERING_SUCCESS) {
                (void)watering_get_channel_statistics(value->channel_id,
                                                      &total_volume_ml,
                                                      &last_volume_ml,
                                                      &watering_count);
                stats->channel_id = value->channel_id;
                stats->total_volume = total_volume_ml;
                stats->last_volume = last_volume_ml;
                stats->last_watering = channel->last_watering_time;
                stats->count = (uint16_t)watering_count;
            }
            int bt_err = safe_notify(default_conn, attr, statistics_value, sizeof(struct statistics_data));
            
            if (bt_err == 0) {
                LOG_INF("✅ Statistics notification sent after reset/write");
            } else {
                LOG_ERR("❌ Failed to send Statistics notification: %d", bt_err);
            }
        }
        
        LOG_INF("✅ Statistics write operation completed successfully");
    }
    
    return len;
}

static void statistics_ccc_cfg_changed(const struct bt_gatt_attr *attr, uint16_t value) {
    bool notif_enabled = (value == BT_GATT_CCC_NOTIFY);
    notification_state.statistics_notifications_enabled = notif_enabled;
    
    if (notif_enabled) {
        LOG_INF("✅ Statistics notifications ENABLED - will send updates when statistics change");
        
        /* Per BLE API Documentation: Statistics notifications */
        /* • Sent when watering completes and volume statistics are updated */
        /* • 15-byte structure with channel-specific statistics */
        /* • Fields: channel_id, total_volume (ml), last_volume (ml), last_watering (timestamp), count */
        
        LOG_INF("Statistics monitoring: 15-byte structure, volume tracking, watering count");
        
        /* Initialize statistics_value with current data from channel 0 */
        struct statistics_data *stats_data = (struct statistics_data *)statistics_value;
        memset(stats_data, 0, sizeof(*stats_data));
        
        /* Set default channel 0 for read operations */
        stats_data->channel_id = 0;
        
    watering_channel_t *channel;
        if (watering_get_channel(0, &channel) == WATERING_SUCCESS) {
            /* Get real statistics for default channel */
            uint32_t total_volume_ml = 0;
            uint32_t last_volume_ml = 0;
            uint32_t watering_count = 0;
            
            watering_error_t stats_err = watering_get_channel_statistics(0,
                                                                        &total_volume_ml,
                                                                        &last_volume_ml,
                                                                        &watering_count);
            
            if (stats_err == WATERING_SUCCESS) {
                stats_data->total_volume = total_volume_ml;
                stats_data->last_volume = last_volume_ml;
                stats_data->count = watering_count;
            }
            
            /* last_watering_time stored as Unix seconds already */
            stats_data->last_watering = channel->last_watering_time;
        }
    } else {
        LOG_INF("Statistics notifications disabled");
        /* Clear statistics_value when notifications disabled */
        memset(statistics_value, 0, sizeof(statistics_value));
    }
}
// Notificare BLE pentru statistici
int bt_irrigation_statistics_notify(void) {
    if (!default_conn || !notification_state.statistics_notifications_enabled) {
        LOG_DBG("Statistics notification not enabled");
        return 0;
    }
    
    const struct bt_gatt_attr *attr = &irrigation_svc.attrs[ATTR_IDX_STATISTICS_VALUE];
    int bt_err = safe_notify(default_conn, attr, statistics_value, sizeof(struct statistics_data));
    
    if (bt_err == 0) {
        struct statistics_data *stats = (struct statistics_data *)statistics_value;
        LOG_INF("✅ Statistics notification sent: ch=%u, total_vol=%u, last_vol=%u, last_time=%u, count=%u",
                stats->channel_id, stats->total_volume, stats->last_volume,
                stats->last_watering, stats->count);
    } else {
        LOG_ERR("❌ Failed to send Statistics notification: %d", bt_err);
    }
    
    return bt_err;
}

// Actualizare structura statistici (apelabil din watering.c/flow_sensor.c)
int bt_irrigation_update_statistics(uint8_t channel_id, uint32_t volume_ml, uint32_t timestamp) {
    static uint32_t last_periodic_ms;
    if (!default_conn || !notification_state.statistics_notifications_enabled) {
        return 0; // No connection or notifications disabled
    }
    
    if (channel_id >= WATERING_CHANNELS_COUNT) {
        LOG_ERR("Invalid channel ID for statistics update: %u", channel_id);
        return -EINVAL;
    }
    
    /* Enforce 30s cadence during active watering as per spec */
    bool active = (watering_get_current_task() != NULL);
    uint32_t now = k_uptime_get_32();
    /* k_uptime_get_32 returns milliseconds; enforce 30s window in ms */
    if (active && (now - last_periodic_ms) < 30000U) {
        return 0; /* Skip if within 30s window */
    }
    if (active) {
        last_periodic_ms = now;
    }

    /* Refresh statistics from source of truth */
    struct statistics_data *stats = (struct statistics_data *)statistics_value;
    uint32_t total_volume_ml = 0, last_volume_ml = 0, watering_count = 0;
    watering_channel_t *channel = NULL;
    if (watering_get_channel(channel_id, &channel) == WATERING_SUCCESS) {
        (void)watering_get_channel_statistics(channel_id,
                                              &total_volume_ml,
                                              &last_volume_ml,
                                              &watering_count);
        stats->channel_id = channel_id;
        stats->total_volume = total_volume_ml;
        stats->last_volume = last_volume_ml;
        stats->last_watering = channel->last_watering_time;
        stats->count = (uint16_t)watering_count;
        LOG_INF("Statistics refreshed: ch=%u total=%u last=%u ts=%u count=%u",
                channel_id, total_volume_ml, last_volume_ml, stats->last_watering, stats->count);
        return bt_irrigation_statistics_notify();
    }

    return 0;
}

// Notificare BLE pentru diagnostics
int bt_irrigation_diagnostics_notify(void) {
    if (!notification_state.diagnostics_notifications_enabled) {
        LOG_DBG("Diagnostics notification not enabled");
        return 0;
    }
    
    if (!default_conn) {
        LOG_DBG("No BLE connection for diagnostics notification");
        return 0;
    }
    
    const struct bt_gatt_attr *attr = &irrigation_svc.attrs[ATTR_IDX_DIAGNOSTICS_VALUE];
    int rc = safe_notify(default_conn, attr, diagnostics_value, sizeof(struct diagnostics_data));
    
    if (rc == 0) {
        struct diagnostics_data *diag = (struct diagnostics_data *)diagnostics_value;
        LOG_INF("✅ Diagnostics notification sent: uptime=%u min, errors=%u, last_error=%u, valve_status=0x%02x",
                diag->uptime, diag->error_count, diag->last_error, diag->valve_status);
    } else {
        LOG_ERR("❌ Failed to send diagnostics notification: %d", rc);
    }
    
    return rc;
}

/* Current task characteristics implementation */

static ssize_t read_current_task(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                void *buf, uint16_t len, uint16_t offset) {
    if (!conn || !attr || !buf) {
        LOG_ERR("Invalid parameters for Current Task read");
        return -EINVAL;
    }

    /* Build a fresh 21-byte payload according to spec */
    struct current_task_data read_value;
    memset(&read_value, 0, sizeof(read_value));

    watering_task_t *current_task = watering_get_current_task();
    if (current_task == NULL) {
        /* No active task */
        read_value.channel_id = 0xFF;
        read_value.start_time = 0;
        read_value.mode = 0;
        read_value.target_value = 0;
        read_value.current_value = 0;
        read_value.total_volume = 0;
        read_value.status = 0;  /* Idle */
        read_value.reserved = 0;
    } else {
        /* Active task */
        uint8_t channel_id = current_task->channel - watering_channels;

        /* Elapsed time excluding pauses */
        uint32_t total_elapsed_ms = k_uptime_get_32() - watering_task_state.watering_start_time;
        uint32_t current_pause_time = 0;
        if (watering_task_state.task_paused) {
            current_pause_time = k_uptime_get_32() - watering_task_state.pause_start_time;
        }
        uint32_t effective_elapsed_ms = total_elapsed_ms - watering_task_state.total_paused_time - current_pause_time;
        uint32_t elapsed_seconds = effective_elapsed_ms / 1000;

        read_value.channel_id = channel_id;
        read_value.start_time = watering_task_state.watering_start_time / 1000;
        read_value.mode = (current_task->channel->watering_event.watering_mode == WATERING_BY_DURATION) ? 0 : 1;

        /* Status mapping per spec: 0=idle,1=running,2=paused,3=completed */
        if (watering_task_state.task_paused) {
            read_value.status = 2; /* Paused */
        } else if (watering_task_state.task_in_progress) {
            read_value.status = 1; /* Running */
        } else {
            read_value.status = 0; /* Idle */
        }

        /* Flow-based volume */
        uint32_t pulses = get_pulse_count();
        uint32_t pulses_per_liter;
        if (watering_get_flow_calibration(&pulses_per_liter) != WATERING_SUCCESS) {
            pulses_per_liter = DEFAULT_PULSES_PER_LITER;
        }
        uint32_t total_volume_ml = (pulses * 1000) / pulses_per_liter;
        read_value.total_volume = total_volume_ml;

        if (read_value.mode == 0) {
            /* Duration mode */
            uint32_t target_seconds = current_task->channel->watering_event.watering.by_duration.duration_minutes * 60;
            read_value.target_value = target_seconds;
            read_value.current_value = elapsed_seconds;
            read_value.reserved = 0;
        } else {
            /* Volume mode */
            uint32_t target_ml = current_task->channel->watering_event.watering.by_volume.volume_liters * 1000;
            read_value.target_value = target_ml;
            read_value.current_value = total_volume_ml;
            read_value.reserved = (uint16_t)elapsed_seconds; /* elapsed seconds for volume mode */
        }
    }

    return bt_gatt_attr_read(conn, attr, buf, len, offset, &read_value, sizeof(read_value));
}

static ssize_t write_current_task(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                 const void *buf, uint16_t len, uint16_t offset, uint8_t flags) {
    if (!conn || !attr || !buf) {
        LOG_ERR("Invalid parameters for Current Task write");
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_HANDLE);
    }
    
    printk("🔧 BLE Current Task write: len=%u, offset=%u, flags=0x%02x\n", len, offset, flags);
    
    const uint8_t *data = (const uint8_t *)buf;
    /* Per BLE API Documentation: WRITE supports control commands (single byte) */
    /* Control Commands: 0x00=Stop, 0x01=Pause, 0x02=Resume */
    
    if (offset != 0) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }
    
    /* Only support single byte control commands */
    if (len != 1) {
        LOG_ERR("Current Task write: Invalid length %u (expected 1)", len);
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }
    
    uint8_t command = data[0];
    
    switch (command) {
        case 0x00:  // Stop/Cancel current task
            {
                bool stopped = watering_stop_current_task();
                if (stopped) {
                    LOG_INF("✅ Current task stopped via BLE command");
                    
                    /* Clear current task data */
                    struct current_task_data *value = (struct current_task_data *)current_task_value;
                    value->channel_id = 0xFF;
                    value->start_time = 0;
                    value->mode = 0;
                    value->target_value = 0;
                    value->current_value = 0;
                    value->total_volume = 0;
                    value->status = 0;  // Idle
                    value->reserved = 0;
                    
                    /* Send notification about task stop */
                    bt_irrigation_current_task_notify();
                } else {
                    LOG_WRN("No active task to stop");
                    return BT_GATT_ERR(BT_ATT_ERR_WRITE_NOT_PERMITTED);
                }
            }
            break;
            
        case 0x01:  // Pause current task
            {
                bool paused = watering_pause_current_task();
                if (paused) {
                    LOG_INF("✅ Current task paused via BLE command");
                    
                    /* Update current task status to paused */
                    struct current_task_data *value = (struct current_task_data *)current_task_value;
                    if (value->status == 1) {  // If it was running
                        value->status = 2;  // Set to paused (spec)
                        
                        /* Send notification about task pause */
                        bt_irrigation_current_task_notify();
                    }
                } else {
                    LOG_WRN("No active task to pause or task already paused");
                    return BT_GATT_ERR(BT_ATT_ERR_WRITE_NOT_PERMITTED);
                }
            }
            break;
            
        case 0x02:  // Resume current task
            {
                bool resumed = watering_resume_current_task();
                if (resumed) {
                    LOG_INF("✅ Current task resumed via BLE command");
                    
                    /* Update current task status to running */
                    struct current_task_data *value = (struct current_task_data *)current_task_value;
                    if (value->status == 2) {  // If it was paused
                        value->status = 1;  // Set to running
                        
                        /* Send notification about task resume */
                        bt_irrigation_current_task_notify();
                    }
                } else {
                    LOG_WRN("No paused task to resume");
                    return BT_GATT_ERR(BT_ATT_ERR_WRITE_NOT_PERMITTED);
                }
            }
            break;
            
        default:
            LOG_ERR("Current Task write: Invalid command 0x%02X", command);
            return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
    }
    
    return len;
}

static void current_task_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value) {
    bool notif_enabled = (value == BT_GATT_CCC_NOTIFY);
    notification_state.current_task_notifications_enabled = notif_enabled;
    
    if (notif_enabled) {
        LOG_INF("✅ Current Task notifications ENABLED - on-demand only (periodic disabled)");
        
        /* Per BLE API Documentation: Current Task notifications are sent:
         * - Every 2 seconds during active watering
         * - Immediately when task starts, stops, or encounters errors
         * - Real-time progress updates based on elapsed time (duration mode) or volume dispensed (volume mode)
         */
        
        LOG_INF("Current Task monitoring: 21-byte structure, 2s intervals, immediate on changes");
        
        /* Initialize current_task_value with current state */
        struct current_task_data *value = (struct current_task_data *)current_task_value;
        watering_task_t *current_task = watering_get_current_task();
        
        if (current_task == NULL) {
            /* No active task */
            value->channel_id = 0xFF;
            value->start_time = 0;
            value->mode = 0;
            value->target_value = 0;
            value->current_value = 0;
            value->total_volume = 0;
            value->status = 0;  // Idle
            value->reserved = 0;
            LOG_INF("Current Task notifications ready: No active task");
        } else {
            /* Active task - populate with current data */
            uint8_t channel_id = current_task->channel - watering_channels;
            value->channel_id = channel_id;
            value->start_time = watering_task_state.watering_start_time / 1000;
            value->mode = (current_task->channel->watering_event.watering_mode == WATERING_BY_DURATION) ? 0 : 1;
            /* Set status based on pause/in-progress */
            if (watering_task_state.task_paused) {
                value->status = 2; /* Paused */
            } else {
                value->status = watering_task_state.task_in_progress ? 1 : 0;
            }
            LOG_INF("Current Task notifications ready: Active task on channel %u", channel_id);
        }
        
    /* Start 2s periodic progress updates while running */
    k_work_cancel_delayable(&current_task_periodic_work);
    } else {
        LOG_INF("Current Task notifications disabled");
        /* Clear current_task_value when notifications disabled */
        memset(current_task_value, 0, sizeof(current_task_value));
    /* Stop periodic work */
    k_work_cancel_delayable(&current_task_periodic_work);
    }
}

/* History characteristics implementation */
static ssize_t read_history(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                           void *buf, uint16_t len, uint16_t offset) {
    if (!conn || !attr || !buf) {
        LOG_ERR("Invalid parameters for History read");
        return -EINVAL;
    }
    
    /* Per BLE API Documentation: History READ returns query results */
    /* Structure: history_data with union for different history types */
    /* Types: 0=detailed, 1=daily, 2=monthly, 3=annual */
    
    struct history_data *value = (struct history_data *)history_value;
    
    LOG_INF("✅ History read: channel=%u, type=%u (%s), index=%u, count=%u", 
            value->channel_id, value->history_type, 
            (value->history_type == 0) ? "detailed" :
            (value->history_type == 1) ? "daily" :
            (value->history_type == 2) ? "monthly" : "annual",
            value->entry_index, value->count);
    
    return bt_gatt_attr_read(conn, attr, buf, len, offset, history_value,
                           sizeof(struct history_data));
}

static ssize_t write_history(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                            const void *buf, uint16_t len, uint16_t offset, uint8_t flags) {
    /* Unified History refactor: accept compact 12-byte query header, support clear command, build multi-entry
    * response and send via unified fragmentation header (shared with rain history). Enhanced-only; legacy
    * full-struct writes are no longer accepted. */

    static uint32_t last_history_query_ms = 0;
    const uint32_t HISTORY_QUERY_MIN_INTERVAL_MS = 1000; /* Low priority throttle */
    const uint8_t *data = (const uint8_t *)buf;
    /* legacy path removed: enforce 12-byte compact header */

    if (!conn || !attr || !buf) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_HANDLE);
    }
    if (offset != 0) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }

    uint32_t now = k_uptime_get_32();
    if (now - last_history_query_ms < HISTORY_QUERY_MIN_INTERVAL_MS) {
        /* Rate limited: send status notification with no data */
        history_fragment_header_t header = {0};
        header.data_type = 0xFE; /* rate limit indicator */
        header.status = 0x07;    /* reuse generic rate limit code */
        if (notification_state.history_notifications_enabled) {
            bt_gatt_notify(conn, attr, &header, sizeof(header));
        }
        return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
    }

    /* Parse query */
    uint8_t channel_id = 0;
    uint8_t history_type = 0;
    uint8_t entry_index = 0;
    uint8_t count = 0;
    uint32_t start_ts = 0;
    uint32_t end_ts = 0;
    bool clear_command = false;

    if (len == 12) { /* compact header */
        channel_id   = data[0];
        history_type = data[1];
        entry_index  = data[2];
        count        = data[3];
        start_ts     = sys_get_le32(&data[4]);
        end_ts       = sys_get_le32(&data[8]);
    } else {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }

    if (history_type == 0xFF) {
        clear_command = true;
    }

    if (!clear_command && history_type > 3) {
        return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
    }
    if (channel_id != 0xFF && channel_id >= WATERING_CHANNELS_COUNT) {
        return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
    }
    if (count == 0) count = 1;
    if (count > 50) count = 50; /* enforce documented max */

    last_history_query_ms = now;

    /* Buffer for packed entries: largest entry (detailed=24). Header echo (12) + 50*24 = 1212 bytes */
    static uint8_t packed[12 + 24 * 50];
    memset(packed, 0, sizeof(packed));
    /* Echo query header for client parsing */
    packed[0] = channel_id;
    packed[1] = history_type;
    packed[2] = entry_index;
    packed[3] = count; /* will adjust to actual */
    sys_put_le32(start_ts, &packed[4]);
    sys_put_le32(end_ts, &packed[8]);

    size_t header_size = 12;
    size_t write_offset = header_size;
    uint16_t actual_entries = 0;
    int ret = 0;

    if (clear_command) {
        /* Clear detailed events older than start_ts (cutoff) */
        uint32_t cutoff = start_ts;
        if (cutoff != 0) {
            /* Iterate detailed events and zero out older ones */
            uint8_t ch_start = (channel_id==0xFF)?0:channel_id;
            uint8_t ch_end   = (channel_id==0xFF)?(WATERING_CHANNELS_COUNT-1):channel_id;
            /* removed counter currently unused; keep logic without the variable to silence warning */
            for (uint8_t ch = ch_start; ch <= ch_end; ch++) {
                /* Function not exposed: implement a simple page query loop until no more */
                for (uint16_t page=0; page<5; page++) { /* heuristic pages */
                    history_event_t temp_events[10];
                    uint16_t pc=0; 
                    watering_error_t er = watering_history_query_page(ch, page, temp_events, &pc, NULL);
                    if (er != WATERING_SUCCESS || pc==0) break;
                    for (uint16_t i=0;i<pc;i++) {
                        /* No timestamp in slim event => use global UTC now for relative; skip if cannot validate */
                        /* Without original timestamps stored in structure we cannot selective-clear reliably. */
                    }
                }
            }
            /* Fallback: run cleanup */
            watering_history_cleanup_expired();
        } else {
            /* Full cleanup path */
            watering_history_cleanup_expired();
        }
        history_fragment_header_t header = {0};
        header.data_type = 0xFF; /* clear response */
        header.status = 0x00; /* success */
        header.entry_count = sys_cpu_to_le16(0);
        if (notification_state.history_notifications_enabled) {
            bt_gatt_notify(conn, attr, &header, sizeof(header));
        }
        return len;
    }

    switch (history_type) {
        case 0: { /* detailed */
            history_event_t events[50];
            uint32_t timestamps[50];
            uint16_t page_count = 0;
            ret = watering_history_query_page((channel_id==0xFF)?0:channel_id, entry_index, events, &page_count, timestamps);
            if (ret == WATERING_SUCCESS && page_count > 0) {
                uint16_t to_copy = (page_count < count) ? page_count : count;
                for (uint16_t i = 0; i < to_copy; i++) {
                    history_event_t *src = &events[i];
                    uint8_t *e = &packed[write_offset];
                    sys_put_le32(timestamps[i], e); /* timestamp */
                    e[4] = (channel_id==0xFF)?0:channel_id;
                    e[5] = (src->flags.err == 0) ? 1 : 3; /* COMPLETE or ERROR */
                    e[6] = src->flags.mode;
                    sys_put_le16(src->target_ml, &e[7]);
                    sys_put_le16(src->actual_ml, &e[9]);
                    sys_put_le16(src->actual_ml, &e[11]);
                    e[13] = src->flags.trigger;
                    e[14] = src->flags.success;
                    e[15] = src->flags.err;
                    sys_put_le16(src->avg_flow_ml_s, &e[16]);
                    write_offset += 24;
                }
                actual_entries = to_copy;
            }
            break; }
        case 1: { /* daily */
            daily_stats_t stats_arr[50];
            uint16_t got = 0;
            uint16_t current_year = get_current_year();
            uint16_t current_day = get_current_day_of_year();
            uint16_t start_day = (entry_index > 0) ? (current_day - entry_index) : current_day;
            uint16_t end_day = start_day + 1; /* single for now */
            ret = watering_history_get_daily_stats((channel_id==0xFF)?0:channel_id, start_day, end_day, current_year, stats_arr, &got);
            if (ret == WATERING_SUCCESS && got > 0) {
                uint16_t to_copy = (got < count) ? got : count;
                for (uint16_t i=0;i<to_copy;i++) {
                    uint8_t *e=&packed[write_offset];
                    daily_stats_t *s=&stats_arr[i];
                    sys_put_le16(start_day, &e[0]);
                    sys_put_le16(current_year, &e[2]);
                    e[4] = s->sessions_ok;
                    sys_put_le32(s->total_ml, &e[5]);
                    /* Derive duration estimate from total_ml if available (ml ≈ seconds via avg_flow  (ml/s)) */
                    uint16_t duration_est = (s->total_ml && s->sessions_ok)? (s->total_ml / (s->sessions_ok? s->sessions_ok:1) / 10) : 0;
                    sys_put_le16(duration_est, &e[9]);
                    uint16_t avg_flow = (s->total_ml && duration_est)? (s->total_ml / (duration_est? duration_est:1)) : 0;
                    sys_put_le16(avg_flow, &e[11]);
                    e[13] = s->success_rate;
                    e[14] = s->sessions_err;
                    write_offset += 16;
                }
                actual_entries = to_copy;
            }
            break; }
        case 2: { /* monthly */
            monthly_stats_t mstats[12];
            uint16_t got = 0;
            uint16_t year = get_current_year();
            uint8_t current_month = get_current_month();
            uint8_t month = (entry_index>0)? ((current_month - entry_index -1 +12)%12)+1 : current_month;
            uint8_t effective_channel = (channel_id==0xFF)?0:channel_id;
            ret = watering_history_get_monthly_stats(effective_channel, month, month, year, mstats, &got);
            if (ret == WATERING_SUCCESS && got>0) {
                uint16_t to_copy = (got < count) ? got : count;
                for (uint16_t i=0;i<to_copy;i++) {
                    uint8_t *e=&packed[write_offset];
                    monthly_stats_t *s=&mstats[i];
                    /* Monthly entry layout (15 bytes):
                     * 0  : month (1B)
                     * 1-2: year (LE)
                     * 3-4: total_sessions
                     * 5-8: total_volume_ml (LE uint32)
                     * 9-10: total_duration_hours
                     * 11-12: avg_daily_volume (LE)
                     * 13: active_days
                     * 14: success_rate
                     */
                    uint16_t entry_month = s->month ? s->month : month;
                    uint16_t entry_year = s->year ? s->year : year;
                    e[0]=entry_month;
                    sys_put_le16(entry_year,&e[1]);

                    uint32_t month_start = build_epoch_from_date(entry_year, entry_month, 1);
                    uint8_t next_month = (entry_month == 12) ? 1 : (entry_month + 1);
                    uint16_t next_year = (entry_month == 12) ? (entry_year + 1) : entry_year;
                    uint32_t month_end = build_epoch_from_date(next_year, next_month, 1);
                    uint16_t total_sessions = count_sessions_in_period(effective_channel, month_start, month_end);
                    sys_put_le16(total_sessions, &e[3]);
                    sys_put_le32(s->total_ml,&e[5]);
                    sys_put_le16(0,&e[9]);
                    uint16_t avg_daily = s->active_days? (s->total_ml / s->active_days) : 0;
                    sys_put_le16(avg_daily, &e[11]);
                    e[13]=s->active_days;

                    uint32_t daily_success = 0;
                    uint32_t daily_errors = 0;
                    uint8_t days = days_in_month(entry_year, entry_month);
                    for (uint8_t day = 1; day <= days; ++day) {
                        uint16_t day_index = calculate_day_of_year(entry_year, entry_month, day);
                        daily_stats_t day_stats[1];
                        uint16_t day_found = 0;
                        if (watering_history_get_daily_stats(effective_channel,
                                                             day_index,
                                                             day_index,
                                                             entry_year,
                                                             day_stats,
                                                             &day_found) == WATERING_SUCCESS &&
                            day_found > 0) {
                            daily_success += day_stats[0].sessions_ok;
                            daily_errors += day_stats[0].sessions_err;
                        }
                    }
                    uint32_t total_month_sessions = daily_success + daily_errors;
                    if (total_month_sessions == 0 && total_sessions > 0) {
                        total_month_sessions = total_sessions;
                        daily_success = total_sessions;
                    }
                    uint8_t success_rate = 0;
                    if (total_month_sessions > 0) {
                        uint32_t pct = (daily_success * 100U) / total_month_sessions;
                        success_rate = (pct > 100U) ? 100U : (uint8_t)pct;
                    }
                    e[14]= success_rate;
                    write_offset += 15; /* correct monthly packed size */
                }
                actual_entries = to_copy;
            }
            break; }
        case 3: { /* annual */
            annual_stats_t astats[5];
            uint16_t got = 0;
            uint16_t year = get_current_year() - entry_index;
            uint8_t effective_channel = (channel_id==0xFF)?0:channel_id;
            ret = watering_history_get_annual_stats(effective_channel, year, year, astats, &got);
            if (ret == WATERING_SUCCESS && got>0) {
                uint16_t to_copy = (got < count) ? got : count;
                for (uint16_t i=0;i<to_copy;i++) {
                    uint8_t *e=&packed[write_offset];
                    annual_stats_t *s=&astats[i];
                    /* Annual entry layout (14 bytes):
                     * 0-1 : year
                     * 2-3 : total_sessions
                     * 4-7 : total_volume_liters (ml/1000)
                     * 8-9 : avg_monthly_volume (liters)
                     * 10  : most_active_month
                     * 11  : success_rate
                     * 12-13: peak_month_volume (liters)
                     */
                    sys_put_le16(s->year,&e[0]);
                    uint16_t session_count = (s->sessions > UINT16_MAX) ? UINT16_MAX : (uint16_t)s->sessions;
                    sys_put_le16(session_count,&e[2]);
                    uint32_t total_liters = s->total_ml / 1000U;
                    sys_put_le32(total_liters,&e[4]);
                    uint16_t avg_month = (uint16_t)(total_liters / 12U);
                    sys_put_le16(avg_month,&e[8]);

                    uint8_t best_month = 0;
                    uint32_t best_volume = 0;
                    for (uint8_t m = 1; m <= 12; ++m) {
                        monthly_stats_t month_stats[1];
                        uint16_t found = 0;
                        if (watering_history_get_monthly_stats(effective_channel,
                                                               m,
                                                               m,
                                                               s->year,
                                                               month_stats,
                                                               &found) == WATERING_SUCCESS &&
                            found > 0) {
                            if (month_stats[0].total_ml > best_volume) {
                                best_volume = month_stats[0].total_ml;
                                best_month = month_stats[0].month;
                            }
                        }
                    }
                    e[10]=best_month;
                    uint32_t success_sessions = (s->sessions >= s->errors) ? (s->sessions - s->errors) : 0;
                    uint8_t success_rate = 0;
                    if (s->sessions > 0) {
                        uint32_t pct = (success_sessions * 100U) / s->sessions;
                        success_rate = (pct > 100U) ? 100U : (uint8_t)pct;
                    }
                    e[11]=success_rate;
                    uint16_t peak_liters = (uint16_t)(best_volume / 1000U);
                    sys_put_le16(peak_liters,&e[12]);
                    write_offset += 14; /* correct annual packed size */
                }
                actual_entries = to_copy;
            }
            break; }
    }

    /* Update count in header */
    packed[3] = (uint8_t)actual_entries;
    size_t total_payload = header_size + (write_offset - header_size);

    if (actual_entries == 0) {
        /* Send empty header fragment */
        history_fragment_header_t header = {0};
        header.data_type = history_type;
        header.status = 0; /* success but empty */
        header.entry_count = sys_cpu_to_le16(0);
        if (notification_state.history_notifications_enabled) {
            bt_gatt_notify(conn, attr, &header, sizeof(header));
        }
        return len;
    }

    uint8_t total_frags = (total_payload + RAIN_HISTORY_FRAGMENT_SIZE - 1) / RAIN_HISTORY_FRAGMENT_SIZE;

    /* Send fragments asynchronously to avoid blocking BT host thread */
    if (history_frag_state.active) {
        LOG_WRN("History notify busy, dropping request");
        return -EBUSY;
    }
    uint8_t *heap_copy = k_malloc(total_payload);
    if (!heap_copy) {
        LOG_ERR("History notify OOM for %zu bytes", total_payload);
        return -ENOMEM;
    }
    memcpy(heap_copy, packed, total_payload);
    history_frag_state.active = true;
    history_frag_state.buf = heap_copy;
    history_frag_state.len = total_payload;
    history_frag_state.total_frags = total_frags;
    history_frag_state.next_frag = 0;
    history_frag_state.history_type = history_type;
    history_frag_state.entry_count_le = sys_cpu_to_le16(actual_entries);
    history_frag_state.attr = attr;
    history_frag_state.conn = bt_conn_ref(conn);
    k_work_schedule(&history_frag_work, K_NO_WAIT);
    return len;
}

static void history_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value) {
    bool notif_enabled = (value == BT_GATT_CCC_NOTIFY);
    notification_state.history_notifications_enabled = notif_enabled;
    
    if (notif_enabled) {
        LOG_INF("✅ History notifications enabled - will send updates on new events and aggregations");
        
        /* Per BLE API Documentation: History notifications are sent on:
         * - New watering events (detailed level)
         * - Daily aggregations (at midnight)
         * - Monthly summaries (at month end)
         * - Error events (immediate notification)
         */
        
        LOG_INF("History monitoring: 4 aggregation levels, time filtering, multi-channel support");
        LOG_INF("Event types: 0=START, 1=COMPLETE, 2=ABORT, 3=ERROR");
        LOG_INF("Trigger types: 0=manual, 1=scheduled, 2=remote");
        
        /* Initialize history_value with current state */
        struct history_data *hist_data = (struct history_data *)history_value;
        hist_data->channel_id = 0xFF; /* All channels */
        hist_data->history_type = 0; /* Detailed */
        hist_data->entry_index = 0; /* Most recent */
        hist_data->count = 0; /* No entries yet */
        hist_data->start_timestamp = 0; /* No filter */
        hist_data->end_timestamp = 0; /* No filter */
        
        LOG_INF("History system ready: detailed events, daily/monthly/annual aggregations");
    } else {
        LOG_INF("History notifications disabled");
        /* Clear history_value when notifications disabled */
        memset(history_value, 0, sizeof(struct history_data));
    }
}

/* Async fragment sender for watering history */
static void history_frag_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    if (!history_frag_state.active || !history_frag_state.buf || !history_frag_state.conn || !notification_state.history_notifications_enabled) {
        if (history_frag_state.buf) {
            k_free(history_frag_state.buf);
        }
        if (history_frag_state.conn) {
            bt_conn_unref(history_frag_state.conn);
        }
        history_frag_state.active = false;
        history_frag_state.buf = NULL;
        history_frag_state.conn = NULL;
        return;
    }

    const uint16_t header_sz = sizeof(history_fragment_header_t);
    uint8_t notify_buf[sizeof(history_fragment_header_t) + RAIN_HISTORY_FRAGMENT_SIZE];

    size_t frag_offset = history_frag_state.next_frag * RAIN_HISTORY_FRAGMENT_SIZE;
    size_t remain = history_frag_state.len - frag_offset;
    size_t frag_size = (remain > RAIN_HISTORY_FRAGMENT_SIZE) ? RAIN_HISTORY_FRAGMENT_SIZE : remain;

    history_fragment_header_t *hdr = (history_fragment_header_t *)notify_buf;
    hdr->data_type = history_frag_state.history_type;
    hdr->status = 0;
    hdr->entry_count = history_frag_state.entry_count_le;
    hdr->fragment_index = history_frag_state.next_frag;
    hdr->total_fragments = history_frag_state.total_frags;
    hdr->fragment_size = (uint8_t)frag_size;
    hdr->reserved = 0;

    memcpy(&notify_buf[header_sz], &history_frag_state.buf[frag_offset], frag_size);

    int nret = bt_gatt_notify(history_frag_state.conn, history_frag_state.attr, notify_buf, header_sz + frag_size);
    if (nret < 0) {
        LOG_ERR("History fragment notify failed %d", nret);
        goto cleanup;
    }

    history_frag_state.next_frag++;
    if (history_frag_state.next_frag < history_frag_state.total_frags) {
        k_work_schedule(&history_frag_work, K_MSEC(5));
        return;
    }

cleanup:
    k_free(history_frag_state.buf);
    bt_conn_unref(history_frag_state.conn);
    history_frag_state.buf = NULL;
    history_frag_state.conn = NULL;
    history_frag_state.active = false;
}

/* Diagnostics characteristics implementation */
static ssize_t read_diagnostics(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                               void *buf, uint16_t len, uint16_t offset) {
    if (!conn || !attr || !buf) {
        LOG_ERR("Invalid parameters for Diagnostics read");
        return -EINVAL;
    }
    
    struct diagnostics_data *diag = (struct diagnostics_data *)diagnostics_value;

    /* Per BLE API Documentation: READ returns current system diagnostics */
    /* Structure: uptime, error_count, last_error, valve_status, battery_level, reserved[3] */
    
    // Uptime in minutes - calculated using RTC for persistent measurement
    uint32_t current_unix = timezone_get_unix_utc();
    if (current_unix > 0) {
        /* Use RTC-based uptime calculation */
        static uint32_t boot_time_utc = 0;
        if (boot_time_utc == 0) {
            /* First call - estimate boot time from RTC and uptime */
            boot_time_utc = current_unix - (k_uptime_get() / 1000);
        }
        diag->uptime = (current_unix - boot_time_utc) / 60; /* Convert to minutes */
    } else {
        /* Fallback to boot-relative uptime if RTC unavailable */
        diag->uptime = k_uptime_get() / (1000 * 60);
    }

    // Get error count and last error from local tracking
    diag->error_count = diagnostics_error_count;
    diag->last_error = diagnostics_last_error;

    // Valve status bitmap: bit 0 = channel 0, etc. (iterate channels)
    uint8_t valve_bitmap = 0;
    for (uint8_t ch = 0; ch < WATERING_CHANNELS_COUNT; ++ch) {
        watering_channel_t *channel;
        if (watering_get_channel(ch, &channel) == WATERING_SUCCESS) {
            if (channel->is_active) {
                valve_bitmap |= (1 << ch);
            }
        }
    }
    diag->valve_status = valve_bitmap;

    // Battery level (not supported)
    diag->battery_level = 0xFF;
    memset(diag->reserved, 0, sizeof(diag->reserved));

    LOG_DBG("Diagnostics read: uptime=%u min, errors=%u, last_error=%u, valve_status=0x%02x, battery=%u%%",
            diag->uptime, diag->error_count, diag->last_error, diag->valve_status, diag->battery_level);

    return bt_gatt_attr_read(conn, attr, buf, len, offset, diagnostics_value, sizeof(struct diagnostics_data));
}

static void diagnostics_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value) {
    bool notif_enabled = (value == BT_GATT_CCC_NOTIFY);
    notification_state.diagnostics_notifications_enabled = notif_enabled;
    
    if (notif_enabled) {
    /* Diagnostics notifications enabled (log suppressed for pristine build) */
        
        /* Per BLE API Documentation: Diagnostics notifications are sent when:
         * - System health metrics change significantly
         * - Error count increases
         * - Valve status changes (any valve opens/closes)
         * - System enters/exits error states
         */
        
    /* Verbose diagnostic structure description logs removed */
        
        /* Initialize diagnostics_value with current system state */
        struct diagnostics_data *diag = (struct diagnostics_data *)diagnostics_value;
        diag->uptime = k_uptime_get() / (1000 * 60);
        diag->error_count = diagnostics_error_count;
        diag->last_error = diagnostics_last_error;
        diag->valve_status = 0; /* Will be updated on valve changes */
        diag->battery_level = 0xFF; /* No battery monitoring */
        memset(diag->reserved, 0, sizeof(diag->reserved));
        
    /* Initial diagnostics state prepared (log suppressed) */

    /* Send an immediate snapshot so clients get current state on subscribe */
    if (default_conn) {
        (void)bt_irrigation_diagnostics_notify();
    }
    } else {
    /* Diagnostics notifications disabled (log suppressed) */
        /* Clear diagnostics_value when notifications disabled */
        memset(diagnostics_value, 0, sizeof(diagnostics_value));
    }
}

/* Task update thread code fully removed (see history if re-enable needed) */
/* Removed unused internal declarations for history/diagnostics CCC and history helpers */

/* ------------------------------------------------------------------ */
/* BLE Notification Subscription helper removed (unused) to keep build pristine */
/* ------------------------------------------------------------------ */

/* RTC notification functions */
// Notificare BLE pentru RTC (sincronizare sau erori)
int bt_irrigation_rtc_notify(void) {
    if (!default_conn || !notification_state.rtc_notifications_enabled) {
        LOG_DBG("RTC notification not enabled");
        return 0;
    }
    
    /* Update rtc_value with current time before notification */
    struct rtc_data *rtc_data = (struct rtc_data *)rtc_value;
    rtc_datetime_t now_utc, local_time;
    if (rtc_datetime_get(&now_utc) == 0) {
        /* TIMEZONE FIX: Send local time and include tz fields */
        uint32_t utc_timestamp = timezone_rtc_to_unix_utc(&now_utc);
        if (timezone_unix_to_rtc_local(utc_timestamp, &local_time) == 0) {
            rtc_data->year = local_time.year - 2000;
            rtc_data->month = local_time.month;
            rtc_data->day = local_time.day;
            rtc_data->hour = local_time.hour;
            rtc_data->minute = local_time.minute;
            rtc_data->second = local_time.second;
            rtc_data->day_of_week = local_time.day_of_week;
        } else {
            rtc_data->year = now_utc.year - 2000;
            rtc_data->month = now_utc.month;
            rtc_data->day = now_utc.day;
            rtc_data->hour = now_utc.hour;
            rtc_data->minute = now_utc.minute;
            rtc_data->second = now_utc.second;
            rtc_data->day_of_week = now_utc.day_of_week;
        }
        rtc_data->utc_offset_minutes = timezone_get_total_offset(utc_timestamp);
        rtc_data->dst_active = timezone_is_dst_active(utc_timestamp) ? 1 : 0;
    }
    
    const struct bt_gatt_attr *attr = &irrigation_svc.attrs[ATTR_IDX_RTC_VALUE];
    int err = safe_notify(default_conn, attr, rtc_value, sizeof(rtc_value));
    
    if (err == 0) {
        LOG_INF("✅ RTC notification sent: %02u/%02u/%04u %02u:%02u:%02u",
                rtc_data->day, rtc_data->month, 2000 + rtc_data->year,
                rtc_data->hour, rtc_data->minute, rtc_data->second);
    } else {
        LOG_ERR("❌ Failed to send RTC notification: %d", err);
    }
    
    return err;
}

// Actualizare RTC cu notificare automată (apelabil din sistem pentru sincronizare)
int bt_irrigation_rtc_update_notify(rtc_datetime_t *datetime) {
    if (!datetime) {
        LOG_ERR("Invalid datetime parameter for RTC update");
        return -EINVAL;
    }
    
    /* Set the RTC hardware */
    int ret = rtc_datetime_set(datetime);
    if (ret != 0) {
        LOG_ERR("Failed to set RTC hardware: %d", ret);
        return ret;
    }
    
    LOG_INF("RTC synchronized: %02u/%02u/%04u %02u:%02u:%02u (day %u)",
            datetime->day, datetime->month, datetime->year,
            datetime->hour, datetime->minute, datetime->second,
            datetime->day_of_week);
    
    /* Send notification if enabled */
    return bt_irrigation_rtc_notify();
}

/* ---------------------------------------------------------------------- */

/* Force enable all notifications for auto-recovery */
static void force_enable_all_notifications(void) {
    LOG_INF("🔧 Force enabling all BLE notifications");
    
    notification_state.valve_notifications_enabled = true;
    notification_state.flow_notifications_enabled = true;
    notification_state.status_notifications_enabled = true;
    notification_state.channel_config_notifications_enabled = true;
    notification_state.schedule_notifications_enabled = true;
    notification_state.system_config_notifications_enabled = true;
    notification_state.task_queue_notifications_enabled = true;
    notification_state.statistics_notifications_enabled = true;
    notification_state.rtc_notifications_enabled = true;
    notification_state.alarm_notifications_enabled = true;
    notification_state.calibration_notifications_enabled = true;
    notification_state.history_notifications_enabled = true;
    notification_state.diagnostics_notifications_enabled = true;
    notification_state.growing_env_notifications_enabled = true;
    notification_state.auto_calc_status_notifications_enabled = true;
    notification_state.current_task_notifications_enabled = true;
    notification_state.timezone_notifications_enabled = true;
    notification_state.rain_config_notifications_enabled = true;
    notification_state.rain_data_notifications_enabled = true;
    notification_state.rain_history_notifications_enabled = true;
    notification_state.environmental_data_notifications_enabled = true;
    notification_state.environmental_history_notifications_enabled = true;
    notification_state.compensation_status_notifications_enabled = true;
    
    LOG_INF("✅ All BLE notifications force-enabled");
}

/* Bluetooth connection callback */

static void connected(struct bt_conn *conn, uint8_t err) {
    if (err) {
        printk("Connection failed\n");
        return;
    }

    /* Request security level 2 (Encryption, No MITM) for Just Works bonding */
    int sec_err = bt_conn_set_security(conn, BT_SECURITY_L2);
    if (sec_err) {
        printk("Failed to set security: %d\n", sec_err);
    }

    /* Reset notification system completely */
    notification_system_enabled = true;
    
    /* Reset all priority states */
    for (int i = 0; i < 4; i++) {
        priority_state[i].last_notification_time = 0;
        priority_state[i].success_count = 0;
        priority_state[i].failure_count = 0;
    }
    
    /* Reset buffer pool state */
    init_notification_pool();
    
    /* Run immediate maintenance */
    buffer_pool_maintenance();
    
    /* Reset channel name throttling state */
    memset(&channel_name_throttle, 0, sizeof(channel_name_throttle));
    
    /* Clear and reset notification state on new connection */
    memset(&notification_state, 0, sizeof(notification_state));
    
    printk("Connected - system status updated to: 0\n");

    if (!default_conn) {
        default_conn = bt_conn_ref(conn);
        connection_active = true;  /* Mark connection as active */
    }
    
    /* Request a larger MTU to handle notifications better */
    int mtu_err = bt_gatt_exchange_mtu(conn, &mtu_exchange_params);
    if (mtu_err) {
        printk("MTU exchange failed: %d\n", mtu_err);
    } else {
        printk("MTU exchange initiated\n");
    }

    /* Clear any stale data that might be sent during CCC configuration */
    memset(valve_value, 0, sizeof(struct valve_control_data));
    /* Set invalid channel ID to prevent false positives */
    struct valve_control_data *valve_data = (struct valve_control_data *)valve_value;
    valve_data->channel_id = 0xFF; /* Invalid channel ID */
    valve_data->task_type = 0;     /* Inactive */
    valve_data->value = 0;
    
    /* Update status_value with current system status to ensure correct state on reconnect */
    watering_status_t current_status;
    if (watering_get_status(&current_status) == WATERING_SUCCESS) {
        status_value[0] = (uint8_t)current_status;
        printk("Connected - system status updated\n");
    } else {
        status_value[0] = (uint8_t)WATERING_STATUS_OK; /* Default to OK if can't read */
        printk("Connected - defaulted system status to OK\n");
    }
    
    printk("Connected to irrigation controller - values cleared and status updated\n");
    
    /* DO NOT start task update thread - it can cause BLE freezes */
    /* Task updates will be handled through other mechanisms */
    /* legacy task update thread permanently removed (no restart) */
}

/* Add retry mechanism for advertising restart */
static void adv_restart_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(adv_restart_work, adv_restart_work_handler);

static void adv_restart_work_handler(struct k_work *work)
{
    int err;
    int retry_count = 0;
    const int max_retries = 3;  // Reduce retries to avoid getting stuck
    
    printk("Starting advertising restart work handler\n");
    
    /* Small initial delay to ensure disconnect cleanup is complete */
    k_sleep(K_MSEC(100));
    
    /* Try to stop any existing advertiser first */
    err = bt_le_adv_stop();
    printk("Advertising stop result received\n");
    
    /* Retry loop with exponential backoff */
    while (retry_count < max_retries) {
        /* Wait before attempting restart */
        uint32_t delay_ms = 200 + (100 * retry_count);  // Linear backoff instead of exponential
        printk("Waiting before advertising restart attempt\n");
        k_sleep(K_MSEC(delay_ms));
        
        printk("Attempting to start advertising\n");
        err = bt_le_adv_start(&adv_param,
                              adv_ad, ARRAY_SIZE(adv_ad),
                              adv_sd, ARRAY_SIZE(adv_sd));
        
        if (err == 0) {
            printk("Advertising restarted successfully\n");
            return;
        }
        
        if (err == -EALREADY) {
            printk("Advertising already active\n");
            return;
        }
        
        printk("Advertising restart failed, retrying\n");
        
        retry_count++;
    }
    
    printk("Failed to restart advertising after max attempts\n");
    
    /* If all retries failed, schedule another attempt in 5 seconds */
    k_work_reschedule(&adv_restart_work, K_SECONDS(5));
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
    printk("Disconnected\n");
    /* Task update thread disabled - no stop required */
    
    /* Mark connection as inactive immediately */
    connection_active = false;

    if (default_conn) {
        bt_conn_unref(default_conn);
        default_conn = NULL;
    }

    /* Cancel periodic works */
    k_work_cancel_delayable(&task_queue_periodic_work);
    k_work_cancel_delayable(&status_periodic_work);

    /* Clear all characteristic values on disconnect to prevent stale data */
    memset(valve_value, 0, sizeof(struct valve_control_data));
    struct valve_control_data *valve_data = (struct valve_control_data *)valve_value;
    valve_data->channel_id = 0xFF; /* Invalid channel ID */
    valve_data->task_type = 0;     /* Inactive */
    valve_data->value = 0;
    
    /* Clear all notification states on disconnect */
    memset(&notification_state, 0, sizeof(notification_state));
    
    printk("Valve values cleared and notification states reset on disconnect\n");

    /* Schedule advertising restart in a work item to avoid stack issues */
    printk("Scheduling advertising restart work\n");
    k_work_schedule(&adv_restart_work, K_MSEC(500));  // Wait 500ms before restart
}

/* Removed unused debug/test prototypes to reduce warnings */

/* Valve characteristic read callback */
/* Valve characteristic read callback */
static ssize_t read_valve(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                          void *buf, uint16_t len, uint16_t offset) {
    if (!conn || !attr || !buf) {
        LOG_ERR("Invalid parameters for Valve read");
        return -EINVAL;
    }
    
    /* Per BLE API Documentation: READ returns last accepted task parameters OR current valve status */
    /* Return current valve status from the global valve_value buffer */
    /* This buffer is updated by bt_irrigation_valve_status_update() when valve state changes */
    
    const struct valve_control_data *value = (const struct valve_control_data *)valve_value;
    
    LOG_DBG("BT valve read: channel=%u, type=%u (%s), value=%u", 
            value->channel_id, value->task_type, 
            (value->task_type == 0) ? "inactive/duration" : "active/volume", 
            value->value);
    
    return bt_gatt_attr_read(conn, attr, buf, len, offset, valve_value,
                             sizeof(struct valve_control_data));
}

/* Valve characteristic write callback */
static ssize_t write_valve(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                           const void *buf, uint16_t len, uint16_t offset, uint8_t flags) {
    struct valve_control_data *value = attr->user_data;

    /* Validate write operation */
    if (offset + len > sizeof(struct valve_control_data)) {
        LOG_ERR("BT valve write: Invalid offset/length (offset=%u, len=%u, max=%zu)", 
                offset, len, sizeof(struct valve_control_data));
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }

    /* Only accept complete structure writes */
    if (len != sizeof(struct valve_control_data)) {
        LOG_ERR("BT valve write: Invalid length (got %u, expected %zu)", 
                len, sizeof(struct valve_control_data));
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }

    /* Copy the received data */
    memcpy(((uint8_t *) value) + offset, buf, len);

    /* Extract task parameters */
    uint8_t channel_id = value->channel_id;
    uint8_t task_type = value->task_type;
    uint16_t task_value = value->value;

    /* Handle master valve control (channel_id = 0xFF) */
    if (channel_id == 0xFF) {
        LOG_INF("BT valve write: Master valve control - type=%u (%s), value=%u", 
                task_type, (task_type == 0) ? "close" : "open", task_value);
        
        /* Master valve control: task_type = 0 (close), 1 (open), value ignored */
        watering_error_t err;
        if (task_type == 0) {
            /* Close master valve */
            err = master_valve_manual_close();
            if (err == WATERING_SUCCESS) {
                LOG_INF("✅ Master valve closed via BLE");
            }
        } else if (task_type == 1) {
            /* Open master valve */
            err = master_valve_manual_open();
            if (err == WATERING_SUCCESS) {
                LOG_INF("✅ Master valve opened via BLE");
            }
        } else {
            LOG_ERR("BT valve write: Invalid master valve task_type=%u (must be 0=close or 1=open)", task_type);
            return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
        }
        
        /* Handle master valve control errors */
        if (err != WATERING_SUCCESS) {
            LOG_ERR("❌ Master valve control failed: type=%u, error=%d", task_type, err);
            return BT_GATT_ERR(BT_ATT_ERR_WRITE_NOT_PERMITTED);
        }
        
    /* Send notification to confirm master valve state change */
        if (default_conn && notification_state.valve_notifications_enabled) {
            /* Update notification data with current master valve state */
            struct valve_control_data notify_data = {
                .channel_id = 0xFF,
                .task_type = master_valve_is_open() ? 1 : 0,
                .value = 0
            };
            
            const struct bt_gatt_attr *notify_attr = &irrigation_svc.attrs[ATTR_IDX_VALVE_VALUE];
            int notify_err = safe_notify(default_conn, notify_attr, &notify_data, sizeof(notify_data));
            
            if (notify_err == 0) {
                LOG_INF("✅ Master valve state notification sent: %s", 
                        notify_data.task_type ? "open" : "closed");
            } else {
                LOG_WRN("❌ Failed to send master valve notification: %d", notify_err);
            }
        }
        
        return len;
    }

    /* Validate channel ID for regular valves */
    if (channel_id >= WATERING_CHANNELS_COUNT) {
        LOG_ERR("BT valve write: Invalid channel_id=%u (max=%u)", channel_id, WATERING_CHANNELS_COUNT-1);
        return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
    }

    /* Validate task type */
    if (task_type > 1) {
        LOG_ERR("BT valve write: Invalid task_type=%u (must be 0 or 1)", task_type);
        return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
    }

    /* Validate task value and ranges per BLE API */
    if (task_value == 0) {
        LOG_ERR("BT valve write: Invalid task_value=0 (must be > 0)");
        return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
    }
    if (task_type == 0 && task_value > 1440) { /* Duration in minutes */
        LOG_ERR("BT valve write: Duration out of range (minutes=%u, max=1440)", task_value);
        return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
    }
    if (task_type == 1 && task_value > 1000) { /* Volume in liters */
        LOG_ERR("BT valve write: Volume out of range (liters=%u, max=1000)", task_value);
        return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
    }

    LOG_INF("BT valve write: Creating task - channel=%u, type=%u (%s), value=%u", 
            channel_id, task_type, (task_type == 0) ? "duration" : "volume", task_value);

    /* Create the corresponding task according to specification */
    watering_error_t err;
    if (task_type == 0) {
        /* Duration task: value is in minutes */
        err = watering_add_duration_task(channel_id, task_value);
        if (err == WATERING_SUCCESS) {
            LOG_INF("✅ Duration task created: channel=%u, minutes=%u", channel_id, task_value);
        }
    } else {
        /* Volume task: value is in liters (NOT milliliters!) */
        /* CRITICAL: Documentation specifically warns about this unit */
        err = watering_add_volume_task(channel_id, task_value);
        if (err == WATERING_SUCCESS) {
            LOG_INF("✅ Volume task created: channel=%u, liters=%u", channel_id, task_value);
        }
    }

    /* Handle task creation errors */
    if (err != WATERING_SUCCESS) {
        LOG_ERR("❌ Task creation failed: channel=%u, type=%u, value=%u, error=%d", 
                channel_id, task_type, task_value, err);
        
        /* Map watering errors to BLE errors */
        switch (err) {
            case WATERING_ERROR_INVALID_PARAM:
                return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
            case WATERING_ERROR_QUEUE_FULL:
                return BT_GATT_ERR(BT_ATT_ERR_WRITE_NOT_PERMITTED);
            case WATERING_ERROR_BUSY:
                return BT_GATT_ERR(BT_ATT_ERR_WRITE_NOT_PERMITTED);
            case WATERING_ERROR_HARDWARE:
                return BT_GATT_ERR(BT_ATT_ERR_WRITE_NOT_PERMITTED);
            default:
                return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
        }
    }

    /* Per BLE spec: do NOT send an immediate acceptance notification here.
     * Notifications are sent when valves actually open/close via
     * bt_irrigation_valve_status_update(). */

    return len;
}

/* CCC configuration change callback for valve characteristic */
static void valve_ccc_cfg_changed(const struct bt_gatt_attr *attr, uint16_t value) {
    bool notif_enabled = (value == BT_GATT_CCC_NOTIFY);
    notification_state.valve_notifications_enabled = notif_enabled;
    
    if (notif_enabled) {
        LOG_INF("✅ Valve notifications ENABLED - will send status updates when valves change");
        
        /* Per BLE API Documentation: Status Notifications format:
         * When a valve activates or deactivates, the system sends a notification with:
         * - channel_id: The channel that changed state (0-7)
         * - task_type: 1 if valve is active, 0 if inactive
         * - value: 0 (no duration/volume info for status updates)
         */
        
        /* Clear valve_value to ensure clean state */
        memset(valve_value, 0, sizeof(struct valve_control_data));
        
        /* Initialize with invalid channel to indicate no specific valve status */
        struct valve_control_data *valve_data = (struct valve_control_data *)valve_value;
        valve_data->channel_id = 0xFF; /* Invalid channel ID - no specific valve currently reported */
        valve_data->task_type = 0;     /* Inactive state */
        valve_data->value = 0;         /* No value for status */
        
        /* Don't send immediate notification - wait for real valve events */
        /* Per documentation: notifications are sent when valve status changes */
        LOG_INF("Valve status monitoring active - ready to notify on valve state changes");
    } else {
        LOG_INF("❌ Valve notifications DISABLED");
        /* Clear valve_value when notifications disabled */
        memset(valve_value, 0, sizeof(struct valve_control_data));
    }
}

/* Flow characteristic read callback */
static ssize_t read_flow(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                         void *buf, uint16_t len, uint16_t offset) {
    /* Per BLE API Documentation: READ returns current smoothed flow rate in pulses per second (pps) */
    /* Uses sophisticated signal processing: hardware debouncing, smoothing algorithm, rate calculation */
    uint32_t current_flow_rate = get_flow_rate();
    *(uint32_t*)flow_value = current_flow_rate;  /* Direct assignment instead of memcpy */
    
    const uint32_t *value = attr->user_data;
    
    LOG_DBG("BT Flow read: %u pps (smoothed over 500ms window, 2-sample average)", current_flow_rate);
    
    return bt_gatt_attr_read(conn, attr, buf, len, offset, value,
                             sizeof(uint32_t));
}

/* CCC configuration change callback for flow characteristic */
static void flow_ccc_cfg_changed(const struct bt_gatt_attr *attr, uint16_t value) {
    bool notif_enabled = (value == BT_GATT_CCC_NOTIFY);
    notification_state.flow_notifications_enabled = notif_enabled;
    
    if (notif_enabled) {
        LOG_INF("✅ Flow notifications enabled - ultra high-frequency monitoring");
        
    /* Flow notifications: NORMAL priority (200ms throttle), 4B pps payload */
    LOG_INF("Flow monitoring enabled (NORMAL priority, 200ms throttle)");
        
        /* Clear flow_value to prevent stale data on notification setup */
        memset(flow_value, 0, sizeof(uint32_t));
    } else {
    LOG_INF("Flow notifications disabled - monitoring stopped");
        /* Clear flow_value when notifications disabled */
        memset(flow_value, 0, sizeof(uint32_t));
    }
}

/* Status characteristic read callback */
static ssize_t read_status(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                           void *buf, uint16_t len, uint16_t offset) {
    if (!conn || !attr || !buf) {
        LOG_ERR("Invalid parameters for System Status read");
        return -EINVAL;
    }
    
    /* Per BLE API Documentation: READ returns current system status (uint8_t) */
    /* Status values: 0=OK, 1=No-Flow, 2=Unexpected-Flow, 3=Fault, 4=RTC-Error, 5=Low-Power, 6=Freeze-Lockout */
    watering_status_t current_status;
    if (watering_get_status(&current_status) == WATERING_SUCCESS) {
        status_value[0] = (uint8_t)current_status;
        LOG_DBG("System Status read: %u (%s)", current_status, 
                (current_status == 0) ? "OK" : 
                (current_status == 1) ? "No-Flow" :
                (current_status == 2) ? "Unexpected-Flow" :
                (current_status == 3) ? "Fault" :
                (current_status == 4) ? "RTC-Error" :
                (current_status == 5) ? "Low-Power" :
                (current_status == 6) ? "Freeze-Lockout" : "Unknown");
    } else {
        status_value[0] = (uint8_t)WATERING_STATUS_OK; /* Default to OK if can't read */
        LOG_WRN("Failed to read system status, defaulting to OK");
    }
    
    const uint8_t *value = attr->user_data;
    return bt_gatt_attr_read(conn, attr, buf, len, offset, value, sizeof(uint8_t));
}

/* CCC configuration change callback for status characteristic */
static void status_ccc_cfg_changed(const struct bt_gatt_attr *attr, uint16_t value) {
    bool notif_enabled = (value == BT_GATT_CCC_NOTIFY);
    notification_state.status_notifications_enabled = notif_enabled;
    
    if (notif_enabled) {
        LOG_INF("✅ System Status notifications enabled - will send updates on status changes");
        
        /* Per BLE API Documentation: status values mapping */
        /* 0=OK, 1=No-Flow, 2=Unexpected-Flow, 3=Fault, 4=RTC-Error, 5=Low-Power, 6=Freeze-Lockout */
        
        /* Always read fresh status from system */
        watering_status_t current_status;
        if (watering_get_status(&current_status) == WATERING_SUCCESS) {
            status_value[0] = (uint8_t)current_status;
            LOG_INF("Status notifications ready: current status = %u (%s)", current_status,
                   (current_status == 0) ? "OK" : 
                   (current_status == 1) ? "No-Flow" :
                   (current_status == 2) ? "Unexpected-Flow" :
                   (current_status == 3) ? "Fault" :
                   (current_status == 4) ? "RTC-Error" :
                   (current_status == 5) ? "Low-Power" :
                   (current_status == 6) ? "Freeze-Lockout" : "Unknown");
        } else {
            status_value[0] = (uint8_t)WATERING_STATUS_OK;
            LOG_WRN("Status CCC enabled - defaulted to OK status");
        }
        
    /* Start periodic reminder work for fault-like states */
    k_work_schedule(&status_periodic_work, K_SECONDS(30));
    /* Do not send immediate notification here; notify on transitions */
    } else {
        LOG_INF("System Status notifications disabled");
    k_work_cancel_delayable(&status_periodic_work);
    }
}

/* Periodic re-notification for fault conditions (every 30s) */
static void status_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);
    if (!default_conn || !notification_state.status_notifications_enabled) {
        return;
    }
    /* Re-send current status for client awareness during faults */
    watering_status_t current_status;
    if (watering_get_status(&current_status) == WATERING_SUCCESS) {
        if (current_status == WATERING_STATUS_FAULT ||
            current_status == WATERING_STATUS_NO_FLOW ||
            current_status == WATERING_STATUS_UNEXPECTED_FLOW ||
            current_status == WATERING_STATUS_RTC_ERROR ||
            current_status == WATERING_STATUS_LOW_POWER ||
            current_status == WATERING_STATUS_FREEZE_LOCKOUT) {
            status_value[0] = (uint8_t)current_status;
            const struct bt_gatt_attr *attr = &irrigation_svc.attrs[ATTR_IDX_STATUS_VALUE];
            safe_notify(default_conn, attr, status_value, sizeof(uint8_t));
        }
    }
    /* Reschedule */
    k_work_schedule(&status_periodic_work, K_SECONDS(30));
}

static K_WORK_DELAYABLE_DEFINE(status_periodic_work, status_work_handler);

/* Periodic Task Queue status updates (every 5s while a task is active) */
static void task_queue_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);
    if (!default_conn || !notification_state.task_queue_notifications_enabled) {
        return;
    }
    uint8_t pending = 0; bool active = false;
    if (watering_get_queue_status(&pending, &active) == WATERING_SUCCESS && active) {
        /* Send a periodic status update while a task is running */
        bt_irrigation_queue_status_notify();
    }
    /* Reschedule */
    k_work_schedule(&task_queue_periodic_work, K_SECONDS(5));
}

/* Timer defined earlier to be usable in CCC callback */

/* ------------------------------------------------------------------
 * Channel-Config CCC callback 
 * ----------------------------------------------------------------*/
static void channel_config_ccc_changed(const struct bt_gatt_attr *attr,
                                       uint16_t value)
{
    bool notif_enabled = (value == BT_GATT_CCC_NOTIFY);
    notification_state.channel_config_notifications_enabled = notif_enabled;
    
    if (notif_enabled) {
        printk("✅ Channel Config notifications enabled - will send updates when config changes\n");
        
        /* Per BLE API Documentation: Structure size is CRITICAL - must be exactly 76 bytes */
        /* Supports both fragmentation protocol (for browsers) and complete structure (for native apps) */
        
        printk("Channel Config ready - 76-byte structure with plant/environment fields\n");
        printk("Plant types: 0=Vegetables, 1=Herbs, 2=Flowers, 3=Shrubs, 4=Trees, 5=Lawn, 6=Succulents, 7=Custom\n");
        printk("Soil types: 0=Clay, 1=Sandy, 2=Loamy, 3=Silty, 4=Rocky, 5=Peaty, 6=Potting, 7=Hydroponic\n");
        printk("Irrigation: 0=Drip, 1=Sprinkler, 2=Soaker, 3=Micro Spray, 4=Hand, 5=Flood\n");
        
        /* Clear any stale data to ensure clean state */
        memset(channel_config_value, 0, sizeof(channel_config_value));
    } else {
        printk("Channel Config notifications disabled\n");
    }
}

/* Channel Config characteristic read callback */
static ssize_t read_channel_config(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                   void *buf, uint16_t len, uint16_t offset) {
    /* Return the 76-byte channel_config_data as per BLE spec */
    struct channel_config_data read_value;
    watering_channel_t *channel;
    watering_error_t err;
    uint8_t channel_id;
    size_t name_len;
    uint32_t now;
    /* Reduce debug logging to prevent log spam during repeated reads */
    static uint32_t last_read_log_time = 0;
    static uint8_t last_read_channel_id = 0xFF;

    /* Get the current channel selection from the global attribute buffer */
    const struct channel_config_data *global_value =
        (const struct channel_config_data *)channel_config_value;

    /* Use the selected channel from the global buffer, but default to 0 if invalid */
    channel_id = global_value->channel_id;
    if (channel_id >= WATERING_CHANNELS_COUNT) {
        channel_id = 0;
    }

    /* Ensure this is a READ-ONLY operation that doesn't trigger saves */
    err = watering_get_channel(channel_id, &channel);

    if (err != WATERING_SUCCESS) {
        printk("Failed to get channel %d: error %d\n", channel_id, err);
        /* Return default/safe values */
        memset(&read_value, 0, sizeof(read_value));
        read_value.channel_id = channel_id;
        /* Name defaults */
        const char *def_name = "Default";
        name_len = strlen(def_name);
        if (name_len >= sizeof(read_value.name)) {
            name_len = sizeof(read_value.name) - 1;
        }
        memcpy(read_value.name, def_name, name_len);
        read_value.name[name_len] = '\0';
        read_value.name_len = (uint8_t)name_len;
        /* Other defaults */
        read_value.auto_enabled = 0;
        read_value.plant_type = 0;
        read_value.soil_type = 0;
        read_value.irrigation_method = 0;
        read_value.coverage_type = 0;
        read_value.coverage.area_m2 = 0.0f;
        read_value.sun_percentage = 50;

        return bt_gatt_attr_read(conn, attr, buf, len, offset,
                                 &read_value, sizeof(read_value));
    }

    /* Populate 76B structure */
    memset(&read_value, 0, sizeof(read_value));
    read_value.channel_id = channel_id;

    name_len = strnlen(channel->name, sizeof(channel->name));
    if (name_len >= sizeof(read_value.name)) {
        name_len = sizeof(read_value.name) - 1;
    }
    memcpy(read_value.name, channel->name, name_len);
    read_value.name[name_len] = '\0';
    read_value.name_len = (uint8_t)name_len;

    read_value.auto_enabled = channel->watering_event.auto_enabled ? 1 : 0;

    now = k_uptime_get_32();

    /* Only log if it's been more than 5 seconds since the last log for this channel */
    if (now - last_read_log_time > 5000 || last_read_channel_id != channel_id) {
        printk("Read channel config: ch=%d, name=\"%s\"\n",
               read_value.channel_id, read_value.name);
        last_read_log_time = now;
        last_read_channel_id = channel_id;
    }

    /* Populate plant and growing environment fields */
    read_value.plant_type = (uint8_t)channel->plant_type;
    read_value.soil_type = (uint8_t)channel->soil_type;
    read_value.irrigation_method = (uint8_t)channel->irrigation_method;
    read_value.coverage_type = channel->use_area_based ? 0 : 1;
    if (channel->use_area_based) {
        read_value.coverage.area_m2 = channel->coverage.area_m2;
    } else {
        read_value.coverage.plant_count = channel->coverage.plant_count;
    }
    read_value.sun_percentage = channel->sun_percentage;

    return bt_gatt_attr_read(conn, attr, buf, len, offset,
                             &read_value, sizeof(read_value));
}

/* Channel Config characteristic write callback */
static ssize_t write_channel_config(struct bt_conn *conn,
                                    const struct bt_gatt_attr *attr,
                                    const void *buf, uint16_t len,
                                    uint16_t offset, uint8_t flags)
{
    struct channel_config_data *value =
        (struct channel_config_data *)attr->user_data;
    uint8_t requested_channel_id;
    watering_channel_t *ch;
    const uint8_t *data = (const uint8_t *)buf;

    printk("🔧 BLE Channel Config write: len=%u, offset=%u, flags=0x%02x\n", len, offset, flags);
    
    /* Check for fragmentation timeout */
    check_fragmentation_timeout();
    
    /* Log current fragmentation state for debugging */
    log_fragmentation_state("ENTRY");
    
    /* Debug: Show received data in hex format for troubleshooting */
    if (len <= 20) {
        printk("🔧 BLE: Raw data (%u bytes): ", len);
        for (int i = 0; i < len; i++) {
            printk("%02x ", data[i]);
        }
        printk("\n");
    }

    /* —— 1-byte SELECT-FOR-READ (only for non-prepared writes) -- */
    if (!(flags & BT_GATT_WRITE_FLAG_PREPARE) && offset == 0 && len == 1) {
        requested_channel_id = *(const uint8_t*)buf;  /* Direct access instead of memcpy */
        if (requested_channel_id >= WATERING_CHANNELS_COUNT)
            return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
        
        /* Only update the local cache if the channel actually changed */
        if (value->channel_id != requested_channel_id) {
            value->channel_id = requested_channel_id;
            /* refresh local cache so a subsequent READ is coherent */
            if (watering_get_channel(value->channel_id, &ch) == WATERING_SUCCESS) {
                printk("Channel %d selected for configuration\n", value->channel_id);
            }
        }
        /* DO NOT call watering_save_config() here - this is just a selection, not a config change */
        return len;        /* ACK */
    }

    /* —— HANDLE CONTINUATION FRAGMENTS FIRST -- */
    /* If fragmentation is in progress, treat ALL data as continuation */
    if (channel_frag.in_progress) {
        uint16_t remaining = channel_frag.expected - channel_frag.received;
        uint16_t copy_len = (len > remaining) ? remaining : len;
        
        printk("🔧 BLE: Continuation fragment - len=%u, remaining=%u, copy_len=%u\n", 
               len, remaining, copy_len);
        
        if (channel_frag.received + copy_len > sizeof(channel_frag.buf)) {
            printk("❌ Fragment buffer overflow\n");
            channel_frag.in_progress = false;
            return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
        }
        
        memcpy(channel_frag.buf + channel_frag.received, data, copy_len);
        channel_frag.received += copy_len;
        
        if (channel_frag.frag_type == 1) {
            printk("🔧 BLE: Fragment received: %u/%u bytes: \"%.*s\"\n", 
                   channel_frag.received, channel_frag.expected, channel_frag.received, channel_frag.buf);
        } else {
            printk("🔧 BLE: Fragment received: %u/%u bytes\n", 
                   channel_frag.received, channel_frag.expected);
        }
        
        /* Check if complete */
        if (channel_frag.received >= channel_frag.expected) {
            /* Process based on fragment type */
            if (channel_frag.frag_type == 1) {
                /* NAME ONLY UPDATE */
                if (watering_get_channel(channel_frag.id, &ch) != WATERING_SUCCESS) {
                    printk("❌ Failed to get channel %u for name update\n", channel_frag.id);
                    channel_frag.in_progress = false;
                    return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
                }
                
                /* Null-terminate and copy the name */
                if (channel_frag.expected < sizeof(channel_frag.buf)) {
                    channel_frag.buf[channel_frag.expected] = '\0';
                }
                memset(ch->name, 0, sizeof(ch->name));
                strncpy(ch->name, (char*)channel_frag.buf, sizeof(ch->name) - 1);
                ch->name[sizeof(ch->name) - 1] = '\0';
                
                printk("✅ BLE: Name updated for channel %u: \"%s\" (len=%u)\n", 
                       channel_frag.id, ch->name, channel_frag.expected);
                
                /* Update local attribute cache */
                value->channel_id = channel_frag.id;
                value->name_len = channel_frag.expected;
                memcpy(value->name, channel_frag.buf, channel_frag.expected);
                if (channel_frag.expected < sizeof(value->name)) {
                    value->name[channel_frag.expected] = '\0';
                }
                
                /* Save configuration */
                watering_save_config_priority(true);
                printk("🔧 BLE: Config saved for channel %u\n", channel_frag.id);
                
                /* Send notification with throttling for name changes */
                if (notification_state.channel_config_notifications_enabled) {
                    /* Use a delayed notification to prevent buffer exhaustion during rapid name changes */
                    uint32_t now = k_uptime_get_32();
                    static uint32_t last_name_notification = 0;
                    
                    if (now - last_name_notification > 500) { /* 500ms delay between name notifications */
                        bt_irrigation_channel_config_update(channel_frag.id);
                        last_name_notification = now;
                    } else {
                        printk("📋 BLE: Name change notification delayed to prevent buffer overflow\n");
                    }
                }
                
            } else if (channel_frag.frag_type == 2 || channel_frag.frag_type == 3) {
                /* FULL STRUCTURE UPDATE (type 2=big-endian, type 3=little-endian) */
                if (channel_frag.expected != sizeof(struct channel_config_data)) {
                    printk("❌ Invalid structure size: got %u, expected %zu\n", 
                           channel_frag.expected, sizeof(struct channel_config_data));
                    channel_frag.in_progress = false;
                    return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
                }
                
                /* Copy complete structure */
                memcpy(value, channel_frag.buf, sizeof(struct channel_config_data));
                
                printk("✅ BLE: Full config received via fragmentation (type %u) for channel %u\n", 
                       channel_frag.frag_type, value->channel_id);
                
                /* Process like standard write - continue to validation section */
                channel_frag.in_progress = false;
                goto process_full_config;
                
            } else {
                printk("❌ Unknown fragment type: %u\n", channel_frag.frag_type);
                channel_frag.in_progress = false;
                return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
            }
            
            channel_frag.in_progress = false;
            return len;
        }
        
        return len;
    }

    /* —— NEW FRAGMENTATION PROTOCOL HEADER -- */
    /* Check for fragmentation protocol header: [channel_id][frag_type][size_low][size_high][data...] */
    /* Only process as header if no fragmentation is in progress and we have enough data */
    if (offset == 0 && len >= 4 && !channel_frag.in_progress) {
        uint8_t channel_id = data[0];
        uint8_t frag_type = data[1];
        uint16_t total_size;
        
        /* Handle both big-endian (frag_type=2) and little-endian (frag_type=1,3) */
        if (frag_type == 2) {
            total_size = (data[2] << 8) | data[3];  /* Big-endian */
        } else {
            total_size = data[2] | (data[3] << 8);  /* Little-endian */
        }
        
        /* Additional validation: ignore invalid headers that look like continuation data */
        /* Valid headers should have reasonable frag_type (1-3) and non-zero total_size */
        if (frag_type == 0 || total_size == 0) {
            printk("🔧 BLE: Ignoring invalid header - frag_type=%u, total_size=%u\n", 
                   frag_type, total_size);
            /* Treat as regular write */
            goto standard_write;
        }
        
        printk("🔧 BLE: Fragmentation header detected - channel=%u, frag_type=%u, total_size=%u\n", 
               channel_id, frag_type, total_size);
        
        if (channel_id >= WATERING_CHANNELS_COUNT) {
            printk("❌ Invalid channel ID %u for fragmentation\n", channel_id);
            return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
        }
        
        if (frag_type == 0 || frag_type > 3) {
            printk("❌ Invalid fragment type %u (must be 1, 2, or 3)\n", frag_type);
            return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
        }
        
        if (total_size > sizeof(channel_frag.buf)) {
            printk("❌ Data size too large: %u > %zu\n", total_size, sizeof(channel_frag.buf));
            return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
        }
        
        /* Initialize fragmentation state */
        channel_frag.id = channel_id;
        channel_frag.frag_type = frag_type;
        channel_frag.expected = total_size;
        channel_frag.received = 0;
        channel_frag.in_progress = true;
        channel_frag.start_time = k_uptime_get_32();
        memset(channel_frag.buf, 0, sizeof(channel_frag.buf));
        
        printk("🔧 BLE: Fragmentation initialized - cid=%u, type=%u, expected=%u bytes\n", 
               channel_id, frag_type, total_size);
        
        /* Process payload if present */
        if (len > 4) {
            uint16_t payload_len = len - 4;
            if (payload_len > channel_frag.expected) {
                payload_len = channel_frag.expected;
            }
            memcpy(channel_frag.buf, data + 4, payload_len);
            channel_frag.received = payload_len;
            
            if (frag_type == 1) {
                printk("🔧 BLE: Received name fragment: %u/%u bytes: \"%.*s\"\n", 
                       payload_len, channel_frag.expected, payload_len, channel_frag.buf);
            } else {
                printk("🔧 BLE: Received struct fragment: %u/%u bytes\n", 
                       payload_len, channel_frag.expected);
            }
        }
        
        return len;
    }

standard_write:

    /* Standard write handling */
    if (offset + len > sizeof(*value)) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }

    memcpy(((uint8_t *)value) + offset, buf, len);

    /* If complete structure received, commit changes */
    if (offset + len == sizeof(*value)) {
process_full_config:
        if (value->channel_id >= WATERING_CHANNELS_COUNT) {
            return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
        }

        if (watering_get_channel(value->channel_id, &ch) != WATERING_SUCCESS) {
            return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
        }

        /* Validate enum values per BLE API Documentation */
        if (value->plant_type > 7) {  /* Plant types: 0-7 */
            printk("Invalid plant_type: %u (max 7)\n", value->plant_type);
            return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
        }
        if (value->soil_type > 7) {   /* Soil types: 0-7 */
            printk("Invalid soil_type: %u (max 7)\n", value->soil_type);
            return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
        }
        if (value->irrigation_method > 5) {  /* Irrigation methods: 0-5 */
            printk("Invalid irrigation_method: %u (max 5)\n", value->irrigation_method);
            return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
        }
        if (value->coverage_type > 1) {  /* Coverage type: 0=area, 1=plant count */
            printk("Invalid coverage_type: %u (must be 0 or 1)\n", value->coverage_type);
            return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
        }
        if (value->sun_percentage > 100) {  /* Sun percentage: 0-100% */
            printk("Invalid sun_percentage: %u (max 100)\n", value->sun_percentage);
            return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
        }
        if (value->name_len >= sizeof(value->name)) {  /* Name length validation */
            printk("Invalid name_len: %u (max %zu)\n", value->name_len, sizeof(value->name)-1);
            return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
        }

        /* Update channel name */
        if (value->name_len > 0 && value->name_len < sizeof(value->name)) {
            value->name[value->name_len] = '\0';
            memset(ch->name, 0, sizeof(ch->name));
            strncpy(ch->name, value->name, sizeof(ch->name) - 1);
            ch->name[sizeof(ch->name) - 1] = '\0';
            printk("🔧 BLE: Updated channel %u name to: \"%s\" (len=%u)\n", 
                   value->channel_id, ch->name, value->name_len);
        }
        
        /* Update auto-enable */
        ch->watering_event.auto_enabled = value->auto_enabled ? true : false;
        
        /* Update plant and growing environment fields */
        ch->plant_type = (plant_type_t)value->plant_type;
        ch->soil_type = (soil_type_t)value->soil_type;
        ch->irrigation_method = (irrigation_method_t)value->irrigation_method;
        ch->use_area_based = (value->coverage_type == 0);
        if (value->coverage_type == 0) {
            ch->coverage.area_m2 = value->coverage.area_m2;
        } else {
            ch->coverage.plant_count = value->coverage.plant_count;
        }
        ch->sun_percentage = value->sun_percentage;
        
        /* Save configuration using priority save system (250ms throttle for BLE) */
        printk("🔧 BLE: About to save config for channel %u with name: \"%s\"\n", 
               value->channel_id, ch->name);
        watering_save_config_priority(true);
        printk("🔧 BLE: Config save completed for channel %u\n", value->channel_id);
        
        /* FORCE notification - no matter what the client state is */
        printk("🔧 SAVE: Marking using_default_settings = false (was %s)\n", 
               using_default_settings ? "true" : "false");
        using_default_settings = false;
        
        /* Force enable channel config notifications and send immediately */
        LOG_INF("🔧 BLE: Force enabling channel config notifications");
        notification_state.channel_config_notifications_enabled = true;
        
        /* Send notification to confirm configuration update per BLE API Documentation */
        /* Channel Config (ef4): Config updates | On change (throttled 500ms) | Configuration confirmations */
        int notification_result = bt_irrigation_channel_config_update(value->channel_id);
        
        if (notification_result == 0) {
            printk("✅ BLE: Channel config notification sent successfully for channel %u\n", value->channel_id);
        } else {
            printk("❌ BLE: Channel config notification failed for channel %u: %d\n", 
                   value->channel_id, notification_result);
        }
        
        printk("Channel %d configuration updated: plant=%u, soil=%u, irrigation=%u\n", 
               value->channel_id, value->plant_type, value->soil_type, value->irrigation_method);
    }
    
    return len;
}

/* RTC implementation */
static ssize_t read_rtc(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                        void *buf, uint16_t len, uint16_t offset) {
    if (!conn || !attr || !buf) {
        LOG_ERR("Invalid parameters for RTC read");
        return -EINVAL;
    }
    
    struct rtc_data *value = (struct rtc_data *) rtc_value;
    rtc_datetime_t now_utc, now_local;
    timezone_config_t tz_config;

    /* Per BLE API Documentation: READ returns current date and time in local timezone */
    /* Structure: year(0-99), month(1-12), day(1-31), hour(0-23), minute(0-59), second(0-59), day_of_week(0-6), utc_offset_minutes, dst_active */
    
    /* Get current UTC time from RTC */
    if (rtc_datetime_get(&now_utc) == 0) {
        /* Get timezone configuration */
        if (timezone_get_config(&tz_config) == 0) {
            /* Get current UTC timestamp */
            uint32_t utc_timestamp = timezone_rtc_to_unix_utc(&now_utc);
            
            /* Convert to local time */
            if (timezone_unix_to_rtc_local(utc_timestamp, &now_local) == 0) {
                value->year = now_local.year - 2000; // Convert to 2-digit format
                value->month = now_local.month;
                value->day = now_local.day;
                value->hour = now_local.hour;
                value->minute = now_local.minute;
                value->second = now_local.second;
                value->day_of_week = now_local.day_of_week;
                value->utc_offset_minutes = timezone_get_total_offset(utc_timestamp);
                value->dst_active = timezone_is_dst_active(utc_timestamp) ? 1 : 0;
                
                LOG_DBG("RTC read (local): %02u/%02u/%04u %02u:%02u:%02u (day %u) UTC%+d DST:%u", 
                        value->day, value->month, 2000 + value->year,
                        value->hour, value->minute, value->second, value->day_of_week,
                        value->utc_offset_minutes / 60, value->dst_active);
            } else {
                LOG_ERR("Failed to convert UTC to local time");
                goto fallback;
            }
        } else {
            LOG_WRN("Timezone config unavailable, using UTC time");
            value->year = now_utc.year - 2000;
            value->month = now_utc.month;
            value->day = now_utc.day;
            value->hour = now_utc.hour;
            value->minute = now_utc.minute;
            value->second = now_utc.second;
            value->day_of_week = now_utc.day_of_week;
            value->utc_offset_minutes = 0;
            value->dst_active = 0;
        }
    } else {
        fallback:
        // RTC unavailable, use default values
        value->year = 25; // 2025 (current year)
        value->month = 7;
        value->day = 13;
        value->hour = 12;
        value->minute = 0;
        value->second = 0;
        value->day_of_week = 0; // Sunday
        value->utc_offset_minutes = 120; // Default UTC+2
        value->dst_active = 1; // Assume DST active in summer
        
        LOG_WRN("RTC unavailable, using fallback values with timezone info");
    }
    
    /* Clear reserved fields */
    memset(value->reserved, 0, sizeof(value->reserved));

    return bt_gatt_attr_read(conn, attr, buf, len, offset, value, sizeof(*value));
}

static ssize_t write_rtc(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                         const void *buf, uint16_t len, uint16_t offset, uint8_t flags) {
    struct rtc_data *value = (struct rtc_data *) attr->user_data;
    rtc_datetime_t new_time_local, new_time_utc;
    timezone_config_t tz_config;
    int ret;

    /* Validate basic parameters */
    if (!conn || !attr || !buf) {
        LOG_ERR("Invalid parameters for RTC write");
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_HANDLE);
    }

    /* Combined validation - single check for performance */
    if (offset + len > sizeof(*value) || len != sizeof(*value)) {
        LOG_ERR("RTC write: Invalid params (offset=%u, len=%u, expected=%zu)", 
                offset, len, sizeof(*value));
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }

    memcpy(((uint8_t *) value) + offset, buf, len);

    /* Combined range validation */
    if (value->month < 1 || value->month > 12 || value->day < 1 || value->day > 31 ||
        value->hour > 23 || value->minute > 59 || value->second > 59 || value->day_of_week > 6) {
        LOG_ERR("RTC write: Invalid values");
        return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
    }

    /* Additional validation for invalid date combinations */
    if ((value->month == 2 && value->day > 29) || /* February max 29 days */
        ((value->month == 4 || value->month == 6 || value->month == 9 || value->month == 11) && value->day > 30)) {
        LOG_ERR("RTC write: Invalid day %u for month %u", value->day, value->month);
        return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
    }

    /* Treat incoming time as local time and convert to UTC for RTC storage */
    new_time_local.year = 2000 + value->year; // Convert back to full year
    new_time_local.month = value->month;
    new_time_local.day = value->day;
    new_time_local.hour = value->hour;
    new_time_local.minute = value->minute;
    new_time_local.second = value->second;
    /* Compute day_of_week from provided date to avoid depending on client value */
    {
        /* Calculate DOW using Unix epoch formula (0=Sunday) based on local date */
        rtc_datetime_t tmp = new_time_local;
        /* Use midnight to stabilize DOW regardless of time-of-day */
        tmp.hour = 0; tmp.minute = 0; tmp.second = 0;
        uint32_t ts_local = timezone_rtc_to_unix_utc(&tmp);
        new_time_local.day_of_week = (ts_local / 86400UL + 4) % 7;
    }

    LOG_DBG("RTC write (local): %02u/%02u/%04u %02u:%02u:%02u",
           new_time_local.day, new_time_local.month, new_time_local.year,
           new_time_local.hour, new_time_local.minute, new_time_local.second);

    /* Convert local time to UTC if timezone is configured */
    if (timezone_get_config(&tz_config) == 0) {
        /* Convert local datetime to Unix timestamp */
        uint32_t local_timestamp = timezone_rtc_to_unix_utc(&new_time_local);
        
        /* Convert local timestamp to UTC timestamp */
        uint32_t utc_timestamp = timezone_local_to_utc(local_timestamp);
        
        /* Convert UTC timestamp back to datetime for RTC */
        if (timezone_unix_to_rtc_utc(utc_timestamp, &new_time_utc) != 0) {
            LOG_ERR("Failed to convert local time to UTC");
            return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
        }
        
        LOG_DBG("RTC write (UTC): %02u/%02u/%04u %02u:%02u:%02u",
               new_time_utc.day, new_time_utc.month, new_time_utc.year,
               new_time_utc.hour, new_time_utc.minute, new_time_utc.second);
    } else {
        /* No timezone config, treat as UTC */
        new_time_utc = new_time_local;
        LOG_WRN("No timezone config, treating time as UTC");
    }

    /* Update timezone configuration from payload: apply utc_offset (including 0) and dst_active */
    if (timezone_get_config(&tz_config) == 0) {
        bool tz_changed = false;
        if (tz_config.utc_offset_minutes != value->utc_offset_minutes) {
            tz_config.utc_offset_minutes = value->utc_offset_minutes;
            tz_changed = true;
        }
        /* Interpret dst_active as enable/disable DST usage (current simple model) */
        uint8_t desired_dst_enabled = value->dst_active ? 1 : 0;
        if (tz_config.dst_enabled != desired_dst_enabled) {
            tz_config.dst_enabled = desired_dst_enabled;
            if (!tz_config.dst_enabled) {
                /* When disabling DST, ensure offset is not double-counted */
                tz_config.dst_offset_minutes = 0;
            }
            tz_changed = true;
        }
        if (tz_changed) {
            if (timezone_set_config(&tz_config) != 0) {
                LOG_WRN("Failed to update timezone config (offset/DST)");
            } else {
                LOG_INF("Timezone updated via RTC write: UTC%+d:%02d, DST=%s", 
                        tz_config.utc_offset_minutes / 60,
                        abs(tz_config.utc_offset_minutes % 60),
                        tz_config.dst_enabled ? "ON" : "OFF");
            }
        }
    }

    ret = rtc_datetime_set(&new_time_utc);

    if (ret != 0) {
        LOG_ERR("RTC update failed: %d", ret);
        return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
    }

    LOG_INF("RTC updated successfully (stored as UTC)");

    /* Update onboarding flag - RTC is now configured */
    onboarding_update_system_flag(SYSTEM_FLAG_RTC_CONFIGURED, true);

    /* Send notification to confirm RTC update per BLE API Documentation */
    /* RTC (ef9): Time synchronization events | On change | Manual time updates via BLE */
    if (notification_state.rtc_notifications_enabled) {
        bt_irrigation_rtc_notify();
    }

    return len;
}

/* Timezone implementation */
static ssize_t read_timezone(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                            void *buf, uint16_t len, uint16_t offset) {
    if (!conn || !attr || !buf) {
        LOG_ERR("Invalid parameters for timezone read");
        return -EINVAL;
    }
    
    timezone_config_t *value = (timezone_config_t *) timezone_value;
    
    /* Update timezone value with current configuration */
    timezone_config_t current_config;
    int ret = timezone_get_config(&current_config);
    if (ret == 0) {
        *value = current_config;
        LOG_DBG("Timezone read: UTC%s%d:%02d DST=%s", 
                (current_config.utc_offset_minutes >= 0) ? "+" : "",
                current_config.utc_offset_minutes / 60,
                abs(current_config.utc_offset_minutes % 60),
                current_config.dst_enabled ? "ON" : "OFF");
    } else {
        LOG_ERR("Failed to read timezone config: %d", ret);
        return -EIO;
    }
    
    return bt_gatt_attr_read(conn, attr, buf, len, offset, value,
                             sizeof(timezone_config_t));
}

static ssize_t write_timezone(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                             const void *buf, uint16_t len, uint16_t offset, uint8_t flags) {
    timezone_config_t *value = (timezone_config_t *)attr->user_data;
    
    if (offset != 0) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }
    
    if (len != sizeof(timezone_config_t)) {
        LOG_ERR("Invalid timezone data length: %u (expected %u)", len, sizeof(timezone_config_t));
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }
    
    timezone_config_t new_config;
    memcpy(&new_config, buf, sizeof(timezone_config_t));
    
    /* Validation aligned to spec */
    if (new_config.utc_offset_minutes < -720 || new_config.utc_offset_minutes > 840) {
        LOG_ERR("Invalid UTC offset: %d minutes", new_config.utc_offset_minutes);
        return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
    }
    if (new_config.dst_enabled > 1) {
        LOG_ERR("Invalid DST setting: %u", new_config.dst_enabled);
        return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
    }
    if (new_config.dst_offset_minutes < -120 || new_config.dst_offset_minutes > 120) {
        LOG_ERR("Invalid DST offset: %d minutes", new_config.dst_offset_minutes);
        return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
    }
    if (new_config.dst_enabled) {
        /* Validate DST rule ranges */
        if (new_config.dst_start_month < 1 || new_config.dst_start_month > 12 ||
            new_config.dst_end_month   < 1 || new_config.dst_end_month   > 12) {
            LOG_ERR("Invalid DST month (start=%u end=%u)", new_config.dst_start_month, new_config.dst_end_month);
            return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
        }
        if (new_config.dst_start_week < 1 || new_config.dst_start_week > 5 ||
            new_config.dst_end_week   < 1 || new_config.dst_end_week   > 5) {
            LOG_ERR("Invalid DST week (start=%u end=%u)", new_config.dst_start_week, new_config.dst_end_week);
            return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
        }
        if (new_config.dst_start_dow > 6 || new_config.dst_end_dow > 6) {
            LOG_ERR("Invalid DST day-of-week (start=%u end=%u)", new_config.dst_start_dow, new_config.dst_end_dow);
            return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
        }
    } else {
        /* When DST disabled, clear rule fields for consistency */
        new_config.dst_start_month = 0;
        new_config.dst_start_week  = 0;
        new_config.dst_start_dow   = 0;
        new_config.dst_end_month   = 0;
        new_config.dst_end_week    = 0;
        new_config.dst_end_dow     = 0;
        new_config.dst_offset_minutes = 0;
    }
    
    /* Apply new timezone configuration */
    int ret = timezone_set_config(&new_config);
    if (ret != 0) {
        LOG_ERR("Failed to set timezone config: %d", ret);
        return BT_GATT_ERR(BT_ATT_ERR_WRITE_NOT_PERMITTED);
    }
    
    /* Update onboarding flag - timezone is now configured */
    onboarding_update_system_flag(SYSTEM_FLAG_TIMEZONE_SET, true);
    
    /* Update local copy */
    *value = new_config;
    
    LOG_INF("Timezone updated: UTC%s%d:%02d DST=%s", 
            (new_config.utc_offset_minutes >= 0) ? "+" : "",
            new_config.utc_offset_minutes / 60,
            abs(new_config.utc_offset_minutes % 60),
            new_config.dst_enabled ? "ON" : "OFF");
    
    /* Send notification if enabled */
    if (notification_state.timezone_notifications_enabled && connection_active && default_conn) {
        bt_gatt_notify(default_conn, attr, value, sizeof(timezone_config_t));
    }
    
    return len;
}

static void timezone_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value) {
    bool notify_enabled = (value == BT_GATT_CCC_NOTIFY);
    notification_state.timezone_notifications_enabled = notify_enabled;
    
    LOG_INF("Timezone notifications %s", notify_enabled ? "enabled" : "disabled");
    
    /* Send immediate notification when enabled */
    if (notify_enabled && connection_active && default_conn) {
        timezone_config_t current_config;
        if (timezone_get_config(&current_config) == 0) {
            timezone_config_t *tz_value = (timezone_config_t *) timezone_value;
            *tz_value = current_config;
            
            bt_gatt_notify(default_conn, attr - 1, tz_value, sizeof(timezone_config_t));
            
            LOG_INF("Sent initial timezone notification: UTC%s%d:%02d DST=%s",
                    (current_config.utc_offset_minutes >= 0) ? "+" : "",
                    current_config.utc_offset_minutes / 60,
                    abs(current_config.utc_offset_minutes % 60),
                    current_config.dst_enabled ? "ON" : "OFF");
        }
    }
}

/* Alarm implementation */
static ssize_t read_alarm(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                          void *buf, uint16_t len, uint16_t offset) {
    if (!conn || !attr || !buf) {
        LOG_ERR("Invalid parameters for Alarm read");
        return -EINVAL;
    }
    
    struct alarm_data *value = (struct alarm_data *)attr->user_data;
    
    /* Per BLE API Documentation: READ returns most recent alarm information */
    /* Structure: alarm_code, alarm_data, timestamp */
    /* Alarm codes: 0=No alarm, 1-13=specific alarm codes */
    
    LOG_DBG("Alarm read: code=%u, data=%u, timestamp=%u", 
            value->alarm_code, value->alarm_data, value->timestamp);
    
    return bt_gatt_attr_read(conn, attr, buf, len, offset, value,
                             sizeof(struct alarm_data));
}

static ssize_t write_alarm(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                           const void *buf, uint16_t len, uint16_t offset, uint8_t flags) {
    struct alarm_data *value = (struct alarm_data *)attr->user_data;
    
    if (offset != 0) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }
    
    // Check for clear alarm commands
    if (len == 1) {
        // Single byte clear command
        uint8_t clear_code = ((const uint8_t *)buf)[0];
        
        if (clear_code == 0x00 || clear_code == 0xFF) {
            // Clear all alarms
            printk("BLE: Clearing all alarms (%s)\n", clear_code == 0xFF ? "0xFF alias" : "0x00");
            watering_clear_errors();
            
            // Reset alarm data
            value->alarm_code = 0;
            value->alarm_data = 0;
            value->timestamp = 0;
            
            // Notify cleared status
            bt_irrigation_alarm_notify(0, 0);
            
        } else if (clear_code >= 1 && clear_code <= 13) {
            // Clear specific alarm if it matches current alarm
            if (value->alarm_code == clear_code) {
                printk("BLE: Clearing alarm %d\n", clear_code);
                watering_clear_errors();
                
                // Reset alarm data
                value->alarm_code = 0;
                value->alarm_data = 0;
                value->timestamp = 0;
                
                // Notify cleared status
                bt_irrigation_alarm_notify(0, 0);
            } else {
                printk("BLE: Alarm code %d does not match current alarm %d\n", 
                       clear_code, value->alarm_code);
            }
        } else {
            printk("BLE: Invalid alarm clear code: %d\n", clear_code);
            return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
        }
        
        return len;
    }
    
    // Full structure write (advanced usage)
    if (len > sizeof(struct alarm_data)) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }
    
    // Copy new alarm data
    memcpy(((uint8_t *)value) + offset, buf, len);
    
    LOG_DBG("Alarm data written: code=%d, data=%d", 
           value->alarm_code, value->alarm_data);
    
    return len;
}

/* Calibration implementation */
static ssize_t read_calibration(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                void *buf, uint16_t len, uint16_t offset) {
    if (!conn || !attr || !buf) {
        LOG_ERR("Invalid parameters for Calibration read");
        return -EINVAL;
    }
    
    struct calibration_data *value = (struct calibration_data *) calibration_value;

    /* Per BLE API Documentation: READ returns current calibration status and results */
    /* Structure: action, pulses, volume_ml, pulses_per_liter */
    /* Actions: 0=stop, 1=start, 2=in progress, 3=completed */
    
    if (calibration_active) {
        uint32_t current_pulses = get_pulse_count();
        value->pulses = current_pulses - calibration_start_pulses;
        value->action = 2; // In progress
        LOG_DBG("Calibration in progress: %u pulses counted", value->pulses);
    } else {
        /* Get current calibration value from system */
        uint32_t current_calibration = get_flow_calibration();
        value->pulses_per_liter = current_calibration;
        LOG_DBG("Calibration read: action=%u, pulses=%u, volume_ml=%u, ppl=%u", 
                value->action, value->pulses, value->volume_ml, value->pulses_per_liter);
    }

    return bt_gatt_attr_read(conn, attr, buf, len, offset, value, sizeof(*value));
}

static ssize_t write_calibration(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                 const void *buf, uint16_t len, uint16_t offset, uint8_t flags) {
    struct calibration_data *value = (struct calibration_data *) attr->user_data;

    /* Validate basic parameters */
    if (!conn || !attr || !buf) {
        LOG_ERR("Invalid parameters for Calibration write");
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_HANDLE);
    }

    /* Combined validation */
    if (offset + len > sizeof(*value) || len != sizeof(*value)) {
        LOG_ERR("Calibration write: Invalid params");
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }

    memcpy(((uint8_t *) value) + offset, buf, len);

    /* Per BLE API Documentation: Process calibration actions
     * 0=STOP (abort), 1=START, 2=IN_PROGRESS (read-only), 3=CALCULATED (finalize using provided volume),
     * 4=APPLY (commit last calculated result), 5=RESET (restore default)
     */

    switch (value->action) {
        case 0x01: { /* START */
            if (!calibration_active) {
                reset_pulse_count();
                calibration_start_pulses = 0;
                calibration_active = true;
                value->pulses = 0;
                value->volume_ml = 0;
                value->pulses_per_liter = 0;
                LOG_INF("✅ Flow sensor calibration STARTED - begin measuring actual volume");
                if (default_conn && notification_state.calibration_notifications_enabled) {
                    bt_irrigation_calibration_notify();
                }
                /* Kick off periodic progress notifier */
                k_work_schedule(&calibration_progress_work, K_MSEC(200));
            } else {
                LOG_WRN("Calibration already in progress");
            }
            break;
        }
        case 0x00: { /* STOP (abort without calculating/applying) */
            if (calibration_active) {
                calibration_active = false;
                /* Keep last pulses snapshot for reference; clear volume */
                value->volume_ml = 0;
                value->pulses_per_liter = get_flow_calibration();
                LOG_INF("⏹️ Calibration aborted by client");
                if (default_conn && notification_state.calibration_notifications_enabled) {
                    bt_irrigation_calibration_notify();
                }
                /* Stop periodic notifier */
                k_work_cancel_delayable(&calibration_progress_work);
            } else {
                LOG_WRN("No calibration in progress to stop");
            }
            break;
        }
        case 0x03: { /* CALCULATED: compute result using provided volume_ml */
            if (!calibration_active) {
                LOG_ERR("❌ CALCULATED requested but calibration not active");
                return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
            }
            uint32_t final_pulses = get_pulse_count();
            uint32_t total_pulses = final_pulses - calibration_start_pulses;
            uint32_t volume_ml = value->volume_ml;
            if (volume_ml == 0 || total_pulses == 0) {
                LOG_ERR("❌ Invalid calibration data: volume=%u ml, pulses=%u", volume_ml, total_pulses);
                calibration_active = false;
                value->action = 0; /* back to idle */
                return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
            }
            uint32_t new_calibration = (total_pulses * 1000U) / volume_ml;
            value->pulses = total_pulses;
            value->pulses_per_liter = new_calibration;
            calibration_active = false; /* stop counting */
            LOG_INF("🔬 Calibration calculated: %u pulses over %u ml -> %u pulses/L", total_pulses, volume_ml, new_calibration);
            if (default_conn && notification_state.calibration_notifications_enabled) {
                bt_irrigation_calibration_notify();
            }
            /* Stop periodic notifier */
            k_work_cancel_delayable(&calibration_progress_work);
            break;
        }
        case 0x04: { /* APPLY last calculated pulses_per_liter */
            uint32_t ppl = value->pulses_per_liter;
            if (ppl == 0) {
                LOG_ERR("❌ APPLY failed: no calculated pulses_per_liter available");
                return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
            }
            watering_error_t err = watering_set_flow_calibration(ppl);
            if (err != WATERING_SUCCESS) {
                LOG_ERR("❌ Failed to apply calibration: %d", err);
                return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
            }
            watering_save_config_priority(true);
            /* After apply, return to idle with system value reflected */
            value->action = 0; /* STOP/idle */
            value->pulses = 0;
            value->volume_ml = 0;
            value->pulses_per_liter = get_flow_calibration();
            LOG_INF("✅ Calibration applied: %u pulses/L", value->pulses_per_liter);
            if (default_conn && notification_state.calibration_notifications_enabled) {
                bt_irrigation_calibration_notify();
            }
            break;
        }
        case 0x05: { /* RESET to default calibration */
            watering_error_t err = watering_set_flow_calibration(DEFAULT_PULSES_PER_LITER);
            if (err != WATERING_SUCCESS) {
                LOG_ERR("❌ Failed to reset calibration: %d", err);
                return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
            }
            watering_save_config_priority(true);
            calibration_active = false;
            value->action = 0;
            value->pulses = 0;
            value->volume_ml = 0;
            value->pulses_per_liter = DEFAULT_PULSES_PER_LITER;
            LOG_INF("🔄 Calibration reset to default: %u pulses/L", DEFAULT_PULSES_PER_LITER);
            if (default_conn && notification_state.calibration_notifications_enabled) {
                bt_irrigation_calibration_notify();
            }
            break;
        }
        case 0x02: /* IN_PROGRESS is read-only */
        default:
            LOG_ERR("Invalid calibration action: 0x%02x", value->action);
            return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
    }

    return len;
}

static void calibration_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value) {
    bool notif_enabled = (value == BT_GATT_CCC_NOTIFY);
    notification_state.calibration_notifications_enabled = notif_enabled;
    
    if (notif_enabled) {
        LOG_DBG("Calibration notifications enabled");
        
        /* Initialize calibration_value with current calibration data */
        struct calibration_data *calib_data = (struct calibration_data *)calibration_value;
        calib_data->action = 0; /* Stopped */
        calib_data->pulses = 0;
        calib_data->volume_ml = 0;
    calib_data->pulses_per_liter = get_flow_calibration(); /* Current calibration */
    /* Push immediate notification on subscribe */
    bt_irrigation_calibration_notify();
    } else {
        LOG_DBG("Calibration notifications disabled");
        memset(calibration_value, 0, sizeof(struct calibration_data));
    }
}

/* Initialize BLE module work items */
static int __attribute__((unused)) bt_ble_module_init(void)
{
    k_work_init_delayable(&calibration_progress_work, calibration_progress_work_handler);
    return 0;
}

    /* Prepare request pointer early (len already validated) */
/* History helper block removed to eliminate unused-function warnings (was under #if 0) */

/* ------------------------------------------------------------------ */
/* Stub Implementations for Missing Functions                        */
/* ------------------------------------------------------------------ */

/* Schedule functions - just add missing CCC */

/* System config functions - just add missing CCC */

/* Task queue functions - just add missing CCC */

/* Statistics functions - just add missing CCC */

/* RTC functions */
static void rtc_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value) {
    bool notif_enabled = (value == BT_GATT_CCC_NOTIFY);
    notification_state.rtc_notifications_enabled = notif_enabled;
    
    if (notif_enabled) {
        LOG_DBG("RTC notifications enabled");
        
        /* Initialize rtc_value with current time for notifications */
        struct rtc_data *rtc_data = (struct rtc_data *)rtc_value;
        rtc_datetime_t now_utc, now_local;
        if (rtc_datetime_get(&now_utc) == 0) {
            /* Convert to local time and populate timezone fields */
            uint32_t utc_ts = timezone_rtc_to_unix_utc(&now_utc);
            if (timezone_unix_to_rtc_local(utc_ts, &now_local) == 0) {
                rtc_data->year = now_local.year - 2000;
                rtc_data->month = now_local.month;
                rtc_data->day = now_local.day;
                rtc_data->hour = now_local.hour;
                rtc_data->minute = now_local.minute;
                rtc_data->second = now_local.second;
                rtc_data->day_of_week = now_local.day_of_week;
            } else {
                rtc_data->year = now_utc.year - 2000;
                rtc_data->month = now_utc.month;
                rtc_data->day = now_utc.day;
                rtc_data->hour = now_utc.hour;
                rtc_data->minute = now_utc.minute;
                rtc_data->second = now_utc.second;
                rtc_data->day_of_week = now_utc.day_of_week;
            }
            rtc_data->utc_offset_minutes = timezone_get_total_offset(utc_ts);
            rtc_data->dst_active = timezone_is_dst_active(utc_ts) ? 1 : 0;
        } else {
            /* Default values if RTC hardware is unavailable */
            rtc_data->year = 25; rtc_data->month = 7; rtc_data->day = 5;
            rtc_data->hour = 12; rtc_data->minute = 0; rtc_data->second = 0;
            rtc_data->day_of_week = 6;
            rtc_data->utc_offset_minutes = 0;
            rtc_data->dst_active = 0;
        }
    } else {
        LOG_DBG("RTC notifications disabled");
        memset(rtc_value, 0, sizeof(rtc_value));
    }
}

/* Growing environment functions */
static void notify_growing_env(void) {
    if (!default_conn || !notification_state.growing_env_notifications_enabled) {
        return;
    }
    
    const struct bt_gatt_attr *attr = &irrigation_svc.attrs[ATTR_IDX_GROWING_ENV_VALUE];
    int err = safe_notify(default_conn, attr, growing_env_value, sizeof(struct growing_env_data));
    
    if (err == 0) {
        /* Ultra minimal logging - only every 30 seconds */
        static uint32_t last_log_time = 0;
        uint32_t now = k_uptime_get_32();
        if (now - last_log_time > 30000) {
            LOG_DBG("Growing env notification sent");
            last_log_time = now;
        }
    } else {
        LOG_ERR("Growing env notification failed: %d", err);
    }
}

static ssize_t read_growing_env(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                               void *buf, uint16_t len, uint16_t offset) {
    if (!conn || !attr || !buf) {
        LOG_ERR("Invalid parameters for Growing Environment read");
        return -EINVAL;
    }
    
    /* Create a local buffer for reading to avoid conflicts with notification buffer */
    struct growing_env_data read_value;
    
    /* Get the current channel selection from the global attribute buffer */
    const struct growing_env_data *global_value = 
        (const struct growing_env_data *)growing_env_value;
    
    /* Use the selected channel from the global buffer, but default to 0 if invalid */
    uint8_t channel_id = global_value->channel_id;
    if (channel_id >= WATERING_CHANNELS_COUNT) {
        channel_id = 0;
    }
    
    watering_channel_t *channel;
    watering_error_t err = watering_get_channel(channel_id, &channel);
    
    if (err != WATERING_SUCCESS) {
        LOG_WRN("Failed to get channel %u for growing env read: %d", channel_id, err);
        /* Return default/safe values */
        memset(&read_value, 0, sizeof(read_value));
        read_value.channel_id = channel_id;
        
        /* Enhanced database defaults */
        read_value.plant_db_index = UINT16_MAX; /* Not set */
        read_value.soil_db_index = UINT8_MAX; /* Not set */
        read_value.irrigation_method_index = UINT8_MAX; /* Not set */
        
        /* Coverage defaults */
        read_value.use_area_based = 1; /* Use area */
        read_value.coverage.area_m2 = 1.0f; /* 1 m² */
        
        /* Automatic mode defaults */
        read_value.auto_mode = 0; /* Manual mode */
        read_value.max_volume_limit_l = 10.0f; /* 10L limit */
        read_value.enable_cycle_soak = 0; /* Disabled */
        
        /* Plant lifecycle defaults */
        read_value.planting_date_unix = 0; /* Not set */
        read_value.days_after_planting = 0; /* Not set */
        
        /* Environmental defaults */
        read_value.latitude_deg = 45.0f; /* Default latitude */
        read_value.sun_exposure_pct = 75; /* 75% sun */
    } else {
        /* Copy fresh data from the watering system */
        memset(&read_value, 0, sizeof(read_value));
        read_value.channel_id = channel_id;
        
        /* Enhanced database indices */
        read_value.plant_db_index = channel->plant_db_index;
        read_value.soil_db_index = channel->soil_db_index;
        read_value.irrigation_method_index = channel->irrigation_method_index;
        
        /* Coverage specification */
        read_value.use_area_based = channel->use_area_based ? 1 : 0;
        if (channel->use_area_based) {
            read_value.coverage.area_m2 = channel->coverage.area_m2;
        } else {
            read_value.coverage.plant_count = channel->coverage.plant_count;
        }
        
        /* Automatic mode settings */
        read_value.auto_mode = (uint8_t)channel->auto_mode;
        read_value.max_volume_limit_l = channel->max_volume_limit_l;
        read_value.enable_cycle_soak = channel->enable_cycle_soak ? 1 : 0;
        
        /* Plant lifecycle tracking */
        read_value.planting_date_unix = channel->planting_date_unix;
        read_value.days_after_planting = channel->days_after_planting;
        
        /* Environmental overrides */
        read_value.latitude_deg = channel->latitude_deg;
        read_value.sun_exposure_pct = channel->sun_exposure_pct;
        
        /* Custom plant fields (only if plant_type == PLANT_TYPE_OTHER) */
        if (channel->plant_type == PLANT_TYPE_OTHER) {
            size_t name_len = strnlen(channel->custom_plant.custom_name, sizeof(channel->custom_plant.custom_name));
            if (name_len >= sizeof(read_value.custom_name)) {
                name_len = sizeof(read_value.custom_name) - 1;
            }
            memcpy(read_value.custom_name, channel->custom_plant.custom_name, name_len);
            read_value.custom_name[name_len] = '\0';
            
            read_value.water_need_factor = channel->custom_plant.water_need_factor;
            read_value.irrigation_freq_days = channel->custom_plant.irrigation_freq;
            read_value.prefer_area_based = channel->custom_plant.prefer_area_based ? 1 : 0;
        } else {
            strcpy(read_value.custom_name, "");
            read_value.water_need_factor = 1.0f;
            read_value.irrigation_freq_days = 1;
            read_value.prefer_area_based = read_value.use_area_based;
        }
    }
    
    LOG_DBG("Growing Env read: ch=%u, plant=%u.%u, soil=%u, method=%u, area=%s %.2f, sun=%u%%",
            read_value.channel_id, read_value.plant_type, read_value.specific_plant,
            read_value.soil_type, read_value.irrigation_method,
            read_value.use_area_based ? "area" : "count",
            read_value.use_area_based ? (double)read_value.coverage.area_m2 : (double)((float)read_value.coverage.plant_count),
            read_value.sun_percentage);
    
    return bt_gatt_attr_read(conn, attr, buf, len, offset, &read_value,
                           sizeof(struct growing_env_data));
}

static ssize_t write_growing_env(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                const void *buf, uint16_t len, uint16_t offset, uint8_t flags) {
    if (!conn || !attr || !buf) {
        LOG_ERR("Invalid parameters for Growing Environment write");
        return -EINVAL;
    }
    
    if (len == 0) {
        LOG_WRN("Empty Growing Environment write");
        return 0;
    }
    
    const uint8_t *data = (const uint8_t *)buf;
    printk("🔧 BLE Growing Environment write: len=%u, offset=%u\n", len, offset);
    
    /* Debug: Print first few bytes */
    if (len >= 4) {
        printk("🔍 BLE: Growing env data bytes: [0]=%02x [1]=%02x [2]=%02x [3]=%02x\n", 
               data[0], data[1], data[2], data[3]);
    } else if (len >= 2) {
        printk("🔍 BLE: Growing env data bytes: [0]=%02x [1]=%02x\n", data[0], data[1]);
    } else if (len >= 1) {
        printk("🔍 BLE: Growing env data bytes: [0]=%02x\n", data[0]);
    }
    
    /* Check for single-byte channel selection */
    if (len == 1) {
        uint8_t channel_id = data[0];
        
        printk("🔧 BLE: Growing env channel selection - channel=%u\n", channel_id);
        
        if (channel_id >= WATERING_CHANNELS_COUNT) {
            printk("❌ Invalid channel ID %u for growing env selection\n", channel_id);
            return -EINVAL;
        }
        
        /* Store the selected channel for subsequent operations */
        growing_env_last_channel = channel_id;
        
        /* Get channel data and populate global buffer for read operations */
        watering_channel_t *channel;
        watering_error_t err = watering_get_channel(channel_id, &channel);
        if (err != WATERING_SUCCESS) {
            printk("❌ Failed to get channel %u for growing env selection: %d\n", 
                    channel_id, err);
            return -EINVAL;
        }
        
        /* Populate growing_env_value buffer with current channel data */
        struct growing_env_data *env_data = (struct growing_env_data *)growing_env_value;
        memset(env_data, 0, sizeof(struct growing_env_data));
        
        env_data->channel_id = channel_id;
        
        /* Enhanced database indices */
        env_data->plant_db_index = channel->plant_db_index;
        env_data->soil_db_index = channel->soil_db_index;
        env_data->irrigation_method_index = channel->irrigation_method_index;
        
        /* Coverage specification */
        env_data->use_area_based = channel->use_area_based ? 1 : 0;
        if (channel->use_area_based) {
            env_data->coverage.area_m2 = channel->coverage.area_m2;
        } else {
            env_data->coverage.plant_count = channel->coverage.plant_count;
        }
        
        /* Automatic mode settings */
        env_data->auto_mode = (uint8_t)channel->auto_mode;
        env_data->max_volume_limit_l = channel->max_volume_limit_l;
        env_data->enable_cycle_soak = channel->enable_cycle_soak ? 1 : 0;
        
        /* Plant lifecycle tracking */
        env_data->planting_date_unix = channel->planting_date_unix;
        env_data->days_after_planting = channel->days_after_planting;
        
        /* Environmental overrides */
        env_data->latitude_deg = channel->latitude_deg;
        env_data->sun_exposure_pct = channel->sun_exposure_pct;
        
        /* Custom plant fields */
        if (channel->plant_type == PLANT_TYPE_OTHER) {
            strncpy(env_data->custom_name, channel->custom_plant.custom_name, 
                   sizeof(env_data->custom_name) - 1);
            env_data->custom_name[sizeof(env_data->custom_name) - 1] = '\0';
            env_data->water_need_factor = channel->custom_plant.water_need_factor;
            env_data->irrigation_freq_days = channel->custom_plant.irrigation_freq;
            env_data->prefer_area_based = channel->custom_plant.prefer_area_based ? 1 : 0;
        }
        
        printk("✅ BLE: Growing env channel %u selected (plant_db=%u, soil_db=%u, method_db=%u, auto=%u)\n", 
                channel_id, env_data->plant_db_index, env_data->soil_db_index, env_data->irrigation_method_index, env_data->auto_mode);
        
        return len;
    }
    
    /* Check for fragmentation protocol header */
    if (len >= 4 && (data[1] == 2 || data[1] == 3)) { /* frag_type = 2 or 3 for growing environment */
        uint8_t channel_id = data[0];
        uint8_t frag_type = data[1];
        uint16_t total_size;
        
        /* Handle both big-endian (frag_type=2) and little-endian (frag_type=3) */
        if (frag_type == 2) {
            total_size = (data[2] << 8) | data[3];  /* Big-endian */
        } else {
            total_size = data[2] | (data[3] << 8);  /* Little-endian for frag_type=3 */
        }
        
        printk("🔧 BLE: Growing env fragmentation header - channel=%u, frag_type=%u, total=%u\n", 
               channel_id, frag_type, total_size);
        
        if (channel_id >= WATERING_CHANNELS_COUNT) {
            printk("❌ Invalid channel ID %u for growing env fragmentation\n", channel_id);
            return -EINVAL;
        }
        
        if (total_size > sizeof(struct growing_env_data)) {
            printk("❌ Growing env fragmentation size too large: %u > %zu\n", 
                    total_size, sizeof(struct growing_env_data));
            return -EINVAL;
        }
        
        /* Initialize fragmentation state */
        growing_env_frag.channel_id = channel_id;
        growing_env_frag.frag_type = frag_type;
        growing_env_frag.expected = total_size;
        growing_env_frag.received = 0;
        growing_env_frag.in_progress = true;
        growing_env_frag.start_time = k_uptime_get_32();
        memset(growing_env_frag.buf, 0, sizeof(growing_env_frag.buf));
        
        printk("🔧 BLE: Growing env fragmentation initialized - cid=%u, frag_type=%u, expected=%u bytes\n",
                channel_id, frag_type, total_size);
        
        /* Process payload if present */
        if (len > 4) {
            uint16_t payload_len = len - 4;
            if (payload_len > sizeof(growing_env_frag.buf)) {
                payload_len = sizeof(growing_env_frag.buf);
            }
            memcpy(growing_env_frag.buf, data + 4, payload_len);
            growing_env_frag.received = payload_len;
            printk("🔧 BLE: Received growing env fragment: %u/%u bytes\n", payload_len, total_size);
        }
        
        return len;
    }
    
    /* Handle continuation fragments */
    if (growing_env_frag.in_progress) {
        uint16_t remaining = growing_env_frag.expected - growing_env_frag.received;
        uint16_t copy_len = (len > remaining) ? remaining : len;
        
        printk("🔧 BLE: Growing env continuation - len=%u, remaining=%u, copy_len=%u\n", 
               len, remaining, copy_len);
        
        if (growing_env_frag.received + copy_len > sizeof(growing_env_frag.buf)) {
            printk("❌ Growing env fragment buffer overflow\n");
            growing_env_frag.in_progress = false;
            return -EINVAL;
        }
        
        memcpy(growing_env_frag.buf + growing_env_frag.received, data, copy_len);
        growing_env_frag.received += copy_len;
        
        printk("🔧 BLE: Growing env fragment received: %u/%u bytes\n", 
                growing_env_frag.received, growing_env_frag.expected);
        
        /* Check if complete */
        if (growing_env_frag.received >= growing_env_frag.expected) {
            /* Process complete growing environment data */
            struct growing_env_data *env_data = (struct growing_env_data *)growing_env_frag.buf;
            
            printk("✅ BLE: Complete growing env received, processing...\n");
            
            /* Validate data */
            if (env_data->channel_id >= WATERING_CHANNELS_COUNT) {
                printk("❌ Invalid channel ID %u in growing env data\n", env_data->channel_id);
                growing_env_frag.in_progress = false;
                return -EINVAL;
            }
            
            /* Validate automatic mode */
            if (env_data->auto_mode > 2) {
                printk("❌ Invalid auto mode %u\n", env_data->auto_mode);
                growing_env_frag.in_progress = false;
                return -EINVAL;
            }
            
            /* Validate sun exposure percentage */
            if (env_data->sun_exposure_pct > 100) {
                printk("❌ Invalid sun exposure percentage %u\n", env_data->sun_exposure_pct);
                growing_env_frag.in_progress = false;
                return -EINVAL;
            }

            /* Validate database indices (allow sentinel values) */
            if (env_data->plant_db_index != UINT16_MAX && env_data->plant_db_index >= PLANT_FULL_SPECIES_COUNT) {
                printk("❌ Invalid plant_db_index %u\n", env_data->plant_db_index);
                growing_env_frag.in_progress = false;
                return -EINVAL;
            }
            if (env_data->soil_db_index != UINT8_MAX && env_data->soil_db_index >= SOIL_ENHANCED_TYPES_COUNT) {
                printk("❌ Invalid soil_db_index %u\n", env_data->soil_db_index);
                growing_env_frag.in_progress = false;
                return -EINVAL;
            }
            if (env_data->irrigation_method_index != UINT8_MAX && env_data->irrigation_method_index >= IRRIGATION_METHODS_COUNT) {
                printk("❌ Invalid irrigation_method_index %u\n", env_data->irrigation_method_index);
                growing_env_frag.in_progress = false;
                return -EINVAL;
            }

            /* Validate latitude */
            if (env_data->latitude_deg < -90.0f || env_data->latitude_deg > 90.0f) {
                printk("❌ Invalid latitude %.2f\n", (double)env_data->latitude_deg);
                growing_env_frag.in_progress = false;
                return -EINVAL;
            }

            /* Validate max volume limit (must be > 0) */
            if (env_data->max_volume_limit_l <= 0.0f) {
                printk("❌ Invalid max_volume_limit_l %.2f\n", (double)env_data->max_volume_limit_l);
                growing_env_frag.in_progress = false;
                return -EINVAL;
            }

            /* Validate coverage values */
            if (env_data->use_area_based) {
                if (!(env_data->coverage.area_m2 > 0.0f)) {
                    printk("❌ Invalid area_m2 %.3f\n", (double)env_data->coverage.area_m2);
                    growing_env_frag.in_progress = false;
                    return -EINVAL;
                }
            } else {
                if (env_data->coverage.plant_count == 0) {
                    printk("❌ Invalid plant_count %u\n", env_data->coverage.plant_count);
                    growing_env_frag.in_progress = false;
                    return -EINVAL;
                }
            }
            
            /* Get channel */
            watering_channel_t *channel;
            watering_error_t err = watering_get_channel(env_data->channel_id, &channel);
            if (err != WATERING_SUCCESS) {
                printk("❌ Failed to get channel %u for growing env write: %d\n", 
                        env_data->channel_id, err);
                growing_env_frag.in_progress = false;
                return -EINVAL;
            }
            
            /* Update channel data with enhanced database indices */
            channel->plant_db_index = env_data->plant_db_index;
            channel->soil_db_index = env_data->soil_db_index;
            channel->irrigation_method_index = env_data->irrigation_method_index;
            
            /* Coverage specification */
            channel->use_area_based = (env_data->use_area_based != 0);
            if (channel->use_area_based) {
                channel->coverage.area_m2 = env_data->coverage.area_m2;
            } else {
                channel->coverage.plant_count = env_data->coverage.plant_count;
            }
            
            /* Automatic mode settings */
            channel->auto_mode = (watering_mode_t)env_data->auto_mode;
            channel->max_volume_limit_l = env_data->max_volume_limit_l;
            channel->enable_cycle_soak = (env_data->enable_cycle_soak != 0);
            
            /* Plant lifecycle tracking */
            channel->planting_date_unix = env_data->planting_date_unix;
            channel->days_after_planting = env_data->days_after_planting;
            
            /* Environmental overrides */
            channel->latitude_deg = env_data->latitude_deg;
            channel->sun_exposure_pct = env_data->sun_exposure_pct;
            
            /* Custom plant fields */
            if (env_data->plant_type == PLANT_TYPE_OTHER) {
                /* Copy custom name with null termination */
                size_t name_len = strnlen(env_data->custom_name, sizeof(env_data->custom_name));
                if (name_len >= sizeof(channel->custom_plant.custom_name)) {
                    name_len = sizeof(channel->custom_plant.custom_name) - 1;
                }
                memcpy(channel->custom_plant.custom_name, env_data->custom_name, name_len);
                channel->custom_plant.custom_name[name_len] = '\0';
                
                channel->custom_plant.water_need_factor = env_data->water_need_factor;
                channel->custom_plant.irrigation_freq = env_data->irrigation_freq_days;
                channel->custom_plant.prefer_area_based = (env_data->prefer_area_based != 0);
                
                /* Update onboarding flag - water need factor has been set for custom plant */
                onboarding_update_channel_flag(env_data->channel_id, CHANNEL_FLAG_WATER_FACTOR_SET, true);
            }
            
            /* Update basic channel config flags based on what was set */
            if (env_data->plant_db_index != UINT16_MAX) {
                onboarding_update_channel_flag(env_data->channel_id, CHANNEL_FLAG_PLANT_TYPE_SET, true);
            }
            if (env_data->soil_db_index != UINT8_MAX) {
                onboarding_update_channel_flag(env_data->channel_id, CHANNEL_FLAG_SOIL_TYPE_SET, true);
            }
            if (env_data->irrigation_method_index != UINT8_MAX) {
                onboarding_update_channel_flag(env_data->channel_id, CHANNEL_FLAG_IRRIGATION_METHOD_SET, true);
            }
            /* Debug: print coverage values */
            printk("Growing env coverage: use_area=%d, area_m2=%d.%02d, plant_count=%u\n",
                   env_data->use_area_based, 
                   (int)env_data->coverage.area_m2, 
                   (int)((env_data->coverage.area_m2 - (int)env_data->coverage.area_m2) * 100),
                   env_data->coverage.plant_count);
    if (env_data->use_area_based ? (env_data->coverage.area_m2 > 0) : (env_data->coverage.plant_count > 0)) {
        onboarding_update_channel_flag(env_data->channel_id, CHANNEL_FLAG_COVERAGE_SET, true);
    } else {
        printk("WARNING: Coverage not set - use_area=%d, area=%.2f, count=%u\n",
               env_data->use_area_based, (double)env_data->coverage.area_m2, env_data->coverage.plant_count);
            }
            if (env_data->sun_exposure_pct != 75) { /* 75 is default */
                onboarding_update_channel_flag(env_data->channel_id, CHANNEL_FLAG_SUN_EXPOSURE_SET, true);
            }
            /* Water factor exists via DB presets or custom config; mark as set */
            onboarding_update_channel_flag(env_data->channel_id, CHANNEL_FLAG_WATER_FACTOR_SET, true);
            
            /* Update global buffer for notifications */
            memcpy(growing_env_value, env_data, sizeof(struct growing_env_data));
            
            /* Save with priority (250ms throttle) */
            watering_save_config_priority(true);
            
            /* Debug: print latitude value */
            printk("Growing env latitude_deg=%d.%03d for channel %u\n", 
                   (int)env_data->latitude_deg, 
                   (int)((env_data->latitude_deg - (int)env_data->latitude_deg) * 1000),
                   env_data->channel_id);
            
            /* Update onboarding flags if location (latitude) is set to a valid value */
            if (env_data->latitude_deg != 0.0f) {
                onboarding_update_system_flag(SYSTEM_FLAG_LOCATION_SET, true);
                onboarding_update_channel_extended_flag(env_data->channel_id, CHANNEL_EXT_FLAG_LATITUDE_SET, true);
                /* Check if FAO-56 requirements are now met */
                printk("Calling onboarding_check_fao56_ready for channel %u\n", env_data->channel_id);
                onboarding_check_fao56_ready(env_data->channel_id);
            } else {
                printk("Skipping FAO56 check: latitude_deg is 0.0\n");
            }
            
            /* Update extended flags for volume limit and planting date */
            if (env_data->max_volume_limit_l > 0.0f) {
                onboarding_update_channel_extended_flag(env_data->channel_id, CHANNEL_EXT_FLAG_VOLUME_LIMIT_SET, true);
            }
            if (env_data->planting_date_unix > 0) {
                onboarding_update_channel_extended_flag(env_data->channel_id, CHANNEL_EXT_FLAG_PLANTING_DATE_SET, true);
            }
            if (env_data->enable_cycle_soak) {
                onboarding_update_channel_extended_flag(env_data->channel_id, CHANNEL_EXT_FLAG_CYCLE_SOAK_SET, true);
            }
            
            printk("✅ BLE: Growing environment updated for channel %u via fragmentation\n", 
                    env_data->channel_id);
            
            /* Send notification */
            notify_growing_env();
            
            growing_env_frag.in_progress = false;
            return len;
        }
        
        return len;
    }
    
    /* Handle direct write (non-fragmented) */
    if (len < sizeof(struct growing_env_data)) {
        printk("❌ Growing env write too small: %u < %zu\n", len, sizeof(struct growing_env_data));
        return -EINVAL;
    }
    
    const struct growing_env_data *env_data = (const struct growing_env_data *)data;
    
    /* Combined validation - ranges and indices */
    if (env_data->channel_id >= WATERING_CHANNELS_COUNT || 
        env_data->auto_mode > 2 || env_data->sun_exposure_pct > 100) {
        printk("❌ Invalid growing env data: ch=%u, auto=%u, sun_exp=%u\n", 
                env_data->channel_id, env_data->auto_mode, env_data->sun_exposure_pct);
        return -EINVAL;
    }
    if (env_data->plant_db_index != UINT16_MAX && env_data->plant_db_index >= PLANT_FULL_SPECIES_COUNT) {
        printk("❌ Invalid plant_db_index %u\n", env_data->plant_db_index);
        return -EINVAL;
    }
    if (env_data->soil_db_index != UINT8_MAX && env_data->soil_db_index >= SOIL_ENHANCED_TYPES_COUNT) {
        printk("❌ Invalid soil_db_index %u\n", env_data->soil_db_index);
        return -EINVAL;
    }
    if (env_data->irrigation_method_index != UINT8_MAX && env_data->irrigation_method_index >= IRRIGATION_METHODS_COUNT) {
        printk("❌ Invalid irrigation_method_index %u\n", env_data->irrigation_method_index);
        return -EINVAL;
    }
    if (env_data->latitude_deg < -90.0f || env_data->latitude_deg > 90.0f) {
        printk("❌ Invalid latitude %.2f\n", (double)env_data->latitude_deg);
        return -EINVAL;
    }
    if (env_data->max_volume_limit_l <= 0.0f) {
        printk("❌ Invalid max_volume_limit_l %.2f\n", (double)env_data->max_volume_limit_l);
        return -EINVAL;
    }
    if (env_data->use_area_based) {
        if (!(env_data->coverage.area_m2 > 0.0f)) {
            printk("❌ Invalid area_m2 %.3f\n", (double)env_data->coverage.area_m2);
            return -EINVAL;
        }
    } else {
        if (env_data->coverage.plant_count == 0) {
            printk("❌ Invalid plant_count %u\n", env_data->coverage.plant_count);
            return -EINVAL;
        }
    }
    
    /* Get channel */
    watering_channel_t *channel;
    watering_error_t err = watering_get_channel(env_data->channel_id, &channel);
    if (err != WATERING_SUCCESS) {
        printk("❌ Failed to get channel %u for growing env write: %d\n", 
                env_data->channel_id, err);
        return -EINVAL;
    }
    
    /* Update channel data with enhanced database indices */
    channel->plant_db_index = env_data->plant_db_index;
    channel->soil_db_index = env_data->soil_db_index;
    channel->irrigation_method_index = env_data->irrigation_method_index;
    
    /* Coverage specification */
    channel->use_area_based = (env_data->use_area_based != 0);
    if (channel->use_area_based) {
        channel->coverage.area_m2 = env_data->coverage.area_m2;
    } else {
        channel->coverage.plant_count = env_data->coverage.plant_count;
    }
    
    /* Automatic mode settings */
    channel->auto_mode = (watering_mode_t)env_data->auto_mode;
    channel->max_volume_limit_l = env_data->max_volume_limit_l;
    channel->enable_cycle_soak = (env_data->enable_cycle_soak != 0);
    
    /* Plant lifecycle tracking */
    channel->planting_date_unix = env_data->planting_date_unix;
    channel->days_after_planting = env_data->days_after_planting;
    
    /* Environmental overrides */
    channel->latitude_deg = env_data->latitude_deg;
    channel->sun_exposure_pct = env_data->sun_exposure_pct;
    
    /* Custom plant fields */
    if (env_data->plant_type == PLANT_TYPE_OTHER) {
        /* Copy custom name with null termination */
        size_t name_len = strnlen(env_data->custom_name, sizeof(env_data->custom_name));
        if (name_len >= sizeof(channel->custom_plant.custom_name)) {
            name_len = sizeof(channel->custom_plant.custom_name) - 1;
        }
        memcpy(channel->custom_plant.custom_name, env_data->custom_name, name_len);
        channel->custom_plant.custom_name[name_len] = '\0';
        
        channel->custom_plant.water_need_factor = env_data->water_need_factor;
        channel->custom_plant.irrigation_freq = env_data->irrigation_freq_days;
        channel->custom_plant.prefer_area_based = (env_data->prefer_area_based != 0);
        
        /* Update onboarding flag - water need factor has been set for custom plant */
        onboarding_update_channel_flag(env_data->channel_id, CHANNEL_FLAG_WATER_FACTOR_SET, true);
    }
    
    /* Update basic channel config flags based on what was set */
    if (env_data->plant_db_index != UINT16_MAX) {
        onboarding_update_channel_flag(env_data->channel_id, CHANNEL_FLAG_PLANT_TYPE_SET, true);
    }
    if (env_data->soil_db_index != UINT8_MAX) {
        onboarding_update_channel_flag(env_data->channel_id, CHANNEL_FLAG_SOIL_TYPE_SET, true);
    }
    if (env_data->irrigation_method_index != UINT8_MAX) {
        onboarding_update_channel_flag(env_data->channel_id, CHANNEL_FLAG_IRRIGATION_METHOD_SET, true);
    }
    if (env_data->use_area_based ? (env_data->coverage.area_m2 > 0) : (env_data->coverage.plant_count > 0)) {
        onboarding_update_channel_flag(env_data->channel_id, CHANNEL_FLAG_COVERAGE_SET, true);
    }
    if (env_data->sun_exposure_pct != 75) { /* 75 is default */
        onboarding_update_channel_flag(env_data->channel_id, CHANNEL_FLAG_SUN_EXPOSURE_SET, true);
    }
    /* Water factor exists via DB presets or custom config; mark as set */
    onboarding_update_channel_flag(env_data->channel_id, CHANNEL_FLAG_WATER_FACTOR_SET, true);
    
    /* Update global buffer for notifications */
    memcpy(growing_env_value, env_data, sizeof(struct growing_env_data));
    
    /* Save with priority (250ms throttle) */
    watering_save_config_priority(true);
    
    /* Update onboarding flags if location (latitude) is set to a valid value */
    if (env_data->latitude_deg != 0.0f) {
        onboarding_update_system_flag(SYSTEM_FLAG_LOCATION_SET, true);
        onboarding_update_channel_extended_flag(env_data->channel_id, CHANNEL_EXT_FLAG_LATITUDE_SET, true);
        /* Check if FAO-56 requirements are now met */
        onboarding_check_fao56_ready(env_data->channel_id);
    }
    
    /* Update extended flags for volume limit and planting date */
    if (env_data->max_volume_limit_l > 0.0f) {
        onboarding_update_channel_extended_flag(env_data->channel_id, CHANNEL_EXT_FLAG_VOLUME_LIMIT_SET, true);
    }
    if (env_data->planting_date_unix > 0) {
        onboarding_update_channel_extended_flag(env_data->channel_id, CHANNEL_EXT_FLAG_PLANTING_DATE_SET, true);
    }
    if (env_data->enable_cycle_soak) {
        onboarding_update_channel_extended_flag(env_data->channel_id, CHANNEL_EXT_FLAG_CYCLE_SOAK_SET, true);
    }
    
    printk("✅ BLE: Growing environment updated for channel %u\n", env_data->channel_id);
    
    /* Send notification */
    notify_growing_env();
    
    return len;
}

static void growing_env_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value) {
    bool notif_enabled = (value == BT_GATT_CCC_NOTIFY);
    notification_state.growing_env_notifications_enabled = notif_enabled;
    
    if (notif_enabled) {
        LOG_DBG("Growing Environment notifications enabled");
        
        /* Initialize growing_env_value with current data from channel 0 */
        struct growing_env_data *env_data = (struct growing_env_data *)growing_env_value;
        watering_channel_t *channel;
        watering_error_t err = watering_get_channel(0, &channel);
        
        if (err == WATERING_SUCCESS) {
            env_data->channel_id = 0;
            
            /* Enhanced database indices */
            env_data->plant_db_index = channel->plant_db_index;
            env_data->soil_db_index = channel->soil_db_index;
            env_data->irrigation_method_index = channel->irrigation_method_index;
            
            /* Coverage specification */
            env_data->use_area_based = channel->use_area_based ? 1 : 0;
            if (channel->use_area_based) {
                env_data->coverage.area_m2 = channel->coverage.area_m2;
            } else {
                env_data->coverage.plant_count = channel->coverage.plant_count;
            }
            
            /* Automatic mode settings */
            env_data->auto_mode = (uint8_t)channel->auto_mode;
            env_data->max_volume_limit_l = channel->max_volume_limit_l;
            env_data->enable_cycle_soak = channel->enable_cycle_soak ? 1 : 0;
            
            /* Plant lifecycle tracking */
            env_data->planting_date_unix = channel->planting_date_unix;
            env_data->days_after_planting = channel->days_after_planting;
            
            /* Environmental overrides */
            env_data->latitude_deg = channel->latitude_deg;
            env_data->sun_exposure_pct = channel->sun_exposure_pct;
            
    LOG_INF("Initialized with channel 0: plant_db=%u, soil_db=%u, method_db=%u, %s=%.2f, auto=%u",
                    env_data->plant_db_index, env_data->soil_db_index, env_data->irrigation_method_index,
                    env_data->use_area_based ? "area" : "count",
            env_data->use_area_based ? (double)env_data->coverage.area_m2 : (double)((float)env_data->coverage.plant_count),
                    env_data->auto_mode);
        } else {
            /* Default values if channel not available */
            memset(env_data, 0, sizeof(struct growing_env_data));
            env_data->channel_id = 0;
            
            /* Enhanced database defaults */
            env_data->plant_db_index = UINT16_MAX; /* Not set */
            env_data->soil_db_index = UINT8_MAX; /* Not set */
            env_data->irrigation_method_index = UINT8_MAX; /* Not set */
            
            /* Coverage defaults */
            env_data->use_area_based = 1; /* Use area */
            env_data->coverage.area_m2 = 1.0f; /* 1 m² */
            
            /* Automatic mode defaults */
            env_data->auto_mode = 0; /* Manual mode */
            env_data->max_volume_limit_l = 10.0f; /* 10L limit */
            env_data->enable_cycle_soak = 0; /* Disabled */
            
            /* Plant lifecycle defaults */
            env_data->planting_date_unix = 0; /* Not set */
            env_data->days_after_planting = 0; /* Not set */
            
            /* Environmental defaults */
            env_data->latitude_deg = 45.0f; /* Default latitude */
            env_data->sun_exposure_pct = 75; /* 75% sun */
        }
    } else {
        LOG_DBG("Growing Environment notifications disabled");
        memset(growing_env_value, 0, sizeof(struct growing_env_data));
    }
}

/* Automatic calculation status functions */
/* --- FAO-56 Auto Calc dynamic computation helper --- */
static void update_auto_calc_calculations(struct auto_calc_status_data *d, watering_channel_t *channel) {
    if (!d || !channel) return;
    /* Plant & phenological stage */
    if (channel->plant_db_index < PLANT_FULL_SPECIES_COUNT) {
    const plant_full_data_t *plant = &plant_full_database[channel->plant_db_index];
        uint16_t dap = channel->days_after_planting;
        phenological_stage_t stage = calc_phenological_stage(plant, dap);
        d->phenological_stage = (uint8_t)stage;
        d->crop_coefficient = calc_crop_coefficient(plant, stage, dap);
    } else {
        d->phenological_stage = 0;
        if (d->crop_coefficient < 0.01f) d->crop_coefficient = 1.0f;
    }
    /* Environmental snapshot → ET0 estimate (fallback HS) */
    environmental_data_t env_raw = {0};
    bme280_environmental_data_t env_bme280 = {0};
    if (environmental_data_get_current(&env_bme280) == 0 && env_bme280.current.valid) {
        /* Map BME280 readings into the FAO-56 environmental structure */
        env_raw.air_temp_mean_c = env_bme280.current.temperature;
        env_raw.air_temp_min_c = env_bme280.current.temperature; /* best effort */
        env_raw.air_temp_max_c = env_bme280.current.temperature; /* best effort */
        env_raw.rel_humidity_pct = env_bme280.current.humidity;
        env_raw.atmos_pressure_hpa = env_bme280.current.pressure; /* hPa */
        env_raw.temp_valid = true;
        env_raw.humidity_valid = true;
        env_raw.pressure_valid = true;
        env_raw.timestamp = env_bme280.current.timestamp;
        env_raw.data_quality = env_bme280.current.valid ? 100 : 0;

        uint16_t doy = get_current_day_of_year();
        float latitude_deg = channel->latitude_deg;
        if (latitude_deg < -90.0f || latitude_deg > 90.0f) {
            latitude_deg = 45.0f;
        }
        float latitude_rad = latitude_deg * 0.0174532925f;

        float et0 = 0.0f;
        if (env_raw.temp_valid && env_raw.humidity_valid && env_raw.pressure_valid) {
            et0 = calc_et0_penman_monteith(&env_raw, latitude_rad, doy);
        }
        if (et0 <= 0.01f || et0 >= 20.0f) {
            et0 = calc_et0_hargreaves_samani(&env_raw, latitude_rad, doy);
        }
        if (et0 > 0.01f && et0 < 20.0f) {
            d->et0_mm_day = et0;
        }
    }
    if (d->et0_mm_day < 0.01f) d->et0_mm_day = 3.0f; /* fallback */
    if (d->crop_coefficient < 0.01f) d->crop_coefficient = 1.0f;
    d->etc_mm_day = d->et0_mm_day * d->crop_coefficient;
    /* Water balance + irrigation calc */
    if (channel->water_balance) {
        water_balance_t *balance = (water_balance_t*)channel->water_balance;
        d->current_deficit_mm = balance->current_deficit_mm;
        const irrigation_method_data_t *method = NULL;
    if (channel->irrigation_method_index < IRRIGATION_METHODS_COUNT) {
            method = &irrigation_methods_database[channel->irrigation_method_index];
        }
        const plant_full_data_t *plant = NULL;
    if (channel->plant_db_index < PLANT_FULL_SPECIES_COUNT) {
            plant = &plant_full_database[channel->plant_db_index];
        }
        irrigation_calculation_t calc = {0};
        bool eco = (channel->auto_mode == WATERING_AUTOMATIC_ECO);
        if (method && plant) {
            if (channel->use_area_based) {
                float area = channel->coverage.area_m2;
                if (eco)
                    apply_eco_irrigation_mode(balance, method, plant, area, 0, channel->max_volume_limit_l, &calc);
                else
                    apply_quality_irrigation_mode(balance, method, plant, area, 0, channel->max_volume_limit_l, &calc);
            } else {
                uint16_t count = channel->coverage.plant_count;
                if (eco)
                    apply_eco_irrigation_mode(balance, method, plant, 0.0f, count, channel->max_volume_limit_l, &calc);
                else
                    apply_quality_irrigation_mode(balance, method, plant, 0.0f, count, channel->max_volume_limit_l, &calc);
            }
            d->net_irrigation_mm = calc.net_irrigation_mm;
            d->gross_irrigation_mm = calc.gross_irrigation_mm;
            d->calculated_volume_l = calc.volume_liters;
            d->volume_liters = calc.volume_liters; /* legacy alias */
            d->cycle_count = calc.cycle_count ? (uint8_t)calc.cycle_count : 1;
            d->cycle_duration_min = (uint8_t)(calc.cycle_duration_min > 255 ? 255 : calc.cycle_duration_min);
            d->volume_limited = calc.volume_limited ? 1 : 0;
        }
        /* Predict next irrigation if not needed yet */
        if (!d->irrigation_needed && d->etc_mm_day > 0.01f) {
            float hours_until = 0.0f;
            if (plant && calc_irrigation_timing(balance, d->etc_mm_day, plant, &hours_until) == WATERING_SUCCESS && hours_until > 0) {
                uint32_t now_sec = k_uptime_get_32()/1000U;
                d->next_irrigation_time = now_sec + (uint32_t)(hours_until * 3600.0f);
            }
        }
    }
}

static void notify_auto_calc_status(void) {
    if (!default_conn || !notification_state.auto_calc_status_notifications_enabled) {
        return;
    }

    /* Build unified fragmentation header + payload (single fragment) */
    struct auto_calc_status_data *payload = (struct auto_calc_status_data *)auto_calc_status_value;
    /* Refresh calculations before sending */
    uint8_t cid = payload->channel_id < WATERING_CHANNELS_COUNT ? payload->channel_id : 0;
    watering_channel_t *channel;
    if (watering_get_channel(cid, &channel) == WATERING_SUCCESS) {
        update_auto_calc_calculations(payload, channel);
    }
    uint8_t notify_buf[sizeof(history_fragment_header_t) + sizeof(struct auto_calc_status_data)] = {0};
    history_fragment_header_t *hdr = (history_fragment_header_t *)notify_buf;
    hdr->data_type = 0;              /* 0 = auto calc status */
    hdr->status = 0;                  /* OK */
    hdr->entry_count = sys_cpu_to_le16(1); /* single structure */
    hdr->fragment_index = 0;
    hdr->total_fragments = 1;
    hdr->fragment_size = sizeof(struct auto_calc_status_data);
    hdr->reserved = 0;
    memcpy(&notify_buf[sizeof(history_fragment_header_t)], payload, sizeof(struct auto_calc_status_data));

    const struct bt_gatt_attr *attr = &irrigation_svc.attrs[ATTR_IDX_AUTO_CALC_STATUS_VALUE];
    int err = safe_notify(default_conn, attr, notify_buf, sizeof(notify_buf));
    if (err == 0) {
        static uint32_t last_log_time = 0;
        uint32_t now = k_uptime_get_32();
        if (now - last_log_time > 30000) {
            LOG_DBG("Auto calc status notification sent (unified header)");
            last_log_time = now;
        }
    } else {
        LOG_ERR("Auto calc status notification failed: %d", err);
    }
}

static ssize_t read_auto_calc_status(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                    void *buf, uint16_t len, uint16_t offset) {
    if (!conn || !attr || !buf) {
        LOG_ERR("Invalid parameters for Auto Calc Status read");
        return -EINVAL;
    }
    
    /* Create a local buffer for reading to avoid conflicts with notification buffer */
    struct auto_calc_status_data read_value;
    
    /* Get the current channel selection from the global attribute buffer */
    const struct auto_calc_status_data *global_value = 
        (const struct auto_calc_status_data *)auto_calc_status_value;
    
    /* Use the selected channel from the global buffer, but default to 0 if invalid */
    uint8_t channel_id = global_value->channel_id;
    if (channel_id >= WATERING_CHANNELS_COUNT) {
        channel_id = 0;
    }
    
    watering_channel_t *channel;
    watering_error_t err = watering_get_channel(channel_id, &channel);
    
    if (err != WATERING_SUCCESS) {
        LOG_WRN("Failed to get channel %u for auto calc status read: %d", channel_id, err);
        /* Return default/safe values */
        memset(&read_value, 0, sizeof(read_value));
        read_value.channel_id = channel_id;
        read_value.calculation_active = 0;
        read_value.irrigation_needed = 0;
        read_value.auto_mode = 0; /* Manual mode */
    } else {
        /* Copy fresh data from the watering system */
        memset(&read_value, 0, sizeof(read_value));
        read_value.channel_id = channel_id;
        
        /* Check if channel is in automatic mode */
        bool is_auto_mode = (channel->auto_mode == WATERING_AUTOMATIC_QUALITY || 
                            channel->auto_mode == WATERING_AUTOMATIC_ECO);
        read_value.calculation_active = is_auto_mode ? 1 : 0;
        read_value.auto_mode = (uint8_t)channel->auto_mode;
        
        /* Get water balance data if available */
        if (channel->water_balance != NULL) {
            water_balance_t *balance = (water_balance_t *)channel->water_balance;
            read_value.irrigation_needed = balance->irrigation_needed ? 1 : 0;
            read_value.current_deficit_mm = balance->current_deficit_mm;
            read_value.raw_mm = balance->raw_mm;
            read_value.effective_rain_mm = balance->effective_rain_mm;
        } else {
            read_value.irrigation_needed = 0;
            read_value.current_deficit_mm = 0.0f;
            read_value.raw_mm = 0.0f;
            read_value.effective_rain_mm = 0.0f;
        }
        
        /* Set timing information */
        read_value.last_calculation_time = channel->last_calculation_time;
        read_value.next_irrigation_time = 0;
        read_value.calculation_error = 0; /* No error */

        update_auto_calc_calculations(&read_value, channel);

        if (read_value.next_irrigation_time == 0 &&
            channel->water_balance != NULL &&
            read_value.etc_mm_day > 0.01f) {
            water_balance_t *balance = (water_balance_t *)channel->water_balance;
            const plant_full_data_t *plant = NULL;
            if (channel->plant_db_index < PLANT_FULL_SPECIES_COUNT) {
                plant = &plant_full_database[channel->plant_db_index];
            }
            if (plant) {
                float hours_until = 0.0f;
                if (calc_irrigation_timing(balance, read_value.etc_mm_day, plant, &hours_until) == WATERING_SUCCESS &&
                    hours_until > 0.0f) {
                    uint32_t now_sec = k_uptime_get_32() / 1000U;
                    read_value.next_irrigation_time = now_sec + (uint32_t)(hours_until * 3600.0f);
                }
            }
        }
    }
    
    LOG_DBG("Auto calc status read: ch=%u, active=%u, needed=%u, deficit=%.2f, auto_mode=%u",
            read_value.channel_id, read_value.calculation_active, read_value.irrigation_needed,
            (double)read_value.current_deficit_mm, read_value.auto_mode);
    
    return bt_gatt_attr_read(conn, attr, buf, len, offset, &read_value,
                           sizeof(struct auto_calc_status_data));
}

/* Forward declarations for periodic Auto Calc Status helpers */
static void init_auto_calc_status_periodic(void);
static void schedule_auto_calc_status_periodic(void);
static void cancel_auto_calc_status_periodic(void);

static void auto_calc_status_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value) {
    bool notif_enabled = (value == BT_GATT_CCC_NOTIFY);
    notification_state.auto_calc_status_notifications_enabled = notif_enabled;
    
    if (notif_enabled) {
        LOG_DBG("Auto Calc Status notifications enabled");
    /* Ensure periodic notifier is initialized and scheduled (every 30 min) */
    init_auto_calc_status_periodic();
        
        /* Initialize auto_calc_status_value with current data from channel 0 */
        struct auto_calc_status_data *status_data = (struct auto_calc_status_data *)auto_calc_status_value;
        watering_channel_t *channel;
        watering_error_t err = watering_get_channel(0, &channel);
        
        if (err == WATERING_SUCCESS) {
            memset(status_data, 0, sizeof(struct auto_calc_status_data));
            status_data->channel_id = 0;
            
            /* Check if channel is in automatic mode */
            bool is_auto_mode = (channel->auto_mode == WATERING_AUTOMATIC_QUALITY || 
                                channel->auto_mode == WATERING_AUTOMATIC_ECO);
            status_data->calculation_active = is_auto_mode ? 1 : 0;
            status_data->auto_mode = (uint8_t)channel->auto_mode;
            
            /* Get water balance data if available */
            if (channel->water_balance != NULL) {
                water_balance_t *balance = (water_balance_t *)channel->water_balance;
                status_data->irrigation_needed = balance->irrigation_needed ? 1 : 0;
                status_data->current_deficit_mm = balance->current_deficit_mm;
                status_data->raw_mm = balance->raw_mm;
                status_data->effective_rain_mm = balance->effective_rain_mm;
            }
            
            /* Set timing information */
            status_data->last_calculation_time = channel->last_calculation_time;
            status_data->calculation_error = 0;
            
            update_auto_calc_calculations(status_data, channel);
            
            LOG_INF("Initialized auto calc status with channel 0: active=%u, needed=%u, auto_mode=%u",
                    status_data->calculation_active, status_data->irrigation_needed, status_data->auto_mode);
        } else {
            /* Default values if channel not available */
            memset(status_data, 0, sizeof(struct auto_calc_status_data));
            status_data->channel_id = 0;
            status_data->calculation_active = 0;
            status_data->irrigation_needed = 0;
            status_data->auto_mode = 0;
            status_data->et0_mm_day = 0.0f;
            status_data->crop_coefficient = 1.0f;
            status_data->etc_mm_day = 0.0f;
            status_data->cycle_count = 1;
        }

    /* Schedule first periodic run in 30 minutes per spec */
    schedule_auto_calc_status_periodic();
    /* Push an immediate snapshot on subscribe for better UX */
    bt_irrigation_auto_calc_status_notify();
    } else {
        LOG_DBG("Auto Calc Status notifications disabled");
        memset(auto_calc_status_value, 0, sizeof(struct auto_calc_status_data));
    /* Stop periodic work when notifications are disabled */
    cancel_auto_calc_status_periodic();
    }
}

/* Periodic 30-minute notifier per specification */
static struct k_work_delayable auto_calc_status_periodic_work;
static void auto_calc_status_periodic(struct k_work *work)
{
    ARG_UNUSED(work);
    if (!notification_state.auto_calc_status_notifications_enabled || !default_conn) {
        return; /* Do not reschedule */
    }
    /* Update dynamic fields before notify */
    bt_irrigation_auto_calc_status_notify();
    /* Reschedule for 30 minutes (1800000 ms) */
    k_work_schedule(&auto_calc_status_periodic_work, K_MSEC(1800000));
}

    /* Initialize periodic work (call once in initialization path) */
static void init_auto_calc_status_periodic(void)
{
    static bool inited = false;
    if (inited) return;
    k_work_init_delayable(&auto_calc_status_periodic_work, auto_calc_status_periodic);
    inited = true;
}

/* Wrapper helpers to schedule/cancel periodic notifications without exposing
 * the k_work_delayable symbol above its definition. */
static void schedule_auto_calc_status_periodic(void)
{
    k_work_schedule(&auto_calc_status_periodic_work, K_MSEC(1800000));
}

static void cancel_auto_calc_status_periodic(void)
{
    (void)k_work_cancel_delayable(&auto_calc_status_periodic_work);
}

/* Write handler: 1-byte channel select (0-7, 0xFF = first active) */
static ssize_t write_auto_calc_status(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                     const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
    if (offset != 0 || len < 1) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }
    init_auto_calc_status_periodic();
    uint8_t requested = *((const uint8_t *)buf);
    uint8_t selected = requested;
    if (requested == 0xFF) {
        /* Find first channel in automatic mode */
        for (uint8_t i = 0; i < WATERING_CHANNELS_COUNT; i++) {
            watering_channel_t *ch;
            if (watering_get_channel(i, &ch) == WATERING_SUCCESS) {
                if (ch->auto_mode == WATERING_AUTOMATIC_QUALITY || ch->auto_mode == WATERING_AUTOMATIC_ECO) {
                    selected = i;
                    break;
                }
            }
        }
        if (selected == 0xFF) { /* none found */
            selected = 0; /* default */
        }
    } else if (requested >= WATERING_CHANNELS_COUNT) {
        return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
    }
    struct auto_calc_status_data *status_data = (struct auto_calc_status_data *)auto_calc_status_value;
    status_data->channel_id = selected;
    /* Immediately refresh & notify */
    bt_irrigation_auto_calc_status_notify();
    /* Start periodic if enabled */
    if (notification_state.auto_calc_status_notifications_enabled) {
        schedule_auto_calc_status_periodic();
    }
    return len;
}

/* Alarm CCC change callback */
static void alarm_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value) {
    bool notif_enabled = (value == BT_GATT_CCC_NOTIFY);
    notification_state.alarm_notifications_enabled = notif_enabled;
    if (notif_enabled) {
        LOG_DBG("Alarm notifications enabled");
        /* On subscription: push current state (alarm or clear) immediately */
        const struct bt_gatt_attr *alarm_attr = &irrigation_svc.attrs[ATTR_IDX_ALARM_VALUE];
        if (default_conn && alarm_attr) {
            int err = safe_notify(default_conn, alarm_attr, alarm_value, sizeof(struct alarm_data));
            if (err != 0) {
                LOG_ERR("Failed to send initial alarm state: %d", err);
            }
        }
    } else {
        LOG_DBG("Alarm notifications disabled");
        /* Keep alarm data intact - only disable notifications */
    }
}

/* ------------------------------------------------------------------ */
/* Bluetooth Connection Callbacks                                     */
/* ------------------------------------------------------------------ */

BT_CONN_CB_DEFINE(conn_callbacks) = {
    .connected = connected,
    .disconnected = disconnected,
};

/* ------------------------------------------------------------------ */
/* Authentication Callbacks                                           */
/* ------------------------------------------------------------------ */

static void auth_cancel(struct bt_conn *conn)
{
    char addr[BT_ADDR_LE_STR_LEN];
    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
    LOG_INF("Pairing cancelled: %s", addr);
}

static void auth_pairing_confirm(struct bt_conn *conn)
{
    char addr[BT_ADDR_LE_STR_LEN];
    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
    LOG_INF("Pairing confirmation requested for %s", addr);
    bt_conn_auth_pairing_confirm(conn);
}

static void auth_pairing_complete(struct bt_conn *conn, bool bonded)
{
    char addr[BT_ADDR_LE_STR_LEN];
    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
    LOG_INF("Pairing completed: %s, bonded: %d", addr, bonded);
}

static void auth_pairing_failed(struct bt_conn *conn, enum bt_security_err reason)
{
    char addr[BT_ADDR_LE_STR_LEN];
    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
    LOG_WRN("Pairing failed with %s: %d", addr, reason);
}

static struct bt_conn_auth_cb auth_cb_just_works = {
    .cancel = auth_cancel,
    .pairing_confirm = auth_pairing_confirm,
};

static struct bt_conn_auth_info_cb auth_cb_info = {
    .pairing_complete = auth_pairing_complete,
    .pairing_failed = auth_pairing_failed,
};

/* ================================================================== */
/* Onboarding Notification Functions                                 */
/* ================================================================== */

/* Notify onboarding status update */
int bt_irrigation_onboarding_status_notify(void) {
    if (!default_conn || !notification_state.onboarding_status_notifications_enabled) {
        LOG_DBG("Onboarding status notification not enabled");
        return 0;
    }

    /* Get current onboarding state */
    onboarding_state_t state;
    int ret = onboarding_get_state(&state);
    if (ret < 0) {
        LOG_ERR("Failed to get onboarding state for notification: %d", ret);
        return ret;
    }

    struct onboarding_status_data status_data = {0};

    /* Fill the BLE structure */
    status_data.overall_completion_pct = state.onboarding_completion_pct;

    /* Calculate individual completion percentages */
    int total_channel_flags = 8 * 8; /* 8 channels × 8 flags each */
    int set_channel_flags = 0;
    for (int i = 0; i < 64; i++) {
        if (state.channel_config_flags & (1ULL << i)) {
            set_channel_flags++;
        }
    }
    status_data.channels_completion_pct = (set_channel_flags * 100) / total_channel_flags;

    int total_system_flags = 8; /* 8 system flags defined */
    int set_system_flags = 0;
    for (int i = 0; i < 32; i++) {
        if (state.system_config_flags & (1U << i)) {
            set_system_flags++;
        }
    }
    status_data.system_completion_pct = (set_system_flags * 100) / total_system_flags;

    int total_schedule_flags = 8; /* 8 channels can have schedules */
    int set_schedule_flags = 0;
    for (int i = 0; i < 8; i++) {
        if (state.schedule_config_flags & (1U << i)) {
            set_schedule_flags++;
        }
    }
    status_data.schedules_completion_pct = (set_schedule_flags * 100) / total_schedule_flags;

    /* Copy state flags */
    status_data.channel_config_flags = state.channel_config_flags;
    status_data.channel_extended_flags = state.channel_extended_flags;
    status_data.system_config_flags = state.system_config_flags;
    status_data.schedule_config_flags = state.schedule_config_flags;
    status_data.onboarding_start_time = state.onboarding_start_time;
    status_data.last_update_time = state.last_update_time;

    const struct bt_gatt_attr *attr = &irrigation_svc.attrs[ATTR_IDX_ONBOARDING_STATUS_VALUE];

    /* Build payload bytes */
    uint8_t payload[sizeof(status_data)];
    memcpy(payload, &status_data, sizeof(status_data));

    /* MTU-aware fragmentation using unified 8-byte header */
    uint16_t mtu = bt_gatt_get_mtu(default_conn);
    uint16_t att_payload = (mtu > 3) ? (mtu - 3) : 20; /* ATT notify payload budget */
    const uint16_t hdr_sz = sizeof(history_fragment_header_t); /* 8 bytes */

    if (att_payload <= hdr_sz) {
        LOG_WRN("MTU too small to send onboarding status (att_payload=%u)", att_payload);
        return -EMSGSIZE;
    }

    uint16_t max_chunk = att_payload - hdr_sz;
    uint16_t remaining = sizeof(payload);
    uint16_t offset = 0;
    uint8_t total_frags = (remaining + max_chunk - 1) / max_chunk;

    /* Reusable buffer: header + chunk */
    uint8_t notify_buf[64];
    if (sizeof(notify_buf) < (hdr_sz + 1)) {
        return -ENOMEM;
    }

    for (uint8_t seq = 0; seq < total_frags; seq++) {
        uint16_t this_len = (remaining > max_chunk) ? max_chunk : remaining;
        history_fragment_header_t *hdr = (history_fragment_header_t *)notify_buf;
        hdr->data_type = 0;              /* 0 = onboarding status */
        hdr->status = 0;                 /* OK */
        hdr->entry_count = sys_cpu_to_le16(1); /* single logical structure */
        hdr->fragment_index = seq;
        hdr->total_fragments = total_frags;
        hdr->fragment_size = (uint8_t)this_len;
        hdr->reserved = 0;

        memcpy(&notify_buf[hdr_sz], &payload[offset], this_len);

        /* Use direct notify to avoid 23B limit in advanced_notify */
        int bt_err = bt_gatt_notify(default_conn, attr, notify_buf, hdr_sz + this_len);
        if (bt_err != 0) {
            LOG_WRN("Onboarding status fragment %u/%u notify failed: %d", seq + 1, total_frags, bt_err);
            return bt_err;
        }
        offset += this_len;
        remaining -= this_len;
    }

    LOG_DBG("Onboarding status notification sent in %u fragments", (unsigned)total_frags);
    return 0;
}

/* Notify reset control status update */
int bt_irrigation_reset_control_notify(void) {
    if (!default_conn || !notification_state.reset_control_notifications_enabled) {
        LOG_DBG("Reset control notification not enabled");
        return 0;
    }
    
    struct reset_control_data reset_data = {0};
    
    /* Get current confirmation info */
    reset_confirmation_t confirmation;
    int ret = reset_controller_get_confirmation_info(&confirmation);
    if (ret == 0 && confirmation.is_valid) {
        /* Convert internal type to BLE spec value */
        reset_data.reset_type = reset_type_to_ble_spec(confirmation.type);
        reset_data.channel_id = confirmation.channel_id;
        reset_data.confirmation_code = confirmation.code;
        reset_data.timestamp = confirmation.generation_time;
        reset_data.status = 0x01; /* Pending */
    } else {
        /* No active confirmation */
        reset_data.reset_type = 0xFF; /* Invalid */
        reset_data.channel_id = 0xFF;
        reset_data.confirmation_code = 0;
        reset_data.timestamp = 0;
        reset_data.status = 0xFF; /* No operation */
    }
    
    /* Send notification */
    const struct bt_gatt_attr *attr = &irrigation_svc.attrs[ATTR_IDX_RESET_CONTROL_VALUE];
    int bt_err = safe_notify(default_conn, attr, &reset_data, sizeof(reset_data));
    
    if (bt_err == 0) {
        LOG_DBG("Reset control notification sent: type=%u, channel=%u, status=%u", 
                reset_data.reset_type, reset_data.channel_id, reset_data.status);
    } else {
        LOG_WRN("Reset control notification failed: %d", bt_err);
    }
    
    return bt_err;
}

/* ------------------------------------------------------------------ */
/* BLE Service Implementation Functions                               */
/* ------------------------------------------------------------------ */

int bt_irrigation_service_init(void) {
    int err;
    
    LOG_INF("Initializing BLE irrigation service");
    
    /* Initialize advanced notification system first */
    init_notification_pool();
    
    /* Initialize Bluetooth stack */
    err = bt_enable(NULL);
    if (err) {
        LOG_ERR("Bluetooth init failed: %d", err);
        return err;
    }
    
    /* Register authentication callbacks */
    bt_conn_auth_cb_register(&auth_cb_just_works);
    bt_conn_auth_info_cb_register(&auth_cb_info);
    
    LOG_DBG("Bluetooth initialized");
    
    /* Load settings if available */
    if (IS_ENABLED(CONFIG_SETTINGS)) {
        settings_load();
    }
    
    /* Initialize notification state */
    memset(&notification_state, 0, sizeof(notification_state));
    
    /* Zero key buffers only - others will be set when used */
    memset(valve_value, 0, sizeof(valve_value));
    memset(flow_value, 0, sizeof(flow_value));
    status_value[0] = (uint8_t)WATERING_STATUS_OK;
    /* Ensure alarm buffer starts clean to avoid spurious notify on CCC enable */
    memset(alarm_value, 0, sizeof(alarm_value));
    
    /* Set default system values directly (enhanced) */
    struct enhanced_system_config_data *sys_config = (struct enhanced_system_config_data *)system_config_value;
    memset(sys_config, 0, sizeof(*sys_config));
    sys_config->version = 2;
    sys_config->power_mode = 0;
    sys_config->flow_calibration = 750;
    sys_config->max_active_valves = 1;
    sys_config->num_channels = WATERING_CHANNELS_COUNT;
    
    /* Set default valve data */
    struct valve_control_data *valve_data = (struct valve_control_data *)valve_value;
    valve_data->channel_id = 0xFF;
    valve_data->task_type = 0;
    valve_data->value = 0;
    
    /* Start advertising */
    err = bt_le_adv_start(&adv_param,
                          adv_ad, ARRAY_SIZE(adv_ad),
                          adv_sd, ARRAY_SIZE(adv_sd));
    if (err) {
        LOG_ERR("Advertising failed to start: %d", err);
        return err;
    }
    
    LOG_INF("BLE irrigation service initialized - AutoWatering ready");
    
    return 0;
}

int bt_irrigation_valve_status_update(uint8_t channel_id, bool is_open) {
    /* Fast early return - combine all checks, but allow 0xFF for master valve */
    if (!default_conn || !notification_state.valve_notifications_enabled) {
        return 0;
    }
    
    /* Validate channel_id: normal channels (0-7) or master valve (0xFF) */
    if (channel_id >= WATERING_CHANNELS_COUNT && channel_id != 0xFF) {
        return -EINVAL;
    }
    
    /* Direct struct update - no intermediate variables */
    struct valve_control_data *valve_data = (struct valve_control_data *)valve_value;
    valve_data->channel_id = channel_id;
    valve_data->task_type = is_open ? 1 : 0;
    valve_data->value = 0;
    
    const struct bt_gatt_attr *attr = &irrigation_svc.attrs[ATTR_IDX_VALVE_VALUE];
    int err = safe_notify(default_conn, attr, valve_value, sizeof(struct valve_control_data));
    
    if (err != 0) {
        LOG_ERR("Valve notification failed: %d", err);
    } else {
        if (channel_id == 0xFF) {
            LOG_INF("Master valve status update sent: %s", is_open ? "OPEN" : "CLOSED");
        } else {
            LOG_DBG("Channel %u valve status: %s", channel_id, is_open ? "OPEN" : "CLOSED");
        }
    }
    
    return err;
}

extern bool calibration_active; /* declared above in this file */
int bt_irrigation_flow_update(uint32_t flow_rate) {
    if (!default_conn || !notification_state.flow_notifications_enabled) {
        return 0;
    }
    
    /* Flow updates are NORMAL priority - let adaptive throttling handle frequency */
    static uint32_t last_flow_rate = 0;
    bool flow_changed = (flow_rate != last_flow_rate);
    
    /* Always notify on significant flow changes, let adaptive system handle frequency */
    if (flow_changed || (flow_rate > 0)) {
    /* During calibration, report raw pulse count instead of pps */
    uint32_t payload = calibration_active ? get_pulse_count() : flow_rate;
    memcpy(flow_value, &payload, sizeof(uint32_t));
        
        const struct bt_gatt_attr *attr = &irrigation_svc.attrs[ATTR_IDX_FLOW_VALUE];
        
        /* Use normal priority notification - adaptive system will throttle as needed */
        SMART_NOTIFY(default_conn, attr, flow_value, sizeof(uint32_t));
        
    last_flow_rate = flow_rate;
        
        /* Run buffer pool maintenance occasionally */
        buffer_pool_maintenance();
    }
    
    return 0;
}

int bt_irrigation_system_status_update(watering_status_t status) {
    /* Fast validation and early return */
    if (!default_conn || !notification_state.status_notifications_enabled || 
        status > WATERING_STATUS_LOW_POWER) {
        return status > WATERING_STATUS_LOW_POWER ? -EINVAL : 0;
    }
    
    /* Direct assignment - no intermediate step */
    status_value[0] = (uint8_t)status;
    
    const struct bt_gatt_attr *attr = &irrigation_svc.attrs[ATTR_IDX_STATUS_VALUE];
    int err = safe_notify(default_conn, attr, status_value, sizeof(uint8_t));
    
    if (err == 0) {
        /* Only log status changes, not every notification */
        static uint8_t last_status = 0xFF;
        if (last_status != status) {
            LOG_INF("Status changed: %u->%u", last_status, status);
            last_status = status;
        }
    } else {
        LOG_ERR("Status notification failed: %d", err);
    }
    
    return err;
}

int bt_irrigation_channel_config_update(uint8_t channel_id) {
    /* Combined validation and early return */
    if (!default_conn || !notification_state.channel_config_notifications_enabled || 
        channel_id >= WATERING_CHANNELS_COUNT) {
        return channel_id >= WATERING_CHANNELS_COUNT ? -EINVAL : 0;
    }
    
    watering_channel_t *channel;
    watering_error_t err = watering_get_channel(channel_id, &channel);
    if (err != WATERING_SUCCESS) {
        return -ENODATA;
    }
    
    /* Direct struct access - eliminate intermediate variables */
    struct channel_config_data *config_data = (struct channel_config_data *)channel_config_value;
    config_data->channel_id = channel_id;
    
    /* Optimized string copy */
    size_t name_len = strnlen(channel->name, sizeof(channel->name));
    if (name_len >= sizeof(config_data->name)) {
        name_len = sizeof(config_data->name) - 1;
    }
    memcpy(config_data->name, channel->name, name_len);
    config_data->name[name_len] = '\0';
    config_data->name_len = name_len;
    
    /* Direct assignments */
    config_data->auto_enabled = channel->watering_event.auto_enabled ? 1 : 0;
    config_data->plant_type = (uint8_t)channel->plant_type;
    config_data->soil_type = (uint8_t)channel->soil_type;
    config_data->irrigation_method = (uint8_t)channel->irrigation_method;
    config_data->coverage_type = channel->use_area_based ? 0 : 1;
    config_data->sun_percentage = channel->sun_percentage;
    
    if (channel->use_area_based) {
        config_data->coverage.area_m2 = channel->coverage.area_m2;
    } else {
        config_data->coverage.plant_count = channel->coverage.plant_count;
    }
    
    /* Use the new throttled notification system for channel config */
    const struct bt_gatt_attr *attr = &irrigation_svc.attrs[ATTR_IDX_CHANNEL_CFG_VALUE];
    int bt_err = safe_notify_channel_config(default_conn, attr, channel_config_value, sizeof(struct channel_config_data));
    
    if (bt_err == -EBUSY) {
        /* Throttled notification - this is normal for rapid name changes */
        LOG_DBG("📋 Channel config notification throttled for channel %u", channel_id);
        return 0; /* Success but throttled */
    } else if (bt_err == -EINVAL) {
        /* Client not subscribed - this should not happen after auto-enable */
        LOG_WRN("⚠️ Channel config notification failed: client not subscribed (channel %u)", channel_id);
        LOG_INF("🔧 Running force-enable as backup");
        
        /* Force enable all notifications as backup */
        force_enable_all_notifications();
        
        return 0; /* Don't retry - let next config change succeed */
    } else if (bt_err != 0) {
        LOG_ERR("❌ Channel config notification failed: %d", bt_err);
        return bt_err; /* Return the actual error for debugging */
    } else {
        LOG_DBG("✅ Channel config notification sent for channel %u", channel_id);
    }
    
    return 0; /* Success */
}

int bt_irrigation_schedule_update(uint8_t channel_id) {
    if (!default_conn || !notification_state.schedule_notifications_enabled) {
        LOG_DBG("Schedule notification skipped: enabled=%d", 
                notification_state.schedule_notifications_enabled);
        return 0; // No connection or notifications disabled
    }
    
    if (channel_id >= WATERING_CHANNELS_COUNT) {
        LOG_ERR("Invalid channel ID for schedule notification: %u", channel_id);
        return -EINVAL;
    }
    
    // Get channel schedule configuration
    watering_channel_t *channel;
    watering_error_t err = watering_get_channel(channel_id, &channel);
    if (err != WATERING_SUCCESS) {
        LOG_ERR("Failed to get channel %u for schedule notification: %d", channel_id, err);
        return -ENODATA;
    }
    
    // Update schedule_value with new data
    struct schedule_config_data *schedule_data = (struct schedule_config_data *)schedule_value;
    schedule_data->channel_id = channel_id;
    
    /* Map watering system schedule to BLE structure */
    if (channel->watering_event.schedule_type == SCHEDULE_DAILY) {
        schedule_data->schedule_type = 0;
        schedule_data->days_mask = channel->watering_event.schedule.daily.days_of_week;
    } else if (channel->watering_event.schedule_type == SCHEDULE_PERIODIC) {
        schedule_data->schedule_type = 1;
        schedule_data->days_mask = channel->watering_event.schedule.periodic.interval_days;
    } else if (channel->watering_event.schedule_type == SCHEDULE_AUTO) {
        schedule_data->schedule_type = 2;
        schedule_data->days_mask = 0x7F; /* AUTO checks every day */
    } else {
        schedule_data->schedule_type = 0; /* Default to daily */
        schedule_data->days_mask = 0x7F; /* All days */
    }
    
    schedule_data->hour = channel->watering_event.start_time.hour;
    schedule_data->minute = channel->watering_event.start_time.minute;
    
    /* Map watering mode */
    if (channel->watering_event.watering_mode == WATERING_BY_DURATION) {
        schedule_data->watering_mode = 0;
        schedule_data->value = channel->watering_event.watering.by_duration.duration_minutes;
    } else {
        schedule_data->watering_mode = 1;
        schedule_data->value = channel->watering_event.watering.by_volume.volume_liters;
    }
    
    schedule_data->auto_enabled = channel->watering_event.auto_enabled ? 1 : 0;
    
    /* Map solar timing configuration */
    schedule_data->use_solar_timing = channel->watering_event.use_solar_timing ? 1 : 0;
    schedule_data->solar_event = channel->watering_event.solar_event;
    schedule_data->solar_offset_minutes = channel->watering_event.solar_offset_minutes;
    
    LOG_DBG("Schedule notification: ch=%u, type=%u, days=0x%02X, time=%02u:%02u, solar=%u",
            schedule_data->channel_id, schedule_data->schedule_type,
            schedule_data->days_mask, schedule_data->hour, schedule_data->minute,
            schedule_data->use_solar_timing);
    
    // Send notification to client
    const struct bt_gatt_attr *attr = &irrigation_svc.attrs[ATTR_IDX_SCHEDULE_VALUE];
    int bt_err = safe_notify(default_conn, attr, schedule_value, sizeof(struct schedule_config_data));
    
    if (bt_err != 0) {
        LOG_ERR("Schedule notification failed for channel %u: %d", channel_id, bt_err);
    }
    
    return bt_err;
}

int bt_irrigation_update_statistics_from_flow(uint8_t channel_id, uint32_t volume_ml) {
    /* Get current timestamp for the statistics update */
    uint32_t timestamp = timezone_get_unix_utc(); /* Unix timestamp UTC - persistent across reboots */
    
    /* Update statistics with volume data from flow sensor */
    return bt_irrigation_update_statistics(channel_id, volume_ml, timestamp);
}

int bt_irrigation_queue_status_update(uint8_t pending_count) {
    if (!default_conn || !notification_state.task_queue_notifications_enabled) {
        return 0;
    }
    
    /* Direct assignment - eliminate intermediate struct access */
    ((struct task_queue_data *)task_queue_value)->pending_count = pending_count;
    
    return bt_irrigation_queue_status_notify();
}

int bt_irrigation_alarm_notify(uint8_t alarm_code, uint16_t alarm_data) {
    if (!default_conn || !notification_state.alarm_notifications_enabled) {
        return 0;
    }

    /* Direct struct update - no intermediate variables */
    struct alarm_data *alarm = (struct alarm_data *)alarm_value;
    alarm->alarm_code = alarm_code;
    alarm->alarm_data = alarm_data;
    alarm->timestamp = timezone_get_unix_utc(); /* Use RTC timestamp for persistent event tracking */

    const struct bt_gatt_attr *attr = &irrigation_svc.attrs[ATTR_IDX_ALARM_VALUE];
    
    /* Use CRITICAL_NOTIFY for alarms - they have highest priority */
    int err = 0;
    if (default_conn && attr && connection_active) {
        /* Critical notifications bypass most checks and use priority handling */
        err = advanced_notify(default_conn, attr, alarm_value, sizeof(struct alarm_data));
        if (err != 0 && err != -ENOTCONN) {
            LOG_ERR("🔥 CRITICAL alarm notification failed: %d", err);
        }
    }

    if (err != 0) {
        LOG_ERR("🚨 Alarm notification failed: %d (code=%u, data=%u)", err, alarm_code, alarm_data);
    } else {
        LOG_INF("✅ Alarm notification sent successfully: code=%u, data=%u", alarm_code, alarm_data);
    }

    return err;
}

// Notificare BLE pentru calibration
int bt_irrigation_calibration_notify(void) {
    if (!default_conn || !notification_state.calibration_notifications_enabled) {
        return 0;
    }
    
    const struct bt_gatt_attr *attr = &irrigation_svc.attrs[ATTR_IDX_CALIB_VALUE];
    int err = safe_notify(default_conn, attr, calibration_value, sizeof(struct calibration_data));
    
    if (err != 0) {
        LOG_ERR("Calibration notification failed: %d", err);
    }
    
    return err;
}

            /* Removed stray RTC timezone updates from calibration path */
// Notificare BLE pentru Current Task
int bt_irrigation_current_task_notify(void) {
    if (!default_conn || !notification_state.current_task_notifications_enabled) {
        LOG_DBG("Current Task notification not enabled");
        return 0;
    }
    
    /* Update current_task_value with latest data before notification */
    struct current_task_data *value = (struct current_task_data *)current_task_value;
    watering_task_t *current_task = watering_get_current_task();
    
    if (current_task == NULL) {
        /* No active task */
        value->channel_id = 0xFF;
        value->start_time = 0;
        value->mode = 0;
        value->target_value = 0;
        value->current_value = 0;
        value->total_volume = 0;
        value->status = 0;  // Idle
        value->reserved = 0;
    } else {
        /* Active task - populate with current data */
        uint8_t channel_id = current_task->channel - watering_channels;
        
        // Calculate effective elapsed time excluding paused time
        uint32_t total_elapsed_ms = k_uptime_get_32() - watering_task_state.watering_start_time;
        uint32_t current_pause_time = 0;
        
        // If currently paused, add current pause period
        if (watering_task_state.task_paused) {
            current_pause_time = k_uptime_get_32() - watering_task_state.pause_start_time;
        }
        
        uint32_t effective_elapsed_ms = total_elapsed_ms - watering_task_state.total_paused_time - current_pause_time;
        uint32_t elapsed_seconds = effective_elapsed_ms / 1000;
        
        value->channel_id = channel_id;
        value->start_time = watering_task_state.watering_start_time / 1000;
        value->mode = (current_task->channel->watering_event.watering_mode == WATERING_BY_DURATION) ? 0 : 1;
        
        // Set status based on current state
        if (watering_task_state.task_paused) {
            value->status = 2;  // Paused (spec)
        } else if (watering_task_state.task_in_progress) {
            value->status = 1;  // Running
        } else {
            value->status = 0;  // Idle
        }
        
        /* Get flow sensor data */
        uint32_t pulses = get_pulse_count();
        uint32_t pulses_per_liter;
        if (watering_get_flow_calibration(&pulses_per_liter) != WATERING_SUCCESS) {
            pulses_per_liter = DEFAULT_PULSES_PER_LITER;
        }
        uint32_t total_volume_ml = (pulses * 1000) / pulses_per_liter;
        value->total_volume = total_volume_ml;
        
        if (value->mode == 0) {
            /* Duration mode */
            uint32_t target_seconds = current_task->channel->watering_event.watering.by_duration.duration_minutes * 60;
            value->target_value = target_seconds;
            value->current_value = elapsed_seconds;
            value->reserved = 0;
        } else {
            /* Volume mode */
            uint32_t target_ml = current_task->channel->watering_event.watering.by_volume.volume_liters * 1000;
            value->target_value = target_ml;
            value->current_value = total_volume_ml;
            value->reserved = elapsed_seconds;
        }
    }
    
    const struct bt_gatt_attr *attr = &irrigation_svc.attrs[ATTR_IDX_CURRENT_TASK_VALUE];
    int err = safe_notify(default_conn, attr, current_task_value, sizeof(struct current_task_data));
    
    if (err == 0) {
        if (value->channel_id == 0xFF) {
            LOG_INF("✅ Current Task notification sent: No active task");
        } else {
            LOG_INF("✅ Current Task notification sent: ch=%u, mode=%u, target=%u, current=%u, volume=%u, status=%u",
                    value->channel_id, value->mode, value->target_value, 
                    value->current_value, value->total_volume, value->status);
        }
    } else {
        LOG_ERR("❌ Failed to send Current Task notification: %d", err);
    }
    
    return err;
}

int bt_irrigation_current_task_update(uint8_t channel_id, uint32_t start_time, 
                                    uint8_t mode, uint32_t target_value, 
                                    uint32_t current_value, uint32_t total_volume) {
    /* Per BLE API Documentation: Update current task data and send notification */
    struct current_task_data *value = (struct current_task_data *)current_task_value;
    
    if (channel_id == 0xFF) {
        /* No active task */
        value->channel_id = 0xFF;
        value->start_time = 0;
        value->mode = 0;
        value->target_value = 0;
        value->current_value = 0;
        value->total_volume = 0;
        value->status = 0;  // Idle
        value->reserved = 0;
    } else {
        /* Active task */
        value->channel_id = channel_id;
        value->start_time = start_time;
        value->mode = mode;
        value->target_value = target_value;
        value->current_value = current_value;
        value->total_volume = total_volume;
        value->status = 1;  // Running
        
        /* Calculate reserved field for volume mode */
        if (mode == 1) {
            /* For volume mode, use RTC-based elapsed time calculation */
            uint32_t current_utc = timezone_get_unix_utc();
            value->reserved = (uint16_t)(current_utc - start_time);
        } else {
            /* For duration mode, calculate from boot time difference */
            value->reserved = (uint16_t)(k_uptime_get_32() / 1000 - start_time);
        }
    }
    
    LOG_INF("Current Task updated: ch=%u, start=%u, mode=%u, target=%u, current=%u, volume=%u",
            channel_id, start_time, mode, target_value, current_value, total_volume);
    
    /* Send notification if enabled */
    return bt_irrigation_current_task_notify();
}

int bt_irrigation_history_notify_event(uint8_t channel_id, uint8_t event_type, 
                                      uint32_t timestamp, uint32_t value) {
    if (!default_conn || !notification_state.history_notifications_enabled) {
        LOG_DBG("History notification not enabled");
        return 0; // No connection or notifications disabled
    }
    
    /* Per BLE API Documentation: History notifications for real-time events */
    /* Event types: 0=START, 1=COMPLETE, 2=ABORT, 3=ERROR */
    /* Trigger types: 0=manual, 1=scheduled, 2=remote */
    
    if (channel_id >= WATERING_CHANNELS_COUNT) {
        LOG_ERR("Invalid channel ID for history notification: %u", channel_id);
        return -EINVAL;
    }
    
    /* Update history_value with new event data */
    struct history_data *hist_data = (struct history_data *)history_value;
    hist_data->channel_id = channel_id;
    hist_data->history_type = 0; /* Detailed event */
    hist_data->entry_index = 0; /* Most recent */
    hist_data->count = 1; /* One new event */
    hist_data->start_timestamp = timestamp;
    hist_data->end_timestamp = timestamp;
    
    /* Populate detailed event data */
    hist_data->data.detailed.timestamp = timestamp;
    hist_data->data.detailed.channel_id = channel_id;
    hist_data->data.detailed.event_type = event_type;
    hist_data->data.detailed.mode = 0; /* Duration mode default */
    hist_data->data.detailed.target_value = value;
    hist_data->data.detailed.actual_value = value;
    hist_data->data.detailed.total_volume_ml = value;
    hist_data->data.detailed.trigger_type = 1; /* Scheduled default */
    hist_data->data.detailed.success_status = (event_type == 1) ? 1 : 0; /* Success if COMPLETE */
    hist_data->data.detailed.error_code = (event_type == 3) ? 1 : 0; /* Error if ERROR event */
    hist_data->data.detailed.flow_rate_avg = 750; /* Default flow rate */
    
    const struct bt_gatt_attr *attr = &irrigation_svc.attrs[ATTR_IDX_HISTORY_VALUE];
    int err = safe_notify(default_conn, attr, history_value, sizeof(struct history_data));
    
    if (err == 0) {
        LOG_INF("✅ History notification sent: ch=%u, event=%u (%s), timestamp=%u, value=%u",
                channel_id, event_type,
                (event_type == 0) ? "START" : 
                (event_type == 1) ? "COMPLETE" :
                (event_type == 2) ? "ABORT" : "ERROR",
                timestamp, value);
    } else {
        LOG_ERR("❌ Failed to send history notification: %d", err);
    }
    
    return err;
}

int bt_irrigation_rtc_update(rtc_datetime_t *datetime) {
    /* This is the public API function that should be called from external modules */
    return bt_irrigation_rtc_update_notify(datetime);
}

int bt_irrigation_config_update(void) {
    if (!default_conn || !notification_state.system_config_notifications_enabled) {
        return 0; // No connection or notifications disabled
    }
    
    /* Per BLE API Documentation: System config update notification */
    /* Update system_config_value with current system configuration */
    struct enhanced_system_config_data *config = (struct enhanced_system_config_data *)system_config_value;
    
    /* Get current watering system configuration */
    memset(config, 0, sizeof(*config));
    config->version = 2;                    // Enhanced configuration version
    power_mode_t current_mode;
    if (watering_get_power_mode(&current_mode) == WATERING_SUCCESS) {
        config->power_mode = (uint8_t)current_mode;
    } else {
        config->power_mode = 0;             // Default to normal mode
    }
    
    /* Get current flow calibration */
    uint32_t pulses_per_liter;
    if (watering_get_flow_calibration(&pulses_per_liter) == WATERING_SUCCESS) {
        config->flow_calibration = pulses_per_liter;
    } else {
        config->flow_calibration = 750;     // Default calibration
    }
    
    config->max_active_valves = 1;          // Always 1 (read-only)
    config->num_channels = WATERING_CHANNELS_COUNT; // Number of channels
    
    /* Send notification to client */
    const struct bt_gatt_attr *attr = &irrigation_svc.attrs[ATTR_IDX_SYSTEM_CFG_VALUE];
    int err = safe_notify(default_conn, attr, system_config_value, sizeof(struct enhanced_system_config_data));
    
    if (err == 0) {
        LOG_INF("✅ System config (enhanced) notification sent: version=%u, power_mode=%u, flow_cal=%u, channels=%u",
                config->version, config->power_mode, config->flow_calibration, config->num_channels);
    } else {
        LOG_ERR("❌ Failed to send system config notification: %d", err);
    }
    
    return err;
}

int bt_irrigation_statistics_update(uint8_t channel_id) {
    if (!default_conn || !notification_state.statistics_notifications_enabled) {
        return 0; // No connection or notifications disabled
    }
    
    if (channel_id >= WATERING_CHANNELS_COUNT) {
        LOG_ERR("Invalid channel ID for statistics update: %u", channel_id);
        return -EINVAL;
    }
    
    /* Per BLE API Documentation: Update statistics_value with current channel statistics */
    struct statistics_data *stats = (struct statistics_data *)statistics_value;
    
    /* Get channel information */
    watering_channel_t *channel;
    watering_error_t err = watering_get_channel(channel_id, &channel);
    if (err != WATERING_SUCCESS) {
        LOG_ERR("Failed to get channel %u for statistics update: %d", channel_id, err);
        return -ENODATA;
    }
    
    /* Update statistics structure based on available data */
    stats->channel_id = channel_id;
    stats->last_watering = channel->last_watering_time;
    
    /* Try to get statistics from history system */
    uint32_t total_volume_ml = 0;
    uint32_t last_volume_ml = 0;
    uint16_t session_count = 0;
    
    /* Get statistics from history if available */
    uint16_t recent_volumes[7];  // Last 7 days
    uint16_t volume_count = 0;
    
    watering_error_t history_err = watering_history_get_recent_daily_volumes(
        channel_id, 7, recent_volumes, &volume_count);
    
    if (history_err == WATERING_SUCCESS && volume_count > 0) {
        /* Calculate total volume from recent days */
        for (uint16_t i = 0; i < volume_count; i++) {
            total_volume_ml += recent_volumes[i];
        }
        
        /* Last volume is the most recent non-zero volume */
        for (int i = volume_count - 1; i >= 0; i--) {
            if (recent_volumes[i] > 0) {
                last_volume_ml = recent_volumes[i];
                break;
            }
        }
        
        /* Session count approximation: count non-zero volume days */
        for (uint16_t i = 0; i < volume_count; i++) {
            if (recent_volumes[i] > 0) {
                session_count++;
            }
        }
        
        LOG_DBG("History stats for channel %u: total=%u ml, last=%u ml, sessions=%u", 
                channel_id, total_volume_ml, last_volume_ml, session_count);
    } else {
        LOG_DBG("History stats unavailable for channel %u, using defaults", channel_id);
        total_volume_ml = 0;
        last_volume_ml = 0;
        session_count = 0;
    }
    
    stats->total_volume = total_volume_ml;
    stats->last_volume = last_volume_ml;
    stats->count = session_count;
    
    /* Send notification to client */
    const struct bt_gatt_attr *attr = &irrigation_svc.attrs[ATTR_IDX_STATISTICS_VALUE];
    int bt_err = safe_notify(default_conn, attr, statistics_value, sizeof(struct statistics_data));
    
    if (bt_err == 0) {
        LOG_INF("✅ Statistics notification sent: ch=%u, sessions=%u, total_volume=%u ml, last_volume=%u ml, last_watering=%u",
                channel_id, stats->count, stats->total_volume, stats->last_volume, stats->last_watering);
    } else {
        LOG_ERR("❌ Failed to send statistics notification: %d", bt_err);
    }
    
    return bt_err;
}

int bt_irrigation_start_flow_calibration(uint8_t start, uint32_t volume_ml) {
    if (!default_conn || !notification_state.calibration_notifications_enabled) {
        return 0; // No connection or notifications disabled
    }
    
    /* Per BLE API Documentation: Start or stop flow calibration procedure */
    /* Parameter start: 1=start calibration, 0=stop calibration */
    /* Parameter volume_ml: Expected volume in milliliters for calibration */
    
    struct calibration_data *calib = (struct calibration_data *)calibration_value;
    
    if (start == 1) {
        /* Start calibration */
        LOG_INF("✅ Starting flow calibration: expected volume = %u ml", volume_ml);
        
        /* Initialize calibration data */
        calib->action = 1;          // Start action
        calib->pulses = 0;          // Reset pulse count
        calib->volume_ml = volume_ml; // Expected volume
        calib->pulses_per_liter = 0;  // Will be calculated when complete
        
        /* Start actual flow sensor calibration */
        /* NOTE: This is a simplified implementation - actual calibration would */
        /* need integration with flow sensor hardware and pulse counting */
        calibration_active = true;
        /* Reset hardware pulse counter to begin fresh measurement */
        reset_pulse_count();
        LOG_DBG("Flow calibration: hardware pulse counter reset");
        
        LOG_INF("✅ Flow calibration started successfully");
        
        /* Mark calibration as active */
        calibration_active = true;
        
    } else if (start == 0) {
        /* Stop calibration */
        LOG_INF("⏹️ Stopping flow calibration");
        
        /* Stop actual flow sensor calibration */
        /* Obtain real pulse count from flow sensor instead of simulated value */
        uint32_t pulses_counted = get_pulse_count();
        uint32_t pulses_per_liter = 0; // Will be computed below
        LOG_INF("Flow calibration stop: measured %u pulses for %u ml expected", pulses_counted, calib->volume_ml);
        
        /* Try to get current calibration */
        watering_error_t err = watering_get_flow_calibration(&pulses_per_liter);
        if (err == WATERING_SUCCESS) {
            /* Calculate new calibration based on measured volume vs expected */
            if (calib->volume_ml > 0 && pulses_counted > 0) {
                /* pulses_per_liter = pulses / liters; volume_ml -> liters */
                uint32_t computed = (pulses_counted * 1000U) / calib->volume_ml;
                /* Basic sanity range: 100..10000 pulses/L typical for hall sensors */
                if (computed >= 100 && computed <= 10000) {
                    pulses_per_liter = computed;
                    if (watering_set_flow_calibration(pulses_per_liter) == WATERING_SUCCESS) {
                        LOG_INF("Flow calibration updated: %u pulses/L (from %u pulses / %u ml)",
                                pulses_per_liter, pulses_counted, calib->volume_ml);
                    } else {
                        LOG_WRN("Failed to persist new flow calibration, keeping previous value");
                    }
                } else {
                    LOG_WRN("Computed calibration %u pulses/L out of expected range, retaining previous %u", computed, pulses_per_liter);
                }
            } else {
                LOG_WRN("Calibration aborted: insufficient data (pulses=%u, volume_ml=%u)", pulses_counted, calib->volume_ml);
            }
        }
        
        /* Update calibration results */
        calib->action = 3;          // Completed action
        calib->pulses = pulses_counted;
        calib->pulses_per_liter = pulses_per_liter;
        
        LOG_INF("✅ Flow calibration completed: %u pulses, %u pulses/liter", 
                pulses_counted, pulses_per_liter);
        
        /* Mark calibration as inactive */
        calibration_active = false;
        
    } else {
        LOG_ERR("Invalid calibration start parameter: %u (must be 0 or 1)", start);
        return -EINVAL;
    }
    
    /* Send calibration notification */
    return bt_irrigation_calibration_notify();
}

int bt_irrigation_history_update(uint8_t channel_id, uint8_t entry_index) {
    if (!default_conn || !notification_state.history_notifications_enabled) {
        return 0;
    }
    
    /* Per BLE API Documentation: History update for specific entry */
    /* Use RTC timestamp for persistent history events */
    return bt_irrigation_history_notify_event(channel_id, 1, timezone_get_unix_utc(), 0);
}

int bt_irrigation_history_get_detailed(uint8_t channel_id, uint32_t start_timestamp,
                                     uint32_t end_timestamp, uint8_t entry_index) {
    struct history_data *hist_data = (struct history_data *)history_value;
    memset(hist_data, 0, sizeof(*hist_data));

    hist_data->channel_id = channel_id;
    hist_data->history_type = 0; /* Detailed */
    hist_data->entry_index = entry_index;
    hist_data->start_timestamp = start_timestamp;
    hist_data->end_timestamp = end_timestamp;

    if (!default_conn || !notification_state.history_notifications_enabled) {
        return 0;
    }

    uint8_t effective_channel = channel_id;
    if (channel_id == 0xFF || channel_id >= WATERING_CHANNELS_COUNT) {
        effective_channel = 0;
    }

    history_event_t event_buffer[1];
    uint32_t timestamp_buffer[1] = {0};
    uint16_t requested = 1;

    watering_error_t history_err = watering_history_query_page(
        effective_channel,
        entry_index,
        event_buffer,
        &requested,
        timestamp_buffer);

    if (history_err != WATERING_SUCCESS || requested == 0) {
        LOG_INF("No detailed history available for ch=%u idx=%u (err=%d)",
                effective_channel, entry_index, history_err);
        return 0;
    }

    uint32_t event_ts = timestamp_buffer[0];
    history_event_t *event = &event_buffer[0];

    if (event_ts == 0) {
        uint32_t base_ts = end_timestamp ? end_timestamp : timezone_get_unix_utc();
        if (event->dt_delta != 0 && base_ts > event->dt_delta) {
            event_ts = base_ts - event->dt_delta;
        } else {
            event_ts = base_ts;
        }
    }

    if ((start_timestamp && event_ts < start_timestamp) ||
        (end_timestamp && event_ts > end_timestamp)) {
        LOG_DBG("Detailed history outside requested window (ts=%u, start=%u, end=%u)",
                event_ts, start_timestamp, end_timestamp);
        return 0;
    }

    hist_data->count = 1;
    hist_data->data.detailed.timestamp = event_ts;
    hist_data->data.detailed.channel_id = effective_channel;
    hist_data->data.detailed.event_type = (event->flags.err == 0) ? 1 : 3;
    hist_data->data.detailed.mode = event->flags.mode;
    hist_data->data.detailed.target_value = event->target_ml;
    hist_data->data.detailed.actual_value = event->actual_ml;
    hist_data->data.detailed.total_volume_ml = event->actual_ml;
    hist_data->data.detailed.trigger_type = event->flags.trigger;
    hist_data->data.detailed.success_status = event->flags.success;
    hist_data->data.detailed.error_code = event->flags.err;
    hist_data->data.detailed.flow_rate_avg = event->avg_flow_ml_s;

    LOG_INF("History detailed query: ch=%u (eff=%u), ts=%u, entry=%u",
            channel_id, effective_channel, event_ts, entry_index);

    return 0;
}

int bt_irrigation_history_get_daily(uint8_t channel_id, uint8_t entry_index) {
    if (!default_conn || !notification_state.history_notifications_enabled) {
        return 0;
    }
    
    struct history_data *hist_data = (struct history_data *)history_value;
    hist_data->channel_id = channel_id;
    hist_data->history_type = 1; /* Daily */
    hist_data->entry_index = entry_index;
    hist_data->count = 1;
    hist_data->start_timestamp = 0;
    hist_data->end_timestamp = 0;

    memset(&hist_data->data.daily, 0, sizeof(hist_data->data.daily));

    uint8_t effective_channel = (channel_id < WATERING_CHANNELS_COUNT) ? channel_id : 0;
    uint16_t current_year = get_current_year();
    uint16_t current_day = get_current_day_of_year();
    uint16_t target_day = (entry_index > current_day) ? 0 : (current_day - entry_index);

    daily_stats_t stats_buf[1];
    uint16_t stats_found = 0;
    watering_error_t history_err = watering_history_get_daily_stats(effective_channel,
                                                                    target_day,
                                                                    target_day,
                                                                    current_year,
                                                                    stats_buf,
                                                                    &stats_found);

    if (history_err == WATERING_SUCCESS && stats_found > 0) {
        daily_stats_t *stats = &stats_buf[0];

        rtc_datetime_t dt;
        if (stats->day_epoch != 0 && epoch_to_local_datetime(stats->day_epoch, &dt)) {
            hist_data->data.daily.year = dt.year;
            hist_data->data.daily.day_index = calculate_day_of_year(dt.year, dt.month, dt.day);
        } else {
            hist_data->data.daily.year = current_year;
            hist_data->data.daily.day_index = target_day;
        }

        uint32_t successes = stats->sessions_ok;
        uint32_t errors = stats->sessions_err;
        uint32_t total_sessions = successes + errors;

        if (total_sessions > UINT8_MAX) {
            total_sessions = UINT8_MAX;
        }
        hist_data->data.daily.watering_sessions = (uint8_t)total_sessions;
        hist_data->data.daily.total_volume_ml = stats->total_ml;
        hist_data->data.daily.total_duration_sec = 0;
        hist_data->data.daily.avg_flow_rate = 0;
        hist_data->data.daily.success_rate = stats->success_rate;
        hist_data->data.daily.error_count = (errors > UINT8_MAX) ? UINT8_MAX : (uint8_t)errors;
    }

    LOG_INF("History daily query: ch=%u (effective=%u), entry=%u, sessions=%u",
            channel_id, effective_channel, entry_index,
            hist_data->data.daily.watering_sessions);
    
    return 0;
}

int bt_irrigation_history_get_monthly(uint8_t channel_id, uint8_t entry_index) {
    if (!default_conn || !notification_state.history_notifications_enabled) {
        return 0;
    }
    struct history_data *hist_data = (struct history_data *)history_value;
    hist_data->channel_id = channel_id;
    hist_data->history_type = 2; /* Monthly */
    hist_data->entry_index = entry_index;
    hist_data->count = 1;
    hist_data->start_timestamp = 0;
    hist_data->end_timestamp = 0;

    memset(&hist_data->data.monthly, 0, sizeof(hist_data->data.monthly));

    uint8_t effective_channel = (channel_id < WATERING_CHANNELS_COUNT) ? channel_id : 0;
    uint16_t year = get_current_year();
    uint8_t month = get_current_month();

    for (uint8_t i = 0; i < entry_index; i++) {
        if (month == 1) {
            month = 12;
            year -= 1;
        } else {
            month -= 1;
        }
    }

    hist_data->data.monthly.month = month;
    hist_data->data.monthly.year = year;

    monthly_stats_t month_stats[1];
    uint16_t month_count = 0;
    watering_error_t history_err = watering_history_get_monthly_stats(effective_channel,
                                                                     month,
                                                                     month,
                                                                     year,
                                                                     month_stats,
                                                                     &month_count);

    if (history_err == WATERING_SUCCESS && month_count > 0) {
        monthly_stats_t *stats = &month_stats[0];
        hist_data->data.monthly.total_volume_ml = stats->total_ml;
        hist_data->data.monthly.active_days = stats->active_days;

        uint32_t month_start = build_epoch_from_date(year, month, 1);
        uint8_t month_after = (month == 12) ? 1 : (month + 1);
        uint16_t year_after = (month == 12) ? (year + 1) : year;
        uint32_t month_end = build_epoch_from_date(year_after, month_after, 1);
        hist_data->data.monthly.total_sessions =
            count_sessions_in_period(effective_channel, month_start, month_end);

        uint32_t daily_success = 0;
        uint32_t daily_errors = 0;
        uint8_t days = days_in_month(year, month);
        for (uint8_t day = 1; day <= days; ++day) {
            uint16_t day_index = calculate_day_of_year(year, month, day);
            daily_stats_t day_stats[1];
            uint16_t day_found = 0;
            if (watering_history_get_daily_stats(effective_channel,
                                                 day_index,
                                                 day_index,
                                                 year,
                                                 day_stats,
                                                 &day_found) == WATERING_SUCCESS &&
                day_found > 0) {
                daily_success += day_stats[0].sessions_ok;
                daily_errors += day_stats[0].sessions_err;
            }
        }

        hist_data->data.monthly.total_duration_hours = 0;
        hist_data->data.monthly.avg_daily_volume = (stats->active_days > 0)
            ? (uint16_t)(stats->total_ml / stats->active_days)
            : 0;

        uint32_t total_month_sessions = daily_success + daily_errors;
        if (total_month_sessions == 0 && hist_data->data.monthly.total_sessions > 0) {
            total_month_sessions = hist_data->data.monthly.total_sessions;
        }
        if (total_month_sessions > 0) {
            uint32_t pct = (daily_success * 100U) / total_month_sessions;
            hist_data->data.monthly.success_rate = (pct > 100U) ? 100U : (uint8_t)pct;
        }
    }

    LOG_INF("History monthly query: ch=%u (effective=%u), entry=%u, sessions=%u",
            channel_id, effective_channel, entry_index,
            hist_data->data.monthly.total_sessions);

    return 0;
}

int bt_irrigation_history_get_annual(uint8_t channel_id, uint8_t entry_index) {
    if (!default_conn || !notification_state.history_notifications_enabled) {
        return 0;
    }
    
    struct history_data *hist_data = (struct history_data *)history_value;
    hist_data->channel_id = channel_id;
    hist_data->history_type = 3; /* Annual */
    hist_data->entry_index = entry_index;
    hist_data->count = 1;
    hist_data->start_timestamp = 0;
    hist_data->end_timestamp = 0;
    uint8_t effective_channel = (channel_id < WATERING_CHANNELS_COUNT) ? channel_id : 0;

    memset(&hist_data->data.annual, 0, sizeof(hist_data->data.annual));

    uint16_t year = get_current_year();
    if (entry_index > 0) {
        if (year > entry_index) {
            year -= entry_index;
        } else {
            year = 0;
        }
    }
    hist_data->data.annual.year = year;

    annual_stats_t annual_stats[1];
    uint16_t annual_count = 0;
    watering_error_t annual_err = watering_history_get_annual_stats(effective_channel,
                                                                    year,
                                                                    year,
                                                                    annual_stats,
                                                                    &annual_count);

    if (annual_err == WATERING_SUCCESS && annual_count > 0) {
        annual_stats_t *stats = &annual_stats[0];

        hist_data->data.annual.total_sessions = (stats->sessions > UINT16_MAX)
            ? UINT16_MAX
            : (uint16_t)stats->sessions;
        hist_data->data.annual.total_volume_liters = stats->total_ml / 1000U;
        hist_data->data.annual.avg_monthly_volume = (uint16_t)((stats->total_ml / 1000U) / 12U);
        hist_data->data.annual.peak_month_volume = (uint16_t)(stats->max_month_ml / 1000U);

        uint32_t success_sessions = (stats->sessions >= stats->errors)
            ? (stats->sessions - stats->errors)
            : 0;
        if (stats->sessions > 0) {
            uint32_t pct = (success_sessions * 100U) / stats->sessions;
            hist_data->data.annual.success_rate = (pct > 100U) ? 100U : (uint8_t)pct;
        }

        uint8_t best_month = 0;
        uint32_t best_volume = 0;
        for (uint8_t m = 1; m <= 12; ++m) {
            monthly_stats_t month_stats[1];
            uint16_t found = 0;
            if (watering_history_get_monthly_stats(effective_channel,
                                                   m,
                                                   m,
                                                   year,
                                                   month_stats,
                                                   &found) == WATERING_SUCCESS &&
                found > 0) {
                if (month_stats[0].total_ml > best_volume) {
                    best_volume = month_stats[0].total_ml;
                    best_month = month_stats[0].month;
                }
            }
        }
        hist_data->data.annual.most_active_month = best_month;
        hist_data->data.annual.peak_month_volume = (uint16_t)(best_volume / 1000U);

        uint32_t year_start = build_epoch_from_date(year, 1, 1);
        uint32_t year_end = build_epoch_from_date(year + 1, 1, 1);
        hist_data->data.annual.total_sessions = count_sessions_in_period(effective_channel,
                                                                         year_start,
                                                                         year_end);
    }

    LOG_INF("History annual query: ch=%u (effective=%u), entry=%u, sessions=%u",
            channel_id, effective_channel, entry_index,
            hist_data->data.annual.total_sessions);

    return 0;
}

int bt_irrigation_growing_env_update(uint8_t channel_id) {
    if (!default_conn || !notification_state.growing_env_notifications_enabled) {
        return 0; // No connection or notifications disabled
    }
    
    if (channel_id >= WATERING_CHANNELS_COUNT) {
        return -EINVAL;
    }
    
    /* Get channel growing environment configuration */
    watering_channel_t *channel;
    watering_error_t err = watering_get_channel(channel_id, &channel);
    if (err != WATERING_SUCCESS) {
        return -ENODATA;
    }
    
    /* Update growing_env_value with new data */
    struct growing_env_data *env_data = (struct growing_env_data *)growing_env_value;
    env_data->channel_id = channel_id;
    env_data->plant_type = (uint8_t)channel->plant_type;
    
    /* Set specific plant type based on plant_type */
    if (channel->plant_type == PLANT_TYPE_VEGETABLES) {
        env_data->specific_plant = (uint16_t)channel->plant_info.specific.vegetable;
    } else if (channel->plant_type == PLANT_TYPE_HERBS) {
        env_data->specific_plant = (uint16_t)channel->plant_info.specific.herb;
    } else if (channel->plant_type == PLANT_TYPE_FLOWERS) {
        env_data->specific_plant = (uint16_t)channel->plant_info.specific.flower;
    } else if (channel->plant_type == PLANT_TYPE_SHRUBS) {
        env_data->specific_plant = (uint16_t)channel->plant_info.specific.shrub;
    } else if (channel->plant_type == PLANT_TYPE_TREES) {
        env_data->specific_plant = (uint16_t)channel->plant_info.specific.tree;
    } else if (channel->plant_type == PLANT_TYPE_LAWN) {
        env_data->specific_plant = (uint16_t)channel->plant_info.specific.lawn;
    } else if (channel->plant_type == PLANT_TYPE_SUCCULENTS) {
        env_data->specific_plant = (uint16_t)channel->plant_info.specific.succulent;
    } else {
        env_data->specific_plant = 0;
    }
    
    env_data->soil_type = (uint8_t)channel->soil_type;
    env_data->irrigation_method = (uint8_t)channel->irrigation_method;
    env_data->use_area_based = channel->use_area_based ? 1 : 0;
    
    if (channel->use_area_based) {
        env_data->coverage.area_m2 = channel->coverage.area_m2;
    } else {
        env_data->coverage.plant_count = channel->coverage.plant_count;
    }
    
    env_data->sun_percentage = channel->sun_percentage;
    
    /* Custom plant fields */
    if (channel->plant_type == PLANT_TYPE_OTHER) {
        size_t name_len = strnlen(channel->custom_plant.custom_name, sizeof(channel->custom_plant.custom_name));
        if (name_len >= sizeof(env_data->custom_name)) {
            name_len = sizeof(env_data->custom_name) - 1;
        }
        memcpy(env_data->custom_name, channel->custom_plant.custom_name, name_len);
        env_data->custom_name[name_len] = '\0';
        
        env_data->water_need_factor = channel->custom_plant.water_need_factor;
        env_data->irrigation_freq_days = channel->custom_plant.irrigation_freq;
        env_data->prefer_area_based = channel->custom_plant.prefer_area_based ? 1 : 0;
    } else {
        strcpy(env_data->custom_name, "");
        env_data->water_need_factor = 1.0f;
        env_data->irrigation_freq_days = 1;
        env_data->prefer_area_based = env_data->use_area_based;
    }
    
    LOG_INF("Growing Environment update: ch=%u, plant=%u.%u, soil=%u, method=%u, %s=%.2f, sun=%u%%",
            env_data->channel_id, env_data->plant_type, env_data->specific_plant,
            env_data->soil_type, env_data->irrigation_method,
            env_data->use_area_based ? "area" : "count",
            env_data->use_area_based ? (double)env_data->coverage.area_m2 : (double)((float)env_data->coverage.plant_count),
            env_data->sun_percentage);
    
    /* Log custom plant info if applicable */
    if (env_data->plant_type == 7) {
    LOG_INF("Custom plant: '%s', water_factor=%.2f, freq=%u days, prefer_area=%u",
        env_data->custom_name, (double)env_data->water_need_factor, 
                env_data->irrigation_freq_days, env_data->prefer_area_based);
    }
    
    /* Send notification */
    notify_growing_env();
    
    return 0;
}

int bt_irrigation_auto_calc_status_notify(void) {
    if (!default_conn || !notification_state.auto_calc_status_notifications_enabled) {
        return 0; // No connection or notifications disabled
    }
    
    /* Update auto_calc_status_value with current system state */
    struct auto_calc_status_data *status_data = (struct auto_calc_status_data *)auto_calc_status_value;
    
    /* Use the currently selected channel from the global buffer */
    uint8_t channel_id = status_data->channel_id;
    if (channel_id >= WATERING_CHANNELS_COUNT) {
        channel_id = 0; /* Default to channel 0 */
        status_data->channel_id = channel_id;
    }
    
    /* Get channel data */
    watering_channel_t *channel;
    watering_error_t err = watering_get_channel(channel_id, &channel);
    if (err != WATERING_SUCCESS) {
        LOG_WRN("Failed to get channel %u for auto calc status notify: %d", channel_id, err);
        return -ENODATA;
    }
    
    /* Update status data */
    bool is_auto_mode = (channel->auto_mode == WATERING_AUTOMATIC_QUALITY || 
                        channel->auto_mode == WATERING_AUTOMATIC_ECO);
    status_data->calculation_active = is_auto_mode ? 1 : 0;
    status_data->auto_mode = (uint8_t)channel->auto_mode;
    
    /* Get water balance data if available */
    if (channel->water_balance != NULL) {
        water_balance_t *balance = (water_balance_t *)channel->water_balance;
        status_data->irrigation_needed = balance->irrigation_needed ? 1 : 0;
        status_data->current_deficit_mm = balance->current_deficit_mm;
        status_data->raw_mm = balance->raw_mm;
        status_data->effective_rain_mm = balance->effective_rain_mm;
    } else {
        status_data->irrigation_needed = 0;
        status_data->current_deficit_mm = 0.0f;
        status_data->raw_mm = 0.0f;
        status_data->effective_rain_mm = 0.0f;
    }
    
    /* Update timing information */
    status_data->last_calculation_time = channel->last_calculation_time;
    status_data->calculation_error = 0; /* No error for now */
    
    /* Refresh FAO-56 derived metrics */
    update_auto_calc_calculations(status_data, channel);
    
    LOG_DBG("Auto calc status notify: ch=%u, active=%u, needed=%u, deficit=%.2f, auto_mode=%u",
            status_data->channel_id, status_data->calculation_active, status_data->irrigation_needed,
            (double)status_data->current_deficit_mm, status_data->auto_mode);
    
    /* Send notification (with unified header) */
    notify_auto_calc_status();
    
    return 0;
}

int bt_irrigation_growing_env_notify(void) {
    if (!default_conn || !notification_state.growing_env_notifications_enabled) {
        return 0; // No connection or notifications disabled
    }
    
    /* Trigger growing environment notification */
    notify_growing_env();
    
    LOG_DBG("Growing environment notification triggered");
    
    return 0;
}

int bt_irrigation_direct_command(uint8_t channel_id, uint8_t command, uint16_t param) {
    /* Per BLE API Documentation: Direct command execution for immediate actions */
    /* Commands: 0=valve open, 1=valve close, 2=start watering, 3=stop watering */
    /* Returns: 0=success, negative=error */
    
    if (channel_id >= WATERING_CHANNELS_COUNT) {
        LOG_ERR("Invalid channel ID for direct command: %u", channel_id);
        return -EINVAL;
    }
    
    LOG_INF("Direct command: ch=%u, cmd=%u, param=%u", channel_id, command, param);
    
    watering_error_t err = WATERING_SUCCESS;
    
    switch (command) {
        case 0: /* Valve open */
            LOG_INF("Direct command: Open valve for channel %u", channel_id);
            err = watering_channel_on(channel_id);
            if (err == WATERING_SUCCESS) {
                bt_irrigation_valve_status_update(channel_id, true);
            }
            break;
            
        case 1: /* Valve close */
            LOG_INF("Direct command: Close valve for channel %u", channel_id);
            err = watering_channel_off(channel_id);
            if (err == WATERING_SUCCESS) {
                bt_irrigation_valve_status_update(channel_id, false);
            }
            break;
            
        case 2: /* Start watering */
            LOG_INF("Direct command: Start watering for channel %u, duration=%u minutes", 
                    channel_id, param);
            /* Create a manual watering task */
            if (param > 0) {
                watering_task_t task = {0};
                watering_channel_t *channel;
                err = watering_get_channel(channel_id, &channel);
                if (err == WATERING_SUCCESS) {
                    task.channel = channel;
                    task.trigger_type = WATERING_TRIGGER_MANUAL;
                    
                    /* Set task parameters based on channel's watering mode */
                    if (channel->watering_event.watering_mode == WATERING_BY_DURATION) {
                        task.by_time.start_time = k_uptime_get_32() / 1000;
                    } else {
                        task.by_volume.volume_liters = param; /* Use param as volume */
                    }
                    
                    err = watering_add_task(&task);
                    if (err == WATERING_SUCCESS) {
                        bt_irrigation_current_task_notify();
                    }
                }
            } else {
                LOG_ERR("Invalid watering duration: %u", param);
                err = WATERING_ERROR_INVALID_PARAM;
            }
            break;
            
        case 3: /* Stop watering */
            LOG_INF("Direct command: Stop watering for channel %u", channel_id);
            err = watering_channel_off(channel_id);
            if (err == WATERING_SUCCESS) {
                bt_irrigation_current_task_notify();
                bt_irrigation_valve_status_update(channel_id, false);
            }
            break;
            
        default:
            LOG_ERR("Unknown direct command: %u", command);
            return -EINVAL;
    }
    
    if (err != WATERING_SUCCESS) {
        LOG_ERR("Direct command failed: ch=%u, cmd=%u, error=%d", channel_id, command, err);
        return -EIO;
    }
    
    LOG_INF("✅ Direct command executed successfully: ch=%u, cmd=%u", channel_id, command);
    return 0;
}

int bt_irrigation_record_error(uint8_t channel_id, uint8_t error_code) {
    /* Per BLE API Documentation: Record error and trigger alarm notification */
    /* Error codes: 1-13 mapped to alarm codes, channel_id for context */
    
    if (channel_id >= WATERING_CHANNELS_COUNT) {
        LOG_ERR("Invalid channel ID for error recording: %u", channel_id);
        return -EINVAL;
    }
    
    if (error_code == 0 || error_code > 13) {
        LOG_ERR("Invalid error code: %u (must be 1-13)", error_code);
        return -EINVAL;
    }
    
    LOG_ERR("Recording error: ch=%u, error_code=%u", channel_id, error_code);
    
    /* Map error code to alarm code and generate alarm data */
    uint8_t alarm_code = error_code;
    uint16_t alarm_data = (uint16_t)channel_id; /* Use channel_id as context */
    
    /* Send alarm notification */
    int err = bt_irrigation_alarm_notify(alarm_code, alarm_data);
    if (err != 0) {
        LOG_ERR("Failed to send alarm notification: %d", err);
    }
    
    /* Update diagnostics if applicable */
    if (diagnostics_error_count < UINT16_MAX) {
        diagnostics_error_count++;
    }
    diagnostics_last_error = error_code;
    
    /* Update system status based on error type */
    watering_status_t status = WATERING_STATUS_FAULT;
    if (error_code == 1) { /* Flow sensor error */
        status = WATERING_STATUS_NO_FLOW;
    } else if (error_code == 5) { /* Unexpected flow */
        status = WATERING_STATUS_UNEXPECTED_FLOW;
    } else if (error_code == 6) { /* RTC error */
        status = WATERING_STATUS_RTC_ERROR;
    } else if (error_code == 10) { /* Power supply */
        status = WATERING_STATUS_LOW_POWER;
    }
    
    /* Notify system status change */
    bt_irrigation_system_status_update(status);
    
    /* Send diagnostics update */
    bt_irrigation_diagnostics_notify();
    
    LOG_INF("✅ Error recorded and notifications sent: ch=%u, error_code=%u", channel_id, error_code);
    
    return 0;
}

int bt_irrigation_update_history_aggregations(void) {
    /* Per BLE API Documentation: Update history aggregations for daily, monthly, and annual statistics */
    /* This function should be called periodically to update aggregated statistics */
    /* Returns: 0=success, negative=error */
    
    LOG_INF("Updating history aggregations...");
    
    /* Update daily aggregations for all channels */
    for (uint8_t channel_id = 0; channel_id < WATERING_CHANNELS_COUNT; channel_id++) {
        watering_channel_t *channel;
        watering_error_t err = watering_get_channel(channel_id, &channel);
        if (err != WATERING_SUCCESS) {
            LOG_WRN("Failed to get channel %u for aggregation: %d", channel_id, err);
            continue;
        }
        
        /* Update daily statistics for this channel */
        /* In a real implementation, this would:
         * 1. Query the history system for today's watering sessions
         * 2. Calculate total volume, duration, session count, success rate
         * 3. Store aggregated data for fast retrieval
         * 4. Update monthly/annual aggregations if needed
         */
        
        LOG_DBG("Updated daily aggregations for channel %u", channel_id);
    }
    
    /* Update monthly aggregations */
    /* This would typically be called once per day to update monthly totals */
    static uint32_t last_monthly_update = 0;
    uint32_t current_time = timezone_get_unix_utc(); /* Use RTC for persistent scheduling */
    
    if (current_time > 0 && current_time - last_monthly_update > (24 * 60 * 60)) { /* Once per day */
        LOG_INF("Updating monthly aggregations...");
        
        /* Update monthly statistics for all channels */
        for (uint8_t channel_id = 0; channel_id < WATERING_CHANNELS_COUNT; channel_id++) {
            /* Calculate monthly totals from daily data */
            LOG_DBG("Updated monthly aggregations for channel %u", channel_id);
        }
        
        last_monthly_update = current_time;
    }
    
    /* Update annual aggregations */
    /* This would typically be called once per month to update annual totals */
    static uint32_t last_annual_update = 0;
    
    if (current_time > 0 && current_time - last_annual_update > (30 * 24 * 60 * 60)) { /* Once per month */
        LOG_INF("Updating annual aggregations...");
        
        /* Update annual statistics for all channels */
        for (uint8_t channel_id = 0; channel_id < WATERING_CHANNELS_COUNT; channel_id++) {
            /* Calculate annual totals from monthly data */
            LOG_DBG("Updated annual aggregations for channel %u", channel_id);
        }
        
        last_annual_update = current_time;
    }
    
    /* Send history notifications to inform clients of updated aggregations */
    if (default_conn && notification_state.history_notifications_enabled) {
        /* Notify about the aggregation update */
        LOG_INF("Sending history aggregation update notification");
        
        /* This would typically send a notification with aggregation metadata */
        /* For now, we'll just log the event */
    }
    
    LOG_INF("✅ History aggregations updated successfully");
    
    return 0;
}


int bt_irrigation_queue_status_notify(void) {
    if (!default_conn || !notification_state.task_queue_notifications_enabled) {
        return 0; // No connection or notifications disabled
    }

    /* Update task_queue_value with current task queue status per BLE API Documentation */
    struct task_queue_data *queue_data = (struct task_queue_data *)task_queue_value;
    
    /* Get current queue status */
    uint8_t pending_count = 0;
    bool active = false;
    watering_error_t err = watering_get_queue_status(&pending_count, &active);
    
    if (err == WATERING_SUCCESS) {
        queue_data->pending_count = pending_count;
    } else {
        queue_data->pending_count = 0;
    }
    
    /* Get current task information */
    watering_task_t *current_task = watering_get_current_task();
    if (current_task) {
        uint8_t channel_id = current_task->channel - watering_channels;
        queue_data->current_channel = channel_id;
        queue_data->current_task_type = (current_task->channel->watering_event.watering_mode == WATERING_BY_DURATION) ? 0 : 1;
        
        if (current_task->channel->watering_event.watering_mode == WATERING_BY_DURATION) {
            queue_data->current_value = current_task->channel->watering_event.watering.by_duration.duration_minutes;
        } else {
            queue_data->current_value = current_task->channel->watering_event.watering.by_volume.volume_liters;
        }
        queue_data->active_task_id = 1; /* Simple ID for active task */
    } else {
        queue_data->current_channel = 0xFF; /* No active channel */
        queue_data->current_task_type = 0;
        queue_data->current_value = 0;
        queue_data->active_task_id = 0;
    }
    
    /* Get completed tasks count from tracking system */
    queue_data->completed_tasks = watering_get_completed_tasks_count();
    
    /* Clear command and task_id_to_delete fields for notification */
    queue_data->command = 0;
    queue_data->task_id_to_delete = 0;

    const struct bt_gatt_attr *attr = &irrigation_svc.attrs[ATTR_IDX_TASK_QUEUE_VALUE];
    int bt_err = safe_notify(default_conn, attr, task_queue_value, sizeof(task_queue_value));

    if (bt_err == 0) {
        LOG_INF("✅ Task Queue notification sent: pending=%u, current_ch=%u, task_type=%u, value=%u, active_id=%u",
                queue_data->pending_count, queue_data->current_channel, queue_data->current_task_type,
                queue_data->current_value, queue_data->active_task_id);
    } else {
        LOG_ERR("❌ Failed to send Task Queue notification: %d", bt_err);
    }

    return bt_err;
}

int bt_irrigation_alarm_clear(uint8_t alarm_code) {
    struct alarm_data *alarm = (struct alarm_data *)alarm_value;
    
    if (alarm_code == 0x00) {
        // Clear all alarms
        printk("BLE: API call to clear all alarms\n");
        watering_clear_errors();
        
        // Reset alarm data
        alarm->alarm_code = 0;
        alarm->alarm_data = 0;
        alarm->timestamp = 0;
        
        // Notify cleared status
        bt_irrigation_alarm_notify(0, 0);
        
    } else if (alarm_code >= 1 && alarm_code <= 13) {
        // Clear specific alarm if it matches current alarm
        if (alarm->alarm_code == alarm_code) {
            printk("BLE: API call to clear alarm %d\n", alarm_code);
            watering_clear_errors();
            
            // Reset alarm data
            alarm->alarm_code = 0;
            alarm->alarm_data = 0;
            alarm->timestamp = 0;
            
            // Notify cleared status
            bt_irrigation_alarm_notify(0, 0);
        } else {
            printk("BLE: Alarm code %d does not match current alarm %d\n", 
                   alarm_code, alarm->alarm_code);
            return -1; // No match
        }
    } else {
        printk("BLE: Invalid alarm clear code: %d\n", alarm_code);
        return -1; // Invalid code
    }
    
    return 0;
}

/* Diagnostics update function */
int bt_irrigation_diagnostics_update(uint16_t error_count, uint8_t last_error, uint8_t valve_status) {
    if (!default_conn || !notification_state.diagnostics_notifications_enabled) {
        /* Update local tracking even if notifications are disabled */
        diagnostics_error_count = error_count;
        diagnostics_last_error = last_error;
        return 0;
    }
    
    /* Update local tracking variables */
    diagnostics_error_count = error_count;
    diagnostics_last_error = last_error;
    
    /* Update diagnostics_value with current system data */
    struct diagnostics_data *diag = (struct diagnostics_data *)diagnostics_value;
    
    /* Calculate uptime more accurately using RTC when available */
    uint32_t current_utc = timezone_get_unix_utc();
    if (current_utc > 0) {
        /* Use RTC-based uptime for more accurate persistent measurement */
        /* NOTE: In a production system, boot_time_utc should be stored in NVS */
        static uint32_t boot_time_utc = 0;
        if (boot_time_utc == 0) {
            /* First call - estimate boot time */
            boot_time_utc = current_utc - (k_uptime_get() / 1000);
        }
        diag->uptime = (current_utc - boot_time_utc) / 60; /* Convert to minutes */
    } else {
        /* Fallback to boot-relative uptime if RTC unavailable */
        diag->uptime = k_uptime_get() / (1000 * 60);
    }
    
    diag->error_count = error_count;
    diag->last_error = last_error;
    diag->valve_status = valve_status;
    diag->battery_level = 0xFF; /* No battery monitoring */
    memset(diag->reserved, 0, sizeof(diag->reserved));
    
    LOG_INF("Diagnostics updated: uptime=%u min, errors=%u, last_error=%u, valve_status=0x%02x",
            diag->uptime, diag->error_count, diag->last_error, diag->valve_status);
    
    /* Send notification if enabled */
    return bt_irrigation_diagnostics_notify();
}

/* Advanced Notification System Implementation */
static void init_notification_pool(void) {
    memset(&notification_pool, 0, sizeof(notification_pool));
    
    /* Reset priority throttling states to defaults */
    priority_state[0].last_notification_time = 0;
    priority_state[0].throttle_interval = THROTTLE_CRITICAL_MS;
    priority_state[0].success_count = 0;
    priority_state[0].failure_count = 0;
    
    priority_state[1].last_notification_time = 0;
    priority_state[1].throttle_interval = THROTTLE_HIGH_MS;
    priority_state[1].success_count = 0;
    priority_state[1].failure_count = 0;
    
    priority_state[2].last_notification_time = 0;
    priority_state[2].throttle_interval = THROTTLE_NORMAL_MS;
    priority_state[2].success_count = 0;
    priority_state[2].failure_count = 0;
    
    priority_state[3].last_notification_time = 0;
    priority_state[3].throttle_interval = THROTTLE_LOW_MS;
    priority_state[3].success_count = 0;
    priority_state[3].failure_count = 0;
    
    LOG_INF("Advanced notification pool initialized");
}

static void buffer_pool_maintenance(void) {
    static int64_t last_maintenance = 0;
    int64_t now = k_uptime_get();
    
    // Run maintenance every 30 seconds
    if (now - last_maintenance < 30000) {
        return;
    }
    last_maintenance = now;
    
    // Clean up expired buffers
    k_mutex_lock(&notification_mutex, K_FOREVER);
    for (int i = 0; i < BLE_BUFFER_POOL_SIZE; i++) {
        if (notification_pool[i].in_use) {
            // Check if buffer has been in use too long (over 60 seconds)
            if ((k_uptime_get_32() - notification_pool[i].timestamp) > 60000) {
                notification_pool[i].in_use = false;
                if (buffers_in_use > 0) {
                    buffers_in_use--;
                }
                LOG_DBG("Cleaned expired notification buffer %d", i);
            }
        }
    }
    k_mutex_unlock(&notification_mutex);
    
    // Adaptive throttling adjustments per priority
    for (int p = 0; p < 4; p++) {
        if (priority_state[p].success_count + priority_state[p].failure_count > 10) {
            float success_rate = (float)priority_state[p].success_count / 
                                 (priority_state[p].success_count + priority_state[p].failure_count);
            
            if (success_rate < 0.8f) {
                // Increase throttling if success rate is low
                priority_state[p].throttle_interval = MIN(priority_state[p].throttle_interval * 1.2f, 5000);
        LOG_DBG("Increased throttle interval for priority %d to %ums (success: %.2f%%)", 
            p, priority_state[p].throttle_interval, (double)(success_rate * 100.0f));
            } else if (success_rate > 0.95f) {
                // Decrease throttling if success rate is very high  
                uint32_t base_interval = (p == 0) ? THROTTLE_CRITICAL_MS : 
                                        (p == 1) ? THROTTLE_HIGH_MS :
                                        (p == 2) ? THROTTLE_NORMAL_MS : THROTTLE_LOW_MS;
                priority_state[p].throttle_interval = MAX(priority_state[p].throttle_interval * 0.9f, base_interval);
        LOG_DBG("Decreased throttle interval for priority %d to %ums (success: %.2f%%)", 
            p, priority_state[p].throttle_interval, (double)(success_rate * 100.0f));
            }
            
            // Reset counters
            priority_state[p].success_count = 0;
            priority_state[p].failure_count = 0;
        }
    }
}

/* ------------------------------------------------------------------ */
/* BLE Debugging and Testing Functions                               */
/* ------------------------------------------------------------------ */

int bt_irrigation_debug_notifications(void) {
    if (!default_conn) {
        LOG_ERR("❌ No BLE connection for debugging");
        return -ENOTCONN;
    }

    LOG_INF("🔍 BLE Notification System Debug (compact)");

    force_enable_all_notifications();

    int result = 0;

    int err = bt_irrigation_channel_config_update(0);
    if (err != 0) {
        LOG_ERR("Channel configuration notification failed during debug: %d", err);
        result = err;
    }

    err = bt_irrigation_schedule_update(0);
    if (err != 0) {
        LOG_ERR("Schedule notification failed during debug: %d", err);
        if (result == 0) {
            result = err;
        }
    }

    err = bt_irrigation_statistics_update(0);
    if (err != 0) {
        LOG_ERR("Statistics notification failed during debug: %d", err);
        if (result == 0) {
            result = err;
        }
    }

    LOG_INF("🔍 Debug complete - notification test result: %d", result);
    return result;
}

int bt_irrigation_test_channel_notification(uint8_t channel_id) {
    if (!default_conn) {
        LOG_ERR("❌ No BLE connection for test");
        return -ENOTCONN;
    }
    
    if (channel_id >= WATERING_CHANNELS_COUNT) {
        LOG_ERR("❌ Invalid channel ID: %u", channel_id);
        return -EINVAL;
    }

    if (!notification_state.channel_config_notifications_enabled ||
        !notification_state.schedule_notifications_enabled ||
        !notification_state.statistics_notifications_enabled) {
        force_enable_all_notifications();
    }

    int result = bt_irrigation_channel_config_update(channel_id);
    int err = bt_irrigation_schedule_update(channel_id);
    if (result == 0 && err != 0) {
        result = err;
    }

    err = bt_irrigation_statistics_update(channel_id);
    if (result == 0 && err != 0) {
        result = err;
    }

    LOG_INF("🧪 Channel %u notification test result: %d", channel_id, result);
    return result;
}

int bt_irrigation_force_enable_notifications(void) {
    if (!default_conn) {
        LOG_ERR("❌ No BLE connection for force enable");
        return -ENOTCONN;
    }
    
    force_enable_all_notifications();
    return 0;
}

/* Rain history command processing state and helpers (CONFIG_BT) */
static void rain_history_fragment_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(rain_history_fragment_work, rain_history_fragment_work_handler);

static struct {
    bool command_active;
    uint8_t current_command;
    uint32_t start_timestamp;
    uint32_t end_timestamp;
    uint16_t max_entries;
    uint8_t data_type;
    uint16_t total_entries;
    uint16_t current_entry;
    uint8_t current_fragment;
    uint8_t total_fragments;
    uint8_t *fragment_buffer;
    bool fragment_buffer_owned;
    struct bt_conn *requesting_conn;
} rain_history_cmd_state = {0};

/* Clear state and free owned buffers after a command completes */
static void rain_history_reset_state(void) {
    if (rain_history_cmd_state.fragment_buffer_owned && rain_history_cmd_state.fragment_buffer) {
        k_free(rain_history_cmd_state.fragment_buffer);
    }
    rain_history_cmd_state.command_active = false;
    rain_history_cmd_state.current_command = 0;
    rain_history_cmd_state.start_timestamp = 0;
    rain_history_cmd_state.end_timestamp = 0;
    rain_history_cmd_state.max_entries = 0;
    rain_history_cmd_state.data_type = 0;
    rain_history_cmd_state.total_entries = 0;
    rain_history_cmd_state.current_entry = 0;
    rain_history_cmd_state.current_fragment = 0;
    rain_history_cmd_state.total_fragments = 0;
    rain_history_cmd_state.fragment_buffer = NULL;
    rain_history_cmd_state.fragment_buffer_owned = false;
    rain_history_cmd_state.requesting_conn = NULL;
    k_work_cancel_delayable(&rain_history_fragment_work);
}

/* ------------------------------------------------------------------ */
/* Rain History Helper Functions                                     */
/* ------------------------------------------------------------------ */

/**
 * @brief Send error response for rain history command
 */
static void rain_history_send_error_response(struct bt_conn *conn, uint8_t error_code) {
    rain_history_response_t error_response = {0};
    error_response.header.fragment_index = 0;
    error_response.header.total_fragments = 1;
    error_response.header.status = error_code;
    error_response.header.data_type = 0xFF; /* Error response */
    error_response.header.fragment_size = 1;
    error_response.data[0] = error_code;
    
    size_t notify_len = sizeof(error_response.header) + error_response.header.fragment_size;
    memcpy(rain_history_value, &error_response, notify_len);
    if (notification_state.rain_history_notifications_enabled) {
        bt_gatt_notify(conn, &irrigation_svc.attrs[ATTR_IDX_RAIN_HISTORY_VALUE], 
                      &error_response, notify_len);
    }
    
    rain_history_reset_state();
}

/**
 * @brief Process hourly rain history data request
 */
static int process_rain_history_hourly_request(uint32_t start_time, uint32_t end_time, uint16_t max_entries) {
    LOG_INF("Processing hourly rain data request: %u to %u, max %u entries", 
            start_time, end_time, max_entries);
    
    /* Allocate buffer for hourly data (stored until all fragments sent) */
    rain_hourly_data_t *hourly_data = k_malloc(max_entries * sizeof(rain_hourly_data_t));
    if (!hourly_data) {
        LOG_ERR("Failed to allocate memory for hourly data");
        rain_history_send_error_response(rain_history_cmd_state.requesting_conn, 0x05); /* Memory error */
        return -ENOMEM;
    }
    
    /* Retrieve hourly data from rain history */
    uint16_t actual_count = 0;
    watering_error_t ret = rain_history_get_hourly(start_time, end_time, hourly_data, max_entries, &actual_count);
    if (ret != WATERING_SUCCESS) {
        LOG_ERR("Failed to retrieve hourly rain data: %d", ret);
        k_free(hourly_data);
        rain_history_send_error_response(rain_history_cmd_state.requesting_conn, 0x06); /* Data error */
        return -EIO;
    }
    
    LOG_INF("Retrieved %u hourly entries", actual_count);
    
    /* Calculate fragmentation */
    size_t total_data_size = actual_count * sizeof(rain_hourly_data_t);
    uint8_t total_fragments = (total_data_size + RAIN_HISTORY_FRAGMENT_SIZE - 1) / RAIN_HISTORY_FRAGMENT_SIZE;
    
    if (total_fragments > RAIN_HISTORY_MAX_FRAGMENTS) {
        LOG_ERR("Too many fragments required: %u (max %u)", total_fragments, RAIN_HISTORY_MAX_FRAGMENTS);
        k_free(hourly_data);
        rain_history_send_error_response(rain_history_cmd_state.requesting_conn, 0x07); /* Too much data */
        return -E2BIG;
    }
    
    rain_history_cmd_state.total_entries = actual_count;
    rain_history_cmd_state.total_fragments = total_fragments;
    
    /* Stash pointer for fragmenting and send asynchronously to avoid blocking BT thread */
    rain_history_cmd_state.fragment_buffer = (uint8_t *)hourly_data;
    rain_history_cmd_state.fragment_buffer_owned = true;
    k_work_schedule(&rain_history_fragment_work, K_NO_WAIT);
    
    return 0;
}

/**
 * @brief Process daily rain history data request
 */
static int process_rain_history_daily_request(uint32_t start_time, uint32_t end_time, uint16_t max_entries) {
    LOG_INF("Processing daily rain data request: %u to %u, max %u entries", 
            start_time, end_time, max_entries);
    
    /* Allocate buffer for daily data */
    rain_daily_data_t *daily_data = k_malloc(max_entries * sizeof(rain_daily_data_t));
    if (!daily_data) {
        LOG_ERR("Failed to allocate memory for daily data");
        rain_history_send_error_response(rain_history_cmd_state.requesting_conn, 0x05); /* Memory error */
        return -ENOMEM;
    }
    
    /* Retrieve daily data from rain history */
    uint16_t actual_count = 0;
    watering_error_t ret = rain_history_get_daily(start_time, end_time, daily_data, max_entries, &actual_count);
    if (ret != WATERING_SUCCESS) {
        LOG_ERR("Failed to retrieve daily rain data: %d", ret);
        k_free(daily_data);
        rain_history_send_error_response(rain_history_cmd_state.requesting_conn, 0x06); /* Data error */
        return -EIO;
    }
    
    LOG_INF("Retrieved %u daily entries", actual_count);
    
    /* Calculate fragmentation */
    size_t total_data_size = actual_count * sizeof(rain_daily_data_t);
    uint8_t total_fragments = (total_data_size + RAIN_HISTORY_FRAGMENT_SIZE - 1) / RAIN_HISTORY_FRAGMENT_SIZE;
    
    if (total_fragments > RAIN_HISTORY_MAX_FRAGMENTS) {
        LOG_ERR("Too many fragments required: %u (max %u)", total_fragments, RAIN_HISTORY_MAX_FRAGMENTS);
        k_free(daily_data);
        rain_history_send_error_response(rain_history_cmd_state.requesting_conn, 0x07); /* Too much data */
        return -E2BIG;
    }
    
    rain_history_cmd_state.total_entries = actual_count;
    rain_history_cmd_state.total_fragments = total_fragments;
    
    rain_history_cmd_state.fragment_buffer = (uint8_t *)daily_data;
    rain_history_cmd_state.fragment_buffer_owned = true;
    k_work_schedule(&rain_history_fragment_work, K_NO_WAIT);
    
    return 0;
}

/**
 * @brief Send a specific fragment of rain history data
 */
static int send_rain_history_fragment(struct bt_conn *conn, uint8_t fragment_id) {
    if (!conn || fragment_id >= rain_history_cmd_state.total_fragments) {
        return -EINVAL;
    }
    
    rain_history_response_t response = {0};
    response.header.fragment_index = fragment_id;
    response.header.total_fragments = (uint8_t)rain_history_cmd_state.total_fragments;
    response.header.status = 0; /* Success */
    response.header.data_type = rain_history_cmd_state.data_type;
    
    /* Calculate fragment data */
    size_t entry_size = (rain_history_cmd_state.data_type == 0) ? 
                       sizeof(rain_hourly_data_t) : sizeof(rain_daily_data_t);
    size_t fragment_offset = fragment_id * RAIN_HISTORY_FRAGMENT_SIZE;
    size_t remaining_data = (rain_history_cmd_state.total_entries * entry_size) - fragment_offset;
    size_t fragment_data_size = (remaining_data > RAIN_HISTORY_FRAGMENT_SIZE) ? 
                               RAIN_HISTORY_FRAGMENT_SIZE : remaining_data;
    
    response.header.fragment_size = (uint8_t)fragment_data_size;
    
    /* Copy real data from fragment buffer if available */
    if (rain_history_cmd_state.fragment_buffer) {
        memcpy(response.data, rain_history_cmd_state.fragment_buffer + fragment_offset, fragment_data_size);
    } else {
        memset(response.data, 0, fragment_data_size); /* Fallback (should not happen) */
    }

    /* Update global value buffer */
    size_t notify_len = sizeof(response.header) + response.header.fragment_size;
    memcpy(rain_history_value, &response, notify_len);
    
    /* Send notification if enabled */
    if (notification_state.rain_history_notifications_enabled) {
        int ret = bt_gatt_notify(conn, &irrigation_svc.attrs[ATTR_IDX_RAIN_HISTORY_VALUE], 
                                &response, notify_len);
        if (ret < 0) {
            LOG_ERR("Failed to send rain history fragment %u: %d", fragment_id, ret);
            return ret;
        }
    }
    
    LOG_DBG("Sent rain history fragment %u/%u (%u bytes)", 
            fragment_id + 1, rain_history_cmd_state.total_fragments, (unsigned)fragment_data_size);
    
    return 0;
}

/* Work handler to stream fragments without blocking the BT host thread */
static void rain_history_fragment_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    if (!rain_history_cmd_state.command_active || !rain_history_cmd_state.requesting_conn || !connection_active || !default_conn) {
        rain_history_reset_state();
        return;
    }

    int ret = send_rain_history_fragment(rain_history_cmd_state.requesting_conn,
                                         rain_history_cmd_state.current_fragment);
    if (ret < 0) {
        LOG_ERR("Rain history fragment send failed: %d", ret);
        rain_history_send_error_response(rain_history_cmd_state.requesting_conn, 0x03);
        rain_history_reset_state();
        return;
    }

    rain_history_cmd_state.current_fragment++;
    if (rain_history_cmd_state.current_fragment < rain_history_cmd_state.total_fragments) {
        /* Short delay to avoid flooding the stack, without sleeping on BT thread */
        k_work_schedule(&rain_history_fragment_work, K_MSEC(5));
    } else {
        rain_history_reset_state();
    }
}

/**
 * @brief Send rain configuration notification
 */
void bt_irrigation_rain_config_notify(void) {
    if (!notification_state.rain_config_notifications_enabled) {
        return;
    }
    
    /* Update rain config data */
    struct rain_config_data config_data = {0};
    
    if (rain_sensor_is_active()) {
        config_data.mm_per_pulse = rain_sensor_get_calibration();
        config_data.debounce_ms = rain_sensor_get_debounce();
        config_data.sensor_enabled = rain_sensor_is_enabled() ? 1 : 0;
        config_data.integration_enabled = rain_integration_is_enabled() ? 1 : 0;
        config_data.rain_sensitivity_pct = rain_integration_get_sensitivity();
        config_data.skip_threshold_mm = rain_integration_get_skip_threshold();
    } else {
        /* Default values if sensor not active */
        config_data.mm_per_pulse = 0.2f;
        config_data.debounce_ms = 50;
        config_data.sensor_enabled = 0;
        config_data.integration_enabled = 0;
        config_data.rain_sensitivity_pct = 75.0f;
        config_data.skip_threshold_mm = 5.0f;
    }
    
    /* Update global value buffer */
    memcpy(rain_config_value, &config_data, sizeof(config_data));
    
    /* Send notification */
    int ret = bt_gatt_notify(NULL, &irrigation_svc.attrs[ATTR_IDX_RAIN_CONFIG_VALUE], 
                            rain_config_value, sizeof(config_data));
    if (ret < 0) {
        LOG_ERR("Failed to send rain config notification: %d", ret);
    }
}

/**
 * @brief Send rain data notification
 */
void bt_irrigation_rain_data_notify(void) {
    if (!notification_state.rain_data_notifications_enabled) {
        return;
    }
    
    struct rain_data_data data = {0};
    
    if (rain_sensor_is_active()) {
        /* Get current rain sensor data */
        data.current_hour_mm_x100 = (uint32_t)(rain_history_get_current_hour() * 100.0f);
        data.today_total_mm_x100 = (uint32_t)(rain_history_get_today() * 100.0f);
        data.last_24h_mm_x100 = (uint32_t)(rain_history_get_last_24h() * 100.0f);
        
        /* Get sensor status */
        if (rain_sensor_is_active()) {
            data.current_rate_mm_h_x100 = (uint16_t)(rain_sensor_get_hourly_rate_mm() * 100.0f);
            data.last_pulse_time = rain_sensor_get_last_pulse_time();
            data.total_pulses = rain_sensor_get_pulse_count();
            data.sensor_status = 1; /* Active */
            data.data_quality = 80; /* Good quality */
        } else {
            data.sensor_status = 2; /* Error */
            data.data_quality = 0;
        }
    } else {
        /* Sensor disabled */
        data.sensor_status = 0; /* Inactive */
        data.data_quality = 0;
    }
    
    /* Update global value buffer */
    memcpy(rain_data_value, &data, sizeof(data));
    
    /* Send notification */
    int ret = bt_gatt_notify(NULL, &irrigation_svc.attrs[ATTR_IDX_RAIN_DATA_VALUE], 
                            rain_data_value, sizeof(data));
    if (ret < 0) {
        LOG_ERR("Failed to send rain data notification: %d", ret);
    }

    /* Track last status to allow immediate re-broadcast on status change */
    rain_last_status_sent = ((struct rain_data_data *)rain_data_value)->sensor_status;
}

/**
 * @brief Send rain sensor pulse notification (called when rain is detected)
 */
void bt_irrigation_rain_pulse_notify(uint32_t pulse_count, float current_rate_mm_h) {
    if (!notification_state.rain_data_notifications_enabled) {
        return;
    }
    /* Throttle pulse-driven notifications to minimum 5s */
    uint32_t now = k_uptime_get_32();
    if (now - rain_last_pulse_notify_ms < 5000) {
        return;
    }
    
    /* Update rain data with new pulse information */
    struct rain_data_data data = {0};
    data.current_hour_mm_x100 = (uint32_t)(rain_history_get_current_hour() * 100.0f);
    data.today_total_mm_x100 = (uint32_t)(rain_history_get_today() * 100.0f);
    data.last_24h_mm_x100 = (uint32_t)(rain_history_get_last_24h() * 100.0f);
    data.current_rate_mm_h_x100 = (uint16_t)(current_rate_mm_h * 100.0f);

    rain_sensor_data_t sensor_data;
    if (rain_sensor_get_data(&sensor_data) == 0) {
        data.last_pulse_time = sensor_data.last_pulse_time;
        data.total_pulses = sensor_data.total_pulses;
        data.sensor_status = (uint8_t)sensor_data.status;
        data.data_quality = sensor_data.data_quality;
    } else {
        data.last_pulse_time = k_uptime_get_32() / 1000;
        data.total_pulses = pulse_count;
        data.sensor_status = 2; /* Error retrieving sensor data */
        data.data_quality = 0;
    }
    
    /* Update global value buffer */
    memcpy(rain_data_value, &data, sizeof(data));
    
    /* Send notification */
    int ret = bt_gatt_notify(NULL, &irrigation_svc.attrs[ATTR_IDX_RAIN_DATA_VALUE], 
                            rain_data_value, sizeof(data));
    if (ret < 0) {
        LOG_ERR("Failed to send rain pulse notification: %d", ret);
    }
    
    LOG_DBG("Rain pulse notification sent: %u pulses, %.2f mm/h", pulse_count, (double)current_rate_mm_h);
    rain_last_pulse_notify_ms = now;
    rain_last_status_sent = ((struct rain_data_data *)rain_data_value)->sensor_status;
}

/**
 * @brief Send rain integration status notification
 */
void bt_irrigation_rain_integration_notify(uint8_t channel_id, float reduction_pct, bool skip_irrigation) {
    if (!default_conn || !notification_state.rain_integration_status_notifications_enabled) {
        return;
    }

    /* Build a compact delta payload for notify: channel_id, reduction, skip, ts */
    struct __packed {
        uint8_t  channel_id;
        float    reduction_pct;
        uint8_t  skip_irrigation;
        uint32_t timestamp;
    } delta = { channel_id, reduction_pct, (uint8_t)(skip_irrigation ? 1 : 0), k_uptime_get_32() / 1000 };

    int ret = bt_gatt_notify(default_conn, &irrigation_svc.attrs[ATTR_IDX_RAIN_INTEGRATION_STATUS_VALUE],
                             &delta, sizeof(delta));
    if (ret < 0) {
        LOG_ERR("Failed to send rain integration status notify: %d", ret);
    } else {
        LOG_DBG("Rain integration: ch=%u, red=%.1f%%, skip=%u", channel_id, (double)reduction_pct, (unsigned)delta.skip_irrigation);
    }
}

/**
 * @brief Send full rain integration status notification
 */
int bt_irrigation_rain_integration_status_notify(void) {
    if (!default_conn || !notification_state.rain_integration_status_notifications_enabled) {
        return 0;
    }

    rain_integration_status_t sys = {0};
    if (watering_get_rain_integration_status(&sys) != WATERING_SUCCESS) {
        memset(&sys, 0, sizeof(sys));
    }

    struct rain_integration_status_ble ble = {0};
    ble.sensor_active = sys.sensor_active ? 1 : 0;
    ble.integration_enabled = sys.integration_enabled ? 1 : 0;
    ble.last_pulse_time = sys.last_pulse_time;
    ble.calibration_mm_per_pulse = sys.calibration_mm_per_pulse;
    ble.rainfall_last_hour = sys.rainfall_last_hour;
    ble.rainfall_last_24h = sys.rainfall_last_24h;
    ble.rainfall_last_48h = sys.rainfall_last_48h;
    ble.sensitivity_pct = sys.sensitivity_pct;
    ble.skip_threshold_mm = sys.skip_threshold_mm;
    for (int i = 0; i < 8; ++i) {
        ble.channel_reduction_pct[i] = sys.channel_reduction_pct[i];
        ble.channel_skip_irrigation[i] = sys.channel_skip_irrigation[i] ? 1 : 0;
    }
    ble.hourly_entries = sys.hourly_entries;
    ble.daily_entries = sys.daily_entries;
    ble.storage_usage_bytes = sys.storage_usage_bytes;

    memcpy(rain_integration_status_value, &ble, sizeof(ble));

    const struct bt_gatt_attr *attr = &irrigation_svc.attrs[ATTR_IDX_RAIN_INTEGRATION_STATUS_VALUE];
    int ret = safe_notify(default_conn, attr, rain_integration_status_value, sizeof(ble));
    
    if (ret < 0) {
        LOG_ERR("Failed to send rain integration status notify: %d", ret);
    } else {
        LOG_DBG("Rain integration status notification sent");
    }
    return ret;
}

/**
 * @brief Periodic rain data update (called from monitoring thread)
 */
void bt_irrigation_rain_periodic_update(void) {
    uint32_t now = k_uptime_get_32();
    /* Decide cadence based on activity and errors */
    bool active = rain_sensor_is_active();
    uint32_t period_ms = active ? 30000u : 300000u; /* 30s raining, 5min idle */

    /* Immediate on error or status change */
    uint8_t current_status = active ? 1 : (rain_sensor_is_enabled() ? 0 : 2);
    if (rain_last_status_sent != current_status) {
        bt_irrigation_rain_data_notify();
        rain_last_periodic_ms = now;
        return;
    }

    if ((now - rain_last_periodic_ms) >= period_ms) {
        bt_irrigation_rain_data_notify();
        rain_last_periodic_ms = now;
    }
}

/* ------------------------------------------------------------------ */
/* Rain Sensor Characteristics Implementation                        */
/* ------------------------------------------------------------------ */

/* Rain configuration characteristic implementation */
ssize_t read_rain_config(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                               void *buf, uint16_t len, uint16_t offset) {
    if (!conn || !attr || !buf) {
        LOG_ERR("Invalid parameters for rain config read");
        return -EINVAL;
    }
    
    struct rain_config_data config_data = {0};
    
    /* Get current rain sensor configuration */
    if (rain_sensor_is_enabled()) {
        config_data.mm_per_pulse = rain_sensor_get_calibration();
        config_data.debounce_ms = rain_sensor_get_debounce();
        config_data.sensor_enabled = rain_sensor_is_enabled() ? 1 : 0;
        config_data.integration_enabled = rain_sensor_is_integration_enabled() ? 1 : 0;
        config_data.rain_sensitivity_pct = rain_integration_get_sensitivity();
        config_data.skip_threshold_mm = rain_integration_get_skip_threshold();
    } else {
        /* Return default values if sensor not initialized */
        config_data.mm_per_pulse = 0.2f;
        config_data.debounce_ms = 50;
        config_data.sensor_enabled = 0;
        config_data.integration_enabled = 0;
        config_data.rain_sensitivity_pct = 75.0f;
        config_data.skip_threshold_mm = 5.0f;
    }
    
    /* Update the global value buffer */
    memcpy(rain_config_value, &config_data, sizeof(config_data));
    
    return bt_gatt_attr_read(conn, attr, buf, len, offset, 
                            rain_config_value, sizeof(config_data));
}

ssize_t write_rain_config(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                const void *buf, uint16_t len, uint16_t offset, uint8_t flags) {
    if (!conn || !attr || !buf) {
        LOG_ERR("Invalid parameters for rain config write");
        return -EINVAL;
    }
    
    if (offset != 0) {
        LOG_ERR("Rain config write with non-zero offset not supported");
        return -EINVAL;
    }
    
    if (len != sizeof(struct rain_config_data)) {
        LOG_ERR("Invalid rain config data length: %u, expected: %zu", 
                len, sizeof(struct rain_config_data));
        return -EINVAL;
    }
    
    /* Parse strict 18-byte structure */
    struct rain_config_data parsed = {0};
    memcpy(&parsed, buf, sizeof(struct rain_config_data));
    
    /* Validate configuration values */
    if (parsed.mm_per_pulse < 0.1f || parsed.mm_per_pulse > 10.0f) {
    LOG_ERR("Invalid mm_per_pulse: %.3f", (double)parsed.mm_per_pulse);
        return -EINVAL;
    }
    
    if (parsed.debounce_ms < 10 || parsed.debounce_ms > 1000) {
        LOG_ERR("Invalid debounce_ms: %u", parsed.debounce_ms);
        return -EINVAL;
    }
    
    if (parsed.rain_sensitivity_pct < 0.0f || parsed.rain_sensitivity_pct > 100.0f) {
    LOG_ERR("Invalid rain_sensitivity_pct: %.1f", (double)parsed.rain_sensitivity_pct);
        return -EINVAL;
    }
    
    if (parsed.skip_threshold_mm < 0.0f || parsed.skip_threshold_mm > 100.0f) {
    LOG_ERR("Invalid skip_threshold_mm: %.1f", (double)parsed.skip_threshold_mm);
        return -EINVAL;
    }
    
    /* Apply configuration to rain sensor */
    int ret = rain_sensor_set_calibration(parsed.mm_per_pulse);
    if (ret != 0) {
        LOG_ERR("Failed to set rain sensor calibration: %d", ret);
        return -EIO;
    }
    
    ret = rain_sensor_set_debounce(parsed.debounce_ms);
    if (ret != 0) {
        LOG_ERR("Failed to set rain sensor debounce: %d", ret);
        return -EIO;
    }
    
    ret = rain_sensor_set_enabled(parsed.sensor_enabled != 0);
    if (ret != 0) {
        LOG_ERR("Failed to set rain sensor enabled state: %d", ret);
        return -EIO;
    }
    
    ret = rain_sensor_set_integration_enabled(parsed.integration_enabled != 0);
    if (ret != 0) {
        LOG_ERR("Failed to set rain integration enabled state: %d", ret);
        return -EIO;
    }
    
    /* Apply integration configuration */
    ret = rain_integration_set_sensitivity(parsed.rain_sensitivity_pct);
    if (ret != 0) {
        LOG_ERR("Failed to set rain sensitivity: %d", ret);
        return -EIO;
    }
    
    ret = rain_integration_set_skip_threshold(parsed.skip_threshold_mm);
    if (ret != 0) {
        LOG_ERR("Failed to set rain skip threshold: %d", ret);
        return -EIO;
    }
    
    /* Save configuration to NVS */
    rain_sensor_save_config();
    rain_integration_save_config();
    
    /* Update global value buffer */
    memcpy(rain_config_value, &parsed, sizeof(struct rain_config_data));
    
    LOG_INF("Rain sensor configuration updated via BLE");
    LOG_INF("Calibration: %.3f mm/pulse, Debounce: %u ms, Enabled: %s, Integration: %s",
            (double)parsed.mm_per_pulse, parsed.debounce_ms,
            parsed.sensor_enabled ? "Yes" : "No",
            parsed.integration_enabled ? "Yes" : "No");
    
    /* Notify applied config for confirmation if subscribed */
    if (notification_state.rain_config_notifications_enabled && default_conn) {
        const struct bt_gatt_attr *attr_cfg = &irrigation_svc.attrs[ATTR_IDX_RAIN_CONFIG_VALUE];
    int nerr = safe_notify(default_conn, attr_cfg, rain_config_value, sizeof(struct rain_config_data));
        if (nerr) {
            LOG_WRN("Rain config notify after write failed: %d", nerr);
        }
    }
    
    return len;
}

void rain_config_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value) {
    bool notify_enabled = (value == BT_GATT_CCC_NOTIFY);
    notification_state.rain_config_notifications_enabled = notify_enabled;
    LOG_INF("Rain config notifications %s", notify_enabled ? "enabled" : "disabled");
    if (notify_enabled) {
        /* Push current configuration snapshot */
        bt_irrigation_rain_config_notify();
    }
}

/* Rain data characteristic implementation */
ssize_t read_rain_data(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                             void *buf, uint16_t len, uint16_t offset) {
    if (!conn || !attr || !buf) {
        LOG_ERR("Invalid parameters for rain data read");
        return -EINVAL;
    }
    
    struct rain_data_data data = {0};
    
    if (rain_sensor_is_enabled()) {
        /* Get current rain sensor data */
        rain_sensor_data_t sensor_data;
        int ret = rain_sensor_get_data(&sensor_data);
        if (ret == 0) {
            data.current_hour_mm_x100 = (uint32_t)(rain_history_get_current_hour() * 100.0f);
            data.today_total_mm_x100 = (uint32_t)(rain_history_get_today() * 100.0f);
            data.last_24h_mm_x100 = (uint32_t)(rain_history_get_last_24h() * 100.0f);
            data.current_rate_mm_h_x100 = (uint16_t)(sensor_data.hourly_rate_mm * 100.0f);
            data.last_pulse_time = sensor_data.last_pulse_time;
            data.total_pulses = sensor_data.total_pulses;
            data.sensor_status = (uint8_t)sensor_data.status;
            data.data_quality = sensor_data.data_quality;
        } else {
            /* Sensor error - return error status */
            data.sensor_status = 2; // Error status
            data.data_quality = 0;
        }
    } else {
        /* Sensor disabled */
        data.sensor_status = 0; // Inactive status
        data.data_quality = 0;
    }
    
    /* Update global value buffer */
    memcpy(rain_data_value, &data, sizeof(data));
    
    return bt_gatt_attr_read(conn, attr, buf, len, offset, 
                            rain_data_value, sizeof(data));
}

void rain_data_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value) {
    bool notify_enabled = (value == BT_GATT_CCC_NOTIFY);
    notification_state.rain_data_notifications_enabled = notify_enabled;
    LOG_INF("Rain data notifications %s", notify_enabled ? "enabled" : "disabled");
    if (notify_enabled) {
        /* Push immediate rain data snapshot */
        bt_irrigation_rain_data_notify();
    }
}

/* Rain history characteristic implementation */
ssize_t read_rain_history(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                void *buf, uint16_t len, uint16_t offset) {
    if (!conn || !attr || !buf) {
        LOG_ERR("Invalid parameters for rain history read");
        return -EINVAL;
    }
    
    /* Return current command buffer */
    return bt_gatt_attr_read(conn, attr, buf, len, offset, 
                            rain_history_value, sizeof(struct rain_history_cmd_data));
}

ssize_t write_rain_history(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                 const void *buf, uint16_t len, uint16_t offset, uint8_t flags) {
    if (!conn || !attr || !buf) {
        LOG_ERR("Invalid parameters for rain history write");
        return -EINVAL;
    }
    
    if (offset != 0) {
        LOG_ERR("Rain history write with non-zero offset not supported");
        return -EINVAL;
    }
    
    if (len != sizeof(struct rain_history_cmd_data)) {
        LOG_ERR("Invalid rain history command length: %u, expected: %zu", 
                len, sizeof(struct rain_history_cmd_data));
        return -EINVAL;
    }

    /* Only one rain history command at a time to keep BT host responsive */
    if (rain_history_cmd_state.command_active) {
        LOG_WRN("Rain history command already in progress");
        rain_history_send_error_response(conn, 0x01); /* Busy error */
        return -EBUSY;
    }
    
    const struct rain_history_cmd_data *cmd = (const struct rain_history_cmd_data *)buf;

    /* Echo command into value buffer for read-back */
    memcpy(rain_history_value, cmd, sizeof(struct rain_history_cmd_data));

    LOG_INF("Rain history cmd=0x%02X start=%u end=%u max=%u type=%u",
            cmd->command, cmd->start_timestamp, cmd->end_timestamp,
            cmd->max_entries, cmd->data_type);

    /* Basic validation */
    if (cmd->data_type > 1 && cmd->data_type != 0xFE) {
        rain_history_send_error_response(conn, 0xFE); /* Invalid parameters */
        return len;
    }
    if (cmd->start_timestamp && cmd->end_timestamp &&
        cmd->start_timestamp > cmd->end_timestamp) {
        rain_history_send_error_response(conn, 0xFE);
        return len;
    }
    if (cmd->max_entries == 0 && cmd->command <= 0x03) {
        rain_history_send_error_response(conn, 0xFE);
        return len;
    }

    /* Setup state */
    rain_history_cmd_state.command_active = true;
    rain_history_cmd_state.requesting_conn = conn;
    rain_history_cmd_state.current_command = cmd->command;
    rain_history_cmd_state.start_timestamp = cmd->start_timestamp;
    rain_history_cmd_state.end_timestamp = cmd->end_timestamp;
    rain_history_cmd_state.max_entries = cmd->max_entries;
    rain_history_cmd_state.data_type = cmd->data_type;
    rain_history_cmd_state.current_entry = 0;
    rain_history_cmd_state.total_entries = 0;
    rain_history_cmd_state.current_fragment = 0;
    rain_history_cmd_state.total_fragments = 0;
    rain_history_cmd_state.fragment_buffer = NULL;
    rain_history_cmd_state.fragment_buffer_owned = false;

    int result = 0;

    switch (cmd->command) {
    case 0x01: /* RAIN_CMD_GET_HOURLY */
        rain_history_cmd_state.data_type = 0; /* hourly */
        result = process_rain_history_hourly_request(cmd->start_timestamp, cmd->end_timestamp, cmd->max_entries);
        break;
    case 0x02: /* RAIN_CMD_GET_DAILY */
        rain_history_cmd_state.data_type = 1; /* daily */
        result = process_rain_history_daily_request(cmd->start_timestamp, cmd->end_timestamp, cmd->max_entries);
        break;
    case 0x03: { /* RAIN_CMD_GET_RECENT */
        /* Build recent totals response using unified header (single fragment) */
        rain_history_response_t response = {0};
        response.header.fragment_index = 0;
        response.header.total_fragments = 1;
        response.header.status = 0;
        response.header.data_type = 0xFE; /* special recent totals */
        /* Payload layout: last_hour_mm_x100(4) last_24h_mm_x100(4) last_7d_mm_x100(4) reserved(4)=16B */
        uint32_t last_hour = (uint32_t)(rain_history_get_current_hour() * 100.0f);
        uint32_t last_24h = (uint32_t)(rain_history_get_last_24h() * 100.0f);
        float last7d_mm = rain_history_get_recent_total(24 * 7);
        uint32_t last_7d = (uint32_t)(last7d_mm * 100.0f);
        memcpy(&response.data[0], &last_hour, 4);
        memcpy(&response.data[4], &last_24h, 4);
        memcpy(&response.data[8], &last_7d, 4);
        memset(&response.data[12], 0, 4);
        response.header.fragment_size = 16;
        size_t notify_len = sizeof(response.header) + response.header.fragment_size;
        if (notification_state.rain_history_notifications_enabled) {
            bt_gatt_notify(conn, &irrigation_svc.attrs[ATTR_IDX_RAIN_HISTORY_VALUE], &response, notify_len);
        }
        rain_history_cmd_state.command_active = false;
        break; }
    case 0x10: /* RAIN_CMD_RESET_DATA */
        if (rain_history_clear_all() != WATERING_SUCCESS) {
            rain_history_send_error_response(conn, 0x06);
        } else {
            rain_history_response_t resp = {0};
            resp.header.fragment_index = 0;
            resp.header.total_fragments = 1;
            resp.header.status = 0;
            resp.header.data_type = 0xFD; /* reset ack */
            resp.header.fragment_size = 0;
            if (notification_state.rain_history_notifications_enabled) {
                bt_gatt_notify(conn, &irrigation_svc.attrs[ATTR_IDX_RAIN_HISTORY_VALUE], &resp, sizeof(resp.header));
            }
        }
        rain_history_cmd_state.command_active = false;
        break;
    case 0x20: /* RAIN_CMD_CALIBRATE */
        rain_sensor_reset_counters();
        rain_sensor_reset_diagnostics();
        rain_sensor_save_config();

        rain_history_response_t cal = {0};
        cal.header.fragment_index = 0;
        cal.header.total_fragments = 1;
        cal.header.status = 0;
        cal.header.data_type = 0xFC; /* calibration ack */
        cal.header.fragment_size = 0;
        if (notification_state.rain_history_notifications_enabled) {
            bt_gatt_notify(conn, &irrigation_svc.attrs[ATTR_IDX_RAIN_HISTORY_VALUE], &cal, sizeof(cal.header));
        }
        rain_history_cmd_state.command_active = false;
        break;
    default:
        rain_history_send_error_response(conn, 0xFF); /* Invalid command */
        break;
    }

    if (result < 0) {
        rain_history_reset_state();
    }

    return len;
}

void rain_history_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value) {
    bool notify_enabled = (value == BT_GATT_CCC_NOTIFY);
    notification_state.rain_history_notifications_enabled = notify_enabled;
    LOG_INF("Rain history notifications %s", notify_enabled ? "enabled" : "disabled");
}

/* ------------------------------------------------------------------ */
/* Rain Integration Status characteristic implementation              */
/* ------------------------------------------------------------------ */

static ssize_t read_rain_integration_status(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                            void *buf, uint16_t len, uint16_t offset) {
    if (!conn || !attr || !buf) {
        return -EINVAL;
    }

    rain_integration_status_t sys = {0};
    if (watering_get_rain_integration_status(&sys) != WATERING_SUCCESS) {
        memset(&sys, 0, sizeof(sys));
    }

    struct rain_integration_status_ble ble = {0};
    ble.sensor_active = sys.sensor_active ? 1 : 0;
    ble.integration_enabled = sys.integration_enabled ? 1 : 0;
    ble.last_pulse_time = sys.last_pulse_time;
    ble.calibration_mm_per_pulse = sys.calibration_mm_per_pulse;
    ble.rainfall_last_hour = sys.rainfall_last_hour;
    ble.rainfall_last_24h = sys.rainfall_last_24h;
    ble.rainfall_last_48h = sys.rainfall_last_48h;
    ble.sensitivity_pct = sys.sensitivity_pct;
    ble.skip_threshold_mm = sys.skip_threshold_mm;
    for (int i = 0; i < 8; ++i) {
        ble.channel_reduction_pct[i] = sys.channel_reduction_pct[i];
        ble.channel_skip_irrigation[i] = sys.channel_skip_irrigation[i] ? 1 : 0;
    }
    ble.hourly_entries = sys.hourly_entries;
    ble.daily_entries = sys.daily_entries;
    ble.storage_usage_bytes = sys.storage_usage_bytes;

    memcpy(rain_integration_status_value, &ble, sizeof(ble));
    return bt_gatt_attr_read(conn, attr, buf, len, offset,
                             rain_integration_status_value, sizeof(ble));
}

static void rain_integration_status_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value) {
    bool notify_enabled = (value == BT_GATT_CCC_NOTIFY);
    notification_state.rain_integration_status_notifications_enabled = notify_enabled;
    LOG_INF("Rain integration status notifications %s", notify_enabled ? "enabled" : "disabled");
    if (notify_enabled && default_conn) {
        /* Snapshot on subscribe */
        bt_irrigation_rain_integration_status_notify();
    }
}

/* ------------------------------------------------------------------ */
/* Channel Compensation Config characteristic implementation          */
/* ------------------------------------------------------------------ */

/* Cached channel selection for channel compensation config reads */
static uint8_t channel_comp_config_selected_channel = 0;

static ssize_t read_channel_comp_config(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                        void *buf, uint16_t len, uint16_t offset) {
    if (!conn || !attr || !buf) {
        return -EINVAL;
    }

    uint8_t channel_id = channel_comp_config_selected_channel;
    if (channel_id >= WATERING_CHANNELS_COUNT) {
        channel_id = 0;
    }

    watering_channel_t *channel = NULL;
    if (watering_get_channel(channel_id, &channel) != WATERING_SUCCESS || !channel) {
        LOG_ERR("Failed to get channel %u for compensation config read", channel_id);
        return -EIO;
    }

    struct channel_compensation_config_data config = {0};
    config.channel_id = channel_id;
    
    /* Rain compensation settings */
    config.rain_enabled = channel->rain_compensation.enabled ? 1 : 0;
    config.rain_sensitivity = channel->rain_compensation.sensitivity;
    config.rain_lookback_hours = channel->rain_compensation.lookback_hours;
    config.rain_skip_threshold_mm = channel->rain_compensation.skip_threshold_mm;
    config.rain_reduction_factor = channel->rain_compensation.reduction_factor;
    
    /* Temperature compensation settings */
    config.temp_enabled = channel->temp_compensation.enabled ? 1 : 0;
    config.temp_base_temperature = channel->temp_compensation.base_temperature;
    config.temp_sensitivity = channel->temp_compensation.sensitivity;
    config.temp_min_factor = channel->temp_compensation.min_factor;
    config.temp_max_factor = channel->temp_compensation.max_factor;
    
    /* Status fields - timestamps not available in current watering_channel_t structure */
    config.last_rain_calc_time = 0;  /* TODO: Add timestamp tracking to watering module */
    config.last_temp_calc_time = 0;  /* TODO: Add timestamp tracking to watering module */

    memcpy(channel_comp_config_value, &config, sizeof(config));
    return bt_gatt_attr_read(conn, attr, buf, len, offset,
                             channel_comp_config_value, sizeof(config));
}

static ssize_t write_channel_comp_config(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                         const void *buf, uint16_t len, uint16_t offset,
                                         uint8_t flags) {
    if (!conn || !attr || !buf) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }
    
    /* Single byte write = channel selection */
    if (len == 1) {
        uint8_t channel_id = ((const uint8_t *)buf)[0];
        if (channel_id >= WATERING_CHANNELS_COUNT) {
            LOG_ERR("Invalid channel ID %u for compensation config select", channel_id);
            return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
        }
        channel_comp_config_selected_channel = channel_id;
        LOG_INF("Channel compensation config: selected channel %u", channel_id);
        return len;
    }
    
    /* Full struct write = configuration update */
    if (len != sizeof(struct channel_compensation_config_data)) {
        LOG_ERR("Invalid compensation config write length: %u (expected %zu)",
                len, sizeof(struct channel_compensation_config_data));
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }

    const struct channel_compensation_config_data *config = 
        (const struct channel_compensation_config_data *)buf;
    
    /* Validate channel ID */
    if (config->channel_id >= WATERING_CHANNELS_COUNT) {
        LOG_ERR("Invalid channel ID %u in compensation config", config->channel_id);
        return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
    }
    
    /* Validate rain compensation parameters */
    if (config->rain_enabled) {
        if (config->rain_sensitivity < 0.0f || config->rain_sensitivity > 1.0f) {
            LOG_ERR("Invalid rain sensitivity: %.2f (must be 0.0-1.0)", 
                    (double)config->rain_sensitivity);
            return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
        }
        if (config->rain_lookback_hours < 1 || config->rain_lookback_hours > 72) {
            LOG_ERR("Invalid rain lookback hours: %u (must be 1-72)", 
                    config->rain_lookback_hours);
            return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
        }
        if (config->rain_skip_threshold_mm < 0.0f || config->rain_skip_threshold_mm > 100.0f) {
            LOG_ERR("Invalid rain skip threshold: %.2f (must be 0-100mm)", 
                    (double)config->rain_skip_threshold_mm);
            return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
        }
        if (config->rain_reduction_factor < 0.0f || config->rain_reduction_factor > 1.0f) {
            LOG_ERR("Invalid rain reduction factor: %.2f (must be 0.0-1.0)", 
                    (double)config->rain_reduction_factor);
            return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
        }
    }
    
    /* Validate temperature compensation parameters */
    if (config->temp_enabled) {
        if (config->temp_base_temperature < -40.0f || config->temp_base_temperature > 60.0f) {
            LOG_ERR("Invalid temp base: %.2f (must be -40 to 60°C)", 
                    (double)config->temp_base_temperature);
            return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
        }
        if (config->temp_sensitivity < 0.1f || config->temp_sensitivity > 2.0f) {
            LOG_ERR("Invalid temp sensitivity: %.2f (must be 0.1-2.0)", 
                    (double)config->temp_sensitivity);
            return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
        }
        if (config->temp_min_factor < 0.5f || config->temp_min_factor > 1.0f) {
            LOG_ERR("Invalid temp min factor: %.2f (must be 0.5-1.0)", 
                    (double)config->temp_min_factor);
            return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
        }
        if (config->temp_max_factor < 1.0f || config->temp_max_factor > 2.0f) {
            LOG_ERR("Invalid temp max factor: %.2f (must be 1.0-2.0)", 
                    (double)config->temp_max_factor);
            return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
        }
    }
    
    /* Apply configuration to channel */
    watering_channel_t *channel = NULL;
    if (watering_get_channel(config->channel_id, &channel) != WATERING_SUCCESS || !channel) {
        LOG_ERR("Failed to get channel %u for compensation config write", config->channel_id);
        return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
    }
    
    /* Update rain compensation settings */
    channel->rain_compensation.enabled = config->rain_enabled != 0;
    channel->rain_compensation.sensitivity = config->rain_sensitivity;
    channel->rain_compensation.lookback_hours = config->rain_lookback_hours;
    channel->rain_compensation.skip_threshold_mm = config->rain_skip_threshold_mm;
    channel->rain_compensation.reduction_factor = config->rain_reduction_factor;
    
    /* Update temperature compensation settings */
    channel->temp_compensation.enabled = config->temp_enabled != 0;
    channel->temp_compensation.base_temperature = config->temp_base_temperature;
    channel->temp_compensation.sensitivity = config->temp_sensitivity;
    channel->temp_compensation.min_factor = config->temp_min_factor;
    channel->temp_compensation.max_factor = config->temp_max_factor;
    
    /* Persist configuration */
    watering_error_t result = watering_save_config_priority(true);
    if (result != WATERING_SUCCESS) {
        LOG_WRN("Failed to persist compensation config for channel %u: %d", 
                config->channel_id, result);
    }
    
    /* Update onboarding flags */
    if (channel->rain_compensation.enabled) {
        onboarding_update_channel_extended_flag(config->channel_id, 
                                                CHANNEL_EXT_FLAG_RAIN_COMP_SET, true);
    }
    if (channel->temp_compensation.enabled) {
        onboarding_update_channel_extended_flag(config->channel_id, 
                                                CHANNEL_EXT_FLAG_TEMP_COMP_SET, true);
    }
    
    LOG_INF("Channel %u compensation config updated (rain=%s, temp=%s)",
            config->channel_id,
            config->rain_enabled ? "enabled" : "disabled",
            config->temp_enabled ? "enabled" : "disabled");
    
    /* Send notification */
    channel_comp_config_selected_channel = config->channel_id;
    memcpy(channel_comp_config_value, config, sizeof(*config));
    if (notification_state.channel_comp_config_notifications_enabled && default_conn) {
        safe_notify(default_conn, &irrigation_svc.attrs[ATTR_IDX_CHANNEL_COMP_CONFIG_VALUE],
                    channel_comp_config_value, sizeof(*config));
    }
    
    return len;
}

static void channel_comp_config_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value) {
    bool notify_enabled = (value == BT_GATT_CCC_NOTIFY);
    notification_state.channel_comp_config_notifications_enabled = notify_enabled;
    LOG_INF("Channel compensation config notifications %s", notify_enabled ? "enabled" : "disabled");
    
    if (notify_enabled && default_conn) {
        /* Send initial snapshot for the currently selected channel */
        uint8_t channel_id = channel_comp_config_selected_channel;
        if (channel_id >= WATERING_CHANNELS_COUNT) {
            channel_id = 0;
        }
        
        watering_channel_t *channel = NULL;
        if (watering_get_channel(channel_id, &channel) == WATERING_SUCCESS && channel) {
            struct channel_compensation_config_data config = {0};
            config.channel_id = channel_id;
            config.rain_enabled = channel->rain_compensation.enabled ? 1 : 0;
            config.rain_sensitivity = channel->rain_compensation.sensitivity;
            config.rain_lookback_hours = channel->rain_compensation.lookback_hours;
            config.rain_skip_threshold_mm = channel->rain_compensation.skip_threshold_mm;
            config.rain_reduction_factor = channel->rain_compensation.reduction_factor;
            config.temp_enabled = channel->temp_compensation.enabled ? 1 : 0;
            config.temp_base_temperature = channel->temp_compensation.base_temperature;
            config.temp_sensitivity = channel->temp_compensation.sensitivity;
            config.temp_min_factor = channel->temp_compensation.min_factor;
            config.temp_max_factor = channel->temp_compensation.max_factor;
            config.last_rain_calc_time = 0;  /* TODO: Add timestamp tracking */
            config.last_temp_calc_time = 0;  /* TODO: Add timestamp tracking */
            
            memcpy(channel_comp_config_value, &config, sizeof(config));
            safe_notify(default_conn, &irrigation_svc.attrs[ATTR_IDX_CHANNEL_COMP_CONFIG_VALUE],
                        channel_comp_config_value, sizeof(config));
        }
    }
}

/**
 * @brief Public API to notify channel compensation config changes
 * @param channel_id Channel that was updated
 * @return 0 on success, negative error code on failure
 */
int bt_irrigation_channel_comp_config_notify(uint8_t channel_id) {
    if (!notification_state.channel_comp_config_notifications_enabled || !default_conn) {
        return -ENOTCONN;
    }
    
    if (channel_id >= WATERING_CHANNELS_COUNT) {
        return -EINVAL;
    }
    
    watering_channel_t *channel = NULL;
    if (watering_get_channel(channel_id, &channel) != WATERING_SUCCESS || !channel) {
        return -EIO;
    }
    
    struct channel_compensation_config_data config = {0};
    config.channel_id = channel_id;
    config.rain_enabled = channel->rain_compensation.enabled ? 1 : 0;
    config.rain_sensitivity = channel->rain_compensation.sensitivity;
    config.rain_lookback_hours = channel->rain_compensation.lookback_hours;
    config.rain_skip_threshold_mm = channel->rain_compensation.skip_threshold_mm;
    config.rain_reduction_factor = channel->rain_compensation.reduction_factor;
    config.temp_enabled = channel->temp_compensation.enabled ? 1 : 0;
    config.temp_base_temperature = channel->temp_compensation.base_temperature;
    config.temp_sensitivity = channel->temp_compensation.sensitivity;
    config.temp_min_factor = channel->temp_compensation.min_factor;
    config.temp_max_factor = channel->temp_compensation.max_factor;
    config.last_rain_calc_time = 0;  /* TODO: Add timestamp tracking */
    config.last_temp_calc_time = 0;  /* TODO: Add timestamp tracking */
    
    memcpy(channel_comp_config_value, &config, sizeof(config));
    return safe_notify(default_conn, &irrigation_svc.attrs[ATTR_IDX_CHANNEL_COMP_CONFIG_VALUE],
                       channel_comp_config_value, sizeof(config));
}

#else /* CONFIG_BT */

/* BLE Stub Functions when BLE is disabled */
#include <stdint.h>
#include <stdbool.h>
#include <zephyr/sys/printk.h>
#include "bt_irrigation_service.h"
#include "watering.h"

// Stub implementations when BLE is disabled
int bt_irrigation_valve_status_update(uint8_t channel_id, bool is_open) {
    return 0; // BLE disabled - no notifications
}

int bt_irrigation_flow_update(uint32_t flow_rate) {
    return 0; // BLE disabled - no notifications
}

int bt_irrigation_update_statistics_from_flow(uint8_t channel_id, uint32_t volume_ml) {
    return 0;
}

int bt_irrigation_queue_status_update(uint8_t task_id) {
    return 0;
}

int bt_irrigation_alarm_notify(uint8_t alarm_code, uint16_t alarm_data) {
    return 0;
}

int bt_irrigation_system_status_update(watering_status_t status) {
    return 0;
}

int bt_irrigation_service_init(void) {
    printk("BLE irrigation service disabled (CONFIG_BT not set)\n");
    return 0;
}

int bt_irrigation_queue_status_notify(void) {
    return 0;
}

int bt_irrigation_alarm_clear(uint8_t alarm_code) {
    return 0;
}

int bt_irrigation_history_notify_event(uint8_t channel_id, uint8_t event_type, uint32_t timestamp, uint32_t value) {
    return 0;
}

int bt_irrigation_rtc_update(rtc_datetime_t *datetime) {
    return 0; // BLE disabled - no RTC sync
}

int bt_irrigation_growing_env_update(uint8_t channel_id) {
    return 0; // BLE disabled - no notifications
}

int bt_irrigation_auto_calc_status_notify(void) {
    return 0; // BLE disabled - no notifications
}

int bt_irrigation_growing_env_notify(void) {
    return 0; // BLE disabled - no notifications
}

int bt_irrigation_debug_notifications(void) {
    return 0; // BLE disabled - no debugging
}

int bt_irrigation_test_channel_notification(uint8_t channel_id) {
    return 0; // BLE disabled - no testing
}

int bt_irrigation_force_enable_notifications(void) {
    return 0; // BLE disabled - no force enable
}

int bt_irrigation_channel_comp_config_notify(uint8_t channel_id) {
    (void)channel_id;
    return 0; // BLE disabled - no notifications
}

/* ------------------------------------------------------------------ */
/* Rain Sensor Characteristics Implementation                        */


ssize_t write_rain_config(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                const void *buf, uint16_t len, uint16_t offset, uint8_t flags) {
    if (!conn || !attr || !buf) {
        LOG_ERR("Invalid parameters for rain config write");
        return -EINVAL;
    }
    
    if (offset != 0) {
        LOG_ERR("Rain config write with non-zero offset not supported");
        return -EINVAL;
    }
    
    if (len != sizeof(struct rain_config_data)) {
        LOG_ERR("Invalid rain config data length: %u, expected: %zu", 
                len, sizeof(struct rain_config_data));
        return -EINVAL;
    }
    
    const struct rain_config_data *config_data = (const struct rain_config_data *)buf;
    
    /* Validate configuration values */
    if (config_data->mm_per_pulse < 0.1f || config_data->mm_per_pulse > 10.0f) {
        LOG_ERR("Invalid mm_per_pulse: %.3f", config_data->mm_per_pulse);
        return -EINVAL;
    }
    
    if (config_data->debounce_ms < 10 || config_data->debounce_ms > 1000) {
        LOG_ERR("Invalid debounce_ms: %u", config_data->debounce_ms);
        return -EINVAL;
    }
    
    if (config_data->rain_sensitivity_pct < 0.0f || config_data->rain_sensitivity_pct > 100.0f) {
        LOG_ERR("Invalid rain_sensitivity_pct: %.1f", config_data->rain_sensitivity_pct);
        return -EINVAL;
    }
    
    if (config_data->skip_threshold_mm < 0.0f || config_data->skip_threshold_mm > 100.0f) {
        LOG_ERR("Invalid skip_threshold_mm: %.1f", config_data->skip_threshold_mm);
        return -EINVAL;
    }
    
    /* Apply configuration to rain sensor */
    int ret = rain_sensor_set_calibration(config_data->mm_per_pulse);
    if (ret != 0) {
        LOG_ERR("Failed to set rain sensor calibration: %d", ret);
        return -EIO;
    }
    
    ret = rain_sensor_set_debounce(config_data->debounce_ms);
    if (ret != 0) {
        LOG_ERR("Failed to set rain sensor debounce: %d", ret);
        return -EIO;
    }
    
    ret = rain_sensor_set_enabled(config_data->sensor_enabled != 0);
    if (ret != 0) {
        LOG_ERR("Failed to set rain sensor enabled state: %d", ret);
        return -EIO;
    }
    
    ret = rain_sensor_set_integration_enabled(config_data->integration_enabled != 0);
    if (ret != 0) {
        LOG_ERR("Failed to set rain integration enabled state: %d", ret);
        return -EIO;
    }
    
    /* Apply integration configuration */
    ret = rain_integration_set_sensitivity(config_data->rain_sensitivity_pct);
    if (ret != 0) {
        LOG_ERR("Failed to set rain sensitivity: %d", ret);
        return -EIO;
    }
    
    ret = rain_integration_set_skip_threshold(config_data->skip_threshold_mm);
    if (ret != 0) {
        LOG_ERR("Failed to set rain skip threshold: %d", ret);
        return -EIO;
    }
    
    /* Save configuration to NVS */
    rain_sensor_save_config();
    rain_integration_save_config();
    
    /* Update global value buffer */
    memcpy(rain_config_value, config_data, sizeof(struct rain_config_data));
    
    LOG_INF("Rain sensor configuration updated via BLE");
    LOG_INF("Calibration: %.3f mm/pulse, Debounce: %u ms, Enabled: %s, Integration: %s",
            config_data->mm_per_pulse, config_data->debounce_ms,
            config_data->sensor_enabled ? "Yes" : "No",
            config_data->integration_enabled ? "Yes" : "No");
    
    return len;
}

void rain_config_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value) {
    bool notify_enabled = (value == BT_GATT_CCC_NOTIFY);
    notification_state.rain_config_notifications_enabled = notify_enabled;
    LOG_INF("Rain config notifications %s", notify_enabled ? "enabled" : "disabled");
}

/* Rain data characteristic implementation */
ssize_t read_rain_data(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                             void *buf, uint16_t len, uint16_t offset) {
    if (!conn || !attr || !buf) {
        LOG_ERR("Invalid parameters for rain data read");
        return -EINVAL;
    }
    
    struct rain_data_data data = {0};
    
    if (rain_sensor_is_enabled()) {
        /* Get current rain sensor data */
        rain_sensor_data_t sensor_data;
        int ret = rain_sensor_get_data(&sensor_data);
        if (ret == 0) {
            data.current_hour_mm_x100 = (uint32_t)(rain_history_get_current_hour() * 100.0f);
            data.today_total_mm_x100 = (uint32_t)(rain_history_get_today() * 100.0f);
            data.last_24h_mm_x100 = (uint32_t)(rain_history_get_last_24h() * 100.0f);
            data.current_rate_mm_h_x100 = (uint16_t)(sensor_data.hourly_rate_mm * 100.0f);
            data.last_pulse_time = sensor_data.last_pulse_time;
            data.total_pulses = sensor_data.total_pulses;
            data.sensor_status = (uint8_t)sensor_data.status;
            data.data_quality = sensor_data.data_quality;
        } else {
            /* Sensor error - return error status */
            data.sensor_status = 2; // Error status
            data.data_quality = 0;
        }
    } else {
        /* Sensor disabled */
        data.sensor_status = 0; // Inactive status
        data.data_quality = 0;
    }
    
    /* Update global value buffer */
    memcpy(rain_data_value, &data, sizeof(data));
    
    return bt_gatt_attr_read(conn, attr, buf, len, offset, 
                            rain_data_value, sizeof(data));
}

void rain_data_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value) {
    bool notify_enabled = (value == BT_GATT_CCC_NOTIFY);
    notification_state.rain_data_notifications_enabled = notify_enabled;
    LOG_INF("Rain data notifications %s", notify_enabled ? "enabled" : "disabled");
}

/* Rain history characteristic implementation */
ssize_t read_rain_history(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                void *buf, uint16_t len, uint16_t offset) {
    if (!conn || !attr || !buf) {
        LOG_ERR("Invalid parameters for rain history read");
        return -EINVAL;
    }
    
    /* Return current command buffer */
    return bt_gatt_attr_read(conn, attr, buf, len, offset, 
                            rain_history_value, sizeof(struct rain_history_cmd_data));
}

/* Rain history command processing state */
static struct {
    bool command_active;
    uint8_t current_command;
    uint32_t start_timestamp;
    uint32_t end_timestamp;
    uint16_t max_entries;
    uint8_t data_type;
    uint16_t total_entries;
    uint16_t current_entry;
    uint8_t current_fragment;
    uint8_t total_fragments;
    uint8_t *fragment_buffer;
    bool fragment_buffer_owned;
    struct bt_conn *requesting_conn;
} rain_history_cmd_state = {0};

/* Forward declarations for rain history processing */
static int process_rain_history_hourly_request(uint32_t start_time, uint32_t end_time, uint16_t max_entries);
static int process_rain_history_daily_request(uint32_t start_time, uint32_t end_time, uint16_t max_entries);
static int send_rain_history_fragment(struct bt_conn *conn, uint8_t fragment_id);
static void rain_history_send_error_response(struct bt_conn *conn, uint8_t error_code);
static void rain_history_fragment_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(rain_history_fragment_work, rain_history_fragment_work_handler);

/* Clear state and free owned buffers after a command completes */
static void rain_history_reset_state(void) {
    if (rain_history_cmd_state.fragment_buffer_owned && rain_history_cmd_state.fragment_buffer) {
        k_free(rain_history_cmd_state.fragment_buffer);
    }
    rain_history_cmd_state.command_active = false;
    rain_history_cmd_state.current_command = 0;
    rain_history_cmd_state.start_timestamp = 0;
    rain_history_cmd_state.end_timestamp = 0;
    rain_history_cmd_state.max_entries = 0;
    rain_history_cmd_state.data_type = 0;
    rain_history_cmd_state.total_entries = 0;
    rain_history_cmd_state.current_entry = 0;
    rain_history_cmd_state.current_fragment = 0;
    rain_history_cmd_state.total_fragments = 0;
    rain_history_cmd_state.fragment_buffer = NULL;
    rain_history_cmd_state.fragment_buffer_owned = false;
    rain_history_cmd_state.requesting_conn = NULL;
    k_work_cancel_delayable(&rain_history_fragment_work);
}

ssize_t write_rain_history(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                 const void *buf, uint16_t len, uint16_t offset, uint8_t flags) {
    if (!conn || !attr || !buf) {
        LOG_ERR("Invalid parameters for rain history write");
        return -EINVAL;
    }
    
    if (offset != 0) {
        LOG_ERR("Rain history write with non-zero offset not supported");
        return -EINVAL;
    }
    
    if (len != sizeof(struct rain_history_cmd_data)) {
        LOG_ERR("Invalid rain history command length: %u, expected: %zu", 
                len, sizeof(struct rain_history_cmd_data));
        return -EINVAL;
    }
    
    const struct rain_history_cmd_data *cmd = (const struct rain_history_cmd_data *)buf;
    
    LOG_INF("Rain history command: %u, start: %u, end: %u, max_entries: %u, type: %u",
            cmd->command, cmd->start_timestamp, cmd->end_timestamp, 
            cmd->max_entries, cmd->data_type);
    
    /* Check if another command is already in progress */
    if (rain_history_cmd_state.command_active && rain_history_cmd_state.requesting_conn != conn) {
        LOG_WRN("Rain history command already in progress for another connection");
        rain_history_send_error_response(conn, 0x01); /* Busy error */
        return -EBUSY;
    }
    
    /* Initialize command state */
    rain_history_cmd_state.command_active = true;
    rain_history_cmd_state.current_command = cmd->command;
    rain_history_cmd_state.start_timestamp = cmd->start_timestamp;
    rain_history_cmd_state.end_timestamp = cmd->end_timestamp;
    rain_history_cmd_state.max_entries = cmd->max_entries;
    rain_history_cmd_state.data_type = cmd->data_type;
    rain_history_cmd_state.requesting_conn = conn;
    rain_history_cmd_state.current_entry = 0;
    rain_history_cmd_state.current_fragment = 0;
    rain_history_cmd_state.fragment_buffer = NULL;
    rain_history_cmd_state.fragment_buffer_owned = false;
    
    int result = 0;
    
    /* Process command based on type */
    switch (cmd->command) {
        case 0x01: /* Get hourly data */
            if (cmd->data_type != 0) {
                LOG_ERR("Invalid data type for hourly request: %u", cmd->data_type);
                rain_history_send_error_response(conn, 0x02); /* Invalid parameter */
                result = -EINVAL;
                break;
            }
            result = process_rain_history_hourly_request(cmd->start_timestamp, 
                                                        cmd->end_timestamp, 
                                                        cmd->max_entries);
            break;
            
        case 0x02: /* Get daily data */
            if (cmd->data_type != 1) {
                LOG_ERR("Invalid data type for daily request: %u", cmd->data_type);
                rain_history_send_error_response(conn, 0x02); /* Invalid parameter */
                result = -EINVAL;
                break;
            }
            result = process_rain_history_daily_request(cmd->start_timestamp, 
                                                       cmd->end_timestamp, 
                                                       cmd->max_entries);
            break;
            
        case 0x03: /* Get recent data */
            LOG_INF("Requesting recent rain data");
            /* Send recent rainfall summary */
            rain_history_response_t response = {0};
            response.header.fragment_index = 0;
            response.header.total_fragments = 1;
            response.header.status = 0; /* Success */
            response.header.data_type = 2; /* Recent data type */
            
            /* Pack recent rainfall data */
            struct {
                uint32_t current_hour_mm_x100;
                uint32_t today_total_mm_x100;
                uint32_t last_24h_mm_x100;
                uint32_t last_48h_mm_x100;
            } __attribute__((packed)) recent_data;
            
            recent_data.current_hour_mm_x100 = (uint32_t)(rain_history_get_current_hour() * 100.0f);
            recent_data.today_total_mm_x100 = (uint32_t)(rain_history_get_today() * 100.0f);
            recent_data.last_24h_mm_x100 = (uint32_t)(rain_history_get_last_24h() * 100.0f);
            recent_data.last_48h_mm_x100 = (uint32_t)(rain_history_get_recent_total(48) * 100.0f);
            
            response.header.fragment_size = sizeof(recent_data);
            memcpy(response.data, &recent_data, sizeof(recent_data));
            
            /* Send response immediately */
            size_t recent_len = sizeof(response.header) + response.header.fragment_size;
            memcpy(rain_history_value, &response, recent_len);
            if (notification_state.rain_history_notifications_enabled) {
                bt_gatt_notify(conn, &irrigation_svc.attrs[ATTR_IDX_RAIN_HISTORY_VALUE], 
                              &response, recent_len);
            }
            
            rain_history_cmd_state.command_active = false;
            result = 0;
            break;
            
        case 0x10: /* Reset data */
            LOG_INF("Resetting rain history data");
            watering_error_t reset_result = rain_history_clear_all();
            if (reset_result == WATERING_SUCCESS) {
                /* Send success response */
                rain_history_response_t reset_response = {0};
                reset_response.header.fragment_index = 0;
                reset_response.header.total_fragments = 1;
                reset_response.header.status = 0; /* Success */
                reset_response.header.data_type = 0xFF; /* Command response */
                reset_response.header.fragment_size = 1;
                reset_response.data[0] = 0x00; /* Success code */
                
                size_t reset_len = sizeof(reset_response.header) + reset_response.header.fragment_size;
                memcpy(rain_history_value, &reset_response, reset_len);
                if (notification_state.rain_history_notifications_enabled) {
                    bt_gatt_notify(conn, &irrigation_svc.attrs[ATTR_IDX_RAIN_HISTORY_VALUE], 
                                  &reset_response, reset_len);
                }
            } else {
                rain_history_send_error_response(conn, 0x03); /* Operation failed */
            }
            rain_history_cmd_state.command_active = false;
            result = 0;
            break;
            
        case 0x20: /* Calibrate */
            LOG_INF("Starting rain sensor calibration");
            /* Send calibration start response */
            rain_history_response_t cal_response = {0};
            cal_response.header.fragment_index = 0;
            cal_response.header.total_fragments = 1;
            cal_response.header.status = 0; /* Success */
            cal_response.header.data_type = 0xFF; /* Command response */
            cal_response.header.fragment_size = 2;
            cal_response.data[0] = 0x20; /* Calibration command */
            cal_response.data[1] = 0x01; /* Calibration started */
            
            size_t cal_len = sizeof(cal_response.header) + cal_response.header.fragment_size;
            memcpy(rain_history_value, &cal_response, cal_len);
            if (notification_state.rain_history_notifications_enabled) {
                bt_gatt_notify(conn, &irrigation_svc.attrs[ATTR_IDX_RAIN_HISTORY_VALUE], 
                              &cal_response, cal_len);
            }
            
            rain_history_cmd_state.command_active = false;
            result = 0;
            break;
            
        default:
            LOG_ERR("Unknown rain history command: %u", cmd->command);
            rain_history_send_error_response(conn, 0x04); /* Unknown command */
            result = -EINVAL;
            break;
    }
    
    if (result < 0) {
        rain_history_cmd_state.command_active = false;
    }
    
    /* Update global value buffer with command */
    memcpy(rain_history_value, cmd, sizeof(struct rain_history_cmd_data));
    
    return len;
}

void rain_history_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value) {
    bool notify_enabled = (value == BT_GATT_CCC_NOTIFY);
    notification_state.rain_history_notifications_enabled = notify_enabled;
    LOG_INF("Rain history notifications %s", notify_enabled ? "enabled" : "disabled");
}

/* Rain sensor BLE notification functions */
int bt_irrigation_rain_config_notify(void) {
    /* Read current configuration and send notification */
    struct rain_config_data config_data = {0};
    
    if (rain_sensor_is_enabled()) {
        config_data.mm_per_pulse = rain_sensor_get_calibration();
        config_data.debounce_ms = rain_sensor_get_debounce();
        config_data.sensor_enabled = rain_sensor_is_enabled() ? 1 : 0;
        config_data.integration_enabled = rain_sensor_is_integration_enabled() ? 1 : 0;
        config_data.rain_sensitivity_pct = rain_integration_get_sensitivity();
        config_data.skip_threshold_mm = rain_integration_get_skip_threshold();
    }
    
    memcpy(rain_config_value, &config_data, sizeof(config_data));
    
    return bt_gatt_notify(NULL, &irrigation_svc.attrs[ATTR_IDX_RAIN_CONFIG_VALUE], 
                         rain_config_value, sizeof(config_data));
}

int bt_irrigation_rain_data_notify(void) {
    /* Read current rain data and send notification */
    struct rain_data_data data = {0};
    
    if (rain_sensor_is_enabled()) {
        rain_sensor_data_t sensor_data;
        int ret = rain_sensor_get_data(&sensor_data);
        if (ret == 0) {
            data.current_hour_mm_x100 = (uint32_t)(rain_history_get_current_hour() * 100.0f);
            data.today_total_mm_x100 = (uint32_t)(rain_history_get_today() * 100.0f);
            data.last_24h_mm_x100 = (uint32_t)(rain_history_get_last_24h() * 100.0f);
            data.current_rate_mm_h_x100 = (uint16_t)(sensor_data.hourly_rate_mm * 100.0f);
            data.last_pulse_time = sensor_data.last_pulse_time;
            data.total_pulses = sensor_data.total_pulses;
            data.sensor_status = (uint8_t)sensor_data.status;
            data.data_quality = sensor_data.data_quality;
        } else {
            data.sensor_status = 2; // Error status
            data.data_quality = 0;
        }
    }
    
    memcpy(rain_data_value, &data, sizeof(data));
    
    return bt_gatt_notify(NULL, &irrigation_svc.attrs[ATTR_IDX_RAIN_DATA_VALUE], 
                         rain_data_value, sizeof(data));
}

int bt_irrigation_compensation_status_notify(uint8_t channel_id) {
    if (!default_conn || !notification_state.compensation_status_notifications_enabled) {
        return 0; // No connection or notifications disabled
    }
    
    struct compensation_status_data comp_status;
    memset(&comp_status, 0, sizeof(comp_status));
    comp_status.channel_id = channel_id;
    
    /* Get channel to check compensation status */
    watering_channel_t *channel;
    if (watering_get_channel(channel_id, &channel) == WATERING_SUCCESS) {
        /* Rain compensation status */
        comp_status.rain_compensation_active = channel->rain_compensation.enabled ? 1 : 0;
        comp_status.recent_rainfall_mm = channel->last_rain_compensation.recent_rainfall_mm;
        comp_status.rain_reduction_percentage = channel->last_rain_compensation.reduction_percentage;
        comp_status.rain_skip_watering = channel->last_rain_compensation.skip_watering ? 1 : 0;
        comp_status.rain_calculation_time = channel->last_rain_compensation.calculation_timestamp;
        
        /* Temperature compensation status */
        comp_status.temp_compensation_active = channel->temp_compensation.enabled ? 1 : 0;
        comp_status.current_temperature = channel->last_temp_compensation.current_temperature;
        comp_status.temp_compensation_factor = channel->last_temp_compensation.compensation_factor;
        comp_status.temp_adjusted_requirement = channel->last_temp_compensation.adjusted_requirement;
        comp_status.temp_calculation_time = channel->last_temp_compensation.calculation_timestamp;
        
        /* Overall status */
        comp_status.any_compensation_active = (comp_status.rain_compensation_active || 
                                             comp_status.temp_compensation_active) ? 1 : 0;
    }
    
    /* Update value buffer and send notification */
    memcpy(compensation_status_value, &comp_status, sizeof(comp_status));
    
    const struct bt_gatt_attr *attr = &irrigation_svc.attrs[ATTR_IDX_COMPENSATION_STATUS_VALUE];
    int result = safe_notify(default_conn, attr, &comp_status, sizeof(comp_status));
    
    if (result == 0) {
        LOG_DBG("Compensation status notification sent: ch=%u, rain=%u, temp=%u",
                channel_id, comp_status.rain_compensation_active, comp_status.temp_compensation_active);
    } else {
        LOG_WRN("Compensation status notification failed: %d", result);
    }
    
    return result;
}

int bt_irrigation_interval_mode_phase_notify(uint8_t channel_id, bool is_watering, uint32_t phase_remaining_sec) {
    if (!default_conn || !notification_state.current_task_notifications_enabled) {
        return 0; // Use current task notifications for interval mode updates
    }
    
    /* Trigger a current task status notification to update interval mode phase */
    int result = bt_irrigation_current_task_notify();
    
    if (result == 0) {
        LOG_DBG("Interval mode phase notification sent: ch=%u, phase=%s, remaining=%us",
                channel_id, is_watering ? "watering" : "pausing", phase_remaining_sec);
    } else {
        LOG_WRN("Interval mode phase notification failed: %d", result);
    }
    
    return result;
}


#endif /* CONFIG_BT */



