#include "reset_controller.h"
#include "onboarding_state.h"
#include "nvs_config.h"
#include "timezone.h"
#include "watering.h"
#include "watering_internal.h"
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
    
    /* Reset enhanced channel configuration to defaults */
    enhanced_channel_config_t default_config = DEFAULT_ENHANCED_CHANNEL_CONFIG;
    int ret = nvs_save_enhanced_channel_config(channel_id, &default_config);
    if (ret < 0) {
        return ret;
    }
    
    /* CRITICAL: Also update RAM structure to match NVS defaults */
    /* This prevents stale values from being re-saved and triggering flags */
    watering_channels[channel_id].plant_db_index = UINT16_MAX;
    watering_channels[channel_id].soil_db_index = UINT8_MAX;
    watering_channels[channel_id].irrigation_method_index = UINT8_MAX;
    watering_channels[channel_id].sun_exposure_pct = 75;  /* Default sun exposure */
    watering_channels[channel_id].use_area_based = true;
    watering_channels[channel_id].coverage.area_m2 = 1.0f;
    
    /* Reset water balance configuration */
    water_balance_config_t default_balance = DEFAULT_WATER_BALANCE_CONFIG;
    ret = nvs_save_water_balance_config(channel_id, &default_balance);
    if (ret < 0) {
        return ret;
    }
    
    /* Clear channel name */
    ret = nvs_save_channel_name(channel_id, "");
    if (ret < 0) {
        return ret;
    }
    
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
    return 0;
}

static int reset_controller_reset_channel_schedule(uint8_t channel_id) {
    /* This would reset schedule configuration for the channel */
    /* Implementation depends on how schedules are stored */
    /* For now, just update the onboarding flag */
    
    int ret = onboarding_update_schedule_flag(channel_id, false);
    if (ret < 0) {
        return ret;
    }
    
    printk("Channel %d schedule reset\n", channel_id);
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
    return 0;
}

static int reset_controller_reset_calibration(void) {
    /* Reset flow calibration to default */
    uint32_t default_calibration = 750; /* Default pulses per liter */
    int ret = nvs_save_flow_calibration(default_calibration);
    if (ret < 0) {
        return ret;
    }
    
    /* Clear calibration flag */
    onboarding_update_system_flag(SYSTEM_FLAG_FLOW_CALIBRATED, false);
    
    printk("Calibration data reset to defaults\n");
    return 0;
}

static int reset_controller_reset_history(void) {
    /* Clear rain history */
    int ret = nvs_clear_rain_history();
    if (ret < 0) {
        return ret;
    }
    
    /* Reset days since start */
    ret = nvs_save_days_since_start(0);
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
