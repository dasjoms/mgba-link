# Link Net v1 Validation Plan

This document defines reproducible validation scenarios for Link Net v1 behavior across local and internet-like conditions. It is intended to be run against the Qt frontend + GBA-only v1 scope and paired with relay/server logs for incident triage.

## Test matrix (v1)

Use the following template fields for every run:

- **Build/commit**: emulator and relay commit SHA.
- **Topology**: direct LAN/WAN, relay host region, NAT type (if known).
- **Players**: 2P or 4P room.
- **Game ROM + mode**: deterministic multiplayer mode used.
- **Run length**: target minimum 10-15 minutes per run.
- **Artifacts**: relay logs, client logs, and timestamped notes.

### Scenario 1: LAN low latency

- **Setup**: all clients on same LAN, relay reachable with RTT typically < 10 ms.
- **Reproducibility**: high; repeat at least 5 runs per player-count variant.
- **Stall frequency**: expected rare to none; record total stalls and stalls/min.
- **Desync incidence**: expected none; any desync is a v1 blocker.
- **Recovery outcome**: temporary stalls should self-clear; hard disconnect should terminate session (no v1 mid-session rejoin).

### Scenario 2: WAN moderate latency

- **Setup**: geographically separated clients, expected RTT ~40-120 ms.
- **Reproducibility**: medium-high; repeat at least 5 runs using consistent relay region.
- **Stall frequency**: expected occasional lockstep waits; record count, duration, and worst observed burst.
- **Desync incidence**: expected none; capture full logs if observed.
- **Recovery outcome**: jitter-induced stalls may recover; prolonged delays can timeout and tear down session.

### Scenario 3: Induced delay and jitter

- **Setup**: inject delay/jitter/loss on one or more clients (for example using `tc netem`).
- **Reproducibility**: medium; run with fixed profiles (e.g., +80 ms, +150 ms jitter 30 ms, 1-2% loss) and note exact command.
- **Stall frequency**: expected to increase with induced impairment; record threshold where playability becomes unacceptable.
- **Desync incidence**: should remain none under lockstep; investigate immediately if it occurs.
- **Recovery outcome**: short impairment bursts may recover; sustained impairment should produce timeout/disconnect rather than desync.

### Scenario 4: 2-player and 4-player rooms

- **Setup**: execute equivalent game flow in both 2P and 4P rooms.
- **Reproducibility**: high for 2P, medium for 4P due to coordination overhead; run at least 5 sessions each.
- **Stall frequency**: compare 2P vs 4P stalls/min and 95th-percentile stall duration.
- **Desync incidence**: expected none in both configurations.
- **Recovery outcome**: room teardown/disconnect semantics should be consistent across both sizes.

### Scenario 5: Abrupt disconnect and reconnect attempts

- **Setup**: terminate one client process/network path mid-session; attempt reconnect with same user/slot.
- **Reproducibility**: high; test host and non-host disconnect cases.
- **Stall frequency**: immediate stall expected when a required peer vanishes.
- **Desync incidence**: expected none; session should end/abort cleanly.
- **Recovery outcome**: **v1 policy is no mid-session rejoin**. Reconnect attempts after simulation start should be rejected and users should be directed to create a new room/session.

### Scenario 6: Savestate behavior while connected

- **Setup**: attempt save/load state during active netplay session from each client role.
- **Reproducibility**: high; run in both 2P and 4P rooms.
- **Stall frequency**: if savestate is disabled, no savestate-induced stalls should occur.
- **Desync incidence**: savestate load in deterministic lockstep is desync-prone and therefore must be blocked/disabled in v1.
- **Recovery outcome**: **v1 behavior should be explicit disablement** of incompatible savestate actions while connected, with user-facing messaging.

## Known limitations (v1)

- Deterministic lockstep prioritizes correctness over smoothness; stalls are expected under jitter/latency spikes.
- No rollback/prediction in v1; responsiveness degrades as worst-peer latency rises.
- No mid-session rejoin after simulation start.
- Savestate load during connected play is unsupported and should be disabled.
- TCP transport means ordered delivery; packet delay on one stream can stall simulation progress.

## Recommended network conditions for users

- Prefer wired Ethernet or stable Wi‑Fi with minimal contention.
- Keep typical RTT in low-to-moderate ranges (ideally < 80 ms for best experience).
- Avoid high jitter links (target jitter < 20-30 ms where possible).
- Close background bandwidth-heavy applications on all peers.
- Select a relay region geographically central to all players.

## Reporting format

When filing a validation result, include:

1. Scenario name and player count.
2. Network profile (baseline or induced settings).
3. Reproducibility summary (runs attempted / runs reproduced).
4. Stall frequency metrics.
5. Desync incidence.
6. Recovery outcome and user-visible behavior.
7. Log/artifact links.
