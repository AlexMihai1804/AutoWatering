#include "nvs_config.h"
#include "water_balance_types.h"  /* For water_balance_t definition */
#include "watering.h"  /* For watering_channel_t and related types */
#include "watering_internal.h"  /* For watering_channels array */
#include "watering_enhanced.h"  /* For config_reset_log_t */
#include "onboarding_state.h"  /* For onboarding flag updates */
#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>    // for DT_MTD_FROM_FIXED_PARTITION, DEVICE_DT_GET
#include <zephyr/drivers/flash.h>
#include <string.h>   /* ΓåÉ NEW */
#include <stdio.h>    /* for snprintf */
#include <errno.h>    /* for EINVAL */

/* ΓÇöΓÇöΓÇö instan┼ú─â NVS ΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇö */
static struct nvs_fs fs;
static bool nvs_ready;

/* ΓÇöΓÇöΓÇö ini┼úializare ΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇö */
int nvs_config_init(void)
{
    const struct device *flash_dev =
        DEVICE_DT_GET(DT_MTD_FROM_FIXED_PARTITION(NVS_DT_NODE));
    if (!device_is_ready(flash_dev)) {
        printk("NVS flash device not ready\n");
        return -ENODEV;
    }

    /* assign device before mounting */
    fs.offset       = NVS_OFFSET;
    fs.sector_size  = NVS_SECTOR_SIZE;
    fs.sector_count = NVS_SECTOR_COUNT;
    fs.flash_device = flash_dev;          /* <ΓÇöΓÇö fix esen┼úial */

    int rc = nvs_mount(&fs);
    if (rc == 0) {
        nvs_ready = true;
    } else {
        printk("NVS mount failed (%d)\n", rc);
    }
    return rc;
}

bool nvs_config_is_ready(void)
{
    return nvs_ready;
}

/* ΓÇöΓÇöΓÇö RAW read/write/delete ΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇö */
int nvs_config_read(uint16_t id, void *data, size_t len)
{
    return nvs_ready ? nvs_read (&fs, id, data, len) : -ENODEV;
}

int nvs_config_write(uint16_t id, const void *data, size_t len)
{
    return nvs_ready ? nvs_write(&fs, id, data, len) : -ENODEV;
}

int nvs_config_delete(uint16_t id)
{
    return nvs_ready ? nvs_delete(&fs, id) : -ENODEV;
}

/* ΓÇöΓÇöΓÇö IDs logice ΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇö */
enum {
    ID_WATERING_CFG     =  1,
    ID_CHANNEL_CFG_BASE = 100,   /* +ch (0-7) */
    ID_FLOW_CALIB       = 200,
    ID_DAYS_SINCE_START = 201,
    ID_CHANNEL_NAME_BASE= 300,   /* +ch (0-7)  ΓåÉ NEW */
    ID_TIMEZONE_CONFIG  = 400,   /* timezone_config_t */
    ID_ENHANCED_CHANNEL_CFG_BASE = 500, /* Enhanced channel config +ch (0-7) */
    ID_WATER_BALANCE_BASE = 600, /* Water balance state +ch (0-7) */
    ID_AUTOMATIC_CALC_STATE = 700, /* Automatic calculation state */
    ID_RAIN_CONFIG = 800,        /* Rain sensor configuration */
    ID_RAIN_STATE = 801,         /* Rain sensor state */
    ID_RAIN_HOURLY_DATA = 802,   /* Rain hourly history data */
    ID_RAIN_DAILY_DATA = 803,    /* Rain daily history data */
    ID_ONBOARDING_STATE = 900,   /* Onboarding state */
    ID_CHANNEL_FLAGS_BASE = 910, /* Channel flags +ch (0-7) */
    ID_SYSTEM_FLAGS = 920,       /* System configuration flags */
    ID_CONFIG_STATUS_BASE = 930, /* Configuration status +ch (0-7) */
    ID_CONFIG_RESET_LOG_BASE = 940, /* Configuration reset logs +ch (0-7) */
};

/* ΓÇöΓÇöΓÇö nivel ├«nalt ΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇö */
int nvs_save_watering_config(const void *cfg, size_t sz)
{
    return nvs_config_write(ID_WATERING_CFG, cfg, sz);
}

int nvs_load_watering_config(void *cfg, size_t sz)
{
    return nvs_config_read(ID_WATERING_CFG, cfg, sz);
}

int nvs_save_channel_config(uint8_t ch, const void *cfg, size_t sz)
{
    return nvs_config_write(ID_CHANNEL_CFG_BASE + ch, cfg, sz);
}

int nvs_load_channel_config(uint8_t ch, void *cfg, size_t sz)
{
    return nvs_config_read(ID_CHANNEL_CFG_BASE + ch, cfg, sz);
}

int nvs_save_config_reset_log(uint8_t ch, const config_reset_log_t *log)
{
    if (!log || ch >= WATERING_CHANNELS_COUNT) {
        return -EINVAL;
    }
    return nvs_config_write(ID_CONFIG_RESET_LOG_BASE + ch, log, sizeof(*log));
}

int nvs_load_config_reset_log(uint8_t ch, config_reset_log_t *log)
{
    if (!log || ch >= WATERING_CHANNELS_COUNT) {
        return -EINVAL;
    }
    return nvs_config_read(ID_CONFIG_RESET_LOG_BASE + ch, log, sizeof(*log));
}

int nvs_clear_config_reset_log(uint8_t ch)
{
    if (ch >= WATERING_CHANNELS_COUNT) {
        return -EINVAL;
    }
    return nvs_config_delete(ID_CONFIG_RESET_LOG_BASE + ch);
}

int nvs_save_flow_calibration(uint32_t cal)
{
    int ret = nvs_config_write(ID_FLOW_CALIB, &cal, sizeof(cal));
    if (ret >= 0) {
        /* Update onboarding flag - any explicit calibration save means flow is configured */
        /* Even 750 pulses/L is valid if user confirmed it */
        onboarding_update_system_flag(SYSTEM_FLAG_FLOW_CALIBRATED, true);
    }
    return ret;
}

int nvs_load_flow_calibration(uint32_t *cal)
{
    return nvs_config_read(ID_FLOW_CALIB, cal, sizeof(*cal));
}

int nvs_save_days_since_start(uint16_t days)
{
    return nvs_config_write(ID_DAYS_SINCE_START, &days, sizeof(days));
}

int nvs_load_days_since_start(uint16_t *days)
{
    return nvs_config_read(ID_DAYS_SINCE_START, days, sizeof(*days));
}

int nvs_save_channel_name(uint8_t ch, const char *name)
{
    if (!name) {
        /* Save empty string */
        int ret = nvs_config_write(ID_CHANNEL_NAME_BASE + ch, "", 1);
        if (ret >= 0) {
            onboarding_update_channel_flag(ch, CHANNEL_FLAG_NAME_SET, false);
        }
        return ret;
    }
    
    size_t len = strnlen(name, 63) + 1;   /* include '\0', max 64 chars */
    int ret = nvs_config_write(ID_CHANNEL_NAME_BASE + ch, name, len);
    if (ret >= 0) {
        /* Update onboarding flag for channel name */
        onboarding_update_channel_flag(ch, CHANNEL_FLAG_NAME_SET, len > 1); /* More than just '\0' */
    }
    
    return ret;
}

int nvs_load_channel_name(uint8_t ch, char *buf, size_t sz)
{
    if (!buf || sz == 0) return -EINVAL;
    
    /* Initialize buffer to empty string */
    buf[0] = '\0';
    
    int ret = nvs_config_read(ID_CHANNEL_NAME_BASE + ch, buf, sz - 1);  /* leave space for '\0' */
    if (ret >= 0) {
        if (ret < sz - 1) {
            buf[ret] = '\0';  /* ensure NUL termination */
        } else {
            buf[sz - 1] = '\0';  /* truncate if needed */
        }
    }
    return ret;
}

/* Timezone configuration functions */
int nvs_save_timezone_config(const timezone_config_t *config)
{
    if (!config) {
        return -EINVAL;
    }
    int ret = nvs_config_write(ID_TIMEZONE_CONFIG, config, sizeof(*config));
    if (ret >= 0) {
        /* Update onboarding flag - timezone has been configured */
        onboarding_update_system_flag(SYSTEM_FLAG_TIMEZONE_SET, true);
    }
    return ret;
}

int nvs_load_timezone_config(timezone_config_t *config)
{
    if (!config) {
        return -EINVAL;
    }

    int ret = nvs_config_read(ID_TIMEZONE_CONFIG, config, sizeof(*config));
    if (ret < 0) {
        timezone_config_t default_config = DEFAULT_TIMEZONE_CONFIG;
        *config = default_config;
        ret = sizeof(*config);
    } else if (ret != sizeof(*config)) {
        timezone_config_t default_config = DEFAULT_TIMEZONE_CONFIG;
        *config = default_config;
        ret = sizeof(*config);
    }
    return ret;
}

/* Enhanced Growing Environment Configuration Functions */

int nvs_save_enhanced_channel_config(uint8_t ch, const enhanced_channel_config_t *config)
{
    if (!config || ch >= 8) {  /* Assuming 8 channels max */
        return -EINVAL;
    }
    
    int ret = nvs_config_write(ID_ENHANCED_CHANNEL_CFG_BASE + ch, config, sizeof(*config));
    if (ret >= 0) {
        /* Update onboarding flags based on configuration */
        /* Use UINT16_MAX/UINT8_MAX as "not set" sentinel values (0 is a valid index!) */
        onboarding_update_channel_flag(ch, CHANNEL_FLAG_PLANT_TYPE_SET, 
                                     config->plant_db_index != UINT16_MAX);
        onboarding_update_channel_flag(ch, CHANNEL_FLAG_SOIL_TYPE_SET, 
                                     config->soil_db_index != UINT8_MAX);
        onboarding_update_channel_flag(ch, CHANNEL_FLAG_IRRIGATION_METHOD_SET, 
                                     config->irrigation_method_index != UINT8_MAX);
        onboarding_update_channel_flag(ch, CHANNEL_FLAG_SUN_EXPOSURE_SET, 
                                     config->sun_exposure_pct != 75); /* 75% is default */
    }
    
    return ret;
}

int nvs_load_enhanced_channel_config(uint8_t ch, enhanced_channel_config_t *config)
{
    if (!config || ch >= 8) {
        return -EINVAL;
    }
    
    int ret = nvs_config_read(ID_ENHANCED_CHANNEL_CFG_BASE + ch, config, sizeof(*config));
    if (ret < 0) {
        /* Load default configuration if not found */
        enhanced_channel_config_t default_config = DEFAULT_ENHANCED_CHANNEL_CONFIG;
        *config = default_config;
        
        /* Save default configuration for future use */
        nvs_save_enhanced_channel_config(ch, config);
        ret = sizeof(*config);
    } else if (ret != sizeof(*config)) {
        printk("Enhanced channel config size mismatch (got %d, expected %u). Resetting defaults.\n",
               ret, (unsigned int)sizeof(*config));
        enhanced_channel_config_t default_config = DEFAULT_ENHANCED_CHANNEL_CONFIG;
        *config = default_config;
        int write_ret = nvs_save_enhanced_channel_config(ch, config);
        if (write_ret < 0) {
            return write_ret;
        }
        ret = sizeof(*config);
    }
    return ret;
}

int nvs_save_water_balance_config(uint8_t ch, const water_balance_config_t *balance)
{
    if (!balance || ch >= 8) {
        return -EINVAL;
    }
    return nvs_config_write(ID_WATER_BALANCE_BASE + ch, balance, sizeof(*balance));
}

int nvs_load_water_balance_config(uint8_t ch, water_balance_config_t *balance)
{
    if (!balance || ch >= 8) {
        return -EINVAL;
    }
    
    int ret = nvs_config_read(ID_WATER_BALANCE_BASE + ch, balance, sizeof(*balance));
    if (ret < 0) {
        /* Load default water balance if not found */
        water_balance_config_t default_balance = DEFAULT_WATER_BALANCE_CONFIG;
        *balance = default_balance;
        
        /* Save default configuration for future use */
        nvs_save_water_balance_config(ch, balance);
        ret = sizeof(*balance);
    } else if (ret != sizeof(*balance)) {
        printk("Water balance config size mismatch (got %d, expected %u). Resetting defaults.\n",
               ret, (unsigned int)sizeof(*balance));
        water_balance_config_t default_balance = DEFAULT_WATER_BALANCE_CONFIG;
        *balance = default_balance;
        int write_ret = nvs_save_water_balance_config(ch, balance);
        if (write_ret < 0) {
            return write_ret;
        }
        ret = sizeof(*balance);
    }
    return ret;
}

int nvs_save_automatic_calc_state(const automatic_calc_state_t *state)
{
    if (!state) {
        return -EINVAL;
    }
    return nvs_config_write(ID_AUTOMATIC_CALC_STATE, state, sizeof(*state));
}

int nvs_load_automatic_calc_state(automatic_calc_state_t *state)
{
    if (!state) {
        return -EINVAL;
    }
    
    int ret = nvs_config_read(ID_AUTOMATIC_CALC_STATE, state, sizeof(*state));
    if (ret < 0) {
        /* Load default state if not found */
        automatic_calc_state_t default_state = DEFAULT_AUTOMATIC_CALC_STATE;
        *state = default_state;
        
        /* Save default state for future use */
        nvs_save_automatic_calc_state(state);
        ret = sizeof(*state);
    } else if (ret != sizeof(*state)) {
        printk("Automatic calc state size mismatch (got %d, expected %u). Resetting defaults.\n",
               ret, (unsigned int)sizeof(*state));
        automatic_calc_state_t default_state = DEFAULT_AUTOMATIC_CALC_STATE;
        *state = default_state;
        int write_ret = nvs_save_automatic_calc_state(state);
        if (write_ret < 0) {
            return write_ret;
        }
        ret = sizeof(*state);
    }
    return ret;
}

/* ΓÇöΓÇöΓÇö Enhanced Channel Management Functions ΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇö */

int nvs_save_complete_channel_config(uint8_t ch, const watering_channel_t *channel)
{
    if (!channel || ch >= 8) {
        return -EINVAL;
    }
    
    int ret;
    
    /* Save the basic channel configuration (existing functionality) */
    ret = nvs_save_channel_config(ch, channel, sizeof(*channel));
    if (ret < 0) {
        return ret;
    }
    
    /* Save channel name separately for better management */
    ret = nvs_save_channel_name(ch, channel->name);
    if (ret < 0) {
        return ret;
    }
    
    /* Extract and save enhanced configuration */
    enhanced_channel_config_t enhanced_config = {
        .plant_db_index = channel->plant_db_index,
        .soil_db_index = channel->soil_db_index,
        .irrigation_method_index = channel->irrigation_method_index,
        .use_area_based = channel->use_area_based,
        .auto_mode = (uint8_t)channel->auto_mode,
        .max_volume_limit_l = channel->max_volume_limit_l,
        .enable_cycle_soak = channel->enable_cycle_soak,
        .planting_date_unix = channel->planting_date_unix,
        .days_after_planting = channel->days_after_planting,
        .latitude_deg = channel->latitude_deg,
        .sun_exposure_pct = channel->sun_exposure_pct,
        .last_calculation_time = channel->last_calculation_time
    };
    
    /* Handle union assignment separately */
    if (channel->use_area_based) {
        enhanced_config.coverage.area_m2 = channel->coverage.area_m2;
    } else {
        enhanced_config.coverage.plant_count = channel->coverage.plant_count;
    }
    
    ret = nvs_save_enhanced_channel_config(ch, &enhanced_config);
    if (ret < 0) {
        return ret;
    }
    
    /* Update additional onboarding flags based on channel configuration */
    onboarding_update_channel_flag(ch, CHANNEL_FLAG_COVERAGE_SET, 
                                 channel->use_area_based ? 
                                 (channel->coverage.area_m2 > 0) : 
                                 (channel->coverage.plant_count > 0));
    onboarding_update_channel_flag(ch, CHANNEL_FLAG_ENABLED, 
                                 channel->config_status.basic_configured);
    
    /* Save water balance state if available */
    if (channel->water_balance) {
        water_balance_config_t balance_config = {
            .rwz_awc_mm = channel->water_balance->rwz_awc_mm,
            .wetting_awc_mm = channel->water_balance->wetting_awc_mm,
            .raw_mm = channel->water_balance->raw_mm,
            .current_deficit_mm = channel->water_balance->current_deficit_mm,
            .effective_rain_mm = channel->water_balance->effective_rain_mm,
            .irrigation_needed = channel->water_balance->irrigation_needed,
            .last_update_time = k_uptime_get_32(),
            .data_quality = 75
        };
        
        ret = nvs_save_water_balance_config(ch, &balance_config);
        if (ret < 0) {
            return ret;
        }
    }
    
    return 0;
}

int nvs_load_complete_channel_config(uint8_t ch, watering_channel_t *channel)
{
    if (!channel || ch >= 8) {
        return -EINVAL;
    }
    
    int ret;
    
    /* Load the basic channel configuration first */
    ret = nvs_load_channel_config(ch, channel, sizeof(*channel));
    if (ret < 0) {
        return ret;
    }
    
    /* Load channel name */
    ret = nvs_load_channel_name(ch, channel->name, sizeof(channel->name));
    if (ret < 0) {
        /* Set default name if not found */
        snprintf(channel->name, sizeof(channel->name), "Channel %d", ch + 1);
    }
    
    /* Load enhanced configuration */
    enhanced_channel_config_t enhanced_config;
    ret = nvs_load_enhanced_channel_config(ch, &enhanced_config);
    if (ret >= 0) {
        /* Update channel with enhanced configuration */
        channel->plant_db_index = enhanced_config.plant_db_index;
        channel->soil_db_index = enhanced_config.soil_db_index;
        channel->irrigation_method_index = enhanced_config.irrigation_method_index;
        channel->use_area_based = enhanced_config.use_area_based;
        /* Handle union assignment separately */
        if (enhanced_config.use_area_based) {
            channel->coverage.area_m2 = enhanced_config.coverage.area_m2;
        } else {
            channel->coverage.plant_count = enhanced_config.coverage.plant_count;
        }
        channel->auto_mode = (watering_mode_t)enhanced_config.auto_mode;
        channel->max_volume_limit_l = enhanced_config.max_volume_limit_l;
        channel->enable_cycle_soak = enhanced_config.enable_cycle_soak;
        channel->planting_date_unix = enhanced_config.planting_date_unix;
        channel->days_after_planting = enhanced_config.days_after_planting;
        channel->latitude_deg = enhanced_config.latitude_deg;
        channel->sun_exposure_pct = enhanced_config.sun_exposure_pct;
        channel->last_calculation_time = enhanced_config.last_calculation_time;
    }
    
    /* Load water balance configuration if available */
    water_balance_config_t balance_config;
    ret = nvs_load_water_balance_config(ch, &balance_config);
    if (ret >= 0 && channel->water_balance) {
        channel->water_balance->rwz_awc_mm = balance_config.rwz_awc_mm;
        channel->water_balance->wetting_awc_mm = balance_config.wetting_awc_mm;
        channel->water_balance->raw_mm = balance_config.raw_mm;
        channel->water_balance->current_deficit_mm = balance_config.current_deficit_mm;
        channel->water_balance->effective_rain_mm = balance_config.effective_rain_mm;
        channel->water_balance->irrigation_needed = balance_config.irrigation_needed;
    }
    
    return 0;
}



int nvs_validate_enhanced_config(const enhanced_channel_config_t *config)
{
    if (!config) {
        return -EINVAL;
    }
    
    /* Validate configuration parameters */
    if (config->max_volume_limit_l < 0.1f || config->max_volume_limit_l > 1000.0f) {
        return -EINVAL;  /* Volume limit out of reasonable range */
    }
    
    if (config->sun_exposure_pct > 100) {
        return -EINVAL;  /* Sun exposure percentage invalid */
    }
    
    if (config->latitude_deg < -90.0f || config->latitude_deg > 90.0f) {
        return -EINVAL;  /* Latitude out of valid range */
    }
    
    if (config->use_area_based) {
        if (config->coverage.area_m2 <= 0.0f || config->coverage.area_m2 > 10000.0f) {
            return -EINVAL;  /* Area out of reasonable range */
        }
    } else {
        if (config->coverage.plant_count == 0 || config->coverage.plant_count > 10000) {
            return -EINVAL;  /* Plant count out of reasonable range */
        }
    }
    
    return 0;  /* Configuration is valid */
}

int nvs_save_all_channel_configs(void)
{
    int ret;
    int failed_channels = 0;
    
    /* Save all channel configurations */
    for (uint8_t ch = 0; ch < 8; ch++) {
        ret = nvs_save_complete_channel_config(ch, &watering_channels[ch]);
        if (ret < 0) {
            printk("Failed to save channel %d configuration: %d\n", ch, ret);
            failed_channels++;
        }
    }
    
    /* Save automatic calculation state */
    automatic_calc_state_t calc_state;
    ret = nvs_load_automatic_calc_state(&calc_state);
    if (ret >= 0) {
        ret = nvs_save_automatic_calc_state(&calc_state);
        if (ret < 0) {
            printk("Failed to save automatic calculation state: %d\n", ret);
            failed_channels++;
        }
    }
    
    return failed_channels == 0 ? 0 : -1;
}

int nvs_load_all_channel_configs(void)
{
    int ret;
    int failed_channels = 0;
    
    /* Load all channel configurations */
    for (uint8_t ch = 0; ch < 8; ch++) {
        ret = nvs_load_complete_channel_config(ch, &watering_channels[ch]);
        if (ret < 0) {
            printk("Failed to load channel %d configuration: %d\n", ch, ret);
            failed_channels++;
            
            /* Apply default configuration on failure */
            enhanced_channel_config_t default_config = DEFAULT_ENHANCED_CHANNEL_CONFIG;
            nvs_save_enhanced_channel_config(ch, &default_config);
        }
    }
    
    /* Load automatic calculation state */
    automatic_calc_state_t calc_state;
    ret = nvs_load_automatic_calc_state(&calc_state);
    if (ret < 0) {
        printk("Failed to load automatic calculation state: %d\n", ret);
        failed_channels++;
    }
    
    return failed_channels == 0 ? 0 : -1;
}

int nvs_reset_enhanced_configs(void)
{
    int ret;
    int failed_operations = 0;
    
    /* Reset all enhanced channel configurations to defaults */
    for (uint8_t ch = 0; ch < 8; ch++) {
        enhanced_channel_config_t default_config = DEFAULT_ENHANCED_CHANNEL_CONFIG;
        ret = nvs_save_enhanced_channel_config(ch, &default_config);
        if (ret < 0) {
            printk("Failed to reset enhanced config for channel %d: %d\n", ch, ret);
            failed_operations++;
        }
        
        water_balance_config_t default_balance = DEFAULT_WATER_BALANCE_CONFIG;
        ret = nvs_save_water_balance_config(ch, &default_balance);
        if (ret < 0) {
            printk("Failed to reset water balance for channel %d: %d\n", ch, ret);
            failed_operations++;
        }
    }
    
    /* Reset automatic calculation state */
    automatic_calc_state_t default_state = DEFAULT_AUTOMATIC_CALC_STATE;
    ret = nvs_save_automatic_calc_state(&default_state);
    if (ret < 0) {
        printk("Failed to reset automatic calculation state: %d\n", ret);
        failed_operations++;
    }
    
    return failed_operations == 0 ? 0 : -1;
}

int nvs_backup_configuration(void)
{
    /* This function could be extended to create a backup of all configurations
     * to a separate NVS area or external storage. For now, it validates
     * that all configurations are properly stored. */
    
    int validation_errors = 0;
    
    for (uint8_t ch = 0; ch < 8; ch++) {
        enhanced_channel_config_t config;
        int ret = nvs_load_enhanced_channel_config(ch, &config);
        if (ret < 0) {
            printk("Channel %d enhanced config validation failed: %d\n", ch, ret);
            validation_errors++;
            continue;
        }
        
        ret = nvs_validate_enhanced_config(&config);
        if (ret < 0) {
            printk("Channel %d enhanced config invalid: %d\n", ch, ret);
            validation_errors++;
        }
        
        water_balance_config_t balance;
        ret = nvs_load_water_balance_config(ch, &balance);
        if (ret < 0) {
            printk("Channel %d water balance validation failed: %d\n", ch, ret);
            validation_errors++;
        }
    }
    
    automatic_calc_state_t calc_state;
    int ret = nvs_load_automatic_calc_state(&calc_state);
    if (ret < 0) {
        printk("Automatic calculation state validation failed: %d\n", ret);
        validation_errors++;
    }
    
    if (validation_errors == 0) {
        printk("Configuration backup validation successful\n");
    } else {
        printk("Configuration backup validation found %d errors\n", validation_errors);
    }
    
    return validation_errors == 0 ? 0 : -1;
}

int nvs_get_storage_usage(size_t *used_bytes, size_t *total_bytes)
{
    if (!used_bytes || !total_bytes) {
        return -EINVAL;
    }
    
    /* Calculate approximate storage usage */
    *total_bytes = NVS_SIZE;
    
    /* Estimate used space based on stored configurations */
    *used_bytes = 0;
    
    /* Basic configurations */
    *used_bytes += sizeof(uint32_t);           /* Flow calibration */
    *used_bytes += sizeof(uint16_t);           /* Days since start */
    
    /* Channel configurations */
    for (uint8_t ch = 0; ch < 8; ch++) {
        *used_bytes += sizeof(watering_channel_t);        /* Basic channel */
        *used_bytes += sizeof(enhanced_channel_config_t); /* Enhanced config */
        *used_bytes += sizeof(water_balance_config_t);    /* Water balance */
        *used_bytes += 64;                                /* Channel name */
    }
    
    /* Automatic calculation state */
    *used_bytes += sizeof(automatic_calc_state_t);
    
    /* Add NVS overhead (approximately 20% for metadata) */
    *used_bytes = (*used_bytes * 120) / 100;
    
    /* Ensure we don't report more than total */
    if (*used_bytes > *total_bytes) {
        *used_bytes = *total_bytes;
    }
    
    return 0;
}/* ΓÇöΓÇöΓÇö R
ain History NVS Functions ΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇö */

int nvs_save_rain_config(const rain_nvs_config_t *config)
{
    if (!config) {
        return -EINVAL;
    }
    
    int ret = nvs_config_write(ID_RAIN_CONFIG, config, sizeof(*config));
    if (ret >= 0) {
        /* Update onboarding flag - user has configured rain sensor settings */
        /* Once configured, flag stays true (onboarding complete for this feature) */
        onboarding_update_system_flag(SYSTEM_FLAG_RAIN_SENSOR_SET, true);
    }
    
    return ret;
}

int nvs_load_rain_config(rain_nvs_config_t *config)
{
    if (!config) {
        return -EINVAL;
    }
    
    int ret = nvs_config_read(ID_RAIN_CONFIG, config, sizeof(*config));
    if (ret < 0) {
        /* Load default configuration if not found */
        rain_nvs_config_t default_config = DEFAULT_RAIN_CONFIG;
        *config = default_config;
        
        /* Save default configuration for future use */
        nvs_save_rain_config(config);
        ret = sizeof(*config);
    }
    return ret;
}

int nvs_save_rain_state(const rain_nvs_state_t *state)
{
    if (!state) {
        return -EINVAL;
    }
    return nvs_config_write(ID_RAIN_STATE, state, sizeof(*state));
}

int nvs_load_rain_state(rain_nvs_state_t *state)
{
    if (!state) {
        return -EINVAL;
    }
    
    int ret = nvs_config_read(ID_RAIN_STATE, state, sizeof(*state));
    if (ret < 0) {
        /* Load default state if not found */
        rain_nvs_state_t default_state = DEFAULT_RAIN_STATE;
        *state = default_state;
        
        /* Save default state for future use */
        nvs_save_rain_state(state);
        ret = sizeof(*state);
    }
    return ret;
}

/**
 * @brief Simple compression for rain history data
 * 
 * This implements a basic run-length encoding for zero values and
 * delta compression for timestamps to reduce storage usage.
 */
static int compress_rain_data(const void *input_data, size_t input_size,
                             void *output_buffer, size_t buffer_size,
                             size_t *compressed_size)
{
    if (!input_data || !output_buffer || !compressed_size) {
        return -EINVAL;
    }
    
    /* For now, implement simple compression by removing zero entries */
    const uint8_t *input = (const uint8_t *)input_data;
    uint8_t *output = (uint8_t *)output_buffer;
    /* removed unused output_pos variable (was 0) */
    
    /* Simple copy for now - real compression would be more sophisticated */
    if (input_size > buffer_size) {
        return -ENOMEM;
    }
    
    memcpy(output, input, input_size);
    *compressed_size = input_size;
    
    return 0;
}

/**
 * @brief Decompress rain history data
 */
static int decompress_rain_data(const void *compressed_data, size_t compressed_size,
                               void *output_buffer, size_t buffer_size,
                               size_t *decompressed_size)
{
    if (!compressed_data || !output_buffer || !decompressed_size) {
        return -EINVAL;
    }
    
    /* Simple decompression - just copy for now */
    if (compressed_size > buffer_size) {
        return -ENOMEM;
    }
    
    memcpy(output_buffer, compressed_data, compressed_size);
    *decompressed_size = compressed_size;
    
    return 0;
}

/**
 * @brief Calculate simple checksum for data integrity
 */
static uint32_t calculate_checksum(const void *data, size_t size)
{
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t checksum = 0;
    
    for (size_t i = 0; i < size; i++) {
        checksum = ((checksum << 1) | (checksum >> 31)) ^ bytes[i];
    }
    
    return checksum;
}

int nvs_save_rain_hourly_data(const void *hourly_data, uint16_t entry_count, size_t data_size)
{
    if (!hourly_data || entry_count == 0 || data_size == 0) {
        return -EINVAL;
    }
    
    /* Allocate buffer for compressed data + header */
    size_t max_compressed_size = data_size + 256; /* Extra space for compression overhead */
    uint8_t *buffer = k_malloc(sizeof(rain_history_header_t) + max_compressed_size);
    if (!buffer) {
        return -ENOMEM;
    }
    
    rain_history_header_t *header = (rain_history_header_t *)buffer;
    uint8_t *compressed_data = buffer + sizeof(rain_history_header_t);
    
    /* Compress the data */
    size_t compressed_size;
    int ret = compress_rain_data(hourly_data, data_size, compressed_data, 
                                max_compressed_size, &compressed_size);
    if (ret < 0) {
        k_free(buffer);
        return ret;
    }
    
    /* Fill header */
    header->entry_count = entry_count;
    header->compressed_size = (uint16_t)compressed_size;
    header->checksum = calculate_checksum(compressed_data, compressed_size);
    
    /* Extract timestamps from first and last entries */
    if (data_size >= sizeof(uint32_t) * 2) {
        const uint32_t *timestamps = (const uint32_t *)hourly_data;
        header->oldest_timestamp = timestamps[0];
        /* Last entry timestamp is at (entry_count-1) * entry_size */
        size_t entry_size = data_size / entry_count;
        const uint32_t *last_entry = (const uint32_t *)((const uint8_t *)hourly_data + 
                                                        (entry_count - 1) * entry_size);
        header->newest_timestamp = *last_entry;
    } else {
        header->oldest_timestamp = 0;
        header->newest_timestamp = 0;
    }
    
    /* Save to NVS */
    size_t total_size = sizeof(rain_history_header_t) + compressed_size;
    ret = nvs_config_write(ID_RAIN_HOURLY_DATA, buffer, total_size);
    
    k_free(buffer);
    
    if (ret >= 0) {
        printk("Rain hourly data saved: %u entries, %zu bytes compressed to %zu bytes\n",
               entry_count, data_size, compressed_size);
    }
    
    return ret;
}

int nvs_load_rain_hourly_data(void *hourly_data, uint16_t max_entries, uint16_t *actual_count)
{
    if (!hourly_data || !actual_count) {
        return -EINVAL;
    }
    
    *actual_count = 0;
    
    /* Read header first to determine data size */
    rain_history_header_t header;
    int ret = nvs_config_read(ID_RAIN_HOURLY_DATA, &header, sizeof(header));
    if (ret < 0) {
        return ret; /* No data found */
    }
    
    if (ret < sizeof(header)) {
        return -ENODATA; /* Incomplete header */
    }
    
    /* Validate header */
    if (header.entry_count == 0) {
        return -EINVAL; /* Invalid data */
    }
    
    /* Allocate buffer for full data */
    size_t total_size = sizeof(rain_history_header_t) + header.compressed_size;
    uint8_t *buffer = k_malloc(total_size);
    if (!buffer) {
        return -ENOMEM;
    }
    
    /* Read full data */
    ret = nvs_config_read(ID_RAIN_HOURLY_DATA, buffer, total_size);
    if (ret < 0 || ret < total_size) {
        k_free(buffer);
        return ret < 0 ? ret : -ENODATA;
    }
    
    rain_history_header_t *full_header = (rain_history_header_t *)buffer;
    uint8_t *compressed_data = buffer + sizeof(rain_history_header_t);
    
    /* Verify checksum */
    uint32_t calculated_checksum = calculate_checksum(compressed_data, full_header->compressed_size);
    if (calculated_checksum != full_header->checksum) {
        k_free(buffer);
        return -EILSEQ; /* Data corruption detected */
    }
    
    /* Decompress data */
    size_t max_output_size = max_entries * 8; /* Assuming 8 bytes per hourly entry */
    size_t decompressed_size;
    ret = decompress_rain_data(compressed_data, full_header->compressed_size,
                              hourly_data, max_output_size, &decompressed_size);
    
    if (ret < 0) {
        k_free(buffer);
        return ret;
    }
    
    /* Calculate actual entry count */
    *actual_count = (uint16_t)(decompressed_size / 8); /* 8 bytes per hourly entry */
    if (*actual_count > max_entries) {
        *actual_count = max_entries;
    }
    
    k_free(buffer);
    
    printk("Rain hourly data loaded: %u entries, %zu bytes decompressed\n",
           *actual_count, decompressed_size);
    
    return 0;
}

int nvs_save_rain_daily_data(const void *daily_data, uint16_t entry_count, size_t data_size)
{
    if (!daily_data || entry_count == 0 || data_size == 0) {
        return -EINVAL;
    }
    
    /* Allocate buffer for compressed data + header */
    size_t max_compressed_size = data_size + 256;
    uint8_t *buffer = k_malloc(sizeof(rain_history_header_t) + max_compressed_size);
    if (!buffer) {
        return -ENOMEM;
    }
    
    rain_history_header_t *header = (rain_history_header_t *)buffer;
    uint8_t *compressed_data = buffer + sizeof(rain_history_header_t);
    
    /* Compress the data */
    size_t compressed_size;
    int ret = compress_rain_data(daily_data, data_size, compressed_data, 
                                max_compressed_size, &compressed_size);
    if (ret < 0) {
        k_free(buffer);
        return ret;
    }
    
    /* Fill header */
    header->entry_count = entry_count;
    header->compressed_size = (uint16_t)compressed_size;
    header->checksum = calculate_checksum(compressed_data, compressed_size);
    
    /* Extract timestamps from first and last entries */
    if (data_size >= sizeof(uint32_t) * 2) {
        const uint32_t *timestamps = (const uint32_t *)daily_data;
        header->oldest_timestamp = timestamps[0];
        /* Last entry timestamp */
        size_t entry_size = data_size / entry_count;
        const uint32_t *last_entry = (const uint32_t *)((const uint8_t *)daily_data + 
                                                        (entry_count - 1) * entry_size);
        header->newest_timestamp = *last_entry;
    } else {
        header->oldest_timestamp = 0;
        header->newest_timestamp = 0;
    }
    
    /* Save to NVS */
    size_t total_size = sizeof(rain_history_header_t) + compressed_size;
    ret = nvs_config_write(ID_RAIN_DAILY_DATA, buffer, total_size);
    
    k_free(buffer);
    
    if (ret >= 0) {
        printk("Rain daily data saved: %u entries, %zu bytes compressed to %zu bytes\n",
               entry_count, data_size, compressed_size);
    }
    
    return ret;
}

int nvs_load_rain_daily_data(void *daily_data, uint16_t max_entries, uint16_t *actual_count)
{
    if (!daily_data || !actual_count) {
        return -EINVAL;
    }
    
    *actual_count = 0;
    
    /* Read header first */
    rain_history_header_t header;
    int ret = nvs_config_read(ID_RAIN_DAILY_DATA, &header, sizeof(header));
    if (ret < 0) {
        return ret; /* No data found */
    }
    
    if (ret < sizeof(header)) {
        return -ENODATA;
    }
    
    /* Validate header */
    if (header.entry_count == 0) {
        return -EINVAL;
    }
    
    /* Allocate buffer for full data */
    size_t total_size = sizeof(rain_history_header_t) + header.compressed_size;
    uint8_t *buffer = k_malloc(total_size);
    if (!buffer) {
        return -ENOMEM;
    }
    
    /* Read full data */
    ret = nvs_config_read(ID_RAIN_DAILY_DATA, buffer, total_size);
    if (ret < 0 || ret < total_size) {
        k_free(buffer);
        return ret < 0 ? ret : -ENODATA;
    }
    
    rain_history_header_t *full_header = (rain_history_header_t *)buffer;
    uint8_t *compressed_data = buffer + sizeof(rain_history_header_t);
    
    /* Verify checksum */
    uint32_t calculated_checksum = calculate_checksum(compressed_data, full_header->compressed_size);
    if (calculated_checksum != full_header->checksum) {
        k_free(buffer);
        return -EILSEQ;
    }
    
    /* Decompress data */
    size_t max_output_size = max_entries * 12; /* Assuming 12 bytes per daily entry */
    size_t decompressed_size;
    ret = decompress_rain_data(compressed_data, full_header->compressed_size,
                              daily_data, max_output_size, &decompressed_size);
    
    if (ret < 0) {
        k_free(buffer);
        return ret;
    }
    
    /* Calculate actual entry count */
    *actual_count = (uint16_t)(decompressed_size / 12); /* 12 bytes per daily entry */
    if (*actual_count > max_entries) {
        *actual_count = max_entries;
    }
    
    k_free(buffer);
    
    printk("Rain daily data loaded: %u entries, %zu bytes decompressed\n",
           *actual_count, decompressed_size);
    
    return 0;
}

int nvs_clear_rain_history(void)
{
    int ret1 = nvs_config_delete(ID_RAIN_HOURLY_DATA);
    int ret2 = nvs_config_delete(ID_RAIN_DAILY_DATA);
    
    /* Return success if at least one deletion succeeded or both failed with "not found" */
    if ((ret1 >= 0 || ret1 == -ENOENT) && (ret2 >= 0 || ret2 == -ENOENT)) {
        printk("Rain history data cleared\n");
        return 0;
    }
    
    return (ret1 < 0) ? ret1 : ret2;
}

int nvs_get_rain_storage_usage(size_t *used_bytes, size_t *total_bytes)
{
    if (!used_bytes || !total_bytes) {
        return -EINVAL;
    }
    
    *used_bytes = 0;
    *total_bytes = 0;
    
    /* Calculate storage usage for rain data */
    rain_history_header_t header;
    
    /* Check hourly data */
    int ret = nvs_config_read(ID_RAIN_HOURLY_DATA, &header, sizeof(header));
    if (ret >= 0) {
        *used_bytes += sizeof(rain_history_header_t) + header.compressed_size;
    }
    
    /* Check daily data */
    ret = nvs_config_read(ID_RAIN_DAILY_DATA, &header, sizeof(header));
    if (ret >= 0) {
        *used_bytes += sizeof(rain_history_header_t) + header.compressed_size;
    }
    
    /* Add configuration and state storage */
    *used_bytes += sizeof(rain_nvs_config_t);
    *used_bytes += sizeof(rain_nvs_state_t);
    
    /* Estimate total available space for rain data (conservative estimate) */
    *total_bytes = 32 * 1024; /* 32KB allocated for rain history */
    
    return 0;
}
/* 
ΓÇöΓÇöΓÇö Onboarding State NVS Functions ΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇöΓÇö */

int nvs_save_onboarding_state(const onboarding_state_t *state)
{
    if (!state) {
        return -EINVAL;
    }
    return nvs_config_write(ID_ONBOARDING_STATE, state, sizeof(*state));
}

int nvs_load_onboarding_state(onboarding_state_t *state)
{
    if (!state) {
        return -EINVAL;
    }
    
    int ret = nvs_config_read(ID_ONBOARDING_STATE, state, sizeof(*state));
    if (ret < 0) {
        /* Load default state if not found */
        onboarding_state_t default_state = DEFAULT_ONBOARDING_STATE;
        *state = default_state;
        
        /* Save default state for future use */
        nvs_save_onboarding_state(state);
        ret = sizeof(*state);
    } else if (ret != sizeof(*state)) {
        printk("Onboarding state size mismatch (got %d, expected %u). Resetting defaults.\n",
               ret, (unsigned int)sizeof(*state));
        onboarding_state_t default_state = DEFAULT_ONBOARDING_STATE;
        *state = default_state;
        int write_ret = nvs_save_onboarding_state(state);
        if (write_ret < 0) {
            return write_ret;
        }
        ret = sizeof(*state);
    }
    return ret;
}

int nvs_save_channel_flags(uint8_t channel_id, uint8_t flags)
{
    if (channel_id >= 8) {
        return -EINVAL;
    }
    return nvs_config_write(ID_CHANNEL_FLAGS_BASE + channel_id, &flags, sizeof(flags));
}

int nvs_load_channel_flags(uint8_t channel_id, uint8_t *flags)
{
    if (!flags || channel_id >= 8) {
        return -EINVAL;
    }
    
    int ret = nvs_config_read(ID_CHANNEL_FLAGS_BASE + channel_id, flags, sizeof(*flags));
    if (ret < 0) {
        /* Default to no flags set */
        *flags = 0;
        ret = sizeof(*flags);
    }
    return ret;
}

int nvs_save_system_flags(uint32_t flags)
{
    return nvs_config_write(ID_SYSTEM_FLAGS, &flags, sizeof(flags));
}

int nvs_load_system_flags(uint32_t *flags)
{
    if (!flags) {
        return -EINVAL;
    }
    
    int ret = nvs_config_read(ID_SYSTEM_FLAGS, flags, sizeof(*flags));
    if (ret < 0) {
        /* Default to no flags set */
        *flags = 0;
        ret = sizeof(*flags);
    }
    return ret;
}

int nvs_clear_onboarding_data(void)
{
    int ret;
    int failed_operations = 0;
    
    /* Clear main onboarding state */
    ret = nvs_config_delete(ID_ONBOARDING_STATE);
    if (ret < 0) {
        printk("Failed to clear onboarding state: %d\n", ret);
        failed_operations++;
    }
    
    /* Clear channel flags */
    for (uint8_t ch = 0; ch < 8; ch++) {
        ret = nvs_config_delete(ID_CHANNEL_FLAGS_BASE + ch);
        if (ret < 0) {
            printk("Failed to clear channel %d flags: %d\n", ch, ret);
            failed_operations++;
        }
    }
    
    /* Clear system flags */
    ret = nvs_config_delete(ID_SYSTEM_FLAGS);
    if (ret < 0) {
        printk("Failed to clear system flags: %d\n", ret);
        failed_operations++;
    }
    
    return failed_operations == 0 ? 0 : -1;
}
