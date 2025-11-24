# Valve Control Characteristic (UUID: def1)

> Operation Summary
| Operation | Payload Struct | Size | Fragmentation | Notes |
|-----------|----------------|------|---------------|-------|
| Read | `struct valve_control_data` | 4 B | None | Last valve (channel or master) state snapshot |
| Write | `struct valve_control_data` | 4 B | None | Creates task (channel) or toggles master valve (channel_id=0xFF) |
| Notify | `struct valve_control_data` | 4 B | None | Emitted only on actual valve state change |

## Overview

The Valve Control characteristic provides manual valve operation and master valve control functionality. It serves as the primary interface for direct valve manipulation, task creation, and real-time valve status monitoring.

## Characteristic Details

- **UUID**: `12345678-1234-5678-1234-56789abcdef1`
- **Properties**: Read, Write, Notify
- **Size**: 4 bytes
- **Fragmentation**: Not required
- **Notification Priority**: HIGH (adaptive throttle baseline 50ms)
- Writes must provide the full 4-byte structure (partial writes rejected per length check).
- No immediate "acceptance" notification is sent on a successful write; only actual valve state transitions (including master valve) generate notifications.

## Data Structure

```c
struct valve_control_data {
    uint8_t  channel_id;   // 0-7: target channel
    uint8_t  task_type;    // 0=duration [min], 1=volume [L] (for task creation)
                           // Also used for status: 0=inactive, 1=active (for notifications)
    uint16_t value;        // minutes (task_type=0) or liters (task_type=1)
                           // For status notifications: 0 (no value)
} __packed;               // TOTAL SIZE: 4 bytes
```

## Byte Layout

| Offset | Size | Field      | Description |
|--------|------|------------|-------------|
| 0      | 1    | channel_id | Channel identifier (0-7) |
| 1      | 1    | task_type  | Task type or status indicator |
| 2      | 2    | value      | Duration in minutes or volume in liters |

## Field Descriptions

### channel_id (uint8_t)
- **Normal Channels**: 0-7 (0 to WATERING_CHANNELS_COUNT-1)
- **Master Valve**: `0xFF` (special control / status)
- **Sentinel on Connect**: Reads may briefly return `channel_id=0xFF` with `task_type=0` right after (re)connect before any valve activity - this is an initialization sentinel, not an error.
- Any write with a value >= WATERING_CHANNELS_COUNT that is **not 0xFF** is rejected (ATT error returned; no custom error payload).

### task_type (uint8_t)
Context dependent:
- **Write (normal channel)**: `0` = duration task (value=minutes), `1` = volume task (value=liters)
- **Write (master valve, channel_id=0xFF)**: `0` = close master valve, `1` = open master valve (value ignored)
- **Notify / Read (status)**: `0` = inactive/closed, `1` = active/open (value always 0)

### value (uint16_t)
- **Duration task**: Minutes (1-1440)
- **Volume task**: Liters (1-1000) (unit is liters, not ml)
- **Master valve write**: Ignored
- **Status notifications & reads**: 0 (not used)

## Operations

### Write Operations (Task Creation)

#### Duration-Based Watering
Creates a new watering task with a specified duration.

**Example**: Water channel 2 for 30 minutes
```c
struct valve_control_data task = {
    .channel_id = 2,
    .task_type = 0,    // Duration mode
    .value = 30        // 30 minutes
};
```

**Byte Representation**: `02 00 1E 00`

#### Volume-Based Watering
Creates a new watering task with a specified volume target.

**Example**: Water channel 0 with 5 liters
```c
struct valve_control_data task = {
    .channel_id = 0,
    .task_type = 1,    // Volume mode
    .value = 5         // 5 liters
};
```

**Byte Representation**: `00 01 05 00`

### Read Operations (Status Query)

Reads return the last valve status snapshot stored in the service buffer:
- If a channel changed state last, its channel_id (0-7) and active flag (task_type 0/1) appear.
- If the master valve changed last, `channel_id=0xFF` with task_type 0/1.
- Immediately after (re)connect before any valve events: `channel_id=0xFF`, `task_type=0`, `value=0` (sentinel / no active valve yet).

### Notification Operations (Status Updates)

Notifications are emitted only on actual valve state transitions (channel or master valve open/close). Writes that only queue a task do not trigger a notification until the valve physically changes state.

#### Channel Activation Notification
Sent when a channel valve opens and watering begins.

**Example**: Channel 1 becomes active
```c
struct valve_control_data notification = {
    .channel_id = 1,
    .task_type = 1,    // Active
    .value = 0         // No value
};
```

#### Channel Deactivation Notification
Sent when a channel valve closes and watering stops.

**Example**: Channel 1 becomes inactive
```c
struct valve_control_data notification = {
    .channel_id = 1,
    .task_type = 0,    // Inactive
    .value = 0         // No value
};
```

## Master Valve Integration

Master valve control is exposed through the same characteristic using `channel_id=0xFF`:
- `task_type=0` close, `task_type=1` open (value ignored)
- On success a notification reflects the resulting state (`channel_id=0xFF`, `task_type` = 0/1, `value=0`).
- If master valve auto-management is enabled or the feature is disabled at system level, the write is rejected (`BT_ATT_ERR_WRITE_NOT_PERMITTED` emitted after `master_valve_manual_*` returns `WATERING_ERROR_BUSY` / `WATERING_ERROR_CONFIG`) - no custom error notification is produced.

Timing (pre-delay, overlap grace, post-delay) is managed internally (see System Configuration characteristic). These timings are not encoded in this characteristic payload.

## Error Handling

This characteristic does **not** send custom error payloads. All validation / system failures are surfaced via standard GATT ATT error codes returned from the write request. No notification follows a failed write.

| Condition | Code Path | ATT Error Returned |
|-----------|-----------|--------------------|
| Partial / wrong length write | Length checks | `BT_ATT_ERR_INVALID_ATTRIBUTE_LEN` / `BT_ATT_ERR_INVALID_OFFSET` |
| Invalid channel (not 0-7 or 0xFF) | Channel check | `BT_ATT_ERR_VALUE_NOT_ALLOWED` |
| Invalid task_type (>1 for normal or >1 master) | Task type check | `BT_ATT_ERR_VALUE_NOT_ALLOWED` |
| Zero value | Value check | `BT_ATT_ERR_VALUE_NOT_ALLOWED` |
| Duration > 1440 min | Range check | `BT_ATT_ERR_VALUE_NOT_ALLOWED` |
| Volume > 1000 L | Range check | `BT_ATT_ERR_VALUE_NOT_ALLOWED` |
| Master valve manual control rejected (auto mgmt / disabled) | `master_valve_manual_*` | `BT_ATT_ERR_WRITE_NOT_PERMITTED` |
| Queue full / busy / hardware error during task creation | Task add mapping | `BT_ATT_ERR_WRITE_NOT_PERMITTED` / `BT_ATT_ERR_UNLIKELY` |

Client applications should interpret these ATT errors and present appropriate feedback. A successful write that queues a task may still lead to a later system-level failure (e.g. hardware) which will surface through other status/ alarm characteristics, not through this characteristic.

## Implementation Examples

### Web Bluetooth (JavaScript)

```javascript
class ValveController {
    constructor(characteristic) {
        this.characteristic = characteristic;
        this.characteristic.addEventListener('characteristicvaluechanged', 
                                          this.handleNotification.bind(this));
    }

    async startDurationWatering(channelId, minutes) {
        const data = new Uint8Array(4);
        data[0] = channelId;
        data[1] = 0; // Duration mode
        data[2] = minutes & 0xFF;
        data[3] = (minutes >> 8) & 0xFF;
        
        try {
            await this.characteristic.writeValue(data);
            console.log(`Started ${minutes}min watering on channel ${channelId}`);
        } catch (error) {
            console.error('Failed to start watering:', error);
        }
    }

    async startVolumeWatering(channelId, liters) {
        const data = new Uint8Array(4);
        data[0] = channelId;
        data[1] = 1; // Volume mode
        data[2] = liters & 0xFF;
        data[3] = (liters >> 8) & 0xFF;
        
        try {
            await this.characteristic.writeValue(data);
            console.log(`Started ${liters}L watering on channel ${channelId}`);
        } catch (error) {
            console.error('Failed to start watering:', error);
        }
    }

    async getValveStatus() {
        try {
            const value = await this.characteristic.readValue();
            const channelId = value.getUint8(0);
            const taskType = value.getUint8(1);
            const valueField = value.getUint16(2, true); // Little-endian
            
            return {
                channelId: channelId,
                isActive: taskType === 1,
                value: valueField
            };
        } catch (error) {
            console.error('Failed to read valve status:', error);
            return null;
        }
    }

    handleNotification(event) {
        const value = event.target.value;
        const channelId = value.getUint8(0);
        const taskType = value.getUint8(1);
        const valueField = value.getUint16(2, true);
        
        if (channelId === 0xFF) {
            const masterValveOpen = taskType === 1;
            console.log(`Master valve is now ${masterValveOpen ? 'OPEN' : 'CLOSED'}`);
            this.updateMasterValveStatus(masterValveOpen);
            return;
        }

        const isActive = taskType === 1;
        console.log(`Channel ${channelId} is now ${isActive ? 'ACTIVE' : 'INACTIVE'}`);

        // Trigger UI update
        this.updateChannelStatus(channelId, isActive);
    }

    updateChannelStatus(channelId, isActive) {
        // Update UI to reflect valve status change
        const channelElement = document.getElementById(`channel-${channelId}`);
        if (channelElement) {
            channelElement.classList.toggle('active', isActive);
            channelElement.classList.toggle('inactive', !isActive);
        }
    }

    updateMasterValveStatus(isOpen) {
        const masterValveElement = document.getElementById('master-valve-state');
        if (masterValveElement) {
            masterValveElement.textContent = isOpen ? 'OPEN' : 'CLOSED';
        }
    }
}

// Usage example
async function initializeValveControl(service) {
    const characteristic = await service.getCharacteristic('12345678-1234-5678-1234-56789abcdef1');
    await characteristic.startNotifications();
    
    const controller = new ValveController(characteristic);
    
    // Start 15-minute watering on channel 0
    await controller.startDurationWatering(0, 15);
    
    // Start 3-liter watering on channel 2
    await controller.startVolumeWatering(2, 3);
    
    // Check current status
    const status = await controller.getValveStatus();
    console.log('Current valve status:', status);
}
### Android Kotlin (BluetoothGatt)

```kotlin
import android.bluetooth.*
import java.util.*

class ValveControlManager(private val bluetoothGatt: BluetoothGatt) {
    companion object {
        private val VALVE_CONTROL_UUID = UUID.fromString("12345678-1234-5678-1234-56789abcdef1")
    }
    
    private var valveCharacteristic: BluetoothGattCharacteristic? = null
    
    fun initialize(service: BluetoothGattService) {
        valveCharacteristic = service.getCharacteristic(VALVE_CONTROL_UUID)
        valveCharacteristic?.let { characteristic ->
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
    
    fun startDurationWatering(channelId: Int, minutes: Int) {
        valveCharacteristic?.let { characteristic ->
            val data = ByteArray(4)
            data[0] = channelId.toByte()
            data[1] = 0 // Duration mode
            data[2] = (minutes and 0xFF).toByte()
            data[3] = ((minutes shr 8) and 0xFF).toByte()
            
            characteristic.value = data
            bluetoothGatt.writeCharacteristic(characteristic)
            println("Started ${minutes}min watering on channel $channelId")
        }
    }
    
    fun startVolumeWatering(channelId: Int, liters: Int) {
        valveCharacteristic?.let { characteristic ->
            val data = ByteArray(4)
            data[0] = channelId.toByte()
            data[1] = 1 // Volume mode
            data[2] = (liters and 0xFF).toByte()
            data[3] = ((liters shr 8) and 0xFF).toByte()
            
            characteristic.value = data
            bluetoothGatt.writeCharacteristic(characteristic)
            println("Started ${liters}L watering on channel $channelId")
        }
    }
    
    fun readValveStatus() {
        valveCharacteristic?.let { characteristic ->
            bluetoothGatt.readCharacteristic(characteristic)
        }
    }
    
    fun handleCharacteristicChanged(characteristic: BluetoothGattCharacteristic) {
        if (characteristic.uuid == VALVE_CONTROL_UUID) {
            val data = characteristic.value
            if (data.size == 4) {
                val channelId = data[0].toInt() and 0xFF
                val taskType = data[1].toInt() and 0xFF
                val value = ((data[3].toInt() and 0xFF) shl 8) or (data[2].toInt() and 0xFF)
                
                if (channelId == 0xFF) {
                    val masterValveOpen = taskType == 1
                    println("Master valve is now ${if (masterValveOpen) "OPEN" else "CLOSED"}")
                    updateMasterValveStatus(masterValveOpen)
                    return
                }

                val isActive = taskType == 1
                println("Channel $channelId is now ${if (isActive) "ACTIVE" else "INACTIVE"}")

                // Update UI
                updateChannelStatus(channelId, isActive)
            }
        }
    }
    
    private fun updateChannelStatus(channelId: Int, isActive: Boolean) {
        // Update UI to reflect valve status change
        // This would typically involve updating RecyclerView items or UI components
    }

    private fun updateMasterValveStatus(isOpen: Boolean) {
        // Update UI element that reflects master valve state
    }
}
```

## Testing and Validation

### Test Cases

1. **Valid Duration Task Creation**
   - Write: `{0, 0, 15, 0}` (Channel 0, 15 minutes)
   - Expected: Task created successfully, notification sent when valve opens

2. **Valid Volume Task Creation**
   - Write: `{2, 1, 5, 0}` (Channel 2, 5 liters)
   - Expected: Task created successfully, notification sent when valve opens

3. **Invalid Channel ID**
    - Write: `{8, 0, 10, 0}` (Invalid channel)
    - Expected: GATT error (VALUE_NOT_ALLOWED), no notification

4. **Invalid Task Type**
    - Write: `{0, 2, 10, 0}` (Invalid task type)
    - Expected: GATT error (VALUE_NOT_ALLOWED)

5. **Value Out of Range**
    - Write: `{0x00, 0x00, 0xD0, 0x07}` (2000 minutes = invalid)
    - Expected: GATT error (VALUE_NOT_ALLOWED)

6. **Status Read Operation**
   - Read characteristic when channel 1 is active
   - Expected: `{1, 1, 0, 0}` (Channel 1 active)

7. **Notification Handling**
   - Start watering task and monitor notifications
   - Expected: Activation notification when valve opens, deactivation when valve closes

### Performance Testing

1. **Notification Throttling**
    - Trigger rapid sequential valve changes (open/close)
    - Verify adaptive HIGH priority (base 50ms) does not spam (adaptive algorithm may lengthen interval on failures)

2. **Master Valve Integration**
    - Issue open/close commands with auto-management disabled
    - Confirm notifications only on actual state change (not merely accepted write)

3. **Error Handling**
    - Send invalid writes; confirm client receives ATT errors and no notifications

## Related Characteristics

- **System Configuration (def6)**: Master valve settings
- **Task Queue Management (def7)**: Task scheduling and queue status
- **Current Task Status (deff)**: Real-time task monitoring
- **Flow Sensor (def2)**: Flow monitoring during valve operation
- **System Status (def3)**: Overall system health during valve operations

## Best Practices

1. **Always enable notifications** to receive real-time valve status updates
2. **Validate input parameters** before sending write commands
3. **Handle error responses** gracefully and provide user feedback
4. **Monitor master valve timing** when multiple channels are active
5. **Implement retry logic** for failed write operations
6. **Use appropriate timeouts** for BLE operations
7. **Update UI responsively** based on notification events
