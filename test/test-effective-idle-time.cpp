// Sane Break is a gentle break reminder that helps you avoid mindlessly skipping breaks
// Copyright (C) 2026 Sane Break developers
// SPDX-License-Identifier: GPL-3.0-or-later

#include <qtestcase.h>

#include <QByteArray>
#include <QDateTime>
#include <QObject>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryFile>
#include <QTest>

#include "core/idle-time.h"
#include "core/preferences.h"
#include "lib/effective-idle-time.h"
#include "lib/peer-packet.h"
#include "lib/remote-activity-monitor.h"

namespace {

class MockIdleTime : public SystemIdleTime {
 public:
  using SystemIdleTime::SystemIdleTime;
  void startWatching() override { m_watching = true; }
  void stopWatching() override { m_watching = false; }
  void setWatchAccuracy(int a) override { m_lastAccuracy = a; }
  void setMinIdleTime(int ms) override { m_lastMinIdle = ms; }
  void triggerIdleStart() {
    m_isIdle = true;
    emit idleStart();
  }
  void triggerIdleEnd() {
    m_isIdle = false;
    emit idleEnd();
  }
  bool watching() const { return m_watching; }
  int lastAccuracy() const { return m_lastAccuracy; }
  int lastMinIdle() const { return m_lastMinIdle; }

 private:
  bool m_watching = false;
  int m_lastAccuracy = -1;
  int m_lastMinIdle = -1;
};

QByteArray makeSecret() {
  return QByteArray::fromHex(
      "0123456789abcdef0123456789abcdef"
      "0123456789abcdef0123456789abcdef");
}

QByteArray makeUuid(uint8_t prefix) {
  QByteArray u(16, '\0');
  u[0] = static_cast<char>(prefix);
  return u;
}

QByteArray activityBytes(const QByteArray& peerUuid, qint64 ts, quint64 nonce,
                         const QByteArray& secret) {
  peer::Packet p;
  p.senderUuid = peerUuid;
  p.hostname = "peer";
  p.timestamp = ts;
  p.nonce = nonce;
  p.eventType = peer::EVENT_ACTIVITY;
  p.payload = QByteArray(4, '\0');
  return peer::encodePacket(p, secret);
}

QByteArray idleBytes(const QByteArray& peerUuid, qint64 ts, quint64 nonce,
                     uint8_t state, const QByteArray& secret) {
  peer::Packet p;
  p.senderUuid = peerUuid;
  p.hostname = "peer";
  p.timestamp = ts;
  p.nonce = nonce;
  p.eventType = peer::EVENT_IDLE_TRANSITION;
  p.payload.append(static_cast<char>(state));
  return peer::encodePacket(p, secret);
}

}  // namespace

class TestEffectiveIdleTime : public QObject {
  Q_OBJECT

 private:
  QTemporaryFile* m_settingsFile = nullptr;
  SanePreferences* m_prefs = nullptr;

  peer::RemoteActivityMonitor* makeRam(MockIdleTime* idle) {
    auto* ram = new peer::RemoteActivityMonitor(m_prefs, idle);
    ram->testSetSecret(makeSecret());
    ram->testAllowInterface(3);
    return ram;
  }

 private slots:
  void init() {
    m_settingsFile = new QTemporaryFile();
    m_settingsFile->open();
    m_prefs = new SanePreferences(
        new QSettings(m_settingsFile->fileName(), QSettings::IniFormat));
  }
  void cleanup() {
    delete m_prefs;
    m_prefs = nullptr;
    delete m_settingsFile;
    m_settingsFile = nullptr;
  }

  // ---- No-peers pass-through -----------------------------------------

  void local_idle_no_peers_fires_idleStart() {
    MockIdleTime wrapped;
    EffectiveIdleTime eff(&wrapped, nullptr);
    QSignalSpy spyStart(&eff, &SystemIdleTime::idleStart);
    wrapped.triggerIdleStart();
    QCOMPARE(spyStart.count(), 1);
    QVERIFY(eff.isIdle());
  }

  void local_active_no_peers_fires_idleEnd() {
    MockIdleTime wrapped;
    wrapped.triggerIdleStart();
    EffectiveIdleTime eff(&wrapped, nullptr);
    QSignalSpy spyEnd(&eff, &SystemIdleTime::idleEnd);
    wrapped.triggerIdleEnd();
    QCOMPARE(spyEnd.count(), 1);
    QVERIFY(!eff.isIdle());
  }

  void duplicate_wrapped_edges_suppressed() {
    MockIdleTime wrapped;
    EffectiveIdleTime eff(&wrapped, nullptr);
    QSignalSpy spyStart(&eff, &SystemIdleTime::idleStart);
    QSignalSpy spyEnd(&eff, &SystemIdleTime::idleEnd);
    wrapped.triggerIdleStart();
    wrapped.triggerIdleStart();  // no duplicate edge
    QCOMPARE(spyStart.count(), 1);
    wrapped.triggerIdleEnd();
    wrapped.triggerIdleEnd();
    QCOMPARE(spyEnd.count(), 1);
  }

  void forwards_setters_to_wrapped() {
    MockIdleTime wrapped;
    EffectiveIdleTime eff(&wrapped, nullptr);
    eff.startWatching();
    QVERIFY(wrapped.watching());
    eff.setWatchAccuracy(42);
    QCOMPARE(wrapped.lastAccuracy(), 42);
    eff.setMinIdleTime(4200);
    QCOMPARE(wrapped.lastMinIdle(), 4200);
    eff.stopWatching();
    QVERIFY(!wrapped.watching());
  }

  // ---- Fusion with peers ---------------------------------------------

  void local_idle_peer_active_suppresses_idleStart() {
    MockIdleTime wrapped;
    auto* ram = makeRam(&wrapped);
    EffectiveIdleTime eff(&wrapped, ram);

    // Peer sends ACTIVITY — anyPeerActive becomes true.
    const QDateTime now = QDateTime::fromSecsSinceEpoch(1'700'000'000);
    ram->handleReceivedDatagram(
        activityBytes(makeUuid(0x55), now.toSecsSinceEpoch(), 1, makeSecret()),
        3, now);
    QVERIFY(ram->anyPeerActive());

    QSignalSpy spyStart(&eff, &SystemIdleTime::idleStart);
    wrapped.triggerIdleStart();  // local goes idle
    QCOMPARE(spyStart.count(), 0);  // peer active suppresses
    QVERIFY(!eff.isIdle());  // fused isIdle reflects peer active
    delete ram;
  }

  void peer_idle_while_local_idle_triggers_idleStart() {
    MockIdleTime wrapped;
    auto* ram = makeRam(&wrapped);
    EffectiveIdleTime eff(&wrapped, ram);

    // Peer active, local active initially.
    const QDateTime now = QDateTime::fromSecsSinceEpoch(1'700'000'000);
    ram->handleReceivedDatagram(
        activityBytes(makeUuid(0x55), now.toSecsSinceEpoch(), 1, makeSecret()),
        3, now);
    QVERIFY(ram->anyPeerActive());

    // Local goes idle; effective still active (peer suppressing).
    wrapped.triggerIdleStart();
    QVERIFY(!eff.isIdle());

    QSignalSpy spyStart(&eff, &SystemIdleTime::idleStart);
    // Peer sends IDLE_TRANSITION{idle} → aggregate flips to false → effective
    // idle becomes true → idleStart fires.
    const QDateTime later = now.addSecs(2);
    ram->handleReceivedDatagram(
        idleBytes(makeUuid(0x55), later.toSecsSinceEpoch(), 2, peer::STATE_IDLE,
                  makeSecret()),
        3, later);
    QCOMPARE(spyStart.count(), 1);
    QVERIFY(eff.isIdle());
    delete ram;
  }

  void peer_active_while_local_idle_fires_idleEnd() {
    MockIdleTime wrapped;
    auto* ram = makeRam(&wrapped);
    EffectiveIdleTime eff(&wrapped, ram);

    // Local goes idle, no peers active → effective idleStart.
    wrapped.triggerIdleStart();
    QVERIFY(eff.isIdle());

    QSignalSpy spyEnd(&eff, &SystemIdleTime::idleEnd);
    const QDateTime now = QDateTime::fromSecsSinceEpoch(1'700'000'000);
    ram->handleReceivedDatagram(
        activityBytes(makeUuid(0x55), now.toSecsSinceEpoch(), 1, makeSecret()),
        3, now);
    QCOMPARE(spyEnd.count(), 1);
    QVERIFY(!eff.isIdle());
    delete ram;
  }

  void isIdle_returns_fused_value() {
    MockIdleTime wrapped;
    auto* ram = makeRam(&wrapped);
    EffectiveIdleTime eff(&wrapped, ram);

    // Both active → not idle
    QVERIFY(!eff.isIdle());

    // Local goes idle, no peers → effective idle
    wrapped.triggerIdleStart();
    QVERIFY(eff.isIdle());

    // Peer becomes active → effective not idle
    const QDateTime now = QDateTime::fromSecsSinceEpoch(1'700'000'000);
    ram->handleReceivedDatagram(
        activityBytes(makeUuid(0x55), now.toSecsSinceEpoch(), 1, makeSecret()),
        3, now);
    QVERIFY(!eff.isIdle());
    delete ram;
  }
};

QTEST_MAIN(TestEffectiveIdleTime)
#include "test-effective-idle-time.moc"
