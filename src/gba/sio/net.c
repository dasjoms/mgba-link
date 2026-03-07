/* Copyright (c) 2013-2024 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <mgba/internal/gba/sio/net.h>

#define DRIVER_ID 0x2074654E
#define DRIVER_STATE_VERSION 1

enum {
	GBA_SIO_NET_SAVESTATE_SIZE = 0x40,
};

DECL_BITFIELD(GBASIONetSerializedFlags, uint32_t);
DECL_BITS(GBASIONetSerializedFlags, DriverMode, 0, 4);
DECL_BITS(GBASIONetSerializedFlags, DriverState, 4, 4);
DECL_BIT(GBASIONetSerializedFlags, TransferArmed, 8);
DECL_BITS(GBASIONetSerializedFlags, AttachedPlayerMask, 9, 4);

struct GBASIONetSerializedState {
	uint32_t driverId;
	uint32_t version;
	GBASIONetSerializedFlags flags;
	int32_t roomPlayerCount;
	int32_t localPlayerId;
	uint16_t lastSIOCNT;
	uint16_t lastRCNT;
	uint16_t multiplayerData[4];
	uint8_t normalData8;
	uint8_t reserved8[3];
	uint32_t normalData32;
	uint32_t committedTransferOrdinal;
	uint32_t transferOrdinal;
	int64_t nextOutboundSequence;
	uint8_t reserved[8];
};
static_assert(sizeof(struct GBASIONetSerializedState) == GBA_SIO_NET_SAVESTATE_SIZE,
	"GBA net savestate struct sized wrong");

static bool GBASIONetDriverInit(struct GBASIODriver* driver);
static void GBASIONetDriverDeinit(struct GBASIODriver* driver);
static void GBASIONetDriverReset(struct GBASIODriver* driver);
static uint32_t GBASIONetDriverId(const struct GBASIODriver* driver);
static bool GBASIONetDriverLoadState(struct GBASIODriver* driver, const void* state, size_t size);
static void GBASIONetDriverSaveState(struct GBASIODriver* driver, void** state, size_t* size);
static void GBASIONetDriverSetMode(struct GBASIODriver* driver, enum GBASIOMode mode);
static bool GBASIONetDriverHandlesMode(struct GBASIODriver* driver, enum GBASIOMode mode);
static int GBASIONetDriverConnectedDevices(struct GBASIODriver* driver);
static int GBASIONetDriverDeviceId(struct GBASIODriver* driver);
static uint16_t GBASIONetDriverWriteSIOCNT(struct GBASIODriver* driver, uint16_t value);
static uint16_t GBASIONetDriverWriteRCNT(struct GBASIODriver* driver, uint16_t value);
static bool GBASIONetDriverStart(struct GBASIODriver* driver);
static void GBASIONetDriverFinishMultiplayer(struct GBASIODriver* driver, uint16_t data[4]);
static uint8_t GBASIONetDriverFinishNormal8(struct GBASIODriver* driver);
static uint32_t GBASIONetDriverFinishNormal32(struct GBASIODriver* driver);

static bool _isTopologyEvent(const struct GBASIONetEvent* event);
static bool _applyTopologyEvent(struct GBASIONetDriver* net, const struct GBASIONetEvent* event);

static size_t _expectedPayloadSize(enum GBASIOMode mode) {
	switch (mode) {
	case GBA_SIO_MULTI:
		return sizeof(uint16_t) * MAX_GBAS;
	case GBA_SIO_NORMAL_8:
		return 1;
	case GBA_SIO_NORMAL_32:
		return sizeof(uint32_t);
	default:
		return 0;
	}
}

static void _setProtocolError(struct GBASIONetDriver* net, const char* message) {
	net->protocolError = true;
	net->state = GBA_SIO_NET_DEGRADED;
	mLOG(GBA_SIO, ERROR, "Net adapter protocol error: %s", message);
}

static bool _pushOutboundIntent(struct GBASIONetDriver* net, const struct GBASIONetEvent* event) {
	if (!net->outboundQueue) {
		return false;
	}
	return GBASIONetEventQueuePush(net->outboundQueue, event);
}

static bool _decodeTransferResult(struct GBASIONetDriver* net, const struct GBASIONetEvent* event) {
	size_t expectedPayloadSize = _expectedPayloadSize(net->mode);
	if (expectedPayloadSize == 0) {
		_setProtocolError(net, "unsupported mode for transfer result");
		return false;
	}

	if (!event->transferResult.payload || event->transferResult.payloadSize != expectedPayloadSize) {
		_setProtocolError(net, "malformed transfer result payload size");
		return false;
	}

	const uint8_t* payload = event->transferResult.payload;
	switch (net->mode) {
	case GBA_SIO_MULTI:
		for (int i = 0; i < MAX_GBAS; ++i) {
			net->multiplayerData[i] = payload[i * 2] | (payload[i * 2 + 1] << 8);
		}
		break;
	case GBA_SIO_NORMAL_8:
		net->normalData8 = payload[0];
		break;
	case GBA_SIO_NORMAL_32:
		net->normalData32 = payload[0] | (payload[1] << 8) | (payload[2] << 16) | (payload[3] << 24);
		break;
	default:
		_setProtocolError(net, "unsupported mode while decoding payload");
		return false;
	}

	net->committedTransferReady = true;
	net->committedTransferOrdinal = net->transferOrdinal;
	return true;
}

static bool _pollInboundForCurrentTransfer(struct GBASIONetDriver* net) {
	if (!net->inboundQueue) {
		return false;
	}

	struct GBASIONetEvent event;
	while (GBASIONetEventQueueTryPop(net->inboundQueue, &event)) {
		if (_isTopologyEvent(&event)) {
			_applyTopologyEvent(net, &event);
			continue;
		}
		if (event.type != GBA_SIO_NET_EV_TRANSFER_RESULT) {
			continue;
		}
		if (event.transferResult.playerId != net->localPlayerId) {
			continue;
		}
		return _decodeTransferResult(net, &event);
	}
	return false;
}

void GBASIONetDriverCreate(struct GBASIONetDriver* driver) {
	memset(driver, 0, sizeof(*driver));
	driver->d.init = GBASIONetDriverInit;
	driver->d.deinit = GBASIONetDriverDeinit;
	driver->d.reset = GBASIONetDriverReset;
	driver->d.driverId = GBASIONetDriverId;
	driver->d.loadState = GBASIONetDriverLoadState;
	driver->d.saveState = GBASIONetDriverSaveState;
	driver->d.setMode = GBASIONetDriverSetMode;
	driver->d.handlesMode = GBASIONetDriverHandlesMode;
	driver->d.connectedDevices = GBASIONetDriverConnectedDevices;
	driver->d.deviceId = GBASIONetDriverDeviceId;
	driver->d.writeSIOCNT = GBASIONetDriverWriteSIOCNT;
	driver->d.writeRCNT = GBASIONetDriverWriteRCNT;
	driver->d.start = GBASIONetDriverStart;
	driver->d.finishMultiplayer = GBASIONetDriverFinishMultiplayer;
	driver->d.finishNormal8 = GBASIONetDriverFinishNormal8;
	driver->d.finishNormal32 = GBASIONetDriverFinishNormal32;

	driver->state = GBA_SIO_NET_DISCONNECTED;
	driver->mode = (enum GBASIOMode) -1;
	driver->roomPlayerCount = 1;
	driver->attachedPlayerMask = 0x1;
	driver->nextOutboundSequence = 1;
}

void GBASIONetDriverSetQueues(struct GBASIONetDriver* driver, struct GBASIONetEventQueue* outboundQueue, struct GBASIONetEventQueue* inboundQueue) {
	driver->outboundQueue = outboundQueue;
	driver->inboundQueue = inboundQueue;
}

static bool GBASIONetDriverInit(struct GBASIODriver* driver) {
	GBASIONetDriverReset(driver);
	return true;
}

static void GBASIONetDriverDeinit(struct GBASIODriver* driver) {
	struct GBASIONetDriver* net = (struct GBASIONetDriver*) driver;
	net->state = GBA_SIO_NET_DISCONNECTED;
	net->transferArmed = false;
	net->committedTransferReady = false;
}

static void GBASIONetDriverReset(struct GBASIODriver* driver) {
	struct GBASIONetDriver* net = (struct GBASIONetDriver*) driver;
	net->state = GBA_SIO_NET_DISCONNECTED;
	net->mode = driver->p ? driver->p->mode : (enum GBASIOMode) -1;
	net->roomPlayerCount = 1;
	net->localPlayerId = 0;
	net->attachedPlayerMask = 0x1;
	net->lastSIOCNT = 0;
	net->lastRCNT = RCNT_INITIAL;
	memset(net->multiplayerData, 0xFF, sizeof(net->multiplayerData));
	net->normalData8 = 0xFF;
	net->normalData32 = 0xFFFFFFFF;
	net->committedTransferReady = false;
	net->committedTransferOrdinal = 0;
	net->transferArmed = false;
	net->protocolError = false;
	net->transferOrdinal = 0;
	net->nextOutboundSequence = 1;
}

static uint32_t GBASIONetDriverId(const struct GBASIODriver* driver) {
	UNUSED(driver);
	return DRIVER_ID;
}

static bool GBASIONetDriverLoadState(struct GBASIODriver* driver, const void* data, size_t size) {
	if (size != sizeof(struct GBASIONetSerializedState)) {
		mLOG(GBA_SIO, WARN, "Incorrect state size: expected %" PRIz "X, got %" PRIz "X", sizeof(struct GBASIONetSerializedState), size);
		return false;
	}

	const struct GBASIONetSerializedState* state = data;
	struct GBASIONetDriver* net = (struct GBASIONetDriver*) driver;
	uint32_t driverId;
	LOAD_32LE(driverId, 0, &state->driverId);
	if (driverId != DRIVER_ID) {
		mLOG(GBA_SIO, WARN, "Invalid net save state driver ID: expected %08X, got %08X", DRIVER_ID, driverId);
		return false;
	}

	uint32_t version;
	LOAD_32LE(version, 0, &state->version);
	if (version > DRIVER_STATE_VERSION) {
		mLOG(GBA_SIO, WARN, "Invalid or too new save state: expected %u, got %u", DRIVER_STATE_VERSION, version);
		return false;
	}

	GBASIONetSerializedFlags flags;
	LOAD_32LE(flags, 0, &state->flags);
	uint32_t mode = GBASIONetSerializedFlagsGetDriverMode(flags);
	if (mode == GBA_SIO_UART) {
		mode = GBA_SIO_NORMAL_8;
	}
	net->mode = (enum GBASIOMode) mode;

	uint32_t driverState = GBASIONetSerializedFlagsGetDriverState(flags);
	if (driverState > GBA_SIO_NET_DEGRADED) {
		driverState = GBA_SIO_NET_DEGRADED;
	}

	if (net->state >= GBA_SIO_NET_IN_ROOM || driverState >= GBA_SIO_NET_IN_ROOM) {
		mLOG(GBA_SIO, WARN, "Refusing to load net savestate while connected (v1 policy)");
		return false;
	}

	net->state = driverState;
	net->transferArmed = GBASIONetSerializedFlagsGetTransferArmed(flags);
	net->attachedPlayerMask = GBASIONetSerializedFlagsGetAttachedPlayerMask(flags) & ((1U << MAX_GBAS) - 1);

	LOAD_32LE(net->roomPlayerCount, 0, &state->roomPlayerCount);
	if (net->roomPlayerCount < 1 || net->roomPlayerCount > MAX_GBAS) {
		net->roomPlayerCount = 1;
	}
	LOAD_32LE(net->localPlayerId, 0, &state->localPlayerId);
	if (net->localPlayerId < 0 || net->localPlayerId >= MAX_GBAS) {
		net->localPlayerId = 0;
	}
	if (net->attachedPlayerMask == 0) {
		net->attachedPlayerMask = 1U << net->localPlayerId;
	}
	LOAD_16LE(net->lastSIOCNT, 0, &state->lastSIOCNT);
	LOAD_16LE(net->lastRCNT, 0, &state->lastRCNT);
	for (int i = 0; i < MAX_GBAS; ++i) {
		LOAD_16LE(net->multiplayerData[i], 0, &state->multiplayerData[i]);
	}
	net->normalData8 = state->normalData8;
	LOAD_32LE(net->normalData32, 0, &state->normalData32);
	LOAD_32LE(net->committedTransferOrdinal, 0, &state->committedTransferOrdinal);
	LOAD_32LE(net->transferOrdinal, 0, &state->transferOrdinal);
	LOAD_64LE(net->nextOutboundSequence, 0, &state->nextOutboundSequence);
	net->committedTransferReady = net->transferArmed && net->committedTransferOrdinal == net->transferOrdinal;
	net->protocolError = false;
	return true;
}

static void GBASIONetDriverSaveState(struct GBASIODriver* driver, void** stateOut, size_t* sizeOut) {
	struct GBASIONetDriver* net = (struct GBASIONetDriver*) driver;
	struct GBASIONetSerializedState* state = calloc(1, sizeof(*state));

	STORE_32LE(DRIVER_ID, 0, &state->driverId);
	STORE_32LE(DRIVER_STATE_VERSION, 0, &state->version);
	GBASIONetSerializedFlags flags = 0;
	flags = GBASIONetSerializedFlagsSetDriverMode(flags, net->mode & 0xF);
	flags = GBASIONetSerializedFlagsSetDriverState(flags, net->state & 0xF);
	flags = GBASIONetSerializedFlagsSetTransferArmed(flags, net->transferArmed);
	flags = GBASIONetSerializedFlagsSetAttachedPlayerMask(flags, net->attachedPlayerMask & ((1U << MAX_GBAS) - 1));
	STORE_32LE(flags, 0, &state->flags);
	STORE_32LE(net->roomPlayerCount, 0, &state->roomPlayerCount);
	STORE_32LE(net->localPlayerId, 0, &state->localPlayerId);
	STORE_16LE(net->lastSIOCNT, 0, &state->lastSIOCNT);
	STORE_16LE(net->lastRCNT, 0, &state->lastRCNT);
	for (int i = 0; i < MAX_GBAS; ++i) {
		STORE_16LE(net->multiplayerData[i], 0, &state->multiplayerData[i]);
	}
	state->normalData8 = net->normalData8;
	STORE_32LE(net->normalData32, 0, &state->normalData32);
	STORE_32LE(net->committedTransferOrdinal, 0, &state->committedTransferOrdinal);
	STORE_32LE(net->transferOrdinal, 0, &state->transferOrdinal);
	STORE_64LE(net->nextOutboundSequence, 0, &state->nextOutboundSequence);

	*stateOut = state;
	*sizeOut = sizeof(*state);
}

static void GBASIONetDriverSetMode(struct GBASIODriver* driver, enum GBASIOMode mode) {
	struct GBASIONetDriver* net = (struct GBASIONetDriver*) driver;
	net->mode = mode;
	if (net->state == GBA_SIO_NET_ACTIVE_TRANSFER) {
		net->state = GBA_SIO_NET_IN_ROOM;
	}

	struct GBASIONetEvent event = {
		.type = GBA_SIO_NET_EV_MODE_SET,
		.senderPlayerId = net->localPlayerId,
		.sequence = net->nextOutboundSequence++,
		.modeSet = {
			.playerId = net->localPlayerId,
			.mode = mode,
		},
	};
	if (!_pushOutboundIntent(net, &event)) {
		mLOG(GBA_SIO, DEBUG, "Net adapter dropped mode-set intent (queue unavailable/full)");
	}
}

static bool GBASIONetDriverHandlesMode(struct GBASIODriver* driver, enum GBASIOMode mode) {
	UNUSED(driver);
	switch (mode) {
	case GBA_SIO_NORMAL_8:
	case GBA_SIO_NORMAL_32:
	case GBA_SIO_MULTI:
		return true;
	default:
		return false;
	}
}

static int _popcountMask(uint8_t mask) {
	int count = 0;
	for (int i = 0; i < MAX_GBAS; ++i) {
		if (mask & (1U << i)) {
			++count;
		}
	}
	return count;
}

static bool _isTopologyEvent(const struct GBASIONetEvent* event) {
	return event->type == GBA_SIO_NET_EV_PEER_ATTACH || event->type == GBA_SIO_NET_EV_PEER_DETACH;
}

static bool _applyTopologyEvent(struct GBASIONetDriver* net, const struct GBASIONetEvent* event) {
	int playerId = -1;
	if (event->type == GBA_SIO_NET_EV_PEER_ATTACH) {
		playerId = event->peerAttach.playerId;
	} else if (event->type == GBA_SIO_NET_EV_PEER_DETACH) {
		playerId = event->peerDetach.playerId;
	}
	if (playerId < 0 || playerId >= MAX_GBAS) {
		_setProtocolError(net, "peer topology event has invalid player ID");
		return false;
	}

	uint8_t bit = (uint8_t) (1U << playerId);
	if (net->transferArmed) {
		_setProtocolError(net, "peer topology changed during active transfer");
		return false;
	}
	if (event->type == GBA_SIO_NET_EV_PEER_ATTACH) {
		net->attachedPlayerMask |= bit;
	} else {
		net->attachedPlayerMask &= ~bit;
		if (playerId == net->localPlayerId || net->attachedPlayerMask == 0) {
			_setProtocolError(net, "peer detach removed local player topology");
			return false;
		}
	}

	net->roomPlayerCount = _popcountMask(net->attachedPlayerMask);
	if (net->roomPlayerCount < 1) {
		net->roomPlayerCount = 1;
	}
	if (net->roomPlayerCount > MAX_GBAS) {
		net->roomPlayerCount = MAX_GBAS;
	}
	if (net->state >= GBA_SIO_NET_IN_ROOM && !net->protocolError) {
		net->state = GBA_SIO_NET_IN_ROOM;
	}
	return true;
}

static int GBASIONetDriverConnectedDevices(struct GBASIODriver* driver) {
	struct GBASIONetDriver* net = (struct GBASIONetDriver*) driver;
	if (net->state < GBA_SIO_NET_IN_ROOM) {
		return 0;
	}
	int connected = net->roomPlayerCount - 1;
	if (connected < 0) {
		connected = 0;
	}
	if (connected >= MAX_GBAS) {
		connected = MAX_GBAS - 1;
	}
	return connected;
}

static int GBASIONetDriverDeviceId(struct GBASIODriver* driver) {
	struct GBASIONetDriver* net = (struct GBASIONetDriver*) driver;
	if (net->state < GBA_SIO_NET_IN_ROOM) {
		return 0;
	}
	if (net->localPlayerId < 0 || net->localPlayerId >= MAX_GBAS) {
		return 0;
	}
	return net->localPlayerId;
}

static uint16_t GBASIONetDriverWriteSIOCNT(struct GBASIODriver* driver, uint16_t value) {
	struct GBASIONetDriver* net = (struct GBASIONetDriver*) driver;
	net->lastSIOCNT = value;

	switch (net->mode) {
	case GBA_SIO_NORMAL_8:
	case GBA_SIO_NORMAL_32:
		value = GBASIONormalFillSi(value);
		break;
	case GBA_SIO_MULTI:
		value = GBASIOMultiplayerFillReady(value);
		break;
	default:
		break;
	}
	return value;
}

static uint16_t GBASIONetDriverWriteRCNT(struct GBASIODriver* driver, uint16_t value) {
	struct GBASIONetDriver* net = (struct GBASIONetDriver*) driver;
	net->lastRCNT = value;
	return value;
}

static bool GBASIONetDriverStart(struct GBASIODriver* driver) {
	struct GBASIONetDriver* net = (struct GBASIONetDriver*) driver;
	if (net->protocolError || net->state < GBA_SIO_NET_IN_ROOM) {
		return false;
	}
	if (!net->transferArmed) {
		++net->transferOrdinal;
		net->transferArmed = true;
		net->committedTransferReady = false;

		struct GBASIONetEvent event = {
			.type = GBA_SIO_NET_EV_TRANSFER_START,
			.senderPlayerId = net->localPlayerId,
			.sequence = net->nextOutboundSequence++,
			.transferStart = {
				.playerId = net->localPlayerId,
				.mode = net->mode,
				.finishCycle = -1,
			},
		};
		if (!_pushOutboundIntent(net, &event)) {
			mLOG(GBA_SIO, DEBUG, "Net adapter dropped transfer-start intent (queue unavailable/full)");
		}
	}

	if (net->committedTransferReady && net->committedTransferOrdinal == net->transferOrdinal) {
		if (net->state >= GBA_SIO_NET_IN_ROOM) {
			net->state = GBA_SIO_NET_ACTIVE_TRANSFER;
		}
		return true;
	}

	if (!_pollInboundForCurrentTransfer(net)) {
		if (net->state >= GBA_SIO_NET_IN_ROOM) {
			net->state = GBA_SIO_NET_ACTIVE_TRANSFER;
		}
		return false;
	}

	if (net->state >= GBA_SIO_NET_IN_ROOM) {
		net->state = GBA_SIO_NET_ACTIVE_TRANSFER;
	}
	return true;
}

static void GBASIONetDriverFinishMultiplayer(struct GBASIODriver* driver, uint16_t data[4]) {
	struct GBASIONetDriver* net = (struct GBASIONetDriver*) driver;
	if (!net->committedTransferReady || net->committedTransferOrdinal != net->transferOrdinal) {
		_setProtocolError(net, "missing committed multiplayer completion payload");
	}
	for (int i = 0; i < MAX_GBAS; ++i) {
		data[i] = net->multiplayerData[i];
	}
	net->transferArmed = false;
	net->committedTransferReady = false;
	if (net->state == GBA_SIO_NET_ACTIVE_TRANSFER) {
		net->state = GBA_SIO_NET_IN_ROOM;
	}
}

static uint8_t GBASIONetDriverFinishNormal8(struct GBASIODriver* driver) {
	struct GBASIONetDriver* net = (struct GBASIONetDriver*) driver;
	if (!net->committedTransferReady || net->committedTransferOrdinal != net->transferOrdinal) {
		_setProtocolError(net, "missing committed normal8 completion payload");
	}
	net->transferArmed = false;
	net->committedTransferReady = false;
	if (net->state == GBA_SIO_NET_ACTIVE_TRANSFER) {
		net->state = GBA_SIO_NET_IN_ROOM;
	}
	return net->normalData8;
}

static uint32_t GBASIONetDriverFinishNormal32(struct GBASIODriver* driver) {
	struct GBASIONetDriver* net = (struct GBASIONetDriver*) driver;
	if (!net->committedTransferReady || net->committedTransferOrdinal != net->transferOrdinal) {
		_setProtocolError(net, "missing committed normal32 completion payload");
	}
	net->transferArmed = false;
	net->committedTransferReady = false;
	if (net->state == GBA_SIO_NET_ACTIVE_TRANSFER) {
		net->state = GBA_SIO_NET_IN_ROOM;
	}
	return net->normalData32;
}
