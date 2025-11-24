#ifndef RAIN_CONFIG_H
#define RAIN_CONFIG_H

#include <stdint.h>
#include <stdbool.h>

/**
 * @file rain_config.h
 * @brief Rain sensor configuration management
 * 
 * This header defines structures and functions for managing rain sensor
 * configuration including NVS storage, validation, and default values.
 */

/* NVS storage IDs for rain sensor data */
#define NVS_RAIN_CONFIG_ID      0x0180
#define NVS_RAIN_STATE_ID       0x0181
#define NVS_RAIN_HOURLY_ID      0x0182
#define NVS_RAIN_DAILY_ID       0x0183

/**
 * @brief Rain sensor configuration stored in NVS
 */
typedef struct __attribute__((packed)) {
    float mm_per_pulse;           /**< Calibration: mm per pulse */
    uint16_t debounce_ms;         /**< Debounce time in milliseconds */
    uint8_t sensor_enabled;       /**< 1=enabled, 0=disabled */
    uint8_t integration_enabled;  /**< 1=enabled, 0=disabled */
    float rain_sensitivity_pct;   /**< Rain sensitivity for irrigation (0-100%) */
    float skip_threshold_mm;      /**< mm threshold to skip irrigation */
    uint32_t last_reset_time;     /**< Last counter reset timestamp */
    uint8_t reserved[4];          /**< Reserved for future use */
} rain_nvs_config_t;              /* 24 bytes */

/**
 * @brief Rain sensor persistent state stored in NVS
 */
typedef struct __attribute__((packed)) {
    uint32_t total_pulses;        /**< Persistent pulse counter */
    uint32_t last_pulse_time;     /**< Last pulse timestamp */
    float current_hour_mm;        /**< Current hour accumulation */
    float today_total_mm;         /**< Today's total rainfall */
    uint32_t hour_start_time;     /**< Current hour start time */
    uint32_t day_start_time;      /**< Current day start time */
    uint8_t reserved[4];          /**< Reserved for future use */
} rain_nvs_state_t;               /* 28 bytes */

/**
 * @brief Default rain sensor configuration
 */
#define DEFAULT_RAIN_CONFIG { \
    .mm_per_pulse = 0.2f,         /* 0.2mm per pulse (typical tipping bucket) */ \
    .debounce_ms = 50,            /* 50ms debounce */ \
    .sensor_enabled = 1,          /* Enabled by default */ \
    .integration_enabled = 1,     /* Integration enabled */ \
    .rain_sensitivity_pct = 75.0f, /* 75% sensitivity */ \
    .skip_threshold_mm = 5.0f,    /* Skip irrigation after 5mm rain */ \
    .last_reset_time = 0,         /* Never reset */ \
    .reserved = {0} \
}

/**
 * @brief Default rain sensor state
 */
#define DEFAULT_RAIN_STATE { \
    .total_pulses = 0,            /* No pulses initially */ \
    .last_pulse_time = 0,         /* No pulses yet */ \
    .current_hour_mm = 0.0f,      /* No current hour rain */ \
    .today_total_mm = 0.0f,       /* No today rain */ \
    .hour_start_time = 0,         /* Will be set on init */ \
    .day_start_time = 0,          /* Will be set on init */ \
    .reserved = {0} \
}

/**
 * @brief Save rain sensor configuration to NVS
 * 
 * @param config Configuration to save
 * @return 0 on success, negative error code on failure
 */
int rain_config_save(const rain_nvs_config_t *config);

/**
 * @brief Load rain sensor configuration from NVS
 * 
 * @param config Buffer to store loaded configuration
 * @return 0 on success, negative error code on failure
 */
int rain_config_load(rain_nvs_config_t *config);

/**
 * @brief Save rain sensor state to NVS
 * 
 * @param state State to save
 * @return 0 on success, negative error code on failure
 */
int rain_state_save(const rain_nvs_state_t *state);

/**
 * @brief Load rain sensor state from NVS
 * 
 * @param state Buffer to store loaded state
 * @return 0 on success, negative error code on failure
 */
int rain_state_load(rain_nvs_state_t *state);

/**
 * @brief Validate rain sensor configuration
 * 
 * @param config Configuration to validate
 * @return 0 if valid, negative error code if invalid
 */
int rain_config_validate(const rain_nvs_config_t *config);

/**
 * @brief Get default rain sensor configuration
 * 
 * @param config Buffer to fill with default configuration
 */
void rain_config_get_default(rain_nvs_config_t *config);

/**
 * @brief Get default rain sensor state
 * 
 * @param state Buffer to fill with default state
 */
void rain_state_get_default(rain_nvs_state_t *state);

/**
 * @brief Reset rain sensor configuration to defaults
 * 
 * @return 0 on success, negative error code on failure
 */
int rain_config_reset(void);

/**
 * @brief Reset rain sensor state (clear counters)
 * 
 * @return 0 on success, negative error code on failure
 */
int rain_state_reset(void);

#endif // RAIN_CONFIG_H