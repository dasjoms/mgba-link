/* Copyright (c) 2013-2015 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "netplay/SessionMessageAdapter.h"

#include <utility>

#include <QDebug>

namespace QGBA {
namespace Netplay {

SessionMessageAdapter::SessionMessageAdapter() = default;

void SessionMessageAdapter::setControllerCallbacks(ControllerCallbacks callbacks) {
	m_callbacks = std::move(callbacks);
}

ClientCreateRoomIntent SessionMessageAdapter::hostRoom(const QString& roomName, int maxPlayers) const {
	ClientCreateRoomIntent intent;
	intent.roomName = roomName;
	intent.maxPlayers = maxPlayers;
	return intent;
}

ClientJoinRoomIntent SessionMessageAdapter::joinRoom(const QString& roomId) const {
	ClientJoinRoomIntent intent;
	intent.roomId = roomId;
	return intent;
}

ClientLeaveRoomIntent SessionMessageAdapter::leaveRoom(const QString& roomId) const {
	ClientLeaveRoomIntent intent;
	intent.roomId = roomId;
	return intent;
}

ClientPublishLinkEventIntent SessionMessageAdapter::broadcastLinkEvent(qint64 tickMarker, const QByteArray& payload) {
	ClientPublishLinkEventIntent intent;
	intent.event.sequence = m_nextOutboundSequence++;
	intent.event.senderPlayerId = m_localPlayerId;
	intent.event.tickMarker = tickMarker;
	intent.event.payload = payload;
	return intent;
}

void SessionMessageAdapter::handleServerEvent(const ServerRoomJoinedEvent& event) {
	if (event.roomId.isEmpty()) {
		_reportProtocolError(106, QStringLiteral("roomJoined missing room ID"), NetplayErrorCategory::ProtocolMismatch, -1, -1, {
			{QStringLiteral("eventKind"), QStringLiteral(SERVER_EVENT_ROOM_JOINED)},
		});
		return;
	}

	_resetRoomState();
	m_roomId = event.roomId;
	if (m_callbacks.onRoomJoined) {
		m_callbacks.onRoomJoined(event);
	}
}

void SessionMessageAdapter::handleServerEvent(const ServerPlayerAssignedEvent& event) {
	if (!_validateRoomScope(event.roomId, 107, QStringLiteral("playerAssigned room mismatch"), {
		{QStringLiteral("eventKind"), QStringLiteral(SERVER_EVENT_PLAYER_ASSIGNED)},
		{QStringLiteral("eventRoomId"), event.roomId},
	})) {
		return;
	}

	if (event.playerId < 0) {
		_reportProtocolError(101, QStringLiteral("Invalid local player assignment"), NetplayErrorCategory::ProtocolMismatch, -1, -1, {
			{QStringLiteral("eventKind"), QStringLiteral(SERVER_EVENT_PLAYER_ASSIGNED)},
			{QStringLiteral("playerId"), event.playerId},
		});
		return;
	}
	m_localPlayerId = event.playerId;
	m_knownPlayers.insert(event.playerId);
	if (m_callbacks.onPlayerAssigned) {
		m_callbacks.onPlayerAssigned(event);
	}
}

void SessionMessageAdapter::handleServerEvent(const ServerPeerJoinedEvent& event) {
	if (!_validateRoomScope(event.roomId, 108, QStringLiteral("peerJoined room mismatch"), {
		{QStringLiteral("eventKind"), QStringLiteral(SERVER_EVENT_PEER_JOINED)},
		{QStringLiteral("eventRoomId"), event.roomId},
	})) {
		return;
	}

	if (event.playerId < 0) {
		_reportProtocolError(102, QStringLiteral("Invalid peer join player ID"), NetplayErrorCategory::ProtocolMismatch, -1, -1, {
			{QStringLiteral("eventKind"), QStringLiteral(SERVER_EVENT_PEER_JOINED)},
			{QStringLiteral("playerId"), event.playerId},
		});
		return;
	}
	m_knownPlayers.insert(event.playerId);
	if (m_callbacks.onPeerJoined) {
		m_callbacks.onPeerJoined(event);
	}
}

void SessionMessageAdapter::handleServerEvent(const ServerPeerLeftEvent& event) {
	if (!_validateRoomScope(event.roomId, 109, QStringLiteral("peerLeft room mismatch"), {
		{QStringLiteral("eventKind"), QStringLiteral(SERVER_EVENT_PEER_LEFT)},
		{QStringLiteral("eventRoomId"), event.roomId},
	})) {
		return;
	}

	if (event.playerId < 0) {
		_reportProtocolError(110, QStringLiteral("Invalid peer left player ID"), NetplayErrorCategory::ProtocolMismatch, -1, -1, {
			{QStringLiteral("eventKind"), QStringLiteral(SERVER_EVENT_PEER_LEFT)},
			{QStringLiteral("playerId"), event.playerId},
		});
		return;
	}

	if (!m_knownPlayers.contains(event.playerId)) {
		_reportProtocolError(103, QStringLiteral("Unknown player left room"), NetplayErrorCategory::ProtocolMismatch, -1, -1, {
			{QStringLiteral("eventKind"), QStringLiteral(SERVER_EVENT_PEER_LEFT)},
			{QStringLiteral("playerId"), event.playerId},
		});
		return;
	}
	m_knownPlayers.remove(event.playerId);
	m_lastInboundSequenceByPlayer.remove(event.playerId);
	if (event.playerId == m_localPlayerId) {
		m_localPlayerId = -1;
	}
	if (m_callbacks.onPeerLeft) {
		m_callbacks.onPeerLeft(event);
	}
}

void SessionMessageAdapter::handleServerEvent(const ServerInboundLinkEvent& event) {
	if (!m_roomId.isEmpty() && event.event.roomId != m_roomId) {
		_reportProtocolError(111, QStringLiteral("inboundLinkEvent room mismatch"), NetplayErrorCategory::ProtocolMismatch, event.event.sequence, -1, {
			{QStringLiteral("eventKind"), QStringLiteral(SERVER_EVENT_INBOUND_LINK_EVENT)},
			{QStringLiteral("eventRoomId"), event.event.roomId},
			{QStringLiteral("adapterRoomId"), m_roomId},
			{QStringLiteral("senderPlayerId"), event.event.senderPlayerId},
		});
		return;
	}

	const int senderPlayerId = event.event.senderPlayerId;
	if (senderPlayerId < 0) {
		_reportProtocolError(112, QStringLiteral("inboundLinkEvent has invalid senderPlayerId"), NetplayErrorCategory::ProtocolMismatch, event.event.sequence, -1, {
			{QStringLiteral("eventKind"), QStringLiteral(SERVER_EVENT_INBOUND_LINK_EVENT)},
			{QStringLiteral("senderPlayerId"), senderPlayerId},
			{QStringLiteral("sequence"), event.event.sequence},
		});
		return;
	}

	if (event.event.tickMarker < 0) {
		_reportProtocolError(113, QStringLiteral("inboundLinkEvent has malformed tick marker"), NetplayErrorCategory::MalformedMessage, event.event.sequence, -1, {
			{QStringLiteral("eventKind"), QStringLiteral(SERVER_EVENT_INBOUND_LINK_EVENT)},
			{QStringLiteral("senderPlayerId"), senderPlayerId},
			{QStringLiteral("tickMarker"), event.event.tickMarker},
		});
		return;
	}

	if (!m_knownPlayers.contains(senderPlayerId)) {
		_reportProtocolError(104, QStringLiteral("Link event from unknown player"), NetplayErrorCategory::ProtocolMismatch, event.event.sequence, -1, {
			{QStringLiteral("eventKind"), QStringLiteral(SERVER_EVENT_INBOUND_LINK_EVENT)},
			{QStringLiteral("senderPlayerId"), senderPlayerId},
			{QStringLiteral("sequence"), event.event.sequence},
		});
		return;
	}

	const qint64 lastSequence = m_lastInboundSequenceByPlayer.value(senderPlayerId, -1);
	if (event.event.sequence < lastSequence) {
		_reportProtocolError(105, QStringLiteral("Out-of-order link event sequence"), NetplayErrorCategory::MalformedMessage, event.event.sequence, lastSequence + 1, {
			{QStringLiteral("eventKind"), QStringLiteral(SERVER_EVENT_INBOUND_LINK_EVENT)},
			{QStringLiteral("senderPlayerId"), senderPlayerId},
			{QStringLiteral("lastSequence"), lastSequence},
			{QStringLiteral("receivedSequence"), event.event.sequence},
		});
		return;
	}

	if (event.event.sequence == lastSequence) {
		_reportProtocolError(114, QStringLiteral("Duplicate link event sequence dropped"), NetplayErrorCategory::ProtocolMismatch, event.event.sequence, lastSequence + 1, {
			{QStringLiteral("eventKind"), QStringLiteral(SERVER_EVENT_INBOUND_LINK_EVENT)},
			{QStringLiteral("policy"), QStringLiteral("drop_duplicate_event")},
			{QStringLiteral("senderPlayerId"), senderPlayerId},
			{QStringLiteral("lastSequence"), lastSequence},
			{QStringLiteral("receivedSequence"), event.event.sequence},
		});
		return;
	}

	m_lastInboundSequenceByPlayer.insert(senderPlayerId, event.event.sequence);
	if (m_callbacks.onLinkEvent) {
		m_callbacks.onLinkEvent(event);
	}
}

void SessionMessageAdapter::handleServerEvent(const ServerDisconnectedEvent& event) {
	if (!m_roomId.isEmpty() && !event.roomId.isEmpty() && event.roomId != m_roomId) {
		_reportProtocolError(115, QStringLiteral("disconnected room mismatch"), NetplayErrorCategory::ProtocolMismatch, -1, -1, {
			{QStringLiteral("eventKind"), QStringLiteral(SERVER_EVENT_DISCONNECTED)},
			{QStringLiteral("eventRoomId"), event.roomId},
			{QStringLiteral("adapterRoomId"), m_roomId},
		});
		return;
	}

	_resetRoomState();
	if (m_callbacks.onDisconnected) {
		m_callbacks.onDisconnected(event);
	}
}

void SessionMessageAdapter::handleServerEvent(const ServerErrorEvent& event) {
	_reportProtocolError(event.code, event.message, NetplayErrorCategory::ProtocolMismatch);
}

int SessionMessageAdapter::localPlayerId() const {
	return m_localPlayerId;
}

bool SessionMessageAdapter::_validateRoomScope(const QString& roomId, int code, const QString& message, const QVariantMap& details) const {
	if (m_roomId.isEmpty() || roomId.isEmpty() || roomId == m_roomId) {
		return true;
	}

	QVariantMap enrichedDetails = details;
	enrichedDetails.insert(QStringLiteral("adapterRoomId"), m_roomId);
	_reportProtocolError(code, message, NetplayErrorCategory::ProtocolMismatch, -1, -1, enrichedDetails);
	return false;
}

void SessionMessageAdapter::_reportProtocolError(int code, const QString& message, NetplayErrorCategory category, qint64 sequence, qint64 expectedSequence, const QVariantMap& details) const {
	qWarning().noquote()
		<< QStringLiteral("[netplay][layer=%1][category=%2] backend adapter error code=%3 message=\"%4\" room=%5 sequence=%6 expectedSequence=%7")
			.arg(QString::fromLatin1(netplayFailureLayerName(NetplayFailureLayer::BackendAdapter)))
			.arg(QString::fromLatin1(netplayErrorCategoryName(category)))
			.arg(code)
			.arg(message)
			.arg(m_roomId.isEmpty() ? QStringLiteral("n/a") : m_roomId)
			.arg(sequence >= 0 ? QString::number(sequence) : QStringLiteral("n/a"))
			.arg(expectedSequence >= 0 ? QString::number(expectedSequence) : QStringLiteral("n/a"));

	if (!m_callbacks.onProtocolError) {
		return;
	}
	SessionProtocolError error;
	error.code = code;
	error.message = message;
	error.category = category;
	error.layer = NetplayFailureLayer::BackendAdapter;
	error.roomId = m_roomId;
	error.sequence = sequence;
	error.expectedSequence = expectedSequence;
	error.details = details;
	m_callbacks.onProtocolError(error);
}

void SessionMessageAdapter::_resetRoomState() {
	m_localPlayerId = -1;
	m_roomId.clear();
	m_knownPlayers.clear();
	m_lastInboundSequenceByPlayer.clear();
}

} // namespace Netplay
} // namespace QGBA
