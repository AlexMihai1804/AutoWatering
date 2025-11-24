/**
 * @file custom_soil_db.c
 * @brief Custom soil database management implementation
 */

#include "custom_soil_db.h"
#include "nvs_config.h"
#include "soil_enhanced_db.inc"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <math.h>

LOG_MODULE_REGISTER(custom_soil_db, LOG_LEVEL_DBG);

/* NVS key definitions for custom soil storage */
#define NVS_CUSTOM_SOIL_BASE_KEY    0x4000
#define NVS_CUSTOM_SOIL_KEY(ch)     (NVS_CUSTOM_SOIL_BASE_KEY + (ch))

/* Validation constants */
#define MIN_FIELD_CAPACITY          5.0f    // Minimum 5%
#define MAX_FIELD_CAPACITY          80.0f   // Maximum 80%
#define MIN_WILTING_POINT           1.0f    // Minimum 1%
#define MAX_WILTING_POINT           40.0f   // Maximum 40%
#define MIN_INFILTRATION_RATE       0.1f    // Minimum 0.1 mm/hr
#define MAX_INFILTRATION_RATE       1000.0f // Maximum 1000 mm/hr
#define MIN_BULK_DENSITY            0.5f    // Minimum 0.5 g/cm³
#define MAX_BULK_DENSITY            2.5f    // Maximum 2.5 g/cm³
#define MIN_ORGANIC_MATTER          0.0f    // Minimum 0%
#define MAX_ORGANIC_MATTER          100.0f  // Maximum 100%

/* CRC32 calculation for data integrity */
static uint32_t calculate_crc32(const void *data, size_t length);

watering_error_t custom_soil_db_init(void)
{
    LOG_INF("Initializing custom soil database");
    
    /* NVS should already be initialized by the main system */
    if (!nvs_config_is_ready()) {
        LOG_ERR("NVS not ready for custom soil database");
        return WATERING_ERROR_STORAGE;
    }
    
    LOG_INF("Custom soil database initialized successfully");
    return WATERING_SUCCESS;
}

watering_error_t custom_soil_db_validate_parameters(float field_capacity,
                                                   float wilting_point,
                                                   float infiltration_rate,
                                                   float bulk_density,
                                                   float organic_matter)
{
    /* Validate field capacity */
    if (field_capacity < MIN_FIELD_CAPACITY || field_capacity > MAX_FIELD_CAPACITY) {
        LOG_ERR("Invalid field capacity: %.2f (range: %.1f-%.1f)", 
                (double)field_capacity, (double)MIN_FIELD_CAPACITY, (double)MAX_FIELD_CAPACITY);
        return WATERING_ERROR_CUSTOM_SOIL_INVALID;
    }
    
    /* Validate wilting point */
    if (wilting_point < MIN_WILTING_POINT || wilting_point > MAX_WILTING_POINT) {
        LOG_ERR("Invalid wilting point: %.2f (range: %.1f-%.1f)", 
                (double)wilting_point, (double)MIN_WILTING_POINT, (double)MAX_WILTING_POINT);
        return WATERING_ERROR_CUSTOM_SOIL_INVALID;
    }
    
    /* Wilting point must be less than field capacity */
    if (wilting_point >= field_capacity) {
        LOG_ERR("Wilting point (%.2f) must be less than field capacity (%.2f)", 
                (double)wilting_point, (double)field_capacity);
        return WATERING_ERROR_CUSTOM_SOIL_INVALID;
    }
    
    /* Validate infiltration rate */
    if (infiltration_rate < MIN_INFILTRATION_RATE || infiltration_rate > MAX_INFILTRATION_RATE) {
        LOG_ERR("Invalid infiltration rate: %.2f (range: %.1f-%.1f)", 
                (double)infiltration_rate, (double)MIN_INFILTRATION_RATE, (double)MAX_INFILTRATION_RATE);
        return WATERING_ERROR_CUSTOM_SOIL_INVALID;
    }
    
    /* Validate bulk density */
    if (bulk_density < MIN_BULK_DENSITY || bulk_density > MAX_BULK_DENSITY) {
        LOG_ERR("Invalid bulk density: %.2f (range: %.1f-%.1f)", 
                (double)bulk_density, (double)MIN_BULK_DENSITY, (double)MAX_BULK_DENSITY);
        return WATERING_ERROR_CUSTOM_SOIL_INVALID;
    }
    
    /* Validate organic matter */
    if (organic_matter < MIN_ORGANIC_MATTER || organic_matter > MAX_ORGANIC_MATTER) {
        LOG_ERR("Invalid organic matter: %.2f (range: %.1f-%.1f)", 
                (double)organic_matter, (double)MIN_ORGANIC_MATTER, (double)MAX_ORGANIC_MATTER);
        return WATERING_ERROR_CUSTOM_SOIL_INVALID;
    }
    
    return WATERING_SUCCESS;
}

watering_error_t custom_soil_db_create(uint8_t channel_id,
                                      const char *name,
                                      float field_capacity,
                                      float wilting_point,
                                      float infiltration_rate,
                                      float bulk_density,
                                      float organic_matter)
{
    if (channel_id >= WATERING_CHANNELS_COUNT) {
        LOG_ERR("Invalid channel ID: %d", channel_id);
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    if (!name || strlen(name) == 0 || strlen(name) >= sizeof(((custom_soil_entry_t*)0)->name)) {
        LOG_ERR("Invalid soil name");
        return WATERING_ERROR_CUSTOM_SOIL_INVALID;
    }
    
    /* Validate parameters */
    watering_error_t err = custom_soil_db_validate_parameters(field_capacity, wilting_point,
                                                            infiltration_rate, bulk_density,
                                                            organic_matter);
    if (err != WATERING_SUCCESS) {
        return err;
    }
    
    /* Create custom soil entry */
    custom_soil_entry_t entry = {0};
    entry.channel_id = channel_id;
    strncpy(entry.name, name, sizeof(entry.name) - 1);
    entry.name[sizeof(entry.name) - 1] = '\0';
    entry.field_capacity = field_capacity;
    entry.wilting_point = wilting_point;
    entry.infiltration_rate = infiltration_rate;
    entry.bulk_density = bulk_density;
    entry.organic_matter = organic_matter;
    entry.created_timestamp = k_uptime_get_32();
    entry.modified_timestamp = entry.created_timestamp;
    entry.crc32 = calculate_crc32(&entry, sizeof(entry) - sizeof(entry.crc32));
    
    /* Save to NVS */
    int ret = nvs_config_write(NVS_CUSTOM_SOIL_KEY(channel_id), &entry, sizeof(entry));
    if (ret < 0) {
        LOG_ERR("Failed to save custom soil for channel %d: %d", channel_id, ret);
        return WATERING_ERROR_STORAGE;
    }
    
    LOG_INF("Created custom soil '%s' for channel %d", name, channel_id);
    return WATERING_SUCCESS;
}

watering_error_t custom_soil_db_read(uint8_t channel_id, custom_soil_entry_t *entry)
{
    if (channel_id >= WATERING_CHANNELS_COUNT) {
        LOG_ERR("Invalid channel ID: %d", channel_id);
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    if (!entry) {
        LOG_ERR("NULL entry pointer");
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    /* Read from NVS */
    int ret = nvs_config_read(NVS_CUSTOM_SOIL_KEY(channel_id), entry, sizeof(*entry));
    if (ret < 0) {
        if (ret == -ENOENT) {
            LOG_DBG("No custom soil found for channel %d", channel_id);
            return WATERING_ERROR_INVALID_DATA;
        }
        LOG_ERR("Failed to read custom soil for channel %d: %d", channel_id, ret);
        return WATERING_ERROR_STORAGE;
    }
    
    /* Verify data integrity */
    uint32_t calculated_crc = calculate_crc32(entry, sizeof(*entry) - sizeof(entry->crc32));
    if (calculated_crc != entry->crc32) {
        LOG_ERR("Custom soil data corruption detected for channel %d", channel_id);
        return WATERING_ERROR_ENV_DATA_CORRUPT;
    }
    
    /* Verify channel ID matches */
    if (entry->channel_id != channel_id) {
        LOG_ERR("Channel ID mismatch in custom soil data: expected %d, got %d", 
                channel_id, entry->channel_id);
        return WATERING_ERROR_ENV_DATA_CORRUPT;
    }
    
    LOG_DBG("Read custom soil '%s' for channel %d", entry->name, channel_id);
    return WATERING_SUCCESS;
}

watering_error_t custom_soil_db_update(uint8_t channel_id,
                                      const char *name,
                                      float field_capacity,
                                      float wilting_point,
                                      float infiltration_rate,
                                      float bulk_density,
                                      float organic_matter)
{
    /* Read existing entry to preserve timestamps */
    custom_soil_entry_t entry;
    watering_error_t err = custom_soil_db_read(channel_id, &entry);
    if (err != WATERING_SUCCESS) {
        LOG_ERR("Cannot update non-existent custom soil for channel %d", channel_id);
        return err;
    }
    
    if (!name || strlen(name) == 0 || strlen(name) >= sizeof(entry.name)) {
        LOG_ERR("Invalid soil name for update");
        return WATERING_ERROR_CUSTOM_SOIL_INVALID;
    }
    
    /* Validate new parameters */
    err = custom_soil_db_validate_parameters(field_capacity, wilting_point,
                                           infiltration_rate, bulk_density,
                                           organic_matter);
    if (err != WATERING_SUCCESS) {
        return err;
    }
    
    /* Update entry */
    strncpy(entry.name, name, sizeof(entry.name) - 1);
    entry.name[sizeof(entry.name) - 1] = '\0';
    entry.field_capacity = field_capacity;
    entry.wilting_point = wilting_point;
    entry.infiltration_rate = infiltration_rate;
    entry.bulk_density = bulk_density;
    entry.organic_matter = organic_matter;
    entry.modified_timestamp = k_uptime_get_32();
    entry.crc32 = calculate_crc32(&entry, sizeof(entry) - sizeof(entry.crc32));
    
    /* Save updated entry */
    int ret = nvs_config_write(NVS_CUSTOM_SOIL_KEY(channel_id), &entry, sizeof(entry));
    if (ret < 0) {
        LOG_ERR("Failed to update custom soil for channel %d: %d", channel_id, ret);
        return WATERING_ERROR_STORAGE;
    }
    
    LOG_INF("Updated custom soil '%s' for channel %d", name, channel_id);
    return WATERING_SUCCESS;
}

watering_error_t custom_soil_db_delete(uint8_t channel_id)
{
    if (channel_id >= WATERING_CHANNELS_COUNT) {
        LOG_ERR("Invalid channel ID: %d", channel_id);
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    /* Delete from NVS */
    int ret = nvs_config_delete(NVS_CUSTOM_SOIL_KEY(channel_id));
    if (ret < 0 && ret != -ENOENT) {
        LOG_ERR("Failed to delete custom soil for channel %d: %d", channel_id, ret);
        return WATERING_ERROR_STORAGE;
    }
    
    LOG_INF("Deleted custom soil for channel %d", channel_id);
    return WATERING_SUCCESS;
}

bool custom_soil_db_exists(uint8_t channel_id)
{
    if (channel_id >= WATERING_CHANNELS_COUNT) {
        return false;
    }
    
    custom_soil_entry_t entry;
    watering_error_t err = custom_soil_db_read(channel_id, &entry);
    return (err == WATERING_SUCCESS);
}

float custom_soil_db_get_awc(const custom_soil_entry_t *entry)
{
    if (!entry) {
        return 0.0f;
    }
    
    /* Available Water Capacity = Field Capacity - Wilting Point */
    float awc_percent = entry->field_capacity - entry->wilting_point;
    
    /* Convert to mm/m assuming typical soil depth relationships */
    /* This is a simplified calculation - in reality it depends on soil depth */
    float awc_mm_per_m = awc_percent * 10.0f; // Rough conversion
    
    return awc_mm_per_m;
}

float custom_soil_db_get_depletion_fraction(const custom_soil_entry_t *entry)
{
    if (!entry) {
        return 0.5f; // Default depletion fraction
    }
    
    /* Estimate depletion fraction based on soil properties */
    /* Sandy soils (low field capacity) have higher depletion fractions */
    /* Clay soils (high field capacity) have lower depletion fractions */
    
    float base_fraction = 0.5f;
    
    /* Adjust based on field capacity */
    if (entry->field_capacity < 15.0f) {
        base_fraction = 0.7f; // Sandy soil - can deplete more
    } else if (entry->field_capacity > 35.0f) {
        base_fraction = 0.3f; // Clay soil - should not deplete as much
    }
    
    /* Adjust based on organic matter (higher OM = better water retention) */
    if (entry->organic_matter > 5.0f) {
        base_fraction *= 0.9f; // Reduce depletion fraction for high OM
    }
    
    /* Ensure reasonable bounds */
    if (base_fraction < 0.2f) base_fraction = 0.2f;
    if (base_fraction > 0.8f) base_fraction = 0.8f;
    
    return base_fraction;
}

watering_error_t custom_soil_db_to_enhanced_format(const custom_soil_entry_t *entry,
                                                  soil_enhanced_data_t *soil_data)
{
    if (!entry || !soil_data) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    /* Clear the output structure */
    memset(soil_data, 0, sizeof(*soil_data));
    
    /* Fill in the enhanced format structure */
    soil_data->soil_id = 255; // Special ID for custom soil
    soil_data->soil_type = entry->name; // Note: This is a pointer, not a copy
    soil_data->texture = "Custom"; // Generic texture description
    
    /* Convert percentages to scaled integers as used in the database */
    soil_data->fc_pctvol_x100 = (uint16_t)(entry->field_capacity * 100.0f);
    soil_data->pwp_pctvol_x100 = (uint16_t)(entry->wilting_point * 100.0f);
    soil_data->awc_mm_per_m = (uint16_t)custom_soil_db_get_awc(entry);
    soil_data->infil_mm_h = (uint16_t)entry->infiltration_rate;
    soil_data->p_raw_x1000 = (uint16_t)(custom_soil_db_get_depletion_fraction(entry) * 1000.0f);
    
    return WATERING_SUCCESS;
}

watering_error_t custom_soil_db_get_all(custom_soil_entry_t entries[8], uint8_t *count)
{
    if (!entries || !count) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    *count = 0;
    
    for (uint8_t ch = 0; ch < WATERING_CHANNELS_COUNT; ch++) {
        watering_error_t err = custom_soil_db_read(ch, &entries[*count]);
        if (err == WATERING_SUCCESS) {
            (*count)++;
        } else if (err != WATERING_ERROR_INVALID_DATA) {
            LOG_ERR("Error reading custom soil for channel %d: %d", ch, err);
            return err;
        }
    }
    
    LOG_DBG("Found %d custom soil configurations", *count);
    return WATERING_SUCCESS;
}

watering_error_t custom_soil_db_clear_all(void)
{
    watering_error_t last_error = WATERING_SUCCESS;
    
    for (uint8_t ch = 0; ch < WATERING_CHANNELS_COUNT; ch++) {
        watering_error_t err = custom_soil_db_delete(ch);
        if (err != WATERING_SUCCESS) {
            LOG_ERR("Failed to delete custom soil for channel %d", ch);
            last_error = err;
        }
    }
    
    LOG_INF("Cleared all custom soil configurations");
    return last_error;
}

watering_error_t custom_soil_db_get_storage_usage(size_t *used_bytes, size_t *total_bytes)
{
    if (!used_bytes || !total_bytes) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    *used_bytes = 0;
    *total_bytes = WATERING_CHANNELS_COUNT * sizeof(custom_soil_entry_t);
    
    /* Count existing entries */
    for (uint8_t ch = 0; ch < WATERING_CHANNELS_COUNT; ch++) {
        if (custom_soil_db_exists(ch)) {
            *used_bytes += sizeof(custom_soil_entry_t);
        }
    }
    
    return WATERING_SUCCESS;
}

/* CRC32 calculation for data integrity */
static uint32_t calculate_crc32(const void *data, size_t length)
{
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFF;
    
    for (size_t i = 0; i < length; i++) {
        crc ^= bytes[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 1) {
                crc = (crc >> 1) ^ 0xEDB88320;
            } else {
                crc >>= 1;
            }
        }
    }
    
    return ~crc;
}