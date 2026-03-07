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

## Appendix A) Server implementation contract (client-internals independent)

This appendix defines the minimum server-side behavior for v1 so a relay can be implemented without reading Qt client source.

### A.1 Responsibilities by message kind

Message routing and responsibilities are defined by top-level discriminator:

- Client -> server: `intent`
- Server -> client: `kind`

#### A.1.1 Client intents (`intent`)

1. `hello`
   - Validate framing, JSON schema, and `protocolVersion`.
   - Validate any authentication token, if required by deployment policy.
   - On success, transition connection into authenticated/session-ready state.
   - Emit either:
     - success event(s) that establish identity/session (`playerAssigned` at minimum), or
     - `error` then `disconnected` if rejected.

2. `joinRoom`
   - Validate room existence/creation policy and capacity.
   - Attach connection to target room atomically.
   - Assign/confirm room-local player slot before sending room-joined visibility events.
   - Notify caller first (room membership success), then notify peers (`peerJoined`).

3. `leaveRoom`
   - Detach client from current room.
   - Notify remaining peers that player left.
   - If server policy closes idle rooms, close room and emit `disconnected`/room closure notices as needed.

4. `publishLinkEvent`
   - Validate envelope fields (`sequence`, `senderPlayerId`, payload presence/type).
   - Enforce per-sender monotonic `LinkEventEnvelope.sequence`.
   - Assign canonical room-wide `serverSequence` (see A.2).
   - Rebroadcast to all intended recipients in canonical order.

5. `heartbeat`
   - Record liveness timestamp on receipt.
   - Emit `heartbeatAck` promptly (see A.3).
   - Do not mutate room ordering state except optional observability counters.

6. `disconnect` (if used by deployment)
   - Treat as graceful client-initiated disconnect.
   - Emit peer leave notifications as applicable.
   - Close TCP connection after flushing terminal events.

#### A.1.2 Server events (`kind`)

Server must produce consistent events with v1 semantics:

- `playerAssigned`: confirms identity/slot for this connection.
- `peerJoined`: announces a new room participant to existing peers.
- `peerLeft`: announces participant departure.
- `linkEvent`: rebroadcasted gameplay event with canonical `serverSequence`.
- `heartbeatAck`: acknowledges most recent heartbeat handling.
- `error`: machine-actionable failure with `code` + message.
- `disconnected`: terminal reason before connection close whenever feasible.

### A.2 Canonical rebroadcast ordering and `serverSequence`

For each room, the server MUST maintain a single monotonic `serverSequence` counter:

- Scope: per room.
- Initial value: implementation-defined (`0` or `1` recommended), but stable within deployment.
- Increment rule: increment exactly once per accepted `publishLinkEvent` before rebroadcast.

Canonical processing algorithm:

1. Accept inbound `publishLinkEvent` from an authenticated client in a joined room.
2. Validate sender ordering (`event.sequence` strictly increasing for that sender connection/player).
3. Atomically reserve next room `serverSequence` value.
4. Attach reserved `serverSequence` to outbound `linkEvent`.
5. Fan out to recipients preserving ascending `serverSequence` emission order.

Non-negotiable ordering rules:

- No two accepted link events in the same room may share a `serverSequence`.
- Recipients must never observe decreasing `serverSequence` values.
- On rejection/validation failure, no `serverSequence` is consumed.
- Concurrent publishes must serialize at assignment point, not at socket-read order assumptions.

### A.3 Heartbeat ACK timing and timeout semantics

Heartbeat behavior is liveness-critical and should be deterministic:

- ACK trigger: every valid inbound `heartbeat` intent receives one `heartbeatAck`.
- ACK timing target: immediate on receive-path completion (recommended under 1 second).
- ACK ordering: `heartbeatAck` must not overtake already-queued earlier `linkEvent` frames for that connection.

Timeout semantics:

- Server tracks last-received heartbeat timestamp per connection.
- If no valid heartbeat (or any policy-accepted liveness traffic) is seen within server timeout window, server:
  1. emits `error` with timeout code (when connection still writable),
  2. emits `disconnected` with reason `"networkTimeout"`,
  3. closes TCP connection.
- Timeout duration is deployment-configurable, but all clients in a deployment should be held to the same default.

### A.4 Error codes and required disconnect reasons

Server error `code` values are numeric and stable. Recommended minimum v1 set:

- `400` - malformed message/frame/schema (`MalformedMessage`).
- `401` - authentication required/failed.
- `403` - forbidden (room access denied / kicked policy).
- `404` - room not found (when auto-create is disabled).
- `409` - sequencing conflict (duplicate or out-of-order `clientSequence` or link `sequence`).
- `413` - payload too large.
- `426` - unsupported protocol version (`ProtocolMismatch`).
- `429` - rate limited / too many requests.
- `500` - internal server error.
- `503` - temporary unavailable / server shutting down.

Disconnect reasons the server must emit via `disconnected.reason` when applicable:

- `"clientRequested"` for graceful client disconnect.
- `"serverShutdown"` for planned server termination.
- `"networkTimeout"` for heartbeat/liveness timeout.
- `"protocolError"` for framing/schema/version/ordering violations causing disconnect.
- `"roomClosed"` when room lifecycle ends session.
- `"kicked"` for administrative/policy removal.

Reason strings must match exactly to preserve enum mapping in clients (see section 6).

### A.5 Example end-to-end transcript

Illustrative sequence (JSON payloads only; each payload is carried in framed transport as defined in section 3):

1) Client connects TCP

2) Client -> Server (`hello`)

```json
{
  "intent": "hello",
  "protocolVersion": 1,
  "clientSequence": 0,
  "authToken": "token-abc"
}
```

3) Server -> Client (`playerAssigned`)

```json
{
  "kind": "playerAssigned",
  "protocolVersion": 1,
  "playerId": 2,
  "displayName": "Player 3"
}
```

4) Client -> Server (`joinRoom`)

```json
{
  "intent": "joinRoom",
  "clientSequence": 1,
  "roomId": "room-42"
}
```

5) Server -> Client (`roomJoined` style confirmation)

```json
{
  "kind": "roomJoined",
  "roomId": "room-42",
  "memberCount": 2
}
```

6) Server -> Client (`peerJoined` for another participant)

```json
{
  "kind": "peerJoined",
  "roomId": "room-42",
  "playerId": 7,
  "displayName": "Player 8"
}
```

7) Client -> Server (`publishLinkEvent`)

```json
{
  "intent": "publishLinkEvent",
  "clientSequence": 2,
  "event": {
    "sequence": 15,
    "senderPlayerId": 2,
    "tickMarker": 123456,
    "payload": "BASE64_BYTES"
  }
}
```

8) Server -> Room participants (`linkEvent` rebroadcast with canonical `serverSequence`)

```json
{
  "kind": "linkEvent",
  "roomId": "room-42",
  "serverSequence": 98,
  "event": {
    "sequence": 15,
    "senderPlayerId": 2,
    "tickMarker": 123456,
    "payload": "BASE64_BYTES"
  }
}
```

9) Client -> Server (`heartbeat`)

```json
{
  "intent": "heartbeat",
  "clientSequence": 3,
  "timestampMs": 1735000000
}
```

10) Server -> Client (`heartbeatAck`)

```json
{
  "kind": "heartbeatAck",
  "ackClientSequence": 3,
  "serverTimeMs": 1735000005
}
```

11) Client -> Server (`disconnect`)

```json
{
  "intent": "disconnect",
  "clientSequence": 4,
  "reason": "clientRequested"
}
```

12) Server -> Client (`disconnected`)

```json
{
  "kind": "disconnected",
  "reason": "clientRequested",
  "message": "Client requested disconnect"
}
```
