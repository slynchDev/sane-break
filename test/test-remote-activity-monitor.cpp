// Sane Break is a gentle break reminder that helps you avoid mindlessly skipping breaks
// Copyright (C) 2026 Sane Break developers
// SPDX-License-Identifier: GPL-3.0-or-later

#include <qtestcase.h>

#include <QByteArray>
#include <QDateTime>
#include <QObject>
#include <QSettings>
#include <QSignalSpy>
#include <QString>
#include <QTemporaryFile>
#include <QTest>

#include "core/idle-time.h"
#include "core/preferences.h"
#include "lib/peer-packet.h"
#include "lib/remote-activity-monitor.h"

namespace {

class MockIdleTime : public SystemIdleTime {
 public:
  using SystemIdleTime::SystemIdleTime;
  void startWatching() override {}
  void stopWatching() override {}
  void setWatchAccuracy(int) override {}
  void setMinIdleTime(int) override {}
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

QByteArray activityBytes(const QByteArray& peerUuid, const QString& hostname,
                         qint64 timestampSecs, quint64 nonce,
                         const QByteArray& secret) {
  peer::Packet p;
  p.senderUuid = peerUuid;
  p.hostname = hostname;
  p.timestamp = timestampSecs;
  p.nonce = nonce;
  p.eventType = peer::EVENT_ACTIVITY;
  p.payload = QByteArray(4, '\0');
  return peer::encodePacket(p, secret);
}

QByteArray idleBytes(const QByteArray& peerUuid, const QString& hostname,
                     qint64 timestampSecs, quint64 nonce, uint8_t state,
                     const QByteArray& secret) {
  peer::Packet p;
  p.senderUuid = peerUuid;
  p.hostname = hostname;
  p.timestamp = timestampSecs;
  p.nonce = nonce;
  p.eventType = peer::EVENT_IDLE_TRANSITION;
  p.payload = QByteArray(1, static_cast<char>(state));
  return peer::encodePacket(p, secret);
}

}  // namespace

class TestRemoteActivityMonitor : public QObject {
  Q_OBJECT

 private:
  QTemporaryFile* m_settingsFile = nullptr;
  SanePreferences* m_prefs = nullptr;
  MockIdleTime* m_idle = nullptr;

  peer::RemoteActivityMonitor* makeMonitor() {
    auto* ram = new peer::RemoteActivityMonitor(m_prefs, m_idle);
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
    m_idle = new MockIdleTime();
  }

  void cleanup() {
    delete m_idle;
    m_idle = nullptr;
    delete m_prefs;
    m_prefs = nullptr;
    delete m_settingsFile;
    m_settingsFile = nullptr;
  }

  void sender_uuid_is_16_bytes() {
    auto ram = std::unique_ptr<peer::RemoteActivityMonitor>(makeMonitor());
    QCOMPARE(ram->senderUuid().size(), qsizetype(16));
  }

  void activity_packet_updates_peer_state() {
    auto ram = std::unique_ptr<peer::RemoteActivityMonitor>(makeMonitor());
    const QByteArray peerUuid = makeUuid(0x55);
    const QDateTime now = QDateTime::fromSecsSinceEpoch(1'700'000'000);

    QSignalSpy spy(ram.get(), &peer::RemoteActivityMonitor::peerActivityChanged);
    ram->handleReceivedDatagram(
        activityBytes(peerUuid, "hp", now.toSecsSinceEpoch(), 1, makeSecret()),
        3, now);

    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.first().first().toBool(), true);
    QVERIFY(ram->anyPeerActive());
    QCOMPARE(ram->peerCount(), 1);
  }

  void idle_transition_active_then_idle_flips_aggregate() {
    auto ram = std::unique_ptr<peer::RemoteActivityMonitor>(makeMonitor());
    const QByteArray peerUuid = makeUuid(0x55);
    const QDateTime t0 = QDateTime::fromSecsSinceEpoch(1'700'000'000);

    QSignalSpy spy(ram.get(), &peer::RemoteActivityMonitor::peerActivityChanged);

    // ACTIVITY → Active (transition false→true)
    ram->handleReceivedDatagram(
        activityBytes(peerUuid, "hp", t0.toSecsSinceEpoch(), 1, makeSecret()), 3,
        t0);
    QVERIFY(ram->anyPeerActive());
    QCOMPARE(spy.count(), 1);

    // IDLE_TRANSITION{active} → still Active, no new transition
    const QDateTime t1 = t0.addSecs(2);
    ram->handleReceivedDatagram(
        idleBytes(peerUuid, "hp", t1.toSecsSinceEpoch(), 2, peer::STATE_ACTIVE,
                  makeSecret()),
        3, t1);
    QCOMPARE(spy.count(), 1);

    // IDLE_TRANSITION{idle} → Idle (transition true→false)
    const QDateTime t2 = t0.addSecs(4);
    ram->handleReceivedDatagram(
        idleBytes(peerUuid, "hp", t2.toSecsSinceEpoch(), 3, peer::STATE_IDLE,
                  makeSecret()),
        3, t2);
    QVERIFY(!ram->anyPeerActive());
    QCOMPARE(spy.count(), 2);
    QCOMPARE(spy.last().first().toBool(), false);
  }

  void self_packet_is_dropped() {
    auto ram = std::unique_ptr<peer::RemoteActivityMonitor>(makeMonitor());
    const QDateTime now = QDateTime::fromSecsSinceEpoch(1'700'000'000);

    // Use the RAM's own senderUuid — self-loopback must not mutate peer state.
    const QByteArray bytes = activityBytes(
        ram->senderUuid(), "self", now.toSecsSinceEpoch(), 1, makeSecret());

    QSignalSpy spy(ram.get(), &peer::RemoteActivityMonitor::peerActivityChanged);
    ram->handleReceivedDatagram(bytes, 3, now);
    QCOMPARE(spy.count(), 0);
    QCOMPARE(ram->peerCount(), 0);
  }

  void ingress_filter_drops_non_allowlisted_interface() {
    auto ram = std::unique_ptr<peer::RemoteActivityMonitor>(makeMonitor());
    const QByteArray peerUuid = makeUuid(0x55);
    const QDateTime now = QDateTime::fromSecsSinceEpoch(1'700'000'000);
    const QByteArray bytes = activityBytes(peerUuid, "hp", now.toSecsSinceEpoch(),
                                           1, makeSecret());

    // Interface 99 is not allowlisted (only 3 was via testAllowInterface).
    ram->handleReceivedDatagram(bytes, 99, now);
    QCOMPARE(ram->peerCount(), 0);
    QVERIFY(!ram->anyPeerActive());
  }

  void duplicate_nonce_is_dropped() {
    auto ram = std::unique_ptr<peer::RemoteActivityMonitor>(makeMonitor());
    const QByteArray peerUuid = makeUuid(0x55);
    const QDateTime now = QDateTime::fromSecsSinceEpoch(1'700'000'000);
    const QByteArray bytes = activityBytes(peerUuid, "hp", now.toSecsSinceEpoch(),
                                           42, makeSecret());

    ram->handleReceivedDatagram(bytes, 3, now);
    QCOMPARE(ram->peerCount(), 1);
    // Identical bytes on second arrival → replay dedup drops.
    ram->handleReceivedDatagram(bytes, 3, now);
    QCOMPARE(ram->peerCount(), 1);
  }

  void old_timestamp_is_dropped() {
    auto ram = std::unique_ptr<peer::RemoteActivityMonitor>(makeMonitor());
    const QByteArray peerUuid = makeUuid(0x55);
    const QDateTime now = QDateTime::fromSecsSinceEpoch(1'700'000'000);
    // 31 s in the past → beyond the 30 s replay window.
    const QByteArray bytes = activityBytes(
        peerUuid, "hp", now.toSecsSinceEpoch() - 31, 1, makeSecret());
    ram->handleReceivedDatagram(bytes, 3, now);
    QCOMPARE(ram->peerCount(), 0);
  }

  void future_timestamp_is_dropped() {
    auto ram = std::unique_ptr<peer::RemoteActivityMonitor>(makeMonitor());
    const QByteArray peerUuid = makeUuid(0x55);
    const QDateTime now = QDateTime::fromSecsSinceEpoch(1'700'000'000);
    // 6 s in the future → beyond the 5 s clock-skew tolerance.
    const QByteArray bytes = activityBytes(
        peerUuid, "hp", now.toSecsSinceEpoch() + 6, 1, makeSecret());
    ram->handleReceivedDatagram(bytes, 3, now);
    QCOMPARE(ram->peerCount(), 0);
  }

  void silent_peer_aggregate_flips_within_one_second_of_active_window() {
    auto ram = std::unique_ptr<peer::RemoteActivityMonitor>(makeMonitor());
    const QByteArray peerUuid = makeUuid(0x55);
    const QDateTime t0 = QDateTime::fromSecsSinceEpoch(1'700'000'000);

    ram->handleReceivedDatagram(
        activityBytes(peerUuid, "hp", t0.toSecsSinceEpoch(), 1, makeSecret()), 3,
        t0);
    QVERIFY(ram->anyPeerActive());

    // Default activeWindow = 15 s. Tick at t0+14 — still active.
    ram->tick(t0.addSecs(14));
    QVERIFY(ram->anyPeerActive());

    // Tick at t0+16 — window crossed; aggregate flips to false even though no
    // packet arrived (this is the periodic recompute safeguard).
    ram->tick(t0.addSecs(16));
    QVERIFY(!ram->anyPeerActive());
    // Peer still in map (unreachable window hasn't elapsed).
    QCOMPARE(ram->peerCount(), 1);
  }

  void peer_offline_timeout_removes_and_flips_aggregate() {
    auto ram = std::unique_ptr<peer::RemoteActivityMonitor>(makeMonitor());
    const QByteArray peerUuid = makeUuid(0x55);
    const QDateTime t0 = QDateTime::fromSecsSinceEpoch(1'700'000'000);

    ram->handleReceivedDatagram(
        activityBytes(peerUuid, "hp", t0.toSecsSinceEpoch(), 1, makeSecret()), 3,
        t0);
    QVERIFY(ram->anyPeerActive());

    // Default unreachable window = 60 s. Tick at t0+61 — peer removed.
    ram->tick(t0.addSecs(61));
    QCOMPARE(ram->peerCount(), 0);
    QVERIFY(!ram->anyPeerActive());
  }

  void stop_clears_peers_and_emits_false() {
    auto ram = std::unique_ptr<peer::RemoteActivityMonitor>(makeMonitor());
    const QByteArray peerUuid = makeUuid(0x55);
    const QDateTime t0 = QDateTime::fromSecsSinceEpoch(1'700'000'000);
    ram->handleReceivedDatagram(
        activityBytes(peerUuid, "hp", t0.toSecsSinceEpoch(), 1, makeSecret()), 3,
        t0);
    QVERIFY(ram->anyPeerActive());

    QSignalSpy spy(ram.get(), &peer::RemoteActivityMonitor::peerActivityChanged);
    ram->stop();
    QVERIFY(!ram->anyPeerActive());
    QCOMPARE(ram->peerCount(), 0);
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.first().first().toBool(), false);
  }

  void peer_statuses_returns_host_label_and_is_active() {
    auto ram = std::unique_ptr<peer::RemoteActivityMonitor>(makeMonitor());
    const QByteArray peerUuid = makeUuid(0x55);
    const QDateTime t0 = QDateTime::fromSecsSinceEpoch(1'700'000'000);
    ram->handleReceivedDatagram(
        activityBytes(peerUuid, "r16", t0.toSecsSinceEpoch(), 1, makeSecret()),
        3, t0);

    const auto statuses = ram->peerStatuses(t0);
    QCOMPARE(statuses.size(), qsizetype(1));
    QCOMPARE(statuses.first().hostLabel, QString("r16"));
    QVERIFY(statuses.first().isActive);
  }

  void peer_statuses_uuid_fallback_when_no_hostname() {
    auto ram = std::unique_ptr<peer::RemoteActivityMonitor>(makeMonitor());
    const QByteArray peerUuid = makeUuid(0xAA);
    const QDateTime t0 = QDateTime::fromSecsSinceEpoch(1'700'000'000);
    ram->handleReceivedDatagram(
        activityBytes(peerUuid, "", t0.toSecsSinceEpoch(), 1, makeSecret()), 3,
        t0);

    const auto statuses = ram->peerStatuses(t0);
    QCOMPARE(statuses.size(), qsizetype(1));
    // Fallback is first 8 bytes of the uuid hex-encoded = 16 hex chars.
    QCOMPARE(statuses.first().hostLabel.size(), qsizetype(16));
    QVERIFY(statuses.first().hostLabel.startsWith("aa"));
  }

  void peer_activity_changed_only_on_transitions() {
    auto ram = std::unique_ptr<peer::RemoteActivityMonitor>(makeMonitor());
    const QByteArray peerUuid = makeUuid(0x55);
    const QDateTime t0 = QDateTime::fromSecsSinceEpoch(1'700'000'000);

    QSignalSpy spy(ram.get(), &peer::RemoteActivityMonitor::peerActivityChanged);
    // Four consecutive ACTIVITY packets; aggregate transitions once (false→true).
    for (quint64 n = 1; n <= 4; ++n) {
      ram->handleReceivedDatagram(
          activityBytes(peerUuid, "hp", t0.addSecs(n).toSecsSinceEpoch(), n,
                        makeSecret()),
          3, t0.addSecs(n));
    }
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.first().first().toBool(), true);
  }

  void tampered_packet_is_dropped() {
    auto ram = std::unique_ptr<peer::RemoteActivityMonitor>(makeMonitor());
    const QByteArray peerUuid = makeUuid(0x55);
    const QDateTime now = QDateTime::fromSecsSinceEpoch(1'700'000'000);
    QByteArray bytes =
        activityBytes(peerUuid, "hp", now.toSecsSinceEpoch(), 1, makeSecret());
    // Flip the last HMAC byte.
    bytes[bytes.size() - 1] = bytes[bytes.size() - 1] ^ 0xFF;
    ram->handleReceivedDatagram(bytes, 3, now);
    QCOMPARE(ram->peerCount(), 0);
  }
};

QTEST_MAIN(TestRemoteActivityMonitor)
#include "test-remote-activity-monitor.moc"
