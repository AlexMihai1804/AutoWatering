# Pack Storage API Documentation

**Version**: 1.0.0  
**Files**: `src/pack_storage.h`, `src/pack_storage.c`

## Overview

The Pack Storage module provides a filesystem abstraction layer for managing custom plant and pack data on external SPI flash. It uses LittleFS mounted on the `ext_storage_partition` for wear-leveling and power-fail safety.

---

## Configuration

### Partition Configuration

Defined in `boards/arduino_nano_33_ble.overlay`:

```dts
&w25q128 {
    partitions {
        ext_storage_partition: partition@1E2000 {
            label = "ext_storage";
            reg = <0x001E2000 0x00E1E000>;  // ~14.4 MB
        };
    };
};
```

### Mount Point

```c
#define PACK_MOUNT_POINT    "/lfs_ext"
```

### Directory Structure

```
/lfs_ext/
├── plants/          Plant data files
│   ├── 0001.bin
│   ├── 0002.bin
│   └── ...
└── packs/           Pack metadata files
    ├── 0001.bin
    └── ...
```

---

## Initialization

### pack_storage_init

```c
pack_result_t pack_storage_init(void);
```

Mount LittleFS on the external flash partition and create required directories.

**Returns:**
- `PACK_RESULT_SUCCESS` - Mounted successfully
- `PACK_RESULT_IO_ERROR` - Mount failed

**Example:**
```c
#include "pack_storage.h"

int main(void) {
    pack_result_t result = pack_storage_init();
    if (result != PACK_RESULT_SUCCESS) {
        LOG_ERR("Pack storage init failed: %d", result);
        // Continue without custom plants
    }
    
    // Rest of initialization...
}
```

**Notes:**
- Safe to call multiple times (idempotent)
- Creates `/lfs_ext/plants/` and `/lfs_ext/packs/` directories
- Called from `main.c` during boot sequence

---

### pack_storage_is_ready

```c
bool pack_storage_is_ready(void);
```

Check if pack storage is mounted and ready.

**Returns:**
- `true` - Storage is ready
- `false` - Not initialized or mount failed

---

## Plant Operations

### pack_storage_get_plant

```c
pack_result_t pack_storage_get_plant(uint16_t plant_id, pack_plant_v1_t *plant);
```

Read a plant from flash storage.

**Parameters:**
| Param | Type | Description |
|-------|------|-------------|
| plant_id | uint16_t | Plant ID to retrieve |
| plant | pack_plant_v1_t* | Output buffer for plant data |

**Returns:**
| Result | Description |
|--------|-------------|
| `PACK_RESULT_SUCCESS` | Plant loaded successfully |
| `PACK_RESULT_NOT_FOUND` | Plant file doesn't exist |
| `PACK_RESULT_CRC_MISMATCH` | File corrupted |
| `PACK_RESULT_IO_ERROR` | Read error |

**Example:**
```c
pack_plant_v1_t plant;
pack_result_t result = pack_storage_get_plant(1001, &plant);

if (result == PACK_RESULT_SUCCESS) {
    LOG_INF("Loaded: %s (Kc_mid=%u)", plant.common_name, plant.kc_mid);
}
```

---

### pack_storage_install_plant

```c
pack_result_t pack_storage_install_plant(const pack_plant_v1_t *plant);
```

Install or update a plant in storage.

**Parameters:**
| Param | Type | Description |
|-------|------|-------------|
| plant | const pack_plant_v1_t* | Plant data to install |

**Returns:**
| Result | Description |
|--------|-------------|
| `PACK_RESULT_SUCCESS` | New plant installed |
| `PACK_RESULT_UPDATED` | Existing plant updated to newer version |
| `PACK_RESULT_ALREADY_CURRENT` | Same or newer version exists |
| `PACK_RESULT_INVALID_DATA` | Validation failed |
| `PACK_RESULT_STORAGE_FULL` | Not enough space |
| `PACK_RESULT_IO_ERROR` | Write error |

**Example:**
```c
pack_plant_v1_t tomato = {
    .plant_id = 1001,
    .pack_id = 1,
    .version = 1,
    .source = PLANT_SOURCE_PACK,
    .common_name = "Tomato",
    .scientific_name = "Solanum lycopersicum",
    .kc_ini = 60,
    .kc_mid = 115,
    .kc_end = 80,
    // ... other fields
};

pack_result_t result = pack_storage_install_plant(&tomato);
if (result == PACK_RESULT_SUCCESS) {
    LOG_INF("Tomato installed");
} else if (result == PACK_RESULT_ALREADY_CURRENT) {
    LOG_INF("Tomato already up to date");
}
```

**Notes:**
- Uses atomic write (temp file + rename)
- Validates plant data before writing
- CRC32 computed and stored in header

---

### pack_storage_delete_plant

```c
pack_result_t pack_storage_delete_plant(uint16_t plant_id);
```

Delete a plant from storage.

**Parameters:**
| Param | Type | Description |
|-------|------|-------------|
| plant_id | uint16_t | Plant ID to delete |

**Returns:**
| Result | Description |
|--------|-------------|
| `PACK_RESULT_SUCCESS` | Plant deleted |
| `PACK_RESULT_NOT_FOUND` | Plant doesn't exist |
| `PACK_RESULT_IO_ERROR` | Delete error |

**Example:**
```c
pack_result_t result = pack_storage_delete_plant(1001);
if (result == PACK_RESULT_SUCCESS) {
    LOG_INF("Plant 1001 deleted");
}
```

---

### pack_storage_list_plants

```c
pack_result_t pack_storage_list_plants(pack_plant_list_entry_t *entries,
                                       uint16_t max_entries,
                                       uint16_t *count,
                                       uint16_t offset);
```

List installed plants with pagination.

**Parameters:**
| Param | Type | Description |
|-------|------|-------------|
| entries | pack_plant_list_entry_t* | Output array |
| max_entries | uint16_t | Array size |
| count | uint16_t* | Output: entries returned |
| offset | uint16_t | Skip first N plants |

**Returns:**
- `PACK_RESULT_SUCCESS` always (empty list is valid)

**Entry Structure:**
```c
typedef struct {
    uint16_t plant_id;
    uint16_t pack_id;
    uint16_t version;
    plant_source_t source;
    char name[32];
} pack_plant_list_entry_t;
```

**Example:**
```c
pack_plant_list_entry_t plants[10];
uint16_t count;

pack_storage_list_plants(plants, 10, &count, 0);

for (int i = 0; i < count; i++) {
    LOG_INF("Plant %u: %s (pack %u)", 
            plants[i].plant_id, plants[i].name, plants[i].pack_id);
}
```

---

### pack_storage_get_plant_count

```c
uint16_t pack_storage_get_plant_count(void);
```

Get total number of installed custom plants.

**Returns:** Number of plant files in `/lfs_ext/plants/`

---

## Pack Operations

### pack_storage_get_pack

```c
pack_result_t pack_storage_get_pack(uint16_t pack_id, 
                                    pack_pack_v1_t *pack,
                                    uint16_t *installed_plants);
```

Get pack metadata.

**Parameters:**
| Param | Type | Description |
|-------|------|-------------|
| pack_id | uint16_t | Pack ID to retrieve |
| pack | pack_pack_v1_t* | Output buffer |
| installed_plants | uint16_t* | Output: plants currently installed |

---

### pack_storage_install_pack

```c
pack_result_t pack_storage_install_pack(const pack_pack_v1_t *pack,
                                        const pack_plant_v1_t *plants,
                                        uint16_t plant_count);
```

Install a pack with all its plants.

**Parameters:**
| Param | Type | Description |
|-------|------|-------------|
| pack | const pack_pack_v1_t* | Pack metadata |
| plants | const pack_plant_v1_t* | Array of plants |
| plant_count | uint16_t | Number of plants |

**Notes:**
- Installs pack metadata first
- Installs each plant, skipping already-current versions
- Pack is marked complete after all plants installed

---

### pack_storage_delete_pack

```c
pack_result_t pack_storage_delete_pack(uint16_t pack_id, bool delete_plants);
```

Delete a pack and optionally its plants.

**Parameters:**
| Param | Type | Description |
|-------|------|-------------|
| pack_id | uint16_t | Pack to delete |
| delete_plants | bool | Also delete plants with this pack_id |

---

### pack_storage_list_packs

```c
pack_result_t pack_storage_list_packs(pack_pack_list_entry_t *entries,
                                      uint16_t max_entries,
                                      uint16_t *count);
```

List installed packs.

---

## FAO-56 Integration Helpers

These functions provide unified access to plant data from both ROM and custom sources.

### pack_storage_get_kc

```c
uint16_t pack_storage_get_kc(uint16_t plant_id, growth_stage_t stage);
```

Get crop coefficient (Kc) for a plant and growth stage.

**Parameters:**
| Param | Type | Description |
|-------|------|-------------|
| plant_id | uint16_t | Plant ID (ROM or custom) |
| stage | growth_stage_t | Current growth stage |

**Returns:** Kc value scaled ×100 (e.g., 115 = 1.15)

**Growth Stages:**
```c
typedef enum {
    GROWTH_STAGE_INITIAL = 0,
    GROWTH_STAGE_DEVELOPMENT = 1,
    GROWTH_STAGE_MID = 2,
    GROWTH_STAGE_LATE = 3,
} growth_stage_t;
```

**Example:**
```c
uint16_t kc = pack_storage_get_kc(1001, GROWTH_STAGE_MID);
float kc_float = (float)kc / 100.0f;  // 1.15

// Use in ET calculation
float etc = eto * kc_float;
```

**Notes:**
- For ROM plants (ID < 1000): reads from plant_full_db
- For custom plants (ID ≥ 1000): reads from flash
- Returns 100 (1.00) on error

---

### pack_storage_get_root_depth

```c
uint16_t pack_storage_get_root_depth(uint16_t plant_id, uint16_t days_since_planting);
```

Get effective root depth based on plant age.

**Parameters:**
| Param | Type | Description |
|-------|------|-------------|
| plant_id | uint16_t | Plant ID |
| days_since_planting | uint16_t | Days since planting/transplant |

**Returns:** Root depth in mm

**Notes:**
- Interpolates between `root_depth_min` and `root_depth_max`
- Uses growth rate for development calculation
- Caps at `root_depth_max`

---

## Statistics

### pack_storage_get_stats

```c
pack_result_t pack_storage_get_stats(pack_storage_stats_t *stats);
```

Get storage usage statistics.

**Output Structure:**
```c
typedef struct {
    uint32_t total_bytes;       // Total partition size
    uint32_t used_bytes;        // Currently used
    uint32_t free_bytes;        // Available
    uint16_t plant_count;       // Installed plants
    uint16_t pack_count;        // Installed packs
} pack_storage_stats_t;
```

**Example:**
```c
pack_storage_stats_t stats;
pack_storage_get_stats(&stats);

LOG_INF("Storage: %u/%u bytes used (%u plants, %u packs)",
        stats.used_bytes, stats.total_bytes,
        stats.plant_count, stats.pack_count);
```

---

## Built-in Database Access

### pack_storage_get_builtin_pack

```c
pack_result_t pack_storage_get_builtin_pack(pack_pack_list_entry_t *pack);
```

Get virtual pack info for ROM database.

**Returns:**
```c
pack_pack_list_entry_t builtin = {
    .pack_id = 0,
    .version = 1,
    .plant_count = 223,  // PLANT_FULL_SPECIES_COUNT
    .name = "Built-in Plants"
};
```

---

## CRC32 Utility

### pack_storage_crc32

```c
uint32_t pack_storage_crc32(const void *data, size_t len);
```

Calculate CRC32 (IEEE polynomial) for data integrity.

**Algorithm:** CRC-32/ISO-HDLC (polynomial 0xEDB88320)

---

## Thread Safety

The storage module uses a mutex for all operations:

```c
static struct k_mutex pack_storage_mutex;
```

All public functions acquire the mutex before accessing the filesystem.

---

## Error Handling

### Common Error Scenarios

| Scenario | Result Code | Recovery |
|----------|-------------|----------|
| Flash not mounted | `IO_ERROR` | Re-init |
| File corrupted | `CRC_MISMATCH` | Delete and reinstall |
| Partition full | `STORAGE_FULL` | Delete unused plants |
| Plant not found | `NOT_FOUND` | Check ID or install |

### Logging

All operations log via Zephyr logging:
```c
LOG_MODULE_REGISTER(pack_storage, CONFIG_LOG_DEFAULT_LEVEL);
```

---

## Example: Complete Workflow

```c
#include "pack_storage.h"
#include "pack_schema.h"

void example_pack_workflow(void) {
    // 1. Initialize storage
    pack_result_t res = pack_storage_init();
    if (res != PACK_RESULT_SUCCESS) {
        LOG_ERR("Storage init failed");
        return;
    }
    
    // 2. Create a custom plant
    pack_plant_v1_t pepper = {
        .plant_id = 1002,
        .pack_id = 1,
        .version = 1,
        .source = PLANT_SOURCE_CUSTOM,
        .common_name = "Bell Pepper",
        .scientific_name = "Capsicum annuum",
        .kc_ini = 60,
        .kc_mid = 105,
        .kc_end = 90,
        .l_ini_days = 30,
        .l_dev_days = 35,
        .l_mid_days = 40,
        .l_end_days = 20,
        .root_depth_min = 300,
        .root_depth_max = 1000,
        .depletion_fraction = 30,
        .temp_min = 15,
        .temp_max = 35,
    };
    
    // 3. Install plant
    res = pack_storage_install_plant(&pepper);
    LOG_INF("Install result: %d", res);
    
    // 4. Get Kc for mid-season
    uint16_t kc = pack_storage_get_kc(1002, GROWTH_STAGE_MID);
    LOG_INF("Kc mid = %u (%.2f)", kc, kc / 100.0f);
    
    // 5. Get storage stats
    pack_storage_stats_t stats;
    pack_storage_get_stats(&stats);
    LOG_INF("Using %u of %u bytes", stats.used_bytes, stats.total_bytes);
    
    // 6. List all plants
    pack_plant_list_entry_t list[10];
    uint16_t count;
    pack_storage_list_plants(list, 10, &count, 0);
    
    for (int i = 0; i < count; i++) {
        LOG_INF("  [%u] %s", list[i].plant_id, list[i].name);
    }
}
```

---

## Performance Characteristics

| Operation | Typical Time | Notes |
|-----------|--------------|-------|
| Init (mount) | 50-200ms | First boot may format |
| Read plant | 5-15ms | Includes CRC verify |
| Write plant | 20-50ms | Atomic with temp file |
| Delete plant | 5-10ms | Single unlink |
| List plants | 10-100ms | Depends on count |
| Get stats | 50-200ms | FS stat traversal |

---

## Limitations

1. **Maximum plants**: Limited by flash size (~14MB / 172B ≈ 81,000)
2. **Plant ID range**: Custom plants must use ID ≥ 1000
3. **Pack ID 0**: Reserved for virtual built-in pack
4. **Name length**: Common name 48, scientific name 64 characters max (including null)
5. **Concurrent access**: Single-threaded via mutex
