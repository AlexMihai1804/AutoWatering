#include "reset_controller.h"
#include "onboarding_state.h"
#include "nvs_config.h"
#include "timezone.h"
#include "watering.h"
#include "watering_internal.h"
#include "water_balance_types.h"
#include "rain_history.h"
#include "rain_sensor.h"
#include "rain_config.h"
#include "rain_compensation.h"

#ifdef CONFIG_BT
#include "bt_irrigation_service.h"
#endif
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/random/random.h>
#include <string.h>
#include <errno.h>

/**
 * @file reset_controller.c
 * @brief Implementation of reset controller for AutoWatering system
 * 
 * This module provides comprehensive reset capabilities with confirmation
 * code validation for safe reset operations during onboarding.
 */

/* Forward declarations for individual reset operations */
static int reset_controller_reset_channel_config(uint8_t channel_id);
static int reset_controller_reset_channel_schedule(uint8_t channel_id);
static int reset_controller_reset_all_channels(void);
static int reset_controller_reset_all_schedules(void);
static int reset_controller_reset_system_config(void);
static int reset_controller_reset_calibration(void);
static int reset_controller_reset_history(void);
static int reset_controller_factory_reset(void);

/* Global reset controller state */
static reset_confirmation_t current_confirmation;
static bool controller_initialized = false;

/* Mutex for thread safety */
K_MUTEX_DEFINE(reset_controller_mutex);

static void reset_controller_build_default_channel(uint8_t channel_id,
                                                  watering_channel_t *out_channel,
                                                  struct water_balance_t *existing_balance)
{
    if (!out_channel) {
        return;
    }

    memset(out_channel, 0, sizeof(*out_channel));

    /* Default name: per reset spec, wipe name */
    out_channel->name[0] = '\0';

    /* Default schedule: disabled but with sane example values */
    out_channel->watering_event.auto_enabled = false;
    out_channel->watering_event.schedule_type = SCHEDULE_DAILY;
    out_channel->watering_event.schedule.daily.days_of_week = 0x3E; /* Mon-Fri */
    out_channel->watering_event.start_time.hour = 7;
    out_channel->watering_event.start_time.minute = 0;
    out_channel->watering_event.watering_mode = WATERING_BY_DURATION;
    out_channel->watering_event.watering.by_duration.duration_minutes = 5;

    /* Legacy defaults */
    out_channel->plant_type = PLANT_TYPE_VEGETABLES;
    out_channel->plant_info.main_type = PLANT_TYPE_VEGETABLES;
    out_channel->plant_info.specific.vegetable = VEGETABLE_TOMATOES;
    out_channel->soil_type = SOIL_TYPE_LOAMY;
    out_channel->irrigation_method = IRRIGATION_DRIP;
    out_channel->sun_percentage = 75;

    /* Enhanced defaults aligned with DEFAULT_ENHANCED_CHANNEL_CONFIG */
    out_channel->use_area_based = true;
    out_channel->coverage.area_m2 = 1.0f;
    out_channel->sun_exposure_pct = 75;
    out_channel->auto_mode = WATERING_BY_DURATION; /* disabled/manual */
    out_channel->max_volume_limit_l = 10.0f;
    out_channel->enable_cycle_soak = false;
    out_channel->planting_date_unix = 0;
    out_channel->days_after_planting = 0;
    out_channel->latitude_deg = 0.0f;
    out_channel->longitude_deg = 0.0f;
    out_channel->last_calculation_time = 0;
    out_channel->last_auto_check_julian_day = 0;
    out_channel->auto_check_ran_today = false;

    /* Database indexes: sentinel values mean "not configured" */
    out_channel->plant_db_index = UINT16_MAX;
    out_channel->soil_db_index = UINT8_MAX;
    out_channel->irrigation_method_index = UINT8_MAX;

    /* Custom plant defaults */
    memset(&out_channel->custom_plant, 0, sizeof(out_channel->custom_plant));
    out_channel->custom_plant.water_need_factor = 1.0f;
    out_channel->custom_plant.irrigation_freq = 3;
    out_channel->custom_plant.prefer_area_based = true;

    /* Compensation defaults */
    out_channel->rain_compensation.enabled = false;
    out_channel->rain_compensation.sensitivity = 0.75f;
    out_channel->rain_compensation.lookback_hours = 24;
    out_channel->rain_compensation.skip_threshold_mm = 5.0f;
    out_channel->rain_compensation.reduction_factor = 0.5f;

    out_channel->temp_compensation.enabled = false;
    out_channel->temp_compensation.base_temperature = 20.0f;
    out_channel->temp_compensation.sensitivity = 0.05f;
    out_channel->temp_compensation.min_factor = 0.5f;
    out_channel->temp_compensation.max_factor = 1.5f;

    /* Soil config defaults */
    out_channel->soil_config.use_custom_soil = false;
    memset(&out_channel->soil_config.custom, 0, sizeof(out_channel->soil_config.custom));

    /* Interval defaults */
    out_channel->interval_config.configured = false;
    memset(&out_channel->interval_config_shadow, 0, sizeof(out_channel->interval_config_shadow));

    /* Hydraulic monitoring defaults */
    out_channel->hydraulic.nominal_flow_ml_min = 0;
    out_channel->hydraulic.ramp_up_time_sec = 0;
    out_channel->hydraulic.profile_type = PROFILE_AUTO;
    out_channel->hydraulic.tolerance_high_percent = 30;
    out_channel->hydraulic.tolerance_low_percent = 40;
    out_channel->hydraulic.is_calibrated = false;
    out_channel->hydraulic.monitoring_enabled = true;
    out_channel->hydraulic.learning_runs = 0;
    out_channel->hydraulic.stable_runs = 0;
    out_channel->hydraulic.estimated = false;

    out_channel->hydraulic_lock.level = HYDRAULIC_LOCK_NONE;
    out_channel->hydraulic_lock.reason = HYDRAULIC_LOCK_REASON_NONE;
    out_channel->hydraulic_lock.locked_at_epoch = 0;
    out_channel->hydraulic_lock.retry_after_epoch = 0;

    out_channel->hydraulic_anomaly.no_flow_runs = 0;
    out_channel->hydraulic_anomaly.high_flow_runs = 0;
    out_channel->hydraulic_anomaly.unexpected_flow_runs = 0;
    out_channel->hydraulic_anomaly.last_anomaly_epoch = 0;

    /* Water balance defaults (keep pointer stable if already allocated) */
    out_channel->water_balance = existing_balance;
    if (out_channel->water_balance) {
        water_balance_config_t def = DEFAULT_WATER_BALANCE_CONFIG;
        out_channel->water_balance->rwz_awc_mm = def.rwz_awc_mm;
        out_channel->water_balance->wetting_awc_mm = def.wetting_awc_mm;
        out_channel->water_balance->raw_mm = def.raw_mm;
        out_channel->water_balance->current_deficit_mm = def.current_deficit_mm;
        out_channel->water_balance->effective_rain_mm = def.effective_rain_mm;
        out_channel->water_balance->irrigation_needed = def.irrigation_needed;
        out_channel->water_balance->last_update_time = def.last_update_time;
    }

    /* Reset runtime status snapshots */
    out_channel->last_rain_compensation.reduction_percentage = 0.0f;
    out_channel->last_rain_compensation.skip_watering = false;
    out_channel->last_temp_compensation.compensation_factor = 1.0f;
    out_channel->last_temp_compensation.adjusted_requirement = 0.0f;

    /* Reset config status bookkeeping */
    memset(&out_channel->config_status, 0, sizeof(out_channel->config_status));
    out_channel->config_status.last_reset_timestamp = timezone_get_unix_utc();

    (void)channel_id; /* channel_id currently only used for caller context */
}

/* Helper function to get current timestamp */
static uint32_t get_current_timestamp(void) {
    return k_uptime_get_32() / 1000; /* Convert to seconds */
}

/* Helper function to generate random confirmation code */
static uint32_t generate_random_code(void) {
    uint32_t code;
    sys_rand_get(&code, sizeof(code));
    
    /* Ensure code is not 0 (reserved for invalid) */
    if (code == 0) {
        code = 1;
    }
    
    return code;
}

int reset_controller_init(void) {
    k_mutex_lock(&reset_controller_mutex, K_FOREVER);
    
    /* Initialize confirmation structure */
    memset(&current_confirmation, 0, sizeof(current_confirmation));
    current_confirmation.is_valid = false;
    
    controller_initialized = true;
    
    k_mutex_unlock(&reset_controller_mutex);
    
    printk("Reset controller initialized\n");
    return 0;
}

uint32_t reset_controller_generate_confirmation_code(reset_type_t type, uint8_t channel_id) {
    if (!controller_initialized) {
        return 0;
    }
    
    /* Validate parameters */
    if (type >= 8) { /* Assuming 8 reset types defined */
        return 0;
    }
    
    if (reset_controller_requires_channel_id(type) && channel_id >= 8) {
        return 0;
    }
    
    k_mutex_lock(&reset_controller_mutex, K_FOREVER);
    
    /* Generate new confirmation code */
    uint32_t timestamp = get_current_timestamp();
    uint32_t code = generate_random_code();
    
    /* Update confirmation structure */
    current_confirmation.code = code;
    current_confirmation.type = type;
    current_confirmation.channel_id = channel_id;
    current_confirmation.generation_time = timestamp;
    current_confirmation.expiry_time = timestamp + RESET_CONFIRMATION_VALIDITY_SEC;
    current_confirmation.is_valid = true;
    
    k_mutex_unlock(&reset_controller_mutex);
    
    printk("Generated confirmation code 0x%08x for reset type %d, channel %d\n", 
           code, type, channel_id);
    
    return code;
}

bool reset_controller_validate_confirmation_code(uint32_t code, reset_type_t type, uint8_t channel_id) {
    if (!controller_initialized || code == 0) {
        return false;
    }
    
    k_mutex_lock(&reset_controller_mutex, K_FOREVER);
    
    bool valid = false;
    uint32_t current_time = get_current_timestamp();
    
    /* Check if confirmation is valid and not expired */
    if (current_confirmation.is_valid && 
        current_confirmation.code == code &&
        current_confirmation.type == type &&
        current_time <= current_confirmation.expiry_time) {
        
        /* For channel-specific resets, also check channel ID */
        if (reset_controller_requires_channel_id(type)) {
            valid = (current_confirmation.channel_id == channel_id);
        } else {
            valid = true;
        }
    }
    
    k_mutex_unlock(&reset_controller_mutex);
    
    return valid;
}

reset_status_t reset_controller_execute(const reset_request_t *request) {
    if (!controller_initialized || !request) {
        return RESET_STATUS_SYSTEM_ERROR;
    }
    
    /* Validate reset type */
    if (request->type >= 8) { /* Assuming 8 reset types defined */
        return RESET_STATUS_INVALID_TYPE;
    }
    
    /* Validate channel ID for channel-specific resets */
    if (reset_controller_requires_channel_id(request->type) && request->channel_id >= 8) {
        return RESET_STATUS_INVALID_CHANNEL;
    }
    
    /* Validate confirmation code */
    if (!reset_controller_validate_confirmation_code(request->confirmation_code, 
                                                    request->type, 
                                                    request->channel_id)) {
        uint32_t current_time = get_current_timestamp();
        
        k_mutex_lock(&reset_controller_mutex, K_FOREVER);
        bool expired = (current_confirmation.is_valid && 
                       current_time > current_confirmation.expiry_time);
        k_mutex_unlock(&reset_controller_mutex);
        
        return expired ? RESET_STATUS_CODE_EXPIRED : RESET_STATUS_INVALID_CODE;
    }
    
    /* Execute the reset operation */
    reset_status_t status = RESET_STATUS_SUCCESS;
    int ret;
    
    switch (request->type) {
        case RESET_TYPE_CHANNEL_CONFIG:
            ret = reset_controller_reset_channel_config(request->channel_id);
            break;
            
        case RESET_TYPE_CHANNEL_SCHEDULE:
            ret = reset_controller_reset_channel_schedule(request->channel_id);
            break;
            
        case RESET_TYPE_ALL_CHANNELS:
            ret = reset_controller_reset_all_channels();
            break;
            
        case RESET_TYPE_ALL_SCHEDULES:
            ret = reset_controller_reset_all_schedules();
            break;
            
        case RESET_TYPE_SYSTEM_CONFIG:
            ret = reset_controller_reset_system_config();
            break;
            
        case RESET_TYPE_CALIBRATION:
            ret = reset_controller_reset_calibration();
            break;
            
        case RESET_TYPE_HISTORY:
            ret = reset_controller_reset_history();
            break;
            
        case RESET_TYPE_FACTORY_RESET:
            ret = reset_controller_factory_reset();
            break;
            
        default:
            return RESET_STATUS_INVALID_TYPE;
    }
    
    if (ret < 0) {
        status = RESET_STATUS_STORAGE_ERROR;
    }
    
    /* Clear the confirmation code after use */
    reset_controller_clear_confirmation_code();
    
    printk("Reset operation type %d completed with status %d\n", request->type, status);
    
    return status;
}

int reset_controller_get_confirmation_info(reset_confirmation_t *confirmation) {
    if (!controller_initialized || !confirmation) {
        return -EINVAL;
    }
    
    k_mutex_lock(&reset_controller_mutex, K_FOREVER);
    *confirmation = current_confirmation;
    k_mutex_unlock(&reset_controller_mutex);
    
    return 0;
}

int reset_controller_clear_confirmation_code(void) {
    if (!controller_initialized) {
        return -ENODEV;
    }
    
    k_mutex_lock(&reset_controller_mutex, K_FOREVER);
    current_confirmation.is_valid = false;
    current_confirmation.code = 0;
    k_mutex_unlock(&reset_controller_mutex);
    
    return 0;
}

bool reset_controller_requires_channel_id(reset_type_t type) {
    switch (type) {
        case RESET_TYPE_CHANNEL_CONFIG:
        case RESET_TYPE_CHANNEL_SCHEDULE:
            return true;
            
        case RESET_TYPE_ALL_CHANNELS:
        case RESET_TYPE_ALL_SCHEDULES:
        case RESET_TYPE_SYSTEM_CONFIG:
        case RESET_TYPE_CALIBRATION:
        case RESET_TYPE_HISTORY:
        case RESET_TYPE_FACTORY_RESET:
            return false;
            
        default:
            return false;
    }
}

const char* reset_controller_get_type_description(reset_type_t type) {
    switch (type) {
        case RESET_TYPE_CHANNEL_CONFIG:
            return "Channel Configuration Reset";
        case RESET_TYPE_CHANNEL_SCHEDULE:
            return "Channel Schedule Reset";
        case RESET_TYPE_ALL_CHANNELS:
            return "All Channels Reset";
        case RESET_TYPE_ALL_SCHEDULES:
            return "All Schedules Reset";
        case RESET_TYPE_SYSTEM_CONFIG:
            return "System Configuration Reset";
        case RESET_TYPE_CALIBRATION:
            return "Calibration Data Reset";
        case RESET_TYPE_HISTORY:
            return "History Data Reset";
        case RESET_TYPE_FACTORY_RESET:
            return "Factory Reset";
        default:
            return "Unknown Reset Type";
    }
}

const char* reset_controller_get_status_description(reset_status_t status) {
    switch (status) {
        case RESET_STATUS_SUCCESS:
            return "Reset completed successfully";
        case RESET_STATUS_INVALID_TYPE:
            return "Invalid reset type";
        case RESET_STATUS_INVALID_CHANNEL:
            return "Invalid channel ID";
        case RESET_STATUS_INVALID_CODE:
            return "Invalid confirmation code";
        case RESET_STATUS_CODE_EXPIRED:
            return "Confirmation code expired";
        case RESET_STATUS_STORAGE_ERROR:
            return "Storage error during reset";
        case RESET_STATUS_SYSTEM_ERROR:
            return "System error";
        default:
            return "Unknown status";
    }
}

/* Individual reset operation implementations */

static int reset_controller_reset_channel_config(uint8_t channel_id) {
    /* First, clear ALL channel flags before saving any config */
    /* This ensures flags are reset even if nvs_save_* functions update them */
    onboarding_update_channel_flag(channel_id, CHANNEL_FLAG_PLANT_TYPE_SET, false);
    onboarding_update_channel_flag(channel_id, CHANNEL_FLAG_SOIL_TYPE_SET, false);
    onboarding_update_channel_flag(channel_id, CHANNEL_FLAG_IRRIGATION_METHOD_SET, false);
    onboarding_update_channel_flag(channel_id, CHANNEL_FLAG_COVERAGE_SET, false);
    onboarding_update_channel_flag(channel_id, CHANNEL_FLAG_SUN_EXPOSURE_SET, false);
    onboarding_update_channel_flag(channel_id, CHANNEL_FLAG_NAME_SET, false);
    onboarding_update_channel_flag(channel_id, CHANNEL_FLAG_WATER_FACTOR_SET, false);
    onboarding_update_channel_flag(channel_id, CHANNEL_FLAG_ENABLED, false);
    
    /* Also clear extended flags for this channel */
    onboarding_update_channel_extended_flag(channel_id, CHANNEL_EXT_FLAG_FAO56_READY, false);
    onboarding_update_channel_extended_flag(channel_id, CHANNEL_EXT_FLAG_RAIN_COMP_SET, false);
    onboarding_update_channel_extended_flag(channel_id, CHANNEL_EXT_FLAG_TEMP_COMP_SET, false);
    onboarding_update_channel_extended_flag(channel_id, CHANNEL_EXT_FLAG_CONFIG_COMPLETE, false);
    onboarding_update_channel_extended_flag(channel_id, CHANNEL_EXT_FLAG_LATITUDE_SET, false);
    
    /* Reset COMPLETE channel configuration (RAM + NVS) */
    struct water_balance_t *existing_balance = watering_channels[channel_id].water_balance;
    watering_channel_t default_channel;
    reset_controller_build_default_channel(channel_id, &default_channel, existing_balance);

    /* Apply to live RAM immediately (fixes "needs reboot after reset") */
    watering_channels[channel_id] = default_channel;

    /* Persist full channel struct so reboot won't resurrect old fields */
    int ret = nvs_save_complete_channel_config(channel_id, &watering_channels[channel_id]);
    if (ret < 0) {
        return ret;
    }

    /* Ensure water balance is reset even if pointer is NULL */
    water_balance_config_t default_balance = DEFAULT_WATER_BALANCE_CONFIG;
    ret = nvs_save_water_balance_config(channel_id, &default_balance);
    if (ret < 0) {
        return ret;
    }

    /* Clear any reset logs for this channel */
    (void)nvs_clear_config_reset_log(channel_id);

    /* Also clear rain compensation statistics for this channel */
    (void)rain_compensation_reset_statistics(channel_id);
    
    /* Re-clear flags after save operations to ensure they stay cleared */
    /* (in case nvs_save_* functions updated them based on saved values) */
    onboarding_update_channel_flag(channel_id, CHANNEL_FLAG_PLANT_TYPE_SET, false);
    onboarding_update_channel_flag(channel_id, CHANNEL_FLAG_SOIL_TYPE_SET, false);
    onboarding_update_channel_flag(channel_id, CHANNEL_FLAG_IRRIGATION_METHOD_SET, false);
    onboarding_update_channel_flag(channel_id, CHANNEL_FLAG_COVERAGE_SET, false);
    onboarding_update_channel_flag(channel_id, CHANNEL_FLAG_SUN_EXPOSURE_SET, false);
    onboarding_update_channel_flag(channel_id, CHANNEL_FLAG_NAME_SET, false);
    onboarding_update_channel_flag(channel_id, CHANNEL_FLAG_WATER_FACTOR_SET, false);
    onboarding_update_channel_flag(channel_id, CHANNEL_FLAG_ENABLED, false);
    
    printk("Channel %d configuration reset to defaults\n", channel_id);

#ifdef CONFIG_BT
    /* Push fresh values to BLE subscribers */
    (void)bt_irrigation_channel_config_update(channel_id);
    (void)bt_irrigation_schedule_update(channel_id);
    (void)bt_irrigation_channel_comp_config_notify(channel_id);
    (void)bt_irrigation_hydraulic_status_notify(channel_id);
#endif
    return 0;
}

static int reset_controller_reset_channel_schedule(uint8_t channel_id) {
    /* Reset schedule configuration in RAM and persist it */
    watering_channels[channel_id].watering_event.auto_enabled = false;
    watering_channels[channel_id].watering_event.schedule_type = SCHEDULE_DAILY;
    watering_channels[channel_id].watering_event.schedule.daily.days_of_week = 0x3E; /* Mon-Fri */
    watering_channels[channel_id].watering_event.start_time.hour = 7;
    watering_channels[channel_id].watering_event.start_time.minute = 0;
    watering_channels[channel_id].watering_event.watering_mode = WATERING_BY_DURATION;
    watering_channels[channel_id].watering_event.watering.by_duration.duration_minutes = 5;

    int ret = nvs_save_complete_channel_config(channel_id, &watering_channels[channel_id]);
    if (ret < 0) {
        return ret;
    }

    ret = onboarding_update_schedule_flag(channel_id, false);
    if (ret < 0) {
        return ret;
    }

    printk("Channel %d schedule reset\n", channel_id);

#ifdef CONFIG_BT
    (void)bt_irrigation_schedule_update(channel_id);
    (void)bt_irrigation_channel_config_update(channel_id);
#endif

    return 0;
}

static int reset_controller_reset_all_channels(void) {
    int ret;
    
    /* Reset all channel configurations */
    for (int ch = 0; ch < 8; ch++) {
        ret = reset_controller_reset_channel_config(ch);
        if (ret < 0) {
            return ret;
        }
    }
    
    printk("All channel configurations reset to defaults\n");
    return 0;
}

static int reset_controller_reset_all_schedules(void) {
    int ret;
    
    /* Reset all schedule configurations */
    for (int ch = 0; ch < 8; ch++) {
        ret = reset_controller_reset_channel_schedule(ch);
        if (ret < 0) {
            return ret;
        }
    }
    
    printk("All schedules reset\n");
    return 0;
}

static int reset_controller_reset_system_config(void) {
    /* Reset timezone to default (persisted) */
    timezone_config_t default_timezone = DEFAULT_TIMEZONE_CONFIG;
    int ret = timezone_set_config(&default_timezone);
    if (ret < 0) {
        return ret;
    }
    
    /* Reset automatic calculation state */
    automatic_calc_state_t default_calc_state = DEFAULT_AUTOMATIC_CALC_STATE;
    ret = nvs_save_automatic_calc_state(&default_calc_state);
    if (ret < 0) {
        return ret;
    }
    
    /* Clear system configuration flags */
    onboarding_update_system_flag(SYSTEM_FLAG_RTC_CONFIGURED, false);
    onboarding_update_system_flag(SYSTEM_FLAG_MASTER_VALVE_SET, false);
    onboarding_update_system_flag(SYSTEM_FLAG_POWER_MODE_SET, false);
    onboarding_update_system_flag(SYSTEM_FLAG_LOCATION_SET, false);
    onboarding_update_system_flag(SYSTEM_FLAG_INITIAL_SETUP_DONE, false);
    onboarding_update_system_flag(SYSTEM_FLAG_TIMEZONE_SET, false);
    onboarding_update_system_flag(SYSTEM_FLAG_RAIN_SENSOR_SET, false);
    
    printk("System configuration reset to defaults\n");

#ifdef CONFIG_BT
    /* Let clients refresh onboarding + config snapshots as needed */
    (void)bt_irrigation_onboarding_status_notify();
#endif
    return 0;
}

static int reset_controller_reset_calibration(void) {
    /* Reset flow calibration to default (RAM + NVS) */
    uint32_t default_calibration = 750; /* Default pulses per liter */
    watering_error_t err = watering_set_flow_calibration(default_calibration);
    int ret = (err == WATERING_SUCCESS) ? 0 : -EIO;
    if (ret < 0) {
        return ret;
    }
    
    /* Clear calibration flag */
    onboarding_update_system_flag(SYSTEM_FLAG_FLOW_CALIBRATED, false);
    
    printk("Calibration data reset to defaults\n");
    return 0;
}

static int reset_controller_reset_history(void) {
    /* Clear rain history (RAM + NVS) */
    watering_error_t hist_err = rain_history_clear_all();
    if (hist_err != WATERING_SUCCESS && hist_err != WATERING_ERROR_NOT_INITIALIZED) {
        return -EIO;
    }

    /* Reset rain sensor counters/state (so reads don't look cached) */
    rain_sensor_reset_counters();
    (void)rain_state_reset();

    /* Reset days since start (RAM + NVS) */
    days_since_start = 0;
    int ret = nvs_save_days_since_start(0);
    if (ret < 0) {
        return ret;
    }

    printk("History data cleared\n");
    return 0;
}

static int reset_controller_factory_reset(void) {
    int ret;
    
    /* Reset all channels */
    ret = reset_controller_reset_all_channels();
    if (ret < 0) {
        return ret;
    }
    
    /* Reset all schedules */
    ret = reset_controller_reset_all_schedules();
    if (ret < 0) {
        return ret;
    }
    
    /* Reset system configuration */
    ret = reset_controller_reset_system_config();
    if (ret < 0) {
        return ret;
    }
    
    /* Reset calibration */
    ret = reset_controller_reset_calibration();
    if (ret < 0) {
        return ret;
    }
    
    /* Reset history */
    ret = reset_controller_reset_history();
    if (ret < 0) {
        return ret;
    }
    
    /* Clear all onboarding data */
    ret = nvs_clear_onboarding_data();
    if (ret < 0) {
        return ret;
    }
    
    /* Reset onboarding state */
    ret = onboarding_reset_state();
    if (ret < 0) {
        return ret;
    }
    
    printk("Factory reset completed - all data cleared\n");
    return 0;
}
