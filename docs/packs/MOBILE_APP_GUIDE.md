# Mobile App Implementation Guide

**Version**: 1.0.0

## Overview

This guide provides implementation details for mobile app developers integrating with the AutoWatering Pack system. It covers BLE communication, data serialization, and recommended UX patterns.

---

## Prerequisites

- BLE 4.2+ support
- MTU negotiation (247+ recommended)
- Bonding/pairing support (required for encrypted characteristics)

---

## Service Discovery

### UUIDs

```swift
// Service
let PACK_SERVICE_UUID = CBUUID(string: "12345678-1234-5678-9abc-def123456800")

// Characteristics
let PACK_PLANT_UUID = CBUUID(string: "12345678-1234-5678-9abc-def123456786")
let PACK_STATS_UUID = CBUUID(string: "12345678-1234-5678-9abc-def123456787")
let PACK_XFER_UUID  = CBUUID(string: "12345678-1234-5678-9abc-def123456788")
```

### Discovery Flow

```swift
func centralManager(_ central: CBCentralManager, 
                    didConnect peripheral: CBPeripheral) {
    // 1. Discover Pack Service
    peripheral.discoverServices([PACK_SERVICE_UUID])
}

func peripheral(_ peripheral: CBPeripheral, 
                didDiscoverServices error: Error?) {
    guard let service = peripheral.services?.first(where: { 
        $0.uuid == PACK_SERVICE_UUID 
    }) else { return }
    
    // 2. Discover characteristics
    peripheral.discoverCharacteristics([
        PACK_PLANT_UUID,
        PACK_STATS_UUID,
        PACK_XFER_UUID
    ], for: service)
}
```

---

## Data Structures

### Plant Structure (156 bytes)

Matches `pack_plant_v1_t` from `src/pack_schema.h`:

```swift
struct PackPlant: Codable {
    // Identification (8 bytes)
    var plantId: UInt16         // Unique plant ID (1..65534)
    var packId: UInt16          // Owning pack ID (0=standalone, 1+=from pack)
    var version: UInt16         // Plant data version for updates
    var reserved: UInt16        // Reserved for alignment
    
    // Names (112 bytes)
    var commonName: String      // 48 bytes, null-terminated
    var scientificName: String  // 64 bytes, null-terminated
    
    // Crop coefficients ×1000 (8 bytes)
    var kcIniX1000: UInt16      // Kc initial stage
    var kcDevX1000: UInt16      // Kc development stage
    var kcMidX1000: UInt16      // Kc mid-season stage
    var kcEndX1000: UInt16      // Kc end season stage
    
    // Root depth in mm (4 bytes)
    var rootDepthMinMm: UInt16  // Minimum root depth
    var rootDepthMaxMm: UInt16  // Maximum root depth
    
    // Growth stages in days (6 bytes)
    var stageDaysIni: UInt8     // Initial stage duration
    var stageDaysDev: UInt8     // Development stage duration
    var stageDaysMid: UInt16    // Mid-season stage duration
    var stageDaysEnd: UInt8     // End season stage duration
    var growthCycle: UInt8      // Growth cycle type
    
    // Depletion and spacing (10 bytes)
    var depletionFractionPX1000: UInt16  // Allowable depletion fraction ×1000
    var spacingRowMm: UInt16    // Row spacing in mm
    var spacingPlantMm: UInt16  // Plant spacing in mm
    var densityX100: UInt16     // Default density plants/m² ×100
    var canopyMaxX1000: UInt16  // Max canopy cover fraction ×1000
    
    // Temperature (3 bytes)
    var frostToleranceC: Int8   // Frost tolerance temperature °C
    var tempOptMinC: UInt8      // Optimal minimum temperature °C
    var tempOptMaxC: UInt8      // Optimal maximum temperature °C
    
    // Irrigation (1 byte)
    var typIrrigMethodId: UInt8 // Typical irrigation method ID
    
    // User-adjustable parameters (4 bytes)
    var waterNeedFactorX100: UInt16  // Water need multiplier (10-500 = 0.1-5.0, default 100)
    var irrigationFreqDays: UInt8    // Recommended irrigation frequency in days (1-30, default 3)
    var preferAreaBased: UInt8       // 1 = area-based (m²), 0 = plant count based
}
```

### Serialization (Swift)

```swift
extension PackPlant {
    func serialize() -> Data {
        var data = Data(capacity: 156)
        
        // Identification (8 bytes)
        data.append(contentsOf: withUnsafeBytes(of: plantId.littleEndian) { Array($0) })
        data.append(contentsOf: withUnsafeBytes(of: packId.littleEndian) { Array($0) })
        data.append(contentsOf: withUnsafeBytes(of: version.littleEndian) { Array($0) })
        data.append(contentsOf: withUnsafeBytes(of: reserved.littleEndian) { Array($0) })
        
        // Names (112 bytes)
        data.append(contentsOf: commonName.paddedUTF8(to: 48))
        data.append(contentsOf: scientificName.paddedUTF8(to: 64))
        
        // Kc values ×1000 (8 bytes)
        data.append(contentsOf: withUnsafeBytes(of: kcIniX1000.littleEndian) { Array($0) })
        data.append(contentsOf: withUnsafeBytes(of: kcDevX1000.littleEndian) { Array($0) })
        data.append(contentsOf: withUnsafeBytes(of: kcMidX1000.littleEndian) { Array($0) })
        data.append(contentsOf: withUnsafeBytes(of: kcEndX1000.littleEndian) { Array($0) })
        
        // Root depth (4 bytes)
        data.append(contentsOf: withUnsafeBytes(of: rootDepthMinMm.littleEndian) { Array($0) })
        data.append(contentsOf: withUnsafeBytes(of: rootDepthMaxMm.littleEndian) { Array($0) })
        
        // Growth stages (6 bytes)
        data.append(stageDaysIni)
        data.append(stageDaysDev)
        data.append(contentsOf: withUnsafeBytes(of: stageDaysMid.littleEndian) { Array($0) })
        data.append(stageDaysEnd)
        data.append(growthCycle)
        
        // Depletion and spacing (10 bytes)
        data.append(contentsOf: withUnsafeBytes(of: depletionFractionPX1000.littleEndian) { Array($0) })
        data.append(contentsOf: withUnsafeBytes(of: spacingRowMm.littleEndian) { Array($0) })
        data.append(contentsOf: withUnsafeBytes(of: spacingPlantMm.littleEndian) { Array($0) })
        data.append(contentsOf: withUnsafeBytes(of: densityX100.littleEndian) { Array($0) })
        data.append(contentsOf: withUnsafeBytes(of: canopyMaxX1000.littleEndian) { Array($0) })
        
        // Temperature (3 bytes)
        data.append(UInt8(bitPattern: frostToleranceC))
        data.append(tempOptMinC)
        data.append(tempOptMaxC)
        
        // Irrigation (1 byte)
        data.append(typIrrigMethodId)
        
        // User-adjustable (4 bytes)
        data.append(contentsOf: withUnsafeBytes(of: waterNeedFactorX100.littleEndian) { Array($0) })
        data.append(irrigationFreqDays)
        data.append(preferAreaBased)
        
        assert(data.count == 156)
        return data
    }
}

extension String {
    func paddedUTF8(to length: Int) -> [UInt8] {
        var bytes = Array(self.utf8.prefix(length - 1))
        bytes.append(0) // Null terminator
        while bytes.count < length {
            bytes.append(0)
        }
        return bytes
    }
}
```

### Serialization (Kotlin)

```kotlin
data class PackPlant(
    // Identification (8 bytes)
    val plantId: UShort,
    val packId: UShort,
    val version: UShort,
    val reserved: UShort,
    
    // Names (112 bytes)
    val commonName: String,      // 48 bytes
    val scientificName: String,  // 64 bytes
    
    // Kc coefficients ×1000 (8 bytes)
    val kcIniX1000: UShort,
    val kcDevX1000: UShort,
    val kcMidX1000: UShort,
    val kcEndX1000: UShort,
    
    // Root depth mm (4 bytes)
    val rootDepthMinMm: UShort,
    val rootDepthMaxMm: UShort,
    
    // Growth stages days (6 bytes)
    val stageDaysIni: UByte,
    val stageDaysDev: UByte,
    val stageDaysMid: UShort,
    val stageDaysEnd: UByte,
    val growthCycle: UByte,
    
    // Depletion and spacing (10 bytes)
    val depletionFractionPX1000: UShort,
    val spacingRowMm: UShort,
    val spacingPlantMm: UShort,
    val densityX100: UShort,
    val canopyMaxX1000: UShort,
    
    // Temperature (3 bytes)
    val frostToleranceC: Byte,
    val tempOptMinC: UByte,
    val tempOptMaxC: UByte,
    
    // Irrigation (1 byte)
    val typIrrigMethodId: UByte,
    
    // User-adjustable (4 bytes)
    val waterNeedFactorX100: UShort,
    val irrigationFreqDays: UByte,
    val preferAreaBased: UByte
) {
    fun serialize(): ByteArray {
        val buffer = ByteBuffer.allocate(156).order(ByteOrder.LITTLE_ENDIAN)
        
        // Identification (8 bytes)
        buffer.putShort(plantId.toShort())
        buffer.putShort(packId.toShort())
        buffer.putShort(version.toShort())
        buffer.putShort(reserved.toShort())
        
        // Names (112 bytes)
        buffer.put(commonName.toFixedBytes(48))
        buffer.put(scientificName.toFixedBytes(64))
        
        // Kc values ×1000 (8 bytes)
        buffer.putShort(kcIniX1000.toShort())
        buffer.putShort(kcDevX1000.toShort())
        buffer.putShort(kcMidX1000.toShort())
        buffer.putShort(kcEndX1000.toShort())
        
        // Root depth (4 bytes)
        buffer.putShort(rootDepthMinMm.toShort())
        buffer.putShort(rootDepthMaxMm.toShort())
        
        // Growth stages (6 bytes)
        buffer.put(stageDaysIni.toByte())
        buffer.put(stageDaysDev.toByte())
        buffer.putShort(stageDaysMid.toShort())
        buffer.put(stageDaysEnd.toByte())
        buffer.put(growthCycle.toByte())
        
        // Depletion and spacing (10 bytes)
        buffer.putShort(depletionFractionPX1000.toShort())
        buffer.putShort(spacingRowMm.toShort())
        buffer.putShort(spacingPlantMm.toShort())
        buffer.putShort(densityX100.toShort())
        buffer.putShort(canopyMaxX1000.toShort())
        
        // Temperature (3 bytes)
        buffer.put(frostToleranceC)
        buffer.put(tempOptMinC.toByte())
        buffer.put(tempOptMaxC.toByte())
        
        // Irrigation (1 byte)
        buffer.put(typIrrigMethodId.toByte())
        
        // User-adjustable (4 bytes)
        buffer.putShort(waterNeedFactorX100.toShort())
        buffer.put(irrigationFreqDays.toByte())
        buffer.put(preferAreaBased.toByte())
        
        return buffer.array()
    }
}

fun String.toFixedBytes(length: Int): ByteArray {
    val bytes = this.toByteArray(Charsets.UTF_8)
    return ByteArray(length) { i -> if (i < bytes.size) bytes[i] else 0 }
}
```

---

## Single Plant Operations

### Install a Plant

```swift
class PackManager {
    var plantCharacteristic: CBCharacteristic?
    var peripheral: CBPeripheral?
    
    func installPlant(_ plant: PackPlant) {
        guard let char = plantCharacteristic,
              let peripheral = peripheral else { return }
        
        // Enable notifications first
        peripheral.setNotifyValue(true, for: char)
        
        // Write plant data (156 bytes)
        let data = plant.serialize()
        peripheral.writeValue(data, for: char, type: .withResponse)
    }
}

// Handle notification response
func peripheral(_ peripheral: CBPeripheral,
                didUpdateValueFor characteristic: CBCharacteristic,
                error: Error?) {
    guard characteristic.uuid == PACK_PLANT_UUID,
          let data = characteristic.value,
          data.count >= 8 else { return }
    
    let operation = data[0]  // 0=install, 1=delete
    let result = data[1]     // pack_result_t
    let plantId = data.subdata(in: 2..<4).withUnsafeBytes { 
        $0.load(as: UInt16.self) 
    }
    
    switch result {
    case 0: print("Success: Plant \(plantId)")
    case 1: print("Updated: Plant \(plantId)")
    case 2: print("Already current: Plant \(plantId)")
    default: print("Error \(result) for plant \(plantId)")
    }
}
```

### List Plants

```swift
func listPlants(offset: UInt16 = 0, maxResults: UInt8 = 8) {
    guard let char = plantCharacteristic,
          let peripheral = peripheral else { return }
    
    var data = Data(capacity: 4)
    data.append(contentsOf: withUnsafeBytes(of: offset.littleEndian) { Array($0) })
    data.append(maxResults)
    data.append(0xFF)  // All packs
    
    peripheral.writeValue(data, for: char, type: .withResponse)
    
    // Then read
    peripheral.readValue(for: char)
}

func parseListResponse(_ data: Data) -> [PlantListEntry] {
    guard data.count >= 4 else { return [] }
    
    let totalCount = data.subdata(in: 0..<2).withUnsafeBytes { 
        $0.load(as: UInt16.self) 
    }
    let returnedCount = data[2]
    
    var entries: [PlantListEntry] = []
    var offset = 4
    
    for _ in 0..<returnedCount {
        guard offset + 20 <= data.count else { break }
        
        let entry = PlantListEntry(
            plantId: data.subdata(in: offset..<offset+2).withUnsafeBytes { 
                $0.load(as: UInt16.self) 
            },
            packId: data[offset + 2],
            version: data[offset + 3],
            name: String(data: data.subdata(in: offset+4..<offset+20), 
                        encoding: .utf8)?.trimmingCharacters(in: .controlCharacters) ?? ""
        )
        entries.append(entry)
        offset += 20
    }
    
    return entries
}
```

### Delete a Plant

```swift
func deletePlant(_ plantId: UInt16) {
    guard let char = plantCharacteristic,
          let peripheral = peripheral else { return }
    
    var data = Data(capacity: 2)
    data.append(contentsOf: withUnsafeBytes(of: plantId.littleEndian) { Array($0) })
    
    peripheral.writeValue(data, for: char, type: .withResponse)
}
```

---

## Storage Statistics

```swift
func readStats() {
    guard let char = statsCharacteristic,
          let peripheral = peripheral else { return }
    
    peripheral.readValue(for: char)
}

struct PackStats {
    let totalBytes: UInt32
    let usedBytes: UInt32
    let freeBytes: UInt32
    let plantCount: UInt16
    let packCount: UInt16
    let builtinCount: UInt16
    let status: UInt8
    let changeCounter: UInt32  // For cache invalidation
    
    init?(data: Data) {
        guard data.count >= 24 else { return nil }
        
        totalBytes = data.subdata(in: 0..<4).withUnsafeBytes { $0.load(as: UInt32.self) }
        usedBytes = data.subdata(in: 4..<8).withUnsafeBytes { $0.load(as: UInt32.self) }
        freeBytes = data.subdata(in: 8..<12).withUnsafeBytes { $0.load(as: UInt32.self) }
        plantCount = data.subdata(in: 12..<14).withUnsafeBytes { $0.load(as: UInt16.self) }
        packCount = data.subdata(in: 14..<16).withUnsafeBytes { $0.load(as: UInt16.self) }
        builtinCount = data.subdata(in: 16..<18).withUnsafeBytes { $0.load(as: UInt16.self) }
        status = data[18]
        // data[19] is reserved
        changeCounter = data.subdata(in: 20..<24).withUnsafeBytes { $0.load(as: UInt32.self) }
    }
    
    var usagePercent: Float {
        return Float(usedBytes) / Float(totalBytes) * 100
    }
}
```

---

## Caching Strategy

The device provides efficient cache invalidation via the `changeCounter` field in Pack Stats.

### How It Works

1. **changeCounter** is **persisted to flash** - survives device reboot
2. Increments every time a plant/pack is installed or deleted
3. App caches plant list along with the changeCounter value
4. On reconnect, compare changeCounter - if same, skip re-listing

### Persistence Details

The counter is stored at `/lfs_ext/packs/counter.bin` on the device's external flash:
- Loaded at pack storage initialization
- Saved after each modification
- Survives power cycles and reboots

### Implementation

```swift
class PlantCache {
    // Built-in plants (bundled with app - never change)
    static let builtInPlants: [Plant] = loadFromBundle("plants.json")  // 223 plants
    
    // Custom plants from device (cached)
    private var customPlants: [PlantListEntry] = []
    private var cachedChangeCounter: UInt32 = 0
    
    func loadPlants(from stats: PackStats, 
                    fetchCustom: (@escaping ([PlantListEntry]) -> Void) -> Void,
                    completion: @escaping ([Plant]) -> Void) {
        
        // Check if cache is valid
        if stats.changeCounter == cachedChangeCounter && !customPlants.isEmpty {
            print("✅ Cache valid (changeCounter=\(cachedChangeCounter))")
            completion(Self.builtInPlants + customPlantsAsPlants())
            return
        }
        
        // Cache invalid or empty - fetch from device
        print("🔄 Cache miss (cached=\(cachedChangeCounter), device=\(stats.changeCounter))")
        
        // Only fetch custom plants (built-in are in app bundle)
        fetchCustom { [weak self] entries in
            self?.customPlants = entries
            self?.cachedChangeCounter = stats.changeCounter
            self?.saveToStorage()
            completion(Self.builtInPlants + (self?.customPlantsAsPlants() ?? []))
        }
    }
    
    private func customPlantsAsPlants() -> [Plant] {
        return customPlants.map { entry in
            Plant(id: entry.plantId, name: entry.name, packId: entry.packId)
        }
    }
    
    private func saveToStorage() {
        // Persist to UserDefaults or Core Data
        UserDefaults.standard.set(cachedChangeCounter, forKey: "plantCacheCounter")
        // ... save customPlants
    }
    
    private func loadFromStorage() {
        cachedChangeCounter = UInt32(UserDefaults.standard.integer(forKey: "plantCacheCounter"))
        // ... load customPlants
    }
}
```

### Connection Flow

```swift
func onDeviceConnected() {
    // 1. Read stats (1 BLE request, ~50ms)
    readStats { stats in
        // 2. Check cache
        self.plantCache.loadPlants(from: stats, fetchCustom: { completion in
            // Only called if cache invalid
            self.listCustomPlants { entries in
                completion(entries)
            }
        }) { allPlants in
            // 3. Update UI
            self.updatePlantPicker(allPlants)
        }
    }
}

func listCustomPlants(completion: @escaping ([PlantListEntry]) -> Void) {
    // List only custom plants (plant_count from stats, usually 0-20)
    // This is FAST: typically 0-3 BLE requests
    listPlants(offset: 0, maxResults: 8) { entries in
        completion(entries.filter { $0.packId != 0 })  // Exclude built-in
    }
}
```

### Timing Summary

| Scenario | BLE Requests | Time |
|----------|--------------|------|
| Cache valid | 1 (stats only) | ~50ms |
| Cache invalid, 5 custom plants | 2 (stats + list) | ~100ms |
| Cache invalid, 50 custom plants | 8 | ~400ms |
| First connect ever | 2+ | ~100-400ms |

**Note:** Built-in 223 plants are NEVER fetched from device - they're bundled with the app.

---

## Pack Transfer Protocol

### Complete Transfer Implementation

```swift
class PackTransfer {
    let peripheral: CBPeripheral
    let characteristic: CBCharacteristic
    
    var plants: [PackPlant] = []
    var packId: UInt16 = 0
    var version: UInt16 = 0
    var packName: String = ""
    
    var state: TransferState = .idle
    var progress: Float = 0
    
    enum TransferState {
        case idle, sending, complete, error(String)
    }
    
    func startTransfer(plants: [PackPlant], 
                       packId: UInt16, 
                       version: UInt16, 
                       name: String) async throws {
        self.plants = plants
        self.packId = packId
        self.version = version
        self.packName = name
        
        // Enable notifications
        peripheral.setNotifyValue(true, for: characteristic)
        
        // Calculate payload
        let payload = plants.map { $0.serialize() }.reduce(Data(), +)
        let crc = crc32(payload)
        
        // 1. Send START
        try await sendStart(
            packId: packId,
            version: version,
            plantCount: UInt16(plants.count),
            totalSize: UInt32(payload.count),
            crc32: crc,
            name: name
        )
        
        state = .sending
        
        // 2. Send DATA chunks
        let chunkSize = 240
        var offset = 0
        
        while offset < payload.count {
            let end = min(offset + chunkSize, payload.count)
            let chunk = payload.subdata(in: offset..<end)
            
            try await sendData(offset: UInt32(offset), data: chunk)
            
            offset = end
            progress = Float(offset) / Float(payload.count)
        }
        
        // 3. Send COMMIT
        try await sendCommit()
        
        state = .complete
    }
    
    private func sendStart(packId: UInt16, 
                          version: UInt16,
                          plantCount: UInt16,
                          totalSize: UInt32,
                          crc32: UInt32,
                          name: String) async throws {
        var data = Data(capacity: 47)
        data.append(0x01)  // Opcode START
        data.append(contentsOf: withUnsafeBytes(of: packId.littleEndian) { Array($0) })
        data.append(contentsOf: withUnsafeBytes(of: version.littleEndian) { Array($0) })
        data.append(contentsOf: withUnsafeBytes(of: plantCount.littleEndian) { Array($0) })
        data.append(contentsOf: withUnsafeBytes(of: totalSize.littleEndian) { Array($0) })
        data.append(contentsOf: withUnsafeBytes(of: crc32.littleEndian) { Array($0) })
        data.append(contentsOf: name.paddedUTF8(to: 32))
        
        try await writeWithResponse(data)
    }
    
    private func sendData(offset: UInt32, data chunk: Data) async throws {
        var data = Data(capacity: 7 + chunk.count)
        data.append(0x02)  // Opcode DATA
        data.append(contentsOf: withUnsafeBytes(of: offset.littleEndian) { Array($0) })
        data.append(contentsOf: withUnsafeBytes(of: UInt16(chunk.count).littleEndian) { Array($0) })
        data.append(chunk)
        
        try await writeWithResponse(data)
    }
    
    private func sendCommit() async throws {
        let data = Data([0x03])  // Opcode COMMIT
        try await writeWithResponse(data)
    }
    
    func abort() {
        let data = Data([0x04])  // Opcode ABORT
        peripheral.writeValue(data, for: characteristic, type: .withResponse)
        state = .idle
    }
    
    private func writeWithResponse(_ data: Data) async throws {
        // Use async wrapper for BLE write
        peripheral.writeValue(data, for: characteristic, type: .withResponse)
        // Wait for notification or implement proper async handling
    }
}

// CRC32 calculation
func crc32(_ data: Data) -> UInt32 {
    var crc: UInt32 = 0xFFFFFFFF
    
    for byte in data {
        crc ^= UInt32(byte)
        for _ in 0..<8 {
            crc = (crc >> 1) ^ (0xEDB88320 & (crc & 1 != 0 ? 0xFFFFFFFF : 0))
        }
    }
    
    return ~crc
}
```

### Transfer Status Parsing

```swift
struct TransferStatus {
    let state: UInt8
    let progressPercent: UInt8
    let packId: UInt16
    let bytesReceived: UInt32
    let bytesExpected: UInt32
    let lastError: UInt8
    
    init?(data: Data) {
        guard data.count >= 16 else { return nil }
        
        state = data[0]
        progressPercent = data[1]
        packId = data.subdata(in: 2..<4).withUnsafeBytes { $0.load(as: UInt16.self) }
        bytesReceived = data.subdata(in: 4..<8).withUnsafeBytes { $0.load(as: UInt32.self) }
        bytesExpected = data.subdata(in: 8..<12).withUnsafeBytes { $0.load(as: UInt32.self) }
        lastError = data[12]
    }
    
    var stateName: String {
        switch state {
        case 0: return "Idle"
        case 1: return "Receiving"
        case 2: return "Complete"
        case 3: return "Error"
        default: return "Unknown"
        }
    }
    
    var errorName: String? {
        guard lastError != 0 else { return nil }
        switch lastError {
        case 3: return "Invalid Data"
        case 5: return "Storage Full"
        case 6: return "I/O Error"
        case 8: return "CRC Mismatch"
        default: return "Error \(lastError)"
        }
    }
}
```

---

## UI/UX Recommendations

### Plant Browser

```
┌──────────────────────────────────────┐
│ 🌱 Custom Plants                    │
├──────────────────────────────────────┤
│ ┌────┐ Tomato                        │
│ │ 🍅 │ Solanum lycopersicum         │
│ └────┘ Pack: Vegetables v1    [⋮]   │
├──────────────────────────────────────┤
│ ┌────┐ Bell Pepper                   │
│ │ 🫑 │ Capsicum annuum              │
│ └────┘ Pack: Vegetables v1    [⋮]   │
├──────────────────────────────────────┤
│ ┌────┐ Basil                         │
│ │ 🌿 │ Ocimum basilicum             │
│ └────┘ Custom               [⋮]     │
└──────────────────────────────────────┘
     [+ Add Plant]   [📦 Install Pack]
```

### Transfer Progress

```
┌──────────────────────────────────────┐
│ Installing "Vegetables" Pack         │
├──────────────────────────────────────┤
│                                      │
│  ████████████░░░░░░░░  60%          │
│                                      │
│  Transferring plant data...          │
│  3 of 5 plants                       │
│                                      │
│           [Cancel]                   │
└──────────────────────────────────────┘
```

### Error Handling

```
┌──────────────────────────────────────┐
│ ⚠️ Transfer Failed                   │
├──────────────────────────────────────┤
│                                      │
│  CRC verification failed.            │
│                                      │
│  The data may have been corrupted    │
│  during transfer.                    │
│                                      │
│        [Retry]    [Cancel]           │
└──────────────────────────────────────┘
```

---

## Error Handling

### Result Codes

| Code | Name | User Message | Recovery |
|------|------|--------------|----------|
| 0 | SUCCESS | "Plant installed" | - |
| 1 | UPDATED | "Plant updated" | - |
| 2 | ALREADY_CURRENT | "Already up to date" | - |
| 3 | INVALID_DATA | "Invalid plant data" | Check data |
| 5 | STORAGE_FULL | "Storage full" | Delete plants |
| 6 | IO_ERROR | "Storage error" | Retry |
| 7 | NOT_FOUND | "Plant not found" | Refresh list |
| 8 | CRC_MISMATCH | "Data corrupted" | Retry transfer |

### Retry Logic

```swift
func installWithRetry(_ plant: PackPlant, maxRetries: Int = 3) async throws {
    var lastError: Error?
    
    for attempt in 1...maxRetries {
        do {
            try await installPlant(plant)
            return
        } catch let error {
            lastError = error
            
            if attempt < maxRetries {
                // Exponential backoff
                try await Task.sleep(nanoseconds: UInt64(pow(2.0, Double(attempt))) * 500_000_000)
            }
        }
    }
    
    throw lastError ?? PackError.unknown
}
```

---

## MTU Negotiation

```swift
func peripheral(_ peripheral: CBPeripheral, 
                didOpen channel: CBL2CAPChannel?, 
                error: Error?) {
    // Request larger MTU for pack transfers
    let mtu = peripheral.maximumWriteValueLength(for: .withResponse)
    print("MTU: \(mtu)")
    
    // Adjust chunk size based on MTU
    chunkSize = min(mtu - 7, 240)  // 7 bytes for DATA header
}
```

---

## Testing Checklist

### Single Plant Operations
- [ ] Install new plant
- [ ] Update existing plant (higher version)
- [ ] Install same version (should return ALREADY_CURRENT)
- [ ] Delete plant
- [ ] List empty plants
- [ ] List with pagination

### Pack Transfer
- [ ] Transfer 1 plant
- [ ] Transfer 64 plants (max)
- [ ] Verify CRC validation
- [ ] Test abort during transfer
- [ ] Test timeout handling
- [ ] Test resume after disconnect

### Edge Cases
- [ ] Plant name with UTF-8 characters
- [ ] Maximum length names (31 chars)
- [ ] Empty plant name
- [ ] Concurrent operations
- [ ] Low MTU (23 bytes)
- [ ] Connection loss during transfer

---

## Sample Plant Data

### Tomato

```swift
let tomato = PackPlant(
    plantId: 1001,
    packId: 1,
    version: 1,
    source: 2,  // PLANT_SOURCE_PACK
    flags: 0,
    reservedId: 0,
    commonName: "Tomato",
    scientificName: "Solanum lycopersicum",
    kcIni: 60,      // 0.60
    kcMid: 115,     // 1.15
    kcEnd: 80,      // 0.80
    kcFlags: 0,
    lIniDays: 35,
    lDevDays: 40,
    lMidDays: 40,
    lEndDays: 20,
    rootDepthMin: 300,   // 30 cm
    rootDepthMax: 1500,  // 150 cm
    rootGrowthRate: 25,  // 2.5 mm/day
    rootFlags: 0,
    depletionFraction: 40,  // 0.40
    yieldResponse: 110,     // 1.10
    criticalDepletion: 60,  // 0.60
    waterFlags: 0,
    tempMin: 10,
    tempMax: 35,
    tempOptimalLow: 20,
    tempOptimalHigh: 27,
    humidityMin: 50,
    humidityMax: 80,
    lightMin: 30,
    lightMax: 80,
    reserved: 0
)
```
