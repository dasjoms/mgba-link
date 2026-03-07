/* Copyright (c) 2013-2024 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef GBA_SIO_NET_H
#define GBA_SIO_NET_H

#include <mgba-util/common.h>

CXX_GUARD_START

#include <mgba/internal/gba/sio.h>
#include <mgba/internal/gba/sio/net-bridge.h>

#include <stdbool.h>
#include <stdint.h>

enum GBASIONetDriverState {
	GBA_SIO_NET_DISCONNECTED,
	GBA_SIO_NET_CONNECTING,
	GBA_SIO_NET_IN_ROOM,
	GBA_SIO_NET_ACTIVE_TRANSFER,
	GBA_SIO_NET_DEGRADED,
};

enum GBASIONetLatePacketPolicyState {
	GBA_SIO_NET_LATE_ON_TIME,
	GBA_SIO_NET_LATE_WAITING_WITHIN_BUDGET,
	GBA_SIO_NET_LATE_MISSED_DEADLINE,
	GBA_SIO_NET_LATE_DEGRADED_PERSISTENT,
};

struct GBASIONetDriver {
	struct GBASIODriver d;
	struct GBASIONetEventQueue* outboundQueue;
	struct GBASIONetEventQueue* inboundQueue;
	enum GBASIONetDriverState state;
	enum GBASIOMode mode;
	int roomPlayerCount;
	int localPlayerId;
	uint8_t attachedPlayerMask;
	uint16_t lastSIOCNT;
	uint16_t lastRCNT;
	uint16_t multiplayerData[4];
	uint8_t normalData8;
	uint32_t normalData32;
	uint32_t committedTransferOrdinal;
	bool committedTransferReady;
	bool transferArmed;
	bool protocolError;
	int64_t nextOutboundSequence;
	uint32_t transferOrdinal;
	enum GBASIONetLatePacketPolicyState latePolicyState;
	uint32_t lateMissCount;
	uint32_t lateMissThreshold;
	bool sessionDisconnected;
	enum GBASIONetSessionFailureKind lastFailureKind;
	int lastFailureCode;
};

void GBASIONetDriverCreate(struct GBASIONetDriver* driver);
void GBASIONetDriverSetQueues(struct GBASIONetDriver* driver, struct GBASIONetEventQueue* outboundQueue, struct GBASIONetEventQueue* inboundQueue);

CXX_GUARD_END

#endif
