#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/sys/printk.h>

#include "bt_irrigation_service.h"
#include "watering.h"
#include "watering_internal.h"  // Add this include to access internal state

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

static struct bt_uuid_128 irrigation_service_uuid = BT_UUID_INIT_128(BT_UUID_IRRIGATION_SERVICE_VAL);
static struct bt_uuid_128 valve_char_uuid = BT_UUID_INIT_128(BT_UUID_IRRIGATION_VALVE_VAL);
static struct bt_uuid_128 flow_char_uuid = BT_UUID_INIT_128(BT_UUID_IRRIGATION_FLOW_VAL);
static struct bt_uuid_128 status_char_uuid = BT_UUID_INIT_128(BT_UUID_IRRIGATION_STATUS_VAL);
static struct bt_uuid_128 channel_config_uuid = BT_UUID_INIT_128(BT_UUID_IRRIGATION_CHANNEL_CONFIG_VAL);
static struct bt_uuid_128 schedule_uuid = BT_UUID_INIT_128(BT_UUID_IRRIGATION_SCHEDULE_VAL);
static struct bt_uuid_128 system_config_uuid = BT_UUID_INIT_128(BT_UUID_IRRIGATION_SYSTEM_CONFIG_VAL);
static struct bt_uuid_128 task_queue_uuid = BT_UUID_INIT_128(BT_UUID_IRRIGATION_TASK_QUEUE_VAL);
static struct bt_uuid_128 statistics_uuid = BT_UUID_INIT_128(BT_UUID_IRRIGATION_STATISTICS_VAL);

/* Valve control data structure - modificată pentru a crea task-uri */
struct valve_control_data {
    uint8_t channel_id;
    uint8_t task_type;  // 0=durată, 1=volum
    uint16_t value;     // minute sau litri
} __packed;

/* Channel configuration structure */
struct channel_config_data {
    uint8_t channel_id;
    uint8_t name_len;
    char name[16];
    uint8_t auto_enabled;
} __packed;

/* Schedule configuration structure */
struct schedule_config_data {
    uint8_t channel_id;
    uint8_t schedule_type;       // 0=zilnic, 1=periodic
    uint8_t days_mask;           // Zilele pentru programare zilnică sau interval zile pentru periodic
    uint8_t hour;
    uint8_t minute;
    uint8_t watering_mode;       // 0=durată, 1=volum
    uint16_t value;              // Minute sau litri
} __packed;

/* System configuration structure */
struct system_config_data {
    uint8_t power_mode;
    uint32_t flow_calibration;   // Impulsuri per litru
    uint8_t max_active_valves;
} __packed;

/* Task queue structure */
struct task_queue_data {
    uint8_t pending_tasks;
    uint8_t completed_tasks;
    uint8_t current_channel;     // Canalul activ curent (0xFF dacă nu există)
    uint8_t current_task_type;   // 0=durată, 1=volum
    uint16_t current_value;      // Minute sau litri pentru task-ul curent
} __packed;

/* Statistics structure for a channel */
struct statistics_data {
    uint8_t channel_id;
    uint32_t total_volume;       // Volum total în ml
    uint32_t last_volume;        // Ultimul volum în ml
    uint32_t last_watering;      // Timestamp ultimă udare
    uint16_t count;              // Număr total de udări
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
static ssize_t read_channel_config(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                        void *buf, uint16_t len, uint16_t offset);
static ssize_t write_channel_config(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                         const void *buf, uint16_t len, uint16_t offset, uint8_t flags);
static void channel_config_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value);
static ssize_t read_schedule(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                        void *buf, uint16_t len, uint16_t offset);
static ssize_t write_schedule(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                         const void *buf, uint16_t len, uint16_t offset, uint8_t flags);
static void schedule_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value);
static ssize_t read_system_config(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                        void *buf, uint16_t len, uint16_t offset);
static ssize_t write_system_config(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                         const void *buf, uint16_t len, uint16_t offset, uint8_t flags);
static void system_config_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value);
static ssize_t read_task_queue(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                        void *buf, uint16_t len, uint16_t offset);
static ssize_t write_task_queue(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                         const void *buf, uint16_t len, uint16_t offset, uint8_t flags);
static void task_queue_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value);
static ssize_t read_statistics(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                        void *buf, uint16_t len, uint16_t offset);
static void statistics_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value);

/* Irrigation Service Definition */
BT_GATT_SERVICE_DEFINE(irrigation_svc,
    BT_GATT_PRIMARY_SERVICE(&irrigation_service_uuid),
    
    /* Task Creation Characteristic (fostă Valve Control) */
    BT_GATT_CHARACTERISTIC(&valve_char_uuid.uuid,
                         BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE | BT_GATT_CHRC_NOTIFY,
                         BT_GATT_PERM_READ | BT_GATT_PERM_WRITE,
                         read_valve, write_valve, &valve_value),
    BT_GATT_CCC(valve_ccc_cfg_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
    
    /* Flow Data Characteristic */
    BT_GATT_CHARACTERISTIC(&flow_char_uuid.uuid,
                         BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
                         BT_GATT_PERM_READ,
                         read_flow, NULL, &flow_value),
    BT_GATT_CCC(flow_ccc_cfg_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
    
    /* System Status Characteristic */
    BT_GATT_CHARACTERISTIC(&status_char_uuid.uuid,
                         BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
                         BT_GATT_PERM_READ,
                         read_status, NULL, &status_value),
    BT_GATT_CCC(status_ccc_cfg_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
                         
    /* Channel Configuration Characteristic */
    BT_GATT_CHARACTERISTIC(&channel_config_uuid.uuid,
                         BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE | BT_GATT_CHRC_NOTIFY,
                         BT_GATT_PERM_READ | BT_GATT_PERM_WRITE,
                         read_channel_config, write_channel_config, &channel_config_value),
    BT_GATT_CCC(channel_config_ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
    
    /* Schedule Configuration Characteristic */
    BT_GATT_CHARACTERISTIC(&schedule_uuid.uuid,
                         BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE | BT_GATT_CHRC_NOTIFY,
                         BT_GATT_PERM_READ | BT_GATT_PERM_WRITE,
                         read_schedule, write_schedule, &schedule_value),
    BT_GATT_CCC(schedule_ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
                         
    /* System Configuration Characteristic */
    BT_GATT_CHARACTERISTIC(&system_config_uuid.uuid,
                         BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE | BT_GATT_CHRC_NOTIFY,
                         BT_GATT_PERM_READ | BT_GATT_PERM_WRITE,
                         read_system_config, write_system_config, &system_config_value),
    BT_GATT_CCC(system_config_ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
                         
    /* Task Queue Characteristic */
    BT_GATT_CHARACTERISTIC(&task_queue_uuid.uuid,
                         BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE | BT_GATT_CHRC_NOTIFY,
                         BT_GATT_PERM_READ | BT_GATT_PERM_WRITE,
                         read_task_queue, write_task_queue, &task_queue_value),
    BT_GATT_CCC(task_queue_ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
                         
    /* Statistics Characteristic */
    BT_GATT_CHARACTERISTIC(&statistics_uuid.uuid,
                         BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
                         BT_GATT_PERM_READ,
                         read_statistics, NULL, &statistics_value),
    BT_GATT_CCC(statistics_ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
);

/* Bluetooth connection callback */
static struct bt_conn *default_conn;

static void connected(struct bt_conn *conn, uint8_t err)
{
    if (err) {
        printk("Connection failed (err %u)\n", err);
        return;
    }

    if (!default_conn) {
        default_conn = bt_conn_ref(conn);
    }
    
    printk("Connected to irrigation controller\n");
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
    printk("Disconnected (reason %u)\n", reason);

    if (default_conn) {
        bt_conn_unref(default_conn);
        default_conn = NULL;
    }
}

static struct bt_conn_cb conn_callbacks = {
    .connected = connected,
    .disconnected = disconnected,
};

/* Valve characteristic read callback */
static ssize_t read_valve(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                        void *buf, uint16_t len, uint16_t offset)
{
    const struct valve_control_data *value = attr->user_data;
    
    return bt_gatt_attr_read(conn, attr, buf, len, offset, value,
                            sizeof(struct valve_control_data));
}

/* Valve characteristic write callback - modificat pentru a crea taskuri */
static ssize_t write_valve(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                         const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
    struct valve_control_data *value = attr->user_data;
    
    if (offset + len > sizeof(struct valve_control_data)) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }

    memcpy(((uint8_t *)value) + offset, buf, len);
    
    /* Process task creation request */
    uint8_t channel_id = value->channel_id;
    uint8_t task_type = value->task_type;
    uint16_t task_value = value->value;
    
    if (channel_id >= WATERING_CHANNELS_COUNT) {
        return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
    }
    
    printk("BT request: canal %d, tip task %d, valoare %d\n", 
           channel_id, task_type, task_value);
    
    /* Creează task-ul corespunzător */
    watering_error_t err;
    if (task_type == 0) {  // Durată (minute)
        err = watering_add_duration_task(channel_id, task_value);
        if (err == WATERING_SUCCESS) {
            printk("Task de durată adăugat prin Bluetooth: canal %d, %d minute\n", 
                  channel_id + 1, task_value);
        }
    } else if (task_type == 1) {  // Volum (litri)
        err = watering_add_volume_task(channel_id, task_value);
        if (err == WATERING_SUCCESS) {
            printk("Task de volum adăugat prin Bluetooth: canal %d, %d litri\n", 
                  channel_id + 1, task_value);
        }
    } else {
        return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
    }
    
    if (err != WATERING_SUCCESS) {
        printk("Eroare la adăugarea task-ului: %d\n", err);
        return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
    }
    
    return len;
}

/* CCC configuration change callback for valve characteristic */
static void valve_ccc_cfg_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
    bool notif_enabled = (value == BT_GATT_CCC_NOTIFY);
    printk("Valve notifications %s\n", notif_enabled ? "enabled" : "disabled");
}

/* Flow characteristic read callback */
static ssize_t read_flow(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                       void *buf, uint16_t len, uint16_t offset)
{
    const uint32_t *value = attr->user_data;
    
    return bt_gatt_attr_read(conn, attr, buf, len, offset, value,
                            sizeof(uint32_t));
}

/* CCC configuration change callback for flow characteristic */
static void flow_ccc_cfg_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
    bool notif_enabled = (value == BT_GATT_CCC_NOTIFY);
    printk("Flow notifications %s\n", notif_enabled ? "enabled" : "disabled");
}

/* Status characteristic read callback */
static ssize_t read_status(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                         void *buf, uint16_t len, uint16_t offset)
{
    const uint8_t *value = attr->user_data;
    
    return bt_gatt_attr_read(conn, attr, buf, len, offset, value, sizeof(uint8_t));
}

/* CCC configuration change callback for status characteristic */
static void status_ccc_cfg_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
    bool notif_enabled = (value == BT_GATT_CCC_NOTIFY);
    printk("Status notifications %s\n", notif_enabled ? "enabled" : "disabled");
}

/* Channel Config characteristic read callback */
static ssize_t read_channel_config(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                        void *buf, uint16_t len, uint16_t offset)
{
    struct channel_config_data *value = (struct channel_config_data *)channel_config_value;
    
    if (value->channel_id >= WATERING_CHANNELS_COUNT) {
        // Default to channel 0 if invalid
        value->channel_id = 0;
    }
    
    // Fetch latest data for the requested channel
    watering_channel_t *channel;
    watering_get_channel(value->channel_id, &channel);
    
    // Update the data
    value->channel_id = value->channel_id;
    strncpy(value->name, channel->name, sizeof(value->name) - 1);
    value->name_len = strlen(value->name) > sizeof(value->name) ? sizeof(value->name) : strlen(value->name);
    value->auto_enabled = channel->watering_event.auto_enabled ? 1 : 0;
    
    return bt_gatt_attr_read(conn, attr, buf, len, offset, value, sizeof(*value));
}

/* Channel Config characteristic write callback */
static ssize_t write_channel_config(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                         const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
    struct channel_config_data *value = (struct channel_config_data *)attr->user_data;
    
    if (offset + len > sizeof(*value)) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }

    memcpy(((uint8_t *)value) + offset, buf, len);
    
    if (value->channel_id >= WATERING_CHANNELS_COUNT) {
        return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
    }
    
    // Update the channel configuration
    watering_channel_t *channel;
    watering_error_t err = watering_get_channel(value->channel_id, &channel);
    if (err != WATERING_SUCCESS) {
        return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
    }
    
    // Update name if it's set in the write
    if (value->name_len > 0 && value->name_len <= sizeof(value->name)) {
        value->name[value->name_len] = '\0'; // Ensure null termination
        strncpy(channel->name, value->name, sizeof(channel->name) - 1);
        channel->name[sizeof(channel->name) - 1] = '\0';
    }
    
    // Update auto-enabled if changed
    channel->watering_event.auto_enabled = value->auto_enabled ? true : false;
    
    // Save the updated configuration
    watering_save_config();
    
    return len;
}

/* CCC configuration change callback for channel config characteristic */
static void channel_config_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
    bool notif_enabled = (value == BT_GATT_CCC_NOTIFY);
    printk("Channel config notifications %s\n", notif_enabled ? "enabled" : "disabled");
}

/* Schedule characteristic read callback */
static ssize_t read_schedule(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                        void *buf, uint16_t len, uint16_t offset)
{
    struct schedule_config_data *value = (struct schedule_config_data *)schedule_value;
    
    if (value->channel_id >= WATERING_CHANNELS_COUNT) {
        // Default to channel 0 if invalid
        value->channel_id = 0;
    }
    
    // Fetch latest data for the requested channel
    watering_channel_t *channel;
    watering_get_channel(value->channel_id, &channel);
    watering_event_t *event = &channel->watering_event;
    
    // Update the data
    value->schedule_type = event->schedule_type;
    if (event->schedule_type == SCHEDULE_DAILY) {
        value->days_mask = event->schedule.daily.days_of_week;
    } else {
        value->days_mask = event->schedule.periodic.interval_days;
    }
    value->hour = event->start_time.hour;
    value->minute = event->start_time.minute;
    value->watering_mode = event->watering_mode;
    if (event->watering_mode == WATERING_BY_DURATION) {
        value->value = event->watering.by_duration.duration_minutes;
    } else {
        value->value = event->watering.by_volume.volume_liters;
    }
    
    return bt_gatt_attr_read(conn, attr, buf, len, offset, value, sizeof(*value));
}

/* Schedule characteristic write callback */
static ssize_t write_schedule(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                         const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
    struct schedule_config_data *value = (struct schedule_config_data *)attr->user_data;
    
    if (offset + len > sizeof(*value)) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }

    memcpy(((uint8_t *)value) + offset, buf, len);
    
    if (value->channel_id >= WATERING_CHANNELS_COUNT) {
        return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
    }
    
    // Update the schedule configuration
    watering_channel_t *channel;
    watering_error_t err = watering_get_channel(value->channel_id, &channel);
    if (err != WATERING_SUCCESS) {
        return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
    }
    
    watering_event_t *event = &channel->watering_event;
    event->schedule_type = value->schedule_type;
    if (value->schedule_type == SCHEDULE_DAILY) {
        event->schedule.daily.days_of_week = value->days_mask;
    } else {
        event->schedule.periodic.interval_days = value->days_mask;
    }
    event->start_time.hour = value->hour;
    event->start_time.minute = value->minute;
    event->watering_mode = value->watering_mode;
    if (value->watering_mode == WATERING_BY_DURATION) {
        event->watering.by_duration.duration_minutes = value->value;
    } else {
        event->watering.by_volume.volume_liters = value->value;
    }
    
    // Validate the configuration
    if (watering_validate_event_config(event) != WATERING_SUCCESS) {
        // If validation fails, revert to previous values
        read_schedule(conn, attr, NULL, 0, 0);
        return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
    }
    
    // Save the updated configuration
    watering_save_config();
    
    return len;
}

/* CCC configuration change callback for schedule characteristic */
static void schedule_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
    bool notif_enabled = (value == BT_GATT_CCC_NOTIFY);
    printk("Schedule notifications %s\n", notif_enabled ? "enabled" : "disabled");
}

/* System Config characteristic read callback */
static ssize_t read_system_config(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                        void *buf, uint16_t len, uint16_t offset)
{
    struct system_config_data *value = (struct system_config_data *)system_config_value;
    
    // Fetch current system configuration
    power_mode_t power_mode;
    watering_get_power_mode(&power_mode);
    value->power_mode = power_mode;
    
    uint32_t calibration;
    watering_get_flow_calibration(&calibration);
    value->flow_calibration = calibration;
    
    // Hard-coded to 1 since this is enforced in valve_control.c now
    value->max_active_valves = 1;
    
    return bt_gatt_attr_read(conn, attr, buf, len, offset, value, sizeof(*value));
}

/* System Config characteristic write callback */
static ssize_t write_system_config(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                         const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
    struct system_config_data *value = (struct system_config_data *)attr->user_data;
    
    if (offset + len > sizeof(*value)) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }

    memcpy(((uint8_t *)value) + offset, buf, len);
    
    // Update system configuration
    if (value->power_mode <= POWER_MODE_ULTRA_LOW_POWER) {
        watering_set_power_mode((power_mode_t)value->power_mode);
    }
    
    if (value->flow_calibration > 0) {
        watering_set_flow_calibration(value->flow_calibration);
    }
    
    // max_active_valves is read-only, enforced by the system
    
    // Save the updated configuration
    watering_save_config();
    
    return len;
}

/* CCC configuration change callback for system config characteristic */
static void system_config_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
    bool notif_enabled = (value == BT_GATT_CCC_NOTIFY);
    printk("System config notifications %s\n", notif_enabled ? "enabled" : "disabled");
}

/* Task Queue characteristic read callback */
static ssize_t read_task_queue(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                        void *buf, uint16_t len, uint16_t offset)
{
    struct task_queue_data *value = (struct task_queue_data *)task_queue_value;
    
    // Note: To implement this correctly, we would need to add functions to count queue items
    // For now, we'll just report some dummy values
    value->pending_tasks = 0; // would need to be implemented
    value->completed_tasks = 0; // would need to be implemented
    
    // Check if there's a current active task
    if (watering_task_state.current_active_task != NULL) {
        watering_channel_t *active_channel = watering_task_state.current_active_task->channel;
        value->current_channel = active_channel - watering_channels;
        
        if (active_channel->watering_event.watering_mode == WATERING_BY_DURATION) {
            value->current_task_type = 0; // Duration
            value->current_value = active_channel->watering_event.watering.by_duration.duration_minutes;
        } else {
            value->current_task_type = 1; // Volume
            value->current_value = active_channel->watering_event.watering.by_volume.volume_liters;
        }
    } else {
        // No active task
        value->current_channel = 0xFF; // Invalid channel ID
        value->current_task_type = 0;
        value->current_value = 0;
    }
    
    return bt_gatt_attr_read(conn, attr, buf, len, offset, value, sizeof(*value));
}

/* Task Queue characteristic write callback */
static ssize_t write_task_queue(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                         const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
    struct task_queue_data *value = (struct task_queue_data *)attr->user_data;
    
    if (offset + len > sizeof(*value)) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }

    memcpy(((uint8_t *)value) + offset, buf, len);
    
    // Check if this is a command to cancel tasks
    // We could implement a command to clear all pending tasks or cancel the current one
    // For now, let's say if pending_tasks is set to 0xFF, we'll try to stop current task
    if (value->pending_tasks == 0xFF) {
        if (watering_stop_current_task()) {
            printk("Current task cancelled via Bluetooth\n");
        }
    }
    
    return len;
}

/* CCC configuration change callback for task queue characteristic */
static void task_queue_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
    bool notif_enabled = (value == BT_GATT_CCC_NOTIFY);
    printk("Task queue notifications %s\n", notif_enabled ? "enabled" : "disabled");
}

/* Statistics characteristic read callback */
static ssize_t read_statistics(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                        void *buf, uint16_t len, uint16_t offset)
{
    struct statistics_data *value = (struct statistics_data *)statistics_value;
    
    if (value->channel_id >= WATERING_CHANNELS_COUNT) {
        // Default to channel 0 if invalid
        value->channel_id = 0;
    }
    
    // Fetch latest data for the requested channel
    watering_channel_t *channel;
    watering_get_channel(value->channel_id, &channel);
    
    // Note: These fields would need to be implemented in the watering system
    // For now we'll provide dummy values or available values
    value->total_volume = 0; // Would need watering stats implementation
    value->last_volume = 0; // Would need watering stats implementation
    value->last_watering = channel->last_watering_time;
    value->count = 0; // Would need watering stats implementation
    
    return bt_gatt_attr_read(conn, attr, buf, len, offset, value, sizeof(*value));
}

/* CCC configuration change callback for statistics characteristic */
static void statistics_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
    bool notif_enabled = (value == BT_GATT_CCC_NOTIFY);
    printk("Statistics notifications %s\n", notif_enabled ? "enabled" : "disabled");
}

/* Initialize the Bluetooth irrigation service */
int bt_irrigation_service_init(void)
{
    bt_conn_cb_register(&conn_callbacks);

    /* Initialize Bluetooth */
    int err = bt_enable(NULL);
    if (err) {
        printk("Bluetooth init failed (err %d)\n", err);
        return err;
    }

    printk("Bluetooth initialized\n");
    printk("Irrigation service initialized\n");
    
    /* Set advertising data using non-deprecated API */
    struct bt_data ad[] = {
        BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
        BT_DATA_BYTES(BT_DATA_UUID128_ALL, 
                     0xf0, 0xde, 0xbc, 0x9a, 0x78, 0x56, 0x34, 0x12,
                     0x34, 0x12, 0x78, 0x56, 0x34, 0x12, 0x78, 0x12),
    };
    
    struct bt_data sd[] = {
        BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME, sizeof(CONFIG_BT_DEVICE_NAME) - 1),
    };
    
    /* Use pre-defined BT_LE_ADV_CONN instead of custom parameters with deprecated options */
    err = bt_le_adv_start(BT_LE_ADV_CONN, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
    if (err) {
        printk("Advertising failed to start (err %d)\n", err);
        return err;
    }

    printk("Advertising successfully started\n");
    return 0;
}

/* Update valve status via Bluetooth - modificat pentru a raporta taskuri în execuție */
int bt_irrigation_valve_status_update(uint8_t channel_id, bool state)
{
    if (!default_conn) {
        return -ENOTCONN;
    }

    // Raportează doar statusul canalului, fără a permite control direct
    valve_value[0] = channel_id;
    valve_value[1] = state ? 1 : 0;
    valve_value[2] = 0;  // Nu este relevant pentru notificări
    valve_value[3] = 0;  // Nu este relevant pentru notificări

    return bt_gatt_notify(default_conn, &irrigation_svc.attrs[2],
                        valve_value, sizeof(struct valve_control_data));
}

/* Update flow data via Bluetooth */
int bt_irrigation_flow_update(uint32_t pulses)
{
    if (!default_conn) {
        return -ENOTCONN;
    }

    memcpy(flow_value, &pulses, sizeof(pulses));
    
    return bt_gatt_notify(default_conn, &irrigation_svc.attrs[5],
                        flow_value, sizeof(uint32_t));
}

/* Update system status via Bluetooth */
int bt_irrigation_system_status_update(watering_status_t status)
{
    if (!default_conn) {
        return -ENOTCONN;
    }

    status_value[0] = (uint8_t)status;
    
    return bt_gatt_notify(default_conn, &irrigation_svc.attrs[8],
                        status_value, sizeof(uint8_t));
}

/* Update channel configuration via Bluetooth */
int bt_irrigation_channel_config_update(uint8_t channel_id)
{
    if (!default_conn) {
        return -ENOTCONN;
    }

    if (channel_id >= WATERING_CHANNELS_COUNT) {
        return -EINVAL;
    }

    struct channel_config_data *config = (struct channel_config_data *)channel_config_value;
    watering_channel_t *channel;
    watering_error_t err = watering_get_channel(channel_id, &channel);
    if (err != WATERING_SUCCESS) {
        return -EINVAL;
    }
    
    config->channel_id = channel_id;
    strncpy(config->name, channel->name, sizeof(config->name) - 1);
    config->name_len = strlen(config->name);
    config->auto_enabled = channel->watering_event.auto_enabled ? 1 : 0;
    
    return bt_gatt_notify(default_conn, &irrigation_svc.attrs[11],
                        config, sizeof(*config));
}

/* Update queue status via Bluetooth */
int bt_irrigation_queue_status_update(uint8_t count)
{
    if (!default_conn) {
        return -ENOTCONN;
    }

    struct task_queue_data *queue = (struct task_queue_data *)task_queue_value;
    queue->pending_tasks = count;
    
    // Other fields are filled in read_task_queue
    read_task_queue(NULL, &irrigation_svc.attrs[20], NULL, 0, 0);
    
    return bt_gatt_notify(default_conn, &irrigation_svc.attrs[20],
                        queue, sizeof(*queue));
}

/* Update system configuration via Bluetooth */
int bt_irrigation_config_update(void)
{
    if (!default_conn) {
        return -ENOTCONN;
    }

    // Populate with current data
    read_system_config(NULL, &irrigation_svc.attrs[17], NULL, 0, 0);
    
    return bt_gatt_notify(default_conn, &irrigation_svc.attrs[17],
                        system_config_value, sizeof(struct system_config_data));
}

/* Update statistics via Bluetooth */
int bt_irrigation_statistics_update(uint8_t channel_id)
{
    if (!default_conn) {
        return -ENOTCONN;
    }

    if (channel_id >= WATERING_CHANNELS_COUNT) {
        return -EINVAL;
    }

    struct statistics_data *stats = (struct statistics_data *)statistics_value;
    stats->channel_id = channel_id;
    
    // Populate with current data
    read_statistics(NULL, &irrigation_svc.attrs[23], NULL, 0, 0);
    
    return bt_gatt_notify(default_conn, &irrigation_svc.attrs[23],
                        stats, sizeof(*stats));
}
