/* Copyright (c) 2013-2015 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#pragma once

#include <functional>

#include <QByteArray>
#include <QHash>
#include <QSet>

#include "netplay/SessionTypes.h"

namespace QGBA {
namespace Netplay {

class SessionMessageAdapter {
public:
	struct ControllerCallbacks {
		std::function<void(const ServerRoomJoinedEvent&)> onRoomJoined;
		std::function<void(const ServerPlayerAssignedEvent&)> onPlayerAssigned;
		std::function<void(const ServerPeerJoinedEvent&)> onPeerJoined;
		std::function<void(const ServerPeerLeftEvent&)> onPeerLeft;
		std::function<void(const ServerInboundLinkEvent&)> onLinkEvent;
		std::function<void(const ServerDisconnectedEvent&)> onDisconnected;
		std::function<void(const SessionProtocolError&)> onProtocolError;
	};

	SessionMessageAdapter();

	void setControllerCallbacks(ControllerCallbacks callbacks);

	ClientCreateRoomIntent hostRoom(const QString& roomName, int maxPlayers) const;
	ClientJoinRoomIntent joinRoom(const QString& roomId) const;
	ClientLeaveRoomIntent leaveRoom(const QString& roomId) const;
	ClientPublishLinkEventIntent broadcastLinkEvent(qint64 tickMarker, const QByteArray& payload);

	void handleServerEvent(const ServerRoomJoinedEvent& event);
	void handleServerEvent(const ServerPlayerAssignedEvent& event);
	void handleServerEvent(const ServerPeerJoinedEvent& event);
	void handleServerEvent(const ServerPeerLeftEvent& event);
	void handleServerEvent(const ServerInboundLinkEvent& event);
	void handleServerEvent(const ServerDisconnectedEvent& event);
	void handleServerEvent(const ServerErrorEvent& event);

	int localPlayerId() const;

private:
	void _reportProtocolError(int code, const QString& message, const QVariantMap& details = QVariantMap()) const;
	void _resetRoomState();

	ControllerCallbacks m_callbacks;
	qint64 m_nextOutboundSequence = 0;
	int m_localPlayerId = -1;
	QSet<int> m_knownPlayers;
	QHash<int, qint64> m_lastInboundSequenceByPlayer;
};

} // namespace Netplay
} // namespace QGBA
