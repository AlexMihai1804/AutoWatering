
#ifndef BT_GATT_STRUCTS_H
#define BT_GATT_STRUCTS_H

#include <zephyr/types.h>
#include <zephyr/sys/util.h> /* BUILD_ASSERT */

/* Max history payload bytes per fragment (header is 8B; total = 8 + payload <= 240B @ MTU=247). */
#define RAIN_HISTORY_FRAGMENT_SIZE 232

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
    uint8_t schedule_type; // 0=daily, 1=periodic, 2=auto (FAO-56 smart)
    uint8_t days_mask; // Days for daily schedule or interval days for periodic (ignored for auto)
    uint8_t hour;
    uint8_t minute;
    uint8_t watering_mode; // 0=duration, 1=volume
    uint16_t value; // Minutes or liters (auto mode calculates volume automatically)
    uint8_t auto_enabled; // 0=disabled, 1=enabled
    /* Solar timing fields (v2 extension) */
    uint8_t use_solar_timing;    // 0=use fixed time, 1=use sunrise/sunset
    uint8_t solar_event;         // 0=sunset, 1=sunrise
    int8_t solar_offset_minutes; // Offset from solar event (-120 to +120)
} __packed;

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
} __packed;

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
} __packed;

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
} __packed;

/* Structure for alarms and notifications */
struct alarm_data {
    uint8_t alarm_code; /* Alarm code */
    uint16_t alarm_data; /* Additional alarm-specific data */
    uint32_t timestamp; /* Timestamp when alarm occurred */
} __packed;

/* Structure for flow sensor calibration */
struct calibration_data {
    uint8_t action; /* 0=stop, 1=start, 2=in progress, 3=calculated */
    uint32_t pulses; /* Number of pulses counted */
    uint32_t volume_ml; /* Volume in ml (input or calculated) */
    uint32_t pulses_per_liter; /* Calibration result */
} __packed;

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
} __packed;

/* Structure for diagnostics */
struct diagnostics_data {
    uint32_t uptime; /* System uptime in minutes */
    uint16_t error_count; /* Total error count since boot */
    uint8_t last_error; /* Code of the most recent error (0 if no errors) */
    uint8_t valve_status; /* Valve status bitmap (bit 0 = channel 0, etc.) */
    uint8_t battery_level; /* Battery level in percent (0xFF if not applicable) */
    uint8_t reserved[3]; /* Reserved for future use */
} __packed;

/* Structure for enhanced growing environment configuration */
struct growing_env_data {
    uint8_t channel_id;           /* Channel ID (0-7) */
    
    /* Enhanced database indices */
    uint16_t plant_db_index;      /* Index into plant_full_database (0-based, UINT16_MAX = not set) */
    uint8_t soil_db_index;        /* Index into soil_enhanced_database (0-based, UINT8_MAX = not set) */
    uint8_t irrigation_method_index; /* Index into irrigation_methods_database (0-based, UINT8_MAX = not set) */
    
    /* Coverage specification */
    uint8_t use_area_based;       /* 1=area in m², 0=plant count */
    union {
        float area_m2;            /* Area in square meters */
        uint16_t plant_count;     /* Number of plants */
    } coverage;
    
    /* Automatic mode settings */
    uint8_t auto_mode;            /* 0=manual, 1=quality (100%), 2=eco (70%) */
    float max_volume_limit_l;     /* Maximum irrigation volume limit (liters) */
    uint8_t enable_cycle_soak;    /* Enable cycle and soak for clay soils */
    
    /* Plant lifecycle tracking */
    uint32_t planting_date_unix;  /* When plants were established (Unix timestamp) */
    uint16_t days_after_planting; /* Calculated field - days since planting */
    
    /* Environmental overrides */
    float latitude_deg;           /* Location latitude for solar calculations */
    uint8_t sun_exposure_pct;     /* Site-specific sun exposure (0-100%) */
    
    /* Legacy fields for backward compatibility */
    uint8_t plant_type;           /* Legacy plant type (0-7) */
    uint16_t specific_plant;      /* Legacy specific plant type */
    uint8_t soil_type;            /* Legacy soil type (0-7) */
    uint8_t irrigation_method;    /* Legacy irrigation method (0-5) */
    uint8_t sun_percentage;       /* Legacy sun exposure percentage (0-100) */
    
    /* Custom plant fields (legacy - used only when plant_type=7) */
    char custom_name[32];         /* CUSTOM PLANT NAME (32 bytes): Species name when plant_type=Custom */
    float water_need_factor;      /* Water need multiplier (0.1-5.0) */
    uint8_t irrigation_freq_days; /* Recommended irrigation frequency (days) */
    uint8_t prefer_area_based;    /* 1=plant prefers m² measurement, 0=prefers plant count */
    
    /* Pack storage custom plant (v3.1+) */
    uint16_t custom_plant_id;     /* Custom plant ID from pack storage (0 = use plant_db_index, >=1000 = custom) */
} __packed;

/* Structure for automatic calculation status */
struct auto_calc_status_data {
    uint8_t channel_id;               /* Channel ID (0-7) */
    uint8_t calculation_active;       /* 1 if automatic calculations are active, 0 if not */
    uint8_t irrigation_needed;        /* 1 if irrigation is needed based on calculations */
    float current_deficit_mm;         /* Current soil water deficit (mm) */
    float et0_mm_day;                 /* Reference evapotranspiration (mm/day) */
    float crop_coefficient;           /* Current crop coefficient (Kc) */
    float net_irrigation_mm;          /* Net irrigation requirement (mm) */
    float gross_irrigation_mm;        /* Gross irrigation with losses (mm) */
    float calculated_volume_l;        /* Calculated irrigation volume (liters) */
    uint32_t last_calculation_time;   /* Last calculation timestamp (Unix) */
    uint32_t next_irrigation_time;    /* Next scheduled irrigation (Unix) */
    uint16_t days_after_planting;     /* Days since planting */
    uint8_t phenological_stage;       /* Current growth stage (0-3) */
    uint8_t quality_mode;             /* 0=manual, 1=quality, 2=eco */
    uint8_t volume_limited;           /* 1 if volume was limited by max constraint */
    
    /* Additional fields used by bt_irrigation_service.c */
    uint8_t auto_mode;                /* Automatic mode setting */
    float raw_mm;                     /* Raw rainfall amount */
    float effective_rain_mm;          /* Effective rainfall amount */
    uint8_t calculation_error;        /* Calculation error flag */
    float etc_mm_day;                 /* Crop evapotranspiration */
    float volume_liters;              /* Volume in liters */
    uint8_t cycle_count;              /* Number of cycles */
    uint8_t cycle_duration_min;       /* Cycle duration in minutes */
    uint8_t reserved[4];              /* Reserved for alignment */
} __packed;

/* Rain configuration data structure (18B per BLE spec) */
struct rain_config_data {
    float mm_per_pulse;               /* Millimeters per sensor pulse */
    uint16_t debounce_ms;             /* Debounce time in milliseconds */
    uint8_t sensor_enabled;           /* Whether rain sensor is enabled */
    uint8_t integration_enabled;      /* Whether rain integration is enabled */
    float rain_sensitivity_pct;       /* Rain sensitivity percentage (0-100) */
    float skip_threshold_mm;          /* Rain threshold to skip watering (mm) */
    uint8_t reserved[2];              /* Reserved for future use */
} __packed;

/* Rain data structure (24 bytes, matches BLE doc 19) */
struct rain_data_data {
    uint32_t current_hour_mm_x100;    /* Current hour rainfall ×100 (0.01mm precision) */
    uint32_t today_total_mm_x100;     /* Today's total rainfall ×100 */
    uint32_t last_24h_mm_x100;        /* Last 24h rainfall ×100 */
    uint16_t current_rate_mm_h_x100;  /* Current rate mm/h ×100 */
    uint32_t last_pulse_time;         /* Last pulse timestamp (Unix epoch) */
    uint32_t total_pulses;            /* Total pulse count since reset */
    uint8_t sensor_status;            /* 0=inactive, 1=active, 2=error */
    uint8_t data_quality;             /* Data quality 0-100% */
} __packed;

/* Rain history command data structure (16 bytes) */
struct rain_history_cmd_data {
    uint8_t  command;                 /* Command type */
    uint32_t start_timestamp;         /* Start time for history query (0 = from earliest) */
    uint32_t end_timestamp;           /* End time for history query (0 = until now) */
    uint16_t max_entries;             /* Maximum entries to return */
    uint8_t  data_type;               /* 0=hourly, 1=daily, 0xFE=recent totals */
    uint8_t  reserved[4];             /* Reserved (set to 0) */
} __packed;                           /* Total: 16 bytes */


/* Unified history fragmentation header */
typedef struct {
    uint8_t data_type;         /* Data type: 0=hourly, 1=daily, 2=monthly, etc. */
    uint8_t status;            /* Status: 0=OK, nonzero=error */
    uint16_t entry_count;      /* Number of entries in this response */
    uint8_t fragment_index;    /* Index of this fragment */
    uint8_t total_fragments;   /* Total number of fragments */
    uint8_t fragment_size;     /* Size of this fragment's data payload */
    uint8_t reserved;          /* Reserved for alignment */
} __packed history_fragment_header_t;

/* Rain history response structure (unified) */
typedef struct {
    history_fragment_header_t header;
    uint8_t data[RAIN_HISTORY_FRAGMENT_SIZE]; /* Fragment data */
} rain_history_response_t;

struct lifecycle_config_data {
    uint8_t channel_id;           /* Channel ID (0-7) */
    uint32_t planting_date_unix;  /* When plants were established (Unix timestamp) */
    float latitude_deg;           /* Location latitude for solar calculations */
    uint8_t sun_exposure_pct;     /* Site-specific sun exposure (0-100%) */
} __packed;

struct growing_env_config_data {
    uint8_t channel_id;
    uint16_t plant_db_index;
    uint8_t soil_db_index;
    uint8_t irrigation_method_index;
    uint8_t use_area_based;
    union {
        float area_m2;
        uint16_t plant_count;
    } coverage;
    uint8_t auto_mode;
    float max_volume_limit_l;
    uint8_t enable_cycle_soak;
} __packed;

/* Onboarding status structure (layout must match BLE doc offsets; extended flags stay last) */
struct onboarding_status_data {
    uint8_t overall_completion_pct;     /* 0-100% overall completion */
    uint8_t channels_completion_pct;    /* 0-100% channel config completion */
    uint8_t system_completion_pct;      /* 0-100% system config completion */
    uint8_t schedules_completion_pct;   /* 0-100% schedule completion */
    
    uint64_t channel_config_flags;      /* Channel configuration flags (basic) */
    uint32_t system_config_flags;       /* System configuration flags */
    uint8_t schedule_config_flags;      /* Schedule configuration flags */
    
    uint32_t onboarding_start_time;     /* When onboarding began */
    uint32_t last_update_time;          /* Last state update */
    uint64_t channel_extended_flags;    /* Channel extended flags (FAO56, rain/temp comp) */
} __packed;

/* Reset control structure
 * Status byte (wipe_state_t):
 *   0x00 = IDLE (no operation pending)
 *   0x01 = AWAIT_CONFIRM (confirmation code valid, waiting for write)
 *   0x02 = IN_PROGRESS (factory wipe executing step-by-step)
 *   0x03 = DONE_OK (wipe completed successfully)
 *   0x04 = DONE_ERROR (wipe failed, check last_error)
 *
 * Reserved bytes carry wipe progress when status >= 0x02:
 *   reserved[0] = progress_pct (0-100)
 *   reserved[1] = current_step (wipe_step_t)
 *   reserved[2] = attempt_count (retries for current step)
 *   reserved[3..4] = last_error (uint16_t LE, 0 = no error)
 */
struct reset_control_data {
    uint8_t reset_type;                 /* Type of reset to perform (reset_type_t) */
    uint8_t channel_id;                 /* Channel ID for channel-specific resets */
    uint32_t confirmation_code;         /* Required confirmation code */
    uint8_t status;                     /* Wipe state (wipe_state_t) */
    uint32_t timestamp;                 /* When reset was performed */
    uint8_t reserved[5];                /* [0]=progress%, [1]=step, [2]=retries, [3..4]=error LE */
} __packed;

/* Rain Integration Status structure for BLE (packed, snapshot + notify) */
struct rain_integration_status_ble {
    uint8_t  sensor_active;                  /* 0/1 */
    uint8_t  integration_enabled;            /* 0/1 */
    uint32_t last_pulse_time;                /* Unix seconds */
    float    calibration_mm_per_pulse;       /* mm per pulse */
    float    rainfall_last_hour;             /* mm */
    float    rainfall_last_24h;              /* mm */
    float    rainfall_last_48h;              /* mm */
    float    sensitivity_pct;                /* 0-100 */
    float    skip_threshold_mm;              /* mm */
    float    channel_reduction_pct[8];       /* Per-channel reduction % */
    uint8_t  channel_skip_irrigation[8];     /* Per-channel skip flag */
    uint16_t hourly_entries;                 /* Hourly history entries */
    uint16_t daily_entries;                  /* Daily history entries */
    uint32_t storage_usage_bytes;            /* Storage usage */
} __packed;                                   /* Total: 78 bytes */

/**
 * @brief Per-channel compensation configuration for BLE
 * 
 * Exposes rain and temperature compensation settings for individual channels.
 * This allows mobile apps to configure per-channel thresholds instead of
 * relying only on global defaults from Rain Sensor Config (#18).
 * 
 * NOTE: Compensation only applies to TIME and VOLUME watering modes.
 * FAO-56 modes (AUTO_QUALITY, AUTO_ECO) already incorporate weather data
 * in their calculations; applying compensation would double-count.
 */
struct channel_compensation_config_data {
    uint8_t channel_id;                      /* Channel ID (0-7) */
    
    /* Rain compensation settings */
    uint8_t rain_enabled;                    /* 0=disabled, 1=enabled */
    float   rain_sensitivity;                /* Sensitivity factor (0.0-1.0) */
    uint16_t rain_lookback_hours;            /* Hours to look back for rain data (1-72) */
    float   rain_skip_threshold_mm;          /* Rain threshold to skip watering (0-100mm) */
    float   rain_reduction_factor;           /* Duration/volume reduction factor (0.0-1.0) */
    
    /* Temperature compensation settings */
    uint8_t temp_enabled;                    /* 0=disabled, 1=enabled */
    float   temp_base_temperature;           /* Base temperature for calculations (°C) */
    float   temp_sensitivity;                /* Temperature sensitivity factor (0.1-2.0) */
    float   temp_min_factor;                 /* Minimum compensation factor (0.5-1.0) */
    float   temp_max_factor;                 /* Maximum compensation factor (1.0-2.0) */
    
    /* Status fields (read-only, updated by firmware) */
    uint32_t last_rain_calc_time;            /* Last rain compensation calculation (Unix) */
    uint32_t last_temp_calc_time;            /* Last temp compensation calculation (Unix) */
    
    uint8_t reserved[3];                     /* Reserved for future use */
} __packed;                                   /* Total: 44 bytes */

/* Compile-time verification of BLE struct sizes (must stay in sync with docs) */
BUILD_ASSERT(sizeof(struct rain_config_data) == 18, "rain_config_data must be 18 bytes");
BUILD_ASSERT(sizeof(struct rain_data_data) == 24, "rain_data_data must be 24 bytes");
BUILD_ASSERT(sizeof(struct rain_history_cmd_data) == 16, "rain_history_cmd_data must be 16 bytes");
BUILD_ASSERT(sizeof(history_fragment_header_t) == 8, "history_fragment_header_t must be 8 bytes");
BUILD_ASSERT(sizeof(struct rain_integration_status_ble) == 78, "rain_integration_status_ble must be 78 bytes");
BUILD_ASSERT(sizeof(struct channel_compensation_config_data) == 44, "channel_compensation_config_data must be 44 bytes");
BUILD_ASSERT(sizeof(struct growing_env_data) == 73, "growing_env_data must be 73 bytes");
BUILD_ASSERT(sizeof(struct auto_calc_status_data) == 64, "auto_calc_status_data must be 64 bytes");
BUILD_ASSERT(sizeof(struct onboarding_status_data) == 33, "onboarding_status_data must be 33 bytes");
/* Newly audited: ensure reset control struct stays at 16 bytes */
BUILD_ASSERT(sizeof(struct reset_control_data) == 16, "reset_control_data must be 16 bytes");

/* Additional size guards (recently audited) */
BUILD_ASSERT(sizeof(struct valve_control_data) == 4, "valve_control_data must be 4 bytes");
BUILD_ASSERT(sizeof(struct channel_config_data) == 76, "channel_config_data must be 76 bytes");
BUILD_ASSERT(sizeof(struct schedule_config_data) == 12, "schedule_config_data unexpected size (9->12 with solar timing)");
BUILD_ASSERT(sizeof(struct task_queue_data) == 9, "task_queue_data unexpected size");
BUILD_ASSERT(sizeof(struct statistics_data) == 15, "statistics_data unexpected size");
BUILD_ASSERT(sizeof(struct current_task_data) == 21, "current_task_data unexpected size");
BUILD_ASSERT(sizeof(struct rtc_data) == 16, "rtc_data unexpected size");
BUILD_ASSERT(sizeof(struct alarm_data) == 7, "alarm_data unexpected size");
BUILD_ASSERT(sizeof(struct calibration_data) == 13, "calibration_data unexpected size");
BUILD_ASSERT(sizeof(struct diagnostics_data) == 12, "diagnostics_data unexpected size");
BUILD_ASSERT(sizeof(struct rain_config_data) == 18, "rain_config_data mismatch"); /* duplicate guard acceptable */
BUILD_ASSERT(sizeof(struct rain_data_data) == 24, "rain_data_data mismatch");
BUILD_ASSERT(sizeof(struct rain_history_cmd_data) == 16, "rain_history_cmd_data mismatch");
BUILD_ASSERT(sizeof(struct history_data) <= 64, "history_data base struct header unexpectedly grew");

#endif // BT_GATT_STRUCTS_H
