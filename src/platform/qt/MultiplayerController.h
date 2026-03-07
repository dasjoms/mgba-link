/* Copyright (c) 2013-2015 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#pragma once

#include <QHash>
#include <QList>
#include <QMutex>
#include <QObject>

#include <mgba/core/core.h>
#include <mgba/core/lockstep.h>
#ifdef M_CORE_GBA
#include <mgba/internal/gba/sio/lockstep.h>
#include <mgba/internal/gba/sio/net.h>
#endif
#ifdef M_CORE_GB
#include <mgba/internal/gb/sio/lockstep.h>
#endif

#include <atomic>
#include <memory>
#include <thread>

struct GBSIOLockstepNode;
struct GBASIOLockstepNode;

namespace QGBA {

namespace Netplay {
class DriverEventQueueBridge;
class Session;
enum class SessionState;
enum class NetplayErrorCategory;
struct SessionPeer;
struct SessionProtocolError;
struct SessionEventEnvelope;
}

class ConfigController;
class CoreController;

class MultiplayerController : public QObject {
Q_OBJECT

public:
	struct RemoteSessionConfig {
		QString host;
		quint16 port = 0;
		QString room;
		QString sharedSecret;

		QString endpoint() const;
		bool hasServerEndpoint() const;
	};

	MultiplayerController();
	~MultiplayerController();

	bool attachGame(CoreController*);
	void detachGame(CoreController*);

	int attached();
	int playerId(CoreController*) const;
	int saveId(CoreController*) const;

	bool startRemoteSession(std::unique_ptr<Netplay::Session> session);
	void stopRemoteSession();
	bool isRemoteSessionActive() const;
	int remotePlayerCount() const;
	int remotePlayerId() const;

	void loadRemoteSessionConfig(const ConfigController* config);
	void saveRemoteSessionConfig(ConfigController* config) const;
	const RemoteSessionConfig& remoteSessionConfig() const { return m_remoteSessionConfig; }
	void setRemoteSessionConfig(RemoteSessionConfig config);
	bool startConfiguredRemoteSession(std::unique_ptr<Netplay::Session> session, bool createRoom);

signals:
	void gameAttached();
	void gameDetached();
	void remoteSessionStatusChanged();
	void remoteSessionFailureNotified(const QString& state, const QString& userMessage, int code, const QString& category, bool terminal);

private:
	union Node {
		GBSIOLockstepNode* gb;
		GBASIOLockstepDriver* gba;
		GBASIONetDriver* gbaNet;
	};
	struct Player {
		Player(CoreController* controller);

		int id() const;
		bool operator<(const Player&) const;

		CoreController* controller;
		Node node = {nullptr};
		int awake = 1;
		int32_t cyclesPosted = 0;
		unsigned waitMask = 0;
		int saveId = 1;
		int preferredId = 0;
		bool attached = false;
		bool remoteBackend = false;
	};
	struct LockstepUser : mLockstepThreadUser {
		MultiplayerController* controller;
		int pid;
	};

	Player* player(int id);
	const Player* player(int id) const;
	void fixOrder();

	bool initBackendForPlatform(mPlatform platform);
	void deinitBackend();
	bool attachPlayerToBackend(Player& player, bool delayedAttach);
	void detachPlayerFromBackend(Player& player, mCoreThread* thread);
	void setPlayerAttached(Player& player, bool attached);
	void clearRemoteSessionBookkeeping();
	void onRemoteSessionStateChanged(Netplay::SessionState state);
	void onRemoteSessionPeerJoined(const Netplay::SessionPeer& peer);
	void onRemoteSessionPeerLeft(const Netplay::SessionPeer& peer);
	void onRemoteSessionProtocolError(const Netplay::SessionProtocolError& error);
	void onRemoteSessionInboundLinkEvent(const Netplay::SessionEventEnvelope& event);
	void refreshRemoteSessionBookkeepingFromSession();
	void dispatchPendingRemoteRoomAction();
	void emitControllerRemoteSessionError(int code, const QString& message, Netplay::NetplayErrorCategory category, const QString& action);
	bool dispatchRemoteOutboundDriverEvents();
	bool dispatchRemoteOutboundDriverEvent(const GBASIONetEvent& event);
	Netplay::SessionEventEnvelope mapOutboundDriverEventEnvelope(const GBASIONetEvent& event) const;
	void startRemoteDriverDispatcher();
	void stopRemoteDriverDispatcher();
	bool isRemoteNetDriverActive() const;
	int parseRemotePlayerId(const QString& peerId) const;

	enum class PendingRemoteRoomAction {
		None,
		CreateRoom,
		JoinRoom,
	};

	union {
		mLockstep m_lockstep;
#ifdef M_CORE_GB
		GBSIOLockstep m_gbLockstep;
#endif
	};

#ifdef M_CORE_GBA
	GBASIOLockstepCoordinator m_gbaCoordinator;
#endif

	mPlatform m_platform = mPLATFORM_NONE;
	int m_nextPid = 0;
	int m_claimedIds = 0;
	QHash<int, Player> m_pids;
	QList<int> m_players;
	QMutex m_lock;
	QHash<QPair<QString, QString>, int> m_claimedSaves;
	std::unique_ptr<Netplay::Session> m_remoteSession;
	int m_remotePlayerCount = 0;
	int m_remotePlayerId = -1;
	PendingRemoteRoomAction m_pendingRemoteRoomAction = PendingRemoteRoomAction::None;
	RemoteSessionConfig m_remoteSessionConfig;
	std::unique_ptr<Netplay::DriverEventQueueBridge> m_remoteDriverBridge;
	std::atomic<bool> m_remoteDriverDispatchStop = false;
	std::thread m_remoteDriverDispatchThread;
	bool m_remoteFailureNotified = false;
};

}
