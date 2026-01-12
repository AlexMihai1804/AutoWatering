# Watering System Integration Guide

**Version**: 2.0.0

## Overview

This guide explains how the Pack system integrates with the AutoWatering FAO-56 irrigation calculations. All plants (default and custom) are stored in pack storage on external flash and accessed through a unified `plant_id` field.

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
                                    ▼
                    ┌─────────────────────────────────┐
                    │   Unified Pack Storage          │
                    │   (pack_storage.h)              │
                    │                                 │
                    │   plant_id = 0     → not set    │
                    │   plant_id = 1-223 → default    │
                    │   plant_id ≥ 1000  → custom     │
                    └─────────────────────────────────┘
```

---

## Unified Plant Storage System

### Overview

All plants are stored on external flash using LittleFS. The 223 default plants from ROM are provisioned to flash on first boot, eliminating the need for dual storage lookups.

### Plant ID Scheme

| plant_id | Description |
|----------|-------------|
| 0 | Not configured |
| 1-223 | Default plants (provisioned from ROM) |
| ≥1000 | Custom plants from user or packs |

### Storage Path

Plants are stored at: `/lfs_ext/packs/plants/p_XXXX.bin`

---

## Channel Configuration

### watering_channel_t Structure

Each irrigation channel uses a single `plant_id` field:

```c
// In watering.h
typedef struct {
    // ... other fields ...
    
    uint16_t plant_id;          // Unified plant ID (0 = not set)
    
    // ... other fields ...
} watering_channel_t;
```

### Interpretation

| plant_id | Source |
|----------|--------|
| 0 | Not configured - AUTO mode disabled |
| 1-223 | Default plant from pack storage |
| 1000+ | Custom plant from pack storage |

---

## NVS Persistence

Plant IDs are persisted in NVS:

```c
// In nvs_config.h
typedef struct __attribute__((packed)) {
    // ... other fields ...
    uint16_t plant_id;          // Unified plant ID (0 = not set)
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
        .plant_id = config->plant_id,
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
    
    config->plant_id = enhanced.plant_id;
    
    return 0;
}
```

---

## Getting Plant Parameters

### Unified Access Pattern

All plant data is accessed through pack storage:

```c
#include "pack_storage.h"
#include "fao56_calc.h"

// Using high-level helper (recommended)
const plant_full_data_t *plant = fao56_get_channel_plant(channel, channel_id);
if (plant) {
    float kc_mid = plant->kc_mid_x1000 / 1000.0f;
    uint16_t root_max_mm = plant->root_depth_max_m_x1000; // Note: same units
}

// Or using pack storage directly
float kc;
pack_result_t res = pack_storage_get_kc(channel->plant_id, dap, &kc);
if (res == PACK_RESULT_SUCCESS) {
    // Use interpolated Kc value
}
```

### Helper Functions in pack_storage.h

```c
/**
 * Get Kc for any plant from pack storage
 * 
 * @param plant_id Plant ID (1-223=default, >=1000=custom, 0=error)
 * @param days_after_planting Days since planting for interpolation
 * @param out_kc Output: interpolated Kc value
 * @return PACK_RESULT_SUCCESS or error
 */
pack_result_t pack_storage_get_kc(uint16_t plant_id,
                                   uint16_t days_after_planting,
                                   float *out_kc);

/**
 * Get root depth for any plant
 * 
 * @param plant_id Plant ID
 * @param days_after_planting Days since start
 * @param out_root_depth_mm Output: root depth in mm
 * @return PACK_RESULT_SUCCESS or error
 */
pack_result_t pack_storage_get_root_depth(uint16_t plant_id, 
                                           uint16_t days_after_planting,
                                           float *out_root_depth_mm);

/**
 * Get full plant data from pack storage
 * 
 * @param plant_id Plant ID
 * @param plant Output buffer for plant data
 * @return PACK_RESULT_SUCCESS or error
 */
pack_result_t pack_storage_get_fao56_plant(uint16_t plant_id,
                                            pack_plant_v1_t *plant);
```

---

## FAO-56 Calculation Integration

### ETc Calculation

```c
// In fao56_calc.c

float calculate_etc(const watering_channel_t *channel,
                    uint8_t channel_id,
                    float eto,
                    uint16_t days_after_planting)
{
    // Get plant data from pack storage (cached per-channel)
    const plant_full_data_t *plant = fao56_get_channel_plant(channel, channel_id);
    if (!plant) {
        return 0.0f;  // No plant configured
    }
    
    // Get interpolated Kc
    float kc = 1.0f;
    pack_storage_get_kc(channel->plant_id, days_after_planting, &kc);
    
    // Apply stress factors
    float ks = calculate_stress_factor(channel);
    
    // ETc = ETo × Kc × Ks
    return eto * kc * ks;
}
```

### Soil Water Balance

```c
float calculate_soil_water_balance(const watering_channel_t *channel,
                                   uint8_t channel_id,
                                   uint16_t days_after_planting)
{
    // Get plant from pack storage
    const plant_full_data_t *plant = fao56_get_channel_plant(channel, channel_id);
    if (!plant) {
        return 0.0f;
    }
    
    // Get root zone depth
    float root_depth_mm = 0.0f;
    pack_storage_get_root_depth(channel->plant_id, days_after_planting, &root_depth_mm);
    
    // Calculate TAW (Total Available Water)
    float taw = calculate_taw(root_depth_mm, channel->soil_db_index);
    
    // Get depletion fraction
    float p = plant->depletion_fraction_p_x1000 / 1000.0f;
    
    // RAW = TAW × p
    float raw = taw * p;
    
    return raw;
}
```

---

## BLE Configuration

### Setting Plant via BLE

Plant assignment is done through the Growing Environment characteristic:

```c
// Write to Growing Environment Config characteristic
struct growing_env_config_data {
    uint8_t channel_id;
    uint16_t plant_id;              // Unified plant ID (0 = not set)
    uint8_t soil_db_index;
    uint8_t irrigation_method_index;
    uint8_t use_area_based;
    // ... other config fields ...
} __packed;
```

### Workflow

1. (For custom plants) Install custom plant via Pack Plant characteristic
2. Note the plant_id (1-223 for default, ≥1000 for custom)
3. Write growing environment config with the `plant_id`
4. Channel now uses plant parameters from pack storage

---

## Example: Complete Integration

### Using Default Plants

```c
// Configure channel to use default Tomato (ID = 15 for example)
watering_channel_t channel;
channel.plant_id = 15;          // Default plant from provisioned storage
channel.enabled = true;
// ... other config ...

// Save to NVS
nvs_save_complete_channel_config(0, &channel);

// Calculate watering
const plant_full_data_t *plant = fao56_get_channel_plant(&channel, 0);
if (plant) {
    float kc_mid = plant->kc_mid_x1000 / 1000.0f;
    // ... use plant parameters
}
```

### Installing and Using a Custom Plant

```c
// 1. Create custom plant (ID must be >= 1000)
pack_plant_v1_t my_herb = {
    .plant_id = 1001,
    .pack_id = 1,
    .version = 1,
    .source = PLANT_SOURCE_CUSTOM,
    .common_name = "My Basil",
    .scientific_name = "Ocimum basilicum",
    .kc_ini_x1000 = 400,       // 0.40
    .kc_mid_x1000 = 1050,      // 1.05
    .kc_end_x1000 = 900,       // 0.90
    .stage_days_ini = 20,
    .stage_days_dev = 30,
    .stage_days_mid = 40,
    .stage_days_end = 10,
    .root_depth_min_mm = 200,
    .root_depth_max_mm = 600,
    .depletion_fraction_p_x1000 = 350,  // 0.35
    // ... other fields ...
};

// 2. Install plant
pack_result_t result = pack_storage_install_plant(&my_herb);

// 3. Configure channel
watering_channel_t channel;
channel.plant_id = 1001;        // Use custom plant
channel.enabled = true;
// ... other config ...

// 4. Save to NVS
nvs_save_complete_channel_config(0, &channel);

// 5. Calculate watering using fao56_calc helpers
const plant_full_data_t *plant = fao56_get_channel_plant(&channel, 0);
if (plant) {
    float kc = 1.0f;
    pack_storage_get_kc(channel.plant_id, days_after_planting, &kc);
    float etc = eto * kc;
    float watering_mm = etc - effective_rainfall;
}
```

---

## Compatibility Notes

### ROM Plant Access

ROM plants remain accessible via:
- `plant_full_get_by_index(index)` - Direct access
- `pack_storage_get_kc(id, stage)` with ID < 1000 - Unified access

### Default Plant Provisioning

At first boot, the 223 default plants are copied from ROM to pack storage:

```c
// In main.c or init sequence
if (!pack_storage_defaults_provisioned()) {
    pack_storage_provision_defaults();
}
```

This enables:
- Unified access pattern for all plants
- Future updates to default plants via OTA
- Consistent caching strategy

### Version Migration

When updating plant data:
1. Install new version via pack (higher version number)
2. Channel automatically uses updated parameters
3. No channel reconfiguration needed

### Fallback Behavior

If plant cannot be read from pack storage:
- Error is returned (no silent fallback)
- Log warning issued
- Channel should check return values

```c
pack_result_t pack_storage_get_kc(uint16_t plant_id,
                                   uint16_t days_after_planting,
                                   float *out_kc)
{
    if (plant_id == 0) {
        LOG_WRN("plant_id=0 means no plant configured");
        return PACK_RESULT_INVALID_DATA;
    }
    
    pack_plant_v1_t plant;
    pack_result_t res = pack_storage_get_plant(plant_id, &plant);
    if (res != PACK_RESULT_SUCCESS) {
        LOG_WRN("Cannot read plant %u: %d", plant_id, res);
        return res;
    }
    
    // Interpolate Kc based on days_after_planting
    *out_kc = interpolate_kc(&plant, days_after_planting);
    return PACK_RESULT_SUCCESS;
}
```

---

## Performance Considerations

### Built-in Caching

The `fao56_get_channel_plant()` function provides per-channel caching:

```c
// Cache for active channel plants (internal to fao56_calc.c)
static plant_full_data_t s_custom_plant_cache[WATERING_CHANNELS_COUNT];
static uint16_t s_custom_plant_cache_id[WATERING_CHANNELS_COUNT];

const plant_full_data_t *fao56_get_channel_plant(const watering_channel_t *channel,
                                                  uint8_t channel_id)
{
    // Check cache first
    if (s_custom_plant_cache_id[channel_id] == channel->plant_id) {
        return &s_custom_plant_cache[channel_id];
    }
    
    // Load from pack storage and cache
    pack_plant_v1_t pack_plant;
    if (pack_storage_get_plant(channel->plant_id, &pack_plant) == PACK_RESULT_SUCCESS) {
        // Convert and cache...
        s_custom_plant_cache_id[channel_id] = channel->plant_id;
        return &s_custom_plant_cache[channel_id];
    }
    
    return NULL;
}
```

### Access Timing

| Operation | Typical Time |
|-----------|--------------|
| Cached plant access | <1μs |
| Pack storage read (flash) | 5-15ms |
| ROM database access | <1μs (but no longer used at runtime) |

Recommendation: Use `fao56_get_channel_plant()` for automatic caching.

---

## Testing

### Unit Test Example

```c
void test_unified_plant_access(void)
{
    // Test default plant access (plant_id 1-223)
    float kc;
    pack_result_t res = pack_storage_get_kc(15, 30, &kc);
    assert(res == PACK_RESULT_SUCCESS);
    assert(kc > 0.0f && kc < 2.0f);
    
    // Install test custom plant
    pack_plant_v1_t test = {
        .plant_id = 9999,
        .kc_ini_x1000 = 400,
        .kc_mid_x1000 = 1200,
        .kc_end_x1000 = 800,
        .stage_days_ini = 20,
        .stage_days_dev = 30,
        .stage_days_mid = 40,
        .stage_days_end = 10,
    };
    pack_storage_install_plant(&test);
    
    // Verify custom plant Kc retrieval
    res = pack_storage_get_kc(9999, 25, &kc);  // Development stage
    assert(res == PACK_RESULT_SUCCESS);
    
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
