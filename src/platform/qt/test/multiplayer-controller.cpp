/* Copyright (c) 2013-2015 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "platform/qt/MultiplayerController.h"
#include "platform/qt/CoreController.h"
#include "platform/qt/Log.h"
#include "platform/qt/netplay/Session.h"

#include <QTest>

using namespace QGBA;
using namespace QGBA::Netplay;


namespace QGBA {

mPlatform CoreController::platform() const {
	return mPLATFORM_GBA;
}

void CoreController::setSync(bool) {
}

Log* Log::s_target = nullptr;

Log::Stream::Stream(Log* target, int level, int category)
	: m_level(level)
	, m_category(category)
	, m_log(target) {
}

Log::Stream::~Stream() {
}

Log::Stream& Log::Stream::operator<<(const QString&) {
	return *this;
}

Log::Stream Log::log(int level, int category) {
	return Stream(s_target, level, category);
}

void Log::setDefaultTarget(Log* target) {
	s_target = target;
}

Log::Log() {
}

Log::~Log() {
}

void Log::postLog(int, int, const QString&) {
}

}

int _mLOG_CAT_QT = 0;

namespace {

class FakeSession final : public Session {
public:
	SessionState state() const override { return m_state; }
	SessionRoom room() const override { return m_room; }
	SessionPeer localPeer() const override { return m_localPeer; }

	void setCallbacks(SessionCallbacks callbacks) override { m_callbacks = std::move(callbacks); }

	bool connect(const SessionConnectRequest& request) override {
		++connectCalls;
		lastConnectRequest = request;
		return connectResult;
	}

	void disconnect() override {
		++disconnectCalls;
		m_state = SessionState::Disconnected;
	}

	bool createRoom(const SessionCreateRoomRequest& request) override {
		++createRoomCalls;
		lastCreateRoomRequest = request;
		return createRoomResult;
	}

	bool joinRoom(const SessionJoinRoomRequest& request) override {
		++joinRoomCalls;
		lastJoinRoomRequest = request;
		return joinRoomResult;
	}

	void leaveRoom() override {}
	bool sendEvent(const SessionEventEnvelope&) override { return true; }

	void emitState(SessionState state) {
		m_state = state;
		if (m_callbacks.onStateChanged) {
			m_callbacks.onStateChanged(state);
		}
	}

	bool connectResult = true;
	bool createRoomResult = true;
	bool joinRoomResult = true;

	int connectCalls = 0;
	int disconnectCalls = 0;
	int createRoomCalls = 0;
	int joinRoomCalls = 0;
	SessionConnectRequest lastConnectRequest;
	SessionCreateRoomRequest lastCreateRoomRequest;
	SessionJoinRoomRequest lastJoinRoomRequest;

private:
	SessionCallbacks m_callbacks;
	SessionState m_state = SessionState::Disconnected;
	SessionRoom m_room;
	SessionPeer m_localPeer;
};

} // namespace

class MultiplayerControllerNetplayTest : public QObject {
Q_OBJECT

private:
	MultiplayerController::RemoteSessionConfig config() const {
		MultiplayerController::RemoteSessionConfig cfg;
		cfg.host = QStringLiteral("127.0.0.1");
		cfg.port = 5000;
		cfg.room = QStringLiteral("room-1");
		cfg.sharedSecret = QStringLiteral("secret");
		return cfg;
	}

private slots:
	void createRoomHappyPath() {
		MultiplayerController controller;
		controller.setRemoteSessionConfig(config());

		auto session = std::make_unique<FakeSession>();
		FakeSession* raw = session.get();
		QVERIFY(controller.startConfiguredRemoteSession(std::move(session), true));
		QCOMPARE(raw->connectCalls, 1);
		QCOMPARE(raw->createRoomCalls, 0);

		raw->emitState(SessionState::Connected);
		QCOMPARE(raw->createRoomCalls, 1);
		QCOMPARE(raw->lastCreateRoomRequest.roomName, QStringLiteral("room-1"));
		QCOMPARE(raw->lastCreateRoomRequest.maxPeers, 4);
	}

	void joinRoomHappyPath() {
		MultiplayerController controller;
		controller.setRemoteSessionConfig(config());

		auto session = std::make_unique<FakeSession>();
		FakeSession* raw = session.get();
		QVERIFY(controller.startConfiguredRemoteSession(std::move(session), false));
		QCOMPARE(raw->connectCalls, 1);
		QCOMPARE(raw->joinRoomCalls, 0);

		raw->emitState(SessionState::Connected);
		QCOMPARE(raw->joinRoomCalls, 1);
		QCOMPARE(raw->lastJoinRoomRequest.roomId, QStringLiteral("room-1"));
	}

	void roomActionFailureStopsSession() {
		MultiplayerController controller;
		controller.setRemoteSessionConfig(config());

		auto session = std::make_unique<FakeSession>();
		FakeSession* raw = session.get();
		raw->createRoomResult = false;
		QVERIFY(controller.startConfiguredRemoteSession(std::move(session), true));
		QVERIFY(controller.isRemoteSessionActive());

		raw->emitState(SessionState::Connected);
		QCOMPARE(raw->createRoomCalls, 1);
		QCOMPARE(raw->disconnectCalls, 1);
		QVERIFY(!controller.isRemoteSessionActive());
	}

	void reconnectDoesNotDuplicateRoomAction() {
		MultiplayerController controller;
		controller.setRemoteSessionConfig(config());

		auto session = std::make_unique<FakeSession>();
		FakeSession* raw = session.get();
		QVERIFY(controller.startConfiguredRemoteSession(std::move(session), false));

		raw->emitState(SessionState::Connected);
		QCOMPARE(raw->joinRoomCalls, 1);

		raw->emitState(SessionState::Connected);
		QCOMPARE(raw->joinRoomCalls, 1);
	}
};

QTEST_APPLESS_MAIN(MultiplayerControllerNetplayTest)
#include "multiplayer-controller.moc"
