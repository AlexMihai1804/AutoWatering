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

### Plant Structure (120 bytes)

```swift
struct PackPlant: Codable {
    var plantId: UInt16
    var packId: UInt16
    var version: UInt16
    var source: UInt8
    var flags: UInt8
    var reservedId: UInt32
    var commonName: String      // 32 bytes, null-terminated
    var scientificName: String  // 32 bytes, null-terminated
    var kcIni: UInt16           // ×100
    var kcMid: UInt16           // ×100
    var kcEnd: UInt16           // ×100
    var kcFlags: UInt16
    var lIniDays: UInt16
    var lDevDays: UInt16
    var lMidDays: UInt16
    var lEndDays: UInt16
    var rootDepthMin: UInt16    // mm
    var rootDepthMax: UInt16    // mm
    var rootGrowthRate: UInt16  // ×10
    var rootFlags: UInt16
    var depletionFraction: UInt16  // ×100
    var yieldResponse: UInt16      // ×100
    var criticalDepletion: UInt16  // ×100
    var waterFlags: UInt16
    var tempMin: Int8           // °C
    var tempMax: Int8
    var tempOptimalLow: Int8
    var tempOptimalHigh: Int8
    var humidityMin: UInt8      // %
    var humidityMax: UInt8
    var lightMin: UInt8         // klux
    var lightMax: UInt8
    var reserved: UInt32
}
```

### Serialization (Swift)

```swift
extension PackPlant {
    func serialize() -> Data {
        var data = Data(capacity: 120)
        
        // Identification (12 bytes)
        data.append(contentsOf: withUnsafeBytes(of: plantId.littleEndian) { Array($0) })
        data.append(contentsOf: withUnsafeBytes(of: packId.littleEndian) { Array($0) })
        data.append(contentsOf: withUnsafeBytes(of: version.littleEndian) { Array($0) })
        data.append(source)
        data.append(flags)
        data.append(contentsOf: withUnsafeBytes(of: reservedId.littleEndian) { Array($0) })
        
        // Names (64 bytes)
        data.append(contentsOf: commonName.paddedUTF8(to: 32))
        data.append(contentsOf: scientificName.paddedUTF8(to: 32))
        
        // Kc values (8 bytes)
        data.append(contentsOf: withUnsafeBytes(of: kcIni.littleEndian) { Array($0) })
        data.append(contentsOf: withUnsafeBytes(of: kcMid.littleEndian) { Array($0) })
        data.append(contentsOf: withUnsafeBytes(of: kcEnd.littleEndian) { Array($0) })
        data.append(contentsOf: withUnsafeBytes(of: kcFlags.littleEndian) { Array($0) })
        
        // Growth stages (8 bytes)
        data.append(contentsOf: withUnsafeBytes(of: lIniDays.littleEndian) { Array($0) })
        data.append(contentsOf: withUnsafeBytes(of: lDevDays.littleEndian) { Array($0) })
        data.append(contentsOf: withUnsafeBytes(of: lMidDays.littleEndian) { Array($0) })
        data.append(contentsOf: withUnsafeBytes(of: lEndDays.littleEndian) { Array($0) })
        
        // Root (8 bytes)
        data.append(contentsOf: withUnsafeBytes(of: rootDepthMin.littleEndian) { Array($0) })
        data.append(contentsOf: withUnsafeBytes(of: rootDepthMax.littleEndian) { Array($0) })
        data.append(contentsOf: withUnsafeBytes(of: rootGrowthRate.littleEndian) { Array($0) })
        data.append(contentsOf: withUnsafeBytes(of: rootFlags.littleEndian) { Array($0) })
        
        // Water (8 bytes)
        data.append(contentsOf: withUnsafeBytes(of: depletionFraction.littleEndian) { Array($0) })
        data.append(contentsOf: withUnsafeBytes(of: yieldResponse.littleEndian) { Array($0) })
        data.append(contentsOf: withUnsafeBytes(of: criticalDepletion.littleEndian) { Array($0) })
        data.append(contentsOf: withUnsafeBytes(of: waterFlags.littleEndian) { Array($0) })
        
        // Environment (8 bytes)
        data.append(UInt8(bitPattern: tempMin))
        data.append(UInt8(bitPattern: tempMax))
        data.append(UInt8(bitPattern: tempOptimalLow))
        data.append(UInt8(bitPattern: tempOptimalHigh))
        data.append(humidityMin)
        data.append(humidityMax)
        data.append(lightMin)
        data.append(lightMax)
        
        // Reserved (4 bytes)
        data.append(contentsOf: withUnsafeBytes(of: reserved.littleEndian) { Array($0) })
        
        assert(data.count == 120)
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
    val plantId: UShort,
    val packId: UShort,
    val version: UShort,
    val source: UByte,
    val flags: UByte,
    val reservedId: UInt,
    val commonName: String,
    val scientificName: String,
    val kcIni: UShort,
    val kcMid: UShort,
    val kcEnd: UShort,
    val kcFlags: UShort,
    val lIniDays: UShort,
    val lDevDays: UShort,
    val lMidDays: UShort,
    val lEndDays: UShort,
    val rootDepthMin: UShort,
    val rootDepthMax: UShort,
    val rootGrowthRate: UShort,
    val rootFlags: UShort,
    val depletionFraction: UShort,
    val yieldResponse: UShort,
    val criticalDepletion: UShort,
    val waterFlags: UShort,
    val tempMin: Byte,
    val tempMax: Byte,
    val tempOptimalLow: Byte,
    val tempOptimalHigh: Byte,
    val humidityMin: UByte,
    val humidityMax: UByte,
    val lightMin: UByte,
    val lightMax: UByte,
    val reserved: UInt
) {
    fun serialize(): ByteArray {
        val buffer = ByteBuffer.allocate(120).order(ByteOrder.LITTLE_ENDIAN)
        
        buffer.putShort(plantId.toShort())
        buffer.putShort(packId.toShort())
        buffer.putShort(version.toShort())
        buffer.put(source.toByte())
        buffer.put(flags.toByte())
        buffer.putInt(reservedId.toInt())
        buffer.put(commonName.toFixedBytes(32))
        buffer.put(scientificName.toFixedBytes(32))
        buffer.putShort(kcIni.toShort())
        buffer.putShort(kcMid.toShort())
        buffer.putShort(kcEnd.toShort())
        buffer.putShort(kcFlags.toShort())
        buffer.putShort(lIniDays.toShort())
        buffer.putShort(lDevDays.toShort())
        buffer.putShort(lMidDays.toShort())
        buffer.putShort(lEndDays.toShort())
        buffer.putShort(rootDepthMin.toShort())
        buffer.putShort(rootDepthMax.toShort())
        buffer.putShort(rootGrowthRate.toShort())
        buffer.putShort(rootFlags.toShort())
        buffer.putShort(depletionFraction.toShort())
        buffer.putShort(yieldResponse.toShort())
        buffer.putShort(criticalDepletion.toShort())
        buffer.putShort(waterFlags.toShort())
        buffer.put(tempMin)
        buffer.put(tempMax)
        buffer.put(tempOptimalLow)
        buffer.put(tempOptimalHigh)
        buffer.put(humidityMin.toByte())
        buffer.put(humidityMax.toByte())
        buffer.put(lightMin.toByte())
        buffer.put(lightMax.toByte())
        buffer.putInt(reserved.toInt())
        
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
        
        // Write plant data (120 bytes)
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
    
    init?(data: Data) {
        guard data.count >= 20 else { return nil }
        
        totalBytes = data.subdata(in: 0..<4).withUnsafeBytes { $0.load(as: UInt32.self) }
        usedBytes = data.subdata(in: 4..<8).withUnsafeBytes { $0.load(as: UInt32.self) }
        freeBytes = data.subdata(in: 8..<12).withUnsafeBytes { $0.load(as: UInt32.self) }
        plantCount = data.subdata(in: 12..<14).withUnsafeBytes { $0.load(as: UInt16.self) }
        packCount = data.subdata(in: 14..<16).withUnsafeBytes { $0.load(as: UInt16.self) }
        builtinCount = data.subdata(in: 16..<18).withUnsafeBytes { $0.load(as: UInt16.self) }
        status = data[18]
    }
    
    var usagePercent: Float {
        return Float(usedBytes) / Float(totalBytes) * 100
    }
}
```

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
