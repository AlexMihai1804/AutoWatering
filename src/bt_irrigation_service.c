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

/* ------------------------------------------------------------------ */
/* 1. Correct type & provide forward declaration of the service       */
/* ------------------------------------------------------------------ */
extern const struct bt_gatt_service_static irrigation_svc;

/* ------------------------------------------------------------------ */
/* 2. Macro used by many *_ccc_changed() handlers – restore           */
/* ------------------------------------------------------------------ */
#ifndef SEND_NOTIF
#define SEND_NOTIF(value_ptr, size) \
    bt_gatt_notify(default_conn, \
                   &irrigation_svc.attrs[ATTR_IDX_VALVE_VALUE], \
                   (value_ptr), (size))
#endif
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* NEW: forward declarations needed before first use                  */
/* ------------------------------------------------------------------ */
/* Remove the conflicting irrigation_svc declaration - BT_GATT_SERVICE_DEFINE handles this */

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

/* Task creation data structure */
struct valve_control_data {
    uint8_t channel_id;
    uint8_t task_type; // 0=duration, 1=volume
    uint16_t value; // minutes or liters
}
        __packed;

/* Channel configuration structure */
struct channel_config_data {
    uint8_t channel_id;
    uint8_t name_len;
    char    name[64];      /* ← was 16, now full 64-byte buffer */
    uint8_t auto_enabled;
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
}
        __packed;

/* System configuration structure */
struct system_config_data {
    uint8_t version;           /* Configuration version - NOT IN DOCS */
    uint8_t power_mode;
    uint32_t flow_calibration; /* Pulses per liter */
    uint8_t max_active_valves;
    uint8_t num_channels;      /* Number of channels - NOT IN DOCS */
}
        __packed;

/* Task queue structure */
struct task_queue_data {
    uint8_t pending_count;       /* Should be 'pending_tasks' per docs */
    uint8_t completed_tasks;
    uint8_t current_channel;     /* 0xFF if none              */
    uint8_t current_task_type;   /* 0=duration, 1=volume      */
    uint16_t current_value;      /* minutes or liters         */
    uint8_t command;             /* 0=none, 1=cancel current,
                                    2=clear queue, 3=delete id,
                                    4=clear ERRORS  ← NEW      */
    uint8_t task_id_to_delete;   /* for future use            */
    uint8_t active_task_id;      /* Active task ID - NOT IN DOCS */
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

/* Structure for irrigation history */
struct history_data {
    uint8_t channel_id; /* Channel (0-7) */
    uint8_t entry_index; /* Entry index (0=most recent) */
    uint32_t timestamp; /* When it occurred */
    uint8_t mode; /* 0=duration, 1=volume */
    uint16_t duration; /* Duration in seconds or volume in ml */
    uint8_t success; /* 1=success, 0=failed */
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

/* Bluetooth connection callback */
static struct bt_conn *default_conn;

static void connected(struct bt_conn *conn, uint8_t err) {
    if (err) {
        printk("Connection failed (err %u)\n", err);
        return;
    }

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

    printk("Connected to irrigation controller\n");
}

static void disconnected(struct bt_conn *conn, uint8_t reason) {
    printk("Disconnected (reason %u)\n", reason);

    if (default_conn) {
        bt_conn_unref(default_conn);
        default_conn = NULL;
    }

    /* restart advertising so a new central can connect */
    int err = bt_le_adv_start(&adv_param,
                              adv_ad, ARRAY_SIZE(adv_ad),
                              adv_sd, ARRAY_SIZE(adv_sd));
    if (err && err != -EALREADY) {
        printk("Failed to restart advertising (err %d)\n", err);
    } else {
        printk("Advertising restarted\n");
    }
}

/* Valve characteristic read callback */
static ssize_t read_valve(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                          void *buf, uint16_t len, uint16_t offset) {
    const struct valve_control_data *value = attr->user_data;

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
            printk("Duration task added via Bluetooth: channel %d, %d minutes\n",
                   channel_id + 1, task_value);
        }
    } else if (task_type == 1) {
        // Volume (liters)
        err = watering_add_volume_task(channel_id, task_value);
        if (err == WATERING_SUCCESS) {
            printk("Volume task added via Bluetooth: channel %d, %d liters\n",
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
    printk("Valve notifications %s\n", notif_enabled ? "enabled" : "disabled");
    SEND_NOTIF(valve_value, sizeof(struct valve_control_data));
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
    printk("Flow notifications %s\n", notif_enabled ? "enabled" : "disabled");
    SEND_NOTIF(flow_value, sizeof(uint32_t));
}

/* Status characteristic read callback */
static ssize_t read_status(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                           void *buf, uint16_t len, uint16_t offset) {
    const uint8_t *value = attr->user_data;

    return bt_gatt_attr_read(conn, attr, buf, len, offset, value, sizeof(uint8_t));
}

/* CCC configuration change callback for status characteristic */
static void status_ccc_cfg_changed(const struct bt_gatt_attr *attr, uint16_t value) {
    bool notif_enabled = (value == BT_GATT_CCC_NOTIFY);
    printk("Status notifications %s\n", notif_enabled ? "enabled" : "disabled");
    SEND_NOTIF(status_value, sizeof(status_value));
}

/* ------------------------------------------------------------------
 * NEW: Channel-Config CCC callback (was only prototyped – now coded)
 * ----------------------------------------------------------------*/
static void channel_config_ccc_changed(const struct bt_gatt_attr *attr,
                                       uint16_t value)
{
    bool notif_enabled = (value == BT_GATT_CCC_NOTIFY);
    printk("Channel Config notifications %s\n",
           notif_enabled ? "enabled" : "disabled");
    SEND_NOTIF(channel_config_value, sizeof(channel_config_value));
}

/* Channel Config characteristic read callback */
static ssize_t read_channel_config(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                   void *buf, uint16_t len, uint16_t offset) {
    struct channel_config_data *value =
            (struct channel_config_data *) channel_config_value;

    if (value->channel_id >= WATERING_CHANNELS_COUNT) {
        value->channel_id = 0;
    }

    watering_channel_t *channel;
    watering_get_channel(value->channel_id, &channel);

    /* copy fresh data */
    size_t name_len = strnlen(channel->name, sizeof(channel->name));
    if (name_len >= sizeof(value->name)) {
        name_len = sizeof(value->name) - 1;
    }
    memcpy(value->name, channel->name, name_len);
    value->name[name_len] = '\0';

    value->name_len = name_len;
    value->auto_enabled = channel->watering_event.auto_enabled ? 1 : 0;

    return bt_gatt_attr_read(conn, attr, buf, len, offset,
                             value, sizeof(*value));
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
		memcpy(&value->channel_id, buf, 1);
		if (value->channel_id >= WATERING_CHANNELS_COUNT)
			return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
		/* refresh local cache so a subsequent READ is coherent */
		watering_channel_t *ch;
		if (watering_get_channel(value->channel_id, &ch) == WATERING_SUCCESS) {
			size_t n = strnlen(ch->name, sizeof(ch->name));
			if (n >= sizeof(value->name)) n = sizeof(value->name) - 1;
			memcpy(value->name, ch->name, n);
			value->name[n]  = '\0';
			value->name_len = n;
			value->auto_enabled = ch->watering_event.auto_enabled ? 1 : 0;
		}
		return len;        /* ACK */
	}

	/* —— FRAGMENTED short-write (header + piece of name) -------- */
	if (!(flags & BT_GATT_WRITE_FLAG_PREPARE) && offset == 0 && len >= 2) {
		const uint8_t *p = buf;
		uint8_t cid      = p[0];
		uint8_t total    = p[1];
		const uint8_t *payload = p + 2;
		uint8_t pay_len  = len - 2;

		/* sanity checks */
		if (cid >= WATERING_CHANNELS_COUNT || total > 64 ||
		    pay_len > total ||                       /* impossible */
		    pay_len == 0) {
			return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
		}

		/* first fragment → reset accumulator */
		if (!name_frag.in_progress) {
			name_frag.id        = cid;
			name_frag.expected  = total;
			name_frag.received  = 0;
			name_frag.in_progress = true;
		}

		/* fragments must belong to the same channel */
		if (cid != name_frag.id || name_frag.received + pay_len > 64) {
			name_frag.in_progress = false;                /* abort */
			return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
		}

		/* append slice */
		memcpy(name_frag.buf + name_frag.received, payload, pay_len);
		name_frag.received += pay_len;

		/* done? -> commit */
		if (name_frag.received == name_frag.expected) {
			name_frag.buf[name_frag.expected] = '\0';

			watering_channel_t *ch;
			if (watering_get_channel(cid, &ch) == WATERING_SUCCESS) {
				strncpy(ch->name, name_frag.buf,
				        sizeof(ch->name) - 1);
				ch->name[sizeof(ch->name) - 1] = '\0';
				watering_save_config();
				printk("Channel %d name set to \"%s\"\n",
				       cid, ch->name);
			}
			name_frag.in_progress = false;
		}
		return len;     /* ACK current fragment */
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
			return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
		}

		if (value->name_len > 0 && value->name_len < sizeof(value->name)) {
			value->name[value->name_len] = '\0';
			strncpy(ch->name, value->name, sizeof(ch->name) - 1);
			ch->name[sizeof(ch->name) - 1] = '\0';
		}
		ch->watering_event.auto_enabled = value->auto_enabled ? true : false;
		watering_save_config();
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
			value->name[value->name_len] = '\0';
			strncpy(ch->name, value->name, sizeof(ch->name) - 1);
			ch->name[sizeof(ch->name) - 1] = '\0';
		}
		/* auto-enable */
		ch->watering_event.auto_enabled = value->auto_enabled ? true : false;

		watering_save_config();
		return sizeof(struct channel_config_data); /* success */
	}

	/* any other write (regular full write ≤ MTU) -------------------- */
	if (offset + len > sizeof(*value))
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	memcpy(((uint8_t *)value) + offset, buf, len);
	/* if entire struct fits in one request commit immediately */
	if (offset + len == sizeof(*value)) {
		/* same commit block as EXECUTE */
		if (value->channel_id >= WATERING_CHANNELS_COUNT)
			return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);

		watering_channel_t *ch;
		if (watering_get_channel(value->channel_id, &ch) != WATERING_SUCCESS)
			return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);

		if (value->name_len > 0 && value->name_len < sizeof(value->name)) {
			value->name[value->name_len] = '\0';
			strncpy(ch->name, value->name, sizeof(ch->name) - 1);
			ch->name[sizeof(ch->name) - 1] = '\0';
		}
		ch->watering_event.auto_enabled = value->auto_enabled ? true : false;
		watering_save_config();
	}
    return len;
}

/* Comment out unused conn_callbacks to avoid warning
static struct bt_conn_cb conn_callbacks = {
    .connected = connected,
    .disconnected = disconnected,
};
*/

/* Restore the connection callbacks - they are needed! */
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

    return bt_gatt_notify(conn,
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

static void alarm_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value) {
    bool notif_enabled = (value == BT_GATT_CCC_NOTIFY);
    printk("Alarm notifications %s\n", notif_enabled ? "enabled" : "disabled");
    SEND_NOTIF(alarm_value, sizeof(struct alarm_data));
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
                watering_save_config();

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
    printk("Calibration notifications %s\n", notif_enabled ? "enabled" : "disabled");
    SEND_NOTIF(calibration_value, sizeof(struct calibration_data));
}

/* History implementation */
static ssize_t read_history(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                            void *buf, uint16_t len, uint16_t offset) {
    struct history_data *value = (struct history_data *) history_value;

    // Here we should have a real history implementation
    // For now, we use static values

    return bt_gatt_attr_read(conn, attr, buf, len, offset, value, sizeof(*value));
}

static ssize_t write_history(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                             const void *buf, uint16_t len, uint16_t offset, uint8_t flags) {
    struct history_data *value = (struct history_data *) attr->user_data;

    if (offset + len > sizeof(*value)) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }

    memcpy(((uint8_t *) value) + offset, buf, len);

    // Client is requesting a specific history entry
    if (value->channel_id < WATERING_CHANNELS_COUNT) {
        // In a real implementation, you should load actual data from history
        // Currently we simulate responses
        value->timestamp = k_uptime_get_32() - (value->entry_index * 3600 * 1000);
        value->mode = value->entry_index % 2; // Alternate between modes
        value->duration = 500 + (value->entry_index * 100); // Simulated values
        value->success = 1; // Assume all were successful

        printk("History request for channel %d, entry %d\n",
               value->channel_id, value->entry_index);
    }

    return len;
}

static void history_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value) {
    bool notif_enabled = (value == BT_GATT_CCC_NOTIFY);
    printk("History notifications %s\n", notif_enabled ? "enabled" : "disabled");
    SEND_NOTIF(history_value, sizeof(struct history_data));
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
    printk("Diagnostics notifications %s\n", notif_enabled ? "enabled" : "disabled");
    SEND_NOTIF(diagnostics_value, sizeof(struct diagnostics_data));
}

/* Define the complete GATT service */
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
                          BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
                          BT_GATT_PERM_READ,
                          read_alarm, NULL, alarm_value),
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
);

/* Update flow data via Bluetooth */
int bt_irrigation_flow_update(uint32_t pulses) {
    if (!default_conn) { return -ENOTCONN; }

    memcpy(flow_value, &pulses, sizeof(pulses));
    return bt_gatt_notify(default_conn,
                          &irrigation_svc.attrs[ATTR_IDX_FLOW_VALUE],
                          flow_value, sizeof(uint32_t));
}

/* Update system status via Bluetooth */
int bt_irrigation_system_status_update(watering_status_t status) {
    if (!default_conn) {
        return -ENOTCONN;
    }

    status_value[0] = (uint8_t) status;

    return bt_gatt_notify(default_conn, &irrigation_svc.attrs[ATTR_IDX_STATUS_VALUE],
                          status_value, sizeof(uint8_t));
}

/* Update channel configuration via Bluetooth */
int bt_irrigation_channel_config_update(uint8_t channel_id) {
    if (!default_conn) {
        return -ENOTCONN;
    }

    if (channel_id >= WATERING_CHANNELS_COUNT) {
        return -EINVAL;
    }

    struct channel_config_data *config = (struct channel_config_data *) channel_config_value;
    watering_channel_t *channel;
    watering_error_t err = watering_get_channel(channel_id, &channel);
    if (err != WATERING_SUCCESS) {
        return -EINVAL;
    }

    config->channel_id = channel_id;

    // Fix string truncation warning with proper size checking
    size_t name_len = strlen(channel->name);
    if (name_len >= sizeof(config->name)) {
        name_len = sizeof(config->name) - 1;
    }
    memcpy(config->name, channel->name, name_len);
    config->name[name_len] = '\0'; // Ensure null termination

    config->name_len = strlen(config->name);
    config->auto_enabled = channel->watering_event.auto_enabled ? 1 : 0;

    return bt_gatt_notify(default_conn, &irrigation_svc.attrs[ATTR_IDX_CHANNEL_CFG_VALUE],
                          config, sizeof(*config));
}

/* Update queue status via Bluetooth */
int bt_irrigation_queue_status_update(uint8_t count) {
    if (!default_conn) {
        return -ENOTCONN;
    }

    struct task_queue_data *queue = (struct task_queue_data *) task_queue_value;

    // Use the actual number of tasks if specified or get it from the system
    if (count == 0xFF) {
        queue->pending_count = watering_get_pending_tasks_count();
    } else {
        queue->pending_count = count;
    }

    // Other fields are filled in read_task_queue
    read_task_queue(NULL, &irrigation_svc.attrs[20], NULL, 0, 0);

    return bt_gatt_notify(default_conn, &irrigation_svc.attrs[20],
                          queue, sizeof(*queue));
}

/* Update system configuration via Bluetooth */
int bt_irrigation_config_update(void) {
    if (!default_conn) {
        return -ENOTCONN;
    }

    // Populate with current data
    read_system_config(NULL, &irrigation_svc.attrs[17], NULL, 0, 0);

    return bt_gatt_notify(default_conn, &irrigation_svc.attrs[17],
                          system_config_value, sizeof(struct system_config_data));
}

/* Update statistics via Bluetooth */
int bt_irrigation_statistics_update(uint8_t channel_id) {
    if (!default_conn) {
        return -ENOTCONN;
    }

    if (channel_id >= WATERING_CHANNELS_COUNT) {
        return -EINVAL;
    }

    struct statistics_data *stats = (struct statistics_data *) statistics_value;
    stats->channel_id = channel_id;

    // Populate with current data
    read_statistics(NULL, &irrigation_svc.attrs[23], NULL, 0, 0);

    return bt_gatt_notify(default_conn, &irrigation_svc.attrs[23],
                          stats, sizeof(*stats));
}

/**
 * @brief Update RTC time via Bluetooth
 */
int bt_irrigation_rtc_update(rtc_datetime_t *datetime) {
    if (!default_conn) {
        return -ENOTCONN;
    }

    struct rtc_data *value = (struct rtc_data *) rtc_value;

    value->year = datetime->year - 2000;
    value->month = datetime->month;
    value->day = datetime->day;
    value->hour = datetime->hour;
    value->minute = datetime->minute;
    value->second = datetime->second;
    value->day_of_week = datetime->day_of_week;

    // Find the RTC characteristic index in the service - adjust based on characteristic order
    int rtc_index = 26; // This index should be adjusted based on the characteristic order in BT_GATT_SERVICE_DEFINE

    return bt_gatt_notify(default_conn, &irrigation_svc.attrs[rtc_index], value, sizeof(*value));
}

/**
 * @brief Notify Bluetooth client about an alarm
 */
int bt_irrigation_alarm_notify(uint8_t alarm_code, uint16_t alarm_data) {
    if (!default_conn) {
        return -ENOTCONN;
    }

    struct alarm_data *value = (struct alarm_data *) alarm_value;

    value->alarm_code = alarm_code;
    value->alarm_data = alarm_data;
    value->timestamp = k_uptime_get_32();

    // Find the alarm characteristic index in service - adjust based on characteristic order
    int alarm_index = 29; // This index should be adjusted based on the characteristic order in BT_GATT_SERVICE_DEFINE

    return bt_gatt_notify(default_conn, &irrigation_svc.attrs[alarm_index], value, sizeof(*value));
}

/**
 * @brief Start flow sensor calibration session
 */
int bt_irrigation_start_flow_calibration(uint8_t start, uint32_t volume_ml) {
    if (!default_conn) {
        return -ENOTCONN;
    }

    struct calibration_data *value = (struct calibration_data *) calibration_value;

    if (start) {
        // Start calibration
        value->action = 1;
        value->volume_ml = 0; // Will be set when stopping
        value->pulses = 0;

        // Actual calibration happens when client writes to characteristic
    } else {
        // Stop and calculate calibration
        value->action = 0;
        value->volume_ml = volume_ml;

        // Actual calibration happens when client writes to characteristic
    }

    // Find the calibration characteristic index in service - adjust based on characteristic order
    int calibration_index = 32;
    // This index should be adjusted based on the characteristic order in BT_GATT_SERVICE_DEFINE

    return bt_gatt_notify(default_conn, &irrigation_svc.attrs[calibration_index], value, sizeof(*value));
}

/**
 * @brief Update irrigation history
 */
int bt_irrigation_history_update(uint8_t channel_id, uint8_t entry_index) {
    if (!default_conn) {
        return -ENOTCONN;
    }

    if (channel_id >= WATERING_CHANNELS_COUNT) {
        return -EINVAL;
    }

    struct history_data *value = (struct history_data *) history_value;

    value->channel_id = channel_id;
    value->entry_index = entry_index;

    // In a real implementation, you should retrieve real history data
    // Here we use simulated values for demonstration
    value->timestamp = k_uptime_get_32() - (entry_index * 3600 * 1000);
    value->mode = entry_index % 2; // Alternate between modes
    value->duration = 500 + (entry_index * 100); // Simulated values
    value->success = 1; // Assume all were successful

    // Find the history characteristic index in service - adjust based on characteristic order
    int history_index = ATTR_IDX_HISTORY_VALUE;

    return bt_gatt_notify(default_conn, &irrigation_svc.attrs[history_index], value, sizeof(*value));
}

/**
 * @brief Update system diagnostics
 */
int bt_irrigation_diagnostics_update(void)
{
    /* Fail fast if we are not connected */
    if (!default_conn) {
        return -ENOTCONN;
    }

    /* Update diagnostics_value with fresh data */
    read_diagnostics(NULL,
                     &irrigation_svc.attrs[ATTR_IDX_DIAGNOSTICS_VALUE],
                     NULL, 0, 0);

    /* Notify central */
    return bt_gatt_notify(default_conn,
                          &irrigation_svc.attrs[ATTR_IDX_DIAGNOSTICS_VALUE],
                          diagnostics_value,
                          sizeof(struct diagnostics_data));
}

/**
 * @brief Execute a direct command on a channel
 *
 * This allows direct control of valves through Bluetooth
 * Commands: 0=close, 1=open, 2=pulse
 */
int bt_irrigation_direct_command(uint8_t channel_id, uint8_t command, uint16_t param) {
    ARG_UNUSED(channel_id);
    ARG_UNUSED(command);
    ARG_UNUSED(param);
    /* Direct open/close este interzis – folosim doar task-uri */
    return -ENOTSUP;
}

/* Update valve status via Bluetooth */
int bt_irrigation_valve_status_update(uint8_t channel_id, bool state) {
    if (!default_conn) {
        return -ENOTCONN;
    }

    struct valve_control_data *data = (struct valve_control_data *) valve_value;
    data->channel_id = channel_id;
    
    /* Valve status update - we can infer task type from current state */
    /* This is a status update, not a command */
    
    return bt_gatt_notify(default_conn,
                          &irrigation_svc.attrs[ATTR_IDX_VALVE_VALUE],
                          valve_value, sizeof(struct valve_control_data));
}

/* =================================================================== */
/* Disable the UNINTENTIONAL FULL DUPLICATION found later in this file */
/* =================================================================== */
#if 0
// ...all the repeated connection-callbacks, GATT definitions,
//     and other duplicate bodies – nothing is compiled inside...
#endif /* duplicate block disabled */
/* =================================================================== */

/* ----------  IMPLEMENTATIONS FOR CHARACTERISTICS --------------- */
static ssize_t read_schedule(struct bt_conn *c,
                             const struct bt_gatt_attr *a,
                             void *buf, uint16_t len, uint16_t off)
{
    struct schedule_config_data *sched = (struct schedule_config_data *)schedule_value;
    
    // Validate channel_id
    if (sched->channel_id >= WATERING_CHANNELS_COUNT) {
        sched->channel_id = 0;
    }
    
    // Get channel schedule data
    watering_channel_t *channel;
    if (watering_get_channel(sched->channel_id, &channel) == WATERING_SUCCESS) {
        sched->schedule_type = channel->watering_event.schedule_type;
        sched->days_mask = 0; // Not implemented yet
        sched->hour = 0; // Not implemented yet
        sched->minute = 0; // Not implemented yet
        sched->watering_mode = channel->watering_event.watering_mode;
        sched->value = 0; // Default value
    }
    
	return bt_gatt_attr_read(c, a, buf, len, off,
	                         a->user_data, sizeof(schedule_value));
}

static ssize_t write_schedule(struct bt_conn *c,
                              const struct bt_gatt_attr *a,
                              const void *buf, uint16_t len,
                              uint16_t off, uint8_t flags)
{
    // Support single-byte write to select channel for read
    if (len == 1 && off == 0) {
        struct schedule_config_data *sched = (struct schedule_config_data *)a->user_data;
        sched->channel_id = ((uint8_t *)buf)[0];
        
        if (sched->channel_id >= WATERING_CHANNELS_COUNT) {
            return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
        }
        
        return 1;
    }
    
    // Full structure write
	if (off + len > sizeof(schedule_value))
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);

	memcpy(((uint8_t *)a->user_data) + off, buf, len);
	
	// Apply schedule if complete structure received
	if (off + len == sizeof(struct schedule_config_data)) {
	    struct schedule_config_data *sched = (struct schedule_config_data *)a->user_data;
	    
	    // Validate and apply schedule
	    if (sched->channel_id < WATERING_CHANNELS_COUNT) {
	        watering_channel_t *channel;
	        if (watering_get_channel(sched->channel_id, &channel) == WATERING_SUCCESS) {
	            channel->watering_event.schedule_type = sched->schedule_type;
	            // Store schedule parameters (not fully implemented in watering_event_t yet)
	            channel->watering_event.watering_mode = sched->watering_mode;
	            
	            watering_save_config();
	            printk("BLE: Schedule updated for channel %d\n", sched->channel_id);
	        }
	    }
	}
	
	return len;
}

static void schedule_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	printk("Schedule notifications %s\n",
	       (value == BT_GATT_CCC_NOTIFY) ? "enabled" : "disabled");
}

static ssize_t read_system_config(struct bt_conn *c,
                                  const struct bt_gatt_attr *a,
                                  void *buf, uint16_t len, uint16_t off)
{
    struct system_config_data *config = (struct system_config_data *)system_config_value;
    
    // Populate with current system data according to docs
    config->power_mode = 0; // Normal mode by default
    uint32_t calibration = 0;
    watering_get_flow_calibration(&calibration);
    config->flow_calibration = calibration;
    config->max_active_valves = 1; // Always 1 as per docs
    
    // These fields are not in the documentation
    config->version = WATERING_CONFIG_VERSION;
    config->num_channels = WATERING_CHANNELS_COUNT;
    
	return bt_gatt_attr_read(c, a, buf, len, off,
	                         a->user_data,
	                         sizeof(system_config_value));
}

static ssize_t write_system_config(struct bt_conn *c,
                                   const struct bt_gatt_attr *a,
                                   const void *buf, uint16_t len,
                                   uint16_t off, uint8_t flags)
{
	if (off + len > sizeof(system_config_value))
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);

	memcpy(((uint8_t *)a->user_data) + off, buf, len);
	
	struct system_config_data *config = (struct system_config_data *)a->user_data;
	
	// Apply the configuration changes
	if (config->flow_calibration > 0) {
	    watering_set_flow_calibration(config->flow_calibration);
	    watering_save_config();
	}
	
	// Power mode changes could be implemented here
	// config->power_mode handling...
	
	printk("BLE: system-config written (%u bytes)\n", len);
	return len;
}

static void system_config_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	printk("System-Config notifications %s\n",
	       (value == BT_GATT_CCC_NOTIFY) ? "enabled" : "disabled");
}

static ssize_t read_task_queue(struct bt_conn *c,
                               const struct bt_gatt_attr *a,
                               void *buf, uint16_t len, uint16_t off)
{
    struct task_queue_data *queue = (struct task_queue_data *)task_queue_value;
    
    // Populate with current queue state
    queue->pending_count = watering_get_pending_tasks_count();
    queue->completed_tasks = 0; // Reserved for future use
    
    // Get current task info - simplified version since watering_get_current_task doesn't exist
    watering_state_t state;
    watering_get_state(&state);
    if (state == WATERING_STATE_WATERING) {
        // We can infer there's a current task but can't get details without API changes
        queue->current_channel = 0xFF; // Unknown
        queue->current_task_type = 0;
        queue->current_value = 0;
    } else {
        queue->current_channel = 0xFF; // No active task
        queue->current_task_type = 0;
        queue->current_value = 0;
    }
    
    // command field is write-only, task_id_to_delete is reserved
    queue->command = 0;
    queue->task_id_to_delete = 0;
    queue->active_task_id = 0; // Not implemented
    
	return bt_gatt_attr_read(c, a, buf, len, off,
	                         a->user_data,
	                         sizeof(task_queue_value));
}

static ssize_t write_task_queue(struct bt_conn *c,
                                const struct bt_gatt_attr *a,
                                const void *buf, uint16_t len,
                                uint16_t off, uint8_t flags)
{
	if (off + len > sizeof(task_queue_value))
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);

	memcpy(((uint8_t *)a->user_data) + off, buf, len);
	
	struct task_queue_data *queue = (struct task_queue_data *)a->user_data;
	
	// Process commands as per documentation
	switch (queue->command) {
	    case 1: // Cancel current task
	        watering_stop_current_task();
	        printk("BLE: Cancelled current task\n");
	        break;
	        
	    case 2: // Clear entire queue
	        watering_clear_task_queue();
	        printk("BLE: Cleared task queue\n");
	        break;
	        
	    case 3: // Delete specific task (reserved)
	        printk("BLE: Delete specific task not implemented\n");
	        break;
	        
	    case 4: // Clear runtime errors/alarms
	        watering_clear_errors();
	        // Send immediate status update
	        bt_irrigation_system_status_update(WATERING_STATE_IDLE);
	        printk("BLE: Cleared runtime errors\n");
	        break;
	        
	    default:
	        break;
	}
	
	// Reset command after processing
	queue->command = 0;
	
	printk("BLE: task-queue written (%u bytes)\n", len);
	return len;
}

static void task_queue_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	printk("Task-Queue notifications %s\n",
	       (value == BT_GATT_CCC_NOTIFY) ? "enabled" : "disabled");
}

static ssize_t read_statistics(struct bt_conn *c,
                               const struct bt_gatt_attr *a,
                               void *buf, uint16_t len, uint16_t off)
{
    struct statistics_data *stats = (struct statistics_data *)statistics_value;
    
    // Validate channel_id
    if (stats->channel_id >= WATERING_CHANNELS_COUNT) {
        stats->channel_id = 0;
    }
    
    // Get channel statistics - simplified since statistics member doesn't exist
    watering_channel_t *channel;
    if (watering_get_channel(stats->channel_id, &channel) == WATERING_SUCCESS) {
        // Return dummy values for now
        stats->total_volume = 0;
        stats->last_volume = 0;
        stats->last_watering = 0;
        stats->count = 0;
    }
    
	return bt_gatt_attr_read(c, a, buf, len, off,
	                         a->user_data,
	                         sizeof(statistics_value));
}

static ssize_t write_statistics(struct bt_conn *c,
                                const struct bt_gatt_attr *a,
                                const void *buf, uint16_t len,
                                uint16_t off, uint8_t flags)
{
    // Support single-byte write to select channel for read
    if (len == 1 && off == 0) {
        struct statistics_data *stats = (struct statistics_data *)a->user_data;
        stats->channel_id = ((uint8_t *)buf)[0];
        
        if (stats->channel_id >= WATERING_CHANNELS_COUNT) {
            return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
        }
        
        return 1;
    }
    
    return BT_GATT_ERR(BT_ATT_ERR_NOT_SUPPORTED);
}

static void statistics_ccc_cfg_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	printk("Statistics notifications %s\n",
	       (value == BT_GATT_CCC_NOTIFY) ? "enabled" : "disabled");
}

static void rtc_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	printk("RTC notifications %s\n",
	       (value == BT_GATT_CCC_NOTIFY) ? "enabled" : "disabled");
}
/* ------------------------------------------------------------------- */

/* Initialize Bluetooth irrigation service */
int bt_irrigation_service_init(void) {
    int err;
    
    printk("Starting Bluetooth irrigation service...\n");
    
    /* Register connection callbacks */
    bt_conn_cb_register(&conn_callbacks);
    
    /* Enable Bluetooth */
    err = bt_enable(NULL);
    if (err) {
        printk("Bluetooth init failed (err %d)\n", err);
        return err;
    }
    
    printk("Bluetooth initialized\n");
    
    /* Load settings after Bluetooth initialization */
    if (IS_ENABLED(CONFIG_SETTINGS)) {
        settings_load();
    }
    
    /* Start advertising */
    err = bt_le_adv_start(&adv_param,
                          adv_ad, ARRAY_SIZE(adv_ad),
                          adv_sd, ARRAY_SIZE(adv_sd));
    if (err) {
        printk("Advertising failed to start (err %d)\n", err);
        return err;
    }
    
    printk("Advertising successfully started\n");
    return 0;
}