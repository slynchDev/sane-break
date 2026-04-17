// Sane Break is a gentle break reminder that helps you avoid mindlessly skipping breaks
// Copyright (C) 2026 Sane Break developers
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QHash>
#include <QHostAddress>
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>

#include "core/idle-time.h"
#include "core/preferences.h"
#include "peer-packet.h"

QT_BEGIN_NAMESPACE
class QTimer;
class QUdpSocket;
QT_END_NAMESPACE

namespace peer {

enum class PeerLastState {
  Active,
  Idle,
};

struct PeerState {
  QString hostname;
  QDateTime lastSeenActive;
  QDateTime lastSeenIdle;
  PeerLastState lastState = PeerLastState::Idle;
  int activeSecondsSinceLastBreak = 0;
};

struct PeerStatus {
  QString hostLabel;
  QDateTime lastSeenActive;
  QDateTime lastSeenIdle;
  bool isActive = false;
};

// Subscribes to signed peer activity packets on the LAN, maintains per-peer
// liveness state, and exposes an aggregate peerActivityChanged(bool) signal
// consumed by EffectiveIdleTime.
//
// `localIdle` MUST be the RAW local SystemIdleTime, NOT an EffectiveIdleTime
// facade — wiring the facade here would create a peer-feedback loop: peer
// reports active -> facade reports active -> we re-broadcast "active" ->
// peer sees us as active and stays active indefinitely.
class RemoteActivityMonitor : public QObject {
  Q_OBJECT
 public:
  RemoteActivityMonitor(SanePreferences* prefs, SystemIdleTime* localIdle,
                        QObject* parent = nullptr);
  ~RemoteActivityMonitor() override;

  // Load the peer secret, open UDP sockets on every allowlisted interface,
  // and start the 1s offline-cleanup / aggregate-recompute tick. Idempotent.
  void start();

  // Tear down sockets + timers, clear the peer map, and emit
  // peerActivityChanged(false) if the cached aggregate was true so the
  // EffectiveIdleTime facade falls through to pass-through semantics on the
  // same event-loop turn. Idempotent.
  void stop();

  // Dispatch a received datagram. In production called by the QUdpSocket
  // readyRead handler; exposed so tests can inject packets without real
  // network I/O. `now` is passed explicitly so tests can control time.
  void handleReceivedDatagram(const QByteArray& bytes, int interfaceIndex,
                              const QDateTime& now);

  // Drive the 1s periodic cleanup + aggregate recompute. In production
  // called by the internal timer; exposed so tests can step time.
  void tick(const QDateTime& now);

  // Build an ACTIVITY packet with the given `eventCount` payload and a fresh
  // nonce, using `now` as the timestamp. Returns an empty QByteArray if the
  // monitor has no secret loaded (caller should treat as a no-op).
  QByteArray buildActivityPacket(const QDateTime& now, quint32 eventCount) const;

  // Build an IDLE_TRANSITION packet with `state` ∈ {peer::STATE_IDLE,
  // peer::STATE_ACTIVE} and a fresh nonce.
  QByteArray buildIdleTransitionPacket(const QDateTime& now, uint8_t state) const;

  QByteArray senderUuid() const { return m_senderUuid; }
  bool anyPeerActive() const { return m_anyPeerActive; }
  int peerCount() const { return m_peers.size(); }
  QList<PeerStatus> peerStatuses(const QDateTime& now) const;

  // Test hooks — safe to call from tests that bypass start() to avoid real
  // socket binding. Production code never calls these.
  void testSetSecret(const QByteArray& secret) { m_secret = secret; }
  void testAllowInterface(int interfaceIndex) {
    m_allowedInterfaceIndexes.insert(interfaceIndex);
  }

 signals:
  void peerActivityChanged(bool anyPeerActive);

 public slots:
  // Wire targets — public so tests can trigger them deterministically
  // without spinning up a QEventLoop / sleeping for the timer interval.
  void onHeartbeatTick();
  void onLocalIdleStart();
  void onLocalIdleEnd();

 private slots:
  void onDatagramReady();
  void onTimerTick();

 private:
  void openSockets();
  void closeSockets();
  bool computeAnyPeerActive(const QDateTime& now) const;
  void recomputeAnyPeerActive(const QDateTime& now);
  // Write the given bytes to every bound socket's subnet-directed broadcast.
  // Logs at debug level (at most once per interface per 60s) on transmit
  // failure to avoid flooding on a downed NIC.
  void sendToAllInterfaces(const QByteArray& bytes, const QDateTime& now);

  struct InterfaceSocket {
    QUdpSocket* socket = nullptr;
    QHostAddress localAddr;
    QHostAddress subnetBroadcast;
    int interfaceIndex = 0;
  };

  SanePreferences* m_prefs;
  SystemIdleTime* m_localIdle;
  QByteArray m_secret;
  QByteArray m_senderUuid;
  PeerReplayBuffer m_replay;
  QHash<QByteArray, PeerState> m_peers;
  bool m_anyPeerActive = false;
  bool m_running = false;
  QList<InterfaceSocket> m_sockets;
  QSet<int> m_allowedInterfaceIndexes;
  QTimer* m_tickTimer = nullptr;
  QTimer* m_heartbeatTimer = nullptr;
  // Per-interface-index last-error timestamp for rate-limited transmit-failure
  // logging (at most once per interface per 60 s).
  QHash<int, QDateTime> m_lastSendWarnAt;
};

}  // namespace peer
