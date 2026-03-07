/* Copyright (c) 2013-2015 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "netplay/DriverEventQueueBridge.h"

#include <chrono>
#include <thread>

using namespace QGBA::Netplay;

DriverEventQueueBridge::DriverEventQueueBridge() {
	static const GBASIONetEventQueueVTable DRIVER_EVENT_QUEUE_VTABLE = {
		.push = DriverEventQueueBridge::_push,
		.tryPop = DriverEventQueueBridge::_tryPop,
		.waitPop = DriverEventQueueBridge::_waitPop,
		.size = DriverEventQueueBridge::_size,
		.wake = DriverEventQueueBridge::_wake,
	};
	m_inboundQueue.context = this;
	m_inboundQueue.vtable = &DRIVER_EVENT_QUEUE_VTABLE;
}

GBASIONetEventQueue* DriverEventQueueBridge::inboundQueue() {
	return &m_inboundQueue;
}

bool DriverEventQueueBridge::enqueueTransferResult(int senderPlayerId, int targetPlayerId, int64_t sequence, int32_t tickMarker, const QByteArray& payload) {
	GBASIONetEvent event = {
		.type = GBA_SIO_NET_EV_TRANSFER_RESULT,
		.senderPlayerId = senderPlayerId,
		.sequence = sequence,
		.transferResult = {
			.playerId = targetPlayerId,
			.tickMarker = tickMarker,
			.payload = reinterpret_cast<const uint8_t*>(payload.constData()),
			.payloadSize = static_cast<size_t>(payload.size()),
		},
	};
	return push(event);
}

bool DriverEventQueueBridge::enqueuePeerAttach(int playerId, int64_t sequence) {
	GBASIONetEvent event = {
		.type = GBA_SIO_NET_EV_PEER_ATTACH,
		.senderPlayerId = playerId,
		.sequence = sequence,
		.peerAttach = {
			.playerId = playerId,
		},
	};
	return push(event);
}

bool DriverEventQueueBridge::enqueuePeerDetach(int playerId, int64_t sequence) {
	GBASIONetEvent event = {
		.type = GBA_SIO_NET_EV_PEER_DETACH,
		.senderPlayerId = playerId,
		.sequence = sequence,
		.peerDetach = {
			.playerId = playerId,
		},
	};
	return push(event);
}


size_t DriverEventQueueBridge::pendingInboundDepth() const {
	return size();
}

bool DriverEventQueueBridge::enqueueSessionFailure(GBASIONetSessionFailureKind kind, int code, int64_t sequence) {
	GBASIONetEvent event = {
		.type = GBA_SIO_NET_EV_SESSION_FAILURE,
		.senderPlayerId = -1,
		.sequence = sequence,
		.sessionFailure = {
			.kind = kind,
			.code = code,
		},
	};
	return push(event);
}

bool DriverEventQueueBridge::push(const GBASIONetEvent& event) {
	Item item;
	item.event = event;
	if (event.type == GBA_SIO_NET_EV_TRANSFER_RESULT && event.transferResult.payload && event.transferResult.payloadSize > 0) {
		item.payload.assign(event.transferResult.payload, event.transferResult.payload + event.transferResult.payloadSize);
		item.event.transferResult.payload = item.payload.data();
		item.event.transferResult.payloadSize = item.payload.size();
	}

	std::lock_guard<std::mutex> lock(m_mutex);
	m_items.push_back(std::move(item));
	return true;
}

bool DriverEventQueueBridge::tryPop(GBASIONetEvent* outEvent) {
	if (!outEvent) {
		return false;
	}
	std::lock_guard<std::mutex> lock(m_mutex);
	if (m_items.empty()) {
		return false;
	}
	*outEvent = m_items.front().event;
	if (outEvent->type == GBA_SIO_NET_EV_TRANSFER_RESULT && outEvent->transferResult.payload && outEvent->transferResult.payloadSize > 0) {
		m_lastPoppedPayload.assign(outEvent->transferResult.payload, outEvent->transferResult.payload + outEvent->transferResult.payloadSize);
		outEvent->transferResult.payload = m_lastPoppedPayload.data();
		outEvent->transferResult.payloadSize = m_lastPoppedPayload.size();
	} else {
		m_lastPoppedPayload.clear();
	}
	m_items.pop_front();
	return true;
}

size_t DriverEventQueueBridge::size() const {
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_items.size();
}

bool DriverEventQueueBridge::_push(GBASIONetEventQueue* queue, const GBASIONetEvent* event) {
	DriverEventQueueBridge* self = static_cast<DriverEventQueueBridge*>(queue ? queue->context : nullptr);
	return self && event ? self->push(*event) : false;
}

bool DriverEventQueueBridge::_tryPop(GBASIONetEventQueue* queue, GBASIONetEvent* outEvent) {
	DriverEventQueueBridge* self = static_cast<DriverEventQueueBridge*>(queue ? queue->context : nullptr);
	return self ? self->tryPop(outEvent) : false;
}

bool DriverEventQueueBridge::_waitPop(GBASIONetEventQueue* queue, GBASIONetEvent* outEvent, int32_t timeoutMs) {
	DriverEventQueueBridge* self = static_cast<DriverEventQueueBridge*>(queue ? queue->context : nullptr);
	if (!self) {
		return false;
	}
	if (timeoutMs <= 0) {
		return self->tryPop(outEvent);
	}
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
	while (std::chrono::steady_clock::now() < deadline) {
		if (self->tryPop(outEvent)) {
			return true;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	return self->tryPop(outEvent);
}

size_t DriverEventQueueBridge::_size(const GBASIONetEventQueue* queue) {
	const DriverEventQueueBridge* self = static_cast<const DriverEventQueueBridge*>(queue ? queue->context : nullptr);
	return self ? self->size() : 0;
}

void DriverEventQueueBridge::_wake(GBASIONetEventQueue* queue) {
	(void) queue;
}
