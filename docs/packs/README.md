# Plant Packs System Documentation

**Version**: 1.0.0  
**Author**: AutoWatering Team  
**Last Updated**: January 2026

## Overview

The Plant Packs system enables storage and management of custom plant profiles on external flash storage. This extends the built-in database of 223 ROM plants with user-defined species, supporting FAO-56 irrigation calculations.

## Architecture

```
┌─────────────────────────────────────────────────────────────────────┐
│                         BLE Interface                                │
│  ┌─────────────────┐  ┌─────────────────┐  ┌─────────────────────┐  │
│  │  Pack Plant     │  │  Pack Stats     │  │  Pack Transfer      │  │
│  │  (def...6786)   │  │  (def...6787)   │  │  (def...6788)       │  │
│  └────────┬────────┘  └────────┬────────┘  └──────────┬──────────┘  │
└───────────┼────────────────────┼─────────────────────┼──────────────┘
            │                    │                     │
            ▼                    ▼                     ▼
┌─────────────────────────────────────────────────────────────────────┐
│                      bt_pack_handlers.c                              │
│  • Single plant install/delete/list                                  │
│  • Storage statistics                                                │
│  • Multi-part transfer state machine                                 │
└────────────────────────────────┬────────────────────────────────────┘
                                 │
                                 ▼
┌─────────────────────────────────────────────────────────────────────┐
│                        pack_storage.c                                │
│  • LittleFS mount on ext_storage_partition                          │
│  • Atomic file operations with CRC32                                 │
│  • Plant/Pack CRUD operations                                        │
│  • FAO-56 coefficient helpers                                        │
└────────────────────────────────┬────────────────────────────────────┘
                                 │
                                 ▼
┌─────────────────────────────────────────────────────────────────────┐
│                     External SPI Flash                               │
│  W25Q128 (16MB) @ /lfs_ext                                          │
│  ┌──────────────┐  ┌──────────────┐                                 │
│  │ /plants/     │  │ /packs/      │                                 │
│  │  0001.bin    │  │  0001.bin    │                                 │
│  │  0002.bin    │  │  ...         │                                 │
│  └──────────────┘  └──────────────┘                                 │
└─────────────────────────────────────────────────────────────────────┘
```

## Key Components

| Component | File | Description |
|-----------|------|-------------|
| Schema | [pack_schema.h](PACK_SCHEMA.md) | Binary structure definitions |
| Storage | [pack_storage.h/c](PACK_STORAGE.md) | Flash filesystem operations |
| BLE Handlers | [bt_pack_handlers.h/c](BLE_PACK_SERVICE.md) | GATT service implementation |
| Transfer Protocol | - | [TRANSFER_PROTOCOL.md](TRANSFER_PROTOCOL.md) |
| **Sync Guide** | - | [SYNC_GUIDE.md](SYNC_GUIDE.md) | **Mobile app sync workflow** |
| Mobile App Guide | - | [MOBILE_APP_GUIDE.md](MOBILE_APP_GUIDE.md) | Full mobile implementation |

## Quick Start

### Reading a Custom Plant (C Code)

```c
#include "pack_storage.h"

pack_plant_v1_t plant;
pack_result_t result = pack_storage_get_plant(1001, &plant);

if (result == PACK_RESULT_SUCCESS) {
    LOG_INF("Plant: %s (Kc_mid=%.2f)", 
            plant.common_name, 
            (float)plant.kc_mid / 100.0f);
}
```

### Getting Kc for FAO-56 (Watering Integration)

```c
#include "pack_storage.h"

// For custom plant (ID >= 1000)
uint16_t kc_scaled = pack_storage_get_kc(plant_id, stage);
float kc = (float)kc_scaled / 100.0f;

// For built-in ROM plant (ID < 1000)
const plant_full_species_t *rom = plant_full_get_by_index(index);
uint16_t kc = PLANT_KC_MID(rom); // Already scaled x100
```

### BLE Plant Installation (Mobile App)

```
1. Connect to device
2. Enable notifications on Pack Plant characteristic
3. Write 156-byte pack_plant_v1_t structure
4. Read notification for result
```

## Storage Layout

```
/lfs_ext/
├── plants/
│   ├── 0001.bin     (16B header + 156B plant)
│   ├── 0002.bin
│   └── ...
└── packs/
    ├── 0001.bin     (16B header + 40B pack metadata)
    └── ...
```

## ID Ranges

| Range | Type | Description |
|-------|------|-------------|
| 0 | Virtual Pack | Built-in ROM database (223 plants) |
| 1-999 | ROM Plants | Built-in plant_full_species_t |
| 1000+ | Custom Plants | User-installed pack_plant_v1_t |

## Memory Budget

| Resource | Size | Notes |
|----------|------|-------|
| Transfer buffer | 9,984 bytes | 64 plants max per transfer |
| Plant structure | 156 bytes | pack_plant_v1_t |
| Pack metadata | 40 bytes | pack_pack_v1_t |
| File header | 16 bytes | Magic + version + CRC |
| Flash partition | ~14.4 MB | ext_storage_partition |

## Documentation Index

1. **[PACK_SCHEMA.md](PACK_SCHEMA.md)** - Binary structure definitions
2. **[PACK_STORAGE.md](PACK_STORAGE.md)** - Storage API reference
3. **[BLE_PACK_SERVICE.md](BLE_PACK_SERVICE.md)** - BLE service documentation
4. **[TRANSFER_PROTOCOL.md](TRANSFER_PROTOCOL.md)** - Multi-part transfer protocol
5. **[INTEGRATION_GUIDE.md](INTEGRATION_GUIDE.md)** - Watering system integration
6. **[MOBILE_APP_GUIDE.md](MOBILE_APP_GUIDE.md)** - Client implementation guide

## Version History

| Version | Date | Changes |
|---------|------|---------|
| 1.0.0 | Jan 2026 | Initial release with schema v1 |

