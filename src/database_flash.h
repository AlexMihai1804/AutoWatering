/**
 * @file database_flash.h
 * @brief External flash database storage using LittleFS
 * 
 * Provides access to plant, soil, and irrigation method databases
 * stored on W25Q128 external flash in binary format.
 * 
 * File format:
 *   - Header: 16 bytes (magic + version + count + crc32 + record_size)
 *   - Records: packed binary structures with fixed sizes
 * 
 * Record sizes must match csv2binary_flash.py exactly:
 *   - Plant: 48 bytes
 *   - Soil: 24 bytes
 *   - Irrigation: 24 bytes
 */

#ifndef DATABASE_FLASH_H
#define DATABASE_FLASH_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*******************************************************************************
 * Database File Magic Numbers and Version
 ******************************************************************************/
#define DB_MAGIC_PLANT      0x504C4E54U  /* "PLNT" */
#define DB_MAGIC_SOIL       0x534F494CU  /* "SOIL" */
#define DB_MAGIC_IRRIGATION 0x49525247U  /* "IRRG" */
#define DB_VERSION_CURRENT  1

/*******************************************************************************
 * Binary Database Header (16 bytes, matches Python HEADER_FORMAT)
 ******************************************************************************/
typedef struct __attribute__((packed)) {
    uint32_t magic;           /* 4: File type magic number */
    uint16_t version;         /* 2: Format version */
    uint16_t record_count;    /* 2: Number of records */
    uint32_t crc32;           /* 4: CRC32 of all record data */
    uint16_t record_size;     /* 2: Size of each record in bytes */
    uint16_t reserved;        /* 2: Reserved/padding */
} db_file_header_t;

#define DB_HEADER_SIZE 16
_Static_assert(sizeof(db_file_header_t) == DB_HEADER_SIZE, 
               "db_file_header_t must be 16 bytes");

/*******************************************************************************
 * Binary Plant Record (48 bytes)
 * 
 * Fields are scaled to fit in bytes:
 *   - Kc values: stored as value * 100 (e.g., 1.15 -> 115)
 *   - Root depth: stored in decimeters (0.5m -> 5)
 *   - Depletion fraction: stored as value * 100
 ******************************************************************************/
typedef struct __attribute__((packed)) {
    uint16_t subtype_id;            /* 2: Plant type ID */
    uint8_t  category_id;           /* 1: Category enum (Agriculture=0, etc.) */
    uint8_t  _padding;              /* 1: Alignment padding */
    char     common_name[24];       /* 24: Plant name (null-terminated) */
    uint8_t  kc_ini;                /* 1: Kc initial * 100 */
    uint8_t  kc_mid;                /* 1: Kc mid * 100 */
    uint8_t  kc_end;                /* 1: Kc end * 100 */
    uint8_t  root_depth_max_dm;     /* 1: Max root depth in decimeters */
    uint8_t  depletion_fraction;    /* 1: Depletion fraction * 100 */
    uint8_t  stage_ini;             /* 1: Initial stage days */
    uint8_t  stage_dev;             /* 1: Development stage days */
    uint8_t  stage_mid;             /* 1: Mid-season stage days */
    uint8_t  stage_end;             /* 1: Late season stage days */
    uint8_t  flags;                 /* 1: Bit flags (indoor|toxic|edible) */
    uint8_t  drought_tolerance;     /* 1: 0=LOW, 1=MED, 2=HIGH, 3=VHIGH */
    uint8_t  default_irrigation;    /* 1: Default irrigation method ID */
    uint8_t  reserved[8];           /* 8: Reserved for future use */
} db_plant_record_t;

#define DB_PLANT_RECORD_SIZE 48
_Static_assert(sizeof(db_plant_record_t) == DB_PLANT_RECORD_SIZE, 
               "db_plant_record_t must be 48 bytes");

/* Plant flags */
#define DB_PLANT_FLAG_INDOOR  0x01
#define DB_PLANT_FLAG_TOXIC   0x02
#define DB_PLANT_FLAG_EDIBLE  0x04

/* Plant categories */
typedef enum {
    DB_CAT_AGRICULTURE = 0,
    DB_CAT_VEGETABLES  = 1,
    DB_CAT_FRUITS      = 2,
    DB_CAT_HERBS       = 3,
    DB_CAT_ORNAMENTAL  = 4,
    DB_CAT_TREES       = 5,
    DB_CAT_HOUSEPLANTS = 6,
    DB_CAT_LAWNS       = 7,
    DB_CAT_COUNT
} db_plant_category_t;

/* Drought tolerance levels */
typedef enum {
    DB_DROUGHT_LOW   = 0,
    DB_DROUGHT_MED   = 1,
    DB_DROUGHT_HIGH  = 2,
    DB_DROUGHT_VHIGH = 3,
} db_drought_tolerance_t;

/*******************************************************************************
 * Binary Soil Record (24 bytes)
 ******************************************************************************/
typedef struct __attribute__((packed)) {
    uint8_t  soil_id;               /* 1: Soil type ID */
    char     soil_type[15];         /* 15: Soil type name (null-terminated) */
    uint8_t  fc_pctvol;             /* 1: Field capacity % by volume */
    uint8_t  pwp_pctvol;            /* 1: Permanent wilting point % */
    uint16_t awc_mm_per_m;          /* 2: Available water capacity mm/m */
    uint8_t  infil_mm_h;            /* 1: Infiltration rate mm/hour */
    uint8_t  p_raw;                 /* 1: Raw depletion fraction * 100 */
    uint8_t  reserved[2];           /* 2: Reserved/padding */
} db_soil_record_t;

#define DB_SOIL_RECORD_SIZE 24
_Static_assert(sizeof(db_soil_record_t) == DB_SOIL_RECORD_SIZE, 
               "db_soil_record_t must be 24 bytes");

/*******************************************************************************
 * Binary Irrigation Method Record (24 bytes)
 ******************************************************************************/
typedef struct __attribute__((packed)) {
    uint8_t  method_id;             /* 1: Method ID */
    char     method_name[15];       /* 15: Method name (null-terminated) */
    uint8_t  efficiency_pct;        /* 1: Efficiency percentage */
    uint8_t  wetting_fraction;      /* 1: Wetting fraction * 100 */
    uint8_t  depth_typical_mm;      /* 1: Typical application depth mm */
    uint8_t  application_rate_mm_h; /* 1: Application rate mm/hour */
    uint8_t  distribution_uniformity_pct; /* 1: DU percentage */
    uint8_t  reserved[3];           /* 3: Reserved/padding */
} db_irrigation_record_t;

#define DB_IRRIGATION_RECORD_SIZE 24
_Static_assert(sizeof(db_irrigation_record_t) == DB_IRRIGATION_RECORD_SIZE, 
               "db_irrigation_record_t must be 24 bytes");

/*******************************************************************************
 * Database File Paths on LittleFS
 ******************************************************************************/
#define DB_MOUNT_POINT      "/lfs"
#define DB_PATH_PLANTS      "/lfs/db/plants.bin"
#define DB_PATH_SOILS       "/lfs/db/soils.bin"
#define DB_PATH_IRRIGATION  "/lfs/db/irrigation.bin"

/*******************************************************************************
 * Helper Macros for Value Conversion
 ******************************************************************************/
/** Convert stored Kc value to float (e.g., 115 -> 1.15) */
#define DB_KC_TO_FLOAT(kc)  ((float)(kc) / 100.0f)

/** Convert stored depletion fraction to float (e.g., 55 -> 0.55) */
#define DB_DEPLETION_TO_FLOAT(d)  ((float)(d) / 100.0f)

/** Convert stored root depth (dm) to meters (e.g., 5 -> 0.5) */
#define DB_ROOT_DEPTH_TO_M(d)  ((float)(d) / 10.0f)

/** Convert stored wetting fraction to float */
#define DB_WETTING_TO_FLOAT(w)  ((float)(w) / 100.0f)

/*******************************************************************************
 * Database Handle (runtime state)
 ******************************************************************************/
typedef struct {
    bool mounted;
    bool plants_loaded;
    bool soils_loaded;
    bool irrigation_loaded;
    
    uint16_t plant_count;
    uint16_t soil_count;
    uint16_t irrigation_count;
    
    /* Cached pointers to loaded data (NULL if not loaded) */
    const db_plant_record_t *plants;
    const db_soil_record_t *soils;
    const db_irrigation_record_t *irrigation;
} db_flash_handle_t;

/*******************************************************************************
 * API Functions
 ******************************************************************************/

/**
 * @brief Initialize LittleFS on database partition
 * @return 0 on success, negative errno on failure
 */
int db_flash_init(void);

/**
 * @brief Mount the database filesystem
 * @return 0 on success, negative errno on failure
 */
int db_flash_mount(void);

/**
 * @brief Unmount the database filesystem
 * @return 0 on success, negative errno on failure
 */
int db_flash_unmount(void);

/**
 * @brief Format the database partition (erases all data!)
 * @return 0 on success, negative errno on failure
 */
int db_flash_format(void);

/**
 * @brief Load plant database into RAM cache
 * @return Number of records loaded, or negative errno
 */
int db_flash_load_plants(void);

/**
 * @brief Load soil database into RAM cache  
 * @return Number of records loaded, or negative errno
 */
int db_flash_load_soils(void);

/**
 * @brief Load irrigation methods database into RAM cache
 * @return Number of records loaded, or negative errno
 */
int db_flash_load_irrigation(void);

/**
 * @brief Get plant record by index
 * @param index Plant index (0 to plant_count-1)
 * @return Pointer to record, or NULL if not found
 */
const db_plant_record_t *db_flash_get_plant(uint16_t index);

/**
 * @brief Get soil record by ID
 * @param soil_id Soil type ID
 * @return Pointer to record, or NULL if not found
 */
const db_soil_record_t *db_flash_get_soil(uint8_t soil_id);

/**
 * @brief Get irrigation method record by ID
 * @param method_id Method ID
 * @return Pointer to record, or NULL if not found
 */
const db_irrigation_record_t *db_flash_get_irrigation(uint8_t method_id);

/**
 * @brief Get total plant count
 */
uint16_t db_flash_get_plant_count(void);

/**
 * @brief Get total soil type count
 */
uint16_t db_flash_get_soil_count(void);

/**
 * @brief Get total irrigation method count
 */
uint16_t db_flash_get_irrigation_count(void);

/**
 * @brief Check if database files exist
 * @return true if all database files present
 */
bool db_flash_files_exist(void);

/**
 * @brief Get database handle for status inspection
 */
const db_flash_handle_t *db_flash_get_handle(void);

/**
 * @brief Free loaded database caches
 */
void db_flash_unload_all(void);

#ifdef __cplusplus
}
#endif

#endif /* DATABASE_FLASH_H */
