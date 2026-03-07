/* Copyright (c) 2013-2015 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "netplay/TcpSession.h"

#include "netplay/NetplayCodec.h"

#include <QDateTime>
#include <QDebug>

namespace QGBA {
namespace Netplay {

namespace {

static const quint32 MAX_FRAME_SIZE = 8 * 1024 * 1024;

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
	m_endpoint = request.endpoint.trimmed();
	m_authToken = request.authToken;
	m_protocolVersion = request.options.value(QStringLiteral("protocolVersion"), 1).toInt();
	if (m_protocolVersion <= 0) {
		m_protocolVersion = 1;
	}
	m_clientName = request.options.value(QStringLiteral("clientName")).toString().trimmed();
	m_buildTag = request.options.value(QStringLiteral("buildTag")).toString().trimmed();
	m_handshakeCompleted = false;
	m_expectedServerSequence = -1;
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
		QVariantMap details = request.options;
		details[QStringLiteral("endpoint")] = request.endpoint;
		_dispatchProtocolError(1, QStringLiteral("Invalid endpoint"), NetplayErrorCategory::ConnectionFailure, -1, -1, details);
		_setState(SessionState::Error);
		return false;
	}

	_setState(SessionState::Connecting);
	m_endpoint = QStringLiteral("tcp://%1:%2").arg(m_host).arg(m_port);
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
	m_handshakeCompleted = false;
	m_expectedServerSequence = -1;
	_setState(SessionState::Disconnected);
}

bool TcpSession::createRoom(const SessionCreateRoomRequest& request) {
	if (m_state != SessionState::Connected) {
		_dispatchProtocolError(10, QStringLiteral("createRoom requires Connected state"), NetplayErrorCategory::ProtocolMismatch);
		return false;
	}

	QVariantMap intent;
	intent[QStringLiteral("intent")] = QStringLiteral("createRoom");
	intent[QStringLiteral("roomName")] = request.roomName;
	intent[QStringLiteral("maxPlayers")] = request.maxPeers;
	intent[QStringLiteral("options")] = request.options;
	return _sendIntent(intent);
}

bool TcpSession::joinRoom(const SessionJoinRoomRequest& request) {
	if (m_state != SessionState::Connected) {
		_dispatchProtocolError(11, QStringLiteral("joinRoom requires Connected state"), NetplayErrorCategory::ProtocolMismatch);
		return false;
	}

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
	if (m_state != SessionState::InRoom || !_hasActiveRoom()) {
		_dispatchProtocolError(12, QStringLiteral("publishLinkEvent requires active room"), NetplayErrorCategory::ProtocolMismatch);
		return false;
	}
	if (m_localPeer.peerId.isEmpty()) {
		_dispatchProtocolError(13, QStringLiteral("publishLinkEvent requires local player assignment"), NetplayErrorCategory::ProtocolMismatch);
		return false;
	}

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
	helloIntent[QStringLiteral("protocolVersion")] = m_protocolVersion;
	helloIntent[QStringLiteral("authToken")] = m_authToken;
	QVariantMap metadata;
	if (!m_clientName.isEmpty()) {
		metadata[QStringLiteral("clientName")] = m_clientName;
	}
	if (!m_buildTag.isEmpty()) {
		metadata[QStringLiteral("buildTag")] = m_buildTag;
	}
	if (!metadata.isEmpty()) {
		helloIntent[QStringLiteral("metadata")] = metadata;
	}
	_sendIntent(helloIntent);
}

void TcpSession::_onDisconnected() {
	m_heartbeatSendTimer.stop();
	m_heartbeatWatchdogTimer.stop();
	m_receiveBuffer.clear();
	m_room = SessionRoom();
	m_localPeer = SessionPeer();
	m_handshakeCompleted = false;
	m_expectedServerSequence = -1;
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
	_dispatchProtocolError(2, QStringLiteral("Socket error"), NetplayErrorCategory::ConnectionFailure, -1, -1, details);
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
	_dispatchProtocolError(3, QStringLiteral("Heartbeat timeout"), NetplayErrorCategory::HeartbeatTimeout, m_heartbeatCounter, -1, details);
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
		_dispatchProtocolError(4, QStringLiteral("Frame too large"), NetplayErrorCategory::MalformedMessage);
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
	CodecError codecError;
	QByteArray encoded = encodeFrame(wrappedIntent, &codecError);
	if (encoded.isEmpty()) {
		_dispatchProtocolError(codecError.code ? codecError.code : 5,
			codecError.message.isEmpty() ? QStringLiteral("Intent encoding failed") : codecError.message,
			codecError.category,
			m_nextSequence - 1,
			-1,
			codecError.details);
		return false;
	}
	return _sendFrame(encoded);
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
			_dispatchProtocolError(6, QStringLiteral("Invalid frame length"), NetplayErrorCategory::MalformedMessage);
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
	DecodedMessage decoded = decodeFrame(payload);
	if (!decoded.isValid()) {
		_dispatchProtocolError(decoded.error.code ? decoded.error.code : 7,
			decoded.error.message.isEmpty() ? QStringLiteral("Invalid JSON frame") : decoded.error.message,
			decoded.error.category,
			-1,
			-1,
			decoded.error.details);
		return;
	}

	QVariantMap event = decoded.payload;
	QString kind = decoded.kind;
	qint64 serverSequence = -1;
	if (event.contains(QStringLiteral("serverSequence"))) {
		bool ok = false;
		serverSequence = event.value(QStringLiteral("serverSequence")).toLongLong(&ok);
		if (!ok || serverSequence < 0) {
			_dispatchProtocolError(14, QStringLiteral("Invalid server sequence"), NetplayErrorCategory::MalformedMessage, -1, m_expectedServerSequence, event);
			return;
		}
		if (m_expectedServerSequence >= 0 && serverSequence != m_expectedServerSequence) {
			_dispatchProtocolError(15, QStringLiteral("Unexpected server sequence"), NetplayErrorCategory::ProtocolMismatch, serverSequence, m_expectedServerSequence, event);
			return;
		}
		m_expectedServerSequence = serverSequence + 1;
	}

	if (!m_handshakeCompleted && kind != QStringLiteral("roomJoined") && kind != QStringLiteral("playerAssigned")
			&& kind != QStringLiteral("heartbeatAck") && kind != QStringLiteral("error") && kind != QStringLiteral("disconnected")) {
		_dispatchProtocolError(18, QStringLiteral("Received non-handshake event before handshake completion"), NetplayErrorCategory::ProtocolMismatch, serverSequence, -1, event);
		return;
	}

	if (kind == QStringLiteral("roomJoined")) {
		m_handshakeCompleted = true;
		m_room.roomId = event.value(QStringLiteral("roomId")).toString();
		m_room.roomName = event.value(QStringLiteral("roomName")).toString();
		m_room.maxPeers = event.value(QStringLiteral("maxPlayers")).toInt();
		_setState(SessionState::InRoom);
		return;
	}
	if (kind == QStringLiteral("playerAssigned")) {
		m_handshakeCompleted = true;
		m_localPeer.peerId = QString::number(event.value(QStringLiteral("playerId")).toInt());
		m_localPeer.displayName = event.value(QStringLiteral("displayName")).toString();
		m_localPeer.isLocal = true;
		return;
	}
	if (kind == QStringLiteral("peerJoined")) {
		if (!_hasActiveRoom()) {
			_dispatchProtocolError(19, QStringLiteral("peerJoined without active room"), NetplayErrorCategory::ProtocolMismatch, serverSequence, -1, event);
			return;
		}
		SessionPeer peer;
		peer.peerId = QString::number(event.value(QStringLiteral("playerId")).toInt());
		peer.displayName = event.value(QStringLiteral("displayName")).toString();
		if (m_callbacks.onPeerJoined) {
			m_callbacks.onPeerJoined(peer);
		}
		return;
	}
	if (kind == QStringLiteral("peerLeft")) {
		SessionPeer peer;
		peer.peerId = QString::number(event.value(QStringLiteral("playerId")).toInt());
		peer.displayName = event.value(QStringLiteral("displayName")).toString();
		if (m_callbacks.onPeerLeft) {
			m_callbacks.onPeerLeft(peer);
		}
		return;
	}
	if (kind == QStringLiteral("inboundLinkEvent")) {
		if (!_hasActiveRoom()) {
			_dispatchProtocolError(20, QStringLiteral("inboundLinkEvent without active room"), NetplayErrorCategory::ProtocolMismatch, serverSequence, -1, event);
			return;
		}
		SessionEventEnvelope envelope;
		envelope.eventId = event.value(QStringLiteral("eventId")).toString();
		envelope.sourcePeerId = event.value(QStringLiteral("sourcePeerId")).toString();
		envelope.sequence = event.value(QStringLiteral("sequence")).toLongLong();
		envelope.type = static_cast<SessionEventType>(event.value(QStringLiteral("type")).toInt());
		envelope.payload = event.value(QStringLiteral("payload")).toByteArray();
		envelope.sentAtUtc = QDateTime::fromMSecsSinceEpoch(event.value(QStringLiteral("sentAtUtcMs")).toLongLong(), Qt::UTC);
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
		const int code = event.value(QStringLiteral("code")).toInt();
		const QString message = event.value(QStringLiteral("message")).toString();
		const QString lowerMessage = message.toLower();
		NetplayErrorCategory category = NetplayErrorCategory::ProtocolMismatch;
		if (code == 403 || code == 409 || code == 429
				|| lowerMessage.contains(QStringLiteral("full"))
				|| lowerMessage.contains(QStringLiteral("reject"))
				|| lowerMessage.contains(QStringLiteral("denied"))) {
			category = NetplayErrorCategory::RoomRejectedOrFull;
		}
		_dispatchProtocolError(code, message, category, serverSequence, -1, event);
		_setState(SessionState::Error);
		return;
	}
	if (kind == QStringLiteral("disconnected")) {
		QVariantMap details = event;
		details[QStringLiteral("reason")]
			= static_cast<int>(_disconnectReasonFromString(event.value(QStringLiteral("reason")).toString()));
		_dispatchProtocolError(8, event.value(QStringLiteral("message")).toString(), NetplayErrorCategory::ConnectionFailure, serverSequence, -1, details);
		disconnect();
		return;
	}

	_dispatchProtocolError(9, QStringLiteral("Unknown server event kind"), NetplayErrorCategory::ProtocolMismatch, serverSequence, -1, event);
}

bool TcpSession::_hasActiveRoom() const {
	return !m_room.roomId.isEmpty();
}

void TcpSession::_dispatchProtocolError(int code, const QString& message, NetplayErrorCategory category, qint64 sequence, qint64 expectedSequence, const QVariantMap& details) {
	qWarning().noquote()
		<< QStringLiteral("[netplay][layer=%1][category=%2] transport/session error code=%3 message=\"%4\" endpoint=%5 room=%6 sequence=%7 expectedSequence=%8")
			.arg(QString::fromLatin1(netplayFailureLayerName(NetplayFailureLayer::TransportSession)))
			.arg(QString::fromLatin1(netplayErrorCategoryName(category)))
			.arg(code)
			.arg(message)
			.arg(m_endpoint.isEmpty() ? QStringLiteral("n/a") : m_endpoint)
			.arg(m_room.roomId.isEmpty() ? QStringLiteral("n/a") : m_room.roomId)
			.arg(sequence >= 0 ? QString::number(sequence) : QStringLiteral("n/a"))
			.arg(expectedSequence >= 0 ? QString::number(expectedSequence) : QStringLiteral("n/a"));

	if (!m_callbacks.onProtocolError) {
		return;
	}
	SessionProtocolError error;
	error.code = code;
	error.message = message;
	error.category = category;
	error.layer = NetplayFailureLayer::TransportSession;
	error.endpoint = m_endpoint;
	error.roomId = m_room.roomId;
	error.sequence = sequence;
	error.expectedSequence = expectedSequence;
	error.details = details;
	m_callbacks.onProtocolError(error);
}

} // namespace Netplay
} // namespace QGBA
