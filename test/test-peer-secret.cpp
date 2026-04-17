// Sane Break is a gentle break reminder that helps you avoid mindlessly skipping breaks
// Copyright (C) 2026 Sane Break developers
// SPDX-License-Identifier: GPL-3.0-or-later

#include <qtestcase.h>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QObject>
#include <QString>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QTest>

#include "lib/peer-secret.h"

namespace {

constexpr const char* kHex64 =
    "0123456789abcdef0123456789abcdef"
    "0123456789abcdef0123456789abcdef";

}  // namespace

class TestPeerSecret : public QObject {
  Q_OBJECT

 private:
  QTemporaryDir m_dir;

  QString writeSecret(const QByteArray& content,
                      QFileDevice::Permissions perms = QFileDevice::ReadOwner |
                                                       QFileDevice::WriteOwner) {
    QString path = m_dir.path() + "/sane-break-peer";
    QFile f(path);
    f.open(QIODevice::WriteOnly);
    f.write(content);
    f.close();
    QFile::setPermissions(path, perms);
    qputenv("SANE_BREAK_PEER_SECRET_FILE", path.toLocal8Bit());
    return path;
  }

 private slots:
  void init() { QVERIFY(m_dir.isValid()); }

  void cleanup() { qunsetenv("SANE_BREAK_PEER_SECRET_FILE"); }

  void resolved_path_env_override() {
    qputenv("SANE_BREAK_PEER_SECRET_FILE", "/tmp/custom-path");
    QCOMPARE(peer::resolvedPeerSecretPath(), QString("/tmp/custom-path"));
  }

  void resolved_path_default() {
    qunsetenv("SANE_BREAK_PEER_SECRET_FILE");
    QCOMPARE(peer::resolvedPeerSecretPath(),
             QDir::homePath() + "/.secrets.d/sane-break-peer");
  }

  void valid_hex_round_trips() {
    writeSecret(QByteArray(kHex64));
    QString err;
    QByteArray decoded = peer::loadPeerSecret(&err);
    QVERIFY2(err.isEmpty(), qPrintable(err));
    QCOMPARE(decoded.size(), qsizetype(32));
    // First byte should be 0x01 (from "01" in hex)
    QCOMPARE(uint8_t(decoded[0]), uint8_t(0x01));
    // Last byte should be 0xef (from "ef" in hex)
    QCOMPARE(uint8_t(decoded[31]), uint8_t(0xef));
  }

  void trailing_lf_stripped() {
    writeSecret(QByteArray(kHex64) + "\n");
    QString err;
    QByteArray decoded = peer::loadPeerSecret(&err);
    QVERIFY2(err.isEmpty(), qPrintable(err));
    QCOMPARE(decoded.size(), qsizetype(32));
  }

  void trailing_crlf_stripped() {
    writeSecret(QByteArray(kHex64) + "\r\n");
    QString err;
    QByteArray decoded = peer::loadPeerSecret(&err);
    QVERIFY2(err.isEmpty(), qPrintable(err));
    QCOMPARE(decoded.size(), qsizetype(32));
  }

  void too_short_rejected() {
    writeSecret(QByteArray(kHex64).left(63));
    QString err;
    QVERIFY(peer::loadPeerSecret(&err).isEmpty());
    QVERIFY(err.contains("64 hex characters"));
  }

  void too_long_rejected() {
    writeSecret(QByteArray(kHex64) + "0");
    QString err;
    QVERIFY(peer::loadPeerSecret(&err).isEmpty());
    QVERIFY(err.contains("64 hex characters"));
  }

  void non_hex_rejected() {
    // 64 bytes where one is non-hex
    QByteArray bad(kHex64);
    bad[30] = 'z';
    writeSecret(bad);
    QString err;
    QVERIFY(peer::loadPeerSecret(&err).isEmpty());
    QVERIFY(err.contains("non-hex"));
  }

  void whitespace_rejected() {
    // Embedded space — QByteArray::fromHex would silently accept, strict check rejects
    QByteArray bad = QByteArray(kHex64);
    bad[30] = ' ';
    writeSecret(bad);
    QString err;
    QVERIFY(peer::loadPeerSecret(&err).isEmpty());
    QVERIFY(err.contains("non-hex"));
  }

  void missing_file_rejected() {
    // No writeSecret; env points at a path that doesn't exist
    qputenv("SANE_BREAK_PEER_SECRET_FILE",
            (m_dir.path() + "/does-not-exist").toLocal8Bit());
    QString err;
    QVERIFY(peer::loadPeerSecret(&err).isEmpty());
    QVERIFY(err.contains("not found"));
  }

  void empty_file_rejected() {
    writeSecret(QByteArray());
    QString err;
    QVERIFY(peer::loadPeerSecret(&err).isEmpty());
    QVERIFY(err.contains("64 hex characters"));
  }

  void permissions_auto_repaired() {
    // World-readable perms
    QFileDevice::Permissions wide =
        QFileDevice::ReadOwner | QFileDevice::WriteOwner |
        QFileDevice::ReadGroup | QFileDevice::ReadOther;
    QString path = writeSecret(QByteArray(kHex64), wide);
    QString err;
    QByteArray decoded = peer::loadPeerSecret(&err);
    QVERIFY2(err.isEmpty(), qPrintable(err));
    QCOMPARE(decoded.size(), qsizetype(32));
    // After load, perms should be 0600
    QFileDevice::Permissions after = QFile::permissions(path);
    QVERIFY((after & QFileDevice::ReadGroup) == 0);
    QVERIFY((after & QFileDevice::ReadOther) == 0);
    QVERIFY(after & QFileDevice::ReadOwner);
    QVERIFY(after & QFileDevice::WriteOwner);
  }

  void uppercase_hex_accepted() {
    QByteArray upper =
        QByteArray("0123456789ABCDEF0123456789ABCDEF"
                   "0123456789ABCDEF0123456789ABCDEF");
    writeSecret(upper);
    QString err;
    QByteArray decoded = peer::loadPeerSecret(&err);
    QVERIFY2(err.isEmpty(), qPrintable(err));
    QCOMPARE(decoded.size(), qsizetype(32));
  }
};

QTEST_MAIN(TestPeerSecret)
#include "test-peer-secret.moc"
