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

	GBASIONetEventQueue* inboundQueue();

	bool enqueueTransferResult(int senderPlayerId, int targetPlayerId, int64_t sequence, int32_t tickMarker, const QByteArray& payload);
	bool enqueuePeerAttach(int playerId, int64_t sequence);
	bool enqueuePeerDetach(int playerId, int64_t sequence);
	bool enqueueSessionFailure(GBASIONetSessionFailureKind kind, int code, int64_t sequence);

private:
	struct Item {
		GBASIONetEvent event = {};
		std::vector<uint8_t> payload;
	};

	static bool _push(GBASIONetEventQueue* queue, const GBASIONetEvent* event);
	static bool _tryPop(GBASIONetEventQueue* queue, GBASIONetEvent* outEvent);
	static bool _waitPop(GBASIONetEventQueue* queue, GBASIONetEvent* outEvent, int32_t timeoutMs);
	static size_t _size(const GBASIONetEventQueue* queue);
	static void _wake(GBASIONetEventQueue* queue);

	bool push(const GBASIONetEvent& event);
	bool tryPop(GBASIONetEvent* outEvent);
	size_t size() const;

	mutable std::mutex m_mutex;
	std::deque<Item> m_items;
	GBASIONetEventQueue m_inboundQueue = {};
};

} // namespace Netplay
} // namespace QGBA
