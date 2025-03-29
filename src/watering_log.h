#ifndef WATERING_LOG_H
#define WATERING_LOG_H
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

/**
 * @file watering_log.h
 * @brief Configurable logging system for the irrigation system
 * 
 * This header defines macros for logging with different severity levels
 * and filtering based on configuration settings.
 */

// Logging levels - Renamed to avoid conflicts with Zephyr's LOG_LEVEL_*
typedef enum {
    WATERING_LOG_LEVEL_NONE = 0,     // Disabled
    WATERING_LOG_LEVEL_ERROR = 1,    // Errors only
    WATERING_LOG_LEVEL_WARNING = 2,  // Warnings and errors
    WATERING_LOG_LEVEL_INFO = 3,     // General information + warnings + errors
    WATERING_LOG_LEVEL_DEBUG = 4     // All debug messages
} watering_log_level_t;

// Logging macros
#define LOG_ERROR(fmt, ...) LOG_ERR(fmt, ##__VA_ARGS__)
#define LOG_WARNING(fmt, ...) LOG_WRN(fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...) LOG_INF(fmt, ##__VA_ARGS__)
#define LOG_DEBUG(fmt, ...) LOG_DBG(fmt, ##__VA_ARGS__)

/**
 * @brief Initialize the logging system
 * 
 * @param level Initial logging level
 */
void watering_log_init(watering_log_level_t level);

/**
 * @brief Change logging level during runtime
 * 
 * @param level New logging level
 */
void watering_log_set_level(watering_log_level_t level);

/**
 * @brief Enable or disable logging to file
 * 
 * @param enable True to enable, false to disable
 * @param file_path Path to log file (when enabled)
 * @return 0 on success, error on failure
 */
int watering_log_to_file(bool enable, const char *file_path);

#endif // WATERING_LOG_H
