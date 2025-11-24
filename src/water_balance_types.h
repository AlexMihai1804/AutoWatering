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
    float current_deficit_mm;          /**< Current water deficit (mm) */
    float effective_rain_mm;           /**< Effective precipitation (mm) */
    bool irrigation_needed;            /**< Trigger flag for irrigation */
    uint32_t last_update_time;         /**< Last update timestamp */
} water_balance_t;

#endif // WATER_BALANCE_TYPES_H