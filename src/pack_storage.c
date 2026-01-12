/**
 * @file pack_storage.c
 * @brief External flash storage implementation for plant packs
 * 
 * Mounts LittleFS on ext_storage_partition at /lfs_ext and provides
 * atomic read/write operations for plant and pack files.
 */

#include "pack_storage.h"
#include "plant_db.h"
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/fs/fs.h>
#include <zephyr/fs/littlefs.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <stdio.h>

LOG_MODULE_REGISTER(pack_storage, CONFIG_LOG_DEFAULT_LEVEL);

/* ============================================================================
 * LittleFS Configuration
 * ============================================================================ */

/* Use ext_storage_partition from devicetree */
#define EXT_STORAGE_PARTITION_LABEL ext_storage_partition
#define EXT_STORAGE_PARTITION_ID    FIXED_PARTITION_ID(EXT_STORAGE_PARTITION_LABEL)

/* LittleFS mount configuration for pack storage */
FS_LITTLEFS_DECLARE_DEFAULT_CONFIG(pack_lfs_storage);

static struct fs_mount_t pack_lfs_mount = {
    .type = FS_LITTLEFS,
    .fs_data = &pack_lfs_storage,
    .storage_dev = (void *)EXT_STORAGE_PARTITION_ID,
    .mnt_point = PACK_MOUNT_POINT,
};

/* ============================================================================
 * State
 * ============================================================================ */

static bool pack_storage_initialized = false;
static struct k_mutex pack_storage_mutex;
static uint32_t pack_change_counter = 0;  /* Increments on each install/delete */

#define PACK_COUNTER_PATH PACK_BASE_PATH "/counter.bin"

/* ============================================================================
 * Change Counter Persistence
 * ============================================================================ */

static void load_change_counter(void)
{
    struct fs_file_t file;
    fs_file_t_init(&file);
    
    int rc = fs_open(&file, PACK_COUNTER_PATH, FS_O_READ);
    if (rc < 0) {
        pack_change_counter = 0;
        return;
    }
    
    ssize_t bytes = fs_read(&file, &pack_change_counter, sizeof(pack_change_counter));
    fs_close(&file);
    
    if (bytes != sizeof(pack_change_counter)) {
        pack_change_counter = 0;
    }
    
    LOG_INF("Loaded change_counter = %u", pack_change_counter);
}

static void save_change_counter(void)
{
    struct fs_file_t file;
    fs_file_t_init(&file);
    
    int rc = fs_open(&file, PACK_COUNTER_PATH, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC);
    if (rc < 0) {
        LOG_ERR("Failed to save change_counter: %d", rc);
        return;
    }
    
    fs_write(&file, &pack_change_counter, sizeof(pack_change_counter));
    fs_sync(&file);
    fs_close(&file);
}

static void increment_change_counter(void)
{
    pack_change_counter++;
    save_change_counter();
    LOG_DBG("change_counter = %u", pack_change_counter);
}

/* ============================================================================
 * CRC32 Calculation
 * ============================================================================ */

uint32_t pack_storage_crc32(const void *data, size_t len)
{
    const uint8_t *buf = data;
    uint32_t crc = 0xFFFFFFFF;
    
    for (size_t i = 0; i < len; i++) {
        crc ^= buf[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
        }
    }
    return ~crc;
}

/* ============================================================================
 * Path Helpers
 * ============================================================================ */

static void make_plant_path(uint16_t plant_id, char *buf, size_t buf_size)
{
    snprintf(buf, buf_size, "%s/p_%04X.bin", PACK_PLANTS_DIR, plant_id);
}

static void make_plant_temp_path(uint16_t plant_id, char *buf, size_t buf_size)
{
    snprintf(buf, buf_size, "%s/p_%04X.tmp", PACK_PLANTS_DIR, plant_id);
}

static void make_pack_path(uint16_t pack_id, char *buf, size_t buf_size)
{
    snprintf(buf, buf_size, "%s/k_%04X.bin", PACK_PACKS_DIR, pack_id);
}

static void make_pack_temp_path(uint16_t pack_id, char *buf, size_t buf_size)
{
    snprintf(buf, buf_size, "%s/k_%04X.tmp", PACK_PACKS_DIR, pack_id);
}

/* ============================================================================
 * Directory Management
 * ============================================================================ */

static int ensure_directory(const char *path)
{
    struct fs_dirent entry;
    int rc = fs_stat(path, &entry);
    
    if (rc == 0) {
        if (entry.type == FS_DIR_ENTRY_DIR) {
            return 0; /* Already exists */
        }
        LOG_ERR("Path exists but is not a directory: %s", path);
        return -ENOTDIR;
    }
    
    if (rc == -ENOENT) {
        rc = fs_mkdir(path);
        if (rc < 0) {
            LOG_ERR("Failed to create directory %s: %d", path, rc);
            return rc;
        }
        LOG_INF("Created directory: %s", path);
        return 0;
    }
    
    return rc;
}

/* ============================================================================
 * Initialization
 * ============================================================================ */

pack_result_t pack_storage_init(void)
{
    int rc;
    
    if (pack_storage_initialized) {
        return PACK_RESULT_SUCCESS;
    }
    
    k_mutex_init(&pack_storage_mutex);
    
    /* Mount LittleFS on ext_storage_partition */
    rc = fs_mount(&pack_lfs_mount);
    if (rc < 0) {
        if (rc == -ENOENT) {
            LOG_WRN("ext_storage_partition not found, pack storage unavailable");
        } else {
            LOG_ERR("Failed to mount pack storage: %d", rc);
        }
        return PACK_RESULT_IO_ERROR;
    }
    
    LOG_INF("Mounted pack storage at %s", PACK_MOUNT_POINT);
    
    /* Create required directories */
    rc = ensure_directory(PACK_BASE_PATH);
    if (rc < 0) {
        fs_unmount(&pack_lfs_mount);
        return PACK_RESULT_IO_ERROR;
    }
    
    rc = ensure_directory(PACK_PLANTS_DIR);
    if (rc < 0) {
        fs_unmount(&pack_lfs_mount);
        return PACK_RESULT_IO_ERROR;
    }
    
    rc = ensure_directory(PACK_PACKS_DIR);
    if (rc < 0) {
        fs_unmount(&pack_lfs_mount);
        return PACK_RESULT_IO_ERROR;
    }
    
    pack_storage_initialized = true;
    
    /* Load persisted change counter */
    load_change_counter();
    
    LOG_INF("Pack storage initialized successfully");
    
    return PACK_RESULT_SUCCESS;
}

bool pack_storage_is_ready(void)
{
    return pack_storage_initialized;
}

void pack_storage_deinit(void)
{
    if (pack_storage_initialized) {
        fs_unmount(&pack_lfs_mount);
        pack_storage_initialized = false;
        LOG_INF("Pack storage unmounted");
    }
}

/* ============================================================================
 * File I/O Helpers
 * ============================================================================ */

static pack_result_t read_plant_file(const char *path, pack_plant_v1_t *plant)
{
    struct fs_file_t file;
    pack_file_header_t header;
    int rc;
    
    fs_file_t_init(&file);
    rc = fs_open(&file, path, FS_O_READ);
    if (rc < 0) {
        if (rc == -ENOENT) {
            return PACK_RESULT_NOT_FOUND;
        }
        LOG_ERR("Failed to open %s: %d", path, rc);
        return PACK_RESULT_IO_ERROR;
    }
    
    /* Read header */
    ssize_t bytes = fs_read(&file, &header, sizeof(header));
    if (bytes != sizeof(header)) {
        fs_close(&file);
        LOG_ERR("Failed to read header from %s", path);
        return PACK_RESULT_IO_ERROR;
    }
    
    /* Validate header */
    if (header.magic != PACK_MAGIC_PLANT) {
        fs_close(&file);
        LOG_ERR("Invalid magic in %s: 0x%08X", path, header.magic);
        return PACK_RESULT_INVALID_DATA;
    }
    
    if (header.schema_version > PACK_SCHEMA_VERSION) {
        fs_close(&file);
        LOG_ERR("Unsupported schema version %d in %s", header.schema_version, path);
        return PACK_RESULT_INVALID_VERSION;
    }
    
    if (header.payload_size != sizeof(pack_plant_v1_t)) {
        fs_close(&file);
        LOG_ERR("Payload size mismatch in %s: %u vs %zu", 
                path, header.payload_size, sizeof(pack_plant_v1_t));
        return PACK_RESULT_INVALID_DATA;
    }
    
    /* Read payload */
    bytes = fs_read(&file, plant, sizeof(*plant));
    fs_close(&file);
    
    if (bytes != sizeof(*plant)) {
        LOG_ERR("Failed to read plant data from %s", path);
        return PACK_RESULT_IO_ERROR;
    }
    
    /* Validate CRC */
    uint32_t calculated_crc = pack_storage_crc32(plant, sizeof(*plant));
    if (calculated_crc != header.crc32) {
        LOG_ERR("CRC mismatch in %s: 0x%08X vs 0x%08X", 
                path, calculated_crc, header.crc32);
        return PACK_RESULT_CRC_MISMATCH;
    }
    
    return PACK_RESULT_SUCCESS;
}

static pack_result_t write_plant_file(const char *path, const pack_plant_v1_t *plant)
{
    struct fs_file_t file;
    pack_file_header_t header;
    int rc;
    
    /* Prepare header */
    header.magic = PACK_MAGIC_PLANT;
    header.schema_version = PACK_SCHEMA_VERSION;
    memset(header.reserved, 0, sizeof(header.reserved));
    header.payload_size = sizeof(pack_plant_v1_t);
    header.crc32 = pack_storage_crc32(plant, sizeof(*plant));
    
    /* Open file for writing */
    fs_file_t_init(&file);
    rc = fs_open(&file, path, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC);
    if (rc < 0) {
        LOG_ERR("Failed to create %s: %d", path, rc);
        return PACK_RESULT_IO_ERROR;
    }
    
    /* Write header */
    ssize_t bytes = fs_write(&file, &header, sizeof(header));
    if (bytes != sizeof(header)) {
        fs_close(&file);
        fs_unlink(path);
        LOG_ERR("Failed to write header to %s", path);
        return PACK_RESULT_IO_ERROR;
    }
    
    /* Write payload */
    bytes = fs_write(&file, plant, sizeof(*plant));
    if (bytes != sizeof(*plant)) {
        fs_close(&file);
        fs_unlink(path);
        LOG_ERR("Failed to write plant data to %s", path);
        return PACK_RESULT_IO_ERROR;
    }
    
    rc = fs_sync(&file);
    fs_close(&file);
    
    if (rc < 0) {
        fs_unlink(path);
        LOG_ERR("Failed to sync %s: %d", path, rc);
        return PACK_RESULT_IO_ERROR;
    }
    
    return PACK_RESULT_SUCCESS;
}

/* ============================================================================
 * Plant Operations
 * ============================================================================ */

pack_result_t pack_storage_get_plant(uint16_t plant_id, pack_plant_v1_t *plant)
{
    if (!pack_storage_initialized || !plant) {
        return PACK_RESULT_IO_ERROR;
    }
    
    if (plant_id == PLANT_ID_INVALID) {
        return PACK_RESULT_INVALID_DATA;
    }
    
    char path[64];
    make_plant_path(plant_id, path, sizeof(path));
    
    k_mutex_lock(&pack_storage_mutex, K_FOREVER);
    pack_result_t result = read_plant_file(path, plant);
    k_mutex_unlock(&pack_storage_mutex);
    
    return result;
}

pack_result_t pack_storage_install_plant(const pack_plant_v1_t *plant)
{
    if (!pack_storage_initialized || !plant) {
        return PACK_RESULT_IO_ERROR;
    }
    
    /* Validate input */
    pack_result_t result = pack_storage_validate_plant(plant);
    if (result != PACK_RESULT_SUCCESS) {
        return result;
    }
    
    char path[64];
    char temp_path[64];
    make_plant_path(plant->plant_id, path, sizeof(path));
    make_plant_temp_path(plant->plant_id, temp_path, sizeof(temp_path));
    
    k_mutex_lock(&pack_storage_mutex, K_FOREVER);
    
    /* Check if plant already exists with same or higher version */
    pack_plant_v1_t existing;
    result = read_plant_file(path, &existing);
    if (result == PACK_RESULT_SUCCESS) {
        if (existing.version >= plant->version) {
            k_mutex_unlock(&pack_storage_mutex);
            LOG_INF("Plant %04X already at version %u (incoming: %u)",
                    plant->plant_id, existing.version, plant->version);
            return PACK_RESULT_ALREADY_CURRENT;
        }
        LOG_INF("Updating plant %04X from version %u to %u",
                plant->plant_id, existing.version, plant->version);
    }
    
    /* Write to temp file first */
    result = write_plant_file(temp_path, plant);
    if (result != PACK_RESULT_SUCCESS) {
        k_mutex_unlock(&pack_storage_mutex);
        return result;
    }
    
    /* Validate temp file by reading it back */
    pack_plant_v1_t verify;
    result = read_plant_file(temp_path, &verify);
    if (result != PACK_RESULT_SUCCESS) {
        fs_unlink(temp_path);
        k_mutex_unlock(&pack_storage_mutex);
        LOG_ERR("Verification failed for plant %04X", plant->plant_id);
        return PACK_RESULT_INVALID_DATA;
    }
    
    /* Atomic rename: delete old file first, then rename temp */
    fs_unlink(path); /* Ignore error if doesn't exist */
    int rc = fs_rename(temp_path, path);
    if (rc < 0) {
        fs_unlink(temp_path);
        k_mutex_unlock(&pack_storage_mutex);
        LOG_ERR("Failed to rename temp file for plant %04X: %d", plant->plant_id, rc);
        return PACK_RESULT_IO_ERROR;
    }
    
    k_mutex_unlock(&pack_storage_mutex);
    
    increment_change_counter();  /* Persist for cache invalidation */
    LOG_INF("Installed plant %04X (version %u)", plant->plant_id, plant->version);
    return PACK_RESULT_UPDATED;
}

pack_result_t pack_storage_delete_plant(uint16_t plant_id)
{
    if (!pack_storage_initialized) {
        return PACK_RESULT_IO_ERROR;
    }
    
    if (plant_id == PLANT_ID_INVALID) {
        return PACK_RESULT_INVALID_DATA;
    }
    
    char path[64];
    make_plant_path(plant_id, path, sizeof(path));
    
    k_mutex_lock(&pack_storage_mutex, K_FOREVER);
    int rc = fs_unlink(path);
    k_mutex_unlock(&pack_storage_mutex);
    
    if (rc < 0) {
        if (rc == -ENOENT) {
            return PACK_RESULT_NOT_FOUND;
        }
        LOG_ERR("Failed to delete plant %04X: %d", plant_id, rc);
        return PACK_RESULT_IO_ERROR;
    }
    
    increment_change_counter();  /* Persist for cache invalidation */
    LOG_INF("Deleted plant %04X", plant_id);
    return PACK_RESULT_SUCCESS;
}

pack_result_t pack_storage_list_plants(pack_plant_list_entry_t *entries,
                                       uint16_t max_entries,
                                       uint16_t *out_count,
                                       uint16_t offset)
{
    if (!pack_storage_initialized || !entries || !out_count) {
        return PACK_RESULT_IO_ERROR;
    }
    
    struct fs_dir_t dir;
    struct fs_dirent dirent;
    uint16_t count = 0;
    uint16_t skipped = 0;
    int rc;
    
    fs_dir_t_init(&dir);
    
    k_mutex_lock(&pack_storage_mutex, K_FOREVER);
    
    rc = fs_opendir(&dir, PACK_PLANTS_DIR);
    if (rc < 0) {
        k_mutex_unlock(&pack_storage_mutex);
        if (rc == -ENOENT) {
            *out_count = 0;
            return PACK_RESULT_SUCCESS;
        }
        LOG_ERR("Failed to open plants directory: %d", rc);
        return PACK_RESULT_IO_ERROR;
    }
    
    while (count < max_entries) {
        rc = fs_readdir(&dir, &dirent);
        if (rc < 0 || dirent.name[0] == 0) {
            break; /* End of directory */
        }
        
        /* Skip non-plant files */
        if (dirent.type != FS_DIR_ENTRY_FILE) {
            continue;
        }
        if (dirent.name[0] != 'p' || dirent.name[1] != '_') {
            continue;
        }
        if (strstr(dirent.name, ".bin") == NULL) {
            continue;
        }
        
        /* Handle pagination offset */
        if (skipped < offset) {
            skipped++;
            continue;
        }
        
        /* Parse plant ID from filename (p_XXXX.bin) */
        uint16_t plant_id;
        if (sscanf(dirent.name, "p_%04hX.bin", &plant_id) != 1) {
            continue;
        }
        
        /* Read plant to get metadata */
        char path[64];
        make_plant_path(plant_id, path, sizeof(path));
        
        pack_plant_v1_t plant;
        pack_result_t result = read_plant_file(path, &plant);
        if (result != PACK_RESULT_SUCCESS) {
            LOG_WRN("Skipping corrupt plant file: %s", dirent.name);
            continue;
        }
        
        /* Fill entry */
        entries[count].plant_id = plant.plant_id;
        entries[count].pack_id = plant.pack_id;
        entries[count].version = plant.version;
        entries[count].source = (plant.pack_id == 0) ? PLANT_SOURCE_CUSTOM : PLANT_SOURCE_PACK;
        strncpy(entries[count].name, plant.common_name, PACK_COMMON_NAME_MAX_LEN - 1);
        entries[count].name[PACK_COMMON_NAME_MAX_LEN - 1] = '\0';
        
        count++;
    }
    
    fs_closedir(&dir);
    k_mutex_unlock(&pack_storage_mutex);
    
    *out_count = count;
    return PACK_RESULT_SUCCESS;
}

uint16_t pack_storage_get_plant_count(void)
{
    if (!pack_storage_initialized) {
        return 0;
    }
    
    struct fs_dir_t dir;
    struct fs_dirent dirent;
    uint16_t count = 0;
    int rc;
    
    fs_dir_t_init(&dir);
    
    k_mutex_lock(&pack_storage_mutex, K_FOREVER);
    
    rc = fs_opendir(&dir, PACK_PLANTS_DIR);
    if (rc < 0) {
        k_mutex_unlock(&pack_storage_mutex);
        return 0;
    }
    
    while (true) {
        rc = fs_readdir(&dir, &dirent);
        if (rc < 0 || dirent.name[0] == 0) {
            break;
        }
        
        if (dirent.type == FS_DIR_ENTRY_FILE &&
            dirent.name[0] == 'p' && dirent.name[1] == '_' &&
            strstr(dirent.name, ".bin") != NULL) {
            count++;
        }
    }
    
    fs_closedir(&dir);
    k_mutex_unlock(&pack_storage_mutex);
    
    return count;
}

/* ============================================================================
 * Pack Operations (stubs for now - will implement with issue #7)
 * ============================================================================ */

pack_result_t pack_storage_get_pack(uint16_t pack_id, 
                                    pack_pack_v1_t *pack,
                                    uint16_t *plant_ids,
                                    uint16_t max_plant_ids)
{
    if (!pack_storage_initialized || !pack) {
        return PACK_RESULT_IO_ERROR;
    }
    
    /* Handle built-in pack */
    if (pack_id == PACK_ID_BUILTIN) {
        pack->pack_id = PACK_ID_BUILTIN;
        pack->version = 1;
        strncpy(pack->name, "Built-in Database", PACK_NAME_MAX_LEN - 1);
        pack->name[PACK_NAME_MAX_LEN - 1] = '\0';
        pack->plant_count = PLANT_FULL_SPECIES_COUNT;
        pack->reserved = 0;
        
        /* Fill plant IDs if requested */
        if (plant_ids && max_plant_ids > 0) {
            uint16_t to_fill = (max_plant_ids < PLANT_FULL_SPECIES_COUNT) ? 
                               max_plant_ids : PLANT_FULL_SPECIES_COUNT;
            for (uint16_t i = 0; i < to_fill; i++) {
                plant_ids[i] = i;
            }
        }
        
        return PACK_RESULT_SUCCESS;
    }
    
    /* TODO: Read pack file from flash */
    char path[64];
    make_pack_path(pack_id, path, sizeof(path));
    
    struct fs_dirent entry;
    if (fs_stat(path, &entry) < 0) {
        return PACK_RESULT_NOT_FOUND;
    }
    
    /* Pack file reading implementation needed */
    return PACK_RESULT_NOT_FOUND;
}

pack_result_t pack_storage_install_pack(const pack_pack_v1_t *pack,
                                        const pack_plant_v1_t *plants,
                                        uint16_t plant_count)
{
    if (!pack_storage_initialized || !pack) {
        return PACK_RESULT_IO_ERROR;
    }
    
    if (pack->pack_id == PACK_ID_BUILTIN || pack->pack_id == PACK_ID_INVALID) {
        return PACK_RESULT_INVALID_DATA;
    }
    
    /* Install all plants first */
    if (plants && plant_count > 0) {
        for (uint16_t i = 0; i < plant_count; i++) {
            pack_result_t result = pack_storage_install_plant(&plants[i]);
            if (result != PACK_RESULT_SUCCESS && 
                result != PACK_RESULT_UPDATED &&
                result != PACK_RESULT_ALREADY_CURRENT) {
                LOG_ERR("Failed to install plant %u of pack %04X", i, pack->pack_id);
                return result;
            }
        }
    }
    
    /* TODO: Write pack manifest file */
    
    increment_change_counter();  /* Pack installed */
    LOG_INF("Installed pack %04X with %u plants", pack->pack_id, plant_count);
    
    return PACK_RESULT_SUCCESS;
}

pack_result_t pack_storage_delete_pack(uint16_t pack_id, bool delete_plants)
{
    if (!pack_storage_initialized) {
        return PACK_RESULT_IO_ERROR;
    }
    
    if (pack_id == PACK_ID_BUILTIN || pack_id == PACK_ID_INVALID) {
        return PACK_RESULT_INVALID_DATA;
    }
    
    uint16_t deleted_count = 0;
    
    /* Delete all plants belonging to this pack if requested */
    if (delete_plants) {
        struct fs_dir_t dir;
        struct fs_dirent dirent;
        pack_plant_v1_t plant;
        char path[64];
        int rc;
        
        fs_dir_t_init(&dir);
        
        k_mutex_lock(&pack_storage_mutex, K_FOREVER);
        
        rc = fs_opendir(&dir, PACK_PLANTS_DIR);
        if (rc == 0) {
            while (true) {
                rc = fs_readdir(&dir, &dirent);
                if (rc < 0 || dirent.name[0] == 0) {
                    break;
                }
                
                if (dirent.type == FS_DIR_ENTRY_FILE &&
                    dirent.name[0] == 'p' && dirent.name[1] == '_' &&
                    strstr(dirent.name, ".bin") != NULL &&
                    strlen(dirent.name) < 32) {  /* Sanity check filename length */
                    
                    snprintf(path, sizeof(path), "%s/%.31s", PACK_PLANTS_DIR, dirent.name);
                    
                    /* Read plant to check pack_id */
                    pack_result_t result = read_plant_file(path, &plant);
                    if (result == PACK_RESULT_SUCCESS && plant.pack_id == pack_id) {
                        /* Delete this plant */
                        fs_unlink(path);
                        deleted_count++;
                        LOG_INF("Deleted plant %04X from pack %04X", plant.plant_id, pack_id);
                    }
                }
            }
            fs_closedir(&dir);
        }
        
        k_mutex_unlock(&pack_storage_mutex);
    }
    
    /* Delete pack manifest file */
    char pack_path[64];
    make_pack_path(pack_id, pack_path, sizeof(pack_path));
    
    k_mutex_lock(&pack_storage_mutex, K_FOREVER);
    int rc = fs_unlink(pack_path);
    k_mutex_unlock(&pack_storage_mutex);
    
    /* If we deleted plants or the manifest existed, count as success */
    if (deleted_count > 0 || rc == 0) {
        increment_change_counter();  /* Pack deleted */
        LOG_INF("Deleted pack %04X (%u plants removed)", pack_id, deleted_count);
        return PACK_RESULT_SUCCESS;
    }
    
    if (rc == -ENOENT) {
        return PACK_RESULT_NOT_FOUND;
    }
    
    LOG_ERR("Failed to delete pack %04X: %d", pack_id, rc);
    return PACK_RESULT_IO_ERROR;
}

pack_result_t pack_storage_list_packs(pack_pack_list_entry_t *entries,
                                      uint16_t max_entries,
                                      uint16_t *out_count,
                                      uint16_t offset)
{
    if (!entries || !out_count) {
        return PACK_RESULT_IO_ERROR;
    }
    
    uint16_t count = 0;
    
    /* First entry is always built-in pack (if room and offset allows) */
    if (offset == 0 && max_entries > 0) {
        entries[0].pack_id = PACK_ID_BUILTIN;
        entries[0].version = 1;
        entries[0].plant_count = PLANT_FULL_SPECIES_COUNT;
        strncpy(entries[0].name, "Built-in Database", PACK_NAME_MAX_LEN - 1);
        entries[0].name[PACK_NAME_MAX_LEN - 1] = '\0';
        count = 1;
    }
    
    /* TODO: List installed packs from flash */
    
    *out_count = count;
    return PACK_RESULT_SUCCESS;
}

uint16_t pack_storage_get_pack_count(void)
{
    /* TODO: Count pack files */
    return 0; /* Excludes built-in */
}

/* ============================================================================
 * Built-in Database Integration
 * ============================================================================ */

bool pack_storage_is_builtin_plant(uint16_t plant_id, uint16_t pack_id)
{
    return (pack_id == PACK_ID_BUILTIN && plant_id < PLANT_FULL_SPECIES_COUNT);
}

pack_result_t pack_storage_get_builtin_pack(pack_pack_list_entry_t *pack)
{
    if (!pack) {
        return PACK_RESULT_INVALID_DATA;
    }
    
    pack->pack_id = PACK_ID_BUILTIN;
    pack->version = 1;
    pack->plant_count = PLANT_FULL_SPECIES_COUNT;
    strncpy(pack->name, "Built-in Database", PACK_NAME_MAX_LEN - 1);
    pack->name[PACK_NAME_MAX_LEN - 1] = '\0';
    
    return PACK_RESULT_SUCCESS;
}

/* ============================================================================
 * Utility Functions
 * ============================================================================ */

pack_result_t pack_storage_get_stats(pack_storage_stats_t *stats)
{
    if (!stats) {
        return PACK_RESULT_INVALID_DATA;
    }
    
    memset(stats, 0, sizeof(*stats));
    
    if (!pack_storage_initialized) {
        return PACK_RESULT_IO_ERROR;
    }
    
    struct fs_statvfs stat;
    int rc = fs_statvfs(PACK_MOUNT_POINT, &stat);
    if (rc < 0) {
        LOG_ERR("Failed to get storage stats: %d", rc);
        return PACK_RESULT_IO_ERROR;
    }
    
    stats->total_bytes = stat.f_bsize * stat.f_blocks;
    stats->free_bytes = stat.f_bsize * stat.f_bfree;
    stats->used_bytes = stats->total_bytes - stats->free_bytes;
    stats->plant_count = pack_storage_get_plant_count();
    stats->pack_count = pack_storage_get_pack_count();
    stats->change_counter = pack_change_counter;
    
    return PACK_RESULT_SUCCESS;
}

pack_result_t pack_storage_validate_plant(const pack_plant_v1_t *plant)
{
    if (!plant) {
        return PACK_RESULT_INVALID_DATA;
    }
    
    /* Check ID validity */
    if (plant->plant_id == PLANT_ID_INVALID) {
        LOG_ERR("Invalid plant_id: 0x%04X", plant->plant_id);
        return PACK_RESULT_INVALID_DATA;
    }
    
    /* Check pack_id is not claiming built-in */
    if (plant->pack_id == PACK_ID_BUILTIN && plant->plant_id >= PLANT_FULL_SPECIES_COUNT) {
        LOG_ERR("Plant claims built-in pack but ID out of range: %u", plant->plant_id);
        return PACK_RESULT_INVALID_DATA;
    }
    
    /* Check name is not empty */
    if (plant->common_name[0] == '\0') {
        LOG_ERR("Plant has empty common_name");
        return PACK_RESULT_INVALID_DATA;
    }
    
    /* Check Kc values are reasonable */
    if (plant->kc_ini_x1000 > 2000 || plant->kc_mid_x1000 > 2000 ||
        plant->kc_end_x1000 > 2000 || plant->kc_dev_x1000 > 2000) {
        LOG_ERR("Plant has Kc values out of range");
        return PACK_RESULT_INVALID_DATA;
    }
    
    /* Check root depth makes sense */
    if (plant->root_depth_min_mm > plant->root_depth_max_mm) {
        LOG_ERR("Plant has min root depth > max root depth");
        return PACK_RESULT_INVALID_DATA;
    }
    
    if (plant->root_depth_max_mm > 5000) { /* 5 meters max */
        LOG_ERR("Plant has unreasonable max root depth: %u mm", plant->root_depth_max_mm);
        return PACK_RESULT_INVALID_DATA;
    }
    
    return PACK_RESULT_SUCCESS;
}

/* ============================================================================
 * FAO-56 Integration Helpers
 * ============================================================================ */

/**
 * @brief Linear interpolation helper for Kc stages
 */
static float interpolate_kc(float kc_start, float kc_end, 
                            uint16_t day_in_stage, uint16_t stage_length)
{
    if (stage_length == 0) {
        return kc_end;
    }
    float t = (float)day_in_stage / (float)stage_length;
    if (t > 1.0f) t = 1.0f;
    return kc_start + (kc_end - kc_start) * t;
}

pack_result_t pack_storage_get_kc(uint16_t plant_id,
                                   uint16_t days_after_planting,
                                   float *out_kc)
{
    if (!out_kc) {
        return PACK_RESULT_INVALID_DATA;
    }
    
    /* Default Kc if nothing found */
    *out_kc = 1.0f;
    
    if (plant_id == 0) {
        LOG_WRN("No plant configured (plant_id=0)");
        return PACK_RESULT_INVALID_DATA;
    }
    
    /* Load from pack storage (all plants are there after provisioning) */
    pack_plant_v1_t plant;
    pack_result_t res = pack_storage_get_plant(plant_id, &plant);
    if (res != PACK_RESULT_SUCCESS) {
        LOG_ERR("Failed to load plant %u for Kc: %d", plant_id, res);
        return res;
    }
    
    /* Convert x1000 values to float */
    float kc_ini = (float)plant.kc_ini_x1000 / 1000.0f;
    float kc_dev = (float)plant.kc_dev_x1000 / 1000.0f;
    float kc_mid = (float)plant.kc_mid_x1000 / 1000.0f;
    float kc_end = (float)plant.kc_end_x1000 / 1000.0f;
    
    /* Calculate Kc based on growth stage */
    uint16_t l_ini = plant.stage_days_ini;
    uint16_t l_dev = plant.stage_days_dev;
    uint16_t l_mid = plant.stage_days_mid;
    uint16_t l_late = plant.stage_days_end;
    
    if (days_after_planting < l_ini) {
        /* Initial stage */
        *out_kc = kc_ini;
    } else if (days_after_planting < l_ini + l_dev) {
        /* Development stage - interpolate from kc_dev to kc_mid */
        uint16_t day_in_stage = days_after_planting - l_ini;
        *out_kc = interpolate_kc(kc_dev, kc_mid, day_in_stage, l_dev);
    } else if (days_after_planting < l_ini + l_dev + l_mid) {
        /* Mid-season stage */
        *out_kc = kc_mid;
    } else if (days_after_planting < l_ini + l_dev + l_mid + l_late) {
        /* Late season stage - interpolate from kc_mid to kc_end */
        uint16_t day_in_stage = days_after_planting - (l_ini + l_dev + l_mid);
        *out_kc = interpolate_kc(kc_mid, kc_end, day_in_stage, l_late);
    } else {
        /* Post-season */
        *out_kc = kc_end;
    }
    
    LOG_DBG("Plant %u DAP=%u -> Kc=%.2f", plant_id, days_after_planting, (double)*out_kc);
    return PACK_RESULT_SUCCESS;
}

pack_result_t pack_storage_get_root_depth(uint16_t plant_id,
                                           uint16_t days_after_planting,
                                           float *out_root_depth_mm)
{
    if (!out_root_depth_mm) {
        return PACK_RESULT_INVALID_DATA;
    }
    
    /* Default root depth */
    *out_root_depth_mm = 300.0f;
    
    if (plant_id == 0) {
        LOG_WRN("No plant configured (plant_id=0)");
        return PACK_RESULT_INVALID_DATA;
    }
    
    /* Load from pack storage */
    pack_plant_v1_t plant;
    pack_result_t res = pack_storage_get_plant(plant_id, &plant);
    if (res != PACK_RESULT_SUCCESS) {
        LOG_ERR("Failed to load plant %u for root depth: %d", plant_id, res);
        return res;
    }
    
    /* Calculate root depth based on growth stage */
    uint16_t total_season = plant.stage_days_ini + plant.stage_days_dev + 
                            plant.stage_days_mid + plant.stage_days_end;
    
    if (total_season == 0 || days_after_planting >= total_season) {
        *out_root_depth_mm = (float)plant.root_depth_max_mm;
    } else {
        /* Linear interpolation from min to max */
        float t = (float)days_after_planting / (float)total_season;
        *out_root_depth_mm = (float)plant.root_depth_min_mm + 
                             t * ((float)plant.root_depth_max_mm - (float)plant.root_depth_min_mm);
    }
    
    return PACK_RESULT_SUCCESS;
}

pack_result_t pack_storage_get_fao56_plant(uint16_t plant_id,
                                            pack_plant_v1_t *plant)
{
    if (!plant || plant_id == 0) {
        return PACK_RESULT_INVALID_DATA;
    }
    
    return pack_storage_get_plant(plant_id, plant);
}

/* ============================================================================
 * Default Plant Provisioning
 * ============================================================================ */

/**
 * @brief Convert ROM plant_full_data_t to pack_plant_v1_t
 */
static void rom_to_pack_plant(uint16_t plant_id, const plant_full_data_t *rom, pack_plant_v1_t *pack)
{
    memset(pack, 0, sizeof(*pack));
    
    /* Identification - use sequential IDs starting from 1 */
    pack->plant_id = plant_id;
    pack->pack_id = 0;      /* Built-in plants have no pack */
    pack->version = 1;
    
    /* Names - copy from ROM (truncate if needed) */
    if (rom->common_name_en) {
        strncpy(pack->common_name, rom->common_name_en, PACK_COMMON_NAME_MAX_LEN - 1);
    }
    if (rom->scientific_name) {
        strncpy(pack->scientific_name, rom->scientific_name, PACK_SCIENTIFIC_NAME_MAX_LEN - 1);
    }
    
    /* Crop coefficients (same x1000 format) */
    pack->kc_ini_x1000 = rom->kc_ini_x1000;
    pack->kc_dev_x1000 = rom->kc_dev_x1000;
    pack->kc_mid_x1000 = rom->kc_mid_x1000;
    pack->kc_end_x1000 = rom->kc_end_x1000;
    
    /* Root depth: ROM uses m×1000, pack uses mm (same numeric value) */
    pack->root_depth_min_mm = rom->root_depth_min_m_x1000;
    pack->root_depth_max_mm = rom->root_depth_max_m_x1000;
    
    /* Growth stages */
    pack->stage_days_ini = rom->stage_days_ini;
    pack->stage_days_dev = rom->stage_days_dev;
    pack->stage_days_mid = rom->stage_days_mid;
    pack->stage_days_end = rom->stage_days_end;
    pack->growth_cycle = rom->growth_cycle;
    
    /* Depletion and spacing */
    pack->depletion_fraction_p_x1000 = rom->depletion_fraction_p_x1000;
    pack->spacing_row_mm = rom->spacing_row_m_x1000;      /* m×1000 = mm */
    pack->spacing_plant_mm = rom->spacing_plant_m_x1000;  /* m×1000 = mm */
    pack->density_x100 = rom->default_density_plants_m2_x100;
    pack->canopy_max_x1000 = rom->canopy_cover_max_frac_x1000;
    
    /* Temperature */
    pack->frost_tolerance_c = rom->frost_tolerance_c;
    pack->temp_opt_min_c = rom->temp_opt_min_c;
    pack->temp_opt_max_c = rom->temp_opt_max_c;
    
    /* Irrigation */
    pack->typ_irrig_method_id = rom->typ_irrig_method_id;
    
    /* User-adjustable defaults (unified system - no separate custom_plant struct) */
    pack->water_need_factor_x100 = 100;  /* Default 1.0x */
    pack->irrigation_freq_days = 3;       /* Default 3 days */
    pack->prefer_area_based = 1;          /* Default to area-based */
}

pack_result_t pack_storage_provision_defaults(void)
{
    if (!pack_storage_initialized) {
        LOG_ERR("Pack storage not mounted for provisioning");
        return PACK_RESULT_IO_ERROR;
    }
    
    LOG_INF("Provisioning %u default plants from ROM to flash...", PLANT_FULL_SPECIES_COUNT);
    
    uint16_t provisioned = 0;
    uint16_t skipped = 0;
    uint16_t failed = 0;
    
    for (uint16_t i = 0; i < PLANT_FULL_SPECIES_COUNT; i++) {
        uint16_t plant_id = i + 1;  /* Plant IDs start at 1 */
        
        /* Check if already exists */
        pack_plant_v1_t existing;
        pack_result_t check = pack_storage_get_plant(plant_id, &existing);
        if (check == PACK_RESULT_SUCCESS) {
            skipped++;
            continue;  /* Already provisioned */
        }
        
        /* Convert ROM to pack format */
        pack_plant_v1_t pack_plant;
        rom_to_pack_plant(plant_id, &plant_full_database[i], &pack_plant);
        
        /* Save to flash using install function */
        pack_result_t res = pack_storage_install_plant(&pack_plant);
        if (res != PACK_RESULT_SUCCESS && res != PACK_RESULT_ALREADY_CURRENT) {
            LOG_ERR("Failed to provision plant %u (%s): %d", 
                    plant_id, pack_plant.common_name, res);
            failed++;
        } else {
            provisioned++;
        }
        
        /* Yield periodically to avoid watchdog issues */
        if ((i % 20) == 19) {
            k_yield();
        }
    }
    
    LOG_INF("Provisioning complete: %u new, %u existing, %u failed", 
            provisioned, skipped, failed);
    
    return (failed > 0) ? PACK_RESULT_IO_ERROR : PACK_RESULT_SUCCESS;
}

bool pack_storage_defaults_provisioned(void)
{
    if (!pack_storage_initialized) {
        return false;
    }
    
    /* Check if plant ID 1 and last plant exist */
    pack_plant_v1_t plant;
    if (pack_storage_get_plant(1, &plant) != PACK_RESULT_SUCCESS) {
        return false;
    }
    if (pack_storage_get_plant(PLANT_FULL_SPECIES_COUNT, &plant) != PACK_RESULT_SUCCESS) {
        return false;
    }
    
    return true;
}
