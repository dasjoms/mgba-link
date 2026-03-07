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
- Hard protocol violations emit `error` and then `disconnected` before socket close.
