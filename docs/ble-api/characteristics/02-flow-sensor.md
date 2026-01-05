# Flow Sensor Characteristic (UUID: def2)

> Operation Summary

| Operation | Payload | Size | Fragmentation | Notes |
| --- | --- | --- | --- | --- |
| Read | `uint32_t flow_rate_or_pulses` | 4 B | None | Smoothed pulses/sec or raw pulses (calibration) |
| Notify | `uint32_t flow_rate_or_pulses` | 4 B | None | Interval or pulse-delta triggered |

## Overview

The Flow Sensor characteristic provides real-time flow monitoring, pulse counting, and calibration procedures. It serves as the primary interface for monitoring water flow during irrigation tasks and provides essential feedback for volume-based watering operations.

## Characteristic Details

- **UUID**: `12345678-1234-5678-1234-56789abcdef2`
- **Properties**: Read, Notify
- **Size**: 4 bytes
- **Fragmentation**: Not required
- **Notification Priority**: NORMAL (adaptive baseline 200ms)
- Provides a single 32-bit unsigned little-endian value representing either smoothed flow rate (pulses/second) or raw pulse count during calibration.

## Data Structure

```c
// Flow sensor data (uint32_t LE)
// Normal operation: smoothed pulses per second
// Calibration active: cumulative raw pulse count
uint32_t flow_rate_or_pulses;
```

## Byte Layout

| Offset | Size | Field | Description |
| --- | --- | --- | --- |
| 0 | 4 | flow_rate_or_pulses | Smoothed flow rate (pps) OR raw pulse count (calibration) |

## Data Interpretation

Interpretation depends on calibration state:

| Mode | Meaning | Notes |
| --- | --- | --- |
| Normal | Smoothed pulses per second (pps) | 2-sample average updated at 10 s intervals; may hold previous value between calculations |
| Calibration active | Raw cumulative pulse count | Direct snapshot of internal counter (32-bit wraps naturally) |
| Idle / no pulses | 0 | Reported when no pulses counted in recent window |

Smoothed flow rate calculation window: 10 s interval with minimum 5 pulses; low pulse situations (<5) still calculate but flagged internally as potentially noisy.

## Flow Sensor Configuration

### Calibration & Hardware Parameters (Implemented)

- **Default pulses per liter (Devicetree)**: Value comes from `sensor_config.flow-calibration`. On `arduino_nano_33_ble.overlay` this is 750 pulses/L; if the property is missing the firmware falls back to 450. `DEFAULT_PULSES_PER_LITER` (750) remains the reset target used by the watering subsystem.
- **Runtime calibration range**: 100-10000 pulses/L (writes outside rejected).
- **Debounce**: Taken from `sensor_config.debounce-ms` (2 ms on the main hardware overlay, 5 ms fallback when unset).
- **Minimum pulses for stable rate**: 5 (else still computed but considered low flow). No fixed pps detection threshold; algorithm relies on pulse deltas.
- **Update window**: 10 s (was 5 s historically; doc corrected to current implementation).
- **Notification triggers**: Total pulse count +100 or 30 s since the last send. Work submissions are queued every 25 pulses, so extremely low flows (<25 pulses over long periods) can stretch the effective interval beyond 30 s.

### Flow Rate Conversion (Client-Side Helpers)

```c
static inline float pulses_per_second_to_ml_per_sec(uint32_t pps, uint32_t pulses_per_liter) {
    return (pulses_per_liter == 0) ? 0.0f : (float)pps * 1000.0f / (float)pulses_per_liter;
}
static inline float pulses_per_second_to_l_per_min(uint32_t pps, uint32_t pulses_per_liter) {
    return pulses_per_second_to_ml_per_sec(pps, pulses_per_liter) * 60.0f / 1000.0f;
}
```

## Operations

### Read Operations

Read returns the current smoothed pps (or raw pulses during calibration). Because the smoothing window updates every 10 s, sequential reads within the window may repeat the same value even while low-rate pulses accumulate.

Examples (little-endian encoding shown):

| Scenario | Value | Bytes |
| --- | --- | --- |
| No flow | 0 | `00 00 00 00` |
| Moderate flow (~12 pps) | 12 | `0C 00 00 00` |
| Higher flow (125 pps) | 125 | `7D 00 00 00` |
| Calibration raw pulses (e.g., after 342 pulses) | 342 | `56 01 00 00` |

### Notification Operations

Flow notifications are emitted under two conditions:

1. Significant pulse accumulation since last notify (>=100 additional pulses total)
2. Fallback timeout interval (>=30 s since last notify)

This conservative strategy intentionally reduces BLE traffic to prevent stack overload (earlier more aggressive strategy was deprecated). Adaptive NORMAL priority throttling still applies.

## Internal Behavior (Implemented)

- Interrupt driven pulse counting with a debounce interval sourced from Devicetree (2 ms on the shipping overlay, 5 ms fallback when unspecified).
- Work handler computes rate every 10 s using pulse delta / elapsed ms, then 2-sample averaging.
- Notifications dispatched only when thresholds met (>=100 pulses delta or >=30 s).
- During active watering tasks, notifications also update per-channel statistics via `bt_irrigation_update_statistics_from_flow()`. The conversion currently uses the Devicetree calibration constant (`FLOW_CALIB_DT`, 750 by default) rather than the live BLE calibration value.

## Integration with Volume-Based Watering

### Volume Tracking Algorithm

```c
struct volume_tracker {
    uint32_t target_volume_ml;      // Target volume for task
    uint32_t current_volume_ml;     // Accumulated volume
    uint32_t last_pulse_count;      // Last pulse reading
    uint32_t pulses_per_liter;      // Calibration factor
    uint32_t start_time;            // Task start timestamp
    bool volume_reached;            // Target reached flag
};

void update_volume_tracking(struct volume_tracker *tracker, uint32_t current_pulses) {
    uint32_t pulse_delta = current_pulses - tracker->last_pulse_count;
    float volume_delta_ml = ((float)pulse_delta / (float)tracker->pulses_per_liter) * 1000.0f;
    
    tracker->current_volume_ml += (uint32_t)volume_delta_ml;
    tracker->last_pulse_count = current_pulses;
    
    if (tracker->current_volume_ml >= tracker->target_volume_ml) {
        tracker->volume_reached = true;
    }
}
```

## Error Detection and Handling

### Alarm & Anomaly Detection (External Module)

Actual anomaly detection (no-flow, unexpected flow) occurs in `watering_monitor.c` using:

- Stall / never-started logic (5 s start, 3 s stall watchdog)
- Unexpected flow threshold: 10 accumulated pulses when no valves active (`UNEXPECTED_FLOW_THRESHOLD`)

No inline high/low pps alarm thresholds are currently enforced in the flow sensor module (older examples removed).

## Implementation Examples

### Web Bluetooth (JavaScript)

```javascript
class FlowSensorMonitor {
    constructor(characteristic) {
        this.characteristic = characteristic;
        this.pulsesPerLiter = 750; // Default calibration
        this.flowHistory = [];
        this.maxHistorySize = 100;
        
        this.characteristic.addEventListener('characteristicvaluechanged', 
                                          this.handleFlowUpdate.bind(this));
    }

    async startMonitoring() {
        try {
            await this.characteristic.startNotifications();
            console.log('Flow sensor monitoring started');
            
            // Get initial reading
            const value = await this.characteristic.readValue();
            this.handleFlowData(value);
        } catch (error) {
            console.error('Failed to start flow monitoring:', error);
        }
    }

    handleFlowUpdate(event) {
        this.handleFlowData(event.target.value);
    }

    handleFlowData(dataView) {
        const pulsesPerSecond = dataView.getUint32(0, true); // Little-endian
        const timestamp = Date.now();
        
        // Calculate flow rate in ml/second
        const flowRateMLPerSec = this.pulsesPerLiter > 0 ? 
            (pulsesPerSecond / (this.pulsesPerLiter / 1000)) : 0;
        
        // Calculate flow rate in L/min for display
        const flowRateLPerMin = (flowRateMLPerSec * 60) / 1000;
        
        const flowData = {
            timestamp: timestamp,
            pulsesPerSecond: pulsesPerSecond,
            flowRateMLPerSec: flowRateMLPerSec,
            flowRateLPerMin: flowRateLPerMin
        };
        
        // Add to history
        this.flowHistory.push(flowData);
        if (this.flowHistory.length > this.maxHistorySize) {
            this.flowHistory.shift();
        }
        
        // Update UI
        this.updateFlowDisplay(flowData);
        
        // Check for flow conditions
        this.checkFlowConditions(flowData);
        
        console.log(`Flow: ${flowRateLPerMin.toFixed(2)} L/min (${pulsesPerSecond} pps)`);
    }

    updateFlowDisplay(flowData) {
        // Update real-time flow display
        const flowElement = document.getElementById('current-flow-rate');
        if (flowElement) {
            flowElement.textContent = `${flowData.flowRateLPerMin.toFixed(2)} L/min`;
        }
        
        // Update flow indicator
        const indicatorElement = document.getElementById('flow-indicator');
        if (indicatorElement) {
            indicatorElement.classList.toggle('flowing', flowData.pulsesPerSecond > 0);
            indicatorElement.classList.toggle('no-flow', flowData.pulsesPerSecond === 0);
        }
        
        // Update flow chart if available
        this.updateFlowChart(flowData);
    }

    updateFlowChart(flowData) {
        // Update real-time flow chart
        if (this.flowChart) {
            this.flowChart.addData(flowData.timestamp, flowData.flowRateLPerMin);
        }
    }

    checkFlowConditions(flowData) {
        const now = Date.now();
        
        // Check for no-flow condition (valve should be open)
        if (this.valveExpectedOpen && flowData.pulsesPerSecond === 0) {
            if (!this.noFlowStartTime) {
                this.noFlowStartTime = now;
            } else if (now - this.noFlowStartTime > 30000) { // 30 seconds
                this.triggerNoFlowAlarm();
            }
        } else {
            this.noFlowStartTime = null;
        }
        
        // Check for unexpected flow (no valve should be open)
        if (!this.valveExpectedOpen && flowData.pulsesPerSecond > 5) {
            this.triggerUnexpectedFlowAlarm(flowData.pulsesPerSecond);
        }
        
        // Check for abnormal flow rates
        if (flowData.pulsesPerSecond > 0) {
            if (flowData.pulsesPerSecond < 2) {
                this.triggerLowFlowWarning(flowData.pulsesPerSecond);
            } else if (flowData.pulsesPerSecond > 200) {
                this.triggerHighFlowWarning(flowData.pulsesPerSecond);
            }
        }
    }

    triggerNoFlowAlarm() {
        console.error('NO FLOW ALARM: Expected flow but none detected');
        this.showAlert('No Flow Detected', 'Water flow expected but not detected. Check for blockages or valve issues.', 'error');
    }

    triggerUnexpectedFlowAlarm(pulsesPerSecond) {
        console.error(`UNEXPECTED FLOW ALARM: ${pulsesPerSecond} pps detected`);
        this.showAlert('Unexpected Flow', `Water flow detected when no valves should be open (${pulsesPerSecond} pps). Check for leaks.`, 'error');
    }

    triggerLowFlowWarning(pulsesPerSecond) {
        console.warn(`LOW FLOW WARNING: ${pulsesPerSecond} pps`);
        this.showAlert('Low Flow Rate', `Flow rate is lower than expected (${pulsesPerSecond} pps). Check for partial blockages.`, 'warning');
    }

    triggerHighFlowWarning(pulsesPerSecond) {
        console.warn(`HIGH FLOW WARNING: ${pulsesPerSecond} pps`);
        this.showAlert('High Flow Rate', `Flow rate is higher than expected (${pulsesPerSecond} pps). Check system pressure.`, 'warning');
    }

    showAlert(title, message, type) {
        // Show alert to user (implementation depends on UI framework)
        if (this.alertCallback) {
            this.alertCallback(title, message, type);
        }
    }

    setValveExpectedState(isOpen) {
        this.valveExpectedOpen = isOpen;
        if (!isOpen) {
            this.noFlowStartTime = null;
        }
    }

    setPulsesPerLiter(pulsesPerLiter) {
        this.pulsesPerLiter = pulsesPerLiter;
        console.log(`Flow sensor calibration updated: ${pulsesPerLiter} pulses/liter`);
    }

    getAverageFlowRate(durationMs = 60000) {
        const cutoffTime = Date.now() - durationMs;
        const recentData = this.flowHistory.filter(data => data.timestamp > cutoffTime);
        
        if (recentData.length === 0) return 0;
        
        const totalFlow = recentData.reduce((sum, data) => sum + data.flowRateLPerMin, 0);
        return totalFlow / recentData.length;
    }

    getTotalVolume(startTime, endTime = Date.now()) {
        const relevantData = this.flowHistory.filter(data => 
            data.timestamp >= startTime && data.timestamp <= endTime
        );
        
        if (relevantData.length < 2) return 0;
        
        let totalVolume = 0;
        for (let i = 1; i < relevantData.length; i++) {
            const timeDelta = (relevantData[i].timestamp - relevantData[i-1].timestamp) / 1000; // seconds
            const avgFlowRate = (relevantData[i].flowRateMLPerSec + relevantData[i-1].flowRateMLPerSec) / 2;
            totalVolume += avgFlowRate * timeDelta;
        }
        
        return totalVolume; // ml
    }
}

// Usage example
async function initializeFlowSensor(service) {
    const characteristic = await service.getCharacteristic('12345678-1234-5678-1234-56789abcdef2');
    const monitor = new FlowSensorMonitor(characteristic);
    
    // Set up alert callback
    monitor.alertCallback = (title, message, type) => {
        // Show alert in UI
        showNotification(title, message, type);
    };
    
    // Start monitoring
    await monitor.startMonitoring();
    
    // Example: Set valve state for flow validation
    monitor.setValveExpectedState(true); // Valve should be open
    
    // Example: Update calibration
    monitor.setPulsesPerLiter(800); // New calibration value
    
    return monitor;
}
```

### iOS Swift (Core Bluetooth)

```swift
import CoreBluetooth

class FlowSensorManager: NSObject {
    private var characteristic: CBCharacteristic?
    private var peripheral: CBPeripheral?
    private var pulsesPerLiter: UInt32 = 750
    private var flowHistory: [FlowData] = []
    private let maxHistorySize = 100
    private var noFlowStartTime: Date?
    private var valveExpectedOpen = false
    
    struct FlowData {
        let timestamp: Date
        let pulsesPerSecond: UInt32
        let flowRateMLPerSec: Double
        let flowRateLPerMin: Double
    }
    
    func setCharacteristic(_ characteristic: CBCharacteristic, peripheral: CBPeripheral) {
        self.characteristic = characteristic
        self.peripheral = peripheral
        peripheral.setNotifyValue(true, for: characteristic)
    }
    
    func readCurrentFlow() {
        guard let characteristic = characteristic,
              let peripheral = peripheral else { return }
        
        peripheral.readValue(for: characteristic)
    }
    
    func setPulsesPerLiter(_ pulses: UInt32) {
        pulsesPerLiter = pulses
        print("Flow sensor calibration updated: \(pulses) pulses/liter")
    }
    
    func setValveExpectedState(_ isOpen: Bool) {
        valveExpectedOpen = isOpen
        if !isOpen {
            noFlowStartTime = nil
        }
    }
    
    private func processFlowData(_ data: Data) {
        guard data.count == 4 else { return }
        
        let pulsesPerSecond = data.withUnsafeBytes { $0.load(as: UInt32.self) }
        let timestamp = Date()
        
        // Calculate flow rate
        let flowRateMLPerSec = pulsesPerLiter > 0 ? 
            Double(pulsesPerSecond) / (Double(pulsesPerLiter) / 1000.0) : 0.0
        let flowRateLPerMin = (flowRateMLPerSec * 60.0) / 1000.0
        
        let flowData = FlowData(
            timestamp: timestamp,
            pulsesPerSecond: pulsesPerSecond,
            flowRateMLPerSec: flowRateMLPerSec,
            flowRateLPerMin: flowRateLPerMin
        )
        
        // Add to history
        flowHistory.append(flowData)
        if flowHistory.count > maxHistorySize {
            flowHistory.removeFirst()
        }
        
        // Update UI on main thread
        DispatchQueue.main.async {
            self.updateFlowDisplay(flowData)
        }
        
        // Check flow conditions
        checkFlowConditions(flowData)
        
        print("Flow: \(String(format: "%.2f", flowRateLPerMin)) L/min (\(pulsesPerSecond) pps)")
    }
    
    private func updateFlowDisplay(_ flowData: FlowData) {
        // Post notification for UI update
        NotificationCenter.default.post(
            name: .flowDataUpdated,
            object: nil,
            userInfo: [
                "flowData": flowData,
                "isFlowing": flowData.pulsesPerSecond > 0
            ]
        )
    }
    
    private func checkFlowConditions(_ flowData: FlowData) {
        let now = Date()
        
        // Check for no-flow condition
        if valveExpectedOpen && flowData.pulsesPerSecond == 0 {
            if noFlowStartTime == nil {
                noFlowStartTime = now
            } else if let startTime = noFlowStartTime,
                      now.timeIntervalSince(startTime) > 30.0 { // 30 seconds
                triggerNoFlowAlarm()
            }
        } else {
            noFlowStartTime = nil
        }
        
        // Check for unexpected flow
        if !valveExpectedOpen && flowData.pulsesPerSecond > 5 {
            triggerUnexpectedFlowAlarm(pulsesPerSecond: flowData.pulsesPerSecond)
        }
        
        // Check for abnormal flow rates
        if flowData.pulsesPerSecond > 0 {
            if flowData.pulsesPerSecond < 2 {
                triggerLowFlowWarning(pulsesPerSecond: flowData.pulsesPerSecond)
            } else if flowData.pulsesPerSecond > 200 {
                triggerHighFlowWarning(pulsesPerSecond: flowData.pulsesPerSecond)
            }
        }
    }
    
    private func triggerNoFlowAlarm() {
        print("NO FLOW ALARM: Expected flow but none detected")
        showAlert(title: "No Flow Detected", 
                 message: "Water flow expected but not detected. Check for blockages or valve issues.",
                 type: .error)
    }
    
    private func triggerUnexpectedFlowAlarm(pulsesPerSecond: UInt32) {
        print("UNEXPECTED FLOW ALARM: \(pulsesPerSecond) pps detected")
        showAlert(title: "Unexpected Flow",
                 message: "Water flow detected when no valves should be open (\(pulsesPerSecond) pps). Check for leaks.",
                 type: .error)
    }
    
    private func triggerLowFlowWarning(pulsesPerSecond: UInt32) {
        print("LOW FLOW WARNING: \(pulsesPerSecond) pps")
        showAlert(title: "Low Flow Rate",
                 message: "Flow rate is lower than expected (\(pulsesPerSecond) pps). Check for partial blockages.",
                 type: .warning)
    }
    
    private func triggerHighFlowWarning(pulsesPerSecond: UInt32) {
        print("HIGH FLOW WARNING: \(pulsesPerSecond) pps")
        showAlert(title: "High Flow Rate",
                 message: "Flow rate is higher than expected (\(pulsesPerSecond) pps). Check system pressure.",
                 type: .warning)
    }
    
    private func showAlert(title: String, message: String, type: AlertType) {
        DispatchQueue.main.async {
            NotificationCenter.default.post(
                name: .flowAlertTriggered,
                object: nil,
                userInfo: [
                    "title": title,
                    "message": message,
                    "type": type
                ]
            )
        }
    }
    
    func getAverageFlowRate(durationSeconds: TimeInterval = 60.0) -> Double {
        let cutoffTime = Date().addingTimeInterval(-durationSeconds)
        let recentData = flowHistory.filter { $0.timestamp > cutoffTime }
        
        guard !recentData.isEmpty else { return 0.0 }
        
        let totalFlow = recentData.reduce(0.0) { $0 + $1.flowRateLPerMin }
        return totalFlow / Double(recentData.count)
    }
    
    func getTotalVolume(startTime: Date, endTime: Date = Date()) -> Double {
        let relevantData = flowHistory.filter { 
            $0.timestamp >= startTime && $0.timestamp <= endTime 
        }
        
        guard relevantData.count >= 2 else { return 0.0 }
        
        var totalVolume = 0.0
        for i in 1..<relevantData.count {
            let timeDelta = relevantData[i].timestamp.timeIntervalSince(relevantData[i-1].timestamp)
            let avgFlowRate = (relevantData[i].flowRateMLPerSec + relevantData[i-1].flowRateMLPerSec) / 2.0
            totalVolume += avgFlowRate * timeDelta
        }
        
        return totalVolume // ml
    }
}

extension FlowSensorManager: CBPeripheralDelegate {
    func peripheral(_ peripheral: CBPeripheral, didUpdateValueFor characteristic: CBCharacteristic, error: Error?) {
        guard let data = characteristic.value else { return }
        processFlowData(data)
    }
}

enum AlertType {
    case error, warning, info
}

extension Notification.Name {
    static let flowDataUpdated = Notification.Name("flowDataUpdated")
    static let flowAlertTriggered = Notification.Name("flowAlertTriggered")
}
```

### Android Kotlin (BluetoothGatt)

```kotlin
import android.bluetooth.*
import java.util.*
import kotlin.math.abs

class FlowSensorManager(private val bluetoothGatt: BluetoothGatt) {
    companion object {
        private val FLOW_SENSOR_UUID = UUID.fromString("12345678-1234-5678-1234-56789abcdef2")
        private const val MAX_HISTORY_SIZE = 100
        private const val NO_FLOW_TIMEOUT_MS = 30000L
    }
    
    data class FlowData(
        val timestamp: Long,
        val pulsesPerSecond: Int,
        val flowRateMLPerSec: Double,
        val flowRateLPerMin: Double
    )
    
    private var flowCharacteristic: BluetoothGattCharacteristic? = null
    private var pulsesPerLiter: Int = 750
    private val flowHistory = mutableListOf<FlowData>()
    private var noFlowStartTime: Long? = null
    private var valveExpectedOpen = false
    private var flowUpdateCallback: ((FlowData) -> Unit)? = null
    private var alertCallback: ((String, String, String) -> Unit)? = null
    
    fun initialize(service: BluetoothGattService) {
        flowCharacteristic = service.getCharacteristic(FLOW_SENSOR_UUID)
        flowCharacteristic?.let { characteristic ->
            bluetoothGatt.setCharacteristicNotification(characteristic, true)
            
            // Enable notifications
            val descriptor = characteristic.getDescriptor(
                UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")
            )
            descriptor?.let {
                it.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
                bluetoothGatt.writeDescriptor(it)
            }
        }
    }
    
    fun readCurrentFlow() {
        flowCharacteristic?.let { characteristic ->
            bluetoothGatt.readCharacteristic(characteristic)
        }
    }
    
    fun setPulsesPerLiter(pulses: Int) {
        pulsesPerLiter = pulses
        println("Flow sensor calibration updated: $pulses pulses/liter")
    }
    
    fun setValveExpectedState(isOpen: Boolean) {
        valveExpectedOpen = isOpen
        if (!isOpen) {
            noFlowStartTime = null
        }
    }
    
    fun setFlowUpdateCallback(callback: (FlowData) -> Unit) {
        flowUpdateCallback = callback
    }
    
    fun setAlertCallback(callback: (String, String, String) -> Unit) {
        alertCallback = callback
    }
    
    fun handleCharacteristicChanged(characteristic: BluetoothGattCharacteristic) {
        if (characteristic.uuid == FLOW_SENSOR_UUID) {
            processFlowData(characteristic.value)
        }
    }
    
    private fun processFlowData(data: ByteArray) {
        if (data.size != 4) return
        
        // Convert little-endian bytes to uint32
        val pulsesPerSecond = ((data[3].toInt() and 0xFF) shl 24) or
                             ((data[2].toInt() and 0xFF) shl 16) or
                             ((data[1].toInt() and 0xFF) shl 8) or
                             (data[0].toInt() and 0xFF)
        
        val timestamp = System.currentTimeMillis()
        
        // Calculate flow rate
        val flowRateMLPerSec = if (pulsesPerLiter > 0) {
            pulsesPerSecond.toDouble() / (pulsesPerLiter.toDouble() / 1000.0)
        } else {
            0.0
        }
        val flowRateLPerMin = (flowRateMLPerSec * 60.0) / 1000.0
        
        val flowData = FlowData(
            timestamp = timestamp,
            pulsesPerSecond = pulsesPerSecond,
            flowRateMLPerSec = flowRateMLPerSec,
            flowRateLPerMin = flowRateLPerMin
        )
        
        // Add to history
        synchronized(flowHistory) {
            flowHistory.add(flowData)
            if (flowHistory.size > MAX_HISTORY_SIZE) {
                flowHistory.removeAt(0)
            }
        }
        
        // Update UI callback
        flowUpdateCallback?.invoke(flowData)
        
        // Check flow conditions
        checkFlowConditions(flowData)
        
        println("Flow: ${"%.2f".format(flowRateLPerMin)} L/min ($pulsesPerSecond pps)")
    }
    
    private fun checkFlowConditions(flowData: FlowData) {
        val now = System.currentTimeMillis()
        
        // Check for no-flow condition
        if (valveExpectedOpen && flowData.pulsesPerSecond == 0) {
            if (noFlowStartTime == null) {
                noFlowStartTime = now
            } else {
                noFlowStartTime?.let { startTime ->
                    if (now - startTime > NO_FLOW_TIMEOUT_MS) {
                        triggerNoFlowAlarm()
                    }
                }
            }
        } else {
            noFlowStartTime = null
        }
        
        // Check for unexpected flow
        if (!valveExpectedOpen && flowData.pulsesPerSecond > 5) {
            triggerUnexpectedFlowAlarm(flowData.pulsesPerSecond)
        }
        
        // Check for abnormal flow rates
        if (flowData.pulsesPerSecond > 0) {
            when {
                flowData.pulsesPerSecond < 2 -> triggerLowFlowWarning(flowData.pulsesPerSecond)
                flowData.pulsesPerSecond > 200 -> triggerHighFlowWarning(flowData.pulsesPerSecond)
            }
        }
    }
    
    private fun triggerNoFlowAlarm() {
        println("NO FLOW ALARM: Expected flow but none detected")
        alertCallback?.invoke(
            "No Flow Detected",
            "Water flow expected but not detected. Check for blockages or valve issues.",
            "error"
        )
    }
    
    private fun triggerUnexpectedFlowAlarm(pulsesPerSecond: Int) {
        println("UNEXPECTED FLOW ALARM: $pulsesPerSecond pps detected")
        alertCallback?.invoke(
            "Unexpected Flow",
            "Water flow detected when no valves should be open ($pulsesPerSecond pps). Check for leaks.",
            "error"
        )
    }
    
    private fun triggerLowFlowWarning(pulsesPerSecond: Int) {
        println("LOW FLOW WARNING: $pulsesPerSecond pps")
        alertCallback?.invoke(
            "Low Flow Rate",
            "Flow rate is lower than expected ($pulsesPerSecond pps). Check for partial blockages.",
            "warning"
        )
    }
    
    private fun triggerHighFlowWarning(pulsesPerSecond: Int) {
        println("HIGH FLOW WARNING: $pulsesPerSecond pps")
        alertCallback?.invoke(
            "High Flow Rate",
            "Flow rate is higher than expected ($pulsesPerSecond pps). Check system pressure.",
            "warning"
        )
    }
    
    fun getAverageFlowRate(durationMs: Long = 60000L): Double {
        val cutoffTime = System.currentTimeMillis() - durationMs
        val recentData = synchronized(flowHistory) {
            flowHistory.filter { it.timestamp > cutoffTime }
        }
        
        return if (recentData.isNotEmpty()) {
            recentData.map { it.flowRateLPerMin }.average()
        } else {
            0.0
        }
    }
    
    fun getTotalVolume(startTime: Long, endTime: Long = System.currentTimeMillis()): Double {
        val relevantData = synchronized(flowHistory) {
            flowHistory.filter { it.timestamp in startTime..endTime }
        }
        
        if (relevantData.size < 2) return 0.0
        
        var totalVolume = 0.0
        for (i in 1 until relevantData.size) {
            val timeDelta = (relevantData[i].timestamp - relevantData[i-1].timestamp) / 1000.0 // seconds
            val avgFlowRate = (relevantData[i].flowRateMLPerSec + relevantData[i-1].flowRateMLPerSec) / 2.0
            totalVolume += avgFlowRate * timeDelta
        }
        
        return totalVolume // ml
    }
    
    fun getFlowHistory(): List<FlowData> {
        return synchronized(flowHistory) {
            flowHistory.toList()
        }
    }
}
```

## Testing and Validation

### Test Cases

1. **Idle (no pulses)**: Expect `00 00 00 00`.
2. **Moderate flow**: Generate pulses to reach stable ~12 pps; expect `0C 00 00 00` after next 10 s window.
3. **High flow**: Sustained >100 pulses over 10 s; expect corresponding pps value.
4. **Calibration active**: Start calibration; observe monotonically increasing raw pulse count (not pps) in notifications.
5. **Notification delta trigger**: Produce exactly 100 new pulses quickly; expect notification even if 30 s interval not elapsed.
6. **Interval trigger**: No significant pulse delta; after 30 s expect periodic notification (value likely unchanged).

### Performance Testing

1. **Notification Policy**: Confirm no more than one notification per <100 pulse increment unless 30 s elapsed.
2. **Calibration Switch**: Enter calibration; confirm payload switches to raw pulse counter immediately.
3. **Persistence**: Power cycle and verify calibration value restored from NVS (within valid range) else default.
4. **Debounce Efficacy**: Inject synthetic bounce pulses <5 ms apart; ensure they are rejected.
5. **Statistics Update**: During active task, verify channel statistics volumes increase consistently with pulse count.

## Related Characteristics

- **Valve Control (def1)**: Triggers watering tasks influencing flow.
- **Current Task Status (deff)**: Tracks accumulated volume; uses pulse data indirectly.
- **Calibration Management (defb)**: Activates calibration mode (raw pulses reported).
- **System Configuration (def6)**: Provides current pulses-per-liter calibration.
- **Alarm Status (defa)**: Conveys no-flow / unexpected-flow alarms (derived from monitor module).

## Best Practices

1. Enable notifications to receive periodic updates without polling.
2. Use ATT errors / alarm characteristic rather than assuming pps thresholds for faults.
3. Convert pps to volume client-side only when you need user-facing units (liters/min, ml/s).
4. Calibrate within the 100-10000 pulses/L range; persist handled by firmware.
5. Expect sparse notifications-design UI to interpolate smoothly, not assume 5-10 Hz updates.
6. During calibration ignore pps conversions (value is raw pulses).
7. Treat unchanged values across multiple notifications as normal (window not elapsed or no new pulses).
