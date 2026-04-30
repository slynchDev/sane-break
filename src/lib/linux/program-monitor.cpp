
// Sane Break is a gentle break reminder that helps you avoid mindlessly skipping breaks
// Copyright (C) 2024-2026 Sane Break developers
// SPDX-License-Identifier: GPL-3.0-or-later

#include "program-monitor.h"

#include <QProcess>
#include <QString>
#include <QStringList>
#include <Qt>

static constexpr int kSubprocessTimeoutMs = 2000;

const QStringList RunningProgramsMonitor::runningPrograms() {
  QProcess process;
  process.start("ps", QStringList() << "-e" << "-o" << "args=");
  if (!process.waitForFinished(kSubprocessTimeoutMs)) {
    process.kill();
    return {};
  }
  QString output = process.readAllStandardOutput();
  if (output.isEmpty()) return {};
  return output.split('\n', Qt::SkipEmptyParts);
}

static QStringList parsePactlOutput(const QString& output) {
  QStringList result;
  for (const QString& line : output.split('\n')) {
    QString trimmed = line.trimmed();
    // Match exactly `application.process.binary = "..."` or `application.name = "..."`
    // to avoid capturing keys like `application.process.binary.name` or
    // `application.name.icon`.
    if (!trimmed.startsWith("application.process.binary = \"") &&
        !trimmed.startsWith("application.name = \"")) {
      continue;
    }
    int start = trimmed.indexOf('"');
    int end = trimmed.lastIndexOf('"');
    if (start >= 0 && end > start) {
      result.append(trimmed.mid(start + 1, end - start - 1));
    }
  }
  return result;
}

static constexpr int kAudioQueryWatchdogMs = 5000;

void RunningProgramsMonitor::startAudioProgramsQuery() {
  // Async query PulseAudio/PipeWire for active source outputs. Results land
  // in m_lastAudioPrograms via QProcess::finished so tick() never blocks on
  // pactl. An active source output indicates a meeting/call is in progress.
  if (m_audioProcess &&
      m_audioProcess->state() != QProcess::NotRunning) {
    // Previous query still in flight; skip to avoid piling up.
    return;
  }
  if (!m_audioProcess) {
    m_audioProcess = new QProcess(this);
    m_audioWatchdog = new QTimer(this);
    m_audioWatchdog->setSingleShot(true);
    m_audioWatchdog->setInterval(kAudioQueryWatchdogMs);
    connect(m_audioWatchdog, &QTimer::timeout, this, [this]() {
      // pactl hung; kill it. QProcess::finished will fire with CrashExit and
      // the handler below clears m_lastAudioPrograms.
      if (m_audioProcess->state() != QProcess::NotRunning) m_audioProcess->kill();
    });
    connect(
        m_audioProcess,
        QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
        [this](int, QProcess::ExitStatus status) {
          m_audioWatchdog->stop();
          if (status != QProcess::NormalExit) {
            m_lastAudioPrograms.clear();
            return;
          }
          m_lastAudioPrograms = parsePactlOutput(m_audioProcess->readAllStandardOutput());
        });
    // Covers FailedToStart (pactl missing from PATH) and other startup errors
    // where `finished` may not fire; ensures the cache never retains stale data.
    connect(m_audioProcess, &QProcess::errorOccurred, this,
            [this](QProcess::ProcessError) {
              m_audioWatchdog->stop();
              m_lastAudioPrograms.clear();
            });
  }
  m_audioProcess->start("pactl", QStringList() << "list" << "source-outputs");
  m_audioWatchdog->start();
}
