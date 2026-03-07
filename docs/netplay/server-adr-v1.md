# ADR: Netplay Server Stack for v1

- Status: Accepted
- Date: 2026-03-07
- Decision owners: netplay maintainers
- Related: `docs/netplay/protocol-v1.md`

## Context

Netplay protocol v1 is already constrained to a TCP, length-prefixed, JSON message stream with strict sequencing and disconnect/error mapping rules. The server implementation must satisfy the protocol as written and be practical for self-hosted deployments on a single machine.

For v1, we need an explicit implementation stack decision so contributors can align implementation, ops docs, and testing around one target.

## Candidate options

### 1) C++ in-tree executable

- Pros:
  - Maximum code sharing opportunities with in-repo C++ types and conventions.
  - No extra runtime dependency for operators beyond a compiled binary.
- Cons:
  - Higher implementation complexity for concurrent networking and structured service ergonomics (config layering, logs, lifecycle hooks) unless additional libraries are adopted.
  - Slower iteration for service-only contributors unfamiliar with the emulator codebase.

### 2) Go service

- Pros:
  - Strong standard library for TCP servers and JSON handling.
  - Simple static binary distribution and straightforward cross-compilation.
  - Concurrency model (goroutines/channels) is well suited for room/event fan-out.
- Cons:
  - Adds a second language/toolchain outside current core emulator implementation language.
  - Requires deliberate guardrails to keep per-room event ordering deterministic under concurrency.

### 3) Rust service

- Pros:
  - Strong correctness and type-safety story for protocol/state machine implementation.
  - Good async networking ecosystem and structured logging support.
  - Produces single-binary deploy artifacts.
- Cons:
  - Steeper onboarding for contributors not already using Rust.
  - Higher perceived implementation overhead for rapid v1 delivery.

### 4) Node service

- Pros:
  - Fastest initial prototyping loop.
  - Large ecosystem for transport and observability tooling.
- Cons:
  - Runtime dependency on Node.js for operators.
  - Greater risk of accidental event-loop blocking under load.
  - Operational variance across Node versions compared with static binaries.

## Decision criteria (repo-specific)

The v1 stack must satisfy all of the following:

1. **Protocol fidelity**
   - Must match `docs/netplay/protocol-v1.md` exactly (TCP framing, message shapes, versioning, sequencing semantics, and disconnect/error behavior).
2. **Self-host simplicity**
   - Must be easy to run on one machine by an operator with minimal setup.
3. **Deterministic ordering + robust logs**
   - Must support deterministic per-room ordering of forwarded link events.
   - Must provide structured, parseable logs sufficient for ordering/desync and auth failure diagnostics.

## Final choice

**Chosen stack for v1: Go service (single standalone relay executable).**

### Rationale

- Go balances implementation speed and operational simplicity for this repository's immediate v1 goals.
- A static single binary aligns with the self-host-on-one-machine requirement without forcing operators to manage a language runtime.
- Go's standard networking primitives are sufficient to implement protocol-v1 framing/validation while keeping dependencies modest.
- Deterministic per-room ordering is achieved by constraining each room to a single serialized event-processing loop (one authoritative ordering point per room) even if connection I/O is concurrent.
- Structured logging can be standardized as JSON lines with required fields (`direction`, `kind`, `roomId`, `playerId`, `sequence`, `serverSequence`, `state`, `code`, `reason`) to align with protocol-v1 diagnostics guidance.

## Process model (v1)

### Runtime shape

- **Single binary** process that handles accept loop, session lifecycle, room management, and event fan-out.

### Configuration

- Support **config file and environment variables**.
- Environment variables override config-file defaults.

### Minimum operator settings

- `host` and `port` bind values (for example `0.0.0.0:8765`).
- Shared **auth secret** (or token verification key material) for deployments requiring admission control.
- Room capacity and global room limits:
  - max players per room
  - max concurrent rooms

### Ordering model

- Each room owns a single serialized queue/loop for inbound intents that can affect room-visible ordering.
- Broadcast to peers occurs from that ordering point so all peers observe the same per-room order.
- Backpressure and disconnect handling must not reorder accepted events.

## Compatibility note

- The canonical server event name is **`inboundLinkEvent`**.
- A legacy **`linkEvent`** alias may be accepted by decoders only for compatibility with older payloads.
- New server emissions and docs must use `inboundLinkEvent` as the source-of-truth event name.

## Consequences

- Implementation and deployment docs should assume a Go-based relay for v1 unless superseded by a future ADR.
- Future protocol revisions can revisit stack choice if scale, ecosystem alignment, or maintainership changes materially.
