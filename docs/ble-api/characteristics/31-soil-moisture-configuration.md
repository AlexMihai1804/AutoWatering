# Soil Moisture Configuration Characteristic (UUID: 12345678-1234-5678-9abc-def123456784)

Part of the **Custom Configuration Service** (`12345678-1234-5678-9abc-def123456780`), separate from the main Irrigation Service.

This characteristic configures the **antecedent soil moisture estimate** used by the FAO‑56 **effective precipitation / runoff** model. It supports:
- a **global** override (applies to all channels)
- an optional **per-channel** override (takes precedence over global)

## Quick overview
- **Properties**: Read, Write, Notify (no fragmentation).
- **Security**: Read/Write Encrypt (pairing required).
- **Size**: 8 B fixed (`soil_moisture_config_data`).
- **CCC**: single CCC for this characteristic.
- **Flow**: client writes a request; firmware updates the in-memory response buffer and (if CCC is ON) notifies the final response.

## Payload structure (`soil_moisture_config_data`, 8 B)
| Offset | Field | Type | Notes |
|--------|-------|------|------|
| 0 | `channel_id` | u8 | `0..7` = per-channel override, `0xFF` = global |
| 1 | `operation` | u8 | `0` = read, `1` = set |
| 2 | `enabled` | u8 | `0` = disable override, `1` = enable override |
| 3 | `moisture_pct` | u8 | `0..100` (%), antecedent moisture estimate |
| 4 | `status` | u8 | result (`watering_error_t`) |
| 5 | `has_data` | u8 | `0` = no stored value (defaults in use), `1` = value present in NVS |
| 6 | `reserved[2]` | u8 | must be `0` |

All fields are single-byte (endianness does not apply).

## Effective moisture precedence
The FAO‑56 layer resolves an “effective” antecedent moisture percent per channel like this:

1. If **per-channel override** is enabled for that channel → use that `moisture_pct`
2. Else if **global override** is enabled → use global `moisture_pct`
3. Else → use the default `50%`

## Validation & rules
- `channel_id` must be `0xFF` or `< 8`.
- `operation` must be `0` or `1`.
- For `operation=1` (set): `moisture_pct` must be `0..100`.
- Offset must be `0` and `len` must be exactly `8`.

Invalid requests return ATT errors (`BT_ATT_ERR_VALUE_NOT_ALLOWED` or `BT_ATT_ERR_INVALID_ATTRIBUTE_LEN`) and set `status` to `WATERING_ERROR_INVALID_PARAM`.

## Operation semantics
### Read (`operation = 0`)
- `channel_id = 0xFF`: returns the global override state.
- `channel_id = 0..7`: returns the per-channel override state for that channel.

Fields `enabled` and `moisture_pct` in the response are filled by firmware.

### Set (`operation = 1`)
- `channel_id = 0xFF`: sets global override enable + percent.
- `channel_id = 0..7`: sets per-channel override enable + percent.

On success: `status = 0`.

## Read
A GATT read returns the last response buffer (8 bytes). Typically you:
1) write a READ request, then
2) read back the characteristic (or rely on notify).

## Notify
If CCC is set to Notify, firmware sends an 8-byte notification after each valid write.

## Persistence
- Global config is persisted in NVS.
- Per-channel overrides are persisted in NVS as individual records per channel.

### Missing-data marker
If no NVS record exists yet (fresh device / after an NVS erase), the firmware **auto-seeds default records** during boot so the client does not need to explicitly set soil moisture.

- After successful seeding, reads return `has_data=1` with defaults (`enabled=0`, `moisture_pct=50`).
- If seeding fails (e.g., NVS not available), firmware still reports usable defaults but may return `has_data=0`.

## Usage recipes
### 1) Read global
- Write: `channel_id=0xFF`, `operation=0` (other fields can be 0)
- Read or wait notify: returns `enabled` + `moisture_pct`

### 2) Set per-channel override (example: channel 3, 65%)
- Write: `channel_id=3`, `operation=1`, `enabled=1`, `moisture_pct=65`
- Confirm `status==0`

### 3) Disable override (fall back to global/default)
- Write: `channel_id=3`, `operation=1`, `enabled=0`, `moisture_pct` ignored (but still must be 0..100)

## Related
- Handlers: `src/bt_custom_soil_handlers.c`
- Structs: `src/bt_gatt_structs_enhanced.h`
- Runtime config cache: `src/soil_moisture_config.c`
- Persistence helpers: `src/nvs_config.c`
