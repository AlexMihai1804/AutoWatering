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

## Plant Structure (120 bytes)

The `pack_plant_v1_t` structure contains all FAO-56 parameters for a custom plant species:

```c
typedef struct __attribute__((packed)) {
    // === Identification (12 bytes) ===
    uint16_t plant_id;              // Unique plant ID (1000+)
    uint16_t pack_id;               // Owning pack ID (0 = standalone)
    uint16_t version;               // Plant data version
    plant_source_t source;          // Data source (1 byte enum)
    uint8_t flags;                  // Feature flags
    uint32_t reserved_id;           // Reserved
    
    // === Names (64 bytes) ===
    char common_name[32];           // Common name (null-terminated)
    char scientific_name[32];       // Scientific name (null-terminated)
    
    // === FAO-56 Crop Coefficients (8 bytes) ===
    uint16_t kc_ini;                // Kc initial (×100, e.g., 35 = 0.35)
    uint16_t kc_mid;                // Kc mid-season (×100)
    uint16_t kc_end;                // Kc end (×100)
    uint16_t kc_flags;              // Coefficient flags
    
    // === Growth Stage Durations (8 bytes) ===
    uint16_t l_ini_days;            // Initial stage (days)
    uint16_t l_dev_days;            // Development stage (days)
    uint16_t l_mid_days;            // Mid-season stage (days)
    uint16_t l_end_days;            // Late season stage (days)
    
    // === Root Characteristics (8 bytes) ===
    uint16_t root_depth_min;        // Min root depth (mm)
    uint16_t root_depth_max;        // Max root depth (mm)
    uint16_t root_growth_rate;      // Growth rate (mm/day ×10)
    uint16_t root_flags;            // Root zone flags
    
    // === Water Requirements (8 bytes) ===
    uint16_t depletion_fraction;    // Allowable depletion (×100)
    uint16_t yield_response;        // Ky factor (×100)
    uint16_t critical_depletion;    // Critical stress point (×100)
    uint16_t water_flags;           // Water management flags
    
    // === Environmental Tolerances (8 bytes) ===
    int8_t temp_min;                // Min temperature (°C)
    int8_t temp_max;                // Max temperature (°C)
    int8_t temp_optimal_low;        // Optimal low temp (°C)
    int8_t temp_optimal_high;       // Optimal high temp (°C)
    uint8_t humidity_min;           // Min humidity (%)
    uint8_t humidity_max;           // Max humidity (%)
    uint8_t light_min;              // Min light (klux)
    uint8_t light_max;              // Max light (klux)
    
    // === Reserved (4 bytes) ===
    uint32_t reserved;              // Future expansion
} pack_plant_v1_t;

#define PACK_PLANT_V1_SIZE 120
```

### Field Details

#### Identification Fields

| Field | Size | Description |
|-------|------|-------------|
| `plant_id` | 2 | Unique ID, must be ≥1000 for custom plants |
| `pack_id` | 2 | Pack this plant belongs to (0 = standalone) |
| `version` | 2 | Version number for updates |
| `source` | 1 | See `plant_source_t` enum |
| `flags` | 1 | Bit flags (see below) |

#### Plant Flags (flags field)

| Bit | Name | Description |
|-----|------|-------------|
| 0 | `PACK_PLANT_FLAG_PERENNIAL` | Perennial vs annual |
| 1 | `PACK_PLANT_FLAG_GREENHOUSE` | Greenhouse cultivation |
| 2 | `PACK_PLANT_FLAG_DROUGHT_TOLERANT` | Low water needs |
| 3 | `PACK_PLANT_FLAG_HUMID` | High humidity needs |
| 4-7 | Reserved | Future use |

#### FAO-56 Crop Coefficients (Kc)

All Kc values are scaled ×100 to avoid floating-point:

| Field | Range | Example |
|-------|-------|---------|
| `kc_ini` | 15-80 | 35 = 0.35 |
| `kc_mid` | 80-130 | 115 = 1.15 |
| `kc_end` | 25-110 | 70 = 0.70 |

```c
// Convert to float
float kc_mid_float = (float)plant.kc_mid / 100.0f;
```

#### Growth Stages

Based on FAO-56 Table 11 methodology:

| Stage | Field | Typical Range |
|-------|-------|---------------|
| Initial | `l_ini_days` | 10-60 days |
| Development | `l_dev_days` | 25-75 days |
| Mid-season | `l_mid_days` | 25-80 days |
| Late season | `l_end_days` | 10-40 days |

```c
// Calculate current growth stage
int total_days = ini + dev + mid + end;
int days_since_planting = ...;

if (days_since_planting <= ini) {
    stage = GROWTH_STAGE_INITIAL;
} else if (days_since_planting <= ini + dev) {
    stage = GROWTH_STAGE_DEVELOPMENT;
}
// ... etc
```

#### Root Depth

| Field | Unit | Range |
|-------|------|-------|
| `root_depth_min` | mm | 100-600 |
| `root_depth_max` | mm | 300-2000 |
| `root_growth_rate` | mm/day ×10 | 5-30 (0.5-3.0) |

#### Depletion Fraction (p)

FAO-56 allowable depletion before stress:

| Field | Description | Typical |
|-------|-------------|---------|
| `depletion_fraction` | p value ×100 | 40-65 (0.40-0.65) |
| `yield_response` | Ky ×100 | 80-150 |
| `critical_depletion` | Critical p ×100 | 60-80 |

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
#define PACK_COMMON_NAME_MAX_LEN    32
#define PACK_SCIENTIFIC_NAME_MAX_LEN 32
#define PACK_NAME_MAX_LEN           32

#define PACK_FILE_HEADER_SIZE       16
#define PACK_PLANT_V1_SIZE          120
#define PACK_PACK_V1_SIZE           40

// Total file sizes
#define PACK_PLANT_FILE_SIZE        (16 + 120)  // 136 bytes
#define PACK_PACK_FILE_SIZE         (16 + 40)   // 56 bytes
```

---

## Binary Layout Examples

### Plant File (136 bytes total)

```
Offset  Size  Field
------  ----  -----
0x00    4     magic (0x504C4E54 "PLNT")
0x04    2     version (1)
0x06    2     data_type (1 = plant)
0x08    4     crc32
0x0C    4     reserved
--- Header end (16 bytes) ---
0x10    2     plant_id
0x12    2     pack_id
0x14    2     version
0x16    1     source
0x17    1     flags
0x18    4     reserved_id
0x1C    32    common_name
0x3C    32    scientific_name
0x5C    2     kc_ini
0x5E    2     kc_mid
0x60    2     kc_end
0x62    2     kc_flags
0x64    2     l_ini_days
0x66    2     l_dev_days
0x68    2     l_mid_days
0x6A    2     l_end_days
0x6C    2     root_depth_min
0x6E    2     root_depth_max
0x70    2     root_growth_rate
0x72    2     root_flags
0x74    2     depletion_fraction
0x76    2     yield_response
0x78    2     critical_depletion
0x7A    2     water_flags
0x7C    1     temp_min
0x7D    1     temp_max
0x7E    1     temp_optimal_low
0x7F    1     temp_optimal_high
0x80    1     humidity_min
0x81    1     humidity_max
0x82    1     light_min
0x83    1     light_max
0x84    4     reserved
--- Total: 136 bytes ---
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
