/* Copyright (c) 2013-2015 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#pragma once

#include <functional>

#include "netplay/SessionTypes.h"

namespace QGBA {
namespace Netplay {

struct SessionConnectRequest {
	QString endpoint;
	QString authToken;
	QVariantMap options;
};

struct SessionCreateRoomRequest {
	QString roomName;
	int maxPeers = 2;
	QVariantMap options;
};

struct SessionJoinRoomRequest {
	QString roomId;
	QVariantMap options;
};

// Callback hooks exposed by the abstract netplay session layer.
//
// This keeps transport/session behavior decoupled from emulator details and is
// intended for MultiplayerController integration.
struct SessionCallbacks {
	std::function<void(SessionState)> onStateChanged;
	std::function<void(const SessionPeer&)> onPeerJoined;
	std::function<void(const SessionPeer&)> onPeerLeft;
	std::function<void(const SessionEventEnvelope&)> onInboundLinkEvent;
	std::function<void()> onHeartbeatTimeout;
	std::function<void(const SessionProtocolError&)> onProtocolError;
};

class Session {
public:
	virtual ~Session() = default;

	virtual SessionState state() const = 0;
	virtual SessionRoom room() const = 0;
	virtual SessionPeer localPeer() const = 0;

	virtual void setCallbacks(SessionCallbacks callbacks) = 0;

	virtual bool connect(const SessionConnectRequest& request) = 0;
	virtual void disconnect() = 0;

	virtual bool createRoom(const SessionCreateRoomRequest& request) = 0;
	virtual bool joinRoom(const SessionJoinRoomRequest& request) = 0;
	virtual void leaveRoom() = 0;

	virtual bool sendEvent(const SessionEventEnvelope& event) = 0;
};

} // namespace Netplay
} // namespace QGBA
