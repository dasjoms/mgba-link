/* Copyright (c) 2013-2015 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#pragma once

#include <QByteArray>

#include <mgba/internal/gba/sio/net-bridge.h>

#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

namespace QGBA {
namespace Netplay {

class DriverEventQueueBridge {
public:
	DriverEventQueueBridge();

	GBASIONetEventQueue* outboundQueue();
	GBASIONetEventQueue* inboundQueue();

	bool enqueueTransferResult(int senderPlayerId, int targetPlayerId, int64_t sequence, int32_t tickMarker, const QByteArray& payload);
	bool enqueuePeerAttach(int playerId, int64_t sequence);
	bool enqueuePeerDetach(int playerId, int64_t sequence);
	bool enqueueSessionFailure(GBASIONetSessionFailureKind kind, int code, int64_t sequence);
	bool tryDequeueOutbound(GBASIONetEvent* outEvent);
	size_t pendingOutboundDepth() const;
	size_t pendingInboundDepth() const;

private:
	struct Item {
		GBASIONetEvent event = {};
		std::vector<uint8_t> payload;
	};

	static bool _pushOutbound(GBASIONetEventQueue* queue, const GBASIONetEvent* event);
	static bool _tryPopOutbound(GBASIONetEventQueue* queue, GBASIONetEvent* outEvent);
	static bool _waitPopOutbound(GBASIONetEventQueue* queue, GBASIONetEvent* outEvent, int32_t timeoutMs);
	static size_t _sizeOutbound(const GBASIONetEventQueue* queue);
	static void _wakeOutbound(GBASIONetEventQueue* queue);

	static bool _pushInbound(GBASIONetEventQueue* queue, const GBASIONetEvent* event);
	static bool _tryPopInbound(GBASIONetEventQueue* queue, GBASIONetEvent* outEvent);
	static bool _waitPopInbound(GBASIONetEventQueue* queue, GBASIONetEvent* outEvent, int32_t timeoutMs);
	static size_t _sizeInbound(const GBASIONetEventQueue* queue);
	static void _wakeInbound(GBASIONetEventQueue* queue);

	bool pushOutbound(const GBASIONetEvent& event);
	bool pushInbound(const GBASIONetEvent& event);
	bool tryPopOutbound(GBASIONetEvent* outEvent);
	bool tryPopInbound(GBASIONetEvent* outEvent);
	size_t outboundSize() const;
	size_t inboundSize() const;

	static bool _waitPop(GBASIONetEventQueue* queue, GBASIONetEvent* outEvent, int32_t timeoutMs, bool outbound);

	mutable std::mutex m_mutex;
	std::deque<Item> m_outboundItems;
	std::deque<Item> m_inboundItems;
	std::vector<uint8_t> m_lastPoppedOutboundPayload;
	std::vector<uint8_t> m_lastPoppedInboundPayload;
	GBASIONetEventQueue m_outboundQueue = {};
	GBASIONetEventQueue m_inboundQueue = {};
};

} // namespace Netplay
} // namespace QGBA
