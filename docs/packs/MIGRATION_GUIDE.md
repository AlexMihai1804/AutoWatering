# Mobile App Migration Guide

**Version**: 1.0.0  
**Date**: January 2026

## Overview

This guide helps mobile app developers migrate from the legacy CSV-based plant database to the new unified Pack Storage system.

---

## What Changed

### Before (Legacy System)

```
┌─────────────────────────────────────────────────────────────┐
│  App Bundle                                                  │
│  └── plants.csv (223 species)                               │
│      └── Parsed at runtime                                   │
│      └── No versioning                                       │
│      └── Must update app to add plants                       │
└─────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────┐
│  Device                                                      │
│  ├── ROM Database (223 plants, read-only)                   │
│  └── Custom plants (separate struct in channel config)      │
│      └── Stored per-channel                                  │
│      └── Limited fields                                      │
└─────────────────────────────────────────────────────────────┘
```

### After (Pack Storage System)

```
┌─────────────────────────────────────────────────────────────┐
│  App Bundle                                                  │
│  └── plants.json (223 species, converted from CSV)          │
│      └── Same data, JSON format                              │
│      └── Used as local cache                                 │
│      └── Never needs to be fetched from device               │
└─────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────┐
│  Device                                                      │
│  ├── ROM Database (223 plants, pack_id=0)                   │
│  ├── Pack Storage (external flash, LittleFS)                │
│  │   ├── Custom plants (full FAO-56 data)                   │
│  │   ├── User packs                                          │
│  │   └── change_counter (for cache invalidation)            │
│  └── Unified plant_id across all sources                     │
└─────────────────────────────────────────────────────────────┘
```

---

## Migration Steps

### Step 1: Convert CSV to JSON

Your existing `plants.csv` should be converted to JSON and bundled with the app:

**Before (plants.csv):**
```csv
id,common_name,scientific_name,kc_ini,kc_mid,kc_end,root_depth_mm,depletion_factor
1,Tomato,Solanum lycopersicum,0.60,1.15,0.80,1000,50
2,Pepper,Capsicum annuum,0.60,1.05,0.90,700,55
...
```

**After (plants.json):**
```json
{
  "version": 1,
  "generated": "2026-01-11",
  "source": "AutoWatering ROM Database v1.0",
  "plants": [
    {
      "id": 1,
      "commonName": "Tomato",
      "scientificName": "Solanum lycopersicum",
      "kcIni": 0.60,
      "kcMid": 1.15,
      "kcEnd": 0.80,
      "rootDepthMm": 1000,
      "depletionFactor": 50,
      "packId": 0
    },
    {
      "id": 2,
      "commonName": "Pepper",
      "scientificName": "Capsicum annuum",
      "kcIni": 0.60,
      "kcMid": 1.05,
      "kcEnd": 0.90,
      "rootDepthMm": 700,
      "depletionFactor": 55,
      "packId": 0
    }
  ]
}
```

**Python conversion script:**
```python
import csv
import json
from datetime import date

def convert_plants_csv_to_json(csv_path, json_path):
    plants = []
    
    with open(csv_path, 'r', encoding='utf-8') as f:
        reader = csv.DictReader(f)
        for row in reader:
            plant = {
                "id": int(row['id']),
                "commonName": row['common_name'],
                "scientificName": row.get('scientific_name', ''),
                "kcIni": float(row['kc_ini']),
                "kcMid": float(row['kc_mid']),
                "kcEnd": float(row['kc_end']),
                "rootDepthMm": int(row['root_depth_mm']),
                "depletionFactor": int(row['depletion_factor']),
                "packId": 0  # Built-in plants
            }
            plants.append(plant)
    
    output = {
        "version": 1,
        "generated": str(date.today()),
        "source": "AutoWatering ROM Database v1.0",
        "plants": plants
    }
    
    with open(json_path, 'w', encoding='utf-8') as f:
        json.dump(output, f, indent=2)
    
    print(f"Converted {len(plants)} plants to {json_path}")

# Usage
convert_plants_csv_to_json('plants.csv', 'plants.json')
```

### Step 2: Bundle JSON with App

**iOS (Swift):**
```swift
class BuiltInPlantDatabase {
    static let shared = BuiltInPlantDatabase()
    
    private(set) var plants: [Plant] = []
    
    private init() {
        loadFromBundle()
    }
    
    private func loadFromBundle() {
        guard let url = Bundle.main.url(forResource: "plants", withExtension: "json"),
              let data = try? Data(contentsOf: url),
              let json = try? JSONDecoder().decode(PlantDatabase.self, from: data) else {
            fatalError("Failed to load plants.json from bundle")
        }
        
        plants = json.plants
        print("Loaded \(plants.count) built-in plants")
    }
}

struct PlantDatabase: Codable {
    let version: Int
    let generated: String
    let source: String
    let plants: [Plant]
}

struct Plant: Codable, Identifiable {
    let id: Int
    let commonName: String
    let scientificName: String
    let kcIni: Double
    let kcMid: Double
    let kcEnd: Double
    let rootDepthMm: Int
    let depletionFactor: Int
    let packId: Int
    
    var isBuiltIn: Bool { packId == 0 }
}
```

**Android (Kotlin):**
```kotlin
class BuiltInPlantDatabase private constructor(context: Context) {
    
    val plants: List<Plant>
    
    init {
        val json = context.assets.open("plants.json").bufferedReader().use { it.readText() }
        val database = Gson().fromJson(json, PlantDatabase::class.java)
        plants = database.plants
        Log.d("Plants", "Loaded ${plants.size} built-in plants")
    }
    
    companion object {
        @Volatile
        private var instance: BuiltInPlantDatabase? = null
        
        fun getInstance(context: Context): BuiltInPlantDatabase {
            return instance ?: synchronized(this) {
                instance ?: BuiltInPlantDatabase(context.applicationContext).also { instance = it }
            }
        }
    }
}

data class PlantDatabase(
    val version: Int,
    val generated: String,
    val source: String,
    val plants: List<Plant>
)

data class Plant(
    val id: Int,
    val commonName: String,
    val scientificName: String,
    val kcIni: Double,
    val kcMid: Double,
    val kcEnd: Double,
    val rootDepthMm: Int,
    val depletionFactor: Int,
    val packId: Int
) {
    val isBuiltIn: Boolean get() = packId == 0
}
```

### Step 3: Add Cache Support for Custom Plants

**New PlantManager with caching:**

```swift
class PlantManager {
    static let shared = PlantManager()
    
    // Built-in plants (from bundle)
    private let builtInPlants = BuiltInPlantDatabase.shared.plants
    
    // Custom plants (from device, cached)
    private var customPlants: [PlantListEntry] = []
    private var cachedChangeCounter: UInt32 = 0
    
    // Combined list for UI
    var allPlants: [Plant] {
        return builtInPlants + customPlants.map { $0.asPlant() }
    }
    
    init() {
        loadCache()
    }
    
    // MARK: - Cache Management
    
    func syncWithDevice(stats: PackStats, 
                        bleManager: BLEManager,
                        completion: @escaping () -> Void) {
        
        // Check if cache is still valid
        if stats.changeCounter == cachedChangeCounter && 
           stats.plantCount == UInt16(customPlants.count) {
            print("✅ Cache valid (counter=\(cachedChangeCounter))")
            completion()
            return
        }
        
        print("🔄 Cache invalid (cached=\(cachedChangeCounter), device=\(stats.changeCounter))")
        
        // Fetch custom plants from device
        bleManager.listCustomPlants { [weak self] entries in
            self?.customPlants = entries
            self?.cachedChangeCounter = stats.changeCounter
            self?.saveCache()
            completion()
        }
    }
    
    private func saveCache() {
        let encoder = JSONEncoder()
        if let data = try? encoder.encode(customPlants) {
            UserDefaults.standard.set(data, forKey: "customPlantsCache")
        }
        UserDefaults.standard.set(Int(cachedChangeCounter), forKey: "plantCacheCounter")
    }
    
    private func loadCache() {
        cachedChangeCounter = UInt32(UserDefaults.standard.integer(forKey: "plantCacheCounter"))
        if let data = UserDefaults.standard.data(forKey: "customPlantsCache"),
           let plants = try? JSONDecoder().decode([PlantListEntry].self, from: data) {
            customPlants = plants
        }
    }
}
```

### Step 4: Update Connection Flow

**Old flow:**
```swift
func onConnect() {
    // Nothing to sync - plants were parsed from bundled CSV
    showPlantPicker(plants: parsedCSVPlants)
}
```

**New flow:**
```swift
func onConnect() {
    // 1. Read Pack Stats (50ms)
    bleManager.readPackStats { stats in
        // 2. Sync with cache (0ms if valid, 100-400ms if invalid)
        PlantManager.shared.syncWithDevice(stats: stats, 
                                            bleManager: bleManager) {
            // 3. Show combined list
            showPlantPicker(plants: PlantManager.shared.allPlants)
        }
    }
}
```

---

## Key Differences Summary

| Aspect | Legacy (CSV) | New (Pack Storage) |
|--------|--------------|-------------------|
| **Built-in plants** | Parsed from CSV at runtime | Bundled as JSON (same data) |
| **Custom plants** | Per-channel struct, limited fields | Full FAO-56 data in flash |
| **Plant ID range** | 1-223 default, >=1000 custom | Unified: 0=not set, 1+=valid |
| **Sync on connect** | None needed | Quick check via changeCounter |
| **Add new plants** | Update app + CSV | BLE transfer, no app update |
| **Storage location** | App bundle only | App bundle + device flash |
| **Cache invalidation** | N/A | changeCounter in Pack Stats |

---

## What to Do With Your CSV

1. **Convert it once** to JSON using the script above
2. **Bundle the JSON** with your app (iOS: Copy Bundle Resources, Android: assets/)
3. **Delete the CSV** from your project - it's no longer needed
4. **Never fetch built-in plants from device** - they're already in your app

---

## Testing Checklist

- [ ] JSON loads correctly from bundle
- [ ] 223 built-in plants appear in picker
- [ ] Pack Stats read returns valid changeCounter
- [ ] Cache invalidation works (install plant → counter changes → refresh)
- [ ] Custom plants appear after refresh
- [ ] App survives device reboot (counter persisted)
- [ ] UI shows combined list (built-in + custom)

---

## Common Migration Issues

### Issue: "Plants not loading"
**Cause:** JSON file not in bundle or wrong filename  
**Fix:** Verify `plants.json` is in Copy Bundle Resources (iOS) or assets (Android)

### Issue: "changeCounter always 0"
**Cause:** Older firmware without persistence  
**Fix:** Update firmware to version with persistent counter

### Issue: "Custom plants not syncing"
**Cause:** Cache validation too strict  
**Fix:** Compare both changeCounter AND plantCount

### Issue: "Duplicate plants in picker"
**Cause:** Built-in plants fetched from device  
**Fix:** Filter by packId - only fetch packId != 0 from device

---

## Firmware Requirements

- Firmware version: **3.2.0+** (with Pack Storage)
- Pack Service UUID: `12345678-1234-5678-9abc-def123456800`
- Required characteristics:
  - Pack Stats (`...456787`) - for changeCounter
  - Pack Plant (`...456786`) - for custom plant list

---

## Questions?

See also:
- [MOBILE_APP_GUIDE.md](MOBILE_APP_GUIDE.md) - Full implementation guide
- [BLE_PACK_SERVICE.md](BLE_PACK_SERVICE.md) - BLE protocol details
- [PACK_STORAGE.md](PACK_STORAGE.md) - Storage implementation
