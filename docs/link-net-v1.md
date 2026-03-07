# Link Net v1 Design (Locked Scope)

## Goals
Define a minimal, shippable networked link design that reuses existing mGBA link abstractions and lockstep behavior.

## 1) Supported core/platforms for v1
- **Core:** GBA only.
- **Frontend:** Qt only.
- **Rationale:**
  - The GBA link surface is already explicit through `GBASIODriver` and multiplayer completion callbacks, which is the right integration seam for a remote transport-backed driver.
  - Qt already orchestrates local multiplayer lifecycle, attachment, and per-player threading; this is the most practical place to add room/join UX and network session wiring first.
- **Deferred:** GB core, SDL, libretro, CLI frontends.

## 2) Session size limits
- **Supported player counts:** 2-4 players.
- **Hard cap:** 4, matching `MAX_GBAS` behavior in the existing GBA SIO implementation.
- **Implication for protocol/state:** all per-player arrays and wire payloads should remain fixed-width for 4 slots, with active-player mask for 2-3 player sessions.

## 3) Room model
- **Creation:** one player creates a room; server returns a short room code.
- **Join:** peers join by room code.
- **Identity:** server assigns stable player IDs (`0..N-1`) for the room session.
- **Host role:** host is only the creator/control plane owner (create/start/end room); simulation authority remains deterministic lockstep across all connected players.
- **Reconnect policy (v1):** no mid-session rejoin after simulation start.

## 4) Transport choice
- **Transport for v1:** TCP.
- **Why now:**
  - simpler framing/reliability/ordering semantics,
  - easier implementation/debugging for first ship,
  - aligns with deterministic lockstep expectations where missing ordered inputs stall progress anyway.
- **Deferred transports:** UDP/QUIC.
- **Why deferred:** they require additional complexity (loss recovery, congestion behavior tuning, packet pacing, optional FEC, NAT traversal strategy) before proving baseline correctness.

## 5) Latency and jitter assumptions
- **Target network profile (v1 assumption):** low-to-moderate RTT home internet / LAN quality.
- **Budget assumption:** lockstep exchange cadence should tolerate typical jitter bursts without desync by **stalling** simulation until required remote input arrives.
- **Behavior under jitter:**
  - short jitter burst -> temporary stall/wait,
  - prolonged delay/disconnect -> timeout and session teardown/error state.
- **Desync policy:** no rollback in v1; correctness > smoothness.

## 6) Non-goals for v1
- Matchmaking/discovery service.
- Rollback/prediction netcode.
- Spectator mode.
- Mobile clients.

## Existing link surfaces this design builds on
- `include/mgba/gba/interface.h`: `GBASIODriver` interface is the integration seam for transport-backed multiplayer link behavior.
- `src/gba/sio/lockstep.c`: canonical lockstep timing/synchronization semantics to preserve over network transport.
- `src/platform/qt/MultiplayerController.cpp`: current local multiplayer orchestration and player attachment flow to extend with network room/session flow.
