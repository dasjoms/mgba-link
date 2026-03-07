/* Copyright (c) 2013-2015 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#pragma once

#include <QObject>
#include <QByteArray>
#include <QTimer>
#include <QTcpSocket>

#include "netplay/Session.h"

namespace QGBA {
namespace Netplay {

class TcpSession : public QObject, public Session {
	Q_OBJECT

public:
	explicit TcpSession(QObject* parent = nullptr);
	~TcpSession() override;

	SessionState state() const override;
	SessionRoom room() const override;
	SessionPeer localPeer() const override;

	void setCallbacks(SessionCallbacks callbacks) override;

	bool connect(const SessionConnectRequest& request) override;
	void disconnect() override;

	bool createRoom(const SessionCreateRoomRequest& request) override;
	bool joinRoom(const SessionJoinRoomRequest& request) override;
	void leaveRoom() override;

	bool sendEvent(const SessionEventEnvelope& event) override;

private slots:
	void _onConnected();
	void _onDisconnected();
	void _onReadyRead();
	void _onSocketError(QAbstractSocket::SocketError error);
	void _sendHeartbeat();
	void _checkHeartbeatTimeout();

private:
	void _setState(SessionState state);
	bool _sendFrame(const QByteArray& payload);
	bool _sendIntent(const QVariantMap& intent);
	void _drainFrames();
	void _handleFrame(const QByteArray& payload);
	bool _hasActiveRoom() const;
	void _dispatchProtocolError(int code, const QString& message,
		NetplayErrorCategory category = NetplayErrorCategory::ProtocolMismatch,
		qint64 sequence = -1,
		qint64 expectedSequence = -1,
		const QVariantMap& details = QVariantMap(),
		const QString& direction = QString(),
		const QString& kind = QString(),
		qint64 serverSequence = -1,
		const QString& playerId = QString());

	QTcpSocket m_socket;
	QByteArray m_receiveBuffer;
	SessionCallbacks m_callbacks;
	SessionState m_state = SessionState::Disconnected;
	SessionRoom m_room;
	SessionPeer m_localPeer;

	QString m_host;
	QString m_endpoint;
	quint16 m_port = 0;
	QString m_authToken;
	QString m_clientName;
	QString m_buildTag;
	int m_protocolVersion = 1;
	bool m_handshakeCompleted = false;
	qint64 m_expectedServerSequence = -1;
	qint64 m_nextSequence = 0;
	qint64 m_heartbeatCounter = 0;
	qint64 m_lastInboundHeartbeatMs = 0;
	int m_heartbeatIntervalMs = 5000;
	int m_heartbeatTimeoutMs = 15000;
	QTimer m_heartbeatSendTimer;
	QTimer m_heartbeatWatchdogTimer;
};

} // namespace Netplay
} // namespace QGBA
