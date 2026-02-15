#include "soil_moisture_config.h"

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "nvs_config.h"

LOG_MODULE_REGISTER(soil_moisture_cfg, LOG_LEVEL_INF);

static bool initialized;

static bool global_has_data;
static bool channel_has_data[WATERING_CHANNELS_COUNT];

static soil_moisture_global_config_t global_cfg;
static soil_moisture_channel_override_t channel_cfg[WATERING_CHANNELS_COUNT];

static uint8_t clamp_pct(uint8_t pct)
{
    return (pct > 100u) ? 100u : pct;
}

static bool soil_moisture_any_channel_override_enabled(void)
{
    for (uint8_t ch = 0; ch < WATERING_CHANNELS_COUNT; ch++) {
        if (channel_cfg[ch].override_enabled != 0u) {
            return true;
        }
    }
    return false;
}

static bool soil_moisture_any_channel_has_data(void)
{
    for (uint8_t ch = 0; ch < WATERING_CHANNELS_COUNT; ch++) {
        if (channel_has_data[ch]) {
            return true;
        }
    }
    return false;
}

static uint8_t soil_moisture_get_global_derived_pct(void)
{
    uint16_t sum = 0u;
    uint8_t count = 0u;

    for (uint8_t ch = 0; ch < WATERING_CHANNELS_COUNT; ch++) {
        if (channel_cfg[ch].override_enabled != 0u) {
            sum += clamp_pct(channel_cfg[ch].moisture_pct);
            count++;
        }
    }

    if (count == 0u) {
        return SOIL_MOISTURE_DEFAULT_PCT;
    }

    return (uint8_t)(sum / count);
}

int soil_moisture_config_init(void)
{
    int ret = nvs_load_soil_moisture_global_config(&global_cfg);
    if (ret < 0) {
        global_has_data = false;
        if (ret != -ENOENT) {
            LOG_WRN("Failed to load global soil moisture config: %d", ret);
        }
        global_cfg = (soil_moisture_global_config_t)DEFAULT_SOIL_MOISTURE_GLOBAL_CONFIG;

        /*
         * If the record is missing, seed defaults once so clients don't have to
         * explicitly "set" soil moisture just to avoid a missing/unconfigured UI.
         *
         * Keep defaults disabled so computation behavior remains unchanged
         * (effective moisture still falls back to SOIL_MOISTURE_DEFAULT_PCT).
         */
        if (ret == -ENOENT) {
            int seed_ret = nvs_save_soil_moisture_global_config(&global_cfg);
            if (seed_ret < 0) {
                LOG_WRN("Failed to seed default global soil moisture config: %d", seed_ret);
            } else {
                global_has_data = true;
            }
        }
    } else {
        global_has_data = true;
    }

    for (uint8_t ch = 0; ch < WATERING_CHANNELS_COUNT; ch++) {
        ret = nvs_load_soil_moisture_channel_override(ch, &channel_cfg[ch]);
        if (ret < 0) {
            channel_has_data[ch] = false;
            if (ret != -ENOENT) {
                LOG_WRN("Failed to load soil moisture override for ch%u: %d", ch, ret);
            }
            channel_cfg[ch] = (soil_moisture_channel_override_t)DEFAULT_SOIL_MOISTURE_CHANNEL_OVERRIDE;

            if (ret == -ENOENT) {
                int seed_ret = nvs_save_soil_moisture_channel_override(ch, &channel_cfg[ch]);
                if (seed_ret < 0) {
                    LOG_WRN("Failed to seed default soil moisture override for ch%u: %d", ch, seed_ret);
                } else {
                    channel_has_data[ch] = true;
                }
            }
        } else {
            channel_has_data[ch] = true;
        }
    }

    initialized = true;
    LOG_INF("Soil moisture config ready (global=%u/%u%%)", global_cfg.enabled, global_cfg.moisture_pct);
    return 0;
}

uint8_t soil_moisture_get_global_effective_pct(void)
{
    if (!initialized) {
        (void)soil_moisture_config_init();
    }

    return soil_moisture_get_global_derived_pct();
}

uint8_t soil_moisture_get_effective_pct(uint8_t channel_id)
{
    if (initialized && channel_id < WATERING_CHANNELS_COUNT) {
        if (channel_cfg[channel_id].override_enabled) {
            return clamp_pct(channel_cfg[channel_id].moisture_pct);
        }
    }
    return SOIL_MOISTURE_DEFAULT_PCT;
}

int soil_moisture_get_global(bool *enabled, uint8_t *moisture_pct)
{
    if (!enabled || !moisture_pct) {
        return -EINVAL;
    }

    if (!initialized) {
        (void)soil_moisture_config_init();
    }

    *enabled = soil_moisture_any_channel_override_enabled();
    *moisture_pct = soil_moisture_get_global_derived_pct();
    return 0;
}

int soil_moisture_get_global_with_presence(bool *enabled, uint8_t *moisture_pct, bool *has_data)
{
    if (!enabled || !moisture_pct || !has_data) {
        return -EINVAL;
    }

    if (!initialized) {
        (void)soil_moisture_config_init();
    }

    *enabled = soil_moisture_any_channel_override_enabled();
    *moisture_pct = soil_moisture_get_global_derived_pct();
    *has_data = soil_moisture_any_channel_has_data();
    return 0;
}

int soil_moisture_set_global(bool enabled, uint8_t moisture_pct)
{
    ARG_UNUSED(enabled);
    ARG_UNUSED(moisture_pct);

    if (!initialized) {
        (void)soil_moisture_config_init();
    }

    LOG_INF("Ignoring manual global soil moisture set; global is derived from channel overrides");

    return 0;
}

int soil_moisture_get_channel_override(uint8_t channel_id, bool *enabled, uint8_t *moisture_pct)
{
    if (!enabled || !moisture_pct || channel_id >= WATERING_CHANNELS_COUNT) {
        return -EINVAL;
    }

    if (!initialized) {
        (void)soil_moisture_config_init();
    }

    *enabled = (channel_cfg[channel_id].override_enabled != 0);
    *moisture_pct = clamp_pct(channel_cfg[channel_id].moisture_pct);
    return 0;
}

int soil_moisture_get_channel_override_with_presence(uint8_t channel_id, bool *enabled, uint8_t *moisture_pct, bool *has_data)
{
    if (!enabled || !moisture_pct || !has_data || channel_id >= WATERING_CHANNELS_COUNT) {
        return -EINVAL;
    }

    if (!initialized) {
        (void)soil_moisture_config_init();
    }

    *enabled = (channel_cfg[channel_id].override_enabled != 0);
    *moisture_pct = clamp_pct(channel_cfg[channel_id].moisture_pct);
    *has_data = channel_has_data[channel_id];
    return 0;
}

int soil_moisture_set_channel_override(uint8_t channel_id, bool enabled, uint8_t moisture_pct)
{
    if (channel_id >= WATERING_CHANNELS_COUNT) {
        return -EINVAL;
    }

    if (!initialized) {
        (void)soil_moisture_config_init();
    }

    channel_cfg[channel_id].override_enabled = enabled ? 1u : 0u;
    channel_cfg[channel_id].moisture_pct = clamp_pct(moisture_pct);

    int ret = nvs_save_soil_moisture_channel_override(channel_id, &channel_cfg[channel_id]);
    if (ret < 0) {
        LOG_WRN("Failed to save soil moisture override for ch%u: %d", channel_id, ret);
        return ret;
    }

    channel_has_data[channel_id] = true;

    return 0;
}
