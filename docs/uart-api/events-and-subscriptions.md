# UCP UART v1 Events and Subscriptions

## Model

UCP uses:

- `request/response` for peer-initiated reads/writes/commands
- `async push` for nRF-originated state changes (`EVENT_CHANGE`)

Peer subscribes with `SUB_REQ` and may stop delivery with `UNSUB_REQ`.

## `EVENT_CHANGE` (delta)

Each event includes (TLV payload):

- `boot_id`
- `event_seq`
- `topic_id`
- `service_id`
- `resource_id`
- `instance_id`
- `resource_version`
- `origin`
- `timestamp_utc_ms`
- `delta_tlv`

Default v1 behavior: **delta only** (not full snapshot).

## Topic IDs (dense, bitset-friendly)

- `1` `system.status`
- `2` `config.changed`
- `3` `rtc_tz.changed`
- `4` `task.current`
- `5` `alarm`
- `6` `ota.transition`
- `7` `peer.power`
- `8` `diagnostics`

## Subscription Semantics

- `SUB_REQ` carries a topic bitmask (`topic_id` 1..32 mapped to bits 0..31)
- `UNSUB_REQ` may clear a subset or all topics
- `EVENT_HEARTBEAT` is sent periodically while subscriptions are active and no events were emitted recently
- current firmware may include optional coalescing counters in `EVENT_HEARTBEAT` payload (helps distinguish link loss from intentional rate-limited live push)

## Current Implementation Status

Implemented:

- selective topic bitmask subscribe/unsubscribe
- `EVENT_CHANGE` for UCP `SET/CMD` changes across implemented resources
- `EVENT_CHANGE` for RTC/timezone changes applied from BLE paths (`origin=ble`)
- `EVENT_CHANGE` for channel/schedule/task/system/flow/rain/environment/packs changes (poll-based detection where direct hooks are not available)
- direct alarm and valve-path UCP publish hooks (plus scheduler alarm/system-status hooks)
- `EVENT_HEARTBEAT`
- replay buffer (`64` events, in-RAM, per-boot)
- `SUB_REQ` `replay_then_live` via `sub_mode` + `since_event_seq`
- `EVENT_OVERFLOW` with `first_available_event_seq`, `last_event_seq`, `dropped_count`, `resync_required`
- live push rate-limit/coalescing (topic-based min intervals, replay remains intact for resync)

Still pending:

- additional direct hooks for domains that currently rely mainly on polling (to improve origin attribution granularity)

See also:

- `docs/uart-api/read-set-notify-checklist.md` for per-service `GET/SET/CMD/NOTIFY/STREAM` coverage.
- `docs/uart-api/wire-reference.md` for exact `EVENT_CHANGE` / `EVENT_OVERFLOW` / `EVENT_HEARTBEAT` payload bytes.
- `docs/uart-api/api-reference.md` for topic-to-resource payload families and notifier behavior per service.
- `docs/uart-api/examples-and-traces.md` for replay/resync and overflow recovery traces.
- `docs/uart-api/client-implementation-guide.md` for client-side event apply/resync strategy.
