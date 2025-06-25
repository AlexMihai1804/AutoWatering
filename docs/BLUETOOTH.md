# AutoWatering – Bluetooth API (GATT)

Single custom service – "Irrigation Service".

| Item | UUID | ATT props | Purpose |
|------|------|-----------|---------|
| Service | `12345678-1234-5678-1234-56789abcdef0` | – | Root container |
| 1. Task Creator | `…ef1` | R/W/N | Queue watering tasks |
| 2. Flow | `…ef2` | R/N | Pulse counter live feed |
| 3. System Status | `…ef3` | R/N | Overall state machine |
| 4. Channel Config | `…ef4` | R/W/N | Per-channel settings |
| 5. Schedule Config | `…ef5` | R/W/N | Automatic schedules |
| 6. System Config | `…ef6` | R/W/N | Global parameters |
| 7. Task Queue | `…ef7` | R/W/N | Queue inspection / control |
| 8. Statistics | `…ef8` | R/N | Per-channel usage stats |
| 9. RTC | `…ef9` | R/W/N | Real-time-clock access |
|10. Alarm | `…efa` | R/N | Fault / alarm reports |
|11. Calibration | `…efb` | R/W/N | Flow-sensor calibration |
|12. History | `…efc` | R/W/N | Finished watering log |
|13. Diagnostics | `…efd` | R/N | Uptime & health info |

All multi-byte integers are little-endian.  
All structures are packed, no padding.

---

## 1. Task Creator  (`…ef1`)

**Purpose**: Create new watering tasks for immediate or queued execution.

```c
struct {
    uint8_t  channel_id;   // 0-7
    uint8_t  task_type;    // 0=duration [min], 1=volume [L]
    uint16_t value;        // minutes or litres
}
```

**Operations**:
- **WRITE**: Creates a new task and adds it to the queue
- **READ**: Returns the last accepted task parameters
- **NOTIFY**: Sent when task is accepted/rejected

**Error conditions**:
- Invalid channel_id (≥8) → rejected
- System in fault state → rejected
- Queue full → rejected

Typical sequence (duration task on channel 3 for 10 min):

1. Write `{3,0,10}`  
2. Wait for Status/Queue notifications.

---

## 2. Flow  (`…ef2`)

**Purpose**: Real-time water flow monitoring.

```c
uint32_t pulses;   // Absolute pulse counter since valve opened
```

**Operations**:
- **READ**: Current pulse count
- **NOTIFY**: Rate-limited updates (max 2 Hz or ≥10 new pulses)

**Notes**:
- Counter resets when all valves close
- Convert to volume: `volume_ml = (pulses * 1000) / flow_calibration`

---

## 3. System Status  (`…ef3`)

**Purpose**: Monitor overall system health and state.

```c
uint8_t status;   // 0=Idle 1=Running 2=Fault 3=Low-Power 4=RTC-Error
```

**Status values**  
• `0` (Idle): System ready, no active tasks  
• `1` (Running): Watering in progress  
• `2` (Fault): A fatal condition (e.g. maximum no-flow retries, queue error) – check Alarm characteristic and clear with Task-Queue command 4  
• `3` (Low-Power): Power-saving mode active  
• `4` (RTC-Error): Real-time clock failure  

---

## 4. Channel Config  (`…ef4`)

**Purpose**: Configure individual watering channels.

```c
struct {
    uint8_t  channel_id;  // 0-7
    uint8_t  name_len;    // ≤64
    char     name[64];    // UTF-8, null-terminated (max 63 chars + '\0')
    uint8_t  auto_enabled;// 1=automatic schedule active
}
```

**Operations**:
- **READ**: Request specific channel config (set channel_id first)
- **WRITE**: Update name and/or auto_enabled flag
- **NOTIFY**: Sent after successful update

**Persistence**: Changes are saved to NVS automatically.

**Default**: on first boot every channel has  
  `auto_enabled = 0`, an empty day-mask and `value = 0`,  
  therefore no automatic watering occurs until the user changes the
  configuration.

### View / Set channel name

Reading the current name:  
1. Write a single byte (`channel_id`) to the characteristic  
2. Read – the firmware responds with the full structure

Writing a new name (or toggling `auto_enabled`):  
• Send the full structure (67 bytes total)  
• `name_len` is the actual string length (0-64)  
• The `name` field should contain the string without padding  
• Set `auto_enabled` at offset 66

Example (Python / Bleak):

```python
# rename channel 2 to "Garden front sprinkler – zone A"
cid  = 2
name = "Garden front sprinkler – zone A".encode('utf-8')
if len(name) > 64:
    name = name[:64]  # truncate if too long

# Build packet: 1 + 1 + 64 + 1 = 67 bytes
pkt = bytearray(67)
pkt[0] = cid                    # channel_id
pkt[1] = len(name)              # name_len
pkt[2:2+len(name)] = name       # actual string
# bytes 2+len(name) to 66 remain zero (padding)
pkt[66] = 1                     # auto_enabled = YES

await client.write_gatt_char(CHANNEL_CONFIG_UUID, pkt, response=True)
```

**Important notes**:
- The struct is packed: 1 byte channel_id + 1 byte name_len + 64 bytes name + 1 byte auto_enabled = 67 bytes total
- When `name_len = 0`, the firmware keeps the existing name unchanged
- When `name_len > 0`, the firmware copies exactly `name_len` bytes from the name field
- UTF-8 encoding is supported, but total byte length must not exceed 64
- To only change `auto_enabled` without changing the name, set `name_len = 0`

Example to toggle auto_enabled without changing name:
```python
# Enable auto watering for channel 3 without changing its name
pkt = bytearray(67)
pkt[0] = 3   # channel_id
pkt[1] = 0   # name_len = 0 (keep existing name)
# bytes 2-65 remain zero
pkt[66] = 1  # auto_enabled = YES

await client.write_gatt_char(CHANNEL_CONFIG_UUID, pkt, response=True)
```

⚠️ **Write size vs. MTU**

A single `writeValueWithoutResponse()` (Web-BLE “command”) cannot exceed `MTU-3`
bytes.  With the default MTU = 23 this is **20 bytes**, far smaller than the
67-byte Channel-Config structure.  Trying to send the whole buffer with
*Write-Without-Response* causes the peripheral to return ATT error **0x06
(Request Not Supported)**.

Use one of the following options:

* **Preferred:** call `writeValue()` (Write **with** Response).  
  Web-Bluetooth automatically performs a Prepare/Execute sequence, so the full
  67-byte payload is accepted in one call.

* **Alternative:** first negotiate a larger MTU (e.g. 247) and then make sure
  every `writeValueWithoutResponse()` chunk is ≤ MTU-3.

Example (JavaScript):

```javascript
// write with response -> ok for 67-byte payload
await characteristic.writeValue(packet);              // WITH response

// write without response -> must stay under 20 bytes unless MTU is larger
await characteristic.writeValueWithoutResponse(chunk); // chunk ≤ MTU-3
```

> **Can I increase MTU from JavaScript?**  
> No. Web-Bluetooth does not expose any API for the central to start the
> `Exchange MTU` procedure, so browsers stay at the default **MTU = 23**.
> For large writes you must call `writeValue()` (with response) and let the
> browser perform the Long-Write sequence automatically.

> **Native apps can negotiate a larger MTU**  
> • Android (Java/Kotlin): `gatt.requestMtu(247)` – call right after
>   `BluetoothGattCallback.onConnectionStateChange(...)`.  
> • BlueZ / Python Bleak: `client.mtu_size = 247` (Bleak ≥0.20) or
>   `bluetoothctl mtu 247`.  
> • iOS sets MTU automatically to 185 and cannot be changed.  
> After a successful exchange you may safely use
> `writeWithoutResponse()` with packets up to `MTU-3` bytes.

### Web-Bluetooth (browser) – fragmented write ≤ 20 B

Most desktop browsers fix MTU to 23 ⇒ ATT payload ≤ 20 B, so the full
67-byte structure cannot be sent in one `writeValueWithoutResponse()`.  
Use the firmware’s “header + slice” mode:

Payload of each fragment  
```
[0] channel_id
[1] len_of_this_slice      // 1-64, must equal ‘name_len’ in first frame
[2…] UTF-8 slice (≤18 B)   // so total frame ≤20 B
```

JavaScript helper:

```javascript
async function setChannelName(charCfg, id, nameUtf8) {
  const enc = new TextEncoder();
  const bytes = enc.encode(nameUtf8);
  if (bytes.length > 64) throw Error("Name too long");
  const CHUNK = 18;                      // 2 B header + 18 B ≤ 20 B
  for (let off = 0; off < bytes.length; off += CHUNK) {
    const slice = bytes.subarray(off, off + CHUNK);
    const frame = new Uint8Array(2 + slice.length);
    frame[0] = id;
    frame[1] = slice.length;             // len of THIS slice
    frame.set(slice, 2);
    await charCfg.writeValueWithoutResponse(frame);
    await new Promise(r => setTimeout(r, 10)); // flow-control for Windows
  }
  console.log("Name sent in", Math.ceil(bytes.length / CHUNK), "frames");
}
```

Basic usage:

```javascript
const dev  = await navigator.bluetooth.requestDevice({ filters:[{name:"AutoWatering"}], optionalServices:["12345678-1234-5678-1234-56789abcdef0"] });
const gatt = await dev.gatt.connect();
const svc  = await gatt.getPrimaryService("12345678-1234-5678-1234-56789abcdef0");
const ch   = await svc.getCharacteristic("12345678-1234-5678-1234-56789abcdef4");

await setChannelName(ch, 0, "Tomato beds – south");
```

To read the updated name:

```javascript
await ch.writeValue(new Uint8Array([0]));   // select-for-read
const data = await ch.readValue();
const nameLen = data.getUint8(1);
const name    = new TextDecoder().decode(new Uint8Array(data.buffer, 2, nameLen));
console.log("Current name:", name);
```

Notes  
•  `writeValue()` (with response) works as well and lets Chrome perform
   a Long-Write automatically, but some platforms (Win/macOS) still cap
   the payload to 20 B unless Experimental Web-Bluetooth flags are
   enabled.  
•  `auto_enabled` remains unchanged; send the full 67-byte struct (with
   `writeValue()`) if you also need to toggle that flag.

---

## 5. Schedule Config  (`…ef5`)

**Purpose**: Configure automatic watering schedules.

```c
struct {
    uint8_t  channel_id;     // 0-7
    uint8_t  schedule_type;  // 0=Daily 1=Periodic
    uint8_t  days_mask;      // Daily: weekday bitmap / Periodic: interval days
    uint8_t  hour;           // 0-23
    uint8_t  minute;         // 0-59
    uint8_t  watering_mode;  // 0=duration 1=volume
    uint16_t value;          // minutes or litres
}
```

**Schedule types**:
- **Daily** (0): `days_mask` is weekday bitmap (bit 0 = Sun … bit 6 = Sat)  
- **Periodic** (1): `days_mask` is interval in days (e.g. 3 = every 3 days)

**Example**: Water channel 2 every Monday & Friday at 06:30 for 15 minutes:
```
{2, 0, 0x22, 6, 30, 0, 15}
```

### Write semantics

| Payload size | Behaviour |
|--------------|-----------|
| 1 byte        | **Select-for-read** – the byte is interpreted as `channel_id`. No flash write is performed. Follow with a **READ** to obtain the current schedule. |
| ≥7 bytes      | **Update** – full structure is written; if `auto_enabled` is `0` (see Channel Config) the firmware accepts `days_mask=0` or `value=0` without error and the schedule remains disabled.|

> Tip: a disabled event ( `auto_enabled=0` ) bypasses strict validation, so you can set `days_mask=0` or `value=0` as placeholders until real values are known.

### Read sequence example (Python)

```python
# select channel 3
await client.write_gatt_char(SCHEDULE_UUID, b'\x03', response=True)
# read full schedule
data = await client.read_gatt_char(SCHEDULE_UUID)
ch, stype, days_mask, hr, mi, mode, val = struct.unpack("<6BH", data)
print(f"Ch{ch} schedule:", stype, days_mask, hr, mi, mode, val)
```

---

## 6. System Config  (`…ef6`)

**Purpose**: Global system parameters.

```c
struct {
    uint8_t  power_mode;       // 0-Normal 1-Energy-Saving 2-Ultra-Low
    uint32_t flow_calibration; // Pulses per litre
    uint8_t  max_active_valves;// Always 1 (read-only)
}
```

**Power modes**:
- `0` (Normal): Full performance
- `1` (Energy-Saving): Reduced check intervals
- `2` (Ultra-Low): Maximum power savings

**Calibration**: Default is 750 pulses/L. Use Calibration characteristic to recalibrate.

---

## 7. Task Queue  (`…ef7`)

**Purpose**: Monitor and control the task queue.

```c
struct {
    uint8_t  pending_tasks;     // Number of queued tasks
    uint8_t  completed_tasks;   // Reserved for future use
    uint8_t  current_channel;   // 0-7 or 0xFF if idle
    uint8_t  current_task_type; // 0=duration 1=volume
    uint16_t current_value;     // minutes or litres
    uint8_t  command;           // Write-only control field
    uint8_t  task_id_to_delete; // Reserved for future use
}
```

**Commands** (write `command` field then respond `0`):  
| Code | Action |
|------|--------|
| `0`  | No-op |
| `1`  | Cancel current task |
| `2`  | Clear entire queue |
| `3`  | Delete specific task (reserved) |
| `4`  | **Clear run-time errors / alarms** – resets FAULT, NO_FLOW, UNEXPECTED_FLOW, zeroes internal counters and immediately sends a System-Status **OK** notification |

Example – clear all run-time errors from the mobile app:

```python
queue = bytearray(8)      # struct task_queue_data
queue[6] = 4              # command = CLEAR ERRORS
await client.write_gatt_char(TASK_QUEUE_UUID, queue, response=True)
```

**Example**: Cancel current task:
```python
queue_data = bytearray(8)
queue_data[6] = 1  # command = cancel current
await client.write_gatt_char(uuid, queue_data)
```

The firmware resets `system_status`, flow-monitor counters and immediately
sends a System-Status notification (`status = 0 / OK`).

---

## 8. Statistics  (`…ef8`)

**Purpose**: Track water usage per channel.

```c
struct {
    uint8_t  channel_id;      // 0-7
    uint32_t total_volume;    // ml
    uint32_t last_volume;     // ml
    uint32_t last_watering;   // ms since boot
    uint16_t count;
}
```

**Notes**:
- Statistics persist across reboots
- Write channel_id to request specific channel data

---

## 12. History  (`…efc`)

**Purpose**: Access watering event history.

```c
struct {
    uint8_t  channel_id;   // 0-7
    uint8_t  entry_index;  // 0 = most recent
    uint32_t timestamp;    // ms since boot (k_uptime_get_32)
    uint8_t  mode;         // 0=duration 1=volume
    uint16_t duration;     // Seconds or ml
    uint8_t  success;      // 1=OK 0=failed
}
```

_Remark_: timestamp is *not* Unix time.

**Usage**:
1. Write `{channel_id, entry_index}` to request specific record
2. Read response with full history entry
3. Notification sent with requested data

**Limitations**: 
- Maximum 10 entries per channel
- Older entries are overwritten

---

## 13. Diagnostics  (`…efd`)

**Purpose**: System health monitoring.

```c
struct {
    uint32_t uptime;       // minutes
    uint8_t  error_count;
    uint8_t  last_error;
    uint8_t  valve_status; // Bitmap
    uint8_t  battery_level;// 0xFF = N/A
}
```

**Valve status bitmap**:
- Bit 0: Channel 0 valve (0=closed, 1=open)
- Bit 1: Channel 1 valve
- ...
- Bit 7: Channel 7 valve

---

## Connection Parameters

**Recommended settings**:
- Connection interval: 30-50 ms
- Slave latency: 0
- Supervision timeout: 4000 ms (4 seconds)

**MTU**: Default 23 bytes (supports up to 247 if negotiated)

---

## Error Handling

**ATT Error Codes**:
- `0x05` (Insufficient Authentication): Not used
- `0x06` (Request Not Supported): Invalid operation
- `0x07` (Invalid Offset): Write beyond characteristic size
- `0x08` (Insufficient Authorization): Not used
- `0x09` (Prepare Queue Full): Not used
- `0x0A` (Attribute Not Found): Invalid UUID
- `0x0B` (Attribute Not Long): Not used
- `0x0C` (Insufficient Encryption Key Size): Not used
- `0x0D` (Invalid Attribute Value Length): Wrong data size
- `0x0E` (Unlikely Error): Internal system error
- `0x0F` (Insufficient Encryption): Not used
- `0x10` (Unsupported Group Type): Not used
- `0x11` (Insufficient Resources): System busy

---

## Implementation Examples

### Python (using Bleak)

```python
import asyncio
import struct
from bleak import BleakClient

SERVICE_UUID = "12345678-1234-5678-1234-56789abcdef0"
TASK_UUID = "12345678-1234-5678-1234-56789abcdef1"
FLOW_UUID = "12345678-1234-5678-1234-56789abcdef2"
RTC_UUID = "12345678-1234-5678-1234-56789abcdef9"

async def water_for_duration(client, channel, minutes):
    """Create duration-based watering task"""
    data = struct.pack("<BBH", channel, 0, minutes)
    await client.write_gatt_char(TASK_UUID, data, response=True)

async def monitor_flow(client):
    """Monitor water flow in real-time"""
    def flow_handler(sender, data):
        pulses = struct.unpack("<I", data)[0]
        print(f"Flow: {pulses} pulses")
    
    await client.start_notify(FLOW_UUID, flow_handler)

async def sync_time(client):
    """Sync RTC with system time"""
    import datetime
    now = datetime.datetime.now()
    data = struct.pack("<7B",
        now.year - 2000,
        now.month,
        now.day,
        now.hour,
        now.minute,
        now.second,
        now.weekday())
    await client.write_gatt_char(RTC_UUID, data, response=True)

async def main():
    async with BleakClient("XX:XX:XX:XX:XX:XX") as client:
        # Enable notifications for all characteristics
        for char in client.services.characteristics.values():
            if "notify" in char.properties:
                await client.start_notify(char.uuid, lambda s, d: None)
        
        # Sync time
        await sync_time(client)
        
        # Start watering channel 0 for 5 minutes
        await water_for_duration(client, 0, 5)
        
        # Monitor flow
        await monitor_flow(client)
        await asyncio.sleep(30)

asyncio.run(main())
```

### Error Recovery Example

```python
async def robust_write(client, uuid, data, max_retries=3):
    """Write with automatic retry on failure"""
    for attempt in range(max_retries):
        try:
            await client.write_gatt_char(uuid, data, response=True)
            return True
        except Exception as e:
            print(f"Write failed (attempt {attempt + 1}): {e}")
            if attempt < max_retries - 1:
                await asyncio.sleep(1)
    return False
```

---

## Troubleshooting

**Connection Issues**:
- Ensure device is advertising (Status LED blinking)
- Check for interference on 2.4 GHz band
- Verify Bluetooth adapter supports BLE 4.0+

**Data Format Issues**:
- All structures are little-endian
- Strings are UTF-8 encoded
- Unused bytes in structures should be zero

**Notification Issues**:
- Must enable CCC (Client Characteristic Configuration)
- Some characteristics require initial read before notify
- Connection parameters affect notification latency

---

## Security Considerations

**Current Implementation**:
- No pairing/bonding required
- No encryption
- No authentication

**Recommendations for Production**:
- Enable pairing with passkey
- Use encrypted connections
- Implement command authentication
- Add rate limiting for writes

---

## Version History
- **v1.0**: Initial GATT service
- **v1.1**: Added RTC and Alarm characteristics
- **v1.2**: Added Calibration and History
- **v1.3**: Added Diagnostics characteristic
- **v1.4**: Improved error handling and notifications

---

This document covers all Bluetooth LE GATT characteristics exposed by the AutoWatering system. For hardware setup and general usage, see other documentation files.
