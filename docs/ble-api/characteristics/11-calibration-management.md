# Calibration Management Characteristic (UUID: defb)

> Operation Summary
| Operation | Payload Struct | Size | Fragmentation | Notes |
|-----------|----------------|------|---------------|-------|
| Read | `struct calibration_data` | 13 B | None | Returns cached calibration snapshot; `action` may report 0x02 while counting |
| Write | `struct calibration_data` | 13 B | None | Full-frame writes only; `action` selects the state-machine branch |
| Notify | `struct calibration_data` | 13 B | None | Snapshot on CCC enable, state transitions, and progress ticks while active |

Interactive flow sensor calibration. The BLE handlers live in `read_calibration`, `write_calibration`, `calibration_ccc_changed`, and `bt_irrigation_calibration_notify` inside `src/bt_irrigation_service.c`. Progress snapshots are driven by the delayable work item `calibration_progress_work_handler`. Hardware calibration is stored through `watering_set_flow_calibration()` and persisted with `watering_save_config_priority(true)`.

## Characteristic Metadata
| Item | Value |
|------|-------|
| UUID | `12345678-1234-5678-1234-56789abcdefb` |
| Properties | Read, Write, Notify |
| Permissions | Read, Write |
| Payload Size | 13 bytes (`BUILD_ASSERT(sizeof(struct calibration_data) == 13)`) |
| Fragmentation | Not supported; writes must supply the full structure |
| Notification Priority | Normal (base 200 ms throttle via `advanced_notify`, adaptive up to ~2 s under congestion) |

`calibration_value` is zeroed on service init and again whenever notifications are disabled so CCC re-subscription always starts from a clean frame.

## Payload Layout
| Offset | Field | Type | Size | Access | Meaning | Source on Read | Handling on Write |
|--------|-------|------|------|--------|---------|----------------|-------------------|
| 0 | `action` | `uint8_t` | 1 | RW | State-machine command/result | `calibration_value->action` updated by handlers | Drives switch statement; only 0x00/01/03/04/05 accepted; 0x02 rejected |
| 1 | `pulses` | `uint32_t` | 4 | RW | Pulses counted since START | For active runs: `get_pulse_count() - calibration_start_pulses`; otherwise last stored value | Ignored on write (firmware recomputes) |
| 5 | `volume_ml` | `uint32_t` | 4 | RW | Client-provided dispense volume in millilitres | Echoed from `calibration_value`; cleared on START/STOP/APPLY/RESET | Only read during CALCULATED; other actions overwrite with firmware defaults |
| 9 | `pulses_per_liter` | `uint32_t` | 4 | RW | Current or newly computed calibration constant | `get_flow_calibration()` when idle; new result on CALCULATED | Ignored on START; required for APPLY; reset to defaults on RESET |

All fields use little-endian encoding.

## Action Semantics
| Action | When Written | Preconditions | Result | Notifications |
|--------|--------------|---------------|--------|---------------|
| `0x01` START | Begin measurement | `calibration_active` false | Resets pulse counter (`reset_pulse_count()`), zeros `pulses`, `volume_ml`, `pulses_per_liter`, sets `calibration_active = true`, schedules progress work every 200 ms | Immediate notify (if CCC on) showing `action = 0x01`, followed by periodic frames with `action = 0x02` |
| `0x00` STOP | Abort without computing | `calibration_active` true | Cancels progress work, sets `calibration_active = false`, loads `pulses_per_liter` from `get_flow_calibration()`, clears `volume_ml` | Notifies updated idle frame; leaves `pulses` at last sample |
| `0x03` CALCULATED | Finalise measurement | `calibration_active` true AND `volume_ml > 0` AND pulses>0 | Computes `pulses_per_liter = pulses*1000 / volume_ml`, stores final `pulses`, clears `calibration_active`, cancels progress work | Notifies result (`action = 0x03`) |
| `0x04` APPLY | Persist new calibration | `pulses_per_liter != 0` | Calls `watering_set_flow_calibration()`, saves to NVS (returns `BT_ATT_ERR_UNLIKELY` if persistence fails), clears fields, sets `action = 0`, reloads actual system calibration | Notifies refreshed idle frame |
| `0x05` RESET | Restore default | none | Applies `DEFAULT_PULSES_PER_LITER`, saves config, clears state, sets `action = 0`, ensures `calibration_active = false` | Notifies idle frame with default constant |
| `0x02` IN_PROGRESS | - | - | Read-only value used in notifications while counting | Any attempt to write returns `BT_ATT_ERR_VALUE_NOT_ALLOWED` |

Invalid command sequences (e.g., CALCULATED when idle, APPLY with zero result, START while already active) return `BT_ATT_ERR_VALUE_NOT_ALLOWED`. START while already running only logs a warning and leaves the current run intact.

## Read Path (`read_calibration`)
1. Copies the current `calibration_value` buffer.
2. If `calibration_active` is true, recomputes `pulses` from the hardware counter and forces `action = 0x02` before returning.
3. If idle, simply mirrors whatever the last state machine wrote (`action` typically 0, 3 after CALCULATED, etc.) and refreshes `pulses_per_liter` with `get_flow_calibration()`.
4. Returns the packed struct via `bt_gatt_attr_read()`.

## Write Path (`write_calibration`)
- Rejects any frame that is not 13 bytes at offset 0 with `BT_ATT_ERR_INVALID_OFFSET`.
- Updates the backing buffer then executes the action switch above.
- CALCULATED failures (`volume_ml == 0` or zero pulses) clear `calibration_active`, set `action = 0`, and surface `BT_ATT_ERR_VALUE_NOT_ALLOWED`.
- APPLY/RESET propagate `BT_ATT_ERR_UNLIKELY` when either `watering_set_flow_calibration()` or `watering_save_config_priority(true)` fails.

`pulses` from the client is ignored for every action; firmware always derives counts from the current hardware counters.

## Notification Behaviour
- `calibration_ccc_changed()` primes the buffer (action=0, pulses=0, volume=0, `pulses_per_liter = get_flow_calibration()`) and pushes an immediate snapshot when notifications are enabled.
- `calibration_progress_work_handler` runs every 200 ms while `calibration_active` is true, updates `pulses`, forces `action = 0x02`, and calls `bt_irrigation_calibration_notify()`. Normal-priority throttling may stretch the effective cadence; congestion also increases the adaptive interval.
- State transitions inside `write_calibration` trigger direct notifications (START/STOP/CALCULATED/APPLY/RESET) whenever CCC is enabled and a connection is present.
- Disabling notifications clears `calibration_value` to zeros so stale values are not leaked to future subscribers.

## Client Guidance
- Always write the full 13-byte struct. For commands that do not need `pulses`/`pulses_per_liter`, fill them with zero; firmware replaces them.
- Record the actual dispense volume before sending CALCULATED-if the write fails with `VALUE_NOT_ALLOWED`, retry only after restarting the calibration run.
- APPLY commits immediately to persistent storage; issue a follow-up read to verify the new `pulses_per_liter` and consider re-reading `06-system-configuration` to confirm.
- STOP is purely an abort; it does not compute a result. To obtain the measured constant, go straight from START -> CALCULATED.
- There is no firmware timeout; clients should enforce a watchdog in case water flow stalls.

## Troubleshooting
| Symptom | Likely Cause | Mitigation |
|---------|--------------|------------|
| Progress notifications stop mid-run | Notifications throttled or CCC disabled | Ensure the BLE link is active; normal priority throttle resumes automatically after congestion |
| CALCULATED returns `VALUE_NOT_ALLOWED` | No pulses counted or `volume_ml` = 0 | Verify flow sensor wiring, ensure volume field is non-zero, and restart calibration |
| APPLY returns `UNLIKELY_ERROR` | `watering_set_flow_calibration()` rejected the value | Read current calibration via System Configuration, ensure value is within acceptable range, or RESET to default |
| Action stays at 0 after START | START was issued while another run active | Issue STOP first or wait for the current run to complete (check logs for "already in progress") |

## Firmware References
- `src/bt_irrigation_service.c`: `read_calibration`, `write_calibration`, `calibration_ccc_changed`, `calibration_progress_work_handler`, `bt_irrigation_calibration_notify`.
- `src/bt_irrigation_service.c`: global `calibration_active`, `calibration_start_pulses`, and advanced notification infrastructure.
- `src/watering.c`: `watering_set_flow_calibration`, `watering_save_config_priority` (persistent storage path).
- `src/flow_sensor.c`: `reset_pulse_count()`, `get_pulse_count()` helpers used during calibration.
