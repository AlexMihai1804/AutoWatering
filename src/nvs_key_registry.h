/**
 * @file nvs_key_registry.h
 * @brief Centralised NVS key assignments — T13
 *
 * Every NVS key used anywhere in the firmware is documented here.
 * New keys MUST be added here; never define local NVS key constants.
 *
 * Ranges
 * ------
 *   1 –   99  Core watering configuration
 * 100 – 199  Per-channel config (base + ch 0-7)
 * 200 – 299  Sensor calibration & misc scalars
 * 300 – 399  Per-channel names
 * 400 – 499  Timezone / locale
 * 500 – 599  Enhanced channel configs (base + ch)
 * 600 – 699  Water balance state (base + ch)
 * 700 – 799  Automatic calc, soil moisture
 * 800 – 899  Rain sensor config / state / history
 * 900 – 979  Onboarding, flags, configuration status, hydraulic, wipe, identity
 * 2000 – 7999  Watering history (managed by watering_history.c)
 * 0x6101 – 0x6104  Environmental history (managed by environmental_history.c)
 */

#ifndef NVS_KEY_REGISTRY_H
#define NVS_KEY_REGISTRY_H

/* ------------------------------------------------------------------ */
/* Core / Watering                                                    */
/* ------------------------------------------------------------------ */
#define NVS_KEY_WATERING_CFG            1

/* Per-channel configuration: base + ch (0-7) */
#define NVS_KEY_CHANNEL_CFG_BASE        100

/* Sensor calibration */
#define NVS_KEY_FLOW_CALIB              200
#define NVS_KEY_DAYS_SINCE_START        201

/* Per-channel names: base + ch (0-7) */
#define NVS_KEY_CHANNEL_NAME_BASE       300

/* Timezone */
#define NVS_KEY_TIMEZONE_CONFIG         400

/* Enhanced channel config: base + ch (0-7) */
#define NVS_KEY_ENHANCED_CHANNEL_CFG_BASE 500

/* Water balance state: base + ch (0-7) */
#define NVS_KEY_WATER_BALANCE_BASE      600

/* Automatic calculation and soil moisture */
#define NVS_KEY_AUTOMATIC_CALC_STATE    700
#define NVS_KEY_SOIL_MOISTURE_GLOBAL    710
#define NVS_KEY_SOIL_MOISTURE_CH_BASE   720  /* + ch (0-7) */

/* Rain sensor */
#define NVS_KEY_RAIN_CONFIG             800
#define NVS_KEY_RAIN_STATE              801
#define NVS_KEY_RAIN_HOURLY_DATA        802
#define NVS_KEY_RAIN_DAILY_DATA         803

/* Onboarding / flags / status / hydraulic / wipe / identity */
#define NVS_KEY_ONBOARDING_STATE        900
#define NVS_KEY_CHANNEL_FLAGS_BASE      910  /* + ch (0-7) */
#define NVS_KEY_SYSTEM_FLAGS            920
#define NVS_KEY_CONFIG_STATUS_BASE      930  /* + ch (0-7) */
#define NVS_KEY_CONFIG_RESET_LOG_BASE   940  /* + ch (0-7) */
#define NVS_KEY_HYDRAULIC_GLOBAL_LOCK   950
#define NVS_KEY_WIPE_PROGRESS           960
#define NVS_KEY_DEVICE_IDENTITY         970

/* ------------------------------------------------------------------ */
/* Watering History (managed by watering_history.c)                   */
/* ------------------------------------------------------------------ */
#define NVS_KEY_DETAILED_BASE           2000
#define NVS_KEY_DAILY_BASE              3000
#define NVS_KEY_MONTHLY_BASE            4000
#define NVS_KEY_ANNUAL_BASE             5000
#define NVS_KEY_ROTATION_INFO           6000
#define NVS_KEY_HISTORY_SETTINGS        6001
#define NVS_KEY_INSIGHTS_CACHE          6002
#define NVS_KEY_DETAILED_TS_BASE        7000

/* ------------------------------------------------------------------ */
/* Environmental History (managed by environmental_history.c)         */
/* ------------------------------------------------------------------ */
#define NVS_KEY_ENV_HISTORY_HOURLY      0x6101
#define NVS_KEY_ENV_HISTORY_DAILY       0x6102
#define NVS_KEY_ENV_HISTORY_MONTHLY     0x6103
#define NVS_KEY_ENV_HISTORY_META        0x6104

#endif /* NVS_KEY_REGISTRY_H */
