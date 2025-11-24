# Fragmentation Reference (BLE Characteristics)

Central, implementation-verified summary of custom fragmentation schemes used by the AutoWatering BLE service.

## Principles
- All characteristic base struct sizes are chosen to normally fit common MTUs (>= 23).
- Fragmentation only applied when a payload would exceed `(ATT_MTU - 3)` where 3 = GATT notification header.
- Each characteristic defines its own header format; there is no global multiplex framing.

## Header Types
| Header | Size | Used By | Fields | Purpose |
|--------|------|---------|--------|---------|
| `[seq,total,len]` | 3 B | Environmental Data (notify only) | `uint8_t seq`, `uint8_t total`, `uint8_t len` | Simple contiguous reassembly of fixed total 28B snapshot when MTU too small |
| `history_fragment_header_t` | 8 B | Irrigation History (12) / Rain / Environmental History / Auto Calc (notify) | (see individual docs) | Common 8B header preceding multi-entry payload fragments |
| Long-write accumulation (no header) | - | System Configuration | Uses GATT long write (prepare/execute) to assemble full 67B enhanced config before apply |
| 4B write fragment header | 4 B | Large write commands (e.g. channel/task extended forms) | type/flags/offset/len (see respective docs) | Batches partial writes prior to commit |

## Environmental Data Specifics
- 28B struct fits single notification for MTU >= 31 (since 31-3=28).
- For MTU < 31: payload split into fragments of size `min( (ATT_MTU-3)-3 , internal_chunk_limit )` with 3B header each.
- Receiver reassembles in sequence order; `total` is number of fragments, `len` is bytes in this fragment.
- No checksum; integrity relies on BLE reliability + final size (must sum to 28).

## History Transfer
- History Management (12) builds a contiguous payload: 12B echoed query header followed by fixed-size entry records (types: detailed 24B, daily 16B, monthly 15B, annual 14B) then fragments into <=240B slices each preceded by the 8B header.
- Rain & Environmental History use similar unified header framing (their own entry packing rules). See respective characteristic docs for entry semantics.

## Non-Fragmented Notifications
Most other characteristics (valve control, statistics, reset control, compensation status, onboarding status after internal aggregation) fit in a single notification frame under typical MTUs.

## Reserved / Future
- No compression currently applied.
- Potential future optimization: reuse the 3B header pattern for any snapshot <= 255B if generic path desired (NOT implemented).

_Last audit sync: August 13, 2025._
