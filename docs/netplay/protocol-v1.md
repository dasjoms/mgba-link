# Netplay Protocol v1 (Non-negotiables)

This document locks the protocol invariants that must be upheld **before** implementation work proceeds.

## 1) Scope

Protocol v1 covers:

- Qt netplay client session transport and message handling under `src/platform/qt/netplay/*`.
- Planned self-hosted relay server behavior expected by that client.

Out of scope for this document:

- Emulator-core lockstep algorithm changes.
- Frontends other than Qt.
- Matchmaking/discovery API design.

## 2) Transport choice for v1

- **Required transport:** TCP byte stream.
- **Contract:** ordered, reliable delivery between one client and server connection.
- **Deferred:** UDP/QUIC and any datagram transport variants are explicitly out of scope for v1.

## 3) Wire framing

Every application message is encoded as:

1. 4-byte **big-endian** unsigned length prefix.
2. Exactly `length` payload bytes.

The current Qt implementation already follows this framing via `_sendFrame` and `_drainFrames`.

Payload format for current `TcpSession` frames is JSON text, parsed in `_handleFrame`.

### Framing example (conceptual)

- Length prefix: `00 00 00 2A` (42 bytes)
- Payload bytes: 42 bytes of UTF-8 JSON (for example `{"intent":"heartbeat","clientSequence":12,...}`)

## 4) Protocol versioning

Handshake messages MUST include an integer field:

- `protocolVersion` (e.g. `1` for this spec).

### Version mismatch behavior

If the peer's `protocolVersion` is unsupported:

1. Receiver sends an `error` message indicating protocol mismatch.
2. Receiver disconnects the TCP session.
3. Disconnect reason maps to `DisconnectReason::ProtocolError`.

### Forward compatibility rule

- Unknown **optional** fields MUST be ignored.
- Unknown required fields, invalid types for required fields, or missing required fields are protocol errors.

This permits additive extensions without breaking v1 peers.

## 5) Session sequencing semantics

Two monotonic counters are non-negotiable:

1. `clientSequence` (client->server intents)
   - Monotonic increasing per TCP connection.
   - Assigned by sender on each outbound intent.
2. `LinkEventEnvelope.sequence` (per link-event sender)
   - Monotonic increasing per sender.
   - Used for deterministic ordering and validation.

### Reset boundaries

Both counters reset only when a **new TCP connection** is established.

- They do **not** reset on room leave/join transitions within the same connection.

## 6) Error taxonomy and disconnect mapping

Protocol and transport errors must map to existing `SessionTypes.h` enums.

### `NetplayErrorCategory` mapping

- `ConnectionFailure`: socket/transport failures, remote disconnect transport errors.
- `ProtocolMismatch`: unknown event kinds, unsupported protocol version, sequencing violations.
- `HeartbeatTimeout`: watchdog timeout waiting for inbound heartbeat traffic.
- `RoomRejectedOrFull`: authorization/room-capacity/rejection responses.
- `MalformedMessage`: invalid frame length, non-JSON payload, schema/type errors.

### Disconnect reason strings for wire messages

Wire-level `disconnected.reason` values should map as follows:

- `"none"` -> `DisconnectReason::None`
- `"clientRequested"` -> `DisconnectReason::ClientRequested`
- `"serverShutdown"` -> `DisconnectReason::ServerShutdown`
- `"networkTimeout"` -> `DisconnectReason::NetworkTimeout`
- `"protocolError"` -> `DisconnectReason::ProtocolError`
- `"roomClosed"` -> `DisconnectReason::RoomClosed`
- `"kicked"` -> `DisconnectReason::Kicked`
- any unknown value -> `DisconnectReason::Unknown`

## 7) Message examples (JSON payloads)

These examples are payload JSON carried inside the length-prefixed frame.

### 7.1 Client handshake request

```json
{
  "intent": "hello",
  "protocolVersion": 1,
  "authToken": "optional-token",
  "clientSequence": 0
}
```

### 7.2 Server handshake success response

```json
{
  "kind": "playerAssigned",
  "playerId": 2,
  "displayName": "Player 3",
  "protocolVersion": 1
}
```

### 7.3 Server protocol mismatch error + disconnect

```json
{
  "kind": "error",
  "code": 426,
  "message": "Unsupported protocolVersion: 2. Expected: 1",
  "sequence": 0
}
```

```json
{
  "kind": "disconnected",
  "reason": "protocolError",
  "message": "Protocol version mismatch"
}
```

### 7.4 Client publish link event intent

```json
{
  "intent": "publishLinkEvent",
  "clientSequence": 41,
  "event": {
    "sequence": 1055,
    "senderPlayerId": 2,
    "tickMarker": 987654,
    "payload": "BASE64_BYTES"
  }
}
```

## 8) Terminology alignment references

Implementers should use field names and semantics consistent with:

- `src/platform/qt/netplay/SessionTypes.h`
  - `LinkEventEnvelope`
  - `DisconnectReason`
  - client intent / server event structs
- `src/platform/qt/netplay/TcpSession.cpp`
  - `_sendFrame`
  - `_drainFrames`
  - `_handleFrame`


## 9) Logging for desync/debug analysis

Protocol and codec violations should be logged as a single structured line with these fields:

- `direction` (`in`/`out`), `kind`, `roomId`, `playerId`, `sequence`, `serverSequence`, `state`.
- `code` and concise `reason` for the violation.

Interpretation tips:

- Track `serverSequence` gaps or unexpected jumps to quickly identify ordering/desync faults between relay and client.
- Compare `direction=out` `sequence` to matching server-side receipt logs to detect dropped or rejected intents.
- Treat any token/auth/secret fields in `details` as redacted (`<redacted>`) by design when correlating auth failures.
