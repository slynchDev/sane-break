// Sane Break is a gentle break reminder that helps you avoid mindlessly skipping breaks
// Copyright (C) 2024-2026 Sane Break developers
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QApplication>
#include <QCoreApplication>
#include <QDialog>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFontDatabase>
#include <QLockFile>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QSystemTrayIcon>
#include <QThread>
#include <QTranslator>
#include <Qt>

#include "app.h"
#include "app/welcome.h"
#include "app/widgets/language-select.h"
#include "config.h"
#include "core/preferences.h"

#ifdef Q_OS_LINUX
#include <QDBusConnection>
#include <QDBusConnectionInterface>

#include "lib/linux/system-check.h"
#endif

#ifdef Q_OS_LINUX
// Qt probes the session bus for org.kde.StatusNotifierWatcher the first time
// anything asks about the system tray and caches the answer for the life of
// the process (isDBusTrayAvailable() in qgenericunixthemes.cpp). When the app
// is launched alongside the panel at session start, that first probe can run
// before the panel has claimed the name — and then no amount of retrying
// QSystemTrayIcon::isSystemTrayAvailable() will ever see the tray. So wait for
// the watcher name directly (an uncached query) before touching the tray API.
bool waitForTrayWatcher(int timeoutMs) {
  QDBusConnectionInterface* bus = QDBusConnection::sessionBus().interface();
  if (bus == nullptr) return false;
  QElapsedTimer elapsed;
  elapsed.start();
  while (!bus->isServiceRegistered("org.kde.StatusNotifierWatcher")) {
    if (elapsed.elapsed() >= timeoutMs) return false;
    QThread::msleep(250);
  }
  return true;
}
#endif

bool waitForTray() {
#ifdef Q_OS_LINUX
  if (!waitForTrayWatcher(30000))
    qWarning() << "No StatusNotifierWatcher on the session bus after 30s.";
#endif
  for (int attempt = 0; attempt < 5; attempt++) {
    if (QSystemTrayIcon::isSystemTrayAvailable()) return true;
    QThread::sleep(1);
  }
  qWarning() << "System tray not available.";
  return false;
}

int main(int argc, char* argv[]) {
  QCoreApplication::setOrganizationName("SaneBreak");
  QCoreApplication::setApplicationName("SaneBreak");
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QApplication a(argc, argv);

  a.setApplicationDisplayName("Sane Break");
  if (waitForTray()) a.setQuitOnLastWindowClosed(false);

  SanePreferences* preferences = SanePreferences::createDefault();

#ifdef WITH_TRANSLATIONS
  LanguageSelect::setLanguage(preferences->language->get());
#endif

  // Use lock file to avoid starting multiple instances of app
  const QString lockFilePath =
      QDir(QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation))
#ifndef NDEBUG
          .filePath("sane-break-debug.lock");
#else
          .filePath("sane-break.lock");
#endif
  QLockFile lock(lockFilePath);
  lock.setStaleLockTime(0);
  if (!lock.tryLock() && lock.error() == QLockFile::LockFailedError) {
    QMessageBox msgBox;
    msgBox.setText(QCoreApplication::tr("Another instance of Sane Break is running."));
    msgBox.setInformativeText(QCoreApplication::tr(
        "Please quit the old instance before starting a new one. "
        "If the previous instance has already exited, you can start a new "
        "one anyway."));
    msgBox.setIcon(QMessageBox::Icon::Question);
    msgBox.addButton(QCoreApplication::tr("Quit"), QMessageBox::RejectRole);
    QPushButton* startBtn =
        msgBox.addButton(QCoreApplication::tr("Start Anyway"), QMessageBox::AcceptRole);
    msgBox.exec();
    if (msgBox.clickedButton() != startBtn) return 1;
    lock.removeStaleLockFile();
    if (!lock.tryLock()) {
      QMessageBox::critical(
          nullptr, "Sane Break",
          QCoreApplication::tr(
              "Could not start because another instance is still running. "
              "Please manually delete \"%1\" and try again.")
              .arg(lockFilePath));
      return 1;
    }
  }

#ifdef Q_OS_LINUX
  QDir appPath = a.applicationDirPath();
  appPath.cdUp();
  appPath.cd(CMAKE_INSTALL_LIBDIR);
  a.addLibraryPath(appPath.filePath("sane-break"));
  LinuxSystemSupport::check();
#endif  // Q_OS_LINUX

  QFontDatabase::addApplicationFont(":/fonts/bootstrap-icons.ttf");

  if (!QFile::exists(preferences->settings->fileName())) {
    WelcomeWindow* welcome = new WelcomeWindow(preferences);
    if (welcome->exec() == QDialog::Rejected) {
      return 0;
    } else {
      preferences->shownWelcome->set(true);
    }
    welcome->deleteLater();
  }

  SaneBreakApp* app = SaneBreakApp::create(preferences, &a);

  app->start();
  return a.exec();
}
