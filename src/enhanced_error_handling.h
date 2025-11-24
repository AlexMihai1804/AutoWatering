/**
 * @file enhanced_error_handling.h
 * @brief Enhanced error handling and recovery definitions
 */

#ifndef ENHANCED_ERROR_HANDLING_H
#define ENHANCED_ERROR_HANDLING_H

#include "watering.h"
#include <zephyr/kernel.h>

/**
 * @brief Error handling categories
 */
typedef enum {
    ERROR_CATEGORY_SENSOR,
    ERROR_CATEGORY_STORAGE,
    ERROR_CATEGORY_BLUETOOTH,
    ERROR_CATEGORY_INTERVAL,
    ERROR_CATEGORY_COMPENSATION,
    ERROR_CATEGORY_SYSTEM
} error_category_t;

/**
 * @brief Error severity levels
 */
typedef enum {
    ERROR_SEVERITY_INFO,
    ERROR_SEVERITY_WARN,
    ERROR_SEVERITY_ERROR,
    ERROR_SEVERITY_FATAL
} error_severity_t;

/**
 * @brief Error recovery strategies
 */
typedef enum {
    RECOVERY_NONE,
    RECOVERY_RETRY,
    RECOVERY_FALLBACK,
    RECOVERY_RESTART,
    RECOVERY_DISABLE
} recovery_strategy_t;

/**
 * @brief Initialize enhanced error handling system
 */
watering_error_t enhanced_error_init(void);

/**
 * @brief Initialize enhanced error handling system
 */
watering_error_t enhanced_error_handling_init(void);

/**
 * @brief Handle system error with recovery
 */
watering_error_t enhanced_error_handle(error_category_t category, 
                                       error_severity_t severity,
                                       watering_error_t error_code,
                                       const char *context);

/**
 * @brief Check if system can continue operation
 */
bool enhanced_error_can_continue(void);

/**
 * @brief Get error statistics
 */
watering_error_t enhanced_error_get_stats(void *stats);

#endif /* ENHANCED_ERROR_HANDLING_H */
