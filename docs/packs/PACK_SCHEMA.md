# Pack Schema Documentation

**Version**: 1.0.0 (Schema Version 1)  
**File**: `src/pack_schema.h`

## Overview

The pack schema defines the binary structures used for storing custom plant and pack data on external flash. All structures are packed and little-endian for direct serialization.

## Design Principles

1. **Fixed-size structures** - No variable-length fields for predictable storage
2. **Scaled integers** - Floating-point values stored as scaled uint16_t (×100)
3. **CRC32 validation** - Every file includes header with CRC for integrity
4. **Version field** - Enables future schema evolution

---

## File Header Structure

Every file on flash starts with a 16-byte header:

```c
typedef struct __attribute__((packed)) {
    uint32_t magic;         // Magic number for file type validation
    uint16_t version;       // Schema version (currently 1)
    uint16_t data_type;     // Content type (1=plant, 2=pack)
    uint32_t crc32;         // CRC32 of the data following header
    uint32_t reserved;      // Reserved for future use
} pack_file_header_t;
```

### Magic Numbers

| Constant | Value | Description |
|----------|-------|-------------|
| `PACK_FILE_MAGIC` | `0x504C4E54` | "PLNT" in ASCII |

### Data Types

| Value | Constant | Description |
|-------|----------|-------------|
| 1 | `PACK_DATA_TYPE_PLANT` | Plant species data |
| 2 | `PACK_DATA_TYPE_PACK` | Pack metadata |

---

## Plant Structure (156 bytes)

The `pack_plant_v1_t` structure contains all FAO-56 parameters for a custom plant species.

> **Source:** `src/pack_schema.h`

```c
typedef struct __attribute__((packed)) {
    /* === Identification (8 bytes) === */
    uint16_t plant_id;              // Unique plant ID (1..65534)
    uint16_t pack_id;               // Owning pack ID (0=standalone, 1+=from pack)
    uint16_t version;               // Plant data version for updates
    uint16_t reserved;              // Reserved for alignment
    
    /* === Names (112 bytes) === */
    char common_name[48];           // Common name (null-terminated)
    char scientific_name[64];       // Scientific name (null-terminated)
    
    /* === Crop Coefficients ×1000 (8 bytes) === */
    uint16_t kc_ini_x1000;          // Kc initial stage
    uint16_t kc_dev_x1000;          // Kc development stage
    uint16_t kc_mid_x1000;          // Kc mid-season stage
    uint16_t kc_end_x1000;          // Kc end season stage
    
    /* === Root Depth in mm (4 bytes) === */
    uint16_t root_depth_min_mm;     // Minimum root depth
    uint16_t root_depth_max_mm;     // Maximum root depth
    
    /* === Growth Stages in days (6 bytes) === */
    uint8_t stage_days_ini;         // Initial stage duration
    uint8_t stage_days_dev;         // Development stage duration
    uint16_t stage_days_mid;        // Mid-season stage duration
    uint8_t stage_days_end;         // End season stage duration
    uint8_t growth_cycle;           // Growth cycle type (0=annual, 1=perennial)
    
    /* === Depletion and Spacing (10 bytes) === */
    uint16_t depletion_fraction_p_x1000; // Allowable depletion (×1000)
    uint16_t spacing_row_mm;        // Row spacing in mm
    uint16_t spacing_plant_mm;      // Plant spacing in mm
    uint16_t density_x100;          // Default density plants/m² (×100)
    uint16_t canopy_max_x1000;      // Max canopy cover fraction (×1000)
    
    /* === Temperature (3 bytes) === */
    int8_t frost_tolerance_c;       // Frost tolerance (°C, signed)
    uint8_t temp_opt_min_c;         // Optimal minimum temperature (°C)
    uint8_t temp_opt_max_c;         // Optimal maximum temperature (°C)
    
    /* === Irrigation (1 byte) === */
    uint8_t typ_irrig_method_id;    // Typical irrigation method ID
    
    /* === User-Adjustable Parameters (4 bytes) === */
    uint16_t water_need_factor_x100; // Water need multiplier (10-500 = 0.1-5.0)
    uint8_t irrigation_freq_days;    // Recommended irrigation frequency (1-30 days)
    uint8_t prefer_area_based;       // 1 = area-based (m²), 0 = plant count based
    
} pack_plant_v1_t;

#define PACK_PLANT_V1_SIZE 156
```

### Structure Size Breakdown

| Section | Fields | Bytes |
|---------|--------|-------|
| Identification | `plant_id`, `pack_id`, `version`, `reserved` | 8 |
| Names | `common_name[48]`, `scientific_name[64]` | 112 |
| Crop Coefficients | `kc_ini`, `kc_dev`, `kc_mid`, `kc_end` (×1000) | 8 |
| Root Depth | `root_depth_min_mm`, `root_depth_max_mm` | 4 |
| Growth Stages | `stage_days_ini/dev/mid/end`, `growth_cycle` | 6 |
| Depletion & Spacing | `depletion_fraction_p`, `spacing_*`, `density`, `canopy` | 10 |
| Temperature | `frost_tolerance_c`, `temp_opt_min/max_c` | 3 |
| Irrigation | `typ_irrig_method_id` | 1 |
| User-Adjustable | `water_need_factor`, `irrigation_freq`, `prefer_area` | 4 |
| **Total** | | **156** |

### Field Details

#### Identification Fields

| Field | Size | Description |
|-------|------|-------------|
| `plant_id` | 2 | Unique ID: 1-223 = ROM, ≥224 = custom |
| `pack_id` | 2 | Pack this plant belongs to (0 = standalone) |
| `version` | 2 | Version number for updates |
| `reserved` | 2 | Reserved for alignment |

#### Name Fields

| Field | Size | Description |
|-------|------|-------------|
| `common_name` | 48 | English common name (null-terminated) |
| `scientific_name` | 64 | Latin scientific name (null-terminated) |

#### FAO-56 Crop Coefficients (Kc)

All Kc values are scaled ×1000 for precision without floating-point:

| Field | Range | Example |
|-------|-------|---------|
| `kc_ini_x1000` | 150-800 | 350 = 0.35 |
| `kc_dev_x1000` | 400-1000 | 700 = 0.70 |
| `kc_mid_x1000` | 800-1300 | 1150 = 1.15 |
| `kc_end_x1000` | 250-1100 | 700 = 0.70 |

```c
// Convert to float
float kc_mid_float = (float)plant.kc_mid_x1000 / 1000.0f;
```

#### Growth Stages

Based on FAO-56 Table 11 methodology:

| Stage | Field | Type | Typical Range |
|-------|-------|------|---------------|
| Initial | `stage_days_ini` | uint8 | 10-60 days |
| Development | `stage_days_dev` | uint8 | 25-75 days |
| Mid-season | `stage_days_mid` | uint16 | 25-250 days |
| Late season | `stage_days_end` | uint8 | 10-40 days |
| Cycle | `growth_cycle` | uint8 | 0=annual, 1=perennial |

#### Root Depth

| Field | Unit | Range |
|-------|------|-------|
| `root_depth_min_mm` | mm | 100-600 |
| `root_depth_max_mm` | mm | 300-2000 |

#### Depletion and Spacing

| Field | Description | Typical |
|-------|-------------|---------|
| `depletion_fraction_p_x1000` | Allowable p ×1000 | 400-650 (0.40-0.65) |
| `spacing_row_mm` | Row spacing in mm | 300-1000 |
| `spacing_plant_mm` | Plant spacing in mm | 200-600 |
| `density_x100` | Plants/m² ×100 | 100-2500 (1.0-25.0) |
| `canopy_max_x1000` | Max canopy ×1000 | 600-1000 (0.6-1.0) |

#### Temperature

| Field | Unit | Description |
|-------|------|-------------|
| `frost_tolerance_c` | °C (signed) | Minimum survivable temp |
| `temp_opt_min_c` | °C | Optimal growth min temp |
| `temp_opt_max_c` | °C | Optimal growth max temp |

#### User-Adjustable Parameters

| Field | Range | Default | Description |
|-------|-------|---------|-------------|
| `water_need_factor_x100` | 10-500 | 100 | Water multiplier (0.1-5.0×) |
| `irrigation_freq_days` | 1-30 | 3 | Suggested watering frequency |
| `prefer_area_based` | 0-1 | 0 | Coverage mode preference |

---

## Pack Structure (40 bytes)

Pack metadata for grouping related plants:

```c
typedef struct __attribute__((packed)) {
    // === Identification (8 bytes) ===
    uint16_t pack_id;               // Unique pack ID (1-65535)
    uint16_t version;               // Pack version
    uint16_t plant_count;           // Number of plants in pack
    uint16_t flags;                 // Pack flags
    
    // === Name (32 bytes) ===
    char name[32];                  // Pack name (null-terminated)
} pack_pack_v1_t;

#define PACK_PACK_V1_SIZE 40
```

### Pack Flags

| Bit | Name | Description |
|-----|------|-------------|
| 0 | `PACK_FLAG_OFFICIAL` | Official/verified pack |
| 1 | `PACK_FLAG_REGIONAL` | Region-specific plants |
| 2 | `PACK_FLAG_INDOOR` | Indoor/houseplants |
| 3 | `PACK_FLAG_EDIBLE` | Food crops |
| 4-15 | Reserved | Future use |

---

## Plant Source Enum

```c
typedef enum {
    PLANT_SOURCE_ROM = 0,           // Built-in ROM database
    PLANT_SOURCE_CUSTOM = 1,        // User-defined custom plant
    PLANT_SOURCE_PACK = 2,          // From installed pack
    PLANT_SOURCE_CLOUD = 3,         // Downloaded from cloud
} plant_source_t;
```

---

## Result Codes

```c
typedef enum {
    PACK_RESULT_SUCCESS = 0,        // Operation completed
    PACK_RESULT_UPDATED = 1,        // Existing item updated
    PACK_RESULT_ALREADY_CURRENT = 2,// Already at this version
    PACK_RESULT_INVALID_DATA = 3,   // Validation failed
    PACK_RESULT_INVALID_VERSION = 4,// Schema version unsupported
    PACK_RESULT_STORAGE_FULL = 5,   // Flash full
    PACK_RESULT_IO_ERROR = 6,       // Filesystem error
    PACK_RESULT_NOT_FOUND = 7,      // Item not found
    PACK_RESULT_CRC_MISMATCH = 8,   // CRC validation failed
} pack_result_t;
```

---

## Size Constants

```c
#define PACK_COMMON_NAME_MAX_LEN    48
#define PACK_SCIENTIFIC_NAME_MAX_LEN 64
#define PACK_NAME_MAX_LEN           32

#define PACK_FILE_HEADER_SIZE       16
#define PACK_PLANT_V1_SIZE          156
#define PACK_PACK_V1_SIZE           40

// Total file sizes
#define PACK_PLANT_FILE_SIZE        (16 + 156)  // 172 bytes
#define PACK_PACK_FILE_SIZE         (16 + 40)   // 56 bytes
```

---

## Binary Layout Examples

### Plant File (172 bytes total)

```
Offset  Size  Field
------  ----  -----
0x00    4     magic (0x504C4E54 "PLNT")
0x04    1     schema_version (1)
0x05    3     reserved
0x08    4     crc32
0x0C    4     payload_size (156)
--- Header end (16 bytes) ---
0x10    2     plant_id
0x12    2     pack_id
0x14    2     version
0x16    2     reserved
0x18    48    common_name
0x48    64    scientific_name
0x88    2     kc_ini_x1000
0x8A    2     kc_dev_x1000
0x8C    2     kc_mid_x1000
0x8E    2     kc_end_x1000
0x90    2     root_depth_min_mm
0x92    2     root_depth_max_mm
0x94    1     stage_days_ini
0x95    1     stage_days_dev
0x96    2     stage_days_mid
0x98    1     stage_days_end
0x99    1     growth_cycle
0x9A    2     depletion_fraction_p_x1000
0x9C    2     spacing_row_mm
0x9E    2     spacing_plant_mm
0xA0    2     density_x100
0xA2    2     canopy_max_x1000
0xA4    1     frost_tolerance_c (int8)
0xA5    1     temp_opt_min_c
0xA6    1     temp_opt_max_c
0xA7    1     typ_irrig_method_id
0xA8    2     water_need_factor_x100
0xAA    1     irrigation_freq_days
0xAB    1     prefer_area_based
--- Total: 172 bytes (16 header + 156 plant) ---
```

---

## Validation Rules

### Plant Validation

```c
bool validate_plant(const pack_plant_v1_t *p) {
    // ID range check
    if (p->plant_id < 1000) return false;
    
    // Kc sanity check
    if (p->kc_ini > 200 || p->kc_mid > 200 || p->kc_end > 200) 
        return false;
    
    // Growth stage sum check
    uint32_t total = p->l_ini_days + p->l_dev_days + 
                     p->l_mid_days + p->l_end_days;
    if (total > 400) return false;  // > 400 days unrealistic
    
    // Root depth check
    if (p->root_depth_max < p->root_depth_min) return false;
    
    // Depletion fraction range (0.20-0.90)
    if (p->depletion_fraction < 20 || p->depletion_fraction > 90)
        return false;
    
    return true;
}
```

---

## Example Plant Data

### Tomato (Custom)

```c
pack_plant_v1_t tomato = {
    .plant_id = 1001,
    .pack_id = 1,
    .version = 1,
    .source = PLANT_SOURCE_PACK,
    .flags = 0,
    .common_name = "Tomato",
    .scientific_name = "Solanum lycopersicum",
    
    // FAO-56 Table 12 values
    .kc_ini = 60,       // 0.60
    .kc_mid = 115,      // 1.15
    .kc_end = 80,       // 0.80
    
    // Growth stages (135 day season)
    .l_ini_days = 35,
    .l_dev_days = 40,
    .l_mid_days = 40,
    .l_end_days = 20,
    
    // Root zone
    .root_depth_min = 300,  // 30cm
    .root_depth_max = 1500, // 150cm
    .root_growth_rate = 25, // 2.5mm/day
    
    // Water management
    .depletion_fraction = 40, // p=0.40
    .yield_response = 110,    // Ky=1.10
    .critical_depletion = 60, // 0.60
    
    // Environmental tolerances
    .temp_min = 10,
    .temp_max = 35,
    .temp_optimal_low = 20,
    .temp_optimal_high = 27,
    .humidity_min = 50,
    .humidity_max = 80,
    .light_min = 30,  // 30 klux
    .light_max = 80,
};
```

---

## Future Considerations

### Schema Version 2 (Planned)

Potential additions:
- Salinity tolerance (ECe threshold)
- CO2 response factors
- Growth habit classification
- Companion planting data
- Pest/disease susceptibility

Schema version will increment to 2, with backwards-compatible readers.
