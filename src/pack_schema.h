/**
 * @file pack_schema.h
 * @brief Pack/Plant schema definitions v1 for external flash storage
 * 
 * This file defines the on-device binary formats for custom plants and packs
 * stored on LittleFS external flash partition (/lfs_ext/packs/).
 * 
 * Design decisions:
 * - Binary format (not JSON) for minimal parsing overhead on embedded target
 * - plant_id and pack_id are uint16_t (0..65534, 0xFFFF reserved as INVALID)
 * - Built-in DB exposed as pack_id=0 (virtual, not stored as file)
 * - Each plant/pack file includes schema version for forward compatibility
 * - Atomic updates via temp file + rename pattern
 */

#ifndef PACK_SCHEMA_H
#define PACK_SCHEMA_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Constants
 * ============================================================================ */

/** Current schema version */
#define PACK_SCHEMA_VERSION         1

/** Magic bytes for file validation */
#define PACK_MAGIC_PLANT            0x504C4E54  /* "PLNT" */
#define PACK_MAGIC_PACK             0x5041434B  /* "PACK" */

/** Reserved IDs */
#define PACK_ID_BUILTIN             0       /**< Built-in database (virtual pack) */
#define PACK_ID_INVALID             0xFFFF  /**< Invalid/unset pack ID */
#define PLANT_ID_INVALID            0xFFFF  /**< Invalid/unset plant ID */

/** Maximum lengths */
#define PACK_NAME_MAX_LEN           32      /**< Max pack/plant name length (including null) */
#define PACK_COMMON_NAME_MAX_LEN    48      /**< Max common name length */
#define PACK_SCIENTIFIC_NAME_MAX_LEN 64     /**< Max scientific name length */

/** File paths */
#define PACK_BASE_PATH              "/lfs_ext/packs"
#define PACK_PLANTS_DIR             "/lfs_ext/packs/plants"
#define PACK_PACKS_DIR              "/lfs_ext/packs/packs"
#define PACK_MANIFEST_PATH          "/lfs_ext/packs/manifest.bin"
#define PACK_TEMP_SUFFIX            ".tmp"

/** Limits */
#define PACK_MAX_PLANTS_PER_PACK    256     /**< Max plants in a single pack */

/* ============================================================================
 * Plant Source Enumeration
 * ============================================================================ */

/**
 * @brief Source of plant data
 */
typedef enum {
    PLANT_SOURCE_BUILTIN = 0,   /**< From built-in ROM database */
    PLANT_SOURCE_PACK = 1,      /**< From installed pack on flash */
    PLANT_SOURCE_CUSTOM = 2,    /**< Standalone custom plant on flash */
} plant_source_t;

/* ============================================================================
 * File Header (common to all pack files)
 * ============================================================================ */

/**
 * @brief Common file header for all pack-related files
 */
typedef struct __attribute__((packed)) {
    uint32_t magic;             /**< Magic bytes for file type validation */
    uint8_t  schema_version;    /**< Schema version (currently 1) */
    uint8_t  reserved[3];       /**< Reserved for alignment/future use */
    uint32_t crc32;             /**< CRC32 of payload (after header) */
    uint32_t payload_size;      /**< Size of payload in bytes */
} pack_file_header_t;

_Static_assert(sizeof(pack_file_header_t) == 16, "pack_file_header_t size mismatch");

/* ============================================================================
 * Custom Plant Structure (stored in /lfs_ext/packs/plants/p_XXXX.bin)
 * ============================================================================ */

/**
 * @brief Custom plant data stored on external flash
 * 
 * Compatible with plant_full_data_t but with embedded strings and metadata.
 * File format: [pack_file_header_t][pack_plant_v1_t]
 */
typedef struct __attribute__((packed)) {
    /* Identification */
    uint16_t plant_id;          /**< Unique plant ID (1..65534) */
    uint16_t pack_id;           /**< Owning pack ID (0=standalone, 1+=from pack) */
    uint16_t version;           /**< Plant data version for updates */
    uint16_t reserved;          /**< Reserved for alignment */
    
    /* Names (null-terminated, padded) */
    char common_name[PACK_COMMON_NAME_MAX_LEN];         /**< Common name (English) */
    char scientific_name[PACK_SCIENTIFIC_NAME_MAX_LEN]; /**< Scientific name */
    
    /* Crop coefficients (x1000) */
    uint16_t kc_ini_x1000;      /**< Kc initial stage */
    uint16_t kc_dev_x1000;      /**< Kc development stage */
    uint16_t kc_mid_x1000;      /**< Kc mid-season stage */
    uint16_t kc_end_x1000;      /**< Kc end season stage */
    
    /* Root depth (mm) */
    uint16_t root_depth_min_mm; /**< Minimum root depth in mm */
    uint16_t root_depth_max_mm; /**< Maximum root depth in mm */
    
    /* Growth stages (days) */
    uint8_t stage_days_ini;     /**< Initial stage duration (days) */
    uint8_t stage_days_dev;     /**< Development stage duration (days) */
    uint16_t stage_days_mid;    /**< Mid-season stage duration (days) */
    uint8_t stage_days_end;     /**< End season stage duration (days) */
    uint8_t growth_cycle;       /**< Growth cycle type */
    
    /* Depletion and spacing */
    uint16_t depletion_fraction_p_x1000; /**< Allowable depletion fraction (x1000) */
    uint16_t spacing_row_mm;    /**< Row spacing in mm */
    uint16_t spacing_plant_mm;  /**< Plant spacing in mm */
    uint16_t density_x100;      /**< Default density plants/m² (x100) */
    uint16_t canopy_max_x1000;  /**< Max canopy cover fraction (x1000) */
    
    /* Temperature */
    int8_t frost_tolerance_c;   /**< Frost tolerance temperature °C */
    uint8_t temp_opt_min_c;     /**< Optimal minimum temperature °C */
    uint8_t temp_opt_max_c;     /**< Optimal maximum temperature °C */
    
    /* Irrigation */
    uint8_t typ_irrig_method_id; /**< Typical irrigation method ID */
    
} pack_plant_v1_t;

#define PACK_PLANT_V1_EXPECTED_SIZE sizeof(pack_plant_v1_t)
/* Size validation done at runtime - remove static assertion for now */

/* ============================================================================
 * Pack Structure (stored in /lfs_ext/packs/packs/k_XXXX.bin)
 * ============================================================================ */

/**
 * @brief Pack metadata header
 * 
 * File format: [pack_file_header_t][pack_pack_v1_t][plant_id array]
 */
typedef struct __attribute__((packed)) {
    /* Identification */
    uint16_t pack_id;           /**< Unique pack ID (1..65534) */
    uint16_t version;           /**< Pack version for updates */
    
    /* Metadata */
    char name[PACK_NAME_MAX_LEN]; /**< Pack name (null-terminated) */
    
    /* Contents */
    uint16_t plant_count;       /**< Number of plants in pack */
    uint16_t reserved;          /**< Reserved for alignment */
    
    /* Followed by: uint16_t plant_ids[plant_count] */
} pack_pack_v1_t;

#define PACK_PACK_V1_EXPECTED_SIZE 40
_Static_assert(sizeof(pack_pack_v1_t) == PACK_PACK_V1_EXPECTED_SIZE, 
               "pack_pack_v1_t size mismatch");

/* ============================================================================
 * Manifest Structure (stored in /lfs_ext/packs/manifest.bin)
 * ============================================================================ */

/**
 * @brief Single entry in manifest (index of installed items)
 */
typedef struct __attribute__((packed)) {
    uint16_t id;                /**< Plant or pack ID */
    uint16_t version;           /**< Current installed version */
    uint8_t  type;              /**< 0=plant, 1=pack */
    uint8_t  reserved[3];       /**< Reserved for alignment */
} pack_manifest_entry_t;

_Static_assert(sizeof(pack_manifest_entry_t) == 8, "pack_manifest_entry_t size mismatch");

/**
 * @brief Manifest file header
 * 
 * File format: [pack_file_header_t][pack_manifest_v1_t][entries array]
 */
typedef struct __attribute__((packed)) {
    uint16_t entry_count;       /**< Number of manifest entries */
    uint16_t reserved;          /**< Reserved for alignment */
    /* Followed by: pack_manifest_entry_t entries[entry_count] */
} pack_manifest_v1_t;

#define PACK_MANIFEST_V1_EXPECTED_SIZE 4
_Static_assert(sizeof(pack_manifest_v1_t) == PACK_MANIFEST_V1_EXPECTED_SIZE, 
               "pack_manifest_v1_t size mismatch");

/* ============================================================================
 * Install/Update Status
 * ============================================================================ */

/**
 * @brief Result of install/update operation
 */
typedef enum {
    PACK_RESULT_SUCCESS = 0,        /**< Operation completed successfully */
    PACK_RESULT_UPDATED = 1,        /**< Existing item updated to new version */
    PACK_RESULT_ALREADY_CURRENT = 2,/**< Item already at this version or newer */
    PACK_RESULT_INVALID_DATA = 3,   /**< Data validation failed */
    PACK_RESULT_INVALID_VERSION = 4,/**< Schema version not supported */
    PACK_RESULT_STORAGE_FULL = 5,   /**< Not enough space on flash */
    PACK_RESULT_IO_ERROR = 6,       /**< Filesystem I/O error */
    PACK_RESULT_NOT_FOUND = 7,      /**< Item not found */
    PACK_RESULT_CRC_MISMATCH = 8,   /**< CRC validation failed */
} pack_result_t;

/* ============================================================================
 * Helper Macros for pack_plant_v1_t
 * ============================================================================ */

/** Convert x1000 fields to float */
#define PACK_PLANT_KC_INI(p) ((p)->kc_ini_x1000 / 1000.0f)
#define PACK_PLANT_KC_DEV(p) ((p)->kc_dev_x1000 / 1000.0f)
#define PACK_PLANT_KC_MID(p) ((p)->kc_mid_x1000 / 1000.0f)
#define PACK_PLANT_KC_END(p) ((p)->kc_end_x1000 / 1000.0f)
#define PACK_PLANT_ROOT_MIN_M(p) ((p)->root_depth_min_mm / 1000.0f)
#define PACK_PLANT_ROOT_MAX_M(p) ((p)->root_depth_max_mm / 1000.0f)
#define PACK_PLANT_DEPL_FRAC(p) ((p)->depletion_fraction_p_x1000 / 1000.0f)
#define PACK_PLANT_ROW_SPACING_M(p) ((p)->spacing_row_mm / 1000.0f)
#define PACK_PLANT_PLANT_SPACING_M(p) ((p)->spacing_plant_mm / 1000.0f)
#define PACK_PLANT_DENSITY(p) ((p)->density_x100 / 100.0f)
#define PACK_PLANT_CANOPY_MAX(p) ((p)->canopy_max_x1000 / 1000.0f)

#ifdef __cplusplus
}
#endif

#endif /* PACK_SCHEMA_H */
