# System Status Characteristic (UUID: def3)

> Operation Summary
| Operation | Payload | Size | Fragmentation | Notes |
|-----------|---------|------|---------------|-------|
| Read | `uint8_t status` | 1 B | None | Encoded bit/enum system health snapshot |
| Notify | `uint8_t status` | 1 B | None | On significant system state transitions |

## Overview

The System Status characteristic exposes the firmware-wide state machine that `watering_monitor.c`, `watering_tasks.c`, and `watering.c` maintain. Flow watchdogs, RTC recovery, and power-mode transitions update the shared `system_status` enum, and the BLE layer mirrors those transitions so clients can react to fault or recovery events immediately.

## Characteristic Details

- **UUID**: `12345678-1234-5678-1234-56789abcdef3`
- **Properties**: Read, Notify
- **Size**: 1 byte
- **Fragmentation**: Not required
- **Notification Priority**: HIGH (adaptive baseline 50ms via `NOTIFY_PRIORITY_HIGH`)
- Periodic re-notification every 30s only while status is one of: NO_FLOW, UNEXPECTED_FLOW, FAULT, RTC_ERROR, LOW_POWER, FREEZE_LOCKOUT (handled by `status_periodic_timer` once the client enables CCC).

## Data Structure

```c
// System status is transmitted as a single uint8_t enum value
typedef enum {
    WATERING_STATUS_OK = 0,
    WATERING_STATUS_NO_FLOW = 1,
    WATERING_STATUS_UNEXPECTED_FLOW = 2,
    WATERING_STATUS_FAULT = 3,
    WATERING_STATUS_RTC_ERROR = 4,
    WATERING_STATUS_LOW_POWER = 5,
    WATERING_STATUS_FREEZE_LOCKOUT = 6
} watering_status_t;
```

## Byte Layout

| Offset | Size | Field  | Description |
|--------|------|--------|-------------|
| 0      | 1    | status | System status enumeration value |

## Status Values

### WATERING_STATUS_OK (0)
- **Description**: System operating normally
- **Set By**: Default boot path (`flow_monitor_init`), anomaly recovery (`check_flow_anomalies` when pulses resume, unexpected-flow clears), exiting ultra-low-power mode, successful RTC recovery
- **Action Required**: None

### WATERING_STATUS_NO_FLOW (1)
- **Description**: Expected water flow not detected / stalled
- **Trigger Points**:
    - Active watering task has run for >5s while the total pulse count is still zero (`never_started` branch)
    - Pulse count stops increasing for more than `NO_FLOW_STALL_TIMEOUT_MS` (3000 ms) while the task remains active
- **Behaviour**: `flow_error_attempts` increments on each detection; the monitor queues a retry when possible and escalates to FAULT after `MAX_FLOW_ERROR_ATTEMPTS` (3) failures or when the retry enqueue fails
- **Recovery**: Status returns to OK as soon as pulses increment again (watchdog timestamp reset) and the paired alarm is cleared
- **Action Required**: Check for blockages, verify valve operation

### WATERING_STATUS_UNEXPECTED_FLOW (2)
- **Description**: Water flow detected with no active tasks
- **Condition (implemented)**: Raw pulse counter exceeds `UNEXPECTED_FLOW_THRESHOLD` (10 pulses) while no valves are open. The monitor resets the pulse accumulator and force-closes any channel that still reports active.
- **Recovery**: Clears after pulses fall below half the threshold (<5 pulses) and the leak subsides
- **Action Required**: Check for leaks, verify valve closure

### WATERING_STATUS_FAULT (3)
- **Description**: Escalated fault condition (e.g., repeated no-flow retries exhausted, internal error paths)
- **Trigger Points**: Flow watchdog retries exhausted, requeue failure during recovery, or explicit transitions into error recovery state
- **Behaviour**: Locks the scheduler out (`watering_state` moves to ERROR_RECOVERY) until `watering_reset_fault` or `attempt_error_recovery` clears the condition
- **Action Required**: Investigate the root cause, reset the system once the fault is corrected

### WATERING_STATUS_RTC_ERROR (4)
- **Description**: RTC failure or invalid time detection (set by RTC handling code)
- **Trigger Points**: `handle_rtc_failure` sees `MAX_RTC_ERRORS` (5) consecutive RTC failures and cannot reinitialise the hardware
- **Behaviour**: Leaves automatic scheduling paused until RTC recovers; the handler continues to retry but status stays latched until recovery succeeds
- **Action Required**: Check RTC battery, reconfigure time

### WATERING_STATUS_LOW_POWER (5)
- **Description**: Entered low power condition (set by power management logic)
- **Trigger Points**: `watering_set_power_mode(POWER_MODE_ULTRA_LOW_POWER)` places the system in ultra-low power mode; exiting that mode returns to OK
- **Action Required**: Check power supply, replace batteries

### WATERING_STATUS_FREEZE_LOCKOUT (6)
- **Description**: Anti-freeze safety lockout; irrigation blocked while ambient temperature is at/under the configured freeze threshold or environmental data is stale/unavailable.
- **Trigger Points**: `watering_tasks.c` detects BME280 temperature ≤2°C (default) or environmental data older than 10 minutes when enqueueing/starting tasks or running the scheduler.
- **Behaviour**: Blocks enqueue/start, scheduler skips tasks, raises alarm code 3; remains active until cleared.
- **Recovery**: Clears automatically after valid readings above the clear threshold (default 4°C) or fresh data resumes; sends alarm clear (code 3, data=0).
- **Action Required**: Wait for temperature to rise or restore BME280 readings; optional override not recommended.

## Status Transitions

### Transition Summary (Implemented)
| From | To | Trigger (simplified) |
|------|----|----------------------|
| OK | NO_FLOW | (a) >5s after start and pulses==0 OR (b) pulses stalled >3s during active task |
| NO_FLOW | OK | Pulses resume (flow count increases) |
| OK | UNEXPECTED_FLOW | Total pulses exceed `UNEXPECTED_FLOW_THRESHOLD` (10) while no valves active |
| UNEXPECTED_FLOW | OK | Pulses drop below ~half threshold (cleared condition) |
| ANY | FAULT | Escalation paths (e.g., max no-flow retries exceeded, unrecoverable internal error) |
| ANY | FREEZE_LOCKOUT | Temperature ≤ freeze threshold or environmental data stale/unavailable |
| FREEZE_LOCKOUT | OK | Temperature > clear threshold with fresh data |
| ANY (fault-like) | Periodic notify | 30s timer fires while status in fault-like set |

### Transition Conditions

Detailed pps thresholds are not used. The firmware relies on pulse accumulation and stall timing constants defined in `watering_internal.h` (`NO_FLOW_STALL_TIMEOUT_MS`, `UNEXPECTED_FLOW_THRESHOLD`, `MAX_FLOW_ERROR_ATTEMPTS`). See `watering_monitor.c` for the full state machine.

## Operations

### Read Operations

Reading the characteristic returns the current system status.

#### Normal Operation
```c
uint8_t status = WATERING_STATUS_OK; // 0
```
**Byte Representation**: `00`

#### No Flow Detected
```c
uint8_t status = WATERING_STATUS_NO_FLOW; // 1
```
**Byte Representation**: `01`

#### Unexpected Flow
```c
uint8_t status = WATERING_STATUS_UNEXPECTED_FLOW; // 2
```
**Byte Representation**: `02`

#### Freeze Lockout Active
```c
uint8_t status = WATERING_STATUS_FREEZE_LOCKOUT; // 6
```
**Byte Representation**: `06`

### Notification Operations

The characteristic automatically sends notifications when system status changes.

#### Status Change Notification
Sent immediately when system status transitions to a different state through `bt_irrigation_system_status_update` (high-priority notification path).

#### Periodic Status Notification
Sent every 30 seconds while status is in { NO_FLOW, UNEXPECTED_FLOW, FAULT, RTC_ERROR, LOW_POWER } via the `status_periodic_timer`. The timer only runs while notifications are enabled (CCC=notify).

## Error Recovery Notes

Explicit multi-step recovery routines shown in earlier drafts (close/reopen valves, forced RTC re-init blocks) are **not** part of the current BLE-facing implementation and have been removed here to avoid implying guaranteed automated repair flows. Recovery is handled across internal modules; clients should rely on:
- Status transitions back to OK
- Alarm characteristic for specific fault codes
- Diagnostics for deeper metrics

## Implementation Examples

### Web Bluetooth (JavaScript)

```javascript
class SystemStatusMonitor {
    constructor(characteristic) {
        this.characteristic = characteristic;
        this.currentStatus = null;
        this.statusHistory = [];
        this.statusCallbacks = new Map();
        
        // Status descriptions
        this.statusDescriptions = {
            0: { name: 'OK', description: 'System operating normally', severity: 'success' },
            1: { name: 'No Flow', description: 'Expected water flow not detected', severity: 'error' },
            2: { name: 'Unexpected Flow', description: 'Water flow detected unexpectedly', severity: 'error' },
            3: { name: 'System Fault', description: 'General system fault detected', severity: 'critical' },
            4: { name: 'RTC Error', description: 'Real-time clock malfunction', severity: 'warning' },
            5: { name: 'Low Power', description: 'Power supply voltage below threshold', severity: 'warning' }
        };
        
        this.characteristic.addEventListener('characteristicvaluechanged', 
                                          this.handleStatusUpdate.bind(this));
    }

    async startMonitoring() {
        try {
            await this.characteristic.startNotifications();
            console.log('System status monitoring started');
            
            // Get initial status
            const value = await this.characteristic.readValue();
            this.handleStatusData(value);
        } catch (error) {
            console.error('Failed to start status monitoring:', error);
        }
    }

    handleStatusUpdate(event) {
        this.handleStatusData(event.target.value);
    }

    handleStatusData(dataView) {
        const statusValue = dataView.getUint8(0);
        const timestamp = Date.now();
        
        // Update current status
        const previousStatus = this.currentStatus;
        this.currentStatus = statusValue;
        
        // Add to history
        this.statusHistory.push({
            status: statusValue,
            timestamp: timestamp,
            description: this.getStatusDescription(statusValue)
        });
        
        // Limit history size
        if (this.statusHistory.length > 1000) {
            this.statusHistory.shift();
        }
        
        // Log status change
        if (previousStatus !== statusValue) {
            const statusInfo = this.statusDescriptions[statusValue];
            console.log(`System status changed: ${statusInfo.name} - ${statusInfo.description}`);
            
            // Trigger status change callbacks
            this.notifyStatusChange(statusValue, previousStatus);
        }
        
        // Update UI
        this.updateStatusDisplay(statusValue);
        
        // Handle critical conditions
        this.handleCriticalStatus(statusValue);
    }

    updateStatusDisplay(statusValue) {
        const statusInfo = this.statusDescriptions[statusValue];
        
        // Update status indicator
        const statusElement = document.getElementById('system-status');
        if (statusElement) {
            statusElement.textContent = statusInfo.name;
            statusElement.className = `status-indicator ${statusInfo.severity}`;
        }
        
        // Update status description
        const descElement = document.getElementById('status-description');
        if (descElement) {
            descElement.textContent = statusInfo.description;
        }
        
        // Update status icon
        const iconElement = document.getElementById('status-icon');
        if (iconElement) {
            iconElement.className = `status-icon ${this.getStatusIcon(statusValue)}`;
        }
    }

    getStatusIcon(statusValue) {
        const iconMap = {
            0: 'check-circle',      // OK
            1: 'exclamation-triangle', // No Flow
            2: 'exclamation-triangle', // Unexpected Flow
            3: 'times-circle',      // Fault
            4: 'clock',             // RTC Error
            5: 'battery-quarter'    // Low Power
        };
        return iconMap[statusValue] || 'question-circle';
    }

    handleCriticalStatus(statusValue) {
        const statusInfo = this.statusDescriptions[statusValue];
        
        if (statusInfo.severity === 'critical' || statusInfo.severity === 'error') {
            // Show critical alert
            this.showCriticalAlert(statusInfo.name, statusInfo.description);
            
            // Play alert sound if available
            this.playAlertSound();
            
            // Send push notification if supported
            this.sendPushNotification(statusInfo.name, statusInfo.description);
        }
    }

    showCriticalAlert(title, message) {
        // Show modal alert for critical conditions
        if (this.alertCallback) {
            this.alertCallback(title, message, 'critical');
        } else {
            // Fallback to browser alert
            alert(`${title}: ${message}`);
        }
    }

    playAlertSound() {
        // Play alert sound if audio is enabled
        if (this.audioEnabled && this.alertSound) {
            this.alertSound.play().catch(e => console.log('Could not play alert sound:', e));
        }
    }

    sendPushNotification(title, message) {
        // Send push notification if permission granted
        if ('Notification' in window && Notification.permission === 'granted') {
            new Notification(title, {
                body: message,
                icon: '/icons/alert-icon.png',
                tag: 'system-status'
            });
        }
    }

    getStatusDescription(statusValue) {
        return this.statusDescriptions[statusValue] || 
               { name: 'Unknown', description: 'Unknown status', severity: 'warning' };
    }

    onStatusChange(callback) {
        const id = Date.now() + Math.random();
        this.statusCallbacks.set(id, callback);
        return id; // Return ID for unsubscribing
    }

    offStatusChange(id) {
        this.statusCallbacks.delete(id);
    }

    notifyStatusChange(newStatus, previousStatus) {
        const statusInfo = this.getStatusDescription(newStatus);
        this.statusCallbacks.forEach(callback => {
            try {
                callback(newStatus, previousStatus, statusInfo);
            } catch (error) {
                console.error('Error in status change callback:', error);
            }
        });
    }

    getCurrentStatus() {
        return {
            value: this.currentStatus,
            description: this.getStatusDescription(this.currentStatus),
            timestamp: this.statusHistory.length > 0 ? 
                      this.statusHistory[this.statusHistory.length - 1].timestamp : null
        };
    }

    getStatusHistory(hours = 24) {
        const cutoffTime = Date.now() - (hours * 60 * 60 * 1000);
        return this.statusHistory.filter(entry => entry.timestamp > cutoffTime);
    }

    getStatusStatistics(hours = 24) {
        const history = this.getStatusHistory(hours);
        const stats = {};
        
        // Count occurrences of each status
        history.forEach(entry => {
            const statusName = this.statusDescriptions[entry.status].name;
            stats[statusName] = (stats[statusName] || 0) + 1;
        });
        
        // Calculate uptime percentage
        const okCount = stats['OK'] || 0;
        const totalCount = history.length;
        const uptimePercentage = totalCount > 0 ? (okCount / totalCount) * 100 : 100;
        
        return {
            statusCounts: stats,
            totalEvents: totalCount,
            uptimePercentage: uptimePercentage.toFixed(2)
        };
    }

    setAlertCallback(callback) {
        this.alertCallback = callback;
    }

    setAudioEnabled(enabled) {
        this.audioEnabled = enabled;
        if (enabled && !this.alertSound) {
            this.alertSound = new Audio('/sounds/alert.mp3');
        }
    }

    async requestNotificationPermission() {
        if ('Notification' in window && Notification.permission === 'default') {
            const permission = await Notification.requestPermission();
            return permission === 'granted';
        }
        return Notification.permission === 'granted';
    }
}

// Usage example
async function initializeSystemStatus(service) {
    const characteristic = await service.getCharacteristic('12345678-1234-5678-1234-56789abcdef3');
    const monitor = new SystemStatusMonitor(characteristic);
    
    // Set up alert callback
    monitor.setAlertCallback((title, message, severity) => {
        showAlert(title, message, severity);
    });
    
    // Enable audio alerts
    monitor.setAudioEnabled(true);
    
    // Request notification permission
    await monitor.requestNotificationPermission();
    
    // Set up status change handler
    monitor.onStatusChange((newStatus, previousStatus, statusInfo) => {
        console.log(`Status changed from ${previousStatus} to ${newStatus}: ${statusInfo.name}`);
        
        // Update dashboard
        updateSystemDashboard(statusInfo);
        
        // Log to analytics
        logStatusChange(newStatus, previousStatus);
    });
    
    // Start monitoring
    await monitor.startMonitoring();
    
    return monitor;
}

// Helper functions
function showAlert(title, message, severity) {
    // Implementation depends on UI framework
    const alertDiv = document.createElement('div');
    alertDiv.className = `alert alert-${severity}`;
    alertDiv.innerHTML = `<strong>${title}</strong>: ${message}`;
    document.body.appendChild(alertDiv);
    
    // Auto-remove after 10 seconds
    setTimeout(() => {
        if (alertDiv.parentNode) {
            alertDiv.parentNode.removeChild(alertDiv);
        }
    }, 10000);
}

function updateSystemDashboard(statusInfo) {
    // Update main dashboard with current status
    const dashboard = document.getElementById('system-dashboard');
    if (dashboard) {
        dashboard.setAttribute('data-status', statusInfo.severity);
    }
}

function logStatusChange(newStatus, previousStatus) {
    // Log status change for analytics
    if (window.analytics) {
        window.analytics.track('System Status Changed', {
            newStatus: newStatus,
            previousStatus: previousStatus,
            timestamp: Date.now()
        });
    }
}
```

### iOS Swift (Core Bluetooth)

```swift
import CoreBluetooth
import UserNotifications

class SystemStatusManager: NSObject {
    private var characteristic: CBCharacteristic?
    private var peripheral: CBPeripheral?
    private var currentStatus: UInt8?
    private var statusHistory: [StatusEntry] = []
    private var statusChangeCallbacks: [(UInt8, UInt8?) -> Void] = []
    
    struct StatusEntry {
        let status: UInt8
        let timestamp: Date
        let description: StatusDescription
    }
    
    struct StatusDescription {
        let name: String
        let description: String
        let severity: StatusSeverity
    }
    
    enum StatusSeverity {
        case success, warning, error, critical
    }
    
    private let statusDescriptions: [UInt8: StatusDescription] = [
        0: StatusDescription(name: "OK", description: "System operating normally", severity: .success),
        1: StatusDescription(name: "No Flow", description: "Expected water flow not detected", severity: .error),
        2: StatusDescription(name: "Unexpected Flow", description: "Water flow detected unexpectedly", severity: .error),
        3: StatusDescription(name: "System Fault", description: "General system fault detected", severity: .critical),
        4: StatusDescription(name: "RTC Error", description: "Real-time clock malfunction", severity: .warning),
        5: StatusDescription(name: "Low Power", description: "Power supply voltage below threshold", severity: .warning)
    ]
    
    func setCharacteristic(_ characteristic: CBCharacteristic, peripheral: CBPeripheral) {
        self.characteristic = characteristic
        self.peripheral = peripheral
        peripheral.setNotifyValue(true, for: characteristic)
    }
    
    func readCurrentStatus() {
        guard let characteristic = characteristic,
              let peripheral = peripheral else { return }
        
        peripheral.readValue(for: characteristic)
    }
    
    func addStatusChangeCallback(_ callback: @escaping (UInt8, UInt8?) -> Void) {
        statusChangeCallbacks.append(callback)
    }
    
    private func processStatusData(_ data: Data) {
        guard data.count == 1 else { return }
        
        let statusValue = data[0]
        let timestamp = Date()
        let previousStatus = currentStatus
        
        // Update current status
        currentStatus = statusValue
        
        // Get status description
        let statusDesc = statusDescriptions[statusValue] ?? 
            StatusDescription(name: "Unknown", description: "Unknown status", severity: .warning)
        
        // Add to history
        let entry = StatusEntry(status: statusValue, timestamp: timestamp, description: statusDesc)
        statusHistory.append(entry)
        
        // Limit history size
        if statusHistory.count > 1000 {
            statusHistory.removeFirst()
        }
        
        // Log status change
        if previousStatus != statusValue {
            print("System status changed: \(statusDesc.name) - \(statusDesc.description)")
            
            // Notify callbacks
            statusChangeCallbacks.forEach { callback in
                callback(statusValue, previousStatus)
            }
            
            // Post notification
            DispatchQueue.main.async {
                NotificationCenter.default.post(
                    name: .systemStatusChanged,
                    object: nil,
                    userInfo: [
                        "status": statusValue,
                        "previousStatus": previousStatus as Any,
                        "description": statusDesc
                    ]
                )
            }
        }
        
        // Handle critical conditions
        handleCriticalStatus(statusValue, description: statusDesc)
    }
    
    private func handleCriticalStatus(_ statusValue: UInt8, description: StatusDescription) {
        if description.severity == .critical || description.severity == .error {
            // Show local notification
            showLocalNotification(title: description.name, message: description.description)
            
            // Trigger haptic feedback
            triggerHapticFeedback()
            
            // Play alert sound
            playAlertSound()
        }
    }
    
    private func showLocalNotification(title: String, message: String) {
        let content = UNMutableNotificationContent()
        content.title = "AutoWatering Alert"
        content.subtitle = title
        content.body = message
        content.sound = .default
        
        let request = UNNotificationRequest(
            identifier: "system-status-\(Date().timeIntervalSince1970)",
            content: content,
            trigger: nil
        )
        
        UNUserNotificationCenter.current().add(request) { error in
            if let error = error {
                print("Failed to show notification: \(error)")
            }
        }
    }
    
    private func triggerHapticFeedback() {
        let impactFeedback = UIImpactFeedbackGenerator(style: .heavy)
        impactFeedback.impactOccurred()
    }
    
    private func playAlertSound() {
        // Play system alert sound
        AudioServicesPlaySystemSound(kSystemSoundID_Vibrate)
    }
    
    func getCurrentStatus() -> (value: UInt8?, description: StatusDescription?) {
        guard let status = currentStatus else { return (nil, nil) }
        return (status, statusDescriptions[status])
    }
    
    func getStatusHistory(hours: Int = 24) -> [StatusEntry] {
        let cutoffTime = Date().addingTimeInterval(-TimeInterval(hours * 3600))
        return statusHistory.filter { $0.timestamp > cutoffTime }
    }
    
    func getStatusStatistics(hours: Int = 24) -> [String: Any] {
        let history = getStatusHistory(hours: hours)
        var statusCounts: [String: Int] = [:]
        
        // Count occurrences
        for entry in history {
            let statusName = entry.description.name
            statusCounts[statusName] = (statusCounts[statusName] ?? 0) + 1
        }
        
        // Calculate uptime
        let okCount = statusCounts["OK"] ?? 0
        let totalCount = history.count
        let uptimePercentage = totalCount > 0 ? Double(okCount) / Double(totalCount) * 100 : 100
        
        return [
            "statusCounts": statusCounts,
            "totalEvents": totalCount,
            "uptimePercentage": uptimePercentage
        ]
    }
}

extension SystemStatusManager: CBPeripheralDelegate {
    func peripheral(_ peripheral: CBPeripheral, didUpdateValueFor characteristic: CBCharacteristic, error: Error?) {
        guard let data = characteristic.value else { return }
        processStatusData(data)
    }
}

extension Notification.Name {
    static let systemStatusChanged = Notification.Name("systemStatusChanged")
}
```

### Android Kotlin (BluetoothGatt)

```kotlin
import android.bluetooth.*
import android.content.Context
import android.app.NotificationChannel
import android.app.NotificationManager
import android.os.VibrationEffect
import android.os.Vibrator
import androidx.core.app.NotificationCompat
import java.util.*

class SystemStatusManager(
    private val context: Context,
    private val bluetoothGatt: BluetoothGatt
) {
    companion object {
        private val SYSTEM_STATUS_UUID = UUID.fromString("12345678-1234-5678-1234-56789abcdef3")
        private const val NOTIFICATION_CHANNEL_ID = "system_status_alerts"
        private const val MAX_HISTORY_SIZE = 1000
    }
    
    data class StatusEntry(
        val status: Int,
        val timestamp: Long,
        val description: StatusDescription
    )
    
    data class StatusDescription(
        val name: String,
        val description: String,
        val severity: StatusSeverity
    )
    
    enum class StatusSeverity {
        SUCCESS, WARNING, ERROR, CRITICAL
    }
    
    private var statusCharacteristic: BluetoothGattCharacteristic? = null
    private var currentStatus: Int? = null
    private val statusHistory = mutableListOf<StatusEntry>()
    private val statusChangeCallbacks = mutableListOf<(Int, Int?) -> Unit>()
    private val notificationManager = context.getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
    private val vibrator = context.getSystemService(Context.VIBRATOR_SERVICE) as Vibrator
    
    private val statusDescriptions = mapOf(
        0 to StatusDescription("OK", "System operating normally", StatusSeverity.SUCCESS),
        1 to StatusDescription("No Flow", "Expected water flow not detected", StatusSeverity.ERROR),
        2 to StatusDescription("Unexpected Flow", "Water flow detected unexpectedly", StatusSeverity.ERROR),
        3 to StatusDescription("System Fault", "General system fault detected", StatusSeverity.CRITICAL),
        4 to StatusDescription("RTC Error", "Real-time clock malfunction", StatusSeverity.WARNING),
        5 to StatusDescription("Low Power", "Power supply voltage below threshold", StatusSeverity.WARNING)
    )
    
    init {
        createNotificationChannel()
    }
    
    fun initialize(service: BluetoothGattService) {
        statusCharacteristic = service.getCharacteristic(SYSTEM_STATUS_UUID)
        statusCharacteristic?.let { characteristic ->
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
    
    fun readCurrentStatus() {
        statusCharacteristic?.let { characteristic ->
            bluetoothGatt.readCharacteristic(characteristic)
        }
    }
    
    fun addStatusChangeCallback(callback: (Int, Int?) -> Unit) {
        statusChangeCallbacks.add(callback)
    }
    
    fun handleCharacteristicChanged(characteristic: BluetoothGattCharacteristic) {
        if (characteristic.uuid == SYSTEM_STATUS_UUID) {
            processStatusData(characteristic.value)
        }
    }
    
    private fun processStatusData(data: ByteArray) {
        if (data.size != 1) return
        
        val statusValue = data[0].toInt() and 0xFF
        val timestamp = System.currentTimeMillis()
        val previousStatus = currentStatus
        
        // Update current status
        currentStatus = statusValue
        
        // Get status description
        val statusDesc = statusDescriptions[statusValue] ?: 
            StatusDescription("Unknown", "Unknown status", StatusSeverity.WARNING)
        
        // Add to history
        synchronized(statusHistory) {
            statusHistory.add(StatusEntry(statusValue, timestamp, statusDesc))
            if (statusHistory.size > MAX_HISTORY_SIZE) {
                statusHistory.removeAt(0)
            }
        }
        
        // Log status change
        if (previousStatus != statusValue) {
            println("System status changed: ${statusDesc.name} - ${statusDesc.description}")
            
            // Notify callbacks
            statusChangeCallbacks.forEach { callback ->
                try {
                    callback(statusValue, previousStatus)
                } catch (e: Exception) {
                    println("Error in status change callback: ${e.message}")
                }
            }
        }
        
        // Handle critical conditions
        handleCriticalStatus(statusValue, statusDesc)
    }
    
    private fun handleCriticalStatus(statusValue: Int, description: StatusDescription) {
        if (description.severity == StatusSeverity.CRITICAL || description.severity == StatusSeverity.ERROR) {
            // Show notification
            showNotification(description.name, description.description)
            
            // Trigger vibration
            triggerVibration()
        }
    }
    
    private fun createNotificationChannel() {
        val channel = NotificationChannel(
            NOTIFICATION_CHANNEL_ID,
            "System Status Alerts",
            NotificationManager.IMPORTANCE_HIGH
        ).apply {
            description = "Notifications for critical system status changes"
            enableVibration(true)
            setShowBadge(true)
        }
        
        notificationManager.createNotificationChannel(channel)
    }
    
    private fun showNotification(title: String, message: String) {
        val notification = NotificationCompat.Builder(context, NOTIFICATION_CHANNEL_ID)
            .setSmallIcon(android.R.drawable.ic_dialog_alert)
            .setContentTitle("AutoWatering Alert")
            .setContentText("$title: $message")
            .setStyle(NotificationCompat.BigTextStyle().bigText(message))
            .setPriority(NotificationCompat.PRIORITY_HIGH)
            .setAutoCancel(true)
            .build()
        
        notificationManager.notify(System.currentTimeMillis().toInt(), notification)
    }
    
    private fun triggerVibration() {
        if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.O) {
            vibrator.vibrate(VibrationEffect.createOneShot(500, VibrationEffect.DEFAULT_AMPLITUDE))
        } else {
            @Suppress("DEPRECATION")
            vibrator.vibrate(500)
        }
    }
    
    fun getCurrentStatus(): Pair<Int?, StatusDescription?> {
        val status = currentStatus
        return Pair(status, status?.let { statusDescriptions[it] })
    }
    
    fun getStatusHistory(hours: Int = 24): List<StatusEntry> {
        val cutoffTime = System.currentTimeMillis() - (hours * 3600 * 1000L)
        return synchronized(statusHistory) {
            statusHistory.filter { it.timestamp > cutoffTime }
        }
    }
    
    fun getStatusStatistics(hours: Int = 24): Map<String, Any> {
        val history = getStatusHistory(hours)
        val statusCounts = mutableMapOf<String, Int>()
        
        // Count occurrences
        history.forEach { entry ->
            val statusName = entry.description.name
            statusCounts[statusName] = (statusCounts[statusName] ?: 0) + 1
        }
        
        // Calculate uptime
        val okCount = statusCounts["OK"] ?: 0
        val totalCount = history.size
        val uptimePercentage = if (totalCount > 0) {
            (okCount.toDouble() / totalCount.toDouble()) * 100
        } else {
            100.0
        }
        
        return mapOf(
            "statusCounts" to statusCounts,
            "totalEvents" to totalCount,
            "uptimePercentage" to uptimePercentage
        )
    }
}
```

## Testing and Validation

### Test Cases

1. **Normal Operation**
    - Expected: `00` (WATERING_STATUS_OK)
    - Condition: All systems functioning normally

2. **No Flow Condition**
    - Expected: `01` (WATERING_STATUS_NO_FLOW)
    - Condition: Start a watering task and block pulses so that either (a) the task runs >5s without any pulses or (b) pulses stall for >3s

3. **Unexpected Flow**
    - Expected: `02` (WATERING_STATUS_UNEXPECTED_FLOW)
    - Condition: Inject >10 pulses after tasks finish to trip `UNEXPECTED_FLOW_THRESHOLD`

4. **System Fault**
   - Expected: `03` (WATERING_STATUS_FAULT)
   - Condition: Hardware failure or critical error

5. **RTC Error**
   - Expected: `04` (WATERING_STATUS_RTC_ERROR)
   - Condition: Real-time clock malfunction

6. **Low Power**
   - Expected: `05` (WATERING_STATUS_LOW_POWER)
   - Condition: Power supply below threshold

### Performance Testing

1. **Status Change Notifications**
    - Trigger transitions (simulate no-flow & recovery) and confirm single notification per change plus 30s periodic reminders during fault-like states.

2. **Periodic Timer**
    - While in any fault-like status, ensure periodic 30s notifications continue; stopping when returning to OK.

3. **Escalation Path**
    - Force repeated no-flow stall attempts until fault escalation; verify FAULT status then periodic reminders.

Enhanced diagnostic bitmaps from `enhanced_system_status_info_t` remain internal and are not exported over BLE yet; this characteristic will stay a single-byte enum until that integration completes.

## Related Characteristics

- **Flow Sensor (def2)**: Flow-related status conditions
- **Valve Control (def1)**: Valve operation status
- **Alarm Status (defa)**: Detailed alarm information
- **Diagnostics (defd)**: System health metrics
- **RTC Configuration (def9)**: Time-related status

## Best Practices

1. Enable notifications and persist latest status in UI.
2. Treat repeated periodic notifications as a heartbeat during degraded states (not spam).
3. Couple status changes with Alarm characteristic for rich context.
4. Avoid assuming fixed pps thresholds; rely on provided status codes.
5. Log timestamped transitions to compute uptime and MTTR.
6. Provide user-facing guidance per status code (e.g., check supply, inspect leaks).
7. Do not poll aggressively; let notifications drive updates.
