# mGBA Link Relay (experimental)

A lightweight TCP relay service for link-session traffic.

## Layout

- `main.go`: service entrypoint.
- `config/`: CLI and environment-driven configuration loading.
- `logging/`: logger construction.
- `protocol/`: newline-delimited JSON protocol primitives.
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

Line-delimited JSON messages:

- Client joins:

```json
{"type":"join","roomId":"abc","player":"p1","secret":"my-secret"}
```

- Client payload:

```json
{"type":"message","payload":{"frame":1234,"data":"..."}}
```

- Heartbeat reply:

```json
{"type":"pong"}
```
