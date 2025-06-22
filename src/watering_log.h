#ifndef WATERING_LOG_H
#define WATERING_LOG_H

/**
 * @file watering_log.h
 * @brief Header for the watering system logging functions
 */

#include "watering_internal.h"

/**
 * @brief Initialize the logging system
 *
 * @param level Maximum log level to display
 */
void watering_log_init(int level);

/**
 * @brief Log a message at a specific level
 *
 * @param level Log level of this message
 * @param msg Message to log
 * @param err_code Error code (if applicable)
 */
void watering_log(int level, const char *msg, int err_code);

// Redefine our logging macros with distinct names to avoid conflicts
#define WLOG_ERROR(msg, err_code) watering_log(WATERING_LOG_LEVEL_ERROR, msg, err_code)
#define WLOG_WARNING(msg, err_code) watering_log(WATERING_LOG_LEVEL_WARNING, msg, err_code)
#define WLOG_INFO(msg) watering_log(WATERING_LOG_LEVEL_INFO, msg, 0)
#define WLOG_DEBUG(msg) watering_log(WATERING_LOG_LEVEL_DEBUG, msg, 0)

#endif // WATERING_LOG_H
