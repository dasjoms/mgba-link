/* Copyright (c) 2013-2015 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "platform/qt/netplay/DriverEventQueueBridge.h"

#include <QTest>

#include <mgba/internal/gba/sio/net.h>

using namespace QGBA::Netplay;

class NetplayDriverBridgeTest : public QObject {
Q_OBJECT

private slots:

	void outboundQueueReceivesDriverIntentEvents() {
		DriverEventQueueBridge bridge;
		GBASIONetDriver net;
		GBASIONetDriverCreate(&net);
		GBASIONetDriverSetQueues(&net, bridge.outboundQueue(), bridge.inboundQueue());
		net.state = GBA_SIO_NET_IN_ROOM;
		net.localPlayerId = 1;
		net.mode = GBA_SIO_NORMAL_8;

		net.d.setMode(&net.d, GBA_SIO_MULTI);
		QCOMPARE(bridge.pendingOutboundDepth(), size_t(1));
		QVERIFY(!net.d.start(&net.d));
		QCOMPARE(bridge.pendingOutboundDepth(), size_t(2));

		GBASIONetEvent first = {};
		GBASIONetEvent second = {};
		QVERIFY(bridge.tryDequeueOutbound(&first));
		QVERIFY(bridge.tryDequeueOutbound(&second));
		QCOMPARE(first.type, GBA_SIO_NET_EV_MODE_SET);
		QCOMPARE(first.modeSet.playerId, 1);
		QCOMPARE(first.modeSet.mode, GBA_SIO_MULTI);
		QCOMPARE(second.type, GBA_SIO_NET_EV_TRANSFER_START);
		QCOMPARE(second.transferStart.playerId, 1);
		QCOMPARE(second.transferStart.mode, GBA_SIO_MULTI);
		QVERIFY(second.sequence > first.sequence);
	}

	void injectedInboundTransferResultReachesDriverCompletionPath() {
		DriverEventQueueBridge bridge;
		GBASIONetDriver net;
		GBASIONetDriverCreate(&net);
		GBASIONetDriverSetQueues(&net, bridge.outboundQueue(), bridge.inboundQueue());
		net.state = GBA_SIO_NET_IN_ROOM;
		net.localPlayerId = 1;
		net.roomPlayerCount = 2;
		net.attachedPlayerMask = (1U << 1) | (1U << 2);
		net.mode = GBA_SIO_NORMAL_8;

		QVERIFY(!net.d.start(&net.d));
		QVERIFY(net.transferArmed);

		QVERIFY(bridge.enqueueTransferResult(2, 1, 7, 99, QByteArray::fromHex("5a")));
		QVERIFY(net.d.start(&net.d));
		QCOMPARE(net.d.finishNormal8(&net.d), static_cast<uint8_t>(0x5A));
	}

	void sessionFailureEventForcesDisconnectedFallbackPolicy() {
		DriverEventQueueBridge bridge;
		GBASIONetDriver net;
		GBASIONetDriverCreate(&net);
		GBASIONetDriverSetQueues(&net, bridge.outboundQueue(), bridge.inboundQueue());
		net.state = GBA_SIO_NET_IN_ROOM;
		net.localPlayerId = 1;
		net.mode = GBA_SIO_NORMAL_8;

		QVERIFY(!net.d.start(&net.d));
		QVERIFY(bridge.enqueueSessionFailure(GBA_SIO_NET_FAIL_DISCONNECTED, 0, 3));
		QVERIFY(!net.d.start(&net.d));
		QCOMPARE(net.state, GBA_SIO_NET_DISCONNECTED);
		QCOMPARE(net.d.finishNormal8(&net.d), static_cast<uint8_t>(0xFF));
	}

	void peerAttachDetachEventsUpdateTopology() {
		DriverEventQueueBridge bridge;
		GBASIONetDriver net;
		GBASIONetDriverCreate(&net);
		GBASIONetDriverSetQueues(&net, bridge.outboundQueue(), bridge.inboundQueue());
		net.state = GBA_SIO_NET_IN_ROOM;
		net.localPlayerId = 1;
		net.roomPlayerCount = 1;
		net.attachedPlayerMask = (1U << 1);
		net.mode = GBA_SIO_NORMAL_8;

		QVERIFY(bridge.enqueuePeerAttach(2, 1));
		QVERIFY(!net.d.start(&net.d));
		QCOMPARE(net.d.connectedDevices(&net.d), 1);

		QVERIFY(bridge.enqueuePeerDetach(2, 2));
		QVERIFY(!net.d.start(&net.d));
		QCOMPARE(net.d.connectedDevices(&net.d), 0);
	}

	void controlEventsAreAppliedBeforeQueuedTransferResults() {
		DriverEventQueueBridge bridge;
		GBASIONetDriver net;
		GBASIONetDriverCreate(&net);
		GBASIONetDriverSetQueues(&net, bridge.outboundQueue(), bridge.inboundQueue());
		net.state = GBA_SIO_NET_IN_ROOM;
		net.localPlayerId = 1;
		net.roomPlayerCount = 2;
		net.attachedPlayerMask = (1U << 1) | (1U << 2);
		net.mode = GBA_SIO_NORMAL_8;

		QVERIFY(bridge.enqueueTransferResult(2, 1, 9, 55, QByteArray::fromHex("66")));
		QVERIFY(bridge.enqueueSessionFailure(GBA_SIO_NET_FAIL_DISCONNECTED, 0, 10));

		QVERIFY(!net.d.start(&net.d));
		QCOMPARE(net.state, GBA_SIO_NET_DISCONNECTED);
		QCOMPARE(net.d.finishNormal8(&net.d), static_cast<uint8_t>(0xFF));
		QVERIFY(!net.d.start(&net.d));
	}

	void disconnectMidTransferForcesDeterministicFallback() {
		DriverEventQueueBridge bridge;
		GBASIONetDriver net;
		GBASIONetDriverCreate(&net);
		GBASIONetDriverSetQueues(&net, bridge.outboundQueue(), bridge.inboundQueue());
		net.state = GBA_SIO_NET_IN_ROOM;
		net.localPlayerId = 1;
		net.mode = GBA_SIO_NORMAL_32;

		QVERIFY(!net.d.start(&net.d));
		QVERIFY(net.transferArmed);

		QVERIFY(bridge.enqueueSessionFailure(GBA_SIO_NET_FAIL_DISCONNECTED, 17, 1));
		QVERIFY(!net.d.start(&net.d));
		QCOMPARE(net.lastFailureCode, 17);
		QCOMPARE(net.state, GBA_SIO_NET_DISCONNECTED);
		QCOMPARE(net.d.finishNormal32(&net.d), static_cast<uint32_t>(0xFFFFFFFF));
	}

	void saveLoadRoundTripPreservesDisconnectedTransferState() {
		GBASIONetDriver source;
		GBASIONetDriver restored;
		GBASIONetDriverCreate(&source);
		GBASIONetDriverCreate(&restored);

		source.state = GBA_SIO_NET_DISCONNECTED;
		source.mode = GBA_SIO_MULTI;
		source.localPlayerId = 2;
		source.roomPlayerCount = 3;
		source.attachedPlayerMask = 0x7;
		source.transferArmed = true;
		source.transferOrdinal = 9;
		source.committedTransferOrdinal = 9;
		source.nextOutboundSequence = 42;

		void* stateBlob = nullptr;
		size_t stateBlobSize = 0;
		source.d.saveState(&source.d, &stateBlob, &stateBlobSize);
		QVERIFY(stateBlob);
		QCOMPARE(stateBlobSize, size_t(0x40));

		QVERIFY(restored.d.loadState(&restored.d, stateBlob, stateBlobSize));
		free(stateBlob);

		QCOMPARE(restored.state, GBA_SIO_NET_DISCONNECTED);
		QCOMPARE(restored.mode, GBA_SIO_MULTI);
		QCOMPARE(restored.localPlayerId, 2);
		QCOMPARE(restored.roomPlayerCount, 3);
		QCOMPARE(restored.attachedPlayerMask, static_cast<uint8_t>(0x7));
		QCOMPARE(restored.transferOrdinal, static_cast<uint32_t>(9));
		QCOMPARE(restored.committedTransferOrdinal, static_cast<uint32_t>(9));
		QCOMPARE(restored.nextOutboundSequence, static_cast<int64_t>(42));
		QVERIFY(restored.committedTransferReady);
	}
};

QTEST_APPLESS_MAIN(NetplayDriverBridgeTest)
#include "netplay-driver-bridge.moc"
