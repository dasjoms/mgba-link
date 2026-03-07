/* Copyright (c) 2013-2015 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "netplay/SessionMessageAdapter.h"

#include <utility>

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
	_resetRoomState();
	if (m_callbacks.onRoomJoined) {
		m_callbacks.onRoomJoined(event);
	}
}

void SessionMessageAdapter::handleServerEvent(const ServerPlayerAssignedEvent& event) {
	if (event.playerId < 0) {
		_reportProtocolError(101, QStringLiteral("Invalid local player assignment"), {
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
	if (event.playerId < 0) {
		_reportProtocolError(102, QStringLiteral("Invalid peer join player ID"), {
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
	if (!m_knownPlayers.contains(event.playerId)) {
		_reportProtocolError(103, QStringLiteral("Unknown player left room"), {
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
	const int senderPlayerId = event.event.senderPlayerId;
	if (!m_knownPlayers.contains(senderPlayerId)) {
		_reportProtocolError(104, QStringLiteral("Link event from unknown player"), {
			{QStringLiteral("senderPlayerId"), senderPlayerId},
			{QStringLiteral("sequence"), event.event.sequence},
		});
		return;
	}

	const qint64 lastSequence = m_lastInboundSequenceByPlayer.value(senderPlayerId, -1);
	if (event.event.sequence <= lastSequence) {
		_reportProtocolError(105, QStringLiteral("Out-of-order link event sequence"), {
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
	_resetRoomState();
	if (m_callbacks.onDisconnected) {
		m_callbacks.onDisconnected(event);
	}
}

void SessionMessageAdapter::handleServerEvent(const ServerErrorEvent& event) {
	_reportProtocolError(event.code, event.message);
}

int SessionMessageAdapter::localPlayerId() const {
	return m_localPlayerId;
}

void SessionMessageAdapter::_reportProtocolError(int code, const QString& message, const QVariantMap& details) const {
	if (!m_callbacks.onProtocolError) {
		return;
	}
	SessionProtocolError error;
	error.code = code;
	error.message = message;
	error.details = details;
	m_callbacks.onProtocolError(error);
}

void SessionMessageAdapter::_resetRoomState() {
	m_localPlayerId = -1;
	m_knownPlayers.clear();
	m_lastInboundSequenceByPlayer.clear();
}

} // namespace Netplay
} // namespace QGBA
