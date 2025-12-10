/**
 * @file database_flash.c
 * @brief External flash database storage implementation using LittleFS
 */

#include "database_flash.h"
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/fs/fs.h>
#include <zephyr/fs/littlefs.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <stdlib.h>

LOG_MODULE_REGISTER(database_flash, CONFIG_LOG_DEFAULT_LEVEL);

/*******************************************************************************
 * LittleFS Configuration
 ******************************************************************************/
/* Use database_partition from devicetree */
#define DATABASE_PARTITION_LABEL database_partition
#define DATABASE_PARTITION_ID    FIXED_PARTITION_ID(DATABASE_PARTITION_LABEL)

/* LittleFS mount configuration */
FS_LITTLEFS_DECLARE_DEFAULT_CONFIG(lfs_storage);

static struct fs_mount_t lfs_mount = {
    .type = FS_LITTLEFS,
    .fs_data = &lfs_storage,
    .storage_dev = (void *)DATABASE_PARTITION_ID,
    .mnt_point = DB_MOUNT_POINT,
};

/*******************************************************************************
 * Static State
 ******************************************************************************/
static db_flash_handle_t db_handle = {0};
static bool initialized = false;

/* RAM buffers for loaded databases */
static db_plant_record_t *plant_cache = NULL;
static db_soil_record_t *soil_cache = NULL;
static db_irrigation_record_t *irrigation_cache = NULL;

/*******************************************************************************
 * Helper Functions
 ******************************************************************************/
static uint32_t crc32_calc(const void *data, size_t len)
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

static int read_db_header(const char *path, db_file_header_t *header)
{
    struct fs_file_t file;
    fs_file_t_init(&file);
    
    int rc = fs_open(&file, path, FS_O_READ);
    if (rc < 0) {
        LOG_ERR("Failed to open %s: %d", path, rc);
        return rc;
    }
    
    ssize_t bytes = fs_read(&file, header, sizeof(*header));
    fs_close(&file);
    
    if (bytes != sizeof(*header)) {
        LOG_ERR("Failed to read header from %s", path);
        return -EIO;
    }
    
    return 0;
}

static int load_database_file(const char *path, uint32_t expected_magic,
                              size_t record_size, void **out_data,
                              uint16_t *out_count)
{
    db_file_header_t header;
    struct fs_file_t file;
    int rc;
    
    /* Read and validate header */
    rc = read_db_header(path, &header);
    if (rc < 0) {
        return rc;
    }
    
    if (header.magic != expected_magic) {
        LOG_ERR("Invalid magic in %s: 0x%08X (expected 0x%08X)",
                path, header.magic, expected_magic);
        return -EINVAL;
    }
    
    if (header.version != DB_VERSION_CURRENT) {
        LOG_WRN("Version mismatch in %s: %d (current: %d)",
                path, header.version, DB_VERSION_CURRENT);
    }
    
    if (header.record_size != record_size) {
        LOG_ERR("Record size mismatch in %s: %d (expected %zu)",
                path, header.record_size, record_size);
        return -EINVAL;
    }
    
    if (header.record_count == 0) {
        LOG_WRN("Empty database: %s", path);
        *out_data = NULL;
        *out_count = 0;
        return 0;
    }
    
    /* Allocate buffer for records */
    size_t data_size = header.record_count * record_size;
    void *data = k_malloc(data_size);
    if (!data) {
        LOG_ERR("Failed to allocate %zu bytes for %s", data_size, path);
        return -ENOMEM;
    }
    
    /* Read records */
    fs_file_t_init(&file);
    rc = fs_open(&file, path, FS_O_READ);
    if (rc < 0) {
        k_free(data);
        return rc;
    }
    
    /* Skip header */
    rc = fs_seek(&file, DB_HEADER_SIZE, FS_SEEK_SET);
    if (rc < 0) {
        fs_close(&file);
        k_free(data);
        return rc;
    }
    
    /* Read all records */
    ssize_t bytes = fs_read(&file, data, data_size);
    fs_close(&file);
    
    if (bytes != (ssize_t)data_size) {
        LOG_ERR("Short read from %s: %zd/%zu", path, bytes, data_size);
        k_free(data);
        return -EIO;
    }
    
    /* Verify CRC */
    uint32_t calc_crc = crc32_calc(data, data_size);
    if (calc_crc != header.crc32) {
        LOG_ERR("CRC mismatch in %s: 0x%08X (expected 0x%08X)",
                path, calc_crc, header.crc32);
        k_free(data);
        return -EILSEQ;
    }
    
    LOG_INF("Loaded %s: %d records (%zu bytes)", 
            path, header.record_count, data_size);
    
    *out_data = data;
    *out_count = header.record_count;
    return header.record_count;
}

/*******************************************************************************
 * Public API Implementation
 ******************************************************************************/
int db_flash_init(void)
{
    if (initialized) {
        return 0;
    }
    
    memset(&db_handle, 0, sizeof(db_handle));
    initialized = true;
    
    LOG_INF("Database flash storage initialized");
    return 0;
}

int db_flash_mount(void)
{
    if (!initialized) {
        db_flash_init();
    }
    
    if (db_handle.mounted) {
        return 0;
    }
    
    int rc = fs_mount(&lfs_mount);
    if (rc < 0) {
        if (rc == -ENODEV) {
            LOG_WRN("No filesystem, formatting...");
            rc = db_flash_format();
            if (rc < 0) {
                return rc;
            }
            rc = fs_mount(&lfs_mount);
        }
        if (rc < 0) {
            LOG_ERR("Failed to mount LittleFS: %d", rc);
            return rc;
        }
    }
    
    db_handle.mounted = true;
    LOG_INF("LittleFS mounted at %s", DB_MOUNT_POINT);
    return 0;
}

int db_flash_unmount(void)
{
    if (!db_handle.mounted) {
        return 0;
    }
    
    db_flash_unload_all();
    
    int rc = fs_unmount(&lfs_mount);
    if (rc < 0) {
        LOG_ERR("Failed to unmount LittleFS: %d", rc);
        return rc;
    }
    
    db_handle.mounted = false;
    LOG_INF("LittleFS unmounted");
    return 0;
}

int db_flash_format(void)
{
    /* Unmount first if mounted */
    if (db_handle.mounted) {
        fs_unmount(&lfs_mount);
        db_handle.mounted = false;
    }
    
    /* Get flash area info */
    const struct flash_area *fa;
    int rc = flash_area_open(DATABASE_PARTITION_ID, &fa);
    if (rc < 0) {
        LOG_ERR("Failed to open flash area: %d", rc);
        return rc;
    }
    
    LOG_WRN("Erasing database partition (%u bytes)...", (unsigned)fa->fa_size);
    rc = flash_area_erase(fa, 0, fa->fa_size);
    flash_area_close(fa);
    
    if (rc < 0) {
        LOG_ERR("Failed to erase partition: %d", rc);
        return rc;
    }
    
    /* Mount to create filesystem */
    rc = fs_mount(&lfs_mount);
    if (rc < 0) {
        LOG_ERR("Failed to format/mount: %d", rc);
        return rc;
    }
    
    db_handle.mounted = true;
    LOG_INF("Database partition formatted");
    return 0;
}

int db_flash_load_plants(void)
{
    if (!db_handle.mounted) {
        int rc = db_flash_mount();
        if (rc < 0) return rc;
    }
    
    if (db_handle.plants_loaded && plant_cache) {
        return db_handle.plant_count;
    }
    
    /* Free old cache if any */
    if (plant_cache) {
        k_free(plant_cache);
        plant_cache = NULL;
    }
    
    void *data = NULL;
    uint16_t count = 0;
    
    int rc = load_database_file(DB_PATH_PLANTS, DB_MAGIC_PLANT,
                                DB_PLANT_RECORD_SIZE, &data, &count);
    if (rc < 0) {
        return rc;
    }
    
    plant_cache = data;
    db_handle.plants = plant_cache;
    db_handle.plant_count = count;
    db_handle.plants_loaded = true;
    
    return count;
}

int db_flash_load_soils(void)
{
    if (!db_handle.mounted) {
        int rc = db_flash_mount();
        if (rc < 0) return rc;
    }
    
    if (db_handle.soils_loaded && soil_cache) {
        return db_handle.soil_count;
    }
    
    if (soil_cache) {
        k_free(soil_cache);
        soil_cache = NULL;
    }
    
    void *data = NULL;
    uint16_t count = 0;
    
    int rc = load_database_file(DB_PATH_SOILS, DB_MAGIC_SOIL,
                                DB_SOIL_RECORD_SIZE, &data, &count);
    if (rc < 0) {
        return rc;
    }
    
    soil_cache = data;
    db_handle.soils = soil_cache;
    db_handle.soil_count = count;
    db_handle.soils_loaded = true;
    
    return count;
}

int db_flash_load_irrigation(void)
{
    if (!db_handle.mounted) {
        int rc = db_flash_mount();
        if (rc < 0) return rc;
    }
    
    if (db_handle.irrigation_loaded && irrigation_cache) {
        return db_handle.irrigation_count;
    }
    
    if (irrigation_cache) {
        k_free(irrigation_cache);
        irrigation_cache = NULL;
    }
    
    void *data = NULL;
    uint16_t count = 0;
    
    int rc = load_database_file(DB_PATH_IRRIGATION, DB_MAGIC_IRRIGATION,
                                DB_IRRIGATION_RECORD_SIZE, &data, &count);
    if (rc < 0) {
        return rc;
    }
    
    irrigation_cache = data;
    db_handle.irrigation = irrigation_cache;
    db_handle.irrigation_count = count;
    db_handle.irrigation_loaded = true;
    
    return count;
}

const db_plant_record_t *db_flash_get_plant(uint16_t index)
{
    if (!db_handle.plants_loaded || !plant_cache) {
        if (db_flash_load_plants() < 0) {
            return NULL;
        }
    }
    
    if (index >= db_handle.plant_count) {
        return NULL;
    }
    
    return &plant_cache[index];
}

const db_soil_record_t *db_flash_get_soil(uint8_t soil_id)
{
    if (!db_handle.soils_loaded || !soil_cache) {
        if (db_flash_load_soils() < 0) {
            return NULL;
        }
    }
    
    for (uint16_t i = 0; i < db_handle.soil_count; i++) {
        if (soil_cache[i].soil_id == soil_id) {
            return &soil_cache[i];
        }
    }
    
    return NULL;
}

const db_irrigation_record_t *db_flash_get_irrigation(uint8_t method_id)
{
    if (!db_handle.irrigation_loaded || !irrigation_cache) {
        if (db_flash_load_irrigation() < 0) {
            return NULL;
        }
    }
    
    for (uint16_t i = 0; i < db_handle.irrigation_count; i++) {
        if (irrigation_cache[i].method_id == method_id) {
            return &irrigation_cache[i];
        }
    }
    
    return NULL;
}

uint16_t db_flash_get_plant_count(void)
{
    if (!db_handle.plants_loaded) {
        db_flash_load_plants();
    }
    return db_handle.plant_count;
}

uint16_t db_flash_get_soil_count(void)
{
    if (!db_handle.soils_loaded) {
        db_flash_load_soils();
    }
    return db_handle.soil_count;
}

uint16_t db_flash_get_irrigation_count(void)
{
    if (!db_handle.irrigation_loaded) {
        db_flash_load_irrigation();
    }
    return db_handle.irrigation_count;
}

bool db_flash_files_exist(void)
{
    if (!db_handle.mounted) {
        if (db_flash_mount() < 0) {
            return false;
        }
    }
    
    struct fs_dirent entry;
    
    if (fs_stat(DB_PATH_PLANTS, &entry) < 0) return false;
    if (fs_stat(DB_PATH_SOILS, &entry) < 0) return false;
    if (fs_stat(DB_PATH_IRRIGATION, &entry) < 0) return false;
    
    return true;
}

const db_flash_handle_t *db_flash_get_handle(void)
{
    return &db_handle;
}

void db_flash_unload_all(void)
{
    if (plant_cache) {
        k_free(plant_cache);
        plant_cache = NULL;
    }
    if (soil_cache) {
        k_free(soil_cache);
        soil_cache = NULL;
    }
    if (irrigation_cache) {
        k_free(irrigation_cache);
        irrigation_cache = NULL;
    }
    
    db_handle.plants = NULL;
    db_handle.soils = NULL;
    db_handle.irrigation = NULL;
    db_handle.plants_loaded = false;
    db_handle.soils_loaded = false;
    db_handle.irrigation_loaded = false;
    db_handle.plant_count = 0;
    db_handle.soil_count = 0;
    db_handle.irrigation_count = 0;
    
    LOG_INF("All database caches unloaded");
}
