# Link Net v1 Validation Results

This document records scenario-by-scenario validation executions derived from `docs/netplay/validation-v1.md`.

- **Results version**: v1
- **Emulator commit (this repo)**: `b901aed03ba21ff77650fdf4ec9b8e04c4d425a7`
- **Relay commit**: `UNKNOWN` (relay source/commit SHA was not available in this repository at the time of writing)
- **Result status legend**:
  - `COMPLETE`: run executed with attached logs
  - `PENDING`: run slot created; waiting for execution artifacts

## Scenario 1 — LAN low latency

| Run ID | Status | Topology / Network profile | Players | Stall / Desync / Recovery metrics | Known limitations observed | Logs |
|---|---|---|---|---|---|---|
| S1-R1 | PENDING | Same-LAN clients, expected RTT <10 ms | 2P | Stall count: N/A, stalls/min: N/A, desyncs: N/A, recovery: N/A | N/A (run not executed) | Client logs: `docs/netplay/logs/S1-R1/client-*.log`<br>Relay logs: `docs/netplay/logs/S1-R1/relay.log` |
| S1-R2 | PENDING | Same-LAN clients, expected RTT <10 ms | 4P | Stall count: N/A, stalls/min: N/A, desyncs: N/A, recovery: N/A | N/A (run not executed) | Client logs: `docs/netplay/logs/S1-R2/client-*.log`<br>Relay logs: `docs/netplay/logs/S1-R2/relay.log` |

## Scenario 2 — WAN moderate latency

| Run ID | Status | Topology / Network profile | Players | Stall / Desync / Recovery metrics | Known limitations observed | Logs |
|---|---|---|---|---|---|---|
| S2-R1 | PENDING | WAN, relay region fixed (target RTT 40-120 ms) | 2P | Stall count: N/A, stall duration p95: N/A, desyncs: N/A, recovery: N/A | N/A (run not executed) | Client logs: `docs/netplay/logs/S2-R1/client-*.log`<br>Relay logs: `docs/netplay/logs/S2-R1/relay.log` |
| S2-R2 | PENDING | WAN, relay region fixed (target RTT 40-120 ms) | 4P | Stall count: N/A, stall duration p95: N/A, desyncs: N/A, recovery: N/A | N/A (run not executed) | Client logs: `docs/netplay/logs/S2-R2/client-*.log`<br>Relay logs: `docs/netplay/logs/S2-R2/relay.log` |

## Scenario 3 — Induced delay and jitter

| Run ID | Status | Topology / Network profile | Players | Stall / Desync / Recovery metrics | Known limitations observed | Logs |
|---|---|---|---|---|---|---|
| S3-R1 | PENDING | 2P with impairment profile A: `+80ms delay` | 2P | Stall count: N/A, playability threshold: N/A, desyncs: N/A, recovery: N/A | N/A (run not executed) | Client logs: `docs/netplay/logs/S3-R1/client-*.log`<br>Relay logs: `docs/netplay/logs/S3-R1/relay.log` |
| S3-R2 | PENDING | 4P with impairment profile B: `+150ms delay, 30ms jitter, 1-2% loss` | 4P | Stall count: N/A, playability threshold: N/A, desyncs: N/A, recovery: N/A | N/A (run not executed) | Client logs: `docs/netplay/logs/S3-R2/client-*.log`<br>Relay logs: `docs/netplay/logs/S3-R2/relay.log` |

## Scenario 4 — 2-player and 4-player coverage comparison

| Run ID | Status | Topology / Network profile | Players | Stall / Desync / Recovery metrics | Known limitations observed | Logs |
|---|---|---|---|---|---|---|
| S4-R1 | PENDING | Baseline profile (same flow as Scenario 1/2) | 2P | Stalls/min: N/A, p95 stall duration: N/A, desyncs: N/A, teardown behavior: N/A | N/A (run not executed) | Client logs: `docs/netplay/logs/S4-R1/client-*.log`<br>Relay logs: `docs/netplay/logs/S4-R1/relay.log` |
| S4-R2 | PENDING | Baseline profile (same flow as Scenario 1/2) | 4P | Stalls/min: N/A, p95 stall duration: N/A, desyncs: N/A, teardown behavior: N/A | N/A (run not executed) | Client logs: `docs/netplay/logs/S4-R2/client-*.log`<br>Relay logs: `docs/netplay/logs/S4-R2/relay.log` |

## Scenario 5 — Abrupt disconnect and reconnect attempts

| Run ID | Status | Topology / Network profile | Players | Stall / Desync / Recovery metrics | Known limitations observed | Logs |
|---|---|---|---|---|---|---|
| S5-R1 | PENDING | Host disconnect mid-session | 2P | Immediate stall: N/A, desyncs: N/A, session termination: N/A, reconnect rejection: N/A | N/A (run not executed) | Client logs: `docs/netplay/logs/S5-R1/client-*.log`<br>Relay logs: `docs/netplay/logs/S5-R1/relay.log` |
| S5-R2 | PENDING | Non-host disconnect mid-session | 4P | Immediate stall: N/A, desyncs: N/A, session termination: N/A, reconnect rejection: N/A | N/A (run not executed) | Client logs: `docs/netplay/logs/S5-R2/client-*.log`<br>Relay logs: `docs/netplay/logs/S5-R2/relay.log` |

## Scenario 6 — Savestate behavior while connected

| Run ID | Status | Topology / Network profile | Players | Stall / Desync / Recovery metrics | Known limitations observed | Logs |
|---|---|---|---|---|---|---|
| S6-R1 | PENDING | Attempt save/load while in active netplay | 2P | Savestate action blocked: N/A, induced stalls: N/A, desyncs: N/A | N/A (run not executed) | Client logs: `docs/netplay/logs/S6-R1/client-*.log`<br>Relay logs: `docs/netplay/logs/S6-R1/relay.log` |
| S6-R2 | PENDING | Attempt save/load while in active netplay | 4P | Savestate action blocked: N/A, induced stalls: N/A, desyncs: N/A | N/A (run not executed) | Client logs: `docs/netplay/logs/S6-R2/client-*.log`<br>Relay logs: `docs/netplay/logs/S6-R2/relay.log` |

## Cross-scenario summary

- **2P coverage slots**: S1-R1, S2-R1, S3-R1, S4-R1, S5-R1, S6-R1
- **4P coverage slots**: S1-R2, S2-R2, S3-R2, S4-R2, S5-R2, S6-R2
- **Observed known limitations**: No executed runs yet; limitations cannot be confirmed from artifacts.

## Artifact checklist (per run)

For each run ID above, attach:

1. Relay log (`relay.log`) with timestamp and server sequence markers.
2. Per-client logs (`client-<player>.log`) with link event and disconnect diagnostics.
3. Short notes file (`notes.md`) capturing ROM/mode, room ID, start/end time, and pass/fail call.

Suggested directory layout:

```text
docs/netplay/logs/<RUN_ID>/
  relay.log
  client-host.log
  client-p2.log
  client-p3.log        # optional for 4P
  client-p4.log        # optional for 4P
  notes.md
```
