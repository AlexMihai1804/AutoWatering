# Reset Control Characteristic (UUID: 12345678-1234-5678-1234-56789abcde21)

> Operation Summary
| Operation | Payload | Size | Notes |
|-----------|---------|------|-------|
| Read | `struct reset_control_data` | 16 B | Pending confirmation snapshot or idle state |
| Write | `struct reset_control_data` | 16 B | `confirmation_code = 0` -> request; otherwise execute |
| Notify | `struct reset_control_data` | 16 B | Sent after code generation and after successful execution |

Two-stage safeguard for destructive actions. Clients request a confirmation code, then present it to execute the selected reset. Responses fit within a single ATT PDU (no fragmentation).

## Characteristic Metadata
| Item | Value |
|------|-------|
| Properties | Read, Write, Notify |
| Permissions | Read, Write |
| Notification Priority | Normal (`NOTIFY_PRIORITY_NORMAL`, adaptive throttle ~=200 ms) |
| Notification Helper | `safe_notify` (no header, raw struct) |
| Confirmation Timeout | 300 s (`RESET_CONFIRMATION_VALIDITY_SEC`) |

## Payload Layout (`struct reset_control_data`)
| Offset | Field | Type | Notes |
|--------|-------|------|-------|
| 0 | `reset_type` | `uint8_t` | External opcode (table below) |
| 1 | `channel_id` | `uint8_t` | Required for channel-scoped resets, `0xFF` when idle |
| 2 | `confirmation_code` | `uint32_t` | Little-endian; `0` requests generation |
| 6 | `status` | `uint8_t` | Wipe state (see Status Semantics) |
| 7 | `timestamp` | `uint32_t` | Generation time (s since boot epoch) when pending |
| 11 | `reserved[0]` | `uint8_t` | Progress percentage (0-100) during factory wipe |
| 12 | `reserved[1]` | `uint8_t` | Current wipe step (see Wipe Steps table) |
| 13 | `reserved[2]` | `uint8_t` | Retry attempt count for current step |
| 14 | `reserved[3..4]` | `uint16_t` | Last error code (LE), 0 = no error |

Reads and notifications mirror this structure byte-for-byte.

## Status Semantics (`status` byte)
| Value | Name | Description |
|-------|------|-------------|
| `0x00` | IDLE | No operation pending or in progress |
| `0x01` | AWAIT_CONFIRM | Confirmation code is active, waiting for execution |
| `0x02` | IN_PROGRESS | Factory wipe executing step-by-step |
| `0x03` | DONE_OK | Factory wipe completed successfully |
| `0x04` | DONE_ERROR | Factory wipe failed (check `reserved[3..4]` for error) |

Note: Status values `0x02-0x04` only apply to factory reset (`0xFF`). Other reset types complete synchronously.

## Wipe Steps (`reserved[1]` during IN_PROGRESS)
| Value | Step | Description |
|-------|------|-------------|
| 0 | PREPARE | Initialize wipe, persist state |
| 1 | RESET_CHANNELS | Reset all 8 channel configurations |
| 2 | RESET_SYSTEM | Reset system configuration |
| 3 | RESET_CALIBRATION | Reset calibration data |
| 4 | CLEAR_RAIN_HIST | Clear rain history (flash erase) |
| 5 | CLEAR_ENV_HIST | Clear environmental history |
| 6 | CLEAR_ONBOARDING | Clear onboarding NVS flags |
| 7 | VERIFY | Verify all data erased |
| 8 | DONE | Cleanup and finalize |

## Supported Reset Opcodes
| Code | Description | Channel Required | Firmware Action |
|------|-------------|------------------|-----------------|
| `0x01` | Reset channel configuration | Yes | Restores enhanced channel config, clears onboarding bits, wipes name & water balance |
| `0x02` | Reset channel schedules | Yes | Clears onboarding schedule flag (schedule storage reset placeholder) |
| `0x10` | Reset all channel configurations | No | Iterates channels, invoking channel reset routine |
| `0x11` | Reset all schedules | No | Iterates channels, clearing schedule flags |
| `0x12` | Reset system configuration | No | Restores timezone defaults, clears system flags, resets automatic calc state |
| `0x14` | Reset history | No | Clears rain history and related counters |
| `0xFF` | Factory reset | No | Executes every supported reset plus calibration cleanup |

Opcodes `0x03`, `0x04`, `0x13`, `0x15`, and any other values return `BT_ATT_ERR_VALUE_NOT_ALLOWED`.

## Status Semantics
- `0x01`: A confirmation code is active. `reset_type`, `channel_id`, `confirmation_code`, and `timestamp` describe the pending request.
- `0xFF`: No code is active. Fields collapse to idle defaults (`reset_type = channel_id = 0xFF`, `confirmation_code = timestamp = 0`).

There are no in-progress or failure states in notifications; failures surface as write errors.

## Factory Wipe Progress Tracking

Factory reset (`0xFF`) uses a persistent state machine that survives reboots:

1. **Initiation** - Client writes with valid confirmation code, firmware responds immediately with `status=0x02` (IN_PROGRESS).
2. **Step Execution** - Firmware executes steps sequentially, persisting progress to NVS after each step.
3. **Progress Notifications** - After each step, firmware sends a notification with updated `reserved[]` fields.
4. **Completion** - When all steps complete, `status` transitions to `0x03` (DONE_OK) or `0x04` (DONE_ERROR).
5. **Resume on Reboot** - If device reboots during wipe, it resumes from the last completed step.
6. **Acknowledgment** - After app observes DONE_OK/DONE_ERROR, it should write any request to clear the state (optional).

### Timing Expectations
| Step | Typical Duration |
|------|------------------|
| RESET_CHANNELS | ~4s (8 channels, NVS writes) |
| CLEAR_RAIN_HIST | ~1s (flash erase) |
| CLEAR_ONBOARDING | ~1.5s (NVS deletes) |
| **Total** | ~6-8s |

Progress notifications allow the app to show a progress bar without timeout concerns.

## Two-Step Workflow (Non-Factory Resets)
1. **Request** - Write struct with desired `reset_type`, `channel_id`, and `confirmation_code = 0`.
2. **Notify** - Firmware generates a random non-zero code, persists it with a 5-minute expiry, and notifies subscribers (Normal priority, ~=200 ms queue spacing).
3. **Execute** - Client writes the same struct with the returned `confirmation_code`.
4. **Complete** - On success, firmware clears the code, performs the reset, emits an idle notification, and (for applicable resets) triggers an onboarding status refresh.

Requesting a new code replaces any previous confirmation regardless of type.

## ATT Error Mapping
| Condition | ATT Error |
|-----------|-----------|
| Unsupported opcode or improper channel | `BT_ATT_ERR_VALUE_NOT_ALLOWED` |
| Wrong confirmation code | `BT_ATT_ERR_AUTHENTICATION` |
| Expired confirmation code (>300 s) | `BT_ATT_ERR_AUTHORIZATION` |
| Storage/NVS failure | `BT_ATT_ERR_INSUFFICIENT_RESOURCES` |
| Other internal fault | `BT_ATT_ERR_UNLIKELY` |

Errors are reported synchronously as the write response; no notification follows.

## Client Guidance
- Enable notifications before requesting a code; unlike other characteristics, CCC enable does not push an initial snapshot.
- Cache the pending struct until execution succeeds; the notification payload can be used verbatim (modulo code) for the second write.
- Guard destructive operations (especially factory reset) with additional user confirmation layers.
- If a reset fails with `AUTHORIZATION`, request a fresh code before retrying.

### Minimal Example (TypeScript/JS)
```javascript
async function performReset(characteristic, type, channel = 0) {
  const buf = new DataView(new ArrayBuffer(16));
  buf.setUint8(0, type);
  buf.setUint8(1, channel);
  await characteristic.writeValue(buf.buffer); // request code

  const code = await new Promise((resolve, reject) => {
    const handler = event => {
      const view = new DataView(event.target.value.buffer);
      if (view.getUint8(6) === 0x01 && view.getUint8(0) === type && view.getUint8(1) === channel) {
        characteristic.removeEventListener('characteristicvaluechanged', handler);
        resolve(view.getUint32(2, true));
      }
    };
    characteristic.addEventListener('characteristicvaluechanged', handler, { once: false });
    setTimeout(() => reject(new Error('Timeout waiting for confirmation code')), 10000);
  });

  const exec = new DataView(new ArrayBuffer(16));
  exec.setUint8(0, type);
  exec.setUint8(1, channel);
  exec.setUint32(2, code, true);
  await characteristic.writeValue(exec.buffer);
}
```

## Safety & Expiry
- Confirmation codes expire after 300 s; when expired the second write returns `BT_ATT_ERR_AUTHORIZATION`.
- Factory reset chains every supported operation (including calibration clear) and may reboot the device.
- Storage errors (`BT_ATT_ERR_INSUFFICIENT_RESOURCES`) indicate NVS issues; user intervention is required before retrying.

## Firmware Reference Points
- `write_reset_control()` - opcode validation, error mapping, notification triggers.
- `reset_controller_generate_confirmation_code()` - random code generation and persistence.
- `reset_controller_start_factory_wipe()` - initiates persistent wipe state machine.
- `reset_controller_execute_wipe_step()` - executes single step with retry logic.
- `reset_controller_resume_wipe()` - called at boot to resume interrupted wipes.
- `bt_irrigation_reset_control_notify()` - Normal-priority `safe_notify` emitter without fragmentation.
- `wipe_step_work_handler()` - async work queue handler for step execution.