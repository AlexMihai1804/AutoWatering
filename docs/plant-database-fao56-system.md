# Plant Database and FAO-56 Irrigation Engine

## Overview

AutoWatering pairs a generated agronomic database with a FAO-56 compliant calculation engine to drive every automatic watering cycle. The firmware stores the entire dataset in flash (`src/plant_full_db.*`, `src/soil_enhanced_db.*`, `src/irrigation_methods_db.*`) and uses the functions in `src/fao56_calc.c` to turn sensor readings and configuration data into water-balance decisions.

## Generated Databases

- **Plant coverage**: `PLANT_FULL_SPECIES_COUNT` is 223 species (`src/plant_full_db.inc`). Records are emitted by `tools/build_database.py` from the curated CSV set (`tools/plants_full.csv`).
- **Soil library**: 15 soil types (`SOIL_ENHANCED_TYPES_COUNT`) with field capacity, wilting point, and infiltration data (`src/soil_enhanced_db.inc`).
- **Irrigation methods**: 15 delivery profiles with efficiency, wetting fraction, and application envelope (`src/irrigation_methods_db.inc`).
- **Categories**: Ten channel categories (`CATEGORY_AGRICULTURE` ... `CATEGORY_SHRUB`) used for filtering and UI grouping; older narrative labels are intentionally absent from firmware logic.
- **Distribution**: The `.inc` headers hold the packed structs and macros; the `.c` companions contain the generated constant arrays and stay under linker control sections defined in `Kconfig`.

### Plant Record Layout

`plant_full_data_t` is a 44-byte packed structure that keeps fixed-point FAO-56 inputs:

```c
typedef struct {
    const char *subtype_enum;      // Symbolic enum string
    const char *common_name_en;    // Pointer to shared flash string
    const char *scientific_name;   // Pointer to shared flash string
  uint16_t kc_ini_x1000;         // Crop coefficient (scaled x1000)
    uint16_t kc_mid_x1000;
    uint16_t kc_end_x1000;
    uint16_t kc_dev_x1000;
    uint16_t root_depth_min_m_x1000;
    uint16_t root_depth_max_m_x1000;
    uint16_t stage_days_mid;
    uint16_t depletion_fraction_p_x1000;
    uint16_t spacing_row_m_x1000;
    uint16_t spacing_plant_m_x1000;
    uint16_t default_density_plants_m2_x100;
    uint16_t canopy_cover_max_frac_x1000;
    uint8_t stage_days_ini;
    uint8_t stage_days_dev;
    uint8_t stage_days_end;
    int8_t  frost_tolerance_c;
    uint8_t temp_opt_min_c;
    uint8_t temp_opt_max_c;
    uint8_t growth_cycle;
    uint8_t typ_irrig_method_id;
} plant_full_data_t;
```

Helper macros (`PLANT_KC_MID`, `PLANT_ROOT_MIN_M`, `PLANT_DEPL_FRACTION`, etc.) convert the scaled integers to floats at call sites. Accessors live in `plant_db.h` / `plant_db_api.c` and are the supported route for consumers.

## Environmental Inputs

`env_sensors.c` produces `environmental_data_t`, containing mean/min/max temperature, relative humidity, pressure, rainfall totals, and derived vapour pressures. The sensor manager updates derived fields via `env_sensors_calculate_derived()`, while `env_sensors_generate_fallback()` supplies seasonal defaults when hardware data gaps occur. Rainfall is collected through the rain task (`rain_integration.c`) and stored as `rain_mm_24h` for the FAO-56 layer.

## Calculation Flow (`src/fao56_calc.c`)

### Reference Evapotranspiration

- **Primary path**: `calc_et0_penman_monteith()` implements the FAO-56 Penman-Monteith equation and expects valid humidity, pressure, latitude (radians), and day-of-year. It accounts for the assumptions codified at the top of the file (`ASSUMED_WIND_SPEED_M_S`, `ASSUMED_SUNSHINE_RATIO`, etc.).
- **Fallbacks**: `calc_et0_hargreaves_samani()` is used when radiation data is absent but diurnal temperature swing exists. When only mean temperature and humidity survive validation, `calc_water_balance()` falls back to the heuristic ET0 band defined by `HEURISTIC_ET0_*` constants to maintain continuity.
- **Caching**: `fao56_cache_get_et0()` / `fao56_cache_store_et0()` reuse ET0 results per channel when sensor inputs stay within tolerance. Cache invalidation is driven by explicit change flags and age windows (`CACHE_MAX_AGE_SECONDS`).

### Crop Coefficient & Phenology

- `calc_phenological_stage()` maps days-after-planting to one of the four FAO growth stages using the generated stage duration fields.
- `calc_crop_coefficient()` interpolates between the packed coefficients (`kc_ini`, `kc_dev`, `kc_mid`, `kc_end`) and clamps results to plausible agronomic bounds before handing the value to volume planners.
- Cache helpers mirror the ET0 flow (`fao56_cache_get_crop_coeff()` / `store`), decoupling slow-changing crop data from per-cycle execution.

### Soil Water Balance

- `calc_water_balance()` is the entry point used by `watering.c` and BLE handlers. It:
  - Computes available water capacity from the soil profile (`soil->awc_mm_per_m`) and current root depth (`calc_current_root_depth()` for channel state).
  - Accounts for irrigation method wetting fractions to produce `wetting_awc_mm`.
  - Applies the plant depletion fraction to derive `raw_mm`.
  - Converts rainfall to effective precipitation via `calc_effective_precipitation()`.
  - Calls `track_deficit_accumulation()` (internal static helper) to update `water_balance_t` using ETc (ET0 x Kc), rainfall, and any manual irrigation feedback.
  - Flags `irrigation_needed` when the current deficit crosses `raw_mm`.

#### Antecedent Soil Moisture (Effective Rainfall)

The FAO-56 engine models **effective precipitation** by estimating runoff and infiltration. One of the key drivers is the **antecedent soil moisture estimate** (how wet the soil was before the rain event):
- Higher antecedent moisture -> higher runoff -> lower effective rainfall
- Lower antecedent moisture -> lower runoff -> higher effective rainfall

AutoWatering supports configuring this estimate as a percentage (`0..100`) via a **global** value and optional **per-channel** overrides.

**Precedence (per channel):**
1. If per-channel override is enabled -> use the channel's `moisture_pct`
2. Else if global override is enabled -> use the global `moisture_pct`
3. Else -> default to `50%`

This value is applied inside `calc_effective_precipitation()` / AUTO mode's daily deficit update, and can change whether a channel decides to irrigate (because effective rain reduces the computed deficit).

**Persistence:** stored in NVS (global record + per-channel records).

**BLE configuration:** exposed via the Custom Configuration Service characteristic `12345678-1234-5678-9abc-def123456784` (see `docs/ble-api/characteristics/31-soil-moisture-configuration.md`).

The persisted water balance structure is defined in `src/water_balance_types.h`:

```c
typedef struct water_balance_t {
    float rwz_awc_mm;
    float wetting_awc_mm;
    float raw_mm;
    float current_deficit_mm;
    float effective_rain_mm;
    bool irrigation_needed;
    uint32_t last_update_time;
} water_balance_t;
```

NVS serialization uses `water_balance_config_t` in `src/nvs_config.h`, ensuring the balance survives resets.

### Irrigation Volume & Modes

- `calc_irrigation_volume_area()` and `calc_irrigation_volume_plants()` convert deficits to liters, honouring irrigation efficiency, distribution uniformity, wetting fraction corrections, and per-channel maximum volume limits.
- `apply_quality_irrigation_mode()` and `apply_eco_irrigation_mode()` encapsulate the 100% vs 70% application logic. Eco mode multiplies calculated volumes by 0.70 and logs the reduction so the task framework can surface the decision.
- `calc_cycle_and_soak()` computes cycle/soak parameters for slow-draining soils; current scheduling still enqueues a single volume task (cycle counts are not executed).
- `apply_volume_limiting()` performs the last safeguard before a schedule is accepted, clamping to user-defined maxima and emitting warnings through `watering_log`.

### Rainfall Integration & Triggering

- `calc_effective_precipitation()` filters raw rainfall through soil infiltration characteristics and irrigation method runoff behaviour.
- `integrate_rainfall_with_irrigation()` can reduce or cancel a scheduled watering if recent effective rainfall already satisfied the deficit.
- `check_irrigation_trigger_mad()` compares `current_deficit_mm` against the plant-specific management allowed depletion (MAD) threshold, optionally adjusted by `apply_environmental_stress_adjustment()` when high-temperature or low-humidity stressors are detected.
- `calc_irrigation_timing()` estimates the hours remaining until MAD is exceeded, which drives proactive scheduling in `watering_tasks.c`.

## Persistence, Tasks, and BLE Consumers

- Channel configuration structs reference the same indices used by the databases, so changes to `plant_full_data_t` fields must stay in sync with BLE payload packers (`src/bt_custom_soil_handlers.c`, `src/bt_environmental_history_handlers.c`).
- `watering_tasks.c` invokes the FAO-56 helpers within queue consumers; the implementation deliberately avoids long blocking sections to keep message latency predictable.
- BLE requests for preview calculations (`bt_irrigation_service.c`) call the same `apply_quality_irrigation_mode()` / `apply_eco_irrigation_mode()` functions to guarantee parity with autonomous operation.

## Maintenance Workflow

1. Update the CSV sources under `tools/` (plants, soils, methods).
2. Run `python tools\build_database.py` (PowerShell) to regenerate the `.inc/.c` assets and JSON mirror files for companion apps.
3. Commit the regenerated artifacts together with any documentation or firmware changes. Manual edits to generated `.inc` files are not supported.

## Key Files

- `src/fao56_calc.c` and `src/fao56_calc.h` - FAO-56 algorithms, caching, and volume planners.
- `src/plant_full_db.inc` / `.c` - plant dataset definitions and string tables.
- `src/soil_enhanced_db.inc` / `.c` - soil hydraulic profiles.
- `src/irrigation_methods_db.inc` / `.c` - irrigation method efficiency and flow characteristics.
- `src/water_balance_types.h` - runtime balance structure shared with NVS and watering tasks.
- `tools/build_database.py` - orchestrates regeneration from CSV inputs.

## Usage in Firmware and BLE

- Channels store only the plant/soil/method indices; all FAO-56 calculations pull the full structs from the compiled databases at runtime.
- BLE characteristics `14-growing-environment` and `15-auto-calc-status` expose the exact indices and the calculation outputs so clients stay in sync with firmware decisions.
- Time-related values are handled in UTC; timezone/DST settings live in `timezone_config_t` and are persisted in NVS together with other system flags.
- The `watering_tasks` queue calls FAO-56 helpers for both autonomous and on-demand (BLE-triggered) calculations, guaranteeing identical results.
- Regenerated databases must ship with their accompanying fingerprints to keep size/layout guards valid at build time.

## FAO-56 Workflow (developer cheatsheet)

1. Collect inputs: latest environmental snapshot (with fallbacks), rain history, channel config (coverage, indices, eco/quality mode, max volume), and planting date.
2. Compute ET0 (Penman-Monteith -> Hargreaves-Samani fallback -> heuristic band if needed) with caching keyed on stable inputs.
3. Map days-after-planting to growth stage; interpolate crop coefficients and root depth from the plant database.
4. Compute available water capacity using soil profile + irrigation method wetting fraction; derive RAW/MAD thresholds.
5. Convert recent rainfall to effective precipitation; update water balance and decide if irrigation is required.
6. Convert deficits to litres (area or plant-count coverage), apply eco/quality reductions, enforce max-volume limits; cycle/soak parameters are computed but not executed by the scheduler yet.
7. Persist water balance updates; publish results through BLE (`auto-calc-status`, `current-task-status`) and enqueue tasks when thresholds are crossed.

## Full Species List (223 entries)

- Wheat (Triticum aestivum)
- Barley (Hordeum vulgare)
- Maize (Corn) (Zea mays)
- Rice (Oryza sativa)
- Oat (Avena sativa)
- Sorghum (Sorghum bicolor)
- Millet (Pennisetum glaucum)
- Soybean (Glycine max)
- Peanut (Arachis hypogaea)
- Sunflower (Helianthus annuus)
- Rapeseed (Canola) (Brassica napus)
- Cotton (Gossypium hirsutum)
- Sugarcane (Saccharum officinarum)
- Sugar beet (Beta vulgaris)
- Flax (Linum usitatissimum)
- Safflower (Carthamus tinctorius)
- Sesame (Sesamum indicum)
- Castorbean (Ricinus communis)
- Tobacco (Nicotiana tabacum)
- Alfalfa (Medicago sativa)
- Tomato (Solanum lycopersicum)
- Bell Pepper (Capsicum annuum)
- Eggplant (Solanum melongena)
- Cucumber (Cucumis sativus)
- Zucchini (Cucurbita pepo)
- Pumpkin (Cucurbita maxima)
- Watermelon (Citrullus lanatus)
- Cantaloupe (Cucumis melo)
- Cabbage (Brassica oleracea var. capitata)
- Cauliflower (Brassica oleracea var. botrytis)
- Broccoli (Brassica oleracea var. italica)
- Lettuce (Lactuca sativa)
- Spinach (Spinacia oleracea)
- Onion (dry bulb) (Allium cepa)
- Garlic (Allium sativum)
- Carrot (Daucus carota)
- Radish (Raphanus sativus)
- Celery (Apium graveolens)
- Potato (Solanum tuberosum)
- Sweet Potato (Ipomoea batatas)
- Cassava (Manihot esculenta)
- Turnip (Brassica rapa subsp. rapa)
- Parsnip (Pastinaca sativa)
- Table Beet (Beta vulgaris, table)
- Green Bean (Phaseolus vulgaris, green)
- Dry Bean (Phaseolus vulgaris, dry)
- Pea (Pisum sativum)
- Chickpea (Cicer arietinum)
- Lentil (Lens culinaris)
- Strawberry (Fragaria x ananassa)
- Artichoke (Cynara scolymus)
- Asparagus (Asparagus officinalis)
- Brussels Sprouts (Brassica oleracea var. gemmifera)
- Kale (Brassica oleracea var. sabellica)
- Sweet Corn (Zea mays var. saccharata)
- Basil (Ocimum basilicum)
- Parsley (Petroselinum crispum)
- Dill (Anethum graveolens)
- Mint (Mentha spicata)
- Rosemary (Salvia rosmarinus)
- Thyme (Thymus vulgaris)
- Sage (Salvia officinalis)
- Oregano (Origanum vulgare)
- Chives (Allium schoenoprasum)
- Tarragon (Artemisia dracunculus)
- Oak (Quercus robur)
- Maple (Acer platanoides)
- Birch (Betula pendula)
- Ash (Fraxinus excelsior)
- Elm (Ulmus glabra)
- Pine (Pinus sylvestris)
- Spruce (Picea abies)
- Cedar (Cedrus atlantica)
- Cypress (Cupressus sempervirens)
- Willow (Salix alba)
- Poplar (Populus nigra)
- Magnolia (Magnolia grandiflora)
- Palm (ornamental) (Phoenix canariensis)
- Yew (Taxus baccata)
- Juniper (Juniperus communis)
- Arborvitae (Thuja occidentalis)
- Holly (Ilex aquifolium)
- Azalea (Rhododendron spp.)
- Rhododendron (Rhododendron catawbiense)
- Rose (Rosa sp.)
- Hydrangea (Hydrangea macrophylla)
- Bougainvillea (Bougainvillea spectabilis)
- Hibiscus (Hibiscus syriacus)
- Oleander (Nerium oleander)
- Jasmine (Jasminum officinale)
- Ivy (Hedera helix)
- Wisteria (Wisteria sinensis)
- Tulip (Tulipa gesneriana)
- Daffodil (Narcissus pseudonarcissus)
- Marigold (Tagetes patula)
- Petunia (Petunia hybrid)
- Geranium (Pelargonium hortorum)
- Pansy (Viola tricolor)
- Impatiens (Impatiens walleriana)
- Chrysanthemum (Chrysanthemum morifolium)
- Shasta Daisy (Leucanthemum x superbum)
- Lily (Lilium candidum)
- Iris (Iris germanica)
- Dahlia (Dahlia pinnata)
- Peony (Paeonia officinalis)
- Monstera (Monstera deliciosa)
- Spider Plant (Chlorophytum comosum)
- Peace Lily (Spathiphyllum wallisii)
- Snake Plant (Dracaena trifasciata)
- Pothos (Epipremnum aureum)
- Dracaena (Dracaena fragrans)
- Fiddle Leaf Fig (Ficus lyrata)
- Rubber Plant (Ficus elastica)
- ZZ Plant (Zamioculcas zamiifolia)
- Areca Palm (Dypsis lutescens)
- Croton (Codiaeum variegatum)
- Dieffenbachia (Dieffenbachia seguine)
- African Violet (Saintpaulia ionantha)
- Boston Fern (Nephrolepis exaltata)
- Aloe Vera (Aloe vera)
- Agave (Agave americana)
- Jade Plant (Crassula ovata)
- Sedum (Sedum spurium)
- Sempervivum (Sempervivum tectorum)
- Prickly Pear Cactus (Opuntia ficus-indica)
- Crown of Thorns (Euphorbia milii)
- Kalanchoe (Kalanchoe blossfeldiana)
- Yucca (Yucca filamentosa)
- Barrel Cactus (Echinocactus grusonii)
- Apple (Malus domestica)
- Pear (Pyrus communis)
- Sweet Cherry (Prunus avium)
- Sour Cherry (Prunus cerasus)
- Plum (Prunus domestica)
- Peach (Prunus persica)
- Apricot (Prunus armeniaca)
- Quince (Cydonia oblonga)
- Fig (Ficus carica)
- Lemon (Citrus limon)
- Orange (Citrus sinensis)
- Mandarin (Citrus reticulata)
- Grapefruit (Citrus paradisi)
- Lime (Citrus aurantiifolia)
- Pomegranate (Punica granatum)
- Persimmon (Diospyros kaki)
- Olive (Olea europaea)
- Almond (Prunus dulcis)
- Walnut (Juglans regia)
- Hazelnut (Corylus avellana)
- Chestnut (Castanea sativa)
- Grape (Vitis vinifera)
- Kiwi (Actinidia deliciosa)
- Mulberry (Morus alba)
- Pecan (Carya illinoinensis)
- Blueberry (Vaccinium corymbosum)
- Raspberry (Rubus idaeus)
- Blackberry (Rubus fruticosus)
- Strawberry (perennial) (Fragaria x ananassa)
- Leek (Allium ampeloprasum)
- Fennel (Foeniculum vulgare)
- Okra (Abelmoschus esculentus)
- Horseradish (Armoracia rusticana)
- Jerusalem Artichoke (Helianthus tuberosus)
- Rhubarb (Rheum rhabarbarum)
- Endive (Cichorium endivia)
- Radicchio (Cichorium intybus var.)
- Bok Choy (Brassica rapa subsp. chinensis)
- Arugula (Eruca sativa)
- Swiss Chard (Beta vulgaris var. cicla)
- Okra (red) (Abelmoschus spp.)
- Lemon Balm (Melissa officinalis)
- Lemon Verbena (Aloysia citriodora)
- Stevia (Stevia rebaudiana)
- Echinacea (Echinacea purpurea)
- Chamomile (Matricaria chamomilla)
- Lovage (Levisticum officinale)
- Catnip (Nepeta cataria)
- Bermuda Grass (Cynodon dactylon)
- Zoysia (Zoysia japonica)
- St Augustine Grass (Stenotaphrum secundatum)
- Kentucky Bluegrass (Poa pratensis)
- Tall Fescue (Festuca arundinacea)
- Fine Fescue (Festuca rubra)
- Perennial Ryegrass (Lolium perenne)
- Annual Ryegrass (Lolium multiflorum)
- Buffalo Grass (Bouteloua dactyloides)
- Centipede Grass (Eremochloa ophiuroides)
- Bahia Grass (Paspalum notatum)
- White Clover (Trifolium repens)
- Creeping Thyme (Thymus serpyllum)
- Vinca (Vinca minor)
- Ornamental Grass (Miscanthus sinensis)
- Coral Bells (Heuchera spp.)
- Garden Fern (Dryopteris filix-mas)
- Boxwood (Buxus sempervirens)
- Privet (Ligustrum vulgare)
- Cherry Laurel (Prunus laurocerasus)
- Rosehip (Rosa canina)
- Elderberry (Sambucus nigra)
- Sea Buckthorn (Hippophae rhamnoides)
- Chokeberry (Aronia melanocarpa)
- Firethorn (Pyracantha coccinea)
- Barberry (Berberis thunbergii)
- Currant (Ribes rubrum)
- Gooseberry (Ribes uva-crispa)
- Blackcurrant (Ribes nigrum)
- Raspberry (bed) (Rubus idaeus)
- Calathea (Calathea roseopicta)
- Orchid (Phalaenopsis sp.)
- Yucca (indoor) (Yucca gigantea)
- Aglaonema (Aglaonema commutatum)
- Bromeliad (Guzmania lingulata)
- Tradescantia (Tradescantia zebrina)
- Money Tree (Pachira aquatica)
- Schefflera (Schefflera arboricola)
- Hoya (Hoya carnosa)
- Air Plant (Tillandsia ionantha)
- Echeveria (Echeveria elegans)
- Haworthia (Haworthia fasciata)
- Lithops (Lithops spp.)
- Ponytail Palm (Beaucarnea recurvata)
- Wax Plant (Hoya carnosa, succ.)
- Air Plant (Tillandsia spp.)

## Summary

- **Fixed-point agronomy data** shipped with the firmware underpins every irrigation decision.
- **FAO-56 compliant calculations** (Penman-Monteith with fallbacks) feed the water balance cache per channel.
- **Dynamic crop coefficients and root depth models** convert planting dates into accurate Kc and AWC values.
- **Rainfall and eco-mode integration** ensure the system honours precipitation and conservation settings without diverging from the underlying science.
- **Regeneration tooling** keeps the plant, soil, and irrigation databases auditable and reproducible from CSV sources.
