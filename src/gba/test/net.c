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
	assert_int_equal(net.transferOrdinal, 2);
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
	assert_true(net.protocolError);
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

M_TEST_SUITE_DEFINE(GBANet,
	cmocka_unit_test(setModeEnqueuesIntent),
	cmocka_unit_test(startStallsUntilInboundResultThenFinishConsumes),
	cmocka_unit_test(finishMissingCommittedPayloadTriggersDeterministicSentinelAndError),
	cmocka_unit_test(reentrantStartWithoutCompletionPredictablyStalls))
