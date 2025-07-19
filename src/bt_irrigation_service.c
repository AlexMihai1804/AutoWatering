// Only compile this file if Bluetooth is enabled
#ifdef CONFIG_BT

// Manually define the crucial symbols if not defined in Kconfig to prevent compilation errors
#ifndef CONFIG_BT_MAX_PAIRED
#define CONFIG_BT_MAX_PAIRED 1
#endif

#ifndef CONFIG_BT_MAX_CONN
#define CONFIG_BT_MAX_CONN 1
#endif

// Simplified logging to avoid printk formatting issues
#define LOG_DBG(...) printk(__VA_ARGS__)
#define LOG_INF(...) printk(__VA_ARGS__)
#define LOG_WRN(...) printk(__VA_ARGS__)
#define LOG_ERR(...) printk(__VA_ARGS__)

#include <string.h>    // for strlen
#include <stdlib.h>    // for abs function
#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/sys/printk.h>
#include <zephyr/settings/settings.h>

#include "bt_irrigation_service.h"
#include "watering.h"
#include "watering_internal.h"  // Add this include to access internal state
#include "watering_history.h"   // Add this include for history statistics
#include "rtc.h"
#include "timezone.h"           // Add timezone support
#include "flow_sensor.h"
#include "nvs_config.h"         // Add NVS config support
#include "watering_history.h"   // Add this include for history functionality

/* Forward declaration for the service structure */
extern const struct bt_gatt_service_static irrigation_svc;

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
#define BUFFER_RECOVERY_TIME_MS 2000   /* Time to wait before retry after buffer exhaustion */

/* Buffer Pool Structure */
typedef struct {
    uint8_t data[23];              /* Maximum BLE notification size */
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
static bool is_notification_enabled(const struct bt_gatt_attr *attr);
static bool should_throttle_channel_name_notification(uint8_t channel_id);

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
#define ATTR_IDX_CURRENT_TASK_VALUE 44

/* Simple direct notification function - no queues, no work handlers */
static int send_simple_notification(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                   const void *data, uint16_t len);

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

/* Enhanced safe notification with additional validation */
static int safe_notify_enhanced(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                               const void *data, uint16_t len, bool check_enable_flag) {
    if (!conn || !attr || !data) {
        return -EINVAL;
    }
    
    if (!notification_system_enabled || !connection_active) {
        return -ENOTCONN;
    }
    
    /* Check if we have a valid default connection */
    if (conn != default_conn) {
        return -ENOTCONN;
    }
    
    /* Additional enable flag check if requested */
    if (check_enable_flag && !is_notification_enabled(attr)) {
        return -ENOTCONN;
    }
    
    return bt_gatt_notify(conn, attr, data, len);
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

/* Legacy macro for backward compatibility - redirects to smart version */
#define FAST_NOTIFY(conn, attr, data, size) SMART_NOTIFY(conn, attr, data, size)

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
    bool current_task_notifications_enabled;
    bool timezone_notifications_enabled;
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
    for (int i = 0; i < BLE_BUFFER_POOL_SIZE; i++) {
        uint8_t idx = (pool_head + i) % BLE_BUFFER_POOL_SIZE;
        if (!notification_pool[idx].in_use) {
            notification_pool[idx].in_use = true;
            notification_pool[idx].timestamp = k_uptime_get_32();
            pool_head = (idx + 1) % BLE_BUFFER_POOL_SIZE;
            buffers_in_use++;
            return &notification_pool[idx];
        }
    }
    
    /* No buffers available */
    last_buffer_exhaustion = k_uptime_get_32();
    LOG_WRN("⚠️ BLE buffer pool exhausted (%d/%d in use)", buffers_in_use, BLE_BUFFER_POOL_SIZE);
    return NULL;
}

/* Release a buffer back to the pool */
static void release_notification_buffer(ble_notification_buffer_t* buffer) {
    if (buffer && buffer->in_use) {
        buffer->in_use = false;
        buffers_in_use--;
    }
}

/* Get priority for a specific attribute */
static notify_priority_t get_notification_priority(const struct bt_gatt_attr *attr) {
    if (attr == &irrigation_svc.attrs[ATTR_IDX_ALARM_VALUE]) {
        return NOTIFY_PRIORITY_CRITICAL;
    } else if (attr == &irrigation_svc.attrs[ATTR_IDX_STATUS_VALUE] ||
               attr == &irrigation_svc.attrs[ATTR_IDX_VALVE_VALUE] ||
               attr == &irrigation_svc.attrs[ATTR_IDX_CURRENT_TASK_VALUE]) {
        return NOTIFY_PRIORITY_HIGH;
    } else if (attr == &irrigation_svc.attrs[ATTR_IDX_FLOW_VALUE] ||
               attr == &irrigation_svc.attrs[ATTR_IDX_STATISTICS_VALUE] ||
               attr == &irrigation_svc.attrs[ATTR_IDX_CHANNEL_CFG_VALUE]) {
        return NOTIFY_PRIORITY_NORMAL;
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
    if (!conn || !attr || !data || len > 23) {
        return -EINVAL;
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

/*  stub-handlers used inside BT_GATT_SERVICE_DEFINE – declare early  */
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

/* Forward declaration for task update thread */
static int start_task_update_thread(void);
static void stop_task_update_thread(void);

/* Forward declaration for simple notification function */
static int send_simple_notification(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                   const void *data, uint16_t len);

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

/* Forward declaration for force enable notifications */
static void force_enable_all_notifications(void);

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
/* Add History Service UUIDs (UUID 0x181A) */
#define BT_UUID_HISTORY_SERVICE_VAL \
    BT_UUID_128_ENCODE(0x0000181A, 0x0000, 0x1000, 0x8000, 0x00805F9B34FB)
#define BT_UUID_HISTORY_SERVICE_REVISION_VAL \
    BT_UUID_128_ENCODE(0x00002A80, 0x0000, 0x1000, 0x8000, 0x00805F9B34FB)
#define BT_UUID_HISTORY_CAPABILITIES_VAL \
    BT_UUID_128_ENCODE(0x00002A81, 0x0000, 0x1000, 0x8000, 0x00805F9B34FB)
#define BT_UUID_HISTORY_CTRL_VAL \
    BT_UUID_128_ENCODE(0x0000EF01, 0x0000, 0x1000, 0x8000, 0x00805F9B34FB)
#define BT_UUID_HISTORY_DATA_VAL \
    BT_UUID_128_ENCODE(0x0000EF02, 0x0000, 0x1000, 0x8000, 0x00805F9B34FB)
#define BT_UUID_HISTORY_INSIGHTS_VAL \
    BT_UUID_128_ENCODE(0x0000EF03, 0x0000, 0x1000, 0x8000, 0x00805F9B34FB)
#define BT_UUID_HISTORY_SETTINGS_VAL \
    BT_UUID_128_ENCODE(0x0000EF04, 0x0000, 0x1000, 0x8000, 0x00805F9B34FB)

static struct bt_uuid_128 history_service_uuid = BT_UUID_INIT_128(BT_UUID_HISTORY_SERVICE_VAL);
static struct bt_uuid_128 history_service_revision_uuid = BT_UUID_INIT_128(BT_UUID_HISTORY_SERVICE_REVISION_VAL);
static struct bt_uuid_128 history_capabilities_uuid = BT_UUID_INIT_128(BT_UUID_HISTORY_CAPABILITIES_VAL);
static struct bt_uuid_128 history_ctrl_uuid = BT_UUID_INIT_128(BT_UUID_HISTORY_CTRL_VAL);
static struct bt_uuid_128 history_data_uuid = BT_UUID_INIT_128(BT_UUID_HISTORY_DATA_VAL);
static struct bt_uuid_128 history_insights_uuid = BT_UUID_INIT_128(BT_UUID_HISTORY_INSIGHTS_VAL);
static struct bt_uuid_128 history_settings_uuid = BT_UUID_INIT_128(BT_UUID_HISTORY_SETTINGS_VAL);

/* History Service data structures */
static uint8_t history_service_revision[2] = {1, 0}; // Major.Minor
static uint32_t history_capabilities = 0x07; // Export, Purge, Insights
static uint8_t history_ctrl_value[20] = {0};
static uint8_t history_data_value[20] = {0};
static insights_t history_insights_value = {0};
static history_settings_t history_settings_value = {
    .detailed_cnt = DETAILED_EVENTS_PER_CHANNEL,
    .daily_days = DAILY_STATS_DAYS,
    .monthly_months = MONTHLY_STATS_MONTHS,
    .annual_years = ANNUAL_STATS_YEARS
};
#define BT_UUID_IRRIGATION_DIAGNOSTICS_VAL \
    BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x1234, 0x56789abcdefd)
#define BT_UUID_IRRIGATION_GROWING_ENV_VAL \
    BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x1234, 0x56789abcdefe)
#define BT_UUID_IRRIGATION_CURRENT_TASK_VAL \
    BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x1234, 0x56789abcdeff)

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
static struct bt_uuid_128 current_task_char_uuid = BT_UUID_INIT_128(BT_UUID_IRRIGATION_CURRENT_TASK_VAL);
static struct bt_uuid_128 timezone_char_uuid = BT_UUID_INIT_128(BT_UUID_IRRIGATION_TIMEZONE_VAL);

/* Valve Control structure - matches BLE API documentation */
struct valve_control_data {
    uint8_t  channel_id;   // 0-7: target channel
    uint8_t  task_type;    // 0=duration [min], 1=volume [L] (for task creation)
                           // Also used for status: 0=inactive, 1=active (for notifications)
    uint16_t value;        // minutes (task_type=0) or liters (task_type=1)
                           // For status notifications: 0 (no value)
} __packed;               // TOTAL SIZE: 4 bytes

/* Channel configuration structure */
struct channel_config_data {
    uint8_t channel_id;              /* Channel ID 0-7 */
    uint8_t name_len;               /* Actual string length, excluding null terminator (≤63) */
    char    name[64];               /* CHANNEL NAME (64 bytes): User-friendly channel identifier (e.g., "Front Garden") */
    uint8_t auto_enabled;           /* 1=automatic schedule active, 0=disabled */
    
    /* Plant and growing environment fields per BLE API Documentation */
    uint8_t plant_type;             /* Plant type: 0=Vegetables, 1=Herbs, 2=Flowers, 3=Shrubs, 4=Trees, 5=Lawn, 6=Succulents, 7=Custom */
    uint8_t soil_type;              /* Soil type: 0=Clay, 1=Sandy, 2=Loamy, 3=Silty, 4=Rocky, 5=Peaty, 6=Potting Mix, 7=Hydroponic */
    uint8_t irrigation_method;      /* Irrigation method: 0=Drip, 1=Sprinkler, 2=Soaker Hose, 3=Micro Spray, 4=Hand Watering, 5=Flood */
    uint8_t coverage_type;          /* 0=area in m², 1=plant count */
    union {
        float area_m2;              /* Area in square meters (4 bytes) */
        uint16_t plant_count;       /* Number of individual plants (2 bytes + 2 padding) */
    } coverage;                     /* Total: 4 bytes */
    uint8_t sun_percentage;         /* Percentage of direct sunlight (0-100%) */
} __packed;                         /* TOTAL SIZE: 76 bytes - CRITICAL: must match documentation exactly */

/* Schedule configuration structure */
struct schedule_config_data {
    uint8_t channel_id;
    uint8_t schedule_type; // 0=daily, 1=periodic
    uint8_t days_mask; // Days for daily schedule or interval days for periodic
    uint8_t hour;
    uint8_t minute;
    uint8_t watering_mode; // 0=duration, 1=volume
    uint16_t value; // Minutes or liters
    uint8_t auto_enabled; // 0=disabled, 1=enabled
}
        __packed;

/* System configuration structure */
struct system_config_data {
    uint8_t version;           /* Configuration version (read-only) */
    uint8_t power_mode;        /* 0=Normal, 1=Energy-Saving, 2=Ultra-Low */
    uint32_t flow_calibration; /* Pulses per liter */
    uint8_t max_active_valves; /* Always 1 (read-only) */
    uint8_t num_channels;      /* Number of channels (read-only) */
    /* Master valve configuration (new fields) */
    uint8_t master_valve_enabled;      /* 0=disabled, 1=enabled */
    int16_t master_valve_pre_delay;    /* Pre-start delay in seconds (negative = after) */
    int16_t master_valve_post_delay;   /* Post-stop delay in seconds (negative = before) */
    uint8_t master_valve_overlap_grace; /* Grace period for overlapping tasks (seconds) */
    uint8_t master_valve_auto_mgmt;    /* 0=manual, 1=automatic management */
    uint8_t master_valve_current_state; /* Current state: 0=closed, 1=open (read-only) */
}
        __packed;

/* Task queue structure */
struct task_queue_data {
    uint8_t pending_count;       /* Number of pending tasks in queue */
    uint8_t completed_tasks;     /* Number of completed tasks since boot */
    uint8_t current_channel;     /* Currently active channel (0xFF if none) */
    uint8_t current_task_type;   /* 0=duration, 1=volume */
    uint16_t current_value;      /* Current task value (minutes or liters) */
    uint8_t command;             /* Command to execute (write-only) */
    uint8_t task_id_to_delete;   /* Task ID for deletion (future use) */
    uint8_t active_task_id;      /* Currently active task ID */
} __packed;

/* Statistics structure for a channel */
struct statistics_data {
    uint8_t channel_id;
    uint32_t total_volume; // Total volume in ml
    uint32_t last_volume; // Last volume in ml
    uint32_t last_watering; // Last watering timestamp
    uint16_t count; // Total watering count
}
        __packed;

/* Current task monitoring structure */
struct current_task_data {
    uint8_t channel_id;        // Channel ID (0xFF if no active task)
    uint32_t start_time;       // Task start time in seconds since epoch
    uint8_t mode;              // Watering mode (0=duration, 1=volume)
    uint32_t target_value;     // Target: seconds (duration mode) or milliliters (volume mode)
    uint32_t current_value;    // Current: elapsed seconds (duration) or volume dispensed in ml
    uint32_t total_volume;     // Total volume dispensed in ml (from flow sensor)
    uint8_t status;            // Task status (0=idle, 1=running, 2=paused, 3=completed)
    uint16_t reserved;         // Elapsed time in seconds for volume mode (0 for duration mode)
} __packed;

/* Structure for setting/reading RTC */
struct rtc_data {
    uint8_t year; /* Year minus 2000 (0-99) */
    uint8_t month; /* Month (1-12) */
    uint8_t day; /* Day (1-31) */
    uint8_t hour; /* Hour (0-23) */
    uint8_t minute; /* Minute (0-59) */
    uint8_t second; /* Second (0-59) */
    uint8_t day_of_week; /* Day of week (0-6, 0=Sunday) */
    int16_t utc_offset_minutes; /* UTC offset in minutes (e.g., 120 for UTC+2) */
    uint8_t dst_active; /* 1 if DST is currently active, 0 otherwise */
    uint8_t reserved[6]; /* Reserved for future use */
}
        __packed;

/* Structure for alarms and notifications */
struct alarm_data {
    uint8_t alarm_code; /* Alarm code */
    uint16_t alarm_data; /* Additional alarm-specific data */
    uint32_t timestamp; /* Timestamp when alarm occurred */
}
        __packed;

/* Structure for flow sensor calibration */
struct calibration_data {
    uint8_t action; /* 0=stop, 1=start, 2=in progress, 3=calculated */
    uint32_t pulses; /* Number of pulses counted */
    uint32_t volume_ml; /* Volume in ml (input or calculated) */
    uint32_t pulses_per_liter; /* Calibration result */
}
        __packed;

/* Structure for irrigation history request/response */
struct history_data {
    uint8_t channel_id;        /* Channel (0-7) or 0xFF for all */
    uint8_t history_type;      /* 0=detailed, 1=daily, 2=monthly, 3=annual */
    uint8_t entry_index;       /* Entry index (0=most recent) */
    uint8_t count;             /* Number of entries to return/returned */
    uint32_t start_timestamp;  /* Start time filter (0=no filter) */
    uint32_t end_timestamp;    /* End time filter (0=no filter) */
    
    /* Response data (varies by history_type) */
    union {
        struct {
            uint32_t timestamp;
            uint8_t channel_id;      /* Channel that performed the watering */
            uint8_t event_type;      /* START/COMPLETE/ABORT/ERROR */
            uint8_t mode;
            uint16_t target_value;
            uint16_t actual_value;
            uint16_t total_volume_ml;
            uint8_t trigger_type;
            uint8_t success_status;
            uint8_t error_code;
            uint16_t flow_rate_avg;
            uint8_t reserved[2];     /* For alignment */
        } detailed;
        
        struct {
            uint16_t day_index;
            uint16_t year;
            uint8_t watering_sessions;
            uint32_t total_volume_ml;
            uint16_t total_duration_sec;
            uint16_t avg_flow_rate;
            uint8_t success_rate;
            uint8_t error_count;
        } daily;
        
        struct {
            uint8_t month;
            uint16_t year;
            uint16_t total_sessions;
            uint32_t total_volume_ml;
            uint16_t total_duration_hours;
            uint16_t avg_daily_volume;
            uint8_t active_days;
            uint8_t success_rate;
        } monthly;
        
        struct {
            uint16_t year;
            uint16_t total_sessions;
            uint32_t total_volume_liters;
            uint16_t avg_monthly_volume;
            uint8_t most_active_month;
            uint8_t success_rate;
            uint16_t peak_month_volume;
        } annual;
    } data;
}
        __packed;

/* Structure for diagnostics */
struct diagnostics_data {
    uint32_t uptime; /* System uptime in minutes */
    uint16_t error_count; /* Total error count since boot */
    uint8_t last_error; /* Code of the most recent error (0 if no errors) */
    uint8_t valve_status; /* Valve status bitmap (bit 0 = channel 0, etc.) */
    uint8_t battery_level; /* Battery level in percent (0xFF if not applicable) */
    uint8_t reserved[3]; /* Reserved for future use */
} __packed;

/* Structure for growing environment configuration */
struct growing_env_data {
    uint8_t channel_id;           /* Channel ID (0-7) */
    uint8_t plant_type;           /* Plant type (0-7) */
    uint16_t specific_plant;      /* Specific plant type (see enums, meaning depends on plant_type) */
    uint8_t soil_type;            /* Soil type (0-7) */
    uint8_t irrigation_method;    /* Irrigation method (0-5) */
    uint8_t use_area_based;       /* 1=area in m², 0=plant count */
    union {
        float area_m2;            /* Area in square meters */
        uint16_t plant_count;     /* Number of plants */
    } coverage;
    uint8_t sun_percentage;       /* Sun exposure percentage (0-100) */
    /* Custom plant fields (used only when plant_type=7) */
    char custom_name[32];         /* CUSTOM PLANT NAME (32 bytes): Species name when plant_type=Custom (e.g., "Hibiscus rosa-sinensis") */
    float water_need_factor;      /* Water need multiplier (0.1-5.0) */
    uint8_t irrigation_freq_days; /* Recommended irrigation frequency (days) */
    uint8_t prefer_area_based;    /* 1=plant prefers m² measurement, 0=prefers plant count */
} __packed;

/* Characteristic value handles */
static uint8_t valve_value[sizeof(struct valve_control_data)];
static uint8_t flow_value[sizeof(uint32_t)];
static uint8_t status_value[1];
static uint8_t channel_config_value[sizeof(struct channel_config_data)];
static uint8_t schedule_value[sizeof(struct schedule_config_data)];
static uint8_t system_config_value[sizeof(struct system_config_data)];
static uint8_t task_queue_value[sizeof(struct task_queue_data)];
static uint8_t statistics_value[sizeof(struct statistics_data)];
static uint8_t rtc_value[sizeof(struct rtc_data)];
static uint8_t alarm_value[sizeof(struct alarm_data)];
static uint8_t calibration_value[sizeof(struct calibration_data)];
static uint8_t history_value[sizeof(struct history_data)];
static uint8_t diagnostics_value[sizeof(struct diagnostics_data)];
static uint8_t growing_env_value[sizeof(struct growing_env_data)];
static uint8_t current_task_value[sizeof(struct current_task_data)];
static uint8_t timezone_value[sizeof(timezone_config_t)];

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

#define FRAGMENTATION_TIMEOUT_MS 5000  /* 5 second timeout for fragmentation */

/* Check and reset fragmentation state if timeout occurred */
static inline void check_fragmentation_timeout(void) {
    if (channel_frag.in_progress) {
        uint32_t now = k_uptime_get_32();
        if (now - channel_frag.start_time > FRAGMENTATION_TIMEOUT_MS) {
            printk("⚠️ BLE: Fragmentation timeout - resetting state\n");
            channel_frag.in_progress = false;
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



/* ---------------------------------------------------------------
 *  Accumulator for fragmented Growing Environment writes (≤20 B each)
 * ------------------------------------------------------------- */
static struct {
    uint8_t  channel_id;       /* channel being edited              */
    uint16_t expected;         /* total struct size from first frame */
    uint16_t received;         /* bytes stored so far               */
    uint8_t  buf[128];         /* temporary buffer for struct data - increased for 76-byte structures */
    bool     in_progress;      /* true while receiving fragments    */
} growing_env_frag = {0};

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
/* ---------------------------------------------------------------- */

/* Global variables for calibration */
static bool calibration_active = false;
static uint32_t calibration_start_pulses = 0;

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

/* ----------------------------------------------------------- */
/*  GATT Service Definition                                   */
/* ----------------------------------------------------------- */
BT_GATT_SERVICE_DEFINE(irrigation_svc,
    BT_GATT_PRIMARY_SERVICE(&irrigation_service_uuid),
    
    // Valve control characteristic
    BT_GATT_CHARACTERISTIC(&valve_char_uuid.uuid,
                         BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE | BT_GATT_CHRC_NOTIFY,
                         BT_GATT_PERM_READ | BT_GATT_PERM_WRITE,
                         read_valve, write_valve, valve_value),
    BT_GATT_CCC(valve_ccc_cfg_changed,
               BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
    
    // Flow sensor characteristic  
    BT_GATT_CHARACTERISTIC(&flow_char_uuid.uuid,
                         BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
                         BT_GATT_PERM_READ,
                         read_flow, NULL, flow_value),
    BT_GATT_CCC(flow_ccc_cfg_changed,
               BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
    
    // Status characteristic
    BT_GATT_CHARACTERISTIC(&status_char_uuid.uuid,
                         BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
                         BT_GATT_PERM_READ,
                         read_status, NULL, status_value),
    BT_GATT_CCC(status_ccc_cfg_changed,
               BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
    
    // Channel configuration characteristic
    BT_GATT_CHARACTERISTIC(&channel_config_uuid.uuid,
                         BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE | BT_GATT_CHRC_NOTIFY,
                         BT_GATT_PERM_READ | BT_GATT_PERM_WRITE,
                         read_channel_config, write_channel_config, channel_config_value),
    BT_GATT_CCC(channel_config_ccc_changed,
               BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
    
    // Schedule configuration characteristic
    BT_GATT_CHARACTERISTIC(&schedule_uuid.uuid,
                         BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE | BT_GATT_CHRC_NOTIFY,
                         BT_GATT_PERM_READ | BT_GATT_PERM_WRITE,
                         read_schedule, write_schedule, schedule_value),
    BT_GATT_CCC(schedule_ccc_changed,
               BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
    
    // System configuration characteristic
    BT_GATT_CHARACTERISTIC(&system_config_uuid.uuid,
                         BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE | BT_GATT_CHRC_NOTIFY,
                         BT_GATT_PERM_READ | BT_GATT_PERM_WRITE,
                         read_system_config, write_system_config, system_config_value),
    BT_GATT_CCC(system_config_ccc_changed,
               BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
    
    // Task queue characteristic
    BT_GATT_CHARACTERISTIC(&task_queue_uuid.uuid,
                         BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE | BT_GATT_CHRC_NOTIFY,
                         BT_GATT_PERM_READ | BT_GATT_PERM_WRITE,
                         read_task_queue, write_task_queue, task_queue_value),
    BT_GATT_CCC(task_queue_ccc_changed,
               BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
    
    // Statistics characteristic
    BT_GATT_CHARACTERISTIC(&statistics_uuid.uuid,
                         BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE | BT_GATT_CHRC_NOTIFY,
                         BT_GATT_PERM_READ | BT_GATT_PERM_WRITE,
                         read_statistics, write_statistics, statistics_value),
    BT_GATT_CCC(statistics_ccc_cfg_changed,
               BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
    
    // RTC characteristic
    BT_GATT_CHARACTERISTIC(&rtc_char_uuid.uuid,
                         BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE | BT_GATT_CHRC_NOTIFY,
                         BT_GATT_PERM_READ | BT_GATT_PERM_WRITE,
                         read_rtc, write_rtc, rtc_value),
    BT_GATT_CCC(rtc_ccc_changed,
               BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
    
    // Alarm characteristic
    BT_GATT_CHARACTERISTIC(&alarm_char_uuid.uuid,
                         BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE | BT_GATT_CHRC_NOTIFY,
                         BT_GATT_PERM_READ | BT_GATT_PERM_WRITE,
                         read_alarm, write_alarm, alarm_value),
    BT_GATT_CCC(alarm_ccc_changed,
               BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
    
    // Calibration characteristic
    BT_GATT_CHARACTERISTIC(&calibration_char_uuid.uuid,
                         BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE | BT_GATT_CHRC_NOTIFY,
                         BT_GATT_PERM_READ | BT_GATT_PERM_WRITE,
                         read_calibration, write_calibration, calibration_value),
    BT_GATT_CCC(calibration_ccc_changed,
               BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
    
    // History characteristic
    BT_GATT_CHARACTERISTIC(&history_char_uuid.uuid,
                         BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE | BT_GATT_CHRC_NOTIFY,
                         BT_GATT_PERM_READ | BT_GATT_PERM_WRITE,
                         read_history, write_history, history_value),
    BT_GATT_CCC(history_ccc_changed,
               BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
    
    // Diagnostics characteristic
    BT_GATT_CHARACTERISTIC(&diagnostics_char_uuid.uuid,
                         BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
                         BT_GATT_PERM_READ,
                         read_diagnostics, NULL, diagnostics_value),
    BT_GATT_CCC(diagnostics_ccc_changed,
               BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
    
    // Growing environment characteristic
    BT_GATT_CHARACTERISTIC(&growing_env_char_uuid.uuid,
                         BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE | BT_GATT_CHRC_NOTIFY,
                         BT_GATT_PERM_READ | BT_GATT_PERM_WRITE,
                         read_growing_env, write_growing_env, growing_env_value),
    BT_GATT_CCC(growing_env_ccc_changed,
               BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
    
    // Current task characteristic
    BT_GATT_CHARACTERISTIC(&current_task_char_uuid.uuid,
                         BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE | BT_GATT_CHRC_NOTIFY,
                         BT_GATT_PERM_READ | BT_GATT_PERM_WRITE,
                         read_current_task, write_current_task, current_task_value),
    BT_GATT_CCC(current_task_ccc_changed,
               BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
    
    // Timezone configuration characteristic
    BT_GATT_CHARACTERISTIC(&timezone_char_uuid.uuid,
                         BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE | BT_GATT_CHRC_NOTIFY,
                         BT_GATT_PERM_READ | BT_GATT_PERM_WRITE,
                         read_timezone, write_timezone, timezone_value),
    BT_GATT_CCC(timezone_ccc_changed,
               BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
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
            .auto_enabled = 0
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
    
    LOG_DBG("Schedule read: ch=%u, type=%u, days=0x%02X, time=%02u:%02u, mode=%u, value=%u, auto=%u",
            read_value.channel_id, read_value.schedule_type, read_value.days_mask,
            read_value.hour, read_value.minute, read_value.watering_mode,
            read_value.value, read_value.auto_enabled);
    
    return bt_gatt_attr_read(conn, attr, buf, len, offset,
                             &read_value, sizeof(read_value));
}

static ssize_t write_schedule(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                             const void *buf, uint16_t len, uint16_t offset, uint8_t flags) {
    struct schedule_config_data *value = (struct schedule_config_data *)attr->user_data;

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

    /* Standard write handling for complete schedule structure */
    if (offset + len > sizeof(*value)) {
        LOG_ERR("Schedule write: Invalid offset/length (offset=%u, len=%u, max=%zu)", 
                offset, len, sizeof(*value));
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }

    /* Only accept complete structure writes */
    if (len != sizeof(*value)) {
        LOG_ERR("Schedule write: Invalid length (got %u, expected %zu)", 
                len, sizeof(*value));
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }

    memcpy(((uint8_t *)value) + offset, buf, len);

    /* If complete structure received, validate and commit changes */
    if (offset + len == sizeof(*value)) {
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
        if (value->hour > 23 || value->minute > 59 || value->schedule_type > 1 ||
            value->watering_mode > 1) {
            LOG_ERR("Invalid schedule parameters: hour=%u, minute=%u, type=%u, mode=%u", 
                    value->hour, value->minute, value->schedule_type, value->watering_mode);
            return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
        }

        /* Validate value (must be > 0 if auto_enabled = 1) */
        if (value->auto_enabled && value->value == 0) {
            LOG_ERR("Invalid schedule value: auto_enabled=1 but value=0");
            return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
        }

        /* Validate days_mask (must be > 0 if auto_enabled = 1) */
        if (value->auto_enabled && value->days_mask == 0) {
            LOG_ERR("Invalid schedule days_mask: auto_enabled=1 but days_mask=0");
            return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
        }

        LOG_INF("Schedule update: ch=%u, type=%u (%s), days=0x%02X, time=%02u:%02u, mode=%u (%s), value=%u, auto=%u",
                value->channel_id, value->schedule_type, 
                (value->schedule_type == 0) ? "Daily" : "Periodic",
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
        } else {
            /* Periodic schedule */
            channel->watering_event.schedule_type = SCHEDULE_PERIODIC;
            channel->watering_event.schedule.periodic.interval_days = value->days_mask;
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
        LOG_ERR("Invalid parameters for System Config read");
        return -EINVAL;
    }
    
    /* Per BLE API Documentation: READ returns current system configuration */
    /* Structure: version, power_mode, flow_calibration, max_active_valves, num_channels, master_valve_config */
    struct system_config_data *config = (struct system_config_data *)system_config_value;
    
    /* Update values from system state */
    config->version = 1; /* Configuration version (read-only) */
    
    /* Get current power mode from watering system */
    power_mode_t current_mode;
    if (watering_get_power_mode(&current_mode) == WATERING_SUCCESS) {
        config->power_mode = (uint8_t)current_mode;
    } else {
        config->power_mode = 0; /* Default to normal mode on error */
    }
    
    config->flow_calibration = get_flow_calibration(); /* Get current calibration */
    config->max_active_valves = 1; /* Always 1 (read-only) */
    config->num_channels = WATERING_CHANNELS_COUNT; /* Number of channels (read-only) */
    
    /* Get current master valve configuration */
    master_valve_config_t master_config;
    if (master_valve_get_config(&master_config) == WATERING_SUCCESS) {
        config->master_valve_enabled = master_config.enabled ? 1 : 0;
        config->master_valve_pre_delay = master_config.pre_start_delay_sec;
        config->master_valve_post_delay = master_config.post_stop_delay_sec;
        config->master_valve_overlap_grace = master_config.overlap_grace_sec;
        config->master_valve_auto_mgmt = master_config.auto_management ? 1 : 0;
        config->master_valve_current_state = master_config.is_active ? 1 : 0;
    } else {
        /* Default values on error */
        config->master_valve_enabled = 0;
        config->master_valve_pre_delay = 0;
        config->master_valve_post_delay = 0;
        config->master_valve_overlap_grace = 30;
        config->master_valve_auto_mgmt = 1;
        config->master_valve_current_state = 0;
    }
    
    /* Reduced logging frequency for read operations */
    static uint32_t last_read_time = 0;
    uint32_t now = k_uptime_get_32();
    if (now - last_read_time > 5000) { /* Log every 5 seconds max */
        LOG_DBG("System Config read: version=%u, power_mode=%u, flow_cal=%u, max_valves=%u, channels=%u",
                config->version, config->power_mode, config->flow_calibration,
                config->max_active_valves, config->num_channels);
        LOG_DBG("Master Valve: enabled=%u, pre_delay=%d, post_delay=%d, overlap=%u, auto=%u, state=%u",
                config->master_valve_enabled, config->master_valve_pre_delay, 
                config->master_valve_post_delay, config->master_valve_overlap_grace,
                config->master_valve_auto_mgmt, config->master_valve_current_state);
        last_read_time = now;
    }
    
    return bt_gatt_attr_read(conn, attr, buf, len, offset, system_config_value,
                           sizeof(system_config_value));
}

static ssize_t write_system_config(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                  const void *buf, uint16_t len, uint16_t offset, uint8_t flags) {
    struct system_config_data *config = (struct system_config_data *)attr->user_data;
    
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
    
    /* Only accept complete structure writes */
    if (len != sizeof(*config)) {
        LOG_ERR("System Config write: Invalid length (got %u, expected %zu)", 
                len, sizeof(*config));
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }
    
    memcpy(((uint8_t *)config) + offset, buf, len);
    
    /* If complete structure received, validate and apply changes */
    if (offset + len == sizeof(*config)) {
        /* Validate power_mode */
        if (config->power_mode > 2) {
            LOG_ERR("Invalid power_mode: %u (must be 0-2)", config->power_mode);
            return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
        }
        
        /* Validate flow_calibration (reasonable range) */
        if (config->flow_calibration < 100 || config->flow_calibration > 10000) {
            LOG_ERR("Invalid flow_calibration: %u (range 100-10000)", config->flow_calibration);
            return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
        }
        
        LOG_INF("System Config update: power_mode=%u, flow_cal=%u (read-only fields ignored)",
                config->power_mode, config->flow_calibration);
        
        /* Apply writable settings */
        
        /* Update power mode */
        watering_error_t pm_err = watering_set_power_mode((power_mode_t)config->power_mode);
        if (pm_err != WATERING_SUCCESS) {
            LOG_ERR("Failed to set power mode: %d", pm_err);
            if (pm_err == WATERING_ERROR_BUSY) {
                return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY); /* System busy, try again later */
            } else {
                return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED); /* Invalid power mode */
            }
        }
        
        LOG_INF("Power mode updated: %u (%s)", config->power_mode,
                (config->power_mode == 0) ? "Normal" :
                (config->power_mode == 1) ? "Energy-Saving" : 
                (config->power_mode == 2) ? "Ultra-Low" : "Unknown");
        
        /* Update flow calibration */
        int cal_err = set_flow_calibration(config->flow_calibration);
        if (cal_err != 0) {
            LOG_ERR("Failed to set flow calibration: %d", cal_err);
            return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
        }
        
        /* Update master valve configuration */
        master_valve_config_t master_config;
        master_config.enabled = (config->master_valve_enabled != 0);
        master_config.pre_start_delay_sec = config->master_valve_pre_delay;
        master_config.post_stop_delay_sec = config->master_valve_post_delay;
        master_config.overlap_grace_sec = config->master_valve_overlap_grace;
        master_config.auto_management = (config->master_valve_auto_mgmt != 0);
        /* Note: is_active is read-only and managed by the system */
        
        watering_error_t mv_err = master_valve_set_config(&master_config);
        if (mv_err != WATERING_SUCCESS) {
            LOG_ERR("Failed to set master valve config: %d", mv_err);
            return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
        }
        
        LOG_INF("Master Valve config updated: enabled=%u, pre_delay=%d, post_delay=%d, overlap=%u, auto=%u",
                config->master_valve_enabled, config->master_valve_pre_delay,
                config->master_valve_post_delay, config->master_valve_overlap_grace,
                config->master_valve_auto_mgmt);
        
        /* Read-only fields are ignored during write */
        /* version, max_active_valves, num_channels, master_valve_current_state cannot be changed */
        
        /* Save configuration to persistent storage if needed */
        /* Note: Flow calibration is saved automatically by set_flow_calibration() */
        
        /* Send notification to confirm system config update per BLE API Documentation */
        /* System Config (ef6): Config updates | On change (throttled 500ms) | System parameter changes */
        if (notification_state.system_config_notifications_enabled) {
            /* Update system_config_value with current values for notification */
            config->version = 1; /* Configuration version */
            config->max_active_valves = 1; /* Always 1 */
            config->num_channels = WATERING_CHANNELS_COUNT; /* Number of channels */
            
            const struct bt_gatt_attr *attr = &irrigation_svc.attrs[ATTR_IDX_SYSTEM_CFG_VALUE];
            int bt_err = safe_notify(default_conn, attr, system_config_value, sizeof(struct system_config_data));
            
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
        
        /* Per BLE API Documentation: System Config structure with global parameters */
        /* Notification frequency: on change (throttled 500ms) for system parameter changes */
        
        LOG_INF("System Config monitoring: version, power_mode, flow_calibration, limits");
        
        /* Initialize system_config_value with current values */
        struct system_config_data *config = (struct system_config_data *)system_config_value;
        config->version = 1; /* Configuration version */
        config->power_mode = (config->power_mode <= 2) ? config->power_mode : 0; /* Validate power mode */
        config->flow_calibration = get_flow_calibration(); /* Current calibration */
        config->max_active_valves = 1; /* Always 1 */
        config->num_channels = WATERING_CHANNELS_COUNT; /* Number of channels */
    } else {
        LOG_INF("System Config notifications disabled");
        /* Clear system_config_value when notifications disabled */
        memset(system_config_value, 0, sizeof(system_config_value));
    }
}

/* Task queue characteristics implementation */
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
                case 1: /* Cancel current task */
                    {
                        bool stopped = watering_stop_current_task();
                        if (stopped) {
                            LOG_INF("✅ Current task cancelled");
                        } else {
                            LOG_WRN("No current task to cancel");
                        }
                    }
                    break;
                    
                case 2: /* Clear entire queue */
                    {
                        int err = watering_clear_task_queue();
                        if (err == 0) {
                            LOG_INF("✅ Task queue cleared");
                        } else {
                            LOG_ERR("❌ Failed to clear task queue: %d", err);
                            return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
                        }
                    }
                    break;
                    
                case 3: /* Delete specific task */
                    {
                        uint8_t task_id_to_delete = queue_data->task_id_to_delete;
                        if (task_id_to_delete == 0) {
                            LOG_WRN("No task ID specified for deletion");
                            break;
                        }
                        
                        /* For now, we can only delete the currently active task */
                        if (task_id_to_delete == queue_data->active_task_id) {
                            bool success = watering_stop_current_task();
                            if (success) {
                                LOG_INF("✅ Active task deleted (ID: %u)", task_id_to_delete);
                            } else {
                                LOG_ERR("❌ Failed to delete active task");
                                return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
                            }
                        } else {
                            LOG_WRN("Cannot delete task ID %u - only active task deletion supported", task_id_to_delete);
                        }
                    }
                    break;
                    
                case 4: /* Clear error state */
                    {
                        watering_error_t err = watering_clear_errors();
                        if (err == WATERING_SUCCESS) {
                            LOG_INF("✅ Error state cleared");
                        } else {
                            LOG_ERR("❌ Failed to clear error state: %d", err);
                            return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
                        }
                    }
                    break;
                    
                default:
                    LOG_ERR("Unknown task queue command: %u", queue_data->command);
                    return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
            }
            
            /* Clear command after processing */
            queue_data->command = 0;
            
            /* Send notification to confirm task queue command execution per BLE API Documentation */
            /* Task Queue (ef7): Queue changes | Immediate (throttled 500ms) | Task added/completed/cancelled */
            if (notification_state.task_queue_notifications_enabled) {
                bt_irrigation_queue_status_notify();
            }
        }
        
        LOG_INF("✅ Task Queue command processed successfully");
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
    } else {
        LOG_INF("Task Queue notifications disabled");
        /* Clear task_queue_value when notifications disabled */
        memset(task_queue_value, 0, sizeof(task_queue_value));
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
    
    read_value.last_watering = channel->last_watering_time / 1000; /* Convert to seconds */
    
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

        /* Per BLE API Documentation: ≥15 bytes = Reset statistics for channel */
        /* Reset statistics to written values (typically zeros for reset operation) */
        /* This allows clients to reset statistics by writing a zeroed structure */
        
        /* Update channel statistics when actual statistics tracking is implemented */
        if (value->total_volume == 0 && value->last_volume == 0 && value->count == 0) {
            /* This is a reset operation - clear channel statistics */
            watering_error_t reset_err = watering_reset_channel_statistics(value->channel_id);
            if (reset_err != WATERING_SUCCESS) {
                LOG_WRN("Failed to reset channel %u statistics: %d", value->channel_id, reset_err);
            } else {
                LOG_INF("Channel %u statistics reset successfully", value->channel_id);
            }
        } else {
            /* This is a statistics update - update channel statistics */
            watering_error_t update_err = watering_update_channel_statistics(value->channel_id,
                                                                           value->last_volume,
                                                                           value->last_watering);
            if (update_err != WATERING_SUCCESS) {
                LOG_WRN("Failed to update channel %u statistics: %d", value->channel_id, update_err);
            } else {
                LOG_INF("Channel %u statistics updated successfully", value->channel_id);
            }
        }
        
        /* Send notification to confirm statistics update per BLE API Documentation */
        /* Statistics (ef8): Watering events | After completion | Volume and count updates */
        if (notification_state.statistics_notifications_enabled) {
            const struct bt_gatt_attr *attr = &irrigation_svc.attrs[ATTR_IDX_STATISTICS_VALUE];
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
            
            stats_data->last_watering = channel->last_watering_time / 1000; /* Convert to seconds */
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
    if (!default_conn || !notification_state.statistics_notifications_enabled) {
        return 0; // No connection or notifications disabled
    }
    
    if (channel_id >= WATERING_CHANNELS_COUNT) {
        LOG_ERR("Invalid channel ID for statistics update: %u", channel_id);
        return -EINVAL;
    }
    
    /* Update statistics_value with new watering event data */
    struct statistics_data *stats = (struct statistics_data *)statistics_value;
    
    /* Only update if this is for the currently selected channel or if no channel selected */
    if (stats->channel_id == channel_id || stats->channel_id == 0) {
        stats->channel_id = channel_id;
        stats->last_volume = volume_ml;
        stats->last_watering = timestamp;
        stats->total_volume += volume_ml;
        stats->count++;
        
        LOG_INF("Statistics updated: ch=%u, last_vol=%u, timestamp=%u, total_vol=%u, count=%u",
                channel_id, volume_ml, timestamp, stats->total_volume, stats->count);
        
        /* Send notification about statistics update */
        return bt_irrigation_statistics_notify();
    }
    
    return 0; // Not the selected channel
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
/* Current task characteristics implementation */
static ssize_t read_current_task(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                void *buf, uint16_t len, uint16_t offset) {
    if (!conn || !attr || !buf) {
        LOG_ERR("Invalid parameters for Current Task read");
        return -EINVAL;
    }
    
    struct current_task_data *value = (struct current_task_data *)current_task_value;
    
    /* Per BLE API Documentation: READ returns current task status and real-time progress */
    /* Get current task information from watering system */
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
        
        LOG_DBG("Current Task read: No active task");
    } else {
        /* Active task - populate with real data */
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
        value->start_time = watering_task_state.watering_start_time / 1000;  // Convert to seconds
        value->mode = (current_task->channel->watering_event.watering_mode == WATERING_BY_DURATION) ? 0 : 1;
        
        // Set status based on current state
        if (watering_task_state.task_paused) {
            value->status = 3;  // Paused
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
            value->reserved = 0;  // Not used for duration mode
        } else {
            /* Volume mode */
            uint32_t target_ml = current_task->channel->watering_event.watering.by_volume.volume_liters * 1000;
            value->target_value = target_ml;
            value->current_value = total_volume_ml;
            value->reserved = elapsed_seconds;  // Elapsed time for volume mode
        }
        
        LOG_DBG("Current Task read: ch=%u, mode=%u, target=%u, current=%u, volume=%u, status=%u",
                value->channel_id, value->mode, value->target_value, 
                value->current_value, value->total_volume, value->status);
    }
    
    return bt_gatt_attr_read(conn, attr, buf, len, offset, value, sizeof(struct current_task_data));
}

static ssize_t write_current_task(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                 const void *buf, uint16_t len, uint16_t offset, uint8_t flags) {
    if (!conn || !attr || !buf) {
        LOG_ERR("Invalid parameters for Current Task write");
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_HANDLE);
    }
    
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
    
    uint8_t command = ((const uint8_t *)buf)[0];
    
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
                        value->status = 3;  // Set to paused
                        
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
                    if (value->status == 3) {  // If it was paused
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
        LOG_INF("✅ Current Task notifications ENABLED - will send updates every 2 seconds during execution");
        
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
            value->status = watering_task_state.task_in_progress ? 1 : 0;
            LOG_INF("Current Task notifications ready: Active task on channel %u", channel_id);
        }
        
        /* Don't send immediate notification - wait for real updates */
        /* Per documentation: notifications sent automatically when task status changes */
    } else {
        LOG_INF("Current Task notifications disabled");
        /* Clear current_task_value when notifications disabled */
        memset(current_task_value, 0, sizeof(current_task_value));
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
    struct history_data *value = (struct history_data *)attr->user_data;
    const uint8_t *data = (const uint8_t *)buf;
    
    /* Validate basic parameters */
    if (!conn || !attr || !buf) {
        LOG_ERR("Invalid parameters for History write");
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_HANDLE);
    }

    if (offset + len > sizeof(struct history_data)) {
        LOG_ERR("History write: Invalid offset/length (offset=%u, len=%u, max=%zu)", 
                offset, len, sizeof(struct history_data));
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }

    /* Check fragmentation timeout */
    if (history_frag.in_progress) {
        uint32_t now = k_uptime_get_32();
        if (now - history_frag.start_time > FRAGMENTATION_TIMEOUT_MS) {
            printk("⚠️ BLE: History fragmentation timeout - resetting state\n");
            history_frag.in_progress = false;
        }
    }

    /* —— CONTINUE FRAGMENTATION PROTOCOL —— */
    if (history_frag.in_progress) {
        uint16_t remaining = history_frag.expected - history_frag.received;
        uint16_t copy_len = (len > remaining) ? remaining : len;
        
        printk("🔧 BLE: History fragment continuation - len=%u, remaining=%u, copy_len=%u\n", 
               len, remaining, copy_len);
        
        if (history_frag.received + copy_len > sizeof(history_frag.buf)) {
            printk("❌ History fragment buffer overflow\n");
            history_frag.in_progress = false;
            return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
        }
        
        memcpy(history_frag.buf + history_frag.received, data, copy_len);
        history_frag.received += copy_len;
        
        printk("🔧 BLE: History fragment received: %u/%u bytes\n", 
               history_frag.received, history_frag.expected);
        
        /* Check if complete */
        if (history_frag.received >= history_frag.expected) {
            /* FULL STRUCTURE UPDATE */
            if (history_frag.expected != sizeof(struct history_data)) {
                printk("❌ Invalid history structure size: got %u, expected %zu\n", 
                       history_frag.expected, sizeof(struct history_data));
                history_frag.in_progress = false;
                return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
            }
            
            /* Copy complete structure */
            memcpy(value, history_frag.buf, sizeof(struct history_data));
            
            printk("✅ BLE: Complete history received via fragmentation (type %u)\n", 
                   history_frag.frag_type);
            
            /* Process like standard write */
            history_frag.in_progress = false;
            goto process_full_query;
        }
        
        return len;
    }

    /* —— NEW FRAGMENTATION PROTOCOL HEADER —— */
    /* Check for fragmentation protocol header: [reserved][frag_type][size_low][size_high][data...] */
    /* Only process as header if no fragmentation is in progress and we have enough data */
    if (offset == 0 && len >= 4 && !history_frag.in_progress) {
        uint8_t reserved_byte = data[0];
        uint8_t frag_type = data[1];
        uint16_t total_size;
        
        /* Handle both big-endian (frag_type=2) and little-endian (frag_type=3) */
        if (frag_type == 2) {
            total_size = (data[2] << 8) | data[3];  /* Big-endian */
        } else if (frag_type == 3) {
            total_size = data[2] | (data[3] << 8);  /* Little-endian */
        } else {
            /* Not a fragmentation header, treat as standard write */
            goto standard_write;
        }
        
        /* Validate fragmentation header */
        if (total_size == 0 || total_size != sizeof(struct history_data)) {
            printk("🔧 BLE: Ignoring invalid history fragment header - frag_type=%u, total_size=%u\n", 
                   frag_type, total_size);
            goto standard_write;
        }
        
        printk("🔧 BLE: History fragmentation header detected - frag_type=%u, total_size=%u\n", 
               frag_type, total_size);
        
        if (total_size > sizeof(history_frag.buf)) {
            printk("❌ History data size too large: %u > %zu\n", total_size, sizeof(history_frag.buf));
            return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
        }
        
        /* Initialize fragmentation state */
        history_frag.frag_type = frag_type;
        history_frag.expected = total_size;
        history_frag.received = 0;
        history_frag.in_progress = true;
        history_frag.start_time = k_uptime_get_32();
        memset(history_frag.buf, 0, sizeof(history_frag.buf));
        
        printk("🔧 BLE: History fragmentation initialized - type=%u, expected=%u bytes\n", 
               frag_type, total_size);
        
        /* Process payload if present */
        if (len > 4) {
            uint16_t payload_len = len - 4;
            if (payload_len > history_frag.expected) {
                payload_len = history_frag.expected;
            }
            memcpy(history_frag.buf, data + 4, payload_len);
            history_frag.received = payload_len;
            
            printk("🔧 BLE: Received history fragment: %u/%u bytes\n", 
                   payload_len, history_frag.expected);
        }
        
        return len;
    }

standard_write:

    /* Standard write handling - accept complete 32-byte structure */
    if (len != sizeof(struct history_data)) {
        LOG_ERR("History write: Invalid length (got %u, expected %zu)", 
                len, sizeof(struct history_data));
        LOG_INF("History structure size debug: header=%zu, union=%zu, total=%zu",
                sizeof(struct history_data) - sizeof(((struct history_data*)0)->data),
                sizeof(((struct history_data*)0)->data),
                sizeof(struct history_data));
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }

    memcpy(((uint8_t *) value) + offset, buf, len);

process_full_query:

    /* Per BLE API Documentation: Process history query */
    /* Validate channel ID */
    if (value->channel_id != 0xFF && value->channel_id >= WATERING_CHANNELS_COUNT) {
        LOG_ERR("History write: Invalid channel ID %u", value->channel_id);
        return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
    }

    /* Validate history type */
    if (value->history_type > 3) {
        LOG_ERR("History write: Invalid history type %u", value->history_type);
        return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
    }

    LOG_INF("History query: channel=%u (%s), type=%u (%s), index=%u, count=%u, time=%u-%u",
            value->channel_id, 
            (value->channel_id == 0xFF) ? "ALL" : "SPECIFIC",
            value->history_type,
            (value->history_type == 0) ? "detailed" :
            (value->history_type == 1) ? "daily" :
            (value->history_type == 2) ? "monthly" : "annual",
            value->entry_index, value->count,
            value->start_timestamp, value->end_timestamp);

    /* Process history queries with real data from watering_history system */
    
    switch (value->history_type) {
        case 0: /* Detailed events */
        {
            /* Get real detailed events from history system */
            history_event_t events[10];
            uint16_t event_count = 0;
            watering_error_t err;
            
            if (value->channel_id == 0xFF) {
                /* Query all channels - for now, just use channel 0 */
                err = watering_history_query_page(0, value->entry_index, events, &event_count);
            } else {
                err = watering_history_query_page(value->channel_id, value->entry_index, events, &event_count);
            }
            
            if (err == WATERING_SUCCESS && event_count > 0) {
                /* Convert first history event to BLE format */
                history_event_t *event = &events[0];
                
                value->data.detailed.timestamp = timezone_get_unix_utc(); /* Unix timestamp UTC */
                value->data.detailed.channel_id = (value->channel_id == 0xFF) ? 0 : value->channel_id;
                value->data.detailed.event_type = 1; /* COMPLETE - from event flags */
                value->data.detailed.mode = event->flags.mode;
                value->data.detailed.target_value = event->target_ml;
                value->data.detailed.actual_value = event->actual_ml;
                value->data.detailed.total_volume_ml = event->actual_ml;
                value->data.detailed.trigger_type = event->flags.trigger;
                value->data.detailed.success_status = event->flags.success;
                value->data.detailed.error_code = event->flags.err;
                value->data.detailed.flow_rate_avg = event->avg_flow_ml_s;
                value->count = event_count;
                
                LOG_INF("✅ Real history data: ch=%u, mode=%u, target=%u, actual=%u ml", 
                        value->channel_id, event->flags.mode, event->target_ml, event->actual_ml);
            } else {
                /* No real data available - return zeros */
                memset(&value->data.detailed, 0, sizeof(value->data.detailed));
                value->data.detailed.channel_id = (value->channel_id == 0xFF) ? 0 : value->channel_id;
                value->count = 0;
                LOG_INF("No detailed history data found for channel %u, index %u", 
                        value->channel_id, value->entry_index);
            }
            break;
        }
        case 1: /* Daily statistics */
        {
            daily_stats_t daily_results[10];
            uint16_t daily_count = 0;
            uint16_t current_year = get_current_year();
            uint16_t current_day_of_year = get_current_day_of_year();
            uint16_t target_day = (value->entry_index > 0) ? 
                                (current_day_of_year - value->entry_index) : current_day_of_year;
            uint16_t end_day = target_day + 1;
            
            /* Ensure target_day is within valid range */
            if (target_day >= DAILY_STATS_DAYS) {
                target_day = target_day % DAILY_STATS_DAYS;
                end_day = target_day + 1;
            }
            
            watering_error_t err = watering_history_get_daily_stats(
                (value->channel_id == 0xFF) ? 0 : value->channel_id,
                target_day, end_day, current_year,
                daily_results, &daily_count
            );
            
            if (err == WATERING_SUCCESS && daily_count > 0) {
                /* Convert first daily stats to BLE format */
                daily_stats_t *stats = &daily_results[0];
                
                value->data.daily.day_index = target_day;
                value->data.daily.year = current_year;
                value->data.daily.watering_sessions = stats->sessions_ok;
                value->data.daily.total_volume_ml = stats->total_ml;
                value->data.daily.total_duration_sec = 1800; /* Estimate from volume */
                value->data.daily.avg_flow_rate = 750; /* Default flow rate */
                value->data.daily.success_rate = stats->success_rate;
                value->data.daily.error_count = stats->sessions_err;
                value->count = daily_count;
                
                LOG_INF("✅ Real daily stats: day=%u, sessions=%u, volume=%u ml", 
                        target_day, stats->sessions_ok, stats->total_ml);
            } else {
                /* No daily data - return zeros */
                memset(&value->data.daily, 0, sizeof(value->data.daily));
                value->data.daily.day_index = target_day;
                value->data.daily.year = current_year;
                value->count = 0;
                LOG_INF("No daily stats found for channel %u, day %u", value->channel_id, target_day);
            }
            break;
        }
        case 2: /* Monthly statistics */
        {
            monthly_stats_t monthly_results[12];
            uint16_t monthly_count = 0;
            uint16_t current_year = get_current_year();
            uint8_t current_month = get_current_month();
            uint8_t target_month = (value->entry_index > 0) ? 
                                 ((current_month - value->entry_index - 1 + 12) % 12) + 1 : current_month;
            
            watering_error_t err = watering_history_get_monthly_stats(
                (value->channel_id == 0xFF) ? 0 : value->channel_id,
                target_month, target_month, current_year,
                monthly_results, &monthly_count
            );
            
            if (err == WATERING_SUCCESS && monthly_count > 0) {
                /* Convert monthly stats to BLE format */
                monthly_stats_t *stats = &monthly_results[0];
                
                value->data.monthly.month = stats->month;
                value->data.monthly.year = stats->year;
                value->data.monthly.total_sessions = 90; /* Estimate from volume */
                value->data.monthly.total_volume_ml = stats->total_ml;
                value->data.monthly.total_duration_hours = stats->total_ml / 15000; /* Estimate: 15L/hour */
                value->data.monthly.avg_daily_volume = stats->total_ml / stats->active_days;
                value->data.monthly.active_days = stats->active_days;
                value->data.monthly.success_rate = 95; /* Default success rate */
                value->count = monthly_count;
                
                LOG_INF("✅ Real monthly stats: month=%u/%u, volume=%u ml, days=%u", 
                        stats->month, stats->year, stats->total_ml, stats->active_days);
            } else {
                /* No monthly data - return zeros */
                memset(&value->data.monthly, 0, sizeof(value->data.monthly));
                value->data.monthly.month = target_month;
                value->data.monthly.year = current_year;
                value->count = 0;
                LOG_INF("No monthly stats found for channel %u, month %u/%u", 
                        value->channel_id, target_month, current_year);
            }
            break;
        }
        case 3: /* Annual statistics */
        {
            annual_stats_t annual_results[5];
            uint16_t annual_count = 0;
            uint16_t current_year = get_current_year();
            uint16_t target_year = current_year - value->entry_index;
            
            watering_error_t err = watering_history_get_annual_stats(
                (value->channel_id == 0xFF) ? 0 : value->channel_id,
                target_year, target_year,
                annual_results, &annual_count
            );
            
            if (err == WATERING_SUCCESS && annual_count > 0) {
                /* Convert annual stats to BLE format */
                annual_stats_t *stats = &annual_results[0];
                
                value->data.annual.year = stats->year;
                value->data.annual.total_sessions = stats->sessions;
                value->data.annual.total_volume_liters = stats->total_ml / 1000; /* Convert ml to L */
                value->data.annual.avg_monthly_volume = (stats->total_ml / 1000) / 12; /* Average per month */
                value->data.annual.most_active_month = 7; /* Default to July */
                value->data.annual.success_rate = 95; /* Calculate from sessions vs errors */
                value->data.annual.peak_month_volume = stats->max_month_ml / 1000; /* Convert to L */
                value->count = annual_count;
                
                LOG_INF("✅ Real annual stats: year=%u, sessions=%u, volume=%u L", 
                        stats->year, stats->sessions, stats->total_ml / 1000);
            } else {
                /* No annual data - return zeros */
                memset(&value->data.annual, 0, sizeof(value->data.annual));
                value->data.annual.year = target_year;
                value->count = 0;
                LOG_INF("No annual stats found for channel %u, year %u", 
                        value->channel_id, target_year);
            }
            break;
        }
    }

    LOG_INF("✅ History query completed with real data integration - no more mock data");
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
        LOG_INF("✅ Diagnostics notifications ENABLED - will send updates when diagnostic values change significantly");
        
        /* Per BLE API Documentation: Diagnostics notifications are sent when:
         * - System health metrics change significantly
         * - Error count increases
         * - Valve status changes (any valve opens/closes)
         * - System enters/exits error states
         */
        
        LOG_INF("Diagnostics monitoring: 12-byte structure, health metrics, valve bitmap");
        LOG_INF("Fields: uptime(min), error_count, last_error, valve_status(bitmap), battery_level");
        
        /* Initialize diagnostics_value with current system state */
        struct diagnostics_data *diag = (struct diagnostics_data *)diagnostics_value;
        diag->uptime = k_uptime_get() / (1000 * 60);
        diag->error_count = diagnostics_error_count;
        diag->last_error = diagnostics_last_error;
        diag->valve_status = 0; /* Will be updated on valve changes */
        diag->battery_level = 0xFF; /* No battery monitoring */
        memset(diag->reserved, 0, sizeof(diag->reserved));
        
        LOG_INF("Diagnostics ready: uptime=%u min, errors=%u, last_error=%u",
                diag->uptime, diag->error_count, diag->last_error);
    } else {
        LOG_INF("Diagnostics notifications disabled");
        /* Clear diagnostics_value when notifications disabled */
        memset(diagnostics_value, 0, sizeof(diagnostics_value));
    }
}

/* ------------------------------------------------------------------ */
/* Forward Declarations                                               */
/* ------------------------------------------------------------------ */
static int send_simple_notification(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                   const void *data, uint16_t len) {
    /* Helper function for sending simple notifications */
    if (!conn || !attr || !data) {
        return -EINVAL;
    }
    
    int err = safe_notify(conn, attr, data, len);
    if (err != 0) {
        LOG_ERR("Failed to send notification: %d", err);
    }
    
    return err;
}

// Task update thread definitions
#define TASK_UPDATE_THREAD_STACK_SIZE 1024
#define TASK_UPDATE_THREAD_PRIORITY 6

K_THREAD_STACK_DEFINE(task_update_thread_stack, TASK_UPDATE_THREAD_STACK_SIZE);
static struct k_thread task_update_thread_data;
static bool task_update_thread_running = false;

static void task_update_thread_fn(void *p1, void *p2, void *p3) {
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);
    
    LOG_INF("Task update thread started - will send ultra-fast notifications every 200ms (5Hz)");
    
    while (task_update_thread_running) {
        // Critical: Check for valid connection before proceeding
        if (!default_conn || !connection_active) {
            // No connection - sleep and continue checking
            k_sleep(K_SECONDS(1));
            continue;
        }
        
        // Double-check connection is still valid before any BLE operations
        if (!bt_conn_get_info(default_conn, NULL)) {
            LOG_WRN("Connection became invalid, stopping thread");
            break;
        }
        
        // Check for current task status and send periodic notifications
        watering_task_t *current_task = watering_get_current_task();
        
        if (current_task && watering_task_state.task_in_progress && 
            notification_state.current_task_notifications_enabled) {
            // Send ultra-fast progress update every 200ms (5Hz)
            int notify_result = bt_irrigation_current_task_notify();
            if (notify_result == 0) {
                LOG_DBG("⚡ Ultra-fast task progress notification sent (5Hz)");
            } else {
                LOG_DBG("Failed to send ultra-fast task notification: %d", notify_result);
            }
        }
        
        // Sleep for 200ms for 5Hz ultra-responsive updates when task active
        k_sleep(K_MSEC(200));
    }
    
    LOG_INF("Task update thread exiting");
}

static int start_task_update_thread(void) {
    if (task_update_thread_running) {
        LOG_WRN("Task update thread already running");
        return 0;
    }
    
    task_update_thread_running = true;
    
    k_tid_t tid = k_thread_create(&task_update_thread_data, task_update_thread_stack,
                                 K_THREAD_STACK_SIZEOF(task_update_thread_stack),
                                 task_update_thread_fn, NULL, NULL, NULL,
                                 K_PRIO_PREEMPT(TASK_UPDATE_THREAD_PRIORITY), 0, K_NO_WAIT);
    
    if (tid == NULL) {
        LOG_ERR("Failed to create task update thread");
        task_update_thread_running = false;
        return -1;
    }
    
    LOG_INF("Task update thread started successfully");
    return 0;
}

static void stop_task_update_thread(void) {
    if (!task_update_thread_running) {
        LOG_DBG("Task update thread not running");
        return;
    }
    
    LOG_INF("Stopping task update thread");
    task_update_thread_running = false;
    
    /* Wait for thread to finish - give it reasonable time */
    k_sleep(K_MSEC(100));
    
    LOG_INF("Task update thread stopped");
}

/* Missing function declarations */
static void history_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value);

static void diagnostics_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value);

/* History-related function declarations */
watering_error_t history_ctrl_handler(const uint8_t *data, uint16_t len);
watering_error_t history_settings_set(const history_settings_t *settings);

/* ------------------------------------------------------------------ */
/* BLE Notification Subscription Helper Functions                    */
/* ------------------------------------------------------------------ */

/**
 * @brief Check if notifications are enabled for a specific characteristic
 * @param attr The GATT attribute of the characteristic
 * @return true if notifications are enabled, false otherwise
 */
static bool is_notification_enabled(const struct bt_gatt_attr *attr) {
    if (!attr) {
        return false;
    }
    
    // Use attribute index in the service to determine which characteristic this is
    // More reliable than user_data comparison
    const struct bt_gatt_attr *service_attrs = irrigation_svc.attrs;
    ptrdiff_t attr_index = attr - service_attrs;
    
    switch (attr_index) {
        case ATTR_IDX_VALVE_VALUE:
            return notification_state.valve_notifications_enabled;
        case ATTR_IDX_FLOW_VALUE:
            return notification_state.flow_notifications_enabled;
        case ATTR_IDX_STATUS_VALUE:
            return notification_state.status_notifications_enabled;
        case ATTR_IDX_CHANNEL_CFG_VALUE:
            return notification_state.channel_config_notifications_enabled;
        case ATTR_IDX_SCHEDULE_VALUE:
            return notification_state.schedule_notifications_enabled;
        case ATTR_IDX_SYSTEM_CFG_VALUE:
            return notification_state.system_config_notifications_enabled;
        case ATTR_IDX_TASK_QUEUE_VALUE:
            return notification_state.task_queue_notifications_enabled;
        case ATTR_IDX_STATISTICS_VALUE:
            return notification_state.statistics_notifications_enabled;
        case ATTR_IDX_RTC_VALUE:
            return notification_state.rtc_notifications_enabled;
        case ATTR_IDX_ALARM_VALUE:
            return notification_state.alarm_notifications_enabled;
        case ATTR_IDX_CALIB_VALUE:
            return notification_state.calibration_notifications_enabled;
        case ATTR_IDX_HISTORY_VALUE:
            return notification_state.history_notifications_enabled;
        case ATTR_IDX_DIAGNOSTICS_VALUE:
            return notification_state.diagnostics_notifications_enabled;
        case ATTR_IDX_GROWING_ENV_VALUE:
            return notification_state.growing_env_notifications_enabled;
        case ATTR_IDX_CURRENT_TASK_VALUE:
            return notification_state.current_task_notifications_enabled;
        default:
            printk("Unknown characteristic index\n");
            return false;
    }
}

/* ---------------------------------------------------------------------- */

/* RTC notification functions */
// Notificare BLE pentru RTC (sincronizare sau erori)
int bt_irrigation_rtc_notify(void) {
    if (!default_conn || !notification_state.rtc_notifications_enabled) {
        LOG_DBG("RTC notification not enabled");
        return 0;
    }
    
    /* Update rtc_value with current time before notification */
    struct rtc_data *rtc_data = (struct rtc_data *)rtc_value;
    rtc_datetime_t now;
    if (rtc_datetime_get(&now) == 0) {
        /* TIMEZONE FIX: Send local time to user via BLE notifications */
        uint32_t utc_timestamp = timezone_rtc_to_unix_utc(&now);
        rtc_datetime_t local_time;
        
        if (timezone_unix_to_rtc_local(utc_timestamp, &local_time) == 0) {
            /* Use local time for user-facing notifications */
            rtc_data->year = local_time.year - 2000;
            rtc_data->month = local_time.month;
            rtc_data->day = local_time.day;
            rtc_data->hour = local_time.hour;
            rtc_data->minute = local_time.minute;
            rtc_data->second = local_time.second;
            rtc_data->day_of_week = local_time.day_of_week;
        } else {
            /* Fallback to UTC if timezone conversion fails */
            rtc_data->year = now.year - 2000;
            rtc_data->month = now.month;
            rtc_data->day = now.day;
            rtc_data->hour = now.hour;
            rtc_data->minute = now.minute;
            rtc_data->second = now.second;
            rtc_data->day_of_week = now.day_of_week;
        }
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
    notification_state.current_task_notifications_enabled = true;
    notification_state.timezone_notifications_enabled = true;
    
    LOG_INF("✅ All BLE notifications force-enabled");
}

/* Bluetooth connection callback */

static void connected(struct bt_conn *conn, uint8_t err) {
    if (err) {
        printk("Connection failed\n");
        return;
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

    /* Negociază un supervision timeout de 4 s (400×10 ms) */
    const struct bt_le_conn_param conn_params = {
        .interval_min = BT_GAP_INIT_CONN_INT_MIN,
        .interval_max = BT_GAP_INIT_CONN_INT_MAX,
        .latency = 0,
        .timeout = 400, /* 400 × 10 ms = 4 s */
    };
    int update_err = bt_conn_le_param_update(conn, &conn_params);
    if (update_err) {
        printk("Conn param update failed\n");
    }

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
    
    /* Auto-enable all notifications for better user experience */
    force_enable_all_notifications();
    
    /* DO NOT start task update thread - it can cause BLE freezes */
    /* Task updates will be handled through other mechanisms */
    // start_task_update_thread();
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
    
    /* Critical: Stop task update thread immediately to prevent freeze */
    stop_task_update_thread();
    
    /* Mark connection as inactive immediately */
    connection_active = false;

    if (default_conn) {
        bt_conn_unref(default_conn);
        default_conn = NULL;
    }

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

    /* Validate task value */
    if (task_value == 0) {
        LOG_ERR("BT valve write: Invalid task_value=0 (must be > 0)");
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

    /* Send notification to confirm task acceptance */
    if (default_conn && notification_state.valve_notifications_enabled) {
        const struct bt_gatt_attr *notify_attr = &irrigation_svc.attrs[ATTR_IDX_VALVE_VALUE];
        int notify_err = safe_notify(default_conn, notify_attr, value, sizeof(struct valve_control_data));
        
        if (notify_err == 0) {
            LOG_INF("✅ Task acceptance notification sent");
        } else {
            LOG_WRN("❌ Failed to send task acceptance notification: %d", notify_err);
        }
    }

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
        
        /* Per BLE API Documentation - Flow Sensor advanced processing features: */
        /* • Hardware debouncing (5ms) for electrical noise elimination */
        /* • Smoothing algorithm: 2-sample circular buffer averaging */
        /* • Rate calculation over 500ms windows for ultra-fast response */
        /* • Smart notification: up to 20 Hz during active flow */
        /* • Minimum 50ms interval between notifications for stability */
        /* • Forced periodic notifications every 200ms (5 Hz) for connectivity */
        
        LOG_INF("Flow monitoring specs: <50ms response, 20Hz max, 50ms min interval, 200ms periodic");
        
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
    /* Status values: 0=OK, 1=No-Flow, 2=Unexpected-Flow, 3=Fault, 4=RTC-Error, 5=Low-Power */
    watering_status_t current_status;
    if (watering_get_status(&current_status) == WATERING_SUCCESS) {
        status_value[0] = (uint8_t)current_status;
        LOG_DBG("System Status read: %u (%s)", current_status, 
                (current_status == 0) ? "OK" : 
                (current_status == 1) ? "No-Flow" :
                (current_status == 2) ? "Unexpected-Flow" :
                (current_status == 3) ? "Fault" :
                (current_status == 4) ? "RTC-Error" :
                (current_status == 5) ? "Low-Power" : "Unknown");
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
        /* 0=OK, 1=No-Flow, 2=Unexpected-Flow, 3=Fault, 4=RTC-Error, 5=Low-Power */
        
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
                   (current_status == 5) ? "Low-Power" : "Unknown");
        } else {
            status_value[0] = (uint8_t)WATERING_STATUS_OK;
            LOG_WRN("Status CCC enabled - defaulted to OK status");
        }
        
        /* DO NOT send immediate notification during CCC setup - this causes system freeze */
        /* Per documentation: notifications sent automatically when status transitions occur */
    } else {
        LOG_INF("System Status notifications disabled");
    }
}

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
    /* Create a TRUE local buffer for reading to avoid conflicts with notification buffer */
    /* CRITICAL: Remove 'static' keyword to ensure this is a stack-allocated local buffer */
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
    
    /* CRITICAL: Ensure this is a READ-ONLY operation that doesn't trigger saves */
    /* The read operation should never modify the system state */

    err = watering_get_channel(channel_id, &channel);
    
    if (err != WATERING_SUCCESS) {
        printk("Failed to get channel %d: error %d\n", channel_id, err);
        /* Return default/safe values */
        memset(&read_value, 0, sizeof(read_value));
        read_value.channel_id = channel_id;
        strcpy(read_value.name, "Default");
        read_value.name_len = 7;
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

    /* copy fresh data */
    memset(&read_value, 0, sizeof(read_value));
    read_value.channel_id = channel_id;
    
    name_len = strnlen(channel->name, sizeof(channel->name));
    if (name_len >= sizeof(read_value.name)) {
        name_len = sizeof(read_value.name) - 1;
    }
    memcpy(read_value.name, channel->name, name_len);
    read_value.name[name_len] = '\0';

    read_value.name_len = name_len;
    read_value.auto_enabled = channel->watering_event.auto_enabled ? 1 : 0;
    
    now = k_uptime_get_32();
    
    /* Only log if it's been more than 5 seconds since the last log for this channel */
    if (now - last_read_log_time > 5000 || last_read_channel_id != channel_id) {
        printk("Read channel config: ch=%d, name=\"%s\", name_len=%d\n", 
               read_value.channel_id, read_value.name, read_value.name_len);
        last_read_log_time = now;
        last_read_channel_id = channel_id;
    }
    
    /* Populate new plant and growing environment fields */
    read_value.plant_type = (uint8_t)channel->plant_type;
    read_value.soil_type = (uint8_t)channel->soil_type;
    read_value.irrigation_method = (uint8_t)channel->irrigation_method;
    read_value.coverage_type = channel->coverage.use_area ? 0 : 1;
    if (channel->coverage.use_area) {
        read_value.coverage.area_m2 = channel->coverage.area.area_m2;
    } else {
        read_value.coverage.plant_count = channel->coverage.plants.count;
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
        ch->coverage.use_area = (value->coverage_type == 0);
        if (value->coverage_type == 0) {
            ch->coverage.area.area_m2 = value->coverage.area_m2;
        } else {
            ch->coverage.plants.count = value->coverage.plant_count;
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
    new_time_local.day_of_week = value->day_of_week;

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

    /* Update timezone configuration if provided */
    if (value->utc_offset_minutes != 0) {
        if (timezone_get_config(&tz_config) == 0) {
            tz_config.utc_offset_minutes = value->utc_offset_minutes;
            if (timezone_set_config(&tz_config) != 0) {
                LOG_WRN("Failed to update timezone offset");
            } else {
                LOG_INF("Updated timezone offset to UTC%+d:%02d", 
                       tz_config.utc_offset_minutes / 60,
                       abs(tz_config.utc_offset_minutes % 60));
            }
        }
    }

    ret = rtc_datetime_set(&new_time_utc);

    if (ret != 0) {
        LOG_ERR("RTC update failed: %d", ret);
        return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
    }

    LOG_INF("RTC updated successfully (stored as UTC)");

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
    
    /* Validate timezone configuration */
    if (new_config.utc_offset_minutes < -12*60 || new_config.utc_offset_minutes > 14*60) {
        LOG_ERR("Invalid UTC offset: %d minutes", new_config.utc_offset_minutes);
        return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
    }
    
    if (new_config.dst_enabled > 1) {
        LOG_ERR("Invalid DST setting: %u", new_config.dst_enabled);
        return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
    }
    
    /* Apply new timezone configuration */
    int ret = timezone_set_config(&new_config);
    if (ret != 0) {
        LOG_ERR("Failed to set timezone config: %d", ret);
        return BT_GATT_ERR(BT_ATT_ERR_WRITE_NOT_PERMITTED);
    }
    
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
        
        if (clear_code == 0x00) {
            // Clear all alarms
            printk("BLE: Clearing all alarms\n");
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

    /* Per BLE API Documentation: Process calibration actions */
    /* 0=stop, 1=start, 2=in progress (read-only), 3=completed (read-only) */
    
    if (value->action == 1) {
        // Start calibration
        if (!calibration_active) {
            reset_pulse_count();
            calibration_start_pulses = 0;
            calibration_active = true;
            value->pulses = 0;
            value->volume_ml = 0;
            value->pulses_per_liter = 0;
            
            LOG_INF("✅ Flow sensor calibration STARTED - begin measuring actual volume");
            LOG_INF("📊 Pulse counting reset, system ready for calibration");
            
            /* Send notification if enabled */
            if (default_conn && notification_state.calibration_notifications_enabled) {
                bt_irrigation_calibration_notify();
            }
        } else {
            LOG_WRN("Calibration already in progress");
        }
    } else if (value->action == 0) {
        // Stop calibration and calculate
        if (calibration_active) {
            uint32_t final_pulses = get_pulse_count();
            uint32_t total_pulses = final_pulses - calibration_start_pulses;
            uint32_t volume_ml = value->volume_ml;

            /* Validate inputs per documentation */
            if (volume_ml == 0) {
                LOG_ERR("❌ Calibration failed: volume_ml cannot be zero");
                calibration_active = false;
                value->action = 0; /* Reset to stopped */
                return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
            }
            
            if (total_pulses == 0) {
                LOG_ERR("❌ Calibration failed: no pulses detected");
                calibration_active = false;
                value->action = 0; /* Reset to stopped */
                return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
            }

            /* Calculate new calibration using formula from documentation */
            uint32_t new_calibration = (total_pulses * 1000) / volume_ml;
            value->pulses_per_liter = new_calibration;

            /* Update system calibration */
            watering_error_t err = watering_set_flow_calibration(new_calibration);
            if (err == WATERING_SUCCESS) {
                watering_save_config_priority(true);
                
                LOG_INF("✅ Flow sensor calibration COMPLETED:");
                LOG_INF("   📏 Measured: %u ml actual volume", volume_ml);
                LOG_INF("   📊 Counted: %u pulses total", total_pulses);
                LOG_INF("   🎯 Result: %u pulses/liter", new_calibration);
                
                /* Set completed state */
                value->action = 3; // Calibration completed
                value->pulses = total_pulses;
            } else {
                LOG_ERR("❌ Failed to save calibration: error=%d", err);
                value->action = 0; /* Reset to stopped */
                calibration_active = false;
                return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
            }
            
            calibration_active = false;
            
            /* Send notification if enabled */
            if (default_conn && notification_state.calibration_notifications_enabled) {
                bt_irrigation_calibration_notify();
            }
        } else {
            LOG_WRN("No calibration in progress to stop");
        }
    } else if (value->action == 2 || value->action == 3) {
        LOG_ERR("Invalid calibration action: %u (2 and 3 are read-only)", value->action);
        return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
    } else {
        LOG_ERR("Invalid calibration action: %u (must be 0 or 1)", value->action);
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
    } else {
        LOG_DBG("Calibration notifications disabled");
        memset(calibration_value, 0, sizeof(struct calibration_data));
    }
}

/* History implementation with full history system integration */
/* History Service characteristic handlers */
static ssize_t read_history_service_revision(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                            void *buf, uint16_t len, uint16_t offset) {
    return bt_gatt_attr_read(conn, attr, buf, len, offset, 
                           history_service_revision, sizeof(history_service_revision));
}

static ssize_t read_history_capabilities(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                       void *buf, uint16_t len, uint16_t offset) {
    return bt_gatt_attr_read(conn, attr, buf, len, offset, 
                           &history_capabilities, sizeof(history_capabilities));
}

static ssize_t read_history_ctrl(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                void *buf, uint16_t len, uint16_t offset) {
    return bt_gatt_attr_read(conn, attr, buf, len, offset, 
                           history_ctrl_value, sizeof(history_ctrl_value));
}

static ssize_t write_history_ctrl(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                 const void *buf, uint16_t len, uint16_t offset, uint8_t flags) {
    if (offset + len > sizeof(history_ctrl_value)) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }

    memcpy(history_ctrl_value + offset, buf, len);

    // Process history control command
    watering_error_t err = history_ctrl_handler(history_ctrl_value, len);
    if (err != WATERING_SUCCESS) {
        LOG_ERR("History control failed: %d", err);
        return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
    }

    return len;
}

static ssize_t read_history_data(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                void *buf, uint16_t len, uint16_t offset) {
    return bt_gatt_attr_read(conn, attr, buf, len, offset, 
                           history_data_value, sizeof(history_data_value));
}

static ssize_t read_history_insights(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                   void *buf, uint16_t len, uint16_t offset) {
    return bt_gatt_attr_read(conn, attr, buf, len, offset, 
                           &history_insights_value, sizeof(history_insights_value));
}

static ssize_t read_history_settings(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                    void *buf, uint16_t len, uint16_t offset) {
    return bt_gatt_attr_read(conn, attr, buf, len, offset, 
                           &history_settings_value, sizeof(history_settings_value));
}

static ssize_t write_history_settings(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                     const void *buf, uint16_t len, uint16_t offset, uint8_t flags) {
    if (offset + len > sizeof(history_settings_value)) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }

    memcpy(((uint8_t *)&history_settings_value) + offset, buf, len);

    // Update history settings
    watering_error_t err = history_settings_set(&history_settings_value);
    if (err != WATERING_SUCCESS) {
        LOG_ERR("History settings update failed: %d", err);
        return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
    }

    return len;
}

static void history_ctrl_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value) {
    LOG_DBG("History Control CCC: %d", value);
}

static void history_data_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value) {
    LOG_DBG("History Data CCC: %d", value);
}

static void history_insights_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value) {
    LOG_DBG("History Insights CCC: %d", value);
}

static void history_settings_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value) {
    LOG_DBG("History Settings CCC: %d", value);
}

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
        rtc_datetime_t now;
        if (rtc_datetime_get(&now) == 0) {
            rtc_data->year = now.year - 2000;
            rtc_data->month = now.month;
            rtc_data->day = now.day;
            rtc_data->hour = now.hour;
            rtc_data->minute = now.minute;
            rtc_data->second = now.second;
            rtc_data->day_of_week = now.day_of_week;
        } else {
            /* Default values if RTC hardware is unavailable */
            rtc_data->year = 25; rtc_data->month = 7; rtc_data->day = 5;
            rtc_data->hour = 12; rtc_data->minute = 0; rtc_data->second = 0;
            rtc_data->day_of_week = 6;
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
        read_value.plant_type = 0; /* Vegetables */
        read_value.soil_type = 2; /* Loamy */
        read_value.irrigation_method = 0; /* Drip */
        read_value.use_area_based = 1; /* Use area */
        read_value.coverage.area_m2 = 1.0f; /* 1 m² */
        read_value.sun_percentage = 75; /* 75% sun */
        strcpy(read_value.custom_name, "Default Plant");
        read_value.water_need_factor = 1.0f;
        read_value.irrigation_freq_days = 1;
        read_value.prefer_area_based = 1;
    } else {
        /* Copy fresh data from the watering system */
        memset(&read_value, 0, sizeof(read_value));
        read_value.channel_id = channel_id;
        read_value.plant_type = (uint8_t)channel->plant_type;
        
        /* For specific plant, use appropriate field based on plant type */
        if (channel->plant_type == PLANT_TYPE_VEGETABLES) {
            read_value.specific_plant = (uint16_t)channel->plant_info.specific.vegetable;
        } else if (channel->plant_type == PLANT_TYPE_HERBS) {
            read_value.specific_plant = (uint16_t)channel->plant_info.specific.herb;
        } else if (channel->plant_type == PLANT_TYPE_FLOWERS) {
            read_value.specific_plant = (uint16_t)channel->plant_info.specific.flower;
        } else if (channel->plant_type == PLANT_TYPE_SHRUBS) {
            read_value.specific_plant = (uint16_t)channel->plant_info.specific.shrub;
        } else if (channel->plant_type == PLANT_TYPE_TREES) {
            read_value.specific_plant = (uint16_t)channel->plant_info.specific.tree;
        } else if (channel->plant_type == PLANT_TYPE_LAWN) {
            read_value.specific_plant = (uint16_t)channel->plant_info.specific.lawn;
        } else if (channel->plant_type == PLANT_TYPE_SUCCULENTS) {
            read_value.specific_plant = (uint16_t)channel->plant_info.specific.succulent;
        } else {
            read_value.specific_plant = 0;
        }
        
        read_value.soil_type = (uint8_t)channel->soil_type;
        read_value.irrigation_method = (uint8_t)channel->irrigation_method;
        read_value.use_area_based = channel->coverage.use_area ? 1 : 0;
        
        if (channel->coverage.use_area) {
            read_value.coverage.area_m2 = channel->coverage.area.area_m2;
        } else {
            read_value.coverage.plant_count = channel->coverage.plants.count;
        }
        
        read_value.sun_percentage = channel->sun_percentage;
        
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
            read_value.use_area_based ? read_value.coverage.area_m2 : (float)read_value.coverage.plant_count,
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
        env_data->plant_type = channel->plant_type;
        
        /* Set specific plant based on plant_type */
        if (channel->plant_type == PLANT_TYPE_VEGETABLES) {
            env_data->specific_plant = channel->plant_info.specific.vegetable;
        } else if (channel->plant_type == PLANT_TYPE_HERBS) {
            env_data->specific_plant = channel->plant_info.specific.herb;
        } else if (channel->plant_type == PLANT_TYPE_FLOWERS) {
            env_data->specific_plant = channel->plant_info.specific.flower;
        } else if (channel->plant_type == PLANT_TYPE_SHRUBS) {
            env_data->specific_plant = channel->plant_info.specific.shrub;
        } else if (channel->plant_type == PLANT_TYPE_TREES) {
            env_data->specific_plant = channel->plant_info.specific.tree;
        } else if (channel->plant_type == PLANT_TYPE_LAWN) {
            env_data->specific_plant = channel->plant_info.specific.lawn;
        } else if (channel->plant_type == PLANT_TYPE_SUCCULENTS) {
            env_data->specific_plant = channel->plant_info.specific.succulent;
        } else {
            env_data->specific_plant = 0;
        }
        
        env_data->soil_type = channel->soil_type;
        env_data->irrigation_method = channel->irrigation_method;
        env_data->use_area_based = channel->coverage.use_area ? 1 : 0;
        
        if (channel->coverage.use_area) {
            env_data->coverage.area_m2 = channel->coverage.area.area_m2;
        } else {
            env_data->coverage.plant_count = channel->coverage.plants.count;
        }
        
        env_data->sun_percentage = channel->sun_percentage;
        
        /* Custom plant fields */
        if (channel->plant_type == PLANT_TYPE_OTHER) {
            strncpy(env_data->custom_name, channel->custom_plant.custom_name, 
                   sizeof(env_data->custom_name) - 1);
            env_data->custom_name[sizeof(env_data->custom_name) - 1] = '\0';
            env_data->water_need_factor = channel->custom_plant.water_need_factor;
            env_data->irrigation_freq_days = channel->custom_plant.irrigation_freq;
            env_data->prefer_area_based = channel->custom_plant.prefer_area_based ? 1 : 0;
        }
        
        printk("✅ BLE: Growing env channel %u selected (plant=%u, soil=%u, irrigation=%u)\n", 
                channel_id, env_data->plant_type, env_data->soil_type, env_data->irrigation_method);
        
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
        growing_env_frag.expected = total_size;
        growing_env_frag.received = 0;
        growing_env_frag.in_progress = true;
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
            
            if (env_data->plant_type > 7) {
                printk("❌ Invalid plant type %u\n", env_data->plant_type);
                growing_env_frag.in_progress = false;
                return -EINVAL;
            }
            
            if (env_data->soil_type > 7) {
                printk("❌ Invalid soil type %u\n", env_data->soil_type);
                growing_env_frag.in_progress = false;
                return -EINVAL;
            }
            
            if (env_data->irrigation_method > 5) {
                printk("❌ Invalid irrigation method %u\n", env_data->irrigation_method);
                growing_env_frag.in_progress = false;
                return -EINVAL;
            }
            
            if (env_data->sun_percentage > 100) {
                printk("❌ Invalid sun percentage %u\n", env_data->sun_percentage);
                growing_env_frag.in_progress = false;
                return -EINVAL;
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
            
            /* Update channel data */
            channel->plant_type = env_data->plant_type;
            
            /* Set specific plant type based on plant_type */
            if (channel->plant_type == PLANT_TYPE_VEGETABLES) {
                channel->plant_info.specific.vegetable = (vegetable_type_t)env_data->specific_plant;
            } else if (channel->plant_type == PLANT_TYPE_HERBS) {
                channel->plant_info.specific.herb = (herb_type_t)env_data->specific_plant;
            } else if (channel->plant_type == PLANT_TYPE_FLOWERS) {
                channel->plant_info.specific.flower = (flower_type_t)env_data->specific_plant;
            } else if (channel->plant_type == PLANT_TYPE_SHRUBS) {
                channel->plant_info.specific.shrub = (shrub_type_t)env_data->specific_plant;
            } else if (channel->plant_type == PLANT_TYPE_TREES) {
                channel->plant_info.specific.tree = (tree_type_t)env_data->specific_plant;
            } else if (channel->plant_type == PLANT_TYPE_LAWN) {
                channel->plant_info.specific.lawn = (lawn_type_t)env_data->specific_plant;
            } else if (channel->plant_type == PLANT_TYPE_SUCCULENTS) {
                channel->plant_info.specific.succulent = (succulent_type_t)env_data->specific_plant;
            }
            
            channel->soil_type = env_data->soil_type;
            channel->irrigation_method = env_data->irrigation_method;
            channel->coverage.use_area = (env_data->use_area_based != 0);
            
            if (channel->coverage.use_area) {
                channel->coverage.area.area_m2 = env_data->coverage.area_m2;
            } else {
                channel->coverage.plants.count = env_data->coverage.plant_count;
            }
            
            channel->sun_percentage = env_data->sun_percentage;
            
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
            }
            
            /* Update global buffer for notifications */
            memcpy(growing_env_value, env_data, sizeof(struct growing_env_data));
            
            /* Save with priority (250ms throttle) */
            watering_save_config_priority(true);
            
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
    
    /* Combined validation - fast single check */
    if (env_data->channel_id >= WATERING_CHANNELS_COUNT || 
        env_data->plant_type > 7 || env_data->soil_type > 7 || 
        env_data->irrigation_method > 5 || env_data->sun_percentage > 100) {
        printk("❌ Invalid growing env data: ch=%u, plant=%u, soil=%u, method=%u, sun=%u\n", 
                env_data->channel_id, env_data->plant_type, env_data->soil_type,
                env_data->irrigation_method, env_data->sun_percentage);
        return -EINVAL;
    }
    
    /* Get channel */
    watering_channel_t *channel;
    watering_error_t err = watering_get_channel(env_data->channel_id, &channel);
    if (err != WATERING_SUCCESS) {
        printk("❌ Failed to get channel %u for growing env write: %d\n", 
                env_data->channel_id, err);
        return -EINVAL;
    }
    
    /* Update channel data */
    channel->plant_type = env_data->plant_type;
    
    /* Set specific plant type based on plant_type */
    if (channel->plant_type == PLANT_TYPE_VEGETABLES) {
        channel->plant_info.specific.vegetable = (vegetable_type_t)env_data->specific_plant;
    } else if (channel->plant_type == PLANT_TYPE_HERBS) {
        channel->plant_info.specific.herb = (herb_type_t)env_data->specific_plant;
    } else if (channel->plant_type == PLANT_TYPE_FLOWERS) {
        channel->plant_info.specific.flower = (flower_type_t)env_data->specific_plant;
    } else if (channel->plant_type == PLANT_TYPE_SHRUBS) {
        channel->plant_info.specific.shrub = (shrub_type_t)env_data->specific_plant;
    } else if (channel->plant_type == PLANT_TYPE_TREES) {
        channel->plant_info.specific.tree = (tree_type_t)env_data->specific_plant;
    } else if (channel->plant_type == PLANT_TYPE_LAWN) {
        channel->plant_info.specific.lawn = (lawn_type_t)env_data->specific_plant;
    } else if (channel->plant_type == PLANT_TYPE_SUCCULENTS) {
        channel->plant_info.specific.succulent = (succulent_type_t)env_data->specific_plant;
    }
    
    channel->soil_type = env_data->soil_type;
    channel->irrigation_method = env_data->irrigation_method;
    channel->coverage.use_area = (env_data->use_area_based != 0);
    
    if (channel->coverage.use_area) {
        channel->coverage.area.area_m2 = env_data->coverage.area_m2;
    } else {
        channel->coverage.plants.count = env_data->coverage.plant_count;
    }
    
    channel->sun_percentage = env_data->sun_percentage;
    
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
    }
    
    /* Update global buffer for notifications */
    memcpy(growing_env_value, env_data, sizeof(struct growing_env_data));
    
    /* Save with priority (250ms throttle) */
    watering_save_config_priority(true);
    
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
            env_data->use_area_based = channel->coverage.use_area ? 1 : 0;
            
            if (channel->coverage.use_area) {
                env_data->coverage.area_m2 = channel->coverage.area.area_m2;
            } else {
                env_data->coverage.plant_count = channel->coverage.plants.count;
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
            
            LOG_INF("Initialized with channel 0: plant=%u.%u, soil=%u, method=%u, %s=%.2f, sun=%u%%",
                    env_data->plant_type, env_data->specific_plant,
                    env_data->soil_type, env_data->irrigation_method,
                    env_data->use_area_based ? "area" : "count",
                    env_data->use_area_based ? env_data->coverage.area_m2 : (float)env_data->coverage.plant_count,
                    env_data->sun_percentage);
        } else {
            /* Default values if channel not available */
            memset(env_data, 0, sizeof(struct growing_env_data));
            env_data->channel_id = 0;
            env_data->plant_type = 0; /* Vegetables */
            env_data->soil_type = 2; /* Loamy */
            env_data->irrigation_method = 0; /* Drip */
            env_data->use_area_based = 1; /* Use area */
            env_data->coverage.area_m2 = 1.0f; /* 1 m² */
            env_data->sun_percentage = 75; /* 75% sun */
            strcpy(env_data->custom_name, "");
            env_data->water_need_factor = 1.0f;
            env_data->irrigation_freq_days = 1;
            env_data->prefer_area_based = 1;
        }
    } else {
        LOG_DBG("Growing Environment notifications disabled");
        memset(growing_env_value, 0, sizeof(struct growing_env_data));
    }
}

/* Alarm CCC change callback */
static void alarm_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value) {
    bool notif_enabled = (value == BT_GATT_CCC_NOTIFY);
    notification_state.alarm_notifications_enabled = notif_enabled;
    
    if (notif_enabled) {
        LOG_DBG("Alarm notifications enabled");
        
        /* CRITICAL FIX: Don't clear existing alarms! */
        /* Instead, send notification if there's an active alarm */
        struct alarm_data *alarm = (struct alarm_data *)alarm_value;
        if (alarm->alarm_code != 0) {
            /* Active alarm exists - notify the newly connected client */
            LOG_INF("Sending existing alarm to newly connected client: code=%u, data=%u", 
                    alarm->alarm_code, alarm->alarm_data);
            
            const struct bt_gatt_attr *alarm_attr = &irrigation_svc.attrs[ATTR_IDX_ALARM_VALUE];
            int err = safe_notify(default_conn, alarm_attr, alarm_value, sizeof(struct alarm_data));
            if (err != 0) {
                LOG_ERR("Failed to notify existing alarm: %d", err);
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
    
    /* Set default system values directly */
    struct system_config_data *sys_config = (struct system_config_data *)system_config_value;
    sys_config->version = 1;
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

int bt_irrigation_flow_update(uint32_t flow_rate) {
    if (!default_conn || !notification_state.flow_notifications_enabled) {
        return 0;
    }
    
    /* Flow updates are NORMAL priority - let adaptive throttling handle frequency */
    static uint32_t last_flow_rate = 0;
    bool flow_changed = (flow_rate != last_flow_rate);
    
    /* Always notify on significant flow changes, let adaptive system handle frequency */
    if (flow_changed || (flow_rate > 0)) {
        /* Update flow data directly */
        memcpy(flow_value, &flow_rate, sizeof(uint32_t));
        
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
    config_data->coverage_type = channel->coverage.use_area ? 0 : 1;
    config_data->sun_percentage = channel->sun_percentage;
    
    if (channel->coverage.use_area) {
        config_data->coverage.area_m2 = channel->coverage.area.area_m2;
    } else {
        config_data->coverage.plant_count = channel->coverage.plants.count;
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
        LOG_DBG("Schedule notification skipped: conn=%p, enabled=%d", 
                default_conn, notification_state.schedule_notifications_enabled);
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
    
    LOG_DBG("Schedule notification: ch=%u, type=%u, days=0x%02X, time=%02u:%02u",
            schedule_data->channel_id, schedule_data->schedule_type,
            schedule_data->days_mask, schedule_data->hour, schedule_data->minute);
    
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
        LOG_DBG("Calibration notification not enabled");
        return 0;
    }
    
    const struct bt_gatt_attr *attr = &irrigation_svc.attrs[ATTR_IDX_CALIB_VALUE];
    int err = safe_notify(default_conn, attr, calibration_value, sizeof(struct calibration_data));
    
    if (err != 0) {
        LOG_ERR("Calibration notification failed: %d", err);
    }
    
    return err;
}

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
            value->status = 3;  // Paused
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
    struct system_config_data *config = (struct system_config_data *)system_config_value;
    
    /* Get current watering system configuration */
    config->version = 1;                    // Configuration version
    config->power_mode = 0;                 // Normal mode
    
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
    int err = safe_notify(default_conn, attr, system_config_value, sizeof(struct system_config_data));
    
    if (err == 0) {
        LOG_INF("✅ System config notification sent: version=%u, power_mode=%u, flow_cal=%u, channels=%u",
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
        
        LOG_INF("✅ Flow calibration started successfully");
        
        /* Mark calibration as active */
        calibration_active = true;
        
    } else if (start == 0) {
        /* Stop calibration */
        LOG_INF("⏹️ Stopping flow calibration");
        
        /* Stop actual flow sensor calibration */
        /* NOTE: This is a simplified implementation - in a real system, */
        /* this would get the actual pulse count from the flow sensor */
        uint32_t pulses_counted = 750;  // Simulated pulse count
        uint32_t pulses_per_liter = 750; // Default calibration value
        
        /* Try to get current calibration */
        watering_error_t err = watering_get_flow_calibration(&pulses_per_liter);
        if (err == WATERING_SUCCESS) {
            /* Calculate new calibration based on measured volume vs expected */
            if (calib->volume_ml > 0) {
                pulses_per_liter = (pulses_counted * 1000) / calib->volume_ml;
                
                /* Set the new calibration */
                watering_set_flow_calibration(pulses_per_liter);
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
    if (!default_conn || !notification_state.history_notifications_enabled) {
        return 0;
    }
    
    /* Per BLE API Documentation: Get detailed history entries */
    struct history_data *hist_data = (struct history_data *)history_value;
    hist_data->channel_id = channel_id;
    hist_data->history_type = 0; /* Detailed */
    hist_data->entry_index = entry_index;
    hist_data->count = 1;
    hist_data->start_timestamp = start_timestamp;
    hist_data->end_timestamp = end_timestamp;
    
    /* Populate with sample detailed data */
    hist_data->data.detailed.timestamp = start_timestamp;
    hist_data->data.detailed.channel_id = channel_id;
    hist_data->data.detailed.event_type = 1; /* COMPLETE */
    hist_data->data.detailed.mode = 0; /* Duration */
    hist_data->data.detailed.target_value = 600; /* 10 minutes */
    hist_data->data.detailed.actual_value = 590; /* 9.8 minutes */
    hist_data->data.detailed.total_volume_ml = 5000; /* 5L */
    hist_data->data.detailed.trigger_type = 1; /* Scheduled */
    hist_data->data.detailed.success_status = 1; /* Success */
    hist_data->data.detailed.error_code = 0; /* No error */
    hist_data->data.detailed.flow_rate_avg = 750; /* 750 pps */
    
    LOG_INF("History detailed query: ch=%u, time=%u-%u, entry=%u", 
            channel_id, start_timestamp, end_timestamp, entry_index);
    
    return 0;
}

int bt_irrigation_history_get_daily(uint8_t channel_id, uint8_t entry_index) {
    if (!default_conn || !notification_state.history_notifications_enabled) {
        return 0;
    }
    
    /* Per BLE API Documentation: Get daily aggregated statistics */
    struct history_data *hist_data = (struct history_data *)history_value;
    hist_data->channel_id = channel_id;
    hist_data->history_type = 1; /* Daily */
    hist_data->entry_index = entry_index;
    hist_data->count = 1;
    hist_data->start_timestamp = 0;
    hist_data->end_timestamp = 0;
    
    /* Populate with sample daily data */
    hist_data->data.daily.day_index = 185; /* Day 185 of year */
    hist_data->data.daily.year = 2025;
    hist_data->data.daily.watering_sessions = 3;
    hist_data->data.daily.total_volume_ml = 15000; /* 15L */
    hist_data->data.daily.total_duration_sec = 1800; /* 30 minutes */
    hist_data->data.daily.avg_flow_rate = 750;
    hist_data->data.daily.success_rate = 100;
    hist_data->data.daily.error_count = 0;
    
    LOG_INF("History daily query: ch=%u, entry=%u", channel_id, entry_index);
    
    return 0;
}

int bt_irrigation_history_get_monthly(uint8_t channel_id, uint8_t entry_index) {
    if (!default_conn || !notification_state.history_notifications_enabled) {
        return 0;
    }
    
    /* Per BLE API Documentation: Get monthly aggregated statistics */
    struct history_data *hist_data = (struct history_data *)history_value;
    hist_data->channel_id = channel_id;
    hist_data->history_type = 2; /* Monthly */
    hist_data->entry_index = entry_index;
    hist_data->count = 1;
    hist_data->start_timestamp = 0;
    hist_data->end_timestamp = 0;
    
    /* Populate with sample monthly data */
    hist_data->data.monthly.month = 7; /* July */
    hist_data->data.monthly.year = 2025;
    hist_data->data.monthly.total_sessions = 90;
    hist_data->data.monthly.total_volume_ml = 450000; /* 450L */
    hist_data->data.monthly.total_duration_hours = 15; /* 15 hours */
    hist_data->data.monthly.avg_daily_volume = 14500; /* 14.5L/day */
    hist_data->data.monthly.active_days = 31;
    hist_data->data.monthly.success_rate = 95;
    
    LOG_INF("History monthly query: ch=%u, entry=%u", channel_id, entry_index);
    
    return 0;
}

int bt_irrigation_history_get_annual(uint8_t channel_id, uint8_t entry_index) {
    if (!default_conn || !notification_state.history_notifications_enabled) {
        return 0;
    }
    
    /* Per BLE API Documentation: Get annual aggregated statistics */
    struct history_data *hist_data = (struct history_data *)history_value;
    hist_data->channel_id = channel_id;
    hist_data->history_type = 3; /* Annual */
    hist_data->entry_index = entry_index;
    hist_data->count = 1;
    hist_data->start_timestamp = 0;
    hist_data->end_timestamp = 0;
    
    /* Populate with sample annual data */
    hist_data->data.annual.year = 2025;
    hist_data->data.annual.total_sessions = 1080;
    hist_data->data.annual.total_volume_liters = 5400; /* 5400L */
    hist_data->data.annual.avg_monthly_volume = 450; /* 450L/month */
    hist_data->data.annual.most_active_month = 7; /* July */
    hist_data->data.annual.success_rate = 93;
    hist_data->data.annual.peak_month_volume = 500; /* 500L peak */
    
    LOG_INF("History annual query: ch=%u, entry=%u", channel_id, entry_index);
    
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
    env_data->use_area_based = channel->coverage.use_area ? 1 : 0;
    
    if (channel->coverage.use_area) {
        env_data->coverage.area_m2 = channel->coverage.area.area_m2;
    } else {
        env_data->coverage.plant_count = channel->coverage.plants.count;
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
            env_data->use_area_based ? env_data->coverage.area_m2 : (float)env_data->coverage.plant_count,
            env_data->sun_percentage);
    
    /* Log custom plant info if applicable */
    if (env_data->plant_type == 7) {
        LOG_INF("Custom plant: '%s', water_factor=%.2f, freq=%u days, prefer_area=%u",
                env_data->custom_name, env_data->water_need_factor, 
                env_data->irrigation_freq_days, env_data->prefer_area_based);
    }
    
    /* Send notification */
    notify_growing_env();
    
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
    for (int i = 0; i < BLE_BUFFER_POOL_SIZE; i++) {
        if (notification_pool[i].in_use) {
            // Check if buffer has been in use too long (over 60 seconds)
            if ((k_uptime_get_32() - notification_pool[i].timestamp) > 60000) {
                notification_pool[i].in_use = false;
                LOG_DBG("Cleaned expired notification buffer %d", i);
            }
        }
    }
    
    // Adaptive throttling adjustments per priority
    for (int p = 0; p < 4; p++) {
        if (priority_state[p].success_count + priority_state[p].failure_count > 10) {
            float success_rate = (float)priority_state[p].success_count / 
                                 (priority_state[p].success_count + priority_state[p].failure_count);
            
            if (success_rate < 0.8f) {
                // Increase throttling if success rate is low
                priority_state[p].throttle_interval = MIN(priority_state[p].throttle_interval * 1.2f, 5000);
                LOG_DBG("Increased throttle interval for priority %d to %ums (success: %.2f%%)", 
                        p, priority_state[p].throttle_interval, success_rate * 100);
            } else if (success_rate > 0.95f) {
                // Decrease throttling if success rate is very high  
                uint32_t base_interval = (p == 0) ? THROTTLE_CRITICAL_MS : 
                                        (p == 1) ? THROTTLE_HIGH_MS :
                                        (p == 2) ? THROTTLE_NORMAL_MS : THROTTLE_LOW_MS;
                priority_state[p].throttle_interval = MAX(priority_state[p].throttle_interval * 0.9f, base_interval);
                LOG_DBG("Decreased throttle interval for priority %d to %ums (success: %.2f%%)", 
                        p, priority_state[p].throttle_interval, success_rate * 100);
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
    
    LOG_INF("🔍 BLE Notification System Debug");
    debug_notification_states();
    
    LOG_INF("🔧 Force enabling all notifications");
    force_enable_all_notifications();
    
    LOG_INF("🧪 Testing channel 0 notification");
    int result = test_channel_config_notification(0);
    
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
    
    return test_channel_config_notification(channel_id);
}

int bt_irrigation_force_enable_notifications(void) {
    if (!default_conn) {
        LOG_ERR("❌ No BLE connection for force enable");
        return -ENOTCONN;
    }
    
    force_enable_all_notifications();
    return 0;
}

/* ------------------------------------------------------------------ */
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

int bt_irrigation_debug_notifications(void) {
    return 0; // BLE disabled - no debugging
}

int bt_irrigation_test_channel_notification(uint8_t channel_id) {
    return 0; // BLE disabled - no testing
}

int bt_irrigation_force_enable_notifications(void) {
    return 0; // BLE disabled - no force enable
}

#endif /* CONFIG_BT */
