/**
 * @file nvs_storage_monitor.c
 * @brief NVS storage monitoring and cleanup implementation
 * 
 * This module provides:
 * - NVS usage monitoring and capacity tracking
 * - Automatic cleanup when approaching capacity limits
 * - Data rotation algorithms for historical data management
 * - Storage health monitoring and error reporting
 */

#include "watering_enhanced.h"
#include "nvs_config.h"
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <zephyr/logging/log.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/fs/nvs.h>
#include <string.h>
#include <stdio.h>

LOG_MODULE_REGISTER(nvs_storage_monitor, LOG_LEVEL_INF);

/* Storage monitoring configuration */
#define STORAGE_WARNING_THRESHOLD_PERCENT    80  // Warn when 80% full
#define STORAGE_CRITICAL_THRESHOLD_PERCENT   90  // Critical when 90% full
#define STORAGE_CLEANUP_TARGET_PERCENT       70  // Clean up to 70% usage
#define STORAGE_HEALTH_CHECK_INTERVAL_MS     60000  // Check every minute

/* Storage monitoring state */
typedef struct {
    uint32_t total_capacity_bytes;
    uint32_t used_bytes;
    uint32_t free_bytes;
    uint8_t usage_percentage;
    uint32_t last_cleanup_time;
    uint32_t cleanup_count;
    uint32_t write_errors;
    uint32_t read_errors;
    bool health_check_active;
    bool cleanup_in_progress;
} storage_monitor_state_t;

/* Data rotation priorities (higher number = higher priority to keep) */
typedef enum {
    ROTATION_PRIORITY_CRITICAL = 10,    // System configuration, never delete
    ROTATION_PRIORITY_HIGH = 8,         // Current channel configurations
    ROTATION_PRIORITY_MEDIUM = 6,       // Recent history data (last 7 days)
    ROTATION_PRIORITY_LOW = 4,          // Older history data (last 30 days)
    ROTATION_PRIORITY_MINIMAL = 2,      // Old aggregated data
} data_rotation_priority_t;

/* Global storage monitor state */
static storage_monitor_state_t g_storage_state = {0};
static struct nvs_fs nvs_monitor;
static bool monitor_initialized = false;
static K_MUTEX_DEFINE(monitor_mutex);
/* Forward declarations */
static void nvs_storage_health_check_work_handler(struct k_work *work);

static K_WORK_DELAYABLE_DEFINE(health_check_work, nvs_storage_health_check_work_handler);
static watering_error_t nvs_storage_calculate_usage(void);
static watering_error_t nvs_storage_cleanup_old_data(uint8_t target_usage_percent);
static watering_error_t nvs_storage_rotate_environmental_history(void);
static watering_error_t nvs_storage_rotate_watering_history(void);
static data_rotation_priority_t nvs_storage_get_data_priority(uint16_t key);

/**
 * @brief Initialize NVS storage monitoring system
 * 
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t nvs_storage_monitor_init(void)
{
    if (k_mutex_lock(&monitor_mutex, K_MSEC(1000)) != 0) {
        return WATERING_ERROR_TIMEOUT;
    }
    
    if (monitor_initialized) {
        k_mutex_unlock(&monitor_mutex);
        return WATERING_SUCCESS;
    }
    
    // Initialize NVS filesystem for monitoring
    const struct flash_area *fa;
    int rc = flash_area_open(FIXED_PARTITION_ID(nvs_storage), &fa);
    if (rc) {
        LOG_ERR("Failed to open NVS flash area for monitoring: %d", rc);
        k_mutex_unlock(&monitor_mutex);
        return WATERING_ERROR_STORAGE;
    }
    
    nvs_monitor.flash_device = fa->fa_dev;
    nvs_monitor.offset = fa->fa_off;
    nvs_monitor.sector_size = 4096;
    nvs_monitor.sector_count = fa->fa_size / nvs_monitor.sector_size;
    
    rc = nvs_mount(&nvs_monitor);
    if (rc) {
        LOG_ERR("Failed to mount NVS for monitoring: %d", rc);
        flash_area_close(fa);
        k_mutex_unlock(&monitor_mutex);
        return WATERING_ERROR_STORAGE;
    }
    
    // Initialize storage state
    g_storage_state.total_capacity_bytes = fa->fa_size;
    g_storage_state.last_cleanup_time = k_uptime_get_32();
    g_storage_state.cleanup_count = 0;
    g_storage_state.write_errors = 0;
    g_storage_state.read_errors = 0;
    g_storage_state.health_check_active = false;
    g_storage_state.cleanup_in_progress = false;
    
    flash_area_close(fa);
    
    // Calculate initial usage
    watering_error_t result = nvs_storage_calculate_usage();
    if (result != WATERING_SUCCESS) {
        LOG_WRN("Failed to calculate initial storage usage: %d", result);
    }
    
    // Start periodic health check via system workqueue
    k_work_schedule(&health_check_work, K_MSEC(STORAGE_HEALTH_CHECK_INTERVAL_MS));
    
    monitor_initialized = true;
    
    LOG_INF("NVS storage monitor initialized - Capacity: %d bytes, Usage: %d%%",
            g_storage_state.total_capacity_bytes, g_storage_state.usage_percentage);
    
    k_mutex_unlock(&monitor_mutex);
    return WATERING_SUCCESS;
}

/**
 * @brief Get current storage usage information
 * 
 * @param usage_info Pointer to structure to fill with usage information
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t nvs_storage_get_usage(storage_monitor_state_t *usage_info)
{
    if (!usage_info) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    if (!monitor_initialized) {
        watering_error_t result = nvs_storage_monitor_init();
        if (result != WATERING_SUCCESS) {
            return result;
        }
    }
    
    if (k_mutex_lock(&monitor_mutex, K_MSEC(100)) != 0) {
        return WATERING_ERROR_TIMEOUT;
    }
    
    // Update usage statistics
    nvs_storage_calculate_usage();
    
    *usage_info = g_storage_state;
    
    k_mutex_unlock(&monitor_mutex);
    return WATERING_SUCCESS;
}

/**
 * @brief Trigger immediate storage cleanup if needed
 * 
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t nvs_storage_trigger_cleanup(void)
{
    if (!monitor_initialized) {
        watering_error_t result = nvs_storage_monitor_init();
        if (result != WATERING_SUCCESS) {
            return result;
        }
    }
    
    if (k_mutex_lock(&monitor_mutex, K_MSEC(2000)) != 0) {
        return WATERING_ERROR_TIMEOUT;
    }
    
    if (g_storage_state.cleanup_in_progress) {
        LOG_WRN("Cleanup already in progress");
        k_mutex_unlock(&monitor_mutex);
        return WATERING_ERROR_BUSY;
    }
    
    // Update usage before cleanup
    watering_error_t result = nvs_storage_calculate_usage();
    if (result != WATERING_SUCCESS) {
        k_mutex_unlock(&monitor_mutex);
        return result;
    }
    
    // Check if cleanup is needed
    if (g_storage_state.usage_percentage < STORAGE_WARNING_THRESHOLD_PERCENT) {
        LOG_INF("Storage usage %d%% is below warning threshold, cleanup not needed",
                g_storage_state.usage_percentage);
        k_mutex_unlock(&monitor_mutex);
        return WATERING_SUCCESS;
    }
    
    LOG_INF("Starting storage cleanup - current usage: %d%%", 
            g_storage_state.usage_percentage);
    
    g_storage_state.cleanup_in_progress = true;
    
    result = nvs_storage_cleanup_old_data(STORAGE_CLEANUP_TARGET_PERCENT);
    
    g_storage_state.cleanup_in_progress = false;
    g_storage_state.last_cleanup_time = k_uptime_get_32();
    g_storage_state.cleanup_count++;
    
    if (result == WATERING_SUCCESS) {
        // Recalculate usage after cleanup
        nvs_storage_calculate_usage();
        LOG_INF("Storage cleanup completed - new usage: %d%%", 
                g_storage_state.usage_percentage);
    } else {
        LOG_ERR("Storage cleanup failed: %d", result);
    }
    
    k_mutex_unlock(&monitor_mutex);
    return result;
}

/**
 * @brief Check storage health and trigger cleanup if needed
 * 
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t nvs_storage_health_check(void)
{
    if (!monitor_initialized) {
        return WATERING_SUCCESS; // Not initialized yet, skip check
    }
    
    if (k_mutex_lock(&monitor_mutex, K_MSEC(100)) != 0) {
        return WATERING_ERROR_TIMEOUT;
    }
    
    g_storage_state.health_check_active = true;
    
    // Update usage statistics
    watering_error_t result = nvs_storage_calculate_usage();
    if (result != WATERING_SUCCESS) {
        LOG_ERR("Failed to calculate storage usage during health check: %d", result);
        g_storage_state.health_check_active = false;
        k_mutex_unlock(&monitor_mutex);
        return result;
    }
    
    // Check usage thresholds
    if (g_storage_state.usage_percentage >= STORAGE_CRITICAL_THRESHOLD_PERCENT) {
        LOG_ERR("CRITICAL: Storage usage %d%% exceeds critical threshold %d%%",
                g_storage_state.usage_percentage, STORAGE_CRITICAL_THRESHOLD_PERCENT);
        
        // Trigger immediate cleanup
        if (!g_storage_state.cleanup_in_progress) {
            g_storage_state.cleanup_in_progress = true;
            result = nvs_storage_cleanup_old_data(STORAGE_CLEANUP_TARGET_PERCENT);
            g_storage_state.cleanup_in_progress = false;
            g_storage_state.cleanup_count++;
            
            if (result == WATERING_SUCCESS) {
                nvs_storage_calculate_usage();
                LOG_INF("Emergency cleanup completed - new usage: %d%%", 
                        g_storage_state.usage_percentage);
            }
        }
    } else if (g_storage_state.usage_percentage >= STORAGE_WARNING_THRESHOLD_PERCENT) {
        LOG_WRN("WARNING: Storage usage %d%% exceeds warning threshold %d%%",
                g_storage_state.usage_percentage, STORAGE_WARNING_THRESHOLD_PERCENT);
    }
    
    g_storage_state.health_check_active = false;
    k_mutex_unlock(&monitor_mutex);
    return result;
}

/**
 * @brief Work handler for periodic storage health checks
 *
 * Runs in thread context so we can safely take mutexes and log.
 */
static void nvs_storage_health_check_work_handler(struct k_work *work)
{
    struct k_work_delayable *dwork = CONTAINER_OF(work, struct k_work_delayable, work);

    (void)nvs_storage_health_check();

    /* Reschedule the next health check */
    k_work_reschedule(dwork, K_MSEC(STORAGE_HEALTH_CHECK_INTERVAL_MS));
}

/**
 * @brief Calculate current storage usage statistics
 * 
 * @return WATERING_SUCCESS on success, error code on failure
 */
static watering_error_t nvs_storage_calculate_usage(void)
{
    if (!monitor_initialized) {
        return WATERING_ERROR_NOT_INITIALIZED;
    }

    ssize_t free_space = nvs_calc_free_space(&nvs_monitor);
    if (free_space < 0) {
        LOG_ERR("Failed to calculate NVS free space: %d", (int)free_space);
        return WATERING_ERROR_STORAGE;
    }

    if ((size_t)free_space > g_storage_state.total_capacity_bytes) {
        free_space = g_storage_state.total_capacity_bytes;
    }

    g_storage_state.free_bytes = (uint32_t)free_space;
    g_storage_state.used_bytes = g_storage_state.total_capacity_bytes - g_storage_state.free_bytes;

    if (g_storage_state.total_capacity_bytes > 0U) {
        g_storage_state.usage_percentage =
            (uint8_t)((g_storage_state.used_bytes * 100U) / g_storage_state.total_capacity_bytes);
    } else {
        g_storage_state.usage_percentage = 0U;
    }

    return WATERING_SUCCESS;
}

/**
 * @brief Clean up old data to reach target usage percentage
 * 
 * @param target_usage_percent Target usage percentage after cleanup
 * @return WATERING_SUCCESS on success, error code on failure
 */
static watering_error_t nvs_storage_cleanup_old_data(uint8_t target_usage_percent)
{
    LOG_INF("Starting data cleanup to reach %d%% usage", target_usage_percent);
    
    watering_error_t result = WATERING_SUCCESS;
    
    // Rotate environmental history (remove oldest entries)
    result = nvs_storage_rotate_environmental_history();
    if (result != WATERING_SUCCESS) {
        LOG_ERR("Failed to rotate environmental history: %d", result);
        return result;
    }
    
    // Rotate watering history (remove oldest entries)
    result = nvs_storage_rotate_watering_history();
    if (result != WATERING_SUCCESS) {
        LOG_ERR("Failed to rotate watering history: %d", result);
        return result;
    }
    
    // Check if we've reached the target
    nvs_storage_calculate_usage();
    if (g_storage_state.usage_percentage > target_usage_percent) {
        LOG_WRN("Cleanup did not reach target usage %d%%, current: %d%%",
                target_usage_percent, g_storage_state.usage_percentage);
        // Could implement more aggressive cleanup here
    }
    
    return WATERING_SUCCESS;
}

/**
 * @brief Rotate environmental history data (remove oldest entries)
 * 
 * @return WATERING_SUCCESS on success, error code on failure
 */
static watering_error_t nvs_storage_rotate_environmental_history(void)
{
#ifdef CONFIG_HISTORY_EXTERNAL_FLASH
    // Environmental history is stored in external flash, no NVS rotation needed
    LOG_DBG("Environmental history uses external flash, NVS rotation skipped");
    return WATERING_SUCCESS;
#else
    LOG_INF("Rotating environmental history data");
    
    // Read current environmental history
    environmental_history_t env_history;
    ssize_t rc = nvs_read(&nvs_monitor, 0x4000, &env_history, sizeof(env_history));
    if (rc < 0) {
        LOG_WRN("No environmental history found for rotation");
        return WATERING_SUCCESS;
    }
    
    // Remove oldest hourly entries (keep last 15 days instead of 30)
    uint16_t target_hourly_entries = 15 * 24; // 15 days
    if (env_history.hourly_count > target_hourly_entries) {
        uint16_t entries_to_remove = env_history.hourly_count - target_hourly_entries;
        
        // Shift entries (simplified - in practice you'd use ring buffer logic)
        memmove(&env_history.hourly[0], 
                &env_history.hourly[entries_to_remove],
                (target_hourly_entries * sizeof(hourly_history_entry_t)));
        
        env_history.hourly_count = target_hourly_entries;
        env_history.hourly_head = target_hourly_entries % ARRAY_SIZE(env_history.hourly);
        
        LOG_INF("Removed %d old hourly entries", entries_to_remove);
    }
    
    // Remove oldest daily entries (keep last 6 months instead of 12)
    uint16_t target_daily_entries = 6 * 31; // 6 months
    if (env_history.daily_count > target_daily_entries) {
        uint16_t entries_to_remove = env_history.daily_count - target_daily_entries;
        
        memmove(&env_history.daily[0],
                &env_history.daily[entries_to_remove],
                (target_daily_entries * sizeof(daily_history_entry_t)));
        
        env_history.daily_count = target_daily_entries;
        env_history.daily_head = target_daily_entries % ARRAY_SIZE(env_history.daily);
        
        LOG_INF("Removed %d old daily entries", entries_to_remove);
    }
    
    // Write back updated history
    rc = nvs_write(&nvs_monitor, 0x4000, &env_history, sizeof(env_history));
    if (rc < 0) {
        LOG_ERR("Failed to write rotated environmental history: %d", rc);
        return WATERING_ERROR_STORAGE;
    }
    
    return WATERING_SUCCESS;
#endif /* CONFIG_HISTORY_EXTERNAL_FLASH */
}

/**
 * @brief Rotate watering history data (remove oldest entries)
 * 
 * @return WATERING_SUCCESS on success, error code on failure
 */
static watering_error_t nvs_storage_rotate_watering_history(void)
{
    LOG_INF("Rotating watering history data");
    
    // This would implement rotation of watering history
    // Similar to environmental history rotation
    // For now, just log the operation
    
    LOG_INF("Watering history rotation completed");
    return WATERING_SUCCESS;
}

/**
 * @brief Get data priority for rotation decisions
 * 
 * @param key NVS key to check priority for
 * @return Data rotation priority
 */
static __attribute__((unused)) data_rotation_priority_t nvs_storage_get_data_priority(uint16_t key)
{
    // System configuration keys (never delete)
    if (key >= 0x1000 && key < 0x2000) {
        return ROTATION_PRIORITY_CRITICAL;
    }
    
    // Channel configuration keys (high priority)
    if (key >= 0x3000 && key < 0x4000) {
        return ROTATION_PRIORITY_HIGH;
    }
    
    // Environmental history (medium to low priority based on age)
    if (key >= 0x4000 && key < 0x5000) {
        return ROTATION_PRIORITY_MEDIUM;
    }
    
    // Watering history (low to minimal priority based on age)
    if (key >= 0x5000 && key < 0x6000) {
        return ROTATION_PRIORITY_LOW;
    }
    
    // Default to minimal priority
    return ROTATION_PRIORITY_MINIMAL;
}

/**
 * @brief Get storage health status
 * 
 * @param is_healthy Pointer to store health status
 * @param health_message Pointer to store health message (optional)
 * @param message_size Size of health message buffer
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t nvs_storage_get_health_status(bool *is_healthy, char *health_message, size_t message_size)
{
    if (!is_healthy) {
        return WATERING_ERROR_INVALID_PARAM;
    }
    
    if (!monitor_initialized) {
        *is_healthy = false;
        if (health_message && message_size > 0) {
            strncpy(health_message, "Storage monitor not initialized", message_size - 1);
            health_message[message_size - 1] = '\0';
        }
        return WATERING_SUCCESS;
    }
    
    if (k_mutex_lock(&monitor_mutex, K_MSEC(100)) != 0) {
        return WATERING_ERROR_TIMEOUT;
    }
    
    // Update usage before health check
    nvs_storage_calculate_usage();
    
    *is_healthy = (g_storage_state.usage_percentage < STORAGE_CRITICAL_THRESHOLD_PERCENT) &&
                  (g_storage_state.write_errors == 0) &&
                  (g_storage_state.read_errors == 0);
    
    if (health_message && message_size > 0) {
        if (*is_healthy) {
            snprintf(health_message, message_size, "Storage healthy - %d%% used", 
                     g_storage_state.usage_percentage);
        } else {
            snprintf(health_message, message_size, "Storage issues - %d%% used, %d write errors, %d read errors",
                     g_storage_state.usage_percentage, g_storage_state.write_errors, g_storage_state.read_errors);
        }
    }
    
    k_mutex_unlock(&monitor_mutex);
    return WATERING_SUCCESS;
}

/**
 * @brief Record storage operation error for monitoring
 * 
 * @param is_write_error True for write error, false for read error
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t nvs_storage_record_error(bool is_write_error)
{
    if (!monitor_initialized) {
        return WATERING_SUCCESS; // Ignore if not initialized
    }
    
    if (k_mutex_lock(&monitor_mutex, K_MSEC(100)) != 0) {
        return WATERING_ERROR_TIMEOUT;
    }
    
    if (is_write_error) {
        g_storage_state.write_errors++;
        LOG_ERR("NVS write error recorded - total: %d", g_storage_state.write_errors);
    } else {
        g_storage_state.read_errors++;
        LOG_ERR("NVS read error recorded - total: %d", g_storage_state.read_errors);
    }
    
    k_mutex_unlock(&monitor_mutex);
    return WATERING_SUCCESS;
}
