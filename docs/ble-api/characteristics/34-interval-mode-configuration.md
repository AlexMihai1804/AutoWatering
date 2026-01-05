# Interval Mode Configuration (Cycle & Soak) (UUID: 12345678-1234-5678-9abc-def123456785)

This characteristic exposes the **ON/OFF timing** parameters used by the firmware's interval mode execution (a.k.a. "Cycle & Soak" splitting).

> Note: The simple per-channel toggle `enable_cycle_soak` remains part of **[Growing Environment](14-growing-environment.md)** (offset 15). This characteristic controls the **durations** used when interval mode is enabled.

## Operation Summary
| Operation | Payload | Size | Notes |
|-----------|---------|------|------|
| Read | `struct interval_mode_config_data` | 16 B | Returns configuration for the last selected channel (defaults to channel 0) |
| Write | Channel select byte | 1 B | Sets cached channel context for subsequent reads |
| Write | `struct interval_mode_config_data` | 16 B | Updates per-channel interval timing + enable flag |
| Notify | `struct interval_mode_config_data` | 16 B | Emitted after a successful config write |

## Characteristic Metadata
| Item | Value |
|------|-------|
| Service | Custom Configuration Service |
| UUID | `12345678-1234-5678-9abc-def123456785` |
| Properties | Read, Write, Notify |
| Permissions | Read/Write require encryption |
| Payload Size | 16 bytes (`sizeof(struct interval_mode_config_data)`) |

## Payload Layout (`struct interval_mode_config_data`)
Little-endian encoding for multi-byte fields.

| Offset | Field | Type | Size | Access | Notes |
|--------|-------|------|------|--------|-------|
| 0 | `channel_id` | `uint8_t` | 1 | RW | 0-7 |
| 1 | `enabled` | `uint8_t` | 1 | RW | 0=disabled, 1=enabled |
| 2 | `watering_minutes` | `uint16_t` | 2 | RW | 0-60 |
| 4 | `watering_seconds` | `uint8_t` | 1 | RW | 0-59 |
| 5 | `pause_minutes` | `uint16_t` | 2 | RW | 0-60 |
| 7 | `pause_seconds` | `uint8_t` | 1 | RW | 0-59 |
| 8 | `configured` | `uint8_t` | 1 | R | Mirrors the runtime gate used by execution (derived) |
| 9 | `last_update` | `uint32_t` | 4 | R | Best-effort timestamp for last update |
| 13 | `reserved` | `uint8_t[4]` | 4 | RW | Write zeros |

## Client Guidance
- To **read** a specific channel:
  1. Write 1 byte: `channel_id`.
  2. Read the characteristic.
- To **set timings**:
  - Write the full 16-byte struct with desired `enabled` and timing fields.
- Disabling interval mode (`enabled=0`) keeps durations stored, but prevents interval execution.

## Firmware References
- `src/bt_custom_soil_handlers.c`: GATT characteristic definition and channel selection cache.
- `src/bt_interval_mode_handlers.c`: Applies and validates timing config.
- `src/interval_task_integration.c`: Uses `interval_timing_is_configured()` as the runtime gate.
