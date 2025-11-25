# Current Task Status Characteristic (UUID: deff)

> Operation Summary
| Operation | Payload | Size | Fragmentation | Notes |
|-----------|---------|------|---------------|-------|
| Read | `struct current_task_data` | 21 B | None | Returns the live watering task or the idle sentinel |
| Write | Control opcode (`uint8_t`) | 1 B | None | `0` stop, `1` pause, `2` resume (state validated) |
| Notify | `struct current_task_data` | 21 B | None | Immediate on state changes and every 2 s while actively watering |

The characteristic mirrors the dispatcher's execution state so clients can display progress bars, elapsed time, and actual volume. Updates are driven both by the task lifecycle (start, pause, resume, stop) and by a 2-second periodic worker that runs only while watering is in progress.

## Characteristic Metadata
| Item | Value |
|------|-------|
| UUID | `12345678-1234-5678-1234-56789abcdeff` |
| Properties | Read, Write, Notify |
| Permissions | Read, Write |
| Payload Size | 21 bytes (`BUILD_ASSERT(sizeof(struct current_task_data) == 21)`) |
| Notification Priority | Normal (`safe_notify`) |
| Periodic Worker | `current_task_periodic_work` at 2-second cadence while running |

## Payload Layout (`struct current_task_data`)
| Offset | Field | Type | Description |
|--------|-------|------|-------------|
| 0 | `channel_id` | `uint8_t` | Channel executing the task (`0xFF` when idle) |
| 1 | `start_time` | `uint32_t` | Task start time (seconds, derived from uptime) |
| 5 | `mode` | `uint8_t` | `0` duration-based, `1` volume-based |
| 6 | `target_value` | `uint32_t` | Target duration (s) or target volume (ml) |
| 10 | `current_value` | `uint32_t` | Elapsed seconds (duration mode) or dispensed volume (ml) |
| 14 | `total_volume` | `uint32_t` | Total volume dispensed (ml) from flow sensor pulses |
| 18 | `status` | `uint8_t` | Task state (see table below) |
| 19 | `reserved` | `uint16_t` | Elapsed seconds in volume mode; zero in duration mode |

### Status Values
| Value | Meaning | Behaviour |
|-------|---------|-----------|
| `0` | Idle | No active task (`channel_id = 0xFF`, counters zeroed) |
| `1` | Running | Task in progress; periodic notifications enabled. **Note:** Includes interval-mode wait phases (valve closed but task active). |
| `2` | Paused | Task paused by user command; periodic worker keeps scheduling but early-exits |
| `3` | Completed | Reserved for future use (current firmware transitions directly to Idle) |

### Field Notes
- `start_time` is derived from `watering_task_state.watering_start_time` (ms uptime) and converted to seconds. It does not pause while the device sleeps.
- `total_volume` derives from the global flow pulse counter and therefore represents actual dispensed water in both duration and volume modes.
- In volume mode `current_value` equals `total_volume`, while `reserved` stores the elapsed time so clients can still show timers.
- `target_value` is sourced from the active watering event (`duration_minutes * 60` or `volume_liters * 1000`).

## Behaviour

### Read (`read_current_task`)
- Builds a fresh snapshot every time by interrogating `watering_get_current_task()` and the shared `watering_task_state` structure.
- When no task is active the handler returns the idle frame and clears progress values.
- For running tasks the handler recalculates elapsed time, subtracting paused intervals, and obtains total volume from the flow sensor calibration (`pulses_per_liter`).

### Write (`write_current_task`)
| Opcode | Action | Preconditions | ATT error when invalid |
|--------|--------|---------------|------------------------|
| `0x00` | Stop/Cancel | Task must be running or paused | `WRITE_NOT_PERMITTED` if nothing to stop |
| `0x01` | Pause | Task must be running | `WRITE_NOT_PERMITTED` if idle/already paused |
| `0x02` | Resume | Task must be paused | `WRITE_NOT_PERMITTED` if not paused |

- Writes must be exactly 1 byte at offset `0`. Different lengths -> `BT_ATT_ERR_INVALID_ATTRIBUTE_LEN`; non-zero offset -> `BT_ATT_ERR_INVALID_OFFSET`; other opcodes -> `BT_ATT_ERR_VALUE_NOT_ALLOWED`.
- Successful commands update `current_task_value` and invoke `bt_irrigation_current_task_notify()` immediately so subscribers see the new state without waiting for the periodic tick.

### Notifications (`bt_irrigation_current_task_notify` / `current_task_ccc_changed`)
- CCC enable populates the cached struct with the current task (or idle sentinel), logs the subscription, and schedules the 2-second delayable worker.
- `bt_irrigation_current_task_notify()` is called by multiple paths: task lifecycle hooks, user writes, queue transitions, and the periodic worker when a task is running. Each call refreshes the struct before notifying to avoid stale measurements.
- Notifications carry only the 21-byte struct (no unified header) and use the Normal-priority `safe_notify` helper.
- The periodic worker keeps rescheduling itself every 2 seconds while CCC is enabled; it sends updates only when the dispatcher reports `task_in_progress` and not paused.
- CCC disable clears `current_task_value`, cancels the worker, and stops further notifications.

## Client Guidance
- Treat `channel_id = 0xFF` as the canonical idle indicator; ignore the other fields in that frame.
- Duration-mode progress = `current_value / target_value`; Volume-mode progress = `current_value / target_value` while `reserved` provides UI timer support.
- `total_volume` continues to accumulate even for duration tasks, enabling water-usage reporting regardless of control mode.
- For pause/resume workflows, expect a notification reflecting the new state immediately after a successful write.
- Because `start_time` is based on uptime, devices without an RTC still provide relative timing, but absolute timestamps should be derived on the client if necessary.

## Firmware References
- `src/bt_irrigation_service.c`: `read_current_task`, `write_current_task`, `current_task_ccc_changed`, `bt_irrigation_current_task_notify`, `current_task_periodic_work_handler`.
- `src/watering_tasks.c`, `src/watering.c`: task scheduler, flow integration, and pause/resume helpers invoked by the BLE handlers.