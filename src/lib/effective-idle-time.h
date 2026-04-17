// Sane Break is a gentle break reminder that helps you avoid mindlessly skipping breaks
// Copyright (C) 2026 Sane Break developers
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QObject>

#include "core/idle-time.h"

namespace peer {
class RemoteActivityMonitor;
}

// Facade that wraps the raw local `SystemIdleTime` and a `RemoteActivityMonitor`
// to expose a fused idle view to the rest of the app. Effective_Idle is defined
// as `wrapped->isIdle() AND NOT anyPeerActive` per Spec Req 2.5.
//
// Callers that previously consumed `SystemIdleTime*` (AppContext, AppState
// subclasses) can consume this facade through the unchanged dependency
// without being aware of peers.
class EffectiveIdleTime : public SystemIdleTime {
  Q_OBJECT
 public:
  // `wrapped` is the raw OS-level idle watcher; `peers` may be nullptr when
  // peer fusion is disabled, in which case the facade degrades to a
  // pass-through of the wrapped signals.
  EffectiveIdleTime(SystemIdleTime* wrapped, peer::RemoteActivityMonitor* peers,
                    QObject* parent = nullptr);

  // Forward configuration/lifecycle to the wrapped timer.
  void startWatching() override;
  void stopWatching() override;
  void setWatchAccuracy(int accuracy) override;
  void setMinIdleTime(int idleTime) override;

  // Return the fused value so synchronous callers of
  // `app->idleTimer->isIdle()` observe the peer-aware state consistent with
  // the emitted idleStart/idleEnd signals.
  bool isIdle() override;

 private slots:
  void onWrappedIdleStart();
  void onWrappedIdleEnd();
  void onPeerActivityChanged(bool anyPeerActive);

 private:
  void recompute();

  SystemIdleTime* m_wrapped;
  peer::RemoteActivityMonitor* m_peers;
  bool m_anyPeerActive = false;
  bool m_effectiveIdle = false;
};
