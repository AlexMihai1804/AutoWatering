# AutoWatering - Zephyr RTOS Smart Irrigation System

[![Zephyr RTOS](https://img.shields.io/badge/Zephyr-RTOS-blue)](https://www.zephyrproject.org/)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

AutoWatering is a Zephyr v4.3.0-based irrigation controller for the nRF52840. It drives up to 8 independent zones, handles master-valve sequencing, monitors flow/rain/environmental sensors, and exposes a full Bluetooth Low Energy (BLE) API for mobile/desktop clients.

## Highlights

- 8 irrigation channels with duration/volume/automatic FAO-56 modes (Quality 100% / Eco 70%).
- Master valve orchestration with pre/post delays and overlap grace.
- Rain gauge + BME280 integration, environmental and watering histories.
- FAO-56 water balance engine fed by a 223-species plant database (see docs/plant-database-fao56-system.md).
- BLE control + notifications with fragmentation helpers for large payloads.
- Single active watering task enforced via queueing; interval mode supported.
- Persistent config + watering history stored in NVS; rain/environment history stored on external SPI flash (LittleFS) when `CONFIG_HISTORY_EXTERNAL_FLASH=y`.
- Encrypted GATT access (Just Works, security level 2) with optional bonding for faster reconnects.

## Supported Targets

- `arduino_nano_33_ble` (primary hardware target; board overlays under `boards/`).

## Quick Start

```bash
west init -m https://github.com/AlexMihai1804/AutoWatering.git --mf west-manifest/west.yml
west update
cd AutoWatering
west build -b arduino_nano_33_ble --pristine
west flash
```

For full environment setup (WSL/Linux/macOS), board overlays, and simulator flags, see `docs/INSTALLATION.md`.

## Documentation Map

- Platform setup & builds: `docs/INSTALLATION.md`
- System architecture: `docs/system-architecture.md`
- Feature overview (concise/full): `docs/FEATURES.md`, `docs/FEATURES_FULL.md`
- BLE API (UUIDs, fragmentation, characteristic deep dives): `docs/ble-api/README.md` + `docs/ble-api/characteristics/`
- Plant database + FAO-56 engine + complete species list: `docs/plant-database-fao56-system.md`
- Troubleshooting: `docs/TROUBLESHOOTING.md`

## BLE at a Glance

- Primary irrigation service UUID: `12345678-1234-5678-1234-56789abcdef0`
- Custom configuration service UUID: `12345678-1234-5678-9abc-def123456780`
- 34 total characteristics (29 irrigation service + 5 custom configuration service).
- Single peripheral connection (`CONFIG_BT_MAX_CONN=1`).
- Encrypted access required for all characteristics (BLE security level 2); `CONFIG_BT_MAX_PAIRED=5` bond slots.
- Write payloads stay 20 bytes for compatibility; large structs use the shared fragmentation headers.
- Full characteristic list, properties, and ATT behaviors are maintained in `docs/ble-api/README.md` (kept in sync with `src/bt_irrigation_service.c`).

## Time Handling & Timezone Support

- RTC timestamps are stored in UTC and converted to local time via `timezone_config_t` (offset + DST rules).
- The Timezone characteristic remains active in BLE; scheduling, histories, and user-facing timestamps honor the configured offset/DST.
- Default build ships with UTC (no DST); clients can update timezone over BLE, and values persist in NVS.

## Plants, Soils, and FAO-56

- 223 curated plant entries, 15 soil profiles, and 15 irrigation method profiles are compiled into flash (`src/plant_full_db.*`, `src/soil_enhanced_db.*`, `src/irrigation_methods_db.*`).
- The FAO-56 pipeline (Penman-Monteith + fallbacks) drives automatic irrigation volumes and is shared between autonomous runs and BLE preview calls.
- Regeneration tooling (`tools/build_database.py`) keeps datasets auditable; see `docs/plant-database-fao56-system.md` for structure, algorithms, and the full species roster.

## Contributing

Contributions are welcome! Please read [CONTRIBUTING.md](CONTRIBUTING.md) for guidelines.

## License

MIT License. See [LICENSE](LICENSE) for details.
