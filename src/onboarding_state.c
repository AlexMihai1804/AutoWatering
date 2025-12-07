#include "onboarding_state.h"
#include "nvs_config.h"
#include "bt_irrigation_service.h"  /* For BLE notifications */
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <string.h>
#include <errno.h>

/* Local forward declaration to avoid implicit declaration warning if
 * the header prototype is excluded by build-time conditionals.
 * (Prototype also lives in bt_irrigation_service.h when enabled.)
 */
int bt_irrigation_onboarding_status_notify(void);

/**
 * @file onboarding_state.c
 * @brief Implementation of onboarding state management
 * 
 * This module tracks configuration completeness and provides
 * progress calculation for user onboarding workflows.
 */

/* Global onboarding state */
static onboarding_state_t current_state;
static bool state_initialized = false;

/* Mutex for thread safety */
K_MUTEX_DEFINE(onboarding_mutex);

/* Debounce mechanism for BLE notifications */
#define ONBOARDING_NOTIFY_DEBOUNCE_MS 500  /* Wait 500ms before sending notification to batch all flag updates */
static bool notify_pending = false;

static void onboarding_notify_work_handler(struct k_work *work);
K_WORK_DELAYABLE_DEFINE(onboarding_notify_work, onboarding_notify_work_handler);

/* Debounced notification handler - sends single notification after all updates settle */
static void onboarding_notify_work_handler(struct k_work *work) {
    ARG_UNUSED(work);
    notify_pending = false;
    bt_irrigation_onboarding_status_notify();
}

/* Schedule a debounced notification */
static void schedule_onboarding_notify(void) {
    /* Cancel any pending notification and reschedule */
    k_work_reschedule(&onboarding_notify_work, K_MSEC(ONBOARDING_NOTIFY_DEBOUNCE_MS));
    notify_pending = true;
}

/* Helper function to get current timestamp */
static uint32_t get_current_timestamp(void) {
    return k_uptime_get_32() / 1000; /* Convert to seconds */
}

/* Helper function to count set bits in a value */
static int count_set_bits(uint64_t value) {
    int count = 0;
    while (value) {
        count += value & 1;
        value >>= 1;
    }
    return count;
}

/* Helper function to count set bits in 32-bit value */
static int count_set_bits_32(uint32_t value) {
    int count = 0;
    while (value) {
        count += value & 1;
        value >>= 1;
    }
    return count;
}

/* Helper function to count set bits in 8-bit value */
static int count_set_bits_8(uint8_t value) {
    int count = 0;
    while (value) {
        count += value & 1;
        value >>= 1;
    }
    return count;
}

/* Calculate completion for an arbitrary onboarding_state_t (no global guard) */
static int onboarding_calculate_completion_for(const onboarding_state_t *state) {
    if (!state) {
        return 0;
    }
    
    const int CHANNEL_WEIGHT = 60;  /* 60% for channel configuration */
    const int SYSTEM_WEIGHT = 30;   /* 30% for system configuration */
    const int SCHEDULE_WEIGHT = 10; /* 10% for schedule configuration */

    int total_channel_flags = 8 * 8; /* 8 channels A- 8 flags each */
    int set_channel_flags = count_set_bits(state->channel_config_flags);
    int channel_completion = (set_channel_flags * CHANNEL_WEIGHT) / total_channel_flags;

    int total_system_flags = 8; /* 8 system flags defined */
    int set_system_flags = count_set_bits_32(state->system_config_flags);
    int system_completion = (set_system_flags * SYSTEM_WEIGHT) / total_system_flags;

    int total_schedule_flags = 8; /* 8 channels can have schedules */
    int set_schedule_flags = count_set_bits_8(state->schedule_config_flags);
    int schedule_completion = (set_schedule_flags * SCHEDULE_WEIGHT) / total_schedule_flags;

    int total_completion = channel_completion + system_completion + schedule_completion;
    if (total_completion > 100) {
        total_completion = 100;
    }
    return total_completion;
}

/* Determine if a channel should be marked config-complete (auto/manual rules) - mutex must be held */
static bool channel_config_complete_locked(uint8_t channel_id) {
    uint8_t shift = channel_id * 8;
    uint8_t base_flags = (current_state.channel_config_flags >> shift) & 0xFF;
    uint8_t ext_flags = (current_state.channel_extended_flags >> shift) & 0xFF;
    bool schedule_set = (current_state.schedule_config_flags & (1u << channel_id)) != 0;

    /* FAO-56 mode: plant + soil + method + coverage + sun + name + water_factor + enabled + latitude + cycle_soak + schedule */
    const uint8_t fao_base_required = CHANNEL_FLAG_PLANT_TYPE_SET |
                                      CHANNEL_FLAG_SOIL_TYPE_SET |
                                      CHANNEL_FLAG_IRRIGATION_METHOD_SET |
                                      CHANNEL_FLAG_COVERAGE_SET |
                                      CHANNEL_FLAG_SUN_EXPOSURE_SET |
                                      CHANNEL_FLAG_NAME_SET |
                                      CHANNEL_FLAG_WATER_FACTOR_SET |
                                      CHANNEL_FLAG_ENABLED;
    bool fao_ready = ((base_flags & fao_base_required) == fao_base_required) &&
                     (ext_flags & CHANNEL_EXT_FLAG_LATITUDE_SET) &&
                     (ext_flags & CHANNEL_EXT_FLAG_CYCLE_SOAK_SET) &&
                     schedule_set;

    /* Manual (duration/volume) mode: name + enabled + rain comp + temp comp + cycle soak + schedule */
    const uint8_t manual_base_required = CHANNEL_FLAG_NAME_SET | CHANNEL_FLAG_ENABLED;
    bool manual_ready = ((base_flags & manual_base_required) == manual_base_required) &&
                        (ext_flags & CHANNEL_EXT_FLAG_RAIN_COMP_SET) &&
                        (ext_flags & CHANNEL_EXT_FLAG_TEMP_COMP_SET) &&
                        (ext_flags & CHANNEL_EXT_FLAG_CYCLE_SOAK_SET) &&
                        schedule_set;

    return fao_ready || manual_ready;
}

/* Refresh CONFIG_COMPLETE extended flag for one channel - mutex must be held */
static bool update_channel_complete_flag_locked(uint8_t channel_id) {
    uint8_t shift = channel_id * 8;
    uint64_t mask = (uint64_t)CHANNEL_EXT_FLAG_CONFIG_COMPLETE << shift;
    bool should_set = channel_config_complete_locked(channel_id);
    bool changed = false;

    if (should_set) {
        if ((current_state.channel_extended_flags & mask) == 0) {
            current_state.channel_extended_flags |= mask;
            changed = true;
        }
    } else {
        if (current_state.channel_extended_flags & mask) {
            current_state.channel_extended_flags &= ~mask;
            changed = true;
        }
    }
    return changed;
}

int onboarding_state_init(void) {
    k_mutex_lock(&onboarding_mutex, K_FOREVER);

    if (state_initialized) {
        k_mutex_unlock(&onboarding_mutex);
        return 0;
    }
    
    /* Try to load existing state from NVS */
    int ret = nvs_load_onboarding_state(&current_state);
    if (ret < 0) {
        printk("Failed to load onboarding state: %d\n", ret);
        k_mutex_unlock(&onboarding_mutex);
        return ret;
    }
    
    /* If this is a fresh state, set the start time */
    if (current_state.onboarding_start_time == 0) {
        current_state.onboarding_start_time = get_current_timestamp();
        current_state.last_update_time = current_state.onboarding_start_time;
        
        /* Save updated state */
        ret = nvs_save_onboarding_state(&current_state);
        if (ret < 0) {
            printk("Failed to save initial onboarding state: %d\n", ret);
            k_mutex_unlock(&onboarding_mutex);
            return ret;
        }
        
        printk("Onboarding state initialized with defaults\n");
    } else {
        printk("Onboarding state loaded from NVS: system_flags=0x%08x, channel_flags=0x%016llx, completion=%u%%\n",
               current_state.system_config_flags,
               (unsigned long long)current_state.channel_config_flags,
               current_state.onboarding_completion_pct);
    }

    /* Recompute config-complete flags for all channels after load */
    bool any_changed = false;
    for (uint8_t ch = 0; ch < 8; ch++) {
        if (update_channel_complete_flag_locked(ch)) {
            any_changed = true;
        }
    }
    if (any_changed) {
        /* Persist migrated/updated state */
        nvs_save_onboarding_state(&current_state);
    }

    /* Recompute completion to keep derived field in sync with loaded flags */
    current_state.onboarding_completion_pct = onboarding_calculate_completion_for(&current_state);
    
    state_initialized = true;
    k_mutex_unlock(&onboarding_mutex);
    
    return 0;
}

int onboarding_get_state(onboarding_state_t *state) {
    if (!state) {
        return -EINVAL;
    }
    
    if (!state_initialized) {
        return -ENODEV;
    }
    
    k_mutex_lock(&onboarding_mutex, K_FOREVER);
    *state = current_state;
    k_mutex_unlock(&onboarding_mutex);
    
    return 0;
}

/* Helper function to check and auto-set SYSTEM_FLAG_INITIAL_SETUP_DONE
 * Call this AFTER updating flags but BEFORE releasing mutex
 * Returns true if the flag was just set for the first time
 */
static bool check_and_set_initial_setup_done(void) {
    /* Skip if already marked as done */
    if (current_state.system_config_flags & SYSTEM_FLAG_INITIAL_SETUP_DONE) {
        return false;
    }
    
    /* Check if at least one channel has minimum required configuration */
    bool has_configured_channel = false;
    for (int ch = 0; ch < 8; ch++) {
        uint8_t shift = ch * 8;
        uint8_t channel_flags = (current_state.channel_config_flags >> shift) & 0xFF;
        uint8_t required_flags = CHANNEL_FLAG_PLANT_TYPE_SET | 
                                CHANNEL_FLAG_SOIL_TYPE_SET | 
                                CHANNEL_FLAG_IRRIGATION_METHOD_SET |
                                CHANNEL_FLAG_COVERAGE_SET;
        
        if ((channel_flags & required_flags) == required_flags) {
            has_configured_channel = true;
            break;
        }
    }
    
    /* Check essential system settings (RTC and timezone) */
    uint32_t required_system_flags = SYSTEM_FLAG_RTC_CONFIGURED | SYSTEM_FLAG_TIMEZONE_SET;
    bool has_essential_system = (current_state.system_config_flags & required_system_flags) == required_system_flags;
    
    /* Auto-set initial setup done when minimum requirements are met */
    if (has_configured_channel && has_essential_system) {
        current_state.system_config_flags |= SYSTEM_FLAG_INITIAL_SETUP_DONE;
        printk("Onboarding: INITIAL_SETUP_DONE auto-set (channel + RTC + timezone configured)\n");
        return true;
    }
    
    return false;
}

int onboarding_update_channel_flag(uint8_t channel_id, uint8_t flag, bool set) {
    if (channel_id >= 8) {
        return -EINVAL;
    }
    
    if (!state_initialized) {
        return -ENODEV;
    }
    
    k_mutex_lock(&onboarding_mutex, K_FOREVER);
    
    /* Flag is already a bitmask (e.g., 1<<0, 1<<1, etc.), so we need to find which bit it represents */
    /* Convert flag value to bit index: flag=1->0, flag=2->1, flag=4->2, flag=8->3, etc. */
    uint8_t flag_bit_index = 0;
    uint8_t temp_flag = flag;
    while (temp_flag > 1 && flag_bit_index < 8) {
        temp_flag >>= 1;
        flag_bit_index++;
    }
    
    /* Calculate absolute bit position in the 64-bit field */
    uint8_t bit_position = (channel_id * 8) + flag_bit_index;
    if (bit_position >= 64) {
        k_mutex_unlock(&onboarding_mutex);
        return -EINVAL;
    }
    
    uint64_t flag_mask = 1ULL << bit_position;
    
    if (set) {
        current_state.channel_config_flags |= flag_mask;
    } else {
        current_state.channel_config_flags &= ~flag_mask;
    }
    
    /* Check if initial setup should be auto-marked as done */
    check_and_set_initial_setup_done();
    
    /* Update timestamp and recalculate completion */
    current_state.last_update_time = get_current_timestamp();
    current_state.onboarding_completion_pct = onboarding_calculate_completion();

    /* Update config-complete extended bit for this channel */
    update_channel_complete_flag_locked(channel_id);
    
    /* Save to NVS */
    int ret = nvs_save_onboarding_state(&current_state);
    if (ret < 0) {
        printk("Failed to save onboarding state: %d\n", ret);
    }
    
    k_mutex_unlock(&onboarding_mutex);
    
    /* Schedule debounced BLE notification for onboarding progress update */
    schedule_onboarding_notify();
    
    printk("Channel %u flag %u %s\n", channel_id, flag, set ? "set" : "cleared");
    
    /* Check FAO-56 readiness if a relevant flag was set */
    if (set && (flag == CHANNEL_FLAG_PLANT_TYPE_SET || 
                flag == CHANNEL_FLAG_SOIL_TYPE_SET ||
                flag == CHANNEL_FLAG_IRRIGATION_METHOD_SET ||
                flag == CHANNEL_FLAG_COVERAGE_SET)) {
        onboarding_check_fao56_ready(channel_id);
    }
    
    return ret;
}

int onboarding_update_system_flag(uint32_t flag, bool set) {
    if (!state_initialized) {
        return -ENODEV;
    }
    
    k_mutex_lock(&onboarding_mutex, K_FOREVER);
    
    if (set) {
        current_state.system_config_flags |= flag;
    } else {
        current_state.system_config_flags &= ~flag;
    }
    
    /* Check if initial setup should be auto-marked as done */
    check_and_set_initial_setup_done();
    
    /* Update timestamp and recalculate completion */
    current_state.last_update_time = get_current_timestamp();
    current_state.onboarding_completion_pct = onboarding_calculate_completion();
    
    /* Save to NVS */
    int ret = nvs_save_onboarding_state(&current_state);
    if (ret < 0) {
        printk("Failed to save onboarding state: %d\n", ret);
    }
    
    k_mutex_unlock(&onboarding_mutex);
    
    /* Schedule debounced BLE notification for onboarding progress update */
    schedule_onboarding_notify();
    
    printk("System flag 0x%x %s\n", flag, set ? "set" : "cleared");
    return ret;
}

int onboarding_calculate_completion(void) {
    if (!state_initialized) {
        return 0;
    }
    
    return onboarding_calculate_completion_for(&current_state);
}

bool onboarding_is_complete(void) {
    if (!state_initialized) {
        return false;
    }
    
    /* Consider onboarding complete if we have at least:
     * - Basic configuration for at least one channel
     * - Essential system settings
     */
    
    /* Check if at least one channel has basic configuration */
    bool has_configured_channel = false;
    for (int ch = 0; ch < 8; ch++) {
        uint8_t channel_flags = onboarding_get_channel_flags(ch);
        uint8_t required_flags = CHANNEL_FLAG_PLANT_TYPE_SET | 
                                CHANNEL_FLAG_SOIL_TYPE_SET | 
                                CHANNEL_FLAG_IRRIGATION_METHOD_SET |
                                CHANNEL_FLAG_COVERAGE_SET;
        
        if ((channel_flags & required_flags) == required_flags) {
            has_configured_channel = true;
            break;
        }
    }
    
    /* Check essential system settings */
    uint32_t required_system_flags = SYSTEM_FLAG_RTC_CONFIGURED;
    bool has_system_config = (current_state.system_config_flags & required_system_flags) == required_system_flags;
    
    return has_configured_channel && has_system_config;
}

uint8_t onboarding_get_channel_flags(uint8_t channel_id) {
    if (channel_id >= 8 || !state_initialized) {
        return 0;
    }
    
    /* Extract 8 bits for this channel from the 64-bit field */
    uint8_t shift = channel_id * 8;
    return (current_state.channel_config_flags >> shift) & 0xFF;
}

uint32_t onboarding_get_system_flags(void) {
    if (!state_initialized) {
        return 0;
    }
    
    return current_state.system_config_flags;
}

uint8_t onboarding_get_schedule_flags(void) {
    if (!state_initialized) {
        return 0;
    }
    
    return current_state.schedule_config_flags;
}

int onboarding_update_schedule_flag(uint8_t channel_id, bool has_schedule) {
    if (channel_id >= 8) {
        return -EINVAL;
    }
    
    if (!state_initialized) {
        return -ENODEV;
    }
    
    k_mutex_lock(&onboarding_mutex, K_FOREVER);
    
    uint8_t flag_mask = 1 << channel_id;
    
    if (has_schedule) {
        current_state.schedule_config_flags |= flag_mask;
    } else {
        current_state.schedule_config_flags &= ~flag_mask;
    }
    
    /* Update timestamp and recalculate completion */
    current_state.last_update_time = get_current_timestamp();
    current_state.onboarding_completion_pct = onboarding_calculate_completion();

    /* Update config-complete extended bit for this channel */
    update_channel_complete_flag_locked(channel_id);
    
    /* Save to NVS */
    int ret = nvs_save_onboarding_state(&current_state);
    if (ret < 0) {
        printk("Failed to save onboarding state: %d\n", ret);
    }
    
    k_mutex_unlock(&onboarding_mutex);
    
    /* Schedule debounced BLE notification for onboarding progress update */
    schedule_onboarding_notify();
    
    printk("Channel %u schedule flag %s\n", channel_id, has_schedule ? "set" : "cleared");
    return ret;
}

int onboarding_reset_state(void) {
    if (!state_initialized) {
        return -ENODEV;
    }
    
    k_mutex_lock(&onboarding_mutex, K_FOREVER);
    
    /* Reset to default state */
    onboarding_state_t default_state = DEFAULT_ONBOARDING_STATE;
    current_state = default_state;
    current_state.onboarding_start_time = get_current_timestamp();
    current_state.last_update_time = current_state.onboarding_start_time;
    
    /* Save to NVS */
    int ret = nvs_save_onboarding_state(&current_state);
    if (ret < 0) {
        printk("Failed to save reset onboarding state: %d\n", ret);
    }
    
    k_mutex_unlock(&onboarding_mutex);
    
    printk("Onboarding state reset to defaults\n");
    return ret;
}

int onboarding_update_channel_extended_flag(uint8_t channel_id, uint8_t flag, bool set) {
    if (!state_initialized) {
        return -ENODEV;
    }
    
    if (channel_id >= 8) {
        return -EINVAL;
    }
    
    k_mutex_lock(&onboarding_mutex, K_FOREVER);
    
    /* Calculate bit position: 8 bits per channel */
    uint8_t bit_offset = channel_id * 8;
    uint64_t flag_mask = (uint64_t)flag << bit_offset;
    
    if (set) {
        current_state.channel_extended_flags |= flag_mask;
    } else {
        current_state.channel_extended_flags &= ~flag_mask;
    }
    
    /* Update timestamp */
    current_state.last_update_time = get_current_timestamp();

    /* Update config-complete extended bit for this channel */
    update_channel_complete_flag_locked(channel_id);
    
    /* Save to NVS */
    int ret = nvs_save_onboarding_state(&current_state);
    if (ret < 0) {
        printk("Failed to save onboarding state: %d\n", ret);
    }
    
    k_mutex_unlock(&onboarding_mutex);
    
    /* Schedule debounced BLE notification for onboarding progress update */
    schedule_onboarding_notify();
    
    printk("Channel %u extended flag 0x%x %s\n", channel_id, flag, set ? "set" : "cleared");
    return ret;
}

uint8_t onboarding_get_channel_extended_flags(uint8_t channel_id) {
    if (!state_initialized || channel_id >= 8) {
        return 0;
    }
    
    uint8_t bit_offset = channel_id * 8;
    return (uint8_t)((current_state.channel_extended_flags >> bit_offset) & 0xFF);
}

void onboarding_check_fao56_ready(uint8_t channel_id) {
    if (!state_initialized || channel_id >= 8) {
        return;
    }
    
    /* Get basic channel flags */
    uint8_t basic_flags = onboarding_get_channel_flags(channel_id);
    uint8_t extended_flags = onboarding_get_channel_extended_flags(channel_id);
    
    /* FAO-56 requires: plant type, soil type, irrigation method, coverage, and latitude */
    uint8_t required_basic = CHANNEL_FLAG_PLANT_TYPE_SET | 
                             CHANNEL_FLAG_SOIL_TYPE_SET | 
                             CHANNEL_FLAG_IRRIGATION_METHOD_SET |
                             CHANNEL_FLAG_COVERAGE_SET;
    
    bool basic_ok = (basic_flags & required_basic) == required_basic;
    bool latitude_ok = (extended_flags & CHANNEL_EXT_FLAG_LATITUDE_SET) != 0;
    
    bool fao56_ready = basic_ok && latitude_ok;
    
    /* Debug logging */
    printk("FAO56 check ch=%u: basic_flags=0x%02x (need 0x%02x), ext_flags=0x%02x, basic_ok=%d, lat_ok=%d, ready=%d\n",
           channel_id, basic_flags, required_basic, extended_flags, basic_ok, latitude_ok, fao56_ready);
    
    /* Update FAO-56 ready flag if state changed */
    bool current_fao56 = (extended_flags & CHANNEL_EXT_FLAG_FAO56_READY) != 0;
    if (fao56_ready != current_fao56) {
        onboarding_update_channel_extended_flag(channel_id, CHANNEL_EXT_FLAG_FAO56_READY, fao56_ready);
        printk("Channel %u FAO-56 ready: %s\n", channel_id, fao56_ready ? "YES" : "NO");
    }
}
