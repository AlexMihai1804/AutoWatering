# Watering System Integration Guide

**Version**: 1.0.0

## Overview

This guide explains how the Pack system integrates with the AutoWatering FAO-56 irrigation calculations. Custom plants from packs are fully supported in all watering algorithms.

---

## Architecture

```
┌─────────────────────────────────────────────────────────────────────┐
│                        Watering Engine                               │
│  ┌─────────────────────────────────────────────────────────────┐    │
│  │                     fao56_calc.c                             │    │
│  │  ETc = ETo × Kc × stress_factor × temperature_compensation  │    │
│  └────────────────────────────────┬────────────────────────────┘    │
└───────────────────────────────────┼─────────────────────────────────┘
                                    │
                        ┌───────────┴───────────┐
                        ▼                       ▼
            ┌─────────────────────┐  ┌─────────────────────┐
            │   ROM Plants        │  │   Custom Plants     │
            │   (plant_db.h)      │  │   (pack_storage.h)  │
            │   ID < 1000         │  │   ID ≥ 1000         │
            └─────────────────────┘  └─────────────────────┘
```

---

## Channel Configuration

### watering_channel_t Structure

Each irrigation channel includes a `custom_plant_id` field:

```c
// In watering.h
typedef struct {
    // ... other fields ...
    
    uint8_t plant_index;        // ROM plant index (0-222)
    uint16_t custom_plant_id;   // Custom plant ID (0 = use ROM)
    
    // ... other fields ...
} watering_channel_t;
```

### Interpretation

| custom_plant_id | plant_index | Source |
|-----------------|-------------|--------|
| 0 | 0-222 | ROM plant from `plant_full_db` |
| 1000+ | (ignored) | Custom plant from pack storage |

---

## NVS Persistence

Custom plant IDs are persisted in NVS:

```c
// In nvs_config.h
typedef struct __attribute__((packed)) {
    // ... other fields ...
    uint16_t custom_plant_id;   // Custom plant ID (0 = ROM)
    // ... other fields ...
} enhanced_channel_config_t;
```

### Save Operation

```c
// In nvs_config.c
int nvs_save_complete_channel_config(uint8_t channel, 
                                     const watering_channel_t *config)
{
    enhanced_channel_config_t enhanced = {
        // ... other fields ...
        .custom_plant_id = config->custom_plant_id,
    };
    
    return nvs_write(&enhanced, sizeof(enhanced));
}
```

### Load Operation

```c
int nvs_load_complete_channel_config(uint8_t channel,
                                     watering_channel_t *config)
{
    enhanced_channel_config_t enhanced;
    // ... read from NVS ...
    
    config->custom_plant_id = enhanced.custom_plant_id;
    
    return 0;
}
```

---

## Getting Plant Parameters

### Unified Access Pattern

```c
#include "pack_storage.h"
#include "plant_db.h"
#include "fao56_calc.h"

float get_kc_for_channel(const watering_channel_t *channel, 
                         growth_stage_t stage)
{
    uint16_t kc_scaled;
    
    if (channel->custom_plant_id >= 1000) {
        // Custom plant from pack storage
        kc_scaled = pack_storage_get_kc(channel->custom_plant_id, stage);
    } else {
        // ROM plant
        const plant_full_species_t *plant = 
            plant_full_get_by_index(channel->plant_index);
        
        switch (stage) {
            case GROWTH_STAGE_INITIAL:
                kc_scaled = PLANT_KC_INI(plant);
                break;
            case GROWTH_STAGE_MID:
                kc_scaled = PLANT_KC_MID(plant);
                break;
            case GROWTH_STAGE_END:
                kc_scaled = PLANT_KC_END(plant);
                break;
            default:
                kc_scaled = 100; // 1.00 fallback
        }
    }
    
    return (float)kc_scaled / 100.0f;
}
```

### Helper Functions in pack_storage.h

```c
/**
 * Get Kc for any plant (ROM or custom)
 * 
 * @param plant_id Plant ID (< 1000 for ROM, >= 1000 for custom)
 * @param stage Growth stage
 * @return Kc scaled ×100
 */
uint16_t pack_storage_get_kc(uint16_t plant_id, growth_stage_t stage);

/**
 * Get root depth for any plant
 * 
 * @param plant_id Plant ID
 * @param days_since_planting Days since start
 * @return Root depth in mm
 */
uint16_t pack_storage_get_root_depth(uint16_t plant_id, 
                                     uint16_t days_since_planting);
```

---

## FAO-56 Calculation Integration

### ETc Calculation

```c
// In fao56_calc.c

float calculate_etc(const watering_channel_t *channel,
                    float eto,
                    growth_stage_t stage)
{
    // Get Kc from appropriate source
    float kc;
    
    if (channel->custom_plant_id >= 1000) {
        // Custom plant
        kc = (float)pack_storage_get_kc(channel->custom_plant_id, stage) / 100.0f;
    } else {
        // ROM plant
        kc = get_rom_plant_kc(channel->plant_index, stage);
    }
    
    // Apply stress factors
    float ks = calculate_stress_factor(channel);
    
    // ETc = ETo × Kc × Ks
    return eto * kc * ks;
}
```

### Soil Water Balance

```c
float calculate_soil_water_balance(const watering_channel_t *channel,
                                   uint16_t days_since_planting)
{
    // Get root zone depth
    uint16_t root_depth_mm;
    
    if (channel->custom_plant_id >= 1000) {
        root_depth_mm = pack_storage_get_root_depth(
            channel->custom_plant_id, 
            days_since_planting
        );
    } else {
        root_depth_mm = get_rom_plant_root_depth(
            channel->plant_index,
            days_since_planting
        );
    }
    
    // Calculate TAW (Total Available Water)
    float taw = calculate_taw(root_depth_mm, channel->soil_type);
    
    // Get depletion fraction
    float p = get_depletion_fraction(channel) / 100.0f;
    
    // RAW = TAW × p
    float raw = taw * p;
    
    return raw;
}
```

### Depletion Fraction

```c
uint16_t get_depletion_fraction(const watering_channel_t *channel)
{
    if (channel->custom_plant_id >= 1000) {
        pack_plant_v1_t plant;
        if (pack_storage_get_plant(channel->custom_plant_id, &plant) 
            == PACK_RESULT_SUCCESS) {
            return plant.depletion_fraction;
        }
        return 50; // Default 0.50
    } else {
        const plant_full_species_t *plant = 
            plant_full_get_by_index(channel->plant_index);
        return plant->depletion_fraction;
    }
}
```

---

## BLE Configuration

### Setting Custom Plant via BLE

Custom plant assignment is done through the existing channel configuration characteristic:

```c
// Write to Channel Config characteristic
typedef struct __attribute__((packed)) {
    uint8_t channel;            // Channel index
    uint8_t plant_index;        // ROM plant (ignored if custom_plant_id != 0)
    uint16_t custom_plant_id;   // Custom plant ID (0 = use ROM)
    // ... other config fields ...
} bt_channel_config_t;
```

### Workflow

1. Install custom plant via Pack Plant characteristic
2. Note the plant_id (e.g., 1001)
3. Write channel config with `custom_plant_id = 1001`
4. Channel now uses custom plant parameters

---

## Example: Complete Integration

### Installing and Using a Custom Plant

```c
// 1. Create custom plant
pack_plant_v1_t my_herb = {
    .plant_id = 1001,
    .pack_id = 1,
    .version = 1,
    .source = PLANT_SOURCE_CUSTOM,
    .common_name = "My Basil",
    .scientific_name = "Ocimum basilicum",
    .kc_ini = 40,       // 0.40
    .kc_mid = 105,      // 1.05
    .kc_end = 90,       // 0.90
    .l_ini_days = 20,
    .l_dev_days = 30,
    .l_mid_days = 40,
    .l_end_days = 10,
    .root_depth_min = 200,
    .root_depth_max = 600,
    .depletion_fraction = 35,  // 0.35
    // ... other fields ...
};

// 2. Install plant
pack_result_t result = pack_storage_install_plant(&my_herb);

// 3. Configure channel
watering_channel_t channel;
channel.custom_plant_id = 1001;  // Use custom plant
channel.enabled = true;
// ... other config ...

// 4. Save to NVS
nvs_save_complete_channel_config(0, &channel);

// 5. Calculate watering
float eto = get_reference_et();  // From weather data
float kc = (float)pack_storage_get_kc(1001, GROWTH_STAGE_MID) / 100.0f;
float etc = eto * kc;
float watering_mm = etc - effective_rainfall;
```

---

## Compatibility Notes

### ROM Plant Access

ROM plants remain accessible via:
- `plant_full_get_by_index(index)` - Direct access
- `pack_storage_get_kc(id, stage)` with ID < 1000 - Unified access

### Version Migration

When updating plant data:
1. Install new version via pack
2. Channel automatically uses updated parameters
3. No channel reconfiguration needed

### Fallback Behavior

If custom plant cannot be read:
- Kc returns 100 (1.00)
- Root depth returns 500mm
- Log warning issued

```c
uint16_t pack_storage_get_kc(uint16_t plant_id, growth_stage_t stage)
{
    if (plant_id < 1000) {
        // ROM plant
        return get_rom_kc(plant_id, stage);
    }
    
    pack_plant_v1_t plant;
    if (pack_storage_get_plant(plant_id, &plant) != PACK_RESULT_SUCCESS) {
        LOG_WRN("Cannot read plant %u, using default Kc", plant_id);
        return 100;  // 1.00 fallback
    }
    
    switch (stage) {
        case GROWTH_STAGE_INITIAL: return plant.kc_ini;
        case GROWTH_STAGE_MID:     return plant.kc_mid;
        case GROWTH_STAGE_END:     return plant.kc_end;
        default:                   return plant.kc_mid;
    }
}
```

---

## Performance Considerations

### Caching

For frequently accessed plants, consider caching:

```c
// Cache for active channel plants
static struct {
    uint16_t plant_id;
    pack_plant_v1_t plant;
    bool valid;
} plant_cache[MAX_CHANNELS];

const pack_plant_v1_t *get_cached_plant(uint8_t channel, uint16_t plant_id)
{
    if (plant_cache[channel].valid && 
        plant_cache[channel].plant_id == plant_id) {
        return &plant_cache[channel].plant;
    }
    
    if (pack_storage_get_plant(plant_id, &plant_cache[channel].plant) 
        == PACK_RESULT_SUCCESS) {
        plant_cache[channel].plant_id = plant_id;
        plant_cache[channel].valid = true;
        return &plant_cache[channel].plant;
    }
    
    return NULL;
}
```

### Access Timing

| Operation | Typical Time |
|-----------|--------------|
| ROM plant lookup | <1μs |
| Custom plant read | 5-15ms |
| Cached plant access | <1μs |

Recommendation: Cache custom plant data at channel init and refresh on plant update.

---

## Testing

### Unit Test Example

```c
void test_custom_plant_kc(void)
{
    // Install test plant
    pack_plant_v1_t test = {
        .plant_id = 9999,
        .kc_ini = 40,
        .kc_mid = 120,
        .kc_end = 80,
    };
    pack_storage_install_plant(&test);
    
    // Verify Kc retrieval
    uint16_t kc_ini = pack_storage_get_kc(9999, GROWTH_STAGE_INITIAL);
    assert(kc_ini == 40);
    
    uint16_t kc_mid = pack_storage_get_kc(9999, GROWTH_STAGE_MID);
    assert(kc_mid == 120);
    
    // Cleanup
    pack_storage_delete_plant(9999);
}
```

### Integration Test

```c
void test_channel_with_custom_plant(void)
{
    // Setup
    pack_plant_v1_t tomato = create_tomato_plant();
    pack_storage_install_plant(&tomato);
    
    watering_channel_t channel = {
        .custom_plant_id = tomato.plant_id,
        .enabled = true,
    };
    
    // Test calculation
    float eto = 5.0f;  // mm/day
    float etc = calculate_etc(&channel, eto, GROWTH_STAGE_MID);
    
    // Tomato Kc_mid = 1.15
    float expected = 5.0f * 1.15f;
    assert(fabs(etc - expected) < 0.01f);
    
    // Cleanup
    pack_storage_delete_plant(tomato.plant_id);
}
```
