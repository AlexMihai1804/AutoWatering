#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/sys/printk.h>
#include <stdio.h>  // Add this for snprintf function

#include "bt_irrigation_service.h"
#include "watering.h"
#include "watering_internal.h"  // Add this include to access internal state
#include "rtc.h"
#include "flow_sensor.h"

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

/* Adăugăm noile UUID-uri pentru funcționalitățile extinse */
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
    uint8_t command; /* Comandă pentru controlul cozii: 0=nimic, 1=anulează task curent, 2=golește coada */
    uint8_t task_id_to_delete; /* ID-ul task-ului de șters (dacă se implementează) */
} __packed;

/* Statistics structure for a channel */
struct statistics_data {
    uint8_t channel_id;
    uint32_t total_volume;       // Volum total în ml
    uint32_t last_volume;        // Ultimul volum în ml
    uint32_t last_watering;      // Timestamp ultimă udare
    uint16_t count;              // Număr total de udări
} __packed;

/* Structură pentru setarea/citirea RTC */
struct rtc_data {
    uint8_t year;    /* Anul minus 2000 (0-99) */
    uint8_t month;   /* Luna (1-12) */
    uint8_t day;     /* Ziua (1-31) */
    uint8_t hour;    /* Ora (0-23) */
    uint8_t minute;  /* Minutul (0-59) */
    uint8_t second;  /* Secunda (0-59) */
    uint8_t day_of_week; /* Ziua săptămânii (0-6, 0=Duminică) */
} __packed;

/* Structură pentru alarme și notificări */
struct alarm_data {
    uint8_t alarm_code;   /* Codul alarmei */
    uint16_t alarm_data;  /* Date suplimentare specifice alarmei */
    uint32_t timestamp;   /* Timestamp când s-a produs alarma */
} __packed;

/* Structură pentru calibrarea senzorului de debit */
struct calibration_data {
    uint8_t action;       /* 0=oprește, 1=începe, 2=în desfășurare, 3=calculat */
    uint32_t pulses;      /* Număr de impulsuri contorizate */
    uint32_t volume_ml;   /* Volum în ml (intrare sau calculat) */
    uint32_t pulses_per_liter; /* Rezultatul calibrării */
} __packed;

/* Structură pentru istoricul irigării */
struct history_data {
    uint8_t channel_id;   /* Canal (0-7) */
    uint8_t entry_index;  /* Indexul intrării (0=cea mai recentă) */
    uint32_t timestamp;   /* Când a avut loc */
    uint8_t mode;         /* 0=durată, 1=volum */
    uint16_t duration;    /* Durata în secunde sau volum în ml */
    uint8_t success;      /* 1=succes, 0=eșuat */
} __packed;

/* Structură pentru diagnostice */
struct diagnostics_data {
    uint32_t uptime;      /* Timp de funcționare în minute */
    uint8_t error_count;  /* Număr de erori */
    uint8_t last_error;   /* Ultimul cod de eroare */
    uint8_t valve_status; /* Biți pentru starea supapelor */
    uint8_t battery_level; /* Nivel baterie în procente sau 0xFF dacă nu se aplică */
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

/* Variabile globale pentru calibrare */
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
static ssize_t read_rtc(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                        void *buf, uint16_t len, uint16_t offset);
static ssize_t write_rtc(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                         const void *buf, uint16_t len, uint16_t offset, uint8_t flags);
static void rtc_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value);
static ssize_t read_alarm(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                        void *buf, uint16_t len, uint16_t offset);
static void alarm_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value);
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

    /* RTC Characteristic */
    BT_GATT_CHARACTERISTIC(&rtc_char_uuid.uuid,
                         BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE | BT_GATT_CHRC_NOTIFY,
                         BT_GATT_PERM_READ | BT_GATT_PERM_WRITE,
                         read_rtc, write_rtc, &rtc_value),
    BT_GATT_CCC(rtc_ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
    
    /* Alarm Characteristic */
    BT_GATT_CHARACTERISTIC(&alarm_char_uuid.uuid,
                         BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
                         BT_GATT_PERM_READ,
                         read_alarm, NULL, &alarm_value),
    BT_GATT_CCC(alarm_ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
    
    /* Flow Calibration Characteristic */
    BT_GATT_CHARACTERISTIC(&calibration_char_uuid.uuid,
                         BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE | BT_GATT_CHRC_NOTIFY,
                         BT_GATT_PERM_READ | BT_GATT_PERM_WRITE,
                         read_calibration, write_calibration, &calibration_value),
    BT_GATT_CCC(calibration_ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
    
    /* History Characteristic */
    BT_GATT_CHARACTERISTIC(&history_char_uuid.uuid,
                         BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE | BT_GATT_CHRC_NOTIFY,
                         BT_GATT_PERM_READ | BT_GATT_PERM_WRITE,
                         read_history, write_history, &history_value),
    BT_GATT_CCC(history_ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
    
    /* Diagnostics Characteristic */
    BT_GATT_CHARACTERISTIC(&diagnostics_char_uuid.uuid,
                         BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
                         BT_GATT_PERM_READ,
                         read_diagnostics, NULL, &diagnostics_value),
    BT_GATT_CCC(diagnostics_ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
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
    // Replace strncpy with safer snprintf
    snprintf(value->name, sizeof(value->name), "%s", channel->name);
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
    
    // Utilizăm noua funcție pentru a obține numărul real de task-uri în așteptare
    value->pending_tasks = watering_get_pending_tasks_count();
    value->completed_tasks = 0; // Nu avem încă o monitorizare a task-urilor finalizate
    
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
    
    // Resetăm comanda și ID-ul task-ului
    value->command = 0;
    value->task_id_to_delete = 0;
    
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
    
    // Procesăm comenzile cozii de task-uri
    switch(value->command) {
        case 1:  // Anulează task-ul curent
            if (watering_stop_current_task()) {
                printk("Task-ul curent anulat prin Bluetooth\n");
            } else {
                printk("Nu există task curent de anulat\n");
            }
            break;
            
        case 2:  // Golește întreaga coadă
            {
                int removed = watering_clear_task_queue();
                printk("S-au eliminat %d task-uri din coadă prin comandă Bluetooth\n", removed);
            }
            break;
            
        case 3:  // Șterge un task specific (bazat pe ID)
            // Această funcționalitate este mai complexă deoarece necesită identificarea task-urilor
            // și o structură de date care să permită ștergerea selectivă
            printk("Ștergerea selectivă a unui task nu este încă implementată complet\n");
            break;
            
        default:
            // Nicio comandă sau comandă necunoscută
            break;
    }
    
    // Actualizăm numărul de task-uri în așteptare
    value->pending_tasks = watering_get_pending_tasks_count();
    
    // Resetăm comanda pentru a evita executarea repetată
    value->command = 0;
    
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

/* Implementare pentru RTC */
static ssize_t read_rtc(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                        void *buf, uint16_t len, uint16_t offset)
{
    struct rtc_data *value = (struct rtc_data *)rtc_value;
    rtc_datetime_t now;
    
    // Citește data și ora curentă din RTC
    if (rtc_datetime_get(&now) == 0) {
        value->year = now.year - 2000;    // Convertim în format pe 2 cifre
        value->month = now.month;
        value->day = now.day;
        value->hour = now.hour;
        value->minute = now.minute;
        value->second = now.second;
        value->day_of_week = now.day_of_week;
    } else {
        // RTC indisponibil, folosește valori implicite
        value->year = 23;   // 2023
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
                         const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
    struct rtc_data *value = (struct rtc_data *)attr->user_data;
    
    if (offset + len > sizeof(*value)) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }

    memcpy(((uint8_t *)value) + offset, buf, len);
    
    // Validează datele primite
    if (value->month < 1 || value->month > 12 || value->day < 1 || value->day > 31 ||
        value->hour > 23 || value->minute > 59 || value->second > 59 || value->day_of_week > 6) {
        return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
    }
    
    // Actualizează RTC-ul
    rtc_datetime_t new_time;
    new_time.year = 2000 + value->year;  // Convertim înapoi la an complet
    new_time.month = value->month;
    new_time.day = value->day;
    new_time.hour = value->hour;
    new_time.minute = value->minute;
    new_time.second = value->second;
    new_time.day_of_week = value->day_of_week;
    
    int ret = rtc_datetime_set(&new_time);
    if (ret != 0) {
        printk("Eroare la setarea RTC: %d\n", ret);
        return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
    }
    
    printk("RTC actualizat prin Bluetooth: %02d/%02d/%04d %02d:%02d:%02d (ziua %d)\n",
           new_time.day, new_time.month, new_time.year,
           new_time.hour, new_time.minute, new_time.second,
           new_time.day_of_week);
    
    return len;
}

static void rtc_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
    bool notif_enabled = (value == BT_GATT_CCC_NOTIFY);
    printk("Notificări RTC %s\n", notif_enabled ? "activate" : "dezactivate");
}

/* Implementare pentru alarme */
static ssize_t read_alarm(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                        void *buf, uint16_t len, uint16_t offset)
{
    struct alarm_data *value = (struct alarm_data *)alarm_value;
    
    return bt_gatt_attr_read(conn, attr, buf, len, offset, value, sizeof(*value));
}

static void alarm_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
    bool notif_enabled = (value == BT_GATT_CCC_NOTIFY);
    printk("Notificări alarme %s\n", notif_enabled ? "activate" : "dezactivate");
}

/* Implementare pentru calibrare */
static ssize_t read_calibration(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                        void *buf, uint16_t len, uint16_t offset)
{
    struct calibration_data *value = (struct calibration_data *)calibration_value;
    
    if (calibration_active) {
        uint32_t current_pulses = get_pulse_count();
        value->pulses = current_pulses - calibration_start_pulses;
        value->action = 2; // În desfășurare
    }
    
    return bt_gatt_attr_read(conn, attr, buf, len, offset, value, sizeof(*value));
}

static ssize_t write_calibration(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                         const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
    struct calibration_data *value = (struct calibration_data *)attr->user_data;
    
    if (offset + len > sizeof(*value)) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }

    memcpy(((uint8_t *)value) + offset, buf, len);
    
    // Procesează cererea de calibrare
    if (value->action == 1) {  // Începe calibrarea
        if (!calibration_active) {
            reset_pulse_count();
            calibration_start_pulses = 0;
            calibration_active = true;
            value->pulses = 0;
            printk("Calibrare senzor de debit începută\n");
        }
    } else if (value->action == 0) {  // Oprește calibrarea și calculează
        if (calibration_active) {
            uint32_t final_pulses = get_pulse_count();
            uint32_t total_pulses = final_pulses - calibration_start_pulses;
            uint32_t volume_ml = value->volume_ml;
            
            if (volume_ml > 0 && total_pulses > 0) {
                uint32_t new_calibration = (total_pulses * 1000) / volume_ml;
                value->pulses_per_liter = new_calibration;
                
                // Actualizează calibrarea sistemului
                watering_set_flow_calibration(new_calibration);
                watering_save_config();
                
                printk("Calibrare senzor de debit finalizată: %d impulsuri pentru %d ml = %d impulsuri/litru\n",
                       total_pulses, volume_ml, new_calibration);
                
                // Setează starea completată
                value->action = 3;  // Calibrare calculată
                value->pulses = total_pulses;
            }
            calibration_active = false;
        }
    }
    
    return len;
}

static void calibration_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
    bool notif_enabled = (value == BT_GATT_CCC_NOTIFY);
    printk("Notificări calibrare %s\n", notif_enabled ? "activate" : "dezactivate");
}

/* Implementare pentru istoric */
static ssize_t read_history(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                        void *buf, uint16_t len, uint16_t offset)
{
    struct history_data *value = (struct history_data *)history_value;
    
    // Aici ar trebui să avem o implementare reală a istoricului
    // Pentru moment, folosim valori statice
    
    return bt_gatt_attr_read(conn, attr, buf, len, offset, value, sizeof(*value));
}

static ssize_t write_history(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                         const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
    struct history_data *value = (struct history_data *)attr->user_data;
    
    if (offset + len > sizeof(*value)) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }

    memcpy(((uint8_t *)value) + offset, buf, len);
    
    // Clientul solicită o anumită intrare din istoric
    if (value->channel_id < WATERING_CHANNELS_COUNT) {
        // În implementarea reală, ar trebui să încărcați datele efective din istoric
        // Momentan simulăm răspunsuri
        value->timestamp = k_uptime_get_32() - (value->entry_index * 3600 * 1000);
        value->mode = value->entry_index % 2;  // Alternează între moduri
        value->duration = 500 + (value->entry_index * 100);  // Valori simulate
        value->success = 1;  // Presupunem că toate au reușit
        
        printk("Cerere de istoric pentru canalul %d, intrarea %d\n", 
               value->channel_id, value->entry_index);
    }
    
    return len;
}

static void history_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
    bool notif_enabled = (value == BT_GATT_CCC_NOTIFY);
    printk("Notificări istoric %s\n", notif_enabled ? "activate" : "dezactivate");
}

/* Implementare pentru diagnostice */
static ssize_t read_diagnostics(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                        void *buf, uint16_t len, uint16_t offset)
{
    struct diagnostics_data *value = (struct diagnostics_data *)diagnostics_value;
    
    // Colectează informațiile de diagnosticare
    value->uptime = k_uptime_get_32() / 60000;  // Convertit în minute
    
    // Aici ar trebui să colectăm date reale despre erori, starea supapelor, etc.
    // Pentru moment folosim valori fictive
    value->error_count = 0;
    value->last_error = 0;
    
    // Creează un bitmap cu starea supapelor
    value->valve_status = 0;
    for (int i = 0; i < WATERING_CHANNELS_COUNT && i < 8; i++) {
        if (watering_channels[i].is_active) {
            value->valve_status |= (1 << i);
        }
    }
    
    // Nu avem monitorizare a bateriei în acest sistem
    value->battery_level = 0xFF;
    
    return bt_gatt_attr_read(conn, attr, buf, len, offset, value, sizeof(*value));
}

static void diagnostics_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
    bool notif_enabled = (value == BT_GATT_CCC_NOTIFY);
    printk("Notificări diagnostice %s\n", notif_enabled ? "activate" : "dezactivate");
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
    
    /* Fix the BT_LE_ADV_PARAM_INIT macro by adding the missing peer parameter */
    struct bt_le_adv_param adv_param = BT_LE_ADV_PARAM_INIT(
        BT_LE_ADV_OPT_CONNECTABLE | BT_LE_ADV_OPT_USE_NAME,
        BT_GAP_ADV_FAST_INT_MIN_2,
        BT_GAP_ADV_FAST_INT_MAX_2,
        NULL  /* Add the missing peer parameter */
    );
    
    err = bt_le_adv_start(&adv_param, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
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
    // Replace strncpy with safer snprintf
    snprintf(config->name, sizeof(config->name), "%s", channel->name);
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
    
    // Utilizăm valoarea reală a numărului de task-uri dacă este specificată sau o obținem din sistem
    if (count == 0xFF) {
        queue->pending_tasks = watering_get_pending_tasks_count();
    } else {
        queue->pending_tasks = count;
    }
    
    // Celelalte câmpuri sunt completate în read_task_queue
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

/**
 * @brief Actualizează timpul RTC prin Bluetooth
 */
int bt_irrigation_rtc_update(rtc_datetime_t *datetime) {
    if (!default_conn) {
        return -ENOTCONN;
    }
    
    struct rtc_data *value = (struct rtc_data *)rtc_value;
    
    value->year = datetime->year - 2000;
    value->month = datetime->month;
    value->day = datetime->day;
    value->hour = datetime->hour;
    value->minute = datetime->minute;
    value->second = datetime->second;
    value->day_of_week = datetime->day_of_week;
    
    // Găsim indexul caracteristicii RTC în serviciu - ajustați în funcție de ordinea caracteristicilor
    int rtc_index = 26;  // Acest index trebuie ajustat în funcție de ordinea caracteristicilor în BT_GATT_SERVICE_DEFINE
    
    return bt_gatt_notify(default_conn, &irrigation_svc.attrs[rtc_index], value, sizeof(*value));
}

/**
 * @brief Notifică clientul Bluetooth despre o alarmă
 */
int bt_irrigation_alarm_notify(uint8_t alarm_code, uint16_t alarm_data) {
    if (!default_conn) {
        return -ENOTCONN;
    }
    
    struct alarm_data *value = (struct alarm_data *)alarm_value;
    
    value->alarm_code = alarm_code;
    value->alarm_data = alarm_data;
    value->timestamp = k_uptime_get_32();
    
    // Găsim indexul caracteristicii de alarmă în serviciu - ajustați în funcție de ordinea caracteristicilor
    int alarm_index = 29;  // Acest index trebuie ajustat în funcție de ordinea caracteristicilor în BT_GATT_SERVICE_DEFINE
    
    return bt_gatt_notify(default_conn, &irrigation_svc.attrs[alarm_index], value, sizeof(*value));
}

/**
 * @brief Începe o sesiune de calibrare a senzorului de debit
 */
int bt_irrigation_start_flow_calibration(uint8_t start, uint32_t volume_ml) {
    if (!default_conn) {
        return -ENOTCONN;
    }
    
    struct calibration_data *value = (struct calibration_data *)calibration_value;
    
    if (start) {
        // Începe calibrarea
        value->action = 1;
        value->volume_ml = 0; // Va fi setat la oprire
        value->pulses = 0;
        
        // Calibrarea efectivă se face când clientul scrie în caracteristică
    } else {
        // Oprește și calculează calibrarea
        value->action = 0;
        value->volume_ml = volume_ml;
        
        // Calibrarea efectivă se face când clientul scrie în caracteristică
    }
    
    // Găsim indexul caracteristicii de calibrare în serviciu - ajustați în funcție de ordinea caracteristicilor
    int calibration_index = 32;  // Acest index trebuie ajustat în funcție de ordinea caracteristicilor în BT_GATT_SERVICE_DEFINE
    
    return bt_gatt_notify(default_conn, &irrigation_svc.attrs[calibration_index], value, sizeof(*value));
}

/**
 * @brief Actualizează istoricul de irigare
 */
int bt_irrigation_history_update(uint8_t channel_id, uint8_t entry_index) {
    if (!default_conn) {
        return -ENOTCONN;
    }
    
    if (channel_id >= WATERING_CHANNELS_COUNT) {
        return -EINVAL;
    }
    
    struct history_data *value = (struct history_data *)history_value;
    
    value->channel_id = channel_id;
    value->entry_index = entry_index;
    
    // În implementarea reală, ar trebui să preluăm date reale din istoric
    // Aici folosim valori simulate pentru demonstrație
    value->timestamp = k_uptime_get_32() - (entry_index * 3600 * 1000);
    value->mode = entry_index % 2;  // Alternează între moduri
    value->duration = 500 + (entry_index * 100);  // Valori simulate
    value->success = 1;  // Presupunem că toate au reușit
    
    // Găsim indexul caracteristicii de istoric în serviciu - ajustați în funcție de ordinea caracteristicilor
    int history_index = 35;  // Acest index trebuie ajustat în funcție de ordinea caracteristicilor în BT_GATT_SERVICE_DEFINE
    
    return bt_gatt_notify(default_conn, &irrigation_svc.attrs[history_index], value, sizeof(*value));
}

/**
 * @brief Actualizează diagnosticele sistemului
 */
int bt_irrigation_diagnostics_update(void) {
    if (!default_conn) {
        return -ENOTCONN;
    }
    
    struct diagnostics_data *value = (struct diagnostics_data *)diagnostics_value;
    
    // Colectăm informațiile de diagnosticare actuale
    value->uptime = k_uptime_get_32() / 60000;  // Convertit în minute
    
    // Restul sunt completate în read_diagnostics
    read_diagnostics(NULL, NULL, NULL, 0, 0);
    
    // Găsim indexul caracteristicii de diagnostice în serviciu - ajustați în funcție de ordinea caracteristicilor
    int diagnostics_index = 38;  // Acest index trebuie ajustat în funcție de ordinea caracteristicilor în BT_GATT_SERVICE_DEFINE
    
    return bt_gatt_notify(default_conn, &irrigation_svc.attrs[diagnostics_index], value, sizeof(*value));
}
