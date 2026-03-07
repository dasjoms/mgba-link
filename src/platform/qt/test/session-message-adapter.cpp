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

	void validatesUnknownSenderAndOutOfOrderSequence() {
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

		ServerPlayerAssignedEvent assigned;
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
		peerJoined.playerId = 2;
		adapter.handleServerEvent(peerJoined);

		adapter.handleServerEvent(inbound);
		QCOMPARE(linkEvents, 1);

		adapter.handleServerEvent(inbound);
		QCOMPARE(linkEvents, 1);
		QCOMPARE(protocolErrors.size(), 2);
		QCOMPARE(protocolErrors.back().code, 105);
	}

	void tracksLocalPlayerForBroadcast() {
		SessionMessageAdapter adapter;

		ServerPlayerAssignedEvent assigned;
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
