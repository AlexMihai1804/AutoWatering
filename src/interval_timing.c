#include "interval_timing.h"
#include <stdio.h>
#include <zephyr/logging/log.h>
#include <zephyr/kernel.h>
#include <string.h>

LOG_MODULE_REGISTER(interval_timing, LOG_LEVEL_DBG);

int interval_timing_init_config(interval_config_t *config)
{
    if (!config) {
        LOG_ERR("Invalid config pointer");
        return -EINVAL;
    }

    memset(config, 0, sizeof(interval_config_t));
    
    config->watering_minutes = INTERVAL_DEFAULT_WATERING_MIN;
    config->watering_seconds = INTERVAL_DEFAULT_WATERING_SEC;
    config->pause_minutes = INTERVAL_DEFAULT_PAUSE_MIN;
    config->pause_seconds = INTERVAL_DEFAULT_PAUSE_SEC;
    config->configured = false;
    config->currently_watering = false;
    config->cycles_completed = 0;
    config->total_target = 0;
    config->phase_start_time = 0;
    config->phase_remaining_sec = 0;

    LOG_DBG("Interval timing config initialized with defaults: %u:%02u water, %u:%02u pause",
            config->watering_minutes, config->watering_seconds,
            config->pause_minutes, config->pause_seconds);

    return 0;
}

int interval_timing_validate_config(const interval_config_t *config)
{
    if (!config) {
        LOG_ERR("Invalid config pointer");
        return -EINVAL;
    }

    // Validate watering duration
    int ret = interval_timing_validate_values(config->watering_minutes, config->watering_seconds);
    if (ret != 0) {
        LOG_ERR("Invalid watering duration: %u:%02u", 
                config->watering_minutes, config->watering_seconds);
        return ret;
    }

    // Validate pause duration
    ret = interval_timing_validate_values(config->pause_minutes, config->pause_seconds);
    if (ret != 0) {
        LOG_ERR("Invalid pause duration: %u:%02u",
                config->pause_minutes, config->pause_seconds);
        return ret;
    }

    // Check that both durations are within total limits
    uint32_t watering_sec = interval_get_watering_duration_sec(config);
    uint32_t pause_sec = interval_get_pause_duration_sec(config);

    if (watering_sec < INTERVAL_MIN_DURATION_SEC || watering_sec > INTERVAL_MAX_DURATION_SEC) {
        LOG_ERR("Watering duration out of range: %u seconds", watering_sec);
        return -EINVAL;
    }

    if (pause_sec < INTERVAL_MIN_DURATION_SEC || pause_sec > INTERVAL_MAX_DURATION_SEC) {
        LOG_ERR("Pause duration out of range: %u seconds", pause_sec);
        return -EINVAL;
    }

    return 0;
}

int interval_timing_set_watering_duration(interval_config_t *config,
                                          uint16_t minutes,
                                          uint8_t seconds)
{
    if (!config) {
        LOG_ERR("Invalid config pointer");
        return -EINVAL;
    }

    int ret = interval_timing_validate_values(minutes, seconds);
    if (ret != 0) {
        LOG_ERR("Invalid watering duration values: %u:%02u", minutes, seconds);
        return ret;
    }

    config->watering_minutes = minutes;
    config->watering_seconds = seconds;

    LOG_DBG("Set watering duration: %u:%02u", minutes, seconds);
    return 0;
}

int interval_timing_set_pause_duration(interval_config_t *config,
                                      uint16_t minutes,
                                      uint8_t seconds)
{
    if (!config) {
        LOG_ERR("Invalid config pointer");
        return -EINVAL;
    }

    int ret = interval_timing_validate_values(minutes, seconds);
    if (ret != 0) {
        LOG_ERR("Invalid pause duration values: %u:%02u", minutes, seconds);
        return ret;
    }

    config->pause_minutes = minutes;
    config->pause_seconds = seconds;

    LOG_DBG("Set pause duration: %u:%02u", minutes, seconds);
    return 0;
}

int interval_timing_get_watering_duration(const interval_config_t *config,
                                          uint16_t *minutes,
                                          uint8_t *seconds)
{
    if (!config || !minutes || !seconds) {
        LOG_ERR("Invalid parameters");
        return -EINVAL;
    }

    *minutes = config->watering_minutes;
    *seconds = config->watering_seconds;

    return 0;
}

int interval_timing_get_pause_duration(const interval_config_t *config,
                                      uint16_t *minutes,
                                      uint8_t *seconds)
{
    if (!config || !minutes || !seconds) {
        LOG_ERR("Invalid parameters");
        return -EINVAL;
    }

    *minutes = config->pause_minutes;
    *seconds = config->pause_seconds;

    return 0;
}

int interval_timing_convert_to_seconds(uint16_t minutes,
                                      uint8_t seconds,
                                      uint32_t *total_seconds)
{
    if (!total_seconds) {
        LOG_ERR("Invalid total_seconds pointer");
        return -EINVAL;
    }

    int ret = interval_timing_validate_values(minutes, seconds);
    if (ret != 0) {
        return ret;
    }

    *total_seconds = (minutes * 60) + seconds;
    return 0;
}

int interval_timing_convert_from_seconds(uint32_t total_seconds,
                                        uint16_t *minutes,
                                        uint8_t *seconds)
{
    if (!minutes || !seconds) {
        LOG_ERR("Invalid parameters");
        return -EINVAL;
    }

    if (total_seconds > INTERVAL_MAX_DURATION_SEC) {
        LOG_ERR("Total seconds too large: %u", total_seconds);
        return -EINVAL;
    }

    *minutes = (uint16_t)(total_seconds / 60);
    *seconds = (uint8_t)(total_seconds % 60);

    return 0;
}

int interval_timing_get_cycle_duration(const interval_config_t *config,
                                      uint32_t *cycle_duration_sec)
{
    if (!config || !cycle_duration_sec) {
        LOG_ERR("Invalid parameters");
        return -EINVAL;
    }

    int ret = interval_timing_validate_config(config);
    if (ret != 0) {
        return ret;
    }

    uint32_t watering_sec = interval_get_watering_duration_sec(config);
    uint32_t pause_sec = interval_get_pause_duration_sec(config);

    *cycle_duration_sec = watering_sec + pause_sec;

    LOG_DBG("Cycle duration: %u seconds (water=%u, pause=%u)",
            *cycle_duration_sec, watering_sec, pause_sec);

    return 0;
}

int interval_timing_calculate_cycles(const interval_config_t *config,
                                    uint32_t target_duration_sec,
                                    uint32_t *cycle_count,
                                    uint32_t *remaining_sec)
{
    if (!config || !cycle_count || !remaining_sec) {
        LOG_ERR("Invalid parameters");
        return -EINVAL;
    }

    uint32_t cycle_duration_sec;
    int ret = interval_timing_get_cycle_duration(config, &cycle_duration_sec);
    if (ret != 0) {
        return ret;
    }

    if (cycle_duration_sec == 0) {
        LOG_ERR("Invalid cycle duration: 0 seconds");
        return -EINVAL;
    }

    *cycle_count = target_duration_sec / cycle_duration_sec;
    *remaining_sec = target_duration_sec % cycle_duration_sec;

    LOG_DBG("Calculated cycles: %u complete cycles, %u seconds remaining (target=%u, cycle=%u)",
            *cycle_count, *remaining_sec, target_duration_sec, cycle_duration_sec);

    return 0;
}

int interval_timing_calculate_cycles_for_volume(const interval_config_t *config,
                                               uint32_t target_volume_ml,
                                               float flow_rate_ml_sec,
                                               uint32_t *cycle_count,
                                               uint32_t *remaining_ml)
{
    if (!config || !cycle_count || !remaining_ml) {
        LOG_ERR("Invalid parameters");
        return -EINVAL;
    }

    if (flow_rate_ml_sec <= 0.0f) {
        LOG_ERR("Invalid flow rate: %.3f ml/sec", (double)flow_rate_ml_sec);
        return -EINVAL;
    }

    // Calculate volume per watering phase
    uint32_t watering_sec = interval_get_watering_duration_sec(config);
    uint32_t volume_per_watering = (uint32_t)(watering_sec * flow_rate_ml_sec);

    if (volume_per_watering == 0) {
        LOG_ERR("Volume per watering phase is zero");
        return -EINVAL;
    }

    *cycle_count = target_volume_ml / volume_per_watering;
    *remaining_ml = target_volume_ml % volume_per_watering;

    LOG_DBG("Calculated volume cycles: %u complete cycles, %u ml remaining (target=%u, per_cycle=%u)",
            *cycle_count, *remaining_ml, target_volume_ml, volume_per_watering);

    return 0;
}

int interval_timing_update_config(interval_config_t *config,
                                 uint16_t watering_min,
                                 uint8_t watering_sec,
                                 uint16_t pause_min,
                                 uint8_t pause_sec)
{
    if (!config) {
        LOG_ERR("Invalid config pointer");
        return -EINVAL;
    }

    // Validate all values first
    int ret = interval_timing_validate_values(watering_min, watering_sec);
    if (ret != 0) {
        LOG_ERR("Invalid watering duration: %u:%02u", watering_min, watering_sec);
        return ret;
    }

    ret = interval_timing_validate_values(pause_min, pause_sec);
    if (ret != 0) {
        LOG_ERR("Invalid pause duration: %u:%02u", pause_min, pause_sec);
        return ret;
    }

    // Update configuration
    config->watering_minutes = watering_min;
    config->watering_seconds = watering_sec;
    config->pause_minutes = pause_min;
    config->pause_seconds = pause_sec;
    config->configured = true;

    LOG_INF("Updated interval config: water=%u:%02u, pause=%u:%02u",
            watering_min, watering_sec, pause_min, pause_sec);

    return 0;
}

int interval_timing_clear_config(interval_config_t *config)
{
    if (!config) {
        LOG_ERR("Invalid config pointer");
        return -EINVAL;
    }

    // Reset to defaults but mark as not configured
    interval_timing_init_config(config);
    config->configured = false;

    LOG_DBG("Cleared interval timing configuration");
    return 0;
}

int interval_timing_is_configured(const interval_config_t *config,
                                 bool *is_configured)
{
    if (!config || !is_configured) {
        LOG_ERR("Invalid parameters");
        return -EINVAL;
    }

    *is_configured = config->configured && 
                    interval_timing_validate_config(config) == 0;

    return 0;
}

int interval_timing_get_description(const interval_config_t *config,
                                   char *description)
{
    if (!config || !description) {
        LOG_ERR("Invalid parameters");
        return -EINVAL;
    }

    if (!config->configured) {
        snprintf(description, 128, "Interval mode not configured");
        return 0;
    }

    uint32_t watering_sec = interval_get_watering_duration_sec(config);
    uint32_t pause_sec = interval_get_pause_duration_sec(config);
    uint32_t cycle_sec = watering_sec + pause_sec;

    snprintf(description, 128, 
             "Interval: %u:%02u water, %u:%02u pause (cycle: %u:%02u)",
             config->watering_minutes, config->watering_seconds,
             config->pause_minutes, config->pause_seconds,
             cycle_sec / 60, cycle_sec % 60);

    return 0;
}

int interval_timing_validate_values(uint16_t minutes, uint8_t seconds)
{
    if (minutes > INTERVAL_MAX_MINUTES) {
        LOG_ERR("Minutes value too large: %u (max %u)", minutes, INTERVAL_MAX_MINUTES);
        return -EINVAL;
    }

    if (seconds > INTERVAL_MAX_SECONDS) {
        LOG_ERR("Seconds value too large: %u (max %u)", seconds, INTERVAL_MAX_SECONDS);
        return -EINVAL;
    }

    // Check total duration
    uint32_t total_sec = (minutes * 60) + seconds;
    if (total_sec < INTERVAL_MIN_DURATION_SEC) {
        LOG_ERR("Total duration too small: %u seconds (min %u)", 
                total_sec, INTERVAL_MIN_DURATION_SEC);
        return -EINVAL;
    }

    if (total_sec > INTERVAL_MAX_DURATION_SEC) {
        LOG_ERR("Total duration too large: %u seconds (max %u)",
                total_sec, INTERVAL_MAX_DURATION_SEC);
        return -EINVAL;
    }

    return 0;
}

int interval_timing_get_phase_remaining(const interval_config_t *config,
                                       uint32_t phase_start_time,
                                       bool currently_watering,
                                       uint32_t *remaining_sec)
{
    if (!config || !remaining_sec) {
        LOG_ERR("Invalid parameters");
        return -EINVAL;
    }

    uint32_t current_time = k_uptime_get_32();
    uint32_t elapsed_sec = (current_time - phase_start_time) / 1000;

    uint32_t phase_duration_sec;
    if (currently_watering) {
        phase_duration_sec = interval_get_watering_duration_sec(config);
    } else {
        phase_duration_sec = interval_get_pause_duration_sec(config);
    }

    if (elapsed_sec >= phase_duration_sec) {
        *remaining_sec = 0;
    } else {
        *remaining_sec = phase_duration_sec - elapsed_sec;
    }

    return 0;
}

int interval_timing_update_phase(interval_config_t *config,
                                bool currently_watering,
                                uint32_t phase_start_time)
{
    if (!config) {
        LOG_ERR("Invalid config pointer");
        return -EINVAL;
    }

    config->currently_watering = currently_watering;
    config->phase_start_time = phase_start_time;

    // Update remaining time
    int ret = interval_timing_get_phase_remaining(config, phase_start_time, 
                                                 currently_watering, 
                                                 &config->phase_remaining_sec);
    if (ret != 0) {
        LOG_ERR("Failed to calculate phase remaining time");
        return ret;
    }

    LOG_DBG("Updated phase: %s, start=%u, remaining=%u sec",
            currently_watering ? "watering" : "pausing",
            phase_start_time, config->phase_remaining_sec);

    return 0;
}

int interval_timing_should_switch_phase(const interval_config_t *config,
                                       bool *should_switch)
{
    if (!config || !should_switch) {
        LOG_ERR("Invalid parameters");
        return -EINVAL;
    }

    uint32_t remaining_sec;
    int ret = interval_timing_get_phase_remaining(config, config->phase_start_time,
                                                 config->currently_watering, &remaining_sec);
    if (ret != 0) {
        return ret;
    }

    *should_switch = (remaining_sec == 0);

    LOG_DBG("Phase switch check: remaining=%u sec, should_switch=%s",
            remaining_sec, *should_switch ? "yes" : "no");

    return 0;
}

int interval_timing_reset_state(interval_config_t *config)
{
    if (!config) {
        LOG_ERR("Invalid config pointer");
        return -EINVAL;
    }

    config->cycles_completed = 0;
    config->currently_watering = false;
    config->phase_start_time = 0;
    config->phase_remaining_sec = 0;
    config->total_target = 0;

    LOG_DBG("Reset interval timing state");
    return 0;
}