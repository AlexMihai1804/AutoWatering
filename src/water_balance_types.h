#ifndef WATER_BALANCE_TYPES_H
#define WATER_BALANCE_TYPES_H

#include <stdint.h>
#include <stdbool.h>

/**
 * @brief Water balance tracking structure
 */
typedef struct water_balance_t {
    float rwz_awc_mm;                  /**< Root zone available water capacity (mm) */
    float wetting_awc_mm;              /**< Wetted zone AWC adjusted for irrigation method (mm) */
    float raw_mm;                      /**< Readily available water (mm) */
    float wetting_fraction;            /**< Effective wetting fraction (0-1) used for balance */
    float surface_wet_fraction;        /**< Current surface wet area fraction (0-1) */
    uint32_t surface_wet_update_s;     /**< Last update time for surface wet fraction (s) */
    float current_deficit_mm;          /**< Current water deficit (mm) */
    float surface_deficit_mm;          /**< Surface evaporation deficit (mm), 0=wet, TEW=dry */
    float surface_tew_mm;              /**< Total evaporable water for surface layer (mm) */
    float surface_rew_mm;              /**< Readily evaporable water for surface layer (mm) */
    float effective_rain_mm;           /**< Effective precipitation (mm) */
    bool irrigation_needed;            /**< Trigger flag for irrigation */
    uint32_t last_update_time;         /**< Last update timestamp */
} water_balance_t;

#endif // WATER_BALANCE_TYPES_H
