/* Copyright (c) 2013-2015 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QList>
#include <QString>
#include <QVariantMap>

namespace QGBA {
namespace Netplay {

// Generic lifecycle for a multiplayer session transport.
//
// This is intentionally independent of GBASIODriver and emulator internals.
// The module is intended to back MultiplayerController as an alternative to
// local-only lockstep.
enum class SessionState {
	Disconnected,
	Connecting,
	Connected,
	InRoom,
	Error,
};

enum class SessionEventType {
	LinkInput,
	LinkControl,
	Heartbeat,
	Custom,
};

// Canonical wire-level message names for client->server intents.
// Keep these aligned with backend protocol adapters.
enum class ClientIntentKind {
	Hello,
	CreateRoom,
	JoinRoom,
	LeaveRoom,
	Heartbeat,
	PublishLinkEvent,
};

// Canonical wire-level message names for server->client events.
// Keep these aligned with backend protocol adapters.
enum class ServerEventKind {
	RoomJoined,
	PlayerAssigned,
	PeerJoined,
	PeerLeft,
	InboundLinkEvent,
	HeartbeatAck,
	Error,
	Disconnected,
};

static constexpr const char* CLIENT_INTENT_HELLO = "hello";
static constexpr const char* CLIENT_INTENT_CREATE_ROOM = "createRoom";
static constexpr const char* CLIENT_INTENT_JOIN_ROOM = "joinRoom";
static constexpr const char* CLIENT_INTENT_LEAVE_ROOM = "leaveRoom";
static constexpr const char* CLIENT_INTENT_HEARTBEAT = "heartbeat";
static constexpr const char* CLIENT_INTENT_PUBLISH_LINK_EVENT = "publishLinkEvent";

static constexpr const char* SERVER_EVENT_ROOM_JOINED = "roomJoined";
static constexpr const char* SERVER_EVENT_PLAYER_ASSIGNED = "playerAssigned";
static constexpr const char* SERVER_EVENT_PEER_JOINED = "peerJoined";
static constexpr const char* SERVER_EVENT_PEER_LEFT = "peerLeft";
static constexpr const char* SERVER_EVENT_INBOUND_LINK_EVENT = "inboundLinkEvent";
static constexpr const char* SERVER_EVENT_HEARTBEAT_ACK = "heartbeatAck";
static constexpr const char* SERVER_EVENT_ERROR = "error";
static constexpr const char* SERVER_EVENT_DISCONNECTED = "disconnected";

static constexpr int NETPLAY_MAX_FRAME_PAYLOAD_BYTES = 4096;
static constexpr int NETPLAY_MAX_LINK_PAYLOAD_BYTES = 1024;

enum class NetplayErrorCategory {
	ConnectionFailure,
	ProtocolMismatch,
	HeartbeatTimeout,
	RoomRejectedOrFull,
	MalformedMessage,
};

enum class NetplayFailureLayer {
	TransportSession,
	BackendAdapter,
	ControllerIntegration,
};

static inline const char* netplayErrorCategoryName(NetplayErrorCategory category) {
	switch (category) {
	case NetplayErrorCategory::ConnectionFailure:
		return "connection_failure";
	case NetplayErrorCategory::ProtocolMismatch:
		return "protocol_mismatch";
	case NetplayErrorCategory::HeartbeatTimeout:
		return "heartbeat_timeout";
	case NetplayErrorCategory::RoomRejectedOrFull:
		return "room_rejected_or_full";
	case NetplayErrorCategory::MalformedMessage:
		return "malformed_message";
	}
	return "unknown";
}

static inline const char* netplayFailureLayerName(NetplayFailureLayer layer) {
	switch (layer) {
	case NetplayFailureLayer::TransportSession:
		return "transport_session";
	case NetplayFailureLayer::BackendAdapter:
		return "backend_adapter";
	case NetplayFailureLayer::ControllerIntegration:
		return "controller_integration";
	}
	return "unknown";
}

struct SessionPeer {
	QString peerId;
	QString displayName;
	bool isLocal = false;
};

struct SessionRoom {
	QString roomId;
	QString roomName;
	int maxPeers = 0;
	QList<SessionPeer> peers;
};

// Transport-independent envelope for deterministic ordering of link events.
//
// The fields in this envelope are required regardless of the underlying
// transport (WebSocket, UDP relay, etc.) so that different backends can still
// produce the same simulation order.
struct LinkEventEnvelope {
	// Protocol schema version used by sender and validated by receiver.
	int protocolVersion = 1;

	// Server-authoritative room ID where this event is scoped.
	// Required whenever the sender is room-bound.
	QString roomId;

	// Monotonic, per-sender sequence number assigned by the sender.
	// This value is sender-owned/proposed and validated by the server.
	//
	// Resets to 0 when a client establishes a new connection to the server.
	// It does not reset on room changes within the same transport connection.
	qint64 sequence = 0;

	// Optional server-authored sequence for globally ordered rebroadcasts.
	// Reserved for future server ordering behavior.
	qint64 serverSequence = -1;

	// Server-authoritative numeric player ID of the sender for room-scoped
	// ordering/ownership checks. Assigned by the server when joining a room.
	int senderPlayerId = -1;

	// Logical simulation marker (tick/cycle/frame) chosen by the link layer.
	// Required for deterministic ordering across peers and transport backends.
	// This is sender-owned/proposed and interpreted by deterministic merge logic.
	qint64 tickMarker = 0;

	// Opaque serialized link payload bytes.
	// Maximum size: NETPLAY_MAX_LINK_PAYLOAD_BYTES.
	QByteArray payload;
};

// hello
// Required: protocolVersion.
// Optional: roomId (for reconnect hints), metadata.
struct ClientHelloIntent {
	int protocolVersion = 1;
	QString roomId;
	QVariantMap metadata;
};

// createRoom
// Required: roomName, maxPlayers.
// Optional: protocolVersion.
struct ClientCreateRoomIntent {
	int protocolVersion = 1;
	QString roomName;
	int maxPlayers = 0;
};

// joinRoom
// Required: protocolVersion, roomId.
// Optional: none.
struct ClientJoinRoomIntent {
	int protocolVersion = 1;
	// Server-authoritative room ID returned by room discovery/creation.
	QString roomId;
};

// leaveRoom
// Required: roomId.
// Optional: protocolVersion.
struct ClientLeaveRoomIntent {
	int protocolVersion = 1;
	// Server-authoritative room ID for the room being left.
	QString roomId;
};

// heartbeat
// Required: heartbeatCounter.
// Optional: protocolVersion, roomId.
struct ClientHeartbeatIntent {
	int protocolVersion = 1;
	QString roomId;

	// Monotonic, per-connection heartbeat counter generated by the client.
	// Resets to 0 whenever the transport connection is re-established.
	qint64 heartbeatCounter = 0;
};

// publishLinkEvent
// Required: event.
// Optional: none.
struct ClientPublishLinkEventIntent {
	LinkEventEnvelope event;
};

enum class DisconnectReason {
	None,
	ClientRequested,
	ServerShutdown,
	NetworkTimeout,
	ProtocolError,
	RoomClosed,
	Kicked,
	Unknown,
};

// roomJoined
// Required: protocolVersion, roomId, roomName, maxPlayers.
// Optional: none.
struct ServerRoomJoinedEvent {
	int protocolVersion = 1;

	// Server-authoritative room ID.
	QString roomId;
	QString roomName;
	int maxPlayers = 0;
};

// playerAssigned
// Required: protocolVersion, roomId, playerId.
// Optional: none.
struct ServerPlayerAssignedEvent {
	int protocolVersion = 1;
	QString roomId;

	// Server-authoritative player ID for this connection within the room.
	int playerId = -1;
};

// peerJoined
// Required: protocolVersion, roomId, playerId.
// Optional: displayName.
struct ServerPeerJoinedEvent {
	int protocolVersion = 1;
	QString roomId;

	// Server-authoritative player ID for the peer that joined.
	int playerId = -1;
	QString displayName;
};

// peerLeft
// Required: protocolVersion, roomId, playerId, reason.
// Optional: none.
struct ServerPeerLeftEvent {
	int protocolVersion = 1;
	QString roomId;

	// Server-authoritative player ID for the peer that left.
	int playerId = -1;
	DisconnectReason reason = DisconnectReason::Unknown;
};

// inboundLinkEvent
// Required: event.
// Optional: none.
struct ServerInboundLinkEvent {
	LinkEventEnvelope event;
};

// heartbeatAck
// Required: protocolVersion, heartbeatCounter.
// Optional: roomId.
struct ServerHeartbeatAckEvent {
	int protocolVersion = 1;
	QString roomId;
	qint64 heartbeatCounter = 0;
};

// error
// Required: protocolVersion, code, message.
// Optional: roomId.
struct ServerErrorEvent {
	int protocolVersion = 1;
	QString roomId;
	int code = 0;
	QString message;
};

// disconnected
// Required: protocolVersion, reason.
// Optional: roomId, message.
struct ServerDisconnectedEvent {
	int protocolVersion = 1;
	QString roomId;
	DisconnectReason reason = DisconnectReason::Unknown;
	QString message;
};

// Envelope for synchronized link/session events.
//
// The payload is opaque to this API. Higher-level systems can serialize any
// protocol-specific data and pass it through this envelope.
struct SessionEventEnvelope {
	int protocolVersion = 1;
	QString roomId;
	QString eventId;
	QString sourcePeerId;
	// Sender-owned/proposed monotonic event sequence.
	qint64 sequence = 0;
	// Optional server-authored sequence for globally ordered rebroadcasts.
	qint64 serverSequence = -1;
	SessionEventType type = SessionEventType::Custom;
	// Maximum size: NETPLAY_MAX_FRAME_PAYLOAD_BYTES.
	QByteArray payload;
	QDateTime sentAtUtc;
	QVariantMap metadata;
};

struct SessionProtocolError {
	int code = 0;
	QString message;
	NetplayErrorCategory category = NetplayErrorCategory::ProtocolMismatch;
	NetplayFailureLayer layer = NetplayFailureLayer::TransportSession;
	QString endpoint;
	QString roomId;
	qint64 sequence = -1;
	qint64 expectedSequence = -1;
	QVariantMap details;
};

} // namespace Netplay
} // namespace QGBA
