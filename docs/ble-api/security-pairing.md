# BLE Security & Pairing (Current Minimal Posture)

Status: VERIFIED (doc aligned with firmware - Feb 2025)

This document states the *actual* security posture of the current firmware. Earlier drafts claimed authenticated pairing flows (numeric comparison, passkey) and role-based authorization. Those are **not implemented** today. The device operates in a minimal security mode intended for development / physically controlled environments.

## 1. What Is Implemented Now

| Aspect | Current Behavior | Source / Config |
|--------|------------------|-----------------|
| Bluetooth mode | Peripheral, single connection | `CONFIG_BT_PERIPHERAL=y`, `CONFIG_BT_MAX_CONN=1` |
| SMP / bonding capability | Enabled at Kconfig level but not enforced | `CONFIG_BT_SMP=y`, `CONFIG_BT_MAX_PAIRED=1` |
| Required security level for characteristics | None (all GATT attributes accessible without encryption/auth) | Service implementation (`bt_irrigation_service.c`) |
| Encryption on link | Opportunistic only if central initiates pairing | Handled by Zephyr stack |
| Authentication (MITM / LESC) | Not enforced | No passkey / numeric comparison callbacks coded |
| Authorization / roles | Not present | No application role table |
| At-rest data encryption | Not enabled (plain NVS) | NVS config in `prj.conf` |
| Single bonded device limit | Configured, but unused if no pairing occurs | `CONFIG_BT_MAX_PAIRED=1` |

Effectively: the firmware accepts any connection and honors characteristic reads/writes following only syntactic / range validation inside each write handler. Security relies on *physical proximity* and *short BLE range* only.

## 2. What Earlier Drafts Claimed (Removed)

The following items previously documented are NOT present and should not be assumed by client implementations:
- Numeric comparison pairing flow / passkey entry
- Mandatory pairing before configuration writes
- Enforced authenticated or encrypted reads
- Role-based command authorization (admin vs user)
- Encrypted NVS storage for configuration/history
- Replay protection tokens / rolling nonces
- Security validation dashboard / event audit trail

## 3. Practical Implications & Risks

| Risk | Impact | Current Mitigation | Residual Exposure |
|------|--------|--------------------|-------------------|
| Unauthorized nearby control | Configuration / valve actuation | Physical access assumption; short range | Any BLE central in range can modify settings |
| Passive eavesdropping | Leak of schedules/history | None (no mandatory encryption) | Cleartext ATT traffic if not paired |
| MITM during connection | Command injection | None | Full |
| Replay of writes | Duplicate commands | Some handlers validate semantic ranges only | Potential replay accepted |

If deploying beyond a controlled lab / prototype setting, treat the system as **insecure** until enhancements below are implemented.

## 4. Recommended Near-Term Hardening Roadmap

1. Enforce minimum security level (encryption + MITM) by refusing unencrypted writes (check `bt_conn_get_security` in each write handler).
2. Add passkey or numeric comparison callbacks (Zephyr: implement `bt_conn_auth_cb` + `bt_conn_auth_info_cb`).
3. Introduce a lightweight application token (e.g., 128-bit one-time commissioning secret written once, thereafter required in a signed header for control writes).
4. Enable encrypted settings at rest (migrate to MCUboot + TF-M / or implement application-level AEAD wrap for stored structs).
5. Rate limit / log failed pairing attempts (future ring buffer characteristic or debug log export).

## 5. Minimal Pairing Behavior (Optional Today)

Because SMP is enabled, a central *may* initiate pairing (Just Works). If it does:
- Bond information is stored (`CONFIG_BT_SETTINGS=y`).
- Subsequent reconnect is faster but still not required for access.
- No additional privileges are granted; absence of pairing does not block operations.

## 6. Adding Enforcement (Example Sketch)

Below is a concise illustration (not yet in firmware) of how enforcement could look. DO NOT rely on this existing in current code.

```c
/* Pseudocode to reject unencrypted writes */
static ssize_t secure_write(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                            const void *buf, uint16_t len, uint16_t offset, uint8_t flags) {
    if (!conn) return BT_GATT_ERR(BT_ATT_ERR_INVALID_HANDLE);
    enum bt_security_t sec = bt_conn_get_security(conn);
    if (sec < BT_SECURITY_L2) { // Require encryption
        return BT_GATT_ERR(BT_ATT_ERR_INSUFFICIENT_ENCRYPTION);
    }
    /* Proceed with existing validation */
}
```

To support passkey / numeric comparison:

```c
/* Registration during init */
static void auth_passkey_display(struct bt_conn *conn, unsigned int passkey) {
    printk("Passkey: %06u\n", passkey);
}
static void auth_cancel(struct bt_conn *conn) { printk("Auth canceled\n"); }
static void pairing_complete(struct bt_conn *conn, bool bonded) { printk("Pairing done\n"); }
static void pairing_failed(struct bt_conn *conn, enum bt_security_err reason) { printk("Pairing failed %d\n", reason); }

static struct bt_conn_auth_cb auth_cb = {
    .passkey_display = auth_passkey_display,
    .cancel = auth_cancel,
};
static struct bt_conn_auth_info_cb auth_info_cb = {
    .pairing_complete = pairing_complete,
    .pairing_failed = pairing_failed,
};

// During init:
bt_conn_auth_cb_register(&auth_cb);
bt_conn_auth_info_cb_register(&auth_info_cb);
```

## 7. Client Guidance (Current Firmware)

| Action | Recommendation |
|--------|----------------|
| Mobile / Web clients | Connect directly; do **not** expect pairing prompt |
| Security indicators UI | Show explicit "UNAUTHENTICATED" banner if link not encrypted |
| Sensitive provisioning | Defer until security hardening implemented |
| Logging | Treat all data as public; avoid storing secrets via BLE |
| Commissioning flows | Keep device in physically restricted area until hardened |

## 8. Future Documentation Hooks

When hardening lands, extend this file with:
- Minimum required security matrix per characteristic
- Commissioning / re-key procedure
- Bond reset characteristic or op-code
- Threat model deltas after each milestone

## 9. Summary

Current state = convenience over security. The firmware enables SMP but does **not** *require* any security level. All previous detailed pairing flows and multi-language security manager examples were removed to prevent misleading integrators.

---
Historical verbose JavaScript/Python pairing manager examples were intentionally deleted here. Refer to commit history if retrieval is ever needed for future adaptation.

Revision: pared down & verified against `prj.conf` and service implementation (26 characteristics). Next step in overall doc audit: connection management alignment (see `security-connection-management.md`).

<!-- End of file -->

## (Appendix - Removed Section Marker)

The original long-form examples & high-security narratives ended here in prior versions.

```text
REMOVED_CONTENT: web pairing UI overlays, Bleak security validator, role-based access scaffolding.
```
## 10. Troubleshooting (Given Current Minimal Security)

| Observation | Explanation | Action |
|-------------|------------|--------|
| No pairing dialog appears | Firmware does not request security | Accept (expected) |
| Central shows link unencrypted | Not paired / not requested | OK in prototype; harden per roadmap |
| Attempt to force encryption succeeds but nothing changes functionally | Handlers do not check security state | Implement enforcement if required |
| Multiple phones try to connect | Second cannot connect | Single connection limit (`CONFIG_BT_MAX_CONN=1`) |
| Bond list full after one pairing | `CONFIG_BT_MAX_PAIRED=1` | Clear settings (future: add reset op) |

---
End of current minimal security statement.