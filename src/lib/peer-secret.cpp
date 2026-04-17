// Sane Break is a gentle break reminder that helps you avoid mindlessly skipping breaks
// Copyright (C) 2026 Sane Break developers
// SPDX-License-Identifier: GPL-3.0-or-later

#include "peer-secret.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileDevice>
#include <QFileInfo>

namespace peer {

QString resolvedPeerSecretPath() {
  QByteArray envPath = qgetenv("SANE_BREAK_PEER_SECRET_FILE");
  if (!envPath.isEmpty()) return QString::fromLocal8Bit(envPath);
  return QDir::homePath() + "/.secrets/sane-break-peer";
}

QByteArray loadPeerSecret(QString* outError) {
  auto setError = [outError](const QString& msg) {
    if (outError) *outError = msg;
  };

  QString path = resolvedPeerSecretPath();
  QFileInfo info(path);

  if (!info.exists() || !info.isFile()) {
    setError(QString("peer secret file not found: %1").arg(path));
    return {};
  }

  QFileDevice::Permissions perms = QFile::permissions(path);
  constexpr QFileDevice::Permissions wider = QFileDevice::ReadGroup |
                                             QFileDevice::WriteGroup |
                                             QFileDevice::ExeGroup |
                                             QFileDevice::ReadOther |
                                             QFileDevice::WriteOther |
                                             QFileDevice::ExeOther;
  if (perms & wider) {
    if (!QFile::setPermissions(
            path, QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
      setError(QString("peer secret file has permissions wider than 0600 and "
                       "chmod to 0600 failed: %1")
                   .arg(path));
      return {};
    }
  }

  QFile f(path);
  if (!f.open(QIODevice::ReadOnly)) {
    setError(QString("cannot read peer secret file: %1").arg(path));
    return {};
  }
  QByteArray content = f.readAll();
  f.close();

  // Strip trailing LF and/or CR (handles `\n`, `\r\n`, `\r`).
  while (!content.isEmpty() &&
         (content.back() == '\n' || content.back() == '\r')) {
    content.chop(1);
  }

  if (content.size() != 64) {
    setError(QString("peer secret must be exactly 64 hex characters "
                     "(got %1 after newline strip): %2")
                 .arg(content.size())
                 .arg(path));
    return {};
  }

  // QByteArray::fromHex is lenient (accepts whitespace, ignores non-hex);
  // enforce strict hex by checking each byte ourselves.
  for (char c : content) {
    bool isHex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                 (c >= 'A' && c <= 'F');
    if (!isHex) {
      setError(QString("peer secret contains non-hex characters: %1").arg(path));
      return {};
    }
  }

  QByteArray decoded = QByteArray::fromHex(content);
  if (decoded.size() != 32) {
    setError(QString("peer secret hex decode failed: %1").arg(path));
    return {};
  }

  return decoded;
}

}  // namespace peer
