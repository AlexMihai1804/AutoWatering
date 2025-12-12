/**
 * @file history_flash.c
 * @brief External flash history storage implementation using LittleFS
 * 
 * Implements ring buffer storage for environmental and rain history
 * on W25Q128 external flash, freeing ~70KB of internal RAM.
 */

#include "history_flash.h"
#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>
#include <zephyr/logging/log.h>
#include <errno.h>
#include <string.h>
#include "database_flash.h"

LOG_MODULE_REGISTER(history_flash, CONFIG_LOG_DEFAULT_LEVEL);

/*******************************************************************************
 * Private Data
 ******************************************************************************/

/* File configuration table */
static const struct {
    const char *path;
    uint32_t magic;
    uint16_t capacity;
    uint16_t entry_size;
} history_files[HISTORY_TYPE_COUNT] = {
    [HISTORY_TYPE_ENV_HOURLY] = {
        .path = HISTORY_PATH_ENV_HOURLY,
        .magic = HISTORY_MAGIC_ENV_HOURLY,
        .capacity = HISTORY_ENV_HOURLY_CAPACITY,
        .entry_size = HISTORY_ENV_HOURLY_SIZE,
    },
    [HISTORY_TYPE_ENV_DAILY] = {
        .path = HISTORY_PATH_ENV_DAILY,
        .magic = HISTORY_MAGIC_ENV_DAILY,
        .capacity = HISTORY_ENV_DAILY_CAPACITY,
        .entry_size = HISTORY_ENV_DAILY_SIZE,
    },
    [HISTORY_TYPE_ENV_MONTHLY] = {
        .path = HISTORY_PATH_ENV_MONTHLY,
        .magic = HISTORY_MAGIC_ENV_MONTHLY,
        .capacity = HISTORY_ENV_MONTHLY_CAPACITY,
        .entry_size = HISTORY_ENV_MONTHLY_SIZE,
    },
    [HISTORY_TYPE_RAIN_HOURLY] = {
        .path = HISTORY_PATH_RAIN_HOURLY,
        .magic = HISTORY_MAGIC_RAIN_HOURLY,
        .capacity = HISTORY_RAIN_HOURLY_CAPACITY,
        .entry_size = HISTORY_RAIN_HOURLY_SIZE,
    },
    [HISTORY_TYPE_RAIN_DAILY] = {
        .path = HISTORY_PATH_RAIN_DAILY,
        .magic = HISTORY_MAGIC_RAIN_DAILY,
        .capacity = HISTORY_RAIN_DAILY_CAPACITY,
        .entry_size = HISTORY_RAIN_DAILY_SIZE,
    },
};

/* Runtime state for each history file */
static struct {
    history_file_header_t header;
    bool valid;
} history_state[HISTORY_TYPE_COUNT];

static bool history_initialized = false;
static struct k_mutex history_mutex;
static history_flash_stats_t cached_stats;

/*******************************************************************************
 * Private Functions
 ******************************************************************************/

/**
 * @brief Calculate file offset for entry index
 */
static inline off_t entry_offset(history_type_t type, uint16_t index)
{
    return HISTORY_HEADER_SIZE + (off_t)index * history_files[type].entry_size;
}

/**
 * @brief Get actual ring buffer index for logical index
 * 
 * Logical index 0 = oldest entry, index count-1 = newest entry
 */
static uint16_t logical_to_physical(history_type_t type, uint16_t logical_idx)
{
    const history_file_header_t *hdr = &history_state[type].header;
    
    if (hdr->entry_count < hdr->capacity) {
        /* Buffer not full yet, head is at 0 */
        return logical_idx;
    }
    
    /* Buffer is full, head points to oldest entry */
    return (hdr->head_index + logical_idx) % hdr->capacity;
}

/**
 * @brief Read file header
 */
static int read_header(history_type_t type)
{
    struct fs_file_t file;
    int ret;
    
    fs_file_t_init(&file);
    
    ret = fs_open(&file, history_files[type].path, FS_O_READ);
    if (ret < 0) {
        return ret;
    }
    
    ret = fs_read(&file, &history_state[type].header, HISTORY_HEADER_SIZE);
    fs_close(&file);
    
    if (ret != HISTORY_HEADER_SIZE) {
        return -EIO;
    }
    
    /* Validate header */
    const history_file_header_t *hdr = &history_state[type].header;
    if (hdr->magic != history_files[type].magic ||
        hdr->version != HISTORY_VERSION ||
        hdr->entry_size != history_files[type].entry_size) {
        LOG_WRN("Invalid header for %s", history_files[type].path);
        return -EINVAL;
    }
    
    history_state[type].valid = true;
    return 0;
}

/**
 * @brief Write file header
 */
static int write_header(history_type_t type)
{
    struct fs_file_t file;
    int ret;
    
    fs_file_t_init(&file);
    
    ret = fs_open(&file, history_files[type].path, FS_O_WRITE);
    if (ret < 0) {
        return ret;
    }
    
    ret = fs_write(&file, &history_state[type].header, HISTORY_HEADER_SIZE);
    fs_close(&file);
    
    if (ret != HISTORY_HEADER_SIZE) {
        return -EIO;
    }
    
    return 0;
}

/**
 * @brief Create new history file with empty header
 */
static int create_history_file(history_type_t type)
{
    struct fs_file_t file;
    int ret;
    
    fs_file_t_init(&file);
    
    LOG_INF("Creating history file: %s", history_files[type].path);
    
    ret = fs_open(&file, history_files[type].path, FS_O_CREATE | FS_O_WRITE);
    if (ret < 0) {
        LOG_ERR("Failed to create %s: %d", history_files[type].path, ret);
        return ret;
    }
    
    /* Initialize header */
    history_file_header_t *hdr = &history_state[type].header;
    hdr->magic = history_files[type].magic;
    hdr->version = HISTORY_VERSION;
    hdr->entry_count = 0;
    hdr->head_index = 0;
    hdr->capacity = history_files[type].capacity;
    hdr->entry_size = history_files[type].entry_size;
    hdr->reserved = 0;
    
    ret = fs_write(&file, hdr, HISTORY_HEADER_SIZE);
    if (ret != HISTORY_HEADER_SIZE) {
        fs_close(&file);
        return -EIO;
    }
    
    /* Pre-allocate file space by writing zeros */
    uint8_t zeros[64];
    memset(zeros, 0, sizeof(zeros));
    
    size_t data_size = (size_t)hdr->capacity * hdr->entry_size;
    size_t written = 0;
    
    while (written < data_size) {
        size_t chunk = MIN(sizeof(zeros), data_size - written);
        ret = fs_write(&file, zeros, chunk);
        if (ret < 0) {
            fs_close(&file);
            return ret;
        }
        written += chunk;
    }
    
    fs_close(&file);
    history_state[type].valid = true;
    
    LOG_INF("Created history file: %s (%u entries, %u bytes each)",
            history_files[type].path, hdr->capacity, hdr->entry_size);
    
    return 0;
}

/**
 * @brief Add entry to ring buffer
 */
static int add_entry(history_type_t type, const void *entry)
{
    struct fs_file_t file;
    history_file_header_t *hdr = &history_state[type].header;
    int ret;
    uint16_t write_idx;
    
    if (!history_state[type].valid) {
        return -ENOENT;
    }
    
    k_mutex_lock(&history_mutex, K_FOREVER);
    
    fs_file_t_init(&file);
    
    ret = fs_open(&file, history_files[type].path, FS_O_WRITE);
    if (ret < 0) {
        k_mutex_unlock(&history_mutex);
        cached_stats.write_errors++;
        return ret;
    }
    
    /* Determine write position */
    if (hdr->entry_count < hdr->capacity) {
        /* Buffer not full, append at end */
        write_idx = hdr->entry_count;
        hdr->entry_count++;
    } else {
        /* Buffer full, overwrite oldest (at head) */
        write_idx = hdr->head_index;
        hdr->head_index = (hdr->head_index + 1) % hdr->capacity;
    }
    
    /* Seek to entry position and write */
    off_t offset = entry_offset(type, write_idx);
    ret = fs_seek(&file, offset, FS_SEEK_SET);
    if (ret < 0) {
        fs_close(&file);
        k_mutex_unlock(&history_mutex);
        cached_stats.write_errors++;
        return ret;
    }
    
    ret = fs_write(&file, entry, history_files[type].entry_size);
    if (ret != history_files[type].entry_size) {
        fs_close(&file);
        k_mutex_unlock(&history_mutex);
        cached_stats.write_errors++;
        return -EIO;
    }
    
    /* Update header */
    fs_seek(&file, 0, FS_SEEK_SET);
    ret = fs_write(&file, hdr, HISTORY_HEADER_SIZE);
    fs_close(&file);
    
    k_mutex_unlock(&history_mutex);
    
    if (ret != HISTORY_HEADER_SIZE) {
        cached_stats.write_errors++;
        return -EIO;
    }
    
    return 0;
}

/**
 * @brief Read entries from ring buffer
 */
static int read_entries(history_type_t type, uint16_t start_logical_idx,
                        void *buffer, uint16_t max_entries, uint16_t *out_count)
{
    struct fs_file_t file;
    const history_file_header_t *hdr = &history_state[type].header;
    int ret;
    
    if (!history_state[type].valid) {
        return -ENOENT;
    }
    
    if (start_logical_idx >= hdr->entry_count) {
        *out_count = 0;
        return 0;
    }
    
    k_mutex_lock(&history_mutex, K_FOREVER);
    
    fs_file_t_init(&file);
    
    ret = fs_open(&file, history_files[type].path, FS_O_READ);
    if (ret < 0) {
        k_mutex_unlock(&history_mutex);
        cached_stats.read_errors++;
        return ret;
    }
    
    uint16_t available = hdr->entry_count - start_logical_idx;
    uint16_t to_read = MIN(available, max_entries);
    uint8_t *buf = (uint8_t *)buffer;
    uint16_t entry_size = history_files[type].entry_size;
    
    for (uint16_t i = 0; i < to_read; i++) {
        uint16_t phys_idx = logical_to_physical(type, start_logical_idx + i);
        off_t offset = entry_offset(type, phys_idx);
        
        ret = fs_seek(&file, offset, FS_SEEK_SET);
        if (ret < 0) {
            fs_close(&file);
            k_mutex_unlock(&history_mutex);
            cached_stats.read_errors++;
            return ret;
        }
        
        ret = fs_read(&file, buf + (i * entry_size), entry_size);
        if (ret != entry_size) {
            fs_close(&file);
            k_mutex_unlock(&history_mutex);
            cached_stats.read_errors++;
            return -EIO;
        }
    }
    
    fs_close(&file);
    k_mutex_unlock(&history_mutex);
    
    *out_count = to_read;
    return 0;
}

/*******************************************************************************
 * Public API - Initialization
 ******************************************************************************/

int history_flash_init(void)
{
    int ret;
    struct fs_dirent entry;
    
    if (history_initialized) {
        return 0;
    }
    
    /* Ensure LittleFS is mounted before touching /lfs */
    ret = db_flash_mount();
    if (ret < 0) {
        LOG_ERR("Failed to mount history filesystem: %d", ret);
        return ret;
    }

    k_mutex_init(&history_mutex);
    memset(&cached_stats, 0, sizeof(cached_stats));
    memset(history_state, 0, sizeof(history_state));
    
    /* Create history directory if needed */
    ret = fs_stat(HISTORY_DIR, &entry);
    if (ret == -ENOENT) {
        ret = fs_mkdir(HISTORY_DIR);
        if (ret < 0 && ret != -EEXIST) {
            LOG_ERR("Failed to create history directory: %d", ret);
            return ret;
        }
        LOG_INF("Created history directory: %s", HISTORY_DIR);
    } else if (ret < 0) {
        LOG_ERR("Failed to access history directory %s: %d", HISTORY_DIR, ret);
        return ret;
    }
    
    /* Initialize each history file */
    for (int i = 0; i < HISTORY_TYPE_COUNT; i++) {
        ret = fs_stat(history_files[i].path, &entry);
        
        if (ret == -ENOENT) {
            /* File doesn't exist, create it */
            ret = create_history_file((history_type_t)i);
            if (ret < 0) {
                LOG_ERR("Failed to create %s: %d", history_files[i].path, ret);
                continue;
            }
        } else if (ret == 0) {
            /* File exists, read header */
            ret = read_header((history_type_t)i);
            if (ret < 0) {
                LOG_WRN("Invalid history file %s, recreating", history_files[i].path);
                fs_unlink(history_files[i].path);
                ret = create_history_file((history_type_t)i);
                if (ret < 0) {
                    continue;
                }
            }
        } else {
            LOG_ERR("Failed to stat %s: %d", history_files[i].path, ret);
            continue;
        }
        
        LOG_INF("History %s: %u/%u entries", 
                history_files[i].path,
                history_state[i].header.entry_count,
                history_state[i].header.capacity);
    }
    
    history_initialized = true;
    cached_stats.initialized = true;
    cached_stats.mounted = true;
    
    LOG_INF("History flash storage initialized");
    return 0;
}

int history_flash_deinit(void)
{
    if (!history_initialized) {
        return 0;
    }
    
    /* Flush any pending data by re-writing headers */
    for (int i = 0; i < HISTORY_TYPE_COUNT; i++) {
        if (history_state[i].valid) {
            write_header((history_type_t)i);
        }
    }
    
    history_initialized = false;
    cached_stats.initialized = false;
    
    LOG_INF("History flash storage deinitialized");
    return 0;
}

int history_flash_get_stats(history_flash_stats_t *stats)
{
    if (!stats) {
        return -EINVAL;
    }
    
    if (!history_initialized) {
        return -ENODEV;
    }
    
    k_mutex_lock(&history_mutex, K_FOREVER);
    
    /* Update cached stats from current state */
    #define UPDATE_FILE_STATS(idx, field) do { \
        if (history_state[idx].valid) { \
            cached_stats.field.entry_count = history_state[idx].header.entry_count; \
            cached_stats.field.capacity = history_state[idx].header.capacity; \
            cached_stats.field.file_size_bytes = HISTORY_HEADER_SIZE + \
                (uint32_t)history_state[idx].header.capacity * \
                history_files[idx].entry_size; \
        } \
    } while(0)
    
    UPDATE_FILE_STATS(HISTORY_TYPE_ENV_HOURLY, env_hourly);
    UPDATE_FILE_STATS(HISTORY_TYPE_ENV_DAILY, env_daily);
    UPDATE_FILE_STATS(HISTORY_TYPE_ENV_MONTHLY, env_monthly);
    UPDATE_FILE_STATS(HISTORY_TYPE_RAIN_HOURLY, rain_hourly);
    UPDATE_FILE_STATS(HISTORY_TYPE_RAIN_DAILY, rain_daily);
    
    #undef UPDATE_FILE_STATS
    
    /* Calculate total storage */
    cached_stats.total_storage_bytes = 
        cached_stats.env_hourly.file_size_bytes +
        cached_stats.env_daily.file_size_bytes +
        cached_stats.env_monthly.file_size_bytes +
        cached_stats.rain_hourly.file_size_bytes +
        cached_stats.rain_daily.file_size_bytes;
    
    memcpy(stats, &cached_stats, sizeof(*stats));
    
    k_mutex_unlock(&history_mutex);
    
    return 0;
}

/*******************************************************************************
 * Public API - Environmental History
 ******************************************************************************/

int history_flash_add_env_hourly(const history_env_hourly_t *entry)
{
    if (!entry) {
        return -EINVAL;
    }
    return add_entry(HISTORY_TYPE_ENV_HOURLY, entry);
}

int history_flash_add_env_daily(const history_env_daily_t *entry)
{
    if (!entry) {
        return -EINVAL;
    }
    return add_entry(HISTORY_TYPE_ENV_DAILY, entry);
}

int history_flash_add_env_monthly(const history_env_monthly_t *entry)
{
    if (!entry) {
        return -EINVAL;
    }
    return add_entry(HISTORY_TYPE_ENV_MONTHLY, entry);
}

int history_flash_read_env_hourly(uint16_t start_index,
                                  history_env_hourly_t *entries,
                                  uint16_t max_entries,
                                  uint16_t *count)
{
    if (!entries || !count) {
        return -EINVAL;
    }
    return read_entries(HISTORY_TYPE_ENV_HOURLY, start_index, 
                        entries, max_entries, count);
}

int history_flash_read_env_daily(uint16_t start_index,
                                 history_env_daily_t *entries,
                                 uint16_t max_entries,
                                 uint16_t *count)
{
    if (!entries || !count) {
        return -EINVAL;
    }
    return read_entries(HISTORY_TYPE_ENV_DAILY, start_index,
                        entries, max_entries, count);
}

int history_flash_read_env_monthly(uint16_t start_index,
                                   history_env_monthly_t *entries,
                                   uint16_t max_entries,
                                   uint16_t *count)
{
    if (!entries || !count) {
        return -EINVAL;
    }
    return read_entries(HISTORY_TYPE_ENV_MONTHLY, start_index,
                        entries, max_entries, count);
}

/*******************************************************************************
 * Public API - Rain History
 ******************************************************************************/

int history_flash_add_rain_hourly(const history_rain_hourly_t *entry)
{
    if (!entry) {
        return -EINVAL;
    }
    return add_entry(HISTORY_TYPE_RAIN_HOURLY, entry);
}

int history_flash_add_rain_daily(const history_rain_daily_t *entry)
{
    if (!entry) {
        return -EINVAL;
    }
    return add_entry(HISTORY_TYPE_RAIN_DAILY, entry);
}

int history_flash_read_rain_hourly(uint16_t start_index,
                                   history_rain_hourly_t *entries,
                                   uint16_t max_entries,
                                   uint16_t *count)
{
    if (!entries || !count) {
        return -EINVAL;
    }
    return read_entries(HISTORY_TYPE_RAIN_HOURLY, start_index,
                        entries, max_entries, count);
}

int history_flash_read_rain_daily(uint16_t start_index,
                                  history_rain_daily_t *entries,
                                  uint16_t max_entries,
                                  uint16_t *count)
{
    if (!entries || !count) {
        return -EINVAL;
    }
    return read_entries(HISTORY_TYPE_RAIN_DAILY, start_index,
                        entries, max_entries, count);
}

/*******************************************************************************
 * Public API - Query by Timestamp
 ******************************************************************************/

int history_flash_query_range(history_type_t type,
                              uint32_t start_ts,
                              uint32_t end_ts,
                              uint16_t *start_idx,
                              uint16_t *count)
{
    if (!start_idx || !count || type >= HISTORY_TYPE_COUNT) {
        return -EINVAL;
    }
    
    if (!history_state[type].valid) {
        return -ENOENT;
    }
    
    const history_file_header_t *hdr = &history_state[type].header;
    
    if (hdr->entry_count == 0) {
        *start_idx = 0;
        *count = 0;
        return 0;
    }
    
    /* For now, return all entries - caller can filter by timestamp */
    /* TODO: Implement binary search for efficient range queries */
    *start_idx = 0;
    *count = hdr->entry_count;
    
    return 0;
}

int history_flash_get_latest(history_type_t type, void *buffer, uint16_t *count)
{
    if (!buffer || !count || type >= HISTORY_TYPE_COUNT) {
        return -EINVAL;
    }
    
    if (!history_state[type].valid) {
        return -ENOENT;
    }
    
    const history_file_header_t *hdr = &history_state[type].header;
    uint16_t available = hdr->entry_count;
    uint16_t requested = *count;
    uint16_t to_read = MIN(available, requested);
    
    if (to_read == 0) {
        *count = 0;
        return 0;
    }
    
    /* Start from newest entries (at the end) */
    uint16_t start_logical = available - to_read;
    
    return read_entries(type, start_logical, buffer, to_read, count);
}

/*******************************************************************************
 * Public API - Maintenance
 ******************************************************************************/

int history_flash_clear(history_type_t type)
{
    if (type >= HISTORY_TYPE_COUNT) {
        return -EINVAL;
    }
    
    if (!history_state[type].valid) {
        return -ENOENT;
    }
    
    k_mutex_lock(&history_mutex, K_FOREVER);
    
    /* Reset header counters */
    history_state[type].header.entry_count = 0;
    history_state[type].header.head_index = 0;
    
    int ret = write_header(type);
    
    k_mutex_unlock(&history_mutex);
    
    LOG_INF("Cleared history: %s", history_files[type].path);
    
    return ret;
}

int history_flash_clear_all(void)
{
    int ret = 0;
    
    for (int i = 0; i < HISTORY_TYPE_COUNT; i++) {
        int r = history_flash_clear((history_type_t)i);
        if (r < 0 && r != -ENOENT) {
            ret = r;
        }
    }
    
    LOG_INF("Cleared all history data");
    return ret;
}

int history_flash_compact(void)
{
    /* LittleFS handles wear leveling automatically.
     * This function is provided for future optimization if needed.
     * Currently just syncs all files. */
    
    LOG_DBG("History compact requested (no-op for LittleFS)");
    return 0;
}
