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
#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/sys/printk.h>
#include <zephyr/settings/settings.h>    /* pentru settings_load() */

#include "bt_irrigation_service.h"
#include "watering.h"
#include "watering_internal.h"  // Add this include to access internal state
#include "rtc.h"
#include "flow_sensor.h"
#include "watering_history.h"   // Add this include for history functionality

/* Forward declaration for the service structure */
extern const struct bt_gatt_service_static irrigation_svc;

/* ------------------------------------------------------------------ */
/* SIMPLIFIED BLE Notification System                                */
/* ------------------------------------------------------------------ */
#define NOTIFICATION_DELAY_MS 500  // Longer delay for stability
#define MAX_NOTIFICATION_RETRIES 3  // Maximum retry attempts
static uint32_t last_notification_time = 0;
static bool notification_system_enabled = true;

/* Simple direct notification function - no queues, no work handlers */
static int send_simple_notification(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                   const void *data, uint16_t len);

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
} notification_state_t;

static notification_state_t notification_state = {0};

/* ------------------------------------------------------------------ */
/* NEW: forward declarations needed before first use                  */
/* ------------------------------------------------------------------ */

/*  stub-handlers used inside BT_GATT_SERVICE_DEFINE – declare early  */
static ssize_t read_schedule(struct bt_conn *, const struct bt_gatt_attr *,
                             void *, uint16_t, uint16_t);
static ssize_t write_schedule(struct bt_conn *, const struct bt_gatt_attr *,
                              const void *, uint16_t, uint16_t, uint8_t);
static void    schedule_ccc_changed(const struct bt_gatt_attr *, uint16_t);

static ssize_t read_system_config(struct bt_conn *, const struct bt_gatt_attr *,
                                  void *, uint16_t, uint16_t);
static ssize_t write_system_config(struct bt_conn *, const struct bt_gatt_attr *,
                                   const void *, uint16_t, uint16_t, uint8_t);
static void    system_config_ccc_changed(const struct bt_gatt_attr *, uint16_t);

static ssize_t read_task_queue(struct bt_conn *, const struct bt_gatt_attr *,
                               void *, uint16_t, uint16_t);
static ssize_t write_task_queue(struct bt_conn *, const struct bt_gatt_attr *,
                                const void *, uint16_t, uint16_t, uint8_t);
static void    task_queue_ccc_changed(const struct bt_gatt_attr *, uint16_t);

static ssize_t read_statistics(struct bt_conn *, const struct bt_gatt_attr *,
                               void *, uint16_t, uint16_t);
static ssize_t write_statistics(struct bt_conn *, const struct bt_gatt_attr *,
                                const void *, uint16_t, uint16_t, uint8_t);
static void    statistics_ccc_cfg_changed(const struct bt_gatt_attr *, uint16_t);

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

static ssize_t read_current_task(struct bt_conn *, const struct bt_gatt_attr *,
                                void *, uint16_t, uint16_t);
static ssize_t write_current_task(struct bt_conn *, const struct bt_gatt_attr *,
                                 const void *, uint16_t, uint16_t, uint8_t);
static void    current_task_ccc_changed(const struct bt_gatt_attr *, uint16_t);

/* Forward declaration for task update thread */
static int start_task_update_thread(void);

/* Forward declaration for simple notification function */
static int send_simple_notification(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                   const void *data, uint16_t len);

/* Forward declaration for subscription checking function */
static bool is_notification_enabled(const struct bt_gatt_attr *attr);
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

/* Task creation data structure */
struct valve_control_data {
    uint8_t channel_id;
    uint8_t task_type; // 0=duration, 1=volume
    uint16_t value; // minutes for duration mode, liters for volume mode
}
        __packed;

/* Channel configuration structure */
struct channel_config_data {
    uint8_t channel_id;
    uint8_t name_len;
    char    name[64];      /* ← was 16, now full 64-byte buffer */
    uint8_t auto_enabled;
    
    /* New plant and growing environment fields */
    uint8_t plant_type;          /* Type of plant being grown */
    uint8_t soil_type;           /* Type of soil in the growing area */
    uint8_t irrigation_method;   /* Method of irrigation used */
    uint8_t coverage_type;       /* 0=area, 1=plant count */
    union {
        float area_m2;           /* Area in square meters */
        uint16_t plant_count;    /* Number of individual plants */
    } coverage;
    uint8_t sun_percentage;      /* Percentage of direct sunlight (0-100%) */
}
        __packed;

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
}
        __packed;

/* Structure for setting/reading RTC */
struct rtc_data {
    uint8_t year; /* Year minus 2000 (0-99) */
    uint8_t month; /* Month (1-12) */
    uint8_t day; /* Day (1-31) */
    uint8_t hour; /* Hour (0-23) */
    uint8_t minute; /* Minute (0-59) */
    uint8_t second; /* Second (0-59) */
    uint8_t day_of_week; /* Day of week (0-6, 0=Sunday) */
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
    uint32_t uptime; /* Uptime in minutes */
    uint8_t error_count; /* Number of errors */
    uint8_t last_error; /* Last error code */
    uint8_t valve_status; /* Bits for valve status */
    uint8_t battery_level; /* Battery level in percent or 0xFF if not applicable */
}
        __packed;

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
    char custom_name[32];         /* Custom plant name */
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
/* Currently selected channel for growing environment operations */
static uint8_t selected_channel_id __attribute__((unused)) = 0;

/* ---------------------------------------------------------------
 *  Accumulator for fragmented Channel-Config writes (≤20 B each)
 * ------------------------------------------------------------- */
static struct {
    uint8_t  id;          /* channel being edited              */
    uint8_t  expected;    /* total name_len from first frame   */
    uint8_t  received;    /* bytes stored so far               */
    char     buf[64];     /* temporary buffer                  */
    bool     in_progress; /* true while receiving fragments    */
} name_frag = {0};



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
/* ---------------------------------------------------------------- */

/* Global variables for calibration */
static bool calibration_active = false;
static uint32_t calibration_start_pulses = 0;

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
            printk("Unknown characteristic index: %d\n", (int)attr_index);
            return false;
    }
}

/* ---------------------------------------------------------------------- */

/* Bluetooth connection callback */
static struct bt_conn *default_conn;

static void connected(struct bt_conn *conn, uint8_t err) {
    if (err) {
        printk("Connection failed (err %u)\n", err);
        return;
    }

    /* Reset notification system completely */
    notification_system_enabled = true;
    last_notification_time = 0;
    
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
        printk("Conn param update failed (err %d)\n", update_err);
    }

    if (!default_conn) {
        default_conn = bt_conn_ref(conn);
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
        printk("Connected - system status updated to: %d\n", current_status);
    } else {
        status_value[0] = (uint8_t)WATERING_STATUS_OK; /* Default to OK if can't read */
        printk("Connected - defaulted system status to OK\n");
    }
    
    printk("Connected to irrigation controller - values cleared and status updated\n");
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
    printk("Advertising stop result: %d\n", err);
    
    /* Retry loop with exponential backoff */
    while (retry_count < max_retries) {
        /* Wait before attempting restart */
        uint32_t delay_ms = 200 + (100 * retry_count);  // Linear backoff instead of exponential
        printk("Waiting %d ms before advertising restart attempt %d\n", delay_ms, retry_count + 1);
        k_sleep(K_MSEC(delay_ms));
        
        printk("Attempting to start advertising (attempt %d/%d)\n", retry_count + 1, max_retries);
        err = bt_le_adv_start(&adv_param,
                              adv_ad, ARRAY_SIZE(adv_ad),
                              adv_sd, ARRAY_SIZE(adv_sd));
        
        if (err == 0) {
            printk("Advertising restarted successfully after %d retries\n", retry_count);
            return;
        }
        
        if (err == -EALREADY) {
            printk("Advertising already active\n");
            return;
        }
        
        printk("Advertising restart failed (err %d), retry %d/%d\n", 
               err, retry_count + 1, max_retries);
        
        retry_count++;
    }
    
    printk("Failed to restart advertising after %d attempts - will try again in 5 seconds\n", max_retries);
    
    /* If all retries failed, schedule another attempt in 5 seconds */
    k_work_reschedule(&adv_restart_work, K_SECONDS(5));
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
    printk("Disconnected (reason %u)\n", reason);

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
static ssize_t read_valve(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                          void *buf, uint16_t len, uint16_t offset) {
    /* Return current valve_value, but ensure it has valid data */
    const struct valve_control_data *value = attr->user_data;
    
    /* If the stored value has an invalid channel ID, don't return anything meaningful */
    if (value->channel_id >= WATERING_CHANNELS_COUNT) {
        /* Return empty/inactive state for invalid channel */
        struct valve_control_data empty_data = {0xFF, 0, 0};
        printk("BT valve read: returning empty data (invalid channel_id %d)\n", value->channel_id);
        return bt_gatt_attr_read(conn, attr, buf, len, offset, &empty_data,
                                 sizeof(struct valve_control_data));
    }

    printk("BT valve read: channel %d, task_type %d, value %d\n", 
           value->channel_id, value->task_type, value->value);
    return bt_gatt_attr_read(conn, attr, buf, len, offset, value,
                             sizeof(struct valve_control_data));
}

/* Valve characteristic write callback */
static ssize_t write_valve(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                           const void *buf, uint16_t len, uint16_t offset, uint8_t flags) {
    struct valve_control_data *value = attr->user_data;

    if (offset + len > sizeof(struct valve_control_data)) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }

    memcpy(((uint8_t *) value) + offset, buf, len);

    /* Process task creation request */
    uint8_t channel_id = value->channel_id;
    uint8_t task_type = value->task_type;
    uint16_t task_value = value->value;

    if (channel_id >= WATERING_CHANNELS_COUNT) {
        return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
    }

    printk("BT request: channel %d, task type %d, value %d\n",
           channel_id, task_type, task_value);

    /* Create the corresponding task */
    watering_error_t err;
    if (task_type == 0) {
        // Duration (minutes)
        err = watering_add_duration_task(channel_id, task_value);
        if (err == WATERING_SUCCESS) {
            printk("Duration task added via Bluetooth: channel %d, %d minutes (MANUAL trigger)\n",
                   channel_id + 1, task_value);
        }
    } else if (task_type == 1) {
        // Volume (liters)
        err = watering_add_volume_task(channel_id, task_value);
        if (err == WATERING_SUCCESS) {
            printk("Volume task added via Bluetooth: channel %d, %d liters (MANUAL trigger)\n",
                   channel_id + 1, task_value);
        }
    } else {
        return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
    }

    if (err != WATERING_SUCCESS) {
        printk("Error adding task: %d\n", err);
        return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
    }

    return len;
}

/* CCC configuration change callback for valve characteristic */
static void valve_ccc_cfg_changed(const struct bt_gatt_attr *attr, uint16_t value) {
    bool notif_enabled = (value == BT_GATT_CCC_NOTIFY);
    notification_state.valve_notifications_enabled = notif_enabled;
    printk("Valve notifications %s\n", notif_enabled ? "enabled" : "disabled");
    
    /* Clear valve_value completely to prevent stale data from being read */
    memset(valve_value, 0, sizeof(struct valve_control_data));
    
    if (notif_enabled) {
        printk("Valve notifications enabled - will send status updates when valves change\n");
        
        /* Set channel_id to invalid value to ensure no false active state is reported */
        struct valve_control_data *valve_data = (struct valve_control_data *)valve_value;
        valve_data->channel_id = 0xFF; /* Invalid channel ID */
        valve_data->task_type = 0;     /* Inactive */
        valve_data->value = 0;
        
        /* Don't send any automatic notification - let real valve events trigger them */
        printk("No automatic valve status sent - waiting for real valve events\n");
    }
}

/* Flow characteristic read callback */
static ssize_t read_flow(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                         void *buf, uint16_t len, uint16_t offset) {
    const uint32_t *value = attr->user_data;

    return bt_gatt_attr_read(conn, attr, buf, len, offset, value,
                             sizeof(uint32_t));
}

/* CCC configuration change callback for flow characteristic */
static void flow_ccc_cfg_changed(const struct bt_gatt_attr *attr, uint16_t value) {
    bool notif_enabled = (value == BT_GATT_CCC_NOTIFY);
    notification_state.flow_notifications_enabled = notif_enabled;
    printk("Flow notifications %s\n", notif_enabled ? "enabled" : "disabled");
    
    if (notif_enabled) {
        /* Only prepare current flow reading, don't send notification during setup */
        uint32_t current_flow = get_pulse_count();
        memcpy(flow_value, &current_flow, sizeof(uint32_t));
        /* DO NOT send immediate notification during CCC setup - this causes system freeze */
        printk("Flow notifications enabled - flow value ready: %u\n", current_flow);
    }
}

/* Status characteristic read callback */
static ssize_t read_status(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                           void *buf, uint16_t len, uint16_t offset) {
    /* Always read current status from system to ensure accuracy */
    watering_status_t current_status;
    if (watering_get_status(&current_status) == WATERING_SUCCESS) {
        status_value[0] = (uint8_t)current_status;
    } else {
        status_value[0] = (uint8_t)WATERING_STATUS_OK; /* Default to OK if can't read */
    }
    
    const uint8_t *value = attr->user_data;
    return bt_gatt_attr_read(conn, attr, buf, len, offset, value, sizeof(uint8_t));
}

/* CCC configuration change callback for status characteristic */
static void status_ccc_cfg_changed(const struct bt_gatt_attr *attr, uint16_t value) {
    bool notif_enabled = (value == BT_GATT_CCC_NOTIFY);
    notification_state.status_notifications_enabled = notif_enabled;
    printk("Status notifications %s\n", notif_enabled ? "enabled" : "disabled");
    
    if (notif_enabled) {
        /* Always read fresh status from system */
        watering_status_t current_status;
        if (watering_get_status(&current_status) == WATERING_SUCCESS) {
            status_value[0] = (uint8_t)current_status;
            printk("Status CCC enabled - status ready: %d\n", current_status);
        } else {
            status_value[0] = (uint8_t)WATERING_STATUS_OK;
            printk("Status CCC enabled - defaulted to OK status\n");
        }
        
        /* DO NOT send immediate notification during CCC setup - this causes system freeze */
        /* The client will read the status when ready */
    }
}

/* ------------------------------------------------------------------
 * NEW: Channel-Config CCC callback (was only prototyped – now coded)
 * ----------------------------------------------------------------*/
static void channel_config_ccc_changed(const struct bt_gatt_attr *attr,
                                       uint16_t value)
{
    bool notif_enabled = (value == BT_GATT_CCC_NOTIFY);
    notification_state.channel_config_notifications_enabled = notif_enabled;
    printk("Channel Config notifications %s\n",
           notif_enabled ? "enabled" : "disabled");
    
    if (notif_enabled) {
        printk("Channel Config notifications enabled - will send updates when config changes\n");
        /* Clear any stale data */
        memset(channel_config_value, 0, sizeof(channel_config_value));
    }
}

/* Channel Config characteristic read callback */
static ssize_t read_channel_config(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                   void *buf, uint16_t len, uint16_t offset) {
    /* Create a TRUE local buffer for reading to avoid conflicts with notification buffer */
    /* CRITICAL: Remove 'static' keyword to ensure this is a stack-allocated local buffer */
    struct channel_config_data read_value;
    
    /* Get the current channel selection from the global attribute buffer */
    const struct channel_config_data *global_value = 
        (const struct channel_config_data *)channel_config_value;
    
    /* Use the selected channel from the global buffer, but default to 0 if invalid */
    uint8_t channel_id = global_value->channel_id;
    if (channel_id >= WATERING_CHANNELS_COUNT) {
        channel_id = 0;
    }
    
    /* CRITICAL: Ensure this is a READ-ONLY operation that doesn't trigger saves */
    /* The read operation should never modify the system state */

    watering_channel_t *channel;
    watering_error_t err = watering_get_channel(channel_id, &channel);
    
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
    
    size_t name_len = strnlen(channel->name, sizeof(channel->name));
    if (name_len >= sizeof(read_value.name)) {
        name_len = sizeof(read_value.name) - 1;
    }
    memcpy(read_value.name, channel->name, name_len);
    read_value.name[name_len] = '\0';

    read_value.name_len = name_len;
    read_value.auto_enabled = channel->watering_event.auto_enabled ? 1 : 0;
    
    /* Reduce debug logging to prevent log spam during repeated reads */
    static uint32_t last_read_log_time = 0;
    static uint8_t last_read_channel_id = 0xFF;
    uint32_t now = k_uptime_get_32();
    
    /* Only log if it's been more than 2 seconds since the last log for this channel */
    if (now - last_read_log_time > 2000 || last_read_channel_id != channel_id) {
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

    /* —— 1-byte SELECT-FOR-READ  -------------------------------- */
    if (!(flags & BT_GATT_WRITE_FLAG_PREPARE) && offset == 0 && len == 1) {
        uint8_t requested_channel_id;
        memcpy(&requested_channel_id, buf, 1);
        if (requested_channel_id >= WATERING_CHANNELS_COUNT)
            return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
        
        /* Only update the local cache if the channel actually changed */
        if (value->channel_id != requested_channel_id) {
            value->channel_id = requested_channel_id;
            /* refresh local cache so a subsequent READ is coherent */
            watering_channel_t *ch;
            if (watering_get_channel(value->channel_id, &ch) == WATERING_SUCCESS) {
                size_t n = strnlen(ch->name, sizeof(ch->name));
                if (n >= sizeof(value->name)) n = sizeof(value->name) - 1;
                memcpy(value->name, ch->name, n);
                value->name[n]  = '\0';
                value->name_len = n;
                value->auto_enabled = ch->watering_event.auto_enabled ? 1 : 0;
                printk("Channel selection changed to %d\n", value->channel_id);
            }
        }
        /* DO NOT call watering_save_config() here - this is just a selection, not a config change */
        return len;        /* ACK */
    }

    /* —— FRAGMENTED short-write (header + piece of data) -------- */
    /* Only interpret as fragmentation if packet is SHORT (< full struct size) */
    if (!(flags & BT_GATT_WRITE_FLAG_PREPARE) && offset == 0 && len >= 3 && len < sizeof(*value)) {
        const uint8_t *p = buf;
        uint8_t cid      = p[0];
        uint8_t frag_type = p[1];  // 0 = name only, 1 = full structure
        uint8_t total    = p[2];
        const uint8_t *payload = p + 3;
        uint8_t pay_len  = len - 3;

        printk("Fragment protocol: len=%d, cid=%d, type=%d, total=%d, pay_len=%d\n", 
               len, cid, frag_type, total, pay_len);

        /* sanity checks */
        if (cid >= WATERING_CHANNELS_COUNT) {
            printk("Channel config write error: invalid cid=%d\n", cid);
            return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
        }
        
        /* BACKWARD COMPATIBILITY: Detect old 2-byte header format */
        if (frag_type > 1 || (frag_type == 1 && total != sizeof(*value))) {
            printk("Detected old 2-byte fragmentation format, rejecting. Use new 3-byte format.\n");
            printk("Expected: [channel_id][frag_type][total_length][data...]\n");
            printk("Got what looks like: [channel_id][chunk_length][data...]\n");
            printk("Raw bytes: [%02X][%02X][%02X] (len=%d)\n", p[0], p[1], p[2], len);
            return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
        }
        
        /* Additional validation for structure fragmentation */
        if (frag_type == 1) {
            if (total != sizeof(*value)) {
                printk("Structure fragmentation error: total=%d, expected=%d\n", 
                       total, (int)sizeof(*value));
                printk("Raw header: [%02X][%02X][%02X]\n", p[0], p[1], p[2]);
                return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
            }
            
            /* Check if there's already a fragmentation in progress for a different operation */
            if (name_frag.in_progress) {
                printk("Warning: Name fragmentation in progress, aborting it for structure frag\n");
                name_frag.in_progress = false;
            }
        }
        
        if (frag_type == 0) {
            /* ---- NAME-ONLY FRAGMENTATION (backward compatibility) ---- */
            if (total > 64) {
                printk("Channel config write error: name too long total=%d\n", total);
                return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
            }
            
            /* Special case: empty name (total == 0) */
            if (total == 0) {
                printk("Setting empty name for channel %d\n", cid);
                watering_channel_t *ch;
                if (watering_get_channel(cid, &ch) == WATERING_SUCCESS) {
                    memset(ch->name, 0, sizeof(ch->name));
                    ch->name[0] = '\0';
                    
                    // Priority save for BLE config changes
                    static uint32_t last_clear_save = 0;
                    uint32_t now = k_uptime_get_32();
                    if (now - last_clear_save > 250) {
                        watering_save_config_priority(true);
                        last_clear_save = now;
                    }
                    
                    printk("Channel %d name cleared (empty)\n", cid);
                    
                    // Also update the local cache immediately
                    memset(value->name, 0, sizeof(value->name));
                    value->name[0] = '\0';
                    value->name_len = 0;
                    value->channel_id = cid;
                    printk("Local cache updated: name=\"\" (empty), len=0\n");
                } else {
                    printk("Failed to get channel %d\n", cid);
                    return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
                }
                return len;     /* ACK */
            }
            
            /* For non-empty names, we need some payload */
            if (pay_len == 0) {
                printk("Channel config write error: non-empty name but no payload\n");
                return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
            }

            /* first fragment → reset accumulator */
            if (!name_frag.in_progress) {
                name_frag.id        = cid;
                name_frag.expected  = total;
                name_frag.received  = 0;
                name_frag.in_progress = true;
                memset(name_frag.buf, 0, sizeof(name_frag.buf));
                printk("Starting name fragment for channel %d, expected %d bytes\n", cid, total);
            }

            /* fragments must belong to the same channel and not overflow buffer */
            if (cid != name_frag.id || name_frag.received + pay_len > name_frag.expected ||
                name_frag.received + pay_len > 64) {
                printk("Fragment error: cid mismatch or overflow\n");
                name_frag.in_progress = false;                /* abort */
                return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
            }

            /* append slice */
            memcpy(name_frag.buf + name_frag.received, payload, pay_len);
            name_frag.received += pay_len;
            printk("Received fragment: %d/%d bytes\n", name_frag.received, name_frag.expected);

            /* done? -> commit */
            if (name_frag.received >= name_frag.expected) {
                name_frag.buf[name_frag.expected] = '\0';
                printk("Complete name received: \"%s\"\n", name_frag.buf);

                watering_channel_t *ch;
                if (watering_get_channel(cid, &ch) == WATERING_SUCCESS) {
                    // Clear the existing name first
                    memset(ch->name, 0, sizeof(ch->name));
                    strncpy(ch->name, name_frag.buf, sizeof(ch->name) - 1);
                    ch->name[sizeof(ch->name) - 1] = '\0';
                    
                    // Priority save for BLE config changes
                    static uint32_t last_name_save = 0;
                    uint32_t now = k_uptime_get_32();
                    if (now - last_name_save > 250) {
                        watering_save_config_priority(true);
                        last_name_save = now;
                    }
                    
                    printk("Channel %d name set to \"%s\" (cleared and set)\n", cid, ch->name);
                    
                    // Also update the local cache immediately
                    memset(value->name, 0, sizeof(value->name));
                    strncpy(value->name, ch->name, sizeof(value->name) - 1);
                    value->name[sizeof(value->name) - 1] = '\0';
                    value->name_len = strlen(ch->name);
                    value->channel_id = cid;
                    printk("Local cache updated: name=\"%s\", len=%d\n", value->name, value->name_len);
                } else {
                    printk("Failed to get channel %d\n", cid);
                }
                name_frag.in_progress = false;
            }
            return len;     /* ACK current fragment */
            
        } else if (frag_type == 1) {
            /* ---- FULL STRUCTURE FRAGMENTATION (new feature) ---- */
            if (total != sizeof(*value)) {
                printk("Channel config write error: invalid struct size total=%d, expected=%d\n", 
                       total, (int)sizeof(*value));
                return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
            }
            
            /* Use the existing growing environment fragmentation buffer (larger) */
            if (!growing_env_frag.in_progress) {
                growing_env_frag.channel_id = cid;
                growing_env_frag.expected = total;
                growing_env_frag.received = 0;
                growing_env_frag.in_progress = true;
                memset(growing_env_frag.buf, 0, sizeof(growing_env_frag.buf));
                printk("Starting structure fragment for channel %d, expected %d bytes\n", cid, total);
            }
            
            /* Check for consistency and overflow */
            if (cid != growing_env_frag.channel_id || 
                growing_env_frag.received + pay_len > growing_env_frag.expected ||
                growing_env_frag.received + pay_len > sizeof(growing_env_frag.buf)) {
                printk("Structure fragment error: mismatch or overflow\n");
                printk("  Channel ID: expected=%d, got=%d\n", growing_env_frag.channel_id, cid);
                printk("  Frag type: %d (should be 1)\n", frag_type);
                printk("  Total length: %d (should be %d)\n", total, growing_env_frag.expected);
                printk("  Size check: received=%d + pay_len=%d = %d, expected=%d, buf_size=%d\n", 
                       growing_env_frag.received, pay_len, growing_env_frag.received + pay_len, 
                       growing_env_frag.expected, (int)sizeof(growing_env_frag.buf));
                printk("  Raw header bytes: [%02X][%02X][%02X]\n", 
                       ((uint8_t*)buf)[0], ((uint8_t*)buf)[1], ((uint8_t*)buf)[2]);
                growing_env_frag.in_progress = false;
                return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
            }
            
            /* Append data */
            memcpy(growing_env_frag.buf + growing_env_frag.received, payload, pay_len);
            growing_env_frag.received += pay_len;
            printk("Received structure fragment: %d/%d bytes\n", 
                   growing_env_frag.received, growing_env_frag.expected);
            
            /* Complete? */
            if (growing_env_frag.received >= growing_env_frag.expected) {
                /* Copy complete structure and process it */
                memcpy(value, growing_env_frag.buf, sizeof(*value));
                growing_env_frag.in_progress = false;
                
                printk("Complete structure received, processing...\n");
                
                /* Process the complete structure (same as 76-byte write) */
                if (value->channel_id >= WATERING_CHANNELS_COUNT) {
                    return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
                }
                watering_channel_t *ch;
                if (watering_get_channel(value->channel_id, &ch) != WATERING_SUCCESS) {
                    return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
                }
                
                /* Update channel configuration from received structure */
                ch->watering_event.auto_enabled = value->auto_enabled ? true : false;
                
                /* Update plant and growing environment settings */
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
                
                /* Handle name separately with proper bounds checking */
                if (value->name_len > 0 && value->name_len <= sizeof(ch->name) - 1) {
                    memset(ch->name, 0, sizeof(ch->name));
                    memcpy(ch->name, value->name, value->name_len);
                    ch->name[value->name_len] = '\0';
                } else {
                    ch->name[0] = '\0';
                }
                
                watering_save_config_priority(true);
                printk("Channel %d configuration updated via structure fragmentation\n", cid);
            }
            return len;     /* ACK current fragment */
            
        } else {
            printk("Channel config write error: unknown fragment type=%d\n", frag_type);
            return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
        }
    }

    /* —— existing LONG-WRITE / EXECUTE logic (unchanged) ——— */
    if (flags & BT_GATT_WRITE_FLAG_PREPARE) {
        if (offset + len > sizeof(*value)) {
            return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
        }
        memcpy(((uint8_t *)value) + offset, buf, len);
        return len;                    /* MUST return 'len' to accept fragment */
    }

    /* ---------- EXECUTE-WRITE stage  (buf == NULL, len == 0) ---------- */
    if (buf == NULL && len == 0) {
        /* Commit the structure already assembled in ‘value’ */
        if (value->channel_id >= WATERING_CHANNELS_COUNT) {
            return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
        }
        watering_channel_t *ch;
        if (watering_get_channel(value->channel_id, &ch) != WATERING_SUCCESS) {
            printk("Execute write error: failed to get channel %d\n", value->channel_id);
            return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
        }

        if (value->name_len > 0 && value->name_len < sizeof(value->name)) {
            /* Ensure the name is null-terminated within the specified length */
            value->name[value->name_len] = '\0';
            /* Clear destination first, then copy */
            memset(ch->name, 0, sizeof(ch->name));
            strncpy(ch->name, value->name, sizeof(ch->name) - 1);
            ch->name[sizeof(ch->name) - 1] = '\0';
            printk("Execute write: Channel %d name set to \"%s\" (len=%d)\n", 
                   value->channel_id, ch->name, value->name_len);
        } else if (value->name_len == 0) {
            memset(ch->name, 0, sizeof(ch->name));  /* Clear completely */
            ch->name[0] = '\0';  // Set empty name explicitly
            printk("Execute write: Channel %d name cleared (empty)\n", value->channel_id);
        } else {
            printk("Execute write error: invalid name_len %d (max %d)\n", 
                   value->name_len, (int)sizeof(value->name) - 1);
            return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
        }
        ch->watering_event.auto_enabled = value->auto_enabled ? true : false;
        
        // Throttle config saves to prevent system freeze during BLE operations
        static uint32_t last_execute_save = 0;
        uint32_t now = k_uptime_get_32();
        if (now - last_execute_save > 500) { // Minimum 500ms between saves
            watering_save_config();
            last_execute_save = now;
        }
        return sizeof(struct channel_config_data);
    }

    /* ---- EXECUTE stage ------------------------------------------- */
    if (flags & BT_GATT_WRITE_FLAG_EXECUTE) {
        /* full struct already buffered in value -> validate & save */
        if (value->channel_id >= WATERING_CHANNELS_COUNT)
            return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);

        watering_channel_t *ch;
        if (watering_get_channel(value->channel_id, &ch) != WATERING_SUCCESS)
            return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);

        /* update name */
        if (value->name_len > 0 && value->name_len < sizeof(value->name)) {
            /* Ensure the name is null-terminated within the specified length */
            value->name[value->name_len] = '\0';
            /* Clear destination first, then copy */
            memset(ch->name, 0, sizeof(ch->name));
            strncpy(ch->name, value->name, sizeof(ch->name) - 1);
            ch->name[sizeof(ch->name) - 1] = '\0';
            printk("EXECUTE flag: Channel %d name set to \"%s\" (len=%d)\n", 
                   value->channel_id, ch->name, value->name_len);
        } else if (value->name_len == 0) {
            memset(ch->name, 0, sizeof(ch->name));  /* Clear completely */
            ch->name[0] = '\0';  // Set empty name explicitly
            printk("EXECUTE flag: Channel %d name cleared (empty)\n", value->channel_id);
        } else {
            printk("EXECUTE flag error: invalid name_len %d (max %d)\n", 
                   value->name_len, (int)sizeof(value->name) - 1);
            return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
        }
        /* auto-enable */
        ch->watering_event.auto_enabled = value->auto_enabled ? true : false;
        
        /* Update new plant and growing environment fields */
        ch->plant_type = (plant_type_t)value->plant_type;
        ch->soil_type = (soil_type_t)value->soil_type;
        ch->irrigation_method = (irrigation_method_t)value->irrigation_method;
        ch->coverage.use_area = (value->coverage_type == 0);
        if (ch->coverage.use_area) {
            ch->coverage.area.area_m2 = value->coverage.area_m2;
        } else {
            ch->coverage.plants.count = value->coverage.plant_count;
        }
        ch->sun_percentage = value->sun_percentage;

        // Throttle config saves to prevent system freeze during BLE operations
        static uint32_t last_execute_full_save = 0;
        uint32_t now = k_uptime_get_32();
        if (now - last_execute_full_save > 250) { // Minimum 250ms between priority saves
            watering_save_config_priority(true);
            last_execute_full_save = now;
        }
        return sizeof(struct channel_config_data); /* success */
    }

    /* any other write (regular full write ≤ MTU) -------------------- */
    printk("Regular write protocol: offset=%d, len=%d, struct_size=%d\n", offset, len, sizeof(*value));
    if (offset + len > sizeof(*value)) {
        printk("Regular write error: offset+len > struct size (%d > %d)\n", 
               offset + len, sizeof(*value));
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }
    memcpy(((uint8_t *)value) + offset, buf, len);
    printk("Regular write: offset=%d, len=%d, total=%d\n", offset, len, offset + len);
    
    /* if entire struct fits in one request commit immediately */
    if (offset + len == sizeof(*value)) {
        printk("Regular write: complete struct received, committing\n");
        /* same commit block as EXECUTE */
        if (value->channel_id >= WATERING_CHANNELS_COUNT) {
            printk("Regular write error: invalid channel_id %d\n", value->channel_id);
            return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
        }

        watering_channel_t *ch;
        if (watering_get_channel(value->channel_id, &ch) != WATERING_SUCCESS) {
            printk("Regular write error: failed to get channel %d\n", value->channel_id);
            return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
        }

        if (value->name_len > 0 && value->name_len < sizeof(value->name)) {
            /* Ensure the name is null-terminated within the specified length */
            value->name[value->name_len] = '\0';
            /* Clear destination first, then copy */
            memset(ch->name, 0, sizeof(ch->name));
            strncpy(ch->name, value->name, sizeof(ch->name) - 1);
            ch->name[sizeof(ch->name) - 1] = '\0';
            printk("Regular write: Channel %d name set to \"%s\" (len=%d)\n", 
                   value->channel_id, ch->name, value->name_len);
        } else if (value->name_len == 0) {
            memset(ch->name, 0, sizeof(ch->name));  /* Clear completely */
            ch->name[0] = '\0';  // Set empty name explicitly
            printk("Regular write: Channel %d name cleared (empty)\n", value->channel_id);
        } else {
            printk("Regular write error: invalid name_len %d (max %d)\n", 
                   value->name_len, (int)sizeof(value->name) - 1);
            return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
        }
        ch->watering_event.auto_enabled = value->auto_enabled ? true : false;
        
        /* Update new plant and growing environment fields */
        ch->plant_type = (plant_type_t)value->plant_type;
        ch->soil_type = (soil_type_t)value->soil_type;
        ch->irrigation_method = (irrigation_method_t)value->irrigation_method;
        ch->coverage.use_area = (value->coverage_type == 0);
        if (ch->coverage.use_area) {
            ch->coverage.area.area_m2 = value->coverage.area_m2;
        } else {
            ch->coverage.plants.count = value->coverage.plant_count;
        }
        ch->sun_percentage = value->sun_percentage;
        
        // Throttle config saves during BLE connection setup to prevent freeze
        static uint32_t last_bt_save = 0;
        uint32_t now = k_uptime_get_32();
        if (now - last_bt_save > 500) { // Minimum 500ms between BLE-triggered saves
            watering_save_config_priority(true);
            last_bt_save = now;
        }
    }
    return len;
}

/* Connection callbacks - needed for BLE connection management */
static struct bt_conn_cb conn_callbacks = {
    .connected = connected,
    .disconnected = disconnected,
};

/* Helper: notify Channel-Config respecting connection MTU */
/* Commented out - unused function
static int notify_channel_config(struct bt_conn *conn)
{
    if (!conn) { return -ENOTCONN; }

    // ATT payload can be at most (MTU-3) bytes
    uint16_t mtu = bt_gatt_get_mtu(conn);
    size_t   len = sizeof(struct channel_config_data);
    if (len > mtu - 3) { len = mtu - 3; }

    return send_simple_notification(conn,
                          &irrigation_svc.attrs[ATTR_IDX_CHANNEL_CFG_VALUE],
                          channel_config_value, len);
}
*/

/* RTC implementation */
static ssize_t read_rtc(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                        void *buf, uint16_t len, uint16_t offset) {
    struct rtc_data *value = (struct rtc_data *) rtc_value;
    rtc_datetime_t now;

    // Read current date and time from RTC
    if (rtc_datetime_get(&now) == 0) {
        value->year = now.year - 2000; // Convert to 2-digit format
        value->month = now.month;
        value->day = now.day;
        value->hour = now.hour;
        value->minute = now.minute;
        value->second = now.second;
        value->day_of_week = now.day_of_week;
    } else {
        // RTC unavailable, use default values
        value->year = 23; // 2023
        value->month = 1;
        value->day = 1;
        value->hour = 0;
        value->minute = 0;
        value->second = 0;
        value->day_of_week = 0;
    }

    return bt_gatt_attr_read(conn, attr, buf, len, offset, value, sizeof(*value));
}

static ssize_t write_rtc(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                         const void *buf, uint16_t len, uint16_t offset, uint8_t flags) {
    struct rtc_data *value = (struct rtc_data *) attr->user_data;

    if (offset + len > sizeof(*value)) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }

    memcpy(((uint8_t *) value) + offset, buf, len);

    // Validate received data
    if (value->month < 1 || value->month > 12 || value->day < 1 || value->day > 31 ||
        value->hour > 23 || value->minute > 59 || value->second > 59 || value->day_of_week > 6) {
        return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
    }

    // Update the RTC with timeout protection
    rtc_datetime_t new_time;
    new_time.year = 2000 + value->year; // Convert back to full year
    new_time.month = value->month;
    new_time.day = value->day;
    new_time.hour = value->hour;
    new_time.minute = value->minute;
    new_time.second = value->second;
    new_time.day_of_week = value->day_of_week;

    // Simplify the RTC update process to avoid potential deadlocks
    printk("Initiating RTC update (with simplified timeout protection)\n");
    int ret = rtc_datetime_set(&new_time);

    // Check for errors
    if (ret != 0) {
        printk("Error setting RTC: %d\n", ret);
        return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
    }

    printk("RTC updated via Bluetooth: %02d/%02d/%04d %02d:%02d:%02d (day %d)\n",
           new_time.day, new_time.month, new_time.year,
           new_time.hour, new_time.minute, new_time.second,
           new_time.day_of_week);

    return len;
}

/* Alarm implementation */
static ssize_t read_alarm(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                          void *buf, uint16_t len, uint16_t offset) {
    struct alarm_data *value = (struct alarm_data *) alarm_value;

    return bt_gatt_attr_read(conn, attr, buf, len, offset, value, sizeof(*value));
}

static ssize_t write_alarm(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                           const void *buf, uint16_t len, uint16_t offset, uint8_t flags) {
    struct alarm_data *value = (struct alarm_data *) alarm_value;

    if (offset + len > sizeof(*value)) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }

    if (offset != 0) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }

    if (len == 1) {
        // Single byte write to clear alarm
        uint8_t clear_command = ((uint8_t*)buf)[0];
        if (clear_command == 0) {
            // Clear all alarms
            value->alarm_code = 0;
            value->alarm_data = 0;
            value->timestamp = k_uptime_get_32();
            printk("All alarms cleared via BLE\n");
            
            // CRITICAL FIX: Reset system status from fault state when clearing alarms
            watering_clear_errors();
        } else if (clear_command == value->alarm_code) {
            // Clear specific alarm
            value->alarm_code = 0;
            value->alarm_data = 0;
            value->timestamp = k_uptime_get_32();
            printk("Alarm %d cleared via BLE\n", clear_command);
            
            // CRITICAL FIX: Reset system status from fault state when clearing specific alarm
            watering_clear_errors();
        } else {
            return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
        }
    } else if (len == sizeof(*value)) {
        // Full structure write (advanced usage)
        memcpy((uint8_t*)value + offset, buf, len);
        printk("Alarm data updated via BLE\n");
    } else {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }

    // Notify clients about the alarm state change
    if (default_conn) {
        send_simple_notification(default_conn, &irrigation_svc.attrs[ATTR_IDX_ALARM_VALUE], 
                       value, sizeof(*value));
    }

    return len;
}

static void alarm_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value) {
    bool notif_enabled = (value == BT_GATT_CCC_NOTIFY);
    notification_state.alarm_notifications_enabled = notif_enabled;
    printk("Alarm notifications %s\n", notif_enabled ? "enabled" : "disabled");
    
    if (notif_enabled) {
        /* Only send notifications when real alarms occur - don't send on enable */
        printk("Alarm notifications enabled - will send when alarms occur\n");
    }
}

/* Calibration implementation */
static ssize_t read_calibration(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                void *buf, uint16_t len, uint16_t offset) {
    struct calibration_data *value = (struct calibration_data *) calibration_value;

    if (calibration_active) {
        uint32_t current_pulses = get_pulse_count();
        value->pulses = current_pulses - calibration_start_pulses;
        value->action = 2; // In progress
    }

    return bt_gatt_attr_read(conn, attr, buf, len, offset, value, sizeof(*value));
}

static ssize_t write_calibration(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                 const void *buf, uint16_t len, uint16_t offset, uint8_t flags) {
    struct calibration_data *value = (struct calibration_data *) attr->user_data;

    if (offset + len > sizeof(*value)) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }

    memcpy(((uint8_t *) value) + offset, buf, len);

    // Process calibration request
    if (value->action == 1) {
        // Start calibration
        if (!calibration_active) {
            reset_pulse_count();
            calibration_start_pulses = 0;
            calibration_active = true;
            value->pulses = 0;
            printk("Flow sensor calibration started\n");
        }
    } else if (value->action == 0) {
        // Stop calibration and calculate
        if (calibration_active) {
            uint32_t final_pulses = get_pulse_count();
            uint32_t total_pulses = final_pulses - calibration_start_pulses;
            uint32_t volume_ml = value->volume_ml;

            if (volume_ml > 0 && total_pulses > 0) {
                uint32_t new_calibration = (total_pulses * 1000) / volume_ml;
                value->pulses_per_liter = new_calibration;

                // Update system calibration
                watering_set_flow_calibration(new_calibration);
                watering_save_config_priority(true);

                printk("Flow sensor calibration completed: %d pulses for %d ml = %d pulses/liter\n",
                       total_pulses, volume_ml, new_calibration);

                // Set completed state
                value->action = 3; // Calibration calculated
                value->pulses = total_pulses;
            }
            calibration_active = false;
        }
    }

    return len;
}

static void calibration_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value) {
    bool notif_enabled = (value == BT_GATT_CCC_NOTIFY);
    notification_state.calibration_notifications_enabled = notif_enabled;
    printk("Calibration notifications %s\n", notif_enabled ? "enabled" : "disabled");
    
    if (notif_enabled) {
        /* Only send notifications when calibration state changes - don't send on enable */
        printk("Calibration notifications enabled - will send when calibration state changes\n");
    }
}

/* History implementation with full history system integration */
static ssize_t read_history(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                            void *buf, uint16_t len, uint16_t offset) {
    struct history_data *value = (struct history_data *) history_value;

    // Return current state of the history request/response
    return bt_gatt_attr_read(conn, attr, buf, len, offset, value, sizeof(*value));
}

static ssize_t write_history(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                             const void *buf, uint16_t len, uint16_t offset, uint8_t flags) {
    struct history_data *value = (struct history_data *) attr->user_data;
    watering_error_t err;

    if (offset + len > sizeof(*value)) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }

    memcpy(((uint8_t *) value) + offset, buf, len);

    // Process history request based on type
    if (value->channel_id < WATERING_CHANNELS_COUNT || value->channel_id == 0xFF) {
        printk("History request: channel=%d, type=%d, index=%d, count=%d\n",
               value->channel_id, value->history_type, value->entry_index, value->count);

        switch (value->history_type) {
            case 0: // Detailed events
                {
                    watering_event_detailed_t events[10];
                    uint16_t event_count = 10;
                    
                    // Get detailed events from history system
                    err = watering_history_get_detailed_events(
                        value->channel_id,  // Don't convert 0xFF to 0
                        value->start_timestamp,
                        value->end_timestamp,
                        events,
                        &event_count
                    );
                    
                    if (err == WATERING_SUCCESS && event_count > value->entry_index) {
                        // Fill response with the requested entry
                        watering_event_detailed_t *event = &events[value->entry_index];
                        value->data.detailed.timestamp = event->timestamp;
                        value->data.detailed.channel_id = event->channel_id;
                        value->data.detailed.event_type = event->event_type;
                        value->data.detailed.mode = event->mode;
                        value->data.detailed.target_value = event->target_value;
                        value->data.detailed.actual_value = event->actual_value;
                        value->data.detailed.total_volume_ml = event->total_volume_ml;
                        value->data.detailed.trigger_type = event->trigger_type;
                        value->data.detailed.success_status = event->success_status;
                        value->data.detailed.error_code = event->error_code;
                        value->data.detailed.flow_rate_avg = event->flow_rate_avg;
                        value->data.detailed.reserved[0] = 0;
                        value->data.detailed.reserved[1] = 0;
                        value->count = event_count;
                    } else {
                        // No data available or error
                        value->count = 0;
                        memset(&value->data.detailed, 0, sizeof(value->data.detailed));
                    }
                }
                break;
                
            case 1: // Daily stats
                {
                    daily_stats_t stats[31];
                    uint16_t stats_count = 31;
                    
                    // Get daily stats for the last 31 days
                    uint32_t current_time = k_uptime_get_32() / 1000;
                    uint16_t current_day = (current_time / 86400) % 365;
                    uint16_t current_year = 2024 + (current_time / (365 * 86400));
                    
                    err = watering_history_get_daily_stats(
                        value->channel_id == 0xFF ? 0 : value->channel_id,
                        current_day > 30 ? current_day - 30 : 0,
                        current_day,
                        current_year,
                        stats,
                        &stats_count
                    );
                    
                    if (err == WATERING_SUCCESS && stats_count > value->entry_index) {
                        daily_stats_t *stat = &stats[value->entry_index];
                        value->data.daily.day_index = stat->day_index;
                        value->data.daily.year = stat->year;
                        value->data.daily.watering_sessions = stat->watering_sessions;
                        value->data.daily.total_volume_ml = stat->total_volume_ml;
                        value->data.daily.total_duration_sec = stat->total_duration_sec;
                        value->data.daily.avg_flow_rate = stat->avg_flow_rate;
                        value->data.daily.success_rate = stat->success_rate;
                        value->data.daily.error_count = stat->error_count;
                        value->count = stats_count;
                    } else {
                        value->count = 0;
                        memset(&value->data.daily, 0, sizeof(value->data.daily));
                    }
                }
                break;
                
            case 2: // Monthly stats
                {
                    monthly_stats_t stats[12];
                    uint16_t stats_count = 12;
                    
                    uint32_t current_time = k_uptime_get_32() / 1000;
                    uint16_t current_year = 2024 + (current_time / (365 * 86400));
                    
                    err = watering_history_get_monthly_stats(
                        value->channel_id == 0xFF ? 0 : value->channel_id,
                        1, 12, current_year,
                        stats,
                        &stats_count
                    );
                    
                    if (err == WATERING_SUCCESS && stats_count > value->entry_index) {
                        monthly_stats_t *stat = &stats[value->entry_index];
                        value->data.monthly.month = stat->month;
                        value->data.monthly.year = stat->year;
                        value->data.monthly.total_sessions = stat->total_sessions;
                        value->data.monthly.total_volume_ml = stat->total_volume_ml;
                        value->data.monthly.total_duration_hours = stat->total_duration_hours;
                        value->data.monthly.avg_daily_volume = stat->avg_daily_volume;
                        value->data.monthly.active_days = stat->active_days;

                        value->data.monthly.success_rate = stat->success_rate;
                    } else {
                        value->count = 0;
                        memset(&value->data.monthly, 0, sizeof(value->data.monthly));
                    }
                }
                break;
                
            case 3: // Annual stats
                {
                    annual_stats_t stats[5];
                    uint16_t stats_count = 5;
                    
                    uint32_t current_time = k_uptime_get_32() / 1000;
                    uint16_t current_year = 2024 + (current_time / (365 * 86400));
                    
                    err = watering_history_get_annual_stats(
                        value->channel_id == 0xFF ? 0 : value->channel_id,
                        current_year > 4 ? current_year - 4 : 2024,
                        current_year,
                        stats,
                        &stats_count
                    );
                    
                    if (err == WATERING_SUCCESS && stats_count > value->entry_index) {
                        annual_stats_t *stat = &stats[value->entry_index];
                        value->data.annual.year = stat->year;
                        value->data.annual.total_sessions = stat->total_sessions;
                        value->data.annual.total_volume_liters = stat->total_volume_liters;
                        value->data.annual.avg_monthly_volume = stat->avg_monthly_volume;
                        value->data.annual.most_active_month = stat->most_active_month;
                        value->data.annual.success_rate = stat->success_rate;
                        value->data.annual.peak_month_volume = stat->peak_month_volume;
                        value->count = stats_count;
                    } else {
                        value->count = 0;
                        memset(&value->data.annual, 0, sizeof(value->data.annual));
                    }
                }
                break;
                
            default:
                // Invalid history type
                value->count = 0;
                printk("Invalid history type: %d\n", value->history_type);
                break;
        }
    } else {
        // Invalid channel ID
        value->count = 0;
        printk("Invalid channel ID: %d\n", value->channel_id);
    }

    return len;
}

static void history_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value) {
    bool notif_enabled = (value == BT_GATT_CCC_NOTIFY);
    notification_state.history_notifications_enabled = notif_enabled;
    printk("History notifications %s\n", notif_enabled ? "enabled" : "disabled");
    
    if (notif_enabled) {
        /* Only send notifications when history is updated - don't send on enable */
        printk("History notifications enabled - will send when history updates\n");
    }
}

/* Diagnostics implementation */
static ssize_t read_diagnostics(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                void *buf, uint16_t len, uint16_t offset) {
    struct diagnostics_data *value = (struct diagnostics_data *) diagnostics_value;

    // Collect diagnostic information
    value->uptime = k_uptime_get_32() / 60000; // Convert to minutes

    // Here we should collect real data about errors, valve status, etc.
    // For now we use dummy values
    value->error_count = 0;
    value->last_error = 0;

    // Create a bitmap with valve status
    value->valve_status = 0;
    for (int i = 0; i < WATERING_CHANNELS_COUNT && i < 8; i++) {
        if (watering_channels[i].is_active) {
            value->valve_status |= (1 << i);
        }
    }

    // We don't have battery monitoring in this system
    value->battery_level = 0xFF;

    return bt_gatt_attr_read(conn, attr, buf, len, offset, value, sizeof(*value));
}

static void diagnostics_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value) {
    bool notif_enabled = (value == BT_GATT_CCC_NOTIFY);
    notification_state.diagnostics_notifications_enabled = notif_enabled;
    printk("Diagnostics notifications %s\n", notif_enabled ? "enabled" : "disabled");
    
    if (notif_enabled) {
        /* Only send notifications when diagnostics change - don't send on enable */
        printk("Diagnostics notifications enabled - will send when diagnostics update\n");
    }
}

/* Schedule Configuration characteristic implementation */
static ssize_t read_schedule(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                            void *buf, uint16_t len, uint16_t offset) {
    struct schedule_config_data *value = (struct schedule_config_data *)attr->user_data;
    
    // Ensure we have a valid channel selected (default to 0 if none set)
    if (value->channel_id >= WATERING_CHANNELS_COUNT) {
        value->channel_id = 0; // Default to first channel
    }
    
    printk("Schedule read: channel %d selected\n", value->channel_id);
    
    watering_channel_t *channel;
    if (watering_get_channel(value->channel_id, &channel) == WATERING_SUCCESS) {
        // Map schedule type and timing information
        value->schedule_type = (uint8_t)channel->watering_event.schedule_type;
        if (channel->watering_event.schedule_type == SCHEDULE_DAILY) {
            value->days_mask = channel->watering_event.schedule.daily.days_of_week;
        } else {
            value->days_mask = channel->watering_event.schedule.periodic.interval_days;
        }
        value->hour = channel->watering_event.start_time.hour;
        value->minute = channel->watering_event.start_time.minute;
        value->watering_mode = (uint8_t)channel->watering_event.watering_mode;
        
        // Map watering value based on mode
        if (channel->watering_event.watering_mode == WATERING_BY_DURATION) {
            value->value = channel->watering_event.watering.by_duration.duration_minutes;
        } else {
            value->value = channel->watering_event.watering.by_volume.volume_liters;
        }
        
        value->auto_enabled = channel->watering_event.auto_enabled ? 1 : 0;
    } else {
        printk("Failed to get channel %d for schedule read\n", value->channel_id);
        // Set default values if channel retrieval fails
        value->schedule_type = 0;
        value->days_mask = 0;
        value->hour = 0;
        value->minute = 0;
        value->watering_mode = 0;
        value->value = 0;
        value->auto_enabled = 0;
    }
    
    return bt_gatt_attr_read(conn, attr, buf, len, offset, value, sizeof(*value));
}

static ssize_t write_schedule(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                             const void *buf, uint16_t len, uint16_t offset, uint8_t flags) {
    struct schedule_config_data *value = (struct schedule_config_data *)attr->user_data;
    
    if (offset + len > sizeof(*value)) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }
    
    // Check for select-for-read operation (1 byte = channel_id only)
    if (len == 1 && offset == 0) {
        uint8_t channel_id = *((const uint8_t *)buf);
        if (channel_id >= WATERING_CHANNELS_COUNT) {
            return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
        }
        
        // Store the selected channel for subsequent read operations
        value->channel_id = channel_id;
        printk("Schedule select-for-read: channel %d selected\n", channel_id);
        return len;  // Don't save to flash, just select for read
    }
    
    memcpy(((uint8_t *)value) + offset, buf, len);
    
    if (value->channel_id >= WATERING_CHANNELS_COUNT) {
        return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
    }
    
    watering_channel_t *channel;
    if (watering_get_channel(value->channel_id, &channel) == WATERING_SUCCESS) {
        // Set schedule type and timing
        channel->watering_event.schedule_type = (schedule_type_t)value->schedule_type;
        if (channel->watering_event.schedule_type == SCHEDULE_DAILY) {
            channel->watering_event.schedule.daily.days_of_week = value->days_mask;
        } else {
            channel->watering_event.schedule.periodic.interval_days = value->days_mask;
        }
        channel->watering_event.start_time.hour = value->hour;
        channel->watering_event.start_time.minute = value->minute;
        
        // Set watering mode and value
        channel->watering_event.watering_mode = (watering_mode_t)value->watering_mode;
        if (channel->watering_event.watering_mode == WATERING_BY_DURATION) {
            channel->watering_event.watering.by_duration.duration_minutes = (uint8_t)value->value;
        } else {
            channel->watering_event.watering.by_volume.volume_liters = value->value;
        }
        
        channel->watering_event.auto_enabled = value->auto_enabled ? true : false;
        
        // Throttle config saves during schedule updates to prevent freeze
        static uint32_t last_schedule_save = 0;
        uint32_t now = k_uptime_get_32();
        if (now - last_schedule_save > 250) { // Minimum 250ms between schedule saves
            watering_save_config_priority(true);
            last_schedule_save = now;
        }
    }
    
    return len;
}

static void schedule_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value) {
    bool notif_enabled = (value == BT_GATT_CCC_NOTIFY);
    notification_state.schedule_notifications_enabled = notif_enabled;
    printk("Schedule notifications %s\n", notif_enabled ? "enabled" : "disabled");
}

/* System Configuration characteristic implementation */
static ssize_t read_system_config(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                 void *buf, uint16_t len, uint16_t offset) {
    struct system_config_data *value = (struct system_config_data *)attr->user_data;
    
    // Update with current system configuration
    value->version = 1;
    value->power_mode = 0; // Normal power mode
    value->flow_calibration = 750; // Default pulses per liter (per documentation)
    value->max_active_valves = 1;
    value->num_channels = WATERING_CHANNELS_COUNT;
    
    return bt_gatt_attr_read(conn, attr, buf, len, offset, value, sizeof(*value));
}

static ssize_t write_system_config(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                  const void *buf, uint16_t len, uint16_t offset, uint8_t flags) {
    struct system_config_data *value = (struct system_config_data *)attr->user_data;
    
    if (offset + len > sizeof(*value)) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }
    
    memcpy(((uint8_t *)value) + offset, buf, len);
    
    // Apply system configuration changes
    if (value->flow_calibration > 0) {
        watering_set_flow_calibration(value->flow_calibration);
    }
    
    // Throttle config saves during system config updates to prevent freeze
    static uint32_t last_sys_save = 0;
    uint32_t now = k_uptime_get_32();
    if (now - last_sys_save > 250) { // Minimum 250ms between system config saves
        watering_save_config_priority(true);
        last_sys_save = now;
    }
    
    return len;
}

static void system_config_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value) {
    bool notif_enabled = (value == BT_GATT_CCC_NOTIFY);
    notification_state.system_config_notifications_enabled = notif_enabled;
    printk("System config notifications %s\n", notif_enabled ? "enabled" : "disabled");
}

/* Task Queue characteristic implementation */
static ssize_t read_task_queue(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                              void *buf, uint16_t len, uint16_t offset) {
    struct task_queue_data *value = (struct task_queue_data *)attr->user_data;
    
    // Update with current task queue status
    value->pending_count = watering_get_pending_tasks_count();
    
    // Get actual completed tasks count if available
    uint8_t pending_count;
    bool active;
    if (watering_get_queue_status(&pending_count, &active) == WATERING_SUCCESS) {
        value->completed_tasks = 0; // Note: completed tasks counter not available in current watering system
        value->current_channel = active ? 0 : 0xFF; // Active channel detection requires watering system enhancement
    } else {
        value->completed_tasks = 0;
        value->current_channel = 0xFF; // No active channel by default
    }
    
    value->current_task_type = 0;
    value->current_value = 0;
    
    return bt_gatt_attr_read(conn, attr, buf, len, offset, value, sizeof(*value));
}

static ssize_t write_task_queue(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                               const void *buf, uint16_t len, uint16_t offset, uint8_t flags) {
    struct task_queue_data *value = (struct task_queue_data *)attr->user_data;
    
    if (offset + len > sizeof(*value)) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }
    
    memcpy(((uint8_t *)value) + offset, buf, len);
    
    // Process task queue commands
    if (value->command == 1) { // Cancel current task
        printk("Task queue: Cancel current task command received\n");
        bool stopped = watering_stop_current_task();
        if (!stopped) {
            printk("No active task to cancel\n");
        } else {
            printk("Current task cancelled successfully\n");
        }
    } else if (value->command == 2) { // Clear entire queue
        printk("Task queue: Clear entire queue command received\n");
        int cleared = watering_clear_task_queue();
        printk("Cleared %d tasks from queue\n", cleared);
    } else if (value->command == 3) { // Delete specific task
        printk("Task queue: Delete task ID %d command received\n", value->task_id_to_delete);
        // Note: Specific task deletion by ID is not currently supported in the watering system
        // as tasks are stored in a simple queue without IDs
        printk("Specific task deletion not supported - use clear entire queue instead\n");
    } else if (value->command == 4) { // Clear error state
        printk("Task queue: Clear error state command received\n");
        watering_error_t result = watering_clear_errors();
        if (result != WATERING_SUCCESS) {
            printk("Failed to clear error state: %d\n", result);
            return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
        } else {
            printk("Error state cleared successfully\n");
        }
    }
    
    return len;
}

static void task_queue_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value) {
    bool notif_enabled = (value == BT_GATT_CCC_NOTIFY);
    notification_state.task_queue_notifications_enabled = notif_enabled;
    printk("Task queue notifications %s\n", notif_enabled ? "enabled" : "disabled");
}

/* Statistics characteristic implementation */
static ssize_t read_statistics(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                              void *buf, uint16_t len, uint16_t offset) {
    struct statistics_data *value = (struct statistics_data *)attr->user_data;
    
    if (value->channel_id >= WATERING_CHANNELS_COUNT) {
        value->channel_id = 0; // Default to first channel
    }
    
    // Read actual statistics from watering system if available
    // Note: Current watering system doesn't have centralized statistics
    // Statistics are tracked per watering event in the history system
    value->last_volume = 0;
    value->last_watering = 0;
    value->total_volume = 0;
    value->count = 0;
    
    return bt_gatt_attr_read(conn, attr, buf, len, offset, value, sizeof(*value));
}

static ssize_t write_statistics(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                               const void *buf, uint16_t len, uint16_t offset, uint8_t flags) {
    struct statistics_data *value = (struct statistics_data *)attr->user_data;
    
    if (offset + len > sizeof(*value)) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }
    
    memcpy(((uint8_t *)value) + offset, buf, len);
    
    // Statistics are read-only, but we can use this to select channel
    if (value->channel_id >= WATERING_CHANNELS_COUNT) {
        return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
    }
    
    return len;
}

static void statistics_ccc_cfg_changed(const struct bt_gatt_attr *attr, uint16_t value) {
    bool notif_enabled = (value == BT_GATT_CCC_NOTIFY);
    notification_state.statistics_notifications_enabled = notif_enabled;
    printk("Statistics notifications %s\n", notif_enabled ? "enabled" : "disabled");
}

/* RTC CCC callback */
static void rtc_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value) {
    bool notif_enabled = (value == BT_GATT_CCC_NOTIFY);
    notification_state.rtc_notifications_enabled = notif_enabled;
    printk("RTC notifications %s\n", notif_enabled ? "enabled" : "disabled");
}

/* Growing Environment characteristic implementation */
static ssize_t read_growing_env(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                               void *buf, uint16_t len, uint16_t offset) {
    struct growing_env_data *value = (struct growing_env_data *)attr->user_data;
    
    if (value->channel_id >= WATERING_CHANNELS_COUNT) {
        value->channel_id = 0; // Default to first channel
    }
    
    watering_channel_t *channel;
    if (watering_get_channel(value->channel_id, &channel) == WATERING_SUCCESS) {
        value->plant_type = (uint8_t)channel->plant_type;
        value->soil_type = (uint8_t)channel->soil_type;
        value->irrigation_method = (uint8_t)channel->irrigation_method;
        value->use_area_based = channel->coverage.use_area ? 1 : 0;
        
        if (channel->coverage.use_area) {
            value->coverage.area_m2 = channel->coverage.area.area_m2;
        } else {
            value->coverage.plant_count = channel->coverage.plants.count;
        }
        
        value->sun_percentage = channel->sun_percentage;
        
        /* Include custom plant fields if plant_type is CUSTOM (7) */
        if (channel->plant_type == 7) {
            strncpy(value->custom_name, channel->custom_plant.custom_name, 
                    sizeof(value->custom_name) - 1);
            value->custom_name[sizeof(value->custom_name) - 1] = '\0';
            value->water_need_factor = channel->custom_plant.water_need_factor;
            value->irrigation_freq_days = channel->custom_plant.irrigation_freq;
            value->prefer_area_based = channel->custom_plant.prefer_area_based ? 1 : 0;
        } else {
            /* Clear custom fields for non-custom plants */
            memset(value->custom_name, 0, sizeof(value->custom_name));
            value->water_need_factor = 1.0f;
            value->irrigation_freq_days = 1;
            value->prefer_area_based = 0;
        }
    }
    
    return bt_gatt_attr_read(conn, attr, buf, len, offset, value, sizeof(*value));
}

static ssize_t write_growing_env(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                const void *buf, uint16_t len, uint16_t offset, uint8_t flags) {
    struct growing_env_data *value = (struct growing_env_data *)attr->user_data;
    
    printk("Growing env write: offset=%d, len=%d, flags=0x%02X, struct_size=%d\n", 
           offset, len, flags, sizeof(*value));
    
    /* Handle fragmentation protocol (similar to channel config) */
    if (len >= 3 && offset == 0) {
        uint8_t *data = (uint8_t *)buf;
        uint8_t cid = data[0];
        uint8_t frag_type = data[1];
        uint8_t total = data[2];
        
        printk("Growing env fragmentation: cid=%d, frag_type=%d, total=%d\n", cid, frag_type, total);
        
        if (frag_type == 2) { /* Growing Environment fragmentation */
            if (total != sizeof(*value)) {
                printk("Growing env write error: invalid struct size total=%d, expected=%d\n", 
                       total, (int)sizeof(*value));
                return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
            }
            
            uint8_t pay_len = len - 3;
            uint8_t *payload = data + 3;
            
            if (!growing_env_frag.in_progress) {
                growing_env_frag.channel_id = cid;
                growing_env_frag.expected = total;
                growing_env_frag.received = 0;
                growing_env_frag.in_progress = true;
                memset(growing_env_frag.buf, 0, sizeof(growing_env_frag.buf));
                printk("Starting growing env fragment for channel %d, expected %d bytes\n", cid, total);
            }
            
            /* Check for consistency and overflow */
            if (cid != growing_env_frag.channel_id || 
                growing_env_frag.received + pay_len > growing_env_frag.expected ||
                growing_env_frag.received + pay_len > sizeof(growing_env_frag.buf)) {
                printk("Growing env fragment error: mismatch or overflow\n");
                printk("  Channel ID: expected=%d, got=%d\n", growing_env_frag.channel_id, cid);
                printk("  Size check: received=%d + pay_len=%d = %d, expected=%d, buf_size=%d\n", 
                       growing_env_frag.received, pay_len, growing_env_frag.received + pay_len, 
                       growing_env_frag.expected, (int)sizeof(growing_env_frag.buf));
                growing_env_frag.in_progress = false;
                return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
            }
            
            /* Append data */
            memcpy(growing_env_frag.buf + growing_env_frag.received, payload, pay_len);
            growing_env_frag.received += pay_len;
            printk("Received growing env fragment: %d/%d bytes\n", 
                   growing_env_frag.received, growing_env_frag.expected);
            
            /* Complete? */
            if (growing_env_frag.received >= growing_env_frag.expected) {
                /* Copy complete structure and process it */
                memcpy(value, growing_env_frag.buf, sizeof(*value));
                growing_env_frag.in_progress = false;
                
                printk("Complete growing env received, processing...\n");
                
                /* Process the complete structure */
                if (value->channel_id >= WATERING_CHANNELS_COUNT) {
                    return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
                }
                
                watering_channel_t *channel;
                if (watering_get_channel(value->channel_id, &channel) != WATERING_SUCCESS) {
                    return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
                }
                
                /* Update growing environment settings */
                channel->plant_type = (plant_type_t)value->plant_type;
                channel->soil_type = (soil_type_t)value->soil_type;
                channel->irrigation_method = (irrigation_method_t)value->irrigation_method;
                channel->coverage.use_area = (value->use_area_based == 1);
                
                if (channel->coverage.use_area) {
                    channel->coverage.area.area_m2 = value->coverage.area_m2;
                } else {
                    channel->coverage.plants.count = value->coverage.plant_count;
                }
                
                channel->sun_percentage = value->sun_percentage;
                
                /* Handle custom plant fields if plant_type is CUSTOM (7) */
                if (value->plant_type == 7) {
                    /* Update custom plant name with proper bounds checking */
                    if (strlen(value->custom_name) > 0) {
                        strncpy(channel->custom_plant.custom_name, value->custom_name, 
                                sizeof(channel->custom_plant.custom_name) - 1);
                        channel->custom_plant.custom_name[sizeof(channel->custom_plant.custom_name) - 1] = '\0';
                    }
                    
                    /* Update custom plant parameters */
                    channel->custom_plant.water_need_factor = value->water_need_factor;
                    channel->custom_plant.irrigation_freq = value->irrigation_freq_days;
                    channel->custom_plant.prefer_area_based = (value->prefer_area_based == 1);
                }
                
                /* Priority save for BLE operations */
                watering_save_config_priority(true);
                
                /* Send notification to BLE client */
                bt_irrigation_growing_env_update(value->channel_id);
                
                printk("Growing environment updated for channel %d via fragmentation\n", value->channel_id);
            }
            return len;     /* ACK current fragment */
        }
    }
    
    /* Handle PREPARE/EXECUTE write protocol */
    if (flags & BT_GATT_WRITE_FLAG_PREPARE) {
        if (offset + len > sizeof(*value)) {
            return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
        }
        memcpy(((uint8_t *)value) + offset, buf, len);
        return len;
    }
    
    if (flags & BT_GATT_WRITE_FLAG_EXECUTE) {
        printk("Growing env EXECUTE write: struct_size=%d\n", sizeof(*value));
        
        if (value->channel_id >= WATERING_CHANNELS_COUNT) {
            return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
        }
        
        watering_channel_t *channel;
        if (watering_get_channel(value->channel_id, &channel) != WATERING_SUCCESS) {
            return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
        }
        
        /* Update growing environment settings */
        channel->plant_type = (plant_type_t)value->plant_type;
        channel->soil_type = (soil_type_t)value->soil_type;
        channel->irrigation_method = (irrigation_method_t)value->irrigation_method;
        channel->coverage.use_area = (value->use_area_based == 1);
        
        if (channel->coverage.use_area) {
            channel->coverage.area.area_m2 = value->coverage.area_m2;
        } else {
            channel->coverage.plants.count = value->coverage.plant_count;
        }
        
        channel->sun_percentage = value->sun_percentage;
        
        /* Handle custom plant fields if plant_type is CUSTOM (7) */
        if (value->plant_type == 7) {
            /* Update custom plant name with proper bounds checking */
            if (strlen(value->custom_name) > 0) {
                strncpy(channel->custom_plant.custom_name, value->custom_name, 
                        sizeof(channel->custom_plant.custom_name) - 1);
                channel->custom_plant.custom_name[sizeof(channel->custom_plant.custom_name) - 1] = '\0';
            }
            
            /* Update custom plant parameters */
            channel->custom_plant.water_need_factor = value->water_need_factor;
            channel->custom_plant.irrigation_freq = value->irrigation_freq_days;
            channel->custom_plant.prefer_area_based = (value->prefer_area_based == 1);
        }
        
        /* Throttle config saves to prevent system freeze during BLE operations */
        static uint32_t last_execute_save = 0;
        uint32_t now = k_uptime_get_32();
        if (now - last_execute_save > 250) { // Minimum 250ms between priority saves
            watering_save_config_priority(true);
            last_execute_save = now;
        }
        
        /* Send notification to BLE client */
        bt_irrigation_growing_env_update(value->channel_id);
        
        return sizeof(struct growing_env_data); /* success */
    }
    
    /* Regular write protocol */
    printk("Growing env regular write: offset=%d, len=%d, struct_size=%d\n", offset, len, sizeof(*value));
    if (offset + len > sizeof(*value)) {
        printk("Growing env regular write error: offset+len > struct size (%d > %d)\n", 
               offset + len, sizeof(*value));
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }
    
    memcpy(((uint8_t *)value) + offset, buf, len);
    printk("Growing env regular write: offset=%d, len=%d, total=%d\n", offset, len, offset + len);
    
    /* If entire struct fits in one request, commit immediately */
    if (offset + len == sizeof(*value)) {
        printk("Growing env regular write: complete struct received, committing\n");
        
        if (value->channel_id >= WATERING_CHANNELS_COUNT) {
            printk("Growing env regular write error: invalid channel_id %d\n", value->channel_id);
            return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
        }
        
        watering_channel_t *channel;
        if (watering_get_channel(value->channel_id, &channel) != WATERING_SUCCESS) {
            printk("Growing env regular write error: failed to get channel %d\n", value->channel_id);
            return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
        }
        
        /* Update growing environment settings */
        channel->plant_type = (plant_type_t)value->plant_type;
        channel->soil_type = (soil_type_t)value->soil_type;
        channel->irrigation_method = (irrigation_method_t)value->irrigation_method;
        channel->coverage.use_area = (value->use_area_based == 1);
        
        if (channel->coverage.use_area) {
            channel->coverage.area.area_m2 = value->coverage.area_m2;
        } else {
            channel->coverage.plants.count = value->coverage.plant_count;
        }
        
        channel->sun_percentage = value->sun_percentage;
        
        /* Handle custom plant fields if plant_type is CUSTOM (7) */
        if (value->plant_type == 7) {
            /* Update custom plant name with proper bounds checking */
            if (strlen(value->custom_name) > 0) {
                strncpy(channel->custom_plant.custom_name, value->custom_name, 
                        sizeof(channel->custom_plant.custom_name) - 1);
                channel->custom_plant.custom_name[sizeof(channel->custom_plant.custom_name) - 1] = '\0';
            }
            
            /* Update custom plant parameters */
            channel->custom_plant.water_need_factor = value->water_need_factor;
            channel->custom_plant.irrigation_freq = value->irrigation_freq_days;
            channel->custom_plant.prefer_area_based = (value->prefer_area_based == 1);
        }
        
        /* Throttle config saves during BLE operations to prevent freeze */
        static uint32_t last_bt_save = 0;
        uint32_t now = k_uptime_get_32();
        if (now - last_bt_save > 250) { // Minimum 250ms between BLE-triggered saves
            watering_save_config_priority(true);
            last_bt_save = now;
        }
        
        /* Send notification to BLE client */
        bt_irrigation_growing_env_update(value->channel_id);
        
        printk("Growing environment updated for channel %d via regular write\n", value->channel_id);
    }
    
    return len;
}

static void growing_env_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value) {
    bool notif_enabled = (value == BT_GATT_CCC_NOTIFY);
    notification_state.growing_env_notifications_enabled = notif_enabled;
    printk("Growing environment notifications %s\n", notif_enabled ? "enabled" : "disabled");
}

/* Current Task characteristic implementation */
static ssize_t read_current_task(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                void *buf, uint16_t len, uint16_t offset) {
    struct current_task_data *value = (struct current_task_data *)attr->user_data;
    
    // Update with current task status
    if (watering_task_state.task_in_progress && watering_task_state.current_active_task) {
        uint8_t channel_id = watering_task_state.current_active_task->channel - watering_channels;
        if (channel_id < WATERING_CHANNELS_COUNT) {
            watering_channel_t *channel = watering_task_state.current_active_task->channel;
            
            value->channel_id = channel_id;
            value->status = 1; // Running
            value->start_time = watering_task_state.watering_start_time / 1000; // Convert to seconds
            value->mode = (uint8_t)channel->watering_event.watering_mode;
            
            // Calculate total volume from flow sensor for both modes
            uint32_t pulses = get_pulse_count();
            uint32_t calibration;
            if (watering_get_flow_calibration(&calibration) == WATERING_SUCCESS && calibration > 0) {
                value->total_volume = (pulses * 1000) / calibration; // Convert pulses to ml
            } else {
                value->total_volume = 0;
            }
            
            // Set target value and current progress based on mode
            if (channel->watering_event.watering_mode == WATERING_BY_DURATION) {
                value->target_value = channel->watering_event.watering.by_duration.duration_minutes * 60; // Convert to seconds
                value->current_value = (k_uptime_get_32() / 1000) - value->start_time; // Elapsed seconds
                value->reserved = 0; // Not used for duration mode
            } else {
                value->target_value = channel->watering_event.watering.by_volume.volume_liters * 1000; // Convert to ml
                value->current_value = value->total_volume; // Current volume from flow sensor
                value->reserved = (k_uptime_get_32() / 1000) - value->start_time; // Elapsed time for volume mode
            }
        }
    } else {
        // No active task
        value->channel_id = 0xFF;
        value->status = 0; // Idle
        value->start_time = 0;
        value->mode = 0;
        value->target_value = 0;
        value->current_value = 0;
        value->total_volume = 0;
        value->reserved = 0;
    }
    
    return bt_gatt_attr_read(conn, attr, buf, len, offset, value, sizeof(*value));
}

static ssize_t write_current_task(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                 const void *buf, uint16_t len, uint16_t offset, uint8_t flags) {
    if (offset != 0) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }

    if (len == 1) {
        // Single byte write for simple commands
        uint8_t command = ((uint8_t*)buf)[0];
        
        switch (command) {
            case 0x00: // Stop/Cancel current task
                if (watering_task_state.task_in_progress) {
                    printk("BLE: Stopping current task\n");
                    bool stopped = watering_stop_current_task();
                    if (!stopped) {
                        printk("BLE: Failed to stop current task or no task was running\n");
                        return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
                    }
                    
                    // Send notification about task status change
                    if (default_conn) {
                        send_simple_notification(default_conn, &irrigation_svc.attrs[ATTR_IDX_CURRENT_TASK_VALUE], 
                                       current_task_value, sizeof(struct current_task_data));
                    }
                    
                    printk("BLE: Current task stopped successfully\n");
                } else {
                    printk("BLE: No active task to stop\n");
                    return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
                }
                break;
                
            case 0x01: // Pause current task (if supported)
                if (watering_task_state.task_in_progress) {
                    printk("BLE: Pausing current task\n");
                    // Note: watering_pause_current_task() function would need to be implemented
                    // For now, we'll just return an error
                    return BT_GATT_ERR(BT_ATT_ERR_NOT_SUPPORTED);
                } else {
                    return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
                }
                break;
                
            case 0x02: // Resume current task (if supported)
                if (watering_task_state.task_in_progress) {
                    printk("BLE: Resuming current task\n");
                    // Note: watering_resume_current_task() function would need to be implemented
                    // For now, we'll just return an error
                    return BT_GATT_ERR(BT_ATT_ERR_NOT_SUPPORTED);
                } else {
                    return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
                }
                break;
                
            default:
                return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
        }
    } else if (len == sizeof(struct current_task_data)) {
        // Full structure write (advanced usage - could be used for task modification)
        // For now, we'll just return an error as this is complex to implement safely
        return BT_GATT_ERR(BT_ATT_ERR_NOT_SUPPORTED);
    } else {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }

    return len;
}

static void current_task_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value) {
    bool notif_enabled = (value == BT_GATT_CCC_NOTIFY);
    notification_state.current_task_notifications_enabled = notif_enabled;
    printk("Current task notifications %s\n", notif_enabled ? "enabled" : "disabled");
}

/* ------------------------------------------------------------------ */
/* Simplified Notification System                                     */
/* ------------------------------------------------------------------ */

/**
 * @brief Simple and reliable notification function - no queuing, no work handlers
 */
static int send_simple_notification(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                   const void *data, uint16_t len) {
    if (!conn || !attr || !data) {
        return -EINVAL;
    }
    
    // Check if notification system is disabled
    if (!notification_system_enabled) {
        return -ENODEV;
    }
    
    // Simple throttling - minimum delay between notifications
    uint32_t now = k_uptime_get_32();
    if (now - last_notification_time < NOTIFICATION_DELAY_MS) {
        // Too frequent - skip this notification
        return -EAGAIN;
    }
    
    // Check if notifications are enabled for this characteristic
    if (!is_notification_enabled(attr)) {
        return -EACCES;
    }
    
    // Send notification directly - no queuing, no work handlers
    int ret = bt_gatt_notify(conn, attr, data, len);
    if (ret == 0) {
        last_notification_time = now;
    } else {
        // On error, disable notifications temporarily to prevent spam
        if (ret == -ENOMEM || ret == -EBUSY) {
            notification_system_enabled = false;
            printk("BLE: Notifications temporarily disabled due to error: %d\n", ret);
        }
    }
    
    return ret;
}

/**
 * @brief Re-enable notification system after timeout
 */
static void check_notification_system_recovery(void) {
    if (!notification_system_enabled) {
        uint32_t now = k_uptime_get_32();
        if (now - last_notification_time > 2000) { // 2 second timeout
            notification_system_enabled = true;
            printk("BLE: Notification system re-enabled\n");
        }
    }
}

/* Task update thread stack and thread control block */
#define TASK_UPDATE_THREAD_STACK_SIZE 1024
static K_THREAD_STACK_DEFINE(task_update_thread_stack, TASK_UPDATE_THREAD_STACK_SIZE);
static struct k_thread task_update_thread;

/**
 * @brief Background thread for periodic task status updates
 * 
 * This thread runs continuously and updates the current task status
 * and queue information at regular intervals.
 */
static void task_update_thread_entry(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    printk("Task update thread started\n");

    while (1) {
        // Update current task status if connected
        if (default_conn) {
            // Get current task information
            if (watering_task_state.task_in_progress && watering_task_state.current_active_task) {
                uint8_t channel_id = watering_task_state.current_active_task->channel - watering_channels;
                if (channel_id < WATERING_CHANNELS_COUNT) {
                    // Get channel information for watering mode and duration
                    watering_channel_t *channel = watering_task_state.current_active_task->channel;
                    
                    // Calculate elapsed time and current progress
                    uint32_t current_time_s = k_uptime_get_32() / 1000;
                    uint32_t start_time_s = watering_task_state.watering_start_time / 1000;
                    uint32_t elapsed_time_s = current_time_s - start_time_s;
                    
                    // Calculate current progress value based on watering mode
                    uint32_t current_value;
                    uint32_t target_value;
                    uint32_t total_volume_ml = 0;
                    
                    if (channel->watering_event.watering_mode == WATERING_BY_DURATION) {
                        // For duration-based: target in seconds, current is elapsed time
                        target_value = channel->watering_event.watering.by_duration.duration_minutes * 60;
                        current_value = elapsed_time_s;
                    } else {
                        // For volume-based: target in ml, current is measured volume from flow sensor
                        target_value = channel->watering_event.watering.by_volume.volume_liters * 1000;
                        // Calculate volume from flow sensor
                        uint32_t pulses = get_pulse_count();
                        uint32_t calibration;
                        if (watering_get_flow_calibration(&calibration) == WATERING_SUCCESS && calibration > 0) {
                            current_value = (pulses * 1000) / calibration; // Convert pulses to ml
                        } else {
                            current_value = 0; // Fallback if calibration unavailable
                        }
                    }
                    
                    // Calculate total volume dispensed for both modes
                    uint32_t pulses = get_pulse_count();
                    uint32_t calibration;
                    if (watering_get_flow_calibration(&calibration) == WATERING_SUCCESS && calibration > 0) {
                        total_volume_ml = (pulses * 1000) / calibration;
                    }
                    
                    // Update current task characteristic
                    bt_irrigation_current_task_update(
                        channel_id,
                        start_time_s,
                        channel->watering_event.watering_mode,
                        target_value,
                        current_value,
                        total_volume_ml
                    );
                    
                    // Update valve status
                    bt_irrigation_valve_status_update(channel_id, true);
                }
            } else {
                // No active task - clear current task
                bt_irrigation_current_task_update(0xFF, 0, 0, 0, 0, 0);
            }

            // Update queue status
            uint8_t pending_count = watering_get_pending_tasks_count();
            bt_irrigation_queue_status_update(pending_count);
        }

        // Check and recover notification system if needed
        check_notification_system_recovery();

        // Sleep for update interval
        k_sleep(K_SECONDS(2)); // Update every 2 seconds
    }
}

/* ------------------------------------------------------------------ */
/* BLE GATT Service Definition - placed after all handlers           */
/* ------------------------------------------------------------------ */
BT_GATT_SERVICE_DEFINE(irrigation_svc,
    BT_GATT_PRIMARY_SERVICE(&irrigation_service_uuid),
    
    /* Valve Control Characteristic */
    BT_GATT_CHARACTERISTIC(&valve_char_uuid.uuid,
                          BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE | BT_GATT_CHRC_NOTIFY,
                          BT_GATT_PERM_READ | BT_GATT_PERM_WRITE,
                          read_valve, write_valve, valve_value),
    BT_GATT_CCC(valve_ccc_cfg_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
    
    /* Flow Sensor Characteristic */
    BT_GATT_CHARACTERISTIC(&flow_char_uuid.uuid,
                          BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
                          BT_GATT_PERM_READ,
                          read_flow, NULL, flow_value),
    BT_GATT_CCC(flow_ccc_cfg_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
    
    /* System Status Characteristic */
    BT_GATT_CHARACTERISTIC(&status_char_uuid.uuid,
                          BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
                          BT_GATT_PERM_READ,
                          read_status, NULL, status_value),
    BT_GATT_CCC(status_ccc_cfg_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
    
    /* Channel Configuration Characteristic */
    BT_GATT_CHARACTERISTIC(&channel_config_uuid.uuid,
                          BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE | BT_GATT_CHRC_NOTIFY,
                          BT_GATT_PERM_READ | BT_GATT_PERM_WRITE,
                          read_channel_config, write_channel_config, channel_config_value),
    BT_GATT_CCC(channel_config_ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
    
    /* Schedule Configuration Characteristic */
    BT_GATT_CHARACTERISTIC(&schedule_uuid.uuid,
                          BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE | BT_GATT_CHRC_NOTIFY,
                          BT_GATT_PERM_READ | BT_GATT_PERM_WRITE,
                          read_schedule, write_schedule, schedule_value),
    BT_GATT_CCC(schedule_ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
    
    /* System Configuration Characteristic */
    BT_GATT_CHARACTERISTIC(&system_config_uuid.uuid,
                          BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE | BT_GATT_CHRC_NOTIFY,
                          BT_GATT_PERM_READ | BT_GATT_PERM_WRITE,
                          read_system_config, write_system_config, system_config_value),
    BT_GATT_CCC(system_config_ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
    
    /* Task Queue Characteristic */
    BT_GATT_CHARACTERISTIC(&task_queue_uuid.uuid,
                          BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE | BT_GATT_CHRC_NOTIFY,
                          BT_GATT_PERM_READ | BT_GATT_PERM_WRITE,
                          read_task_queue, write_task_queue, task_queue_value),
    BT_GATT_CCC(task_queue_ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
    
    /* Statistics Characteristic */
    BT_GATT_CHARACTERISTIC(&statistics_uuid.uuid,
                          BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE | BT_GATT_CHRC_NOTIFY,
                          BT_GATT_PERM_READ | BT_GATT_PERM_WRITE,
                          read_statistics, write_statistics, statistics_value),
    BT_GATT_CCC(statistics_ccc_cfg_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
    
    /* RTC Characteristic */
    BT_GATT_CHARACTERISTIC(&rtc_char_uuid.uuid,
                          BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE | BT_GATT_CHRC_NOTIFY,
                          BT_GATT_PERM_READ | BT_GATT_PERM_WRITE,
                          read_rtc, write_rtc, rtc_value),
    BT_GATT_CCC(rtc_ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
    
    /* Alarm Characteristic */
    BT_GATT_CHARACTERISTIC(&alarm_char_uuid.uuid,
                          BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE | BT_GATT_CHRC_NOTIFY,
                          BT_GATT_PERM_READ | BT_GATT_PERM_WRITE,
                          read_alarm, write_alarm, alarm_value),
    BT_GATT_CCC(alarm_ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
    
    /* Calibration Characteristic */
    BT_GATT_CHARACTERISTIC(&calibration_char_uuid.uuid,
                          BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE | BT_GATT_CHRC_NOTIFY,
                          BT_GATT_PERM_READ | BT_GATT_PERM_WRITE,
                          read_calibration, write_calibration, calibration_value),
    BT_GATT_CCC(calibration_ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
    
    /* History Characteristic */
    BT_GATT_CHARACTERISTIC(&history_char_uuid.uuid,
                          BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE | BT_GATT_CHRC_NOTIFY,
                          BT_GATT_PERM_READ | BT_GATT_PERM_WRITE,
                          read_history, write_history, history_value),
    BT_GATT_CCC(history_ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
    
    /* Diagnostics Characteristic */
    BT_GATT_CHARACTERISTIC(&diagnostics_char_uuid.uuid,
                          BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
                          BT_GATT_PERM_READ,
                          read_diagnostics, NULL, diagnostics_value),
    BT_GATT_CCC(diagnostics_ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
    
    /* Growing Environment Characteristic */
    BT_GATT_CHARACTERISTIC(&growing_env_char_uuid.uuid,
                          BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE | BT_GATT_CHRC_NOTIFY,
                          BT_GATT_PERM_READ | BT_GATT_PERM_WRITE,
                          read_growing_env, write_growing_env, growing_env_value),
    BT_GATT_CCC(growing_env_ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
    
    /* Current Task Characteristic */
    BT_GATT_CHARACTERISTIC(&current_task_char_uuid.uuid,
                          BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE | BT_GATT_CHRC_NOTIFY,
                          BT_GATT_PERM_READ | BT_GATT_PERM_WRITE,
                          read_current_task, write_current_task, current_task_value),
    BT_GATT_CCC(current_task_ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
);

/**
 * @brief Start the background task update thread
 * 
 * @return 0 on success, negative error code on failure
 */
static int start_task_update_thread(void)
{
    k_thread_create(&task_update_thread,
                    task_update_thread_stack,
                    K_THREAD_STACK_SIZEOF(task_update_thread_stack),
                    task_update_thread_entry,
                    NULL, NULL, NULL,
                    K_PRIO_COOP(7), // Cooperative priority
                    0, K_NO_WAIT);

    k_thread_name_set(&task_update_thread, "bt_task_update");

    return 0;
}

/* ------------------------------------------------------------------ */
/* BLE Notification Debugging and Status Functions                   */
/* ------------------------------------------------------------------ */

void bt_irrigation_debug_notification_status(void) {
    printk("=== BLE Notification Status ===\n");
    printk("System enabled: %s\n", notification_system_enabled ? "YES" : "NO");
    printk("Connection: %s\n", default_conn ? "YES" : "NO");
    printk("Valve: %s\n", notification_state.valve_notifications_enabled ? "ON" : "OFF");
    printk("Flow: %s\n", notification_state.flow_notifications_enabled ? "ON" : "OFF");
    printk("Status: %s\n", notification_state.status_notifications_enabled ? "ON" : "OFF");
    printk("Alarm: %s\n", notification_state.alarm_notifications_enabled ? "ON" : "OFF");
    printk("Channel Config: %s\n", notification_state.channel_config_notifications_enabled ? "ON" : "OFF");
    printk("Schedule: %s\n", notification_state.schedule_notifications_enabled ? "ON" : "OFF");
    printk("Task Queue: %s\n", notification_state.task_queue_notifications_enabled ? "ON" : "OFF");
    printk("Current Task: %s\n", notification_state.current_task_notifications_enabled ? "ON" : "OFF");
    printk("Last notification: %d ms ago\n", k_uptime_get_32() - last_notification_time);
    printk("===============================\n");
}

/* ------------------------------------------------------------------ */
/* BLE Notification Functions - Public API                           */
/* ------------------------------------------------------------------ */

int bt_irrigation_valve_status_update(uint8_t channel_id, bool state) {
    if (channel_id >= WATERING_CHANNELS_COUNT) {
        printk("BLE: Invalid channel_id %d for valve status update\n", channel_id);
        return -EINVAL;
    }
    
    struct valve_control_data *valve = (struct valve_control_data *)valve_value;
    valve->channel_id = channel_id;
    valve->task_type = state ? 1 : 0; // 1 for on, 0 for off
    valve->value = state ? 1 : 0;
    
    printk("BLE: Valve status update - Channel %d: %s (notifications: %s)\n", 
           channel_id, state ? "ON" : "OFF",
           notification_state.valve_notifications_enabled ? "enabled" : "disabled");
    
    if (default_conn && notification_state.valve_notifications_enabled) {
        int ret = send_simple_notification(default_conn, &irrigation_svc.attrs[ATTR_IDX_VALVE_VALUE],
                                          valve_value, sizeof(struct valve_control_data));
        if (ret != 0) {
            printk("BLE: Valve notification failed: %d\n", ret);
        } else {
            printk("BLE: Valve notification sent successfully\n");
        }
        return ret;
    } else {
        if (!default_conn) {
            printk("BLE: No connection for valve notification\n");
        } else {
            printk("BLE: Valve notifications disabled\n");
        }
    }
    return 0;
}

int bt_irrigation_flow_update(uint32_t flow_rate_or_pulses) {
    memcpy(flow_value, &flow_rate_or_pulses, sizeof(uint32_t));
    
    if (default_conn) {
        return send_simple_notification(default_conn, &irrigation_svc.attrs[ATTR_IDX_FLOW_VALUE],
                                       flow_value, sizeof(uint32_t));
    }
    return 0;
}

int bt_irrigation_system_status_update(watering_status_t status) {
    status_value[0] = (uint8_t)status;
    
    printk("BLE: System status update - Status: %d (notifications: %s)\n",
           status, notification_state.status_notifications_enabled ? "enabled" : "disabled");
    
    if (default_conn && notification_state.status_notifications_enabled) {
        int ret = send_simple_notification(default_conn, &irrigation_svc.attrs[ATTR_IDX_STATUS_VALUE],
                                          status_value, sizeof(status_value));
        if (ret != 0) {
            printk("BLE: Status notification failed: %d\n", ret);
        } else {
            printk("BLE: Status notification sent successfully\n");
        }
        return ret;
    } else {
        if (!default_conn) {
            printk("BLE: No connection for status notification\n");
        } else {
            printk("BLE: Status notifications disabled\n");
        }
    }
    return 0;
}

int bt_irrigation_channel_config_update(uint8_t channel_id) {
    if (channel_id >= WATERING_CHANNELS_COUNT) {
        return -EINVAL;
    }
    
    /* Use a separate LOCAL buffer for notifications to avoid conflicts with read operations */
    /* CRITICAL: Remove 'static' keyword to ensure this is a stack-allocated local buffer */
    struct channel_config_data notif_config;
    
    watering_channel_t *channel;
    watering_error_t err = watering_get_channel(channel_id, &channel);
    if (err != WATERING_SUCCESS) {
        printk("Channel config notification failed: cannot get channel %d\n", channel_id);
        return -EIO;
    }
    
    /* Prepare notification data */
    memset(&notif_config, 0, sizeof(notif_config));
    notif_config.channel_id = channel_id;
    notif_config.auto_enabled = channel->watering_event.auto_enabled ? 1 : 0;
    
    /* Copy name with proper bounds checking */
    size_t name_len = strnlen(channel->name, sizeof(channel->name));
    if (name_len >= sizeof(notif_config.name)) {
        name_len = sizeof(notif_config.name) - 1;
    }
    memcpy(notif_config.name, channel->name, name_len);
    notif_config.name[name_len] = '\0';
    notif_config.name_len = name_len;
    
    /* Set plant and growing environment fields */
    notif_config.plant_type = (uint8_t)channel->plant_type;
    notif_config.soil_type = (uint8_t)channel->soil_type;
    notif_config.irrigation_method = (uint8_t)channel->irrigation_method;
    notif_config.coverage_type = channel->coverage.use_area ? 0 : 1;
    if (channel->coverage.use_area) {
        notif_config.coverage.area_m2 = channel->coverage.area.area_m2;
    } else {
        notif_config.coverage.plant_count = channel->coverage.plants.count;
    }
    notif_config.sun_percentage = channel->sun_percentage;
    
    if (default_conn) {
        return send_simple_notification(default_conn, &irrigation_svc.attrs[ATTR_IDX_CHANNEL_CFG_VALUE],
                                       &notif_config, sizeof(notif_config));
    }
    return 0;
}

int bt_irrigation_queue_status_update(uint8_t count) {
    struct task_queue_data *queue = (struct task_queue_data *)task_queue_value;
    queue->pending_count = count;
    queue->completed_tasks = 0; // Note: Completed tasks counter not available in current watering system
    
    if (default_conn) {
        return send_simple_notification(default_conn, &irrigation_svc.attrs[ATTR_IDX_TASK_QUEUE_VALUE],
                                       task_queue_value, sizeof(struct task_queue_data));
    }
    return 0;
}

int bt_irrigation_current_task_update(uint8_t channel_id, uint32_t start_time, 
                                     uint8_t watering_mode, uint32_t target_value,
                                     uint32_t current_value, uint32_t total_volume_ml) {
    struct current_task_data *task = (struct current_task_data *)current_task_value;
    task->channel_id = channel_id;
    task->start_time = start_time;
    task->mode = watering_mode;
    task->target_value = target_value;
    task->current_value = current_value;
    task->total_volume = total_volume_ml;
    task->status = (channel_id == 0xFF) ? 0 : 1; // 0=idle, 1=running
    task->reserved = 0;
    
    if (default_conn) {
        return send_simple_notification(default_conn, &irrigation_svc.attrs[ATTR_IDX_CURRENT_TASK_VALUE],
                                       current_task_value, sizeof(struct current_task_data));
    }
    return 0;
}

int bt_irrigation_schedule_update(uint8_t channel_id) {
    if (channel_id >= WATERING_CHANNELS_COUNT) {
        return -EINVAL;
    }
    
    // Get current schedule configuration from watering system
    struct schedule_config_data *schedule = (struct schedule_config_data *)schedule_value;
    schedule->channel_id = channel_id;
    
    // Get schedule info from channel configuration
    watering_channel_t *channel;
    if (watering_get_channel(channel_id, &channel) == WATERING_SUCCESS) {
        schedule->schedule_type = (uint8_t)channel->watering_event.schedule_type;
        if (channel->watering_event.schedule_type == SCHEDULE_DAILY) {
            schedule->days_mask = channel->watering_event.schedule.daily.days_of_week;
        } else {
            schedule->days_mask = channel->watering_event.schedule.periodic.interval_days;
        }
        schedule->hour = channel->watering_event.start_time.hour;
        schedule->minute = channel->watering_event.start_time.minute;
        schedule->watering_mode = (uint8_t)channel->watering_event.watering_mode;
        
        if (channel->watering_event.watering_mode == WATERING_BY_DURATION) {
            schedule->value = channel->watering_event.watering.by_duration.duration_minutes;
        } else {
            schedule->value = channel->watering_event.watering.by_volume.volume_liters;
        }
        schedule->auto_enabled = channel->watering_event.auto_enabled ? 1 : 0;
    } else {
        // Set default values if channel can't be retrieved
        schedule->schedule_type = 0;
        schedule->days_mask = 0;
        schedule->hour = 0;
        schedule->minute = 0;
        schedule->watering_mode = 0;
        schedule->value = 0;
        schedule->auto_enabled = 0;
    }
    
    if (default_conn) {
        return send_simple_notification(default_conn, &irrigation_svc.attrs[ATTR_IDX_SCHEDULE_VALUE],
                                       schedule_value, sizeof(struct schedule_config_data));
    }
    return 0;
}

int bt_irrigation_config_update(void) {
    // Update system configuration data
    struct system_config_data *sys_config = (struct system_config_data *)system_config_value;
    
    // Set system configuration values
    sys_config->version = 1;
    sys_config->power_mode = 0; // Normal power mode
    sys_config->flow_calibration = 750; // Default pulses per liter
    sys_config->max_active_valves = 1; // Fixed to 1 as per system design
    sys_config->num_channels = WATERING_CHANNELS_COUNT;
    
    if (default_conn) {
        return send_simple_notification(default_conn, &irrigation_svc.attrs[ATTR_IDX_SYSTEM_CFG_VALUE],
                                       system_config_value, sizeof(struct system_config_data));
    }
    return 0;
}

int bt_irrigation_statistics_update(uint8_t channel_id) {
    if (channel_id >= WATERING_CHANNELS_COUNT) {
        return -EINVAL;
    }
    
    // Update statistics data
    struct statistics_data *stats = (struct statistics_data *)statistics_value;
    stats->channel_id = channel_id;
    
    // Note: Current watering system doesn't maintain centralized statistics
    // Statistics would need to be aggregated from the history system
    stats->total_volume = 0;
    stats->last_volume = 0;
    stats->last_watering = 0;
    stats->count = 0;
    
    if (default_conn) {
        return send_simple_notification(default_conn, &irrigation_svc.attrs[ATTR_IDX_STATISTICS_VALUE],
                                       statistics_value, sizeof(struct statistics_data));
    }
    return 0;
}

int bt_irrigation_update_statistics_from_flow(uint8_t channel_id, uint32_t volume_ml) {
    if (channel_id >= WATERING_CHANNELS_COUNT) {
        return -EINVAL;
    }
    
    // Note: Statistics update from flow is not implemented in current watering system
    // Update BLE statistics notification
    return bt_irrigation_statistics_update(channel_id);
}

int bt_irrigation_rtc_update(rtc_datetime_t *datetime) {
    if (!datetime) {
        return -EINVAL;
    }
    
    struct rtc_data *rtc = (struct rtc_data *)rtc_value;
    rtc->year = datetime->year;
    rtc->month = datetime->month;
    rtc->day = datetime->day;
    rtc->hour = datetime->hour;
    rtc->minute = datetime->minute;
    rtc->second = datetime->second;
    rtc->day_of_week = datetime->day_of_week;
    
    if (default_conn) {
        return send_simple_notification(default_conn, &irrigation_svc.attrs[ATTR_IDX_RTC_VALUE],
                                       rtc_value, sizeof(struct rtc_data));
    }
    return 0;
}

int bt_irrigation_alarm_notify(uint8_t alarm_code, uint16_t alarm_data) {
    struct alarm_data *alarm = (struct alarm_data *)alarm_value;
    alarm->alarm_code = alarm_code;
    alarm->alarm_data = alarm_data;
    alarm->timestamp = k_uptime_get_32() / 1000;
    
    printk("BLE: Alarm notification - Code: %d, Data: %d, Time: %d (notifications: %s)\n",
           alarm_code, alarm_data, alarm->timestamp,
           notification_state.alarm_notifications_enabled ? "enabled" : "disabled");
    
    if (default_conn && notification_state.alarm_notifications_enabled) {
        int ret = send_simple_notification(default_conn, &irrigation_svc.attrs[ATTR_IDX_ALARM_VALUE],
                                          alarm_value, sizeof(struct alarm_data));
        if (ret != 0) {
            printk("BLE: Alarm notification failed: %d\n", ret);
        } else {
            printk("BLE: Alarm notification sent successfully\n");
        }
        return ret;
    } else {
        if (!default_conn) {
            printk("BLE: No connection for alarm notification\n");
        } else {
            printk("BLE: Alarm notifications disabled\n");
        }
    }
    return 0;
}

int bt_irrigation_alarm_clear(uint8_t alarm_code) {
    // Send alarm clear notification
    struct alarm_data *alarm = (struct alarm_data *)alarm_value;
    alarm->alarm_code = alarm_code;
    alarm->alarm_data = 0; // Clear data
    alarm->timestamp = k_uptime_get_32() / 1000;
    
    if (default_conn) {
        return send_simple_notification(default_conn, &irrigation_svc.attrs[ATTR_IDX_ALARM_VALUE],
                                       alarm_value, sizeof(struct alarm_data));
    }
    return 0;
}

int bt_irrigation_start_flow_calibration(uint8_t start, uint32_t volume_ml) {
    struct calibration_data *cal = (struct calibration_data *)calibration_value;
    
    if (start == 1) {
        // Start calibration
        cal->action = 1; // Start
        cal->pulses = 0;
        cal->volume_ml = 0;
        cal->pulses_per_liter = 0;
        
        // Reset pulse count to start calibration
        reset_pulse_count();
        printk("Flow sensor calibration started\n");
    } else {
        // Stop calibration and calculate
        cal->action = 0; // Stop
        cal->volume_ml = volume_ml;
        
        // Get pulse count from flow sensor
        uint32_t pulse_count = get_pulse_count();
        cal->pulses = pulse_count;
        
        // Calculate pulses per liter
        if (volume_ml > 0) {
            cal->pulses_per_liter = (pulse_count * 1000) / volume_ml;
            cal->action = 3; // Calculated
            
            // Update system configuration with new calibration
            watering_set_flow_calibration(cal->pulses_per_liter);
            
            printk("Flow calibration completed: %d pulses/liter\n", cal->pulses_per_liter);
        }
    }
    
    if (default_conn) {
        return send_simple_notification(default_conn, &irrigation_svc.attrs[ATTR_IDX_CALIB_VALUE],
                                       calibration_value, sizeof(struct calibration_data));
    }
    return 0;
}

int bt_irrigation_history_update(uint8_t channel_id, uint8_t entry_index) {
    if (channel_id >= WATERING_CHANNELS_COUNT) {
        return -EINVAL;
    }
    
    struct history_data *hist = (struct history_data *)history_value;
    hist->channel_id = channel_id;
    hist->history_type = 0; // Detailed history
    hist->entry_index = entry_index;
    hist->count = 1;
    hist->start_timestamp = 0;
    hist->end_timestamp = 0;
    
    // Note: History retrieval not fully implemented in current watering system
    // Would require enhancement to watering_history API
    
    if (default_conn) {
        return send_simple_notification(default_conn, &irrigation_svc.attrs[ATTR_IDX_HISTORY_VALUE],
                                       history_value, sizeof(struct history_data));
    }
    return 0;
}

int bt_irrigation_history_notify_event(uint8_t channel_id, uint8_t event_type, uint32_t timestamp, uint32_t value) {
    if (channel_id >= WATERING_CHANNELS_COUNT) {
        return -EINVAL;
    }
    
    struct history_data *hist = (struct history_data *)history_value;
    hist->channel_id = channel_id;
    hist->history_type = 0; // Detailed history
    hist->entry_index = 0; // Most recent
    hist->count = 1;
    hist->start_timestamp = timestamp;
    hist->end_timestamp = timestamp;
    
    // Note: History recording is handled by watering system internally
    // The watering_history_add_event function requires a different signature
    
    if (default_conn) {
        return send_simple_notification(default_conn, &irrigation_svc.attrs[ATTR_IDX_HISTORY_VALUE],
                                       history_value, sizeof(struct history_data));
    }
    return 0;
}

int bt_irrigation_history_get_detailed(uint8_t channel_id, uint32_t start_timestamp, 
                                      uint32_t end_timestamp, uint8_t entry_index) {
    struct history_data *hist = (struct history_data *)history_value;
    hist->channel_id = channel_id;
    hist->history_type = 0; // Detailed history
    hist->entry_index = entry_index;
    hist->count = 1;
    hist->start_timestamp = start_timestamp;
    hist->end_timestamp = end_timestamp;
    
    // Note: Detailed history retrieval not fully implemented in current system
    
    if (default_conn) {
        return send_simple_notification(default_conn, &irrigation_svc.attrs[ATTR_IDX_HISTORY_VALUE],
                                       history_value, sizeof(struct history_data));
    }
    return 0;
}

int bt_irrigation_history_get_daily(uint8_t channel_id, uint8_t entry_index) {
    struct history_data *hist = (struct history_data *)history_value;
    hist->channel_id = channel_id;
    hist->history_type = 1; // Daily history
    hist->entry_index = entry_index;
    hist->count = 1;
    hist->start_timestamp = 0;
    hist->end_timestamp = 0;
    
    // Note: Daily statistics retrieval not fully implemented in current system
    // The watering_history_get_daily_stats function requires different parameters
    
    if (default_conn) {
        return send_simple_notification(default_conn, &irrigation_svc.attrs[ATTR_IDX_HISTORY_VALUE],
                                       history_value, sizeof(struct history_data));
    }
    return 0;
}

int bt_irrigation_history_get_monthly(uint8_t channel_id, uint8_t entry_index) {
    struct history_data *hist = (struct history_data *)history_value;
    hist->channel_id = channel_id;
    hist->history_type = 2; // Monthly history
    hist->entry_index = entry_index;
    hist->count = 1;
    hist->start_timestamp = 0;
    hist->end_timestamp = 0;
    
    // Note: Monthly statistics retrieval not fully implemented in current system
    // The watering_history_get_monthly_stats function requires different parameters
    
    if (default_conn) {
        return send_simple_notification(default_conn, &irrigation_svc.attrs[ATTR_IDX_HISTORY_VALUE],
                                       history_value, sizeof(struct history_data));
    }
    return 0;
}

int bt_irrigation_history_get_annual(uint8_t channel_id, uint8_t entry_index) {
    struct history_data *hist = (struct history_data *)history_value;
    hist->channel_id = channel_id;
    hist->history_type = 3; // Annual history
    hist->entry_index = entry_index;
    hist->count = 1;
    hist->start_timestamp = 0;
    hist->end_timestamp = 0;
    
    // Note: Annual statistics retrieval not fully implemented in current system
    // The watering_history_get_annual_stats function requires different parameters
    
    if (default_conn) {
        return send_simple_notification(default_conn, &irrigation_svc.attrs[ATTR_IDX_HISTORY_VALUE],
                                       history_value, sizeof(struct history_data));
    }
    return 0;
}

/**
 * @brief Force send task queue notification (for important changes)
 * 
 * Sends an immediate task queue status notification, bypassing rate limiting.
 * 
 * @return 0 on success, negative error code on failure
 */
int bt_irrigation_queue_status_notify(void) {
    // Update task queue data
    struct task_queue_data *queue = (struct task_queue_data *)task_queue_value;
    
    // Get current queue status from watering system
    uint8_t pending_count;
    bool active;
    if (watering_get_queue_status(&pending_count, &active) == WATERING_SUCCESS) {
        queue->pending_count = pending_count;
        queue->completed_tasks = 0; // Note: completed tasks counter not available in current watering system
        queue->current_channel = active ? 0 : 0xFF; // Active channel detection requires watering system enhancement
        queue->current_task_type = 0;
        queue->current_value = 0;
        queue->active_task_id = 0;
    }
    
    if (default_conn) {
        return send_simple_notification(default_conn, &irrigation_svc.attrs[ATTR_IDX_TASK_QUEUE_VALUE],
                                       task_queue_value, sizeof(struct task_queue_data));
    }
    return 0;
}

/**
 * @brief Initialize the Bluetooth irrigation service
 * 
 * This function initializes the BLE service, sets up default values for all
 * characteristics, and starts background threads for periodic updates.
 * 
 * @return 0 on success, negative error code on failure
 */
int bt_irrigation_service_init(void)
{
    printk("Initializing Bluetooth irrigation service...\n");

    // Initialize all characteristic values to defaults
    memset(valve_value, 0, sizeof(valve_value));
    memset(flow_value, 0, sizeof(flow_value));
    memset(status_value, 0, sizeof(status_value));
    memset(channel_config_value, 0, sizeof(channel_config_value));
    memset(schedule_value, 0, sizeof(schedule_value));
    memset(system_config_value, 0, sizeof(system_config_value));
    memset(task_queue_value, 0, sizeof(task_queue_value));
    memset(statistics_value, 0, sizeof(statistics_value));
    memset(rtc_value, 0, sizeof(rtc_value));
    memset(alarm_value, 0, sizeof(alarm_value));
    memset(calibration_value, 0, sizeof(calibration_value));
    memset(history_value, 0, sizeof(history_value));
    memset(diagnostics_value, 0, sizeof(diagnostics_value));
    memset(growing_env_value, 0, sizeof(growing_env_value));
    memset(current_task_value, 0, sizeof(current_task_value));

    // Initialize system configuration with defaults
    struct system_config_data *sys_config = (struct system_config_data *)system_config_value;
    sys_config->version = 1;
    sys_config->power_mode = 0; // Normal power mode
    sys_config->flow_calibration = 750; // Default pulses per liter (per documentation)
    sys_config->max_active_valves = 1;
    sys_config->num_channels = WATERING_CHANNELS_COUNT;

    // Initialize task queue status
    struct task_queue_data *queue = (struct task_queue_data *)task_queue_value;
    queue->pending_count = 0;
    queue->completed_tasks = 0;
    queue->current_channel = 0xFF; // No active channel
    queue->current_task_type = 0;
    queue->current_value = 0;
    queue->command = 0;
    queue->task_id_to_delete = 0;

    // Initialize simple notification system
    notification_system_enabled = true;
    last_notification_time = 0;
    
    printk("BLE simple notification system initialized\n");

    // Enable Bluetooth
    printk("Enabling Bluetooth...\n");
    int err = bt_enable(NULL);
    if (err) {
        printk("Bluetooth init failed (err %d)\n", err);
        return err;
    }
    printk("Bluetooth enabled successfully\n");

    // Load settings if available
    printk("Loading Bluetooth settings...\n");
    if (IS_ENABLED(CONFIG_SETTINGS)) {
        settings_load();
    }

    // Register connection callbacks
    bt_conn_cb_register(&conn_callbacks);
    
    // Start advertising
    printk("Starting Bluetooth advertising...\n");
    err = bt_le_adv_start(&adv_param, adv_ad, ARRAY_SIZE(adv_ad),
                          adv_sd, ARRAY_SIZE(adv_sd));
    if (err) {
        printk("Advertising failed to start (err %d)\n", err);
        return err;
    }
    printk("Advertising started successfully\n");
    
    // Start background task update thread
    int ret = start_task_update_thread();
    if (ret != 0) {
        printk("Failed to start task update thread: %d\n", ret);
        return ret;
    }

    printk("Bluetooth irrigation service initialized successfully\n");
    return 0;
}

int bt_irrigation_growing_env_update(uint8_t channel_id) {
    if (channel_id >= WATERING_CHANNELS_COUNT) {
        return -EINVAL;
    }
    
    // Update growing environment data
    struct growing_env_data *env = (struct growing_env_data *)growing_env_value;
    env->channel_id = channel_id;
    
    // Get growing environment configuration from watering system
    watering_channel_t *channel;
    if (watering_get_channel(channel_id, &channel) == WATERING_SUCCESS) {
        env->plant_type = (uint8_t)channel->plant_type;
        env->soil_type = (uint8_t)channel->soil_type;
        env->irrigation_method = (uint8_t)channel->irrigation_method;
        env->use_area_based = channel->coverage.use_area ? 1 : 0;
        
        if (channel->coverage.use_area) {
            env->coverage.area_m2 = channel->coverage.area.area_m2;
        } else {
            env->coverage.plant_count = channel->coverage.plants.count;
        }
        
        env->sun_percentage = channel->sun_percentage;
        
        /* Include custom plant fields if plant_type is CUSTOM (7) */
        if (channel->plant_type == 7) {
            strncpy(env->custom_name, channel->custom_plant.custom_name, 
                    sizeof(env->custom_name) - 1);
            env->custom_name[sizeof(env->custom_name) - 1] = '\0';
            env->water_need_factor = channel->custom_plant.water_need_factor;
            env->irrigation_freq_days = channel->custom_plant.irrigation_freq;
            env->prefer_area_based = channel->custom_plant.prefer_area_based ? 1 : 0;
        } else {
            /* Clear custom fields for non-custom plants */
            memset(env->custom_name, 0, sizeof(env->custom_name));
            env->water_need_factor = 1.0f;
            env->irrigation_freq_days = 1;
            env->prefer_area_based = 0;
        }
    } else {
        // Set default values
        env->plant_type = 0;
        env->soil_type = 0;
        env->irrigation_method = 0;
        env->use_area_based = 0;
        env->coverage.area_m2 = 0.0f;
        env->coverage.plant_count = 0;
        env->sun_percentage = 50; // Default 50% sun
        memset(env->custom_name, 0, sizeof(env->custom_name));
        env->water_need_factor = 1.0f;
        env->irrigation_freq_days = 1;
        env->prefer_area_based = 0;
    }
    
    if (default_conn) {
        return send_simple_notification(default_conn, &irrigation_svc.attrs[ATTR_IDX_GROWING_ENV_VALUE],
                                       growing_env_value, sizeof(struct growing_env_data));
    }
    return 0;
}

int bt_irrigation_direct_command(uint8_t channel_id, uint8_t command, uint16_t param) {
    if (channel_id >= WATERING_CHANNELS_COUNT) {
        return -EINVAL;
    }
    
    int result = 0;
    
    switch (command) {
        case 0: // Close valve
            result = watering_stop_current_task() ? WATERING_SUCCESS : WATERING_ERROR_BUSY;
            if (result == WATERING_SUCCESS) {
                bt_irrigation_valve_status_update(channel_id, false);
            }
            break;
            
        case 1: // Open valve - create manual watering task
            // Note: Direct valve control not supported in current system
            // Use task queue instead
            result = WATERING_ERROR_BUSY;
            break;
            
        case 2: // Pulse valve (param = duration in seconds)
            // Note: Direct valve control not supported in current system
            // Use task queue instead
            result = WATERING_ERROR_BUSY;
            break;
            
        default:
            return -EINVAL;
    }
    
    if (result != WATERING_SUCCESS) {
        return -EIO;
    }
    
    return 0;
}

int bt_irrigation_record_error(uint8_t channel_id, uint8_t error_code) {
    if (channel_id >= WATERING_CHANNELS_COUNT) {
        return -EINVAL;
    }
    
    // Record error in watering history system
    uint32_t timestamp = k_uptime_get_32() / 1000; // Convert to seconds
    watering_history_record_task_error(channel_id, error_code);
    
    // Send alarm notification
    bt_irrigation_alarm_notify(error_code, channel_id);
    
    // Update history notification
    bt_irrigation_history_notify_event(channel_id, 3, timestamp, error_code); // 3 = error event
    
    return 0;
}

int bt_irrigation_update_history_aggregations(void) {
    // Note: History aggregation functions not fully implemented in current watering system
    // This is a placeholder implementation for future enhancement
    
    // Update daily, monthly, and annual aggregations for all channels
    for (uint8_t channel_id = 0; channel_id < WATERING_CHANNELS_COUNT; channel_id++) {
        // Notify about daily stats update
        bt_irrigation_history_get_daily(channel_id, 0);
        
        // Notify about monthly stats update
        bt_irrigation_history_get_monthly(channel_id, 0);
        
        // Notify about annual stats update
        bt_irrigation_history_get_annual(channel_id, 0);
    }
    
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
    return 0;
}

int bt_irrigation_flow_update(uint32_t flow_rate) {
    return 0;
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

/* bt_irrigation_channel_config_update is implemented above - removing duplicate stub */

int bt_irrigation_current_task_update(uint8_t channel_id, uint32_t start_time,
                                    uint8_t mode, uint32_t target_value,
                                    uint32_t current_value, uint32_t total_volume) {
    return 0;
}

int bt_irrigation_schedule_update(uint8_t channel_id) {
    return 0;
}

int bt_irrigation_config_update(void) {
    return 0;
}

int bt_irrigation_statistics_update(uint8_t channel_id) {
    return 0;
}

int bt_irrigation_start_flow_calibration(uint8_t start, uint32_t volume_ml) {
    return 0;
}

int bt_irrigation_history_update(uint8_t channel_id, uint8_t entry_index) {
    return 0;
}

int bt_irrigation_history_get_detailed(uint8_t channel_id, uint32_t start_timestamp,
                                     uint32_t end_timestamp, uint8_t entry_index) {
    return 0;
}

int bt_irrigation_history_get_daily(uint8_t channel_id, uint8_t entry_index) {
    return 0;
}

int bt_irrigation_history_get_monthly(uint8_t channel_id, uint8_t entry_index) {
    return 0;
}

int bt_irrigation_history_get_annual(uint8_t channel_id, uint8_t entry_index) {
    return 0;
}

int bt_irrigation_history_notify_event(uint8_t channel_id, uint8_t event_type,
                                      uint32_t timestamp, uint32_t value) {
    return 0;
}

int bt_irrigation_growing_env_update(uint8_t channel_id) {
    return 0;
}

int bt_irrigation_direct_command(uint8_t channel_id, uint8_t command, uint16_t param) {
    return 0;
}

int bt_irrigation_record_error(uint8_t channel_id, uint8_t error_code) {
    return 0;
}

int bt_irrigation_update_history_aggregations(void) {
    return 0;
}

#endif /* CONFIG_BT */
