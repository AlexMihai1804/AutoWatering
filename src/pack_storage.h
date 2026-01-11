/**
 * @file pack_storage.h
 * @brief External flash storage API for plant packs
 * 
 * Provides filesystem operations for plant/pack files stored on external
 * flash using LittleFS. Mounts /lfs_ext on ext_storage_partition.
 */

#ifndef PACK_STORAGE_H
#define PACK_STORAGE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "pack_schema.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Configuration
 * ============================================================================ */

/** Mount point for pack storage */
#define PACK_MOUNT_POINT    "/lfs_ext"

/** Maximum number of items to list in a single call */
#define PACK_LIST_MAX_ITEMS 32

/* ============================================================================
 * List Entry Structures (for enumeration APIs)
 * ============================================================================ */

/**
 * @brief Summary info for an installed plant
 */
typedef struct {
    uint16_t plant_id;          /**< Plant ID */
    uint16_t pack_id;           /**< Owning pack ID (0 = standalone) */
    uint16_t version;           /**< Installed version */
    plant_source_t source;      /**< Source type */
    char name[PACK_COMMON_NAME_MAX_LEN]; /**< Common name */
} pack_plant_list_entry_t;

/**
 * @brief Summary info for an installed pack
 */
typedef struct {
    uint16_t pack_id;           /**< Pack ID */
    uint16_t version;           /**< Installed version */
    uint16_t plant_count;       /**< Number of plants in pack */
    char name[PACK_NAME_MAX_LEN]; /**< Pack name */
} pack_pack_list_entry_t;

/**
 * @brief Storage statistics
 */
typedef struct {
    uint32_t total_bytes;       /**< Total partition size */
    uint32_t used_bytes;        /**< Used bytes */
    uint32_t free_bytes;        /**< Free bytes */
    uint16_t plant_count;       /**< Installed custom plants */
    uint16_t pack_count;        /**< Installed packs */
} pack_storage_stats_t;

/* ============================================================================
 * Initialization
 * ============================================================================ */

/**
 * @brief Initialize pack storage (mount LittleFS on ext_storage_partition)
 * 
 * Creates required directories if missing. Safe to call multiple times.
 * 
 * @return pack_result_t PACK_RESULT_SUCCESS on success
 */
pack_result_t pack_storage_init(void);

/**
 * @brief Check if pack storage is initialized and available
 * 
 * @return true if storage is mounted and ready
 */
bool pack_storage_is_ready(void);

/**
 * @brief Deinitialize pack storage (unmount filesystem)
 */
void pack_storage_deinit(void);

/* ============================================================================
 * Plant Operations
 * ============================================================================ */

/**
 * @brief Get a custom plant by ID
 * 
 * @param plant_id Plant ID to retrieve
 * @param plant Output buffer for plant data
 * @return pack_result_t PACK_RESULT_SUCCESS or error
 */
pack_result_t pack_storage_get_plant(uint16_t plant_id, pack_plant_v1_t *plant);

/**
 * @brief Install or update a custom plant
 * 
 * Performs atomic update: writes to temp file, validates, then renames.
 * If plant exists with lower version, updates it.
 * If plant exists with same or higher version, returns PACK_RESULT_ALREADY_CURRENT.
 * 
 * @param plant Plant data to install
 * @return pack_result_t Result of operation
 */
pack_result_t pack_storage_install_plant(const pack_plant_v1_t *plant);

/**
 * @brief Delete a custom plant
 * 
 * @param plant_id Plant ID to delete
 * @return pack_result_t PACK_RESULT_SUCCESS or error
 */
pack_result_t pack_storage_delete_plant(uint16_t plant_id);

/**
 * @brief List installed custom plants
 * 
 * @param entries Output array for plant entries
 * @param max_entries Maximum entries to return
 * @param out_count Actual number of entries returned
 * @param offset Starting offset for pagination
 * @return pack_result_t PACK_RESULT_SUCCESS or error
 */
pack_result_t pack_storage_list_plants(pack_plant_list_entry_t *entries,
                                       uint16_t max_entries,
                                       uint16_t *out_count,
                                       uint16_t offset);

/**
 * @brief Get count of installed custom plants
 * 
 * @return Number of custom plants (excludes built-in)
 */
uint16_t pack_storage_get_plant_count(void);

/* ============================================================================
 * Pack Operations
 * ============================================================================ */

/**
 * @brief Get a pack by ID
 * 
 * @param pack_id Pack ID to retrieve
 * @param pack Output buffer for pack header
 * @param plant_ids Optional output buffer for plant ID array (can be NULL)
 * @param max_plant_ids Maximum plant IDs to return
 * @return pack_result_t PACK_RESULT_SUCCESS or error
 */
pack_result_t pack_storage_get_pack(uint16_t pack_id, 
                                    pack_pack_v1_t *pack,
                                    uint16_t *plant_ids,
                                    uint16_t max_plant_ids);

/**
 * @brief Install or update a pack (header + plants)
 * 
 * @param pack Pack header data
 * @param plants Array of plants to install with the pack
 * @param plant_count Number of plants in array
 * @return pack_result_t Result of operation
 */
pack_result_t pack_storage_install_pack(const pack_pack_v1_t *pack,
                                        const pack_plant_v1_t *plants,
                                        uint16_t plant_count);

/**
 * @brief Delete a pack and optionally its plants
 * 
 * @param pack_id Pack ID to delete
 * @param delete_plants If true, also delete plants owned by this pack
 * @return pack_result_t PACK_RESULT_SUCCESS or error
 */
pack_result_t pack_storage_delete_pack(uint16_t pack_id, bool delete_plants);

/**
 * @brief List installed packs
 * 
 * @param entries Output array for pack entries
 * @param max_entries Maximum entries to return
 * @param out_count Actual number of entries returned
 * @param offset Starting offset for pagination
 * @return pack_result_t PACK_RESULT_SUCCESS or error
 */
pack_result_t pack_storage_list_packs(pack_pack_list_entry_t *entries,
                                      uint16_t max_entries,
                                      uint16_t *out_count,
                                      uint16_t offset);

/**
 * @brief Get count of installed packs
 * 
 * @return Number of installed packs (excludes built-in pack 0)
 */
uint16_t pack_storage_get_pack_count(void);

/* ============================================================================
 * Built-in Database Integration
 * ============================================================================ */

/**
 * @brief Check if a plant ID is from built-in database
 * 
 * Built-in plants have pack_id=0 and plant_id in range [0..PLANT_FULL_SPECIES_COUNT-1]
 * 
 * @param plant_id Plant ID to check
 * @param pack_id Pack ID to check
 * @return true if this is a built-in plant
 */
bool pack_storage_is_builtin_plant(uint16_t plant_id, uint16_t pack_id);

/**
 * @brief Get built-in pack info (virtual pack 0)
 * 
 * @param pack Output buffer for pack info
 * @return pack_result_t PACK_RESULT_SUCCESS
 */
pack_result_t pack_storage_get_builtin_pack(pack_pack_list_entry_t *pack);

/* ============================================================================
 * Utility Functions
 * ============================================================================ */

/**
 * @brief Get storage statistics
 * 
 * @param stats Output buffer for statistics
 * @return pack_result_t PACK_RESULT_SUCCESS or error
 */
pack_result_t pack_storage_get_stats(pack_storage_stats_t *stats);

/**
 * @brief Validate a plant structure
 * 
 * @param plant Plant data to validate
 * @return pack_result_t PACK_RESULT_SUCCESS if valid
 */
pack_result_t pack_storage_validate_plant(const pack_plant_v1_t *plant);

/**
 * @brief Calculate CRC32 of data
 * 
 * @param data Data buffer
 * @param len Data length
 * @return CRC32 value
 */
uint32_t pack_storage_crc32(const void *data, size_t len);

/* ============================================================================
 * FAO-56 Integration Helpers
 * ============================================================================ */

/**
 * @brief Get Kc (crop coefficient) for a custom or built-in plant
 * 
 * This helper looks up Kc from either a custom plant (if custom_plant_id > 0)
 * or falls back to the built-in ROM database (via plant_db_index).
 * 
 * @param custom_plant_id Custom plant ID from pack storage (0 = use built-in)
 * @param plant_db_index Built-in plant index (used when custom_plant_id == 0)
 * @param days_after_planting Days since planting for Kc interpolation
 * @param out_kc Output: interpolated Kc value
 * @return pack_result_t PACK_RESULT_SUCCESS or error
 */
pack_result_t pack_storage_get_kc(uint16_t custom_plant_id,
                                   uint16_t plant_db_index,
                                   uint16_t days_after_planting,
                                   float *out_kc);

/**
 * @brief Get root depth for a custom or built-in plant
 * 
 * @param custom_plant_id Custom plant ID from pack storage (0 = use built-in)
 * @param plant_db_index Built-in plant index (used when custom_plant_id == 0)
 * @param days_after_planting Days since planting
 * @param out_root_depth_mm Output: root depth in mm
 * @return pack_result_t PACK_RESULT_SUCCESS or error
 */
pack_result_t pack_storage_get_root_depth(uint16_t custom_plant_id,
                                           uint16_t plant_db_index,
                                           uint16_t days_after_planting,
                                           float *out_root_depth_mm);

/**
 * @brief Get all FAO-56 parameters for a custom plant
 * 
 * @param custom_plant_id Custom plant ID (must be > 0)
 * @param plant Output buffer for plant data
 * @return pack_result_t PACK_RESULT_SUCCESS or error
 */
pack_result_t pack_storage_get_fao56_plant(uint16_t custom_plant_id,
                                            pack_plant_v1_t *plant);

#ifdef __cplusplus
}
#endif

#endif /* PACK_STORAGE_H */
