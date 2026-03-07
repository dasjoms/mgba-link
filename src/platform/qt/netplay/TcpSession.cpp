/* Copyright (c) 2013-2015 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "netplay/TcpSession.h"

#include "netplay/NetplayCodec.h"

#include <QDateTime>
#include <QDebug>
#include <QJsonDocument>
#include <QLoggingCategory>
#include <QRegularExpression>

#include <cmath>

namespace QGBA {
namespace Netplay {

namespace {

static const quint32 MAX_FRAME_SIZE = 8 * 1024 * 1024;


Q_LOGGING_CATEGORY(netplayTcpSessionLog, "netplay.tcpSession")

static QString _stateName(SessionState state) {
	switch (state) {
	case SessionState::Disconnected:
		return QStringLiteral("Disconnected");
	case SessionState::Connecting:
		return QStringLiteral("Connecting");
	case SessionState::Connected:
		return QStringLiteral("Connected");
	case SessionState::InRoom:
		return QStringLiteral("InRoom");
	case SessionState::Error:
		return QStringLiteral("Error");
	}
	return QStringLiteral("Unknown");
}

static bool _isSensitiveLogKey(const QString& key) {
	static const QRegularExpression tokenRegex(QStringLiteral("(auth|token|secret)"), QRegularExpression::CaseInsensitiveOption);
	return tokenRegex.match(key).hasMatch();
}

static QVariant _redactSensitiveVariant(const QVariant& value);

static QVariantMap _redactSensitiveMap(const QVariantMap& map) {
	QVariantMap redacted;
	for (auto it = map.constBegin(); it != map.constEnd(); ++it) {
		if (_isSensitiveLogKey(it.key())) {
			redacted.insert(it.key(), QStringLiteral("<redacted>"));
			continue;
		}
		redacted.insert(it.key(), _redactSensitiveVariant(it.value()));
	}
	return redacted;
}

static QVariant _redactSensitiveVariant(const QVariant& value) {
	if (value.canConvert<QVariantMap>()) {
		return _redactSensitiveMap(value.toMap());
	}
	if (value.canConvert<QVariantList>()) {
		QVariantList redactedList;
		for (const QVariant& element : value.toList()) {
			redactedList.append(_redactSensitiveVariant(element));
		}
		return redactedList;
	}
	return value;
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
	_wireAdapterCallbacks();
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
	_wireAdapterCallbacks();
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
	m_pendingHeartbeatSamples.clear();
	m_lastRttSampleMs = -1;
	m_rttJitterMs = 0.0;
	_resetSessionState();
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
		_dispatchProtocolError(1, QStringLiteral("Invalid endpoint"), NetplayErrorCategory::ConnectionFailure, -1, -1, details, QStringLiteral("out"), QStringLiteral("connect"));
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
	m_pendingHeartbeatSamples.clear();

	if (m_socket.state() != QAbstractSocket::UnconnectedState) {
		m_socket.disconnectFromHost();
		if (m_socket.state() != QAbstractSocket::UnconnectedState) {
			m_socket.abort();
		}
	}

	m_receiveBuffer.clear();
	m_handshakeCompleted = false;
	m_expectedServerSequence = -1;
	_resetSessionState();
	_setState(SessionState::Disconnected);
}

bool TcpSession::createRoom(const SessionCreateRoomRequest& request) {
	if (m_state != SessionState::Connected) {
		_dispatchProtocolError(10, QStringLiteral("createRoom requires Connected state"), NetplayErrorCategory::ProtocolMismatch, -1, -1, QVariantMap(), QStringLiteral("out"), QStringLiteral("createRoom"));
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
		_dispatchProtocolError(11, QStringLiteral("joinRoom requires Connected state"), NetplayErrorCategory::ProtocolMismatch, -1, -1, QVariantMap(), QStringLiteral("out"), QStringLiteral("joinRoom"));
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
	m_localPeer = SessionPeer();
	if (m_state != SessionState::Disconnected) {
		_setState(SessionState::Connected);
	}
}

bool TcpSession::sendEvent(const SessionEventEnvelope& event) {
	if (m_state != SessionState::InRoom || !_hasActiveRoom()) {
		_dispatchProtocolError(12, QStringLiteral("publishLinkEvent requires active room"), NetplayErrorCategory::ProtocolMismatch, -1, -1, QVariantMap(), QStringLiteral("out"), QStringLiteral("publishLinkEvent"));
		return false;
	}
	if (m_messageAdapter.localPlayerId() < 0) {
		_dispatchProtocolError(13, QStringLiteral("publishLinkEvent requires local player assignment"), NetplayErrorCategory::ProtocolMismatch, -1, -1, QVariantMap(), QStringLiteral("out"), QStringLiteral("publishLinkEvent"));
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
	m_pendingHeartbeatSamples.clear();
	m_receiveBuffer.clear();
	m_handshakeCompleted = false;
	m_expectedServerSequence = -1;
	_resetSessionState();
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
	_dispatchProtocolError(2, QStringLiteral("Socket error"), NetplayErrorCategory::ConnectionFailure, -1, -1, details, QStringLiteral("io"), QStringLiteral("socketError"));
	_setState(SessionState::Error);
}

void TcpSession::_sendHeartbeat() {
	if (m_socket.state() != QAbstractSocket::ConnectedState) {
		return;
	}

	QVariantMap intent;
	intent[QStringLiteral("intent")] = QStringLiteral("heartbeat");
	const qint64 heartbeatCounter = m_heartbeatCounter++;
	intent[QStringLiteral("heartbeatCounter")] = heartbeatCounter;
	m_pendingHeartbeatSamples.insert(heartbeatCounter, QDateTime::currentMSecsSinceEpoch());
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
	_dispatchProtocolError(3, QStringLiteral("Heartbeat timeout"), NetplayErrorCategory::HeartbeatTimeout, m_heartbeatCounter, -1, details, QStringLiteral("in"), QStringLiteral("heartbeatAck"));
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
		_dispatchProtocolError(4, QStringLiteral("Frame too large"), NetplayErrorCategory::MalformedMessage, -1, -1, QVariantMap(), QStringLiteral("out"), QStringLiteral("frame"));
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
			codecError.details,
			QStringLiteral("out"),
			wrappedIntent.value(QStringLiteral("intent")).toString());
		return false;
	}
	if (!_sendFrame(encoded)) {
		_dispatchProtocolError(16, QStringLiteral("Socket write failed"), NetplayErrorCategory::ConnectionFailure, wrappedIntent.value(QStringLiteral("clientSequence")).toLongLong(), -1, wrappedIntent, QStringLiteral("out"), wrappedIntent.value(QStringLiteral("intent")).toString());
		return false;
	}
	return true;
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
			_dispatchProtocolError(6, QStringLiteral("Invalid frame length"), NetplayErrorCategory::MalformedMessage, -1, -1, {{QStringLiteral("length"), static_cast<qint64>(length)}}, QStringLiteral("in"), QStringLiteral("frame"));
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
			decoded.error.details,
			QStringLiteral("in"),
			decoded.kind.isEmpty() ? QStringLiteral("unknown") : decoded.kind);
		return;
	}

	const QVariantMap event = decoded.payload;
	const QString kind = decoded.kind;
	qint64 serverSequence = -1;
	if (event.contains(QStringLiteral("serverSequence"))) {
		bool ok = false;
		serverSequence = event.value(QStringLiteral("serverSequence")).toLongLong(&ok);
		if (!ok || serverSequence < 0) {
			_dispatchProtocolError(14, QStringLiteral("Invalid server sequence"), NetplayErrorCategory::MalformedMessage, -1, m_expectedServerSequence, event, QStringLiteral("in"), kind);
			return;
		}
		if (m_expectedServerSequence >= 0 && serverSequence != m_expectedServerSequence) {
			_dispatchProtocolError(15, QStringLiteral("Unexpected server sequence"), NetplayErrorCategory::ProtocolMismatch, serverSequence, m_expectedServerSequence, event, QStringLiteral("in"), kind, serverSequence);
			return;
		}
		const qint64 nextExpectedServerSequence = serverSequence + 1;
		qCDebug(netplayTcpSessionLog).noquote()
			<< QStringLiteral("sequenceProgress roomId=%1 serverSequence=%2 nextExpectedServerSequence=%3 clientSequence=%4")
				.arg(m_room.roomId.isEmpty() ? QStringLiteral("n/a") : m_room.roomId)
				.arg(serverSequence)
				.arg(nextExpectedServerSequence)
				.arg(m_nextSequence);
		m_expectedServerSequence = nextExpectedServerSequence;
	}

	if (!m_handshakeCompleted && kind != QStringLiteral("roomJoined") && kind != QStringLiteral("playerAssigned")
			&& kind != QStringLiteral("heartbeatAck") && kind != QStringLiteral("error") && kind != QStringLiteral("disconnected")) {
		_dispatchProtocolError(18, QStringLiteral("Received non-handshake event before handshake completion"), NetplayErrorCategory::ProtocolMismatch, serverSequence, -1, event, QStringLiteral("in"), kind, serverSequence);
		return;
	}

	if (kind == QStringLiteral("heartbeatAck")) {
		m_lastInboundHeartbeatMs = QDateTime::currentMSecsSinceEpoch();
		const qint64 ackCounter = event.value(QStringLiteral("heartbeatCounter")).toLongLong();
		if (m_pendingHeartbeatSamples.contains(ackCounter)) {
			const qint64 sentAtMs = m_pendingHeartbeatSamples.take(ackCounter);
			const qint64 rttSampleMs = qMax<qint64>(0, m_lastInboundHeartbeatMs - sentAtMs);
			if (m_lastRttSampleMs >= 0) {
				const qreal delta = qAbs(static_cast<qreal>(rttSampleMs - m_lastRttSampleMs));
				m_rttJitterMs += (delta - m_rttJitterMs) / 16.0;
			}
			m_lastRttSampleMs = rttSampleMs;
			qCDebug(netplayTcpSessionLog).noquote()
				<< QStringLiteral("heartbeatHealth roomId=%1 heartbeatCounter=%2 rttMs=%3 jitterMs=%4")
					.arg(m_room.roomId.isEmpty() ? QStringLiteral("n/a") : m_room.roomId)
					.arg(ackCounter)
					.arg(rttSampleMs)
					.arg(QString::number(m_rttJitterMs, 'f', 2));
		}
		return;
	}

	_routeDecodedServerEvent(kind, event, serverSequence);
}

void TcpSession::_wireAdapterCallbacks() {
	SessionMessageAdapter::ControllerCallbacks adapterCallbacks;
	adapterCallbacks.onRoomJoined = [this](const ServerRoomJoinedEvent& event) {
		m_handshakeCompleted = true;
		m_room.roomId = event.roomId;
		m_room.roomName = event.roomName;
		m_room.maxPeers = event.maxPlayers;
		_setState(SessionState::InRoom);
	};
	adapterCallbacks.onPlayerAssigned = [this](const ServerPlayerAssignedEvent& event) {
		m_handshakeCompleted = true;
		m_localPeer.peerId = QString::number(event.playerId);
		m_localPeer.displayName.clear();
		m_localPeer.isLocal = true;
	};
	adapterCallbacks.onPeerJoined = [this](const ServerPeerJoinedEvent& event) {
		SessionPeer peer;
		peer.peerId = QString::number(event.playerId);
		peer.displayName = event.displayName;
		if (m_callbacks.onPeerJoined) {
			m_callbacks.onPeerJoined(peer);
		}
	};
	adapterCallbacks.onPeerLeft = [this](const ServerPeerLeftEvent& event) {
		SessionPeer peer;
		peer.peerId = QString::number(event.playerId);
		if (m_callbacks.onPeerLeft) {
			m_callbacks.onPeerLeft(peer);
		}
		if (peer.peerId == m_localPeer.peerId) {
			m_localPeer = SessionPeer();
		}
	};
	adapterCallbacks.onLinkEvent = [this](const ServerInboundLinkEvent& event) {
		SessionEventEnvelope envelope;
		envelope.roomId = event.event.roomId;
		envelope.sourcePeerId = QString::number(event.event.senderPlayerId);
		envelope.sequence = event.event.sequence;
		envelope.type = SessionEventType::LinkInput;
		envelope.payload = event.event.payload;
		envelope.sentAtUtc = QDateTime::fromMSecsSinceEpoch(event.event.tickMarker, Qt::UTC);
		if (m_callbacks.onInboundLinkEvent) {
			m_callbacks.onInboundLinkEvent(envelope);
		}
	};
	adapterCallbacks.onDisconnected = [this](const ServerDisconnectedEvent& event) {
		QVariantMap details;
		details[QStringLiteral("reason")] = static_cast<int>(event.reason);
		details[QStringLiteral("roomId")] = event.roomId;
		details[QStringLiteral("message")] = event.message;
		_dispatchProtocolError(8, event.message, NetplayErrorCategory::ConnectionFailure, -1, -1, details, QStringLiteral("in"), QStringLiteral("disconnected"));
		disconnect();
	};
	adapterCallbacks.onProtocolError = [this](const SessionProtocolError& error) {
		_dispatchProtocolError(error.code, error.message, error.category, error.sequence, error.expectedSequence, error.details, QStringLiteral("in"), QStringLiteral("adapter"));
	};
	m_messageAdapter.setControllerCallbacks(adapterCallbacks);
}

void TcpSession::_resetSessionState() {
	m_room = SessionRoom();
	m_localPeer = SessionPeer();
	m_messageAdapter.resetState();
}

void TcpSession::_routeDecodedServerEvent(const QString& kind, const QVariantMap& event, qint64 serverSequence) {
	if (kind == QStringLiteral("roomJoined")) {
		ServerRoomJoinedEvent roomJoined;
		roomJoined.roomId = event.value(QStringLiteral("roomId")).toString();
		roomJoined.roomName = event.value(QStringLiteral("roomName")).toString();
		roomJoined.maxPlayers = event.value(QStringLiteral("maxPlayers")).toInt();
		m_messageAdapter.handleServerEvent(roomJoined);
		return;
	}
	if (kind == QStringLiteral("playerAssigned")) {
		ServerPlayerAssignedEvent assigned;
		assigned.roomId = event.value(QStringLiteral("roomId")).toString();
		assigned.playerId = event.value(QStringLiteral("playerId")).toInt();
		m_messageAdapter.handleServerEvent(assigned);
		return;
	}
	if (kind == QStringLiteral("peerJoined")) {
		ServerPeerJoinedEvent joined;
		joined.roomId = event.value(QStringLiteral("roomId")).toString();
		joined.playerId = event.value(QStringLiteral("playerId")).toInt();
		joined.displayName = event.value(QStringLiteral("displayName")).toString();
		m_messageAdapter.handleServerEvent(joined);
		return;
	}
	if (kind == QStringLiteral("peerLeft")) {
		ServerPeerLeftEvent left;
		left.roomId = event.value(QStringLiteral("roomId")).toString();
		left.playerId = event.value(QStringLiteral("playerId")).toInt();
		m_messageAdapter.handleServerEvent(left);
		return;
	}
	if (kind == QStringLiteral("inboundLinkEvent")) {
		ServerInboundLinkEvent inbound;
		inbound.event.roomId = event.value(QStringLiteral("roomId")).toString();
		bool senderOk = false;
		inbound.event.senderPlayerId = event.value(QStringLiteral("sourcePeerId")).toString().toInt(&senderOk);
		if (!senderOk) {
			inbound.event.senderPlayerId = -1;
		}
		inbound.event.sequence = event.value(QStringLiteral("sequence")).toLongLong();
		inbound.event.tickMarker = event.value(QStringLiteral("sentAtUtcMs")).toLongLong();
		inbound.event.payload = event.value(QStringLiteral("payload")).toByteArray();
		m_messageAdapter.handleServerEvent(inbound);
		return;
	}
	if (kind == QStringLiteral("error")) {
		ServerErrorEvent e;
		e.roomId = event.value(QStringLiteral("roomId")).toString();
		e.code = event.value(QStringLiteral("code")).toInt();
		e.message = event.value(QStringLiteral("message")).toString();
		m_messageAdapter.handleServerEvent(e);
		_setState(SessionState::Error);
		return;
	}
	if (kind == QStringLiteral("disconnected")) {
		ServerDisconnectedEvent d;
		d.roomId = event.value(QStringLiteral("roomId")).toString();
		d.reason = _disconnectReasonFromString(event.value(QStringLiteral("reason")).toString());
		d.message = event.value(QStringLiteral("message")).toString();
		m_messageAdapter.handleServerEvent(d);
		return;
	}

	_dispatchProtocolError(9, QStringLiteral("Unknown server event kind"), NetplayErrorCategory::ProtocolMismatch, serverSequence, -1, event, QStringLiteral("in"), kind, serverSequence);
}

bool TcpSession::_hasActiveRoom() const {
	return !m_room.roomId.isEmpty();
}

void TcpSession::_dispatchProtocolError(int code, const QString& message, NetplayErrorCategory category, qint64 sequence, qint64 expectedSequence, const QVariantMap& details, const QString& direction, const QString& kind, qint64 serverSequence, const QString& playerId) {
	const QVariantMap redactedDetails = _redactSensitiveMap(details);
	const QString safeDirection = direction.isEmpty() ? QStringLiteral("n/a") : direction;
	const QString safeKind = kind.isEmpty() ? QStringLiteral("n/a") : kind;
	qCWarning(netplayTcpSessionLog).noquote()
		<< QStringLiteral("protocolViolation code=%1 reason=\"%2\" layer=%3 category=%4 direction=%5 kind=%6 roomId=%7 playerId=%8 sequence=%9 serverSequence=%10 expectedSequence=%11 state=%12 endpoint=%13 details=%14")
			.arg(code)
			.arg(message)
			.arg(QString::fromLatin1(netplayFailureLayerName(NetplayFailureLayer::TransportSession)))
			.arg(QString::fromLatin1(netplayErrorCategoryName(category)))
			.arg(safeDirection)
			.arg(safeKind)
			.arg(m_room.roomId.isEmpty() ? QStringLiteral("n/a") : m_room.roomId)
			.arg(playerId.isEmpty() ? QStringLiteral("n/a") : playerId)
			.arg(sequence >= 0 ? QString::number(sequence) : QStringLiteral("n/a"))
			.arg(serverSequence >= 0 ? QString::number(serverSequence) : QStringLiteral("n/a"))
			.arg(expectedSequence >= 0 ? QString::number(expectedSequence) : QStringLiteral("n/a"))
			.arg(_stateName(m_state))
			.arg(m_endpoint.isEmpty() ? QStringLiteral("n/a") : m_endpoint)
			.arg(QString::fromUtf8(QJsonDocument::fromVariant(redactedDetails).toJson(QJsonDocument::Compact)));

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
