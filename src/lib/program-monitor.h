// Sane Break is a gentle break reminder that helps you avoid mindlessly skipping breaks
// Copyright (C) 2024-2025 Sane Break developers
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QObject>
#include <QProcess>
#include <QRegularExpression>
#include <QStringList>
#include <QTimer>
#include <QtContainerFwd>

class RunningProgramsMonitor : public QObject {
  Q_OBJECT

 public:
  RunningProgramsMonitor(QObject* parent = nullptr);
  void startMonitoring();
  void stopMonitoring();
  void setPrograms(const QStringList& programs);
  void setMeetingDetectionEnabled(bool enabled);
  bool isAnyProgramRunning() const { return previouslySeen; }

 signals:
  void programStarted();
  void programStopped();
  void meetingStarted();
  void meetingStopped();

 private slots:
  void tick();

 private:
  const QStringList runningPrograms();
  // Platform-specific: starts a non-blocking audio-stream query. Results land
  // in m_lastAudioPrograms via QProcess::finished. macOS/Windows stubs no-op.
  void startAudioProgramsQuery();
  const QRegularExpression validProgramFilter = QRegularExpression(".");
  QTimer* monitorTimer;
  QStringList programsToMonitor;
  bool previouslySeen = false;
  bool previouslyInMeeting = false;
  bool meetingDetectionEnabled = false;
  QProcess* m_audioProcess = nullptr;
  QTimer* m_audioWatchdog = nullptr;
  QStringList m_lastAudioPrograms;
};
