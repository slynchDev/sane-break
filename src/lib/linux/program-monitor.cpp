
// Sane Break is a gentle break reminder that helps you avoid mindlessly skipping breaks
// Copyright (C) 2024-2026 Sane Break developers
// SPDX-License-Identifier: GPL-3.0-or-later

#include "program-monitor.h"

#include <QProcess>
#include <QString>
#include <QStringList>
#include <Qt>

const QStringList RunningProgramsMonitor::runningPrograms() {
  QProcess process;
  process.start("ps", QStringList() << "-e" << "-o" << "args=");
  process.waitForFinished();
  QString output = process.readAllStandardOutput();
  if (output.isEmpty()) return {};
  return output.split('\n', Qt::SkipEmptyParts);
}

const QStringList RunningProgramsMonitor::activeAudioPrograms() {
  // Query PulseAudio/PipeWire for active audio source outputs (microphone streams).
  // An active source output indicates a meeting/call is in progress.
  QProcess process;
  process.start("pactl", QStringList() << "list" << "source-outputs");
  process.waitForFinished();
  QString output = process.readAllStandardOutput();
  if (output.isEmpty()) return {};

  QStringList result;
  for (const QString& line : output.split('\n')) {
    // Lines like: application.process.binary = "zoom"
    // or:         application.name = "ZOOM VoiceEngine"
    if (line.contains("application.process.binary") ||
        line.contains("application.name")) {
      // Extract the value between quotes
      int start = line.indexOf('"');
      int end = line.lastIndexOf('"');
      if (start >= 0 && end > start) {
        result.append(line.mid(start + 1, end - start - 1));
      }
    }
  }
  return result;
}
