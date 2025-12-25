# AutoWatering Troubleshooting Guide (January 2025)

This guide covers firmware diagnostics for the AutoWatering irrigation controller running on nRF52840. It focuses on current implementation issues and their resolution.

---

## Quick Reference

| Symptom | Likely Cause | Action |
|---------|--------------|--------|
| Alarm code 1 (No Flow) | Valve open but no pulses | Check flow sensor wiring; verify calibration >0; ensure water supply on |
| Alarm code 2 (Unexpected Flow) | Pulses with all valves closed | Inspect for leak; stuck valve; reset after fix |
| Alarm code 3 (Freeze Lockout) | Freeze safety triggered | Wait for temp > cutoff or refresh environmental data |
| Alarm code 4 (High Flow) | Burst pipe / broken head | Shut down water, fix leak, then manual test |
| Alarm code 5 (Low Flow) | Filter clogged / partial blockage | Clean filters; watering continues |
| Alarm code 6 (Mainline Leak) | Leak with all zones off | Inspect mainline and valves; reset after fix |
| Alarm code 7 (Channel Lock) | Repeated NO_FLOW or HIGH_FLOW | Manual test, then clear lock |
| Alarm code 8 (Global Lock) | Persistent leak or unexpected flow | Fix cause, then manual override test |
| Interval mode never pauses | Interval not configured / wrong mode | Write interval config with configured=1 and select interval mode |
| Rain data always zero | No rain pulses captured | Wiring, debounce (`rain_sensor_configure`), simulate pulse |
| BLE write rejected | Invalid length or out-of-range field | Recheck struct size & field ranges (see characteristic docs) |
| Environmental snapshot invalid flags | BME280 absent or init failed | Verify I2C wiring (SCL P0.31 / SDA P0.29) |
| Automatic task skipped instantly | Rain integration flagged skip | Check Rain Integration Status characteristic and rainfall totals |
| Time drift or RTC error | RTC not responding | Check DS3231 power & I2C; reapply RTC config characteristic |
| DLE request failed: -13 | Controller doesn't support DLE | Expected behavior; system continues with default packet size |
| History transfer slow | Large dataset, MTU not negotiated | Use bulk sync snapshot; ensure MTU 247 negotiated |
| BLE notifications delayed | Throttling active | Expected behavior; task status ~2s, others ≥500ms spacing |

---

## BLE Performance Troubleshooting

### PHY 2M and Data Length Extension

The firmware requests PHY 2M (2 Mbps) and DLE (251-byte packets) at connection for higher throughput. These may fail on some hardware:

**PHY 2M Update Warning:**
```
PHY 2M update requested for higher throughput
[00:00:55.980,163] <wrn> bt_hci_core: opcode 0x2022 status 0x0c
```
**Cause:** The connected central (phone/tablet) or controller doesn't support PHY 2M.
**Impact:** System falls back to 1 Mbps. This is normal and does not affect functionality.
**Action:** No action required. iOS devices typically support PHY 2M; older Android may not.

**DLE Request Failed:**
```
Data Length Extension request failed: -13 (may not be supported)
```
**Cause:** Error -13 (EACCES) means the controller or remote device doesn't support DLE.
**Impact:** System uses default 27-byte packets instead of 251-byte. Transfers are slower but work.
**Action:** No action required. Consider using Bulk Sync Snapshot for large transfers.

### Slow History Transfers

**Symptoms:**
- Environmental/rain history takes >30 seconds
- Multiple BLE reads needed for full dataset
- Client times out waiting for data

**Solutions:**
1. **Use Bulk Sync Snapshot (UUID 0xde60):**
   - Single 60-byte read returns aggregated system state
   - Includes: system status, rain totals, environmental averages, flow stats
   - Ideal for dashboard displays that don't need full history

2. **Negotiate Maximum MTU:**
   - Request MTU 247 from your BLE client
   - Higher MTU = fewer packets per transfer

3. **Use Range Queries Efficiently:**
   - Binary search optimizes range queries (O(log n) instead of O(n))
   - Query specific time ranges rather than full history
   - Use hourly aggregates before requesting raw samples

4. **Transfer Cache:**
   - Re-reading same data within 30 seconds uses cached result
   - Avoid unnecessary re-reads during sync operations

### Notification Throttling

**Expected Behavior:**
- Task Status: ~2 second intervals while watering active
- Flow Sensor: Updates on pulse detection, minimum 100ms spacing
- Environmental: Throttled to ≥500ms between notifications
- Critical alerts: Bypass throttling for immediate delivery

**If notifications seem delayed:**
- This is intentional to prevent BLE congestion
- Critical alarms (NO_FLOW, UNEXPECTED_FLOW) are never throttled
- Task status notifications only active during watering

### Connection Optimization

**For best BLE performance:**
1. Connect with one central only (`CONFIG_BT_MAX_CONN=1`)
2. Request MTU 247 immediately after connection
3. PHY 2M and DLE are auto-requested; no client action needed
4. Use characteristic caching (GATT_CACHING enabled)
5. Batch notification subscriptions before starting transfers

---

## Flow & Alarm Handling

Runtime alarm codes:
- **1 NO_FLOW:** Valve opened; pulses did not appear after retries
- **2 UNEXPECTED_FLOW:** Pulses detected while all valves closed (debounced)
- **3 FREEZE_LOCKOUT:** Freeze safety triggered (stale env or temperature below cutoff)
- **4 HIGH_FLOW:** Flow above learned limit (burst/popup)
- **5 LOW_FLOW:** Flow below learned limit (warning only)
- **6 MAINLINE_LEAK:** Static test detected pulses with all zones off
- **7 CHANNEL_LOCK:** Channel locked after persistent anomalies
- **8 GLOBAL_LOCK:** Global lock due to leak/unexpected flow

**Clear procedure:** Write the clear opcode to Alarm Status characteristic after resolving cause.
**Manual override:** Explicit manual commands (BLE direct command) bypass locks temporarily for verification.

**Checklist:**
1. Inspect flow sensor wires (VCC, GND, signal on expected GPIO)
2. Trigger manual watering; confirm pulse counter increases
3. If pulses extremely low, verify calibration constant (avoid zero) and water supply pressure
4. Calibration must be non-zero; default is 750 pulses/litre (`flow_sensor_get_calibration`)

---

## Interval Mode

**Symptoms:** Continuous watering or immediate stop.

**Causes:**
- Missing `configured` flag
- Zero durations

**Actions:**
1. Set watering and pause durations (each < 60 min, seconds < 60)
2. Ensure chosen mode enum corresponds to interval mode before starting task
3. Observe Current Task Status: phase changes W=watering / P=pausing

---

## Rain Sensor (Optional)

**If no data:**
- Check debounce not too short (<10ms) causing bounce rejection
- Manually create pulses; verify Rain Sensor Data characteristic increments today_total
- Confirm mm_per_pulse within valid range (rejects out-of-range writes)

**Rain Integration:**
- Rain integration applies reductions/skips inside `watering_start_task()`
- Rain Integration Status characteristic exposes per-channel reduction percentage
- Skip returns `WATERING_ERROR_BUSY`; replacement task is caller's responsibility

---

## Environmental Data

**If all fields invalid:**
- BME280 not present or init failed; check wiring, address (0x76/0x77), or supply
- Environmental packets expose temperature, humidity, and pressure only
- Temperature compensation requires valid environmental data; otherwise falls back to unadjusted FAO-56

**I2C Configuration:**
- Default bus label: `i2c0`
- Default address: 0x77 (some modules use 0x76)
- Initialization: `sensor_manager_init_bme280`

---

## Time / RTC

**If scheduling or timestamps are off:**
1. Reconfigure RTC characteristic with correct UTC time
2. Verify timezone config if local conversions needed
3. Hardware: DS3231 crystal & battery present

**Fallback behavior:**
- `rtc.c` falls back to uptime if DS3231 is missing
- Current fallback date: 2023-12-10
- Timezone configuration is persisted (offset + DST)

---

## BLE Interaction

**Connection limits:**
- Only one central can connect at a time (`CONFIG_BT_MAX_CONN=1`)
- Disconnect unused clients before pairing new device

**Write failures:**
- Common cause: ATT error due to wrong payload size
- Always send full struct for configuration characteristics
- Partial writes unsupported; follow documented fragmentation header for large payloads

**Fragmentation:**
- 3-byte header: `fragment_id`, `flags`, `payload_length`
- See `docs/ble-api/fragmentation-guide.md`

**CCC persistence:**
- Values stored via Zephyr settings
- Clearing bonding data requires clearing settings partition or BLE Clear Bonds procedure

**Post-update bonding recovery:**
- If you see repeated `Pairing failed ... reason: 4` (or key-missing errors) after a firmware update, the device now auto-clears its stored bond for that peer on the first failure.
- Still failing? Also "Forget/Unpair" the device on the phone/PC side, then retry pairing.

---

## Persistence & History

**NVS (Non-Volatile Storage):**
- Configuration writes use `nvs_config.c`
- Data loss typically indicates NVS init failure (logged at boot)
- If NVS fails to mount, system halts

**Watering History:**
- Keeps last 30 events per channel (`DETAILED_EVENTS_PER_CHANNEL`)
- Use History Control characteristic to page through entries
- Binary search enables efficient range queries

**History Performance (v3.1.0+):**
- Binary search for O(log n) range queries
- 30-second transfer cache prevents redundant reads
- Early exit optimization skips unnecessary iterations

---

## Build & Flash Diagnostics

Quick reminders (see `docs/INSTALLATION.md` for full details):

```bash
# Create/update workspace
west init -m https://github.com/AlexMihai1804/AutoWatering.git
west update

# Install Python requirements
pip3 install --user -r zephyr/scripts/requirements.txt

# Build for nRF52840 Pro Micro
west build -b nrf52840_promicro --pristine

# Build for Arduino Nano 33 BLE
west build -b arduino_nano_33_ble --pristine

# Native simulation
west build -b native_sim --pristine -- -DEXTRA_DTC_OVERLAY_FILE=boards/native_sim.overlay

# Flash
west flash --runner jlink
```

**If build fails:**
- Run pristine rebuild: `west build -p always ...`
- Check Zephyr SDK in PATH
- Verify west version: `west --version`

---

## Recovery Procedures

**Soft reset:**
- Use reset control characteristic (two-step confirm) if present
- Or power-cycle device

**Clear stuck valve:**
- Disable watering via BLE
- Depower relay module briefly

**Factory reset:**
- Clear NVS partition via J-Link or bootloader
- All configuration will be lost; reconfigure via BLE

---

## Known Limits

| Resource | Limit | Note |
|----------|-------|------|
| Watering channels | 8 | Hard-coded `WATERING_CHANNELS_COUNT` |
| History per channel | 30 events | `DETAILED_EVENTS_PER_CHANNEL` |
| BLE connections | 1 | `CONFIG_BT_MAX_CONN=1` |
| MTU | 247 bytes | Negotiated at connection |
| PHY | 2M requested | May fall back to 1M |
| DLE | 251 bytes | May fail with -13 on some controllers |
| Notification spacing | ≥100ms | Throttled to prevent congestion |

---

## Minimal Diagnostic Steps

1. Read System Status & Alarm Status characteristics
2. Read Current Task Status while watering is active
3. Read Flow Sensor & Rain Data snapshots
4. If hardware suspected: Inspect wiring, then rebuild, then reflash
5. When adding new clients, rediscover services to pick up characteristic changes

---

## When Further Help Is Needed

- Cross-reference module behavior in `docs/system-architecture.md`
- Record Zephyr logs (USB CDC or RTT) while reproducing the issue
- Capture BLE interaction traces (nRF Connect, Ellisys, etc.) when debugging characteristic writes or notifications
- Check `docs/ble-api/` for characteristic specifications and expected payload sizes

---

*Last updated: January 2025 - v3.1.0 BLE Performance Optimizations*
