/* Copyright (c) 2013-2024 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "util/test/suite.h"

#include <mgba/internal/gba/sio/net.h>

#define TEST_QUEUE_CAP 32

struct TestQueue {
	struct GBASIONetEventQueue queue;
	struct GBASIONetEvent events[TEST_QUEUE_CAP];
	size_t size;
};

static bool _push(struct GBASIONetEventQueue* queue, const struct GBASIONetEvent* event) {
	struct TestQueue* test = queue->context;
	if (test->size >= TEST_QUEUE_CAP) {
		return false;
	}
	test->events[test->size++] = *event;
	return true;
}

static bool _tryPop(struct GBASIONetEventQueue* queue, struct GBASIONetEvent* outEvent) {
	struct TestQueue* test = queue->context;
	if (!test->size) {
		return false;
	}
	*outEvent = test->events[0];
	for (size_t i = 1; i < test->size; ++i) {
		test->events[i - 1] = test->events[i];
	}
	--test->size;
	return true;
}

static bool _waitPop(struct GBASIONetEventQueue* queue, struct GBASIONetEvent* outEvent, int32_t timeoutMs) {
	UNUSED(timeoutMs);
	return _tryPop(queue, outEvent);
}

static size_t _size(const struct GBASIONetEventQueue* queue) {
	const struct TestQueue* test = queue->context;
	return test->size;
}

static void _wake(struct GBASIONetEventQueue* queue) {
	UNUSED(queue);
}

static const struct GBASIONetEventQueueVTable TEST_QUEUE_VTABLE = {
	.push = _push,
	.tryPop = _tryPop,
	.waitPop = _waitPop,
	.size = _size,
	.wake = _wake,
};

static void _initQueue(struct TestQueue* queue) {
	memset(queue, 0, sizeof(*queue));
	queue->queue.context = queue;
	queue->queue.vtable = &TEST_QUEUE_VTABLE;
}

static struct GBASIONetEvent _resultEvent(int playerId, const uint8_t* payload, size_t payloadSize) {
	struct GBASIONetEvent event = {
		.type = GBA_SIO_NET_EV_TRANSFER_RESULT,
		.senderPlayerId = playerId,
		.sequence = 1,
		.transferResult = {
			.playerId = playerId,
			.tickMarker = 0,
			.payload = payload,
			.payloadSize = payloadSize,
		},
	};
	return event;
}

M_TEST_DEFINE(setModeEnqueuesIntent) {
	struct GBASIONetDriver net;
	struct TestQueue outbound;
	struct TestQueue inbound;
	_initQueue(&outbound);
	_initQueue(&inbound);
	GBASIONetDriverCreate(&net);
	GBASIONetDriverSetQueues(&net, &outbound.queue, &inbound.queue);
	net.localPlayerId = 2;

	net.d.setMode(&net.d, GBA_SIO_NORMAL_8);
	assert_int_equal(outbound.size, 1);
	assert_int_equal(outbound.events[0].type, GBA_SIO_NET_EV_MODE_SET);
	assert_int_equal(outbound.events[0].modeSet.playerId, 2);
	assert_int_equal(outbound.events[0].modeSet.mode, GBA_SIO_NORMAL_8);
}

M_TEST_DEFINE(startStallsUntilInboundResultThenFinishConsumes) {
	struct GBASIONetDriver net;
	struct TestQueue outbound;
	struct TestQueue inbound;
	uint8_t payload[1] = { 0x5A };
	_initQueue(&outbound);
	_initQueue(&inbound);
	GBASIONetDriverCreate(&net);
	GBASIONetDriverSetQueues(&net, &outbound.queue, &inbound.queue);
	net.localPlayerId = 1;
	net.state = GBA_SIO_NET_IN_ROOM;
	net.mode = GBA_SIO_NORMAL_8;

	assert_false(net.d.start(&net.d));
	assert_true(net.transferArmed);
	assert_int_equal(net.state, GBA_SIO_NET_ACTIVE_TRANSFER);
	assert_int_equal(outbound.size, 1);
	assert_int_equal(outbound.events[0].type, GBA_SIO_NET_EV_TRANSFER_START);

	struct GBASIONetEvent event = _resultEvent(1, payload, sizeof(payload));
	inbound.queue.vtable->push(&inbound.queue, &event);
	assert_true(net.d.start(&net.d));
	assert_int_equal(net.transferOrdinal, 1);
	assert_int_equal(net.d.finishNormal8(&net.d), 0x5A);
	assert_false(net.transferArmed);
	assert_int_equal(net.state, GBA_SIO_NET_IN_ROOM);
}

M_TEST_DEFINE(finishMissingCommittedPayloadTriggersDeterministicSentinelAndError) {
	struct GBASIONetDriver net;
	GBASIONetDriverCreate(&net);
	net.mode = GBA_SIO_NORMAL_32;
	net.state = GBA_SIO_NET_ACTIVE_TRANSFER;
	net.transferArmed = true;
	net.transferOrdinal = 7;
	net.normalData32 = 0xFFFFFFFF;

	assert_int_equal(net.d.finishNormal32(&net.d), 0xFFFFFFFF);
	assert_false(net.protocolError);
	assert_int_equal(net.state, GBA_SIO_NET_IN_ROOM);
}

M_TEST_DEFINE(reentrantStartWithoutCompletionPredictablyStalls) {
	struct GBASIONetDriver net;
	GBASIONetDriverCreate(&net);
	net.state = GBA_SIO_NET_IN_ROOM;
	net.mode = GBA_SIO_NORMAL_8;
	assert_false(net.d.start(&net.d));
	assert_true(net.transferArmed);
	assert_false(net.d.start(&net.d));
	assert_true(net.transferArmed);
	assert_false(net.protocolError);
}


M_TEST_DEFINE(peerAttachDetachUpdatesTopologyAndDeviceView) {
	struct GBASIONetDriver net;
	struct TestQueue inbound;
	_initQueue(&inbound);
	GBASIONetDriverCreate(&net);
	GBASIONetDriverSetQueues(&net, NULL, &inbound.queue);
	net.state = GBA_SIO_NET_IN_ROOM;
	net.localPlayerId = 1;
	net.attachedPlayerMask = (1U << 1);
	net.roomPlayerCount = 1;
	net.mode = GBA_SIO_NORMAL_8;

	struct GBASIONetEvent attach = {
		.type = GBA_SIO_NET_EV_PEER_ATTACH,
		.peerAttach = { .playerId = 2 },
	};
	assert_true(inbound.queue.vtable->push(&inbound.queue, &attach));
	assert_false(net.d.start(&net.d));
	assert_int_equal(net.d.connectedDevices(&net.d), 1);
	assert_int_equal(net.d.deviceId(&net.d), 1);

	uint8_t payload[1] = { 0x11 };
	struct GBASIONetEvent result = _resultEvent(1, payload, sizeof(payload));
	assert_true(inbound.queue.vtable->push(&inbound.queue, &result));
	assert_true(net.d.start(&net.d));
	assert_int_equal(net.d.finishNormal8(&net.d), 0x11);

	struct GBASIONetEvent detach = {
		.type = GBA_SIO_NET_EV_PEER_DETACH,
		.peerDetach = { .playerId = 2 },
	};
	assert_true(inbound.queue.vtable->push(&inbound.queue, &detach));
	assert_false(net.d.start(&net.d));
	assert_int_equal(net.d.connectedDevices(&net.d), 0);
	assert_false(net.protocolError);
}


M_TEST_DEFINE(saveStateLoadStateRoundTripDisconnected) {
	struct GBASIONetDriver source;
	struct GBASIONetDriver dest;
	void* blob = NULL;
	size_t blobSize = 0;
	GBASIONetDriverCreate(&source);
	GBASIONetDriverCreate(&dest);

	source.state = GBA_SIO_NET_DISCONNECTED;
	source.mode = GBA_SIO_MULTI;
	source.roomPlayerCount = 3;
	source.localPlayerId = 2;
	source.attachedPlayerMask = 0x7;
	source.lastSIOCNT = 0x1234;
	source.lastRCNT = 0x5678;
	source.multiplayerData[0] = 0xAAAA;
	source.multiplayerData[1] = 0xBBBB;
	source.multiplayerData[2] = 0xCCCC;
	source.multiplayerData[3] = 0xDDDD;
	source.normalData8 = 0x4D;
	source.normalData32 = 0x89ABCDEF;
	source.transferArmed = true;
	source.transferOrdinal = 41;
	source.committedTransferOrdinal = 41;
	source.nextOutboundSequence = 12345;

	source.d.saveState(&source.d, &blob, &blobSize);
	assert_non_null(blob);
	assert_int_equal(blobSize, 0x40);
	assert_true(dest.d.loadState(&dest.d, blob, blobSize));
	free(blob);

	assert_int_equal(dest.state, GBA_SIO_NET_DISCONNECTED);
	assert_int_equal(dest.mode, GBA_SIO_MULTI);
	assert_int_equal(dest.roomPlayerCount, 3);
	assert_int_equal(dest.localPlayerId, 2);
	assert_int_equal(dest.attachedPlayerMask, 0x7);
	assert_int_equal(dest.lastSIOCNT, 0x1234);
	assert_int_equal(dest.lastRCNT, 0x5678);
	assert_int_equal(dest.multiplayerData[0], 0xAAAA);
	assert_int_equal(dest.multiplayerData[1], 0xBBBB);
	assert_int_equal(dest.multiplayerData[2], 0xCCCC);
	assert_int_equal(dest.multiplayerData[3], 0xDDDD);
	assert_int_equal(dest.normalData8, 0x4D);
	assert_int_equal(dest.normalData32, 0x89ABCDEF);
	assert_true(dest.transferArmed);
	assert_int_equal(dest.transferOrdinal, 41);
	assert_int_equal(dest.committedTransferOrdinal, 41);
	assert_true(dest.committedTransferReady);
	assert_int_equal(dest.nextOutboundSequence, 12345);
}

M_TEST_DEFINE(loadStateRejectsConnectedSessionByPolicy) {
	struct GBASIONetDriver source;
	struct GBASIONetDriver dest;
	void* blob = NULL;
	size_t blobSize = 0;
	GBASIONetDriverCreate(&source);
	GBASIONetDriverCreate(&dest);

	source.state = GBA_SIO_NET_IN_ROOM;
	source.d.saveState(&source.d, &blob, &blobSize);
	assert_non_null(blob);

	dest.state = GBA_SIO_NET_IN_ROOM;
	assert_false(dest.d.loadState(&dest.d, blob, blobSize));
	free(blob);
}
M_TEST_DEFINE(peerDetachDuringTransferTriggersDeterministicDegradePath) {
	struct GBASIONetDriver net;
	struct TestQueue inbound;
	_initQueue(&inbound);
	GBASIONetDriverCreate(&net);
	GBASIONetDriverSetQueues(&net, NULL, &inbound.queue);
	net.state = GBA_SIO_NET_IN_ROOM;
	net.localPlayerId = 1;
	net.attachedPlayerMask = (1U << 1) | (1U << 2);
	net.roomPlayerCount = 2;
	net.mode = GBA_SIO_NORMAL_8;

	assert_false(net.d.start(&net.d));
	assert_true(net.transferArmed);

	struct GBASIONetEvent detach = {
		.type = GBA_SIO_NET_EV_PEER_DETACH,
		.peerDetach = { .playerId = 2 },
	};
	assert_true(inbound.queue.vtable->push(&inbound.queue, &detach));
	assert_false(net.d.start(&net.d));
	assert_true(net.protocolError);
	assert_int_equal(net.state, GBA_SIO_NET_DEGRADED);
}


M_TEST_DEFINE(controlEventsApplyBeforeStateGate) {
	struct GBASIONetDriver net;
	struct TestQueue inbound;
	_initQueue(&inbound);
	GBASIONetDriverCreate(&net);
	GBASIONetDriverSetQueues(&net, NULL, &inbound.queue);
	net.state = GBA_SIO_NET_IN_ROOM;
	net.localPlayerId = 1;
	net.mode = GBA_SIO_NORMAL_8;
	assert_false(net.d.start(&net.d));

	struct GBASIONetEvent failure = {
		.type = GBA_SIO_NET_EV_SESSION_FAILURE,
		.sessionFailure = { .kind = GBA_SIO_NET_FAIL_DISCONNECTED, .code = 42 },
	};
	assert_true(inbound.queue.vtable->push(&inbound.queue, &failure));
	assert_false(net.d.start(&net.d));
	assert_true(net.sessionDisconnected);
	assert_int_equal(net.lastFailureCode, 42);
	assert_int_equal(net.state, GBA_SIO_NET_DISCONNECTED);
}

M_TEST_DEFINE(protocolErrorStateNotOverwrittenByTransferStart) {
	struct GBASIONetDriver net;
	struct TestQueue inbound;
	_initQueue(&inbound);
	GBASIONetDriverCreate(&net);
	GBASIONetDriverSetQueues(&net, NULL, &inbound.queue);
	net.state = GBA_SIO_NET_IN_ROOM;
	net.localPlayerId = 1;
	net.attachedPlayerMask = (1U << 1) | (1U << 2);
	net.roomPlayerCount = 2;
	net.mode = GBA_SIO_NORMAL_8;
	assert_false(net.d.start(&net.d));

	struct GBASIONetEvent detach = {
		.type = GBA_SIO_NET_EV_PEER_DETACH,
		.peerDetach = { .playerId = 2 },
	};
	assert_true(inbound.queue.vtable->push(&inbound.queue, &detach));
	assert_false(net.d.start(&net.d));
	assert_true(net.protocolError);
	assert_int_equal(net.state, GBA_SIO_NET_DEGRADED);
}

M_TEST_DEFINE(repeatedLateMissesTransitionToPersistentDegradedPolicy) {
	struct GBASIONetDriver net;
	GBASIONetDriverCreate(&net);
	net.state = GBA_SIO_NET_ACTIVE_TRANSFER;
	net.transferArmed = true;
	net.transferOrdinal = 1;
	net.mode = GBA_SIO_NORMAL_8;

	assert_int_equal(net.d.finishNormal8(&net.d), 0xFF);
	assert_int_equal(net.latePolicyState, GBA_SIO_NET_LATE_MISSED_DEADLINE);
	assert_int_equal(net.state, GBA_SIO_NET_IN_ROOM);

	net.state = GBA_SIO_NET_ACTIVE_TRANSFER;
	net.transferArmed = true;
	net.transferOrdinal = 2;
	assert_int_equal(net.d.finishNormal8(&net.d), 0xFF);
	assert_int_equal(net.latePolicyState, GBA_SIO_NET_LATE_DEGRADED_PERSISTENT);
	assert_int_equal(net.state, GBA_SIO_NET_DEGRADED);
}

M_TEST_DEFINE(sessionDisconnectMidTransferForcesDeterministicFallbackAndBlocksStart) {
	struct GBASIONetDriver net;
	struct TestQueue inbound;
	_initQueue(&inbound);
	GBASIONetDriverCreate(&net);
	GBASIONetDriverSetQueues(&net, NULL, &inbound.queue);
	net.state = GBA_SIO_NET_IN_ROOM;
	net.localPlayerId = 1;
	net.mode = GBA_SIO_NORMAL_32;
	assert_false(net.d.start(&net.d));

	struct GBASIONetEvent failure = {
		.type = GBA_SIO_NET_EV_SESSION_FAILURE,
		.sessionFailure = { .kind = GBA_SIO_NET_FAIL_DISCONNECTED, .code = 0 },
	};
	assert_true(inbound.queue.vtable->push(&inbound.queue, &failure));
	assert_false(net.d.start(&net.d));
	assert_int_equal(net.state, GBA_SIO_NET_DISCONNECTED);
	assert_true(net.sessionDisconnected);
	assert_int_equal(net.d.finishNormal32(&net.d), 0xFFFFFFFF);
	assert_false(net.d.start(&net.d));
}

M_TEST_SUITE_DEFINE(GBANet,
	cmocka_unit_test(setModeEnqueuesIntent),
	cmocka_unit_test(startStallsUntilInboundResultThenFinishConsumes),
	cmocka_unit_test(finishMissingCommittedPayloadTriggersDeterministicSentinelAndError),
	cmocka_unit_test(reentrantStartWithoutCompletionPredictablyStalls),
	cmocka_unit_test(peerAttachDetachUpdatesTopologyAndDeviceView),
	cmocka_unit_test(saveStateLoadStateRoundTripDisconnected),
	cmocka_unit_test(loadStateRejectsConnectedSessionByPolicy),
	cmocka_unit_test(peerDetachDuringTransferTriggersDeterministicDegradePath),
	cmocka_unit_test(repeatedLateMissesTransitionToPersistentDegradedPolicy),
	cmocka_unit_test(sessionDisconnectMidTransferForcesDeterministicFallbackAndBlocksStart),
	cmocka_unit_test(controlEventsApplyBeforeStateGate),
	cmocka_unit_test(protocolErrorStateNotOverwrittenByTransferStart))
