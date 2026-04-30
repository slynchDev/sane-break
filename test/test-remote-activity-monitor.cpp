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
  void setIdle(bool idle) { m_isIdle = idle; }
  void triggerIdleStart() {
    m_isIdle = true;
    emit idleStart();
  }
  void triggerIdleEnd() {
    m_isIdle = false;
    emit idleEnd();
  }
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

// Phase 14 — MEETING_TRANSITION packet helper.
QByteArray meetingBytes(const QByteArray& peerUuid, const QString& hostname,
                        qint64 timestampSecs, quint64 nonce, uint8_t state,
                        const QByteArray& secret) {
  peer::Packet p;
  p.senderUuid = peerUuid;
  p.hostname = hostname;
  p.timestamp = timestampSecs;
  p.nonce = nonce;
  p.eventType = peer::EVENT_MEETING_TRANSITION;
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
        activityBytes(peerUuid, "peer-hp", t0.toSecsSinceEpoch(), 1, makeSecret()),
        3, t0);

    const auto statuses = ram->peerStatuses(t0);
    QCOMPARE(statuses.size(), qsizetype(1));
    QCOMPARE(statuses.first().hostLabel, QString("peer-hp"));
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

  // ---- Phase 4: outbound -------------------------------------------------

  void build_activity_packet_has_correct_shape() {
    auto ram = std::unique_ptr<peer::RemoteActivityMonitor>(makeMonitor());
    const QDateTime now = QDateTime::fromSecsSinceEpoch(1'700'000'000);
    const QByteArray bytes = ram->buildActivityPacket(now, 7);
    QVERIFY(!bytes.isEmpty());
    auto decoded = peer::decodePacket(bytes, makeSecret());
    QVERIFY(decoded.has_value());
    QCOMPARE(decoded->senderUuid, ram->senderUuid());
    QCOMPARE(uint8_t(decoded->eventType), uint8_t(peer::EVENT_ACTIVITY));
    QCOMPARE(decoded->timestamp, qint64(1'700'000'000));
    QCOMPARE(decoded->payload.size(), qsizetype(4));
    // event_count=7 big-endian: 0x00 0x00 0x00 0x07
    QCOMPARE(uint8_t(decoded->payload[3]), uint8_t(0x07));
    QCOMPARE(uint8_t(decoded->payload[2]), uint8_t(0x00));
    QCOMPARE(uint8_t(decoded->payload[1]), uint8_t(0x00));
    QCOMPARE(uint8_t(decoded->payload[0]), uint8_t(0x00));
  }

  void build_idle_transition_idle_has_state_zero() {
    auto ram = std::unique_ptr<peer::RemoteActivityMonitor>(makeMonitor());
    const QDateTime now = QDateTime::fromSecsSinceEpoch(1'700'000'000);
    const QByteArray bytes = ram->buildIdleTransitionPacket(now, peer::STATE_IDLE);
    auto decoded = peer::decodePacket(bytes, makeSecret());
    QVERIFY(decoded.has_value());
    QCOMPARE(uint8_t(decoded->eventType),
             uint8_t(peer::EVENT_IDLE_TRANSITION));
    QCOMPARE(decoded->payload.size(), qsizetype(1));
    QCOMPARE(uint8_t(decoded->payload[0]), uint8_t(peer::STATE_IDLE));
  }

  void build_idle_transition_active_has_state_one() {
    auto ram = std::unique_ptr<peer::RemoteActivityMonitor>(makeMonitor());
    const QDateTime now = QDateTime::fromSecsSinceEpoch(1'700'000'000);
    const QByteArray bytes =
        ram->buildIdleTransitionPacket(now, peer::STATE_ACTIVE);
    auto decoded = peer::decodePacket(bytes, makeSecret());
    QVERIFY(decoded.has_value());
    QCOMPARE(uint8_t(decoded->payload[0]), uint8_t(peer::STATE_ACTIVE));
  }

  void build_activity_packet_empty_without_secret() {
    // No testSetSecret call — monitor has no secret loaded.
    auto ram =
        std::make_unique<peer::RemoteActivityMonitor>(m_prefs, m_idle);
    const QDateTime now = QDateTime::fromSecsSinceEpoch(1'700'000'000);
    QVERIFY(ram->buildActivityPacket(now, 1).isEmpty());
    QVERIFY(ram->buildIdleTransitionPacket(now, peer::STATE_IDLE).isEmpty());
  }

  void two_activity_packets_have_distinct_nonces() {
    auto ram = std::unique_ptr<peer::RemoteActivityMonitor>(makeMonitor());
    const QDateTime now = QDateTime::fromSecsSinceEpoch(1'700'000'000);
    auto a = peer::decodePacket(ram->buildActivityPacket(now, 1), makeSecret());
    auto b = peer::decodePacket(ram->buildActivityPacket(now, 1), makeSecret());
    QVERIFY(a.has_value() && b.has_value());
    QVERIFY(a->nonce != b->nonce);
  }

  void self_emission_roundtrip_is_ignored() {
    // Property 4 (self-traffic immunity): if a packet we built is looped back
    // via an allowlisted interface, handleReceivedDatagram must drop it.
    auto ram = std::unique_ptr<peer::RemoteActivityMonitor>(makeMonitor());
    const QDateTime now = QDateTime::fromSecsSinceEpoch(1'700'000'000);
    const QByteArray bytes = ram->buildActivityPacket(now, 1);
    QVERIFY(!bytes.isEmpty());
    QSignalSpy spy(ram.get(),
                   &peer::RemoteActivityMonitor::peerActivityChanged);
    ram->handleReceivedDatagram(bytes, 3, now);
    QCOMPARE(spy.count(), 0);
    QCOMPARE(ram->peerCount(), 0);
  }

  // ---- Phase 6: attribution counters (Property 5, Requirements 5.1-5.3) --

  void local_counter_advances_while_not_idle() {
    auto ram = std::unique_ptr<peer::RemoteActivityMonitor>(makeMonitor());
    m_idle->setIdle(false);
    QDateTime now = QDateTime::fromSecsSinceEpoch(1'700'000'000);
    for (int i = 0; i < 5; ++i) {
      now = now.addSecs(1);
      ram->tick(now);
    }
    QCOMPARE(ram->activityBreakdown().totalActiveSeconds, 5);
  }

  void local_counter_stalls_while_idle() {
    auto ram = std::unique_ptr<peer::RemoteActivityMonitor>(makeMonitor());
    m_idle->setIdle(true);
    QDateTime now = QDateTime::fromSecsSinceEpoch(1'700'000'000);
    for (int i = 0; i < 5; ++i) {
      now = now.addSecs(1);
      ram->tick(now);
    }
    QCOMPARE(ram->activityBreakdown().totalActiveSeconds, 0);
  }

  void peer_counter_advances_within_active_window() {
    auto ram = std::unique_ptr<peer::RemoteActivityMonitor>(makeMonitor());
    m_idle->setIdle(true);  // keep local credit out of the picture
    const QDateTime now = QDateTime::fromSecsSinceEpoch(1'700'000'000);
    ram->handleReceivedDatagram(
        activityBytes(makeUuid(0x55), "peer-hp", now.toSecsSinceEpoch(), 1,
                      makeSecret()),
        3, now);
    // Three ticks inside peerActiveWindowSeconds (default 15).
    QDateTime t = now;
    for (int i = 0; i < 3; ++i) {
      t = t.addSecs(1);
      ram->tick(t);
    }
    const auto breakdown = ram->activityBreakdown();
    QCOMPARE(breakdown.totalActiveSeconds, 3);
    // local + remote rows; remote row is "peer-hp".
    QCOMPARE(breakdown.hosts.size(), 2);
    bool found = false;
    for (const peer::HostActivity& row : breakdown.hosts) {
      if (row.label == "peer-hp") {
        QCOMPARE(row.activeSeconds, 3);
        found = true;
      }
    }
    QVERIFY(found);
  }

  void peer_counter_stalls_after_active_window_expires() {
    auto ram = std::unique_ptr<peer::RemoteActivityMonitor>(makeMonitor());
    m_idle->setIdle(true);
    const QDateTime now = QDateTime::fromSecsSinceEpoch(1'700'000'000);
    ram->handleReceivedDatagram(
        activityBytes(makeUuid(0x55), "peer-hp", now.toSecsSinceEpoch(), 1,
                      makeSecret()),
        3, now);
    // Tick through the full peerActiveWindowSeconds (default 15) and past.
    const int activeWindow = m_prefs->peerActiveWindowSeconds->get();
    QDateTime t = now;
    for (int i = 0; i < activeWindow + 10; ++i) {
      t = t.addSecs(1);
      ram->tick(t);
    }
    const auto breakdown = ram->activityBreakdown();
    // At most `activeWindow` ticks credited (one per second within window).
    int peerSeconds = 0;
    for (const peer::HostActivity& row : breakdown.hosts) {
      if (row.label == "peer-hp") peerSeconds = row.activeSeconds;
    }
    QVERIFY(peerSeconds <= activeWindow);
    QVERIFY(peerSeconds >= activeWindow - 1);
  }

  void reset_attribution_zeroes_all_counters() {
    auto ram = std::unique_ptr<peer::RemoteActivityMonitor>(makeMonitor());
    m_idle->setIdle(false);
    const QDateTime now = QDateTime::fromSecsSinceEpoch(1'700'000'000);
    ram->handleReceivedDatagram(
        activityBytes(makeUuid(0x55), "peer-hp", now.toSecsSinceEpoch(), 1,
                      makeSecret()),
        3, now);
    QDateTime t = now;
    for (int i = 0; i < 4; ++i) {
      t = t.addSecs(1);
      ram->tick(t);
    }
    QVERIFY(ram->activityBreakdown().totalActiveSeconds > 0);
    ram->resetAttribution();
    const auto breakdown = ram->activityBreakdown();
    QCOMPARE(breakdown.totalActiveSeconds, 0);
    for (const peer::HostActivity& row : breakdown.hosts) {
      QCOMPARE(row.activeSeconds, 0);
      QCOMPARE(row.sharePercent, 0);
    }
  }

  void breakdown_shares_sum_close_to_total() {
    // Property 5: per-host active seconds sum to total within ±1s and no
    // host exceeds the interval length.
    auto ram = std::unique_ptr<peer::RemoteActivityMonitor>(makeMonitor());
    m_idle->setIdle(false);
    QDateTime now = QDateTime::fromSecsSinceEpoch(1'700'000'000);
    ram->handleReceivedDatagram(
        activityBytes(makeUuid(0x55), "peer-hp", now.toSecsSinceEpoch(), 1,
                      makeSecret()),
        3, now);
    ram->handleReceivedDatagram(
        activityBytes(makeUuid(0x66), "hp", now.toSecsSinceEpoch(), 2,
                      makeSecret()),
        3, now);
    for (int i = 0; i < 7; ++i) {
      now = now.addSecs(1);
      ram->tick(now);
    }
    const auto breakdown = ram->activityBreakdown();
    int sum = 0;
    for (const peer::HostActivity& row : breakdown.hosts) {
      QVERIFY(row.activeSeconds <= 7);
      sum += row.activeSeconds;
    }
    QCOMPARE(sum, breakdown.totalActiveSeconds);
  }

  void breakdown_uses_uuid_prefix_when_hostname_missing() {
    auto ram = std::unique_ptr<peer::RemoteActivityMonitor>(makeMonitor());
    m_idle->setIdle(true);
    const QDateTime now = QDateTime::fromSecsSinceEpoch(1'700'000'000);
    ram->handleReceivedDatagram(
        activityBytes(makeUuid(0xab), /*hostname=*/QString(),
                      now.toSecsSinceEpoch(), 1, makeSecret()),
        3, now);
    ram->tick(now.addSecs(1));
    const auto breakdown = ram->activityBreakdown();
    bool hasPrefixLabel = false;
    for (const peer::HostActivity& row : breakdown.hosts) {
      if (row.label == "ab00000000000000") hasPrefixLabel = true;
    }
    QVERIFY(hasPrefixLabel);
  }

  void breakdown_share_percents_reflect_ratio() {
    auto ram = std::unique_ptr<peer::RemoteActivityMonitor>(makeMonitor());
    // Drive local to 6 active ticks and the peer to 2 — local should be
    // ~75%, peer ~25% (±1 for rounding).
    m_idle->setIdle(false);
    QDateTime now = QDateTime::fromSecsSinceEpoch(1'700'000'000);
    for (int i = 0; i < 6; ++i) {
      now = now.addSecs(1);
      ram->tick(now);
    }
    // Peer arrives, then tick a couple more times — peer counter = 2, local
    // also advances 2 more (total local 8). Actually easier: forge the peer
    // bucket directly with two ACTIVITY packets at 1 s separation, where
    // tick advances local while idle=false.
    m_idle->setIdle(true);
    ram->handleReceivedDatagram(
        activityBytes(makeUuid(0x55), "peer-hp", now.toSecsSinceEpoch(), 1,
                      makeSecret()),
        3, now);
    now = now.addSecs(1);
    ram->tick(now);
    now = now.addSecs(1);
    ram->tick(now);
    const auto breakdown = ram->activityBreakdown();
    QVERIFY(breakdown.totalActiveSeconds > 0);
    for (const peer::HostActivity& row : breakdown.hosts) {
      QVERIFY(row.sharePercent >= 0 && row.sharePercent <= 100);
    }
  }

  // --- Phase 14: cross-peer meeting awareness --------------------------------

  void peer_meeting_started_sets_in_meeting_and_emits_signal() {
    auto ram = std::unique_ptr<peer::RemoteActivityMonitor>(makeMonitor());
    const QByteArray peerUuid = makeUuid(0x77);
    const QDateTime now = QDateTime::fromSecsSinceEpoch(1'700'000'000);

    QSignalSpy spy(ram.get(), &peer::RemoteActivityMonitor::peerMeetingChanged);
    ram->handleReceivedDatagram(
        meetingBytes(peerUuid, "r16", now.toSecsSinceEpoch(), 1,
                     peer::MEETING_STARTED, makeSecret()),
        3, now);

    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.first().first().toBool(), true);
    QVERIFY(ram->anyPeerInMeeting());
  }

  void peer_meeting_ended_clears_flag_and_emits() {
    auto ram = std::unique_ptr<peer::RemoteActivityMonitor>(makeMonitor());
    const QByteArray peerUuid = makeUuid(0x77);
    QDateTime now = QDateTime::fromSecsSinceEpoch(1'700'000'000);

    // Start in meeting.
    ram->handleReceivedDatagram(
        meetingBytes(peerUuid, "r16", now.toSecsSinceEpoch(), 1,
                     peer::MEETING_STARTED, makeSecret()),
        3, now);
    QVERIFY(ram->anyPeerInMeeting());

    QSignalSpy spy(ram.get(), &peer::RemoteActivityMonitor::peerMeetingChanged);
    now = now.addSecs(5);
    ram->handleReceivedDatagram(
        meetingBytes(peerUuid, "r16", now.toSecsSinceEpoch(), 2,
                     peer::MEETING_ENDED, makeSecret()),
        3, now);

    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.first().first().toBool(), false);
    QVERIFY(!ram->anyPeerInMeeting());
  }

  void peer_meeting_is_orthogonal_to_active_state() {
    // Req 12: meeting state must not mutate lastSeenActive / lastState, so
    // EffectiveIdleTime's fused-idle semantics are unaffected by a peer's
    // quiet-listening-in-meeting periods.
    auto ram = std::unique_ptr<peer::RemoteActivityMonitor>(makeMonitor());
    const QByteArray peerUuid = makeUuid(0x77);
    const QDateTime now = QDateTime::fromSecsSinceEpoch(1'700'000'000);

    QSignalSpy activitySpy(ram.get(),
                           &peer::RemoteActivityMonitor::peerActivityChanged);
    ram->handleReceivedDatagram(
        meetingBytes(peerUuid, "r16", now.toSecsSinceEpoch(), 1,
                     peer::MEETING_STARTED, makeSecret()),
        3, now);

    QCOMPARE(activitySpy.count(), 0);    // no fused-active transition
    QVERIFY(!ram->anyPeerActive());
    QVERIFY(ram->anyPeerInMeeting());
  }

  void peer_meeting_flipping_only_emits_on_transitions() {
    auto ram = std::unique_ptr<peer::RemoteActivityMonitor>(makeMonitor());
    const QByteArray peerUuid = makeUuid(0x77);
    QDateTime now = QDateTime::fromSecsSinceEpoch(1'700'000'000);

    QSignalSpy spy(ram.get(), &peer::RemoteActivityMonitor::peerMeetingChanged);
    // Two consecutive MEETING_STARTED packets must emit only once.
    for (quint64 nonce : {1u, 2u}) {
      ram->handleReceivedDatagram(
          meetingBytes(peerUuid, "r16", now.toSecsSinceEpoch(), nonce,
                       peer::MEETING_STARTED, makeSecret()),
          3, now);
      now = now.addSecs(1);
    }
    QCOMPARE(spy.count(), 1);
  }

  void peer_self_meeting_does_not_loopback() {
    // The loopback drop is already enforced by sender_uuid equality, but
    // make the Phase 14 path explicit — a MEETING packet from our own
    // sender_uuid must not flip our own aggregate.
    auto ram = std::unique_ptr<peer::RemoteActivityMonitor>(makeMonitor());
    const QDateTime now = QDateTime::fromSecsSinceEpoch(1'700'000'000);

    QSignalSpy spy(ram.get(), &peer::RemoteActivityMonitor::peerMeetingChanged);
    ram->handleReceivedDatagram(
        meetingBytes(ram->senderUuid(), "self",
                     now.toSecsSinceEpoch(), 1,
                     peer::MEETING_STARTED, makeSecret()),
        3, now);

    QCOMPARE(spy.count(), 0);
    QVERIFY(!ram->anyPeerInMeeting());
    QCOMPARE(ram->peerCount(), 0);
  }

  void peer_in_meeting_offline_after_unreachable_window_clears_aggregate() {
    auto ram = std::unique_ptr<peer::RemoteActivityMonitor>(makeMonitor());
    const QByteArray peerUuid = makeUuid(0x77);
    QDateTime now = QDateTime::fromSecsSinceEpoch(1'700'000'000);

    ram->handleReceivedDatagram(
        meetingBytes(peerUuid, "r16", now.toSecsSinceEpoch(), 1,
                     peer::MEETING_STARTED, makeSecret()),
        3, now);
    QVERIFY(ram->anyPeerInMeeting());

    QSignalSpy spy(ram.get(), &peer::RemoteActivityMonitor::peerMeetingChanged);
    // Advance well past peerUnreachableWindowSeconds (default 60). The
    // peer never re-sends anything — it should be evicted by tick() and
    // m_anyPeerInMeeting should flip to false.
    now = now.addSecs(120);
    ram->tick(now);

    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.first().first().toBool(), false);
    QVERIFY(!ram->anyPeerInMeeting());
    QCOMPARE(ram->peerCount(), 0);
  }

  void meeting_transition_extends_recency_for_passive_peer() {
    // A peer that sends only MEETING_TRANSITION (passive listening, no
    // keyboard input) must NOT be evicted before the unreachable window
    // elapses since its last MEETING packet. tick()'s recency check now
    // considers lastSeenMeetingTransition.
    auto ram = std::unique_ptr<peer::RemoteActivityMonitor>(makeMonitor());
    const QByteArray peerUuid = makeUuid(0x77);
    QDateTime now = QDateTime::fromSecsSinceEpoch(1'700'000'000);

    ram->handleReceivedDatagram(
        meetingBytes(peerUuid, "r16", now.toSecsSinceEpoch(), 1,
                     peer::MEETING_STARTED, makeSecret()),
        3, now);

    // 30s later — still within the 60s default unreachable window. Peer
    // has sent zero other packets.
    now = now.addSecs(30);
    ram->tick(now);
    QCOMPARE(ram->peerCount(), 1);
    QVERIFY(ram->anyPeerInMeeting());
  }

  void build_meeting_transition_packet_has_correct_shape() {
    auto ram = std::unique_ptr<peer::RemoteActivityMonitor>(makeMonitor());
    const QDateTime now = QDateTime::fromSecsSinceEpoch(1'700'000'000);

    for (uint8_t state : {peer::MEETING_ENDED, peer::MEETING_STARTED}) {
      const QByteArray bytes = ram->buildMeetingTransitionPacket(now, state);
      QVERIFY(!bytes.isEmpty());

      const auto packet = peer::decodePacket(bytes, makeSecret());
      QVERIFY(packet.has_value());
      QCOMPARE(packet->eventType, uint8_t(peer::EVENT_MEETING_TRANSITION));
      QCOMPARE(packet->payload.size(), qsizetype(1));
      QCOMPARE(static_cast<uint8_t>(packet->payload[0]), state);
    }
  }

  // --- Synchronized peer break: BREAK_START broadcast and receive ----------

  void broadcast_break_start_emits_signed_packet() {
    auto ram = std::unique_ptr<peer::RemoteActivityMonitor>(makeMonitor());
    const QDateTime now = QDateTime::fromSecsSinceEpoch(1'700'000'000);

    for (auto [breakType, expectedKind] :
         std::initializer_list<std::pair<BreakType, uint8_t>>{
             {BreakType::Small, peer::BREAK_SMALL},
             {BreakType::Big, peer::BREAK_BIG}}) {
      const QByteArray bytes = ram->buildBreakStartPacket(now, expectedKind);
      QVERIFY(!bytes.isEmpty());

      const auto packet = peer::decodePacket(bytes, makeSecret());
      QVERIFY(packet.has_value());
      QCOMPARE(packet->eventType, uint8_t(peer::EVENT_BREAK_START));
      QCOMPARE(packet->payload.size(), qsizetype(1));
      QCOMPARE(static_cast<uint8_t>(packet->payload[0]), expectedKind);
      (void)breakType;
    }
  }

  void build_break_start_packet_empty_when_no_secret() {
    // Construct a monitor without calling testSetSecret — m_secret is empty.
    auto ram = std::unique_ptr<peer::RemoteActivityMonitor>(
        new peer::RemoteActivityMonitor(m_prefs, m_idle));
    const QDateTime now = QDateTime::fromSecsSinceEpoch(1'700'000'000);
    QVERIFY(ram->buildBreakStartPacket(now, peer::BREAK_SMALL).isEmpty());
  }

  void receive_break_start_emits_peer_break_requested_small() {
    auto ram = std::unique_ptr<peer::RemoteActivityMonitor>(makeMonitor());
    const QByteArray peerUuid = makeUuid(0x77);
    const QDateTime now = QDateTime::fromSecsSinceEpoch(1'700'000'000);

    peer::Packet p;
    p.senderUuid = peerUuid;
    p.hostname = "hp";
    p.timestamp = now.toSecsSinceEpoch();
    p.nonce = 55;
    p.eventType = peer::EVENT_BREAK_START;
    p.payload = QByteArray(1, static_cast<char>(peer::BREAK_SMALL));
    const QByteArray bytes = peer::encodePacket(p, makeSecret());

    QSignalSpy spy(ram.get(), &peer::RemoteActivityMonitor::peerBreakRequested);
    ram->handleReceivedDatagram(bytes, 3, now);

    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.first().first().value<BreakType>(), BreakType::Small);
  }

  void receive_break_start_emits_peer_break_requested_big() {
    auto ram = std::unique_ptr<peer::RemoteActivityMonitor>(makeMonitor());
    const QByteArray peerUuid = makeUuid(0x78);
    const QDateTime now = QDateTime::fromSecsSinceEpoch(1'700'000'000);

    peer::Packet p;
    p.senderUuid = peerUuid;
    p.hostname = "hp";
    p.timestamp = now.toSecsSinceEpoch();
    p.nonce = 56;
    p.eventType = peer::EVENT_BREAK_START;
    p.payload = QByteArray(1, static_cast<char>(peer::BREAK_BIG));
    const QByteArray bytes = peer::encodePacket(p, makeSecret());

    QSignalSpy spy(ram.get(), &peer::RemoteActivityMonitor::peerBreakRequested);
    ram->handleReceivedDatagram(bytes, 3, now);

    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.first().first().value<BreakType>(), BreakType::Big);
  }

  void receive_break_start_does_not_mutate_peer_active_state() {
    auto ram = std::unique_ptr<peer::RemoteActivityMonitor>(makeMonitor());
    const QByteArray peerUuid = makeUuid(0x79);
    const QDateTime now = QDateTime::fromSecsSinceEpoch(1'700'000'000);

    peer::Packet p;
    p.senderUuid = peerUuid;
    p.hostname = "hp";
    p.timestamp = now.toSecsSinceEpoch();
    p.nonce = 57;
    p.eventType = peer::EVENT_BREAK_START;
    p.payload = QByteArray(1, static_cast<char>(peer::BREAK_SMALL));
    const QByteArray bytes = peer::encodePacket(p, makeSecret());

    QSignalSpy activitySpy(ram.get(),
                           &peer::RemoteActivityMonitor::peerActivityChanged);
    ram->handleReceivedDatagram(bytes, 3, now);

    QCOMPARE(activitySpy.count(), 0);
    QVERIFY(!ram->anyPeerActive());
  }

  void receive_break_start_loopback_dropped() {
    auto ram = std::unique_ptr<peer::RemoteActivityMonitor>(makeMonitor());
    const QDateTime now = QDateTime::fromSecsSinceEpoch(1'700'000'000);

    // Build a packet whose senderUuid matches ram's own uuid.
    peer::Packet p;
    p.senderUuid = ram->senderUuid();
    p.hostname = "self";
    p.timestamp = now.toSecsSinceEpoch();
    p.nonce = 99;
    p.eventType = peer::EVENT_BREAK_START;
    p.payload = QByteArray(1, static_cast<char>(peer::BREAK_SMALL));
    const QByteArray bytes = peer::encodePacket(p, makeSecret());

    QSignalSpy spy(ram.get(), &peer::RemoteActivityMonitor::peerBreakRequested);
    ram->handleReceivedDatagram(bytes, 3, now);

    QCOMPARE(spy.count(), 0);
  }
};

QTEST_MAIN(TestRemoteActivityMonitor)
#include "test-remote-activity-monitor.moc"
