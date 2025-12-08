# Custom Soil Configuration Characteristic (UUID: 12345678-1234-5678-9abc-def123456781)

Part of the **Custom Configuration Service** (`12345678-1234-5678-9abc-def123456780`), separate from the main Irrigation Service. Provides CRUD for per-channel custom soil profiles stored in NVS.

## Quick overview
- **Properties**: Read, Write, Notify (no fragmentation).
- **Security**: Read/Write Encrypt (pairing required).
- **Size**: 70 B fixed (`custom_soil_config_data`).
- **CCC**: single CCC for this characteristic.
- **Flow**: client writes with `operation` = {read/create/update/delete}; firmware replies with populated struct + `status`. If CCC is ON, it also notifies with the same final payload.

## Payload structure (`custom_soil_config_data`, 70 B)
| Offset | Field | Type | Notes |
|--------|-------|------|------|
| 0 | `channel_id` | u8 | 0-7 |
| 1 | `operation` | u8 | 0=read, 1=create, 2=update, 3=delete |
| 2 | `name[32]` | char | UTF-8, NUL or padded; required on create/update |
| 34 | `field_capacity` | float | % 0-100 |
| 38 | `wilting_point` | float | % 0-100 |
| 42 | `infiltration_rate` | float | mm/hr |
| 46 | `bulk_density` | float | g/cm³ |
| 50 | `organic_matter` | float | % 0-100 |
| 54 | `created_timestamp` | u32 | Unix sec; filled by FW |
| 58 | `modified_timestamp` | u32 | Unix sec; filled by FW |
| 62 | `crc32` | u32 | computed by FW |
| 66 | `status` | u8 | result (`watering_error_t`) |
| 67 | `reserved[3]` | u8 | must be 0 |

Little-endian pentru câmpurile multi-byte.

## Validation & rules
- `channel_id` < 8, `operation` ∈ {0,1,2,3}.
- For `operation` 1/2: `name` non-empty; percentages in 0–100; density/infiltration >0.
- For `operation` 3: numeric fields ignored; deletes entry if present.
- Offset != 0 or `len` != 76 ⇒ `BT_ATT_ERR_INVALID_ATTRIBUTE_LEN`.
- Timestamp/CRC fields may be 0 from client; FW overwrites them.

## Operation semantics
- **Write op=1 (create)**: create/replace custom soil for channel, sets timestamps/CRC, `status=0` on success.
- **Write op=2 (update)**: update values; if missing, behaves like create.
- **Write op=3 (delete)**: delete custom soil for channel; `status=0` on success, `-ENOENT` if absent.
- **Write op=0 (read)**: ignores other fields; FW responds with current struct and `status` (`0` or `-ENOENT`).

## Read
Simple read returns the last response buffer (full struct with `status`). You can read right after a write if you don’t want to rely on notify.

## Notify
If CCC is set to Notify, after each write the FW sends the final struct (with `status` + current timestamps). No fragmentation; single packet.

## Error mapping (`status`)
- `0` = success.
- `-22` (`WATERING_ERROR_INVALID_PARAM`) = bad channel/op/values.
- `-2` (`-ENOENT`) = no custom soil on read/delete.
- Other negatives: NVS/storage or validation failures (see logs).

## Usage recipes
### 1) Create / Update
1. Pair (encrypted link).
2. Write `operation=1` (or 2), fill name + parameters.
3. Check write response; optionally read/notify to confirm (`status==0`).

### 2) Read
1. Write with `operation=0` + `channel_id`.
2. Read characteristic (or wait notify) → `status=0` + fields if present; `status=-ENOENT` if missing.

### 3) Delete
1. Write `operation=3`, set `channel_id`.
2. Confirm `status==0` in notify/read; `-ENOENT` if nothing existed.

### 4) Integrate with channel config
- In `enhanced_channel_config_data`, set `use_custom_soil=1` + custom fields. Handler auto-creates/updates the DB entry.
- To remove, set `use_custom_soil=0` (triggers delete).

## Example (pseudo JS Web Bluetooth)
```js
// Write create/update
const buf = new ArrayBuffer(76);
const dv = new DataView(buf);
dv.setUint8(0, 0);   // channel_id
dv.setUint8(1, 1);   // operation = create
new TextEncoder().encodeInto("MySoil", new Uint8Array(buf, 2, 32));
dv.setFloat32(34, 32.5, true); // field_capacity %
dv.setFloat32(38, 15.0, true); // wilting_point %
dv.setFloat32(42, 12.0, true); // infiltration mm/hr
dv.setFloat32(46, 1.35, true); // bulk_density g/cm3
dv.setFloat32(50, 4.0, true);  // organic_matter %
// rest 0 -> FW will fill
await characteristic.writeValueWithResponse(buf);
// Optionally read back:
const rd = await characteristic.readValue();
const status = rd.getUint8(66);
```

## Integration notes
- Custom Configuration Service base UUID `12345678-1234-5678-9abc-def123456780`; characteristic is `...781`.
- No fragmentation; no unified history header.
- Onboarding: custom soil does not set onboarding flags, but updates config status (`config_custom_soil_complete` in `enhanced_channel_config_data` / config status).
- Persistence: stored in NVS via `custom_soil_db.c`; deleting a channel does not auto-delete custom soil unless op=3 or `use_custom_soil=0` is sent through enhanced channel config.

## Related
- Handlers: `src/bt_custom_soil_handlers.c`
- Structs: `src/bt_gatt_structs_enhanced.h`
- Custom soil DB: `src/custom_soil_db.c`, `custom_soil_db.h`
- Enhanced channel config integration: `bt_convert_from_enhanced_ble_config` (auto create/update/delete on `use_custom_soil`)
