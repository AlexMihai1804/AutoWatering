#include "ucp_uart.h"
#ifdef CONFIG_AUTOWATERING_UCP_UART
#include "ucp_cobs.h"
#include "ucp_crc32c.h"
#include "ucp_protocol.h"
#include "ucp_tlv.h"
#include <zephyr/app_version.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/kernel.h>
#include <zephyr/random/random.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/sys/atomic.h>
#include <errno.h>
#include <string.h>
#ifdef CONFIG_RETENTION_BOOT_MODE
#include <zephyr/retention/bootmode.h>
#endif
#include "timezone.h"
#include "rtc.h"
#include "watering.h"
#include "watering_internal.h"
#include "watering_enhanced.h"
#include "enhanced_system_status.h"
#include "water_balance_types.h"
#include "watering_history.h"
#include "flow_sensor.h"
#include "rain_sensor.h"
#include "rain_history.h"
#include "environmental_data.h"
#include "environmental_history.h"
#include "history_flash.h"
#include "bme280_driver.h"
#include "sensor_manager.h"
#include "temperature_compensation.h"
#include "configuration_status.h"
#include "onboarding_state.h"
#include "reset_controller.h"
#include "custom_soil_db.h"
#include "soil_moisture_config.h"
#include "bt_custom_soil_handlers.h"
#include "bt_interval_mode_handlers.h"
#include "bt_gatt_structs.h"
#include "bt_gatt_structs_enhanced.h"
#ifdef CONFIG_BT
#include "bt_irrigation_service.h"
#endif
#include "plant_db.h"
#include "nvs_config.h"
#include "pack_storage.h"

#define UCP_UART_BAUD_DEFAULT 460800U
#define UCP_RX_ACCUM_MAX UCP_MAX_FRAME_ENCODED_V1
#define UCP_RX_RING_MAX 512U
#define UCP_SESSION_TIMEOUT_MS 15000U
#define UCP_HEARTBEAT_MS 10000U
#define UCP_MAX_EVT_DELTA 160U
#define UCP_REPLAY_DEPTH 64U
#define UCP_OTA_REBOOT_DELAY_MS 60U
#define UCP_STREAM_CHUNK_MAX 240U
#define UCP_STREAM_BUF_MAX 768U
#define UCP_STREAM_IDLE_TIMEOUT_MS 8000U
#define UCP_PEER_POWER_GRACE_MS_DEFAULT 500U
#define UCP_PACK_STREAM_PAGE_MAX 12U
#define UCP_EVENT_MIN_INTERVAL_SENSOR_MS 100U
#define UCP_EVENT_MIN_INTERVAL_CONFIG_MS 120U
#define UCP_EVENT_MIN_INTERVAL_DEFAULT_MS 25U
#define UCP_OTA_CONFIRM_TTL_MS 10000U
#define UCP_ALARM_DEDUP_MS 100U
#define UCP_THREAD_WAIT_MS 100U
#define UCP_THREAD_WAIT_MAX_MS 1000U
#define UCP_NOTIFY_IDLE_NOSESSION_MS 10000U
#define UCP_DEFERRED_ONB_OVERVIEW_MIN_MS 2000U
#define UCP_TASK_POLL_ACTIVE_MS 200U
#define UCP_TASK_POLL_IDLE_MS 2000U
#define UCP_DEFERRED_RESCAN_BUDGET_MS 8U
#define UCP_DEFERRED_RESCAN_RX_QUIET_MS 200U
#define UCP_DEFERRED_RESCAN_POST_HELLO_GRACE_MS 15000U
#define UCP_PACK_STATS_CACHE_TTL_MS 10000U
#define UCP_PACK_STATS_REFRESH_MIN_INTERVAL_MS 3000U
#define UCP_ENV_HIST_STATS_CACHE_TTL_MS 10000U
#define UCP_ENV_HIST_STATS_REFRESH_MIN_INTERVAL_MS 3000U
#define UCP_RAIN_HIST_STATS_CACHE_TTL_MS 10000U
#define UCP_RAIN_HIST_STATS_REFRESH_MIN_INTERVAL_MS 3000U
#define UCP_UTC_CACHE_REFRESH_MS 1000U

#define UCP_SUB_MODE_LIVE_ONLY 0U
#define UCP_SUB_MODE_REPLAY_THEN_LIVE 1U

enum { TZ_UTC_OFF=1, TZ_DST_EN=2, TZ_DST_SM=3, TZ_DST_SW=4, TZ_DST_SD=5, TZ_DST_EM=6, TZ_DST_EW=7, TZ_DST_ED=8, TZ_DST_OFF=9 };
enum { RTC_SEC=1, RTC_MIN=2, RTC_HOUR=3, RTC_DAY=4, RTC_MONTH=5, RTC_YEAR=6, RTC_DOW=7, RTC_UNIX_UTC=8 };
enum { SYS_UPTIME=1, SYS_UTC=2, SYS_RTC=3, SYS_FW=4, SYS_BOOT=5, SYS_W_STATUS=6, SYS_W_STATE=7, SYS_PWR_MODE=8 };
enum { TASK_ACTIVE=1, TASK_PAUSED=2, TASK_PENDING=3, TASK_CUR_CH=4, TASK_TRIGGER=5, TASK_MODE=6, TASK_TARGET=7, TASK_COMPLETED=8, TASK_HAS_CURRENT=9 };
enum {
    RES_CH_ID=1, RES_CH_NAME=2, RES_CH_ACTIVE=3, RES_CH_LAST_WATER_TS=4, RES_CH_PLANT_ID=5,
    RES_CH_CFG_SCORE=6, RES_CH_SOIL_DB=7, RES_CH_IRR_METHOD=8, RES_CH_USE_AREA=9,
    RES_CH_AREA_M2=10, RES_CH_PLANT_COUNT=11, RES_CH_AUTO_MODE=12, RES_CH_MAX_VOL_L=13,
    RES_CH_CYCLE_SOAK=14, RES_CH_PLANTING_TS=15, RES_CH_SUN_PCT=16, RES_CH_LAT_DEG=17, RES_CH_LON_DEG=18
};
enum {
    RES_SCH_TYPE=1, RES_SCH_WATER_MODE=2, RES_SCH_DAYS_MASK=3, RES_SCH_INTERVAL_DAYS=4,
    RES_SCH_DURATION_MIN=5, RES_SCH_VOLUME_L=6, RES_SCH_START_HOUR=7, RES_SCH_START_MIN=8,
    RES_SCH_AUTO_ENABLED=9, RES_SCH_USE_SOLAR=10, RES_SCH_SOLAR_EVENT=11, RES_SCH_SOLAR_OFF_MIN=12
};
enum {
    FLOW_PULSE_COUNT=1, FLOW_RATE_PPS=2, FLOW_CAL_PPL=3
};
enum {
    RAIN_TOTAL_PULSES=1, RAIN_LAST_PULSE_TS=2, RAIN_CUR_HOUR_MM=3, RAIN_HOURLY_RATE_MMH=4,
    RAIN_STATUS=5, RAIN_DATA_QUALITY=6, RAIN_ENABLED=7, RAIN_INTEGRATION_ENABLED=8,
    RAIN_CALIB_MM_PER_PULSE=9, RAIN_DEBOUNCE_MS=10, RAIN_LAST_24H_MM=11, RAIN_HIST_HOURLY_COUNT=12,
    RAIN_HIST_DAILY_COUNT=13
};
enum {
    ENV_TEMP_C=1, ENV_HUMIDITY_PCT=2, ENV_PRESSURE_HPA=3, ENV_READING_TS=4, ENV_VALID=5,
    ENV_READINGS_COUNT=6, ENV_LAST_UPDATE=7, ENV_HIST_HOURLY_USED=8, ENV_HIST_DAILY_USED=9, ENV_HIST_MONTHLY_USED=10
};
enum {
    HIST_STORAGE_DETAILED_B=1, HIST_STORAGE_DAILY_B=2, HIST_STORAGE_MONTHLY_B=3, HIST_STORAGE_ANNUAL_B=4, HIST_STORAGE_TOTAL_KB=5
};
enum {
    PACKS_TOTAL_B=1, PACKS_USED_B=2, PACKS_FREE_B=3, PACKS_PACK_COUNT=4, PACKS_PLANT_COUNT=5,
    PACKS_CUSTOM_PLANT_COUNT=6, PACKS_CHANGE_COUNTER=7, PACKS_STAGE_VALID=8, PACKS_STAGE_LEN=9, PACKS_STAGE_CRC=10
};
enum { OTA_SUP=1, OTA_MODE=2, OTA_WAIT=3, OTA_ACC=4, OTA_REASON=5, OTA_FW=6, OTA_STATE=7, OTA_PENDING=8, OTA_CONFIRM=9,
       OTA_SWAP=10, OTA_CONFIRM_POLICY=11, OTA_ROLLBACK_OBS=12 };
enum { OTA_STATE_IDLE=0, OTA_STATE_ENTERING_RECOVERY=1, OTA_STATE_REBOOT_FAILED=2, OTA_STATE_POSTCHECK=3 };
enum { STRM_PURPOSE=1, STRM_STREAM_ID=2, STRM_CHUNK_SIZE=3, STRM_TOTAL_SIZE=4, STRM_DIRECTION=5, STRM_CHUNK_INDEX=6, STRM_OFFSET=7, STRM_DATA=8, STRM_RESULT=9, STRM_FINAL_CRC32C=10 };
enum {
    STRM_PURPOSE_DIAG_SNAPSHOT=1, STRM_PURPOSE_SCHEMA_TEXT=2, STRM_PURPOSE_UPLOAD_STAGING=3,
    STRM_PURPOSE_HISTORY_PAGE=4, STRM_PURPOSE_PACKS_LIST=5, STRM_PURPOSE_PACKS_PLANTS_LIST=6,
    STRM_PURPOSE_SCHEMA_CATALOG=7, STRM_PURPOSE_RAIN_HISTORY_RANGE=8,
    STRM_PURPOSE_ENV_HISTORY_RANGE=9, STRM_PURPOSE_DB_CATALOG=10,
    STRM_PURPOSE_BULK_SNAPSHOT=11,
    STRM_PURPOSE_WATER_HISTORY_RANGE=12
};
enum { STRM_DIR_OUTBOUND=1, STRM_DIR_INBOUND=2 };
enum { STRM_RESULT_OK=0, STRM_RESULT_ABORTED=1, STRM_RESULT_CRC_FAIL=2, STRM_RESULT_TIMEOUT=3 };
enum {
    SCHEMA_IMPL_MASK=2, SCHEMA_PROTO_CAPS=3, SCHEMA_SERVICE_MASK=4, SCHEMA_TOPIC_MASK=5, SCHEMA_STREAM_DIR_MASK=6, SCHEMA_STREAM_PURPOSE_MASK=7,
    SCHEMA_NAME=8, SCHEMA_KIND=9, SCHEMA_ACCESS_MASK=10, SCHEMA_EVENT_TOPIC=11, SCHEMA_FIELD_COUNT=12,
    SCHEMA_FIELD_ID=13, SCHEMA_FIELD_TYPE=14, SCHEMA_FIELD_FLAGS=15, SCHEMA_UNIT=16, SCHEMA_DESC_TEXT=17,
    SCHEMA_QUERY_INDEX=18, SCHEMA_QUERY_FIELD_ID=19, SCHEMA_STREAM_SCHEMA=20, SCHEMA_CONTENT_TYPE=21,
    SCHEMA_PAGE_OFFSET=22, SCHEMA_TOTAL_LEN=23, SCHEMA_ITEM_COUNT=24
};
enum { PEER_STATE_ON=1, PEER_STATE_HW_BACKED=2, PEER_STATE_INHIBIT=3, PEER_STATE_GRACE_MS=4, PEER_STATE_REASON=5, PEER_POLICY_MODE=6 };
enum { PEER_POLICY_MANUAL=0, PEER_POLICY_AUTO=1, PEER_REASON_NONE=0, PEER_REASON_UART_CMD=1, PEER_REASON_OTA=2 };
enum { DIAG_RX_OK=1, DIAG_RX_COBS_ERR=2, DIAG_RX_CRC_ERR=3, DIAG_RX_PROTO_ERR=4, DIAG_RX_OVERSIZE=5, DIAG_TX_FRAMES=6, DIAG_ERR_FRAMES=7, DIAG_EVT_SENT=8,
       DIAG_STREAM_OUT_OPEN=9, DIAG_STREAM_IN_OPEN=10, DIAG_STREAM_ABORTS=11, DIAG_STREAM_TIMEOUTS=12, DIAG_SESSION_HELLO=13, DIAG_SESSION_TIMEOUTS=14,
       DIAG_SUB_MASK=15, DIAG_REPLAY_COUNT=16, DIAG_LAST_EVENT_SEQ=17, DIAG_LAST_RX_MS=18,
       DIAG_EVT_COALESCED=19, DIAG_EVT_RATE_LIMITED=20, DIAG_LAST_EVT_MS=21 };
enum { DSTRM_OUT_ACTIVE=1, DSTRM_OUT_ID=2, DSTRM_OUT_OFF=3, DSTRM_OUT_TOTAL=4, DSTRM_IN_ACTIVE=5, DSTRM_IN_ID=6, DSTRM_IN_RX=7, DSTRM_IN_CAP=8, DSTRM_IN_PURPOSE=9 };
enum { DSTOR_NVS_READY=1, DSTOR_PACK_READY=2, DSTOR_PACK_TOTAL_B=3, DSTOR_PACK_USED_B=4, DSTOR_PACK_FREE_B=5, DSTOR_HIST_TOTAL_KB=6 };
enum {
    DDOM_VER_SYS=1, DDOM_VER_TZ=2, DDOM_VER_RTC=3, DDOM_VER_TASK=4, DDOM_VER_OTA=5, DDOM_VER_PEER=6, DDOM_VER_DIAG=7, DDOM_VER_SCHEMA=8,
    DDOM_VER_FLOW=9, DDOM_VER_RAIN=10, DDOM_VER_ENV=11, DDOM_VER_HISTORY=12, DDOM_VER_PACKS=13,
    DDOM_LAST_POLL_CFG_MS=14, DDOM_LAST_POLL_SYS_MS=15, DDOM_LAST_POLL_SENSOR_MS=16, DDOM_PEER_HW=17
};
enum { ALARM_CODE_F=1, ALARM_DATA_F=2, ALARM_TS_F=3, ALARM_CLEARED_F=4 };
enum { VALVE_CH_F=1, VALVE_OPEN_F=2, VALVE_IS_MASTER_F=3 };
enum { RAW_BYTES_F=1, RAW_FORMAT_F=2, RAW_SELECTOR_F=3 };
enum { HIST_KIND_F=6, HIST_START_TS_F=7, HIST_END_TS_F=8 };
enum { RESET_TYPE_F=1, RESET_CHANNEL_F=2, RESET_CODE_F=3, RESET_GROUP_F=4, RESET_REASON_F=5 };
enum { HYD_CH_PROFILE_F=32, HYD_CH_TOL_HIGH_F=33, HYD_CH_TOL_LOW_F=34, HYD_CH_MON_EN_F=35 };
enum { DEV_BULK_KIND_F=40 };
enum {
    Q_CHANNEL_ID=100, Q_PAGE_INDEX=101, Q_OFFSET=102, Q_LIMIT=103,
    Q_PACK_ID=104, Q_DELETE_PLANTS=105, Q_PLANT_ID=106,
    Q_DATA_TYPE=107, Q_START_TS=108, Q_END_TS=109, Q_DB_KIND=110
};
enum {
    SCHEMA_ACCESS_GET=BIT(0), SCHEMA_ACCESS_SET=BIT(1), SCHEMA_ACCESS_CMD=BIT(2),
    SCHEMA_ACCESS_STREAM_OUT=BIT(3), SCHEMA_ACCESS_STREAM_IN=BIT(4), SCHEMA_ACCESS_EVENT=BIT(5)
};

enum {
    UCP_CAP_GET            = BIT(0),
    UCP_CAP_SET            = BIT(1),
    UCP_CAP_CMD            = BIT(2),
    UCP_CAP_SUB_EVENTS     = BIT(3),
    UCP_CAP_EVENT_REPLAY   = BIT(4),
    UCP_CAP_STREAM_OUT     = BIT(5),
    UCP_CAP_STREAM_IN      = BIT(6),
    UCP_CAP_OTA_HANDOFF    = BIT(7),
    UCP_CAP_SCHEMA_DISC    = BIT(8),
    UCP_CAP_DIAG           = BIT(9),
    UCP_CAP_PEER_POWER     = BIT(10),
};

#define UCP_HELLO_CAPS_MASK (UCP_CAP_GET | UCP_CAP_SET | UCP_CAP_CMD | UCP_CAP_SUB_EVENTS | \
                             UCP_CAP_EVENT_REPLAY | UCP_CAP_STREAM_OUT | UCP_CAP_STREAM_IN | \
                             UCP_CAP_OTA_HANDOFF | UCP_CAP_SCHEMA_DISC | UCP_CAP_DIAG | UCP_CAP_PEER_POWER)

struct ucp_out_stream_state {
    bool active;
    uint16_t id;
    uint8_t purpose;
    uint16_t chunk_size;
    uint16_t total_size;
    uint16_t offset;
    uint16_t chunk_index;
    uint32_t opened_ms;
    uint32_t last_activity_ms;
    uint8_t data[UCP_STREAM_BUF_MAX];
};

struct ucp_in_stream_state {
    bool active;
    uint16_t id;
    uint8_t purpose;
    uint16_t chunk_size;
    uint16_t received;
    uint16_t chunk_index_next;
    bool expect_total;
    uint16_t expected_total;
    uint32_t opened_ms;
    uint32_t last_activity_ms;
    uint8_t data[UCP_STREAM_BUF_MAX];
};

struct ucp_replay_evt {
    bool valid;
    uint16_t topic;
    uint16_t svc;
    uint16_t res;
    uint32_t instance_id;
    uint32_t ver;
    uint8_t origin;
    uint64_t event_seq;
    uint64_t ts_utc_ms;
    uint16_t delta_len;
    uint8_t delta[UCP_MAX_EVT_DELTA];
};

struct ucp_diag_counters {
    uint32_t rx_ok_frames;
    uint32_t rx_cobs_err;
    uint32_t rx_crc_err;
    uint32_t rx_proto_err;
    uint32_t rx_oversize;
    uint32_t tx_frames;
    uint32_t tx_error_frames;
    uint32_t event_frames;
    uint32_t stream_out_open;
    uint32_t stream_in_open;
    uint32_t stream_aborts;
    uint32_t stream_timeouts;
    uint32_t session_hello;
    uint32_t session_timeouts;
    uint32_t event_coalesced;
    uint32_t event_rate_limited;
};

struct ucp_task_snapshot {
    bool valid;
    bool active;
    bool paused;
    uint8_t pending;
    bool has_current;
    uint8_t channel_id;
    uint8_t trigger;
    uint8_t mode;
    uint32_t target;
    uint32_t completed;
};

struct ucp_channel_state_snapshot {
    bool valid;
    bool is_active;
    uint32_t last_watering_time;
    uint16_t plant_id;
    uint8_t config_score;
};

struct ucp_channel_config_snapshot {
    bool valid;
    char name[64];
    uint16_t plant_id;
    uint8_t soil_db_index;
    uint8_t irrigation_method_index;
    bool use_area_based;
    float area_m2;
    uint16_t plant_count;
    uint8_t auto_mode;
    float max_volume_limit_l;
    bool enable_cycle_soak;
    uint32_t planting_date_unix;
    uint8_t sun_exposure_pct;
    float latitude_deg;
    float longitude_deg;
};

struct ucp_schedule_snapshot {
    bool valid;
    watering_event_t ev;
};

struct ucp_system_snapshot {
    bool valid;
    uint8_t status;
    uint8_t state;
    uint8_t power_mode;
};

struct ucp_flow_snapshot {
    bool valid;
    uint32_t pulses;
    uint32_t rate_pps;
    uint32_t cal_ppl;
};

struct ucp_rain_snapshot {
    bool valid;
    rain_sensor_data_t sensor;
    rain_history_stats_t hist;
    bool hist_ok;
    bool enabled;
    bool integration_enabled;
    float calib;
    uint16_t debounce_ms;
    float last24h_mm;
};

struct ucp_env_snapshot {
    bool valid;
    bme280_environmental_data_t env;
    env_history_stats_t hist;
    bool hist_ok;
};

struct ucp_schema_resource_desc {
    uint16_t svc;
    uint16_t res;
    uint8_t kind;
    uint8_t access_mask;
    uint16_t topic_id;
    uint8_t field_count;
    const char *name;
    const char *desc;
};

struct ucp_schema_stream_purpose_desc {
    uint8_t purpose;
    uint8_t dir_mask;
    uint16_t max_total;
    const char *name;
    const char *desc;
};

static const struct device *g_uart;
static bool g_ready, g_session;
static uint32_t g_session_id, g_tx_seq = 1U, g_last_rx_ms, g_last_evt_ms, g_sub_mask, g_session_start_ms;
static uint64_t g_boot_id, g_evt_seq = 1ULL;
static uint32_t g_ver_sys = 1U, g_ver_tz = 1U, g_ver_rtc = 1U, g_ver_alarm = 1U;
static uint32_t g_ver_task = 1U, g_ver_ota = 1U;
static uint32_t g_ver_peer = 1U, g_ver_diag = 1U, g_ver_schema = 1U;
static uint32_t g_ver_syscfg = 1U, g_ver_bulk = 1U, g_ver_auto_calc = 1U, g_ver_auto_calc_ctl = 1U;
static uint32_t g_ver_flow_cal = 1U;
static uint32_t g_ver_stats[WATERING_CHANNELS_COUNT];
static uint32_t g_ver_hydraulic[WATERING_CHANNELS_COUNT];
static uint32_t g_ver_custom_soil[WATERING_CHANNELS_COUNT];
static uint32_t g_ver_soil_moisture[WATERING_CHANNELS_COUNT + 1U]; /* [0]=global(0xFF), [1..8]=channels 0..7 */
static uint32_t g_ver_interval_cfg[WATERING_CHANNELS_COUNT];
static uint32_t g_ver_cfg_status[WATERING_CHANNELS_COUNT];
static uint32_t g_ver_comp_status[WATERING_CHANNELS_COUNT];
static uint32_t g_ver_comp_cfg[WATERING_CHANNELS_COUNT];
static uint32_t g_ver_rain_integration = 1U;
static uint32_t g_ver_onboarding = 1U, g_ver_onb_overview = 1U, g_ver_reset_ctl = 1U;
static bool g_ota_reboot_pending;
static bool g_peer_power_on = true;
static bool g_peer_power_hw_backed;
static uint8_t g_peer_power_policy = PEER_POLICY_MANUAL;
static uint16_t g_peer_power_grace_ms = UCP_PEER_POWER_GRACE_MS_DEFAULT;
static uint16_t g_next_stream_id = 1U;
static struct ucp_out_stream_state g_out_stream;
static struct ucp_in_stream_state g_in_stream;
static struct ucp_task_snapshot g_task_snapshot;
static struct ucp_channel_state_snapshot g_ch_state_snap[WATERING_CHANNELS_COUNT];
static struct ucp_channel_config_snapshot g_ch_cfg_snap[WATERING_CHANNELS_COUNT];
static struct ucp_schedule_snapshot g_sched_snap[WATERING_CHANNELS_COUNT];
static struct ucp_system_snapshot g_sys_snap;
static struct ucp_flow_snapshot g_flow_snap;
static struct ucp_rain_snapshot g_rain_snap;
static struct ucp_env_snapshot g_env_snap;
static struct ucp_replay_evt g_replay[UCP_REPLAY_DEPTH];
static struct ucp_diag_counters g_diag;
static uint16_t g_replay_head, g_replay_count;
static uint32_t g_ver_channel_state[WATERING_CHANNELS_COUNT];
static uint32_t g_ver_channel_cfg[WATERING_CHANNELS_COUNT];
static uint32_t g_ver_schedule_cfg[WATERING_CHANNELS_COUNT];
static uint32_t g_ver_flow = 1U, g_ver_rain = 1U, g_ver_env = 1U, g_ver_history = 1U, g_ver_packs = 1U;
static uint32_t g_last_poll_cfg_ms, g_last_poll_sensors_ms, g_last_poll_system_ms;
static uint32_t g_last_poll_task_ms, g_last_poll_packs_ms;
static bool g_poll_notifies_enabled;
static atomic_t g_notify_rescan_mask;
static uint32_t g_topic_scope_mask_override;
static uint32_t g_last_pack_change_counter;
static pack_storage_stats_t g_pack_stats_cache;
static bool g_pack_stats_cache_valid;
static uint32_t g_pack_stats_cache_ms;
static uint32_t g_pack_stats_refresh_try_ms;
static atomic_t g_pack_stats_refresh_pending;
static env_history_stats_t g_env_hist_stats_cache;
static bool g_env_hist_stats_cache_valid;
static uint32_t g_env_hist_stats_cache_ms;
static uint32_t g_env_hist_stats_refresh_try_ms;
static atomic_t g_env_hist_stats_refresh_pending;
static rain_history_stats_t g_rain_hist_stats_cache;
static bool g_rain_hist_stats_cache_valid;
static uint32_t g_rain_hist_stats_cache_ms;
static uint32_t g_rain_hist_stats_refresh_try_ms;
static atomic_t g_rain_hist_stats_refresh_pending;
static uint8_t g_cfg_status_rescan_rr_idx;
static uint8_t g_onb_rescan_phase;
static uint8_t g_auto_calc_rescan_rr_idx;
static uint8_t g_comp_status_rescan_rr_idx;
static uint32_t g_last_topic_rescan_req_ms[33];
static uint32_t g_last_topic_emit_ms[33];
static uint32_t g_last_onb_overview_deferred_ms;
static uint32_t g_sig_system_cfg, g_sig_bulk_snapshot, g_sig_onboarding_status, g_sig_onb_overview, g_sig_reset_control, g_sig_rain_integration;
static uint32_t g_sig_flow_cal_status;
static uint32_t g_sig_stats[WATERING_CHANNELS_COUNT];
static uint32_t g_sig_auto_calc[WATERING_CHANNELS_COUNT + 1U]; /* [0]=global */
static uint32_t g_sig_auto_calc_ctl;
static uint32_t g_sig_hydraulic[WATERING_CHANNELS_COUNT];
static uint32_t g_sig_custom_soil[WATERING_CHANNELS_COUNT];
static uint32_t g_sig_soil_moisture[WATERING_CHANNELS_COUNT + 1U];
static uint32_t g_sig_interval_cfg[WATERING_CHANNELS_COUNT];
static uint32_t g_sig_cfg_status[WATERING_CHANNELS_COUNT];
static uint32_t g_sig_comp_status[WATERING_CHANNELS_COUNT];
static uint32_t g_sig_comp_cfg[WATERING_CHANNELS_COUNT];
static bool g_cfg_reset_meta_valid[WATERING_CHANNELS_COUNT];
static uint32_t g_cfg_reset_meta_ts[WATERING_CHANNELS_COUNT];
static uint8_t g_cfg_reset_meta_count[WATERING_CHANNELS_COUNT];
static uint8_t g_cfg_reset_meta_group[WATERING_CHANNELS_COUNT];
static char g_cfg_reset_meta_reason[WATERING_CHANNELS_COUNT][32];
static bool g_staged_blob_valid;
static uint16_t g_staged_blob_len;
static uint32_t g_staged_blob_crc;
static uint8_t g_staged_blob[UCP_STREAM_BUF_MAX];
static history_event_t g_hist_stream_events[DETAILED_EVENTS_PER_CHANNEL];
static uint32_t g_hist_stream_ts[DETAILED_EVENTS_PER_CHANNEL];
static pack_pack_list_entry_t g_pack_stream_entries[UCP_PACK_STREAM_PAGE_MAX];
static pack_plant_list_entry_t g_plant_stream_entries[UCP_PACK_STREAM_PAGE_MAX];
static uint8_t g_rx_accum[UCP_RX_ACCUM_MAX];
static size_t g_rx_len;
static struct ring_buf g_rx_ring;
static uint8_t g_rx_ring_mem[UCP_RX_RING_MAX];
static bool g_uart_irq_rx_mode;
static uint8_t g_decoded[UCP_MAX_FRAME_DECODED_V1];
static uint8_t g_tx_decoded[UCP_MAX_FRAME_DECODED_V1];
static uint8_t g_tx_encoded[UCP_MAX_FRAME_ENCODED_V1];
static uint32_t g_ota_confirm_token;
static uint32_t g_ota_confirm_deadline_ms;
static bool g_ota_boot_telemetry_sent;
static uint8_t g_last_alarm_code;
static uint16_t g_last_alarm_data;
static bool g_last_alarm_cleared;
static uint32_t g_last_alarm_pub_ms;
static uint32_t g_dbg_rx_bytes;
static uint32_t g_dbg_uart_diag_last_ms;
static uint32_t g_dbg_irq_drop_bytes;
static uint32_t g_dbg_stack_diag_last_ms;
static bool g_utc_cache_valid;
static uint32_t g_utc_cache_unix_s;
static uint32_t g_utc_cache_uptime_ms;

static const struct ucp_schema_resource_desc g_schema_resources[] = {
    { UCP_SVC_DEVICE, UCP_RES_DEVICE_SYSTEM_STATUS, 1U, SCHEMA_ACCESS_GET | SCHEMA_ACCESS_EVENT, UCP_TOPIC_SYSTEM_STATUS, 8U, "device.system_status", "System status snapshot + status events" },
    { UCP_SVC_DEVICE, UCP_RES_DEVICE_ALARM_EVENT,   2U, SCHEMA_ACCESS_EVENT, UCP_TOPIC_ALARM, 4U, "device.alarm_event", "Alarm event notifications (event-only in v1)" },
    { UCP_SVC_DEVICE, UCP_RES_DEVICE_SYSTEM_CONFIG, 1U, SCHEMA_ACCESS_GET | SCHEMA_ACCESS_SET | SCHEMA_ACCESS_EVENT, UCP_TOPIC_SYSTEM_CONFIG, 8U, "device.system_config", "BLE-compatible enhanced system config bytes" },
    { UCP_SVC_DEVICE, UCP_RES_DEVICE_STATISTICS,    1U, SCHEMA_ACCESS_GET | SCHEMA_ACCESS_CMD | SCHEMA_ACCESS_EVENT, UCP_TOPIC_STATISTICS, 4U, "device.statistics", "Per-channel statistics bytes + reset commands" },
    { UCP_SVC_DEVICE, UCP_RES_DEVICE_AUTO_CALC_STATUS, 1U, SCHEMA_ACCESS_GET | SCHEMA_ACCESS_SET | SCHEMA_ACCESS_CMD | SCHEMA_ACCESS_EVENT, UCP_TOPIC_AUTO_CALC_STATUS, 6U, "device.auto_calc_status", "Auto-calc status/config (BLE-compatible bytes)" },
    { UCP_SVC_DEVICE, UCP_RES_DEVICE_AUTO_CALC_CONTROL, 1U, SCHEMA_ACCESS_GET | SCHEMA_ACCESS_SET | SCHEMA_ACCESS_EVENT, UCP_TOPIC_AUTO_CALC_STATUS, 4U, "device.auto_calc_control", "Global auto-calc enable + interval (hours)" },
    { UCP_SVC_DEVICE, UCP_RES_DEVICE_BULK_SNAPSHOT, 1U, SCHEMA_ACCESS_GET | SCHEMA_ACCESS_STREAM_OUT | SCHEMA_ACCESS_EVENT, UCP_TOPIC_BULK_SNAPSHOT, 3U, "device.bulk_snapshot", "Compact bulk snapshot + stream export" },
    { UCP_SVC_DEVICE, UCP_RES_DEVICE_ALARM_STATUS,  1U, SCHEMA_ACCESS_GET | SCHEMA_ACCESS_CMD | SCHEMA_ACCESS_EVENT, UCP_TOPIC_ALARM, 4U, "device.alarm_status", "Current latched alarm status + clear command" },
    { UCP_SVC_CHANNEL, UCP_RES_CHANNEL_STATE,       1U, SCHEMA_ACCESS_GET | SCHEMA_ACCESS_CMD | SCHEMA_ACCESS_EVENT, UCP_TOPIC_CHANNEL_STATE, 5U, "channel.state", "Per-channel runtime state + manual/direct commands" },
    { UCP_SVC_CHANNEL, UCP_RES_CHANNEL_CONFIG,      1U, SCHEMA_ACCESS_GET | SCHEMA_ACCESS_SET | SCHEMA_ACCESS_EVENT, UCP_TOPIC_CHANNEL_CONFIG, 18U, "channel.config", "Per-channel configuration" },
    { UCP_SVC_CHANNEL, UCP_RES_CHANNEL_CUSTOM_SOIL, 1U, SCHEMA_ACCESS_GET | SCHEMA_ACCESS_SET | SCHEMA_ACCESS_EVENT, UCP_TOPIC_CUSTOM_SOIL, 4U, "channel.custom_soil", "Custom soil config (BLE-compatible bytes)" },
    { UCP_SVC_CHANNEL, UCP_RES_CHANNEL_SOIL_MOISTURE, 1U, SCHEMA_ACCESS_GET | SCHEMA_ACCESS_SET | SCHEMA_ACCESS_EVENT, UCP_TOPIC_SOIL_MOISTURE, 4U, "channel.soil_moisture", "Soil moisture override config (BLE-compatible bytes)" },
    { UCP_SVC_CHANNEL, UCP_RES_CHANNEL_INTERVAL_CONFIG, 1U, SCHEMA_ACCESS_GET | SCHEMA_ACCESS_SET | SCHEMA_ACCESS_EVENT, UCP_TOPIC_INTERVAL_MODE, 4U, "channel.interval_config", "Interval mode config/status bytes" },
    { UCP_SVC_CHANNEL, UCP_RES_CHANNEL_CONFIG_STATUS, 1U, SCHEMA_ACCESS_GET | SCHEMA_ACCESS_EVENT, UCP_TOPIC_CHANNEL_CFG_STATUS, 4U, "channel.config_status", "Per-channel config completeness bytes" },
    { UCP_SVC_SCHEDULE, UCP_RES_SCHEDULE_CONFIG,    1U, SCHEMA_ACCESS_GET | SCHEMA_ACCESS_SET | SCHEMA_ACCESS_EVENT, UCP_TOPIC_SCHEDULE_CONFIG, 12U, "schedule.config", "Per-channel schedule configuration" },
    { UCP_SVC_TASK, UCP_RES_TASK_STATUS,            1U, SCHEMA_ACCESS_GET | SCHEMA_ACCESS_CMD | SCHEMA_ACCESS_EVENT, UCP_TOPIC_TASK_CURRENT, 9U, "task.status", "Task queue/current task status + queue control" },
    { UCP_SVC_FLOW, UCP_RES_FLOW_STATUS,            1U, SCHEMA_ACCESS_GET | SCHEMA_ACCESS_SET | SCHEMA_ACCESS_EVENT, UCP_TOPIC_FLOW_LIVE, 3U, "flow.status", "Flow sensor live + calibration" },
    { UCP_SVC_FLOW, UCP_RES_FLOW_HYDRAULIC_STATUS,  1U, SCHEMA_ACCESS_GET | SCHEMA_ACCESS_SET | SCHEMA_ACCESS_CMD | SCHEMA_ACCESS_EVENT, UCP_TOPIC_HYDRAULIC_STATUS, 6U, "flow.hydraulic_status", "Per-channel hydraulic status/config bytes" },
    { UCP_SVC_FLOW, UCP_RES_FLOW_CALIBRATION_STATUS, 1U, SCHEMA_ACCESS_GET | SCHEMA_ACCESS_SET | SCHEMA_ACCESS_EVENT, UCP_TOPIC_FLOW_CALIBRATION, 4U, "flow.calibration_status", "Flow calibration workflow status (BLE-compatible)" },
    { UCP_SVC_RAIN, UCP_RES_RAIN_STATUS,            1U, SCHEMA_ACCESS_GET | SCHEMA_ACCESS_SET | SCHEMA_ACCESS_EVENT, UCP_TOPIC_RAIN_LIVE, 13U, "rain.status", "Rain sensor + config + history summary" },
    { UCP_SVC_ENVIRONMENT, UCP_RES_ENV_CURRENT,     1U, SCHEMA_ACCESS_GET | SCHEMA_ACCESS_EVENT, UCP_TOPIC_ENV_LIVE, 10U, "environment.current", "Environmental live data + history summary" },
    { UCP_SVC_ENVIRONMENT, UCP_RES_ENV_SENSOR_CONFIG, 1U, SCHEMA_ACCESS_GET | SCHEMA_ACCESS_SET, 0U, 4U, "environment.sensor_config", "BME280 config bytes" },
    { UCP_SVC_HISTORY, UCP_RES_HISTORY_STORAGE,     1U, SCHEMA_ACCESS_GET | SCHEMA_ACCESS_CMD | SCHEMA_ACCESS_STREAM_OUT | SCHEMA_ACCESS_EVENT, UCP_TOPIC_HISTORY_READY, 5U, "history.storage", "History storage info + reset/export" },
    { UCP_SVC_HISTORY, UCP_RES_HISTORY_RAIN_STORAGE, 1U, SCHEMA_ACCESS_GET | SCHEMA_ACCESS_CMD | SCHEMA_ACCESS_STREAM_OUT | SCHEMA_ACCESS_EVENT, UCP_TOPIC_HISTORY_READY, 4U, "history.rain_storage", "Rain history stats + stream/reset" },
    { UCP_SVC_HISTORY, UCP_RES_HISTORY_ENV_STORAGE, 1U, SCHEMA_ACCESS_GET | SCHEMA_ACCESS_CMD | SCHEMA_ACCESS_STREAM_OUT | SCHEMA_ACCESS_EVENT, UCP_TOPIC_HISTORY_READY, 4U, "history.environment_storage", "Environmental history stats + stream/reset" },
    { UCP_SVC_RTC_TZ, UCP_RES_TZ_CONFIG,            1U, SCHEMA_ACCESS_GET | SCHEMA_ACCESS_SET | SCHEMA_ACCESS_EVENT, UCP_TOPIC_RTC_TZ_CHANGED, 9U, "rtc_tz.timezone_config", "Timezone/DST configuration" },
    { UCP_SVC_RTC_TZ, UCP_RES_RTC_TIME,             1U, SCHEMA_ACCESS_GET | SCHEMA_ACCESS_SET | SCHEMA_ACCESS_EVENT, UCP_TOPIC_RTC_TZ_CHANGED, 8U, "rtc_tz.rtc_time", "RTC date/time" },
    { UCP_SVC_COMPENSATION, UCP_RES_COMP_STATUS,    1U, SCHEMA_ACCESS_GET | SCHEMA_ACCESS_EVENT, UCP_TOPIC_COMP_STATUS, 4U, "compensation.status", "Per-channel compensation status bytes" },
    { UCP_SVC_COMPENSATION, UCP_RES_COMP_CHANNEL_CONFIG, 1U, SCHEMA_ACCESS_GET | SCHEMA_ACCESS_SET | SCHEMA_ACCESS_EVENT, UCP_TOPIC_COMP_CONFIG, 4U, "compensation.channel_config", "Per-channel rain/temp compensation config bytes" },
    { UCP_SVC_COMPENSATION, UCP_RES_COMP_RAIN_INTEGRATION, 1U, SCHEMA_ACCESS_GET | SCHEMA_ACCESS_EVENT, UCP_TOPIC_RAIN_INTEGRATION, 4U, "compensation.rain_integration", "Rain integration status bytes" },
    { UCP_SVC_ONBOARDING_RESET, UCP_RES_ONB_STATUS, 1U, SCHEMA_ACCESS_GET | SCHEMA_ACCESS_EVENT, UCP_TOPIC_ONBOARDING_STATUS, 4U, "onboarding_reset.onboarding_status", "Onboarding status bytes" },
    { UCP_SVC_ONBOARDING_RESET, UCP_RES_ONB_SYSTEM_OVERVIEW, 1U, SCHEMA_ACCESS_GET | SCHEMA_ACCESS_EVENT, UCP_TOPIC_ONBOARDING_STATUS, 4U, "onboarding_reset.system_overview", "Configuration system overview bytes" },
    { UCP_SVC_ONBOARDING_RESET, UCP_RES_ONB_CONFIG_STATUS, 1U, SCHEMA_ACCESS_GET | SCHEMA_ACCESS_CMD | SCHEMA_ACCESS_EVENT, UCP_TOPIC_CHANNEL_CFG_STATUS, 5U, "onboarding_reset.config_status", "Per-channel configuration status bytes + reset group cmd" },
    { UCP_SVC_ONBOARDING_RESET, UCP_RES_ONB_RESET_CONTROL, 1U, SCHEMA_ACCESS_GET | SCHEMA_ACCESS_CMD | SCHEMA_ACCESS_EVENT, UCP_TOPIC_RESET_PROGRESS, 5U, "onboarding_reset.reset_control", "Reset controller state/confirmation + commands" },
    { UCP_SVC_PACKS, UCP_RES_PACKS_STATS,           1U, SCHEMA_ACCESS_GET | SCHEMA_ACCESS_CMD | SCHEMA_ACCESS_STREAM_OUT | SCHEMA_ACCESS_STREAM_IN | SCHEMA_ACCESS_EVENT, UCP_TOPIC_PACKS_CHANGED, 10U, "packs.stats", "Pack storage + staging stats" },
    { UCP_SVC_PEER_POWER, UCP_RES_PEER_POWER_STATE, 1U, SCHEMA_ACCESS_GET | SCHEMA_ACCESS_CMD | SCHEMA_ACCESS_EVENT, UCP_TOPIC_PEER_POWER, 5U, "peer_power.state", "Peer power state (ESP32 EN)" },
    { UCP_SVC_PEER_POWER, UCP_RES_PEER_POWER_POLICY,1U, SCHEMA_ACCESS_GET | SCHEMA_ACCESS_SET | SCHEMA_ACCESS_EVENT, UCP_TOPIC_PEER_POWER, 2U, "peer_power.policy", "Peer power policy" },
    { UCP_SVC_DIAG, UCP_RES_DIAG_COUNTERS,          1U, SCHEMA_ACCESS_GET | SCHEMA_ACCESS_EVENT, UCP_TOPIC_DIAGNOSTICS, 21U, "diag.counters", "UCP transport/event counters" },
    { UCP_SVC_DIAG, UCP_RES_DIAG_STREAM_STATUS,     1U, SCHEMA_ACCESS_GET, 0U, 9U, "diag.stream_status", "Stream runtime state" },
    { UCP_SVC_DIAG, UCP_RES_DIAG_DOMAIN_STATUS,     1U, SCHEMA_ACCESS_GET, 0U, 17U, "diag.domain_status", "Resource versions and poll timestamps" },
    { UCP_SVC_DIAG, UCP_RES_DIAG_STORAGE_HEALTH,    1U, SCHEMA_ACCESS_GET, 0U, 6U, "diag.storage_health", "NVS/pack/history storage health summary" },
    { UCP_SVC_SCHEMA, UCP_RES_SCHEMA_IMPL_MASK,     1U, SCHEMA_ACCESS_GET, 0U, 2U, "schema.impl_mask", "Schema implementation capabilities" },
    { UCP_SVC_SCHEMA, UCP_RES_SCHEMA_PROTO_INFO,    1U, SCHEMA_ACCESS_GET, 0U, 8U, "schema.proto_info", "Protocol/schema capabilities" },
    { UCP_SVC_SCHEMA, UCP_RES_SCHEMA_SERVICE_MASK,  1U, SCHEMA_ACCESS_GET, 0U, 3U, "schema.service_mask", "Implemented service bitmask" },
    { UCP_SVC_SCHEMA, UCP_RES_SCHEMA_TOPIC_MASK,    1U, SCHEMA_ACCESS_GET, 0U, 3U, "schema.topic_mask", "Implemented topic bitmask" },
    { UCP_SVC_SCHEMA, UCP_RES_SCHEMA_STREAM_CAPS,   1U, SCHEMA_ACCESS_GET, 0U, 6U, "schema.stream_caps", "Global stream caps" },
    { UCP_SVC_SCHEMA, UCP_RES_SCHEMA_RESOURCE_DESC, 1U, SCHEMA_ACCESS_GET, 0U, 8U, "schema.resource_desc", "Lookup one resource descriptor" },
    { UCP_SVC_SCHEMA, UCP_RES_SCHEMA_FIELD_DESC,    1U, SCHEMA_ACCESS_GET, 0U, 7U, "schema.field_desc", "Field descriptor (best-effort)" },
    { UCP_SVC_SCHEMA, UCP_RES_SCHEMA_STREAM_PURPOSE_DESC, 1U, SCHEMA_ACCESS_GET, 0U, 7U, "schema.stream_purpose_desc", "Lookup stream purpose descriptor" },
};

static const struct ucp_schema_stream_purpose_desc g_schema_stream_purposes[] = {
    { STRM_PURPOSE_DIAG_SNAPSHOT,     BIT(STRM_DIR_OUTBOUND - 1), UCP_STREAM_BUF_MAX, "diag_snapshot", "Text diagnostic snapshot" },
    { STRM_PURPOSE_SCHEMA_TEXT,       BIT(STRM_DIR_OUTBOUND - 1), UCP_STREAM_BUF_MAX, "schema_text", "Short schema summary text" },
    { STRM_PURPOSE_UPLOAD_STAGING,    BIT(STRM_DIR_INBOUND - 1),  UCP_STREAM_BUF_MAX, "upload_staging", "Inbound staging blob upload" },
    { STRM_PURPOSE_HISTORY_PAGE,      BIT(STRM_DIR_OUTBOUND - 1), UCP_STREAM_BUF_MAX, "history_page", "Binary watering history page" },
    { STRM_PURPOSE_PACKS_LIST,        BIT(STRM_DIR_OUTBOUND - 1), UCP_STREAM_BUF_MAX, "packs_list", "Binary pack list page" },
    { STRM_PURPOSE_PACKS_PLANTS_LIST, BIT(STRM_DIR_OUTBOUND - 1), UCP_STREAM_BUF_MAX, "packs_plants_list", "Binary plant list page" },
    { STRM_PURPOSE_SCHEMA_CATALOG,    BIT(STRM_DIR_OUTBOUND - 1), UCP_STREAM_BUF_MAX, "schema_catalog", "Paged text catalog of implemented UCP resources" },
    { STRM_PURPOSE_RAIN_HISTORY_RANGE, BIT(STRM_DIR_OUTBOUND - 1), UCP_STREAM_BUF_MAX, "rain_history_range", "Binary rain history range export" },
    { STRM_PURPOSE_ENV_HISTORY_RANGE,  BIT(STRM_DIR_OUTBOUND - 1), UCP_STREAM_BUF_MAX, "env_history_range", "Binary environmental history range export" },
    { STRM_PURPOSE_DB_CATALOG,         BIT(STRM_DIR_OUTBOUND - 1), UCP_STREAM_BUF_MAX, "db_catalog", "Paged text catalog of plant/soil/irrigation DBs" },
    { STRM_PURPOSE_BULK_SNAPSHOT,      BIT(STRM_DIR_OUTBOUND - 1), UCP_STREAM_BUF_MAX, "bulk_snapshot", "Binary compact snapshot of key device state" },
    { STRM_PURPOSE_WATER_HISTORY_RANGE, BIT(STRM_DIR_OUTBOUND - 1), UCP_STREAM_BUF_MAX, "water_history_range", "Binary watering history aggregated range export" },
};

K_MUTEX_DEFINE(g_ucp_tx_mutex);
K_MUTEX_DEFINE(g_ucp_evt_mutex);
K_SEM_DEFINE(g_ucp_wake_sem, 0, 1);
/* Keep RAM pressure lower; broad notify polling is disabled by default and direct hooks still publish key changes. */
K_THREAD_STACK_DEFINE(g_ucp_stack, 12288);
static struct k_thread g_ucp_thread;
static void ota_reboot_work_handler(struct k_work *work);
static void ucp_notify_work_handler(struct k_work *work);
K_WORK_DELAYABLE_DEFINE(g_ota_reboot_work, ota_reboot_work_handler);
K_WORK_DELAYABLE_DEFINE(g_ucp_notify_work, ucp_notify_work_handler);

#if DT_NODE_HAS_STATUS(DT_ALIAS(esp32_en), okay)
static const struct gpio_dt_spec g_peer_power_gpio = GPIO_DT_SPEC_GET(DT_ALIAS(esp32_en), gpios);
#define UCP_HAVE_PEER_POWER_GPIO 1
#else
static const struct gpio_dt_spec g_peer_power_gpio = {0};
#define UCP_HAVE_PEER_POWER_GPIO 0
#endif

/* UCP emits a lot of metadata timestamps; reading RTC via timezone_get_unix_utc()
 * for every frame/event is expensive and can starve UART servicing during bursts.
 * Cache UTC seconds and interpolate with uptime between refreshes.
 */
static uint64_t now_utc_ms(void)
{
    uint32_t now_ms = k_uptime_get_32();
    uint32_t utc_s;
    uint32_t sample_ms;

    if (!g_utc_cache_valid || (now_ms - g_utc_cache_uptime_ms) >= UCP_UTC_CACHE_REFRESH_MS) {
        utc_s = timezone_get_unix_utc();
        if (utc_s != 0U) {
            g_utc_cache_unix_s = utc_s;
            g_utc_cache_uptime_ms = now_ms;
            g_utc_cache_valid = true;
        } else if (!g_utc_cache_valid) {
            return 0ULL;
        }
    }

    utc_s = g_utc_cache_unix_s;
    sample_ms = g_utc_cache_uptime_ms;
    return ((uint64_t)utc_s * 1000ULL) + (uint64_t)(now_ms - sample_ms);
}

static void ucp_utc_cache_invalidate(void)
{
    g_utc_cache_valid = false;
}
static bool topic_in_mask(uint32_t mask, uint16_t t) { return t >= 1U && t <= 32U && (mask & (1U << (t - 1U))); }
static bool topic_subscribed(uint16_t t)
{
    uint32_t mask = (g_topic_scope_mask_override != 0U) ? g_topic_scope_mask_override : g_sub_mask;
    return topic_in_mask(mask, t);
}
static bool topic_subscribed_global(uint16_t t)
{
    return topic_in_mask(g_sub_mask, t);
}
static uint32_t topic_min_interval_ms(uint16_t topic)
{
    switch (topic) {
    case UCP_TOPIC_FLOW_LIVE:
    case UCP_TOPIC_FLOW_CALIBRATION:
    case UCP_TOPIC_RAIN_LIVE:
    case UCP_TOPIC_ENV_LIVE:
        return UCP_EVENT_MIN_INTERVAL_SENSOR_MS;
    case UCP_TOPIC_CHANNEL_STATE:
    case UCP_TOPIC_CHANNEL_CONFIG:
    case UCP_TOPIC_SCHEDULE_CONFIG:
    case UCP_TOPIC_TASK_CURRENT:
    case UCP_TOPIC_PACKS_CHANGED:
        return UCP_EVENT_MIN_INTERVAL_CONFIG_MS;
    default:
        return UCP_EVENT_MIN_INTERVAL_DEFAULT_MS;
    }
}

static bool ucp_pack_stats_cache_get(pack_storage_stats_t *out)
{
    if (!g_pack_stats_cache_valid) {
        return false;
    }
    if (out) {
        *out = g_pack_stats_cache;
    }
    return true;
}

static bool ucp_pack_stats_cache_is_stale(uint32_t now_ms, uint32_t ttl_ms)
{
    if (!g_pack_stats_cache_valid) {
        return true;
    }
    return (now_ms - g_pack_stats_cache_ms) > ttl_ms;
}

static bool ucp_env_hist_stats_cache_get(env_history_stats_t *out)
{
    if (!g_env_hist_stats_cache_valid) {
        return false;
    }
    if (out) {
        *out = g_env_hist_stats_cache;
    }
    return true;
}

static bool ucp_env_hist_stats_cache_is_stale(uint32_t now_ms, uint32_t ttl_ms)
{
    if (!g_env_hist_stats_cache_valid) {
        return true;
    }
    return (now_ms - g_env_hist_stats_cache_ms) > ttl_ms;
}

static bool ucp_env_hist_stats_cache_refresh(void)
{
    /* Lightweight: read only in-memory header state via history_flash_get_stats().
     * Avoids env_history_get_stats() which calls env_history_get_oldest_entry()
     * 3× (actual LittleFS file I/O) — too heavy for the 12 KB UCP stack. */
    static history_flash_stats_t fs; /* static to keep UCP stack safe */
    int rc = history_flash_get_stats(&fs);
    if (rc != 0) {
        printk("env hist cache refresh: history_flash_get_stats failed %d\n", rc);
        return false;
    }
    env_history_stats_t st = {0};
    st.hourly_entries_used       = fs.env_hourly.entry_count;
    st.daily_entries_used        = fs.env_daily.entry_count;
    st.monthly_entries_used      = fs.env_monthly.entry_count;
    st.oldest_hourly_timestamp   = fs.env_hourly.oldest_timestamp;
    st.oldest_daily_timestamp    = fs.env_daily.oldest_timestamp;
    st.oldest_monthly_timestamp  = fs.env_monthly.oldest_timestamp;
    st.total_storage_bytes       = fs.env_hourly.file_size_bytes +
                                   fs.env_daily.file_size_bytes +
                                   fs.env_monthly.file_size_bytes;
    st.storage_utilization_pct   = 0; /* skip heavy computation */
    g_env_hist_stats_cache       = st;
    g_env_hist_stats_cache_valid = true;
    g_env_hist_stats_cache_ms    = k_uptime_get_32();
    printk("env hist cache OK: h=%u d=%u m=%u bytes=%u\n",
           st.hourly_entries_used, st.daily_entries_used,
           st.monthly_entries_used, st.total_storage_bytes);
    return true;
}

static void ucp_env_hist_stats_cache_invalidate(void)
{
    g_env_hist_stats_cache_valid = false;
}

static void ucp_env_hist_stats_cache_request_refresh(void)
{
    if (!g_ready) {
        return;
    }
    (void)atomic_set(&g_env_hist_stats_refresh_pending, 1);
    k_sem_give(&g_ucp_wake_sem);
    if (g_poll_notifies_enabled) {
        (void)k_work_reschedule(&g_ucp_notify_work, K_MSEC(5));
    }
}

/* ---- Rain history stats cache (mirrors env hist pattern) ---- */
static bool ucp_rain_hist_stats_cache_get(rain_history_stats_t *out)
{
    if (!g_rain_hist_stats_cache_valid) {
        return false;
    }
    if (out) {
        *out = g_rain_hist_stats_cache;
    }
    return true;
}

static bool ucp_rain_hist_stats_cache_is_stale(uint32_t now_ms, uint32_t ttl_ms)
{
    if (!g_rain_hist_stats_cache_valid) {
        return true;
    }
    return (now_ms - g_rain_hist_stats_cache_ms) > ttl_ms;
}

static bool ucp_rain_hist_stats_cache_refresh(void)
{
    /* Lightweight: call history_flash_get_stats() directly with a static
     * buffer to avoid putting ~96 B history_flash_stats_t on the 12 KB
     * UCP stack and to skip rain_history_get_stats() wrapper overhead. */
    static history_flash_stats_t fs; /* static — not on UCP stack */
    int rc = history_flash_get_stats(&fs);
    if (rc != 0) {
        printk("rain hist cache refresh: history_flash_get_stats failed %d\n", rc);
        return false;
    }
    rain_history_stats_t st = {0};
    st.hourly_entries      = fs.rain_hourly.entry_count;
    st.daily_entries       = fs.rain_daily.entry_count;
    st.oldest_hourly       = fs.rain_hourly.oldest_timestamp;
    st.newest_hourly       = fs.rain_hourly.newest_timestamp;
    st.oldest_daily        = fs.rain_daily.oldest_timestamp;
    st.newest_daily        = fs.rain_daily.newest_timestamp;
    st.total_storage_bytes = fs.rain_hourly.file_size_bytes +
                             fs.rain_daily.file_size_bytes;
    g_rain_hist_stats_cache       = st;
    g_rain_hist_stats_cache_valid = true;
    g_rain_hist_stats_cache_ms    = k_uptime_get_32();
    printk("rain hist cache OK: h=%u d=%u bytes=%u\n",
           st.hourly_entries, st.daily_entries, st.total_storage_bytes);
    return true;
}

static void ucp_rain_hist_stats_cache_request_refresh(void)
{
    if (!g_ready) {
        return;
    }
    (void)atomic_set(&g_rain_hist_stats_refresh_pending, 1);
    k_sem_give(&g_ucp_wake_sem);
    if (g_poll_notifies_enabled) {
        (void)k_work_reschedule(&g_ucp_notify_work, K_MSEC(5));
    }
}

static bool ucp_pack_stats_cache_refresh(void)
{
    pack_storage_stats_t st = {0};

    if (!pack_storage_is_ready()) {
        g_pack_stats_cache_valid = false;
        return false;
    }
    if (pack_storage_get_stats(&st) != PACK_RESULT_SUCCESS) {
        return false;
    }
    g_pack_stats_cache = st;
    g_pack_stats_cache_valid = true;
    g_pack_stats_cache_ms = k_uptime_get_32();
    g_last_pack_change_counter = st.change_counter;
    return true;
}

static void ucp_pack_stats_cache_invalidate(void)
{
    g_pack_stats_cache_valid = false;
}

static void ucp_pack_stats_cache_request_refresh(void)
{
    if (!g_ready || !pack_storage_is_ready()) {
        return;
    }
    (void)atomic_set(&g_pack_stats_refresh_pending, 1);
    k_sem_give(&g_ucp_wake_sem);
    if (g_poll_notifies_enabled) {
        (void)k_work_reschedule(&g_ucp_notify_work, K_MSEC(5));
    }
}

static bool topic_rate_limit_allows_tx(uint16_t topic)
{
    uint32_t now;
    uint32_t min_ms;
    if (topic < 1U || topic > 32U) {
        return true;
    }
    now = k_uptime_get_32();
    min_ms = topic_min_interval_ms(topic);
    if (min_ms == 0U) {
        g_last_topic_emit_ms[topic] = now;
        return true;
    }
    if ((now - g_last_topic_emit_ms[topic]) < min_ms) {
        g_diag.event_coalesced++;
        g_diag.event_rate_limited++;
        return false;
    }
    g_last_topic_emit_ms[topic] = now;
    return true;
}

static const struct ucp_schema_resource_desc *schema_find_resource_desc(uint16_t svc, uint16_t res)
{
    for (size_t i = 0U; i < (sizeof(g_schema_resources) / sizeof(g_schema_resources[0])); i++) {
        if (g_schema_resources[i].svc == svc && g_schema_resources[i].res == res) {
            return &g_schema_resources[i];
        }
    }
    return NULL;
}

static const struct ucp_schema_stream_purpose_desc *schema_find_stream_purpose_desc(uint8_t purpose)
{
    for (size_t i = 0U; i < (sizeof(g_schema_stream_purposes) / sizeof(g_schema_stream_purposes[0])); i++) {
        if (g_schema_stream_purposes[i].purpose == purpose) {
            return &g_schema_stream_purposes[i];
        }
    }
    return NULL;
}

static bool task_channel_id_from_ptr(const watering_channel_t *ch, uint8_t *out_id)
{
    if (!ch || !out_id) return false;
    for (uint8_t i = 0; i < WATERING_CHANNELS_COUNT; i++) {
        watering_channel_t *cand = NULL;
        if (watering_get_channel(i, &cand) != WATERING_SUCCESS || cand == NULL) continue;
        if (cand == ch) {
            *out_id = i;
            return true;
        }
    }
    return false;
}

static bool ucp_tlvw_put_f32(struct ucp_tlv_writer *w, uint8_t field_id, float v)
{
    return ucp_tlvw_put_raw(w, field_id, UCP_TLV_F32, &v, sizeof(v));
}

static bool ucp_tlv_read_f32(const struct ucp_tlv_elem *e, float *out)
{
    if (!e || !out || e->len != sizeof(float)) return false;
    memcpy(out, e->value, sizeof(float));
    return true;
}

#if defined(CONFIG_THREAD_STACK_INFO) && defined(CONFIG_INIT_STACKS)
static void ucp_dbg_log_stack_watermark(void)
{
    size_t unused = 0U;
    if (k_thread_stack_space_get(&g_ucp_thread, &unused) == 0) {
        printk("UCP UART dbg: ucp_thread_stack_unused=%uB\n", (unsigned)unused);
    }
}
#else
static void ucp_dbg_log_stack_watermark(void) {}
#endif

static uint16_t ucp_status_from_werr(watering_error_t rc)
{
    switch (rc) {
    case WATERING_SUCCESS: return UCP_STATUS_OK;
    case WATERING_ERROR_INVALID_PARAM: return UCP_STATUS_INVALID_ARG;
    case WATERING_ERROR_BUSY:
    case WATERING_ERROR_QUEUE_FULL: return UCP_STATUS_BUSY;
    case WATERING_ERROR_TIMEOUT: return UCP_STATUS_TIMEOUT;
    case WATERING_ERROR_STORAGE: return UCP_STATUS_INTERNAL;
    case WATERING_ERROR_HARDWARE:
    case WATERING_ERROR_RTC_FAILURE: return UCP_STATUS_HW_FAULT;
    default: return UCP_STATUS_INTERNAL;
    }
}

static uint16_t ucp_status_from_pack_result(pack_result_t rc)
{
    switch (rc) {
    case PACK_RESULT_SUCCESS:
    case PACK_RESULT_UPDATED:
    case PACK_RESULT_ALREADY_CURRENT:
        return UCP_STATUS_OK;
    case PACK_RESULT_INVALID_DATA:
    case PACK_RESULT_INVALID_VERSION:
        return UCP_STATUS_INVALID_ARG;
    case PACK_RESULT_STORAGE_FULL:
        return UCP_STATUS_BUSY;
    case PACK_RESULT_NOT_FOUND:
        return UCP_STATUS_NOT_FOUND;
    case PACK_RESULT_CRC_MISMATCH:
        return UCP_STATUS_CHECKSUM_FAIL;
    case PACK_RESULT_IO_ERROR:
    default:
        return UCP_STATUS_INTERNAL;
    }
}

static bool parse_channel_id_required(const uint8_t *p, uint16_t len, uint8_t *channel_id)
{
    struct ucp_tlv_elem e;
    size_t off = 0U;
    if (!channel_id) return false;
    while (ucp_tlv_next(p, len, &off, &e)) {
        if (e.field_id == RES_CH_ID) {
            return ucp_tlv_read_u8(&e, channel_id) && (*channel_id < WATERING_CHANNELS_COUNT);
        }
    }
    return false;
}

static bool parse_channel_selector_optional(const uint8_t *p, uint16_t len, uint8_t *channel_id, uint8_t default_id, bool allow_global_ff)
{
    struct ucp_tlv_elem e;
    size_t off = 0U;
    if (!channel_id) return false;
    *channel_id = default_id;
    while (ucp_tlv_next(p, len, &off, &e)) {
        if (e.field_id == RES_CH_ID || e.field_id == Q_CHANNEL_ID) {
            uint8_t v;
            if (!ucp_tlv_read_u8(&e, &v)) return false;
            if (allow_global_ff && v == 0xFFU) {
                *channel_id = v;
                return true;
            }
            if (v >= WATERING_CHANNELS_COUNT) return false;
            *channel_id = v;
            return true;
        }
    }
    return true;
}

static bool parse_raw_bytes_req(const uint8_t *p, uint16_t len,
                                const uint8_t **raw, uint16_t *raw_len,
                                uint8_t *selector_opt, bool *has_selector)
{
    struct ucp_tlv_elem e;
    size_t off = 0U;
    bool got_raw = false;

    if (!raw || !raw_len) return false;
    *raw = NULL;
    *raw_len = 0U;
    if (selector_opt) *selector_opt = 0xFFU;
    if (has_selector) *has_selector = false;

    while (ucp_tlv_next(p, len, &off, &e)) {
        if (e.field_id == RAW_BYTES_F) {
            *raw = e.value;
            *raw_len = e.len;
            got_raw = true;
            continue;
        }
        if ((e.field_id == RAW_SELECTOR_F || e.field_id == RES_CH_ID || e.field_id == Q_CHANNEL_ID) && selector_opt) {
            uint8_t v;
            if (!ucp_tlv_read_u8(&e, &v)) return false;
            *selector_opt = v;
            if (has_selector) *has_selector = true;
            continue;
        }
    }
    return got_raw;
}

static int tlv_raw_bytes_crc32c(const uint8_t *p, uint16_t len, uint32_t *sig_out)
{
    struct ucp_tlv_elem e;
    size_t off = 0U;
    if (!p || !sig_out) return -EINVAL;
    while (ucp_tlv_next(p, len, &off, &e)) {
        if (e.field_id == RAW_BYTES_F) {
            *sig_out = ucp_crc32c(e.value, e.len);
            return 0;
        }
    }
    return -ENOENT;
}

static uint16_t ucp_status_from_reset_status(reset_status_t st)
{
    switch (st) {
    case RESET_STATUS_SUCCESS: return UCP_STATUS_OK;
    case RESET_STATUS_INVALID_TYPE:
    case RESET_STATUS_INVALID_CHANNEL:
        return UCP_STATUS_INVALID_ARG;
    case RESET_STATUS_INVALID_CODE:
        return UCP_STATUS_CONFIRMATION_REQD;
    case RESET_STATUS_CODE_EXPIRED:
        return UCP_STATUS_PRECONDITION_FAILED;
    case RESET_STATUS_STORAGE_ERROR:
    case RESET_STATUS_SYSTEM_ERROR:
    default:
        return UCP_STATUS_INTERNAL;
    }
}

static int save_channel_after_change(uint8_t channel_id)
{
    watering_error_t rc = watering_save_channel_config_priority(channel_id, true);
    return (rc == WATERING_SUCCESS) ? 0 : -EIO;
}

static int peer_power_apply_hw(bool on)
{
    int rc;
    if (!UCP_HAVE_PEER_POWER_GPIO) return 0;
    if (!device_is_ready(g_peer_power_gpio.port)) return -ENODEV;
    rc = gpio_pin_set_dt(&g_peer_power_gpio, on ? 1 : 0);
    return rc;
}

static int send_frame(uint8_t mt, uint16_t svc, uint16_t mid, uint16_t st, const uint8_t *pl, uint16_t pl_len, uint32_t req)
{
    struct ucp_frame_header_v1 h;
    int enc_len;
    uint32_t crc;
    if (!g_uart || !device_is_ready(g_uart)) return -ENODEV;
    if (pl_len > UCP_MAX_PAYLOAD_V1) return -EMSGSIZE;
    memset(&h, 0, sizeof(h));
    h.proto_version = UCP_PROTO_V1; h.msg_type = mt; h.header_len = UCP_HEADER_LEN_V1;
    h.service_id = svc; h.method_or_topic_id = mid; h.status_code = st; h.payload_len = pl_len;
    h.request_id = req; h.seq_tx = g_tx_seq++;
    memcpy(g_tx_decoded, &h, sizeof(h));
    if (pl && pl_len) memcpy(&g_tx_decoded[sizeof(h)], pl, pl_len);
    crc = ucp_crc32c(g_tx_decoded, sizeof(h) + pl_len);
    ucp_tlv_wr_u32_le(&g_tx_decoded[sizeof(h) + pl_len], crc);
    enc_len = ucp_cobs_encode(g_tx_decoded, sizeof(h) + pl_len + 4U, g_tx_encoded, sizeof(g_tx_encoded) - 1U);
    if (enc_len < 0) return enc_len;
    g_tx_encoded[enc_len++] = 0U;
    k_mutex_lock(&g_ucp_tx_mutex, K_FOREVER);
    for (int i = 0; i < enc_len; i++) uart_poll_out(g_uart, g_tx_encoded[i]);
    k_mutex_unlock(&g_ucp_tx_mutex);
    g_diag.tx_frames++;
    if (mt == UCP_MSG_ERROR) g_diag.tx_error_frames++;
    if (mt == UCP_MSG_EVENT_CHANGE || mt == UCP_MSG_EVENT_HEARTBEAT || mt == UCP_MSG_EVENT_OVERFLOW) g_diag.event_frames++;
    if (mt == UCP_MSG_EVENT_CHANGE || mt == UCP_MSG_EVENT_HEARTBEAT) g_last_evt_ms = k_uptime_get_32();
    return 0;
}

static int send_tlv(uint8_t mt, uint16_t svc, uint16_t mid, uint16_t st, struct ucp_tlv_writer *w, uint32_t req)
{
    return send_frame(mt, svc, mid, st, w ? w->buf : NULL, (uint16_t)(w ? w->len : 0U), req);
}

static int send_err(uint16_t st, uint32_t req, const char *msg)
{
    uint8_t b[96]; struct ucp_tlv_writer w; ucp_tlvw_init(&w, b, sizeof(b));
    if (msg) (void)ucp_tlvw_put_str(&w, 1U, UCP_TLV_UTF8, msg);
    return send_tlv(UCP_MSG_ERROR, 0U, 0U, st, &w, req);
}

static uint64_t replay_first_seq(void)
{
    uint16_t oldest_idx;
    if (g_replay_count == 0U) return 0ULL;
    oldest_idx = (uint16_t)((g_replay_head + UCP_REPLAY_DEPTH - g_replay_count) % UCP_REPLAY_DEPTH);
    return g_replay[oldest_idx].event_seq;
}

static uint64_t replay_last_seq(void)
{
    uint16_t last_idx;
    if (g_replay_count == 0U) return (g_evt_seq == 0ULL) ? 0ULL : (g_evt_seq - 1ULL);
    last_idx = (uint16_t)((g_replay_head + UCP_REPLAY_DEPTH - 1U) % UCP_REPLAY_DEPTH);
    return g_replay[last_idx].event_seq;
}

static void replay_store(uint16_t topic, uint16_t svc, uint16_t res, uint32_t instance_id,
                         uint32_t ver, ucp_origin_t origin,
                         uint64_t event_seq, uint64_t ts_utc_ms, const uint8_t *delta, size_t delta_len)
{
    struct ucp_replay_evt *e = &g_replay[g_replay_head];
    e->valid = true;
    e->topic = topic;
    e->svc = svc;
    e->res = res;
    e->instance_id = instance_id;
    e->ver = ver;
    e->origin = (uint8_t)origin;
    e->event_seq = event_seq;
    e->ts_utc_ms = ts_utc_ms;
    e->delta_len = (uint16_t)delta_len;
    if (delta_len > 0U && delta != NULL) memcpy(e->delta, delta, delta_len);
    g_replay_head = (uint16_t)((g_replay_head + 1U) % UCP_REPLAY_DEPTH);
    if (g_replay_count < UCP_REPLAY_DEPTH) g_replay_count++;
}

static int send_change_event_payload(uint16_t topic, uint16_t svc, uint16_t res, uint32_t instance_id,
                                     uint32_t ver, ucp_origin_t origin,
                                     uint64_t event_seq, uint64_t ts_utc_ms, const uint8_t *delta, size_t delta_len)
{
    uint8_t b[UCP_MAX_PAYLOAD_V1]; struct ucp_tlv_writer w;
    if (delta_len > UCP_MAX_EVT_DELTA) return -EMSGSIZE;
    ucp_tlvw_init(&w, b, sizeof(b));
    (void)ucp_tlvw_put_u64(&w, UCP_F_BOOT_ID, UCP_TLV_U64, g_boot_id);
    (void)ucp_tlvw_put_u64(&w, UCP_F_EVENT_SEQ, UCP_TLV_U64, event_seq);
    (void)ucp_tlvw_put_u16(&w, UCP_F_TOPIC_ID, UCP_TLV_U16, topic);
    (void)ucp_tlvw_put_u16(&w, UCP_F_SERVICE_ID, UCP_TLV_U16, svc);
    (void)ucp_tlvw_put_u16(&w, UCP_F_RESOURCE_ID, UCP_TLV_U16, res);
    (void)ucp_tlvw_put_u32(&w, UCP_F_INSTANCE_ID, UCP_TLV_U32, instance_id);
    (void)ucp_tlvw_put_u32(&w, UCP_F_RESOURCE_VERSION, UCP_TLV_U32, ver);
    (void)ucp_tlvw_put_u8(&w, UCP_F_ORIGIN, UCP_TLV_U8, (uint8_t)origin);
    (void)ucp_tlvw_put_u64(&w, UCP_F_TIMESTAMP_UTC_MS, UCP_TLV_U64, ts_utc_ms);
    (void)ucp_tlvw_put_bytes(&w, UCP_F_DELTA_TLV, UCP_TLV_BYTES, delta, (uint16_t)delta_len);
    return send_tlv(UCP_MSG_EVENT_CHANGE, svc, topic, UCP_STATUS_OK, &w, 0U);
}

static int send_event_overflow(uint64_t first_avail_seq, uint64_t last_seq, uint64_t dropped_count)
{
    uint8_t b[96];
    struct ucp_tlv_writer w;
    ucp_tlvw_init(&w, b, sizeof(b));
    (void)ucp_tlvw_put_u64(&w, UCP_F_BOOT_ID, UCP_TLV_U64, g_boot_id);
    (void)ucp_tlvw_put_u64(&w, UCP_F_LAST_EVENT_SEQ, UCP_TLV_U64, last_seq);
    (void)ucp_tlvw_put_u64(&w, UCP_F_FIRST_AVAILABLE_EVENT_SEQ, UCP_TLV_U64, first_avail_seq);
    (void)ucp_tlvw_put_u64(&w, UCP_F_DROPPED_COUNT, UCP_TLV_U64, dropped_count);
    (void)ucp_tlvw_put_u8(&w, UCP_F_RESYNC_REQUIRED, UCP_TLV_BOOL, 1U);
    return send_tlv(UCP_MSG_EVENT_OVERFLOW, 0U, 0U, UCP_STATUS_OK, &w, 0U);
}

static int replay_send_since(uint32_t topic_mask, uint64_t since_event_seq)
{
    uint16_t oldest_idx;
    for (uint16_t i = 0U; i < g_replay_count; i++) {
        uint16_t idx;
        const struct ucp_replay_evt *e;
        oldest_idx = (uint16_t)((g_replay_head + UCP_REPLAY_DEPTH - g_replay_count) % UCP_REPLAY_DEPTH);
        idx = (uint16_t)((oldest_idx + i) % UCP_REPLAY_DEPTH);
        e = &g_replay[idx];
        if (!e->valid) continue;
        if (e->event_seq <= since_event_seq) continue;
        if (!topic_in_mask(topic_mask, e->topic)) continue;
        (void)send_change_event_payload(e->topic, e->svc, e->res, e->instance_id, e->ver, (ucp_origin_t)e->origin,
                                        e->event_seq, e->ts_utc_ms, e->delta, e->delta_len);
    }
    return 0;
}

static int emit_change_instance(uint16_t topic, uint16_t svc, uint16_t res, uint32_t instance_id, uint32_t ver,
                                ucp_origin_t origin, const uint8_t *delta, size_t delta_len)
{
    uint64_t event_seq;
    uint64_t ts_utc_ms;
    int rc = 0;
    if (delta_len > UCP_MAX_EVT_DELTA) return -EMSGSIZE;
    k_mutex_lock(&g_ucp_evt_mutex, K_FOREVER);
    event_seq = g_evt_seq++;
    ts_utc_ms = now_utc_ms();
    replay_store(topic, svc, res, instance_id, ver, origin, event_seq, ts_utc_ms, delta, delta_len);
    if (!g_session || !topic_subscribed(topic)) {
        k_mutex_unlock(&g_ucp_evt_mutex);
        return 0;
    }
    if (!topic_rate_limit_allows_tx(topic)) {
        k_mutex_unlock(&g_ucp_evt_mutex);
        return 0;
    }
    rc = send_change_event_payload(topic, svc, res, instance_id, ver, origin, event_seq, ts_utc_ms, delta, delta_len);
    k_mutex_unlock(&g_ucp_evt_mutex);
    return rc;
}

static int emit_change(uint16_t topic, uint16_t svc, uint16_t res, uint32_t ver, ucp_origin_t origin,
                       const uint8_t *delta, size_t delta_len)
{
    return emit_change_instance(topic, svc, res, 0U, ver, origin, delta, delta_len);
}

static bool tz_valid(const timezone_config_t *c)
{
    if (!c) return false;
    if (c->utc_offset_minutes < -720 || c->utc_offset_minutes > 840) return false;
    if (c->dst_enabled > 1U) return false;
    if (c->dst_offset_minutes < -120 || c->dst_offset_minutes > 120) return false;
    if (!c->dst_enabled) return true;
    if (c->dst_start_month < 1U || c->dst_start_month > 12U || c->dst_end_month < 1U || c->dst_end_month > 12U) return false;
    if (c->dst_start_week < 1U || c->dst_start_week > 5U || c->dst_end_week < 1U || c->dst_end_week > 5U) return false;
    if (c->dst_start_dow > 6U || c->dst_end_dow > 6U) return false;
    return true;
}

static int handle_frame(const struct ucp_frame_header_v1 *h, const uint8_t *p);
static void poll_rx(void);
static void maybe_heartbeat(void);
static void maybe_stream_timeouts(void);
static void maybe_task_status_event(void);
static void maybe_domain_events(void);
static uint32_t ucp_thread_wait_timeout_ms(uint32_t now_ms);
static bool peer_power_inhibit(void);
static int handle_stream_open(uint16_t svc, uint16_t mid, const uint8_t *p, uint16_t len, uint32_t req);
static int handle_stream_abort(const uint8_t *p, uint16_t len, uint32_t req);
static int handle_stream_data(const uint8_t *p, uint16_t len, uint32_t req);
static int handle_stream_end(const uint8_t *p, uint16_t len, uint32_t req);

static int ota_swap_type_safe(void)
{
#ifdef CONFIG_BOOTLOADER_MCUBOOT
    int sw = mcuboot_swap_type();
    return sw;
#else
    return -ENOTSUP;
#endif
}

static bool ota_img_confirmed_safe(void)
{
#ifdef CONFIG_BOOTLOADER_MCUBOOT
    return boot_is_img_confirmed();
#else
    return true;
#endif
}

static uint32_t ota_confirm_token_remaining_ms(void)
{
    uint32_t now;
    if (g_ota_confirm_token == 0U || g_ota_confirm_deadline_ms == 0U) return 0U;
    now = k_uptime_get_32();
    if ((int32_t)(g_ota_confirm_deadline_ms - now) <= 0) {
        g_ota_confirm_token = 0U;
        g_ota_confirm_deadline_ms = 0U;
        return 0U;
    }
    return g_ota_confirm_deadline_ms - now;
}

static uint32_t ota_confirm_token_generate(void)
{
    uint32_t token = sys_rand32_get();
    if (token == 0U) token = 1U;
    g_ota_confirm_token = token;
    g_ota_confirm_deadline_ms = k_uptime_get_32() + UCP_OTA_CONFIRM_TTL_MS;
    return token;
}

static bool ota_confirm_token_matches(uint32_t token)
{
    if (token == 0U) return false;
    if (ota_confirm_token_remaining_ms() == 0U) return false;
    if (token != g_ota_confirm_token) return false;
    g_ota_confirm_token = 0U;
    g_ota_confirm_deadline_ms = 0U;
    return true;
}

static int emit_ota_transition(uint8_t state, bool pending, ucp_origin_t origin, uint8_t rollback_observed)
{
    uint8_t delta_b[64];
    struct ucp_tlv_writer delta;
    ucp_tlvw_init(&delta, delta_b, sizeof(delta_b));
    (void)ucp_tlvw_put_u8(&delta, OTA_STATE, UCP_TLV_U8, state);
    (void)ucp_tlvw_put_u8(&delta, OTA_PENDING, UCP_TLV_BOOL, pending ? 1U : 0U);
    (void)ucp_tlvw_put_u8(&delta, OTA_CONFIRM, UCP_TLV_BOOL, ota_img_confirmed_safe() ? 1U : 0U);
    (void)ucp_tlvw_put_i32(&delta, OTA_SWAP, UCP_TLV_I32, ota_swap_type_safe());
    (void)ucp_tlvw_put_u8(&delta, OTA_ROLLBACK_OBS, UCP_TLV_BOOL, rollback_observed ? 1U : 0U);
    return emit_change(UCP_TOPIC_OTA_TRANSITION, UCP_SVC_OTA, UCP_CMD_OTA_ENTER_RECOVERY,
                       ++g_ver_ota, origin, delta.buf, delta.len);
}

static void ota_reboot_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);

#ifdef CONFIG_RETENTION_BOOT_MODE
    int rc = bootmode_set(BOOT_MODE_TYPE_BOOTLOADER);
    if (rc != 0) {
        g_ota_reboot_pending = false;
        (void)emit_ota_transition(OTA_STATE_REBOOT_FAILED, false, UCP_ORIGIN_LOCAL_FW, 0U);
        printk("UCP OTA: bootmode_set failed (%d)\n", rc);
        return;
    }
#else
    g_ota_reboot_pending = false;
    (void)emit_ota_transition(OTA_STATE_REBOOT_FAILED, false, UCP_ORIGIN_LOCAL_FW, 0U);
    printk("UCP OTA: RETENTION_BOOT_MODE not enabled\n");
    return;
#endif

    /* If reboot returns, clear the pending state to avoid a permanent lock. */
    sys_reboot(SYS_REBOOT_COLD);
    g_ota_reboot_pending = false;
}

static int build_system(struct ucp_tlv_writer *w)
{
    watering_status_t ws = WATERING_STATUS_OK;
    watering_state_t st = WATERING_STATE_IDLE;
    power_mode_t pm = POWER_MODE_NORMAL;
    (void)watering_get_status(&ws);
    (void)watering_get_state(&st);
    (void)watering_get_power_mode(&pm);
    return (ucp_tlvw_put_u32(w, SYS_UPTIME, UCP_TLV_U32, k_uptime_get_32()) &&
            ucp_tlvw_put_u32(w, SYS_UTC, UCP_TLV_U32, timezone_get_unix_utc()) &&
            ucp_tlvw_put_u8(w, SYS_RTC, UCP_TLV_BOOL, rtc_is_available() ? 1U : 0U) &&
            ucp_tlvw_put_str(w, SYS_FW, UCP_TLV_UTF8, APP_VERSION_EXTENDED_STRING) &&
            ucp_tlvw_put_u64(w, SYS_BOOT, UCP_TLV_U64, g_boot_id) &&
            ucp_tlvw_put_u8(w, SYS_W_STATUS, UCP_TLV_U8, (uint8_t)ws) &&
            ucp_tlvw_put_u8(w, SYS_W_STATE, UCP_TLV_U8, (uint8_t)st) &&
            ucp_tlvw_put_u8(w, SYS_PWR_MODE, UCP_TLV_U8, (uint8_t)pm) &&
            ucp_tlvw_put_u32(w, UCP_F_RESOURCE_VERSION, UCP_TLV_U32, g_ver_sys) &&
            ucp_tlvw_put_u64(w, UCP_F_TIMESTAMP_UTC_MS, UCP_TLV_U64, now_utc_ms())) ? 0 : -ENOSPC;
}

static int build_tz(struct ucp_tlv_writer *w, const timezone_config_t *c)
{
    return (ucp_tlvw_put_i32(w, TZ_UTC_OFF, UCP_TLV_I32, (int32_t)c->utc_offset_minutes) &&
            ucp_tlvw_put_u8(w, TZ_DST_EN, UCP_TLV_BOOL, c->dst_enabled) &&
            ucp_tlvw_put_u8(w, TZ_DST_SM, UCP_TLV_U8, c->dst_start_month) &&
            ucp_tlvw_put_u8(w, TZ_DST_SW, UCP_TLV_U8, c->dst_start_week) &&
            ucp_tlvw_put_u8(w, TZ_DST_SD, UCP_TLV_U8, c->dst_start_dow) &&
            ucp_tlvw_put_u8(w, TZ_DST_EM, UCP_TLV_U8, c->dst_end_month) &&
            ucp_tlvw_put_u8(w, TZ_DST_EW, UCP_TLV_U8, c->dst_end_week) &&
            ucp_tlvw_put_u8(w, TZ_DST_ED, UCP_TLV_U8, c->dst_end_dow) &&
            ucp_tlvw_put_i32(w, TZ_DST_OFF, UCP_TLV_I32, (int32_t)c->dst_offset_minutes) &&
            ucp_tlvw_put_u32(w, UCP_F_RESOURCE_VERSION, UCP_TLV_U32, g_ver_tz) &&
            ucp_tlvw_put_u64(w, UCP_F_TIMESTAMP_UTC_MS, UCP_TLV_U64, now_utc_ms())) ? 0 : -ENOSPC;
}

static int build_rtc(struct ucp_tlv_writer *w, const rtc_datetime_t *dt)
{
    return (ucp_tlvw_put_u8(w, RTC_SEC, UCP_TLV_U8, dt->second) &&
            ucp_tlvw_put_u8(w, RTC_MIN, UCP_TLV_U8, dt->minute) &&
            ucp_tlvw_put_u8(w, RTC_HOUR, UCP_TLV_U8, dt->hour) &&
            ucp_tlvw_put_u8(w, RTC_DAY, UCP_TLV_U8, dt->day) &&
            ucp_tlvw_put_u8(w, RTC_MONTH, UCP_TLV_U8, dt->month) &&
            ucp_tlvw_put_u16(w, RTC_YEAR, UCP_TLV_U16, dt->year) &&
            ucp_tlvw_put_u8(w, RTC_DOW, UCP_TLV_U8, dt->day_of_week) &&
            ucp_tlvw_put_u32(w, RTC_UNIX_UTC, UCP_TLV_U32, timezone_rtc_to_unix_utc(dt)) &&
            ucp_tlvw_put_u32(w, UCP_F_RESOURCE_VERSION, UCP_TLV_U32, g_ver_rtc) &&
            ucp_tlvw_put_u64(w, UCP_F_TIMESTAMP_UTC_MS, UCP_TLV_U64, now_utc_ms())) ? 0 : -ENOSPC;
}

static int collect_channel_state_snapshot(uint8_t channel_id, struct ucp_channel_state_snapshot *s)
{
    watering_channel_t *ch = NULL;
    if (!s) return -EINVAL;
    if (watering_get_channel(channel_id, &ch) != WATERING_SUCCESS || !ch) return -ENOENT;
    memset(s, 0, sizeof(*s));
    s->is_active = ch->is_active;
    s->last_watering_time = ch->last_watering_time;
    s->plant_id = ch->plant_id;
    s->config_score = ch->config_status.configuration_score;
    s->valid = true;
    return 0;
}

static int collect_channel_config_snapshot(uint8_t channel_id, struct ucp_channel_config_snapshot *s)
{
    watering_channel_t *ch = NULL;
    if (!s) return -EINVAL;
    if (watering_get_channel(channel_id, &ch) != WATERING_SUCCESS || !ch) return -ENOENT;
    memset(s, 0, sizeof(*s));
    memcpy(s->name, ch->name, sizeof(s->name));
    s->name[sizeof(s->name) - 1] = '\0';
    s->plant_id = ch->plant_id;
    s->soil_db_index = ch->soil_db_index;
    s->irrigation_method_index = ch->irrigation_method_index;
    s->use_area_based = ch->use_area_based;
    s->area_m2 = ch->coverage.area_m2;
    s->plant_count = ch->coverage.plant_count;
    s->auto_mode = (uint8_t)ch->auto_mode;
    s->max_volume_limit_l = ch->max_volume_limit_l;
    s->enable_cycle_soak = ch->enable_cycle_soak;
    s->planting_date_unix = ch->planting_date_unix;
    s->sun_exposure_pct = ch->sun_exposure_pct;
    s->latitude_deg = ch->latitude_deg;
    s->longitude_deg = ch->longitude_deg;
    s->valid = true;
    return 0;
}

static int collect_schedule_snapshot(uint8_t channel_id, struct ucp_schedule_snapshot *s)
{
    watering_channel_t *ch = NULL;
    if (!s) return -EINVAL;
    if (watering_get_channel(channel_id, &ch) != WATERING_SUCCESS || !ch) return -ENOENT;
    memset(s, 0, sizeof(*s));
    s->ev = ch->watering_event;
    s->valid = true;
    return 0;
}

static int collect_system_snapshot(struct ucp_system_snapshot *s)
{
    watering_status_t status = WATERING_STATUS_OK;
    watering_state_t state = WATERING_STATE_IDLE;
    power_mode_t pwr = POWER_MODE_NORMAL;
    if (!s) return -EINVAL;
    if (watering_get_status(&status) != WATERING_SUCCESS) return -EIO;
    if (watering_get_state(&state) != WATERING_SUCCESS) return -EIO;
    if (watering_get_power_mode(&pwr) != WATERING_SUCCESS) return -EIO;
    memset(s, 0, sizeof(*s));
    s->status = (uint8_t)status;
    s->state = (uint8_t)state;
    s->power_mode = (uint8_t)pwr;
    s->valid = true;
    return 0;
}

static int collect_flow_snapshot(struct ucp_flow_snapshot *s)
{
    uint32_t cal = 0U;
    if (!s) return -EINVAL;
    memset(s, 0, sizeof(*s));
    s->pulses = get_pulse_count();
    s->rate_pps = get_flow_rate();
    if (watering_get_flow_calibration(&cal) == WATERING_SUCCESS) {
        s->cal_ppl = cal;
    } else {
        s->cal_ppl = get_flow_calibration();
    }
    s->valid = true;
    return 0;
}

static int collect_rain_snapshot(struct ucp_rain_snapshot *s)
{
    rain_history_stats_t hist = {0};
    rain_sensor_data_t data = {0};
    int rc;
    if (!s) return -EINVAL;
    memset(s, 0, sizeof(*s));
    rc = rain_sensor_get_data(&data);
    if (rc != 0) return -EIO;
    s->sensor = data;
    s->enabled = rain_sensor_is_enabled();
    s->integration_enabled = rain_sensor_is_integration_enabled();
    s->calib = rain_sensor_get_calibration();
    s->debounce_ms = rain_sensor_get_debounce();
    s->last24h_mm = rain_history_get_last_24h();
    if (ucp_rain_hist_stats_cache_get(&hist)) {
        s->hist = hist;
        s->hist_ok = true;
        /* Keep rain.status lightweight under active traffic.
         * Do not trigger background history-stats refresh from this GET path.
         * The cache is refreshed on-demand by history/rain-storage flows.
         */
    }
    s->valid = true;
    return 0;
}

static int collect_env_snapshot(struct ucp_env_snapshot *s)
{
    bme280_environmental_data_t env = {0};
    if (!s) return -EINVAL;
    memset(s, 0, sizeof(*s));
    if (environmental_data_get_current(&env) != 0) return -EIO;
    s->env = env;
    if (ucp_env_hist_stats_cache_get(&s->hist)) {
        s->hist_ok = true;
        /* Keep environment.current lightweight under active traffic.
         * Do not trigger background history-stats refresh from this GET path.
         * The cache is refreshed on-demand by history/env-storage flows.
         */
    }
    s->valid = true;
    return 0;
}

static int build_channel_state(struct ucp_tlv_writer *w, uint8_t channel_id, const struct ucp_channel_state_snapshot *s)
{
    if (!w || !s) return -EINVAL;
    return (ucp_tlvw_put_u8(w, RES_CH_ID, UCP_TLV_U8, channel_id) &&
            ucp_tlvw_put_u8(w, RES_CH_ACTIVE, UCP_TLV_BOOL, s->is_active ? 1U : 0U) &&
            ucp_tlvw_put_u32(w, RES_CH_LAST_WATER_TS, UCP_TLV_U32, s->last_watering_time) &&
            ucp_tlvw_put_u16(w, RES_CH_PLANT_ID, UCP_TLV_U16, s->plant_id) &&
            ucp_tlvw_put_u8(w, RES_CH_CFG_SCORE, UCP_TLV_U8, s->config_score) &&
            ucp_tlvw_put_u32(w, UCP_F_INSTANCE_ID, UCP_TLV_U32, channel_id) &&
            ucp_tlvw_put_u32(w, UCP_F_RESOURCE_VERSION, UCP_TLV_U32, g_ver_channel_state[channel_id]) &&
            ucp_tlvw_put_u64(w, UCP_F_TIMESTAMP_UTC_MS, UCP_TLV_U64, now_utc_ms())) ? 0 : -ENOSPC;
}

static int build_channel_config(struct ucp_tlv_writer *w, uint8_t channel_id, const struct ucp_channel_config_snapshot *s)
{
    if (!w || !s) return -EINVAL;
    return (ucp_tlvw_put_u8(w, RES_CH_ID, UCP_TLV_U8, channel_id) &&
            ucp_tlvw_put_str(w, RES_CH_NAME, UCP_TLV_UTF8, s->name) &&
            ucp_tlvw_put_u16(w, RES_CH_PLANT_ID, UCP_TLV_U16, s->plant_id) &&
            ucp_tlvw_put_u8(w, RES_CH_SOIL_DB, UCP_TLV_U8, s->soil_db_index) &&
            ucp_tlvw_put_u8(w, RES_CH_IRR_METHOD, UCP_TLV_U8, s->irrigation_method_index) &&
            ucp_tlvw_put_u8(w, RES_CH_USE_AREA, UCP_TLV_BOOL, s->use_area_based ? 1U : 0U) &&
            ucp_tlvw_put_f32(w, RES_CH_AREA_M2, s->area_m2) &&
            ucp_tlvw_put_u16(w, RES_CH_PLANT_COUNT, UCP_TLV_U16, s->plant_count) &&
            ucp_tlvw_put_u8(w, RES_CH_AUTO_MODE, UCP_TLV_U8, s->auto_mode) &&
            ucp_tlvw_put_f32(w, RES_CH_MAX_VOL_L, s->max_volume_limit_l) &&
            ucp_tlvw_put_u8(w, RES_CH_CYCLE_SOAK, UCP_TLV_BOOL, s->enable_cycle_soak ? 1U : 0U) &&
            ucp_tlvw_put_u32(w, RES_CH_PLANTING_TS, UCP_TLV_U32, s->planting_date_unix) &&
            ucp_tlvw_put_u8(w, RES_CH_SUN_PCT, UCP_TLV_U8, s->sun_exposure_pct) &&
            ucp_tlvw_put_f32(w, RES_CH_LAT_DEG, s->latitude_deg) &&
            ucp_tlvw_put_f32(w, RES_CH_LON_DEG, s->longitude_deg) &&
            ucp_tlvw_put_u32(w, UCP_F_INSTANCE_ID, UCP_TLV_U32, channel_id) &&
            ucp_tlvw_put_u32(w, UCP_F_RESOURCE_VERSION, UCP_TLV_U32, g_ver_channel_cfg[channel_id]) &&
            ucp_tlvw_put_u64(w, UCP_F_TIMESTAMP_UTC_MS, UCP_TLV_U64, now_utc_ms())) ? 0 : -ENOSPC;
}

static int build_schedule_config(struct ucp_tlv_writer *w, uint8_t channel_id, const watering_event_t *ev)
{
    if (!w || !ev) return -EINVAL;
    return (ucp_tlvw_put_u8(w, RES_CH_ID, UCP_TLV_U8, channel_id) &&
            ucp_tlvw_put_u8(w, RES_SCH_TYPE, UCP_TLV_U8, (uint8_t)ev->schedule_type) &&
            ucp_tlvw_put_u8(w, RES_SCH_WATER_MODE, UCP_TLV_U8, (uint8_t)ev->watering_mode) &&
            ucp_tlvw_put_u8(w, RES_SCH_DAYS_MASK, UCP_TLV_U8, ev->schedule.daily.days_of_week) &&
            ucp_tlvw_put_u8(w, RES_SCH_INTERVAL_DAYS, UCP_TLV_U8, ev->schedule.periodic.interval_days) &&
            ucp_tlvw_put_u8(w, RES_SCH_DURATION_MIN, UCP_TLV_U8, ev->watering.by_duration.duration_minutes) &&
            ucp_tlvw_put_u16(w, RES_SCH_VOLUME_L, UCP_TLV_U16, ev->watering.by_volume.volume_liters) &&
            ucp_tlvw_put_u8(w, RES_SCH_START_HOUR, UCP_TLV_U8, ev->start_time.hour) &&
            ucp_tlvw_put_u8(w, RES_SCH_START_MIN, UCP_TLV_U8, ev->start_time.minute) &&
            ucp_tlvw_put_u8(w, RES_SCH_AUTO_ENABLED, UCP_TLV_BOOL, ev->auto_enabled ? 1U : 0U) &&
            ucp_tlvw_put_u8(w, RES_SCH_USE_SOLAR, UCP_TLV_BOOL, ev->use_solar_timing ? 1U : 0U) &&
            ucp_tlvw_put_u8(w, RES_SCH_SOLAR_EVENT, UCP_TLV_U8, ev->solar_event) &&
            ucp_tlvw_put_i32(w, RES_SCH_SOLAR_OFF_MIN, UCP_TLV_I32, (int32_t)ev->solar_offset_minutes) &&
            ucp_tlvw_put_u32(w, UCP_F_INSTANCE_ID, UCP_TLV_U32, channel_id) &&
            ucp_tlvw_put_u32(w, UCP_F_RESOURCE_VERSION, UCP_TLV_U32, g_ver_schedule_cfg[channel_id]) &&
            ucp_tlvw_put_u64(w, UCP_F_TIMESTAMP_UTC_MS, UCP_TLV_U64, now_utc_ms())) ? 0 : -ENOSPC;
}

static int build_flow_status(struct ucp_tlv_writer *w, const struct ucp_flow_snapshot *s)
{
    if (!w || !s) return -EINVAL;
    return (ucp_tlvw_put_u32(w, FLOW_PULSE_COUNT, UCP_TLV_U32, s->pulses) &&
            ucp_tlvw_put_u32(w, FLOW_RATE_PPS, UCP_TLV_U32, s->rate_pps) &&
            ucp_tlvw_put_u32(w, FLOW_CAL_PPL, UCP_TLV_U32, s->cal_ppl) &&
            ucp_tlvw_put_u32(w, UCP_F_RESOURCE_VERSION, UCP_TLV_U32, g_ver_flow) &&
            ucp_tlvw_put_u64(w, UCP_F_TIMESTAMP_UTC_MS, UCP_TLV_U64, now_utc_ms())) ? 0 : -ENOSPC;
}

static int build_rain_status(struct ucp_tlv_writer *w, const struct ucp_rain_snapshot *s)
{
    if (!w || !s) return -EINVAL;
    return (ucp_tlvw_put_u32(w, RAIN_TOTAL_PULSES, UCP_TLV_U32, s->sensor.total_pulses) &&
            ucp_tlvw_put_u32(w, RAIN_LAST_PULSE_TS, UCP_TLV_U32, s->sensor.last_pulse_time) &&
            ucp_tlvw_put_f32(w, RAIN_CUR_HOUR_MM, s->sensor.current_hour_mm) &&
            ucp_tlvw_put_f32(w, RAIN_HOURLY_RATE_MMH, s->sensor.hourly_rate_mm) &&
            ucp_tlvw_put_u8(w, RAIN_STATUS, UCP_TLV_U8, (uint8_t)s->sensor.status) &&
            ucp_tlvw_put_u8(w, RAIN_DATA_QUALITY, UCP_TLV_U8, s->sensor.data_quality) &&
            ucp_tlvw_put_u8(w, RAIN_ENABLED, UCP_TLV_BOOL, s->enabled ? 1U : 0U) &&
            ucp_tlvw_put_u8(w, RAIN_INTEGRATION_ENABLED, UCP_TLV_BOOL, s->integration_enabled ? 1U : 0U) &&
            ucp_tlvw_put_f32(w, RAIN_CALIB_MM_PER_PULSE, s->calib) &&
            ucp_tlvw_put_u16(w, RAIN_DEBOUNCE_MS, UCP_TLV_U16, s->debounce_ms) &&
            ucp_tlvw_put_f32(w, RAIN_LAST_24H_MM, s->last24h_mm) &&
            ucp_tlvw_put_u16(w, RAIN_HIST_HOURLY_COUNT, UCP_TLV_U16, s->hist_ok ? s->hist.hourly_entries : 0U) &&
            ucp_tlvw_put_u16(w, RAIN_HIST_DAILY_COUNT, UCP_TLV_U16, s->hist_ok ? s->hist.daily_entries : 0U) &&
            ucp_tlvw_put_u32(w, UCP_F_RESOURCE_VERSION, UCP_TLV_U32, g_ver_rain) &&
            ucp_tlvw_put_u64(w, UCP_F_TIMESTAMP_UTC_MS, UCP_TLV_U64, now_utc_ms())) ? 0 : -ENOSPC;
}

static int build_env_status(struct ucp_tlv_writer *w, const struct ucp_env_snapshot *s)
{
    if (!w || !s) return -EINVAL;
    return (ucp_tlvw_put_f32(w, ENV_TEMP_C, s->env.current.temperature) &&
            ucp_tlvw_put_f32(w, ENV_HUMIDITY_PCT, s->env.current.humidity) &&
            ucp_tlvw_put_f32(w, ENV_PRESSURE_HPA, s->env.current.pressure) &&
            ucp_tlvw_put_u32(w, ENV_READING_TS, UCP_TLV_U32, s->env.current.timestamp) &&
            ucp_tlvw_put_u8(w, ENV_VALID, UCP_TLV_BOOL, s->env.current.valid ? 1U : 0U) &&
            ucp_tlvw_put_u16(w, ENV_READINGS_COUNT, UCP_TLV_U16, s->env.readings_count) &&
            ucp_tlvw_put_u32(w, ENV_LAST_UPDATE, UCP_TLV_U32, s->env.last_update) &&
            ucp_tlvw_put_u16(w, ENV_HIST_HOURLY_USED, UCP_TLV_U16, s->hist_ok ? s->hist.hourly_entries_used : 0U) &&
            ucp_tlvw_put_u16(w, ENV_HIST_DAILY_USED, UCP_TLV_U16, s->hist_ok ? s->hist.daily_entries_used : 0U) &&
            ucp_tlvw_put_u16(w, ENV_HIST_MONTHLY_USED, UCP_TLV_U16, s->hist_ok ? s->hist.monthly_entries_used : 0U) &&
            ucp_tlvw_put_u32(w, UCP_F_RESOURCE_VERSION, UCP_TLV_U32, g_ver_env) &&
            ucp_tlvw_put_u64(w, UCP_F_TIMESTAMP_UTC_MS, UCP_TLV_U64, now_utc_ms())) ? 0 : -ENOSPC;
}

static int build_history_storage(struct ucp_tlv_writer *w, const storage_requirements_t *s)
{
    if (!w || !s) return -EINVAL;
    return (ucp_tlvw_put_u32(w, HIST_STORAGE_DETAILED_B, UCP_TLV_U32, s->detailed_events_size) &&
            ucp_tlvw_put_u32(w, HIST_STORAGE_DAILY_B, UCP_TLV_U32, s->daily_stats_size) &&
            ucp_tlvw_put_u32(w, HIST_STORAGE_MONTHLY_B, UCP_TLV_U32, s->monthly_stats_size) &&
            ucp_tlvw_put_u32(w, HIST_STORAGE_ANNUAL_B, UCP_TLV_U32, s->annual_stats_size) &&
            ucp_tlvw_put_u32(w, HIST_STORAGE_TOTAL_KB, UCP_TLV_U32, s->total_storage_kb) &&
            ucp_tlvw_put_u32(w, UCP_F_RESOURCE_VERSION, UCP_TLV_U32, g_ver_history) &&
            ucp_tlvw_put_u64(w, UCP_F_TIMESTAMP_UTC_MS, UCP_TLV_U64, now_utc_ms())) ? 0 : -ENOSPC;
}

static int build_packs_stats(struct ucp_tlv_writer *w, const pack_storage_stats_t *s)
{
    if (!w || !s) return -EINVAL;
    return (ucp_tlvw_put_u32(w, PACKS_TOTAL_B, UCP_TLV_U32, s->total_bytes) &&
            ucp_tlvw_put_u32(w, PACKS_USED_B, UCP_TLV_U32, s->used_bytes) &&
            ucp_tlvw_put_u32(w, PACKS_FREE_B, UCP_TLV_U32, s->free_bytes) &&
            ucp_tlvw_put_u16(w, PACKS_PACK_COUNT, UCP_TLV_U16, s->pack_count) &&
            ucp_tlvw_put_u16(w, PACKS_PLANT_COUNT, UCP_TLV_U16, s->plant_count) &&
            ucp_tlvw_put_u16(w, PACKS_CUSTOM_PLANT_COUNT, UCP_TLV_U16, s->custom_plant_count) &&
            ucp_tlvw_put_u32(w, PACKS_CHANGE_COUNTER, UCP_TLV_U32, s->change_counter) &&
            ucp_tlvw_put_u8(w, PACKS_STAGE_VALID, UCP_TLV_BOOL, g_staged_blob_valid ? 1U : 0U) &&
            ucp_tlvw_put_u16(w, PACKS_STAGE_LEN, UCP_TLV_U16, g_staged_blob_len) &&
            ucp_tlvw_put_u32(w, PACKS_STAGE_CRC, UCP_TLV_U32, g_staged_blob_crc) &&
            ucp_tlvw_put_u32(w, UCP_F_RESOURCE_VERSION, UCP_TLV_U32, g_ver_packs) &&
            ucp_tlvw_put_u64(w, UCP_F_TIMESTAMP_UTC_MS, UCP_TLV_U64, now_utc_ms())) ? 0 : -ENOSPC;
}

static int build_raw_bytes_with_meta(struct ucp_tlv_writer *w, const void *data, uint16_t data_len,
                                     uint32_t version, uint32_t instance_id, bool has_instance)
{
    if (!w || (!data && data_len != 0U)) return -EINVAL;
    if (!ucp_tlvw_put_bytes(w, RAW_BYTES_F, UCP_TLV_BYTES, (const uint8_t *)data, data_len)) return -ENOSPC;
    if (has_instance && !ucp_tlvw_put_u32(w, UCP_F_INSTANCE_ID, UCP_TLV_U32, instance_id)) return -ENOSPC;
    if (!ucp_tlvw_put_u32(w, UCP_F_RESOURCE_VERSION, UCP_TLV_U32, version)) return -ENOSPC;
    if (!ucp_tlvw_put_u64(w, UCP_F_TIMESTAMP_UTC_MS, UCP_TLV_U64, now_utc_ms())) return -ENOSPC;
    return 0;
}

static int build_system_config_bytes(struct ucp_tlv_writer *w)
{
    struct enhanced_system_config_data cfg;
    power_mode_t current_mode;
    master_valve_config_t master_config;
    bme280_config_t bme_cfg;
    bme280_environmental_data_t env_data;
    uint8_t temp_enabled_channels = 0U;
    float temp_sensitivity_accum = 0.0f;
    float temp_base_accum = 0.0f;

    memset(&cfg, 0, sizeof(cfg));
    cfg.version = 2U;
    if (watering_get_power_mode(&current_mode) == WATERING_SUCCESS) cfg.power_mode = (uint8_t)current_mode;
    cfg.flow_calibration = get_flow_calibration();
    cfg.max_active_valves = 1U;
    cfg.num_channels = WATERING_CHANNELS_COUNT;

    if (master_valve_get_config(&master_config) == WATERING_SUCCESS) {
        cfg.master_valve_enabled = master_config.enabled ? 1U : 0U;
        cfg.master_valve_pre_delay = master_config.pre_start_delay_sec;
        cfg.master_valve_post_delay = master_config.post_stop_delay_sec;
        cfg.master_valve_overlap_grace = master_config.overlap_grace_sec;
        cfg.master_valve_auto_mgmt = master_config.auto_management ? 1U : 0U;
        cfg.master_valve_current_state = master_config.is_active ? 1U : 0U;
    } else {
        cfg.master_valve_overlap_grace = 30U;
        cfg.master_valve_auto_mgmt = 1U;
    }

    if (bme280_system_get_config(&bme_cfg) == 0) {
        cfg.bme280_enabled = bme_cfg.enabled ? 1U : 0U;
        cfg.bme280_measurement_interval = bme_cfg.measurement_interval;
        cfg.bme280_sensor_status = bme_cfg.initialized ? 1U : 0U;
    } else {
        cfg.bme280_measurement_interval = 60U;
    }

    for (uint8_t i = 0U; i < WATERING_CHANNELS_COUNT; i++) {
        watering_channel_t *channel = NULL;
        if (watering_get_channel(i, &channel) != WATERING_SUCCESS || !channel) continue;
        if (channel->temp_compensation.enabled) {
            temp_enabled_channels++;
            temp_sensitivity_accum += channel->temp_compensation.sensitivity;
            temp_base_accum += channel->temp_compensation.base_temperature;
        }
        if (channel->rain_compensation.enabled || channel->temp_compensation.enabled) {
            cfg.compensation_active_channels |= BIT(i);
        }
    }
    if (temp_enabled_channels > 0U) {
        cfg.global_temp_compensation_enabled = 1U;
        cfg.global_temp_sensitivity = temp_sensitivity_accum / (float)temp_enabled_channels;
        cfg.global_temp_base_temperature = temp_base_accum / (float)temp_enabled_channels;
    } else {
        cfg.global_temp_sensitivity = TEMP_COMP_DEFAULT_SENSITIVITY;
        cfg.global_temp_base_temperature = TEMP_COMP_DEFAULT_BASE_TEMP;
    }

    (void)enhanced_system_is_interval_mode_active(&cfg.interval_mode_active_channels);
    (void)enhanced_system_has_incomplete_config(&cfg.incomplete_config_channels);

    if (environmental_data_get_current(&env_data) == 0 && env_data.current.valid) {
        env_data_validation_t validation = {0};
        if (env_data_validate_reading(&env_data.current, NULL, &validation) == 0) {
            cfg.environmental_data_quality = env_data_calculate_quality_score(&env_data.current, &validation);
        }
        cfg.last_sensor_reading = env_data.current.timestamp;
    }
    cfg.last_config_update = timezone_get_unix_utc();
    if (cfg.last_sensor_reading == 0U) cfg.last_sensor_reading = cfg.last_config_update;

    return build_raw_bytes_with_meta(w, &cfg, sizeof(cfg), g_ver_syscfg, 0U, false);
}

static int apply_system_config_bytes(const struct enhanced_system_config_data *cfg)
{
    float temp_sensitivity, base_temperature;
    bool temp_enable;
    if (!cfg) return -EINVAL;
    if (cfg->power_mode > 2U) return -ERANGE;
    if (cfg->flow_calibration < 100U || cfg->flow_calibration > 10000U) return -ERANGE;
    if (cfg->bme280_enabled && cfg->bme280_measurement_interval == 0U) return -ERANGE;

    (void)watering_set_power_mode((power_mode_t)cfg->power_mode);
    (void)onboarding_update_system_flag(SYSTEM_FLAG_POWER_MODE_SET, true);
    (void)set_flow_calibration(cfg->flow_calibration);

    {
        master_valve_config_t mv = {0};
        mv.enabled = (cfg->master_valve_enabled != 0U);
        mv.pre_start_delay_sec = cfg->master_valve_pre_delay;
        mv.post_stop_delay_sec = cfg->master_valve_post_delay;
        mv.overlap_grace_sec = cfg->master_valve_overlap_grace;
        mv.auto_management = (cfg->master_valve_auto_mgmt != 0U);
        (void)master_valve_set_config(&mv);
        (void)onboarding_update_system_flag(SYSTEM_FLAG_MASTER_VALVE_SET, true);
    }

    {
        bme280_config_t bme_cfg;
        if (bme280_system_get_config(&bme_cfg) == 0) {
            bme_cfg.enabled = (cfg->bme280_enabled != 0U);
            if (cfg->bme280_measurement_interval != 0U) bme_cfg.measurement_interval = cfg->bme280_measurement_interval;
            (void)sensor_manager_configure_bme280(&bme_cfg);
        }
    }

    temp_sensitivity = cfg->global_temp_sensitivity;
    if (temp_sensitivity < TEMP_COMP_MIN_SENSITIVITY) temp_sensitivity = TEMP_COMP_MIN_SENSITIVITY;
    if (temp_sensitivity > TEMP_COMP_MAX_SENSITIVITY) temp_sensitivity = TEMP_COMP_MAX_SENSITIVITY;
    base_temperature = cfg->global_temp_base_temperature;
    if (base_temperature < TEMP_COMP_MIN_TEMP_C) base_temperature = TEMP_COMP_MIN_TEMP_C;
    if (base_temperature > TEMP_COMP_MAX_TEMP_C) base_temperature = TEMP_COMP_MAX_TEMP_C;
    temp_enable = (cfg->global_temp_compensation_enabled != 0U);
    for (uint8_t ch = 0U; ch < WATERING_CHANNELS_COUNT; ch++) {
        watering_channel_t *channel = NULL;
        if (watering_get_channel(ch, &channel) != WATERING_SUCCESS || !channel) continue;
        channel->temp_compensation.enabled = temp_enable;
        channel->temp_compensation.base_temperature = base_temperature;
        channel->temp_compensation.sensitivity = temp_sensitivity;
        channel->temp_compensation.min_factor = TEMP_COMP_DEFAULT_MIN_FACTOR;
        channel->temp_compensation.max_factor = TEMP_COMP_DEFAULT_MAX_FACTOR;
    }
    (void)watering_save_config_priority(true);
    return 0;
}

static int build_statistics_bytes(struct ucp_tlv_writer *w, uint8_t channel_id)
{
    struct statistics_data st;
    watering_channel_t *channel = NULL;
    uint32_t total_volume_ml = 0U, last_volume_ml = 0U, watering_count = 0U;

    if (channel_id >= WATERING_CHANNELS_COUNT) return -EINVAL;
    memset(&st, 0, sizeof(st));
    st.channel_id = channel_id;
    (void)watering_get_channel_statistics(channel_id, &total_volume_ml, &last_volume_ml, &watering_count);
    if (watering_get_channel(channel_id, &channel) == WATERING_SUCCESS && channel) {
        st.last_watering = channel->last_watering_time;
    }
    st.total_volume = total_volume_ml;
    st.last_volume = last_volume_ml;
    st.count = (watering_count > UINT16_MAX) ? UINT16_MAX : (uint16_t)watering_count;
    return build_raw_bytes_with_meta(w, &st, sizeof(st), g_ver_stats[channel_id], channel_id, true);
}

static int build_auto_calc_status_bytes(struct ucp_tlv_writer *w, uint8_t channel_id)
{
    struct auto_calc_status_data d;
    bool calc_enabled = false;
    uint8_t interval_hours = 0U;
    watering_channel_t *channel = NULL;

    memset(&d, 0, sizeof(d));
    d.channel_id = channel_id;
    (void)watering_get_auto_calc_status(&calc_enabled, &interval_hours);

    if (channel_id == 0xFFU) {
        d.calculation_active = calc_enabled ? 1U : 0U;
        d.next_irrigation_time = 0U;
        return build_raw_bytes_with_meta(w, &d, sizeof(d), g_ver_auto_calc, 0xFFFFFFFFU, true);
    }
    if (channel_id >= WATERING_CHANNELS_COUNT) return -EINVAL;
    if (watering_get_channel(channel_id, &channel) != WATERING_SUCCESS || !channel) return -ENOENT;

    d.calculation_active = ((channel->auto_mode == WATERING_AUTOMATIC_QUALITY || channel->auto_mode == WATERING_AUTOMATIC_ECO) && calc_enabled) ? 1U : 0U;
    d.auto_mode = (uint8_t)channel->auto_mode;
    d.last_calculation_time = channel->last_calculation_time;
    d.calculation_error = 0U;
    d.cycle_count = 1U;
    if (channel->water_balance) {
        const water_balance_t *bal = (const water_balance_t *)channel->water_balance;
        d.irrigation_needed = bal->irrigation_needed ? 1U : 0U;
        d.current_deficit_mm = bal->current_deficit_mm;
        d.raw_mm = bal->raw_mm;
        d.effective_rain_mm = bal->effective_rain_mm;
    }
    d.etc_mm_day = 0.0f;
    d.et0_mm_day = 0.0f;
    d.crop_coefficient = 0.0f;
    d.net_irrigation_mm = 0.0f;
    d.gross_irrigation_mm = 0.0f;
    d.calculated_volume_l = 0.0f;
    d.volume_liters = channel->last_temp_compensation.adjusted_requirement;
    d.quality_mode = (uint8_t)channel->auto_mode;
    d.days_after_planting = channel->days_after_planting;
    d.volume_limited = (channel->max_volume_limit_l > 0.0f &&
                        d.volume_liters > channel->max_volume_limit_l) ? 1U : 0U;
    return build_raw_bytes_with_meta(w, &d, sizeof(d), g_ver_auto_calc, channel_id, true);
}

struct __packed ucp_auto_calc_control_data_v1 {
    uint8_t enabled;
    uint8_t interval_hours;
    uint8_t reserved[2];
};

static int build_auto_calc_control_bytes(struct ucp_tlv_writer *w)
{
    struct ucp_auto_calc_control_data_v1 d;
    bool enabled = false;
    uint8_t interval_hours = 0U;
    watering_error_t rc;

    memset(&d, 0, sizeof(d));
    rc = watering_get_auto_calc_status(&enabled, &interval_hours);
    if (rc != WATERING_SUCCESS) return -EIO;
    d.enabled = enabled ? 1U : 0U;
    d.interval_hours = interval_hours;
    return build_raw_bytes_with_meta(w, &d, sizeof(d), g_ver_auto_calc_ctl, 0U, false);
}

struct __packed ucp_bulk_snapshot_v1 {
    uint8_t version;
    uint8_t system_status;
    uint8_t system_state;
    uint8_t power_mode;
    uint8_t active_channels_bitmap;
    uint8_t interval_mode_channels_bitmap;
    uint8_t incomplete_config_channels_bitmap;
    uint8_t rain_sensor_status;
    uint32_t uptime_ms;
    uint32_t unix_utc;
    uint32_t flow_calibration;
    uint32_t rain_last_pulse;
    float rain_last_24h_mm;
    float env_temp_c;
    float env_humidity_pct;
    float env_pressure_hpa;
    uint32_t env_ts;
    uint8_t peer_power_on;
    uint8_t peer_power_inhibit;
    uint8_t reserved[2];
};

static int build_bulk_snapshot_bytes(struct ucp_tlv_writer *w)
{
    struct ucp_bulk_snapshot_v1 s;
    watering_status_t ws = WATERING_STATUS_OK;
    watering_state_t st = WATERING_STATE_IDLE;
    power_mode_t pm = POWER_MODE_NORMAL;
    bme280_environmental_data_t env = {0};
    rain_sensor_data_t rain = {0};
    uint32_t cal = 0U;

    memset(&s, 0, sizeof(s));
    s.version = 1U;
    (void)watering_get_status(&ws);
    (void)watering_get_state(&st);
    (void)watering_get_power_mode(&pm);
    (void)watering_get_flow_calibration(&cal);
    (void)rain_sensor_get_data(&rain);
    (void)environmental_data_get_current(&env);
    s.system_status = (uint8_t)ws;
    s.system_state = (uint8_t)st;
    s.power_mode = (uint8_t)pm;
    s.uptime_ms = k_uptime_get_32();
    s.unix_utc = timezone_get_unix_utc();
    s.flow_calibration = cal;
    s.rain_sensor_status = (uint8_t)rain.status;
    s.rain_last_pulse = rain.last_pulse_time;
    s.rain_last_24h_mm = rain_history_get_last_24h();
    s.env_temp_c = env.current.temperature;
    s.env_humidity_pct = env.current.humidity;
    s.env_pressure_hpa = env.current.pressure;
    s.env_ts = env.current.timestamp;
    s.peer_power_on = g_peer_power_on ? 1U : 0U;
    s.peer_power_inhibit = peer_power_inhibit() ? 1U : 0U;
    (void)enhanced_system_is_interval_mode_active(&s.interval_mode_channels_bitmap);
    (void)enhanced_system_has_incomplete_config(&s.incomplete_config_channels_bitmap);
    for (uint8_t i = 0U; i < WATERING_CHANNELS_COUNT; i++) {
        watering_channel_t *ch = NULL;
        if (watering_get_channel(i, &ch) == WATERING_SUCCESS && ch && ch->is_active) s.active_channels_bitmap |= BIT(i);
    }
    return build_raw_bytes_with_meta(w, &s, sizeof(s), g_ver_bulk, 0U, false);
}

static void fill_hydraulic_status_ble(uint8_t channel_id, struct hydraulic_status_data *out)
{
    uint8_t resolved = channel_id;
    hydraulic_lock_state_t global_lock = {0};
    watering_channel_t *channel = NULL;
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (resolved >= WATERING_CHANNELS_COUNT) resolved = 0U;
    out->channel_id = resolved;
    if (watering_get_channel(resolved, &channel) == WATERING_SUCCESS && channel) {
        out->profile_type = (uint8_t)channel->hydraulic.profile_type;
        out->lock_level = (uint8_t)channel->hydraulic_lock.level;
        out->lock_reason = (uint8_t)channel->hydraulic_lock.reason;
        out->nominal_flow_ml_min = channel->hydraulic.nominal_flow_ml_min;
        out->ramp_up_time_sec = channel->hydraulic.ramp_up_time_sec;
        out->tolerance_high_percent = channel->hydraulic.tolerance_high_percent;
        out->tolerance_low_percent = channel->hydraulic.tolerance_low_percent;
        out->is_calibrated = channel->hydraulic.is_calibrated ? 1U : 0U;
        out->monitoring_enabled = channel->hydraulic.monitoring_enabled ? 1U : 0U;
        out->learning_runs = channel->hydraulic.learning_runs;
        out->stable_runs = channel->hydraulic.stable_runs;
        out->estimated = channel->hydraulic.estimated ? 1U : 0U;
        out->manual_override_active = watering_hydraulic_manual_override_active(resolved) ? 1U : 0U;
        out->lock_at_epoch = channel->hydraulic_lock.locked_at_epoch;
        out->retry_after_epoch = channel->hydraulic_lock.retry_after_epoch;
        out->no_flow_runs = channel->hydraulic_anomaly.no_flow_runs;
        out->high_flow_runs = channel->hydraulic_anomaly.high_flow_runs;
        out->unexpected_flow_runs = channel->hydraulic_anomaly.unexpected_flow_runs;
        out->last_anomaly_epoch = channel->hydraulic_anomaly.last_anomaly_epoch;
    }
    watering_get_global_hydraulic_lock(&global_lock);
    out->global_lock_level = (uint8_t)global_lock.level;
    out->global_lock_reason = (uint8_t)global_lock.reason;
    out->global_lock_at_epoch = global_lock.locked_at_epoch;
    out->global_retry_after_epoch = global_lock.retry_after_epoch;
}

static int build_hydraulic_status_bytes(struct ucp_tlv_writer *w, uint8_t channel_id)
{
    struct hydraulic_status_data hs;
    if (channel_id >= WATERING_CHANNELS_COUNT) return -EINVAL;
    fill_hydraulic_status_ble(channel_id, &hs);
    return build_raw_bytes_with_meta(w, &hs, sizeof(hs), g_ver_hydraulic[channel_id], channel_id, true);
}

static int apply_hydraulic_status_bytes(const struct hydraulic_status_data *in)
{
    watering_channel_t *ch = NULL;
    if (!in || in->channel_id >= WATERING_CHANNELS_COUNT) return -EINVAL;
    if (watering_get_channel(in->channel_id, &ch) != WATERING_SUCCESS || !ch) return -ENOENT;
    ch->hydraulic.profile_type = (hydraulic_profile_t)in->profile_type;
    ch->hydraulic.tolerance_high_percent = in->tolerance_high_percent;
    ch->hydraulic.tolerance_low_percent = in->tolerance_low_percent;
    ch->hydraulic.monitoring_enabled = (in->monitoring_enabled != 0U);
    (void)save_channel_after_change(in->channel_id);
    return 0;
}

static int build_comp_status_bytes(struct ucp_tlv_writer *w, uint8_t channel_id)
{
    struct compensation_status_data comp;
    watering_channel_t *channel = NULL;
    if (channel_id >= WATERING_CHANNELS_COUNT) return -EINVAL;
    memset(&comp, 0, sizeof(comp));
    comp.channel_id = channel_id;
    if (watering_get_channel(channel_id, &channel) == WATERING_SUCCESS && channel) {
        comp.rain_compensation_active = channel->rain_compensation.enabled ? 1U : 0U;
        comp.recent_rainfall_mm = 0.0f;
        comp.rain_reduction_percentage = channel->last_rain_compensation.reduction_percentage;
        comp.rain_skip_watering = channel->last_rain_compensation.skip_watering ? 1U : 0U;
        comp.rain_calculation_time = channel->last_calculation_time;
        comp.temp_compensation_active = channel->temp_compensation.enabled ? 1U : 0U;
        comp.current_temperature = 0.0f;
        comp.temp_compensation_factor = channel->last_temp_compensation.compensation_factor;
        comp.temp_adjusted_requirement = channel->last_temp_compensation.adjusted_requirement;
        comp.temp_calculation_time = channel->last_calculation_time;
        comp.any_compensation_active = (comp.rain_compensation_active || comp.temp_compensation_active) ? 1U : 0U;
    } else {
        comp.temp_compensation_factor = 1.0f;
    }
    return build_raw_bytes_with_meta(w, &comp, sizeof(comp), g_ver_comp_status[channel_id], channel_id, true);
}

static int build_comp_channel_cfg_bytes(struct ucp_tlv_writer *w, uint8_t channel_id)
{
    struct channel_compensation_config_data cfg;
    watering_channel_t *channel = NULL;
    if (channel_id >= WATERING_CHANNELS_COUNT) return -EINVAL;
    memset(&cfg, 0, sizeof(cfg));
    cfg.channel_id = channel_id;
    if (watering_get_channel(channel_id, &channel) != WATERING_SUCCESS || !channel) return -ENOENT;
    cfg.rain_enabled = channel->rain_compensation.enabled ? 1U : 0U;
    cfg.rain_sensitivity = channel->rain_compensation.sensitivity;
    cfg.rain_lookback_hours = channel->rain_compensation.lookback_hours;
    cfg.rain_skip_threshold_mm = channel->rain_compensation.skip_threshold_mm;
    cfg.rain_reduction_factor = channel->rain_compensation.reduction_factor;
    cfg.temp_enabled = channel->temp_compensation.enabled ? 1U : 0U;
    cfg.temp_base_temperature = channel->temp_compensation.base_temperature;
    cfg.temp_sensitivity = channel->temp_compensation.sensitivity;
    cfg.temp_min_factor = channel->temp_compensation.min_factor;
    cfg.temp_max_factor = channel->temp_compensation.max_factor;
    cfg.last_rain_calc_time = channel->last_calculation_time;
    cfg.last_temp_calc_time = channel->last_calculation_time;
    return build_raw_bytes_with_meta(w, &cfg, sizeof(cfg), g_ver_comp_cfg[channel_id], channel_id, true);
}

static int apply_comp_channel_cfg_bytes(const struct channel_compensation_config_data *cfg)
{
    watering_channel_t *channel = NULL;
    uint8_t ch_id;
    if (!cfg) return -EINVAL;
    ch_id = cfg->channel_id;
    if (ch_id >= WATERING_CHANNELS_COUNT) return -EINVAL;
    if (watering_get_channel(ch_id, &channel) != WATERING_SUCCESS || !channel) return -ENOENT;
    channel->rain_compensation.enabled = (cfg->rain_enabled != 0U);
    channel->rain_compensation.sensitivity = cfg->rain_sensitivity;
    channel->rain_compensation.lookback_hours = cfg->rain_lookback_hours;
    channel->rain_compensation.skip_threshold_mm = cfg->rain_skip_threshold_mm;
    channel->rain_compensation.reduction_factor = cfg->rain_reduction_factor;
    channel->temp_compensation.enabled = (cfg->temp_enabled != 0U);
    channel->temp_compensation.base_temperature = cfg->temp_base_temperature;
    channel->temp_compensation.sensitivity = cfg->temp_sensitivity;
    channel->temp_compensation.min_factor = cfg->temp_min_factor;
    channel->temp_compensation.max_factor = cfg->temp_max_factor;
    channel->config_status.compensation_configured = (cfg->rain_enabled || cfg->temp_enabled) ? true : channel->config_status.compensation_configured;
    (void)save_channel_after_change(ch_id);
    return 0;
}

static int build_rain_integration_bytes(struct ucp_tlv_writer *w)
{
    rain_integration_status_t st;
    struct rain_integration_status_ble ble;
    memset(&st, 0, sizeof(st));
    memset(&ble, 0, sizeof(ble));
    if (watering_get_rain_integration_status(&st) != WATERING_SUCCESS) return -EIO;
    ble.sensor_active = st.sensor_active ? 1U : 0U;
    ble.integration_enabled = st.integration_enabled ? 1U : 0U;
    ble.last_pulse_time = st.last_pulse_time;
    ble.calibration_mm_per_pulse = st.calibration_mm_per_pulse;
    ble.rainfall_last_hour = st.rainfall_last_hour;
    ble.rainfall_last_24h = st.rainfall_last_24h;
    ble.rainfall_last_48h = st.rainfall_last_48h;
    ble.sensitivity_pct = st.sensitivity_pct;
    ble.skip_threshold_mm = st.skip_threshold_mm;
    for (uint8_t i = 0U; i < WATERING_CHANNELS_COUNT; i++) {
        ble.channel_reduction_pct[i] = st.channel_reduction_pct[i];
        ble.channel_skip_irrigation[i] = st.channel_skip_irrigation[i] ? 1U : 0U;
    }
    ble.hourly_entries = st.hourly_entries;
    ble.daily_entries = st.daily_entries;
    ble.storage_usage_bytes = st.storage_usage_bytes;
    return build_raw_bytes_with_meta(w, &ble, sizeof(ble), g_ver_rain_integration, 0U, false);
}

static int build_onboarding_status_bytes(struct ucp_tlv_writer *w)
{
    onboarding_state_t state;
    struct onboarding_status_data out;
    int set_channel_flags = 0;
    int set_system_flags = 0;
    int set_schedule_flags = 0;
    if (onboarding_get_state(&state) < 0) return -EIO;
    memset(&out, 0, sizeof(out));
    out.overall_completion_pct = state.onboarding_completion_pct;
    for (int i = 0; i < 64; i++) if (state.channel_config_flags & (1ULL << i)) set_channel_flags++;
    for (int i = 0; i < 32; i++) if (state.system_config_flags & (1U << i)) set_system_flags++;
    for (int i = 0; i < 8; i++) if (state.schedule_config_flags & (1U << i)) set_schedule_flags++;
    out.channels_completion_pct = (uint8_t)((set_channel_flags * 100) / (8 * 8));
    out.system_completion_pct = (uint8_t)((set_system_flags * 100) / 8);
    out.schedules_completion_pct = (uint8_t)((set_schedule_flags * 100) / 8);
    out.channel_config_flags = state.channel_config_flags;
    out.system_config_flags = state.system_config_flags;
    out.schedule_config_flags = state.schedule_config_flags;
    out.onboarding_start_time = state.onboarding_start_time;
    out.last_update_time = state.last_update_time;
    out.channel_extended_flags = state.channel_extended_flags;
    return build_raw_bytes_with_meta(w, &out, sizeof(out), g_ver_onboarding, 0U, false);
}

static int build_onb_system_overview_bytes(struct ucp_tlv_writer *w)
{
    config_system_overview_t ov;
    if (config_status_get_system_overview(&ov) < 0) return -EIO;
    return build_raw_bytes_with_meta(w, &ov, sizeof(ov), g_ver_onb_overview, 0U, false);
}

/* Fast path for deferred notify rescans: derive onboarding system overview from
 * in-memory channel->config_status shadows instead of config_status_get_system_overview()
 * which loads every channel status from NVS.
 */
static int build_onb_system_overview_bytes_fast(struct ucp_tlv_writer *w)
{
    config_system_overview_t ov;

    if (!w) {
        return -EINVAL;
    }

    memset(&ov, 0, sizeof(ov));
    for (uint8_t channel_id = 0; channel_id < WATERING_CHANNELS_COUNT; channel_id++) {
        watering_channel_t *channel = NULL;
        channel_config_status_t status;

        if (watering_get_channel(channel_id, &channel) != WATERING_SUCCESS || !channel) {
            ov.channel_scores[channel_id] = 0U;
            continue;
        }

        memset(&status, 0, sizeof(status));
        status.basic_configured = channel->config_status.basic_configured;
        status.growing_env_configured = channel->config_status.growing_env_configured;
        status.compensation_configured = channel->config_status.compensation_configured;
        status.custom_soil_configured = channel->config_status.custom_soil_configured;
        status.interval_configured = channel->config_status.interval_configured;
        status.configuration_score = channel->config_status.configuration_score;
        status.last_reset_timestamp = channel->config_status.last_reset_timestamp;

        ov.channel_scores[channel_id] = status.configuration_score;
        ov.overall_system_score += status.configuration_score;

        if (config_status_can_perform_automatic_watering(&status)) {
            ov.channels_ready_for_auto_watering++;
        }
        if (status.configuration_score == 100U) {
            ov.fully_configured_channels++;
        }
        if (status.basic_configured) ov.channels_with_basic++;
        if (status.growing_env_configured) ov.channels_with_growing_env++;
        if (status.compensation_configured) ov.channels_with_compensation++;
        if (status.custom_soil_configured) ov.channels_with_custom_soil++;
        if (status.interval_configured) ov.channels_with_interval++;
    }

    ov.overall_system_score /= WATERING_CHANNELS_COUNT;
    if (ov.overall_system_score >= 80U) {
        ov.system_health = CONFIG_HEALTH_EXCELLENT;
    } else if (ov.overall_system_score >= 60U) {
        ov.system_health = CONFIG_HEALTH_GOOD;
    } else if (ov.overall_system_score >= 40U) {
        ov.system_health = CONFIG_HEALTH_FAIR;
    } else {
        ov.system_health = CONFIG_HEALTH_POOR;
    }

    return build_raw_bytes_with_meta(w, &ov, sizeof(ov), g_ver_onb_overview, 0U, false);
}

static void cfg_reset_meta_cache_update(uint8_t channel_id,
                                        const channel_config_status_t *status,
                                        const config_reset_log_t *log)
{
    if (channel_id >= WATERING_CHANNELS_COUNT) {
        return;
    }

    g_cfg_reset_meta_valid[channel_id] = true;
    g_cfg_reset_meta_ts[channel_id] = status ? status->last_reset_timestamp : 0U;
    g_cfg_reset_meta_count[channel_id] = status ? status->reset_count : 0U;
    g_cfg_reset_meta_group[channel_id] = 0U;
    memset(g_cfg_reset_meta_reason[channel_id], 0, sizeof(g_cfg_reset_meta_reason[channel_id]));

    if (log && log->count > 0U) {
        const config_reset_log_entry_t *last = &log->entries[(uint8_t)((log->head + 15U) % 16U)];
        g_cfg_reset_meta_ts[channel_id] = last->timestamp;
        g_cfg_reset_meta_count[channel_id] = log->count;
        g_cfg_reset_meta_group[channel_id] = (uint8_t)last->group;
        memcpy(g_cfg_reset_meta_reason[channel_id], last->reason, sizeof(g_cfg_reset_meta_reason[channel_id]));
        g_cfg_reset_meta_reason[channel_id][sizeof(g_cfg_reset_meta_reason[channel_id]) - 1U] = '\0';
    }
}

static void cfg_reset_meta_apply_to_response(uint8_t channel_id,
                                             const channel_config_status_t *status,
                                             struct config_status_response_data *out)
{
    if (!out || channel_id >= WATERING_CHANNELS_COUNT) {
        return;
    }

    if (status) {
        out->last_reset_timestamp = status->last_reset_timestamp;
        out->reset_count = status->reset_count;
    }

    if (!g_cfg_reset_meta_valid[channel_id]) {
        return;
    }

    /* Use cached log-derived metadata when timestamps match. This keeps the
     * deferred notify path free of NVS reads while preserving rich fields. */
    if (!status || g_cfg_reset_meta_ts[channel_id] == status->last_reset_timestamp) {
        out->last_reset_timestamp = g_cfg_reset_meta_ts[channel_id];
        out->reset_count = g_cfg_reset_meta_count[channel_id];
        out->last_reset_group = g_cfg_reset_meta_group[channel_id];
        memcpy(out->last_reset_reason, g_cfg_reset_meta_reason[channel_id], sizeof(out->last_reset_reason));
        out->last_reset_reason[sizeof(out->last_reset_reason) - 1U] = '\0';
    }
}

static int build_onb_config_status_bytes(struct ucp_tlv_writer *w, uint8_t channel_id)
{
    channel_config_status_t status;
    config_reset_log_t log = {0};
    struct config_status_response_data out;
    if (channel_id >= WATERING_CHANNELS_COUNT) return -EINVAL;
    if (channel_get_config_status(channel_id, &status) != WATERING_SUCCESS) return -EIO;
    (void)config_status_get_reset_log(channel_id, &log);
    memset(&out, 0, sizeof(out));
    out.channel_id = channel_id;
    out.basic_complete = status.basic_configured ? 1U : 0U;
    out.growing_env_complete = status.growing_env_configured ? 1U : 0U;
    out.compensation_complete = status.compensation_configured ? 1U : 0U;
    out.custom_soil_complete = status.custom_soil_configured ? 1U : 0U;
    out.interval_complete = status.interval_configured ? 1U : 0U;
    out.config_score = status.configuration_score;
    /* Avoid a second full channel assessment: channel_get_config_status() already
     * returns an assessed status snapshot, and can_auto_water derives directly
     * from those flags via configuration_status policy. */
    out.can_auto_water = config_status_can_perform_automatic_watering(&status) ? 1U : 0U;
    cfg_reset_meta_cache_update(channel_id, &status, &log);
    cfg_reset_meta_apply_to_response(channel_id, &status, &out);
    return build_raw_bytes_with_meta(w, &out, sizeof(out), g_ver_cfg_status[channel_id], channel_id, true);
}

/* Fast path for deferred notify rescans: use in-memory channel->config_status
 * instead of full channel_get_config_status() reassessment (which can trigger
 * expensive validation/DB/Fao56 work). GET handlers keep using the full builder.
 */
static int build_onb_config_status_bytes_fast(struct ucp_tlv_writer *w, uint8_t channel_id)
{
    watering_channel_t *channel = NULL;
    channel_config_status_t status;
    struct config_status_response_data out;

    if (!w || channel_id >= WATERING_CHANNELS_COUNT) {
        return -EINVAL;
    }
    if (watering_get_channel(channel_id, &channel) != WATERING_SUCCESS || !channel) {
        return -EIO;
    }

    memset(&status, 0, sizeof(status));
    status.basic_configured = channel->config_status.basic_configured;
    status.growing_env_configured = channel->config_status.growing_env_configured;
    status.compensation_configured = channel->config_status.compensation_configured;
    status.custom_soil_configured = channel->config_status.custom_soil_configured;
    status.interval_configured = channel->config_status.interval_configured;
    status.configuration_score = channel->config_status.configuration_score;
    status.last_reset_timestamp = channel->config_status.last_reset_timestamp;
    if (g_cfg_reset_meta_valid[channel_id]) {
        status.reset_count = g_cfg_reset_meta_count[channel_id];
    }

    memset(&out, 0, sizeof(out));
    out.channel_id = channel_id;
    out.basic_complete = status.basic_configured ? 1U : 0U;
    out.growing_env_complete = status.growing_env_configured ? 1U : 0U;
    out.compensation_complete = status.compensation_configured ? 1U : 0U;
    out.custom_soil_complete = status.custom_soil_configured ? 1U : 0U;
    out.interval_complete = status.interval_configured ? 1U : 0U;
    out.config_score = status.configuration_score;
    out.can_auto_water = config_status_can_perform_automatic_watering(&status) ? 1U : 0U;
    cfg_reset_meta_apply_to_response(channel_id, &status, &out);
    return build_raw_bytes_with_meta(w, &out, sizeof(out), g_ver_cfg_status[channel_id], channel_id, true);
}

static int build_reset_control_bytes(struct ucp_tlv_writer *w)
{
    struct reset_control_data out;
    wipe_progress_t wipe_prog;
    reset_confirmation_t confirmation;
    memset(&out, 0, sizeof(out));
    (void)reset_controller_get_wipe_progress(&wipe_prog);
    if (wipe_prog.state == WIPE_STATE_IN_PROGRESS || wipe_prog.state == WIPE_STATE_DONE_OK || wipe_prog.state == WIPE_STATE_DONE_ERROR) {
        out.reset_type = 0xFFU;
        out.channel_id = 0xFFU;
        out.confirmation_code = 0U;
        out.timestamp = k_uptime_get_32() / 1000U;
        out.status = (uint8_t)wipe_prog.state;
        out.reserved[0] = wipe_step_to_progress_pct(wipe_prog.current_step);
        out.reserved[1] = (uint8_t)wipe_prog.current_step;
        out.reserved[2] = wipe_prog.attempt_count;
        out.reserved[3] = (uint8_t)(wipe_prog.last_error & 0xFFU);
        out.reserved[4] = (uint8_t)((wipe_prog.last_error >> 8) & 0xFFU);
    } else if (reset_controller_get_confirmation_info(&confirmation) == 0 && confirmation.is_valid) {
        out.reset_type = (confirmation.type == RESET_TYPE_CHANNEL_CONFIG) ? 0x01U :
                         (confirmation.type == RESET_TYPE_CHANNEL_SCHEDULE) ? 0x02U :
                         (confirmation.type == RESET_TYPE_ALL_CHANNELS) ? 0x10U :
                         (confirmation.type == RESET_TYPE_ALL_SCHEDULES) ? 0x11U :
                         (confirmation.type == RESET_TYPE_SYSTEM_CONFIG) ? 0x12U :
                         (confirmation.type == RESET_TYPE_CALIBRATION) ? 0x13U :
                         (confirmation.type == RESET_TYPE_HISTORY) ? 0x14U : 0xFFU;
        out.channel_id = confirmation.channel_id;
        out.confirmation_code = confirmation.code;
        out.timestamp = confirmation.generation_time;
        out.status = (uint8_t)WIPE_STATE_AWAIT_CONFIRM;
    } else {
        out.reset_type = 0xFFU;
        out.channel_id = 0xFFU;
        out.status = (uint8_t)WIPE_STATE_IDLE;
    }
    return build_raw_bytes_with_meta(w, &out, sizeof(out), g_ver_reset_ctl, 0U, false);
}

static int build_custom_soil_bytes(struct ucp_tlv_writer *w, uint8_t channel_id)
{
    struct custom_soil_config_data d;
    watering_error_t rc = bt_get_custom_soil_for_ble(channel_id, &d);
    d.operation = 0U;
    d.status = (uint8_t)rc;
    return build_raw_bytes_with_meta(w, &d, sizeof(d), g_ver_custom_soil[channel_id], channel_id, true);
}

static int build_soil_moisture_bytes(struct ucp_tlv_writer *w, uint8_t selector)
{
    struct soil_moisture_config_data d;
    bool enabled = false;
    uint8_t moisture_pct = 0U;
    bool has_data = false;
    int rc;
    memset(&d, 0, sizeof(d));
    d.channel_id = selector;
    d.operation = 0U;
    if (selector == 0xFFU) {
        rc = soil_moisture_get_global_with_presence(&enabled, &moisture_pct, &has_data);
        if (rc < 0) d.status = (uint8_t)WATERING_ERROR_INVALID_PARAM;
        else {
            d.enabled = enabled ? 1U : 0U;
            d.moisture_pct = moisture_pct;
            d.has_data = has_data ? 1U : 0U;
            d.status = 0U;
        }
        return build_raw_bytes_with_meta(w, &d, sizeof(d), g_ver_soil_moisture[0], 0xFFFFFFFFU, true);
    }
    if (selector >= WATERING_CHANNELS_COUNT) return -EINVAL;
    rc = soil_moisture_get_channel_override_with_presence(selector, &enabled, &moisture_pct, &has_data);
    if (rc < 0) d.status = (uint8_t)WATERING_ERROR_INVALID_PARAM;
    else {
        d.enabled = enabled ? 1U : 0U;
        d.moisture_pct = moisture_pct;
        d.has_data = has_data ? 1U : 0U;
        d.status = 0U;
    }
    return build_raw_bytes_with_meta(w, &d, sizeof(d), g_ver_soil_moisture[selector + 1U], selector, true);
}

static int build_interval_config_bytes(struct ucp_tlv_writer *w, uint8_t channel_id)
{
    struct interval_mode_config_data cfg;
    struct interval_mode_status_data st;
    uint8_t blob[sizeof(cfg) + sizeof(st)];
    if (channel_id >= WATERING_CHANNELS_COUNT) return -EINVAL;
    if (bt_interval_mode_get_config(channel_id, &cfg) != 0) return -EIO;
    memset(&st, 0, sizeof(st));
    (void)bt_interval_mode_get_status(&st);
    memcpy(blob, &cfg, sizeof(cfg));
    memcpy(blob + sizeof(cfg), &st, sizeof(st));
    return build_raw_bytes_with_meta(w, blob, sizeof(blob), g_ver_interval_cfg[channel_id], channel_id, true);
}

static int build_env_sensor_config_bytes(struct ucp_tlv_writer *w)
{
    bme280_config_t cfg;
    if (bme280_system_get_config(&cfg) != 0) {
        memset(&cfg, 0, sizeof(cfg));
    }
    return build_raw_bytes_with_meta(w, &cfg, sizeof(cfg), g_ver_env, 0U, false);
}

static int build_history_rain_storage(struct ucp_tlv_writer *w)
{
    rain_history_stats_t st;
    if (!ucp_rain_hist_stats_cache_get(&st)) {
        ucp_rain_hist_stats_cache_request_refresh();
        return -EAGAIN;
    }
    if (ucp_rain_hist_stats_cache_is_stale(k_uptime_get_32(), UCP_RAIN_HIST_STATS_CACHE_TTL_MS)) {
        ucp_rain_hist_stats_cache_request_refresh();
    }
    return build_raw_bytes_with_meta(w, &st, sizeof(st), g_ver_history, 1U, true);
}

static int build_history_env_storage(struct ucp_tlv_writer *w)
{
    env_history_stats_t st;
    if (!ucp_env_hist_stats_cache_get(&st)) {
        ucp_env_hist_stats_cache_request_refresh();
        return -EAGAIN;
    }
    if (ucp_env_hist_stats_cache_is_stale(k_uptime_get_32(), UCP_ENV_HIST_STATS_CACHE_TTL_MS)) {
        ucp_env_hist_stats_cache_request_refresh();
    }
    return build_raw_bytes_with_meta(w, &st, sizeof(st), g_ver_history, 2U, true);
}

static int build_alarm_status(struct ucp_tlv_writer *w)
{
    uint8_t code = 0U;
    uint16_t data = 0U;
    uint32_t ts = 0U;
#ifdef CONFIG_BT
    (void)bt_irrigation_alarm_get_state(&code, &data, &ts);
#else
    code = g_last_alarm_code;
    data = g_last_alarm_data;
    ts = 0U;
#endif
    return (ucp_tlvw_put_u8(w, ALARM_CODE_F, UCP_TLV_U8, code) &&
            ucp_tlvw_put_u16(w, ALARM_DATA_F, UCP_TLV_U16, data) &&
            ucp_tlvw_put_u32(w, ALARM_TS_F, UCP_TLV_U32, ts) &&
            ucp_tlvw_put_u8(w, ALARM_CLEARED_F, UCP_TLV_BOOL, (code == 0U) ? 1U : 0U) &&
            ucp_tlvw_put_u32(w, UCP_F_RESOURCE_VERSION, UCP_TLV_U32, g_ver_alarm) &&
            ucp_tlvw_put_u64(w, UCP_F_TIMESTAMP_UTC_MS, UCP_TLV_U64, now_utc_ms())) ? 0 : -ENOSPC;
}

static int build_flow_calibration_status_bytes(struct ucp_tlv_writer *w)
{
    struct calibration_data d;
    memset(&d, 0, sizeof(d));
#ifdef CONFIG_BT
    if (bt_irrigation_flow_calibration_get_state(&d.action, &d.pulses, &d.volume_ml, &d.pulses_per_liter) != 0) {
        d.action = 0U;
        d.pulses = 0U;
        d.volume_ml = 0U;
        d.pulses_per_liter = get_flow_calibration();
    }
#else
    d.action = 0U;
    d.pulses = 0U;
    d.volume_ml = 0U;
    d.pulses_per_liter = get_flow_calibration();
#endif
    return build_raw_bytes_with_meta(w, &d, sizeof(d), g_ver_flow_cal, 0U, false);
}

static int build_diag_storage_health(struct ucp_tlv_writer *w)
{
    pack_storage_stats_t pst = {0};
    storage_requirements_t hst = {0};
    bool pack_ready = pack_storage_is_ready();
    bool nvs_ready = nvs_config_is_ready();
    (void)watering_history_get_storage_info(&hst);
    if (pack_ready) {
        if (!ucp_pack_stats_cache_get(&pst)) {
            ucp_pack_stats_cache_request_refresh();
        } else if (ucp_pack_stats_cache_is_stale(k_uptime_get_32(), UCP_PACK_STATS_CACHE_TTL_MS)) {
            ucp_pack_stats_cache_request_refresh();
        }
    }
    return (ucp_tlvw_put_u8(w, DSTOR_NVS_READY, UCP_TLV_BOOL, nvs_ready ? 1U : 0U) &&
            ucp_tlvw_put_u8(w, DSTOR_PACK_READY, UCP_TLV_BOOL, pack_ready ? 1U : 0U) &&
            ucp_tlvw_put_u32(w, DSTOR_PACK_TOTAL_B, UCP_TLV_U32, pst.total_bytes) &&
            ucp_tlvw_put_u32(w, DSTOR_PACK_USED_B, UCP_TLV_U32, pst.used_bytes) &&
            ucp_tlvw_put_u32(w, DSTOR_PACK_FREE_B, UCP_TLV_U32, pst.free_bytes) &&
            ucp_tlvw_put_u32(w, DSTOR_HIST_TOTAL_KB, UCP_TLV_U32, hst.total_storage_kb) &&
            ucp_tlvw_put_u32(w, UCP_F_RESOURCE_VERSION, UCP_TLV_U32, g_ver_diag) &&
            ucp_tlvw_put_u64(w, UCP_F_TIMESTAMP_UTC_MS, UCP_TLV_U64, now_utc_ms())) ? 0 : -ENOSPC;
}

static int apply_custom_soil_bytes(const struct custom_soil_config_data *d)
{
    watering_error_t rc;
    if (!d) return -EINVAL;
    rc = bt_process_custom_soil_from_ble(d);
    return (rc == WATERING_SUCCESS) ? 0 : (int)rc;
}

static int apply_soil_moisture_bytes(const struct soil_moisture_config_data *d)
{
    if (!d) return -EINVAL;
    if (d->moisture_pct > 100U) return -ERANGE;
    if (d->channel_id == 0xFFU) {
        return soil_moisture_set_global(d->enabled != 0U, d->moisture_pct);
    }
    if (d->channel_id >= WATERING_CHANNELS_COUNT) return -EINVAL;
    return soil_moisture_set_channel_override(d->channel_id, d->enabled != 0U, d->moisture_pct);
}

static int apply_interval_config_bytes(const struct interval_mode_config_data *d)
{
    if (!d) return -EINVAL;
    return bt_interval_mode_set_config(d);
}

static int apply_env_sensor_config_bytes(const bme280_config_t *cfg)
{
    if (!cfg) return -EINVAL;
    return sensor_manager_configure_bme280(cfg);
}

static uint16_t alloc_stream_id(void)
{
    uint16_t id = g_next_stream_id++;
    if (id == 0U) {
        id = g_next_stream_id++;
    }
    if (id == 0U) {
        id = 1U;
        g_next_stream_id = 2U;
    }
    return id;
}

static void reset_out_stream(void)
{
    memset(&g_out_stream, 0, sizeof(g_out_stream));
}

static void reset_in_stream(void)
{
    memset(&g_in_stream, 0, sizeof(g_in_stream));
}

static bool peer_power_inhibit(void)
{
    return g_ota_reboot_pending;
}

static int emit_peer_power_change(uint8_t reason, ucp_origin_t origin)
{
    uint8_t delta_b[32];
    struct ucp_tlv_writer delta;
    ucp_tlvw_init(&delta, delta_b, sizeof(delta_b));
    (void)ucp_tlvw_put_u8(&delta, PEER_STATE_ON, UCP_TLV_BOOL, g_peer_power_on ? 1U : 0U);
    (void)ucp_tlvw_put_u8(&delta, PEER_STATE_INHIBIT, UCP_TLV_BOOL, peer_power_inhibit() ? 1U : 0U);
    (void)ucp_tlvw_put_u8(&delta, PEER_STATE_REASON, UCP_TLV_U8, reason);
    return emit_change(UCP_TOPIC_PEER_POWER, UCP_SVC_PEER_POWER, UCP_RES_PEER_POWER_STATE, ++g_ver_peer, origin, delta.buf, delta.len);
}

static int emit_diag_change(uint8_t reason_code)
{
    uint8_t delta_b[32];
    struct ucp_tlv_writer delta;
    ucp_tlvw_init(&delta, delta_b, sizeof(delta_b));
    (void)ucp_tlvw_put_u8(&delta, 1U, UCP_TLV_U8, reason_code);
    (void)ucp_tlvw_put_u32(&delta, DIAG_RX_OK, UCP_TLV_U32, g_diag.rx_ok_frames);
    (void)ucp_tlvw_put_u32(&delta, DIAG_STREAM_TIMEOUTS, UCP_TLV_U32, g_diag.stream_timeouts);
    (void)ucp_tlvw_put_u32(&delta, DIAG_STREAM_ABORTS, UCP_TLV_U32, g_diag.stream_aborts);
    return emit_change(UCP_TOPIC_DIAGNOSTICS, UCP_SVC_DIAG, UCP_RES_DIAG_COUNTERS, ++g_ver_diag, UCP_ORIGIN_LOCAL_FW, delta.buf, delta.len);
}

static void send_stream_abort_async(uint16_t stream_id, uint16_t status, uint8_t result)
{
    uint8_t b[48];
    struct ucp_tlv_writer w;
    ucp_tlvw_init(&w, b, sizeof(b));
    (void)ucp_tlvw_put_u16(&w, STRM_STREAM_ID, UCP_TLV_U16, stream_id);
    (void)ucp_tlvw_put_u8(&w, STRM_RESULT, UCP_TLV_U8, result);
    (void)send_tlv(UCP_MSG_STREAM_ABORT, 0U, 0U, status, &w, 0U);
}

static void maybe_stream_timeouts(void)
{
    uint32_t now = k_uptime_get_32();
    if (g_in_stream.active && (now - g_in_stream.last_activity_ms) > UCP_STREAM_IDLE_TIMEOUT_MS) {
        uint16_t sid = g_in_stream.id;
        reset_in_stream();
        g_diag.stream_timeouts++;
        send_stream_abort_async(sid, UCP_STATUS_TIMEOUT, STRM_RESULT_TIMEOUT);
        (void)emit_diag_change(2U);
    }
    if (g_out_stream.active && (now - g_out_stream.last_activity_ms) > UCP_STREAM_IDLE_TIMEOUT_MS) {
        uint16_t sid = g_out_stream.id;
        reset_out_stream();
        g_diag.stream_timeouts++;
        send_stream_abort_async(sid, UCP_STATUS_TIMEOUT, STRM_RESULT_TIMEOUT);
        (void)emit_diag_change(3U);
    }
}

static int collect_task_snapshot(struct ucp_task_snapshot *s)
{
    uint8_t pending = 0U;
    bool active = false;
    watering_task_t *cur = NULL;

    if (!s) return -EINVAL;
    memset(s, 0, sizeof(*s));
    s->channel_id = 0xFFU;

    if (watering_get_queue_status(&pending, &active) != WATERING_SUCCESS) {
        return -EIO;
    }
    s->pending = pending;
    s->active = active;
    s->paused = watering_is_current_task_paused();
    s->completed = (uint32_t)watering_get_completed_tasks_count();

    cur = watering_get_current_task();
    if (cur && cur->channel) {
        uint8_t cid;
        s->has_current = true;
        s->trigger = (uint8_t)cur->trigger_type;
        if (task_channel_id_from_ptr(cur->channel, &cid)) s->channel_id = cid;
        s->mode = (uint8_t)cur->channel->watering_event.watering_mode;
        if (cur->channel->watering_event.watering_mode == WATERING_BY_DURATION) {
            s->target = cur->channel->watering_event.watering.by_duration.duration_minutes;
        } else if (cur->channel->watering_event.watering_mode == WATERING_BY_VOLUME) {
            s->target = cur->channel->watering_event.watering.by_volume.volume_liters;
        }
    }

    s->valid = true;
    return 0;
}

static int build_task_status_from_snapshot(struct ucp_tlv_writer *w, const struct ucp_task_snapshot *s)
{
    if (!w || !s) return -EINVAL;
    return (ucp_tlvw_put_u8(w, TASK_ACTIVE, UCP_TLV_BOOL, s->active ? 1U : 0U) &&
            ucp_tlvw_put_u8(w, TASK_PAUSED, UCP_TLV_BOOL, s->paused ? 1U : 0U) &&
            ucp_tlvw_put_u8(w, TASK_HAS_CURRENT, UCP_TLV_BOOL, s->has_current ? 1U : 0U) &&
            ucp_tlvw_put_u8(w, TASK_PENDING, UCP_TLV_U8, s->pending) &&
            ucp_tlvw_put_u8(w, TASK_CUR_CH, UCP_TLV_U8, s->channel_id) &&
            ucp_tlvw_put_u8(w, TASK_TRIGGER, UCP_TLV_U8, s->trigger) &&
            ucp_tlvw_put_u8(w, TASK_MODE, UCP_TLV_U8, s->mode) &&
            ucp_tlvw_put_u32(w, TASK_TARGET, UCP_TLV_U32, s->target) &&
            ucp_tlvw_put_u32(w, TASK_COMPLETED, UCP_TLV_U32, s->completed) &&
            ucp_tlvw_put_u32(w, UCP_F_RESOURCE_VERSION, UCP_TLV_U32, g_ver_task) &&
            ucp_tlvw_put_u64(w, UCP_F_TIMESTAMP_UTC_MS, UCP_TLV_U64, now_utc_ms())) ? 0 : -ENOSPC;
}

static void maybe_task_status_event(void)
{
    struct ucp_task_snapshot cur;
    uint8_t delta_b[UCP_MAX_EVT_DELTA];
    struct ucp_tlv_writer delta;
    uint32_t now = k_uptime_get_32();
    uint32_t min_poll_ms = UCP_TASK_POLL_IDLE_MS;

    if (!topic_subscribed(UCP_TOPIC_TASK_CURRENT)) return;
    if (g_task_snapshot.valid &&
        (g_task_snapshot.active || g_task_snapshot.paused || g_task_snapshot.has_current || (g_task_snapshot.pending != 0U))) {
        min_poll_ms = UCP_TASK_POLL_ACTIVE_MS;
    }
    if ((now - g_last_poll_task_ms) < min_poll_ms) return;
    g_last_poll_task_ms = now;

    if (collect_task_snapshot(&cur) != 0) return;
    if (g_task_snapshot.valid &&
        memcmp(&g_task_snapshot, &cur, sizeof(cur)) == 0) {
        return;
    }

    g_task_snapshot = cur;
    ucp_tlvw_init(&delta, delta_b, sizeof(delta_b));
    if (build_task_status_from_snapshot(&delta, &cur) != 0) return;
    (void)emit_change(UCP_TOPIC_TASK_CURRENT, UCP_SVC_TASK, UCP_RES_TASK_STATUS, ++g_ver_task, UCP_ORIGIN_LOCAL_FW, delta.buf, delta.len);
}

static bool deferred_rescan_onboarding_topic_step(void)
{
    uint8_t dbuf[UCP_MAX_EVT_DELTA];
    struct ucp_tlv_writer dw;
    uint32_t sig = 0U;
    uint32_t now_ms;
    int rc;
    bool do_status = (g_onb_rescan_phase == 0U);

    if (do_status) {
        ucp_tlvw_init(&dw, dbuf, sizeof(dbuf));
        rc = build_onboarding_status_bytes(&dw);
        if (rc == 0 && tlv_raw_bytes_crc32c(dw.buf, dw.len, &sig) == 0) {
            if (g_sig_onboarding_status != 0U && g_sig_onboarding_status != sig) {
                g_ver_onboarding++;
                (void)emit_change_instance(UCP_TOPIC_ONBOARDING_STATUS, UCP_SVC_ONBOARDING_RESET, UCP_RES_ONB_STATUS,
                                           0U, g_ver_onboarding, UCP_ORIGIN_LOCAL_FW, dw.buf, dw.len);
            }
            g_sig_onboarding_status = sig;
        }
        g_onb_rescan_phase = 1U;
        return true; /* do overview in next slice */
    }

    now_ms = k_uptime_get_32();
    if (g_sig_onb_overview == 0U ||
        (now_ms - g_last_onb_overview_deferred_ms) >= UCP_DEFERRED_ONB_OVERVIEW_MIN_MS) {
        ucp_tlvw_init(&dw, dbuf, sizeof(dbuf));
        rc = build_onb_system_overview_bytes_fast(&dw);
        if (rc == 0 && tlv_raw_bytes_crc32c(dw.buf, dw.len, &sig) == 0) {
            if (g_sig_onb_overview != 0U && g_sig_onb_overview != sig) {
                g_ver_onb_overview++;
                (void)emit_change_instance(UCP_TOPIC_ONBOARDING_STATUS, UCP_SVC_ONBOARDING_RESET, UCP_RES_ONB_SYSTEM_OVERVIEW,
                                           0U, g_ver_onb_overview, UCP_ORIGIN_LOCAL_FW, dw.buf, dw.len);
            }
            g_sig_onb_overview = sig;
            g_last_onb_overview_deferred_ms = now_ms;
        }
    }
    g_onb_rescan_phase = 0U;
    return false;
}

static uint32_t ucp_thread_wait_timeout_ms(uint32_t now_ms)
{
    uint32_t wait_ms = UCP_THREAD_WAIT_MAX_MS;
    uint32_t pending_mask;
    uint32_t elapsed;

    if (!g_ready) {
        return 250U;
    }

    /* If work is pending but currently blocked by grace/quiet throttles, wake soon. */
    pending_mask = (uint32_t)atomic_get(&g_notify_rescan_mask);
    if (pending_mask != 0U) {
        wait_ms = MIN(wait_ms, 100U);
        if (g_session && g_sub_mask != 0U) {
            if (g_session_start_ms != 0U &&
                (now_ms - g_session_start_ms) < UCP_DEFERRED_RESCAN_POST_HELLO_GRACE_MS) {
                elapsed = now_ms - g_session_start_ms;
                wait_ms = MIN(wait_ms, UCP_DEFERRED_RESCAN_POST_HELLO_GRACE_MS - elapsed);
            }
            if ((now_ms - g_last_rx_ms) < UCP_DEFERRED_RESCAN_RX_QUIET_MS) {
                elapsed = now_ms - g_last_rx_ms;
                wait_ms = MIN(wait_ms, UCP_DEFERRED_RESCAN_RX_QUIET_MS - elapsed);
            }
        }
    }

    if (atomic_get(&g_pack_stats_refresh_pending) != 0) {
        uint32_t due = g_pack_stats_refresh_try_ms + UCP_PACK_STATS_REFRESH_MIN_INTERVAL_MS;
        wait_ms = MIN(wait_ms, (now_ms >= due) ? 1U : (due - now_ms));
    }
    if (atomic_get(&g_env_hist_stats_refresh_pending) != 0) {
        uint32_t due = g_env_hist_stats_refresh_try_ms + UCP_ENV_HIST_STATS_REFRESH_MIN_INTERVAL_MS;
        wait_ms = MIN(wait_ms, (now_ms >= due) ? 1U : (due - now_ms));
    }
    if (atomic_get(&g_rain_hist_stats_refresh_pending) != 0) {
        uint32_t due = g_rain_hist_stats_refresh_try_ms + UCP_RAIN_HIST_STATS_REFRESH_MIN_INTERVAL_MS;
        wait_ms = MIN(wait_ms, (now_ms >= due) ? 1U : (due - now_ms));
    }

    if (g_session) {
        uint32_t sess_due = g_last_rx_ms + UCP_SESSION_TIMEOUT_MS;
        wait_ms = MIN(wait_ms, (now_ms >= sess_due) ? 1U : (sess_due - now_ms));
    }

    if (g_in_stream.active) {
        uint32_t due = g_in_stream.last_activity_ms + UCP_STREAM_IDLE_TIMEOUT_MS;
        wait_ms = MIN(wait_ms, (now_ms >= due) ? 1U : (due - now_ms));
    }
    if (g_out_stream.active) {
        uint32_t due = g_out_stream.last_activity_ms + UCP_STREAM_IDLE_TIMEOUT_MS;
        wait_ms = MIN(wait_ms, (now_ms >= due) ? 1U : (due - now_ms));
    }

    if (wait_ms < 1U) wait_ms = 1U;
    if (wait_ms > UCP_THREAD_WAIT_MAX_MS) wait_ms = UCP_THREAD_WAIT_MAX_MS;
    return wait_ms;
}

static void deferred_rescan_system_config_topic(void)
{
    uint8_t dbuf[UCP_MAX_EVT_DELTA];
    struct ucp_tlv_writer dw;
    uint32_t sig = 0U;
    int rc;

    ucp_tlvw_init(&dw, dbuf, sizeof(dbuf));
    rc = build_system_config_bytes(&dw);
    if (rc == 0 && tlv_raw_bytes_crc32c(dw.buf, dw.len, &sig) == 0) {
        if (g_sig_system_cfg != 0U && g_sig_system_cfg != sig) {
            g_ver_syscfg++;
            (void)emit_change_instance(UCP_TOPIC_SYSTEM_CONFIG, UCP_SVC_DEVICE, UCP_RES_DEVICE_SYSTEM_CONFIG,
                                       0U, g_ver_syscfg, UCP_ORIGIN_LOCAL_FW, dw.buf, dw.len);
        }
        g_sig_system_cfg = sig;
    }
}

static void deferred_rescan_bulk_snapshot_topic(void)
{
    uint8_t dbuf[UCP_MAX_EVT_DELTA];
    struct ucp_tlv_writer dw;
    uint32_t sig = 0U;
    int rc;

    ucp_tlvw_init(&dw, dbuf, sizeof(dbuf));
    rc = build_bulk_snapshot_bytes(&dw);
    if (rc == 0 && tlv_raw_bytes_crc32c(dw.buf, dw.len, &sig) == 0) {
        if (g_sig_bulk_snapshot != 0U && g_sig_bulk_snapshot != sig) {
            g_ver_bulk++;
            (void)emit_change_instance(UCP_TOPIC_BULK_SNAPSHOT, UCP_SVC_DEVICE, UCP_RES_DEVICE_BULK_SNAPSHOT,
                                       0U, g_ver_bulk, UCP_ORIGIN_LOCAL_FW, dw.buf, dw.len);
        }
        g_sig_bulk_snapshot = sig;
    }
}

static bool deferred_rescan_channel_cfg_status_topic_step(void)
{
    uint8_t i;
    uint8_t dbuf[UCP_MAX_EVT_DELTA];
    struct ucp_tlv_writer dw;
    uint32_t sig = 0U;
    int rc;

    i = g_cfg_status_rescan_rr_idx;
    if (i >= WATERING_CHANNELS_COUNT) {
        i = 0U;
    }

    ucp_tlvw_init(&dw, dbuf, sizeof(dbuf));
    rc = build_onb_config_status_bytes_fast(&dw, i);
    if (rc == 0 && tlv_raw_bytes_crc32c(dw.buf, dw.len, &sig) == 0) {
        if (g_sig_cfg_status[i] != 0U && g_sig_cfg_status[i] != sig) {
            g_ver_cfg_status[i]++;
            (void)emit_change_instance(UCP_TOPIC_CHANNEL_CFG_STATUS, UCP_SVC_ONBOARDING_RESET, UCP_RES_ONB_CONFIG_STATUS,
                                       i, g_ver_cfg_status[i], UCP_ORIGIN_LOCAL_FW, dw.buf, dw.len);
        }
        g_sig_cfg_status[i] = sig;
    }

    i++;
    if (i >= WATERING_CHANNELS_COUNT) {
        g_cfg_status_rescan_rr_idx = 0U;
        return false;
    }

    g_cfg_status_rescan_rr_idx = i;
    return true;
}

static bool deferred_rescan_auto_calc_topic_step(void)
{
    uint8_t step = g_auto_calc_rescan_rr_idx;
    uint8_t dbuf[UCP_MAX_EVT_DELTA];
    struct ucp_tlv_writer dw;
    uint32_t sig = 0U;
    int rc = -EINVAL;

    if (step >= (uint8_t)(WATERING_CHANNELS_COUNT + 2U)) {
        step = 0U;
    }

    ucp_tlvw_init(&dw, dbuf, sizeof(dbuf));
    if (step == 0U) {
        rc = build_auto_calc_status_bytes(&dw, 0xFFU);
        if (rc == 0 && tlv_raw_bytes_crc32c(dw.buf, dw.len, &sig) == 0) {
            if (g_sig_auto_calc[0] != 0U && g_sig_auto_calc[0] != sig) {
                g_ver_auto_calc++;
                (void)emit_change_instance(UCP_TOPIC_AUTO_CALC_STATUS, UCP_SVC_DEVICE, UCP_RES_DEVICE_AUTO_CALC_STATUS,
                                           0xFFFFFFFFU, g_ver_auto_calc, UCP_ORIGIN_LOCAL_FW, dw.buf, dw.len);
            }
            g_sig_auto_calc[0] = sig;
        }
    } else if (step == 1U) {
        rc = build_auto_calc_control_bytes(&dw);
        if (rc == 0 && tlv_raw_bytes_crc32c(dw.buf, dw.len, &sig) == 0) {
            if (g_sig_auto_calc_ctl != 0U && g_sig_auto_calc_ctl != sig) {
                g_ver_auto_calc_ctl++;
                (void)emit_change(UCP_TOPIC_AUTO_CALC_STATUS, UCP_SVC_DEVICE, UCP_RES_DEVICE_AUTO_CALC_CONTROL,
                                  g_ver_auto_calc_ctl, UCP_ORIGIN_LOCAL_FW, dw.buf, dw.len);
            }
            g_sig_auto_calc_ctl = sig;
        }
    } else {
        uint8_t ch = (uint8_t)(step - 2U);
        rc = build_auto_calc_status_bytes(&dw, ch);
        if (rc == 0 && tlv_raw_bytes_crc32c(dw.buf, dw.len, &sig) == 0) {
            if (g_sig_auto_calc[ch + 1U] != 0U && g_sig_auto_calc[ch + 1U] != sig) {
                g_ver_auto_calc++;
                (void)emit_change_instance(UCP_TOPIC_AUTO_CALC_STATUS, UCP_SVC_DEVICE, UCP_RES_DEVICE_AUTO_CALC_STATUS,
                                           ch, g_ver_auto_calc, UCP_ORIGIN_LOCAL_FW, dw.buf, dw.len);
            }
            g_sig_auto_calc[ch + 1U] = sig;
        }
    }

    step++;
    if (step >= (uint8_t)(WATERING_CHANNELS_COUNT + 2U)) {
        g_auto_calc_rescan_rr_idx = 0U;
        return false;
    }
    g_auto_calc_rescan_rr_idx = step;
    return true;
}

static bool deferred_rescan_comp_status_topic_step(void)
{
    uint8_t i = g_comp_status_rescan_rr_idx;
    uint8_t dbuf[UCP_MAX_EVT_DELTA];
    struct ucp_tlv_writer dw;
    uint32_t sig = 0U;
    int rc;

    if (i >= WATERING_CHANNELS_COUNT) {
        i = 0U;
    }

    ucp_tlvw_init(&dw, dbuf, sizeof(dbuf));
    rc = build_comp_status_bytes(&dw, i);
    if (rc == 0 && tlv_raw_bytes_crc32c(dw.buf, dw.len, &sig) == 0) {
        if (g_sig_comp_status[i] != 0U && g_sig_comp_status[i] != sig) {
            g_ver_comp_status[i]++;
            (void)emit_change_instance(UCP_TOPIC_COMP_STATUS, UCP_SVC_COMPENSATION, UCP_RES_COMP_STATUS,
                                       i, g_ver_comp_status[i], UCP_ORIGIN_SENSOR, dw.buf, dw.len);
        }
        g_sig_comp_status[i] = sig;
    }

    i++;
    if (i >= WATERING_CHANNELS_COUNT) {
        g_comp_status_rescan_rr_idx = 0U;
        return false;
    }
    g_comp_status_rescan_rr_idx = i;
    return true;
}

static void maybe_domain_events(void)
{
    uint32_t now = k_uptime_get_32();
    bool sub_channel_state = topic_subscribed(UCP_TOPIC_CHANNEL_STATE);
    bool sub_channel_cfg = topic_subscribed(UCP_TOPIC_CHANNEL_CONFIG);
    bool sub_schedule_cfg = topic_subscribed(UCP_TOPIC_SCHEDULE_CONFIG);
    bool sub_custom_soil = topic_subscribed(UCP_TOPIC_CUSTOM_SOIL);
    bool sub_soil_moisture = topic_subscribed(UCP_TOPIC_SOIL_MOISTURE);
    bool sub_interval_mode = topic_subscribed(UCP_TOPIC_INTERVAL_MODE);
    bool sub_channel_cfg_status = topic_subscribed(UCP_TOPIC_CHANNEL_CFG_STATUS);
    bool sub_hydraulic = topic_subscribed(UCP_TOPIC_HYDRAULIC_STATUS);
    bool sub_comp_cfg = topic_subscribed(UCP_TOPIC_COMP_CONFIG);
    bool sub_packs = topic_subscribed(UCP_TOPIC_PACKS_CHANGED);
    bool need_cfg_poll = sub_channel_state || sub_channel_cfg || sub_schedule_cfg || sub_custom_soil ||
                         sub_soil_moisture || sub_interval_mode || sub_channel_cfg_status || sub_hydraulic ||
                         sub_comp_cfg || sub_packs;

    bool sub_system_status = topic_subscribed(UCP_TOPIC_SYSTEM_STATUS);
    bool sub_system_cfg = topic_subscribed(UCP_TOPIC_SYSTEM_CONFIG);
    bool sub_bulk = topic_subscribed(UCP_TOPIC_BULK_SNAPSHOT);
    bool sub_onboarding = topic_subscribed(UCP_TOPIC_ONBOARDING_STATUS);
    bool sub_reset_progress = topic_subscribed(UCP_TOPIC_RESET_PROGRESS);
    bool sub_auto_calc = topic_subscribed(UCP_TOPIC_AUTO_CALC_STATUS);
    bool sub_stats = topic_subscribed(UCP_TOPIC_STATISTICS);
    bool need_system_poll = sub_system_status || sub_system_cfg || sub_bulk || sub_onboarding ||
                            sub_reset_progress || sub_auto_calc || sub_stats;

    bool sub_flow = topic_subscribed(UCP_TOPIC_FLOW_LIVE);
    bool sub_flow_cal = topic_subscribed(UCP_TOPIC_FLOW_CALIBRATION);
    bool sub_rain = topic_subscribed(UCP_TOPIC_RAIN_LIVE);
    bool sub_env = topic_subscribed(UCP_TOPIC_ENV_LIVE);
    bool sub_rain_integration = topic_subscribed(UCP_TOPIC_RAIN_INTEGRATION);
    bool sub_comp_status = topic_subscribed(UCP_TOPIC_COMP_STATUS);
    bool need_sensors_poll = sub_flow || sub_flow_cal || sub_rain || sub_env || sub_rain_integration || sub_comp_status;

#define UCP_CHECK_RAW_CHANGED(_build_call, _sig_slot, _ver_slot, _topic, _svc, _res, _inst, _origin) do { \
        uint8_t _dbuf[UCP_MAX_EVT_DELTA]; \
        struct ucp_tlv_writer _dw; \
        uint32_t _sig = 0U; \
        int _rc; \
        ucp_tlvw_init(&_dw, _dbuf, sizeof(_dbuf)); \
        _rc = (_build_call); \
        if (_rc == 0 && tlv_raw_bytes_crc32c(_dw.buf, _dw.len, &_sig) == 0) { \
            if ((_sig_slot) != 0U && (_sig_slot) != _sig) { \
                (_ver_slot)++; \
                (void)emit_change_instance((_topic), (_svc), (_res), (_inst), (_ver_slot), (_origin), _dw.buf, _dw.len); \
            } \
            (_sig_slot) = _sig; \
        } \
    } while (0)

    if (need_cfg_poll && ((now - g_last_poll_cfg_ms) >= 250U)) {
        for (uint8_t i = 0U; i < WATERING_CHANNELS_COUNT; i++) {
            struct ucp_channel_state_snapshot st;
            struct ucp_channel_config_snapshot cfg;
            struct ucp_schedule_snapshot sch;
            if (sub_channel_state && collect_channel_state_snapshot(i, &st) == 0) {
                if (g_ch_state_snap[i].valid && memcmp(&g_ch_state_snap[i], &st, sizeof(st)) != 0) {
                    uint8_t d[16]; struct ucp_tlv_writer dw; ucp_tlvw_init(&dw, d, sizeof(d));
                    (void)ucp_tlvw_put_u8(&dw, RES_CH_ID, UCP_TLV_U8, i);
                    (void)ucp_tlvw_put_u8(&dw, RES_CH_ACTIVE, UCP_TLV_BOOL, st.is_active ? 1U : 0U);
                    g_ver_channel_state[i]++;
                    (void)emit_change_instance(UCP_TOPIC_CHANNEL_STATE, UCP_SVC_CHANNEL, UCP_RES_CHANNEL_STATE, i,
                                               g_ver_channel_state[i], UCP_ORIGIN_LOCAL_FW, dw.buf, dw.len);
                }
                g_ch_state_snap[i] = st;
                if (!g_ch_state_snap[i].valid) g_ch_state_snap[i].valid = true;
            }
            if (sub_channel_cfg && collect_channel_config_snapshot(i, &cfg) == 0) {
                if (g_ch_cfg_snap[i].valid && memcmp(&g_ch_cfg_snap[i], &cfg, sizeof(cfg)) != 0) {
                    uint8_t d[80]; struct ucp_tlv_writer dw; ucp_tlvw_init(&dw, d, sizeof(d));
                    (void)ucp_tlvw_put_u8(&dw, RES_CH_ID, UCP_TLV_U8, i);
                    (void)ucp_tlvw_put_u16(&dw, RES_CH_PLANT_ID, UCP_TLV_U16, cfg.plant_id);
                    (void)ucp_tlvw_put_bytes(&dw, RES_CH_NAME, UCP_TLV_UTF8, (const uint8_t *)cfg.name, (uint16_t)strnlen(cfg.name, sizeof(cfg.name)));
                    g_ver_channel_cfg[i]++;
                    (void)emit_change_instance(UCP_TOPIC_CHANNEL_CONFIG, UCP_SVC_CHANNEL, UCP_RES_CHANNEL_CONFIG, i,
                                               g_ver_channel_cfg[i], UCP_ORIGIN_LOCAL_FW, dw.buf, dw.len);
                }
                g_ch_cfg_snap[i] = cfg;
            }
            if (sub_schedule_cfg && collect_schedule_snapshot(i, &sch) == 0) {
                if (g_sched_snap[i].valid && memcmp(&g_sched_snap[i], &sch, sizeof(sch)) != 0) {
                    uint8_t d[48]; struct ucp_tlv_writer dw; ucp_tlvw_init(&dw, d, sizeof(d));
                    (void)ucp_tlvw_put_u8(&dw, RES_CH_ID, UCP_TLV_U8, i);
                    (void)ucp_tlvw_put_u8(&dw, RES_SCH_TYPE, UCP_TLV_U8, (uint8_t)sch.ev.schedule_type);
                    (void)ucp_tlvw_put_u8(&dw, RES_SCH_WATER_MODE, UCP_TLV_U8, (uint8_t)sch.ev.watering_mode);
                    (void)ucp_tlvw_put_u8(&dw, RES_SCH_AUTO_ENABLED, UCP_TLV_BOOL, sch.ev.auto_enabled ? 1U : 0U);
                    g_ver_schedule_cfg[i]++;
                    (void)emit_change_instance(UCP_TOPIC_SCHEDULE_CONFIG, UCP_SVC_SCHEDULE, UCP_RES_SCHEDULE_CONFIG, i,
                                               g_ver_schedule_cfg[i], UCP_ORIGIN_LOCAL_FW, dw.buf, dw.len);
                }
                g_sched_snap[i] = sch;
            }

            if (sub_custom_soil) {
                UCP_CHECK_RAW_CHANGED(build_custom_soil_bytes(&_dw, i),
                                      g_sig_custom_soil[i], g_ver_custom_soil[i],
                                      UCP_TOPIC_CUSTOM_SOIL, UCP_SVC_CHANNEL, UCP_RES_CHANNEL_CUSTOM_SOIL,
                                      i, UCP_ORIGIN_LOCAL_FW);
            }

            if (sub_soil_moisture) {
                UCP_CHECK_RAW_CHANGED(build_soil_moisture_bytes(&_dw, i),
                                      g_sig_soil_moisture[i + 1U], g_ver_soil_moisture[i + 1U],
                                      UCP_TOPIC_SOIL_MOISTURE, UCP_SVC_CHANNEL, UCP_RES_CHANNEL_SOIL_MOISTURE,
                                      i, UCP_ORIGIN_LOCAL_FW);
            }

            if (sub_interval_mode) {
                UCP_CHECK_RAW_CHANGED(build_interval_config_bytes(&_dw, i),
                                      g_sig_interval_cfg[i], g_ver_interval_cfg[i],
                                      UCP_TOPIC_INTERVAL_MODE, UCP_SVC_CHANNEL, UCP_RES_CHANNEL_INTERVAL_CONFIG,
                                      i, UCP_ORIGIN_LOCAL_FW);
            }

            if (sub_channel_cfg_status) {
                UCP_CHECK_RAW_CHANGED(build_onb_config_status_bytes(&_dw, i),
                                      g_sig_cfg_status[i], g_ver_cfg_status[i],
                                      UCP_TOPIC_CHANNEL_CFG_STATUS, UCP_SVC_ONBOARDING_RESET, UCP_RES_ONB_CONFIG_STATUS,
                                      i, UCP_ORIGIN_LOCAL_FW);
            }

            if (sub_hydraulic) {
                UCP_CHECK_RAW_CHANGED(build_hydraulic_status_bytes(&_dw, i),
                                      g_sig_hydraulic[i], g_ver_hydraulic[i],
                                      UCP_TOPIC_HYDRAULIC_STATUS, UCP_SVC_FLOW, UCP_RES_FLOW_HYDRAULIC_STATUS,
                                      i, UCP_ORIGIN_LOCAL_FW);
            }

            if (sub_comp_cfg) {
                UCP_CHECK_RAW_CHANGED(build_comp_channel_cfg_bytes(&_dw, i),
                                      g_sig_comp_cfg[i], g_ver_comp_cfg[i],
                                      UCP_TOPIC_COMP_CONFIG, UCP_SVC_COMPENSATION, UCP_RES_COMP_CHANNEL_CONFIG,
                                      i, UCP_ORIGIN_LOCAL_FW);
            }

        }

        if (sub_soil_moisture) {
            UCP_CHECK_RAW_CHANGED(build_soil_moisture_bytes(&_dw, 0xFFU),
                                  g_sig_soil_moisture[0], g_ver_soil_moisture[0],
                                  UCP_TOPIC_SOIL_MOISTURE, UCP_SVC_CHANNEL, UCP_RES_CHANNEL_SOIL_MOISTURE,
                                  0xFFFFFFFFU, UCP_ORIGIN_LOCAL_FW);
        }

        /* Packs stats can take >1s and must not run in deferred-rescan (event-driven) path.
         * Keep it only for explicit broad polling mode. Packs APIs remain readable via GET/STREAM.
         */
        if (sub_packs && g_poll_notifies_enabled &&
            (now - g_last_poll_packs_ms) >= 5000U && pack_storage_is_ready()) {
            pack_storage_stats_t st = {0};
            uint32_t prev_change = g_last_pack_change_counter;
            if (ucp_pack_stats_cache_refresh() && ucp_pack_stats_cache_get(&st)) {
                if (prev_change != 0U && st.change_counter != prev_change) {
                    uint8_t d[24]; struct ucp_tlv_writer dw; ucp_tlvw_init(&dw, d, sizeof(d));
                    (void)ucp_tlvw_put_u32(&dw, PACKS_CHANGE_COUNTER, UCP_TLV_U32, st.change_counter);
                    g_ver_packs++;
                    (void)emit_change(UCP_TOPIC_PACKS_CHANGED, UCP_SVC_PACKS, UCP_RES_PACKS_STATS, g_ver_packs, UCP_ORIGIN_LOCAL_FW, dw.buf, dw.len);
                }
            }
            g_last_poll_packs_ms = now;
        }
        g_last_poll_cfg_ms = now;
    }

    if (need_system_poll && ((now - g_last_poll_system_ms) >= 500U)) {
        struct ucp_system_snapshot sys;
        if (sub_system_status && collect_system_snapshot(&sys) == 0) {
            if (g_sys_snap.valid && memcmp(&g_sys_snap, &sys, sizeof(sys)) != 0) {
                uint8_t d[24]; struct ucp_tlv_writer dw; ucp_tlvw_init(&dw, d, sizeof(d));
                (void)ucp_tlvw_put_u8(&dw, SYS_W_STATUS, UCP_TLV_U8, sys.status);
                (void)ucp_tlvw_put_u8(&dw, SYS_W_STATE, UCP_TLV_U8, sys.state);
                (void)ucp_tlvw_put_u8(&dw, SYS_PWR_MODE, UCP_TLV_U8, sys.power_mode);
                (void)emit_change(UCP_TOPIC_SYSTEM_STATUS, UCP_SVC_DEVICE, UCP_RES_DEVICE_SYSTEM_STATUS, ++g_ver_sys, UCP_ORIGIN_LOCAL_FW, dw.buf, dw.len);
            }
            g_sys_snap = sys;
        }

        if (sub_system_cfg) {
            UCP_CHECK_RAW_CHANGED(build_system_config_bytes(&_dw),
                                  g_sig_system_cfg, g_ver_syscfg,
                                  UCP_TOPIC_SYSTEM_CONFIG, UCP_SVC_DEVICE, UCP_RES_DEVICE_SYSTEM_CONFIG,
                                  0U, UCP_ORIGIN_LOCAL_FW);
        }

        if (sub_bulk) {
            UCP_CHECK_RAW_CHANGED(build_bulk_snapshot_bytes(&_dw),
                                  g_sig_bulk_snapshot, g_ver_bulk,
                                  UCP_TOPIC_BULK_SNAPSHOT, UCP_SVC_DEVICE, UCP_RES_DEVICE_BULK_SNAPSHOT,
                                  0U, UCP_ORIGIN_LOCAL_FW);
        }

        if (sub_onboarding) {
            UCP_CHECK_RAW_CHANGED(build_onboarding_status_bytes(&_dw),
                                  g_sig_onboarding_status, g_ver_onboarding,
                                  UCP_TOPIC_ONBOARDING_STATUS, UCP_SVC_ONBOARDING_RESET, UCP_RES_ONB_STATUS,
                                  0U, UCP_ORIGIN_LOCAL_FW);

            UCP_CHECK_RAW_CHANGED(build_onb_system_overview_bytes(&_dw),
                                  g_sig_onb_overview, g_ver_onb_overview,
                                  UCP_TOPIC_ONBOARDING_STATUS, UCP_SVC_ONBOARDING_RESET, UCP_RES_ONB_SYSTEM_OVERVIEW,
                                  0U, UCP_ORIGIN_LOCAL_FW);
        }

        if (sub_reset_progress) {
            UCP_CHECK_RAW_CHANGED(build_reset_control_bytes(&_dw),
                                  g_sig_reset_control, g_ver_reset_ctl,
                                  UCP_TOPIC_RESET_PROGRESS, UCP_SVC_ONBOARDING_RESET, UCP_RES_ONB_RESET_CONTROL,
                                  0U, UCP_ORIGIN_LOCAL_FW);
        }

        if (sub_auto_calc) {
            UCP_CHECK_RAW_CHANGED(build_auto_calc_status_bytes(&_dw, 0xFFU),
                                  g_sig_auto_calc[0], g_ver_auto_calc,
                                  UCP_TOPIC_AUTO_CALC_STATUS, UCP_SVC_DEVICE, UCP_RES_DEVICE_AUTO_CALC_STATUS,
                                  0xFFFFFFFFU, UCP_ORIGIN_LOCAL_FW);

            UCP_CHECK_RAW_CHANGED(build_auto_calc_control_bytes(&_dw),
                                  g_sig_auto_calc_ctl, g_ver_auto_calc_ctl,
                                  UCP_TOPIC_AUTO_CALC_STATUS, UCP_SVC_DEVICE, UCP_RES_DEVICE_AUTO_CALC_CONTROL,
                                  0U, UCP_ORIGIN_LOCAL_FW);
        }

        for (uint8_t i = 0U; i < WATERING_CHANNELS_COUNT; i++) {
            if (sub_stats) {
                UCP_CHECK_RAW_CHANGED(build_statistics_bytes(&_dw, i),
                                      g_sig_stats[i], g_ver_stats[i],
                                      UCP_TOPIC_STATISTICS, UCP_SVC_DEVICE, UCP_RES_DEVICE_STATISTICS,
                                      i, UCP_ORIGIN_LOCAL_FW);
            }

            if (sub_auto_calc) {
                UCP_CHECK_RAW_CHANGED(build_auto_calc_status_bytes(&_dw, i),
                                      g_sig_auto_calc[i + 1U], g_ver_auto_calc,
                                      UCP_TOPIC_AUTO_CALC_STATUS, UCP_SVC_DEVICE, UCP_RES_DEVICE_AUTO_CALC_STATUS,
                                      i, UCP_ORIGIN_LOCAL_FW);
            }
        }
        g_last_poll_system_ms = now;
    }

    if (need_sensors_poll && ((now - g_last_poll_sensors_ms) >= 1000U)) {
        struct ucp_flow_snapshot flow;
        struct ucp_rain_snapshot rain;
        struct ucp_env_snapshot env;

        if (sub_flow && collect_flow_snapshot(&flow) == 0) {
            if (g_flow_snap.valid && memcmp(&g_flow_snap, &flow, sizeof(flow)) != 0) {
                uint8_t d[32]; struct ucp_tlv_writer dw; ucp_tlvw_init(&dw, d, sizeof(d));
                (void)ucp_tlvw_put_u32(&dw, FLOW_PULSE_COUNT, UCP_TLV_U32, flow.pulses);
                (void)ucp_tlvw_put_u32(&dw, FLOW_RATE_PPS, UCP_TLV_U32, flow.rate_pps);
                (void)emit_change(UCP_TOPIC_FLOW_LIVE, UCP_SVC_FLOW, UCP_RES_FLOW_STATUS, ++g_ver_flow, UCP_ORIGIN_SENSOR, dw.buf, dw.len);
            }
            g_flow_snap = flow;
        }
        if (sub_flow_cal) {
            UCP_CHECK_RAW_CHANGED(build_flow_calibration_status_bytes(&_dw),
                                  g_sig_flow_cal_status, g_ver_flow_cal,
                                  UCP_TOPIC_FLOW_CALIBRATION, UCP_SVC_FLOW, UCP_RES_FLOW_CALIBRATION_STATUS,
                                  0U, UCP_ORIGIN_LOCAL_FW);
        }
        if (sub_rain && collect_rain_snapshot(&rain) == 0) {
            if (g_rain_snap.valid && memcmp(&g_rain_snap, &rain, sizeof(rain)) != 0) {
                uint8_t d[64]; struct ucp_tlv_writer dw; ucp_tlvw_init(&dw, d, sizeof(d));
                (void)ucp_tlvw_put_f32(&dw, RAIN_CUR_HOUR_MM, rain.sensor.current_hour_mm);
                (void)ucp_tlvw_put_f32(&dw, RAIN_HOURLY_RATE_MMH, rain.sensor.hourly_rate_mm);
                (void)ucp_tlvw_put_u8(&dw, RAIN_STATUS, UCP_TLV_U8, (uint8_t)rain.sensor.status);
                (void)ucp_tlvw_put_f32(&dw, RAIN_LAST_24H_MM, rain.last24h_mm);
                (void)emit_change(UCP_TOPIC_RAIN_LIVE, UCP_SVC_RAIN, UCP_RES_RAIN_STATUS, ++g_ver_rain, UCP_ORIGIN_SENSOR, dw.buf, dw.len);
            }
            g_rain_snap = rain;
        }
        if (sub_env && collect_env_snapshot(&env) == 0) {
            if (g_env_snap.valid && memcmp(&g_env_snap, &env, sizeof(env)) != 0) {
                uint8_t d[64]; struct ucp_tlv_writer dw; ucp_tlvw_init(&dw, d, sizeof(d));
                (void)ucp_tlvw_put_f32(&dw, ENV_TEMP_C, env.env.current.temperature);
                (void)ucp_tlvw_put_f32(&dw, ENV_HUMIDITY_PCT, env.env.current.humidity);
                (void)ucp_tlvw_put_f32(&dw, ENV_PRESSURE_HPA, env.env.current.pressure);
                (void)ucp_tlvw_put_u8(&dw, ENV_VALID, UCP_TLV_BOOL, env.env.current.valid ? 1U : 0U);
                (void)emit_change(UCP_TOPIC_ENV_LIVE, UCP_SVC_ENVIRONMENT, UCP_RES_ENV_CURRENT, ++g_ver_env, UCP_ORIGIN_SENSOR, dw.buf, dw.len);
            }
            g_env_snap = env;
        }

        if (sub_rain_integration) {
            UCP_CHECK_RAW_CHANGED(build_rain_integration_bytes(&_dw),
                                  g_sig_rain_integration, g_ver_rain_integration,
                                  UCP_TOPIC_RAIN_INTEGRATION, UCP_SVC_COMPENSATION, UCP_RES_COMP_RAIN_INTEGRATION,
                                  0U, UCP_ORIGIN_SENSOR);
        }

        for (uint8_t i = 0U; i < WATERING_CHANNELS_COUNT; i++) {
            if (sub_comp_status) {
                UCP_CHECK_RAW_CHANGED(build_comp_status_bytes(&_dw, i),
                                      g_sig_comp_status[i], g_ver_comp_status[i],
                                      UCP_TOPIC_COMP_STATUS, UCP_SVC_COMPENSATION, UCP_RES_COMP_STATUS,
                                      i, UCP_ORIGIN_SENSOR);
            }
        }
        g_last_poll_sensors_ms = now;
    }

#undef UCP_CHECK_RAW_CHANGED
}

static int stream_send_all(void)
{
    while (g_out_stream.active && g_out_stream.offset < g_out_stream.total_size) {
        uint8_t b[UCP_MAX_PAYLOAD_V1];
        struct ucp_tlv_writer w;
        uint16_t remain = (uint16_t)(g_out_stream.total_size - g_out_stream.offset);
        uint16_t chunk = (remain > g_out_stream.chunk_size) ? g_out_stream.chunk_size : remain;
        ucp_tlvw_init(&w, b, sizeof(b));
        (void)ucp_tlvw_put_u16(&w, STRM_STREAM_ID, UCP_TLV_U16, g_out_stream.id);
        (void)ucp_tlvw_put_u16(&w, STRM_CHUNK_INDEX, UCP_TLV_U16, g_out_stream.chunk_index);
        (void)ucp_tlvw_put_u32(&w, STRM_OFFSET, UCP_TLV_U32, g_out_stream.offset);
        (void)ucp_tlvw_put_bytes(&w, STRM_DATA, UCP_TLV_BYTES, &g_out_stream.data[g_out_stream.offset], chunk);
        if (send_tlv(UCP_MSG_STREAM_DATA, 0U, 0U, UCP_STATUS_OK, &w, 0U) != 0) {
            return -EIO;
        }
        g_out_stream.offset = (uint16_t)(g_out_stream.offset + chunk);
        g_out_stream.chunk_index++;
        g_out_stream.last_activity_ms = k_uptime_get_32();
    }

    if (g_out_stream.active) {
        uint8_t b[96];
        struct ucp_tlv_writer w;
        uint32_t crc = ucp_crc32c(g_out_stream.data, g_out_stream.total_size);
        ucp_tlvw_init(&w, b, sizeof(b));
        (void)ucp_tlvw_put_u16(&w, STRM_STREAM_ID, UCP_TLV_U16, g_out_stream.id);
        (void)ucp_tlvw_put_u32(&w, STRM_TOTAL_SIZE, UCP_TLV_U32, g_out_stream.total_size);
        (void)ucp_tlvw_put_u32(&w, STRM_FINAL_CRC32C, UCP_TLV_U32, crc);
        (void)ucp_tlvw_put_u8(&w, STRM_RESULT, UCP_TLV_U8, STRM_RESULT_OK);
        (void)send_tlv(UCP_MSG_STREAM_END, 0U, 0U, UCP_STATUS_OK, &w, 0U);
        reset_out_stream();
    }
    return 0;
}

static int stream_prepare_diag_snapshot(void)
{
    timezone_config_t tz;
    rtc_datetime_t dt;
    int n = 0;
    uint32_t utc = timezone_get_unix_utc();

    reset_out_stream();
    g_out_stream.active = true;
    g_out_stream.id = alloc_stream_id();
    g_out_stream.purpose = STRM_PURPOSE_DIAG_SNAPSHOT;
    g_out_stream.chunk_size = UCP_STREAM_CHUNK_MAX;
    g_out_stream.opened_ms = k_uptime_get_32();
    g_out_stream.last_activity_ms = g_out_stream.opened_ms;

    n += snprintk((char *)&g_out_stream.data[n], sizeof(g_out_stream.data) - n,
                  "ucp=v1\nboot_id=%llu\nfw=%s\nuptime_ms=%u\nunix_utc=%u\nsession=%u\n",
                  (unsigned long long)g_boot_id, APP_VERSION_EXTENDED_STRING, k_uptime_get_32(),
                  utc, g_session_id);

    if (timezone_get_config(&tz) == 0 && n < (int)sizeof(g_out_stream.data)) {
        n += snprintk((char *)&g_out_stream.data[n], sizeof(g_out_stream.data) - n,
                      "tz.utc_offset_min=%d\ntz.dst_enabled=%u\ntz.dst_offset_min=%d\n",
                      tz.utc_offset_minutes, tz.dst_enabled, tz.dst_offset_minutes);
    }

    if (rtc_datetime_get(&dt) == 0 && n < (int)sizeof(g_out_stream.data)) {
        n += snprintk((char *)&g_out_stream.data[n], sizeof(g_out_stream.data) - n,
                      "rtc=%04u-%02u-%02uT%02u:%02u:%02u dow=%u\n",
                      dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second, dt.day_of_week);
    } else if (n < (int)sizeof(g_out_stream.data)) {
        n += snprintk((char *)&g_out_stream.data[n], sizeof(g_out_stream.data) - n, "rtc=unavailable\n");
    }

    if (n < 0) n = 0;
    if (n > (int)sizeof(g_out_stream.data)) n = sizeof(g_out_stream.data);
    g_out_stream.total_size = (uint16_t)n;
    return 0;
}

static int stream_prepare_schema_text(void)
{
    static const char schema_text[] =
        "services:\n"
        "  0x0001 session\n"
        "  0x0010 device (resource 0x0001 system_status GET)\n"
        "  0x0018 rtc_tz (resource 0x0001 timezone_config GET/SET, 0x0002 rtc_time GET/SET)\n"
        "  0x001C ota (cmd 0x0001 preflight, 0x0002 enter_recovery, 0x0003 post_check)\n"
        "topics:\n"
        "  1 system.status\n"
        "  3 rtc_tz.changed\n"
        "  6 ota.transition\n";

    reset_out_stream();
    g_out_stream.active = true;
    g_out_stream.id = alloc_stream_id();
    g_out_stream.purpose = STRM_PURPOSE_SCHEMA_TEXT;
    g_out_stream.chunk_size = UCP_STREAM_CHUNK_MAX;
    g_out_stream.opened_ms = k_uptime_get_32();
    g_out_stream.last_activity_ms = g_out_stream.opened_ms;
    g_out_stream.total_size = MIN((size_t)sizeof(schema_text) - 1U, sizeof(g_out_stream.data));
    memcpy(g_out_stream.data, schema_text, g_out_stream.total_size);
    return 0;
}

static void schema_catalog_copy_line_window(const char *line, uint16_t page_offset, size_t *total_len,
                                            uint8_t *dst, size_t *dst_len, size_t dst_cap)
{
    size_t line_len;
    size_t line_start;
    size_t start_in_line;
    size_t copy_len;

    if (!line || !total_len || !dst || !dst_len) return;
    line_len = strlen(line);
    line_start = *total_len;
    *total_len += line_len;
    if (*dst_len >= dst_cap) return;
    if ((line_start + line_len) <= page_offset) return;
    start_in_line = (page_offset > line_start) ? (page_offset - line_start) : 0U;
    copy_len = MIN(line_len - start_in_line, dst_cap - *dst_len);
    memcpy(&dst[*dst_len], &line[start_in_line], copy_len);
    *dst_len += copy_len;
}

static int stream_prepare_schema_catalog(uint16_t page_offset, uint32_t *total_catalog_len_out, uint16_t *item_count_out)
{
    size_t n = 0U;
    size_t total_len = 0U;
    uint16_t item_count = 0U;
    char line[192];

    reset_out_stream();
    g_out_stream.active = true;
    g_out_stream.id = alloc_stream_id();
    g_out_stream.purpose = STRM_PURPOSE_SCHEMA_CATALOG;
    g_out_stream.chunk_size = UCP_STREAM_CHUNK_MAX;
    g_out_stream.opened_ms = k_uptime_get_32();
    g_out_stream.last_activity_ms = g_out_stream.opened_ms;

    schema_catalog_copy_line_window("# UCP schema catalog v1\n", page_offset, &total_len, g_out_stream.data, &n, sizeof(g_out_stream.data));
    schema_catalog_copy_line_window("# format: type,ids,name,meta\n", page_offset, &total_len, g_out_stream.data, &n, sizeof(g_out_stream.data));

    for (size_t i = 0U; i < (sizeof(g_schema_resources) / sizeof(g_schema_resources[0])); i++) {
        const struct ucp_schema_resource_desc *d = &g_schema_resources[i];
        int ln = snprintk(line, sizeof(line),
                          "R svc=0x%04X res=0x%04X name=%s kind=%u acc=0x%02X topic=%u fields=%u desc=%s\n",
                          d->svc, d->res, d->name, d->kind, d->access_mask, d->topic_id, d->field_count, d->desc);
        if (ln < 0) continue;
        line[MIN((size_t)ln, sizeof(line) - 1U)] = '\0';
        schema_catalog_copy_line_window(line, page_offset, &total_len, g_out_stream.data, &n, sizeof(g_out_stream.data));
        item_count++;
    }
    for (size_t i = 0U; i < (sizeof(g_schema_stream_purposes) / sizeof(g_schema_stream_purposes[0])); i++) {
        const struct ucp_schema_stream_purpose_desc *d = &g_schema_stream_purposes[i];
        int ln = snprintk(line, sizeof(line),
                          "S purpose=%u name=%s dir_mask=0x%02X max_total=%u desc=%s\n",
                          d->purpose, d->name, d->dir_mask, d->max_total, d->desc);
        if (ln < 0) continue;
        line[MIN((size_t)ln, sizeof(line) - 1U)] = '\0';
        schema_catalog_copy_line_window(line, page_offset, &total_len, g_out_stream.data, &n, sizeof(g_out_stream.data));
        item_count++;
    }

    g_out_stream.total_size = (uint16_t)n;
    if (total_catalog_len_out) *total_catalog_len_out = (uint32_t)total_len;
    if (item_count_out) *item_count_out = item_count;
    return 0;
}

static int stream_prepare_history_page(uint8_t channel_id, uint16_t page_index)
{
    uint16_t count = DETAILED_EVENTS_PER_CHANNEL;
    size_t n = 0U;
    watering_error_t rc;

    if (channel_id >= WATERING_CHANNELS_COUNT) return -EINVAL;

    rc = watering_history_query_page(channel_id, page_index, g_hist_stream_events, &count, g_hist_stream_ts);
    if (rc != WATERING_SUCCESS) return -EIO;

    reset_out_stream();
    g_out_stream.active = true;
    g_out_stream.id = alloc_stream_id();
    g_out_stream.purpose = STRM_PURPOSE_HISTORY_PAGE;
    g_out_stream.chunk_size = UCP_STREAM_CHUNK_MAX;
    g_out_stream.opened_ms = k_uptime_get_32();
    g_out_stream.last_activity_ms = g_out_stream.opened_ms;

    if (sizeof(g_out_stream.data) < 9U) return -ENOSPC;
    memcpy(&g_out_stream.data[n], "UHP1", 4); n += 4;
    g_out_stream.data[n++] = channel_id;
    ucp_tlv_wr_u16_le(&g_out_stream.data[n], page_index); n += 2;
    ucp_tlv_wr_u16_le(&g_out_stream.data[n], count); n += 2;

    for (uint16_t i = 0U; i < count; i++) {
        const history_event_t *ev = &g_hist_stream_events[i];
        if ((n + 12U) > sizeof(g_out_stream.data)) break;
        ucp_tlv_wr_u32_le(&g_out_stream.data[n], g_hist_stream_ts[i]); n += 4;
        g_out_stream.data[n++] = ev->dt_delta;
        g_out_stream.data[n++] = ev->flags.packed;
        ucp_tlv_wr_u16_le(&g_out_stream.data[n], ev->target_ml); n += 2;
        ucp_tlv_wr_u16_le(&g_out_stream.data[n], ev->actual_ml); n += 2;
        ucp_tlv_wr_u16_le(&g_out_stream.data[n], ev->avg_flow_ml_s); n += 2;
    }

    g_out_stream.total_size = (uint16_t)n;
    return 0;
}

static int stream_prepare_packs_list(uint16_t offset, bool plant_list)
{
    size_t n = 0U;
    uint16_t count = 0U;

    reset_out_stream();
    g_out_stream.active = true;
    g_out_stream.id = alloc_stream_id();
    g_out_stream.purpose = plant_list ? STRM_PURPOSE_PACKS_PLANTS_LIST : STRM_PURPOSE_PACKS_LIST;
    g_out_stream.chunk_size = UCP_STREAM_CHUNK_MAX;
    g_out_stream.opened_ms = k_uptime_get_32();
    g_out_stream.last_activity_ms = g_out_stream.opened_ms;

    if (!pack_storage_is_ready()) return -ENODEV;

    if (!plant_list) {
        pack_result_t pr = pack_storage_list_packs(g_pack_stream_entries, UCP_PACK_STREAM_PAGE_MAX, &count, offset);
        if (pr != PACK_RESULT_SUCCESS) return -EIO;
        if (sizeof(g_out_stream.data) < 10U) return -ENOSPC;
        memcpy(&g_out_stream.data[n], "UPK1", 4); n += 4;
        ucp_tlv_wr_u16_le(&g_out_stream.data[n], offset); n += 2;
        ucp_tlv_wr_u16_le(&g_out_stream.data[n], count); n += 2;
        g_out_stream.data[n++] = 0U;
        g_out_stream.data[n++] = 0U;
        for (uint16_t i = 0U; i < count; i++) {
            const pack_pack_list_entry_t *e = &g_pack_stream_entries[i];
            if ((n + 38U) > sizeof(g_out_stream.data)) break;
            ucp_tlv_wr_u16_le(&g_out_stream.data[n], e->pack_id); n += 2;
            ucp_tlv_wr_u16_le(&g_out_stream.data[n], e->version); n += 2;
            ucp_tlv_wr_u16_le(&g_out_stream.data[n], e->plant_count); n += 2;
            memcpy(&g_out_stream.data[n], e->name, PACK_NAME_MAX_LEN); n += PACK_NAME_MAX_LEN;
        }
    } else {
        pack_result_t pr = pack_storage_list_plants(g_plant_stream_entries, UCP_PACK_STREAM_PAGE_MAX, &count, offset);
        if (pr != PACK_RESULT_SUCCESS) return -EIO;
        if (sizeof(g_out_stream.data) < 10U) return -ENOSPC;
        memcpy(&g_out_stream.data[n], "UPT1", 4); n += 4;
        ucp_tlv_wr_u16_le(&g_out_stream.data[n], offset); n += 2;
        ucp_tlv_wr_u16_le(&g_out_stream.data[n], count); n += 2;
        g_out_stream.data[n++] = 0U;
        g_out_stream.data[n++] = 0U;
        for (uint16_t i = 0U; i < count; i++) {
            const pack_plant_list_entry_t *e = &g_plant_stream_entries[i];
            if ((n + 55U) > sizeof(g_out_stream.data)) break;
            ucp_tlv_wr_u16_le(&g_out_stream.data[n], e->plant_id); n += 2;
            ucp_tlv_wr_u16_le(&g_out_stream.data[n], e->pack_id); n += 2;
            ucp_tlv_wr_u16_le(&g_out_stream.data[n], e->version); n += 2;
            g_out_stream.data[n++] = (uint8_t)e->source;
            memcpy(&g_out_stream.data[n], e->name, PACK_COMMON_NAME_MAX_LEN); n += PACK_COMMON_NAME_MAX_LEN;
        }
    }

    g_out_stream.total_size = (uint16_t)n;
    return 0;
}

static int stream_prepare_bulk_snapshot(void)
{
    uint8_t tmp[UCP_MAX_PAYLOAD_V1];
    struct ucp_tlv_writer w;

    ucp_tlvw_init(&w, tmp, sizeof(tmp));
    if (build_bulk_snapshot_bytes(&w) != 0) return -EIO;

    reset_out_stream();
    g_out_stream.active = true;
    g_out_stream.id = alloc_stream_id();
    g_out_stream.purpose = STRM_PURPOSE_BULK_SNAPSHOT;
    g_out_stream.chunk_size = UCP_STREAM_CHUNK_MAX;
    g_out_stream.opened_ms = k_uptime_get_32();
    g_out_stream.last_activity_ms = g_out_stream.opened_ms;
    memcpy(g_out_stream.data, w.buf, w.len);
    g_out_stream.total_size = w.len;
    return 0;
}

static int stream_prepare_rain_history_range(uint8_t data_type, uint32_t start_ts, uint32_t end_ts, uint16_t limit,
                                             uint16_t *item_count_out)
{
    size_t n = 0U;
    uint16_t count = 0U;
    uint16_t max_entries;

    reset_out_stream();
    g_out_stream.active = true;
    g_out_stream.id = alloc_stream_id();
    g_out_stream.purpose = STRM_PURPOSE_RAIN_HISTORY_RANGE;
    g_out_stream.chunk_size = UCP_STREAM_CHUNK_MAX;
    g_out_stream.opened_ms = k_uptime_get_32();
    g_out_stream.last_activity_ms = g_out_stream.opened_ms;

    memcpy(&g_out_stream.data[n], "URH2", 4); n += 4;
    g_out_stream.data[n++] = data_type;
    g_out_stream.data[n++] = 0U; /* item_size */
    n += 2; /* count */
    ucp_tlv_wr_u32_le(&g_out_stream.data[n], start_ts); n += 4;
    ucp_tlv_wr_u32_le(&g_out_stream.data[n], end_ts); n += 4;

    if (data_type == 0U) {
        rain_hourly_data_t entries[64];
        uint16_t cap = (uint16_t)MIN((size_t)ARRAY_SIZE(entries),
                                     (sizeof(g_out_stream.data) - n) / sizeof(entries[0]));
        max_entries = (limit == 0U) ? cap : MIN(limit, cap);
        if (max_entries == 0U) return -ENOSPC;
        if (rain_history_get_hourly(start_ts, end_ts, entries, max_entries, &count) != WATERING_SUCCESS) return -EIO;
        memcpy(&g_out_stream.data[n], entries, (size_t)count * sizeof(entries[0]));
        n += (size_t)count * sizeof(entries[0]);
        g_out_stream.data[5] = (uint8_t)sizeof(entries[0]);
    } else if (data_type == 1U) {
        rain_daily_data_t entries[32];
        uint16_t cap = (uint16_t)MIN((size_t)ARRAY_SIZE(entries),
                                     (sizeof(g_out_stream.data) - n) / sizeof(entries[0]));
        max_entries = (limit == 0U) ? cap : MIN(limit, cap);
        if (max_entries == 0U) return -ENOSPC;
        if (rain_history_get_daily(start_ts, end_ts, entries, max_entries, &count) != WATERING_SUCCESS) return -EIO;
        memcpy(&g_out_stream.data[n], entries, (size_t)count * sizeof(entries[0]));
        n += (size_t)count * sizeof(entries[0]);
        g_out_stream.data[5] = (uint8_t)sizeof(entries[0]);
    } else {
        return -EINVAL;
    }

    ucp_tlv_wr_u16_le(&g_out_stream.data[6], count);
    g_out_stream.total_size = (uint16_t)n;
    if (item_count_out) *item_count_out = count;
    return 0;
}

static int stream_prepare_env_history_range(uint8_t data_type, uint32_t start_ts, uint32_t end_ts, uint16_t limit,
                                            uint16_t *item_count_out)
{
    size_t n = 0U;
    uint16_t count = 0U;
    uint16_t max_entries;

    reset_out_stream();
    g_out_stream.active = true;
    g_out_stream.id = alloc_stream_id();
    g_out_stream.purpose = STRM_PURPOSE_ENV_HISTORY_RANGE;
    g_out_stream.chunk_size = UCP_STREAM_CHUNK_MAX;
    g_out_stream.opened_ms = k_uptime_get_32();
    g_out_stream.last_activity_ms = g_out_stream.opened_ms;

    memcpy(&g_out_stream.data[n], "UEH2", 4); n += 4;
    g_out_stream.data[n++] = data_type;
    g_out_stream.data[n++] = 0U; /* item_size */
    n += 2; /* count */
    ucp_tlv_wr_u32_le(&g_out_stream.data[n], start_ts); n += 4;
    ucp_tlv_wr_u32_le(&g_out_stream.data[n], end_ts); n += 4;

    if (data_type == 0U) {
        hourly_history_entry_t entries[8];
        uint16_t cap = (uint16_t)MIN((size_t)ARRAY_SIZE(entries),
                                     (sizeof(g_out_stream.data) - n) / sizeof(entries[0]));
        max_entries = (limit == 0U) ? cap : MIN(limit, cap);
        if (max_entries == 0U) return -ENOSPC;
        if (env_history_get_hourly_range(start_ts, end_ts, entries, max_entries, &count) != 0) return -EIO;
        memcpy(&g_out_stream.data[n], entries, (size_t)count * sizeof(entries[0]));
        n += (size_t)count * sizeof(entries[0]);
        g_out_stream.data[5] = (uint8_t)sizeof(entries[0]);
    } else if (data_type == 1U) {
        daily_history_entry_t entries[4];
        uint16_t cap = (uint16_t)MIN((size_t)ARRAY_SIZE(entries),
                                     (sizeof(g_out_stream.data) - n) / sizeof(entries[0]));
        max_entries = (limit == 0U) ? cap : MIN(limit, cap);
        if (max_entries == 0U) return -ENOSPC;
        if (env_history_get_daily_range(start_ts, end_ts, entries, max_entries, &count) != 0) return -EIO;
        memcpy(&g_out_stream.data[n], entries, (size_t)count * sizeof(entries[0]));
        n += (size_t)count * sizeof(entries[0]);
        g_out_stream.data[5] = (uint8_t)sizeof(entries[0]);
    } else if (data_type == 2U) {
        monthly_history_entry_t entries[4];
        uint16_t cap = (uint16_t)MIN((size_t)ARRAY_SIZE(entries),
                                     (sizeof(g_out_stream.data) - n) / sizeof(entries[0]));
        max_entries = (limit == 0U) ? cap : MIN(limit, cap);
        if (max_entries == 0U) return -ENOSPC;
        if (env_history_get_monthly_range(start_ts, end_ts, entries, max_entries, &count) != 0) return -EIO;
        memcpy(&g_out_stream.data[n], entries, (size_t)count * sizeof(entries[0]));
        n += (size_t)count * sizeof(entries[0]);
        g_out_stream.data[5] = (uint8_t)sizeof(entries[0]);
    } else {
        return -EINVAL;
    }

    ucp_tlv_wr_u16_le(&g_out_stream.data[6], count);
    g_out_stream.total_size = (uint16_t)n;
    if (item_count_out) *item_count_out = count;
    return 0;
}

static uint16_t ucp_day_of_year(uint16_t year, uint8_t month, uint8_t day)
{
    static const uint16_t month_offsets[] = { 0U, 31U, 59U, 90U, 120U, 151U, 181U, 212U, 243U, 273U, 304U, 334U };
    bool leap = ((year % 4U == 0U && year % 100U != 0U) || (year % 400U == 0U));
    uint16_t doy;
    if (month < 1U || month > 12U || day < 1U || day > 31U) {
        return 1U;
    }
    doy = (uint16_t)(month_offsets[month - 1U] + day);
    if (leap && month > 2U) {
        doy++;
    }
    return doy;
}

static int stream_prepare_water_history_range(uint8_t data_type, uint8_t channel_id,
                                              uint32_t start_ts, uint32_t end_ts, uint16_t limit,
                                              uint16_t *item_count_out)
{
    rtc_datetime_t dt_start = {0}, dt_end = {0};
    size_t n = 0U;
    uint16_t count = 0U;

    if (channel_id >= WATERING_CHANNELS_COUNT) return -EINVAL;
    if (end_ts == 0U) end_ts = timezone_get_unix_utc();
    if (start_ts != 0U && end_ts < start_ts) return -EINVAL;
    if (timezone_unix_to_rtc_utc(start_ts ? start_ts : end_ts, &dt_start) != 0) return -EINVAL;
    if (timezone_unix_to_rtc_utc(end_ts, &dt_end) != 0) return -EINVAL;

    reset_out_stream();
    g_out_stream.active = true;
    g_out_stream.id = alloc_stream_id();
    g_out_stream.purpose = STRM_PURPOSE_WATER_HISTORY_RANGE;
    g_out_stream.chunk_size = UCP_STREAM_CHUNK_MAX;
    g_out_stream.opened_ms = k_uptime_get_32();
    g_out_stream.last_activity_ms = g_out_stream.opened_ms;

    memcpy(&g_out_stream.data[n], "UWH2", 4); n += 4;
    g_out_stream.data[n++] = data_type;
    g_out_stream.data[n++] = 0U; /* item_size */
    n += 2; /* count */
    g_out_stream.data[n++] = channel_id;
    g_out_stream.data[n++] = 0U; /* reserved */
    ucp_tlv_wr_u32_le(&g_out_stream.data[n], start_ts); n += 4;
    ucp_tlv_wr_u32_le(&g_out_stream.data[n], end_ts); n += 4;

    if (data_type == 0U) {
        history_event_t entries[DETAILED_EVENTS_PER_CHANNEL];
        uint16_t cap = (uint16_t)ARRAY_SIZE(entries);
        uint16_t req_count = (limit == 0U) ? cap : MIN(limit, cap);
        if (req_count == 0U) return -ENOSPC;
        count = req_count;
        if (watering_history_query_range(channel_id, start_ts, end_ts, entries, &count) != WATERING_SUCCESS) return -EIO;
        if ((n + ((size_t)count * sizeof(entries[0]))) > sizeof(g_out_stream.data)) {
            count = (uint16_t)((sizeof(g_out_stream.data) - n) / sizeof(entries[0]));
        }
        memcpy(&g_out_stream.data[n], entries, (size_t)count * sizeof(entries[0]));
        n += (size_t)count * sizeof(entries[0]);
        g_out_stream.data[5] = (uint8_t)sizeof(entries[0]);
    } else if (data_type == 1U) {
        daily_stats_t entries[10];
        uint16_t start_day = ucp_day_of_year(dt_start.year, dt_start.month, dt_start.day);
        uint16_t end_day = ucp_day_of_year(dt_end.year, dt_end.month, dt_end.day);
        uint16_t req_cap = (uint16_t)ARRAY_SIZE(entries);
        if (dt_start.year != dt_end.year) return -EINVAL;
        if (limit != 0U) req_cap = MIN(limit, req_cap);
        count = req_cap;
        if (watering_history_get_daily_stats(channel_id, start_day, end_day, dt_end.year, entries, &count) != WATERING_SUCCESS) return -EIO;
        memcpy(&g_out_stream.data[n], entries, (size_t)count * sizeof(entries[0]));
        n += (size_t)count * sizeof(entries[0]);
        g_out_stream.data[5] = (uint8_t)sizeof(entries[0]);
    } else if (data_type == 2U) {
        monthly_stats_t entries[12];
        uint8_t start_month = dt_start.month;
        uint8_t end_month = dt_end.month;
        uint16_t req_cap = (uint16_t)ARRAY_SIZE(entries);
        if (dt_start.year != dt_end.year) return -EINVAL;
        if (limit != 0U) req_cap = MIN(limit, req_cap);
        count = req_cap;
        if (watering_history_get_monthly_stats(channel_id, start_month, end_month, dt_end.year, entries, &count) != WATERING_SUCCESS) return -EIO;
        memcpy(&g_out_stream.data[n], entries, (size_t)count * sizeof(entries[0]));
        n += (size_t)count * sizeof(entries[0]);
        g_out_stream.data[5] = (uint8_t)sizeof(entries[0]);
    } else if (data_type == 3U) {
        annual_stats_t entries[10];
        uint16_t start_year = dt_start.year;
        uint16_t end_year = dt_end.year;
        uint16_t req_cap = (uint16_t)ARRAY_SIZE(entries);
        if (limit != 0U) req_cap = MIN(limit, req_cap);
        count = req_cap;
        if (watering_history_get_annual_stats(channel_id, start_year, end_year, entries, &count) != WATERING_SUCCESS) return -EIO;
        memcpy(&g_out_stream.data[n], entries, (size_t)count * sizeof(entries[0]));
        n += (size_t)count * sizeof(entries[0]);
        g_out_stream.data[5] = (uint8_t)sizeof(entries[0]);
    } else {
        return -EINVAL;
    }

    ucp_tlv_wr_u16_le(&g_out_stream.data[6], count);
    g_out_stream.total_size = (uint16_t)n;
    if (item_count_out) *item_count_out = count;
    return 0;
}

static int stream_prepare_db_catalog(uint8_t db_kind, uint16_t offset, uint16_t limit,
                                     uint32_t *total_catalog_len_out, uint16_t *item_count_out)
{
    int n = 0;
    uint16_t count = 0U;
    uint16_t end = 0U;
    uint32_t total_items = 0U;

    reset_out_stream();
    g_out_stream.active = true;
    g_out_stream.id = alloc_stream_id();
    g_out_stream.purpose = STRM_PURPOSE_DB_CATALOG;
    g_out_stream.chunk_size = UCP_STREAM_CHUNK_MAX;
    g_out_stream.opened_ms = k_uptime_get_32();
    g_out_stream.last_activity_ms = g_out_stream.opened_ms;

    if (limit == 0U) limit = 16U;
    if (db_kind == 1U) total_items = PLANT_FULL_SPECIES_COUNT;
    else if (db_kind == 2U) total_items = SOIL_ENHANCED_TYPES_COUNT;
    else if (db_kind == 3U) total_items = IRRIGATION_METHODS_COUNT;
    else return -EINVAL;

    end = (uint16_t)MIN((uint32_t)offset + (uint32_t)limit, total_items);
    n += snprintk((char *)&g_out_stream.data[n], sizeof(g_out_stream.data) - (size_t)n,
                  "udb1 kind=%u offset=%u total=%u\n", db_kind, offset, (unsigned)total_items);

    for (uint16_t i = offset; i < end && n < (int)sizeof(g_out_stream.data); i++) {
        int wr = 0;
        if (db_kind == 1U) {
            const plant_full_data_t *p1 = plant_db_get_by_index(i);
            if (!p1) continue;
            wr = snprintk((char *)&g_out_stream.data[n], sizeof(g_out_stream.data) - (size_t)n,
                          "%u|%s|%s|m=%u\n", i,
                          p1->common_name_en ? p1->common_name_en : "",
                          p1->scientific_name ? p1->scientific_name : "",
                          p1->typ_irrig_method_id);
        } else if (db_kind == 2U) {
            const soil_enhanced_data_t *s = soil_db_get_by_index((uint8_t)i);
            if (!s) continue;
            wr = snprintk((char *)&g_out_stream.data[n], sizeof(g_out_stream.data) - (size_t)n,
                          "%u|%s|%s|awc=%u\n", i,
                          s->soil_type ? s->soil_type : "",
                          s->texture ? s->texture : "",
                          s->awc_mm_per_m);
        } else {
            const irrigation_method_data_t *m = irrigation_db_get_by_index((uint8_t)i);
            if (!m) continue;
            wr = snprintk((char *)&g_out_stream.data[n], sizeof(g_out_stream.data) - (size_t)n,
                          "%u|%s|eff=%u\n", i,
                          m->method_name ? m->method_name : "",
                          m->efficiency_pct);
        }
        if (wr <= 0) break;
        if ((size_t)wr >= (sizeof(g_out_stream.data) - (size_t)n)) break;
        n += wr;
        count++;
    }

    g_out_stream.total_size = (uint16_t)MAX(n, 0);
    if (total_catalog_len_out) *total_catalog_len_out = total_items;
    if (item_count_out) *item_count_out = count;
    return 0;
}

static int handle_stream_open(uint16_t svc, uint16_t mid, const uint8_t *p, uint16_t len, uint32_t req)
{
    struct ucp_tlv_elem e;
    size_t off = 0U;
    uint8_t purpose = 0U;
    uint8_t direction = 0U;
    uint16_t chunk_pref = 0U;
    uint32_t total_hint = 0U;
    uint8_t q_channel_id = 0xFFU;
    uint16_t q_page_index = 0U;
    uint16_t q_offset = 0U;
    uint16_t q_limit = 0U;
    uint8_t q_data_type = 0U;
    uint8_t q_db_kind = 0U;
    uint32_t q_start_ts = 0U;
    uint32_t q_end_ts = 0U;
    bool has_q_channel = false;
    bool has_q_page = false;
    bool has_q_offset = false;
    bool has_q_limit = false;
    bool has_q_data_type = false;
    bool has_q_db_kind = false;
    bool has_q_start_ts = false;
    bool has_q_end_ts = false;
    bool has_total_hint = false;
    uint8_t rb[160];
    struct ucp_tlv_writer rw;
    uint32_t stream_total_meta = 0U;
    uint16_t stream_item_count_meta = 0U;
    uint8_t content_type_meta = 0U; /* 1=text, 2=binary */
    uint8_t stream_schema_meta = 0U;

    ARG_UNUSED(svc);
    ARG_UNUSED(mid);

    while (ucp_tlv_next(p, len, &off, &e)) {
        if (e.field_id == STRM_PURPOSE) {
            if (!ucp_tlv_read_u8(&e, &purpose)) return send_tlv(UCP_MSG_STREAM_OPEN_RESP, 0U, 0U, UCP_STATUS_INVALID_ARG, NULL, req);
            continue;
        }
        if (e.field_id == STRM_CHUNK_SIZE) {
            if (!ucp_tlv_read_u16(&e, &chunk_pref)) return send_tlv(UCP_MSG_STREAM_OPEN_RESP, 0U, 0U, UCP_STATUS_INVALID_ARG, NULL, req);
            continue;
        }
        if (e.field_id == STRM_DIRECTION) {
            if (!ucp_tlv_read_u8(&e, &direction)) return send_tlv(UCP_MSG_STREAM_OPEN_RESP, 0U, 0U, UCP_STATUS_INVALID_ARG, NULL, req);
            continue;
        }
        if (e.field_id == STRM_TOTAL_SIZE) {
            if (!ucp_tlv_read_u32(&e, &total_hint)) return send_tlv(UCP_MSG_STREAM_OPEN_RESP, 0U, 0U, UCP_STATUS_INVALID_ARG, NULL, req);
            has_total_hint = true;
            continue;
        }
        if (e.field_id == Q_CHANNEL_ID) {
            if (!ucp_tlv_read_u8(&e, &q_channel_id)) return send_tlv(UCP_MSG_STREAM_OPEN_RESP, 0U, 0U, UCP_STATUS_INVALID_ARG, NULL, req);
            has_q_channel = true;
            continue;
        }
        if (e.field_id == Q_PAGE_INDEX) {
            if (!ucp_tlv_read_u16(&e, &q_page_index)) return send_tlv(UCP_MSG_STREAM_OPEN_RESP, 0U, 0U, UCP_STATUS_INVALID_ARG, NULL, req);
            has_q_page = true;
            continue;
        }
        if (e.field_id == Q_OFFSET) {
            if (!ucp_tlv_read_u16(&e, &q_offset)) return send_tlv(UCP_MSG_STREAM_OPEN_RESP, 0U, 0U, UCP_STATUS_INVALID_ARG, NULL, req);
            has_q_offset = true;
            continue;
        }
        if (e.field_id == Q_LIMIT) {
            if (!ucp_tlv_read_u16(&e, &q_limit)) return send_tlv(UCP_MSG_STREAM_OPEN_RESP, 0U, 0U, UCP_STATUS_INVALID_ARG, NULL, req);
            has_q_limit = true;
            continue;
        }
        if (e.field_id == Q_DATA_TYPE) {
            if (!ucp_tlv_read_u8(&e, &q_data_type)) return send_tlv(UCP_MSG_STREAM_OPEN_RESP, 0U, 0U, UCP_STATUS_INVALID_ARG, NULL, req);
            has_q_data_type = true;
            continue;
        }
        if (e.field_id == Q_DB_KIND) {
            if (!ucp_tlv_read_u8(&e, &q_db_kind)) return send_tlv(UCP_MSG_STREAM_OPEN_RESP, 0U, 0U, UCP_STATUS_INVALID_ARG, NULL, req);
            has_q_db_kind = true;
            continue;
        }
        if (e.field_id == Q_START_TS) {
            if (!ucp_tlv_read_u32(&e, &q_start_ts)) return send_tlv(UCP_MSG_STREAM_OPEN_RESP, 0U, 0U, UCP_STATUS_INVALID_ARG, NULL, req);
            has_q_start_ts = true;
            continue;
        }
        if (e.field_id == Q_END_TS) {
            if (!ucp_tlv_read_u32(&e, &q_end_ts)) return send_tlv(UCP_MSG_STREAM_OPEN_RESP, 0U, 0U, UCP_STATUS_INVALID_ARG, NULL, req);
            has_q_end_ts = true;
            continue;
        }
    }

    if (purpose == 0U) return send_tlv(UCP_MSG_STREAM_OPEN_RESP, 0U, 0U, UCP_STATUS_INVALID_ARG, NULL, req);
    if (direction == 0U) {
        direction = (purpose == STRM_PURPOSE_UPLOAD_STAGING) ? STRM_DIR_INBOUND : STRM_DIR_OUTBOUND;
    }
    if (direction != STRM_DIR_OUTBOUND && direction != STRM_DIR_INBOUND) {
        return send_tlv(UCP_MSG_STREAM_OPEN_RESP, 0U, 0U, UCP_STATUS_INVALID_ARG, NULL, req);
    }

    if (direction == STRM_DIR_OUTBOUND) {
        if (g_out_stream.active) return send_tlv(UCP_MSG_STREAM_OPEN_RESP, 0U, 0U, UCP_STATUS_BUSY, NULL, req);

        if (purpose == STRM_PURPOSE_DIAG_SNAPSHOT) {
            (void)stream_prepare_diag_snapshot();
            content_type_meta = 1U; stream_schema_meta = 1U; stream_total_meta = g_out_stream.total_size;
        } else if (purpose == STRM_PURPOSE_SCHEMA_TEXT) {
            (void)stream_prepare_schema_text();
            content_type_meta = 1U; stream_schema_meta = 1U; stream_total_meta = g_out_stream.total_size;
        } else if (purpose == STRM_PURPOSE_SCHEMA_CATALOG) {
            if (stream_prepare_schema_catalog(has_q_offset ? q_offset : 0U, &stream_total_meta, &stream_item_count_meta) != 0) {
                return send_tlv(UCP_MSG_STREAM_OPEN_RESP, 0U, 0U, UCP_STATUS_INTERNAL, NULL, req);
            }
            content_type_meta = 1U;
            stream_schema_meta = 2U;
        } else if (purpose == STRM_PURPOSE_HISTORY_PAGE) {
            if (!has_q_channel || !has_q_page) return send_tlv(UCP_MSG_STREAM_OPEN_RESP, 0U, 0U, UCP_STATUS_INVALID_ARG, NULL, req);
            if (stream_prepare_history_page(q_channel_id, q_page_index) != 0) return send_tlv(UCP_MSG_STREAM_OPEN_RESP, 0U, 0U, UCP_STATUS_INTERNAL, NULL, req);
            content_type_meta = 2U; stream_schema_meta = 3U; stream_total_meta = g_out_stream.total_size;
        } else if (purpose == STRM_PURPOSE_PACKS_LIST) {
            if (stream_prepare_packs_list(has_q_offset ? q_offset : 0U, false) != 0) return send_tlv(UCP_MSG_STREAM_OPEN_RESP, 0U, 0U, UCP_STATUS_INTERNAL, NULL, req);
            content_type_meta = 2U; stream_schema_meta = 4U; stream_total_meta = g_out_stream.total_size;
        } else if (purpose == STRM_PURPOSE_PACKS_PLANTS_LIST) {
            if (stream_prepare_packs_list(has_q_offset ? q_offset : 0U, true) != 0) return send_tlv(UCP_MSG_STREAM_OPEN_RESP, 0U, 0U, UCP_STATUS_INTERNAL, NULL, req);
            content_type_meta = 2U; stream_schema_meta = 5U; stream_total_meta = g_out_stream.total_size;
        } else if (purpose == STRM_PURPOSE_RAIN_HISTORY_RANGE) {
            if (!has_q_data_type) return send_tlv(UCP_MSG_STREAM_OPEN_RESP, 0U, 0U, UCP_STATUS_INVALID_ARG, NULL, req);
            if (stream_prepare_rain_history_range(q_data_type,
                                                  has_q_start_ts ? q_start_ts : 0U,
                                                  has_q_end_ts ? q_end_ts : timezone_get_unix_utc(),
                                                  has_q_limit ? q_limit : 0U,
                                                  &stream_item_count_meta) != 0) {
                return send_tlv(UCP_MSG_STREAM_OPEN_RESP, 0U, 0U, UCP_STATUS_INTERNAL, NULL, req);
            }
            content_type_meta = 2U; stream_schema_meta = 6U; stream_total_meta = g_out_stream.total_size;
        } else if (purpose == STRM_PURPOSE_ENV_HISTORY_RANGE) {
            if (!has_q_data_type) return send_tlv(UCP_MSG_STREAM_OPEN_RESP, 0U, 0U, UCP_STATUS_INVALID_ARG, NULL, req);
            if (stream_prepare_env_history_range(q_data_type,
                                                 has_q_start_ts ? q_start_ts : 0U,
                                                 has_q_end_ts ? q_end_ts : timezone_get_unix_utc(),
                                                 has_q_limit ? q_limit : 0U,
                                                 &stream_item_count_meta) != 0) {
                return send_tlv(UCP_MSG_STREAM_OPEN_RESP, 0U, 0U, UCP_STATUS_INTERNAL, NULL, req);
            }
            content_type_meta = 2U; stream_schema_meta = 7U; stream_total_meta = g_out_stream.total_size;
        } else if (purpose == STRM_PURPOSE_WATER_HISTORY_RANGE) {
            if (!has_q_data_type || !has_q_channel) return send_tlv(UCP_MSG_STREAM_OPEN_RESP, 0U, 0U, UCP_STATUS_INVALID_ARG, NULL, req);
            if (stream_prepare_water_history_range(q_data_type, q_channel_id,
                                                   has_q_start_ts ? q_start_ts : 0U,
                                                   has_q_end_ts ? q_end_ts : timezone_get_unix_utc(),
                                                   has_q_limit ? q_limit : 0U,
                                                   &stream_item_count_meta) != 0) {
                return send_tlv(UCP_MSG_STREAM_OPEN_RESP, 0U, 0U, UCP_STATUS_INTERNAL, NULL, req);
            }
            content_type_meta = 2U; stream_schema_meta = 10U; stream_total_meta = g_out_stream.total_size;
        } else if (purpose == STRM_PURPOSE_DB_CATALOG) {
            if (!has_q_db_kind) return send_tlv(UCP_MSG_STREAM_OPEN_RESP, 0U, 0U, UCP_STATUS_INVALID_ARG, NULL, req);
            if (stream_prepare_db_catalog(q_db_kind,
                                          has_q_offset ? q_offset : 0U,
                                          has_q_limit ? q_limit : 0U,
                                          &stream_total_meta, &stream_item_count_meta) != 0) {
                return send_tlv(UCP_MSG_STREAM_OPEN_RESP, 0U, 0U, UCP_STATUS_INTERNAL, NULL, req);
            }
            content_type_meta = 1U; stream_schema_meta = 8U;
        } else if (purpose == STRM_PURPOSE_BULK_SNAPSHOT) {
            if (stream_prepare_bulk_snapshot() != 0) return send_tlv(UCP_MSG_STREAM_OPEN_RESP, 0U, 0U, UCP_STATUS_INTERNAL, NULL, req);
            content_type_meta = 2U; stream_schema_meta = 9U; stream_total_meta = g_out_stream.total_size;
        } else {
            return send_tlv(UCP_MSG_STREAM_OPEN_RESP, 0U, 0U, UCP_STATUS_UNSUPPORTED, NULL, req);
        }

        if (chunk_pref != 0U && chunk_pref < g_out_stream.chunk_size) {
            g_out_stream.chunk_size = chunk_pref;
        }
        if (g_out_stream.chunk_size > UCP_STREAM_CHUNK_MAX) g_out_stream.chunk_size = UCP_STREAM_CHUNK_MAX;
        g_diag.stream_out_open++;
        g_out_stream.last_activity_ms = k_uptime_get_32();

        ucp_tlvw_init(&rw, rb, sizeof(rb));
        (void)ucp_tlvw_put_u16(&rw, STRM_STREAM_ID, UCP_TLV_U16, g_out_stream.id);
        (void)ucp_tlvw_put_u8(&rw, STRM_PURPOSE, UCP_TLV_U8, g_out_stream.purpose);
        (void)ucp_tlvw_put_u8(&rw, STRM_DIRECTION, UCP_TLV_U8, STRM_DIR_OUTBOUND);
        (void)ucp_tlvw_put_u16(&rw, STRM_CHUNK_SIZE, UCP_TLV_U16, g_out_stream.chunk_size);
        (void)ucp_tlvw_put_u32(&rw, STRM_TOTAL_SIZE, UCP_TLV_U32, g_out_stream.total_size);
        (void)ucp_tlvw_put_u8(&rw, SCHEMA_STREAM_SCHEMA, UCP_TLV_U8, stream_schema_meta);
        (void)ucp_tlvw_put_u8(&rw, SCHEMA_CONTENT_TYPE, UCP_TLV_U8, content_type_meta);
        (void)ucp_tlvw_put_u32(&rw, SCHEMA_TOTAL_LEN, UCP_TLV_U32, stream_total_meta ? stream_total_meta : g_out_stream.total_size);
        (void)ucp_tlvw_put_u16(&rw, SCHEMA_PAGE_OFFSET, UCP_TLV_U16, has_q_offset ? q_offset : 0U);
        if (has_q_channel) (void)ucp_tlvw_put_u8(&rw, Q_CHANNEL_ID, UCP_TLV_U8, q_channel_id);
        if (has_q_page) (void)ucp_tlvw_put_u16(&rw, Q_PAGE_INDEX, UCP_TLV_U16, q_page_index);
        if (has_q_offset) (void)ucp_tlvw_put_u16(&rw, Q_OFFSET, UCP_TLV_U16, q_offset);
        if (has_q_limit) (void)ucp_tlvw_put_u16(&rw, Q_LIMIT, UCP_TLV_U16, q_limit);
        if (has_q_data_type) (void)ucp_tlvw_put_u8(&rw, Q_DATA_TYPE, UCP_TLV_U8, q_data_type);
        if (has_q_db_kind) (void)ucp_tlvw_put_u8(&rw, Q_DB_KIND, UCP_TLV_U8, q_db_kind);
        if (has_q_start_ts) (void)ucp_tlvw_put_u32(&rw, Q_START_TS, UCP_TLV_U32, q_start_ts);
        if (has_q_end_ts) (void)ucp_tlvw_put_u32(&rw, Q_END_TS, UCP_TLV_U32, q_end_ts);
        if (stream_item_count_meta != 0U) (void)ucp_tlvw_put_u16(&rw, SCHEMA_ITEM_COUNT, UCP_TLV_U16, stream_item_count_meta);
        if (send_tlv(UCP_MSG_STREAM_OPEN_RESP, 0U, 0U, UCP_STATUS_OK, &rw, req) != 0) {
            reset_out_stream();
            return -EIO;
        }

        return stream_send_all();
    }

    /* inbound stream */
    if (g_in_stream.active) return send_tlv(UCP_MSG_STREAM_OPEN_RESP, 0U, 0U, UCP_STATUS_BUSY, NULL, req);
    if (purpose != STRM_PURPOSE_UPLOAD_STAGING) return send_tlv(UCP_MSG_STREAM_OPEN_RESP, 0U, 0U, UCP_STATUS_UNSUPPORTED, NULL, req);
    if (has_total_hint && total_hint > UCP_STREAM_BUF_MAX) return send_tlv(UCP_MSG_STREAM_OPEN_RESP, 0U, 0U, UCP_STATUS_OUT_OF_RANGE, NULL, req);

    reset_in_stream();
    g_in_stream.active = true;
    g_in_stream.id = alloc_stream_id();
    g_in_stream.purpose = purpose;
    g_in_stream.chunk_size = (chunk_pref == 0U) ? UCP_STREAM_CHUNK_MAX : MIN(chunk_pref, UCP_STREAM_CHUNK_MAX);
    g_in_stream.chunk_index_next = 0U;
    g_in_stream.opened_ms = k_uptime_get_32();
    g_in_stream.last_activity_ms = g_in_stream.opened_ms;
    if (has_total_hint) {
        g_in_stream.expect_total = true;
        g_in_stream.expected_total = (uint16_t)total_hint;
    }
    g_diag.stream_in_open++;

    ucp_tlvw_init(&rw, rb, sizeof(rb));
    (void)ucp_tlvw_put_u16(&rw, STRM_STREAM_ID, UCP_TLV_U16, g_in_stream.id);
    (void)ucp_tlvw_put_u8(&rw, STRM_PURPOSE, UCP_TLV_U8, g_in_stream.purpose);
    (void)ucp_tlvw_put_u8(&rw, STRM_DIRECTION, UCP_TLV_U8, STRM_DIR_INBOUND);
    (void)ucp_tlvw_put_u16(&rw, STRM_CHUNK_SIZE, UCP_TLV_U16, g_in_stream.chunk_size);
    (void)ucp_tlvw_put_u32(&rw, STRM_TOTAL_SIZE, UCP_TLV_U32, UCP_STREAM_BUF_MAX);
    return send_tlv(UCP_MSG_STREAM_OPEN_RESP, 0U, 0U, UCP_STATUS_OK, &rw, req);
}

static int handle_stream_abort(const uint8_t *p, uint16_t len, uint32_t req)
{
    struct ucp_tlv_elem e;
    size_t off = 0U;
    uint16_t stream_id = 0U;
    bool has_id = false;
    uint8_t rb[32];
    struct ucp_tlv_writer w;

    while (ucp_tlv_next(p, len, &off, &e)) {
        if (e.field_id == STRM_STREAM_ID) {
            if (!ucp_tlv_read_u16(&e, &stream_id)) return send_tlv(UCP_MSG_STREAM_ABORT, 0U, 0U, UCP_STATUS_INVALID_ARG, NULL, req);
            has_id = true;
        }
    }

    if (!g_out_stream.active && !g_in_stream.active) {
        return send_tlv(UCP_MSG_STREAM_ABORT, 0U, 0U, UCP_STATUS_STREAM_STATE_ERR, NULL, req);
    }
    if (has_id) {
        if (g_out_stream.active && stream_id == g_out_stream.id) {
            reset_out_stream();
        } else if (g_in_stream.active && stream_id == g_in_stream.id) {
            reset_in_stream();
        } else {
            return send_tlv(UCP_MSG_STREAM_ABORT, 0U, 0U, UCP_STATUS_NOT_FOUND, NULL, req);
        }
    } else {
        if (g_out_stream.active && g_in_stream.active) {
            return send_tlv(UCP_MSG_STREAM_ABORT, 0U, 0U, UCP_STATUS_CONFLICT, NULL, req);
        }
        if (g_out_stream.active) {
            stream_id = g_out_stream.id;
            reset_out_stream();
        } else {
            stream_id = g_in_stream.id;
            reset_in_stream();
        }
    }
    g_diag.stream_aborts++;
    (void)emit_diag_change(4U);

    ucp_tlvw_init(&w, rb, sizeof(rb));
    (void)ucp_tlvw_put_u16(&w, STRM_STREAM_ID, UCP_TLV_U16, stream_id);
    (void)ucp_tlvw_put_u8(&w, STRM_RESULT, UCP_TLV_U8, STRM_RESULT_ABORTED);
    return send_tlv(UCP_MSG_STREAM_ABORT, 0U, 0U, UCP_STATUS_OK, &w, req);
}

static int handle_stream_data(const uint8_t *p, uint16_t len, uint32_t req)
{
    struct ucp_tlv_elem e;
    size_t off = 0U;
    uint16_t stream_id = 0U, chunk_idx = 0U;
    uint32_t offset = 0U;
    const uint8_t *data = NULL;
    uint16_t data_len = 0U;
    bool has_id = false, has_chunk = false, has_off = false, has_data = false;

    while (ucp_tlv_next(p, len, &off, &e)) {
        if (e.field_id == STRM_STREAM_ID) {
            if (!ucp_tlv_read_u16(&e, &stream_id)) return send_tlv(UCP_MSG_STREAM_ABORT, 0U, 0U, UCP_STATUS_INVALID_ARG, NULL, req);
            has_id = true;
            continue;
        }
        if (e.field_id == STRM_CHUNK_INDEX) {
            if (!ucp_tlv_read_u16(&e, &chunk_idx)) return send_tlv(UCP_MSG_STREAM_ABORT, 0U, 0U, UCP_STATUS_INVALID_ARG, NULL, req);
            has_chunk = true;
            continue;
        }
        if (e.field_id == STRM_OFFSET) {
            if (!ucp_tlv_read_u32(&e, &offset)) return send_tlv(UCP_MSG_STREAM_ABORT, 0U, 0U, UCP_STATUS_INVALID_ARG, NULL, req);
            has_off = true;
            continue;
        }
        if (e.field_id == STRM_DATA) {
            data = e.value;
            data_len = e.len;
            has_data = true;
            continue;
        }
    }

    if (!g_in_stream.active) return send_tlv(UCP_MSG_STREAM_ABORT, 0U, 0U, UCP_STATUS_STREAM_STATE_ERR, NULL, req);
    if (!has_id || !has_chunk || !has_off || !has_data) return send_tlv(UCP_MSG_STREAM_ABORT, 0U, 0U, UCP_STATUS_INVALID_ARG, NULL, req);
    if (stream_id != g_in_stream.id) return send_tlv(UCP_MSG_STREAM_ABORT, 0U, 0U, UCP_STATUS_NOT_FOUND, NULL, req);
    if (chunk_idx != g_in_stream.chunk_index_next || offset != g_in_stream.received) {
        g_diag.stream_aborts++;
        reset_in_stream();
        return send_tlv(UCP_MSG_STREAM_ABORT, 0U, 0U, UCP_STATUS_STREAM_STATE_ERR, NULL, req);
    }
    if (data_len == 0U || data_len > g_in_stream.chunk_size) return send_tlv(UCP_MSG_STREAM_ABORT, 0U, 0U, UCP_STATUS_OUT_OF_RANGE, NULL, req);
    if ((uint32_t)g_in_stream.received + data_len > UCP_STREAM_BUF_MAX) {
        g_diag.stream_aborts++;
        reset_in_stream();
        return send_tlv(UCP_MSG_STREAM_ABORT, 0U, 0U, UCP_STATUS_OUT_OF_RANGE, NULL, req);
    }

    memcpy(&g_in_stream.data[g_in_stream.received], data, data_len);
    g_in_stream.received = (uint16_t)(g_in_stream.received + data_len);
    g_in_stream.chunk_index_next++;
    g_in_stream.last_activity_ms = k_uptime_get_32();
    return 0;
}

static int handle_stream_end(const uint8_t *p, uint16_t len, uint32_t req)
{
    struct ucp_tlv_elem e;
    size_t off = 0U;
    uint16_t stream_id = 0U;
    uint32_t total = 0U, final_crc = 0U, calc_crc;
    bool has_id = false, has_total = false, has_crc = false;
    uint8_t b[96];
    struct ucp_tlv_writer w;
    uint8_t result = STRM_RESULT_OK;
    uint16_t status = UCP_STATUS_OK;

    while (ucp_tlv_next(p, len, &off, &e)) {
        if (e.field_id == STRM_STREAM_ID) {
            if (!ucp_tlv_read_u16(&e, &stream_id)) return send_tlv(UCP_MSG_STREAM_END, 0U, 0U, UCP_STATUS_INVALID_ARG, NULL, req);
            has_id = true;
            continue;
        }
        if (e.field_id == STRM_TOTAL_SIZE) {
            if (!ucp_tlv_read_u32(&e, &total)) return send_tlv(UCP_MSG_STREAM_END, 0U, 0U, UCP_STATUS_INVALID_ARG, NULL, req);
            has_total = true;
            continue;
        }
        if (e.field_id == STRM_FINAL_CRC32C) {
            if (!ucp_tlv_read_u32(&e, &final_crc)) return send_tlv(UCP_MSG_STREAM_END, 0U, 0U, UCP_STATUS_INVALID_ARG, NULL, req);
            has_crc = true;
            continue;
        }
    }

    if (!g_in_stream.active) return send_tlv(UCP_MSG_STREAM_END, 0U, 0U, UCP_STATUS_STREAM_STATE_ERR, NULL, req);
    if (!has_id || stream_id != g_in_stream.id) return send_tlv(UCP_MSG_STREAM_END, 0U, 0U, UCP_STATUS_NOT_FOUND, NULL, req);
    if (g_in_stream.expect_total && g_in_stream.expected_total != g_in_stream.received) {
        result = STRM_RESULT_ABORTED;
        status = UCP_STATUS_CONFLICT;
    }
    if (has_total && total != g_in_stream.received) {
        result = STRM_RESULT_ABORTED;
        status = UCP_STATUS_CONFLICT;
    }
    calc_crc = ucp_crc32c(g_in_stream.data, g_in_stream.received);
    if (has_crc && final_crc != calc_crc) {
        result = STRM_RESULT_CRC_FAIL;
        status = UCP_STATUS_CHECKSUM_FAIL;
    }

    ucp_tlvw_init(&w, b, sizeof(b));
    (void)ucp_tlvw_put_u16(&w, STRM_STREAM_ID, UCP_TLV_U16, g_in_stream.id);
    (void)ucp_tlvw_put_u32(&w, STRM_TOTAL_SIZE, UCP_TLV_U32, g_in_stream.received);
    (void)ucp_tlvw_put_u32(&w, STRM_FINAL_CRC32C, UCP_TLV_U32, calc_crc);
    (void)ucp_tlvw_put_u8(&w, STRM_RESULT, UCP_TLV_U8, result);

    if (status == UCP_STATUS_OK) {
        g_staged_blob_valid = true;
        g_staged_blob_len = g_in_stream.received;
        g_staged_blob_crc = calc_crc;
        if (g_staged_blob_len > 0U) {
            memcpy(g_staged_blob, g_in_stream.data, g_staged_blob_len);
        }
        if (g_in_stream.purpose == STRM_PURPOSE_UPLOAD_STAGING) {
            uint8_t pd[32];
            struct ucp_tlv_writer pw;
            ucp_tlvw_init(&pw, pd, sizeof(pd));
            (void)ucp_tlvw_put_u8(&pw, PACKS_STAGE_VALID, UCP_TLV_BOOL, 1U);
            (void)ucp_tlvw_put_u16(&pw, PACKS_STAGE_LEN, UCP_TLV_U16, g_staged_blob_len);
            (void)ucp_tlvw_put_u32(&pw, PACKS_STAGE_CRC, UCP_TLV_U32, g_staged_blob_crc);
            g_ver_packs++;
            (void)emit_change(UCP_TOPIC_PACKS_CHANGED, UCP_SVC_PACKS, UCP_RES_PACKS_STATS, g_ver_packs, UCP_ORIGIN_UART, pw.buf, pw.len);
        }
        (void)emit_diag_change(1U);
    } else {
        g_diag.stream_aborts++;
    }

    reset_in_stream();
    return send_tlv(UCP_MSG_STREAM_END, 0U, 0U, status, &w, req);
}

static int build_peer_power_state(struct ucp_tlv_writer *w)
{
    return (ucp_tlvw_put_u8(w, PEER_STATE_ON, UCP_TLV_BOOL, g_peer_power_on ? 1U : 0U) &&
            ucp_tlvw_put_u8(w, PEER_STATE_HW_BACKED, UCP_TLV_BOOL, g_peer_power_hw_backed ? 1U : 0U) &&
            ucp_tlvw_put_u8(w, PEER_STATE_INHIBIT, UCP_TLV_BOOL, peer_power_inhibit() ? 1U : 0U) &&
            ucp_tlvw_put_u16(w, PEER_STATE_GRACE_MS, UCP_TLV_U16, g_peer_power_grace_ms) &&
            ucp_tlvw_put_u32(w, UCP_F_RESOURCE_VERSION, UCP_TLV_U32, g_ver_peer) &&
            ucp_tlvw_put_u64(w, UCP_F_TIMESTAMP_UTC_MS, UCP_TLV_U64, now_utc_ms())) ? 0 : -ENOSPC;
}

static int build_peer_power_policy(struct ucp_tlv_writer *w)
{
    return (ucp_tlvw_put_u8(w, PEER_POLICY_MODE, UCP_TLV_U8, g_peer_power_policy) &&
            ucp_tlvw_put_u16(w, PEER_STATE_GRACE_MS, UCP_TLV_U16, g_peer_power_grace_ms) &&
            ucp_tlvw_put_u32(w, UCP_F_RESOURCE_VERSION, UCP_TLV_U32, g_ver_peer) &&
            ucp_tlvw_put_u64(w, UCP_F_TIMESTAMP_UTC_MS, UCP_TLV_U64, now_utc_ms())) ? 0 : -ENOSPC;
}

static int build_diag_counters(struct ucp_tlv_writer *w)
{
    uint32_t replay_count;
    uint64_t last_event_seq;
    k_mutex_lock(&g_ucp_evt_mutex, K_FOREVER);
    replay_count = g_replay_count;
    last_event_seq = (g_evt_seq == 0ULL) ? 0ULL : (g_evt_seq - 1ULL);
    k_mutex_unlock(&g_ucp_evt_mutex);
    return (ucp_tlvw_put_u32(w, DIAG_RX_OK, UCP_TLV_U32, g_diag.rx_ok_frames) &&
            ucp_tlvw_put_u32(w, DIAG_RX_COBS_ERR, UCP_TLV_U32, g_diag.rx_cobs_err) &&
            ucp_tlvw_put_u32(w, DIAG_RX_CRC_ERR, UCP_TLV_U32, g_diag.rx_crc_err) &&
            ucp_tlvw_put_u32(w, DIAG_RX_PROTO_ERR, UCP_TLV_U32, g_diag.rx_proto_err) &&
            ucp_tlvw_put_u32(w, DIAG_RX_OVERSIZE, UCP_TLV_U32, g_diag.rx_oversize) &&
            ucp_tlvw_put_u32(w, DIAG_TX_FRAMES, UCP_TLV_U32, g_diag.tx_frames) &&
            ucp_tlvw_put_u32(w, DIAG_ERR_FRAMES, UCP_TLV_U32, g_diag.tx_error_frames) &&
            ucp_tlvw_put_u32(w, DIAG_EVT_SENT, UCP_TLV_U32, g_diag.event_frames) &&
            ucp_tlvw_put_u32(w, DIAG_STREAM_OUT_OPEN, UCP_TLV_U32, g_diag.stream_out_open) &&
            ucp_tlvw_put_u32(w, DIAG_STREAM_IN_OPEN, UCP_TLV_U32, g_diag.stream_in_open) &&
            ucp_tlvw_put_u32(w, DIAG_STREAM_ABORTS, UCP_TLV_U32, g_diag.stream_aborts) &&
            ucp_tlvw_put_u32(w, DIAG_STREAM_TIMEOUTS, UCP_TLV_U32, g_diag.stream_timeouts) &&
            ucp_tlvw_put_u32(w, DIAG_SESSION_HELLO, UCP_TLV_U32, g_diag.session_hello) &&
            ucp_tlvw_put_u32(w, DIAG_SESSION_TIMEOUTS, UCP_TLV_U32, g_diag.session_timeouts) &&
            ucp_tlvw_put_u32(w, DIAG_SUB_MASK, UCP_TLV_U32, g_sub_mask) &&
            ucp_tlvw_put_u32(w, DIAG_REPLAY_COUNT, UCP_TLV_U32, replay_count) &&
            ucp_tlvw_put_u64(w, DIAG_LAST_EVENT_SEQ, UCP_TLV_U64, last_event_seq) &&
            ucp_tlvw_put_u32(w, DIAG_LAST_RX_MS, UCP_TLV_U32, g_last_rx_ms) &&
            ucp_tlvw_put_u32(w, DIAG_EVT_COALESCED, UCP_TLV_U32, g_diag.event_coalesced) &&
            ucp_tlvw_put_u32(w, DIAG_EVT_RATE_LIMITED, UCP_TLV_U32, g_diag.event_rate_limited) &&
            ucp_tlvw_put_u32(w, DIAG_LAST_EVT_MS, UCP_TLV_U32, g_last_evt_ms) &&
            ucp_tlvw_put_u32(w, UCP_F_RESOURCE_VERSION, UCP_TLV_U32, g_ver_diag) &&
            ucp_tlvw_put_u64(w, UCP_F_TIMESTAMP_UTC_MS, UCP_TLV_U64, now_utc_ms())) ? 0 : -ENOSPC;
}

static int build_diag_stream_status(struct ucp_tlv_writer *w)
{
    return (ucp_tlvw_put_u8(w, DSTRM_OUT_ACTIVE, UCP_TLV_BOOL, g_out_stream.active ? 1U : 0U) &&
            ucp_tlvw_put_u16(w, DSTRM_OUT_ID, UCP_TLV_U16, g_out_stream.id) &&
            ucp_tlvw_put_u32(w, DSTRM_OUT_OFF, UCP_TLV_U32, g_out_stream.offset) &&
            ucp_tlvw_put_u32(w, DSTRM_OUT_TOTAL, UCP_TLV_U32, g_out_stream.total_size) &&
            ucp_tlvw_put_u8(w, DSTRM_IN_ACTIVE, UCP_TLV_BOOL, g_in_stream.active ? 1U : 0U) &&
            ucp_tlvw_put_u16(w, DSTRM_IN_ID, UCP_TLV_U16, g_in_stream.id) &&
            ucp_tlvw_put_u32(w, DSTRM_IN_RX, UCP_TLV_U32, g_in_stream.received) &&
            ucp_tlvw_put_u32(w, DSTRM_IN_CAP, UCP_TLV_U32, UCP_STREAM_BUF_MAX) &&
            ucp_tlvw_put_u8(w, DSTRM_IN_PURPOSE, UCP_TLV_U8, g_in_stream.purpose) &&
            ucp_tlvw_put_u32(w, UCP_F_RESOURCE_VERSION, UCP_TLV_U32, g_ver_diag) &&
            ucp_tlvw_put_u64(w, UCP_F_TIMESTAMP_UTC_MS, UCP_TLV_U64, now_utc_ms())) ? 0 : -ENOSPC;
}

static int build_diag_domain_status(struct ucp_tlv_writer *w)
{
    return (ucp_tlvw_put_u32(w, DDOM_VER_SYS, UCP_TLV_U32, g_ver_sys) &&
            ucp_tlvw_put_u32(w, DDOM_VER_TZ, UCP_TLV_U32, g_ver_tz) &&
            ucp_tlvw_put_u32(w, DDOM_VER_RTC, UCP_TLV_U32, g_ver_rtc) &&
            ucp_tlvw_put_u32(w, DDOM_VER_TASK, UCP_TLV_U32, g_ver_task) &&
            ucp_tlvw_put_u32(w, DDOM_VER_OTA, UCP_TLV_U32, g_ver_ota) &&
            ucp_tlvw_put_u32(w, DDOM_VER_PEER, UCP_TLV_U32, g_ver_peer) &&
            ucp_tlvw_put_u32(w, DDOM_VER_DIAG, UCP_TLV_U32, g_ver_diag) &&
            ucp_tlvw_put_u32(w, DDOM_VER_SCHEMA, UCP_TLV_U32, g_ver_schema) &&
            ucp_tlvw_put_u32(w, DDOM_VER_FLOW, UCP_TLV_U32, g_ver_flow) &&
            ucp_tlvw_put_u32(w, DDOM_VER_RAIN, UCP_TLV_U32, g_ver_rain) &&
            ucp_tlvw_put_u32(w, DDOM_VER_ENV, UCP_TLV_U32, g_ver_env) &&
            ucp_tlvw_put_u32(w, DDOM_VER_HISTORY, UCP_TLV_U32, g_ver_history) &&
            ucp_tlvw_put_u32(w, DDOM_VER_PACKS, UCP_TLV_U32, g_ver_packs) &&
            ucp_tlvw_put_u32(w, DDOM_LAST_POLL_CFG_MS, UCP_TLV_U32, g_last_poll_cfg_ms) &&
            ucp_tlvw_put_u32(w, DDOM_LAST_POLL_SYS_MS, UCP_TLV_U32, g_last_poll_system_ms) &&
            ucp_tlvw_put_u32(w, DDOM_LAST_POLL_SENSOR_MS, UCP_TLV_U32, g_last_poll_sensors_ms) &&
            ucp_tlvw_put_u8(w, DDOM_PEER_HW, UCP_TLV_BOOL, g_peer_power_hw_backed ? 1U : 0U) &&
            ucp_tlvw_put_u32(w, UCP_F_RESOURCE_VERSION, UCP_TLV_U32, g_ver_diag) &&
            ucp_tlvw_put_u64(w, UCP_F_TIMESTAMP_UTC_MS, UCP_TLV_U64, now_utc_ms())) ? 0 : -ENOSPC;
}

static int build_schema_proto_info(struct ucp_tlv_writer *w)
{
    return (ucp_tlvw_put_u8(w, UCP_F_VERSION, UCP_TLV_U8, UCP_PROTO_V1) &&
            ucp_tlvw_put_u32(w, UCP_F_CAPABILITIES, UCP_TLV_U32, UCP_HELLO_CAPS_MASK) &&
            ucp_tlvw_put_u16(w, UCP_F_MAX_PAYLOAD, UCP_TLV_U16, UCP_MAX_PAYLOAD_V1) &&
            ucp_tlvw_put_u16(w, UCP_F_REPLAY_DEPTH, UCP_TLV_U16, UCP_REPLAY_DEPTH) &&
            ucp_tlvw_put_u32(w, SCHEMA_STREAM_DIR_MASK, UCP_TLV_U32, (uint32_t)(BIT(STRM_DIR_OUTBOUND - 1) | BIT(STRM_DIR_INBOUND - 1))) &&
            ucp_tlvw_put_u32(w, SCHEMA_STREAM_PURPOSE_MASK, UCP_TLV_U32,
                             (uint32_t)(BIT(STRM_PURPOSE_DIAG_SNAPSHOT - 1) | BIT(STRM_PURPOSE_SCHEMA_TEXT - 1) |
                                        BIT(STRM_PURPOSE_UPLOAD_STAGING - 1) | BIT(STRM_PURPOSE_HISTORY_PAGE - 1) |
                                        BIT(STRM_PURPOSE_PACKS_LIST - 1) | BIT(STRM_PURPOSE_PACKS_PLANTS_LIST - 1) |
                                        BIT(STRM_PURPOSE_SCHEMA_CATALOG - 1) | BIT(STRM_PURPOSE_RAIN_HISTORY_RANGE - 1) |
                                        BIT(STRM_PURPOSE_ENV_HISTORY_RANGE - 1) | BIT(STRM_PURPOSE_DB_CATALOG - 1) |
                                        BIT(STRM_PURPOSE_BULK_SNAPSHOT - 1) | BIT(STRM_PURPOSE_WATER_HISTORY_RANGE - 1))) &&
            ucp_tlvw_put_u32(w, UCP_F_RESOURCE_VERSION, UCP_TLV_U32, g_ver_schema) &&
            ucp_tlvw_put_u64(w, UCP_F_TIMESTAMP_UTC_MS, UCP_TLV_U64, now_utc_ms())) ? 0 : -ENOSPC;
}

static int build_schema_service_mask(struct ucp_tlv_writer *w)
{
    uint32_t svc_mask = 0U;
    for (size_t i = 0U; i < ARRAY_SIZE(g_schema_resources); i++) {
        uint16_t svc = g_schema_resources[i].svc;
        if (svc >= 1U && svc <= 32U) {
            svc_mask |= BIT(svc - 1U);
        }
    }
    return (ucp_tlvw_put_u8(w, UCP_F_VERSION, UCP_TLV_U8, 1U) &&
            ucp_tlvw_put_u32(w, SCHEMA_SERVICE_MASK, UCP_TLV_U32, svc_mask) &&
            ucp_tlvw_put_u32(w, UCP_F_RESOURCE_VERSION, UCP_TLV_U32, g_ver_schema) &&
            ucp_tlvw_put_u64(w, UCP_F_TIMESTAMP_UTC_MS, UCP_TLV_U64, now_utc_ms())) ? 0 : -ENOSPC;
}

static int build_schema_topic_mask(struct ucp_tlv_writer *w)
{
    const uint32_t topic_mask = BIT(UCP_TOPIC_SYSTEM_STATUS - 1) | BIT(UCP_TOPIC_RTC_TZ_CHANGED - 1) |
                                BIT(UCP_TOPIC_TASK_CURRENT - 1) | BIT(UCP_TOPIC_ALARM - 1) | BIT(UCP_TOPIC_OTA_TRANSITION - 1) |
                                BIT(UCP_TOPIC_PEER_POWER - 1) | BIT(UCP_TOPIC_DIAGNOSTICS - 1) |
                                BIT(UCP_TOPIC_CHANNEL_STATE - 1) | BIT(UCP_TOPIC_CHANNEL_CONFIG - 1) |
                                BIT(UCP_TOPIC_SCHEDULE_CONFIG - 1) | BIT(UCP_TOPIC_FLOW_LIVE - 1) |
                                BIT(UCP_TOPIC_RAIN_LIVE - 1) | BIT(UCP_TOPIC_ENV_LIVE - 1) |
                                BIT(UCP_TOPIC_HISTORY_READY - 1) | BIT(UCP_TOPIC_PACKS_CHANGED - 1) |
                                BIT(UCP_TOPIC_SYSTEM_CONFIG - 1) | BIT(UCP_TOPIC_STATISTICS - 1) |
                                BIT(UCP_TOPIC_AUTO_CALC_STATUS - 1) | BIT(UCP_TOPIC_HYDRAULIC_STATUS - 1) |
                                BIT(UCP_TOPIC_COMP_STATUS - 1) | BIT(UCP_TOPIC_COMP_CONFIG - 1) |
                                BIT(UCP_TOPIC_ONBOARDING_STATUS - 1) | BIT(UCP_TOPIC_RESET_PROGRESS - 1) |
                                BIT(UCP_TOPIC_CUSTOM_SOIL - 1) | BIT(UCP_TOPIC_SOIL_MOISTURE - 1) |
                                BIT(UCP_TOPIC_INTERVAL_MODE - 1) | BIT(UCP_TOPIC_CHANNEL_CFG_STATUS - 1) |
                                BIT(UCP_TOPIC_RAIN_INTEGRATION - 1) | BIT(UCP_TOPIC_BULK_SNAPSHOT - 1) |
                                BIT(UCP_TOPIC_FLOW_CALIBRATION - 1);
    return (ucp_tlvw_put_u32(w, UCP_F_TOPIC_MASK, UCP_TLV_U32, topic_mask) &&
            ucp_tlvw_put_u32(w, UCP_F_RESOURCE_VERSION, UCP_TLV_U32, g_ver_schema) &&
            ucp_tlvw_put_u64(w, UCP_F_TIMESTAMP_UTC_MS, UCP_TLV_U64, now_utc_ms())) ? 0 : -ENOSPC;
}

static int build_schema_stream_caps(struct ucp_tlv_writer *w)
{
    return (ucp_tlvw_put_u32(w, SCHEMA_STREAM_DIR_MASK, UCP_TLV_U32, (uint32_t)(BIT(STRM_DIR_OUTBOUND - 1) | BIT(STRM_DIR_INBOUND - 1))) &&
            ucp_tlvw_put_u32(w, SCHEMA_STREAM_PURPOSE_MASK, UCP_TLV_U32,
                             (uint32_t)(BIT(STRM_PURPOSE_DIAG_SNAPSHOT - 1) | BIT(STRM_PURPOSE_SCHEMA_TEXT - 1) |
                                        BIT(STRM_PURPOSE_UPLOAD_STAGING - 1) | BIT(STRM_PURPOSE_HISTORY_PAGE - 1) |
                                        BIT(STRM_PURPOSE_PACKS_LIST - 1) | BIT(STRM_PURPOSE_PACKS_PLANTS_LIST - 1) |
                                        BIT(STRM_PURPOSE_SCHEMA_CATALOG - 1) | BIT(STRM_PURPOSE_RAIN_HISTORY_RANGE - 1) |
                                        BIT(STRM_PURPOSE_ENV_HISTORY_RANGE - 1) | BIT(STRM_PURPOSE_DB_CATALOG - 1) |
                                        BIT(STRM_PURPOSE_BULK_SNAPSHOT - 1) | BIT(STRM_PURPOSE_WATER_HISTORY_RANGE - 1))) &&
            ucp_tlvw_put_u16(w, STRM_CHUNK_SIZE, UCP_TLV_U16, UCP_STREAM_CHUNK_MAX) &&
            ucp_tlvw_put_u32(w, STRM_TOTAL_SIZE, UCP_TLV_U32, UCP_STREAM_BUF_MAX) &&
            ucp_tlvw_put_u32(w, UCP_F_RESOURCE_VERSION, UCP_TLV_U32, g_ver_schema) &&
            ucp_tlvw_put_u64(w, UCP_F_TIMESTAMP_UTC_MS, UCP_TLV_U64, now_utc_ms())) ? 0 : -ENOSPC;
}

static bool parse_schema_res_query(const uint8_t *p, uint16_t len, uint16_t *svc, uint16_t *res)
{
    struct ucp_tlv_elem e;
    size_t off = 0U;
    bool has_svc = false, has_res = false;
    while (ucp_tlv_next(p, len, &off, &e)) {
        if (e.field_id == UCP_F_SERVICE_ID) {
            if (!ucp_tlv_read_u16(&e, svc)) return false;
            has_svc = true;
        } else if (e.field_id == UCP_F_RESOURCE_ID) {
            if (!ucp_tlv_read_u16(&e, res)) return false;
            has_res = true;
        }
    }
    return has_svc && has_res;
}

static bool parse_schema_stream_query(const uint8_t *p, uint16_t len, uint8_t *purpose)
{
    struct ucp_tlv_elem e;
    size_t off = 0U;
    while (ucp_tlv_next(p, len, &off, &e)) {
        if (e.field_id == STRM_PURPOSE) {
            return ucp_tlv_read_u8(&e, purpose);
        }
    }
    return false;
}

static bool parse_schema_field_query(const uint8_t *p, uint16_t len, uint16_t *svc, uint16_t *res, uint8_t *field_id)
{
    struct ucp_tlv_elem e;
    size_t off = 0U;
    bool has_svc = false, has_res = false, has_field = false;
    while (ucp_tlv_next(p, len, &off, &e)) {
        if (e.field_id == UCP_F_SERVICE_ID) {
            if (!ucp_tlv_read_u16(&e, svc)) return false;
            has_svc = true;
        } else if (e.field_id == UCP_F_RESOURCE_ID) {
            if (!ucp_tlv_read_u16(&e, res)) return false;
            has_res = true;
        } else if (e.field_id == SCHEMA_QUERY_FIELD_ID || e.field_id == SCHEMA_FIELD_ID) {
            if (!ucp_tlv_read_u8(&e, field_id)) return false;
            has_field = true;
        }
    }
    return has_svc && has_res && has_field;
}

static int build_schema_resource_desc(struct ucp_tlv_writer *w, uint16_t svc, uint16_t res)
{
    const struct ucp_schema_resource_desc *d = schema_find_resource_desc(svc, res);
    if (!d) return -ENOENT;
    return (ucp_tlvw_put_u16(w, UCP_F_SERVICE_ID, UCP_TLV_U16, d->svc) &&
            ucp_tlvw_put_u16(w, UCP_F_RESOURCE_ID, UCP_TLV_U16, d->res) &&
            ucp_tlvw_put_u8(w, SCHEMA_KIND, UCP_TLV_U8, d->kind) &&
            ucp_tlvw_put_u8(w, SCHEMA_ACCESS_MASK, UCP_TLV_U8, d->access_mask) &&
            ucp_tlvw_put_u16(w, SCHEMA_EVENT_TOPIC, UCP_TLV_U16, d->topic_id) &&
            ucp_tlvw_put_u8(w, SCHEMA_FIELD_COUNT, UCP_TLV_U8, d->field_count) &&
            ucp_tlvw_put_str(w, SCHEMA_NAME, UCP_TLV_UTF8, d->name) &&
            ucp_tlvw_put_str(w, SCHEMA_DESC_TEXT, UCP_TLV_UTF8, d->desc) &&
            ucp_tlvw_put_u32(w, UCP_F_RESOURCE_VERSION, UCP_TLV_U32, g_ver_schema) &&
            ucp_tlvw_put_u64(w, UCP_F_TIMESTAMP_UTC_MS, UCP_TLV_U64, now_utc_ms())) ? 0 : -ENOSPC;
}

static int build_schema_stream_purpose_desc(struct ucp_tlv_writer *w, uint8_t purpose)
{
    const struct ucp_schema_stream_purpose_desc *d = schema_find_stream_purpose_desc(purpose);
    if (!d) return -ENOENT;
    return (ucp_tlvw_put_u8(w, STRM_PURPOSE, UCP_TLV_U8, d->purpose) &&
            ucp_tlvw_put_u32(w, SCHEMA_STREAM_DIR_MASK, UCP_TLV_U32, d->dir_mask) &&
            ucp_tlvw_put_u16(w, STRM_CHUNK_SIZE, UCP_TLV_U16, UCP_STREAM_CHUNK_MAX) &&
            ucp_tlvw_put_u32(w, STRM_TOTAL_SIZE, UCP_TLV_U32, d->max_total) &&
            ucp_tlvw_put_str(w, SCHEMA_NAME, UCP_TLV_UTF8, d->name) &&
            ucp_tlvw_put_str(w, SCHEMA_DESC_TEXT, UCP_TLV_UTF8, d->desc) &&
            ucp_tlvw_put_u32(w, UCP_F_RESOURCE_VERSION, UCP_TLV_U32, g_ver_schema) &&
            ucp_tlvw_put_u64(w, UCP_F_TIMESTAMP_UTC_MS, UCP_TLV_U64, now_utc_ms())) ? 0 : -ENOSPC;
}

static int build_schema_field_desc(struct ucp_tlv_writer *w, uint16_t svc, uint16_t res, uint8_t field_id)
{
    const struct ucp_schema_resource_desc *rd = schema_find_resource_desc(svc, res);
    uint8_t field_type = UCP_TLV_BYTES;
    uint8_t field_flags = 0U;
    const char *name = "opaque_or_mixed";
    const char *unit = "";

    if (!rd) return -ENOENT;

    /* Best-effort machine-readable field metadata; use schema catalog stream for exhaustive mapping text. */
    switch (field_id) {
    case UCP_F_RESOURCE_VERSION: field_type = UCP_TLV_U32; name = "resource_version"; break;
    case UCP_F_TIMESTAMP_UTC_MS: field_type = UCP_TLV_U64; name = "timestamp_utc_ms"; unit = "ms"; break;
    case UCP_F_OK_BOOL: field_type = UCP_TLV_BOOL; name = "ok"; break;
    case Q_CHANNEL_ID: field_type = UCP_TLV_U8; name = "query.channel_id"; field_flags = BIT(0); break;
    case Q_PAGE_INDEX: field_type = UCP_TLV_U16; name = "query.page_index"; field_flags = BIT(0); break;
    case Q_OFFSET: field_type = UCP_TLV_U16; name = "query.offset"; field_flags = BIT(0); break;
    default:
        break;
    }

    if (svc == UCP_SVC_CHANNEL && (res == UCP_RES_CHANNEL_STATE || res == UCP_RES_CHANNEL_CONFIG) && field_id == RES_CH_ID) {
        field_type = UCP_TLV_U8; name = "channel_id";
    } else if (svc == UCP_SVC_SCHEMA &&
               (res == UCP_RES_SCHEMA_STREAM_CAPS || res == UCP_RES_SCHEMA_STREAM_PURPOSE_DESC)) {
        if (field_id == STRM_PURPOSE) { field_type = UCP_TLV_U8; name = "stream_purpose"; }
        else if (field_id == STRM_CHUNK_SIZE) { field_type = UCP_TLV_U16; name = "chunk_size"; unit = "bytes"; }
        else if (field_id == STRM_TOTAL_SIZE) { field_type = UCP_TLV_U32; name = "total_size"; unit = "bytes"; }
    } else if (svc == UCP_SVC_OTA) {
        if (field_id == OTA_CONFIRM) { field_type = UCP_TLV_U32; name = "ota_confirm_token_or_state"; }
        else if (field_id == OTA_SWAP) { field_type = UCP_TLV_I32; name = "mcuboot_swap_type"; }
        else if (field_id == OTA_STATE) { field_type = UCP_TLV_U8; name = "ota_state"; }
        else if (field_id == OTA_PENDING) { field_type = UCP_TLV_BOOL; name = "ota_pending"; }
    }

    return (ucp_tlvw_put_u16(w, UCP_F_SERVICE_ID, UCP_TLV_U16, svc) &&
            ucp_tlvw_put_u16(w, UCP_F_RESOURCE_ID, UCP_TLV_U16, res) &&
            ucp_tlvw_put_u8(w, SCHEMA_FIELD_ID, UCP_TLV_U8, field_id) &&
            ucp_tlvw_put_u8(w, SCHEMA_FIELD_TYPE, UCP_TLV_U8, field_type) &&
            ucp_tlvw_put_u8(w, SCHEMA_FIELD_FLAGS, UCP_TLV_U8, field_flags) &&
            ucp_tlvw_put_str(w, SCHEMA_NAME, UCP_TLV_UTF8, name) &&
            ucp_tlvw_put_str(w, SCHEMA_UNIT, UCP_TLV_UTF8, unit) &&
            ucp_tlvw_put_str(w, SCHEMA_DESC_TEXT, UCP_TLV_UTF8, "For exhaustive field map use STREAM purpose schema_catalog") &&
            ucp_tlvw_put_u32(w, UCP_F_RESOURCE_VERSION, UCP_TLV_U32, g_ver_schema) &&
            ucp_tlvw_put_u64(w, UCP_F_TIMESTAMP_UTC_MS, UCP_TLV_U64, now_utc_ms())) ? 0 : -ENOSPC;
}

static int handle_get(uint16_t svc, uint16_t res, const uint8_t *p, uint16_t len, uint32_t req)
{
    uint8_t b[UCP_MAX_PAYLOAD_V1]; struct ucp_tlv_writer w; timezone_config_t tz; rtc_datetime_t dt;
    struct ucp_task_snapshot task_snap;
    uint8_t channel_id;
    ucp_tlvw_init(&w, b, sizeof(b));
    if (svc == UCP_SVC_DEVICE && res == UCP_RES_DEVICE_SYSTEM_STATUS) {
        int rc = build_system(&w);
        return send_tlv(UCP_MSG_GET_RESP, svc, res, (rc == 0) ? UCP_STATUS_OK : UCP_STATUS_INTERNAL, (rc == 0) ? &w : NULL, req);
    }
    if (svc == UCP_SVC_DEVICE && res == UCP_RES_DEVICE_SYSTEM_CONFIG) {
        int rc = build_system_config_bytes(&w);
        return send_tlv(UCP_MSG_GET_RESP, svc, res, (rc == 0) ? UCP_STATUS_OK : UCP_STATUS_INTERNAL, (rc == 0) ? &w : NULL, req);
    }
    if (svc == UCP_SVC_DEVICE && res == UCP_RES_DEVICE_STATISTICS) {
        int rc;
        if (!parse_channel_selector_optional(p, len, &channel_id, 0U, false)) {
            return send_tlv(UCP_MSG_GET_RESP, svc, res, UCP_STATUS_INVALID_ARG, NULL, req);
        }
        rc = build_statistics_bytes(&w, channel_id);
        if (rc == -EINVAL) return send_tlv(UCP_MSG_GET_RESP, svc, res, UCP_STATUS_INVALID_ARG, NULL, req);
        return send_tlv(UCP_MSG_GET_RESP, svc, res, (rc == 0) ? UCP_STATUS_OK : UCP_STATUS_INTERNAL, (rc == 0) ? &w : NULL, req);
    }
    if (svc == UCP_SVC_DEVICE && res == UCP_RES_DEVICE_AUTO_CALC_STATUS) {
        int rc;
        if (!parse_channel_selector_optional(p, len, &channel_id, 0xFFU, true)) {
            return send_tlv(UCP_MSG_GET_RESP, svc, res, UCP_STATUS_INVALID_ARG, NULL, req);
        }
        rc = build_auto_calc_status_bytes(&w, channel_id);
        if (rc == -EINVAL) return send_tlv(UCP_MSG_GET_RESP, svc, res, UCP_STATUS_INVALID_ARG, NULL, req);
        if (rc == -ENOENT) return send_tlv(UCP_MSG_GET_RESP, svc, res, UCP_STATUS_NOT_FOUND, NULL, req);
        return send_tlv(UCP_MSG_GET_RESP, svc, res, (rc == 0) ? UCP_STATUS_OK : UCP_STATUS_INTERNAL, (rc == 0) ? &w : NULL, req);
    }
    if (svc == UCP_SVC_DEVICE && res == UCP_RES_DEVICE_AUTO_CALC_CONTROL) {
        int rc = build_auto_calc_control_bytes(&w);
        return send_tlv(UCP_MSG_GET_RESP, svc, res, (rc == 0) ? UCP_STATUS_OK : UCP_STATUS_INTERNAL, (rc == 0) ? &w : NULL, req);
    }
    if (svc == UCP_SVC_DEVICE && res == UCP_RES_DEVICE_BULK_SNAPSHOT) {
        int rc = build_bulk_snapshot_bytes(&w);
        return send_tlv(UCP_MSG_GET_RESP, svc, res, (rc == 0) ? UCP_STATUS_OK : UCP_STATUS_INTERNAL, (rc == 0) ? &w : NULL, req);
    }
    if (svc == UCP_SVC_DEVICE && res == UCP_RES_DEVICE_ALARM_STATUS) {
        int rc = build_alarm_status(&w);
        return send_tlv(UCP_MSG_GET_RESP, svc, res, (rc == 0) ? UCP_STATUS_OK : UCP_STATUS_INTERNAL, (rc == 0) ? &w : NULL, req);
    }
    if (svc == UCP_SVC_TASK && res == UCP_RES_TASK_STATUS) {
        if (collect_task_snapshot(&task_snap) != 0) return send_tlv(UCP_MSG_GET_RESP, svc, res, UCP_STATUS_INTERNAL, NULL, req);
        return send_tlv(UCP_MSG_GET_RESP, svc, res, (build_task_status_from_snapshot(&w, &task_snap) == 0) ? UCP_STATUS_OK : UCP_STATUS_INTERNAL, &w, req);
    }
    if (svc == UCP_SVC_CHANNEL && res == UCP_RES_CHANNEL_STATE) {
        struct ucp_channel_state_snapshot s;
        if (!parse_channel_id_required(p, len, &channel_id)) return send_tlv(UCP_MSG_GET_RESP, svc, res, UCP_STATUS_INVALID_ARG, NULL, req);
        if (collect_channel_state_snapshot(channel_id, &s) != 0) return send_tlv(UCP_MSG_GET_RESP, svc, res, UCP_STATUS_NOT_FOUND, NULL, req);
        return send_tlv(UCP_MSG_GET_RESP, svc, res, (build_channel_state(&w, channel_id, &s) == 0) ? UCP_STATUS_OK : UCP_STATUS_INTERNAL, &w, req);
    }
    if (svc == UCP_SVC_CHANNEL && res == UCP_RES_CHANNEL_CONFIG) {
        struct ucp_channel_config_snapshot s;
        if (!parse_channel_id_required(p, len, &channel_id)) return send_tlv(UCP_MSG_GET_RESP, svc, res, UCP_STATUS_INVALID_ARG, NULL, req);
        if (collect_channel_config_snapshot(channel_id, &s) != 0) return send_tlv(UCP_MSG_GET_RESP, svc, res, UCP_STATUS_NOT_FOUND, NULL, req);
        return send_tlv(UCP_MSG_GET_RESP, svc, res, (build_channel_config(&w, channel_id, &s) == 0) ? UCP_STATUS_OK : UCP_STATUS_INTERNAL, &w, req);
    }
    if (svc == UCP_SVC_CHANNEL && res == UCP_RES_CHANNEL_CUSTOM_SOIL) {
        int rc;
        if (!parse_channel_id_required(p, len, &channel_id)) return send_tlv(UCP_MSG_GET_RESP, svc, res, UCP_STATUS_INVALID_ARG, NULL, req);
        rc = build_custom_soil_bytes(&w, channel_id);
        return send_tlv(UCP_MSG_GET_RESP, svc, res, (rc == 0) ? UCP_STATUS_OK : UCP_STATUS_INTERNAL, (rc == 0) ? &w : NULL, req);
    }
    if (svc == UCP_SVC_CHANNEL && res == UCP_RES_CHANNEL_SOIL_MOISTURE) {
        int rc;
        if (!parse_channel_selector_optional(p, len, &channel_id, 0xFFU, true)) return send_tlv(UCP_MSG_GET_RESP, svc, res, UCP_STATUS_INVALID_ARG, NULL, req);
        rc = build_soil_moisture_bytes(&w, channel_id);
        if (rc == -EINVAL) return send_tlv(UCP_MSG_GET_RESP, svc, res, UCP_STATUS_INVALID_ARG, NULL, req);
        return send_tlv(UCP_MSG_GET_RESP, svc, res, (rc == 0) ? UCP_STATUS_OK : UCP_STATUS_INTERNAL, (rc == 0) ? &w : NULL, req);
    }
    if (svc == UCP_SVC_CHANNEL && res == UCP_RES_CHANNEL_INTERVAL_CONFIG) {
        int rc;
        if (!parse_channel_id_required(p, len, &channel_id)) return send_tlv(UCP_MSG_GET_RESP, svc, res, UCP_STATUS_INVALID_ARG, NULL, req);
        rc = build_interval_config_bytes(&w, channel_id);
        if (rc == -EINVAL) return send_tlv(UCP_MSG_GET_RESP, svc, res, UCP_STATUS_INVALID_ARG, NULL, req);
        return send_tlv(UCP_MSG_GET_RESP, svc, res, (rc == 0) ? UCP_STATUS_OK : UCP_STATUS_INTERNAL, (rc == 0) ? &w : NULL, req);
    }
    if (svc == UCP_SVC_CHANNEL && res == UCP_RES_CHANNEL_CONFIG_STATUS) {
        int rc;
        if (!parse_channel_id_required(p, len, &channel_id)) return send_tlv(UCP_MSG_GET_RESP, svc, res, UCP_STATUS_INVALID_ARG, NULL, req);
        rc = build_onb_config_status_bytes(&w, channel_id);
        if (rc == -EINVAL) return send_tlv(UCP_MSG_GET_RESP, svc, res, UCP_STATUS_INVALID_ARG, NULL, req);
        return send_tlv(UCP_MSG_GET_RESP, svc, res, (rc == 0) ? UCP_STATUS_OK : UCP_STATUS_INTERNAL, (rc == 0) ? &w : NULL, req);
    }
    if (svc == UCP_SVC_SCHEDULE && res == UCP_RES_SCHEDULE_CONFIG) {
        struct ucp_schedule_snapshot s;
        if (!parse_channel_id_required(p, len, &channel_id)) return send_tlv(UCP_MSG_GET_RESP, svc, res, UCP_STATUS_INVALID_ARG, NULL, req);
        if (collect_schedule_snapshot(channel_id, &s) != 0) return send_tlv(UCP_MSG_GET_RESP, svc, res, UCP_STATUS_NOT_FOUND, NULL, req);
        return send_tlv(UCP_MSG_GET_RESP, svc, res, (build_schedule_config(&w, channel_id, &s.ev) == 0) ? UCP_STATUS_OK : UCP_STATUS_INTERNAL, &w, req);
    }
    if (svc == UCP_SVC_FLOW && res == UCP_RES_FLOW_STATUS) {
        struct ucp_flow_snapshot s;
        if (collect_flow_snapshot(&s) != 0) return send_tlv(UCP_MSG_GET_RESP, svc, res, UCP_STATUS_INTERNAL, NULL, req);
        return send_tlv(UCP_MSG_GET_RESP, svc, res, (build_flow_status(&w, &s) == 0) ? UCP_STATUS_OK : UCP_STATUS_INTERNAL, &w, req);
    }
    if (svc == UCP_SVC_FLOW && res == UCP_RES_FLOW_HYDRAULIC_STATUS) {
        int rc;
        if (!parse_channel_selector_optional(p, len, &channel_id, 0U, false)) return send_tlv(UCP_MSG_GET_RESP, svc, res, UCP_STATUS_INVALID_ARG, NULL, req);
        rc = build_hydraulic_status_bytes(&w, channel_id);
        if (rc == -EINVAL) return send_tlv(UCP_MSG_GET_RESP, svc, res, UCP_STATUS_INVALID_ARG, NULL, req);
        if (rc == -ENOENT) return send_tlv(UCP_MSG_GET_RESP, svc, res, UCP_STATUS_NOT_FOUND, NULL, req);
        return send_tlv(UCP_MSG_GET_RESP, svc, res, (rc == 0) ? UCP_STATUS_OK : UCP_STATUS_INTERNAL, (rc == 0) ? &w : NULL, req);
    }
    if (svc == UCP_SVC_FLOW && res == UCP_RES_FLOW_CALIBRATION_STATUS) {
        int rc = build_flow_calibration_status_bytes(&w);
        return send_tlv(UCP_MSG_GET_RESP, svc, res, (rc == 0) ? UCP_STATUS_OK : UCP_STATUS_INTERNAL, (rc == 0) ? &w : NULL, req);
    }
    if (svc == UCP_SVC_RAIN && res == UCP_RES_RAIN_STATUS) {
        struct ucp_rain_snapshot s;
        if (collect_rain_snapshot(&s) != 0) return send_tlv(UCP_MSG_GET_RESP, svc, res, UCP_STATUS_INTERNAL, NULL, req);
        return send_tlv(UCP_MSG_GET_RESP, svc, res, (build_rain_status(&w, &s) == 0) ? UCP_STATUS_OK : UCP_STATUS_INTERNAL, &w, req);
    }
    if (svc == UCP_SVC_ENVIRONMENT && res == UCP_RES_ENV_CURRENT) {
        struct ucp_env_snapshot s;
        if (collect_env_snapshot(&s) != 0) return send_tlv(UCP_MSG_GET_RESP, svc, res, UCP_STATUS_INTERNAL, NULL, req);
        return send_tlv(UCP_MSG_GET_RESP, svc, res, (build_env_status(&w, &s) == 0) ? UCP_STATUS_OK : UCP_STATUS_INTERNAL, &w, req);
    }
    if (svc == UCP_SVC_ENVIRONMENT && res == UCP_RES_ENV_SENSOR_CONFIG) {
        int rc = build_env_sensor_config_bytes(&w);
        return send_tlv(UCP_MSG_GET_RESP, svc, res, (rc == 0) ? UCP_STATUS_OK : UCP_STATUS_INTERNAL, (rc == 0) ? &w : NULL, req);
    }
    if (svc == UCP_SVC_HISTORY && res == UCP_RES_HISTORY_STORAGE) {
        storage_requirements_t st = {0};
        watering_error_t rc = watering_history_get_storage_info(&st);
        if (rc != WATERING_SUCCESS) return send_tlv(UCP_MSG_GET_RESP, svc, res, ucp_status_from_werr(rc), NULL, req);
        return send_tlv(UCP_MSG_GET_RESP, svc, res, (build_history_storage(&w, &st) == 0) ? UCP_STATUS_OK : UCP_STATUS_INTERNAL, &w, req);
    }
    if (svc == UCP_SVC_HISTORY && res == UCP_RES_HISTORY_RAIN_STORAGE) {
        int rc = build_history_rain_storage(&w);
        if (rc == -EAGAIN) {
            printk("GET rain_storage -> BUSY (cache not ready)\n");
            return send_tlv(UCP_MSG_GET_RESP, svc, res, UCP_STATUS_BUSY, NULL, req);
        }
        printk("GET rain_storage -> %s\n", (rc == 0) ? "OK" : "ERR");
        return send_tlv(UCP_MSG_GET_RESP, svc, res, (rc == 0) ? UCP_STATUS_OK : UCP_STATUS_INTERNAL, (rc == 0) ? &w : NULL, req);
    }
    if (svc == UCP_SVC_HISTORY && res == UCP_RES_HISTORY_ENV_STORAGE) {
        int rc = build_history_env_storage(&w);
        if (rc == -EAGAIN) {
            printk("GET env_storage -> BUSY (cache not ready)\n");
            return send_tlv(UCP_MSG_GET_RESP, svc, res, UCP_STATUS_BUSY, NULL, req);
        }
        printk("GET env_storage -> %s\n", (rc == 0) ? "OK" : "ERR");
        return send_tlv(UCP_MSG_GET_RESP, svc, res, (rc == 0) ? UCP_STATUS_OK : UCP_STATUS_INTERNAL, (rc == 0) ? &w : NULL, req);
    }
    if (svc == UCP_SVC_PACKS && res == UCP_RES_PACKS_STATS) {
        pack_storage_stats_t st = {0};
        if (!pack_storage_is_ready()) return send_tlv(UCP_MSG_GET_RESP, svc, res, UCP_STATUS_BUSY, NULL, req);
        if (!ucp_pack_stats_cache_get(&st)) {
            ucp_pack_stats_cache_request_refresh();
            return send_tlv(UCP_MSG_GET_RESP, svc, res, UCP_STATUS_BUSY, NULL, req);
        }
        if (ucp_pack_stats_cache_is_stale(k_uptime_get_32(), UCP_PACK_STATS_CACHE_TTL_MS)) {
            ucp_pack_stats_cache_request_refresh();
        }
        return send_tlv(UCP_MSG_GET_RESP, svc, res, (build_packs_stats(&w, &st) == 0) ? UCP_STATUS_OK : UCP_STATUS_INTERNAL, &w, req);
    }
    if (svc == UCP_SVC_RTC_TZ && res == UCP_RES_TZ_CONFIG) {
        if (timezone_get_config(&tz) < 0) return send_tlv(UCP_MSG_GET_RESP, svc, res, UCP_STATUS_INTERNAL, NULL, req);
        return send_tlv(UCP_MSG_GET_RESP, svc, res, (build_tz(&w, &tz) == 0) ? UCP_STATUS_OK : UCP_STATUS_INTERNAL, &w, req);
    }
    if (svc == UCP_SVC_RTC_TZ && res == UCP_RES_RTC_TIME) {
        if (rtc_datetime_get(&dt) < 0) return send_tlv(UCP_MSG_GET_RESP, svc, res, UCP_STATUS_HW_FAULT, NULL, req);
        return send_tlv(UCP_MSG_GET_RESP, svc, res, (build_rtc(&w, &dt) == 0) ? UCP_STATUS_OK : UCP_STATUS_INTERNAL, &w, req);
    }
    if (svc == UCP_SVC_COMPENSATION && res == UCP_RES_COMP_STATUS) {
        int rc;
        if (!parse_channel_selector_optional(p, len, &channel_id, 0U, false)) return send_tlv(UCP_MSG_GET_RESP, svc, res, UCP_STATUS_INVALID_ARG, NULL, req);
        rc = build_comp_status_bytes(&w, channel_id);
        if (rc == -EINVAL) return send_tlv(UCP_MSG_GET_RESP, svc, res, UCP_STATUS_INVALID_ARG, NULL, req);
        if (rc == -ENOENT) return send_tlv(UCP_MSG_GET_RESP, svc, res, UCP_STATUS_NOT_FOUND, NULL, req);
        return send_tlv(UCP_MSG_GET_RESP, svc, res, (rc == 0) ? UCP_STATUS_OK : UCP_STATUS_INTERNAL, (rc == 0) ? &w : NULL, req);
    }
    if (svc == UCP_SVC_COMPENSATION && res == UCP_RES_COMP_CHANNEL_CONFIG) {
        int rc;
        if (!parse_channel_selector_optional(p, len, &channel_id, 0U, false)) return send_tlv(UCP_MSG_GET_RESP, svc, res, UCP_STATUS_INVALID_ARG, NULL, req);
        rc = build_comp_channel_cfg_bytes(&w, channel_id);
        if (rc == -EINVAL) return send_tlv(UCP_MSG_GET_RESP, svc, res, UCP_STATUS_INVALID_ARG, NULL, req);
        if (rc == -ENOENT) return send_tlv(UCP_MSG_GET_RESP, svc, res, UCP_STATUS_NOT_FOUND, NULL, req);
        return send_tlv(UCP_MSG_GET_RESP, svc, res, (rc == 0) ? UCP_STATUS_OK : UCP_STATUS_INTERNAL, (rc == 0) ? &w : NULL, req);
    }
    if (svc == UCP_SVC_COMPENSATION && res == UCP_RES_COMP_RAIN_INTEGRATION) {
        int rc = build_rain_integration_bytes(&w);
        return send_tlv(UCP_MSG_GET_RESP, svc, res, (rc == 0) ? UCP_STATUS_OK : UCP_STATUS_INTERNAL, (rc == 0) ? &w : NULL, req);
    }
    if (svc == UCP_SVC_ONBOARDING_RESET && res == UCP_RES_ONB_STATUS) {
        int rc = build_onboarding_status_bytes(&w);
        return send_tlv(UCP_MSG_GET_RESP, svc, res, (rc == 0) ? UCP_STATUS_OK : UCP_STATUS_INTERNAL, (rc == 0) ? &w : NULL, req);
    }
    if (svc == UCP_SVC_ONBOARDING_RESET && res == UCP_RES_ONB_SYSTEM_OVERVIEW) {
        int rc = build_onb_system_overview_bytes(&w);
        return send_tlv(UCP_MSG_GET_RESP, svc, res, (rc == 0) ? UCP_STATUS_OK : UCP_STATUS_INTERNAL, (rc == 0) ? &w : NULL, req);
    }
    if (svc == UCP_SVC_ONBOARDING_RESET && res == UCP_RES_ONB_CONFIG_STATUS) {
        int rc;
        if (!parse_channel_id_required(p, len, &channel_id)) return send_tlv(UCP_MSG_GET_RESP, svc, res, UCP_STATUS_INVALID_ARG, NULL, req);
        rc = build_onb_config_status_bytes(&w, channel_id);
        if (rc == -EINVAL) return send_tlv(UCP_MSG_GET_RESP, svc, res, UCP_STATUS_INVALID_ARG, NULL, req);
        return send_tlv(UCP_MSG_GET_RESP, svc, res, (rc == 0) ? UCP_STATUS_OK : UCP_STATUS_INTERNAL, (rc == 0) ? &w : NULL, req);
    }
    if (svc == UCP_SVC_ONBOARDING_RESET && res == UCP_RES_ONB_RESET_CONTROL) {
        int rc = build_reset_control_bytes(&w);
        return send_tlv(UCP_MSG_GET_RESP, svc, res, (rc == 0) ? UCP_STATUS_OK : UCP_STATUS_INTERNAL, (rc == 0) ? &w : NULL, req);
    }
    if (svc == UCP_SVC_SCHEMA && res == UCP_RES_SCHEMA_IMPL_MASK) {
        (void)ucp_tlvw_put_u8(&w, UCP_F_VERSION, UCP_TLV_U8, 1U);
        (void)ucp_tlvw_put_u32(&w, SCHEMA_IMPL_MASK, UCP_TLV_U32, 0x0000000FU);
        return send_tlv(UCP_MSG_GET_RESP, svc, res, UCP_STATUS_OK, &w, req);
    }
    if (svc == UCP_SVC_SCHEMA && res == UCP_RES_SCHEMA_PROTO_INFO) {
        return send_tlv(UCP_MSG_GET_RESP, svc, res, (build_schema_proto_info(&w) == 0) ? UCP_STATUS_OK : UCP_STATUS_INTERNAL, &w, req);
    }
    if (svc == UCP_SVC_SCHEMA && res == UCP_RES_SCHEMA_SERVICE_MASK) {
        return send_tlv(UCP_MSG_GET_RESP, svc, res, (build_schema_service_mask(&w) == 0) ? UCP_STATUS_OK : UCP_STATUS_INTERNAL, &w, req);
    }
    if (svc == UCP_SVC_SCHEMA && res == UCP_RES_SCHEMA_TOPIC_MASK) {
        return send_tlv(UCP_MSG_GET_RESP, svc, res, (build_schema_topic_mask(&w) == 0) ? UCP_STATUS_OK : UCP_STATUS_INTERNAL, &w, req);
    }
    if (svc == UCP_SVC_SCHEMA && res == UCP_RES_SCHEMA_STREAM_CAPS) {
        return send_tlv(UCP_MSG_GET_RESP, svc, res, (build_schema_stream_caps(&w) == 0) ? UCP_STATUS_OK : UCP_STATUS_INTERNAL, &w, req);
    }
    if (svc == UCP_SVC_SCHEMA && res == UCP_RES_SCHEMA_RESOURCE_DESC) {
        uint16_t qsvc = 0U, qres = 0U;
        int rc;
        if (!parse_schema_res_query(p, len, &qsvc, &qres)) return send_tlv(UCP_MSG_GET_RESP, svc, res, UCP_STATUS_INVALID_ARG, NULL, req);
        rc = build_schema_resource_desc(&w, qsvc, qres);
        if (rc == -ENOENT) return send_tlv(UCP_MSG_GET_RESP, svc, res, UCP_STATUS_NOT_FOUND, NULL, req);
        return send_tlv(UCP_MSG_GET_RESP, svc, res, (rc == 0) ? UCP_STATUS_OK : UCP_STATUS_INTERNAL, (rc == 0) ? &w : NULL, req);
    }
    if (svc == UCP_SVC_SCHEMA && res == UCP_RES_SCHEMA_FIELD_DESC) {
        uint16_t qsvc = 0U, qres = 0U;
        uint8_t qfid = 0U;
        int rc;
        if (!parse_schema_field_query(p, len, &qsvc, &qres, &qfid)) return send_tlv(UCP_MSG_GET_RESP, svc, res, UCP_STATUS_INVALID_ARG, NULL, req);
        rc = build_schema_field_desc(&w, qsvc, qres, qfid);
        if (rc == -ENOENT) return send_tlv(UCP_MSG_GET_RESP, svc, res, UCP_STATUS_NOT_FOUND, NULL, req);
        return send_tlv(UCP_MSG_GET_RESP, svc, res, (rc == 0) ? UCP_STATUS_OK : UCP_STATUS_INTERNAL, (rc == 0) ? &w : NULL, req);
    }
    if (svc == UCP_SVC_SCHEMA && res == UCP_RES_SCHEMA_STREAM_PURPOSE_DESC) {
        uint8_t purpose = 0U;
        int rc;
        if (!parse_schema_stream_query(p, len, &purpose)) return send_tlv(UCP_MSG_GET_RESP, svc, res, UCP_STATUS_INVALID_ARG, NULL, req);
        rc = build_schema_stream_purpose_desc(&w, purpose);
        if (rc == -ENOENT) return send_tlv(UCP_MSG_GET_RESP, svc, res, UCP_STATUS_NOT_FOUND, NULL, req);
        return send_tlv(UCP_MSG_GET_RESP, svc, res, (rc == 0) ? UCP_STATUS_OK : UCP_STATUS_INTERNAL, (rc == 0) ? &w : NULL, req);
    }
    if (svc == UCP_SVC_PEER_POWER && res == UCP_RES_PEER_POWER_STATE) {
        return send_tlv(UCP_MSG_GET_RESP, svc, res, (build_peer_power_state(&w) == 0) ? UCP_STATUS_OK : UCP_STATUS_INTERNAL, &w, req);
    }
    if (svc == UCP_SVC_PEER_POWER && res == UCP_RES_PEER_POWER_POLICY) {
        return send_tlv(UCP_MSG_GET_RESP, svc, res, (build_peer_power_policy(&w) == 0) ? UCP_STATUS_OK : UCP_STATUS_INTERNAL, &w, req);
    }
    if (svc == UCP_SVC_DIAG && res == UCP_RES_DIAG_COUNTERS) {
        return send_tlv(UCP_MSG_GET_RESP, svc, res, (build_diag_counters(&w) == 0) ? UCP_STATUS_OK : UCP_STATUS_INTERNAL, &w, req);
    }
    if (svc == UCP_SVC_DIAG && res == UCP_RES_DIAG_STREAM_STATUS) {
        return send_tlv(UCP_MSG_GET_RESP, svc, res, (build_diag_stream_status(&w) == 0) ? UCP_STATUS_OK : UCP_STATUS_INTERNAL, &w, req);
    }
    if (svc == UCP_SVC_DIAG && res == UCP_RES_DIAG_DOMAIN_STATUS) {
        return send_tlv(UCP_MSG_GET_RESP, svc, res, (build_diag_domain_status(&w) == 0) ? UCP_STATUS_OK : UCP_STATUS_INTERNAL, &w, req);
    }
    if (svc == UCP_SVC_DIAG && res == UCP_RES_DIAG_STORAGE_HEALTH) {
        return send_tlv(UCP_MSG_GET_RESP, svc, res, (build_diag_storage_health(&w) == 0) ? UCP_STATUS_OK : UCP_STATUS_INTERNAL, &w, req);
    }
    return send_tlv(UCP_MSG_GET_RESP, svc, res, UCP_STATUS_NOT_FOUND, NULL, req);
}

static int send_set_ok_resp(uint16_t svc, uint16_t res, uint32_t instance_id, uint32_t version, uint32_t req)
{
    uint8_t rb[96];
    struct ucp_tlv_writer rw;
    ucp_tlvw_init(&rw, rb, sizeof(rb));
    (void)ucp_tlvw_put_u8(&rw, UCP_F_OK_BOOL, UCP_TLV_BOOL, 1U);
    (void)ucp_tlvw_put_u32(&rw, UCP_F_INSTANCE_ID, UCP_TLV_U32, instance_id);
    (void)ucp_tlvw_put_u32(&rw, UCP_F_RESOURCE_VERSION, UCP_TLV_U32, version);
    (void)ucp_tlvw_put_u64(&rw, UCP_F_TIMESTAMP_UTC_MS, UCP_TLV_U64, now_utc_ms());
    return send_tlv(UCP_MSG_SET_RESP, svc, res, UCP_STATUS_OK, &rw, req);
}

static int handle_set_channel_config(const uint8_t *p, uint16_t len, uint32_t req)
{
    watering_channel_t *ch = NULL;
    uint8_t channel_id = 0xFFU;
    struct ucp_tlv_elem e;
    size_t off = 0U;
    bool any = false;
    bool changed = false;
    uint8_t delta_b[UCP_MAX_EVT_DELTA];
    struct ucp_tlv_writer delta;

    if (!parse_channel_id_required(p, len, &channel_id)) {
        return send_tlv(UCP_MSG_SET_RESP, UCP_SVC_CHANNEL, UCP_RES_CHANNEL_CONFIG, UCP_STATUS_INVALID_ARG, NULL, req);
    }
    if (watering_get_channel(channel_id, &ch) != WATERING_SUCCESS || !ch) {
        return send_tlv(UCP_MSG_SET_RESP, UCP_SVC_CHANNEL, UCP_RES_CHANNEL_CONFIG, UCP_STATUS_NOT_FOUND, NULL, req);
    }

    ucp_tlvw_init(&delta, delta_b, sizeof(delta_b));
    while (ucp_tlv_next(p, len, &off, &e)) {
        if (e.field_id == RES_CH_ID) continue;
        any = true;
        if (e.field_id == RES_CH_NAME) {
            size_t nlen = MIN((size_t)e.len, sizeof(ch->name) - 1U);
            char tmp[64];
            memset(tmp, 0, sizeof(tmp));
            memcpy(tmp, e.value, nlen);
            if (strncmp(ch->name, tmp, sizeof(ch->name)) != 0) {
                memset(ch->name, 0, sizeof(ch->name));
                memcpy(ch->name, tmp, nlen);
                changed = true;
            }
            (void)ucp_tlvw_put_bytes(&delta, RES_CH_NAME, UCP_TLV_UTF8, (const uint8_t *)tmp, (uint16_t)nlen);
            continue;
        }
        if (e.field_id == RES_CH_PLANT_ID) {
            uint16_t v;
            if (!ucp_tlv_read_u16(&e, &v)) return send_tlv(UCP_MSG_SET_RESP,UCP_SVC_CHANNEL,UCP_RES_CHANNEL_CONFIG,UCP_STATUS_INVALID_ARG,NULL,req);
            if (ch->plant_id != v) { ch->plant_id = v; changed = true; }
            (void)ucp_tlvw_put_u16(&delta, RES_CH_PLANT_ID, UCP_TLV_U16, v);
            continue;
        }
        if (e.field_id == RES_CH_SOIL_DB || e.field_id == RES_CH_IRR_METHOD || e.field_id == RES_CH_AUTO_MODE || e.field_id == RES_CH_SUN_PCT) {
            uint8_t v;
            if (!ucp_tlv_read_u8(&e, &v)) return send_tlv(UCP_MSG_SET_RESP,UCP_SVC_CHANNEL,UCP_RES_CHANNEL_CONFIG,UCP_STATUS_INVALID_ARG,NULL,req);
            if (e.field_id == RES_CH_SUN_PCT && v > 100U) return send_tlv(UCP_MSG_SET_RESP,UCP_SVC_CHANNEL,UCP_RES_CHANNEL_CONFIG,UCP_STATUS_OUT_OF_RANGE,NULL,req);
            if (e.field_id == RES_CH_SOIL_DB) { if (ch->soil_db_index != v) { ch->soil_db_index = v; changed = true; } }
            else if (e.field_id == RES_CH_IRR_METHOD) { if (ch->irrigation_method_index != v) { ch->irrigation_method_index = v; changed = true; } }
            else if (e.field_id == RES_CH_AUTO_MODE) { if ((uint8_t)ch->auto_mode != v) { ch->auto_mode = (watering_mode_t)v; changed = true; } }
            else if (e.field_id == RES_CH_SUN_PCT) { if (ch->sun_exposure_pct != v) { ch->sun_exposure_pct = v; changed = true; } }
            (void)ucp_tlvw_put_u8(&delta, e.field_id, UCP_TLV_U8, v);
            continue;
        }
        if (e.field_id == RES_CH_USE_AREA || e.field_id == RES_CH_CYCLE_SOAK) {
            uint8_t v;
            if (!ucp_tlv_read_u8(&e, &v)) return send_tlv(UCP_MSG_SET_RESP,UCP_SVC_CHANNEL,UCP_RES_CHANNEL_CONFIG,UCP_STATUS_INVALID_ARG,NULL,req);
            if (e.field_id == RES_CH_USE_AREA) { if (ch->use_area_based != (v != 0U)) { ch->use_area_based = (v != 0U); changed = true; } }
            else { if (ch->enable_cycle_soak != (v != 0U)) { ch->enable_cycle_soak = (v != 0U); changed = true; } }
            (void)ucp_tlvw_put_u8(&delta, e.field_id, UCP_TLV_BOOL, v ? 1U : 0U);
            continue;
        }
        if (e.field_id == RES_CH_PLANT_COUNT) {
            uint16_t v;
            if (!ucp_tlv_read_u16(&e, &v)) return send_tlv(UCP_MSG_SET_RESP,UCP_SVC_CHANNEL,UCP_RES_CHANNEL_CONFIG,UCP_STATUS_INVALID_ARG,NULL,req);
            if (ch->coverage.plant_count != v) { ch->coverage.plant_count = v; changed = true; }
            (void)ucp_tlvw_put_u16(&delta, RES_CH_PLANT_COUNT, UCP_TLV_U16, v);
            continue;
        }
        if (e.field_id == RES_CH_PLANTING_TS) {
            uint32_t v;
            if (!ucp_tlv_read_u32(&e, &v)) return send_tlv(UCP_MSG_SET_RESP,UCP_SVC_CHANNEL,UCP_RES_CHANNEL_CONFIG,UCP_STATUS_INVALID_ARG,NULL,req);
            if (ch->planting_date_unix != v) { ch->planting_date_unix = v; changed = true; }
            (void)ucp_tlvw_put_u32(&delta, RES_CH_PLANTING_TS, UCP_TLV_U32, v);
            continue;
        }
        if (e.field_id == RES_CH_AREA_M2 || e.field_id == RES_CH_MAX_VOL_L || e.field_id == RES_CH_LAT_DEG || e.field_id == RES_CH_LON_DEG) {
            float v;
            if (!ucp_tlv_read_f32(&e, &v)) return send_tlv(UCP_MSG_SET_RESP,UCP_SVC_CHANNEL,UCP_RES_CHANNEL_CONFIG,UCP_STATUS_INVALID_ARG,NULL,req);
            if (e.field_id == RES_CH_AREA_M2) { if (v < 0.0f || v > 100000.0f) return send_tlv(UCP_MSG_SET_RESP,UCP_SVC_CHANNEL,UCP_RES_CHANNEL_CONFIG,UCP_STATUS_OUT_OF_RANGE,NULL,req); if (ch->coverage.area_m2 != v) { ch->coverage.area_m2 = v; changed = true; } }
            else if (e.field_id == RES_CH_MAX_VOL_L) { if (v < 0.0f || v > 10000.0f) return send_tlv(UCP_MSG_SET_RESP,UCP_SVC_CHANNEL,UCP_RES_CHANNEL_CONFIG,UCP_STATUS_OUT_OF_RANGE,NULL,req); if (ch->max_volume_limit_l != v) { ch->max_volume_limit_l = v; changed = true; } }
            else if (e.field_id == RES_CH_LAT_DEG) { if (v < -90.0f || v > 90.0f) return send_tlv(UCP_MSG_SET_RESP,UCP_SVC_CHANNEL,UCP_RES_CHANNEL_CONFIG,UCP_STATUS_OUT_OF_RANGE,NULL,req); if (ch->latitude_deg != v) { ch->latitude_deg = v; changed = true; } }
            else { if (v < -180.0f || v > 180.0f) return send_tlv(UCP_MSG_SET_RESP,UCP_SVC_CHANNEL,UCP_RES_CHANNEL_CONFIG,UCP_STATUS_OUT_OF_RANGE,NULL,req); if (ch->longitude_deg != v) { ch->longitude_deg = v; changed = true; } }
            (void)ucp_tlvw_put_f32(&delta, e.field_id, v);
            continue;
        }
    }

    if (!any) return send_tlv(UCP_MSG_SET_RESP, UCP_SVC_CHANNEL, UCP_RES_CHANNEL_CONFIG, UCP_STATUS_INVALID_ARG, NULL, req);
    if (changed) {
        (void)watering_update_days_after_planting(channel_id);
        if (save_channel_after_change(channel_id) != 0) {
            return send_tlv(UCP_MSG_SET_RESP, UCP_SVC_CHANNEL, UCP_RES_CHANNEL_CONFIG, UCP_STATUS_INTERNAL, NULL, req);
        }
        g_ver_channel_cfg[channel_id]++;
        (void)send_set_ok_resp(UCP_SVC_CHANNEL, UCP_RES_CHANNEL_CONFIG, channel_id, g_ver_channel_cfg[channel_id], req);
        (void)emit_change_instance(UCP_TOPIC_CHANNEL_CONFIG, UCP_SVC_CHANNEL, UCP_RES_CHANNEL_CONFIG, channel_id,
                                   g_ver_channel_cfg[channel_id], UCP_ORIGIN_UART, delta.buf, delta.len);
        return 0;
    }
    return send_set_ok_resp(UCP_SVC_CHANNEL, UCP_RES_CHANNEL_CONFIG, channel_id, g_ver_channel_cfg[channel_id], req);
}

static int handle_set_schedule(const uint8_t *p, uint16_t len, uint32_t req)
{
    watering_channel_t *ch = NULL;
    watering_event_t before, after;
    struct ucp_tlv_elem e;
    size_t off = 0U;
    uint8_t channel_id = 0xFFU;
    bool any = false;
    uint8_t delta_b[UCP_MAX_EVT_DELTA];
    struct ucp_tlv_writer delta;

    if (!parse_channel_id_required(p, len, &channel_id)) {
        return send_tlv(UCP_MSG_SET_RESP, UCP_SVC_SCHEDULE, UCP_RES_SCHEDULE_CONFIG, UCP_STATUS_INVALID_ARG, NULL, req);
    }
    if (watering_get_channel(channel_id, &ch) != WATERING_SUCCESS || !ch) {
        return send_tlv(UCP_MSG_SET_RESP, UCP_SVC_SCHEDULE, UCP_RES_SCHEDULE_CONFIG, UCP_STATUS_NOT_FOUND, NULL, req);
    }

    before = ch->watering_event;
    after = before;
    ucp_tlvw_init(&delta, delta_b, sizeof(delta_b));
    while (ucp_tlv_next(p, len, &off, &e)) {
        if (e.field_id == RES_CH_ID) continue;
        any = true;
        if (e.field_id == RES_SCH_SOLAR_OFF_MIN) {
            int32_t v;
            if (!ucp_tlv_read_i32(&e, &v)) return send_tlv(UCP_MSG_SET_RESP,UCP_SVC_SCHEDULE,UCP_RES_SCHEDULE_CONFIG,UCP_STATUS_INVALID_ARG,NULL,req);
            if (v < -127 || v > 127) return send_tlv(UCP_MSG_SET_RESP,UCP_SVC_SCHEDULE,UCP_RES_SCHEDULE_CONFIG,UCP_STATUS_OUT_OF_RANGE,NULL,req);
            after.solar_offset_minutes = (int8_t)v;
            (void)ucp_tlvw_put_i32(&delta, RES_SCH_SOLAR_OFF_MIN, UCP_TLV_I32, v);
            continue;
        }
        if (e.field_id == RES_SCH_VOLUME_L) {
            uint16_t v;
            if (!ucp_tlv_read_u16(&e, &v)) return send_tlv(UCP_MSG_SET_RESP,UCP_SVC_SCHEDULE,UCP_RES_SCHEDULE_CONFIG,UCP_STATUS_INVALID_ARG,NULL,req);
            after.watering.by_volume.volume_liters = v;
            (void)ucp_tlvw_put_u16(&delta, RES_SCH_VOLUME_L, UCP_TLV_U16, v);
            continue;
        }
        {
            uint8_t v;
            if (!ucp_tlv_read_u8(&e, &v)) return send_tlv(UCP_MSG_SET_RESP,UCP_SVC_SCHEDULE,UCP_RES_SCHEDULE_CONFIG,UCP_STATUS_INVALID_ARG,NULL,req);
            switch (e.field_id) {
            case RES_SCH_TYPE: after.schedule_type = (schedule_type_t)v; break;
            case RES_SCH_WATER_MODE: after.watering_mode = (watering_mode_t)v; break;
            case RES_SCH_DAYS_MASK: after.schedule.daily.days_of_week = v; break;
            case RES_SCH_INTERVAL_DAYS: after.schedule.periodic.interval_days = v; break;
            case RES_SCH_DURATION_MIN: after.watering.by_duration.duration_minutes = v; break;
            case RES_SCH_START_HOUR: after.start_time.hour = v; break;
            case RES_SCH_START_MIN: after.start_time.minute = v; break;
            case RES_SCH_AUTO_ENABLED: after.auto_enabled = (v != 0U); break;
            case RES_SCH_USE_SOLAR: after.use_solar_timing = (v != 0U); break;
            case RES_SCH_SOLAR_EVENT: after.solar_event = v; break;
            default: any = any; continue;
            }
            (void)ucp_tlvw_put_u8(&delta, e.field_id,
                                  (e.field_id == RES_SCH_AUTO_ENABLED || e.field_id == RES_SCH_USE_SOLAR) ? UCP_TLV_BOOL : UCP_TLV_U8,
                                  (e.field_id == RES_SCH_AUTO_ENABLED || e.field_id == RES_SCH_USE_SOLAR) ? (v ? 1U : 0U) : v);
        }
    }
    if (!any) return send_tlv(UCP_MSG_SET_RESP, UCP_SVC_SCHEDULE, UCP_RES_SCHEDULE_CONFIG, UCP_STATUS_INVALID_ARG, NULL, req);
    if (watering_validate_event_config(&after) != WATERING_SUCCESS) {
        return send_tlv(UCP_MSG_SET_RESP, UCP_SVC_SCHEDULE, UCP_RES_SCHEDULE_CONFIG, UCP_STATUS_OUT_OF_RANGE, NULL, req);
    }
    if (after.schedule_type == SCHEDULE_AUTO && after.auto_enabled && !watering_channel_auto_mode_valid(ch)) {
        return send_tlv(UCP_MSG_SET_RESP, UCP_SVC_SCHEDULE, UCP_RES_SCHEDULE_CONFIG, UCP_STATUS_PRECONDITION_FAILED, NULL, req);
    }
    if (memcmp(&before, &after, sizeof(after)) != 0) {
        ch->watering_event = after;
        if (save_channel_after_change(channel_id) != 0) {
            ch->watering_event = before;
            return send_tlv(UCP_MSG_SET_RESP, UCP_SVC_SCHEDULE, UCP_RES_SCHEDULE_CONFIG, UCP_STATUS_INTERNAL, NULL, req);
        }
        g_ver_schedule_cfg[channel_id]++;
        (void)send_set_ok_resp(UCP_SVC_SCHEDULE, UCP_RES_SCHEDULE_CONFIG, channel_id, g_ver_schedule_cfg[channel_id], req);
        (void)emit_change_instance(UCP_TOPIC_SCHEDULE_CONFIG, UCP_SVC_SCHEDULE, UCP_RES_SCHEDULE_CONFIG, channel_id,
                                   g_ver_schedule_cfg[channel_id], UCP_ORIGIN_UART, delta.buf, delta.len);
        return 0;
    }
    return send_set_ok_resp(UCP_SVC_SCHEDULE, UCP_RES_SCHEDULE_CONFIG, channel_id, g_ver_schedule_cfg[channel_id], req);
}

static int handle_set_flow(const uint8_t *p, uint16_t len, uint32_t req)
{
    struct ucp_tlv_elem e;
    size_t off = 0U;
    bool any = false;
    bool changed = false;
    uint8_t delta_b[UCP_MAX_EVT_DELTA];
    struct ucp_tlv_writer delta;
    uint32_t cal_before = 0U, cal_after = 0U;

    (void)watering_get_flow_calibration(&cal_before);
    cal_after = cal_before;
    ucp_tlvw_init(&delta, delta_b, sizeof(delta_b));
    while (ucp_tlv_next(p, len, &off, &e)) {
        if (e.field_id != FLOW_CAL_PPL) continue;
        any = true;
        if (!ucp_tlv_read_u32(&e, &cal_after)) return send_tlv(UCP_MSG_SET_RESP,UCP_SVC_FLOW,UCP_RES_FLOW_STATUS,UCP_STATUS_INVALID_ARG,NULL,req);
        (void)ucp_tlvw_put_u32(&delta, FLOW_CAL_PPL, UCP_TLV_U32, cal_after);
    }
    if (!any) return send_tlv(UCP_MSG_SET_RESP, UCP_SVC_FLOW, UCP_RES_FLOW_STATUS, UCP_STATUS_INVALID_ARG, NULL, req);
    if (cal_after != cal_before) {
        watering_error_t rc = watering_set_flow_calibration(cal_after);
        if (rc != WATERING_SUCCESS) return send_tlv(UCP_MSG_SET_RESP, UCP_SVC_FLOW, UCP_RES_FLOW_STATUS, ucp_status_from_werr(rc), NULL, req);
        g_ver_flow++;
        changed = true;
    }
    (void)send_set_ok_resp(UCP_SVC_FLOW, UCP_RES_FLOW_STATUS, 0U, g_ver_flow, req);
    if (changed) (void)emit_change(UCP_TOPIC_FLOW_LIVE, UCP_SVC_FLOW, UCP_RES_FLOW_STATUS, g_ver_flow, UCP_ORIGIN_UART, delta.buf, delta.len);
    return 0;
}

static uint16_t ucp_status_from_neg_errno(int rc)
{
    if (rc == -EINVAL) return UCP_STATUS_INVALID_ARG;
    if (rc == -EBUSY) return UCP_STATUS_BUSY;
    if (rc == -ETIMEDOUT) return UCP_STATUS_TIMEOUT;
    if (rc == -ENODEV) return UCP_STATUS_HW_FAULT;
    return UCP_STATUS_INTERNAL;
}

static int handle_set_rain(const uint8_t *p, uint16_t len, uint32_t req)
{
    struct ucp_tlv_elem e;
    size_t off = 0U;
    bool any = false;
    bool changed = false;
    uint8_t delta_b[UCP_MAX_EVT_DELTA];
    struct ucp_tlv_writer delta;
    int rc = 0;

    ucp_tlvw_init(&delta, delta_b, sizeof(delta_b));
    while (ucp_tlv_next(p, len, &off, &e)) {
        any = true;
        if (e.field_id == RAIN_CALIB_MM_PER_PULSE) {
            float v;
            if (!ucp_tlv_read_f32(&e, &v)) return send_tlv(UCP_MSG_SET_RESP,UCP_SVC_RAIN,UCP_RES_RAIN_STATUS,UCP_STATUS_INVALID_ARG,NULL,req);
            rc = rain_sensor_set_calibration(v);
            if (rc != 0) return send_tlv(UCP_MSG_SET_RESP,UCP_SVC_RAIN,UCP_RES_RAIN_STATUS,ucp_status_from_neg_errno(rc),NULL,req);
            (void)ucp_tlvw_put_f32(&delta, RAIN_CALIB_MM_PER_PULSE, v);
            changed = true;
            continue;
        }
        if (e.field_id == RAIN_DEBOUNCE_MS) {
            uint16_t v;
            if (!ucp_tlv_read_u16(&e, &v)) return send_tlv(UCP_MSG_SET_RESP,UCP_SVC_RAIN,UCP_RES_RAIN_STATUS,UCP_STATUS_INVALID_ARG,NULL,req);
            rc = rain_sensor_set_debounce(v);
            if (rc != 0) return send_tlv(UCP_MSG_SET_RESP,UCP_SVC_RAIN,UCP_RES_RAIN_STATUS,ucp_status_from_neg_errno(rc),NULL,req);
            (void)ucp_tlvw_put_u16(&delta, RAIN_DEBOUNCE_MS, UCP_TLV_U16, v);
            changed = true;
            continue;
        }
        if (e.field_id == RAIN_ENABLED || e.field_id == RAIN_INTEGRATION_ENABLED) {
            uint8_t v;
            if (!ucp_tlv_read_u8(&e, &v)) return send_tlv(UCP_MSG_SET_RESP,UCP_SVC_RAIN,UCP_RES_RAIN_STATUS,UCP_STATUS_INVALID_ARG,NULL,req);
            rc = (e.field_id == RAIN_ENABLED) ? rain_sensor_set_enabled(v != 0U) : rain_sensor_set_integration_enabled(v != 0U);
            if (rc != 0) return send_tlv(UCP_MSG_SET_RESP,UCP_SVC_RAIN,UCP_RES_RAIN_STATUS,ucp_status_from_neg_errno(rc),NULL,req);
            (void)ucp_tlvw_put_u8(&delta, e.field_id, UCP_TLV_BOOL, v ? 1U : 0U);
            changed = true;
            continue;
        }
    }
    if (!any) return send_tlv(UCP_MSG_SET_RESP, UCP_SVC_RAIN, UCP_RES_RAIN_STATUS, UCP_STATUS_INVALID_ARG, NULL, req);
    if (changed) {
        (void)rain_sensor_save_config();
        g_ver_rain++;
    }
    (void)send_set_ok_resp(UCP_SVC_RAIN, UCP_RES_RAIN_STATUS, 0U, g_ver_rain, req);
    if (changed) (void)emit_change(UCP_TOPIC_RAIN_LIVE, UCP_SVC_RAIN, UCP_RES_RAIN_STATUS, g_ver_rain, UCP_ORIGIN_UART, delta.buf, delta.len);
    return 0;
}

static int handle_set_tz(const uint8_t *p, uint16_t len, uint32_t req)
{
    timezone_config_t before, after; struct ucp_tlv_elem e; size_t off = 0U; bool any = false;
    uint8_t delta_b[UCP_MAX_EVT_DELTA]; struct ucp_tlv_writer delta;
    if (timezone_get_config(&before) < 0) return send_tlv(UCP_MSG_SET_RESP, UCP_SVC_RTC_TZ, UCP_RES_TZ_CONFIG, UCP_STATUS_INTERNAL, NULL, req);
    after = before; ucp_tlvw_init(&delta, delta_b, sizeof(delta_b));
    while (ucp_tlv_next(p, len, &off, &e)) {
        any = true;
        if (e.field_id == TZ_UTC_OFF) { int32_t v; if (!ucp_tlv_read_i32(&e, &v)) return send_tlv(UCP_MSG_SET_RESP,UCP_SVC_RTC_TZ,UCP_RES_TZ_CONFIG,UCP_STATUS_INVALID_ARG,NULL,req); after.utc_offset_minutes=(int16_t)v; (void)ucp_tlvw_put_i32(&delta,TZ_UTC_OFF,UCP_TLV_I32,v); continue; }
        if (e.field_id == TZ_DST_OFF) { int32_t v; if (!ucp_tlv_read_i32(&e, &v)) return send_tlv(UCP_MSG_SET_RESP,UCP_SVC_RTC_TZ,UCP_RES_TZ_CONFIG,UCP_STATUS_INVALID_ARG,NULL,req); after.dst_offset_minutes=(int16_t)v; (void)ucp_tlvw_put_i32(&delta,TZ_DST_OFF,UCP_TLV_I32,v); continue; }
        { uint8_t v; if (!ucp_tlv_read_u8(&e, &v)) return send_tlv(UCP_MSG_SET_RESP,UCP_SVC_RTC_TZ,UCP_RES_TZ_CONFIG,UCP_STATUS_INVALID_ARG,NULL,req);
          if (e.field_id == TZ_DST_EN) { after.dst_enabled = v ? 1U : 0U; (void)ucp_tlvw_put_u8(&delta, TZ_DST_EN, UCP_TLV_BOOL, after.dst_enabled); continue; }
          if (e.field_id == TZ_DST_SM) after.dst_start_month = v;
          else if (e.field_id == TZ_DST_SW) after.dst_start_week = v;
          else if (e.field_id == TZ_DST_SD) after.dst_start_dow = v;
          else if (e.field_id == TZ_DST_EM) after.dst_end_month = v;
          else if (e.field_id == TZ_DST_EW) after.dst_end_week = v;
          else if (e.field_id == TZ_DST_ED) after.dst_end_dow = v;
          else continue;
          (void)ucp_tlvw_put_u8(&delta, e.field_id, UCP_TLV_U8, v);
        }
    }
    if (!any) return send_tlv(UCP_MSG_SET_RESP, UCP_SVC_RTC_TZ, UCP_RES_TZ_CONFIG, UCP_STATUS_INVALID_ARG, NULL, req);
    if (!tz_valid(&after)) return send_tlv(UCP_MSG_SET_RESP, UCP_SVC_RTC_TZ, UCP_RES_TZ_CONFIG, UCP_STATUS_OUT_OF_RANGE, NULL, req);
    if (memcmp(&before, &after, sizeof(after)) != 0) {
        if (timezone_set_config(&after) < 0) return send_tlv(UCP_MSG_SET_RESP,UCP_SVC_RTC_TZ,UCP_RES_TZ_CONFIG,UCP_STATUS_INTERNAL,NULL,req);
        ucp_utc_cache_invalidate();
        g_ver_tz++;
    }
    { uint8_t rb[96]; struct ucp_tlv_writer rw; ucp_tlvw_init(&rw, rb, sizeof(rb));
      (void)ucp_tlvw_put_u8(&rw, UCP_F_OK_BOOL, UCP_TLV_BOOL, 1U);
      (void)ucp_tlvw_put_u32(&rw, UCP_F_RESOURCE_VERSION, UCP_TLV_U32, g_ver_tz);
      (void)ucp_tlvw_put_u64(&rw, UCP_F_TIMESTAMP_UTC_MS, UCP_TLV_U64, now_utc_ms());
      (void)send_tlv(UCP_MSG_SET_RESP, UCP_SVC_RTC_TZ, UCP_RES_TZ_CONFIG, UCP_STATUS_OK, &rw, req); }
    if (memcmp(&before, &after, sizeof(after)) != 0) (void)emit_change(UCP_TOPIC_RTC_TZ_CHANGED, UCP_SVC_RTC_TZ, UCP_RES_TZ_CONFIG, g_ver_tz, UCP_ORIGIN_UART, delta.buf, delta.len);
    return 0;
}

static int handle_set_rtc(const uint8_t *p, uint16_t len, uint32_t req)
{
    rtc_datetime_t before, after;
    struct ucp_tlv_elem e;
    size_t off = 0U;
    bool any = false;
    uint8_t delta_b[UCP_MAX_EVT_DELTA];
    struct ucp_tlv_writer delta;
    int rc;

    rc = rtc_datetime_get(&before);
    if (rc < 0) return send_tlv(UCP_MSG_SET_RESP, UCP_SVC_RTC_TZ, UCP_RES_RTC_TIME, UCP_STATUS_HW_FAULT, NULL, req);
    after = before;
    ucp_tlvw_init(&delta, delta_b, sizeof(delta_b));

    while (ucp_tlv_next(p, len, &off, &e)) {
        any = true;
        if (e.field_id == RTC_YEAR) {
            uint16_t y;
            if (!ucp_tlv_read_u16(&e, &y)) return send_tlv(UCP_MSG_SET_RESP,UCP_SVC_RTC_TZ,UCP_RES_RTC_TIME,UCP_STATUS_INVALID_ARG,NULL,req);
            after.year = y;
            (void)ucp_tlvw_put_u16(&delta, RTC_YEAR, UCP_TLV_U16, y);
            continue;
        }
        if (e.field_id == RTC_UNIX_UTC) {
            uint32_t ts;
            if (!ucp_tlv_read_u32(&e, &ts)) return send_tlv(UCP_MSG_SET_RESP,UCP_SVC_RTC_TZ,UCP_RES_RTC_TIME,UCP_STATUS_INVALID_ARG,NULL,req);
            if (timezone_unix_to_rtc_utc(ts, &after) != 0) return send_tlv(UCP_MSG_SET_RESP,UCP_SVC_RTC_TZ,UCP_RES_RTC_TIME,UCP_STATUS_INVALID_ARG,NULL,req);
            (void)ucp_tlvw_put_u32(&delta, RTC_UNIX_UTC, UCP_TLV_U32, ts);
            continue;
        }
        {
            uint8_t v;
            if (!ucp_tlv_read_u8(&e, &v)) return send_tlv(UCP_MSG_SET_RESP,UCP_SVC_RTC_TZ,UCP_RES_RTC_TIME,UCP_STATUS_INVALID_ARG,NULL,req);
            if (e.field_id == RTC_SEC) after.second = v;
            else if (e.field_id == RTC_MIN) after.minute = v;
            else if (e.field_id == RTC_HOUR) after.hour = v;
            else if (e.field_id == RTC_DAY) after.day = v;
            else if (e.field_id == RTC_MONTH) after.month = v;
            else if (e.field_id == RTC_DOW) after.day_of_week = v;
            else continue;
            (void)ucp_tlvw_put_u8(&delta, e.field_id, UCP_TLV_U8, v);
        }
    }

    if (!any) return send_tlv(UCP_MSG_SET_RESP, UCP_SVC_RTC_TZ, UCP_RES_RTC_TIME, UCP_STATUS_INVALID_ARG, NULL, req);
    if (memcmp(&before, &after, sizeof(after)) != 0) {
        rc = rtc_datetime_set(&after);
        if (rc == -EINVAL) return send_tlv(UCP_MSG_SET_RESP, UCP_SVC_RTC_TZ, UCP_RES_RTC_TIME, UCP_STATUS_OUT_OF_RANGE, NULL, req);
        if (rc < 0) return send_tlv(UCP_MSG_SET_RESP, UCP_SVC_RTC_TZ, UCP_RES_RTC_TIME, UCP_STATUS_HW_FAULT, NULL, req);
        ucp_utc_cache_invalidate();
        g_ver_rtc++;
    }

    {
        uint8_t rb[96];
        struct ucp_tlv_writer rw;
        ucp_tlvw_init(&rw, rb, sizeof(rb));
        (void)ucp_tlvw_put_u8(&rw, UCP_F_OK_BOOL, UCP_TLV_BOOL, 1U);
        (void)ucp_tlvw_put_u32(&rw, UCP_F_RESOURCE_VERSION, UCP_TLV_U32, g_ver_rtc);
        (void)ucp_tlvw_put_u64(&rw, UCP_F_TIMESTAMP_UTC_MS, UCP_TLV_U64, now_utc_ms());
        (void)send_tlv(UCP_MSG_SET_RESP, UCP_SVC_RTC_TZ, UCP_RES_RTC_TIME, UCP_STATUS_OK, &rw, req);
    }

    if (memcmp(&before, &after, sizeof(after)) != 0) {
        (void)emit_change(UCP_TOPIC_RTC_TZ_CHANGED, UCP_SVC_RTC_TZ, UCP_RES_RTC_TIME, g_ver_rtc, UCP_ORIGIN_UART, delta.buf, delta.len);
    }
    return 0;
}

static int handle_set_peer_power_policy(const uint8_t *p, uint16_t len, uint32_t req)
{
    struct ucp_tlv_elem e;
    size_t off = 0U;
    uint8_t policy = g_peer_power_policy;
    uint16_t grace_ms = g_peer_power_grace_ms;
    bool any = false;
    bool changed = false;
    uint8_t delta_b[32];
    struct ucp_tlv_writer delta;

    ucp_tlvw_init(&delta, delta_b, sizeof(delta_b));
    while (ucp_tlv_next(p, len, &off, &e)) {
        any = true;
        if (e.field_id == PEER_POLICY_MODE) {
            uint8_t v;
            if (!ucp_tlv_read_u8(&e, &v)) return send_tlv(UCP_MSG_SET_RESP, UCP_SVC_PEER_POWER, UCP_RES_PEER_POWER_POLICY, UCP_STATUS_INVALID_ARG, NULL, req);
            if (v != PEER_POLICY_MANUAL && v != PEER_POLICY_AUTO) return send_tlv(UCP_MSG_SET_RESP, UCP_SVC_PEER_POWER, UCP_RES_PEER_POWER_POLICY, UCP_STATUS_OUT_OF_RANGE, NULL, req);
            policy = v;
            (void)ucp_tlvw_put_u8(&delta, PEER_POLICY_MODE, UCP_TLV_U8, v);
            continue;
        }
        if (e.field_id == PEER_STATE_GRACE_MS) {
            uint16_t v;
            if (!ucp_tlv_read_u16(&e, &v)) return send_tlv(UCP_MSG_SET_RESP, UCP_SVC_PEER_POWER, UCP_RES_PEER_POWER_POLICY, UCP_STATUS_INVALID_ARG, NULL, req);
            if (v > 10000U) return send_tlv(UCP_MSG_SET_RESP, UCP_SVC_PEER_POWER, UCP_RES_PEER_POWER_POLICY, UCP_STATUS_OUT_OF_RANGE, NULL, req);
            grace_ms = v;
            (void)ucp_tlvw_put_u16(&delta, PEER_STATE_GRACE_MS, UCP_TLV_U16, v);
            continue;
        }
    }
    if (!any) return send_tlv(UCP_MSG_SET_RESP, UCP_SVC_PEER_POWER, UCP_RES_PEER_POWER_POLICY, UCP_STATUS_INVALID_ARG, NULL, req);
    changed = (policy != g_peer_power_policy) || (grace_ms != g_peer_power_grace_ms);
    g_peer_power_policy = policy;
    g_peer_power_grace_ms = grace_ms;
    if (changed) {
        (void)emit_peer_power_change(PEER_REASON_UART_CMD, UCP_ORIGIN_UART);
    }
    {
        uint8_t rb[96];
        struct ucp_tlv_writer rw;
        ucp_tlvw_init(&rw, rb, sizeof(rb));
        (void)ucp_tlvw_put_u8(&rw, UCP_F_OK_BOOL, UCP_TLV_BOOL, 1U);
        (void)ucp_tlvw_put_u32(&rw, UCP_F_RESOURCE_VERSION, UCP_TLV_U32, g_ver_peer);
        (void)ucp_tlvw_put_u64(&rw, UCP_F_TIMESTAMP_UTC_MS, UCP_TLV_U64, now_utc_ms());
        return send_tlv(UCP_MSG_SET_RESP, UCP_SVC_PEER_POWER, UCP_RES_PEER_POWER_POLICY, UCP_STATUS_OK, &rw, req);
    }
}

static int handle_set(uint16_t svc, uint16_t res, const uint8_t *p, uint16_t len, uint32_t req)
{
    if (svc == UCP_SVC_DEVICE && res == UCP_RES_DEVICE_SYSTEM_CONFIG) {
        const uint8_t *raw = NULL;
        uint16_t raw_len = 0U;
        struct enhanced_system_config_data cfg;
        int rc;
        if (!parse_raw_bytes_req(p, len, &raw, &raw_len, NULL, NULL) || raw_len != sizeof(cfg)) {
            return send_tlv(UCP_MSG_SET_RESP, svc, res, UCP_STATUS_INVALID_ARG, NULL, req);
        }
        memcpy(&cfg, raw, sizeof(cfg));
        rc = apply_system_config_bytes(&cfg);
        if (rc == -ERANGE) return send_tlv(UCP_MSG_SET_RESP, svc, res, UCP_STATUS_OUT_OF_RANGE, NULL, req);
        if (rc != 0) return send_tlv(UCP_MSG_SET_RESP, svc, res, UCP_STATUS_INTERNAL, NULL, req);
        g_ver_syscfg++;
        (void)send_set_ok_resp(svc, res, 0U, g_ver_syscfg, req);
        (void)emit_change(UCP_TOPIC_SYSTEM_CONFIG, svc, res, g_ver_syscfg, UCP_ORIGIN_UART, p, len);
        return 0;
    }
    if (svc == UCP_SVC_DEVICE && res == UCP_RES_DEVICE_AUTO_CALC_STATUS) {
        const uint8_t *raw = NULL;
        uint16_t raw_len = 0U;
        struct auto_calc_status_data d;
        watering_error_t rc;
        uint32_t inst;
        if (!parse_raw_bytes_req(p, len, &raw, &raw_len, NULL, NULL) || raw_len != sizeof(d)) {
            return send_tlv(UCP_MSG_SET_RESP, svc, res, UCP_STATUS_INVALID_ARG, NULL, req);
        }
        memcpy(&d, raw, sizeof(d));
        rc = watering_set_auto_calc_enabled(d.calculation_active != 0U);
        if (rc != WATERING_SUCCESS) return send_tlv(UCP_MSG_SET_RESP, svc, res, ucp_status_from_werr(rc), NULL, req);
        g_ver_auto_calc++;
        inst = (d.channel_id == 0xFFU) ? 0xFFFFFFFFU : d.channel_id;
        (void)send_set_ok_resp(svc, res, inst, g_ver_auto_calc, req);
        (void)emit_change_instance(UCP_TOPIC_AUTO_CALC_STATUS, svc, res, inst, g_ver_auto_calc, UCP_ORIGIN_UART, p, len);
        return 0;
    }
    if (svc == UCP_SVC_DEVICE && res == UCP_RES_DEVICE_AUTO_CALC_CONTROL) {
        const uint8_t *raw = NULL;
        uint16_t raw_len = 0U;
        struct ucp_auto_calc_control_data_v1 d;
        watering_error_t rc;
        if (!parse_raw_bytes_req(p, len, &raw, &raw_len, NULL, NULL) || raw_len != sizeof(d)) {
            return send_tlv(UCP_MSG_SET_RESP, svc, res, UCP_STATUS_INVALID_ARG, NULL, req);
        }
        memcpy(&d, raw, sizeof(d));
        if (d.enabled > 1U) return send_tlv(UCP_MSG_SET_RESP, svc, res, UCP_STATUS_OUT_OF_RANGE, NULL, req);
        if (d.interval_hours < 1U || d.interval_hours > 24U) {
            return send_tlv(UCP_MSG_SET_RESP, svc, res, UCP_STATUS_OUT_OF_RANGE, NULL, req);
        }
        rc = watering_set_auto_calc_enabled(d.enabled != 0U);
        if (rc != WATERING_SUCCESS) return send_tlv(UCP_MSG_SET_RESP, svc, res, ucp_status_from_werr(rc), NULL, req);
        rc = watering_set_auto_calc_interval(d.interval_hours);
        if (rc != WATERING_SUCCESS) return send_tlv(UCP_MSG_SET_RESP, svc, res, ucp_status_from_werr(rc), NULL, req);
        g_ver_auto_calc_ctl++;
        (void)send_set_ok_resp(svc, res, 0U, g_ver_auto_calc_ctl, req);
        (void)emit_change(UCP_TOPIC_AUTO_CALC_STATUS, svc, res, g_ver_auto_calc_ctl, UCP_ORIGIN_UART, p, len);
        return 0;
    }
    if (svc == UCP_SVC_CHANNEL && res == UCP_RES_CHANNEL_CONFIG) return handle_set_channel_config(p, len, req);
    if (svc == UCP_SVC_CHANNEL && res == UCP_RES_CHANNEL_CUSTOM_SOIL) {
        const uint8_t *raw = NULL;
        uint16_t raw_len = 0U;
        uint8_t sel = 0xFFU;
        bool has_sel = false;
        struct custom_soil_config_data d;
        int rc;
        if (!parse_raw_bytes_req(p, len, &raw, &raw_len, &sel, &has_sel) || raw_len != sizeof(d)) {
            return send_tlv(UCP_MSG_SET_RESP, svc, res, UCP_STATUS_INVALID_ARG, NULL, req);
        }
        memcpy(&d, raw, sizeof(d));
        if (has_sel) d.channel_id = sel;
        rc = apply_custom_soil_bytes(&d);
        if (rc != 0) return send_tlv(UCP_MSG_SET_RESP, svc, res, UCP_STATUS_INVALID_ARG, NULL, req);
        if (d.channel_id >= WATERING_CHANNELS_COUNT) return send_tlv(UCP_MSG_SET_RESP, svc, res, UCP_STATUS_INVALID_ARG, NULL, req);
        g_ver_custom_soil[d.channel_id]++;
        (void)send_set_ok_resp(svc, res, d.channel_id, g_ver_custom_soil[d.channel_id], req);
        (void)emit_change_instance(UCP_TOPIC_CUSTOM_SOIL, svc, res, d.channel_id, g_ver_custom_soil[d.channel_id], UCP_ORIGIN_UART, p, len);
        return 0;
    }
    if (svc == UCP_SVC_CHANNEL && res == UCP_RES_CHANNEL_SOIL_MOISTURE) {
        const uint8_t *raw = NULL;
        uint16_t raw_len = 0U;
        uint8_t sel = 0xFFU;
        bool has_sel = false;
        struct soil_moisture_config_data d;
        int rc;
        uint32_t ver;
        uint32_t inst;
        if (!parse_raw_bytes_req(p, len, &raw, &raw_len, &sel, &has_sel) || raw_len != sizeof(d)) {
            return send_tlv(UCP_MSG_SET_RESP, svc, res, UCP_STATUS_INVALID_ARG, NULL, req);
        }
        memcpy(&d, raw, sizeof(d));
        if (has_sel) d.channel_id = sel;
        rc = apply_soil_moisture_bytes(&d);
        if (rc == -ERANGE) return send_tlv(UCP_MSG_SET_RESP, svc, res, UCP_STATUS_OUT_OF_RANGE, NULL, req);
        if (rc != 0) return send_tlv(UCP_MSG_SET_RESP, svc, res, UCP_STATUS_INVALID_ARG, NULL, req);
        if (d.channel_id == 0xFFU) {
            g_ver_soil_moisture[0]++;
            ver = g_ver_soil_moisture[0];
            inst = 0xFFFFFFFFU;
        } else if (d.channel_id < WATERING_CHANNELS_COUNT) {
            g_ver_soil_moisture[d.channel_id + 1U]++;
            ver = g_ver_soil_moisture[d.channel_id + 1U];
            inst = d.channel_id;
        } else {
            return send_tlv(UCP_MSG_SET_RESP, svc, res, UCP_STATUS_INVALID_ARG, NULL, req);
        }
        (void)send_set_ok_resp(svc, res, inst, ver, req);
        (void)emit_change_instance(UCP_TOPIC_SOIL_MOISTURE, svc, res, inst, ver, UCP_ORIGIN_UART, p, len);
        return 0;
    }
    if (svc == UCP_SVC_CHANNEL && res == UCP_RES_CHANNEL_INTERVAL_CONFIG) {
        const uint8_t *raw = NULL;
        uint16_t raw_len = 0U;
        uint8_t sel = 0xFFU;
        bool has_sel = false;
        struct interval_mode_config_data d;
        int rc;
        if (!parse_raw_bytes_req(p, len, &raw, &raw_len, &sel, &has_sel) || raw_len < sizeof(d)) {
            return send_tlv(UCP_MSG_SET_RESP, svc, res, UCP_STATUS_INVALID_ARG, NULL, req);
        }
        memcpy(&d, raw, sizeof(d));
        if (has_sel) d.channel_id = sel;
        rc = apply_interval_config_bytes(&d);
        if (rc != 0) return send_tlv(UCP_MSG_SET_RESP, svc, res, UCP_STATUS_INVALID_ARG, NULL, req);
        if (d.channel_id >= WATERING_CHANNELS_COUNT) return send_tlv(UCP_MSG_SET_RESP, svc, res, UCP_STATUS_INVALID_ARG, NULL, req);
        g_ver_interval_cfg[d.channel_id]++;
        (void)send_set_ok_resp(svc, res, d.channel_id, g_ver_interval_cfg[d.channel_id], req);
        (void)emit_change_instance(UCP_TOPIC_INTERVAL_MODE, svc, res, d.channel_id, g_ver_interval_cfg[d.channel_id], UCP_ORIGIN_UART, p, len);
        return 0;
    }
    if (svc == UCP_SVC_SCHEDULE && res == UCP_RES_SCHEDULE_CONFIG) return handle_set_schedule(p, len, req);
    if (svc == UCP_SVC_FLOW && res == UCP_RES_FLOW_STATUS) return handle_set_flow(p, len, req);
    if (svc == UCP_SVC_FLOW && res == UCP_RES_FLOW_HYDRAULIC_STATUS) {
        const uint8_t *raw = NULL;
        uint16_t raw_len = 0U;
        uint8_t sel = 0xFFU;
        bool has_sel = false;
        struct hydraulic_status_data d;
        int rc;
        if (!parse_raw_bytes_req(p, len, &raw, &raw_len, &sel, &has_sel) || raw_len != sizeof(d)) {
            return send_tlv(UCP_MSG_SET_RESP, svc, res, UCP_STATUS_INVALID_ARG, NULL, req);
        }
        memcpy(&d, raw, sizeof(d));
        if (has_sel) d.channel_id = sel;
        rc = apply_hydraulic_status_bytes(&d);
        if (rc == -EINVAL) return send_tlv(UCP_MSG_SET_RESP, svc, res, UCP_STATUS_INVALID_ARG, NULL, req);
        if (rc == -ENOENT) return send_tlv(UCP_MSG_SET_RESP, svc, res, UCP_STATUS_NOT_FOUND, NULL, req);
        if (rc != 0) return send_tlv(UCP_MSG_SET_RESP, svc, res, UCP_STATUS_INTERNAL, NULL, req);
        g_ver_hydraulic[d.channel_id]++;
        (void)send_set_ok_resp(svc, res, d.channel_id, g_ver_hydraulic[d.channel_id], req);
        (void)emit_change_instance(UCP_TOPIC_HYDRAULIC_STATUS, svc, res, d.channel_id, g_ver_hydraulic[d.channel_id], UCP_ORIGIN_UART, p, len);
        return 0;
    }
    if (svc == UCP_SVC_FLOW && res == UCP_RES_FLOW_CALIBRATION_STATUS) {
        const uint8_t *raw = NULL;
        uint16_t raw_len = 0U;
        struct calibration_data d;
        uint16_t status = UCP_STATUS_OK;
        bool changed = false;

        if (!parse_raw_bytes_req(p, len, &raw, &raw_len, NULL, NULL) || raw_len != sizeof(d)) {
            return send_tlv(UCP_MSG_SET_RESP, svc, res, UCP_STATUS_INVALID_ARG, NULL, req);
        }
        memcpy(&d, raw, sizeof(d));

#ifdef CONFIG_BT
        if (d.action == 0x01U) {
            int rc = bt_irrigation_start_flow_calibration(1U, d.volume_ml);
            status = (rc == 0) ? UCP_STATUS_OK : UCP_STATUS_INTERNAL;
            changed = (rc == 0);
        } else if (d.action == 0x00U || d.action == 0x03U) {
            /* action=3 (calculate) is mapped to STOP with volume provided */
            int rc = bt_irrigation_start_flow_calibration(0U, d.volume_ml);
            status = (rc == 0) ? UCP_STATUS_OK : UCP_STATUS_INTERNAL;
            changed = (rc == 0);
        } else
#endif
        if (d.action == 0x04U) {
            watering_error_t rc;
            if (d.pulses_per_liter < 100U || d.pulses_per_liter > 10000U) {
                status = UCP_STATUS_OUT_OF_RANGE;
            } else {
                rc = watering_set_flow_calibration(d.pulses_per_liter);
                status = (rc == WATERING_SUCCESS) ? UCP_STATUS_OK : ucp_status_from_werr(rc);
                changed = (rc == WATERING_SUCCESS);
            }
        } else if (d.action == 0x05U) {
            watering_error_t rc = watering_set_flow_calibration(DEFAULT_PULSES_PER_LITER);
            status = (rc == WATERING_SUCCESS) ? UCP_STATUS_OK : ucp_status_from_werr(rc);
            changed = (rc == WATERING_SUCCESS);
        } else if (d.action == 0x02U) {
            status = UCP_STATUS_INVALID_ARG; /* read-only in progress state */
        } else {
            status = UCP_STATUS_UNSUPPORTED;
        }

        if (status != UCP_STATUS_OK) {
            return send_tlv(UCP_MSG_SET_RESP, svc, res, status, NULL, req);
        }
        if (changed) {
            g_ver_flow_cal++;
        }
        (void)send_set_ok_resp(svc, res, 0U, g_ver_flow_cal, req);
        if (changed) {
            uint8_t db[UCP_MAX_EVT_DELTA];
            struct ucp_tlv_writer dw;
            ucp_tlvw_init(&dw, db, sizeof(db));
            if (build_flow_calibration_status_bytes(&dw) == 0) {
                (void)emit_change(UCP_TOPIC_FLOW_CALIBRATION, svc, res, g_ver_flow_cal, UCP_ORIGIN_UART, dw.buf, dw.len);
            }
        }
        return 0;
    }
    if (svc == UCP_SVC_RAIN && res == UCP_RES_RAIN_STATUS) return handle_set_rain(p, len, req);
    if (svc == UCP_SVC_ENVIRONMENT && res == UCP_RES_ENV_SENSOR_CONFIG) {
        const uint8_t *raw = NULL;
        uint16_t raw_len = 0U;
        bme280_config_t cfg;
        int rc;
        if (!parse_raw_bytes_req(p, len, &raw, &raw_len, NULL, NULL) || raw_len != sizeof(cfg)) {
            return send_tlv(UCP_MSG_SET_RESP, svc, res, UCP_STATUS_INVALID_ARG, NULL, req);
        }
        memcpy(&cfg, raw, sizeof(cfg));
        rc = apply_env_sensor_config_bytes(&cfg);
        if (rc != 0) return send_tlv(UCP_MSG_SET_RESP, svc, res, UCP_STATUS_INVALID_ARG, NULL, req);
        g_ver_env++;
        return send_set_ok_resp(svc, res, 0U, g_ver_env, req);
    }
    if (svc == UCP_SVC_RTC_TZ && res == UCP_RES_TZ_CONFIG) return handle_set_tz(p, len, req);
    if (svc == UCP_SVC_RTC_TZ && res == UCP_RES_RTC_TIME) return handle_set_rtc(p, len, req);
    if (svc == UCP_SVC_COMPENSATION && res == UCP_RES_COMP_CHANNEL_CONFIG) {
        const uint8_t *raw = NULL;
        uint16_t raw_len = 0U;
        uint8_t sel = 0xFFU;
        bool has_sel = false;
        struct channel_compensation_config_data cfg;
        int rc;
        if (!parse_raw_bytes_req(p, len, &raw, &raw_len, &sel, &has_sel) || raw_len != sizeof(cfg)) {
            return send_tlv(UCP_MSG_SET_RESP, svc, res, UCP_STATUS_INVALID_ARG, NULL, req);
        }
        memcpy(&cfg, raw, sizeof(cfg));
        if (has_sel) cfg.channel_id = sel;
        rc = apply_comp_channel_cfg_bytes(&cfg);
        if (rc == -EINVAL) return send_tlv(UCP_MSG_SET_RESP, svc, res, UCP_STATUS_INVALID_ARG, NULL, req);
        if (rc == -ENOENT) return send_tlv(UCP_MSG_SET_RESP, svc, res, UCP_STATUS_NOT_FOUND, NULL, req);
        if (rc != 0) return send_tlv(UCP_MSG_SET_RESP, svc, res, UCP_STATUS_INTERNAL, NULL, req);
        g_ver_comp_cfg[cfg.channel_id]++;
        g_ver_comp_status[cfg.channel_id]++;
        (void)send_set_ok_resp(svc, res, cfg.channel_id, g_ver_comp_cfg[cfg.channel_id], req);
        (void)emit_change_instance(UCP_TOPIC_COMP_CONFIG, svc, res, cfg.channel_id, g_ver_comp_cfg[cfg.channel_id], UCP_ORIGIN_UART, p, len);
        return 0;
    }
    if (svc == UCP_SVC_PEER_POWER && res == UCP_RES_PEER_POWER_POLICY) return handle_set_peer_power_policy(p, len, req);
    return send_tlv(UCP_MSG_SET_RESP, svc, res, UCP_STATUS_UNSUPPORTED, NULL, req);
}

static int handle_cmd(uint16_t svc, uint16_t cmd, const uint8_t *p, uint16_t len, uint32_t req)
{
    uint8_t b[UCP_MAX_PAYLOAD_V1]; struct ucp_tlv_writer w; ucp_tlvw_init(&w, b, sizeof(b));
    ARG_UNUSED(p);
    ARG_UNUSED(len);
    if (svc == UCP_SVC_CHANNEL) {
        struct ucp_tlv_elem e;
        size_t off = 0U;
        uint8_t channel_id = 0xFFU;
        uint16_t value = 0U;
        bool has_ch = false;
        bool has_val = false;
        while (ucp_tlv_next(p, len, &off, &e)) {
            if ((e.field_id == RES_CH_ID || e.field_id == Q_CHANNEL_ID) && ucp_tlv_read_u8(&e, &channel_id)) {
                has_ch = true;
                continue;
            }
            if ((e.field_id == Q_LIMIT || e.field_id == RES_CH_ACTIVE) && ucp_tlv_read_u16(&e, &value)) {
                has_val = true;
                continue;
            }
        }

        if (cmd == UCP_CMD_CHANNEL_MASTER_OPEN || cmd == UCP_CMD_CHANNEL_MASTER_CLOSE) {
            watering_error_t rc = (cmd == UCP_CMD_CHANNEL_MASTER_OPEN) ? master_valve_manual_open() : master_valve_manual_close();
            (void)ucp_tlvw_put_u8(&w, UCP_F_OK_BOOL, UCP_TLV_BOOL, (rc == WATERING_SUCCESS) ? 1U : 0U);
            if (rc != WATERING_SUCCESS) return send_tlv(UCP_MSG_CMD_RESP, svc, cmd, ucp_status_from_werr(rc), &w, req);
            (void)ucp_uart_publish_valve_state(0xFFU, (cmd == UCP_CMD_CHANNEL_MASTER_OPEN), true, UCP_ORIGIN_UART);
            return send_tlv(UCP_MSG_CMD_RESP, svc, cmd, UCP_STATUS_OK, &w, req);
        }

        if (!has_ch || channel_id >= WATERING_CHANNELS_COUNT) {
            return send_tlv(UCP_MSG_CMD_RESP, svc, cmd, UCP_STATUS_INVALID_ARG, NULL, req);
        }

        if (cmd == UCP_CMD_CHANNEL_VALVE_OPEN || cmd == UCP_CMD_CHANNEL_VALVE_CLOSE) {
#ifdef CONFIG_BT
            int rc = bt_irrigation_direct_command(channel_id, (cmd == UCP_CMD_CHANNEL_VALVE_OPEN) ? 0U : 1U, 0U);
            (void)ucp_tlvw_put_u8(&w, RES_CH_ID, UCP_TLV_U8, channel_id);
            (void)ucp_tlvw_put_u8(&w, UCP_F_OK_BOOL, UCP_TLV_BOOL, (rc == 0) ? 1U : 0U);
            if (rc != 0) return send_tlv(UCP_MSG_CMD_RESP, svc, cmd, UCP_STATUS_INTERNAL, &w, req);
#else
            watering_error_t rc = (cmd == UCP_CMD_CHANNEL_VALVE_OPEN) ? watering_channel_on(channel_id) : watering_channel_off(channel_id);
            (void)ucp_tlvw_put_u8(&w, RES_CH_ID, UCP_TLV_U8, channel_id);
            (void)ucp_tlvw_put_u8(&w, UCP_F_OK_BOOL, UCP_TLV_BOOL, (rc == WATERING_SUCCESS) ? 1U : 0U);
            if (rc != WATERING_SUCCESS) return send_tlv(UCP_MSG_CMD_RESP, svc, cmd, ucp_status_from_werr(rc), &w, req);
            (void)ucp_uart_publish_valve_state(channel_id, (cmd == UCP_CMD_CHANNEL_VALVE_OPEN), false, UCP_ORIGIN_UART);
#endif
            return send_tlv(UCP_MSG_CMD_RESP, svc, cmd, UCP_STATUS_OK, &w, req);
        }
        if (cmd == UCP_CMD_CHANNEL_QUEUE_DURATION || cmd == UCP_CMD_CHANNEL_QUEUE_VOLUME) {
            watering_error_t rc;
            if (!has_val || value == 0U) return send_tlv(UCP_MSG_CMD_RESP, svc, cmd, UCP_STATUS_INVALID_ARG, NULL, req);
            rc = (cmd == UCP_CMD_CHANNEL_QUEUE_DURATION) ? watering_add_duration_task(channel_id, value)
                                                         : watering_add_volume_task(channel_id, value);
            (void)ucp_tlvw_put_u8(&w, RES_CH_ID, UCP_TLV_U8, channel_id);
            (void)ucp_tlvw_put_u16(&w, Q_LIMIT, UCP_TLV_U16, value);
            (void)ucp_tlvw_put_u8(&w, UCP_F_OK_BOOL, UCP_TLV_BOOL, (rc == WATERING_SUCCESS) ? 1U : 0U);
            if (rc != WATERING_SUCCESS) return send_tlv(UCP_MSG_CMD_RESP, svc, cmd, ucp_status_from_werr(rc), &w, req);
            (void)maybe_task_status_event();
            return send_tlv(UCP_MSG_CMD_RESP, svc, cmd, UCP_STATUS_OK, &w, req);
        }
        return send_tlv(UCP_MSG_CMD_RESP, svc, cmd, UCP_STATUS_NOT_FOUND, NULL, req);
    }

    if (svc == UCP_SVC_TASK) {
        bool ok = false;
        if (cmd == UCP_CMD_TASK_STOP) {
            ok = watering_stop_current_task();
        } else if (cmd == UCP_CMD_TASK_PAUSE) {
            ok = watering_pause_current_task();
        } else if (cmd == UCP_CMD_TASK_RESUME) {
            ok = watering_resume_current_task();
        } else if (cmd == UCP_CMD_TASK_CANCEL_ALL) {
            int rc = watering_cancel_all_tasks();
            ok = (rc >= 0);
            (void)ucp_tlvw_put_i32(&w, 2U, UCP_TLV_I32, rc);
        } else if (cmd == UCP_CMD_TASK_START_NEXT) {
            int rc = watering_process_next_task();
            ok = (rc > 0);
            (void)ucp_tlvw_put_i32(&w, 2U, UCP_TLV_I32, rc);
        } else if (cmd == UCP_CMD_TASK_CLEAR_PENDING) {
            int rc = watering_clear_task_queue();
            ok = (rc == 0);
            (void)ucp_tlvw_put_i32(&w, 2U, UCP_TLV_I32, rc);
        } else {
            return send_tlv(UCP_MSG_CMD_RESP, svc, cmd, UCP_STATUS_NOT_FOUND, NULL, req);
        }
        (void)ucp_tlvw_put_u8(&w, UCP_F_OK_BOOL, UCP_TLV_BOOL, ok ? 1U : 0U);
        if (!ok) {
            return send_tlv(UCP_MSG_CMD_RESP, svc, cmd, UCP_STATUS_BUSY, &w, req);
        }
        (void)maybe_task_status_event();
        return send_tlv(UCP_MSG_CMD_RESP, svc, cmd, UCP_STATUS_OK, &w, req);
    }

    if (svc == UCP_SVC_HISTORY) {
        uint8_t channel_id = 0xFFU;
        watering_error_t hrc;
        if (cmd == UCP_CMD_HISTORY_RESET_ALL) {
            hrc = watering_history_reset_all_history();
            (void)ucp_tlvw_put_u8(&w, UCP_F_OK_BOOL, UCP_TLV_BOOL, (hrc == WATERING_SUCCESS) ? 1U : 0U);
            if (hrc != WATERING_SUCCESS) return send_tlv(UCP_MSG_CMD_RESP, svc, cmd, ucp_status_from_werr(hrc), &w, req);
            g_ver_history++;
            (void)send_tlv(UCP_MSG_CMD_RESP, svc, cmd, UCP_STATUS_OK, &w, req);
            (void)emit_change(UCP_TOPIC_HISTORY_READY, UCP_SVC_HISTORY, UCP_CMD_HISTORY_RESET_ALL, g_ver_history, UCP_ORIGIN_UART, w.buf, w.len);
            return 0;
        }
        if (cmd == UCP_CMD_HISTORY_RESET_CHANNEL) {
            struct ucp_tlv_elem e; size_t off = 0U; bool has_id = false;
            while (ucp_tlv_next(p, len, &off, &e)) {
                if (e.field_id == Q_CHANNEL_ID || e.field_id == RES_CH_ID) {
                    if (!ucp_tlv_read_u8(&e, &channel_id)) return send_tlv(UCP_MSG_CMD_RESP, svc, cmd, UCP_STATUS_INVALID_ARG, NULL, req);
                    has_id = true;
                }
            }
            if (!has_id || channel_id >= WATERING_CHANNELS_COUNT) return send_tlv(UCP_MSG_CMD_RESP, svc, cmd, UCP_STATUS_INVALID_ARG, NULL, req);
            hrc = watering_history_reset_channel_history(channel_id);
            (void)ucp_tlvw_put_u8(&w, UCP_F_OK_BOOL, UCP_TLV_BOOL, (hrc == WATERING_SUCCESS) ? 1U : 0U);
            (void)ucp_tlvw_put_u8(&w, RES_CH_ID, UCP_TLV_U8, channel_id);
            if (hrc != WATERING_SUCCESS) return send_tlv(UCP_MSG_CMD_RESP, svc, cmd, ucp_status_from_werr(hrc), &w, req);
            g_ver_history++;
            (void)send_tlv(UCP_MSG_CMD_RESP, svc, cmd, UCP_STATUS_OK, &w, req);
            (void)emit_change_instance(UCP_TOPIC_HISTORY_READY, UCP_SVC_HISTORY, UCP_CMD_HISTORY_RESET_CHANNEL, channel_id,
                                       g_ver_history, UCP_ORIGIN_UART, w.buf, w.len);
            return 0;
        }
        if (cmd == UCP_CMD_HISTORY_RESET_RAIN) {
            hrc = rain_history_clear_all();
            (void)ucp_tlvw_put_u8(&w, UCP_F_OK_BOOL, UCP_TLV_BOOL, (hrc == WATERING_SUCCESS) ? 1U : 0U);
            if (hrc != WATERING_SUCCESS) return send_tlv(UCP_MSG_CMD_RESP, svc, cmd, ucp_status_from_werr(hrc), &w, req);
            g_ver_history++;
            (void)send_tlv(UCP_MSG_CMD_RESP, svc, cmd, UCP_STATUS_OK, &w, req);
            (void)emit_change(UCP_TOPIC_HISTORY_READY, UCP_SVC_HISTORY, UCP_RES_HISTORY_RAIN_STORAGE, g_ver_history, UCP_ORIGIN_UART, w.buf, w.len);
            return 0;
        }
        if (cmd == UCP_CMD_HISTORY_RESET_ENV) {
            int erc = env_history_reset_all();
            (void)ucp_tlvw_put_u8(&w, UCP_F_OK_BOOL, UCP_TLV_BOOL, (erc == 0) ? 1U : 0U);
            if (erc != 0) return send_tlv(UCP_MSG_CMD_RESP, svc, cmd, UCP_STATUS_INTERNAL, &w, req);
            g_ver_history++;
            (void)send_tlv(UCP_MSG_CMD_RESP, svc, cmd, UCP_STATUS_OK, &w, req);
            (void)emit_change(UCP_TOPIC_HISTORY_READY, UCP_SVC_HISTORY, UCP_RES_HISTORY_ENV_STORAGE, g_ver_history, UCP_ORIGIN_UART, w.buf, w.len);
            return 0;
        }
        return send_tlv(UCP_MSG_CMD_RESP, svc, cmd, UCP_STATUS_NOT_FOUND, NULL, req);
    }

    if (svc == UCP_SVC_DEVICE) {
        if (cmd == UCP_CMD_DEVICE_STATS_RESET_CHANNEL) {
            uint8_t channel_id = 0U;
            watering_error_t rc;
            if (!parse_channel_selector_optional(p, len, &channel_id, 0U, false)) {
                return send_tlv(UCP_MSG_CMD_RESP, svc, cmd, UCP_STATUS_INVALID_ARG, NULL, req);
            }
            rc = watering_reset_channel_statistics(channel_id);
            (void)ucp_tlvw_put_u8(&w, RES_CH_ID, UCP_TLV_U8, channel_id);
            (void)ucp_tlvw_put_u8(&w, UCP_F_OK_BOOL, UCP_TLV_BOOL, (rc == WATERING_SUCCESS) ? 1U : 0U);
            if (rc != WATERING_SUCCESS) return send_tlv(UCP_MSG_CMD_RESP, svc, cmd, ucp_status_from_werr(rc), &w, req);
            g_ver_stats[channel_id]++;
            (void)send_tlv(UCP_MSG_CMD_RESP, svc, cmd, UCP_STATUS_OK, &w, req);
            (void)emit_change_instance(UCP_TOPIC_STATISTICS, UCP_SVC_DEVICE, UCP_RES_DEVICE_STATISTICS, channel_id,
                                       g_ver_stats[channel_id], UCP_ORIGIN_UART, w.buf, w.len);
            return 0;
        }
        if (cmd == UCP_CMD_DEVICE_STATS_RESET_ALL) {
            watering_error_t first_err = WATERING_SUCCESS;
            for (uint8_t i = 0U; i < WATERING_CHANNELS_COUNT; i++) {
                watering_error_t rc = watering_reset_channel_statistics(i);
                if (rc != WATERING_SUCCESS && first_err == WATERING_SUCCESS) first_err = rc;
                if (rc == WATERING_SUCCESS) g_ver_stats[i]++;
            }
            (void)ucp_tlvw_put_u8(&w, UCP_F_OK_BOOL, UCP_TLV_BOOL, (first_err == WATERING_SUCCESS) ? 1U : 0U);
            if (first_err != WATERING_SUCCESS) return send_tlv(UCP_MSG_CMD_RESP, svc, cmd, ucp_status_from_werr(first_err), &w, req);
            (void)send_tlv(UCP_MSG_CMD_RESP, svc, cmd, UCP_STATUS_OK, &w, req);
            (void)emit_change_instance(UCP_TOPIC_STATISTICS, UCP_SVC_DEVICE, UCP_RES_DEVICE_STATISTICS, 0xFFFFFFFFU,
                                       g_ver_stats[0], UCP_ORIGIN_UART, w.buf, w.len);
            return 0;
        }
        if (cmd == UCP_CMD_DEVICE_RUN_AUTO_CALC) {
            watering_error_t rc = watering_run_automatic_calculations();
            (void)ucp_tlvw_put_u8(&w, UCP_F_OK_BOOL, UCP_TLV_BOOL, (rc == WATERING_SUCCESS) ? 1U : 0U);
            if (rc != WATERING_SUCCESS) return send_tlv(UCP_MSG_CMD_RESP, svc, cmd, ucp_status_from_werr(rc), &w, req);
            g_ver_auto_calc++;
            (void)send_tlv(UCP_MSG_CMD_RESP, svc, cmd, UCP_STATUS_OK, &w, req);
            (void)emit_change_instance(UCP_TOPIC_AUTO_CALC_STATUS, UCP_SVC_DEVICE, UCP_RES_DEVICE_AUTO_CALC_STATUS, 0xFFFFFFFFU,
                                       g_ver_auto_calc, UCP_ORIGIN_UART, w.buf, w.len);
            return 0;
        }
        if (cmd == UCP_CMD_DEVICE_CLEAR_ERRORS) {
            watering_error_t rc = watering_clear_errors();
            rain_sensor_clear_errors();
            (void)ucp_tlvw_put_u8(&w, UCP_F_OK_BOOL, UCP_TLV_BOOL, (rc == WATERING_SUCCESS) ? 1U : 0U);
            if (rc != WATERING_SUCCESS) return send_tlv(UCP_MSG_CMD_RESP, svc, cmd, ucp_status_from_werr(rc), &w, req);
            return send_tlv(UCP_MSG_CMD_RESP, svc, cmd, UCP_STATUS_OK, &w, req);
        }
        if (cmd == UCP_CMD_DEVICE_ALARM_CLEAR) {
            uint8_t alarm_code = 0U;
            struct ucp_tlv_elem e;
            size_t off = 0U;
            while (ucp_tlv_next(p, len, &off, &e)) {
                if (e.field_id == ALARM_CODE_F && ucp_tlv_read_u8(&e, &alarm_code)) {
                    break;
                }
            }
#ifdef CONFIG_BT
            {
                int rc = bt_irrigation_alarm_clear(alarm_code);
                (void)ucp_tlvw_put_u8(&w, ALARM_CODE_F, UCP_TLV_U8, alarm_code);
                (void)ucp_tlvw_put_u8(&w, UCP_F_OK_BOOL, UCP_TLV_BOOL, (rc == 0) ? 1U : 0U);
                if (rc != 0) return send_tlv(UCP_MSG_CMD_RESP, svc, cmd, UCP_STATUS_NOT_FOUND, &w, req);
            }
#else
            (void)watering_clear_errors();
            (void)ucp_tlvw_put_u8(&w, ALARM_CODE_F, UCP_TLV_U8, alarm_code);
            (void)ucp_tlvw_put_u8(&w, UCP_F_OK_BOOL, UCP_TLV_BOOL, 1U);
            (void)ucp_uart_publish_alarm(0U, 0U, true, UCP_ORIGIN_UART);
#endif
            return send_tlv(UCP_MSG_CMD_RESP, svc, cmd, UCP_STATUS_OK, &w, req);
        }
        return send_tlv(UCP_MSG_CMD_RESP, svc, cmd, UCP_STATUS_NOT_FOUND, NULL, req);
    }

    if (svc == UCP_SVC_FLOW) {
        uint8_t channel_id = 0U;
        if (cmd == UCP_CMD_FLOW_HYDRAULIC_CLEAR_CH_LOCK) {
            if (!parse_channel_selector_optional(p, len, &channel_id, 0U, false)) return send_tlv(UCP_MSG_CMD_RESP, svc, cmd, UCP_STATUS_INVALID_ARG, NULL, req);
            watering_hydraulic_clear_channel_lock(channel_id);
            g_ver_hydraulic[channel_id]++;
            (void)ucp_tlvw_put_u8(&w, RES_CH_ID, UCP_TLV_U8, channel_id);
            (void)send_tlv(UCP_MSG_CMD_RESP, svc, cmd, UCP_STATUS_OK, &w, req);
            (void)emit_change_instance(UCP_TOPIC_HYDRAULIC_STATUS, UCP_SVC_FLOW, UCP_RES_FLOW_HYDRAULIC_STATUS, channel_id,
                                       g_ver_hydraulic[channel_id], UCP_ORIGIN_UART, w.buf, w.len);
            return 0;
        }
        if (cmd == UCP_CMD_FLOW_HYDRAULIC_CLEAR_GLOBAL_LOCK) {
            watering_hydraulic_clear_global_lock();
            (void)send_tlv(UCP_MSG_CMD_RESP, svc, cmd, UCP_STATUS_OK, &w, req);
            (void)emit_change_instance(UCP_TOPIC_HYDRAULIC_STATUS, UCP_SVC_FLOW, UCP_RES_FLOW_HYDRAULIC_STATUS, 0xFFFFFFFFU,
                                       ++g_ver_flow, UCP_ORIGIN_UART, w.buf, w.len);
            return 0;
        }
        if (cmd == UCP_CMD_FLOW_HYDRAULIC_SET_MANUAL_OVERRIDE) {
            if (!parse_channel_selector_optional(p, len, &channel_id, 0U, false)) return send_tlv(UCP_MSG_CMD_RESP, svc, cmd, UCP_STATUS_INVALID_ARG, NULL, req);
            watering_hydraulic_set_manual_override(channel_id, 300000U);
            g_ver_hydraulic[channel_id]++;
            (void)ucp_tlvw_put_u8(&w, RES_CH_ID, UCP_TLV_U8, channel_id);
            (void)send_tlv(UCP_MSG_CMD_RESP, svc, cmd, UCP_STATUS_OK, &w, req);
            (void)emit_change_instance(UCP_TOPIC_HYDRAULIC_STATUS, UCP_SVC_FLOW, UCP_RES_FLOW_HYDRAULIC_STATUS, channel_id,
                                       g_ver_hydraulic[channel_id], UCP_ORIGIN_UART, w.buf, w.len);
            return 0;
        }
        if (cmd == UCP_CMD_FLOW_HYDRAULIC_CLEAR_MANUAL_OVERRIDE) {
            watering_hydraulic_clear_manual_override();
            (void)send_tlv(UCP_MSG_CMD_RESP, svc, cmd, UCP_STATUS_OK, &w, req);
            (void)emit_change_instance(UCP_TOPIC_HYDRAULIC_STATUS, UCP_SVC_FLOW, UCP_RES_FLOW_HYDRAULIC_STATUS, 0xFFFFFFFFU,
                                       ++g_ver_flow, UCP_ORIGIN_UART, w.buf, w.len);
            return 0;
        }
        return send_tlv(UCP_MSG_CMD_RESP, svc, cmd, UCP_STATUS_NOT_FOUND, NULL, req);
    }

    if (svc == UCP_SVC_ONBOARDING_RESET) {
        struct ucp_tlv_elem e;
        size_t off = 0U;
        uint8_t reset_type = 0xFFU, channel_id = 0xFFU, group = 0xFFU;
        uint32_t code = 0U;
        char reason[32] = {0};
        bool has_type = false, has_channel = false, has_group = false, has_code = false;

        while (ucp_tlv_next(p, len, &off, &e)) {
            if (e.field_id == RESET_TYPE_F) { if (!ucp_tlv_read_u8(&e, &reset_type)) return send_tlv(UCP_MSG_CMD_RESP, svc, cmd, UCP_STATUS_INVALID_ARG, NULL, req); has_type = true; continue; }
            if (e.field_id == RESET_CHANNEL_F) { if (!ucp_tlv_read_u8(&e, &channel_id)) return send_tlv(UCP_MSG_CMD_RESP, svc, cmd, UCP_STATUS_INVALID_ARG, NULL, req); has_channel = true; continue; }
            if (e.field_id == RESET_GROUP_F) { if (!ucp_tlv_read_u8(&e, &group)) return send_tlv(UCP_MSG_CMD_RESP, svc, cmd, UCP_STATUS_INVALID_ARG, NULL, req); has_group = true; continue; }
            if (e.field_id == RESET_CODE_F) { if (!ucp_tlv_read_u32(&e, &code)) return send_tlv(UCP_MSG_CMD_RESP, svc, cmd, UCP_STATUS_INVALID_ARG, NULL, req); has_code = true; continue; }
            if (e.field_id == RESET_REASON_F) {
                size_t cpy = MIN((size_t)e.len, sizeof(reason) - 1U);
                memcpy(reason, e.value, cpy);
                reason[cpy] = '\0';
            }
        }

        if (cmd == UCP_CMD_ONB_RESET_GEN_CODE) {
            uint32_t gen;
            reset_confirmation_t info = {0};
            if (!has_type) return send_tlv(UCP_MSG_CMD_RESP, svc, cmd, UCP_STATUS_INVALID_ARG, NULL, req);
            if (!has_channel) channel_id = 0xFFU;
            gen = reset_controller_generate_confirmation_code((reset_type_t)reset_type, channel_id);
            if (gen == 0U) return send_tlv(UCP_MSG_CMD_RESP, svc, cmd, UCP_STATUS_INTERNAL, NULL, req);
            (void)reset_controller_get_confirmation_info(&info);
            (void)ucp_tlvw_put_u32(&w, RESET_CODE_F, UCP_TLV_U32, gen);
            (void)ucp_tlvw_put_u8(&w, RESET_TYPE_F, UCP_TLV_U8, reset_type);
            (void)ucp_tlvw_put_u8(&w, RESET_CHANNEL_F, UCP_TLV_U8, channel_id);
            (void)ucp_tlvw_put_u32(&w, OTA_WAIT, UCP_TLV_U32,
                                  (info.expiry_time > info.generation_time) ? ((info.expiry_time - info.generation_time) * 1000U) : 0U);
            g_ver_reset_ctl++;
            (void)send_tlv(UCP_MSG_CMD_RESP, svc, cmd, UCP_STATUS_OK, &w, req);
            (void)emit_change(UCP_TOPIC_RESET_PROGRESS, UCP_SVC_ONBOARDING_RESET, UCP_RES_ONB_RESET_CONTROL, g_ver_reset_ctl, UCP_ORIGIN_UART, w.buf, w.len);
            return 0;
        }
        if (cmd == UCP_CMD_ONB_RESET_EXECUTE) {
            reset_request_t rr = {0};
            reset_status_t rs;
            if (!has_type || !has_code) return send_tlv(UCP_MSG_CMD_RESP, svc, cmd, UCP_STATUS_INVALID_ARG, NULL, req);
            rr.type = (reset_type_t)reset_type;
            rr.channel_id = has_channel ? channel_id : 0xFFU;
            rr.confirmation_code = code;
            rs = reset_controller_execute(&rr);
            (void)ucp_tlvw_put_u8(&w, RESET_TYPE_F, UCP_TLV_U8, reset_type);
            (void)ucp_tlvw_put_u8(&w, RESET_CHANNEL_F, UCP_TLV_U8, rr.channel_id);
            (void)ucp_tlvw_put_u8(&w, UCP_F_OK_BOOL, UCP_TLV_BOOL, (rs == RESET_STATUS_SUCCESS) ? 1U : 0U);
            if (rs != RESET_STATUS_SUCCESS) return send_tlv(UCP_MSG_CMD_RESP, svc, cmd, ucp_status_from_reset_status(rs), &w, req);
            g_ver_reset_ctl++;
            g_ver_onboarding++;
            (void)send_tlv(UCP_MSG_CMD_RESP, svc, cmd, UCP_STATUS_OK, &w, req);
            (void)emit_change(UCP_TOPIC_RESET_PROGRESS, UCP_SVC_ONBOARDING_RESET, UCP_RES_ONB_RESET_CONTROL, g_ver_reset_ctl, UCP_ORIGIN_UART, w.buf, w.len);
            (void)emit_change(UCP_TOPIC_ONBOARDING_STATUS, UCP_SVC_ONBOARDING_RESET, UCP_RES_ONB_STATUS, g_ver_onboarding, UCP_ORIGIN_UART, w.buf, w.len);
            return 0;
        }
        if (cmd == UCP_CMD_ONB_RESET_CLEAR_CONFIRM) {
            if (reset_controller_clear_confirmation_code() != 0) return send_tlv(UCP_MSG_CMD_RESP, svc, cmd, UCP_STATUS_INTERNAL, NULL, req);
            g_ver_reset_ctl++;
            (void)ucp_tlvw_put_u8(&w, UCP_F_OK_BOOL, UCP_TLV_BOOL, 1U);
            (void)send_tlv(UCP_MSG_CMD_RESP, svc, cmd, UCP_STATUS_OK, &w, req);
            (void)emit_change(UCP_TOPIC_RESET_PROGRESS, UCP_SVC_ONBOARDING_RESET, UCP_RES_ONB_RESET_CONTROL, g_ver_reset_ctl, UCP_ORIGIN_UART, w.buf, w.len);
            return 0;
        }
        if (cmd == UCP_CMD_ONB_RESET_CLEAR_WIPE_STATE) {
            if (reset_controller_clear_wipe_state() != 0) return send_tlv(UCP_MSG_CMD_RESP, svc, cmd, UCP_STATUS_INTERNAL, NULL, req);
            g_ver_reset_ctl++;
            (void)ucp_tlvw_put_u8(&w, UCP_F_OK_BOOL, UCP_TLV_BOOL, 1U);
            (void)send_tlv(UCP_MSG_CMD_RESP, svc, cmd, UCP_STATUS_OK, &w, req);
            (void)emit_change(UCP_TOPIC_RESET_PROGRESS, UCP_SVC_ONBOARDING_RESET, UCP_RES_ONB_RESET_CONTROL, g_ver_reset_ctl, UCP_ORIGIN_UART, w.buf, w.len);
            return 0;
        }
        if (cmd == UCP_CMD_ONB_ONBOARDING_RESET_STATE) {
            if (onboarding_reset_state() != 0) return send_tlv(UCP_MSG_CMD_RESP, svc, cmd, UCP_STATUS_INTERNAL, NULL, req);
            g_ver_onboarding++;
            (void)ucp_tlvw_put_u8(&w, UCP_F_OK_BOOL, UCP_TLV_BOOL, 1U);
            (void)send_tlv(UCP_MSG_CMD_RESP, svc, cmd, UCP_STATUS_OK, &w, req);
            (void)emit_change(UCP_TOPIC_ONBOARDING_STATUS, UCP_SVC_ONBOARDING_RESET, UCP_RES_ONB_STATUS, g_ver_onboarding, UCP_ORIGIN_UART, w.buf, w.len);
            return 0;
        }
        if (cmd == UCP_CMD_ONB_CONFIG_RESET_GROUP) {
            watering_error_t rc;
            if (!has_channel || !has_group) return send_tlv(UCP_MSG_CMD_RESP, svc, cmd, UCP_STATUS_INVALID_ARG, NULL, req);
            rc = channel_reset_config_group(channel_id, (config_group_t)group, reason[0] ? reason : "ucp");
            (void)ucp_tlvw_put_u8(&w, RESET_CHANNEL_F, UCP_TLV_U8, channel_id);
            (void)ucp_tlvw_put_u8(&w, RESET_GROUP_F, UCP_TLV_U8, group);
            (void)ucp_tlvw_put_u8(&w, UCP_F_OK_BOOL, UCP_TLV_BOOL, (rc == WATERING_SUCCESS) ? 1U : 0U);
            if (rc != WATERING_SUCCESS) return send_tlv(UCP_MSG_CMD_RESP, svc, cmd, ucp_status_from_werr(rc), &w, req);
            if (channel_id < WATERING_CHANNELS_COUNT) g_ver_cfg_status[channel_id]++;
            g_ver_onboarding++;
            (void)send_tlv(UCP_MSG_CMD_RESP, svc, cmd, UCP_STATUS_OK, &w, req);
            (void)emit_change_instance(UCP_TOPIC_CHANNEL_CFG_STATUS, UCP_SVC_ONBOARDING_RESET, UCP_RES_ONB_CONFIG_STATUS, channel_id,
                                       (channel_id < WATERING_CHANNELS_COUNT) ? g_ver_cfg_status[channel_id] : 1U,
                                       UCP_ORIGIN_UART, w.buf, w.len);
            (void)emit_change(UCP_TOPIC_ONBOARDING_STATUS, UCP_SVC_ONBOARDING_RESET, UCP_RES_ONB_STATUS, g_ver_onboarding, UCP_ORIGIN_UART, w.buf, w.len);
            return 0;
        }
        return send_tlv(UCP_MSG_CMD_RESP, svc, cmd, UCP_STATUS_NOT_FOUND, NULL, req);
    }

    if (svc == UCP_SVC_PACKS) {
        struct ucp_tlv_elem e;
        size_t off = 0U;
        uint16_t pack_id = 0U, plant_id = 0U;
        bool delete_plants = false;
        bool has_pack = false, has_plant = false;
        pack_result_t pr;
        pack_storage_stats_t st = {0};
        while (ucp_tlv_next(p, len, &off, &e)) {
            if ((e.field_id == Q_PACK_ID) && ucp_tlv_read_u16(&e, &pack_id)) { has_pack = true; continue; }
            if ((e.field_id == Q_PLANT_ID) && ucp_tlv_read_u16(&e, &plant_id)) { has_plant = true; continue; }
            if ((e.field_id == Q_DELETE_PLANTS) && e.len == 1U) { delete_plants = (e.value[0] != 0U); continue; }
        }
        if (!pack_storage_is_ready()) return send_tlv(UCP_MSG_CMD_RESP, svc, cmd, UCP_STATUS_BUSY, NULL, req);

        if (cmd == UCP_CMD_PACKS_DELETE_PACK) {
            if (!has_pack) return send_tlv(UCP_MSG_CMD_RESP, svc, cmd, UCP_STATUS_INVALID_ARG, NULL, req);
            pr = pack_storage_delete_pack(pack_id, delete_plants);
            (void)ucp_tlvw_put_u16(&w, Q_PACK_ID, UCP_TLV_U16, pack_id);
            (void)ucp_tlvw_put_u8(&w, Q_DELETE_PLANTS, UCP_TLV_BOOL, delete_plants ? 1U : 0U);
            if (pr != PACK_RESULT_SUCCESS) return send_tlv(UCP_MSG_CMD_RESP, svc, cmd, ucp_status_from_pack_result(pr), &w, req);
        } else if (cmd == UCP_CMD_PACKS_DELETE_PLANT) {
            if (!has_plant) return send_tlv(UCP_MSG_CMD_RESP, svc, cmd, UCP_STATUS_INVALID_ARG, NULL, req);
            pr = pack_storage_delete_plant(plant_id);
            (void)ucp_tlvw_put_u16(&w, Q_PLANT_ID, UCP_TLV_U16, plant_id);
            if (pr != PACK_RESULT_SUCCESS) return send_tlv(UCP_MSG_CMD_RESP, svc, cmd, ucp_status_from_pack_result(pr), &w, req);
        } else if (cmd == UCP_CMD_PACKS_INSTALL_STAGED_PLANT) {
            pack_plant_v1_t plant = {0};
            if (!g_staged_blob_valid || g_staged_blob_len != sizeof(plant)) {
                return send_tlv(UCP_MSG_CMD_RESP, svc, cmd, UCP_STATUS_PRECONDITION_FAILED, NULL, req);
            }
            memcpy(&plant, g_staged_blob, sizeof(plant));
            pr = pack_storage_install_plant(&plant);
            (void)ucp_tlvw_put_u16(&w, Q_PLANT_ID, UCP_TLV_U16, plant.plant_id);
            if (pr != PACK_RESULT_SUCCESS && pr != PACK_RESULT_UPDATED && pr != PACK_RESULT_ALREADY_CURRENT) {
                return send_tlv(UCP_MSG_CMD_RESP, svc, cmd, ucp_status_from_pack_result(pr), &w, req);
            }
            g_staged_blob_valid = false;
            g_staged_blob_len = 0U;
            g_staged_blob_crc = 0U;
        } else if (cmd == UCP_CMD_PACKS_INSTALL_STAGED_PACK_BUNDLE) {
            const pack_pack_v1_t *pack_meta;
            const pack_plant_v1_t *plants = NULL;
            uint32_t expected_len;
            if (!g_staged_blob_valid || g_staged_blob_len < sizeof(pack_pack_v1_t)) {
                return send_tlv(UCP_MSG_CMD_RESP, svc, cmd, UCP_STATUS_PRECONDITION_FAILED, NULL, req);
            }
            pack_meta = (const pack_pack_v1_t *)g_staged_blob;
            expected_len = (uint32_t)sizeof(pack_pack_v1_t) + ((uint32_t)pack_meta->plant_count * (uint32_t)sizeof(pack_plant_v1_t));
            if (expected_len != g_staged_blob_len) {
                return send_tlv(UCP_MSG_CMD_RESP, svc, cmd, UCP_STATUS_CONFLICT, NULL, req);
            }
            if (pack_meta->plant_count > 0U) {
                plants = (const pack_plant_v1_t *)(const void *)&g_staged_blob[sizeof(pack_pack_v1_t)];
            }
            pr = pack_storage_install_pack(pack_meta, plants, pack_meta->plant_count);
            (void)ucp_tlvw_put_u16(&w, Q_PACK_ID, UCP_TLV_U16, pack_meta->pack_id);
            (void)ucp_tlvw_put_u16(&w, Q_LIMIT, UCP_TLV_U16, pack_meta->plant_count);
            if (pr != PACK_RESULT_SUCCESS && pr != PACK_RESULT_UPDATED && pr != PACK_RESULT_ALREADY_CURRENT) {
                return send_tlv(UCP_MSG_CMD_RESP, svc, cmd, ucp_status_from_pack_result(pr), &w, req);
            }
            g_staged_blob_valid = false;
            g_staged_blob_len = 0U;
            g_staged_blob_crc = 0U;
        } else {
            return send_tlv(UCP_MSG_CMD_RESP, svc, cmd, UCP_STATUS_NOT_FOUND, NULL, req);
        }

        ucp_pack_stats_cache_invalidate();
        ucp_pack_stats_cache_request_refresh();
        if (ucp_pack_stats_cache_get(&st)) {
            (void)ucp_tlvw_put_u32(&w, PACKS_CHANGE_COUNTER, UCP_TLV_U32, st.change_counter);
        }
        g_ver_packs++;
        (void)send_tlv(UCP_MSG_CMD_RESP, svc, cmd, UCP_STATUS_OK, &w, req);
        (void)emit_change(UCP_TOPIC_PACKS_CHANGED, UCP_SVC_PACKS, UCP_RES_PACKS_STATS, g_ver_packs, UCP_ORIGIN_UART, w.buf, w.len);
        return 0;
    }

    if (svc == UCP_SVC_PEER_POWER) {
        if (cmd == UCP_CMD_PEER_POWER_OFF) {
            if (peer_power_inhibit()) {
                (void)ucp_tlvw_put_u8(&w, PEER_STATE_INHIBIT, UCP_TLV_BOOL, 1U);
                return send_tlv(UCP_MSG_CMD_RESP, svc, cmd, UCP_STATUS_LOCKED, &w, req);
            }
            if (peer_power_apply_hw(false) != 0) return send_tlv(UCP_MSG_CMD_RESP, svc, cmd, UCP_STATUS_HW_FAULT, NULL, req);
            g_peer_power_on = false;
            (void)ucp_tlvw_put_u8(&w, PEER_STATE_ON, UCP_TLV_BOOL, 0U);
            (void)ucp_tlvw_put_u8(&w, PEER_STATE_HW_BACKED, UCP_TLV_BOOL, g_peer_power_hw_backed ? 1U : 0U);
            (void)emit_peer_power_change(PEER_REASON_UART_CMD, UCP_ORIGIN_UART);
            return send_tlv(UCP_MSG_CMD_RESP, svc, cmd, UCP_STATUS_OK, &w, req);
        }
        if (cmd == UCP_CMD_PEER_POWER_ON) {
            if (peer_power_apply_hw(true) != 0) return send_tlv(UCP_MSG_CMD_RESP, svc, cmd, UCP_STATUS_HW_FAULT, NULL, req);
            g_peer_power_on = true;
            (void)ucp_tlvw_put_u8(&w, PEER_STATE_ON, UCP_TLV_BOOL, 1U);
            (void)ucp_tlvw_put_u8(&w, PEER_STATE_HW_BACKED, UCP_TLV_BOOL, g_peer_power_hw_backed ? 1U : 0U);
            (void)emit_peer_power_change(PEER_REASON_UART_CMD, UCP_ORIGIN_UART);
            return send_tlv(UCP_MSG_CMD_RESP, svc, cmd, UCP_STATUS_OK, &w, req);
        }
        if (cmd == UCP_CMD_PEER_POWER_CYCLE) {
            if (peer_power_inhibit()) {
                (void)ucp_tlvw_put_u8(&w, PEER_STATE_INHIBIT, UCP_TLV_BOOL, 1U);
                return send_tlv(UCP_MSG_CMD_RESP, svc, cmd, UCP_STATUS_LOCKED, &w, req);
            }
            if (peer_power_apply_hw(false) != 0) return send_tlv(UCP_MSG_CMD_RESP, svc, cmd, UCP_STATUS_HW_FAULT, NULL, req);
            g_peer_power_on = false;
            (void)emit_peer_power_change(PEER_REASON_UART_CMD, UCP_ORIGIN_UART);
            k_msleep(MIN((uint16_t)50U, g_peer_power_grace_ms));
            if (peer_power_apply_hw(true) != 0) return send_tlv(UCP_MSG_CMD_RESP, svc, cmd, UCP_STATUS_HW_FAULT, NULL, req);
            g_peer_power_on = true;
            (void)emit_peer_power_change(PEER_REASON_UART_CMD, UCP_ORIGIN_UART);
            (void)ucp_tlvw_put_u8(&w, PEER_STATE_ON, UCP_TLV_BOOL, 1U);
            return send_tlv(UCP_MSG_CMD_RESP, svc, cmd, UCP_STATUS_OK, &w, req);
        }
        return send_tlv(UCP_MSG_CMD_RESP, svc, cmd, UCP_STATUS_NOT_FOUND, NULL, req);
    }

    if (svc != UCP_SVC_OTA) return send_tlv(UCP_MSG_CMD_RESP, svc, cmd, UCP_STATUS_UNSUPPORTED, NULL, req);
    if (cmd == UCP_CMD_OTA_PREFLIGHT) {
        int sw = ota_swap_type_safe();
        (void)ucp_tlvw_put_u8(&w, OTA_SUP, UCP_TLV_BOOL, 1U);
        (void)ucp_tlvw_put_u8(&w, OTA_MODE, UCP_TLV_U8, 1U); /* recovery-first */
        (void)ucp_tlvw_put_u8(&w, OTA_CONFIRM_POLICY, UCP_TLV_U8, 1U); /* app auto-confirms on boot */
        (void)ucp_tlvw_put_u8(&w, OTA_CONFIRM, UCP_TLV_BOOL, ota_img_confirmed_safe() ? 1U : 0U);
        (void)ucp_tlvw_put_i32(&w, OTA_SWAP, UCP_TLV_I32, sw);
        (void)ucp_tlvw_put_u8(&w, OTA_ROLLBACK_OBS, UCP_TLV_BOOL,
                              (sw == BOOT_SWAP_TYPE_REVERT || sw == BOOT_SWAP_TYPE_FAIL) ? 1U : 0U);
#ifdef CONFIG_RETENTION_BOOT_MODE
        (void)ucp_tlvw_put_u8(&w, OTA_ACC, UCP_TLV_BOOL, 1U);
#else
        (void)ucp_tlvw_put_u8(&w, OTA_ACC, UCP_TLV_BOOL, 0U);
        (void)ucp_tlvw_put_str(&w, OTA_REASON, UCP_TLV_UTF8, "bootmode retention disabled");
#endif
        (void)ucp_tlvw_put_u32(&w, OTA_WAIT, UCP_TLV_U32, 0U); /* boot-mode path is indefinite until reset/upload */
        (void)ucp_tlvw_put_u8(&w, OTA_PENDING, UCP_TLV_BOOL, g_ota_reboot_pending ? 1U : 0U);
        return send_tlv(UCP_MSG_CMD_RESP, svc, cmd, UCP_STATUS_OK, &w, req);
    }
    if (cmd == UCP_CMD_OTA_ENTER_RECOVERY) {
        uint8_t delta_b[32];
        struct ucp_tlv_writer delta;
        int s_rc;
        uint32_t confirm_token = 0U;
        bool has_confirm_token = false;
        struct ucp_tlv_elem e;
        size_t off = 0U;
        while (ucp_tlv_next(p, len, &off, &e)) {
            if (e.field_id == OTA_CONFIRM) {
                if (!ucp_tlv_read_u32(&e, &confirm_token)) {
                    return send_tlv(UCP_MSG_CMD_RESP, svc, cmd, UCP_STATUS_INVALID_ARG, NULL, req);
                }
                has_confirm_token = true;
            }
        }
#ifndef CONFIG_RETENTION_BOOT_MODE
        (void)ucp_tlvw_put_u8(&w, OTA_ACC, UCP_TLV_BOOL, 0U);
        (void)ucp_tlvw_put_str(&w, OTA_REASON, UCP_TLV_UTF8, "RETENTION_BOOT_MODE unavailable");
        return send_tlv(UCP_MSG_CMD_RESP, svc, cmd, UCP_STATUS_UNSUPPORTED, &w, req);
#else
        if (g_ota_reboot_pending) {
            (void)ucp_tlvw_put_u8(&w, OTA_ACC, UCP_TLV_BOOL, 0U);
            (void)ucp_tlvw_put_str(&w, OTA_REASON, UCP_TLV_UTF8, "recovery reboot pending");
            return send_tlv(UCP_MSG_CMD_RESP, svc, cmd, UCP_STATUS_BUSY, &w, req);
        }
        if (!has_confirm_token || !ota_confirm_token_matches(confirm_token)) {
            uint32_t token = (ota_confirm_token_remaining_ms() != 0U) ? g_ota_confirm_token : ota_confirm_token_generate();
            (void)ucp_tlvw_put_u8(&w, OTA_ACC, UCP_TLV_BOOL, 0U);
            (void)ucp_tlvw_put_u32(&w, OTA_CONFIRM, UCP_TLV_U32, token);
            (void)ucp_tlvw_put_u32(&w, OTA_WAIT, UCP_TLV_U32, ota_confirm_token_remaining_ms());
            (void)ucp_tlvw_put_str(&w, OTA_REASON, UCP_TLV_UTF8, "replay ota.enter_recovery with OTA_CONFIRM token");
            return send_tlv(UCP_MSG_CMD_RESP, svc, cmd, UCP_STATUS_CONFIRMATION_REQD, &w, req);
        }
        g_ota_reboot_pending = true;
        (void)ucp_tlvw_put_u8(&w, OTA_ACC, UCP_TLV_BOOL, 1U);
        (void)ucp_tlvw_put_u8(&w, OTA_MODE, UCP_TLV_U8, 1U);
        (void)ucp_tlvw_put_u32(&w, OTA_WAIT, UCP_TLV_U32, 0U);
        (void)ucp_tlvw_put_str(&w, OTA_REASON, UCP_TLV_UTF8, "bootmode+reboot");
        s_rc = send_tlv(UCP_MSG_CMD_RESP, svc, cmd, UCP_STATUS_OK, &w, req);
        if (s_rc != 0) {
            g_ota_reboot_pending = false;
            return s_rc;
        }

        ucp_tlvw_init(&delta, delta_b, sizeof(delta_b));
        (void)ucp_tlvw_put_u8(&delta, OTA_STATE, UCP_TLV_U8, OTA_STATE_ENTERING_RECOVERY);
        (void)ucp_tlvw_put_u8(&delta, OTA_PENDING, UCP_TLV_BOOL, 1U);
        (void)ucp_tlvw_put_u32(&delta, OTA_WAIT, UCP_TLV_U32, 0U);
        (void)ucp_tlvw_put_u8(&delta, OTA_CONFIRM_POLICY, UCP_TLV_U8, 1U);
        (void)emit_change(UCP_TOPIC_OTA_TRANSITION, UCP_SVC_OTA, UCP_CMD_OTA_ENTER_RECOVERY,
                          ++g_ver_ota, UCP_ORIGIN_UART, delta.buf, delta.len);
        (void)emit_peer_power_change(PEER_REASON_OTA, UCP_ORIGIN_LOCAL_FW);

        if (k_work_reschedule(&g_ota_reboot_work, K_MSEC(UCP_OTA_REBOOT_DELAY_MS)) < 0) {
            g_ota_reboot_pending = false;
#ifdef CONFIG_RETENTION_BOOT_MODE
            (void)bootmode_clear();
#endif
            return -EIO;
        }
        return 0;
#endif
    }
    if (cmd == UCP_CMD_OTA_POST_CHECK) {
        int sw = ota_swap_type_safe();
        bool confirmed = ota_img_confirmed_safe();
        bool rollback = (sw == BOOT_SWAP_TYPE_REVERT || sw == BOOT_SWAP_TYPE_FAIL);
        (void)ucp_tlvw_put_str(&w, OTA_FW, UCP_TLV_UTF8, APP_VERSION_EXTENDED_STRING);
        (void)ucp_tlvw_put_u32(&w, UCP_F_UPTIME_MS, UCP_TLV_U32, k_uptime_get_32());
        (void)ucp_tlvw_put_u8(&w, OTA_STATE, UCP_TLV_U8, OTA_STATE_POSTCHECK);
        (void)ucp_tlvw_put_u8(&w, OTA_PENDING, UCP_TLV_BOOL, g_ota_reboot_pending ? 1U : 0U);
        (void)ucp_tlvw_put_u8(&w, OTA_CONFIRM, UCP_TLV_BOOL, confirmed ? 1U : 0U);
        (void)ucp_tlvw_put_u8(&w, OTA_CONFIRM_POLICY, UCP_TLV_U8, 1U);
        (void)ucp_tlvw_put_i32(&w, OTA_SWAP, UCP_TLV_I32, sw);
        (void)ucp_tlvw_put_u8(&w, OTA_ROLLBACK_OBS, UCP_TLV_BOOL, rollback ? 1U : 0U);
        if (rollback) {
            (void)ucp_tlvw_put_str(&w, OTA_REASON, UCP_TLV_UTF8, "mcuboot swap indicates revert/fail");
        }
        (void)emit_ota_transition(OTA_STATE_POSTCHECK, g_ota_reboot_pending, UCP_ORIGIN_LOCAL_FW, rollback ? 1U : 0U);
        return send_tlv(UCP_MSG_CMD_RESP, svc, cmd, UCP_STATUS_OK, &w, req);
    }
    return send_tlv(UCP_MSG_CMD_RESP, svc, cmd, UCP_STATUS_NOT_FOUND, NULL, req);
}

static int handle_sub(const uint8_t *p, uint16_t len, uint32_t req)
{
    struct ucp_tlv_elem e;
    size_t off = 0U;
    uint32_t mask = 0U;
    uint32_t replay_mask;
    uint8_t sub_mode = UCP_SUB_MODE_LIVE_ONLY;
    uint64_t since_event_seq = 0ULL;
    bool has_since = false;
    uint8_t rb[48];
    struct ucp_tlv_writer w;
    while (ucp_tlv_next(p, len, &off, &e)) {
        if (e.field_id == UCP_F_TOPIC_MASK) {
            if (!ucp_tlv_read_u32(&e, &mask)) return send_tlv(UCP_MSG_SUB_RESP, UCP_SVC_SESSION, 0U, UCP_STATUS_INVALID_ARG, NULL, req);
            continue;
        }
        if (e.field_id == UCP_F_SUB_MODE) {
            if (!ucp_tlv_read_u8(&e, &sub_mode)) return send_tlv(UCP_MSG_SUB_RESP, UCP_SVC_SESSION, 0U, UCP_STATUS_INVALID_ARG, NULL, req);
            continue;
        }
        if (e.field_id == UCP_F_SINCE_EVENT_SEQ) {
            if (!ucp_tlv_read_u64(&e, &since_event_seq)) return send_tlv(UCP_MSG_SUB_RESP, UCP_SVC_SESSION, 0U, UCP_STATUS_INVALID_ARG, NULL, req);
            has_since = true;
            continue;
        }
    }
    if (mask == 0U) return send_tlv(UCP_MSG_SUB_RESP, UCP_SVC_SESSION, 0U, UCP_STATUS_INVALID_ARG, NULL, req);
    if (sub_mode != UCP_SUB_MODE_LIVE_ONLY && sub_mode != UCP_SUB_MODE_REPLAY_THEN_LIVE) {
        return send_tlv(UCP_MSG_SUB_RESP, UCP_SVC_SESSION, 0U, UCP_STATUS_UNSUPPORTED, NULL, req);
    }
    g_sub_mask |= mask;
    replay_mask = mask;
    ucp_tlvw_init(&w, rb, sizeof(rb));
    (void)ucp_tlvw_put_u32(&w, UCP_F_ACCEPTED_TOPIC_MASK, UCP_TLV_U32, g_sub_mask);
    (void)ucp_tlvw_put_u8(&w, UCP_F_SUB_MODE, UCP_TLV_U8, sub_mode);
    (void)send_tlv(UCP_MSG_SUB_RESP, UCP_SVC_SESSION, 0U, UCP_STATUS_OK, &w, req);

    if (sub_mode == UCP_SUB_MODE_REPLAY_THEN_LIVE) {
        uint64_t first_avail = 0ULL;
        uint64_t last_seq = 0ULL;
        uint64_t dropped = 0ULL;
        uint64_t since = has_since ? since_event_seq : 0ULL;
        k_mutex_lock(&g_ucp_evt_mutex, K_FOREVER);
        first_avail = replay_first_seq();
        last_seq = replay_last_seq();
        if (g_replay_count > 0U && since < last_seq && (since + 1ULL) < first_avail) {
            dropped = first_avail - (since + 1ULL);
            k_mutex_unlock(&g_ucp_evt_mutex);
            (void)send_event_overflow(first_avail, last_seq, dropped);
            return 0;
        }
        (void)replay_send_since(replay_mask, since);
        k_mutex_unlock(&g_ucp_evt_mutex);
    }
    if (!g_ota_boot_telemetry_sent && topic_subscribed(UCP_TOPIC_OTA_TRANSITION)) {
        int sw = ota_swap_type_safe();
        bool rollback = (sw == BOOT_SWAP_TYPE_REVERT || sw == BOOT_SWAP_TYPE_FAIL);
        (void)emit_ota_transition(OTA_STATE_IDLE, g_ota_reboot_pending, UCP_ORIGIN_BOOT_RECOVERY, rollback ? 1U : 0U);
        g_ota_boot_telemetry_sent = true;
    }
    return 0;
}

static int handle_unsub(const uint8_t *p, uint16_t len, uint32_t req)
{
    struct ucp_tlv_elem e; size_t off = 0U; uint32_t mask = 0U; bool has = false; uint8_t rb[32]; struct ucp_tlv_writer w;
    while (ucp_tlv_next(p, len, &off, &e)) if (e.field_id == UCP_F_TOPIC_MASK) { if (!ucp_tlv_read_u32(&e, &mask)) return send_tlv(UCP_MSG_UNSUB_RESP,UCP_SVC_SESSION,0U,UCP_STATUS_INVALID_ARG,NULL,req); has = true; }
    if (has) g_sub_mask &= ~mask; else g_sub_mask = 0U;
    (void)k_work_reschedule(&g_ucp_notify_work, K_MSEC(5));
    ucp_tlvw_init(&w, rb, sizeof(rb)); (void)ucp_tlvw_put_u32(&w, UCP_F_ACCEPTED_TOPIC_MASK, UCP_TLV_U32, g_sub_mask);
    return send_tlv(UCP_MSG_UNSUB_RESP, UCP_SVC_SESSION, 0U, UCP_STATUS_OK, &w, req);
}

static int handle_hello(uint32_t req)
{
    uint32_t sid;
    uint8_t b[UCP_MAX_PAYLOAD_V1]; struct ucp_tlv_writer w;
    g_session = true; g_sub_mask = 0U; g_session_start_ms = k_uptime_get_32();
    g_topic_scope_mask_override = 0U;
    (void)atomic_set(&g_notify_rescan_mask, 0);
    (void)k_work_reschedule(&g_ucp_notify_work, K_MSEC(5));
    sid = sys_rand32_get();
    g_session_id = (sid != 0U) ? sid : 1U;
    g_diag.session_hello++;
    ucp_tlvw_init(&w, b, sizeof(b));
    (void)ucp_tlvw_put_u8(&w, UCP_F_VERSION, UCP_TLV_U8, UCP_PROTO_V1);
    (void)ucp_tlvw_put_u32(&w, UCP_F_SESSION_ID, UCP_TLV_U32, g_session_id);
    (void)ucp_tlvw_put_u64(&w, UCP_F_BOOT_ID, UCP_TLV_U64, g_boot_id);
    (void)ucp_tlvw_put_str(&w, UCP_F_FW_VERSION, UCP_TLV_UTF8, APP_VERSION_EXTENDED_STRING);
    (void)ucp_tlvw_put_u32(&w, UCP_F_CAPABILITIES, UCP_TLV_U32, UCP_HELLO_CAPS_MASK);
    (void)ucp_tlvw_put_u64(&w, UCP_F_EVENT_SEQ, UCP_TLV_U64, (g_evt_seq == 0ULL) ? 0ULL : (g_evt_seq - 1ULL));
    (void)ucp_tlvw_put_u32(&w, UCP_F_UART_BAUD, UCP_TLV_U32, UCP_UART_BAUD_DEFAULT);
    (void)ucp_tlvw_put_u16(&w, UCP_F_MAX_PAYLOAD, UCP_TLV_U16, UCP_MAX_PAYLOAD_V1);
    (void)ucp_tlvw_put_u16(&w, UCP_F_REPLAY_DEPTH, UCP_TLV_U16, UCP_REPLAY_DEPTH);
    return send_tlv(UCP_MSG_HELLO_RESP, UCP_SVC_SESSION, 0U, UCP_STATUS_OK, &w, req);
}

static int handle_frame(const struct ucp_frame_header_v1 *h, const uint8_t *p)
{
    g_last_rx_ms = k_uptime_get_32();
    if (h->msg_type != UCP_MSG_HELLO_REQ && !g_session) {
        g_diag.rx_proto_err++;
        return send_err(UCP_STATUS_PRECONDITION_FAILED, h->request_id, "HELLO required");
    }
    if (h->msg_type == UCP_MSG_HELLO_REQ) return handle_hello(h->request_id);
    if (h->msg_type == UCP_MSG_PING_REQ) return send_tlv(UCP_MSG_PING_RESP, UCP_SVC_SESSION, 0U, UCP_STATUS_OK, NULL, h->request_id);
    if (h->msg_type == UCP_MSG_GET_REQ) return handle_get(h->service_id, h->method_or_topic_id, p, h->payload_len, h->request_id);
    if (h->msg_type == UCP_MSG_SET_REQ) return handle_set(h->service_id, h->method_or_topic_id, p, h->payload_len, h->request_id);
    if (h->msg_type == UCP_MSG_CMD_REQ) return handle_cmd(h->service_id, h->method_or_topic_id, p, h->payload_len, h->request_id);
    if (h->msg_type == UCP_MSG_SUB_REQ) return handle_sub(p, h->payload_len, h->request_id);
    if (h->msg_type == UCP_MSG_UNSUB_REQ) return handle_unsub(p, h->payload_len, h->request_id);
    if (h->msg_type == UCP_MSG_STREAM_OPEN_REQ) return handle_stream_open(h->service_id, h->method_or_topic_id, p, h->payload_len, h->request_id);
    if (h->msg_type == UCP_MSG_STREAM_DATA) return handle_stream_data(p, h->payload_len, h->request_id);
    if (h->msg_type == UCP_MSG_STREAM_END) return handle_stream_end(p, h->payload_len, h->request_id);
    if (h->msg_type == UCP_MSG_STREAM_ABORT) return handle_stream_abort(p, h->payload_len, h->request_id);
    g_diag.rx_proto_err++;
    return send_err(UCP_STATUS_UNSUPPORTED, h->request_id, "unsupported msg");
}

static void process_delimited(void)
{
    struct ucp_frame_header_v1 h;
    int dec_len;
    uint32_t got_crc, calc_crc;
    if (g_rx_len == 0U) return;
    dec_len = ucp_cobs_decode(g_rx_accum, g_rx_len, g_decoded, sizeof(g_decoded));
    g_rx_len = 0U;
    if (dec_len < 0) { g_diag.rx_cobs_err++; return; }
    if (dec_len < (int)(sizeof(h) + 4U)) { g_diag.rx_proto_err++; return; }
    memcpy(&h, g_decoded, sizeof(h));
    if (h.proto_version != UCP_PROTO_V1 || h.header_len != UCP_HEADER_LEN_V1) { g_diag.rx_proto_err++; return; }
    if (h.payload_len > UCP_MAX_PAYLOAD_V1) { g_diag.rx_oversize++; return; }
    if ((size_t)dec_len != (sizeof(h) + h.payload_len + 4U)) { g_diag.rx_proto_err++; return; }
    got_crc = ucp_tlv_rd_u32_le(&g_decoded[sizeof(h) + h.payload_len]);
    calc_crc = ucp_crc32c(g_decoded, sizeof(h) + h.payload_len);
    if (got_crc != calc_crc) { g_diag.rx_crc_err++; return; }
    g_diag.rx_ok_frames++;
    (void)handle_frame(&h, &g_decoded[sizeof(h)]);
}

static void ucp_uart_irq_cb(const struct device *dev, void *user_data)
{
    uint8_t tmp[64];
    ARG_UNUSED(user_data);

    if (!dev) {
        return;
    }

    if (!uart_irq_update(dev)) {
        return;
    }

    while (uart_irq_rx_ready(dev)) {
        int rd = uart_fifo_read(dev, tmp, sizeof(tmp));
        if (rd <= 0) {
            break;
        }

        unsigned int key = irq_lock();
        uint32_t written = ring_buf_put(&g_rx_ring, tmp, (uint32_t)rd);
        irq_unlock(key);

        if (written < (uint32_t)rd) {
            g_dbg_irq_drop_bytes += ((uint32_t)rd - written);
            g_diag.rx_oversize++;
        }
        if (written > 0U) {
            k_sem_give(&g_ucp_wake_sem);
        }
    }
}

static void poll_rx(void)
{
    uint8_t c;
    if (g_uart_irq_rx_mode) {
        uint8_t tmp[64];
        while (1) {
            uint32_t got;
            unsigned int key = irq_lock();
            got = ring_buf_get(&g_rx_ring, tmp, sizeof(tmp));
            irq_unlock(key);
            if (got == 0U) {
                break;
            }
            g_dbg_rx_bytes += got;
            for (uint32_t i = 0U; i < got; i++) {
                c = tmp[i];
                if (c == 0U) { process_delimited(); continue; }
                if (g_rx_len >= sizeof(g_rx_accum)) { g_diag.rx_oversize++; g_rx_len = 0U; continue; }
                g_rx_accum[g_rx_len++] = c;
            }
        }
        return;
    }

    while (uart_poll_in(g_uart, &c) == 0) {
        g_dbg_rx_bytes++;
        if (c == 0U) { process_delimited(); continue; }
        if (g_rx_len >= sizeof(g_rx_accum)) { g_diag.rx_oversize++; g_rx_len = 0U; continue; }
        g_rx_accum[g_rx_len++] = c;
    }
}

static void maybe_heartbeat(void)
{
    uint32_t now = k_uptime_get_32();
    uint8_t b[64];
    struct ucp_tlv_writer w;
    uint64_t last_evt_seq;
    uint32_t evt_coalesced;
    uint32_t evt_rate_limited;
    if (!g_session || g_sub_mask == 0U) return;
    if ((now - g_last_evt_ms) < UCP_HEARTBEAT_MS) return;
    k_mutex_lock(&g_ucp_evt_mutex, K_FOREVER);
    last_evt_seq = (g_evt_seq == 0ULL) ? 0ULL : (g_evt_seq - 1ULL);
    evt_coalesced = g_diag.event_coalesced;
    evt_rate_limited = g_diag.event_rate_limited;
    k_mutex_unlock(&g_ucp_evt_mutex);
    ucp_tlvw_init(&w, b, sizeof(b));
    (void)ucp_tlvw_put_u64(&w, UCP_F_BOOT_ID, UCP_TLV_U64, g_boot_id);
    (void)ucp_tlvw_put_u64(&w, UCP_F_EVENT_SEQ, UCP_TLV_U64, last_evt_seq);
    (void)ucp_tlvw_put_u64(&w, UCP_F_TIMESTAMP_UTC_MS, UCP_TLV_U64, now_utc_ms());
    /* Optional hint so live-only consumers can attribute event-seq gaps to coalescing, not link loss. */
    (void)ucp_tlvw_put_u32(&w, DIAG_EVT_COALESCED, UCP_TLV_U32, evt_coalesced);
    (void)ucp_tlvw_put_u32(&w, DIAG_EVT_RATE_LIMITED, UCP_TLV_U32, evt_rate_limited);
    (void)send_tlv(UCP_MSG_EVENT_HEARTBEAT, 0U, 0U, UCP_STATUS_OK, &w, 0U);
}

static void ucp_reset_rx_parser_state(void)
{
    g_rx_len = 0U;
    if (g_uart_irq_rx_mode) {
        unsigned int key = irq_lock();
        ring_buf_init(&g_rx_ring, sizeof(g_rx_ring_mem), g_rx_ring_mem);
        irq_unlock(key);
    }
}

static void ucp_drop_session_state(void)
{
    g_session = false;
    g_session_start_ms = 0U;
    g_sub_mask = 0U;
    g_topic_scope_mask_override = 0U;
    (void)atomic_set(&g_notify_rescan_mask, 0);
    ucp_reset_rx_parser_state();
}

void ucp_uart_request_notify_rescan(uint32_t topic_mask)
{
    uint32_t now_ms;
    uint32_t pending_mask;
    uint32_t filtered_mask = 0U;

    if (topic_mask == 0U) {
        return;
    }
    /* Do not accumulate rescans during boot / pre-HELLO / pre-SUB.
     * Boot-time notification hooks can fire a large burst and the first deferred rescan
     * after SUB becomes very expensive (pack stats/raw snapshots), which can starve UART
     * and has caused lockups on nRF52840. Only accept deferred rescans when a client is
     * actively connected and subscribed.
     */
    if (!(g_ready && g_session && (g_sub_mask != 0U))) {
        return;
    }
    if (g_session_start_ms != 0U &&
        (k_uptime_get_32() - g_session_start_ms) < UCP_DEFERRED_RESCAN_POST_HELLO_GRACE_MS) {
        return;
    }

    now_ms = k_uptime_get_32();
    pending_mask = (uint32_t)atomic_get(&g_notify_rescan_mask);
    for (uint16_t topic = 1U; topic <= 32U; topic++) {
        uint32_t bit = BIT(topic - 1U);
        uint32_t min_ms;

        if ((topic_mask & bit) == 0U) {
            continue;
        }

        if ((pending_mask & bit) != 0U) {
            filtered_mask |= bit;
            continue;
        }

        min_ms = MIN(MAX(topic_min_interval_ms(topic) / 2U, 10U), 250U);
        if ((now_ms - g_last_topic_rescan_req_ms[topic]) < min_ms) {
            continue;
        }
        g_last_topic_rescan_req_ms[topic] = now_ms;
        filtered_mask |= bit;
    }

    if (filtered_mask == 0U) {
        return;
    }

    (void)atomic_or(&g_notify_rescan_mask, (atomic_val_t)filtered_mask);
    k_sem_give(&g_ucp_wake_sem);
    if (g_poll_notifies_enabled) {
        (void)k_work_reschedule(&g_ucp_notify_work, K_MSEC(5));
    }
}

static void ucp_notify_work_handler(struct k_work *work)
{
    uint32_t now;
    ARG_UNUSED(work);

    if (!g_ready) {
        (void)k_work_reschedule(&g_ucp_notify_work, K_MSEC(500));
        return;
    }

    if (g_session && (g_sub_mask != 0U)) {
        if (g_poll_notifies_enabled) {
            uint32_t t0;
            uint32_t rescan_mask = (uint32_t)atomic_set(&g_notify_rescan_mask, 0);
            t0 = k_uptime_get_32();
            if (rescan_mask & BIT(UCP_TOPIC_TASK_CURRENT - 1U)) {
                maybe_task_status_event();
                if ((k_uptime_get_32() - t0) > 200U) {
                    printk("UCP UART warn: maybe_task_status_event slow: %u ms\n", k_uptime_get_32() - t0);
                }
            }

            t0 = k_uptime_get_32();
            maybe_domain_events();
            if ((k_uptime_get_32() - t0) > 300U) {
                printk("UCP UART warn: maybe_domain_events slow: %u ms sub_mask=0x%08x\n",
                       k_uptime_get_32() - t0, g_sub_mask);
            }
        }

        maybe_heartbeat();
        if (g_poll_notifies_enabled) {
            (void)k_work_reschedule(&g_ucp_notify_work, K_MSEC(50));
        } else {
            uint32_t elapsed;
            uint32_t delay_ms;
            now = k_uptime_get_32();
            elapsed = now - g_last_evt_ms;
            delay_ms = (elapsed >= UCP_HEARTBEAT_MS) ? 100U : (UCP_HEARTBEAT_MS - elapsed);
            if (delay_ms < 100U) delay_ms = 100U;
            (void)k_work_reschedule(&g_ucp_notify_work, K_MSEC(delay_ms));
        }
        return;
    }

    (void)k_work_reschedule(&g_ucp_notify_work, K_MSEC(UCP_NOTIFY_IDLE_NOSESSION_MS));
}

static void ucp_thread_entry(void *a, void *b, void *c)
{
    ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);

    /* Eagerly prime env/rain history caches so the first GET doesn't return BUSY */
    k_sleep(K_MSEC(500)); /* let history_flash_init() finish on main thread */
    if (ucp_env_hist_stats_cache_refresh()) {
        printk("env hist cache primed at startup\n");
    } else {
        printk("env hist cache prime FAILED at startup\n");
    }
    if (ucp_rain_hist_stats_cache_refresh()) {
        printk("rain hist cache primed at startup\n");
    } else {
        printk("rain hist cache prime FAILED at startup\n");
    }

    while (1) {
        if (g_ready && g_uart && device_is_ready(g_uart)) {
            uint32_t now_ms = k_uptime_get_32();
            bool can_publish = (g_session && (g_sub_mask != 0U));
            uint32_t rescan_mask = 0U;
            bool packs_need_refresh = false;
            bool env_hist_need_refresh = false;
            poll_rx();
            if (g_session && (k_uptime_get_32() - g_last_rx_ms) > UCP_SESSION_TIMEOUT_MS) {
                ucp_drop_session_state();
                g_diag.session_timeouts++;
            }
            maybe_stream_timeouts();
            packs_need_refresh = (atomic_get(&g_pack_stats_refresh_pending) != 0);
            /* Automatic stale refresh for packs stats is too expensive under active traffic.
             * Keep background refresh only in broad polling mode; otherwise refresh on-demand
             * via explicit request/flag paths.
             */
            if (!packs_need_refresh && g_poll_notifies_enabled && pack_storage_is_ready()) {
                bool packs_interest = topic_subscribed_global(UCP_TOPIC_PACKS_CHANGED);
                if (packs_interest && ucp_pack_stats_cache_is_stale(now_ms, UCP_PACK_STATS_CACHE_TTL_MS)) {
                    packs_need_refresh = true;
                }
            }
            if (packs_need_refresh && pack_storage_is_ready() &&
                ((now_ms - g_pack_stats_refresh_try_ms) >= UCP_PACK_STATS_REFRESH_MIN_INTERVAL_MS)) {
                g_pack_stats_refresh_try_ms = now_ms;
                if (ucp_pack_stats_cache_refresh()) {
                    (void)atomic_set(&g_pack_stats_refresh_pending, 0);
                }
            }

            env_hist_need_refresh = (atomic_get(&g_env_hist_stats_refresh_pending) != 0);
            /* Same policy as packs stats: only auto-refresh in broad polling mode.
             * Otherwise, refresh on-demand to protect request/response latency.
             */
            if (!env_hist_need_refresh && g_poll_notifies_enabled) {
                bool env_interest = topic_subscribed_global(UCP_TOPIC_ENV_LIVE) ||
                                    topic_subscribed_global(UCP_TOPIC_HISTORY_READY);
                if (env_interest && ucp_env_hist_stats_cache_is_stale(now_ms, UCP_ENV_HIST_STATS_CACHE_TTL_MS)) {
                    env_hist_need_refresh = true;
                }
            }
            if (env_hist_need_refresh &&
                ((now_ms - g_env_hist_stats_refresh_try_ms) >= UCP_ENV_HIST_STATS_REFRESH_MIN_INTERVAL_MS)) {
                g_env_hist_stats_refresh_try_ms = now_ms;
                if (ucp_env_hist_stats_cache_refresh()) {
                    (void)atomic_set(&g_env_hist_stats_refresh_pending, 0);
                }
            }

            /* Rain history stats deferred refresh (same pattern as env hist) */
            {
                bool rain_hist_need_refresh = (atomic_get(&g_rain_hist_stats_refresh_pending) != 0);
                if (!rain_hist_need_refresh && g_poll_notifies_enabled) {
                    bool rain_interest = topic_subscribed_global(UCP_TOPIC_HISTORY_READY);
                    if (rain_interest && ucp_rain_hist_stats_cache_is_stale(now_ms, UCP_RAIN_HIST_STATS_CACHE_TTL_MS)) {
                        rain_hist_need_refresh = true;
                    }
                }
                if (rain_hist_need_refresh &&
                    ((now_ms - g_rain_hist_stats_refresh_try_ms) >= UCP_RAIN_HIST_STATS_REFRESH_MIN_INTERVAL_MS)) {
                    g_rain_hist_stats_refresh_try_ms = now_ms;
                    if (ucp_rain_hist_stats_cache_refresh()) {
                        (void)atomic_set(&g_rain_hist_stats_refresh_pending, 0);
                    }
                }
            }
            if (can_publish && !g_poll_notifies_enabled &&
                ((k_uptime_get_32() - g_last_rx_ms) >= UCP_DEFERRED_RESCAN_RX_QUIET_MS)) {
                rescan_mask = (uint32_t)atomic_set(&g_notify_rescan_mask, 0);
                if (rescan_mask != 0U) {
                    uint32_t t0 = k_uptime_get_32();
                    uint32_t scoped_mask = g_sub_mask & rescan_mask;
                    if (rescan_mask & BIT(UCP_TOPIC_TASK_CURRENT - 1U)) {
                        maybe_task_status_event();
                        scoped_mask &= ~BIT(UCP_TOPIC_TASK_CURRENT - 1U);
                    }
                    while (scoped_mask != 0U) {
                        uint32_t bit = scoped_mask & (0U - scoped_mask); /* isolate lowest set bit */
                        bool requeue_same_bit = false;
                        scoped_mask &= ~bit;
                        if (bit == BIT(UCP_TOPIC_ONBOARDING_STATUS - 1U)) {
                            requeue_same_bit = deferred_rescan_onboarding_topic_step();
                        } else if (bit == BIT(UCP_TOPIC_SYSTEM_CONFIG - 1U)) {
                            deferred_rescan_system_config_topic();
                        } else if (bit == BIT(UCP_TOPIC_BULK_SNAPSHOT - 1U)) {
                            deferred_rescan_bulk_snapshot_topic();
                        } else if (bit == BIT(UCP_TOPIC_CHANNEL_CFG_STATUS - 1U)) {
                            requeue_same_bit = deferred_rescan_channel_cfg_status_topic_step();
                        } else if (bit == BIT(UCP_TOPIC_AUTO_CALC_STATUS - 1U)) {
                            requeue_same_bit = deferred_rescan_auto_calc_topic_step();
                        } else if (bit == BIT(UCP_TOPIC_COMP_STATUS - 1U)) {
                            requeue_same_bit = deferred_rescan_comp_status_topic_step();
                        } else {
                            g_topic_scope_mask_override = bit;
                            maybe_domain_events();
                            g_topic_scope_mask_override = 0U;
                        }

                        if (requeue_same_bit) {
                            (void)atomic_or(&g_notify_rescan_mask, (atomic_val_t)bit);
                            k_sem_give(&g_ucp_wake_sem);
                        }

                        if ((k_uptime_get_32() - t0) > UCP_DEFERRED_RESCAN_BUDGET_MS) {
                            if (scoped_mask != 0U) {
                                (void)atomic_or(&g_notify_rescan_mask, (atomic_val_t)scoped_mask);
                                k_sem_give(&g_ucp_wake_sem);
                            }
                            break;
                        }
                    }
                    if ((k_uptime_get_32() - t0) > 300U) {
                        printk("UCP UART warn: deferred rescan slow: %u ms mask=0x%08x sub_mask=0x%08x\n",
                               k_uptime_get_32() - t0, rescan_mask, g_sub_mask);
                    }
                }
            }
            if (!g_session && ((now_ms - g_dbg_uart_diag_last_ms) > 5000U)) {
                g_dbg_uart_diag_last_ms = now_ms;
                printk("UCP UART dbg: rx_bytes=%u accum=%u ok=%u cobs=%u crc=%u proto=%u overs=%u\n",
                       g_dbg_rx_bytes, (unsigned)g_rx_len, g_diag.rx_ok_frames, g_diag.rx_cobs_err,
                       g_diag.rx_crc_err, g_diag.rx_proto_err, g_diag.rx_oversize);
                if (g_uart_irq_rx_mode) {
                    printk("UCP UART dbg: irq_mode=1 ring_drop=%u\n", g_dbg_irq_drop_bytes);
                }
            }
            if ((now_ms - g_dbg_stack_diag_last_ms) > 30000U) {
                g_dbg_stack_diag_last_ms = now_ms;
                ucp_dbg_log_stack_watermark();
            }
            ARG_UNUSED(can_publish);
        }
        {
            uint32_t now_ms = k_uptime_get_32();
            uint32_t wait_ms = ucp_thread_wait_timeout_ms(now_ms);
            (void)k_sem_take(&g_ucp_wake_sem, K_MSEC(wait_ms));
        }
    }
}

bool ucp_uart_is_ready(void) { return g_ready; }

int ucp_uart_init(void)
{
    int ucfg_rc;
    if (g_ready) return 0;
    g_uart = DEVICE_DT_GET_OR_NULL(DT_NODELABEL(uart0));
    if (!g_uart || !device_is_ready(g_uart)) { printk("UCP UART: uart0 not ready, disabled\n"); return -ENODEV; }
    ring_buf_init(&g_rx_ring, sizeof(g_rx_ring_mem), g_rx_ring_mem);
    g_poll_notifies_enabled = false;
    g_topic_scope_mask_override = 0U;
    {
        const struct uart_config cfg = {
            .baudrate = UCP_UART_BAUD_DEFAULT,
            .parity = UART_CFG_PARITY_NONE,
            .stop_bits = UART_CFG_STOP_BITS_1,
            .data_bits = UART_CFG_DATA_BITS_8,
            .flow_ctrl = UART_CFG_FLOW_CTRL_NONE,
        };
        ucfg_rc = uart_configure(g_uart, &cfg);
        if (ucfg_rc == -ENOTSUP) {
            printk("UCP UART runtime configure: baud=%u rc=%d (driver no runtime reconfig; using DTS current-speed)\n",
                   UCP_UART_BAUD_DEFAULT, ucfg_rc);
        } else {
            printk("UCP UART runtime configure: baud=%u rc=%d\n", UCP_UART_BAUD_DEFAULT, ucfg_rc);
        }
    }
    {
        int irq_cb_rc = uart_irq_callback_user_data_set(g_uart, ucp_uart_irq_cb, NULL);
        if (irq_cb_rc == 0) {
            uart_irq_rx_enable(g_uart);
            g_uart_irq_rx_mode = true;
            printk("UCP UART RX mode: interrupt\n");
        } else {
            g_uart_irq_rx_mode = false;
            printk("UCP UART RX mode: polling (irq cb rc=%d)\n", irq_cb_rc);
        }
    }
    g_boot_id = ((uint64_t)sys_rand32_get() << 32) | sys_rand32_get();
    if (g_boot_id == 0ULL) g_boot_id = 1ULL;
    for (uint8_t i = 0U; i < WATERING_CHANNELS_COUNT; i++) {
        g_ver_channel_state[i] = 1U;
        g_ver_channel_cfg[i] = 1U;
        g_ver_schedule_cfg[i] = 1U;
        g_ver_stats[i] = 1U;
        g_ver_hydraulic[i] = 1U;
        g_ver_custom_soil[i] = 1U;
        g_ver_interval_cfg[i] = 1U;
        g_ver_cfg_status[i] = 1U;
        g_ver_comp_status[i] = 1U;
        g_ver_comp_cfg[i] = 1U;
    }
    g_ver_soil_moisture[0] = 1U;
    for (uint8_t i = 0U; i < WATERING_CHANNELS_COUNT; i++) {
        g_ver_soil_moisture[i + 1U] = 1U;
    }
#if UCP_HAVE_PEER_POWER_GPIO
    if (device_is_ready(g_peer_power_gpio.port) &&
        gpio_pin_configure_dt(&g_peer_power_gpio, GPIO_OUTPUT_ACTIVE) == 0) {
        g_peer_power_hw_backed = true;
        g_peer_power_on = true;
    }
#endif
    g_last_rx_ms = k_uptime_get_32(); g_last_evt_ms = g_last_rx_ms; g_ready = true;
    k_thread_create(&g_ucp_thread, g_ucp_stack, K_THREAD_STACK_SIZEOF(g_ucp_stack), ucp_thread_entry, NULL, NULL, NULL, K_PRIO_PREEMPT(10), 0, K_NO_WAIT);
    k_thread_name_set(&g_ucp_thread, "ucp_uart");
    (void)k_work_reschedule(&g_ucp_notify_work, K_MSEC(UCP_NOTIFY_IDLE_NOSESSION_MS));
    printk("UCP UART v1 initialized on uart0 (GET/SET/CMD/SUB + EVENT_CHANGE), peer_power_hw=%u\n", g_peer_power_hw_backed ? 1U : 0U);
    printk("UCP UART notify worker: async (system workqueue; transport thread RX-only); broad polling=%u\n", g_poll_notifies_enabled ? 1U : 0U);
    return 0;
}

int ucp_uart_publish_timezone_changed(const uint8_t *delta_tlv, size_t delta_len, ucp_origin_t origin)
{
    if (!g_ready) return -ENODEV;
    return emit_change(UCP_TOPIC_RTC_TZ_CHANGED, UCP_SVC_RTC_TZ, UCP_RES_TZ_CONFIG, ++g_ver_tz, origin, delta_tlv, delta_len);
}

int ucp_uart_publish_rtc_changed(const uint8_t *delta_tlv, size_t delta_len, ucp_origin_t origin)
{
    if (!g_ready) return -ENODEV;
    return emit_change(UCP_TOPIC_RTC_TZ_CHANGED, UCP_SVC_RTC_TZ, UCP_RES_RTC_TIME, ++g_ver_rtc, origin, delta_tlv, delta_len);
}

int ucp_uart_publish_system_status_changed(const uint8_t *delta_tlv, size_t delta_len, ucp_origin_t origin)
{
    struct ucp_system_snapshot s;
    if (!g_ready) return -ENODEV;
    if (collect_system_snapshot(&s) == 0) {
        g_sys_snap = s;
    }
    return emit_change(UCP_TOPIC_SYSTEM_STATUS, UCP_SVC_DEVICE, UCP_RES_DEVICE_SYSTEM_STATUS, ++g_ver_sys, origin, delta_tlv, delta_len);
}

int ucp_uart_publish_alarm(uint8_t alarm_code, uint16_t alarm_data, bool cleared, ucp_origin_t origin)
{
    uint8_t d[32];
    struct ucp_tlv_writer w;
    uint32_t now_ms;
    if (!g_ready) return -ENODEV;
    now_ms = k_uptime_get_32();
    k_mutex_lock(&g_ucp_evt_mutex, K_FOREVER);
    if ((now_ms - g_last_alarm_pub_ms) < UCP_ALARM_DEDUP_MS &&
        g_last_alarm_code == alarm_code &&
        g_last_alarm_data == alarm_data &&
        g_last_alarm_cleared == cleared) {
        k_mutex_unlock(&g_ucp_evt_mutex);
        return 0;
    }
    g_last_alarm_code = alarm_code;
    g_last_alarm_data = alarm_data;
    g_last_alarm_cleared = cleared;
    g_last_alarm_pub_ms = now_ms;
    k_mutex_unlock(&g_ucp_evt_mutex);
    ucp_tlvw_init(&w, d, sizeof(d));
    (void)ucp_tlvw_put_u8(&w, ALARM_CODE_F, UCP_TLV_U8, alarm_code);
    (void)ucp_tlvw_put_u16(&w, ALARM_DATA_F, UCP_TLV_U16, alarm_data);
    (void)ucp_tlvw_put_u32(&w, ALARM_TS_F, UCP_TLV_U32, timezone_get_unix_utc());
    (void)ucp_tlvw_put_u8(&w, ALARM_CLEARED_F, UCP_TLV_BOOL, cleared ? 1U : 0U);
    return emit_change(UCP_TOPIC_ALARM, UCP_SVC_DEVICE, UCP_RES_DEVICE_ALARM_EVENT, ++g_ver_alarm, origin, w.buf, w.len);
}

int ucp_uart_publish_valve_state(uint8_t channel_id, bool is_open, bool is_master, ucp_origin_t origin)
{
    uint8_t d[16];
    struct ucp_tlv_writer w;
    uint32_t instance_id = is_master ? 0xFFFFU : channel_id;
    uint32_t ver;
    if (!g_ready) return -ENODEV;
    if (!is_master && channel_id >= WATERING_CHANNELS_COUNT) return -EINVAL;
    ucp_tlvw_init(&w, d, sizeof(d));
    (void)ucp_tlvw_put_u8(&w, RES_CH_ID, UCP_TLV_U8, channel_id);
    (void)ucp_tlvw_put_u8(&w, RES_CH_ACTIVE, UCP_TLV_BOOL, is_open ? 1U : 0U);
    if (!is_master && channel_id < WATERING_CHANNELS_COUNT) {
        struct ucp_channel_state_snapshot s;
        if (collect_channel_state_snapshot(channel_id, &s) == 0) {
            g_ch_state_snap[channel_id] = s;
        } else {
            g_ch_state_snap[channel_id].is_active = is_open;
            g_ch_state_snap[channel_id].valid = true;
        }
        ver = ++g_ver_channel_state[channel_id];
    } else {
        ver = ++g_ver_sys;
    }
    return emit_change_instance(UCP_TOPIC_CHANNEL_STATE, UCP_SVC_CHANNEL, UCP_RES_CHANNEL_STATE,
                                instance_id, ver, origin, w.buf, w.len);
}

#endif /* CONFIG_AUTOWATERING_UCP_UART */
