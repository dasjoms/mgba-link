/* Copyright (c) 2013-2015 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "netplay/TcpSession.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>

namespace QGBA {
namespace Netplay {

namespace {

static const quint32 MAX_FRAME_SIZE = 8 * 1024 * 1024;

static QString _stringValue(const QVariantMap& map, const QString& key) {
	return map.value(key).toString();
}

static int _intValue(const QVariantMap& map, const QString& key, int fallback = 0) {
	bool ok = false;
	int value = map.value(key).toInt(&ok);
	return ok ? value : fallback;
}

static qint64 _longValue(const QVariantMap& map, const QString& key, qint64 fallback = 0) {
	bool ok = false;
	qint64 value = map.value(key).toLongLong(&ok);
	return ok ? value : fallback;
}

static QByteArray _byteArrayValue(const QVariantMap& map, const QString& key) {
	QByteArray raw = map.value(key).toByteArray();
	if (!raw.isEmpty()) {
		return QByteArray::fromBase64(raw);
	}
	return QByteArray();
}

static DisconnectReason _disconnectReasonFromString(const QString& reason) {
	if (reason == QStringLiteral("none")) {
		return DisconnectReason::None;
	}
	if (reason == QStringLiteral("clientRequested")) {
		return DisconnectReason::ClientRequested;
	}
	if (reason == QStringLiteral("serverShutdown")) {
		return DisconnectReason::ServerShutdown;
	}
	if (reason == QStringLiteral("networkTimeout")) {
		return DisconnectReason::NetworkTimeout;
	}
	if (reason == QStringLiteral("protocolError")) {
		return DisconnectReason::ProtocolError;
	}
	if (reason == QStringLiteral("roomClosed")) {
		return DisconnectReason::RoomClosed;
	}
	if (reason == QStringLiteral("kicked")) {
		return DisconnectReason::Kicked;
	}
	return DisconnectReason::Unknown;
}

} // namespace

TcpSession::TcpSession(QObject* parent)
	: QObject(parent) {
	m_heartbeatSendTimer.setSingleShot(false);
	m_heartbeatWatchdogTimer.setSingleShot(false);

	QObject::connect(&m_socket, &QTcpSocket::connected, this, &TcpSession::_onConnected);
	QObject::connect(&m_socket, &QTcpSocket::disconnected, this, &TcpSession::_onDisconnected);
	QObject::connect(&m_socket, &QTcpSocket::readyRead, this, &TcpSession::_onReadyRead);
	QObject::connect(&m_socket, qOverload<QAbstractSocket::SocketError>(&QTcpSocket::errorOccurred), this, &TcpSession::_onSocketError);
	QObject::connect(&m_heartbeatSendTimer, &QTimer::timeout, this, &TcpSession::_sendHeartbeat);
	QObject::connect(&m_heartbeatWatchdogTimer, &QTimer::timeout, this, &TcpSession::_checkHeartbeatTimeout);
}

TcpSession::~TcpSession() {
	disconnect();
}

SessionState TcpSession::state() const {
	return m_state;
}

SessionRoom TcpSession::room() const {
	return m_room;
}

SessionPeer TcpSession::localPeer() const {
	return m_localPeer;
}

void TcpSession::setCallbacks(SessionCallbacks callbacks) {
	m_callbacks = callbacks;
}

bool TcpSession::connect(const SessionConnectRequest& request) {
	if (m_state == SessionState::Connecting || m_state == SessionState::Connected || m_state == SessionState::InRoom) {
		disconnect();
	}

	m_host.clear();
	m_port = 0;
	m_authToken = request.authToken;
	m_nextSequence = 0;
	m_heartbeatCounter = 0;
	m_room = SessionRoom();
	m_localPeer = SessionPeer();
	m_receiveBuffer.clear();

	QString endpoint = request.endpoint.trimmed();
	if (endpoint.startsWith(QStringLiteral("tcp://"))) {
		endpoint.remove(0, 6);
	}

	int separator = endpoint.lastIndexOf(':');
	if (separator > 0 && separator < endpoint.size() - 1) {
		m_host = endpoint.left(separator);
		bool ok = false;
		int parsedPort = endpoint.mid(separator + 1).toInt(&ok);
		if (ok && parsedPort > 0 && parsedPort <= 65535) {
			m_port = static_cast<quint16>(parsedPort);
		}
	}

	if (m_host.isEmpty()) {
		m_host = request.options.value(QStringLiteral("host")).toString();
	}
	if (!m_port) {
		bool ok = false;
		int requestedPort = request.options.value(QStringLiteral("port")).toInt(&ok);
		if (ok && requestedPort > 0 && requestedPort <= 65535) {
			m_port = static_cast<quint16>(requestedPort);
		}
	}

	m_heartbeatIntervalMs = request.options.value(QStringLiteral("heartbeatIntervalMs"), 5000).toInt();
	m_heartbeatTimeoutMs = request.options.value(QStringLiteral("heartbeatTimeoutMs"), 15000).toInt();
	if (m_heartbeatIntervalMs <= 0) {
		m_heartbeatIntervalMs = 5000;
	}
	if (m_heartbeatTimeoutMs <= 0) {
		m_heartbeatTimeoutMs = 15000;
	}

	if (m_host.isEmpty() || !m_port) {
		_dispatchProtocolError(1, QStringLiteral("Invalid endpoint"), request.options);
		_setState(SessionState::Error);
		return false;
	}

	_setState(SessionState::Connecting);
	m_socket.connectToHost(m_host, m_port);
	return true;
}

void TcpSession::disconnect() {
	m_heartbeatSendTimer.stop();
	m_heartbeatWatchdogTimer.stop();

	if (m_socket.state() != QAbstractSocket::UnconnectedState) {
		m_socket.disconnectFromHost();
		if (m_socket.state() != QAbstractSocket::UnconnectedState) {
			m_socket.abort();
		}
	}

	m_receiveBuffer.clear();
	m_room = SessionRoom();
	m_localPeer = SessionPeer();
	_setState(SessionState::Disconnected);
}

bool TcpSession::createRoom(const SessionCreateRoomRequest& request) {
	QVariantMap intent;
	intent[QStringLiteral("intent")] = QStringLiteral("createRoom");
	intent[QStringLiteral("roomName")] = request.roomName;
	intent[QStringLiteral("maxPlayers")] = request.maxPeers;
	intent[QStringLiteral("options")] = request.options;
	return _sendIntent(intent);
}

bool TcpSession::joinRoom(const SessionJoinRoomRequest& request) {
	QVariantMap intent;
	intent[QStringLiteral("intent")] = QStringLiteral("joinRoom");
	intent[QStringLiteral("roomId")] = request.roomId;
	intent[QStringLiteral("options")] = request.options;
	return _sendIntent(intent);
}

void TcpSession::leaveRoom() {
	QVariantMap intent;
	intent[QStringLiteral("intent")] = QStringLiteral("leaveRoom");
	intent[QStringLiteral("roomId")] = m_room.roomId;
	_sendIntent(intent);
	m_room = SessionRoom();
	if (m_state != SessionState::Disconnected) {
		_setState(SessionState::Connected);
	}
}

bool TcpSession::sendEvent(const SessionEventEnvelope& event) {
	QVariantMap intent;
	intent[QStringLiteral("intent")] = QStringLiteral("publishLinkEvent");
	intent[QStringLiteral("eventId")] = event.eventId;
	intent[QStringLiteral("sourcePeerId")] = event.sourcePeerId;
	intent[QStringLiteral("sequence")] = event.sequence;
	intent[QStringLiteral("type")] = static_cast<int>(event.type);
	intent[QStringLiteral("payload")] = event.payload.toBase64();
	intent[QStringLiteral("sentAtUtcMs")] = event.sentAtUtc.toMSecsSinceEpoch();
	intent[QStringLiteral("metadata")] = event.metadata;
	return _sendIntent(intent);
}

void TcpSession::_onConnected() {
	m_lastInboundHeartbeatMs = QDateTime::currentMSecsSinceEpoch();
	m_heartbeatSendTimer.start(m_heartbeatIntervalMs);
	m_heartbeatWatchdogTimer.start(1000);
	_setState(SessionState::Connected);

	QVariantMap helloIntent;
	helloIntent[QStringLiteral("intent")] = QStringLiteral("hello");
	helloIntent[QStringLiteral("authToken")] = m_authToken;
	_sendIntent(helloIntent);
}

void TcpSession::_onDisconnected() {
	m_heartbeatSendTimer.stop();
	m_heartbeatWatchdogTimer.stop();
	m_receiveBuffer.clear();
	m_room = SessionRoom();
	m_localPeer = SessionPeer();
	if (m_state != SessionState::Error) {
		_setState(SessionState::Disconnected);
	}
}

void TcpSession::_onReadyRead() {
	m_receiveBuffer.append(m_socket.readAll());
	m_lastInboundHeartbeatMs = QDateTime::currentMSecsSinceEpoch();
	_drainFrames();
}

void TcpSession::_onSocketError(QAbstractSocket::SocketError error) {
	Q_UNUSED(error);
	QVariantMap details;
	details[QStringLiteral("socketError")] = static_cast<int>(m_socket.error());
	details[QStringLiteral("socketErrorString")] = m_socket.errorString();
	_dispatchProtocolError(2, QStringLiteral("Socket error"), details);
	_setState(SessionState::Error);
}

void TcpSession::_sendHeartbeat() {
	if (m_socket.state() != QAbstractSocket::ConnectedState) {
		return;
	}

	QVariantMap intent;
	intent[QStringLiteral("intent")] = QStringLiteral("heartbeat");
	intent[QStringLiteral("heartbeatCounter")] = m_heartbeatCounter++;
	_sendIntent(intent);
}

void TcpSession::_checkHeartbeatTimeout() {
	if (m_state == SessionState::Disconnected) {
		return;
	}

	qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
	if ((nowMs - m_lastInboundHeartbeatMs) <= m_heartbeatTimeoutMs) {
		return;
	}

	if (m_callbacks.onHeartbeatTimeout) {
		m_callbacks.onHeartbeatTimeout();
	}

	QVariantMap details;
	details[QStringLiteral("heartbeatTimeoutMs")] = m_heartbeatTimeoutMs;
	_dispatchProtocolError(3, QStringLiteral("Heartbeat timeout"), details);
	_setState(SessionState::Error);
	disconnect();
}

void TcpSession::_setState(SessionState state) {
	if (state == m_state) {
		return;
	}
	m_state = state;
	if (m_callbacks.onStateChanged) {
		m_callbacks.onStateChanged(m_state);
	}
}

bool TcpSession::_sendFrame(const QByteArray& payload) {
	if (m_socket.state() != QAbstractSocket::ConnectedState) {
		return false;
	}
	if (payload.size() < 0 || payload.size() > static_cast<int>(MAX_FRAME_SIZE)) {
		_dispatchProtocolError(4, QStringLiteral("Frame too large"));
		return false;
	}

	QByteArray frame;
	frame.resize(4);
	quint32 length = static_cast<quint32>(payload.size());
	frame[0] = static_cast<char>((length >> 24) & 0xFF);
	frame[1] = static_cast<char>((length >> 16) & 0xFF);
	frame[2] = static_cast<char>((length >> 8) & 0xFF);
	frame[3] = static_cast<char>(length & 0xFF);
	frame.append(payload);

	qint64 written = m_socket.write(frame);
	return written == frame.size();
}

bool TcpSession::_sendIntent(const QVariantMap& intent) {
	QVariantMap wrappedIntent = intent;
	wrappedIntent[QStringLiteral("clientSequence")] = m_nextSequence++;
	QJsonDocument document = QJsonDocument::fromVariant(wrappedIntent);
	if (!document.isObject()) {
		_dispatchProtocolError(5, QStringLiteral("Intent encoding failed"));
		return false;
	}
	return _sendFrame(document.toJson(QJsonDocument::Compact));
}

void TcpSession::_drainFrames() {
	for (;;) {
		if (m_receiveBuffer.size() < 4) {
			return;
		}

		const uchar* bytes = reinterpret_cast<const uchar*>(m_receiveBuffer.constData());
		quint32 length = (static_cast<quint32>(bytes[0]) << 24) |
			(static_cast<quint32>(bytes[1]) << 16) |
			(static_cast<quint32>(bytes[2]) << 8) |
			(static_cast<quint32>(bytes[3]));

		if (!length || length > MAX_FRAME_SIZE) {
			_dispatchProtocolError(6, QStringLiteral("Invalid frame length"));
			disconnect();
			return;
		}
		if (m_receiveBuffer.size() < static_cast<int>(4 + length)) {
			return;
		}

		QByteArray payload = m_receiveBuffer.mid(4, length);
		m_receiveBuffer.remove(0, 4 + length);
		_handleFrame(payload);
	}
}

void TcpSession::_handleFrame(const QByteArray& payload) {
	QJsonParseError parseError;
	QJsonDocument document = QJsonDocument::fromJson(payload, &parseError);
	if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
		_dispatchProtocolError(7, QStringLiteral("Invalid JSON frame"));
		return;
	}

	QVariantMap event = document.object().toVariantMap();
	QString kind = _stringValue(event, QStringLiteral("kind"));
	if (kind == QStringLiteral("roomJoined")) {
		m_room.roomId = _stringValue(event, QStringLiteral("roomId"));
		m_room.roomName = _stringValue(event, QStringLiteral("roomName"));
		m_room.maxPeers = _intValue(event, QStringLiteral("maxPlayers"));
		_setState(SessionState::InRoom);
		return;
	}
	if (kind == QStringLiteral("playerAssigned")) {
		m_localPeer.peerId = QString::number(_intValue(event, QStringLiteral("playerId"), -1));
		m_localPeer.displayName = _stringValue(event, QStringLiteral("displayName"));
		m_localPeer.isLocal = true;
		return;
	}
	if (kind == QStringLiteral("peerJoined")) {
		SessionPeer peer;
		peer.peerId = QString::number(_intValue(event, QStringLiteral("playerId"), -1));
		peer.displayName = _stringValue(event, QStringLiteral("displayName"));
		if (m_callbacks.onPeerJoined) {
			m_callbacks.onPeerJoined(peer);
		}
		return;
	}
	if (kind == QStringLiteral("peerLeft")) {
		SessionPeer peer;
		peer.peerId = QString::number(_intValue(event, QStringLiteral("playerId"), -1));
		peer.displayName = _stringValue(event, QStringLiteral("displayName"));
		if (m_callbacks.onPeerLeft) {
			m_callbacks.onPeerLeft(peer);
		}
		return;
	}
	if (kind == QStringLiteral("inboundLinkEvent")) {
		SessionEventEnvelope envelope;
		envelope.eventId = _stringValue(event, QStringLiteral("eventId"));
		envelope.sourcePeerId = _stringValue(event, QStringLiteral("sourcePeerId"));
		envelope.sequence = _longValue(event, QStringLiteral("sequence"));
		envelope.type = static_cast<SessionEventType>(_intValue(event, QStringLiteral("type"), static_cast<int>(SessionEventType::Custom)));
		envelope.payload = _byteArrayValue(event, QStringLiteral("payload"));
		envelope.sentAtUtc = QDateTime::fromMSecsSinceEpoch(_longValue(event, QStringLiteral("sentAtUtcMs")), Qt::UTC);
		envelope.metadata = event.value(QStringLiteral("metadata")).toMap();
		if (m_callbacks.onInboundLinkEvent) {
			m_callbacks.onInboundLinkEvent(envelope);
		}
		return;
	}
	if (kind == QStringLiteral("heartbeatAck")) {
		m_lastInboundHeartbeatMs = QDateTime::currentMSecsSinceEpoch();
		return;
	}
	if (kind == QStringLiteral("error")) {
		_dispatchProtocolError(_intValue(event, QStringLiteral("code")), _stringValue(event, QStringLiteral("message")), event);
		_setState(SessionState::Error);
		return;
	}
	if (kind == QStringLiteral("disconnected")) {
		QVariantMap details = event;
		details[QStringLiteral("reason")]
			= static_cast<int>(_disconnectReasonFromString(_stringValue(event, QStringLiteral("reason"))));
		_dispatchProtocolError(8, _stringValue(event, QStringLiteral("message")), details);
		disconnect();
		return;
	}

	_dispatchProtocolError(9, QStringLiteral("Unknown server event kind"), event);
}

void TcpSession::_dispatchProtocolError(int code, const QString& message, const QVariantMap& details) {
	if (!m_callbacks.onProtocolError) {
		return;
	}
	SessionProtocolError error;
	error.code = code;
	error.message = message;
	error.details = details;
	m_callbacks.onProtocolError(error);
}

} // namespace Netplay
} // namespace QGBA
