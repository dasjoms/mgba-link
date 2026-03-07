/* Copyright (c) 2013-2015 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "netplay/DriverEventQueueBridge.h"

#include <chrono>
#include <thread>

using namespace QGBA::Netplay;

namespace {

bool _eventHasPayload(const GBASIONetEvent& event) {
	return event.type == GBA_SIO_NET_EV_TRANSFER_RESULT
		&& event.transferResult.payload
		&& event.transferResult.payloadSize > 0;
}

} // namespace

DriverEventQueueBridge::DriverEventQueueBridge() {
	static const GBASIONetEventQueueVTable DRIVER_OUTBOUND_QUEUE_VTABLE = {
		.push = DriverEventQueueBridge::_pushOutbound,
		.tryPop = DriverEventQueueBridge::_tryPopOutbound,
		.waitPop = DriverEventQueueBridge::_waitPopOutbound,
		.size = DriverEventQueueBridge::_sizeOutbound,
		.wake = DriverEventQueueBridge::_wakeOutbound,
	};
	static const GBASIONetEventQueueVTable DRIVER_INBOUND_QUEUE_VTABLE = {
		.push = DriverEventQueueBridge::_pushInbound,
		.tryPop = DriverEventQueueBridge::_tryPopInbound,
		.waitPop = DriverEventQueueBridge::_waitPopInbound,
		.size = DriverEventQueueBridge::_sizeInbound,
		.wake = DriverEventQueueBridge::_wakeInbound,
	};
	m_outboundQueue.context = this;
	m_outboundQueue.vtable = &DRIVER_OUTBOUND_QUEUE_VTABLE;
	m_inboundQueue.context = this;
	m_inboundQueue.vtable = &DRIVER_INBOUND_QUEUE_VTABLE;
}

GBASIONetEventQueue* DriverEventQueueBridge::outboundQueue() {
	return &m_outboundQueue;
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
	return pushInbound(event);
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
	return pushInbound(event);
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
	return pushInbound(event);
}

size_t DriverEventQueueBridge::pendingOutboundDepth() const {
	return outboundSize();
}

size_t DriverEventQueueBridge::pendingInboundDepth() const {
	return inboundSize();
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
	return pushInbound(event);
}

bool DriverEventQueueBridge::tryDequeueOutbound(GBASIONetEvent* outEvent) {
	return tryPopOutbound(outEvent);
}

bool DriverEventQueueBridge::pushOutbound(const GBASIONetEvent& event) {
	Item item;
	item.event = event;
	if (_eventHasPayload(event)) {
		item.payload.assign(event.transferResult.payload, event.transferResult.payload + event.transferResult.payloadSize);
		item.event.transferResult.payload = item.payload.data();
		item.event.transferResult.payloadSize = item.payload.size();
	}

	std::lock_guard<std::mutex> lock(m_mutex);
	m_outboundItems.push_back(std::move(item));
	return true;
}

bool DriverEventQueueBridge::pushInbound(const GBASIONetEvent& event) {
	Item item;
	item.event = event;
	if (_eventHasPayload(event)) {
		item.payload.assign(event.transferResult.payload, event.transferResult.payload + event.transferResult.payloadSize);
		item.event.transferResult.payload = item.payload.data();
		item.event.transferResult.payloadSize = item.payload.size();
	}

	std::lock_guard<std::mutex> lock(m_mutex);
	m_inboundItems.push_back(std::move(item));
	return true;
}

bool DriverEventQueueBridge::tryPopOutbound(GBASIONetEvent* outEvent) {
	if (!outEvent) {
		return false;
	}
	std::lock_guard<std::mutex> lock(m_mutex);
	if (m_outboundItems.empty()) {
		return false;
	}
	*outEvent = m_outboundItems.front().event;
	if (_eventHasPayload(*outEvent)) {
		m_lastPoppedOutboundPayload.assign(outEvent->transferResult.payload, outEvent->transferResult.payload + outEvent->transferResult.payloadSize);
		outEvent->transferResult.payload = m_lastPoppedOutboundPayload.data();
		outEvent->transferResult.payloadSize = m_lastPoppedOutboundPayload.size();
	} else {
		m_lastPoppedOutboundPayload.clear();
	}
	m_outboundItems.pop_front();
	return true;
}

bool DriverEventQueueBridge::tryPopInbound(GBASIONetEvent* outEvent) {
	if (!outEvent) {
		return false;
	}
	std::lock_guard<std::mutex> lock(m_mutex);
	if (m_inboundItems.empty()) {
		return false;
	}
	*outEvent = m_inboundItems.front().event;
	if (_eventHasPayload(*outEvent)) {
		m_lastPoppedInboundPayload.assign(outEvent->transferResult.payload, outEvent->transferResult.payload + outEvent->transferResult.payloadSize);
		outEvent->transferResult.payload = m_lastPoppedInboundPayload.data();
		outEvent->transferResult.payloadSize = m_lastPoppedInboundPayload.size();
	} else {
		m_lastPoppedInboundPayload.clear();
	}
	m_inboundItems.pop_front();
	return true;
}

size_t DriverEventQueueBridge::outboundSize() const {
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_outboundItems.size();
}

size_t DriverEventQueueBridge::inboundSize() const {
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_inboundItems.size();
}

bool DriverEventQueueBridge::_pushOutbound(GBASIONetEventQueue* queue, const GBASIONetEvent* event) {
	DriverEventQueueBridge* self = static_cast<DriverEventQueueBridge*>(queue ? queue->context : nullptr);
	return self && event ? self->pushOutbound(*event) : false;
}

bool DriverEventQueueBridge::_tryPopOutbound(GBASIONetEventQueue* queue, GBASIONetEvent* outEvent) {
	DriverEventQueueBridge* self = static_cast<DriverEventQueueBridge*>(queue ? queue->context : nullptr);
	return self ? self->tryPopOutbound(outEvent) : false;
}

bool DriverEventQueueBridge::_waitPopOutbound(GBASIONetEventQueue* queue, GBASIONetEvent* outEvent, int32_t timeoutMs) {
	return _waitPop(queue, outEvent, timeoutMs, true);
}

size_t DriverEventQueueBridge::_sizeOutbound(const GBASIONetEventQueue* queue) {
	const DriverEventQueueBridge* self = static_cast<const DriverEventQueueBridge*>(queue ? queue->context : nullptr);
	return self ? self->outboundSize() : 0;
}

void DriverEventQueueBridge::_wakeOutbound(GBASIONetEventQueue* queue) {
	UNUSED(queue);
}

bool DriverEventQueueBridge::_pushInbound(GBASIONetEventQueue* queue, const GBASIONetEvent* event) {
	DriverEventQueueBridge* self = static_cast<DriverEventQueueBridge*>(queue ? queue->context : nullptr);
	return self && event ? self->pushInbound(*event) : false;
}

bool DriverEventQueueBridge::_tryPopInbound(GBASIONetEventQueue* queue, GBASIONetEvent* outEvent) {
	DriverEventQueueBridge* self = static_cast<DriverEventQueueBridge*>(queue ? queue->context : nullptr);
	return self ? self->tryPopInbound(outEvent) : false;
}

bool DriverEventQueueBridge::_waitPopInbound(GBASIONetEventQueue* queue, GBASIONetEvent* outEvent, int32_t timeoutMs) {
	return _waitPop(queue, outEvent, timeoutMs, false);
}

size_t DriverEventQueueBridge::_sizeInbound(const GBASIONetEventQueue* queue) {
	const DriverEventQueueBridge* self = static_cast<const DriverEventQueueBridge*>(queue ? queue->context : nullptr);
	return self ? self->inboundSize() : 0;
}

void DriverEventQueueBridge::_wakeInbound(GBASIONetEventQueue* queue) {
	UNUSED(queue);
}

bool DriverEventQueueBridge::_waitPop(GBASIONetEventQueue* queue, GBASIONetEvent* outEvent, int32_t timeoutMs, bool outbound) {
	DriverEventQueueBridge* self = static_cast<DriverEventQueueBridge*>(queue ? queue->context : nullptr);
	if (!self || !outEvent) {
		return false;
	}
	const auto start = std::chrono::steady_clock::now();
	for (;;) {
		if ((outbound && self->tryPopOutbound(outEvent)) || (!outbound && self->tryPopInbound(outEvent))) {
			return true;
		}
		if (timeoutMs == 0) {
			return false;
		}
		if (timeoutMs > 0) {
			const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
			if (elapsed >= timeoutMs) {
				return false;
			}
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
}
