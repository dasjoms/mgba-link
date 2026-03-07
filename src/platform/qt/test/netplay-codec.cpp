/* Copyright (c) 2013-2015 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "platform/qt/netplay/NetplayCodec.h"
#include "platform/qt/netplay/TcpSession.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>

using namespace QGBA::Netplay;

class NetplayCodecTest : public QObject {
Q_OBJECT

private slots:
	void happyPathEncodeDecodeCoreMessages() {
		CodecError encodeError;
		QVariantMap hello;
		hello[QStringLiteral("intent")] = QStringLiteral("hello");
		hello[QStringLiteral("protocolVersion")] = 1;
		QByteArray encodedHello = encodeFrame(hello, &encodeError);
		QVERIFY2(!encodedHello.isEmpty(), qPrintable(encodeError.message));
		QVERIFY(!encodeError);

		QJsonObject roomJoined;
		roomJoined.insert(QStringLiteral("kind"), QStringLiteral("roomJoined"));
		roomJoined.insert(QStringLiteral("roomId"), QStringLiteral("room-1"));
		roomJoined.insert(QStringLiteral("roomName"), QStringLiteral("Room One"));
		roomJoined.insert(QStringLiteral("maxPlayers"), 4);
		DecodedMessage decodedRoomJoined = decodeFrame(QJsonDocument(roomJoined).toJson(QJsonDocument::Compact));
		QVERIFY2(decodedRoomJoined.isValid(), qPrintable(decodedRoomJoined.error.message));
		QCOMPARE(decodedRoomJoined.kind, QStringLiteral("roomJoined"));

		QJsonObject inboundLinkEvent;
		inboundLinkEvent.insert(QStringLiteral("kind"), QStringLiteral("inboundLinkEvent"));
		inboundLinkEvent.insert(QStringLiteral("eventId"), QStringLiteral("evt-1"));
		inboundLinkEvent.insert(QStringLiteral("sourcePeerId"), QStringLiteral("peer-1"));
		inboundLinkEvent.insert(QStringLiteral("sequence"), 0);
		inboundLinkEvent.insert(QStringLiteral("type"), static_cast<int>(SessionEventType::LinkInput));
		inboundLinkEvent.insert(QStringLiteral("payload"), QStringLiteral("YWJj"));
		inboundLinkEvent.insert(QStringLiteral("sentAtUtcMs"), qint64(123));
		inboundLinkEvent.insert(QStringLiteral("metadata"), QJsonObject());
		DecodedMessage decodedInbound = decodeFrame(QJsonDocument(inboundLinkEvent).toJson(QJsonDocument::Compact));
		QVERIFY2(decodedInbound.isValid(), qPrintable(decodedInbound.error.message));
		QCOMPARE(decodedInbound.kind, QStringLiteral("inboundLinkEvent"));
		QCOMPARE(decodedInbound.payload.value(QStringLiteral("payload")).toByteArray(), QByteArray("abc"));

		QJsonObject disconnected;
		disconnected.insert(QStringLiteral("kind"), QStringLiteral("disconnected"));
		disconnected.insert(QStringLiteral("reason"), QStringLiteral("clientRequested"));
		disconnected.insert(QStringLiteral("message"), QStringLiteral("bye"));
		DecodedMessage decodedDisconnected = decodeFrame(QJsonDocument(disconnected).toJson(QJsonDocument::Compact));
		QVERIFY2(decodedDisconnected.isValid(), qPrintable(decodedDisconnected.error.message));
		QCOMPARE(decodedDisconnected.kind, QStringLiteral("disconnected"));
	}

	void requiredFieldValidationFailures() {
		DecodedMessage missingKind = decodeFrame(QByteArrayLiteral(R"({"roomId":"room-1"})"));
		QVERIFY(!missingKind.isValid());
		QCOMPARE(missingKind.error.code, 100);

		DecodedMessage wrongFieldType = decodeFrame(QByteArrayLiteral(R"({"kind":"roomJoined","roomId":"room-1","roomName":"Room","maxPlayers":"four"})"));
		QVERIFY(!wrongFieldType.isValid());
		QCOMPARE(wrongFieldType.error.code, 104);

		DecodedMessage absentSequence = decodeFrame(QByteArrayLiteral(R"({"kind":"inboundLinkEvent","eventId":"evt-1","sourcePeerId":"peer-1","type":0,"payload":"YQ==","sentAtUtcMs":7,"metadata":{}})"));
		QVERIFY(!absentSequence.isValid());
		QCOMPARE(absentSequence.error.code, 103);
	}

	void enforcesBoundsForPayloadAndPlayerIds() {
		CodecError oversizedPublishError;
		QVariantMap publishIntent;
		publishIntent[QStringLiteral("intent")] = QStringLiteral("publishLinkEvent");
		publishIntent[QStringLiteral("eventId")] = QStringLiteral("evt-1");
		publishIntent[QStringLiteral("sourcePeerId")] = QStringLiteral("peer-1");
		publishIntent[QStringLiteral("sequence")] = 0;
		publishIntent[QStringLiteral("type")] = static_cast<int>(SessionEventType::LinkInput);
		publishIntent[QStringLiteral("payload")] = QByteArray(NETPLAY_MAX_FRAME_PAYLOAD_BYTES + 1, 'x');
		publishIntent[QStringLiteral("sentAtUtcMs")] = qint64(9);
		publishIntent[QStringLiteral("metadata")] = QVariantMap();
		QByteArray oversizedPublish = encodeFrame(publishIntent, &oversizedPublishError);
		QVERIFY(oversizedPublish.isEmpty());
		QCOMPARE(oversizedPublishError.code, 113);

		QByteArray oversizedPayload = QByteArray(NETPLAY_MAX_FRAME_PAYLOAD_BYTES + 1, 'y').toBase64();
		QByteArray oversizedInbound = QByteArrayLiteral("{\"kind\":\"inboundLinkEvent\",\"eventId\":\"evt-2\",\"sourcePeerId\":\"peer-2\",\"sequence\":1,\"type\":0,\"payload\":\"") + oversizedPayload + QByteArrayLiteral("\",\"sentAtUtcMs\":7,\"metadata\":{}}");
		DecodedMessage oversizeInboundDecode = decodeFrame(oversizedInbound);
		QVERIFY(!oversizeInboundDecode.isValid());
		QCOMPARE(oversizeInboundDecode.error.code, 111);

		DecodedMessage invalidPlayerId = decodeFrame(QByteArrayLiteral(R"({"kind":"playerAssigned","playerId":256})"));
		QVERIFY(!invalidPlayerId.isValid());
		QCOMPARE(invalidPlayerId.error.code, 105);
	}

	void rejectsPublishBeforeRoomJoinInTcpSession() {
		TcpSession session;
		QList<SessionProtocolError> protocolErrors;
		SessionCallbacks callbacks;
		callbacks.onProtocolError = [&protocolErrors](const SessionProtocolError& error) {
			protocolErrors.append(error);
		};
		session.setCallbacks(callbacks);

		SessionEventEnvelope event;
		event.eventId = QStringLiteral("evt-1");
		event.sourcePeerId = QStringLiteral("peer-1");
		event.sequence = 0;
		event.type = SessionEventType::LinkInput;
		event.payload = QByteArrayLiteral("abc");
		event.sentAtUtc = QDateTime::fromMSecsSinceEpoch(42, Qt::UTC);

		QVERIFY(!session.sendEvent(event));
		QCOMPARE(protocolErrors.size(), 1);
		QCOMPARE(protocolErrors.front().code, 12);
	}

	void compatibilityChecks() {
		DecodedMessage withUnknownOptional = decodeFrame(QByteArrayLiteral(R"({"kind":"roomJoined","roomId":"room-1","roomName":"Room","maxPlayers":2,"futureOptional":"ok"})"));
		QVERIFY(withUnknownOptional.isValid());

		DecodedMessage unknownRequiredKind = decodeFrame(QByteArrayLiteral(R"({"kind":"futureRequiredKind","roomId":"room-1"})"));
		QVERIFY(!unknownRequiredKind.isValid());
		QCOMPARE(unknownRequiredKind.error.code, 201);
	}
};

QTEST_APPLESS_MAIN(NetplayCodecTest)
#include "netplay-codec.moc"
