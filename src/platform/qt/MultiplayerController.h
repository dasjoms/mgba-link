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
#endif
#ifdef M_CORE_GB
#include <mgba/internal/gb/sio/lockstep.h>
#endif

#include <memory>

struct GBSIOLockstepNode;
struct GBASIOLockstepNode;

namespace QGBA {

namespace Netplay {
class Session;
enum class SessionState;
struct SessionPeer;
struct SessionProtocolError;
}

class CoreController;

class MultiplayerController : public QObject {
Q_OBJECT

public:
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

signals:
	void gameAttached();
	void gameDetached();

private:
	union Node {
		GBSIOLockstepNode* gb;
		GBASIOLockstepDriver* gba;
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
	void refreshRemoteSessionBookkeepingFromSession();

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
};

}
