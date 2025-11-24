# BLE Glossary (AutoWatering)

Canonical terminology used across the BLE documentation to avoid ambiguity.

## Fragmentation & Transfer
- **4B Write Fragment Header**: First write fragment header for large struct writes. Layout: `[channel_id] [frag_type] [size_lo] [size_hi]`. `frag_type` 1=name-only (Channel Config only), 2=big-endian size, 3=little-endian size. Only the first fragment carries the header; continuations are raw payload bytes.
- **Unified 8B Header**: Standard notification / multi-entry response header (`history_fragment_header_t`) used for: History Management (12), Rain History (20), Environmental History (22), Auto Calc Status notifications (15). Never used on read path for simple fixed-size characteristics.
- **Long Write (Offset Accumulation)**: GATT offset-based accumulation used by Enhanced System Configuration (no custom 4B header).
- **Single-PDU Write**: A write that fits entirely within negotiated MTU (after ATT protocol overhead); no custom header or offset semantics.

## Characteristic Operations
- **Channel Select Write**: 1-byte write whose sole purpose is to change the active channel context for a subsequent read/notify (e.g., Auto Calc Status, Growing Environment channel select).
- **Snapshot**: A complete packed struct reflecting internal state at the instant of read/notify generation.

## Status / Priority
- **Priority (Notification)**: Internal categorization (Critical, High, Normal, Low) defining minimum spacing between notifications; adaptive logic may adjust within documented bounds.

## Data Integrity & Evolution
- **BUILD_ASSERT**: Compile-time assertion tying documented struct size to actual `sizeof`.
- **Packed Struct**: Structure declared with packing attribute ensuring no padding between fields (little-endian multi-byte fields).

## Common Fields
- **Timestamp (Unix)**: Unsigned 32-bit seconds since Unix epoch (UTC).
- **Percentage Fields**: Unsigned integer 0-100 inclusive unless specified otherwise.

## Error Signaling
- **ATT Error Codes**: Standard Bluetooth ATT errors (`BT_ATT_ERR_VALUE_NOT_ALLOWED`, `BT_ATT_ERR_INVALID_OFFSET`, etc.) for validation or misuse; richer domain errors map into Unified 8B header `status` when applicable.

## Conventions
- **Size Column (Tables)**: Packed struct size for a single snapshot (excludes headers, aggregated multi-entry payloads).
- **Var / Var (<=N)**: Variable-length or bounded-maximum payload.

---
Maintainers: Update this glossary when introducing new header types, fragmentation strategies, or notification priority classes.

## Internal Enhanced Configuration Management Messages
Auxiliary packed structs used only over internal management characteristics (not exposed as standalone public user-facing characteristics) to coordinate enhanced configuration life-cycle operations (reset sequencing, staged apply status, custom soil profile persistence). They:
- Are defined in code (bt_gatt_structs_enhanced.h) and guarded by BUILD_ASSERT size checks.
- Fit in a single ATT PDU (no custom 4B write header, no Unified 8B header).
- May contain reserved / placeholder fields which MUST be zeroed by senders and ignored by receivers until promoted.
- Are excluded from the stable external protocol versioning surface; changes can occur with minor firmware revisions provided field zero/ignore rules are respected.

Document only if/when promoted to a public characteristic or referenced explicitly in the protocol spec.
