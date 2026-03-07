/* Copyright (c) 2013-2015 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "MultiplayerController.h"

#include "ConfigController.h"
#include "CoreController.h"
#include "LogController.h"
#include "utils.h"
#include "netplay/DriverEventQueueBridge.h"
#include "netplay/Session.h"
#include "netplay/SessionTypes.h"

#ifdef M_CORE_GBA
#include <mgba/internal/gba/gba.h>
#endif
#ifdef M_CORE_GB
#include <mgba/internal/gb/gb.h>
#endif

#include <algorithm>
#include <chrono>
#include <QDataStream>
#include <QIODevice>
#include <QVariantMap>

using namespace QGBA;


namespace {


QByteArray _encodeDriverEventPayload(const GBASIONetEvent& event) {
	QByteArray payload;
	QDataStream stream(&payload, QIODevice::WriteOnly);
	stream.setByteOrder(QDataStream::LittleEndian);
	stream << static_cast<quint8>(1);
	stream << static_cast<quint8>(event.type);
	stream << static_cast<qint32>(event.senderPlayerId);
	stream << static_cast<qint64>(event.sequence);
	switch (event.type) {
	case GBA_SIO_NET_EV_MODE_SET:
		stream << static_cast<qint32>(event.modeSet.playerId);
		stream << static_cast<qint32>(event.modeSet.mode);
		break;
	case GBA_SIO_NET_EV_TRANSFER_START:
		stream << static_cast<qint32>(event.transferStart.playerId);
		stream << static_cast<qint32>(event.transferStart.mode);
		stream << static_cast<qint32>(event.transferStart.finishCycle);
		break;
	case GBA_SIO_NET_EV_TRANSFER_RESULT: {
		stream << static_cast<qint32>(event.transferResult.playerId);
		stream << static_cast<qint32>(event.transferResult.tickMarker);
		const quint32 size = static_cast<quint32>(event.transferResult.payloadSize);
		stream << size;
		if (size && event.transferResult.payload) {
			payload.append(reinterpret_cast<const char*>(event.transferResult.payload), static_cast<int>(event.transferResult.payloadSize));
		}
		break;
	}
	case GBA_SIO_NET_EV_HARD_SYNC:
		stream << static_cast<qint32>(event.hardSync.tickMarker);
		break;
	case GBA_SIO_NET_EV_PEER_ATTACH:
		stream << static_cast<qint32>(event.peerAttach.playerId);
		break;
	case GBA_SIO_NET_EV_PEER_DETACH:
		stream << static_cast<qint32>(event.peerDetach.playerId);
		break;
	case GBA_SIO_NET_EV_SESSION_FAILURE:
		stream << static_cast<qint32>(event.sessionFailure.kind);
		stream << static_cast<qint32>(event.sessionFailure.code);
		break;
	}
	return payload;
}

bool _decodeDriverEventPayload(const QByteArray& payload, GBASIONetEvent* outEvent) {
	if (!outEvent) {
		return false;
	}

	QDataStream stream(payload);
	stream.setByteOrder(QDataStream::LittleEndian);

	quint8 payloadVersion = 0;
	quint8 rawType = 0;
	qint32 senderPlayerId = -1;
	qint64 sequence = -1;
	stream >> payloadVersion;
	stream >> rawType;
	stream >> senderPlayerId;
	stream >> sequence;
	if (stream.status() != QDataStream::Ok || payloadVersion != 1) {
		return false;
	}

	GBASIONetEvent event = {};
	event.type = static_cast<GBASIONetEventType>(rawType);
	event.senderPlayerId = senderPlayerId;
	event.sequence = sequence;

	switch (event.type) {
	case GBA_SIO_NET_EV_MODE_SET:
		stream >> event.modeSet.playerId;
		stream >> event.modeSet.mode;
		break;
	case GBA_SIO_NET_EV_TRANSFER_START:
		stream >> event.transferStart.playerId;
		stream >> event.transferStart.mode;
		stream >> event.transferStart.finishCycle;
		break;
	case GBA_SIO_NET_EV_TRANSFER_RESULT: {
		stream >> event.transferResult.playerId;
		stream >> event.transferResult.tickMarker;
		quint32 payloadSize = 0;
		stream >> payloadSize;
		if (stream.status() != QDataStream::Ok) {
			return false;
		}
		if (payload.size() < stream.device()->pos() + payloadSize) {
			return false;
		}
		event.transferResult.payload = reinterpret_cast<const uint8_t*>(payload.constData() + stream.device()->pos());
		event.transferResult.payloadSize = payloadSize;
		stream.skipRawData(static_cast<int>(payloadSize));
		break;
	}
	case GBA_SIO_NET_EV_HARD_SYNC:
		stream >> event.hardSync.tickMarker;
		break;
	case GBA_SIO_NET_EV_PEER_ATTACH:
		stream >> event.peerAttach.playerId;
		break;
	case GBA_SIO_NET_EV_PEER_DETACH:
		stream >> event.peerDetach.playerId;
		break;
	case GBA_SIO_NET_EV_SESSION_FAILURE:
		stream >> event.sessionFailure.kind;
		stream >> event.sessionFailure.code;
		break;
	default:
		return false;
	}

	if (stream.status() != QDataStream::Ok || !stream.atEnd()) {
		return false;
	}

	*outEvent = event;
	return true;
}

QString _layerTag(QGBA::Netplay::NetplayFailureLayer layer) {
	return QString::fromLatin1(QGBA::Netplay::netplayFailureLayerName(layer));
}

QString _categoryTag(QGBA::Netplay::NetplayErrorCategory category) {
	return QString::fromLatin1(QGBA::Netplay::netplayErrorCategoryName(category));
}


GBASIONetSessionFailureKind _mapFailureKind(QGBA::Netplay::NetplayErrorCategory category) {
	switch (category) {
	case QGBA::Netplay::NetplayErrorCategory::ConnectionFailure:
		return GBA_SIO_NET_FAIL_CONNECTION;
	case QGBA::Netplay::NetplayErrorCategory::ProtocolMismatch:
		return GBA_SIO_NET_FAIL_PROTOCOL;
	case QGBA::Netplay::NetplayErrorCategory::HeartbeatTimeout:
		return GBA_SIO_NET_FAIL_HEARTBEAT_TIMEOUT;
	case QGBA::Netplay::NetplayErrorCategory::RoomRejectedOrFull:
		return GBA_SIO_NET_FAIL_ROOM_REJECTED;
	case QGBA::Netplay::NetplayErrorCategory::MalformedMessage:
		return GBA_SIO_NET_FAIL_MALFORMED_MESSAGE;
	}
	return GBA_SIO_NET_FAIL_DISCONNECTED;
}


QString _uiRemoteStateMessage(const QGBA::Netplay::SessionProtocolError& error) {
	const int code = error.code;
	switch (error.category) {
	case QGBA::Netplay::NetplayErrorCategory::ProtocolMismatch:
		return QObject::tr("Remote netplay protocol mismatch. Please verify client/server versions.");
	case QGBA::Netplay::NetplayErrorCategory::RoomRejectedOrFull:
		return QObject::tr("Remote room is full or join was rejected.");
	case QGBA::Netplay::NetplayErrorCategory::HeartbeatTimeout:
		return QObject::tr("Timed out while waiting for peers.");
	case QGBA::Netplay::NetplayErrorCategory::ConnectionFailure:
		if (code == 409 || code == 426) {
			return QObject::tr("Remote netplay protocol mismatch. Please verify client/server versions.");
		}
		if (code == 403 || code == 404) {
			return QObject::tr("Remote room is full or join was rejected.");
		}
		if (code == 408 || code >= 400000) {
			return QObject::tr("Timed out while waiting for peers.");
		}
		return QObject::tr("Remote netplay connection lost.");
	case QGBA::Netplay::NetplayErrorCategory::MalformedMessage:
		return QObject::tr("Remote netplay protocol mismatch. Please verify client/server versions.");
	}
	return QObject::tr("Remote netplay connection lost.");
}

GBASIONetSessionFailureKind _mapFailureKindFromError(const QGBA::Netplay::SessionProtocolError& error) {
	if (error.category == QGBA::Netplay::NetplayErrorCategory::ProtocolMismatch || error.category == QGBA::Netplay::NetplayErrorCategory::MalformedMessage || error.code == 409 || error.code == 426) {
		return GBA_SIO_NET_FAIL_PROTOCOL;
	}
	if (error.category == QGBA::Netplay::NetplayErrorCategory::RoomRejectedOrFull || error.code == 403 || error.code == 404) {
		return GBA_SIO_NET_FAIL_ROOM_REJECTED;
	}
	if (error.category == QGBA::Netplay::NetplayErrorCategory::HeartbeatTimeout || error.code == 408 || error.code >= 400000) {
		return GBA_SIO_NET_FAIL_HEARTBEAT_TIMEOUT;
	}
	if (error.category == QGBA::Netplay::NetplayErrorCategory::ConnectionFailure) {
		return GBA_SIO_NET_FAIL_CONNECTION;
	}
	return _mapFailureKind(error.category);
}
} // namespace

QString MultiplayerController::RemoteSessionConfig::endpoint() const {
	if (!hasServerEndpoint()) {
		return {};
	}
	return QStringLiteral("tcp://%1:%2").arg(host.trimmed()).arg(port);
}

bool MultiplayerController::RemoteSessionConfig::hasServerEndpoint() const {
	return !host.trimmed().isEmpty() && port > 0;
}

MultiplayerController::Player::Player(CoreController* coreController)
	: controller(coreController)
{
}

int MultiplayerController::Player::id() const {
	switch (controller->platform()) {
#ifdef M_CORE_GBA
	case mPLATFORM_GBA: {
		GBASIODriver* driver = remoteBackend ? &node.gbaNet->d : &node.gba->d;
		int id = driver->deviceId(driver);
		if (id >= 0) {
			return id;
		} else {
			return preferredId;
		}
	}
#endif
#ifdef M_CORE_GB
	case mPLATFORM_GB:
		return node.gb->id;
#endif
	case mPLATFORM_NONE:
		break;
	}
	return -1;
}

bool MultiplayerController::Player::operator<(const MultiplayerController::Player& other) const {
	return id() < other.id();
}

MultiplayerController::MultiplayerController() {
	mLockstepInit(&m_lockstep);
	m_lockstep.context = this;
	m_lockstep.lock = [](mLockstep* lockstep) {
		MultiplayerController* controller = static_cast<MultiplayerController*>(lockstep->context);
		controller->m_lock.lock();
	};
	m_lockstep.unlock = [](mLockstep* lockstep) {
		MultiplayerController* controller = static_cast<MultiplayerController*>(lockstep->context);
		controller->m_lock.unlock();
	};
	m_lockstep.signal = [](mLockstep* lockstep, unsigned mask) {
		MultiplayerController* controller = static_cast<MultiplayerController*>(lockstep->context);
		Player* player = controller->player(0);
		bool woke = false;
		player->waitMask &= ~mask;
		if (!player->waitMask && player->awake < 1) {
			mCoreThreadStopWaiting(player->controller->thread());
			player->awake = 1;
			woke = true;
		}
		return woke;
	};
	m_lockstep.wait = [](mLockstep* lockstep, unsigned mask) {
		MultiplayerController* controller = static_cast<MultiplayerController*>(lockstep->context);
		Player* player = controller->player(0);
		bool slept = false;
		player->waitMask |= mask;
		if (player->awake > 0) {
			mCoreThreadWaitFromThread(player->controller->thread());
			player->awake = 0;
			slept = true;
		}
		player->controller->setSync(true);
		return slept;
	};
	m_lockstep.addCycles = [](mLockstep* lockstep, int id, int32_t cycles) {
		if (cycles < 0) {
			abort();
		}
		MultiplayerController* controller = static_cast<MultiplayerController*>(lockstep->context);
		Player* player = controller->player(id);
		switch (player->controller->platform()) {
#ifdef M_CORE_GBA
		case mPLATFORM_GBA:
			abort();
			break;
#endif
#ifdef M_CORE_GB
		case mPLATFORM_GB:
			if (!id) {
				player = controller->player(1);
				player->controller->setSync(false);
				player->cyclesPosted += cycles;
				if (player->awake < 1) {
					player->node.gb->nextEvent += player->cyclesPosted;
				}
				mCoreThreadStopWaiting(player->controller->thread());
				player->awake = 1;
			} else {
				player->controller->setSync(true);
				player->cyclesPosted += cycles;
			}
			break;
#endif
		default:
			break;
		}
	};
	m_lockstep.useCycles = [](mLockstep* lockstep, int id, int32_t cycles) {
		MultiplayerController* controller = static_cast<MultiplayerController*>(lockstep->context);
		Player* player = controller->player(id);
		player->cyclesPosted -= cycles;
		if (player->cyclesPosted <= 0) {
			mCoreThreadWaitFromThread(player->controller->thread());
			player->awake = 0;
		}
		cycles = player->cyclesPosted;
		return cycles;
	};
	m_lockstep.unusedCycles = [](mLockstep* lockstep, int id) {
		MultiplayerController* controller = static_cast<MultiplayerController*>(lockstep->context);
		Player* player = controller->player(id);
		auto cycles = player->cyclesPosted;
		return cycles;
	};
	m_lockstep.unload = [](mLockstep* lockstep, int id) {
		MultiplayerController* controller = static_cast<MultiplayerController*>(lockstep->context);
		if (id) {
			Player* player = controller->player(id);
			player->controller->setSync(true);
			player->cyclesPosted = 0;

			// release master GBA if it is waiting for this GBA
			player = controller->player(0);
			player->waitMask &= ~(1 << id);
			if (!player->waitMask && player->awake < 1) {
				mCoreThreadStopWaiting(player->controller->thread());
				player->awake = 1;
			}
		} else {
			for (int i = 1; i < controller->m_players.count(); ++i) {
				Player* player = controller->player(i);
				player->controller->setSync(true);
				switch (player->controller->platform()) {
#ifdef M_CORE_GBA
				case mPLATFORM_GBA:
					break;
#endif
#ifdef M_CORE_GB
				case mPLATFORM_GB:
					player->cyclesPosted += reinterpret_cast<GBSIOLockstep*>(lockstep)->players[0]->eventDiff;
					break;
#endif
				default:
					break;
				}
				if (player->awake < 1) {
					switch (player->controller->platform()) {
#ifdef M_CORE_GBA
					case mPLATFORM_GBA:
						break;
#endif
#ifdef M_CORE_GB
					case mPLATFORM_GB:
						player->node.gb->nextEvent += player->cyclesPosted;
						break;
#endif
					default:
						break;
					}
					mCoreThreadStopWaiting(player->controller->thread());
					player->awake = 1;
				}
			}
		}
	};
}

MultiplayerController::~MultiplayerController() {
	stopRemoteSession();
	mLockstepDeinit(&m_lockstep);
	deinitBackend();
}

bool MultiplayerController::initBackendForPlatform(mPlatform platform) {
	switch (platform) {
#ifdef M_CORE_GBA
	case mPLATFORM_GBA:
		// Local-only backend path today: this coordinator drives same-process lockstep link.
		// Remote-session backend events will eventually augment or replace this initialization path.
		GBASIOLockstepCoordinatorInit(&m_gbaCoordinator);
		return true;
#endif
#ifdef M_CORE_GB
	case mPLATFORM_GB:
		GBSIOLockstepInit(&m_gbLockstep);
		return true;
#endif
	default:
		return false;
	}
}

void MultiplayerController::deinitBackend() {
	switch (m_platform) {
#ifdef M_CORE_GBA
	case mPLATFORM_GBA:
		GBASIOLockstepCoordinatorDeinit(&m_gbaCoordinator);
		break;
#endif
	default:
		break;
	}
}

void MultiplayerController::setPlayerAttached(Player& player, bool attached) {
	player.attached = attached;
}

bool MultiplayerController::attachPlayerToBackend(Player& player, bool delayedAttach) {
	switch (player.controller->platform()) {
#ifdef M_CORE_GBA
	case mPLATFORM_GBA: {
		if (delayedAttach) {
			return true;
		}
		struct mCore* core = player.controller->thread()->core;
		if (player.remoteBackend) {
			core->setPeripheral(core, mPERIPH_GBA_LINK_PORT, &player.node.gbaNet->d);
		} else {
			// Local-only attach path: node is attached to the lockstep coordinator and then exposed as
			// the link-port peripheral. Future remote session backends should call setPlayerAttached()
			// from their own lifecycle events without requiring this coordinator call.
			GBASIOLockstepCoordinatorAttach(&m_gbaCoordinator, player.node.gba);
			core->setPeripheral(core, mPERIPH_GBA_LINK_PORT, &player.node.gba->d);
		}
		setPlayerAttached(player, true);
		return true;
	}
#endif
#ifdef M_CORE_GB
	case mPLATFORM_GB:
		setPlayerAttached(player, true);
		return true;
#endif
	default:
		return false;
	}
}

void MultiplayerController::detachPlayerFromBackend(Player& player, mCoreThread* thread) {
	switch (player.controller->platform()) {
#ifdef M_CORE_GBA
	case mPLATFORM_GBA: {
		GBA* gba = static_cast<GBA*>(thread->core->board);
		GBASIODriver* node = gba->sio.driver;
		GBASIODriver* playerNode = player.remoteBackend ? &player.node.gbaNet->d : &player.node.gba->d;
		if (node == playerNode) {
			thread->core->setPeripheral(thread->core, mPERIPH_GBA_LINK_PORT, NULL);
		}
		if (player.attached && !player.remoteBackend) {
			// Local-only detach path: coordinator membership currently tracks active players.
			// Remote backend disconnect events should eventually feed into this shared state transition.
			GBASIOLockstepCoordinatorDetach(&m_gbaCoordinator, player.node.gba);
		}
		setPlayerAttached(player, false);
		break;
	}
#endif
#ifdef M_CORE_GB
	case mPLATFORM_GB:
		setPlayerAttached(player, false);
		break;
#endif
	default:
		break;
	}
}

void MultiplayerController::clearRemoteSessionBookkeeping() {
	m_remotePlayerCount = 0;
	m_remotePlayerId = -1;
	m_pendingRemoteRoomAction = PendingRemoteRoomAction::None;
}

bool MultiplayerController::isRemoteNetDriverActive() const {
	return m_remoteSession && m_platform == mPLATFORM_GBA;
}

int MultiplayerController::parseRemotePlayerId(const QString& peerId) const {
	bool ok = false;
	const int parsedId = peerId.toInt(&ok);
	return ok ? parsedId : -1;
}

bool MultiplayerController::startRemoteSession(std::unique_ptr<Netplay::Session> session) {
	if (!session || m_remoteSession || !m_pids.isEmpty()) {
		return false;
	}
	clearRemoteSessionBookkeeping();
	m_remoteSession = std::move(session);

	Netplay::SessionCallbacks callbacks;
	callbacks.onStateChanged = [this](Netplay::SessionState state) {
		onRemoteSessionStateChanged(state);
	};
	callbacks.onPeerJoined = [this](const Netplay::SessionPeer& peer) {
		onRemoteSessionPeerJoined(peer);
	};
	callbacks.onPeerLeft = [this](const Netplay::SessionPeer& peer) {
		onRemoteSessionPeerLeft(peer);
	};
	callbacks.onProtocolError = [this](const Netplay::SessionProtocolError& error) {
		onRemoteSessionProtocolError(error);
	};
	callbacks.onInboundLinkEvent = [this](const Netplay::SessionEventEnvelope& event) {
		onRemoteSessionInboundLinkEvent(event);
	};
	m_remoteSession->setCallbacks(std::move(callbacks));
	m_remoteDriverBridge.reset(new Netplay::DriverEventQueueBridge());
	startRemoteDriverDispatcher();
	refreshRemoteSessionBookkeepingFromSession();
	return true;
}

void MultiplayerController::stopRemoteSession() {
	if (!m_remoteSession) {
		return;
	}
	stopRemoteDriverDispatcher();
	m_remoteSession->disconnect();
	m_remoteSession.reset();
	m_remoteDriverBridge.reset();
	clearRemoteSessionBookkeeping();
}

bool MultiplayerController::isRemoteSessionActive() const {
	return !!m_remoteSession;
}

int MultiplayerController::remotePlayerCount() const {
	return m_remotePlayerCount;
}

int MultiplayerController::remotePlayerId() const {
	return m_remotePlayerId;
}

void MultiplayerController::loadRemoteSessionConfig(const ConfigController* config) {
	if (!config) {
		return;
	}

	RemoteSessionConfig loaded;
	loaded.host = config->getOption(ConfigController::NETPLAY_SERVER_HOST_KEY, QStringLiteral("127.0.0.1"));
	bool ok = false;
	const int portValue = config->getOption(ConfigController::NETPLAY_SERVER_PORT_KEY, 5000).toInt(&ok);
	if (ok && portValue > 0 && portValue <= 65535) {
		loaded.port = static_cast<quint16>(portValue);
	}
	loaded.room = config->getOption(ConfigController::NETPLAY_ROOM_KEY);
	loaded.sharedSecret = config->getOption(ConfigController::NETPLAY_SHARED_SECRET_KEY);
	setRemoteSessionConfig(std::move(loaded));
}

void MultiplayerController::saveRemoteSessionConfig(ConfigController* config) const {
	if (!config) {
		return;
	}

	config->setOption(ConfigController::NETPLAY_SERVER_HOST_KEY, m_remoteSessionConfig.host);
	config->setOption(ConfigController::NETPLAY_SERVER_PORT_KEY, static_cast<unsigned>(m_remoteSessionConfig.port));
	config->setOption(ConfigController::NETPLAY_ROOM_KEY, m_remoteSessionConfig.room);
	config->setOption(ConfigController::NETPLAY_SHARED_SECRET_KEY, m_remoteSessionConfig.sharedSecret);
}

void MultiplayerController::setRemoteSessionConfig(RemoteSessionConfig config) {
	config.host = config.host.trimmed();
	config.room = config.room.trimmed();
	m_remoteSessionConfig = std::move(config);
}

bool MultiplayerController::startConfiguredRemoteSession(std::unique_ptr<Netplay::Session> session, bool createRoom) {
	if (!session || !m_remoteSessionConfig.hasServerEndpoint()) {
		return false;
	}

	if (!startRemoteSession(std::move(session))) {
		return false;
	}

	Netplay::SessionConnectRequest connectRequest;
	connectRequest.endpoint = m_remoteSessionConfig.endpoint();
	connectRequest.authToken = m_remoteSessionConfig.sharedSecret;
	connectRequest.options[QStringLiteral("host")] = m_remoteSessionConfig.host;
	connectRequest.options[QStringLiteral("port")] = m_remoteSessionConfig.port;
	connectRequest.options[QStringLiteral("room")] = m_remoteSessionConfig.room;
	connectRequest.options[QStringLiteral("roomMode")] = createRoom ? QStringLiteral("create") : QStringLiteral("join");

	if (!m_remoteSession->connect(connectRequest)) {
		emitControllerRemoteSessionError(20, QStringLiteral("Failed to connect remote session"), Netplay::NetplayErrorCategory::ConnectionFailure, QStringLiteral("connect"));
		stopRemoteSession();
		return false;
	}

	m_pendingRemoteRoomAction = createRoom ? PendingRemoteRoomAction::CreateRoom : PendingRemoteRoomAction::JoinRoom;
	return true;
}

void MultiplayerController::onRemoteSessionStateChanged(Netplay::SessionState state) {
	if (state == Netplay::SessionState::Disconnected || state == Netplay::SessionState::Error) {
		if (isRemoteNetDriverActive() && m_remoteDriverBridge) {
			m_remoteDriverBridge->enqueueSessionFailure(GBA_SIO_NET_FAIL_DISCONNECTED, 0, 0);
		}
		clearRemoteSessionBookkeeping();
		return;
	}

	if (state == Netplay::SessionState::Connected) {
		dispatchPendingRemoteRoomAction();
	}

	refreshRemoteSessionBookkeepingFromSession();
}


void MultiplayerController::emitControllerRemoteSessionError(int code, const QString& message, Netplay::NetplayErrorCategory category, const QString& action) {
	Netplay::SessionProtocolError error;
	error.code = code;
	error.message = message;
	error.category = category;
	error.layer = Netplay::NetplayFailureLayer::ControllerIntegration;
	error.endpoint = m_remoteSessionConfig.endpoint();
	error.roomId = m_remoteSessionConfig.room;
	error.details[QStringLiteral("action")] = action;
	onRemoteSessionProtocolError(error);
}

void MultiplayerController::dispatchPendingRemoteRoomAction() {
	if (!m_remoteSession || m_pendingRemoteRoomAction == PendingRemoteRoomAction::None) {
		return;
	}

	const PendingRemoteRoomAction action = m_pendingRemoteRoomAction;
	m_pendingRemoteRoomAction = PendingRemoteRoomAction::None;

	if (action == PendingRemoteRoomAction::CreateRoom) {
		Netplay::SessionCreateRoomRequest request;
		request.roomName = m_remoteSessionConfig.room;
		request.maxPeers = 4;
		if (!m_remoteSession->createRoom(request)) {
			emitControllerRemoteSessionError(21, QStringLiteral("Failed to create room"), Netplay::NetplayErrorCategory::RoomRejectedOrFull, QStringLiteral("createRoom"));
			stopRemoteSession();
		}
		return;
	}

	Netplay::SessionJoinRoomRequest request;
	request.roomId = m_remoteSessionConfig.room;
	if (!m_remoteSession->joinRoom(request)) {
		emitControllerRemoteSessionError(22, QStringLiteral("Failed to join room"), Netplay::NetplayErrorCategory::RoomRejectedOrFull, QStringLiteral("joinRoom"));
		stopRemoteSession();
	}
}
void MultiplayerController::onRemoteSessionPeerJoined(const Netplay::SessionPeer& peer) {
	if (isRemoteNetDriverActive() && m_remoteDriverBridge) {
		const int playerId = parseRemotePlayerId(peer.peerId);
		if (playerId >= 0 && playerId < MAX_GBAS) {
			m_remoteDriverBridge->enqueuePeerAttach(playerId, 0);
		}
	}
	refreshRemoteSessionBookkeepingFromSession();
}

void MultiplayerController::onRemoteSessionPeerLeft(const Netplay::SessionPeer& peer) {
	if (isRemoteNetDriverActive() && m_remoteDriverBridge) {
		const int playerId = parseRemotePlayerId(peer.peerId);
		if (playerId >= 0 && playerId < MAX_GBAS) {
			m_remoteDriverBridge->enqueuePeerDetach(playerId, 0);
		}
	}
	refreshRemoteSessionBookkeepingFromSession();
}

void MultiplayerController::onRemoteSessionProtocolError(const Netplay::SessionProtocolError& error) {
	const QString endpoint = error.endpoint.isEmpty() ? m_remoteSessionConfig.endpoint() : error.endpoint;
	const QString roomId = !error.roomId.isEmpty() ? error.roomId : m_remoteSessionConfig.room;
	const QString sequence = (error.sequence >= 0) ? QString::number(error.sequence) : QStringLiteral("n/a");
	const QString expectedSequence = (error.expectedSequence >= 0) ? QString::number(error.expectedSequence) : QStringLiteral("n/a");

	LOG(QT, ERROR) << tr("Remote netplay failure [layer=%0 category=%1 code=%2] message=%3 endpoint=%4 room=%5 sequence=%6 expectedSequence=%7")
		.arg(_layerTag(Netplay::NetplayFailureLayer::ControllerIntegration))
		.arg(_categoryTag(error.category))
		.arg(error.code)
		.arg(error.message)
		.arg(endpoint)
		.arg(roomId)
		.arg(sequence)
		.arg(expectedSequence);

	if (error.layer != Netplay::NetplayFailureLayer::ControllerIntegration) {
		LOG(QT, ERROR) << tr("Remote netplay origin layer=%0 endpoint=%1 room=%2 sequence=%3 expectedSequence=%4")
			.arg(_layerTag(error.layer))
			.arg(endpoint)
			.arg(roomId)
			.arg(sequence)
			.arg(expectedSequence);
	}

	const QString uiState = _uiRemoteStateMessage(error);
	LOG(QT, ERROR) << uiState;

	if (isRemoteNetDriverActive() && m_remoteDriverBridge) {
		m_remoteDriverBridge->enqueueSessionFailure(_mapFailureKindFromError(error), error.code, error.sequence);
		LOG(QT, INFO) << tr("Remote inbound queue depth=%0 after failure")
			.arg(static_cast<qulonglong>(m_remoteDriverBridge->pendingInboundDepth()));
	}
	clearRemoteSessionBookkeeping();
}

void MultiplayerController::onRemoteSessionInboundLinkEvent(const Netplay::SessionEventEnvelope& event) {
	if (!isRemoteNetDriverActive() || !m_remoteDriverBridge) {
		return;
	}
	if (event.type != Netplay::SessionEventType::LinkInput) {
		return;
	}
	GBASIONetEvent decoded = {};
	if (!_decodeDriverEventPayload(event.payload, &decoded)) {
		emitControllerRemoteSessionError(23, QStringLiteral("Malformed inbound link event payload"), Netplay::NetplayErrorCategory::MalformedMessage, QStringLiteral("decodeLinkInput"));
		if (m_remoteDriverBridge) {
			m_remoteDriverBridge->enqueueSessionFailure(GBA_SIO_NET_FAIL_PROTOCOL, 23, event.sequence);
		}
		return;
	}

	switch (decoded.type) {
	case GBA_SIO_NET_EV_TRANSFER_RESULT:
		m_remoteDriverBridge->enqueueTransferResult(decoded.senderPlayerId, decoded.transferResult.playerId, decoded.sequence, decoded.transferResult.tickMarker,
			QByteArray(reinterpret_cast<const char*>(decoded.transferResult.payload), static_cast<int>(decoded.transferResult.payloadSize)));
		break;
	case GBA_SIO_NET_EV_MODE_SET:
		m_remoteDriverBridge->enqueueModeSet(decoded.senderPlayerId, decoded.modeSet.playerId, decoded.modeSet.mode, decoded.sequence);
		break;
	case GBA_SIO_NET_EV_TRANSFER_START:
		m_remoteDriverBridge->enqueueTransferStart(decoded.senderPlayerId, decoded.transferStart.playerId, decoded.transferStart.mode, decoded.transferStart.finishCycle, decoded.sequence);
		break;
	case GBA_SIO_NET_EV_HARD_SYNC:
		m_remoteDriverBridge->enqueueHardSync(decoded.senderPlayerId, decoded.hardSync.tickMarker, decoded.sequence);
		break;
	case GBA_SIO_NET_EV_PEER_ATTACH:
		m_remoteDriverBridge->enqueuePeerAttach(decoded.senderPlayerId, decoded.peerAttach.playerId, decoded.sequence);
		break;
	case GBA_SIO_NET_EV_PEER_DETACH:
		m_remoteDriverBridge->enqueuePeerDetach(decoded.senderPlayerId, decoded.peerDetach.playerId, decoded.sequence);
		break;
	case GBA_SIO_NET_EV_SESSION_FAILURE:
		m_remoteDriverBridge->enqueueSessionFailure(decoded.senderPlayerId, decoded.sessionFailure.kind, decoded.sessionFailure.code, decoded.sequence);
		break;
	default:
		emitControllerRemoteSessionError(24, QStringLiteral("Unsupported inbound link event type"), Netplay::NetplayErrorCategory::MalformedMessage, QStringLiteral("routeLinkInput"));
		m_remoteDriverBridge->enqueueSessionFailure(GBA_SIO_NET_FAIL_PROTOCOL, 24, event.sequence);
		return;
	}
	LOG(QT, INFO) << tr("Remote inbound queue depth=%0 sequence=%1 serverSequence=%2")
		.arg(static_cast<qulonglong>(m_remoteDriverBridge->pendingInboundDepth()))
		.arg(event.sequence)
		.arg(event.serverSequence >= 0 ? QString::number(event.serverSequence) : QStringLiteral("n/a"));
}


bool MultiplayerController::dispatchRemoteOutboundDriverEvent(const GBASIONetEvent& event) {
	if (!m_remoteSession) {
		return false;
	}
	const Netplay::SessionEventEnvelope envelope = mapOutboundDriverEventEnvelope(event);
	if (m_remoteSession->sendEvent(envelope)) {
		return true;
	}
	if (m_remoteDriverBridge) {
		m_remoteDriverBridge->enqueueSessionFailure(GBA_SIO_NET_FAIL_DISCONNECTED, 0, event.sequence);
	}
	return false;
}

Netplay::SessionEventEnvelope MultiplayerController::mapOutboundDriverEventEnvelope(const GBASIONetEvent& event) const {
	Netplay::SessionEventEnvelope envelope;
	envelope.protocolVersion = 1;
	envelope.roomId = m_remoteSession ? m_remoteSession->room().roomId : QString();
	envelope.sourcePeerId = QString::number(event.senderPlayerId);
	envelope.sequence = event.sequence;
	envelope.type = Netplay::SessionEventType::LinkInput;
	envelope.payload = _encodeDriverEventPayload(event);
	envelope.metadata.insert(QStringLiteral("gbaNetEventType"), static_cast<int>(event.type));
	return envelope;
}

bool MultiplayerController::dispatchRemoteOutboundDriverEvents() {
	if (!isRemoteNetDriverActive() || !m_remoteDriverBridge) {
		return false;
	}
	bool sentAny = false;
	GBASIONetEvent event = {};
	while (m_remoteDriverBridge->tryDequeueOutbound(&event)) {
		sentAny = true;
		dispatchRemoteOutboundDriverEvent(event);
	}
	return sentAny;
}

void MultiplayerController::startRemoteDriverDispatcher() {
	stopRemoteDriverDispatcher();
	m_remoteDriverDispatchStop = false;
	m_remoteDriverDispatchThread = std::thread([this]() {
		while (!m_remoteDriverDispatchStop) {
			const bool sentAny = dispatchRemoteOutboundDriverEvents();
			if (!sentAny) {
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
		}
	});
}

void MultiplayerController::stopRemoteDriverDispatcher() {
	m_remoteDriverDispatchStop = true;
	if (m_remoteDriverDispatchThread.joinable()) {
		m_remoteDriverDispatchThread.join();
	}
}

void MultiplayerController::refreshRemoteSessionBookkeepingFromSession() {
	if (!m_remoteSession) {
		clearRemoteSessionBookkeeping();
		return;
	}
	const Netplay::SessionRoom room = m_remoteSession->room();
	const Netplay::SessionPeer localPeer = m_remoteSession->localPeer();
	m_remotePlayerCount = room.peers.size();
	bool hasLocalPeer = false;
	for (const auto& peer : room.peers) {
		if (peer.peerId == localPeer.peerId) {
			hasLocalPeer = true;
			break;
		}
	}
	if (!localPeer.peerId.isEmpty() && !hasLocalPeer) {
		++m_remotePlayerCount;
	}
	bool ok = false;
	const int parsedId = localPeer.peerId.toInt(&ok);
	m_remotePlayerId = ok ? parsedId : -1;
}

bool MultiplayerController::attachGame(CoreController* controller) {
	QList<CoreController::Interrupter> interrupters;
	interrupters.append(controller);
	for (Player& p : m_pids.values()) {
		interrupters.append(p.controller);
	}

	bool doDelayedAttach = false;
	if (m_platform == mPLATFORM_NONE) {
		if (!initBackendForPlatform(controller->platform())) {
			return false;
		}
		m_platform = controller->platform();
	} else if (controller->platform() != m_platform) {
		return false;
	}

	mCoreThread* thread = controller->thread();
	if (!thread) {
		return false;
	}

	Player player{controller};
	for (int i = 0; i < MAX_GBAS; ++i) {
		if (m_claimedIds & (1 << i)) {
			continue;
		}
		player.preferredId = i;
		m_claimedIds |= 1 << i;
		break;
	}
	switch (controller->platform()) {
#ifdef M_CORE_GBA
	case mPLATFORM_GBA: {
		if (m_remoteSession && attached() >= 1) {
			return false;
		}
		if (!m_remoteSession && attached() >= MAX_GBAS) {
			return false;
		}
		if (m_remoteSession) {
			GBASIONetDriver* node = new GBASIONetDriver;
			GBASIONetDriverCreate(node);
			if (m_remoteDriverBridge) {
				GBASIONetDriverSetQueues(node, m_remoteDriverBridge->outboundQueue(), m_remoteDriverBridge->inboundQueue());
			}
			node->state = GBA_SIO_NET_IN_ROOM;
			node->localPlayerId = (m_remotePlayerId >= 0 && m_remotePlayerId < MAX_GBAS) ? m_remotePlayerId : 0;
			node->roomPlayerCount = std::clamp(m_remotePlayerCount, 1, MAX_GBAS);
			node->attachedPlayerMask = (1U << node->localPlayerId);
			if (m_remoteSession) {
				const auto room = m_remoteSession->room();
				for (const auto& peer : room.peers) {
					const int playerId = parseRemotePlayerId(peer.peerId);
					if (playerId >= 0 && playerId < MAX_GBAS) {
						node->attachedPlayerMask |= (1U << playerId);
					}
				}
			}
			player.node.gbaNet = node;
			player.remoteBackend = true;
		} else {
			GBASIOLockstepDriver* node = new GBASIOLockstepDriver;
			LockstepUser* user = new LockstepUser;
			mLockstepThreadUserInit(user, thread);
			user->controller = this;
			user->pid = m_nextPid;
			user->d.requestedId = [](mLockstepUser* ctx) {
				mLockstepThreadUser* tctx = reinterpret_cast<mLockstepThreadUser*>(ctx);
				LockstepUser* user = static_cast<LockstepUser*>(tctx);
				MultiplayerController* controller = user->controller;
				const auto iter = controller->m_pids.find(user->pid);
				if (iter == controller->m_pids.end()) {
					return -1;
				}
				const Player& p = iter.value();
				return p.preferredId;
			};

			GBASIOLockstepDriverCreate(node, &user->d);
			player.node.gba = node;
			player.remoteBackend = false;

			if (m_pids.size()) {
				doDelayedAttach = true;
			}
		}
		break;
	}
#endif
#ifdef M_CORE_GB
	case mPLATFORM_GB: {
		if (attached() >= 2) {
			return false;
		}

		GB* gb = static_cast<GB*>(thread->core->board);

		GBSIOLockstepNode* node = new GBSIOLockstepNode;
		GBSIOLockstepNodeCreate(node);
		GBSIOLockstepAttachNode(&m_gbLockstep, node);
		player.node.gb = node;

		GBSIOSetDriver(&gb->sio, &node->d);
		attachPlayerToBackend(player, false);
		break;
	}
#endif
	default:
		return false;
	}

	QPair<QString, QString> path(controller->path(), controller->baseDirectory());
	int claimed = m_claimedSaves[path];

	int saveId = 0;
	mCoreConfigGetIntValue(&controller->thread()->core->config, "savePlayerId", &saveId);

	if (claimed) {
		player.saveId = 0;
		for (int i = 0; i < MAX_GBAS; ++i) {
			if (claimed & (1 << i)) {
				continue;
			}
			player.saveId = i + 1;
			break;
		}
		if (!player.saveId) {
			LOG(QT, ERROR) << tr("Couldn't find available save ID");
			player.saveId = 1;
		}
	} else if (saveId) {
		player.saveId = saveId;
	} else {
		player.saveId = 1;
	}
	m_claimedSaves[path] |= 1 << (player.saveId - 1);

	m_pids.insert(m_nextPid, player);
	++m_nextPid;
	fixOrder();

	if (doDelayedAttach) {
		for (auto pid: m_players) {
			Player& player = m_pids.find(pid).value();
			if (player.attached) {
				continue;
			}
			attachPlayerToBackend(player, false);
		}
	}

	emit gameAttached();
	return true;
}

void MultiplayerController::detachGame(CoreController* controller) {
	if (m_players.empty()) {
		return;
	}
	mCoreThread* thread = controller->thread();
	if (!thread) {
		return;
	}
	QList<CoreController::Interrupter> interrupters;

	int pid = -1;
	for (int i = 0; i < m_players.count(); ++i) {
		Player* p = player(i);
		if (!p) {
			continue;
		}
		CoreController* playerController = p->controller;
		if (playerController == controller) {
			pid = m_players[i];
		}
		interrupters.append(playerController);
	}
	if (pid < 0) {
		LOG(QT, WARN) << tr("Trying to detach a multiplayer player that's not attached");
		return;
	}
	Player& p = m_pids.find(pid).value();
	detachPlayerFromBackend(p, thread);
	switch (controller->platform()) {
#ifdef M_CORE_GBA
	case mPLATFORM_GBA:
		if (p.remoteBackend) {
			delete p.node.gbaNet;
		} else {
			delete reinterpret_cast<LockstepUser*>(p.node.gba->user);
			delete p.node.gba;
		}
		break;
#endif
#ifdef M_CORE_GB
	case mPLATFORM_GB: {
		GB* gb = static_cast<GB*>(thread->core->board);
		GBSIOLockstepNode* node = reinterpret_cast<GBSIOLockstepNode*>(gb->sio.driver);
		GBSIOSetDriver(&gb->sio, nullptr);
		if (node) {
			GBSIOLockstepDetachNode(&m_gbLockstep, node);
			delete node;
		}
		break;
	}
#endif
	default:
		break;
	}

	// TODO: This might change if we replace the ROM--make sure to handle this properly
	QPair<QString, QString> path(controller->path(), controller->baseDirectory());
	if (!p.saveId) {
		LOG(QT, WARN) << tr("Clearing invalid save ID");
	} else {
		m_claimedSaves[path] &= ~(1 << (p.saveId - 1));
		if (!m_claimedSaves[path]) {
			m_claimedSaves.remove(path);
		}
	}

	if (p.preferredId < 0) {
		LOG(QT, WARN) << tr("Clearing invalid preferred ID");
	} else {
		m_claimedIds &= ~(1 << p.preferredId);
	}

	m_pids.remove(pid);
	if (m_pids.size() == 0) {
		deinitBackend();
		m_platform = mPLATFORM_NONE;
	} else {
		fixOrder();
	}
	emit gameDetached();
}

int MultiplayerController::playerId(CoreController* controller) const {
	for (int i = 0; i < m_players.count(); ++i) {
		const Player* p = player(i);
		if (!p) {
			LOG(QT, ERROR) << tr("Trying to get player ID for a multiplayer player that's not attached");
			return -1;
		}
		if (p->controller == controller) {
			return i;
		}
	}
	return -1;
}

int MultiplayerController::saveId(CoreController* controller) const {
	for (int i = 0; i < m_players.count(); ++i) {
		const Player* p = player(i);
		if (!p) {
			LOG(QT, ERROR) << tr("Trying to get save ID for a multiplayer player that's not attached");
			return -1;
		}
		if (p->controller == controller) {
			return p->saveId;
		}
	}
	return -1;
}

int MultiplayerController::attached() {
	int num = 0;
	switch (m_platform) {
	case mPLATFORM_GB:
		num = m_lockstep.attached;
		break;
	case mPLATFORM_GBA:
		num = saturateCast<int>(GBASIOLockstepCoordinatorAttached(&m_gbaCoordinator));
		break;
	default:
		break;
	}
	return num;
}

MultiplayerController::Player* MultiplayerController::player(int id) {
	if (id >= m_players.size()) {
		return nullptr;
	}
	int pid = m_players[id];
	auto iter = m_pids.find(pid);
	if (iter == m_pids.end()) {
		return nullptr;
	}
	return &iter.value();
}

const MultiplayerController::Player* MultiplayerController::player(int id) const {
	if (id >= m_players.size()) {
		return nullptr;
	}
	int pid = m_players[id];
	auto iter = m_pids.find(pid);
	if (iter == m_pids.end()) {
		return nullptr;
	}
	return &iter.value();
}

void MultiplayerController::fixOrder() {
	m_players.clear();
	m_players = m_pids.keys();
	std::sort(m_players.begin(), m_players.end());
	switch (m_platform) {
#ifdef M_CORE_GBA
	case mPLATFORM_GBA:
		// TODO: fix
		/*for (int pid : m_pids.keys()) {
			Player& p = m_pids.find(pid).value();
			GBA* gba = static_cast<GBA*>(p.controller->thread()->core->board);
			GBASIOLockstepDriver* node = reinterpret_cast<GBASIOLockstepDriver*>(gba->sio.driver);
			m_players[node->d.deviceId(&node->d)] = pid;
		}*/
		break;
#endif
#ifdef M_CORE_GB
	case mPLATFORM_GB:
		if (player(0)->node.gb->id == 1) {
			std::swap(m_players[0], m_players[1]);
		}
		break;
#endif
	case mPLATFORM_NONE:
		break;
	}
}
