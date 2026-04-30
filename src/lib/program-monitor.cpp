// Sane Break is a gentle break reminder that helps you avoid mindlessly skipping breaks
// Copyright (C) 2024-2025 Sane Break developers
// SPDX-License-Identifier: GPL-3.0-or-later

#include "program-monitor.h"

#include <qglobal.h>

#include <QObject>
#include <QProcess>
#include <QTimer>
#include <Qt>
#include <QtContainerFwd>
#include <utility>

RunningProgramsMonitor::RunningProgramsMonitor(QObject* parent)
    : QObject(parent), monitorTimer(new QTimer(this)) {
  connect(monitorTimer, &QTimer::timeout, this, &RunningProgramsMonitor::tick);
}

void RunningProgramsMonitor::startMonitoring() { monitorTimer->start(5000); }
void RunningProgramsMonitor::stopMonitoring() { monitorTimer->stop(); }

void RunningProgramsMonitor::setPrograms(const QStringList& programs) {
  programsToMonitor = programs.filter(validProgramFilter);
  if (programsToMonitor.isEmpty()) {
    if (previouslySeen) {
      emit programStopped();
      previouslySeen = false;
    }
    if (previouslyInMeeting) {
      emit meetingStopped();
      previouslyInMeeting = false;
    }
  }
  // Invalidate stale audio cache — a late async pactl completion from the
  // previous program set must not match against the new set.
  m_lastAudioPrograms.clear();
  tick();
}

void RunningProgramsMonitor::setMeetingDetectionEnabled(bool enabled) {
  if (meetingDetectionEnabled == enabled) return;
  meetingDetectionEnabled = enabled;
  if (!enabled && previouslyInMeeting) {
    emit meetingStopped();
    previouslyInMeeting = false;
  }
  if (!enabled) m_lastAudioPrograms.clear();
}

static bool matchesAny(const QStringList& haystack, const QStringList& needles) {
  for (const QString& item : haystack) {
    for (const QString& entry : needles) {
      if (item.contains(entry, Qt::CaseInsensitive)) {
        return true;
      }
    }
  }
  return false;
}

void RunningProgramsMonitor::tick() {
  if (programsToMonitor.isEmpty()) return;

  bool currentlySeen = matchesAny(runningPrograms(), programsToMonitor);
  if (!previouslySeen && currentlySeen) emit programStarted();
  if (previouslySeen && !currentlySeen) emit programStopped();
  previouslySeen = currentlySeen;

  // Use the audio result cached from the previous tick's async query. The
  // first tick after a program appears will not detect a meeting until the
  // next cycle (a 5s additional delay is acceptable for the feature).
  bool currentlyInMeeting = meetingDetectionEnabled && currentlySeen &&
                            matchesAny(m_lastAudioPrograms, programsToMonitor);
  if (!previouslyInMeeting && currentlyInMeeting) emit meetingStarted();
  if (previouslyInMeeting && !currentlyInMeeting) emit meetingStopped();
  previouslyInMeeting = currentlyInMeeting;

  if (meetingDetectionEnabled && currentlySeen) {
    startAudioProgramsQuery();
  } else {
    m_lastAudioPrograms.clear();
  }
}
