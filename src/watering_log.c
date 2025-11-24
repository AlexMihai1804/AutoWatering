#include "watering_log.h"
#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>
#include <zephyr/logging/log.h>
#include <stdio.h>
#include <zephyr/sys/printk.h>
#include "watering_internal.h"

/* Fallback if the application forgot CONFIG_LOG_DEFAULT_LEVEL */
#ifndef CONFIG_LOG_DEFAULT_LEVEL
#define CONFIG_LOG_DEFAULT_LEVEL 3   /* INFO */
#endif

/**
 * @file watering_log.c
 * @brief Implementation of configurable logging system
 * 
 * This file implements a flexible logging system with the ability
 * to configure the level of detail and message destination.
 */

// Define logging module for irrigation system
LOG_MODULE_REGISTER(watering, CONFIG_LOG_DEFAULT_LEVEL);

// Logging state variables
static int current_log_level = WATERING_LOG_LEVEL_ERROR;
static bool logging_to_file = false;
static char log_file_path[64] = {0};
static struct fs_file_t log_file;
static K_MUTEX_DEFINE(log_mutex);

/**
 * @brief Initialize the logging system
 * 
 * @param level Initial logging level
 */
void watering_log_init(int level) {
    k_mutex_lock(&log_mutex, K_FOREVER);
    
    if (level >= WATERING_LOG_LEVEL_NONE && level <= WATERING_LOG_LEVEL_DEBUG) {
        current_log_level = level;
    }
    
    logging_to_file = false;
    memset(log_file_path, 0, sizeof(log_file_path));
    
    LOG_INF("Watering log system initialized with level %d", level);
    printk("Watering log initialized (level %d)\n", current_log_level);
    
    k_mutex_unlock(&log_mutex);
}

/**
 * @brief Change logging level during runtime
 * 
 * @param level New logging level
 */
void watering_log_set_level(int level) {
    k_mutex_lock(&log_mutex, K_FOREVER);
    
    if (level != current_log_level) {
        LOG_INF("Changing log level from %d to %d", current_log_level, level);
        current_log_level = level;
    }
    
    k_mutex_unlock(&log_mutex);
}

/**
 * @brief Enable or disable logging to file
 * 
 * @param enable True to enable, false to disable
 * @param file_path Path to log file (when enabled)
 * @return 0 on success, error on failure
 */
int watering_log_to_file(bool enable, const char *file_path) {
    int ret = 0;
    
    k_mutex_lock(&log_mutex, K_FOREVER);
    
    // Disable previous logging if it exists
    if (logging_to_file) {
        fs_close(&log_file);
        logging_to_file = false;
    }
    
    // Enable new file logging if requested
    if (enable && file_path != NULL) {
        memset(&log_file, 0, sizeof(log_file));
        
        // Copy file path
        strncpy(log_file_path, file_path, sizeof(log_file_path) - 1);
        
        // Open file for writing
        ret = fs_open(&log_file, log_file_path, FS_O_CREATE | FS_O_APPEND | FS_O_WRITE);
        if (ret == 0) {
            logging_to_file = true;
            LOG_INF("Log output redirected to file: %s", log_file_path);
            
            // Add header to file
            char header[100];
            snprintf(header, sizeof(header), 
                    "\n--- Watering System Log Started ---\n");
            fs_write(&log_file, header, strlen(header));
        } else {
            LOG_ERR("Failed to open log file: %s (error %d)", log_file_path, ret);
        }
    }
    
    k_mutex_unlock(&log_mutex);
    return ret;
}

/**
 * @brief Write a message to the log file
 * 
 * Used internally by logging macros
 * 
 * @param level Severity level
 * @param message Log message
 */
void watering_log_write(int level, const char *message) {
    // Check if message level is important enough
    if (level > current_log_level) {
        return;
    }
    
    // Check if file logging is enabled
    if (logging_to_file) {
        k_mutex_lock(&log_mutex, K_FOREVER);
        
        if (fs_tell(&log_file) >= 0) {
            // Add timestamp and level to message
            char log_entry[256];
            snprintf(log_entry, sizeof(log_entry),
                    "[%08llu][%d] %s\n", 
                    k_uptime_get(), level, message);
                    
            fs_write(&log_file, log_entry, strlen(log_entry));
            fs_sync(&log_file);
        }
        
        k_mutex_unlock(&log_mutex);
    }
    
    // Message is already displayed through Zephyr logging system
}

/**
 * @brief Log a message at a specific level
 *
 * @param level Log level of this message
 * @param msg Message to log
 * @param err_code Error code (if applicable)
 */
void watering_log(int level, const char *msg, int err_code)
{
    if (level > current_log_level) {
        return;  // Skip messages above current log level
    }
    
    const char *level_str = "UNKNOWN";
    switch (level) {
        case WATERING_LOG_LEVEL_ERROR:
            level_str = "ERROR";
            break;
        case WATERING_LOG_LEVEL_WARNING:
            level_str = "WARN";
            break;
        case WATERING_LOG_LEVEL_INFO:
            level_str = "INFO";
            break;
        case WATERING_LOG_LEVEL_DEBUG:
            level_str = "DEBUG";
            break;
    }
    
    if (err_code != 0) {
        printk("[%s] %s (code: %d)\n", level_str, msg, err_code);
    } else {
        printk("[%s] %s\n", level_str, msg);
    }
}

/**
 * @brief Log a volume constraint event for historical tracking
 * 
 * This function logs when calculated irrigation volumes exceed user-defined
 * maximum limits, providing visibility into when and how often constraints
 * are applied.
 * 
 * @param channel_id Channel ID that was constrained
 * @param calculated_volume_l Originally calculated volume (liters)
 * @param max_volume_limit_l Maximum allowed volume (liters)
 * @param mode_name Mode name (e.g., "Quality", "Eco")
 */
void watering_log_constraint(uint8_t channel_id, 
                           float calculated_volume_l, 
                           float max_volume_limit_l, 
                           const char *mode_name)
{
    if (!mode_name) {
        mode_name = "Unknown";
    }
    
    // Calculate reduction percentage
    float reduction_pct = 0.0f;
    if (calculated_volume_l > 0.0f) {
        reduction_pct = ((calculated_volume_l - max_volume_limit_l) / calculated_volume_l) * 100.0f;
    }
    
    // Create detailed constraint log message
    char constraint_msg[200];
    snprintf(constraint_msg, sizeof(constraint_msg),
             "CONSTRAINT Ch%d %s: %.1fL -> %.1fL (%.1f%% reduction)",
             channel_id, mode_name, 
             (double)calculated_volume_l, (double)max_volume_limit_l, 
             (double)reduction_pct);
    
    // Log as warning level since it's important operational information
    watering_log(WATERING_LOG_LEVEL_WARNING, constraint_msg, 0);
    
    // Also write to file logging if enabled for historical tracking
    watering_log_write(WATERING_LOG_LEVEL_WARNING, constraint_msg);
}
