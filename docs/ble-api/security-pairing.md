# BLE Security & Pairing (Current Posture)

Status: VERIFIED (doc aligned with firmware - Jan 2026)

This document states the *actual* security posture of the current firmware. The device operates in a secure mode where encryption is **enforced** for all characteristic reads and writes.

## 1. What Is Implemented Now

| Aspect | Current Behavior | Source / Config |
| --- | --- | --- |
| Bluetooth mode | Peripheral, single connection | `CONFIG_BT_PERIPHERAL=y`, `CONFIG_BT_MAX_CONN=1` |
| SMP / bonding capability | Enabled and required for access | `CONFIG_BT_SMP=y`, `CONFIG_BT_MAX_PAIRED=1` |
| Required security level for characteristics | **Encryption Required** (Level 2) | Service implementation (`bt_irrigation_service.c`, `bt_custom_soil_handlers.c`) |
| Encryption on link | Enforced by GATT permissions | `BT_GATT_PERM_READ_ENCRYPT`, `BT_GATT_PERM_WRITE_ENCRYPT` |
| Authentication (MITM / LESC) | Callbacks implemented (Logging) | `bt_conn_auth_cb` registered (Passkey display/entry via logs) |
| Authorization / roles | Not present | No application role table |
| At-rest data encryption | Not enabled (plain NVS) | NVS config in `prj.conf` |
| Single bonded device limit | Configured | `CONFIG_BT_MAX_PAIRED=1` |

Effectively: the firmware accepts connections but **rejects** any read/write attempts to characteristics until the link is encrypted (paired/bonded).

## 2. Security Enforcement

All GATT characteristics in the Irrigation Service and Custom Soil Service are configured with:

- `BT_GATT_PERM_READ_ENCRYPT`
- `BT_GATT_PERM_WRITE_ENCRYPT`

This means:

1. A central device can connect.
2. It can discover services and characteristics.
3. However, attempting to **Read** or **Write** any value (including CCCDs) will trigger an `Insufficient Encryption` error if the link is not encrypted.
4. This typically triggers the OS (Android/iOS/Windows) to initiate the pairing process automatically.

## 3. Practical Implications & Risks

| Risk | Impact | Current Mitigation | Residual Exposure |
| --- | --- | --- | --- |
| Unauthorized nearby control | Configuration / valve actuation | **Encryption Required** | Requires pairing (Just Works) |
| Passive eavesdropping | Leak of schedules/history | **Link Encryption** | Protected against passive sniffing after pairing |
| MITM during connection | Command injection | None (Just Works) | Vulnerable during initial pairing if attacker is present |
| Replay of writes | Duplicate commands | Encrypted Link | Protected by BLE link layer security |

## 4. Recommended Near-Term Hardening Roadmap

1. **[COMPLETED]** Add passkey or numeric comparison callbacks (Zephyr: implement `bt_conn_auth_cb` + `bt_conn_auth_info_cb`) to mitigate MITM during pairing.
2. Introduce a lightweight application token (e.g., 128-bit one-time commissioning secret written once, thereafter required in a signed header for control writes).
3. Enable encrypted settings at rest (migrate to MCUboot + TF-M / or implement application-level AEAD wrap for stored structs).
4. Rate limit / log failed pairing attempts (future ring buffer characteristic or debug log export).

## 5. Pairing Behavior

Because SMP is enabled and encryption is enforced:

- When a client accesses a characteristic, the peripheral responds with "Insufficient Encryption".
- The client OS should initiate pairing.
- Since no I/O capabilities are defined, "Just Works" pairing is used.
- Bond information is stored (`CONFIG_BT_SETTINGS=y`).
- Subsequent reconnections use the stored bond keys to encrypt the link immediately.

## 6. Client Guidance (Current Firmware)

| Action | Recommendation |
| --- | --- |
| Mobile / Web clients | Connect, then attempt to read a characteristic to trigger pairing |
| Security indicators UI | Show "Pairing..." or "Encrypted" status |
| Sensitive provisioning | Safe to perform over the encrypted link |
| Logging | Data is encrypted over the air |
| Commissioning flows | Perform initial pairing in a trusted environment |

## 7. Summary

Current state = **Encryption Enforced**. The firmware requires bonding/encryption for all operations. This prevents unauthorized access from non-bonded devices and protects data in transit against passive eavesdropping.

---
Revision: Updated to reflect enforced encryption on all characteristics (Jan 2026).

## 8. Troubleshooting

| Observation | Explanation | Action |
| --- | --- | --- |
| "Insufficient Encryption" error | Client tried to access data without pairing | Initiate pairing / bonding |
| Pairing dialog appears | OS detected security requirement | Accept pairing request |
| Central shows link encrypted | Successful pairing | Normal operation |
| Multiple phones try to connect | Second cannot connect | Single connection limit (`CONFIG_BT_MAX_CONN=1`) |
| Bond list full after one pairing | `CONFIG_BT_MAX_PAIRED=1` | Clear settings (factory reset) to pair new device |
