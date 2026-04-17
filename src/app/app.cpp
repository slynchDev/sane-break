// Sane Break is a gentle break reminder that helps you avoid mindlessly skipping breaks
// Copyright (C) 2024-2026 Sane Break developers
// SPDX-License-Identifier: GPL-3.0-or-later

#include "app.h"

#include <qglobal.h>
#include <qlogging.h>

#include <QApplication>
#include <QDateTime>
#include <QMessageBox>
#include <QObject>
#include <QSqlDatabase>
#include <QString>
#include <QStyleHints>
#include <QTimer>
#include <QWindow>
#include <Qt>

#include "app/break-windows.h"
#include "app/pref-window.h"
#include "app/tray.h"
#include "app/widgets/language-select.h"
#include "core/app-states.h"
#include "core/app.h"
#include "core/db.h"
#include "core/preferences.h"
#include "focus-window.h"
#include "idle/factory.h"
#include "lib/effective-idle-time.h"
#include "lib/remote-activity-monitor.h"
#include "lib/screen-lock.h"
#include "lib/system-monitor.h"
#include "lib/timer.h"
#include "meeting-prompt.h"
#include "meeting-window.h"
#include "postpone-window.h"
#include "stats-window.h"

#if (defined(Q_OS_LINUX) or defined(Q_OS_MACOS))
#include "app/unix/signal-handler.h"
#else
#include "app/windows/signal-handler.h"
#endif

#ifdef Q_OS_MACOS
#include "lib/macos/workspace.h"
#endif

SaneBreakApp::SaneBreakApp(const AppDependencies& deps, QObject* parent)
    : AbstractApp(deps, parent), m_ram(deps.remoteActivityMonitor) {
  prefWindow = new PreferenceWindow(preferences);
  prefWindow->setRemoteActivityMonitor(m_ram);
  tray = StatusTrayWindow::createTrayOrWindow(preferences, this);
  tray->setRemoteActivityMonitor(m_ram);

  if (m_ram) {
    // Task 9.3: toggle fusion on/off without rebuilding the dependency graph.
    // The EffectiveIdleTime facade falls through to pass-through semantics
    // when ram->stop() emits peerActivityChanged(false) within the same
    // event-loop turn, so no AppDependencies rewire is needed.
    connect(preferences->peerFusionEnabled, &SettingWithSignal::changed, this,
            &SaneBreakApp::onPeerFusionToggled);
    // Task 9.4: port or interface changes force a rebind. Handler reverts to
    // the last known-good binding if the new one fails.
    connect(preferences->peerListenPort, &SettingWithSignal::changed, this,
            &SaneBreakApp::onPeerBindingChanged);
    connect(preferences->peerBroadcastInterfaces, &SettingWithSignal::changed, this,
            &SaneBreakApp::onPeerBindingChanged);
    // Task 9.5: breakStart is emitted from AppStateBreak::enter() on every
    // break-entry path (normal expiry, BigBreakNow, EndMeetingBreakNow, etc.),
    // so a single connect hits all of them. resetAttribution() zeros the
    // local and per-peer active-seconds counters for the new cycle.
    connect(this, &AppContext::breakStart, m_ram,
            &peer::RemoteActivityMonitor::resetAttribution);
    // Phase 14: meetingStart/End are emitted from AppStateMeeting::enter/exit
    // so peer machines learn the local host is in a meeting and can suppress
    // their own idle-driven break scheduling for the duration.
    connect(this, &AppContext::meetingStart, m_ram,
            &peer::RemoteActivityMonitor::broadcastMeetingStart);
    connect(this, &AppContext::meetingEnd, m_ram,
            &peer::RemoteActivityMonitor::broadcastMeetingEnd);
    // Phase 14 consumer: a peer entering a meeting raises a pause request
    // with PauseReason::PeerMeeting on this host. Reuses the existing
    // pause machinery (AppStateNormal -> AppStatePaused transition, which
    // also handles long-pause cycle reset on resume). This is the
    // "cleanest" consumer option noted in STATUS.md: no new AppState,
    // no new idle-facade coupling, just a pre-existing reason code.
    connect(m_ram, &peer::RemoteActivityMonitor::peerMeetingChanged, this,
            [this](bool inMeeting) {
              if (inMeeting)
                onPauseRequest(PauseReason::PeerMeeting);
              else
                onResumeRequest(PauseReason::PeerMeeting);
            });
  }

  connect(this, &SaneBreakApp::trayDataUpdated, tray, &StatusTrayWindow::update);
  connect(tray, &StatusTrayWindow::nextBreakRequested, this, &SaneBreakApp::breakNow);
  connect(tray, &StatusTrayWindow::nextBigBreakRequested, this,
          &SaneBreakApp::bigBreakNow);
  connect(tray, &StatusTrayWindow::smallBreakInsteadRequested, this,
          &SaneBreakApp::smallBreakInstead);
  connect(tray, &StatusTrayWindow::postponeRequested, this,
          &SaneBreakApp::openPostponeWindow);
  connect(tray, &StatusTrayWindow::meetingRequested, this,
          &SaneBreakApp::openMeetingWindow);
  connect(tray, &StatusTrayWindow::focusRequested, this,
          &SaneBreakApp::openFocusWindow);
  connect(tray, &StatusTrayWindow::endFocusRequested, this, &SaneBreakApp::endFocus);
  connect(tray, &StatusTrayWindow::endMeetingRequested, this,
          [this]() { endMeetingBreakLater(preferences->smallEvery->get()); });
  connect(tray, &StatusTrayWindow::extendMeetingRequested, this,
          &SaneBreakApp::extendMeeting);
  connect(tray, &StatusTrayWindow::preferenceWindowRequested, this,
          &SaneBreakApp::showPreferences);
  connect(tray, &StatusTrayWindow::statsRequested, this,
          &SaneBreakApp::openStatsWindow);
  connect(tray, &StatusTrayWindow::enableBreakRequested, this,
          &SaneBreakApp::enableBreak);
  connect(tray, &StatusTrayWindow::quitRequested, this, &SaneBreakApp::confirmQuit);
  connect(preferences->language, &SettingWithSignal::changed, this,
          [this]() { LanguageSelect::setLanguage(preferences->language->get()); });

  connect(this, &SaneBreakApp::quit, qApp, &QApplication::quit, Qt::QueuedConnection);
  connect(qApp, &QCoreApplication::aboutToQuit, this, &SaneBreakApp::onExit);
#if (defined(Q_OS_LINUX) or defined(Q_OS_MACOS))
  auto signalHandler = new SignalHandler(this);
  signalHandler->setup();
  connect(signalHandler, &SignalHandler::exitRequested, this, &QCoreApplication::quit);
#endif
#ifdef Q_OS_WINDOWS
  qApp->installNativeEventFilter(new SignalHandler());
#endif
}

SaneBreakApp* SaneBreakApp::create(SanePreferences* preferences, QObject* parent) {
  // rawIdleTimer is referenced by BOTH the RemoteActivityMonitor (as its
  // local idle source, to keep the peer-feedback-loop protection described
  // in remote-activity-monitor.h) and EffectiveIdleTime (as the wrapped
  // timer). Single raw instance, two consumers.
  SystemIdleTime* rawIdleTimer = createIdleTimer(parent);
  auto* ram = new peer::RemoteActivityMonitor(preferences, rawIdleTimer, parent);
  auto* idleTimer = new EffectiveIdleTime(rawIdleTimer, ram, parent);
  AppDependencies deps = {
      .preferences = preferences,
      .db = new BreakDatabase(QSqlDatabase::addDatabase("QSQLITE")),
      .countDownTimer = new Timer(),
      .screenLockTimer = new Timer(),
      .idleTimer = idleTimer,
      .systemMonitor = new SystemMonitor(preferences),
      .breakWindows = new BreakWindows(),
      .meetingPrompt = new MeetingPrompt(parent, preferences),
      .remoteActivityMonitor = ram,
  };
  return new SaneBreakApp(deps, parent);
}

void SaneBreakApp::start() {
  AbstractApp::start();
  if (m_ram && preferences->peerFusionEnabled->get()) {
    m_ram->start();
    snapshotLastGoodPeerBinding();
  }
  tray->show();
}

void SaneBreakApp::snapshotLastGoodPeerBinding() {
  if (!m_ram || !m_ram->isRunning()) return;
  m_lastGoodPort = preferences->peerListenPort->get();
  m_lastGoodInterfaces = preferences->peerBroadcastInterfaces->get();
}

void SaneBreakApp::onPeerFusionToggled() {
  if (!m_ram) return;
  if (preferences->peerFusionEnabled->get()) {
    m_ram->start();
    snapshotLastGoodPeerBinding();
  } else {
    m_ram->stop();
  }
}

void SaneBreakApp::onPeerBindingChanged() {
  if (!m_ram) return;
  if (!preferences->peerFusionEnabled->get()) return;
  // Reverting preferences below fires the same `changed` signal; guard
  // against reentering this handler during the revert.
  if (m_peerRebindGuard) return;
  m_peerRebindGuard = true;

  m_ram->stop();
  m_ram->start();

  if (!m_ram->isRunning()) {
    // Rebind with the new port / interface list failed. Revert to the last
    // known-good snapshot (Req 7.8) and re-bind so the user is left in a
    // working state. If no snapshot was ever captured (fusion never
    // successfully bound in this session), fall through and leave ram
    // stopped — the tray / pref-window live indicator surfaces this.
    qWarning("Peer fusion: rebind failed; reverting port/interface settings");
    if (m_lastGoodPort.has_value()) {
      preferences->peerListenPort->set(*m_lastGoodPort);
    }
    if (m_lastGoodInterfaces.has_value()) {
      preferences->peerBroadcastInterfaces->set(*m_lastGoodInterfaces);
    }
    m_ram->start();
  }

  snapshotLastGoodPeerBinding();
  m_peerRebindGuard = false;
}

void SaneBreakApp::doLockScreen() { lockScreen(); }

static void showAndActivate(QWidget* window) {
  window->show();
  window->raise();
  window->activateWindow();
#ifdef Q_OS_MACOS
  macForceActivation();
#endif
}

void SaneBreakApp::showPreferences() { showAndActivate(prefWindow); }

void SaneBreakApp::openStatsWindow() {
  if (!statsWindow) statsWindow = new StatsWindow(db);
  showAndActivate(statsWindow);
}

void SaneBreakApp::openFocusWindow() {
  if (data->isFocusMode() || m_currentState->getID() == AppState::Meeting ||
      data->isPostponing())
    return;
  if (!focusWindow) {
    focusWindow = new FocusWindow(preferences);
    connect(focusWindow, &FocusWindow::focusRequested, this, &SaneBreakApp::startFocus);
  }
  showAndActivate(focusWindow);
}

void SaneBreakApp::openPostponeWindow() {
  if (data->isFocusMode()) {
    QMessageBox msgBox;
    msgBox.setText(tr("Cannot postpone during focus mode."));
    msgBox.setInformativeText(tr("End focus mode first if you want to postpone."));
    msgBox.setIcon(QMessageBox::Icon::Warning);
    msgBox.addButton(QMessageBox::Ok)->setText(tr("OK"));
    msgBox.exec();
    return;
  }
  if (data->isPostponing()) {
    QMessageBox msgBox;
    msgBox.setText(tr("You have already postponed this break once."));
    msgBox.setInformativeText(tr("No further postpones are allowed."));
    msgBox.setIcon(QMessageBox::Icon::Warning);
    msgBox.addButton(QMessageBox::Ok)->setText(tr("OK"));
    msgBox.setDefaultButton(QMessageBox::Cancel);
    msgBox.exec();
    return;
  }
  if (!postponeWindow) {
    postponeWindow = new PostponeWindow(preferences, db);
    connect(postponeWindow, &PostponeWindow::postponeRequested, this,
            &SaneBreakApp::postpone);
  }
  showAndActivate(postponeWindow);
}

void SaneBreakApp::openMeetingWindow() {
  if (m_currentState->getID() == AppState::Meeting) return;
  if (!meetingWindow) {
    meetingWindow = new MeetingWindow(preferences, db);
    connect(meetingWindow, &MeetingWindow::meetingRequested, this,
            [this](QTime endTime, QString reason) {
              int seconds = QTime::currentTime().secsTo(endTime);
              if (seconds > 0) startMeeting(seconds, reason);
            });
  }
  showAndActivate(meetingWindow);
}

void SaneBreakApp::confirmQuit() {
  QMessageBox msgBox;
  msgBox.setText(tr("Are you sure to quit Sane Break?"));
  msgBox.setInformativeText(tr("You can postpone the breaks instead."));
  msgBox.setIcon(QMessageBox::Icon::Question);
  msgBox.addButton(QMessageBox::Cancel)->setText(tr("Cancel"));
  msgBox.addButton(tr("Postpone"), QMessageBox::NoRole);
  msgBox.addButton(QMessageBox::Yes)->setText(tr("Yes"));

  msgBox.setDefaultButton(QMessageBox::Cancel);
  switch (msgBox.exec()) {
    case QMessageBox::Yes:
      emit quit();
      return;
    case QMessageBox::Cancel:
      return;
    default:
      openPostponeWindow();
      return;
  }
}
