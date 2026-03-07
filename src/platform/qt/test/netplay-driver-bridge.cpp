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
	void injectedInboundTransferResultReachesDriverCompletionPath() {
		DriverEventQueueBridge bridge;
		GBASIONetDriver net;
		GBASIONetDriverCreate(&net);
		GBASIONetDriverSetQueues(&net, nullptr, bridge.inboundQueue());
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

	void peerAttachDetachEventsUpdateTopology() {
		DriverEventQueueBridge bridge;
		GBASIONetDriver net;
		GBASIONetDriverCreate(&net);
		GBASIONetDriverSetQueues(&net, nullptr, bridge.inboundQueue());
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
};

QTEST_APPLESS_MAIN(NetplayDriverBridgeTest)
#include "netplay-driver-bridge.moc"
