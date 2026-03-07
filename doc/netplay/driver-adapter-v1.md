# Netplay Driver Adapter Contract v1

## Status

Draft design contract for mapping remote netplay inputs/events onto the `GBASIODriver` callback seam.

## Goal

Define a single deterministic contract for how a remote session adapter drives SIO behavior through `GBASIODriver` without changing core SIO timing behavior.

This contract is anchored to the existing callback surface in `GBASIODriver` and existing local lockstep semantics (`SIO_EV_MODE_SET`, `SIO_EV_TRANSFER_START`, `SIO_EV_HARD_SYNC`).

---

## 1) Callback contract: authoritative mapping

The adapter is an implementation of `GBASIODriver`. It MUST treat callbacks as the only integration seam with the core.

### 1.1 Callback-to-remote behavior table

| GBASIODriver callback | Adapter role in remote netplay | Blocking behavior | Error behavior |
|---|---|---|---|
| `init` | Allocate adapter state, bind queues, reset deterministic counters. | MUST NOT block. | Return `false` on unrecoverable setup failure. |
| `deinit` | Tear down queues/session bindings and free resources. | MUST NOT block. | Best-effort cleanup; no error channel. |
| `reset` | Clear in-flight transfer state, mode, and sequence/tick cursors. | MUST NOT block. | No hard error; transition to idle state. |
| `driverId` | Return stable implementation identifier for save-state compatibility. | MUST NOT block. | N/A. |
| `loadState` | Restore adapter deterministic state (mode, transfer phase, ordering cursors, pending payload slots). | MUST NOT perform I/O. | Return `false` on version/size mismatch. |
| `saveState` | Serialize deterministic adapter state only (no transport handles). | MUST NOT block. | N/A. |
| `setMode` | Publish mode intent as remote control event (`MODE_SET`) and update local expected mode immediately. | MUST NOT block core thread. | If remote publish fails, mark adapter degraded and keep deterministic local mode state. |
| `handlesMode` | Report supported SIO modes for remote path (normally MULTI/NORMAL8/NORMAL32). | MUST NOT block. | Unsupported modes return `false`. |
| `connectedDevices` | Return room-visible peer count mapped to GBA semantics (secondaries only). | MUST NOT block; read cached value. | On stale/missing session data, return last-known good count or `0` when disconnected. |
| `deviceId` | Return server-authoritative local player slot mapped to SIO ID semantics. | MUST NOT block; read cached assignment. | If unassigned/disconnected, return `0` (safe primary fallback semantics at API boundary). |
| `writeSIOCNT` / `writeRCNT` | Optional filtering/logging; no remote side effects required for v1. | MUST NOT block. | Pass-through by default. |
| `start` | Called when transfer begins. Emit deterministic `TRANSFER_START` envelope with transfer mode + finish marker. | May stall only by deterministic barrier policy (see §5). | Return `false` when transfer cannot be legally started (e.g., no peers, invalid role, disconnected). |
| `finishMultiplayer` | At `_sioFinish`, provide 4-word finalized transfer payload for MULTI. | May stall only by deterministic barrier policy; otherwise uses fallback fill. | On missing data at deadline, fill `0xFFFF` words and record late/missing state. |
| `finishNormal8` | At `_sioFinish`, provide finalized 8-bit payload. | Same as above. | On missing data at deadline, return `0xFF`. |
| `finishNormal32` | At `_sioFinish`, provide finalized 32-bit payload. | Same as above. | On missing data at deadline, return `0xFFFFFFFF`. |

## 2) Event mapping model (local lockstep semantics -> remote adapter)

### 2.1 Canonical event mapping table

| Semantic event | Producer | Remote envelope fields (minimum) | Consumer action | Completion criterion |
|---|---|---|---|---|
| Mode change (`SIO_EV_MODE_SET`) | `setMode` callback | `type=MODE_SET`, `senderPlayerId`, `tickMarker`, `mode` | Update peer mode readiness mask; if authoritative player changed mode, apply immediately in adapter state. | All active peers observed same mode generation OR peer removed. |
| Transfer start (`SIO_EV_TRANSFER_START`) | `start` callback (authoritative starter) | `type=TRANSFER_START`, `senderPlayerId`, `tickMarker`, `mode`, `finishTick/finishCycle` | Arm local transfer phase and schedule/align completion window against core timing marker. | Transfer phase becomes `AwaitingCompletionData`. |
| Transfer completion (data delivery at `_sioFinish`) | `finish*` callback pull | Prior received payload event(s) correlated by transfer generation + sender + marker | Materialize finalized payload and hand to core via `finishMultiplayer/finishNormal8/finishNormal32`. | Callback returns data exactly once per transfer generation. |
| Hard sync (`SIO_EV_HARD_SYNC`) | Periodic or post-transfer barrier from authoritative player | `type=HARD_SYNC`, `tickMarker` | Secondary peers ack barrier and stop drift growth; adapter advances stable frontier marker. | Barrier acked by required peers or disconnect policy removes missing peer(s). |

### 2.2 Completion timing rule

`_sioFinish` in core is the only legal completion handoff point. The adapter MUST have deterministic data selection logic ready for that moment. If complete data is unavailable at deadline, adapter applies mode-specific fallback values (all-ones) and records a late/missing condition.

---

## 3) Ownership and lifecycle boundaries

### 3.1 Thread/ownership split

| Boundary | Owns | Must not own | Allowed operations |
|---|---|---|---|
| Session/transport side (Qt/session thread) | Socket/session lifecycle, room membership, inbound/outbound serialization, server sequence handling | Core timing objects, direct SIO register mutation | Push decoded link/control envelopes into lock-free or mutex-protected adapter inbox; update cached topology (`connectedDevices`, `deviceId`). |
| Core timing side (emulation thread invoking `GBASIODriver`) | Deterministic transfer state machine, mode state, callback responses (`start`, `finish*`) | Blocking network I/O, Qt object lifecycle | Consume pre-decoded envelopes, resolve ordering, answer callbacks synchronously and deterministically. |

### 3.2 Lifecycle phases

1. **Detached**: no active room; `connectedDevices=0`, safe defaults.
2. **Bound**: room joined, player ID assigned, transport events flowing.
3. **TransferActive**: after successful `start`, waiting for completion data.
4. **BarrierSync**: hard-sync barrier in progress.
5. **Degraded**: transport alive but late/missing packets observed.
6. **Disconnected/Error**: transport lost/protocol failure; callback behavior switches to deterministic fallback.

Transport transitions are asynchronous; callback-observable state transitions are applied only at deterministic boundaries on the core thread.

---

## 4) Deterministic ordering guarantees

The adapter MUST enforce all of the following:

1. **Single total order per transfer generation**: events are ordered by `(generation, tickMarker, senderPlayerId, sequence)`.
2. **No out-of-band mutation**: session thread cannot mutate state consumed by callbacks except via queued envelopes.
3. **One completion per start**: each accepted `start` yields exactly one `finish*` response path.
4. **Mode-transfer coherence**: mode used for a transfer is the mode snapshot captured at `start` generation.
5. **Save/load reproducibility**: state required to reproduce ordering/fallback choice is serialized by `saveState`/`loadState`.

---

## 5) Late packet / disconnect behavior states

### 5.1 Late-packet policy states

| State | Trigger | `start` behavior | `finish*` behavior | Exit |
|---|---|---|---|---|
| `OnTime` | All required envelopes arrived before completion deadline | Normal acceptance | Return peer data | Remains until miss detected |
| `WaitingWithinBudget` | Missing required data but stall budget not exhausted | Deterministic bounded wait/stall allowed | Keep waiting up to budget | Data arrives -> `OnTime`; budget expires -> `MissedDeadline` |
| `MissedDeadline` | Stall budget exhausted before required data | Future `start` allowed per policy, but transfer marked degraded | Return all-ones fallback for mode; log miss | New clean generation -> `OnTime` or persistent misses -> `DegradedPersistent` |
| `DegradedPersistent` | Repeated misses over threshold | May reject new `start` (`false`) if configured fail-fast | Fallback values continue | Transport recovers or disconnect |

### 5.2 Disconnect/protocol-failure states

| State | Trigger | Callback-facing behavior |
|---|---|---|
| `PeerDetached` | Explicit peer leave or timeout mapped to detach | Recompute cached `connectedDevices`/readiness between transfers only. During an armed transfer generation, topology changes are treated as deterministic protocol failure and transition to degraded state. |
| `SessionDisconnected` | Transport closed/reset | `connectedDevices=0`; `start` returns `false`; `finish*` returns fallback values. |
| `ProtocolError` | Malformed/out-of-order envelope violating contract | Transition to degraded/error mode; reject new `start`; `finish*` uses fallback; require reset/rejoin to recover. |

---

## 6) What blocks/stalls vs what errors

| Operation | Allowed to stall? | Maximum stall policy | On limit exceeded | Error surfaced where |
|---|---|---|---|---|
| `setMode` | No | 0 | Cache locally; publish async best-effort | Adapter diagnostics only |
| `connectedDevices` / `deviceId` | No | 0 | Return cached/fallback value | No direct error return |
| `start` | Yes, bounded by deterministic transfer-start barrier budget | Configurable fixed cycle/tick budget | Return `false` if start preconditions unmet or barrier failure policy triggers | `start` return value + diagnostics |
| `finishMultiplayer` / `finishNormal8` / `finishNormal32` | Yes, bounded by deterministic completion budget | Configurable fixed cycle/tick budget | Return mode-specific all-ones fallback | Data return value + diagnostics |
| `saveState` / `loadState` | No transport wait; pure memory work only | 0 network stall | `loadState` returns `false` on incompatible state | `loadState` return value |

---

## 7) Contract checklist (acceptance)

- Every `GBASIODriver` callback has explicit remote behavior mapping in §1.1.
- Event mapping for mode change, transfer start, transfer completion-at-finish, and hard sync is defined in §2.1.
- Thread/lifecycle ownership boundaries are explicit in §3.
- Deterministic ordering guarantees are explicit in §4.
- Late-packet/disconnect states are explicit in §5.
- Explicit block/stall vs error matrix is defined in §6.

---

## 8) Savestate compatibility policy (v1)

`GBASIODriver::saveState/loadState` blobs for the network adapter MUST include both a `driverId` and `version` header before driver fields.

### v1 connected-session rule

To preserve deterministic ordering guarantees, v1 explicitly forbids loading a netplay savestate while either:

- the current adapter instance is already connected (`InRoom` or later), or
- the serialized blob reports a connected/degraded net state.

When this rule is hit, `loadState` MUST reject with `false` and emit a clear warning ("Refusing to load net savestate while connected (v1 policy)").
