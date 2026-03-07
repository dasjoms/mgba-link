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

// Envelope for synchronized link/session events.
//
// The payload is opaque to this API. Higher-level systems can serialize any
// protocol-specific data and pass it through this envelope.
struct SessionEventEnvelope {
	QString eventId;
	QString sourcePeerId;
	qint64 sequence = 0;
	SessionEventType type = SessionEventType::Custom;
	QByteArray payload;
	QDateTime sentAtUtc;
	QVariantMap metadata;
};

struct SessionProtocolError {
	int code = 0;
	QString message;
	QVariantMap details;
};

} // namespace Netplay
} // namespace QGBA
