// Sane Break is a gentle break reminder that helps you avoid mindlessly skipping breaks
// Copyright (C) 2024-2026 Sane Break developers
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QTimer>
#include <optional>

#include "app/pref-window.h"
#include "app/tray.h"
#include "core/app.h"
#include "core/preferences.h"
#include "focus-window.h"
#include "meeting-prompt.h"
#include "meeting-window.h"
#include "postpone-window.h"
#include "stats-window.h"

namespace peer {
class RemoteActivityMonitor;
}

class SaneBreakApp : public AbstractApp {
  Q_OBJECT
 public:
  SaneBreakApp(const AppDependencies& deps, QObject* parent = nullptr);
  ~SaneBreakApp() = default;
  static SaneBreakApp* create(SanePreferences* preferences, QObject* parent = nullptr);

  void start() override;
  void doLockScreen() override;
  void openPostponeWindow();
  void openMeetingWindow();
  void openFocusWindow();

  void showPreferences();
  void openStatsWindow();

 signals:
  void quit();

 private:
  PreferenceWindow* prefWindow;
  StatusTrayWindow* tray;
  QPointer<FocusWindow> focusWindow;
  QPointer<PostponeWindow> postponeWindow;
  QPointer<MeetingWindow> meetingWindow;
  QPointer<StatsWindow> statsWindow;
  void confirmQuit();

  // Nullable — peer fusion is optional. When null, the constructor skips
  // wiring the peer signal/slot connections entirely; the slot bodies also
  // carry defensive `if (!m_ram) return;` guards so direct invocation is a
  // no-op. Baseline (pre-fusion) app behavior per Property 7.
  peer::RemoteActivityMonitor* m_ram = nullptr;
  // Guards re-entrance when a rebind failure reverts preferences, which
  // fires their `changed` signal again and would otherwise recurse.
  bool m_peerRebindGuard = false;
  // Last known-good port/interfaces snapshot — captured after every
  // successful start(); used by onPeerBindingChanged() to revert on
  // bind failure (Req 7.8). std::nullopt before the first successful start.
  std::optional<int> m_lastGoodPort;
  std::optional<QStringList> m_lastGoodInterfaces;

  void onPeerFusionToggled();
  void onPeerBindingChanged();
  void snapshotLastGoodPeerBinding();
};
