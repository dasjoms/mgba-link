/* Copyright (c) 2013-2024 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef GBA_SIO_NET_BRIDGE_H
#define GBA_SIO_NET_BRIDGE_H

#include <mgba-util/common.h>

CXX_GUARD_START

#include <mgba/internal/gba/sio.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum GBASIONetEventType {
	GBA_SIO_NET_EV_MODE_SET,
	GBA_SIO_NET_EV_TRANSFER_START,
	GBA_SIO_NET_EV_TRANSFER_RESULT,
	GBA_SIO_NET_EV_HARD_SYNC,
	GBA_SIO_NET_EV_PEER_ATTACH,
	GBA_SIO_NET_EV_PEER_DETACH,
	GBA_SIO_NET_EV_SESSION_FAILURE,
};

enum GBASIONetSessionFailureKind {
	GBA_SIO_NET_FAIL_CONNECTION = 1,
	GBA_SIO_NET_FAIL_PROTOCOL = 2,
	GBA_SIO_NET_FAIL_HEARTBEAT_TIMEOUT = 3,
	GBA_SIO_NET_FAIL_ROOM_REJECTED = 4,
	GBA_SIO_NET_FAIL_MALFORMED_MESSAGE = 5,
	GBA_SIO_NET_FAIL_DISCONNECTED = 6,
};

struct GBASIONetModeSet {
	int playerId;
	enum GBASIOMode mode;
};

struct GBASIONetTransferStart {
	int playerId;
	enum GBASIOMode mode;
	int32_t finishCycle;
};

struct GBASIONetTransferResult {
	int playerId;
	int32_t tickMarker;
	const uint8_t* payload;
	size_t payloadSize;
};

struct GBASIONetHardSync {
	int32_t tickMarker;
};

struct GBASIONetPeerChange {
	int playerId;
};

struct GBASIONetSessionFailure {
	enum GBASIONetSessionFailureKind kind;
	int code;
};

struct GBASIONetEvent {
	enum GBASIONetEventType type;
	int senderPlayerId;
	int64_t sequence;
	union {
		struct GBASIONetModeSet modeSet;
		struct GBASIONetTransferStart transferStart;
		struct GBASIONetTransferResult transferResult;
		struct GBASIONetHardSync hardSync;
		struct GBASIONetPeerChange peerAttach;
		struct GBASIONetPeerChange peerDetach;
		struct GBASIONetSessionFailure sessionFailure;
	};
};

struct GBASIONetEventQueue;

struct GBASIONetEventQueueVTable {
	bool (*push)(struct GBASIONetEventQueue* queue, const struct GBASIONetEvent* event);
	bool (*tryPop)(struct GBASIONetEventQueue* queue, struct GBASIONetEvent* outEvent);
	bool (*waitPop)(struct GBASIONetEventQueue* queue, struct GBASIONetEvent* outEvent, int32_t timeoutMs);
	size_t (*size)(const struct GBASIONetEventQueue* queue);
	void (*wake)(struct GBASIONetEventQueue* queue);
};

struct GBASIONetEventQueue {
	void* context;
	const struct GBASIONetEventQueueVTable* vtable;
};

static inline bool GBASIONetEventQueuePush(struct GBASIONetEventQueue* queue, const struct GBASIONetEvent* event) {
	return queue && queue->vtable && queue->vtable->push && queue->vtable->push(queue, event);
}

static inline bool GBASIONetEventQueueTryPop(struct GBASIONetEventQueue* queue, struct GBASIONetEvent* outEvent) {
	return queue && queue->vtable && queue->vtable->tryPop && queue->vtable->tryPop(queue, outEvent);
}

static inline bool GBASIONetEventQueueWaitPop(struct GBASIONetEventQueue* queue, struct GBASIONetEvent* outEvent, int32_t timeoutMs) {
	return queue && queue->vtable && queue->vtable->waitPop && queue->vtable->waitPop(queue, outEvent, timeoutMs);
}

static inline size_t GBASIONetEventQueueSize(const struct GBASIONetEventQueue* queue) {
	return queue && queue->vtable && queue->vtable->size ? queue->vtable->size(queue) : 0;
}

static inline void GBASIONetEventQueueWake(struct GBASIONetEventQueue* queue) {
	if (queue && queue->vtable && queue->vtable->wake) {
		queue->vtable->wake(queue);
	}
}

CXX_GUARD_END

#endif
