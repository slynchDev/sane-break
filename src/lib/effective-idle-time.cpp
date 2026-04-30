// Sane Break is a gentle break reminder that helps you avoid mindlessly skipping breaks
// Copyright (C) 2026 Sane Break developers
// SPDX-License-Identifier: GPL-3.0-or-later

#include "effective-idle-time.h"

#include "remote-activity-monitor.h"

EffectiveIdleTime::EffectiveIdleTime(SystemIdleTime* wrapped,
                                     peer::RemoteActivityMonitor* peers,
                                     QObject* parent)
    : SystemIdleTime(parent), m_wrapped(wrapped), m_peers(peers) {
  connect(m_wrapped, &SystemIdleTime::idleStart, this,
          &EffectiveIdleTime::onWrappedIdleStart);
  connect(m_wrapped, &SystemIdleTime::idleEnd, this,
          &EffectiveIdleTime::onWrappedIdleEnd);
  if (m_peers) {
    m_anyPeerActive = m_peers->anyPeerActive();
    connect(m_peers, &peer::RemoteActivityMonitor::peerActivityChanged, this,
            &EffectiveIdleTime::onPeerActivityChanged);
  }
  // Seed the cached effective-idle from the initial state of the wrapped
  // timer. No emission on construction — consumers subscribe after.
  m_effectiveIdle = m_wrapped->isIdle() && !m_anyPeerActive;
}

void EffectiveIdleTime::startWatching() { m_wrapped->startWatching(); }
void EffectiveIdleTime::stopWatching() { m_wrapped->stopWatching(); }
void EffectiveIdleTime::setWatchAccuracy(int accuracy) {
  m_wrapped->setWatchAccuracy(accuracy);
}
void EffectiveIdleTime::setMinIdleTime(int idleTime) {
  m_wrapped->setMinIdleTime(idleTime);
}

bool EffectiveIdleTime::isIdle() {
  return m_wrapped->isIdle() && !m_anyPeerActive;
}

void EffectiveIdleTime::onWrappedIdleStart() { recompute(); }
void EffectiveIdleTime::onWrappedIdleEnd() { recompute(); }

void EffectiveIdleTime::onPeerActivityChanged(bool anyPeerActive) {
  m_anyPeerActive = anyPeerActive;
  recompute();
}

void EffectiveIdleTime::recompute() {
  const bool effective = m_wrapped->isIdle() && !m_anyPeerActive;
  if (effective == m_effectiveIdle) return;  // suppress duplicate edges
  m_effectiveIdle = effective;
  if (effective) {
    emit idleStart();
  } else {
    emit idleEnd();
  }
}
