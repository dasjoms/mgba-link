/* Copyright (c) 2013-2015 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "platform/qt/netplay/SessionMessageAdapter.h"

#include <QTest>

using namespace QGBA::Netplay;

class SessionMessageAdapterTest : public QObject {
Q_OBJECT

private slots:
	void createsIntentsFromControllerActions() {
		SessionMessageAdapter adapter;

		const ClientCreateRoomIntent createIntent = adapter.hostRoom(QStringLiteral("Room"), 4);
		QCOMPARE(createIntent.roomName, QStringLiteral("Room"));
		QCOMPARE(createIntent.maxPlayers, 4);

		const ClientJoinRoomIntent joinIntent = adapter.joinRoom(QStringLiteral("room-1"));
		QCOMPARE(joinIntent.roomId, QStringLiteral("room-1"));

		const ClientLeaveRoomIntent leaveIntent = adapter.leaveRoom(QStringLiteral("room-1"));
		QCOMPARE(leaveIntent.roomId, QStringLiteral("room-1"));
	}

	void validatesUnknownSenderDuplicateAndOutOfOrderSequence() {
		SessionMessageAdapter adapter;
		QList<SessionProtocolError> protocolErrors;
		int linkEvents = 0;
		SessionMessageAdapter::ControllerCallbacks callbacks;
		callbacks.onProtocolError = [&protocolErrors](const SessionProtocolError& error) {
			protocolErrors.append(error);
		};
		callbacks.onLinkEvent = [&linkEvents](const ServerInboundLinkEvent&) {
			++linkEvents;
		};
		adapter.setControllerCallbacks(callbacks);

		ServerRoomJoinedEvent joined;
		joined.roomId = QStringLiteral("room-1");
		adapter.handleServerEvent(joined);

		ServerPlayerAssignedEvent assigned;
		assigned.roomId = QStringLiteral("room-1");
		assigned.playerId = 1;
		adapter.handleServerEvent(assigned);

		ServerInboundLinkEvent inbound;
		inbound.event.senderPlayerId = 2;
		inbound.event.sequence = 0;
		adapter.handleServerEvent(inbound);
		QCOMPARE(linkEvents, 0);
		QCOMPARE(protocolErrors.size(), 1);
		QCOMPARE(protocolErrors.front().code, 104);

		ServerPeerJoinedEvent peerJoined;
		peerJoined.roomId = QStringLiteral("room-1");
		peerJoined.playerId = 2;
		adapter.handleServerEvent(peerJoined);

		inbound.event.roomId = QStringLiteral("room-1");
		inbound.event.tickMarker = 10;

		adapter.handleServerEvent(inbound);
		QCOMPARE(linkEvents, 1);

		adapter.handleServerEvent(inbound);
		QCOMPARE(linkEvents, 1);
		QCOMPARE(protocolErrors.size(), 2);
		QCOMPARE(protocolErrors.back().code, 114);

		inbound.event.sequence = -1;
		adapter.handleServerEvent(inbound);
		QCOMPARE(linkEvents, 1);
		QCOMPARE(protocolErrors.size(), 3);
		QCOMPARE(protocolErrors.back().code, 105);
	}


	void validatesRoomMismatchInvalidIdsAndMalformedTick() {
		SessionMessageAdapter adapter;
		QList<SessionProtocolError> protocolErrors;
		int joinedEvents = 0;
		int leftEvents = 0;
		int disconnectedEvents = 0;
		SessionMessageAdapter::ControllerCallbacks callbacks;
		callbacks.onProtocolError = [&protocolErrors](const SessionProtocolError& error) {
			protocolErrors.append(error);
		};
		callbacks.onPeerJoined = [&joinedEvents](const ServerPeerJoinedEvent&) {
			++joinedEvents;
		};
		callbacks.onPeerLeft = [&leftEvents](const ServerPeerLeftEvent&) {
			++leftEvents;
		};
		callbacks.onDisconnected = [&disconnectedEvents](const ServerDisconnectedEvent&) {
			++disconnectedEvents;
		};
		adapter.setControllerCallbacks(callbacks);

		ServerRoomJoinedEvent roomJoined;
		roomJoined.roomId = QStringLiteral("room-1");
		adapter.handleServerEvent(roomJoined);

		ServerPeerJoinedEvent peerJoined;
		peerJoined.roomId = QStringLiteral("room-2");
		peerJoined.playerId = 3;
		adapter.handleServerEvent(peerJoined);
		QCOMPARE(joinedEvents, 0);
		QCOMPARE(protocolErrors.back().code, 108);

		peerJoined.roomId = QStringLiteral("room-1");
		peerJoined.playerId = -3;
		adapter.handleServerEvent(peerJoined);
		QCOMPARE(protocolErrors.back().code, 102);

		peerJoined.playerId = 3;
		adapter.handleServerEvent(peerJoined);
		QCOMPARE(joinedEvents, 1);

		ServerInboundLinkEvent inbound;
		inbound.event.roomId = QStringLiteral("room-2");
		inbound.event.senderPlayerId = 3;
		inbound.event.sequence = 0;
		inbound.event.tickMarker = 1;
		adapter.handleServerEvent(inbound);
		QCOMPARE(protocolErrors.back().code, 111);

		inbound.event.roomId = QStringLiteral("room-1");
		inbound.event.senderPlayerId = -1;
		adapter.handleServerEvent(inbound);
		QCOMPARE(protocolErrors.back().code, 112);

		inbound.event.senderPlayerId = 3;
		inbound.event.tickMarker = -1;
		adapter.handleServerEvent(inbound);
		QCOMPARE(protocolErrors.back().code, 113);

		ServerPeerLeftEvent left;
		left.roomId = QStringLiteral("room-1");
		left.playerId = -1;
		adapter.handleServerEvent(left);
		QCOMPARE(leftEvents, 0);
		QCOMPARE(protocolErrors.back().code, 110);

		ServerDisconnectedEvent disconnected;
		disconnected.roomId = QStringLiteral("room-2");
		adapter.handleServerEvent(disconnected);
		QCOMPARE(disconnectedEvents, 0);
		QCOMPARE(protocolErrors.back().code, 115);
	}

	void tracksLocalPlayerForBroadcast() {
		SessionMessageAdapter adapter;

		ServerRoomJoinedEvent joined;
		joined.roomId = QStringLiteral("room-1");
		adapter.handleServerEvent(joined);

		ServerPlayerAssignedEvent assigned;
		assigned.roomId = QStringLiteral("room-1");
		assigned.playerId = 7;
		adapter.handleServerEvent(assigned);

		ClientPublishLinkEventIntent first = adapter.broadcastLinkEvent(99, QByteArray("abc"));
		QCOMPARE(first.event.senderPlayerId, 7);
		QCOMPARE(first.event.sequence, 0);
		QCOMPARE(first.event.tickMarker, qint64(99));
		QCOMPARE(first.event.payload, QByteArray("abc"));

		ClientPublishLinkEventIntent second = adapter.broadcastLinkEvent(100, QByteArray("def"));
		QCOMPARE(second.event.sequence, 1);
	}
};

QTEST_APPLESS_MAIN(SessionMessageAdapterTest)
#include "session-message-adapter.moc"
