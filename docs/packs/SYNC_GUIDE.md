# Plant Database Sync Guide

**Version**: 1.0.0  
**Updated**: January 2026

## Overview

This guide explains how to synchronize the plant database between your mobile app and the AutoWatering device. It covers the complete workflow from connection to full sync.

---

## Quick Reference

```
┌─────────────────────────────────────────────────────────────────────┐
│                    SYNC WORKFLOW SUMMARY                             │
├─────────────────────────────────────────────────────────────────────┤
│  1. Connect to device                                                │
│  2. Read Pack Stats → get change_counter                            │
│  3. Compare with cached value                                        │
│     ├─ Same? Skip plant list refresh                                │
│     └─ Different? Stream plant list                                 │
│  4. Stream: Write [00 00 FE 00] → receive notifications             │
│  5. Cache plant list + change_counter                               │
└─────────────────────────────────────────────────────────────────────┘
```

---

## Step-by-Step Implementation

### Step 1: Connect and Discover Services

```kotlin
// Android - Kotlin
class PlantSyncManager(private val gatt: BluetoothGatt) {
    
    private lateinit var packPlantChar: BluetoothGattCharacteristic
    private lateinit var packStatsChar: BluetoothGattCharacteristic
    
    fun onServicesDiscovered() {
        val packService = gatt.getService(PACK_SERVICE_UUID)
        packPlantChar = packService.getCharacteristic(PACK_PLANT_UUID)
        packStatsChar = packService.getCharacteristic(PACK_STATS_UUID)
        
        // Enable notifications on Pack Plant
        enableNotifications(packPlantChar)
    }
    
    private fun enableNotifications(char: BluetoothGattCharacteristic) {
        gatt.setCharacteristicNotification(char, true)
        val ccc = char.getDescriptor(CCC_UUID)
        ccc.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
        gatt.writeDescriptor(ccc)
    }
    
    companion object {
        val PACK_SERVICE_UUID = UUID.fromString("12345678-1234-5678-9abc-def123456800")
        val PACK_PLANT_UUID = UUID.fromString("12345678-1234-5678-9abc-def123456786")
        val PACK_STATS_UUID = UUID.fromString("12345678-1234-5678-9abc-def123456787")
        val CCC_UUID = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")
    }
}
```

### Step 2: Check if Sync is Needed

Read Pack Stats and compare `change_counter` with cached value:

```kotlin
// Android - Kotlin
data class PackStats(
    val totalBytes: Long,
    val usedBytes: Long,
    val freeBytes: Long,
    val plantCount: Int,         // Total plants in flash storage
    val customPlantCount: Int,   // Custom plants only (pack_id != 0)
    val packCount: Int, // total packs (built-in + custom)
    val builtinCount: Int,       // ROM plants (223, constant)
    val status: Int,
    val changeCounter: Long
)

fun parsePackStats(data: ByteArray): PackStats {
    val buffer = ByteBuffer.wrap(data).order(ByteOrder.LITTLE_ENDIAN)
    return PackStats(
        totalBytes = buffer.int.toLong() and 0xFFFFFFFFL,
        usedBytes = buffer.int.toLong() and 0xFFFFFFFFL,
        freeBytes = buffer.int.toLong() and 0xFFFFFFFFL,
        plantCount = buffer.short.toInt() and 0xFFFF,
        customPlantCount = buffer.short.toInt() and 0xFFFF,  // NEW: custom only
        packCount = buffer.short.toInt() and 0xFFFF,
        builtinCount = buffer.short.toInt() and 0xFFFF,
        status = buffer.get().toInt() and 0xFF,
        changeCounter = run { buffer.get(); buffer.int.toLong() and 0xFFFFFFFFL }
    )
}

fun checkSyncNeeded(stats: PackStats): Boolean {
    val cachedCounter = sharedPrefs.getLong("plant_change_counter", -1)
    return cachedCounter != stats.changeCounter
}

// Use customPlantCount to decide if streaming custom plants is needed
fun hasCustomPlants(stats: PackStats): Boolean = stats.customPlantCount > 0
```

### Step 3: Request Plant List Streaming

If sync is needed, request the plant list via streaming:

```kotlin
// Android - Kotlin
fun requestPlantStream(filter: PlantFilter) {
    val filterByte = when (filter) {
        PlantFilter.CUSTOM_ONLY -> 0xFF.toByte()  // Only custom plants
        PlantFilter.ALL -> 0xFE.toByte()          // Built-in + custom
        PlantFilter.BUILTIN_ONLY -> 0x00.toByte() // Only built-in (pack 0)
    }
    
    val request = byteArrayOf(
        0x00, 0x00,   // offset = 0 (ignored in streaming)
        filterByte,   // filter
        0x00          // max_count = 0 (STREAMING MODE)
    )
    
    packPlantChar.value = request
    packPlantChar.writeType = BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
    gatt.writeCharacteristic(packPlantChar)
}

enum class PlantFilter {
    CUSTOM_ONLY,  // Use if app has built-in CSV
    ALL,          // Full sync including built-in
    BUILTIN_ONLY  // Only ROM plants
}
```

### Step 4: Handle Streaming Notifications

```kotlin
// Android - Kotlin
data class PlantEntry(
    val plantId: Int,
    val packId: Int,
    val version: Int,
    val name: String
)

class PlantStreamHandler {
    private val plants = mutableListOf<PlantEntry>()
    private var totalExpected = 0
    private var isComplete = false
    
    // Stream flags
    companion object {
        const val FLAG_STARTING = 0x80
        const val FLAG_NORMAL = 0x00
        const val FLAG_COMPLETE = 0x01
        const val FLAG_ERROR = 0x02
    }
    
    fun onNotification(data: ByteArray): StreamState {
        val buffer = ByteBuffer.wrap(data).order(ByteOrder.LITTLE_ENDIAN)
        
        val totalCount = buffer.short.toInt() and 0xFFFF
        val returnedCount = buffer.get().toInt() and 0xFF
        val flags = buffer.get().toInt() and 0xFF
        
        // Handle stream start
        if (flags and FLAG_STARTING != 0) {
            plants.clear()
            totalExpected = totalCount
        }
        
        // Handle error
        if (flags == FLAG_ERROR) {
            return StreamState.ERROR
        }
        
        // Parse plant entries (22 bytes each)
        for (i in 0 until returnedCount) {
            val plantId = buffer.short.toInt() and 0xFFFF
            val packId = buffer.short.toInt() and 0xFFFF
            val version = buffer.short.toInt() and 0xFFFF
            
            val nameBytes = ByteArray(16)
            buffer.get(nameBytes)
            val name = String(nameBytes).trim('\u0000')
            
            plants.add(PlantEntry(plantId, packId, version, name))
        }
        
        // Handle completion
        if (flags == FLAG_COMPLETE) {
            isComplete = true
            return StreamState.COMPLETE
        }
        
        return StreamState.IN_PROGRESS
    }
    
    fun getPlants(): List<PlantEntry> = plants.toList()
    fun getProgress(): Float = if (totalExpected > 0) plants.size.toFloat() / totalExpected else 0f
}

enum class StreamState {
    IN_PROGRESS,
    COMPLETE,
    ERROR
}
```

### Step 5: Cache Results

```kotlin
// Android - Kotlin
fun saveToCache(plants: List<PlantEntry>, changeCounter: Long) {
    // Save change counter
    sharedPrefs.edit()
        .putLong("plant_change_counter", changeCounter)
        .apply()
    
    // Save plants to local database
    database.plantDao().deleteAll()
    database.plantDao().insertAll(plants.map { it.toEntity() })
}
```

---

## Complete Sync Manager (Android)

```kotlin
class PlantSyncManager(
    private val gatt: BluetoothGatt,
    private val database: PlantDatabase,
    private val prefs: SharedPreferences
) : BluetoothGattCallback() {
    
    private var packPlantChar: BluetoothGattCharacteristic? = null
    private var packStatsChar: BluetoothGattCharacteristic? = null
    
    private val streamHandler = PlantStreamHandler()
    private var currentStats: PackStats? = null
    private var syncCallback: ((SyncResult) -> Unit)? = null
    
    // Public API
    fun startSync(callback: (SyncResult) -> Unit) {
        syncCallback = callback
        readPackStats()
    }
    
    // Step 1: Read stats
    private fun readPackStats() {
        packStatsChar?.let { gatt.readCharacteristic(it) }
    }
    
    // Step 2: Check and stream
    override fun onCharacteristicRead(
        gatt: BluetoothGatt,
        characteristic: BluetoothGattCharacteristic,
        status: Int
    ) {
        if (characteristic.uuid == PACK_STATS_UUID && status == BluetoothGatt.GATT_SUCCESS) {
            val stats = parsePackStats(characteristic.value)
            currentStats = stats
            
            val cachedCounter = prefs.getLong("change_counter", -1)
            if (cachedCounter == stats.changeCounter) {
                // No changes, use cache
                syncCallback?.invoke(SyncResult.AlreadyUpToDate)
            } else {
                // Need to sync
                requestPlantStream(PlantFilter.CUSTOM_ONLY)
            }
        }
    }
    
    // Step 3: Handle streaming
    override fun onCharacteristicChanged(
        gatt: BluetoothGatt,
        characteristic: BluetoothGattCharacteristic
    ) {
        if (characteristic.uuid == PACK_PLANT_UUID) {
            when (streamHandler.onNotification(characteristic.value)) {
                StreamState.COMPLETE -> {
                    saveToCache()
                    syncCallback?.invoke(SyncResult.Success(streamHandler.getPlants()))
                }
                StreamState.ERROR -> {
                    syncCallback?.invoke(SyncResult.Error("Stream error"))
                }
                StreamState.IN_PROGRESS -> {
                    // Update progress UI
                }
            }
        }
    }
    
    private fun saveToCache() {
        currentStats?.let { stats ->
            prefs.edit()
                .putLong("change_counter", stats.changeCounter)
                .apply()
        }
        database.plantDao().replaceAll(streamHandler.getPlants())
    }
    
    private fun requestPlantStream(filter: PlantFilter) {
        val request = byteArrayOf(0x00, 0x00, filter.value, 0x00)
        packPlantChar?.let {
            it.value = request
            gatt.writeCharacteristic(it)
        }
    }
}

sealed class SyncResult {
    object AlreadyUpToDate : SyncResult()
    data class Success(val plants: List<PlantEntry>) : SyncResult()
    data class Error(val message: String) : SyncResult()
}
```

---

## Complete Sync Manager (iOS)

```swift
import CoreBluetooth

// Pack Stats structure (26 bytes)
struct PackStats {
    let totalBytes: UInt32
    let usedBytes: UInt32
    let freeBytes: UInt32
    let plantCount: UInt16         // Total plants in flash storage
    let customPlantCount: UInt16   // Custom plants only (pack_id != 0)
    let packCount: UInt16 // total packs (built-in + custom)
    let builtinCount: UInt16       // ROM plants (223, constant)
    let status: UInt8
    let changeCounter: UInt32
    
    static func parse(_ data: Data) -> PackStats? {
        guard data.count >= 26 else { return nil }
        return data.withUnsafeBytes { ptr in
            PackStats(
                totalBytes: ptr.load(fromByteOffset: 0, as: UInt32.self),
                usedBytes: ptr.load(fromByteOffset: 4, as: UInt32.self),
                freeBytes: ptr.load(fromByteOffset: 8, as: UInt32.self),
                plantCount: ptr.load(fromByteOffset: 12, as: UInt16.self),
                customPlantCount: ptr.load(fromByteOffset: 14, as: UInt16.self),
                packCount: ptr.load(fromByteOffset: 16, as: UInt16.self),
                builtinCount: ptr.load(fromByteOffset: 18, as: UInt16.self),
                status: ptr.load(fromByteOffset: 20, as: UInt8.self),
                changeCounter: ptr.load(fromByteOffset: 22, as: UInt32.self)
            )
        }
    }
    
    var hasCustomPlants: Bool { customPlantCount > 0 }
}

class PlantSyncManager: NSObject, CBPeripheralDelegate {
    
    private var peripheral: CBPeripheral
    private var packPlantChar: CBCharacteristic?
    private var packStatsChar: CBCharacteristic?
    
    private var plants: [PlantEntry] = []
    private var totalExpected: Int = 0
    private var currentStats: PackStats?
    private var syncCompletion: ((Result<[PlantEntry], Error>) -> Void)?
    
    // UUIDs
    static let packServiceUUID = CBUUID(string: "12345678-1234-5678-9abc-def123456800")
    static let packPlantUUID = CBUUID(string: "12345678-1234-5678-9abc-def123456786")
    static let packStatsUUID = CBUUID(string: "12345678-1234-5678-9abc-def123456787")
    
    init(peripheral: CBPeripheral) {
        self.peripheral = peripheral
        super.init()
        peripheral.delegate = self
    }
    
    // MARK: - Public API
    
    func startSync(completion: @escaping (Result<[PlantEntry], Error>) -> Void) {
        syncCompletion = completion
        plants.removeAll()
        
        // Read stats first
        if let char = packStatsChar {
            peripheral.readValue(for: char)
        }
    }
    
    // MARK: - CBPeripheralDelegate
    
    func peripheral(_ peripheral: CBPeripheral, 
                    didDiscoverCharacteristicsFor service: CBService, 
                    error: Error?) {
        for char in service.characteristics ?? [] {
            switch char.uuid {
            case Self.packPlantUUID:
                packPlantChar = char
                peripheral.setNotifyValue(true, for: char)
            case Self.packStatsUUID:
                packStatsChar = char
            default:
                break
            }
        }
    }
    
    func peripheral(_ peripheral: CBPeripheral, 
                    didUpdateValueFor characteristic: CBCharacteristic, 
                    error: Error?) {
        guard let data = characteristic.value else { return }
        
        switch characteristic.uuid {
        case Self.packStatsUUID:
            handleStatsRead(data)
        case Self.packPlantUUID:
            handlePlantNotification(data)
        default:
            break
        }
    }
    
    // MARK: - Private
    
    private func handleStatsRead(_ data: Data) {
        guard let stats = PackStats.parse(data) else { return }
        currentStats = stats
        
        let cachedCounter = UserDefaults.standard.integer(forKey: "changeCounter")
        if cachedCounter == stats.changeCounter {
            syncCompletion?(.success([]))  // Already up to date
        } else if stats.hasCustomPlants {
            requestPlantStream(filter: .customOnly)
        } else {
            // No custom plants to sync
            saveToCache()
            syncCompletion?(.success([]))
        }
    }
    
    private func handlePlantNotification(_ data: Data) {
        let totalCount = data.withUnsafeBytes { $0.load(fromByteOffset: 0, as: UInt16.self) }
        let returnedCount = Int(data[2])
        let flags = data[3]
        
        // STARTING
        if flags & 0x80 != 0 {
            plants.removeAll()
            totalExpected = Int(totalCount)
        }
        
        // ERROR
        if flags == 0x02 {
            syncCompletion?(.failure(SyncError.streamError))
            return
        }
        
        // Parse entries
        for i in 0..<returnedCount {
            let offset = 4 + i * 22
            let entry = parsePlantEntry(data: data, offset: offset)
            plants.append(entry)
        }
        
        // COMPLETE
        if flags == 0x01 {
            saveToCache()
            syncCompletion?(.success(plants))
        }
    }
    
    private func requestPlantStream(filter: PlantFilter) {
        var request = Data(count: 4)
        request[0] = 0x00  // offset low
        request[1] = 0x00  // offset high
        request[2] = filter.rawValue
        request[3] = 0x00  // streaming mode
        
        if let char = packPlantChar {
            peripheral.writeValue(request, for: char, type: .withResponse)
        }
    }
    
    private func saveToCache() {
        if let stats = currentStats {
            UserDefaults.standard.set(stats.changeCounter, forKey: "changeCounter")
        }
        // Save plants to Core Data or Realm
    }
    
    private func parsePlantEntry(data: Data, offset: Int) -> PlantEntry {
        let plantId = data.subdata(in: offset..<offset+2)
            .withUnsafeBytes { $0.load(as: UInt16.self) }
        let packId = data.subdata(in: offset+2..<offset+4)
            .withUnsafeBytes { $0.load(as: UInt16.self) }
        let version = data.subdata(in: offset+4..<offset+6)
            .withUnsafeBytes { $0.load(as: UInt16.self) }
        let nameData = data.subdata(in: offset+6..<offset+22)
        let name = String(data: nameData, encoding: .utf8)?
            .trimmingCharacters(in: .controlCharacters) ?? ""
        
        return PlantEntry(
            plantId: Int(plantId),
            packId: Int(packId),
            version: Int(version),
            name: name
        )
    }
}

enum PlantFilter: UInt8 {
    case customOnly = 0xFF
    case all = 0xFE
    case builtinOnly = 0x00
}

enum SyncError: Error {
    case streamError
    case timeout
}
```

---

## Filter Selection Guide

| Scenario | Filter | Value | Rationale |
|----------|--------|-------|-----------|
| App has built-in CSV | `CUSTOM_ONLY` | `0xFF` | Only sync custom plants, use CSV for 223 built-in |
| Full offline mode | `ALL` | `0xFE` | Sync everything including built-in |
| Debugging/testing | `BUILTIN_ONLY` | `0x00` | Verify ROM database access |
| Specific pack | Pack ID | `0x01-0xFD` | Sync only plants from one pack |

### Recommended Approach

Most apps should use `CUSTOM_ONLY` (0xFF) because:

1. **Built-in database is static** - 223 plants never change
2. **CSV is smaller** - Include `plants_full.csv` in app bundle (~50KB)
3. **Faster sync** - Only transfer custom plants (seconds vs ~50ms for 223)
4. **Offline access** - CSV works without BLE connection

```kotlin
// Include in app assets: plants_full.csv
// Parse once at app startup
val builtInPlants = parseCSV(assets.open("plants_full.csv"))

// Only sync custom plants via BLE
requestPlantStream(PlantFilter.CUSTOM_ONLY)
```

---

## Performance Expectations

| Plants | Notifications | Time |
|--------|---------------|------|
| 10 custom | 1 | ~2ms |
| 50 custom | 5 | ~10ms |
| 223 built-in | 23 | ~50ms |
| 223 + 50 custom | 28 | ~60ms |

**Note**: Times assume stable BLE connection with 7.5-15ms connection interval.

---

## Error Handling

### Stream Error Recovery

```kotlin
fun handleStreamError() {
    // Wait before retry
    delay(500)
    
    // Retry up to 3 times
    for (attempt in 1..3) {
        try {
            requestPlantStream(filter)
            return
        } catch (e: Exception) {
            if (attempt == 3) throw e
            delay(attempt * 500L)
        }
    }
}
```

### Connection Loss During Sync

```kotlin
override fun onConnectionStateChange(gatt: BluetoothGatt, status: Int, newState: Int) {
    if (newState == BluetoothProfile.STATE_DISCONNECTED) {
        // Connection lost
        if (streamHandler.isInProgress()) {
            // Mark sync incomplete, retry on reconnect
            prefs.edit().putBoolean("sync_incomplete", true).apply()
        }
    }
}

override fun onServicesDiscovered(gatt: BluetoothGatt, status: Int) {
    if (prefs.getBoolean("sync_incomplete", false)) {
        // Resume sync
        startSync { result ->
            if (result is SyncResult.Success) {
                prefs.edit().putBoolean("sync_incomplete", false).apply()
            }
        }
    }
}
```

---

## Testing Checklist

- [ ] Sync with 0 custom plants
- [ ] Sync with 10+ custom plants
- [ ] Sync with ALL filter (223+ plants)
- [ ] Connection loss mid-stream → reconnect → resume
- [ ] Error flag handling
- [ ] Cache validation (same change_counter skips sync)
- [ ] Cache invalidation (different counter triggers sync)
- [ ] Progress indicator updates
- [ ] Large plant names (max 16 chars after truncation)

---

## Related Documentation

- [BLE_PACK_SERVICE.md](BLE_PACK_SERVICE.md) - Full BLE protocol reference
- [MOBILE_APP_GUIDE.md](MOBILE_APP_GUIDE.md) - Plant installation and management
- [PACK_SCHEMA.md](PACK_SCHEMA.md) - Binary structure definitions
