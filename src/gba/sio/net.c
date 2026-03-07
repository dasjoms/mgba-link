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

struct GBASIONetSerializedState {
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
	uint32_t transferOrdinal;
	uint8_t reserved[24];
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
}

static bool GBASIONetDriverInit(struct GBASIODriver* driver) {
	GBASIONetDriverReset(driver);
	return true;
}

static void GBASIONetDriverDeinit(struct GBASIODriver* driver) {
	struct GBASIONetDriver* net = (struct GBASIONetDriver*) driver;
	net->state = GBA_SIO_NET_DISCONNECTED;
	net->transferArmed = false;
}

static void GBASIONetDriverReset(struct GBASIODriver* driver) {
	struct GBASIONetDriver* net = (struct GBASIONetDriver*) driver;
	net->state = GBA_SIO_NET_DISCONNECTED;
	net->mode = driver->p ? driver->p->mode : (enum GBASIOMode) -1;
	net->roomPlayerCount = 1;
	net->localPlayerId = 0;
	net->lastSIOCNT = 0;
	net->lastRCNT = RCNT_INITIAL;
	memset(net->multiplayerData, 0xFF, sizeof(net->multiplayerData));
	net->normalData8 = 0xFF;
	net->normalData32 = 0xFFFFFFFF;
	net->transferArmed = false;
	net->transferOrdinal = 0;
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
	net->state = driverState;
	net->transferArmed = GBASIONetSerializedFlagsGetTransferArmed(flags);

	LOAD_32LE(net->roomPlayerCount, 0, &state->roomPlayerCount);
	if (net->roomPlayerCount < 1 || net->roomPlayerCount > MAX_GBAS) {
		net->roomPlayerCount = 1;
	}
	LOAD_32LE(net->localPlayerId, 0, &state->localPlayerId);
	if (net->localPlayerId < 0 || net->localPlayerId >= MAX_GBAS) {
		net->localPlayerId = 0;
	}
	LOAD_16LE(net->lastSIOCNT, 0, &state->lastSIOCNT);
	LOAD_16LE(net->lastRCNT, 0, &state->lastRCNT);
	for (int i = 0; i < MAX_GBAS; ++i) {
		LOAD_16LE(net->multiplayerData[i], 0, &state->multiplayerData[i]);
	}
	net->normalData8 = state->normalData8;
	LOAD_32LE(net->normalData32, 0, &state->normalData32);
	LOAD_32LE(net->transferOrdinal, 0, &state->transferOrdinal);
	return true;
}

static void GBASIONetDriverSaveState(struct GBASIODriver* driver, void** stateOut, size_t* sizeOut) {
	struct GBASIONetDriver* net = (struct GBASIONetDriver*) driver;
	struct GBASIONetSerializedState* state = calloc(1, sizeof(*state));

	STORE_32LE(DRIVER_STATE_VERSION, 0, &state->version);
	GBASIONetSerializedFlags flags = 0;
	flags = GBASIONetSerializedFlagsSetDriverMode(flags, net->mode & 0xF);
	flags = GBASIONetSerializedFlagsSetDriverState(flags, net->state & 0xF);
	flags = GBASIONetSerializedFlagsSetTransferArmed(flags, net->transferArmed);
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
	STORE_32LE(net->transferOrdinal, 0, &state->transferOrdinal);

	*stateOut = state;
	*sizeOut = sizeof(*state);
}

static void GBASIONetDriverSetMode(struct GBASIODriver* driver, enum GBASIOMode mode) {
	struct GBASIONetDriver* net = (struct GBASIONetDriver*) driver;
	net->mode = mode;
	if (net->state == GBA_SIO_NET_ACTIVE_TRANSFER) {
		net->state = GBA_SIO_NET_IN_ROOM;
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
	++net->transferOrdinal;
	net->transferArmed = true;

	if (net->state >= GBA_SIO_NET_IN_ROOM) {
		net->state = GBA_SIO_NET_ACTIVE_TRANSFER;
	}
	return true;
}

static void GBASIONetDriverFinishMultiplayer(struct GBASIODriver* driver, uint16_t data[4]) {
	struct GBASIONetDriver* net = (struct GBASIONetDriver*) driver;
	for (int i = 0; i < MAX_GBAS; ++i) {
		data[i] = net->multiplayerData[i];
	}
	net->transferArmed = false;
	if (net->state == GBA_SIO_NET_ACTIVE_TRANSFER) {
		net->state = GBA_SIO_NET_IN_ROOM;
	}
}

static uint8_t GBASIONetDriverFinishNormal8(struct GBASIODriver* driver) {
	struct GBASIONetDriver* net = (struct GBASIONetDriver*) driver;
	net->transferArmed = false;
	if (net->state == GBA_SIO_NET_ACTIVE_TRANSFER) {
		net->state = GBA_SIO_NET_IN_ROOM;
	}
	return net->normalData8;
}

static uint32_t GBASIONetDriverFinishNormal32(struct GBASIODriver* driver) {
	struct GBASIONetDriver* net = (struct GBASIONetDriver*) driver;
	net->transferArmed = false;
	if (net->state == GBA_SIO_NET_ACTIVE_TRANSFER) {
		net->state = GBA_SIO_NET_IN_ROOM;
	}
	return net->normalData32;
}
