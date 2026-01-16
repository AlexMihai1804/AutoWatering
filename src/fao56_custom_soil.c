/**
 * @file fao56_custom_soil.c
 * @brief FAO-56 calculations with custom soil support implementation
 */

#include "fao56_custom_soil.h"
#include "plant_db.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>

LOG_MODULE_REGISTER(fao56_custom_soil, LOG_LEVEL_DBG);

/* Static soil_data_t for custom soil conversion */
static soil_data_t custom_soil_data_cache[WATERING_CHANNELS_COUNT];
static bool custom_soil_cache_valid[WATERING_CHANNELS_COUNT] = {false};
static uint32_t custom_soil_cache_timestamp[WATERING_CHANNELS_COUNT] = {0};

/* Cache timeout for custom soil data (5 minutes) */
#define CUSTOM_SOIL_CACHE_TIMEOUT_MS    (5 * 60 * 1000)

watering_error_t fao56_get_soil_data_with_custom(uint8_t channel_id,
                                                const soil_configuration_t *soil_config,
                                                soil_data_t *soil_data)
{
    if (channel_id >= WATERING_CHANNELS_COUNT || !soil_config || !soil_data) {
        LOG_ERR("Invalid parameters for soil data retrieval");
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    if (soil_config->use_custom_soil) {
        /* Check if we have a valid cached conversion */
        uint32_t current_time = k_uptime_get_32();
        if (custom_soil_cache_valid[channel_id] && 
            (current_time - custom_soil_cache_timestamp[channel_id]) < CUSTOM_SOIL_CACHE_TIMEOUT_MS) {
            
            memcpy(soil_data, &custom_soil_data_cache[channel_id], sizeof(soil_data_t));
            LOG_DBG("Using cached custom soil data for channel %d", channel_id);
            return WATERING_SUCCESS;
        }
        
        /* Read custom soil from database */
        custom_soil_entry_t custom_soil;
        watering_error_t err = custom_soil_db_read(channel_id, &custom_soil);
        if (err != WATERING_SUCCESS) {
            LOG_WRN("Failed to read custom soil for channel %d, falling back to standard soil", channel_id);
            /* Fall through to standard soil handling */
        } else {
            /* Validate custom soil for FAO-56 calculations */
            err = fao56_validate_custom_soil_for_calculations(&custom_soil);
            if (err != WATERING_SUCCESS) {
                LOG_WRN("Custom soil for channel %d failed FAO-56 validation, falling back to standard soil", channel_id);
                /* Fall through to standard soil handling */
            } else {
                /* Convert custom soil to enhanced format */
                err = custom_soil_db_to_enhanced_format(&custom_soil, soil_data);
                if (err != WATERING_SUCCESS) {
                    LOG_ERR("Failed to convert custom soil to enhanced format for channel %d", channel_id);
                    return err;
                }
                
                /* Cache the converted data */
                memcpy(&custom_soil_data_cache[channel_id], soil_data, sizeof(soil_data_t));
                custom_soil_cache_valid[channel_id] = true;
                custom_soil_cache_timestamp[channel_id] = current_time;
                
                fao56_log_custom_soil_usage(channel_id, &custom_soil, "FAO-56 calculation");
                return WATERING_SUCCESS;
            }
        }
    }
    
    /* Use standard soil database */
    const soil_data_t *standard_soil = soil_db_get_by_index(soil_config->standard_type);
    if (!standard_soil) {
        LOG_ERR("Invalid standard soil type %d for channel %d", soil_config->standard_type, channel_id);
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    memcpy(soil_data, standard_soil, sizeof(soil_data_t));
    LOG_DBG("Using standard soil type %d for channel %d", soil_config->standard_type, channel_id);
    return WATERING_SUCCESS;
}

watering_error_t fao56_calc_water_balance_with_custom_soil(uint8_t channel_id,
                                                         const plant_full_data_t *plant,
                                                         const soil_configuration_t *soil_config,
                                                         const irrigation_method_data_t *method,
                                                         const environmental_data_t *env,
                                                         float root_depth_current_m,
                                                         water_balance_t *balance)
{
    if (!plant || !soil_config || !method || !env || !balance) {
        LOG_ERR("Invalid parameters for water balance calculation with custom soil");
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    /* Get soil data (custom or standard) */
    soil_data_t soil_data;
    watering_error_t err = fao56_get_soil_data_with_custom(channel_id, soil_config, &soil_data);
    if (err != WATERING_SUCCESS) {
        LOG_ERR("Failed to get soil data for channel %d", channel_id);
        return err;
    }
    
    /* Use the standard water balance calculation with the resolved soil data */
    // Fetch real days-after-planting from watering channel infrastructure
    uint16_t dap = 0;
    watering_get_days_after_planting(channel_id, &dap); // ignore error, dap stays 0 if fails
    return calc_water_balance(channel_id, plant, &soil_data, method, env, root_depth_current_m, dap, balance);
}

float fao56_calc_effective_precipitation_with_custom_soil(float rainfall_mm,
                                                        const soil_configuration_t *soil_config,
                                                        const irrigation_method_data_t *irrigation_method)
{
    if (!soil_config || !irrigation_method) {
        LOG_ERR("Invalid parameters for effective precipitation calculation");
        return 0.0f;
    }
    
    /* For custom soil, we need to create a temporary soil_data_t */
    if (soil_config->use_custom_soil) {
        soil_data_t soil_data;
        soil_data.infil_mm_h = (uint16_t)soil_config->custom.infiltration_rate;
        
        /* Use the standard function with converted data */
        return calc_effective_precipitation(rainfall_mm, &soil_data, irrigation_method);
    } else {
        /* Use standard soil database */
        const soil_data_t *standard_soil = soil_db_get_by_index(soil_config->standard_type);
        if (!standard_soil) {
            LOG_ERR("Invalid standard soil type %d", soil_config->standard_type);
            return 0.0f;
        }
        
        return calc_effective_precipitation(rainfall_mm, standard_soil, irrigation_method);
    }
}

watering_error_t fao56_calc_cycle_and_soak_with_custom_soil(const irrigation_method_data_t *method,
                                                          const soil_configuration_t *soil_config,
                                                          float application_rate_mm_h,
                                                          irrigation_calculation_t *result)
{
    if (!method || !soil_config || !result) {
        LOG_ERR("Invalid parameters for cycle and soak calculation");
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    /* Get soil data (custom or standard) */
    soil_data_t soil_data;
    if (soil_config->use_custom_soil) {
        custom_soil_entry_t custom_soil;
        custom_soil.infiltration_rate = soil_config->custom.infiltration_rate;
        custom_soil.field_capacity = soil_config->custom.field_capacity;
        custom_soil.wilting_point = soil_config->custom.wilting_point;
        custom_soil.bulk_density = soil_config->custom.bulk_density;
        custom_soil.organic_matter = soil_config->custom.organic_matter;
        
        watering_error_t err = custom_soil_db_to_enhanced_format(&custom_soil, &soil_data);
        if (err != WATERING_SUCCESS) {
            LOG_ERR("Failed to convert custom soil for cycle and soak calculation");
            return err;
        }
    } else {
        const soil_data_t *standard_soil = soil_db_get_by_index(soil_config->standard_type);
        if (!standard_soil) {
            LOG_ERR("Invalid standard soil type %d", soil_config->standard_type);
            return WATERING_ERROR_INVALID_PARAM;
        }
        memcpy(&soil_data, standard_soil, sizeof(soil_data_t));
    }
    
    /* Use the standard function with resolved soil data */
    return calc_cycle_and_soak(method, &soil_data, application_rate_mm_h, result);
}

watering_error_t fao56_calc_localized_wetting_pattern_with_custom_soil(const irrigation_method_data_t *method,
                                                                     const soil_configuration_t *soil_config,
                                                                     float emitter_spacing_m,
                                                                     float *wetted_diameter_m,
                                                                     float *wetted_depth_m)
{
    if (!method || !soil_config || !wetted_diameter_m || !wetted_depth_m) {
        LOG_ERR("Invalid parameters for localized wetting pattern calculation");
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    /* Get soil data (custom or standard) */
    soil_data_t soil_data;
    if (soil_config->use_custom_soil) {
        custom_soil_entry_t custom_soil;
        custom_soil.infiltration_rate = soil_config->custom.infiltration_rate;
        custom_soil.field_capacity = soil_config->custom.field_capacity;
        custom_soil.wilting_point = soil_config->custom.wilting_point;
        custom_soil.bulk_density = soil_config->custom.bulk_density;
        custom_soil.organic_matter = soil_config->custom.organic_matter;
        
        watering_error_t err = custom_soil_db_to_enhanced_format(&custom_soil, &soil_data);
        if (err != WATERING_SUCCESS) {
            LOG_ERR("Failed to convert custom soil for wetting pattern calculation");
            return err;
        }
    } else {
        const soil_data_t *standard_soil = soil_db_get_by_index(soil_config->standard_type);
        if (!standard_soil) {
            LOG_ERR("Invalid standard soil type %d", soil_config->standard_type);
            return WATERING_ERROR_INVALID_PARAM;
        }
        memcpy(&soil_data, standard_soil, sizeof(soil_data_t));
    }
    
    /* Use the standard function with resolved soil data */
    return calc_localized_wetting_pattern(method, &soil_data, emitter_spacing_m, 
                                        wetted_diameter_m, wetted_depth_m);
}

float fao56_adjust_volume_for_partial_wetting_with_custom_soil(float base_volume_mm,
                                                             const irrigation_method_data_t *method,
                                                             const plant_full_data_t *plant,
                                                             const soil_configuration_t *soil_config)
{
    if (!method || !plant || !soil_config) {
        LOG_ERR("Invalid parameters for partial wetting adjustment");
        return base_volume_mm; // Return unchanged volume on error
    }
    
    /* Get soil data (custom or standard) */
    soil_data_t soil_data;
    if (soil_config->use_custom_soil) {
        custom_soil_entry_t custom_soil;
        custom_soil.infiltration_rate = soil_config->custom.infiltration_rate;
        custom_soil.field_capacity = soil_config->custom.field_capacity;
        custom_soil.wilting_point = soil_config->custom.wilting_point;
        custom_soil.bulk_density = soil_config->custom.bulk_density;
        custom_soil.organic_matter = soil_config->custom.organic_matter;
        
        watering_error_t err = custom_soil_db_to_enhanced_format(&custom_soil, &soil_data);
        if (err != WATERING_SUCCESS) {
            LOG_ERR("Failed to convert custom soil for partial wetting adjustment");
            return base_volume_mm; // Return unchanged volume on error
        }
    } else {
        const soil_data_t *standard_soil = soil_db_get_by_index(soil_config->standard_type);
        if (!standard_soil) {
            LOG_ERR("Invalid standard soil type %d", soil_config->standard_type);
            return base_volume_mm; // Return unchanged volume on error
        }
        memcpy(&soil_data, standard_soil, sizeof(soil_data_t));
    }
    
    /* Use the standard function with resolved soil data */
    return adjust_volume_for_partial_wetting(base_volume_mm, method, plant, &soil_data);
}

bool fao56_check_irrigation_trigger_mad_with_custom_soil(const water_balance_t *balance,
                                                       const plant_full_data_t *plant,
                                                       const soil_configuration_t *soil_config,
                                                       float stress_factor)
{
    if (!balance || !plant || !soil_config) {
        LOG_ERR("Invalid parameters for irrigation trigger check");
        return false;
    }
    
    /* Get soil data (custom or standard) */
    soil_data_t soil_data;
    if (soil_config->use_custom_soil) {
        custom_soil_entry_t custom_soil;
        custom_soil.infiltration_rate = soil_config->custom.infiltration_rate;
        custom_soil.field_capacity = soil_config->custom.field_capacity;
        custom_soil.wilting_point = soil_config->custom.wilting_point;
        custom_soil.bulk_density = soil_config->custom.bulk_density;
        custom_soil.organic_matter = soil_config->custom.organic_matter;
        
        watering_error_t err = custom_soil_db_to_enhanced_format(&custom_soil, &soil_data);
        if (err != WATERING_SUCCESS) {
            LOG_ERR("Failed to convert custom soil for irrigation trigger check");
            return false;
        }
    } else {
        const soil_data_t *standard_soil = soil_db_get_by_index(soil_config->standard_type);
        if (!standard_soil) {
            LOG_ERR("Invalid standard soil type %d", soil_config->standard_type);
            return false;
        }
        memcpy(&soil_data, standard_soil, sizeof(soil_data_t));
    }
    
    /* Use the standard function with resolved soil data */
    return check_irrigation_trigger_mad(balance, plant, &soil_data, stress_factor);
}

watering_error_t fao56_calculate_irrigation_requirement_with_custom_soil(uint8_t channel_id,
                                                                       const environmental_data_t *env,
                                                                       irrigation_calculation_t *result)
{
    if (channel_id >= WATERING_CHANNELS_COUNT || !env || !result) {
        LOG_ERR("Invalid parameters for irrigation requirement calculation");
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    /* This function would need access to the channel configuration to determine
     * if custom soil is being used. For now, we'll use the standard calculation
     * and let the calling code handle the custom soil integration.
     * 
     * In a full implementation, this would:
     * 1. Get the channel configuration
     * 2. Check if custom soil is configured
     * 3. Use the appropriate soil data for all calculations
     * 4. Call the modified calculation functions
     */
    
    LOG_DBG("Custom soil irrigation calculation for channel %d", channel_id);
    
    /* For now, delegate to the standard calculation */
    /* The calling code should use the individual custom soil functions */
    return fao56_calculate_irrigation_requirement(channel_id, env, result);
}

watering_error_t fao56_validate_custom_soil_for_calculations(const custom_soil_entry_t *custom_soil)
{
    if (!custom_soil) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    /* First, use the basic validation from custom_soil_db */
    watering_error_t err = custom_soil_db_validate_parameters(
        custom_soil->field_capacity,
        custom_soil->wilting_point,
        custom_soil->infiltration_rate,
        custom_soil->bulk_density,
        custom_soil->organic_matter
    );
    
    if (err != WATERING_SUCCESS) {
        return err;
    }
    
    /* Additional FAO-56 specific validations */
    
    /* Check that AWC is reasonable for calculations */
    float awc = custom_soil->field_capacity - custom_soil->wilting_point;
    if (awc < 5.0f) {
        LOG_ERR("Available water capacity too low for FAO-56 calculations: %.2f%%", (double)awc);
        return WATERING_ERROR_CUSTOM_SOIL_INVALID;
    }
    
    if (awc > 50.0f) {
        LOG_WRN("Available water capacity very high, may indicate measurement error: %.2f%%", (double)awc);
        /* Don't fail, but log warning */
    }
    
    /* Check infiltration rate is reasonable for irrigation calculations */
    if (custom_soil->infiltration_rate < 1.0f) {
        LOG_WRN("Very low infiltration rate may cause runoff issues: %.2f mm/hr", 
                (double)custom_soil->infiltration_rate);
    }
    
    if (custom_soil->infiltration_rate > 500.0f) {
        LOG_WRN("Very high infiltration rate may indicate sandy soil: %.2f mm/hr", 
                (double)custom_soil->infiltration_rate);
    }
    
    /* Check bulk density is reasonable */
    if (custom_soil->bulk_density < 0.8f && custom_soil->organic_matter < 10.0f) {
        LOG_WRN("Low bulk density without high organic matter may indicate error: %.2f g/cm³", 
                (double)custom_soil->bulk_density);
    }
    
    if (custom_soil->bulk_density > 2.0f && custom_soil->field_capacity > 25.0f) {
        LOG_WRN("High bulk density with high field capacity may indicate error");
    }
    
    LOG_DBG("Custom soil validation passed for FAO-56 calculations");
    return WATERING_SUCCESS;
}

float fao56_calc_effective_awc_with_wetting_fraction_custom_soil(const soil_configuration_t *soil_config,
                                                               const irrigation_method_data_t *method,
                                                               const plant_full_data_t *plant,
                                                               float root_depth_m)
{
    if (!soil_config || !method || !plant) {
        LOG_ERR("Invalid parameters for effective AWC calculation");
        return 0.0f;
    }
    
    /* Calculate total AWC based on soil type */
    float total_awc_mm;
    if (soil_config->use_custom_soil) {
        /* For custom soil, calculate AWC from field capacity and wilting point */
        float awc_percent = soil_config->custom.field_capacity - soil_config->custom.wilting_point;
        total_awc_mm = awc_percent * root_depth_m * 10.0f; // Convert % to mm/m
    } else {
        /* Use standard soil database */
        const soil_data_t *standard_soil = soil_db_get_by_index(soil_config->standard_type);
        if (!standard_soil) {
            LOG_ERR("Invalid standard soil type %d", soil_config->standard_type);
            return 0.0f;
        }
        total_awc_mm = standard_soil->awc_mm_per_m * root_depth_m;
    }
    
    /* Use the standard function for wetting fraction calculation */
    return calc_effective_awc_with_wetting_fraction(total_awc_mm, method, plant, root_depth_m);
}

void fao56_log_custom_soil_usage(uint8_t channel_id,
                               const custom_soil_entry_t *custom_soil,
                               const char *calculation_type)
{
    if (!custom_soil || !calculation_type) {
        return;
    }
    
    LOG_INF("Using custom soil '%s' for channel %d in %s", 
            custom_soil->name, channel_id, calculation_type);
    LOG_DBG("Custom soil parameters: FC=%.1f%%, WP=%.1f%%, Infil=%.1f mm/hr, BD=%.2f g/cm³, OM=%.1f%%",
            (double)custom_soil->field_capacity,
            (double)custom_soil->wilting_point,
            (double)custom_soil->infiltration_rate,
            (double)custom_soil->bulk_density,
            (double)custom_soil->organic_matter);
}
