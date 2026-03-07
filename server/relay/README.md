# mGBA Link Relay (experimental)

A lightweight TCP relay service for link-session traffic.

## Layout

- `main.go`: service entrypoint.
- `config/`: CLI and environment-driven configuration loading.
- `logging/`: logger construction.
- `protocol/`: framed transport and protocol validation primitives.
- `rooms/`: room/session management.
- `transport/`: TCP listener and connection handlers.

## Run

This relay is intentionally isolated from the root CMake build.

```bash
cd server/relay
go run . --bind 0.0.0.0 --port 41000 --secret my-secret
```

The Qt netplay client defaults to relay port `41000` on first run, matching this relay default.

Build a standalone binary:

```bash
cd server/relay
go build -o relay .
./relay --help
```

## Configuration knobs

All options can be provided either by CLI flag or environment variable.

| Flag | Env | Default | Purpose |
|---|---|---|---|
| `--bind` | `RELAY_BIND` | `0.0.0.0` | Listener bind interface/IP |
| `--port` | `RELAY_PORT` | `41000` | Listener port |
| `--secret` | `RELAY_SECRET` | empty | Optional shared join secret |
| `--max-rooms` | `RELAY_MAX_ROOMS` | `1024` | Maximum active rooms |
| `--max-players-per-room` | `RELAY_MAX_PLAYERS_PER_ROOM` | `4` | Per-room player cap |
| `--heartbeat-interval` | `RELAY_HEARTBEAT_INTERVAL` | `5s` | Ping cadence |
| `--heartbeat-timeout` | `RELAY_HEARTBEAT_TIMEOUT` | `20s` | Disconnect threshold without pong |
| `--write-timeout` | `RELAY_WRITE_TIMEOUT` | `5s` | Per-write timeout |
| `--read-buffer` | `RELAY_READ_BUFFER` | `65536` | Read buffer size in bytes |

## Protocol sketch

Transport uses 4-byte big-endian length-prefixed framing, followed by UTF-8 JSON payload bytes.

- Client -> server uses top-level discriminator `intent` (`hello`, `createRoom`, `joinRoom`, `leaveRoom`, `heartbeat`, `publishLinkEvent`).
- Server -> client uses top-level discriminator `kind` (`playerAssigned`, `roomJoined`, `inboundLinkEvent`, `heartbeatAck`, `error`, `disconnected`).
- `roomId` is authoritative from relay responses (`playerAssigned`/`roomJoined`) and must be reused by clients for future join/leave/publish intents.
- `createRoom` semantics:
  - if `roomName` is omitted/empty, relay generates a short canonical room code,
  - if `roomName` is provided, relay validates and canonicalizes it to uppercase `[A-Z0-9_-]`.
- `playerId` semantics:
  - `0` means not yet joined to a room,
  - active room participants are assigned stable IDs in range `1..4` for the lifetime of their room session,
  - `publishLinkEvent.event.senderPlayerId` must match the assigned `playerId` (or be `0` for legacy unset clients).
- Hard protocol violations emit `error` and then `disconnected` before socket close.



## Netplay validation scenarios

Cross-environment validation scenarios (LAN/WAN/jitter, room-size coverage, disconnect handling, and savestate v1 behavior) are documented in:

- `docs/netplay/validation-v1.md`

For relay-focused runs, attach relay logs plus the scenario reporting fields from that document.

## Automated relay E2E coverage

The relay includes TCP harness tests that boot a real listener and connect synthetic clients to validate:

- handshake success and auth failure rejection,
- room create/join with deterministic player assignment,
- ordered `inboundLinkEvent` rebroadcast with monotonic `serverSequence`,
- rejection paths (`room is full`, bad publish sequence, invalid payload),
- heartbeat timeout cleanup with peer notification, and
- legacy fixture compatibility policy for incoming `linkEvent` style payloads.

Run the relay E2E suite:

```bash
cd server/relay
go test ./transport -run TestRelayE2E -count=1
```

### Legacy `linkEvent` compatibility policy

Current policy is **strict rejection** for legacy incoming payloads that use `kind:"linkEvent"` instead of `intent`. Those payloads are treated as protocol violations (`400` + `disconnected` with `protocolError`) and are not normalized into `publishLinkEvent`.

Fixture used by tests: `transport/testdata/legacy_link_event.json`.

## Manual smoke script

A quick local smoke check script is provided:

```bash
cd server/relay
./scripts/relay_smoke.sh
```

The smoke script starts a local relay instance, opens two synthetic clients, executes hello/create/join/publish flow, and asserts that both clients receive `inboundLinkEvent` with `serverSequence: 1`.

## Troubleshooting

The relay emits structured JSON logs for inbound/outbound message flow, state transitions, and protocol violations. Sensitive fields (for example `authToken`, `token`, and `secret`) are redacted before being written.

Capture logs while running the server:

```bash
go run . --bind 0.0.0.0 --port 41000 --secret my-secret | tee relay.log
```

Inspect useful markers:

```bash
rg -n "protocolViolation|serverSequence|roomId|redact|token" relay.log server/relay -S
```

### Protocol mismatch

- Look for `event:"protocolViolation"` with `code:426` and `category:"protocolError"`.
- Inspect the nearest inbound `messageKind:"hello"` record to compare `protocolVersion` and `clientSequence`.

### Sequence violations

- Look for `event:"protocolViolation"` with `code:409` and sequence-related messages.
- Correlate the offending inbound publish (`messageKind:"publishLinkEvent"`, `clientSequence`) with the latest outbound `serverSequence` values.

### Room full / join rejected

- Look for `event:"protocolViolation"` with `message` containing `room join denied` or `room create denied`.
- `code:403` typically indicates full/denied; `code:404` indicates room not found.

### Heartbeat timeouts

- Look for `event:"protocolViolation"` with `category:"networkTimeout"` and `message:"heartbeat timeout"`.
- Verify the latest heartbeat inbound record (`messageKind:"heartbeat"`) for that `roomId`/`playerId` and compare against timeout settings.
