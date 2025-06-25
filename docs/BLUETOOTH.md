# AutoWatering – Bluetooth API (GATT)

Single custom service – "Irrigation Service".

| Item | UUID | ATT props | Purpose |
|------|------|-----------|---------|
| Service | `12345678-1234-5678-1234-56789abcdef0` | – | Root container |
| 1. Task Creator (Valve Control) | `…ef1` | R/W/N | Queue watering tasks & valve status |
| 2. Flow | `…ef2` | R/N | Pulse counter live feed |
| 3. System Status | `…ef3` | R/N | Overall state machine |
| 4. Channel Config | `…ef4` | R/W/N | Per-channel settings |
| 5. Schedule Config | `…ef5` | R/W/N | Automatic schedules |
| 6. System Config | `…ef6` | R/W/N | Global parameters |
| 7. Task Queue | `…ef7` | R/W/N | Queue inspection / control |
| 8. Statistics | `…ef8` | R/W/N | Per-channel usage stats |
| 9. RTC | `…ef9` | R/W/N | Real-time-clock access |
|10. Alarm | `…efa` | R/N | Fault / alarm reports |
|11. Calibration | `…efb` | R/W/N | Flow-sensor calibration |
|12. History | `…efc` | R/W/N | Finished watering log |
|13. Diagnostics | `…efd` | R/N | Uptime & health info |

All multi-byte integers are little-endian.  
All structures are packed, no padding.

**Important**: All structure definitions use `__packed` attribute to ensure binary compatibility across different compilers and platforms. When implementing client applications, ensure your structures match the exact byte layout described here.

**Structure Alignment**: The packed structures ensure that all fields are aligned byte-by-byte without padding. This is critical for binary compatibility between the device firmware and client applications. When defining structures in client code:

- C/C++: Use `__attribute__((packed))` or `#pragma pack(1)`
- Python: Use `struct.pack()` and `struct.unpack()` with appropriate format strings
- JavaScript: Use `DataView` for precise byte manipulation
- Java/Kotlin: Use `ByteBuffer` with specific byte ordering

**Endianness**: All multi-byte values (uint16_t, uint32_t) are transmitted in little-endian format regardless of the host system's byte order.

---

## 1. Task Creator / Valve Control  (`…ef1`)

**Purpose**: Create new watering tasks for immediate or queued execution, and receive valve status notifications.

```c
struct {
    uint8_t  channel_id;   // 0-7
    uint8_t  task_type;    // 0=duration [min], 1=volume [L]
    uint16_t value;        // minutes or litres
} __packed;
```

**Operations**:
- **WRITE**: Creates a new task and adds it to the queue
- **READ**: Returns the last accepted task parameters or current valve status
- **NOTIFY**: Sent when:
  - Task is accepted/rejected (after write)
  - Valve status changes (channel activates/deactivates)
  - Real-time valve state updates

**Error conditions**:
- Invalid channel_id (≥8) → rejected
- System in fault state → rejected
- Queue full → rejected

**Status Notifications**:
When a valve activates or deactivates, the system sends a notification with:
- `channel_id`: The channel that changed state (0-7)
- `task_type`: 1 if valve is active, 0 if inactive
- `value`: 0 (no duration/volume info for status updates)

Typical sequence (duration task on channel 3 for 10 min):

1. Write `{3,0,10}`  
2. Wait for Task Queue notification (task added)
3. Wait for Valve Status notification (channel 3 activated)
4. Wait for Valve Status notification (channel 3 deactivated when done)

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
uint8_t status;   // 0=OK 1=No-Flow 2=Unexpected-Flow 3=Fault 4=RTC-Error 5=Low-Power
```

**Status values**  
• `0` (OK): System operating normally  
• `1` (No-Flow): No flow detected when valve is open  
• `2` (Unexpected-Flow): Flow detected when all valves are closed  
• `3` (Fault): System in fault state requiring manual reset  
• `4` (RTC-Error): Real-time clock failure detected  
• `5` (Low-Power): System in low power mode  

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

**Important**: Even `writeValue()` (Write with Response) does **NOT** automatically
handle fragmentation for structures larger than MTU. Browsers do not perform
automatic Long-Write sequences for large payloads. You **must** manually fragment
the data using the firmware's built-in fragmentation protocol described below.

**For Web Browsers**: Use the fragmented write protocol shown in the next section.
**For Native Apps**: Negotiate a larger MTU first, then you can send the full structure.



> **MTU Negotiation Limitation**  
> Web-Bluetooth in browsers does **NOT** expose any API for MTU negotiation.
> Browsers stay at the default **MTU = 23**, so you're limited to 20-byte writes.
> Large structures **must** be sent using multiple smaller writes via the
> fragmentation protocol described below.

> **Native apps can negotiate a larger MTU**  
> • Android (Java/Kotlin): `gatt.requestMtu(247)` – call right after connection  
> • BlueZ / Python Bleak: `client.mtu_size = 247` (Bleak ≥0.20)  
> • iOS sets MTU automatically to 185 and cannot be changed  
> After successful MTU exchange, you can send the full structure in one write.

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

**Notes**  
• The fragmented write protocol above is **required** for Web browsers  
• `auto_enabled` can only be changed by sending the full 67-byte structure,
  which requires MTU negotiation (native apps) or using the fragmentation protocol
• For native apps with MTU ≥70, you can send the full structure in one write

---

## 5. Schedule Config  (`…ef5`)

**Purpose**: Configure automatic watering schedules.

> **⚠️ Structure Updated**: This structure now includes an `auto_enabled` field (total size: 9 bytes). Previous firmware versions used 8 bytes without this field.

```c
struct {
    uint8_t  channel_id;     // 0-7
    uint8_t  schedule_type;  // 0=Daily 1=Periodic
    uint8_t  days_mask;      // Daily: weekday bitmap / Periodic: interval days
    uint8_t  hour;           // 0-23
    uint8_t  minute;         // 0-59
    uint8_t  watering_mode;  // 0=duration 1=volume
    uint16_t value;          // minutes or litres
    uint8_t  auto_enabled;   // 0=disabled 1=enabled
} __packed;
```

**Schedule types**:
- **Daily** (0): `days_mask` is weekday bitmap (bit 0 = Sun … bit 6 = Sat)  
- **Periodic** (1): `days_mask` is interval in days (e.g. 3 = every 3 days)

**Example**: Water channel 2 every Monday & Friday at 06:30 for 15 minutes:
```
{2, 0, 0x22, 6, 30, 0, 15, 1}
```

### Field descriptions

- **`channel_id`** (0-7): Target irrigation channel
- **`schedule_type`**: 
  - `0` = Daily schedule (uses weekday bitmap)
  - `1` = Periodic schedule (uses interval days)
- **`days_mask`**: 
  - For Daily: bitmap where bit 0=Sunday, 1=Monday, ..., 6=Saturday
  - For Periodic: number of days between watering events (1-255)
- **`hour`** (0-23): Start hour in 24-hour format
- **`minute`** (0-59): Start minute
- **`watering_mode`**:
  - `0` = Duration-based (value = minutes)
  - `1` = Volume-based (value = liters)
- **`value`**: Duration in minutes OR volume in liters (depends on mode)
- **`auto_enabled`**: 
  - `0` = Schedule disabled (will not execute automatically)
  - `1` = Schedule enabled (will execute at scheduled times)

### Common examples

```c
// Monday to Friday at 07:00 for 5 minutes (ENABLED)
{ch, 0, 0x3E, 7, 0, 0, 5, 1}

// Every 3 days at 18:30 for 2 liters (ENABLED)  
{ch, 1, 3, 18, 30, 1, 2, 1}

// Weekend only at 06:00 for 10 minutes (DISABLED for testing)
{ch, 0, 0x41, 6, 0, 0, 10, 0}
```

### Write semantics

| Payload size | Behaviour |
|--------------|-----------|
| 1 byte        | **Select-for-read** – the byte is interpreted as `channel_id`. No flash write is performed. Follow with a **READ** to obtain the current schedule. |
| ≥9 bytes      | **Update** – full structure is written; the `auto_enabled` field controls whether the schedule is active.|

> **Note**: When `auto_enabled=0`, the schedule is inactive regardless of other field values. When `auto_enabled=1`, all fields must have valid values (days_mask≠0, value>0, valid time).

### Read sequence example (Python)

```python
# select channel 3
await client.write_gatt_char(SCHEDULE_UUID, b'\x03', response=True)
# read full schedule
data = await client.read_gatt_char(SCHEDULE_UUID)
ch, stype, days_mask, hr, mi, mode, val, auto_en = struct.unpack("<6BHB", data)
print(f"Ch{ch} schedule: type={stype}, days=0x{days_mask:02X}, time={hr:02d}:{mi:02d}, mode={mode}, value={val}, auto_enabled={auto_en}")
```

### Write sequence example (Python)

```python
# Enable watering for channel 1: Monday-Friday (0x3E) at 07:00 for 10 minutes
schedule_data = struct.pack("<6BHB", 
    1,     # channel_id
    0,     # schedule_type (0=Daily)
    0x3E,  # days_mask (Monday-Friday: bits 1-5)
    7,     # hour
    0,     # minute
    0,     # watering_mode (0=duration)
    10,    # value (10 minutes)
    1      # auto_enabled (1=enabled)
)
await client.write_gatt_char(SCHEDULE_UUID, schedule_data, response=True)
```

---

## 6. System Config  (`…ef6`)

**Purpose**: Global system parameters.

```c
struct {
    uint8_t  version;           // Configuration version (read-only)
    uint8_t  power_mode;        // 0=Normal 1=Energy-Saving 2=Ultra-Low
    uint32_t flow_calibration;  // Pulses per litre
    uint8_t  max_active_valves; // Always 1 (read-only)
    uint8_t  num_channels;      // Number of channels (read-only, typically 8)
} __packed;
```

**Power modes**:
- `0` (Normal): Full performance
- `1` (Energy-Saving): Reduced check intervals
- `2` (Ultra-Low): Maximum power savings

**Calibration**: Default is 750 pulses/L. Use Calibration characteristic to recalibrate.

**Read-only fields**: `version`, `max_active_valves`, and `num_channels` are informational and cannot be changed via Bluetooth.

---

## 7. Task Queue  (`…ef7`)

**Purpose**: Monitor and control the task queue.

```c
struct {
    uint8_t  pending_count;     // Number of queued tasks
    uint8_t  completed_tasks;   // Reserved for future use
    uint8_t  current_channel;   // 0-7 or 0xFF if idle
    uint8_t  current_task_type; // 0=duration 1=volume
    uint16_t current_value;     // minutes or litres
    uint8_t  command;           // Write-only control field
    uint8_t  task_id_to_delete; // Reserved for future use
    uint8_t  active_task_id;    // Active task ID (reserved)
} __packed;
```

**Commands** (write `command` field then respond `0`):  
| Code | Action |
|------|--------|
| `0`  | No-op |
| `1`  | Cancel current task |
| `2`  | Clear entire queue |
| `3`  | Delete specific task (reserved) |
| `4`  | **Clear run-time errors / alarms** – resets FAULT, NO_FLOW, UNEXPECTED_FLOW, zeroes internal counters and immediately sends a System-Status **OK** notification |

**Notifications**: This characteristic sends notifications when:
- New tasks are added to the queue
- Tasks are completed or cancelled
- Queue is cleared
- Errors are cleared (command 4)
- **Initial notification** when notifications are enabled (current queue status)

Example – clear all run-time errors from the mobile app:

```python
queue = bytearray(9)      # struct task_queue_data (9 bytes total with active_task_id)
queue[6] = 4              # command = CLEAR ERRORS
await client.write_gatt_char(TASK_QUEUE_UUID, queue, response=True)
```

**Example**: Cancel current task:
```python
queue_data = bytearray(9)  # 9 bytes total
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
    uint16_t count;           // Total watering count
} __packed;
```

**Operations**:
- **READ**: Get statistics for the selected channel
- **WRITE**: 
  - Single byte: Select channel_id for subsequent read
  - Full structure: Update statistics (reserved for future use)
- **NOTIFY**: Sent when statistics are updated after watering events

**Notes**:
- Statistics persist across reboots
- Write channel_id (single byte) to request specific channel data
- Timestamps are in milliseconds since boot (k_uptime_get_32), not Unix time

## 9. RTC  (`…ef9`)

**Purpose**: Real-time clock synchronization and access.

```c
struct {
    uint8_t year;        // Year minus 2000 (0-99)
    uint8_t month;       // Month (1-12)
    uint8_t day;         // Day (1-31)
    uint8_t hour;        // Hour (0-23)
    uint8_t minute;      // Minute (0-59)
    uint8_t second;      // Second (0-59)
    uint8_t day_of_week; // Day of week (0-6, 0=Sunday)
} __packed;
```

**Operations**:
- **READ**: Get current date and time from RTC
- **WRITE**: Set RTC date and time
- **NOTIFY**: Sent when RTC is updated or on significant time events

**Notes**:
- Year is stored as offset from 2000 (e.g., 23 = year 2023)
- Day of week: 0=Sunday, 1=Monday, ..., 6=Saturday
- If RTC hardware is unavailable, returns default values (year=23, month=1, day=1, etc.)
- Time validation is performed on write (rejects invalid dates/times)

**Example** (Python):
```python
import datetime
import struct

# Set current time
now = datetime.datetime.now()
rtc_data = struct.pack("<7B",
    now.year - 2000,     # year
    now.month,           # month
    now.day,             # day
    now.hour,            # hour
    now.minute,          # minute
    now.second,          # second
    (now.weekday() + 1) % 7  # day_of_week (Python: 0=Monday, convert to 0=Sunday)
)
await client.write_gatt_char(RTC_UUID, rtc_data, response=True)
```

---

## 10. Alarm  (`…efa`)

**Purpose**: Real-time fault and alarm notifications.

```c
struct {
    uint8_t  alarm_code;  // Alarm/error code
    uint16_t alarm_data;  // Additional alarm-specific data
    uint32_t timestamp;   // Timestamp when alarm occurred (ms since boot)
} __packed;
```

**Operations**:
- **READ**: Get last alarm information
- **NOTIFY**: Sent immediately when alarms occur

**Common alarm codes**:
- `1`: No flow detected during watering
- `2`: Unexpected flow when valves closed

**Notes**:
- Timestamp is milliseconds since boot, not Unix time
- Use Task Queue command 4 to clear error states
- Only read-only, cannot write to clear alarms via this characteristic
- When `alarm_data` is 0, it typically indicates the alarm condition has been cleared

---

## 11. Calibration  (`…efb`)

**Purpose**: Flow sensor calibration management.

```c
struct {
    uint8_t  action;            // 0=stop, 1=start, 2=in progress, 3=calculated
    uint32_t pulses;            // Number of pulses counted
    uint32_t volume_ml;         // Volume in ml (input when stopping)
    uint32_t pulses_per_liter;  // Calibration result
} __packed;
```

**Actions**:
- `0` (stop): Stop calibration and calculate result
- `1` (start): Start new calibration session
- `2` (in progress): Calibration is running (read-only status)
- `3` (calculated): Calibration completed with result

**Calibration procedure**:
1. Write `{1, 0, 0, 0}` to start calibration
2. Run a known volume of water through the sensor
3. Write `{0, 0, volume_ml, 0}` to stop and calculate
4. Read result from `pulses_per_liter` field
5. New calibration is automatically saved to system config

**Example** (calibrate with 1000ml):
```python
# Start calibration
await client.write_gatt_char(CALIB_UUID, struct.pack("<B3I", 1, 0, 0, 0))

# ... run 1000ml of water through sensor ...

# Stop and calculate
await client.write_gatt_char(CALIB_UUID, struct.pack("<B3I", 0, 0, 1000, 0))

# Read result
data = await client.read_gatt_char(CALIB_UUID)
action, pulses, volume, result = struct.unpack("<B3I", data)
print(f"Calibration result: {result} pulses/liter")
```

---

## 12. History  (`…efc`)

**Purpose**: Access watering event history.

```c
struct {
    uint8_t  channel_id;   // 0-7
    uint8_t  entry_index;  // 0 = most recent
    uint32_t timestamp;    // ms since boot (k_uptime_get_32)
    uint8_t  mode;         // 0=duration 1=volume
    uint16_t duration;     // Seconds (for duration mode) or ml (for volume mode)
    uint8_t  success;      // 1=OK 0=failed
} __packed;
```

**Operations**:
- **READ**: Get the selected history entry
- **WRITE**: Request specific history record by writing `{channel_id, entry_index}`
- **NOTIFY**: Sent when new history entries are added

**Usage**:
1. Write `{channel_id, entry_index}` to request specific record
2. Read response with full history entry
3. Notification sent with requested data

**Notes**:
- `timestamp` is *not* Unix time, it's milliseconds since boot
- `entry_index` 0 is the most recent entry
- Maximum 10 entries per channel
- Older entries are overwritten
- `duration` field meaning depends on `mode`: seconds for duration tasks, ml for volume tasks

**Example** (get last 3 watering events for channel 0):
```python
for i in range(3):
    # Request entry i for channel 0
    await client.write_gatt_char(HISTORY_UUID, struct.pack("<BB", 0, i))
    
    # Read the response
    data = await client.read_gatt_char(HISTORY_UUID)
    ch, idx, ts, mode, dur, success = struct.unpack("<BB I B H B", data)
    
    mode_str = "duration" if mode == 0 else "volume"
    status_str = "SUCCESS" if success else "FAILED"
    print(f"Entry {idx}: {mode_str} task, {dur} {'sec' if mode == 0 else 'ml'}, {status_str}")
```

---

## 13. Diagnostics  (`…efd`)

**Purpose**: System health monitoring and diagnostic information.

```c
struct {
    uint32_t uptime;        // minutes since boot
    uint8_t  error_count;   // Total number of errors since boot
    uint8_t  last_error;    // Last error code (same as alarm codes)
    uint8_t  valve_status;  // Bitmap of valve states
    uint8_t  battery_level; // Battery level in percent (0xFF = N/A)
} __packed;
```

**Operations**:
- **READ**: Get current system diagnostics
- **NOTIFY**: Sent periodically or when significant system changes occur

**Valve status bitmap**:
- Bit 0: Channel 0 valve (0=closed, 1=open)
- Bit 1: Channel 1 valve
- Bit 2: Channel 2 valve
- ...
- Bit 7: Channel 7 valve

**Notes**:
- `uptime` is in minutes since system boot
- `error_count` accumulates all errors since boot
- `last_error` uses same codes as Alarm characteristic
- `battery_level` is 0xFF (255) for systems without battery monitoring
- `valve_status` provides real-time valve state information

**Example** (decode valve status):
```python
data = await client.read_gatt_char(DIAGNOSTICS_UUID)
uptime, errors, last_err, valves, battery = struct.unpack("<I4B", data)

print(f"Uptime: {uptime} minutes")
print(f"Errors: {errors}, Last: {last_err}")

# Decode valve bitmap
for ch in range(8):
    if valves & (1 << ch):
        print(f"Channel {ch}: OPEN")
    else:
        print(f"Channel {ch}: CLOSED")

if battery != 0xFF:
    print(f"Battery: {battery}%")
else:
    print("Battery: N/A")
```

---

## Connection Reliability

**Automatic reconnection**:
The device continuously advertises and accepts new connections. If disconnected, the client should attempt to reconnect with exponential backoff.

**Connection stability**:
- Monitor connection events and handle disconnections gracefully
- Cache critical settings locally to avoid repeated queries
- Implement timeout handling for all operations
- Use notifications for real-time updates rather than polling

**Data consistency**:
- Always verify write operations completed successfully
- Use response-based writes for critical configuration changes
- Monitor notification events to confirm system state changes
- Cache channel names and configurations to reduce BLE traffic

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
STATUS_UUID = "12345678-1234-5678-1234-56789abcdef3"
TASK_QUEUE_UUID = "12345678-1234-5678-1234-56789abcdef7"
RTC_UUID = "12345678-1234-5678-1234-56789abcdef9"

async def water_for_duration(client, channel, minutes):
    """Create duration-based watering task"""
    data = struct.pack("<BBH", channel, 0, minutes)
    await client.write_gatt_char(TASK_UUID, data, response=True)
    print(f"Task created: Channel {channel} for {minutes} minutes")

async def water_for_volume(client, channel, liters):
    """Create volume-based watering task"""
    data = struct.pack("<BBH", channel, 1, liters)
    await client.write_gatt_char(TASK_UUID, data, response=True)
    print(f"Task created: Channel {channel} for {liters} liters")

async def monitor_flow(client):
    """Monitor water flow in real-time"""
    def flow_handler(sender, data):
        pulses = struct.unpack("<I", data)[0]
        # Convert to volume using default calibration (750 pulses/L)
        volume_ml = (pulses * 1000) // 750
        print(f"Flow: {pulses} pulses ({volume_ml} ml)")
    
    await client.start_notify(FLOW_UUID, flow_handler)
    print("Flow monitoring started")

async def monitor_valve_status(client):
    """Monitor valve activation/deactivation"""
    def valve_handler(sender, data):
        ch_id, task_type, value = struct.unpack("<BBH", data)
        if ch_id != 0xFF:  # Valid channel
            if task_type == 1:
                print(f"VALVE ON: Channel {ch_id} activated")
            else:
                print(f"VALVE OFF: Channel {ch_id} deactivated")
    
    await client.start_notify(TASK_UUID, valve_handler)
    print("Valve status monitoring started")

async def monitor_task_queue(client):
    """Monitor task queue changes"""
    def queue_handler(sender, data):
        pending, completed, current_ch, current_type, current_val, cmd, del_id, active_id = struct.unpack("<8B", data)
        print(f"Queue: {pending} pending tasks")
        if current_ch != 0xFF:
            mode = "duration" if current_type == 0 else "volume"
            print(f"Current task: Channel {current_ch}, {mode}, value {current_val}")
    
    await client.start_notify(TASK_QUEUE_UUID, queue_handler)
    print("Task queue monitoring started")

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
        (now.weekday() + 1) % 7)  # Convert to 0=Sunday format
    await client.write_gatt_char(RTC_UUID, data, response=True)
    print(f"RTC synchronized to {now}")

async def clear_errors(client):
    """Clear all system errors"""
    queue_data = bytearray(8)
    queue_data[6] = 4  # Command: clear errors
    await client.write_gatt_char(TASK_QUEUE_UUID, queue_data, response=True)
    print("System errors cleared")

async def get_system_status(client):
    """Read current system status"""
    data = await client.read_gatt_char(STATUS_UUID)
    status = data[0]
    status_names = ["Idle", "Running", "Fault", "Low-Power", "RTC-Error"]
    status_name = status_names[status] if status < len(status_names) else f"Unknown({status})"
    print(f"System status: {status_name}")
    return status

async def main():
    # Replace with your device's MAC address
    device_address = "XX:XX:XX:XX:XX:XX"
    
    async with BleakClient(device_address) as client:
        print(f"Connected to {device_address}")
        
        # Enable all notifications
        await monitor_valve_status(client)
        await monitor_task_queue(client)
        await monitor_flow(client)
        
        # Sync time
        await sync_time(client)
        
        # Check system status
        await get_system_status(client)
        
        # Clear any existing errors
        await clear_errors(client)
        
        # Start watering channel 0 for 5 minutes
        await water_for_duration(client, 0, 5)
        
        # Monitor for 30 seconds
        print("Monitoring for 30 seconds...")
        await asyncio.sleep(30)
        
        print("Monitoring complete")

if __name__ == "__main__":
    asyncio.run(main())
```

### Complete Monitoring Example

```python
import asyncio
import struct
import logging
from bleak import BleakClient, BleakScanner
from bleak.exc import BleakError

# Configure logging
logging.basicConfig(level=logging.INFO)
logger = logging.getLogger(__name__)

# Service and Characteristic UUIDs
SERVICE_UUID = "12345678-1234-5678-1234-56789abcdef0"
TASK_UUID = "12345678-1234-5678-1234-56789abcdef1"
FLOW_UUID = "12345678-1234-5678-1234-56789abcdef2"
STATUS_UUID = "12345678-1234-5678-1234-56789abcdef3"
CHANNEL_CONFIG_UUID = "12345678-1234-5678-1234-56789abcdef4"
SCHEDULE_UUID = "12345678-1234-5678-1234-56789abcdef5"
SYSTEM_CONFIG_UUID = "12345678-1234-5678-1234-56789abcdef6"
TASK_QUEUE_UUID = "12345678-1234-5678-1234-56789abcdef7"
STATISTICS_UUID = "12345678-1234-5678-1234-56789abcdef8"
RTC_UUID = "12345678-1234-5678-1234-56789abcdef9"
ALARM_UUID = "12345678-1234-5678-1234-56789abcdefa"
CALIBRATION_UUID = "12345678-1234-5678-1234-56789abcdefb"
HISTORY_UUID = "12345678-1234-5678-1234-56789abcdefc"
DIAGNOSTICS_UUID = "12345678-1234-5678-1234-56789abcdefd"

class AutoWateringClient:
    def __init__(self, device_name="AutoWatering"):
        self.device_name = device_name
        self.client = None
        self.device = None
        self.connected = False
        
    async def discover_device(self, timeout=10.0):
        """Discover AutoWatering device"""
        logger.info(f"Scanning for {self.device_name}...")
        
        devices = await BleakScanner.discover(timeout=timeout)
        for device in devices:
            if device.name == self.device_name:
                self.device = device
                logger.info(f"Found device: {device.address}")
                return True
                
        logger.error(f"Device {self.device_name} not found")
        return False
        
    async def connect(self, max_retries=3):
        """Connect to device with retry logic"""
        if not self.device:
            if not await self.discover_device():
                return False
                
        for attempt in range(max_retries):
            try:
                logger.info(f"Connection attempt {attempt + 1}/{max_retries}")
                self.client = BleakClient(self.device.address)
                await self.client.connect()
                self.connected = True
                logger.info("Connected successfully")
                return True
                
            except BleakError as e:
                logger.warning(f"Connection failed: {e}")
                if attempt < max_retries - 1:
                    await asyncio.sleep(2 ** attempt)  # Exponential backoff
                    
        logger.error("Failed to connect after all retries")
        return False
        
    async def disconnect(self):
        """Disconnect from device"""
        if self.client and self.connected:
            await self.client.disconnect()
            self.connected = False
            logger.info("Disconnected")
            
    async def setup_notifications(self):
        """Enable all notifications and set up handlers"""
        
        def valve_handler(sender, data):
            ch_id, task_type, value = struct.unpack("<BBH", data)
            if ch_id != 0xFF:
                if task_type == 1:
                    logger.info(f"🚿 VALVE ON: Channel {ch_id} activated")
                else:
                    logger.info(f"⏹️  VALVE OFF: Channel {ch_id} deactivated")
                    
        def flow_handler(sender, data):
            pulses = struct.unpack("<I", data)[0]
            volume_ml = (pulses * 1000) // 750  # Default calibration
            logger.info(f"💧 Flow: {pulses} pulses ({volume_ml} ml)")
            
        def status_handler(sender, data):
            status = data[0]
            status_names = ["OK", "No-Flow", "Unexpected-Flow", "Fault", "RTC-Error", "Low-Power"]
            status_name = status_names[status] if status < len(status_names) else f"Unknown({status})"
            logger.info(f"📊 System Status: {status_name}")
            
        def alarm_handler(sender, data):
            alarm_code, alarm_data, timestamp = struct.unpack("<BHI", data)
            alarm_names = {1: "No Flow", 2: "Unexpected Flow"}
            alarm_name = alarm_names.get(alarm_code, f"Unknown({alarm_code})")
            logger.warning(f"🚨 ALARM: {alarm_name} (data: {alarm_data}, time: {timestamp})")
            
        def queue_handler(sender, data):
            pending, completed, current_ch, current_type, current_val, cmd, del_id, active_id = struct.unpack("<5BHB", data)
            logger.info(f"📋 Queue: {pending} pending tasks")
            if current_ch != 0xFF:
                mode = "duration" if current_type == 0 else "volume"
                logger.info(f"   Current: Channel {current_ch}, {mode}, value {current_val}")
                
        # Enable notifications
        await self.client.start_notify(TASK_UUID, valve_handler)
        await self.client.start_notify(FLOW_UUID, flow_handler)
        await self.client.start_notify(STATUS_UUID, status_handler)
        await self.client.start_notify(ALARM_UUID, alarm_handler)
        await self.client.start_notify(TASK_QUEUE_UUID, queue_handler)
        
        logger.info("All notifications enabled")
        
    async def sync_time(self):
        """Synchronize RTC with system time"""
        import datetime
        now = datetime.datetime.now()
        data = struct.pack("<7B",
            now.year - 2000,
            now.month,
            now.day,
            now.hour,
            now.minute,
            now.second,
            (now.weekday() + 1) % 7)
        await self.client.write_gatt_char(RTC_UUID, data, response=True)
        logger.info(f"🕐 RTC synchronized to {now}")
        
    async def clear_errors(self):
        """Clear all system errors"""
        queue_data = bytearray(9)
        queue_data[6] = 4  # Command: clear errors
        await self.client.write_gatt_char(TASK_QUEUE_UUID, queue_data, response=True)
        logger.info("✅ System errors cleared")
        
    async def get_system_info(self):
        """Read and display system information"""
        # Read system status
        status_data = await self.client.read_gatt_char(STATUS_UUID)
        status = status_data[0]
        status_names = ["OK", "No-Flow", "Unexpected-Flow", "Fault", "RTC-Error", "Low-Power"]
        status_name = status_names[status] if status < len(status_names) else f"Unknown({status})"
        
        # Read system config
        config_data = await self.client.read_gatt_char(SYSTEM_CONFIG_UUID)
        version, power_mode, flow_cal, max_valves, num_channels = struct.unpack("<BB I BB", config_data)
        
        # Read diagnostics
        diag_data = await self.client.read_gatt_char(DIAGNOSTICS_UUID)
        uptime, error_count, last_error, valve_status, battery = struct.unpack("<I4B", diag_data)
        
        logger.info("📋 System Information:")
        logger.info(f"   Status: {status_name}")
        logger.info(f"   Config Version: {version}")
        logger.info(f"   Power Mode: {power_mode}")
        logger.info(f"   Flow Calibration: {flow_cal} pulses/L")
        logger.info(f"   Channels: {num_channels}")
        logger.info(f"   Uptime: {uptime} minutes")
        logger.info(f"   Error Count: {error_count}")
        logger.info(f"   Valve Status: 0b{valve_status:08b}")
        
    async def create_watering_task(self, channel, duration_minutes):
        """Create a duration-based watering task"""
        data = struct.pack("<BBH", channel, 0, duration_minutes)
        await self.client.write_gatt_char(TASK_UUID, data, response=True)
        logger.info(f"🌱 Task created: Channel {channel} for {duration_minutes} minutes")
        
    async def run_monitoring_session(self, duration=60):
        """Run a complete monitoring session"""
        try:
            if not await self.connect():
                return False
                
            await self.setup_notifications()
            await self.clear_errors()
            await self.sync_time()
            await self.get_system_info()
            
            logger.info(f"🔍 Monitoring for {duration} seconds...")
            await asyncio.sleep(duration)
            
        except Exception as e:
            logger.error(f"Error during monitoring: {e}")
            return False
        finally:
            await self.disconnect()
            
        return True

# Usage example
async def main():
    client = AutoWateringClient()
    
    # Run a 60-second monitoring session
    await client.run_monitoring_session(60)
    
    # Or manual control:
    # if await client.connect():
    #     await client.setup_notifications()
    #     await client.create_watering_task(0, 5)  # Water channel 0 for 5 minutes
    #     await asyncio.sleep(30)  # Monitor for 30 seconds
    #     await client.disconnect()

if __name__ == "__main__":
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

async def robust_task_creation(client, channel, task_type, value):
    """Create task with error handling and verification"""
    data = struct.pack("<BBH", channel, task_type, value)
    
    if await robust_write(client, TASK_UUID, data):
        print(f"Task created successfully")
        # Wait for queue notification to confirm
        await asyncio.sleep(0.5)
        return True
    else:
        print("Failed to create task after retries")
        return False
```

---

## Troubleshooting

**Connection Issues**:
- Ensure device is advertising (Status LED blinking)
- Check for interference on 2.4 GHz band
- Verify Bluetooth adapter supports BLE 4.0+
- Try power cycling the device if it's not discoverable
- Ensure no other clients are connected (device supports only 1 connection)

**Data Format Issues**:
- All structures are little-endian
- Strings are UTF-8 encoded
- Unused bytes in structures should be zero
- Verify structure sizes match exactly (use `sizeof()` in C)

**Notification Issues**:
- Must enable CCC (Client Characteristic Configuration) before receiving notifications
- Some characteristics require initial read before notify
- Connection parameters affect notification latency
- Check that notifications are enabled on the correct characteristic

**Write Operation Failures**:
- For structures >20 bytes, use fragmentation protocol or negotiate larger MTU
- Always use `response=True` for critical configuration changes
- Check ATT error codes for specific failure reasons
- Ensure data lengths match expected structure sizes

**Common ATT Error Codes**:
- `0x06` (Request Not Supported): Structure too large for current MTU
- `0x07` (Invalid Offset): Write beyond characteristic size  
- `0x0D` (Invalid Attribute Value Length): Wrong data size
- `0x0E` (Unlikely Error): Internal system error - try reconnecting

**Performance Optimization**:
- Cache channel names and configurations locally
- Use notifications instead of polling for real-time data
- Implement connection retry with exponential backoff
- Monitor connection state and handle disconnections gracefully

**MTU Negotiation**:
- Web browsers: Cannot negotiate MTU, limited to 20-byte writes
- Android: Use `gatt.requestMtu(247)` after connection
- iOS: MTU is automatically set to 185
- Desktop/Server: Use library-specific MTU negotiation APIs

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
- **v1.5**: Updated documentation accuracy, fixed structure definitions and notification behaviors
- **v1.6**: Complete documentation overhaul (June 2025):
  - Updated system status codes to match implementation
  - Corrected alarm codes and error handling
  - Added comprehensive Python examples with error handling
  - Improved structure packing and endianness documentation
  - Enhanced troubleshooting section with common issues
  - Added connection reliability best practices
  - Verified all characteristics against current firmware implementation

---

This document covers all Bluetooth LE GATT characteristics exposed by the AutoWatering system. For hardware setup and general usage, see other documentation files.
