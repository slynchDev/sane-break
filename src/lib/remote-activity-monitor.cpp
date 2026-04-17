// Sane Break is a gentle break reminder that helps you avoid mindlessly skipping breaks
// Copyright (C) 2026 Sane Break developers
// SPDX-License-Identifier: GPL-3.0-or-later

#include "remote-activity-monitor.h"

#include <QHostInfo>
#include <QNetworkAddressEntry>
#include <QNetworkDatagram>
#include <QNetworkInterface>
#include <QStringList>
#include <QTimer>
#include <QUdpSocket>
#include <QUuid>

#include "peer-secret.h"

namespace peer {

namespace {

constexpr int kReplayWindowSeconds = 30;
constexpr int kClockSkewToleranceSeconds = 5;
constexpr int kTickIntervalMs = 1000;

}  // namespace

RemoteActivityMonitor::RemoteActivityMonitor(SanePreferences* prefs,
                                             SystemIdleTime* localIdle,
                                             QObject* parent)
    : QObject(parent),
      m_prefs(prefs),
      m_localIdle(localIdle),
      m_senderUuid(QUuid::createUuid().toRfc4122()) {
  (void)m_localIdle;  // Phase 4 wires signals; reserved for now.
}

RemoteActivityMonitor::~RemoteActivityMonitor() { closeSockets(); }

void RemoteActivityMonitor::start() {
  if (m_running) return;

  QString secretError;
  m_secret = loadPeerSecret(&secretError);
  if (m_secret.isEmpty()) {
    qWarning("Peer fusion: %s — running in single-machine mode",
             qPrintable(secretError));
    return;
  }

  openSockets();
  if (m_sockets.isEmpty()) {
    // No usable interface; graceful degradation per Req 11.4.
    return;
  }

  if (!m_tickTimer) {
    m_tickTimer = new QTimer(this);
    m_tickTimer->setInterval(kTickIntervalMs);
    connect(m_tickTimer, &QTimer::timeout, this,
            &RemoteActivityMonitor::onTimerTick);
  }
  m_tickTimer->start();
  m_running = true;
}

void RemoteActivityMonitor::stop() {
  if (!m_running && m_sockets.isEmpty() && m_peers.isEmpty() &&
      !m_anyPeerActive) {
    return;
  }
  m_running = false;
  if (m_tickTimer) m_tickTimer->stop();
  closeSockets();
  m_peers.clear();
  if (m_anyPeerActive) {
    m_anyPeerActive = false;
    emit peerActivityChanged(false);
  }
}

void RemoteActivityMonitor::openSockets() {
  const QStringList names = m_prefs->peerBroadcastInterfaces->get();
  if (names.isEmpty()) return;

  const int port = m_prefs->peerListenPort->get();
  // Validate port before the narrowing cast to quint16 — user can hand-edit
  // the INI to any int; silent wrap would bind an unintended UDP port.
  if (port < 1024 || port > 65535) {
    qWarning("Peer fusion: peerListenPort=%d out of range [1024, 65535]; "
             "fusion disabled for this session",
             port);
    return;
  }

  for (const QString& name : names) {
    QNetworkInterface iface = QNetworkInterface::interfaceFromName(name);
    if (!iface.isValid() || !(iface.flags() & QNetworkInterface::IsUp) ||
        !(iface.flags() & QNetworkInterface::CanBroadcast)) {
      qWarning("Peer fusion: interface %s unusable (invalid, down, or no "
               "broadcast capability); skipping",
               qPrintable(name));
      continue;
    }
    QNetworkAddressEntry addrEntry;
    bool haveAddr = false;
    for (const QNetworkAddressEntry& e : iface.addressEntries()) {
      if (e.ip().protocol() == QAbstractSocket::IPv4Protocol) {
        addrEntry = e;
        haveAddr = true;
        break;
      }
    }
    if (!haveAddr) {
      qWarning("Peer fusion: interface %s has no IPv4 address; skipping",
               qPrintable(name));
      continue;
    }

    // Record the user's ingress allowlist intent BEFORE attempting bind.
    // Every socket binds AnyIPv4:port with ShareAddress, so a sibling socket
    // on another NIC will still deliver datagrams arriving on this interface
    // (Qt reports the arrival index via QNetworkDatagram::interfaceIndex()).
    // Only skipping the insert on bind failure would silently drop legitimate
    // packets from user-allowlisted NICs.
    m_allowedInterfaceIndexes.insert(iface.index());

    auto* socket = new QUdpSocket(this);
    if (!socket->bind(QHostAddress::AnyIPv4, static_cast<quint16>(port),
                      QUdpSocket::ShareAddress |
                          QUdpSocket::ReuseAddressHint)) {
      qWarning("Peer fusion: bind failed on interface %s port %d: %s",
               qPrintable(name), port, qPrintable(socket->errorString()));
      socket->deleteLater();
      continue;
    }
    connect(socket, &QUdpSocket::readyRead, this,
            &RemoteActivityMonitor::onDatagramReady);
    InterfaceSocket tup{socket, addrEntry.ip(), addrEntry.broadcast(),
                        iface.index()};
    m_sockets.append(tup);
  }
  if (m_sockets.isEmpty()) {
    qWarning("Peer fusion: no interfaces from the allowlist could be bound; "
             "fusion disabled for this session");
  }
}

void RemoteActivityMonitor::closeSockets() {
  for (auto& tup : m_sockets) {
    if (tup.socket) {
      tup.socket->close();
      tup.socket->deleteLater();
      tup.socket = nullptr;
    }
  }
  m_sockets.clear();
  m_allowedInterfaceIndexes.clear();
}

void RemoteActivityMonitor::onDatagramReady() {
  auto* sock = qobject_cast<QUdpSocket*>(sender());
  if (!sock) return;
  const QDateTime now = QDateTime::currentDateTimeUtc();
  while (sock->hasPendingDatagrams()) {
    QNetworkDatagram dg = sock->receiveDatagram();
    if (!dg.isValid()) continue;
    handleReceivedDatagram(dg.data(), dg.interfaceIndex(), now);
  }
}

void RemoteActivityMonitor::handleReceivedDatagram(const QByteArray& bytes,
                                                   int interfaceIndex,
                                                   const QDateTime& now) {
  // Ingress interface filter (Req 2.1): only accept packets arriving on an
  // allowlisted NIC, regardless of the socket's bound address.
  if (!m_allowedInterfaceIndexes.contains(interfaceIndex)) return;

  // Need a secret to verify HMACs.
  if (m_secret.isEmpty()) return;

  auto maybePacket = decodePacket(bytes, m_secret);
  if (!maybePacket.has_value()) return;
  const Packet& packet = *maybePacket;

  // Self-loopback drop (Req 1.7).
  if (packet.senderUuid == m_senderUuid) return;

  // Timestamp window (Req 3.3): too old (>30 s) or too far future (>5 s) drop.
  const qint64 delta = now.toSecsSinceEpoch() - packet.timestamp;
  if (delta > kReplayWindowSeconds) return;
  if (delta < -kClockSkewToleranceSeconds) return;

  // Replay dedup.
  if (m_replay.contains(packet.senderUuid, packet.nonce)) return;
  m_replay.insert(packet.senderUuid, packet.nonce);

  // Normalize event shape → PeerLastState (Req 2.2).
  PeerState& ps = m_peers[packet.senderUuid];
  if (ps.hostname.isEmpty() && !packet.hostname.isEmpty()) {
    ps.hostname = packet.hostname;
  }
  if (packet.eventType == EVENT_ACTIVITY) {
    ps.lastSeenActive = now;
    ps.lastState = PeerLastState::Active;
  } else if (packet.eventType == EVENT_IDLE_TRANSITION) {
    const uint8_t state = static_cast<uint8_t>(packet.payload[0]);
    if (state == STATE_ACTIVE) {
      ps.lastSeenActive = now;
      ps.lastState = PeerLastState::Active;
    } else {  // STATE_IDLE — decoder already validated state ∈ {0x00, 0x01}.
      ps.lastSeenIdle = now;
      ps.lastState = PeerLastState::Idle;
    }
  }

  recomputeAnyPeerActive(now);
}

void RemoteActivityMonitor::onTimerTick() {
  tick(QDateTime::currentDateTimeUtc());
}

void RemoteActivityMonitor::tick(const QDateTime& now) {
  const int unreachableWindow = m_prefs->peerUnreachableWindowSeconds->get();
  for (auto it = m_peers.begin(); it != m_peers.end();) {
    const PeerState& ps = it.value();
    const QDateTime lastSeen = std::max(ps.lastSeenActive, ps.lastSeenIdle);
    if (!lastSeen.isValid() || lastSeen.secsTo(now) > unreachableWindow) {
      it = m_peers.erase(it);
    } else {
      ++it;
    }
  }
  // Always recompute — bounds the active-window → idle transition to 1 s
  // even for peers that simply stop broadcasting.
  recomputeAnyPeerActive(now);
}

bool RemoteActivityMonitor::computeAnyPeerActive(const QDateTime& now) const {
  const int activeWindow = m_prefs->peerActiveWindowSeconds->get();
  for (auto it = m_peers.cbegin(); it != m_peers.cend(); ++it) {
    const PeerState& ps = it.value();
    if (ps.lastState != PeerLastState::Active) continue;
    if (!ps.lastSeenActive.isValid()) continue;
    if (ps.lastSeenActive.secsTo(now) <= activeWindow) return true;
  }
  return false;
}

void RemoteActivityMonitor::recomputeAnyPeerActive(const QDateTime& now) {
  const bool latest = computeAnyPeerActive(now);
  if (latest != m_anyPeerActive) {
    m_anyPeerActive = latest;
    emit peerActivityChanged(latest);
  }
}

QList<PeerStatus> RemoteActivityMonitor::peerStatuses(const QDateTime& now) const {
  const int activeWindow = m_prefs->peerActiveWindowSeconds->get();
  QList<PeerStatus> out;
  out.reserve(m_peers.size());
  for (auto it = m_peers.cbegin(); it != m_peers.cend(); ++it) {
    const PeerState& ps = it.value();
    PeerStatus s;
    s.hostLabel = ps.hostname.isEmpty()
                      ? QString::fromLatin1(it.key().left(8).toHex())
                      : ps.hostname;
    s.lastSeenActive = ps.lastSeenActive;
    s.lastSeenIdle = ps.lastSeenIdle;
    s.isActive = (ps.lastState == PeerLastState::Active) &&
                 ps.lastSeenActive.isValid() &&
                 ps.lastSeenActive.secsTo(now) <= activeWindow;
    out.append(s);
  }
  return out;
}

}  // namespace peer
