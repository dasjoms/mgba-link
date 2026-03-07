/* Copyright (c) 2013-2015 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "netplay/NetplayCodec.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QMetaType>
#include <QRegularExpression>
#include <QVariant>

#include <limits>

namespace QGBA {
namespace Netplay {
namespace {

static const int MAX_WIRE_TEXT = 256;


Q_LOGGING_CATEGORY(netplayCodecLog, "netplay.codec")

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

static void _logCodecProtocolViolation(const CodecError& error, const QString& direction, const QString& kind, qint64 sequence, qint64 serverSequence, const QVariantMap& details = QVariantMap()) {
	QVariantMap merged = _redactSensitiveMap(details);
	for (auto it = error.details.constBegin(); it != error.details.constEnd(); ++it) {
		merged.insert(it.key(), _redactSensitiveVariant(it.value()));
	}
	qCWarning(netplayCodecLog).noquote()
		<< QStringLiteral("protocolViolation code=%1 reason=\"%2\" layer=Codec category=%3 direction=%4 kind=%5 roomId=n/a playerId=n/a sequence=%6 serverSequence=%7 state=n/a details=%8")
			.arg(error.code)
			.arg(error.message)
			.arg(QString::fromLatin1(netplayErrorCategoryName(error.category)))
			.arg(direction)
			.arg(kind.isEmpty() ? QStringLiteral("n/a") : kind)
			.arg(sequence >= 0 ? QString::number(sequence) : QStringLiteral("n/a"))
			.arg(serverSequence >= 0 ? QString::number(serverSequence) : QStringLiteral("n/a"))
			.arg(QString::fromUtf8(QJsonDocument::fromVariant(merged).toJson(QJsonDocument::Compact)));
}


static bool _isStringVariant(const QVariant& value) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
	return value.metaType().id() == QMetaType::QString;
#else
	return value.type() == QVariant::String;
#endif
}

static bool _isByteArrayVariant(const QVariant& value) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
	return value.metaType().id() == QMetaType::QByteArray;
#else
	return value.type() == QVariant::ByteArray;
#endif
}


static bool _isNumericVariant(const QVariant& value) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
	const int typeId = value.metaType().id();
#else
	const int typeId = value.type();
#endif
	return typeId == QMetaType::Int || typeId == QMetaType::UInt || typeId == QMetaType::LongLong || typeId == QMetaType::ULongLong
		|| typeId == QMetaType::Short || typeId == QMetaType::UShort || typeId == QMetaType::Char || typeId == QMetaType::SChar
		|| typeId == QMetaType::UChar;
}

static CodecError _error(int code, const QString& message, const QVariantMap& details = QVariantMap(), NetplayErrorCategory category = NetplayErrorCategory::MalformedMessage) {
	CodecError error;
	error.code = code;
	error.message = message;
	error.details = details;
	error.category = category;
	return error;
}

static bool _requireString(const QVariantMap& map, const QString& key, QString* out, CodecError* error, bool allowEmpty = false, int maxLength = MAX_WIRE_TEXT) {
	if (!map.contains(key) || !_isStringVariant(map.value(key))) {
		*error = _error(100, QStringLiteral("Invalid or missing string field"), {{QStringLiteral("field"), key}});
		return false;
	}
	const QString value = map.value(key).toString();
	if (!allowEmpty && value.isEmpty()) {
		*error = _error(101, QStringLiteral("Required string field is empty"), {{QStringLiteral("field"), key}});
		return false;
	}
	if (value.size() > maxLength) {
		*error = _error(102, QStringLiteral("String field exceeds bounds"), {{QStringLiteral("field"), key}, {QStringLiteral("maxLength"), maxLength}});
		return false;
	}
	*out = value;
	return true;
}

static bool _requireInt(const QVariantMap& map, const QString& key, qint64 minValue, qint64 maxValue, qint64* out, CodecError* error) {
	if (!map.contains(key)) {
		*error = _error(103, QStringLiteral("Missing numeric field"), {{QStringLiteral("field"), key}});
		return false;
	}
	if (!_isNumericVariant(map.value(key))) {
		*error = _error(104, QStringLiteral("Numeric field has invalid type"), {{QStringLiteral("field"), key}});
		return false;
	}
	bool ok = false;
	qint64 value = map.value(key).toLongLong(&ok);
	if (!ok) {
		*error = _error(116, QStringLiteral("Numeric field conversion failed"), {{QStringLiteral("field"), key}});
		return false;
	}
	if (value < minValue || value > maxValue) {
		*error = _error(105, QStringLiteral("Numeric field out of bounds"), {{QStringLiteral("field"), key}, {QStringLiteral("min"), minValue}, {QStringLiteral("max"), maxValue}, {QStringLiteral("value"), value}});
		return false;
	}
	*out = value;
	return true;
}

static bool _requireMap(const QVariantMap& map, const QString& key, QVariantMap* out, CodecError* error, bool required = false) {
	if (!map.contains(key)) {
		if (required) {
			*error = _error(106, QStringLiteral("Missing map field"), {{QStringLiteral("field"), key}});
			return false;
		}
		*out = QVariantMap();
		return true;
	}
	if (!map.value(key).canConvert<QVariantMap>()) {
		*error = _error(107, QStringLiteral("Map field has invalid type"), {{QStringLiteral("field"), key}});
		return false;
	}
	*out = map.value(key).toMap();
	return true;
}

static bool _isCanonicalBase64(const QByteArray& encoded) {
	if (encoded.size() % 4 != 0) {
		return false;
	}
	for (char ch : encoded) {
		if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '+' || ch == '/' || ch == '=') {
			continue;
		}
		return false;
	}
	return true;
}

static bool _decodeBase64Canonical(const QVariant& field, const QString& key, QByteArray* out, CodecError* error, int maxBytes = NETPLAY_MAX_FRAME_PAYLOAD_BYTES) {
	QByteArray encoded;
	if (_isByteArrayVariant(field)) {
		encoded = field.toByteArray();
	} else if (_isStringVariant(field)) {
		encoded = field.toString().toLatin1();
	} else {
		*error = _error(108, QStringLiteral("Binary field must be base64 text or byte array"), {{QStringLiteral("field"), key}});
		return false;
	}
	if (!_isCanonicalBase64(encoded)) {
		*error = _error(109, QStringLiteral("Binary field is not canonical base64"), {{QStringLiteral("field"), key}});
		return false;
	}
	QByteArray decoded = QByteArray::fromBase64(encoded);
	if (decoded.toBase64() != encoded) {
		*error = _error(110, QStringLiteral("Binary field failed deterministic base64 normalization"), {{QStringLiteral("field"), key}});
		return false;
	}
	if (decoded.size() > maxBytes) {
		*error = _error(111, QStringLiteral("Binary field exceeds bounds"), {{QStringLiteral("field"), key}, {QStringLiteral("maxBytes"), maxBytes}, {QStringLiteral("size"), decoded.size()}});
		return false;
	}
	*out = decoded;
	return true;
}

static bool _encodeBinaryField(QVariantMap* map, const QString& key, CodecError* error, int maxBytes = NETPLAY_MAX_FRAME_PAYLOAD_BYTES) {
	if (!map->contains(key)) {
		*error = _error(112, QStringLiteral("Missing binary field"), {{QStringLiteral("field"), key}});
		return false;
	}
	const QVariant value = map->value(key);
	if (_isByteArrayVariant(value)) {
		QByteArray bytes = value.toByteArray();
		if (bytes.size() > maxBytes) {
			*error = _error(113, QStringLiteral("Binary field exceeds bounds"), {{QStringLiteral("field"), key}, {QStringLiteral("maxBytes"), maxBytes}, {QStringLiteral("size"), bytes.size()}});
			return false;
		}
		map->insert(key, QString::fromLatin1(bytes.toBase64()));
		return true;
	}
	QByteArray bytes;
	if (!_decodeBase64Canonical(value, key, &bytes, error, maxBytes)) {
		return false;
	}
	map->insert(key, QString::fromLatin1(bytes.toBase64()));
	return true;
}

static CodecError _validateOutbound(const QVariantMap& payload) {
	QString intent;
	CodecError error;
	if (!_requireString(payload, QStringLiteral("intent"), &intent, &error)) {
		return error;
	}

	if (payload.contains(QStringLiteral("clientSequence"))) {
		qint64 sequence = -1;
		if (!_requireInt(payload, QStringLiteral("clientSequence"), 0, std::numeric_limits<qint64>::max(), &sequence, &error)) {
			return error;
		}
	}

	if (intent == QStringLiteral(CLIENT_INTENT_CREATE_ROOM)) {
		QString roomName;
		qint64 maxPlayers = 0;
		if (!_requireString(payload, QStringLiteral("roomName"), &roomName, &error) || !_requireInt(payload, QStringLiteral("maxPlayers"), 1, 255, &maxPlayers, &error)) {
			return error;
		}
	} else if (intent == QStringLiteral(CLIENT_INTENT_JOIN_ROOM) || intent == QStringLiteral(CLIENT_INTENT_LEAVE_ROOM)) {
		QString roomId;
		if (!_requireString(payload, QStringLiteral("roomId"), &roomId, &error)) {
			return error;
		}
	} else if (intent == QStringLiteral(CLIENT_INTENT_HEARTBEAT)) {
		qint64 counter = -1;
		if (!_requireInt(payload, QStringLiteral("heartbeatCounter"), 0, std::numeric_limits<qint64>::max(), &counter, &error)) {
			return error;
		}
	} else if (intent == QStringLiteral(CLIENT_INTENT_PUBLISH_LINK_EVENT)) {
		QString eventId;
		QString sourcePeerId;
		qint64 sequence = -1;
		qint64 type = -1;
		qint64 sentAtUtcMs = -1;
		if (!_requireString(payload, QStringLiteral("eventId"), &eventId, &error)
				|| !_requireString(payload, QStringLiteral("sourcePeerId"), &sourcePeerId, &error)
				|| !_requireInt(payload, QStringLiteral("sequence"), 0, std::numeric_limits<qint64>::max(), &sequence, &error)
				|| !_requireInt(payload, QStringLiteral("type"), static_cast<int>(SessionEventType::LinkInput), static_cast<int>(SessionEventType::Custom), &type, &error)
				|| !_requireInt(payload, QStringLiteral("sentAtUtcMs"), 0, std::numeric_limits<qint64>::max(), &sentAtUtcMs, &error)) {
			return error;
		}
		QVariantMap metadata;
		if (!_requireMap(payload, QStringLiteral("metadata"), &metadata, &error, false)) {
			return error;
		}
	} else if (intent != QStringLiteral(CLIENT_INTENT_HELLO)) {
		return _error(115, QStringLiteral("Unknown client intent kind"), {{QStringLiteral("intent"), intent}}, NetplayErrorCategory::ProtocolMismatch);
	}

	return CodecError();
}

static CodecError _validateRoomJoined(const QVariantMap& message) {
	CodecError error;
	QString roomId;
	QString roomName;
	qint64 maxPlayers = 0;
	if (!_requireString(message, QStringLiteral("roomId"), &roomId, &error)
			|| !_requireString(message, QStringLiteral("roomName"), &roomName, &error)
			|| !_requireInt(message, QStringLiteral("maxPlayers"), 1, 255, &maxPlayers, &error)) {
		return error;
	}
	return CodecError();
}

static CodecError _validatePlayerAssigned(const QVariantMap& message) {
	CodecError error;
	qint64 playerId = -1;
	if (!_requireInt(message, QStringLiteral("playerId"), 0, 255, &playerId, &error)) {
		return error;
	}
	return CodecError();
}

static CodecError _validatePeerJoined(const QVariantMap& message) {
	CodecError error;
	qint64 playerId = -1;
	if (!_requireInt(message, QStringLiteral("playerId"), 0, 255, &playerId, &error)) {
		return error;
	}
	if (message.contains(QStringLiteral("displayName"))) {
		QString displayName;
		if (!_requireString(message, QStringLiteral("displayName"), &displayName, &error, true)) {
			return error;
		}
	}
	return CodecError();
}

static CodecError _validatePeerLeft(const QVariantMap& message) {
	return _validatePeerJoined(message);
}

static CodecError _validateInboundLinkEvent(QVariantMap* message) {
	CodecError error;
	QString eventId;
	QString sourcePeerId;
	qint64 sequence = -1;
	qint64 type = -1;
	qint64 sentAtUtcMs = -1;
	QVariantMap metadata;
	if (!_requireString(*message, QStringLiteral("eventId"), &eventId, &error)
			|| !_requireString(*message, QStringLiteral("sourcePeerId"), &sourcePeerId, &error)
			|| !_requireInt(*message, QStringLiteral("sequence"), 0, std::numeric_limits<qint64>::max(), &sequence, &error)
			|| !_requireInt(*message, QStringLiteral("type"), static_cast<int>(SessionEventType::LinkInput), static_cast<int>(SessionEventType::Custom), &type, &error)
			|| !_requireInt(*message, QStringLiteral("sentAtUtcMs"), 0, std::numeric_limits<qint64>::max(), &sentAtUtcMs, &error)
			|| !_requireMap(*message, QStringLiteral("metadata"), &metadata, &error, false)) {
		return error;
	}

	QByteArray decodedPayload;
	if (!_decodeBase64Canonical(message->value(QStringLiteral("payload")), QStringLiteral("payload"), &decodedPayload, &error)) {
		return error;
	}
	message->insert(QStringLiteral("payload"), decodedPayload);
	return CodecError();
}

static CodecError _validateHeartbeatAck(const QVariantMap& message) {
	CodecError error;
	qint64 counter = -1;
	if (!_requireInt(message, QStringLiteral("heartbeatCounter"), 0, std::numeric_limits<qint64>::max(), &counter, &error)) {
		return error;
	}
	return CodecError();
}

static CodecError _validateError(const QVariantMap& message) {
	CodecError error;
	qint64 code = 0;
	QString text;
	if (!_requireInt(message, QStringLiteral("code"), 0, std::numeric_limits<int>::max(), &code, &error)
			|| !_requireString(message, QStringLiteral("message"), &text, &error, true, 512)) {
		return error;
	}
	return CodecError();
}

static CodecError _validateDisconnected(const QVariantMap& message) {
	CodecError error;
	QString reason;
	if (!_requireString(message, QStringLiteral("reason"), &reason, &error)) {
		return error;
	}
	if (message.contains(QStringLiteral("message"))) {
		QString text;
		if (!_requireString(message, QStringLiteral("message"), &text, &error, true, 512)) {
			return error;
		}
	}
	return CodecError();
}

} // namespace

QByteArray encodeFrame(const QVariantMap& payload, CodecError* error) {
	CodecError validationError = _validateOutbound(payload);
	if (validationError) {
		const qint64 sequence = payload.value(QStringLiteral("clientSequence")).toLongLong();
		_logCodecProtocolViolation(validationError, QStringLiteral("out"), payload.value(QStringLiteral("intent")).toString(), sequence, -1, payload);
		if (error) {
			*error = validationError;
		}
		return QByteArray();
	}

	QVariantMap frame = payload;
	if (frame.value(QStringLiteral("intent")).toString() == QStringLiteral(CLIENT_INTENT_PUBLISH_LINK_EVENT)) {
		CodecError binaryError;
		if (!_encodeBinaryField(&frame, QStringLiteral("payload"), &binaryError)) {
			const qint64 sequence = frame.value(QStringLiteral("clientSequence")).toLongLong();
			_logCodecProtocolViolation(binaryError, QStringLiteral("out"), frame.value(QStringLiteral("intent")).toString(), sequence, -1, frame);
			if (error) {
				*error = binaryError;
			}
			return QByteArray();
		}
	}

	QJsonDocument document = QJsonDocument::fromVariant(frame);
	if (!document.isObject()) {
		CodecError objectError = _error(114, QStringLiteral("Intent encoding failed"), QVariantMap(), NetplayErrorCategory::MalformedMessage);
		const qint64 sequence = frame.value(QStringLiteral("clientSequence")).toLongLong();
		_logCodecProtocolViolation(objectError, QStringLiteral("out"), frame.value(QStringLiteral("intent")).toString(), sequence, -1, frame);
		if (error) {
			*error = objectError;
		}
		return QByteArray();
	}
	if (error) {
		*error = CodecError();
	}
	return document.toJson(QJsonDocument::Compact);
}

DecodedMessage decodeFrame(const QByteArray& payload) {
	DecodedMessage decoded;

	QJsonParseError parseError;
	QJsonDocument document = QJsonDocument::fromJson(payload, &parseError);
	if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
		decoded.error = _error(200, QStringLiteral("Invalid JSON frame"), {
			{QStringLiteral("offset"), parseError.offset},
			{QStringLiteral("jsonError"), parseError.errorString()}
		});
		_logCodecProtocolViolation(decoded.error, QStringLiteral("in"), QStringLiteral("unknown"), -1, -1);
		return decoded;
	}

	QVariantMap message = document.object().toVariantMap();
	CodecError error;
	if (!_requireString(message, QStringLiteral("kind"), &decoded.kind, &error)) {
		decoded.error = error;
		_logCodecProtocolViolation(decoded.error, QStringLiteral("in"), QStringLiteral("unknown"), -1, -1, message);
		return decoded;
	}

	if (decoded.kind == QStringLiteral(SERVER_EVENT_ROOM_JOINED)) {
		error = _validateRoomJoined(message);
	} else if (decoded.kind == QStringLiteral(SERVER_EVENT_PLAYER_ASSIGNED)) {
		error = _validatePlayerAssigned(message);
	} else if (decoded.kind == QStringLiteral(SERVER_EVENT_PEER_JOINED)) {
		error = _validatePeerJoined(message);
	} else if (decoded.kind == QStringLiteral(SERVER_EVENT_PEER_LEFT)) {
		error = _validatePeerLeft(message);
	} else if (decoded.kind == QStringLiteral(SERVER_EVENT_INBOUND_LINK_EVENT)) {
		error = _validateInboundLinkEvent(&message);
	} else if (decoded.kind == QStringLiteral(SERVER_EVENT_HEARTBEAT_ACK)) {
		error = _validateHeartbeatAck(message);
	} else if (decoded.kind == QStringLiteral(SERVER_EVENT_ERROR)) {
		error = _validateError(message);
	} else if (decoded.kind == QStringLiteral(SERVER_EVENT_DISCONNECTED)) {
		error = _validateDisconnected(message);
	} else {
		error = _error(201, QStringLiteral("Unknown server event kind"), {{QStringLiteral("kind"), decoded.kind}}, NetplayErrorCategory::ProtocolMismatch);
	}

	if (error) {
		decoded.error = error;
		qint64 serverSequence = -1;
		if (message.contains(QStringLiteral("serverSequence"))) {
			bool ok = false;
			const qint64 parsedServerSequence = message.value(QStringLiteral("serverSequence")).toLongLong(&ok);
			if (ok) {
				serverSequence = parsedServerSequence;
			}
		}
		_logCodecProtocolViolation(decoded.error, QStringLiteral("in"), decoded.kind, -1, serverSequence, message);
		return decoded;
	}

	decoded.payload = message;
	return decoded;
}

} // namespace Netplay
} // namespace QGBA
