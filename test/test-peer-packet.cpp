// Sane Break is a gentle break reminder that helps you avoid mindlessly skipping breaks
// Copyright (C) 2026 Sane Break developers
// SPDX-License-Identifier: GPL-3.0-or-later

#include <qtestcase.h>

#include <QByteArray>
#include <QCryptographicHash>
#include <QMessageAuthenticationCode>
#include <QObject>
#include <QString>
#include <QTest>
#include <QtEndian>

#include "lib/peer-packet.h"

namespace {

QByteArray makeUuid(uint8_t prefix) {
  QByteArray u(16, '\0');
  u[0] = static_cast<char>(prefix);
  return u;
}

QByteArray makeSecret(uint8_t fill = 0x42) { return QByteArray(32, static_cast<char>(fill)); }

peer::Packet makeActivityPacket(uint8_t uuidPrefix = 0xAA, quint64 nonce = 42) {
  peer::Packet p;
  p.senderUuid = makeUuid(uuidPrefix);
  p.hostname = "r16";
  p.timestamp = 1'700'000'000;
  p.nonce = nonce;
  p.eventType = peer::EVENT_ACTIVITY;
  p.payload = QByteArray(4, '\0');
  p.payload[3] = 5;  // event_count = 5 (big-endian)
  return p;
}

peer::Packet makeIdleTransition(uint8_t state, uint8_t uuidPrefix = 0xBB) {
  peer::Packet p;
  p.senderUuid = makeUuid(uuidPrefix);
  p.hostname = "hp";
  p.timestamp = 1'700'000'100;
  p.nonce = 77;
  p.eventType = peer::EVENT_IDLE_TRANSITION;
  p.payload.append(static_cast<char>(state));
  return p;
}

// Manually build a body + HMAC so tests can craft malformed packets that
// pass the HMAC check (isolating the decoder's structural validations
// from the HMAC rejection path).
QByteArray buildSignedRaw(std::array<char, 4> magic, uint8_t version, uint8_t keyId,
                          const QByteArray& uuid16, uint8_t hostnameLenField,
                          const QByteArray& hostnameBytes, qint64 timestamp,
                          quint64 nonce, uint8_t eventType, quint16 payloadLenField,
                          const QByteArray& payload, const QByteArray& secret) {
  QByteArray body;
  body.append(magic.data(), magic.size());
  body.append(static_cast<char>(version));
  body.append(static_cast<char>(keyId));
  body.append(uuid16.leftJustified(16, '\0', true));
  body.append(static_cast<char>(hostnameLenField));
  body.append(hostnameBytes);
  char be[8];
  qToBigEndian<qint64>(timestamp, be);
  body.append(be, 8);
  qToBigEndian<quint64>(nonce, be);
  body.append(be, 8);
  body.append(static_cast<char>(eventType));
  char belen[2];
  qToBigEndian<quint16>(payloadLenField, belen);
  body.append(belen, 2);
  body.append(payload);
  QByteArray mac =
      QMessageAuthenticationCode::hash(body, secret, QCryptographicHash::Sha256);
  return body + mac;
}

}  // namespace

class TestPeerPacket : public QObject {
  Q_OBJECT

 private slots:
  // ---- Round trips -----------------------------------------------------

  void round_trip_activity() {
    auto secret = makeSecret();
    auto p = makeActivityPacket();
    QByteArray bytes = peer::encodePacket(p, secret);
    QVERIFY(bytes.size() <= peer::kMaxPacketBytes);
    auto decoded = peer::decodePacket(bytes, secret);
    QVERIFY(decoded.has_value());
    QCOMPARE(decoded->senderUuid, p.senderUuid);
    QCOMPARE(decoded->hostname, QString("r16"));
    QCOMPARE(decoded->timestamp, qint64(1'700'000'000));
    QCOMPARE(decoded->nonce, quint64(42));
    QCOMPARE(uint8_t(decoded->eventType), uint8_t(peer::EVENT_ACTIVITY));
    QCOMPARE(decoded->payload.size(), qsizetype(4));
  }

  void round_trip_idle_transition_active() {
    auto secret = makeSecret();
    auto p = makeIdleTransition(peer::STATE_ACTIVE);
    QByteArray bytes = peer::encodePacket(p, secret);
    auto decoded = peer::decodePacket(bytes, secret);
    QVERIFY(decoded.has_value());
    QCOMPARE(decoded->payload.size(), qsizetype(1));
    QCOMPARE(uint8_t(decoded->payload.at(0)), uint8_t(peer::STATE_ACTIVE));
  }

  void round_trip_idle_transition_idle() {
    auto secret = makeSecret();
    auto p = makeIdleTransition(peer::STATE_IDLE);
    QByteArray bytes = peer::encodePacket(p, secret);
    auto decoded = peer::decodePacket(bytes, secret);
    QVERIFY(decoded.has_value());
    QCOMPARE(uint8_t(decoded->payload.at(0)), uint8_t(peer::STATE_IDLE));
  }

  void round_trip_empty_hostname() {
    auto secret = makeSecret();
    auto p = makeActivityPacket();
    p.hostname = "";
    auto bytes = peer::encodePacket(p, secret);
    auto decoded = peer::decodePacket(bytes, secret);
    QVERIFY(decoded.has_value());
    QCOMPARE(decoded->hostname, QString());
  }

  // ---- Decoder rejection paths ----------------------------------------

  void decode_fails_on_magic_mismatch() {
    auto secret = makeSecret();
    auto bytes = peer::encodePacket(makeActivityPacket(), secret);
    bytes[0] = 'X';
    QVERIFY(!peer::decodePacket(bytes, secret).has_value());
  }

  void decode_fails_on_wrong_version() {
    auto secret = makeSecret();
    auto bytes = buildSignedRaw(peer::kMagic, 0x02, peer::kKeyIdV1, makeUuid(1), 0, {},
                                1'700'000'000, 1, peer::EVENT_ACTIVITY, 4,
                                QByteArray(4, '\0'), secret);
    QString why;
    QVERIFY(!peer::decodePacket(bytes, secret, &why).has_value());
    QCOMPARE(why, QString("unknown version"));
  }

  void decode_fails_on_wrong_key_id() {
    auto secret = makeSecret();
    auto bytes = buildSignedRaw(peer::kMagic, peer::kVersion, 0x01, makeUuid(1), 0, {},
                                1'700'000'000, 1, peer::EVENT_ACTIVITY, 4,
                                QByteArray(4, '\0'), secret);
    QString why;
    QVERIFY(!peer::decodePacket(bytes, secret, &why).has_value());
    QCOMPARE(why, QString("unknown key_id"));
  }

  void decode_fails_on_hostname_len_100() {
    auto secret = makeSecret();
    // hostname_len field = 100 but actual hostname bytes are empty. Decoder
    // must reject on the length-field bound before any buffer reads.
    auto bytes = buildSignedRaw(peer::kMagic, peer::kVersion, peer::kKeyIdV1,
                                makeUuid(1), 100, {}, 1'700'000'000, 1,
                                peer::EVENT_ACTIVITY, 4, QByteArray(4, '\0'), secret);
    QString why;
    QVERIFY(!peer::decodePacket(bytes, secret, &why).has_value());
    QCOMPARE(why, QString("hostname_len out of range"));
  }

  void decode_fails_on_truncated_buffer() {
    auto secret = makeSecret();
    auto bytes = peer::encodePacket(makeActivityPacket(), secret);
    bytes.chop(5);
    QVERIFY(!peer::decodePacket(bytes, secret).has_value());
  }

  void decode_fails_on_payload_length_overflow() {
    auto secret = makeSecret();
    // Claim payload_length=0xFFFF but only supply 4 bytes of actual payload.
    auto bytes = buildSignedRaw(peer::kMagic, peer::kVersion, peer::kKeyIdV1,
                                makeUuid(1), 0, {}, 1'700'000'000, 1,
                                peer::EVENT_ACTIVITY, 0xFFFF, QByteArray(4, '\0'),
                                secret);
    QString why;
    QVERIFY(!peer::decodePacket(bytes, secret, &why).has_value());
    QCOMPARE(why, QString("payload_length mismatch"));
  }

  void decode_fails_on_unknown_event_type() {
    auto secret = makeSecret();
    auto bytes = buildSignedRaw(peer::kMagic, peer::kVersion, peer::kKeyIdV1,
                                makeUuid(1), 0, {}, 1'700'000'000, 1, 0x99, 4,
                                QByteArray(4, '\0'), secret);
    QString why;
    QVERIFY(!peer::decodePacket(bytes, secret, &why).has_value());
    QCOMPARE(why, QString("unknown event_type"));
  }

  void decode_fails_on_activity_payload_wrong_size() {
    auto secret = makeSecret();
    // ACTIVITY with payload_length=3
    auto bytes3 = buildSignedRaw(peer::kMagic, peer::kVersion, peer::kKeyIdV1,
                                 makeUuid(1), 0, {}, 1'700'000'000, 1,
                                 peer::EVENT_ACTIVITY, 3, QByteArray(3, '\0'), secret);
    QString why;
    QVERIFY(!peer::decodePacket(bytes3, secret, &why).has_value());
    QCOMPARE(why, QString("bad ACTIVITY payload size"));
    // ACTIVITY with payload_length=5
    auto bytes5 = buildSignedRaw(peer::kMagic, peer::kVersion, peer::kKeyIdV1,
                                 makeUuid(1), 0, {}, 1'700'000'000, 1,
                                 peer::EVENT_ACTIVITY, 5, QByteArray(5, '\0'), secret);
    QVERIFY(!peer::decodePacket(bytes5, secret, &why).has_value());
    QCOMPARE(why, QString("bad ACTIVITY payload size"));
  }

  void decode_fails_on_idle_transition_payload_wrong_size() {
    auto secret = makeSecret();
    auto bytes0 = buildSignedRaw(peer::kMagic, peer::kVersion, peer::kKeyIdV1,
                                 makeUuid(1), 0, {}, 1'700'000'000, 1,
                                 peer::EVENT_IDLE_TRANSITION, 0, {}, secret);
    QString why;
    QVERIFY(!peer::decodePacket(bytes0, secret, &why).has_value());
    QCOMPARE(why, QString("bad IDLE_TRANSITION payload size"));
    auto bytes2 = buildSignedRaw(peer::kMagic, peer::kVersion, peer::kKeyIdV1,
                                 makeUuid(1), 0, {}, 1'700'000'000, 1,
                                 peer::EVENT_IDLE_TRANSITION, 2, QByteArray(2, '\0'),
                                 secret);
    QVERIFY(!peer::decodePacket(bytes2, secret, &why).has_value());
    QCOMPARE(why, QString("bad IDLE_TRANSITION payload size"));
  }

  void decode_fails_on_idle_transition_bad_state_byte() {
    auto secret = makeSecret();
    QByteArray p02(1, '\x02');
    auto bytes = buildSignedRaw(peer::kMagic, peer::kVersion, peer::kKeyIdV1,
                                makeUuid(1), 0, {}, 1'700'000'000, 1,
                                peer::EVENT_IDLE_TRANSITION, 1, p02, secret);
    QString why;
    QVERIFY(!peer::decodePacket(bytes, secret, &why).has_value());
    QCOMPARE(why, QString("bad IDLE_TRANSITION state byte"));
    QByteArray pFF(1, static_cast<char>(0xFF));
    auto bytesFF = buildSignedRaw(peer::kMagic, peer::kVersion, peer::kKeyIdV1,
                                  makeUuid(1), 0, {}, 1'700'000'000, 1,
                                  peer::EVENT_IDLE_TRANSITION, 1, pFF, secret);
    QVERIFY(!peer::decodePacket(bytesFF, secret, &why).has_value());
    QCOMPARE(why, QString("bad IDLE_TRANSITION state byte"));
  }

  void decode_fails_on_tampered_hmac() {
    auto secret = makeSecret();
    auto bytes = peer::encodePacket(makeActivityPacket(), secret);
    bytes[bytes.size() - 1] = static_cast<char>(bytes[bytes.size() - 1] ^ 0xFF);
    QVERIFY(!peer::decodePacket(bytes, secret).has_value());
  }

  void decode_fails_with_wrong_secret() {
    auto bytes = peer::encodePacket(makeActivityPacket(), makeSecret(0x42));
    QVERIFY(!peer::decodePacket(bytes, makeSecret(0x11)).has_value());
  }

  void decode_fails_on_tiny_buffer() {
    QByteArray tiny(10, '\0');
    QVERIFY(!peer::decodePacket(tiny, makeSecret()).has_value());
  }

  // ---- Replay buffer --------------------------------------------------

  void replay_first_insert_wins() {
    peer::PeerReplayBuffer buf(4);
    QVERIFY(!buf.contains(makeUuid(1), 10));
    buf.insert(makeUuid(1), 10);
    QVERIFY(buf.contains(makeUuid(1), 10));
  }

  void replay_distinguishes_uuid() {
    peer::PeerReplayBuffer buf(4);
    buf.insert(makeUuid(1), 10);
    QVERIFY(!buf.contains(makeUuid(2), 10));
  }

  void replay_distinguishes_nonce() {
    peer::PeerReplayBuffer buf(4);
    buf.insert(makeUuid(1), 10);
    QVERIFY(!buf.contains(makeUuid(1), 11));
  }

  void replay_eviction_at_capacity() {
    peer::PeerReplayBuffer buf(2);
    buf.insert(makeUuid(1), 10);
    buf.insert(makeUuid(2), 20);
    QCOMPARE(buf.size(), 2);
    // 3rd insert evicts oldest (1, 10)
    buf.insert(makeUuid(3), 30);
    QCOMPARE(buf.size(), 2);
    QVERIFY(!buf.contains(makeUuid(1), 10));
    QVERIFY(buf.contains(makeUuid(2), 20));
    QVERIFY(buf.contains(makeUuid(3), 30));
  }

  void replay_257th_evicts_first() {
    peer::PeerReplayBuffer buf(256);
    for (quint64 i = 0; i < 256; ++i) buf.insert(makeUuid(1), i);
    QCOMPARE(buf.size(), 256);
    QVERIFY(buf.contains(makeUuid(1), 0));
    buf.insert(makeUuid(1), 256);  // 257th insert
    QCOMPARE(buf.size(), 256);
    QVERIFY(!buf.contains(makeUuid(1), 0));
    QVERIFY(buf.contains(makeUuid(1), 256));
    QVERIFY(buf.contains(makeUuid(1), 1));
  }
};

QTEST_MAIN(TestPeerPacket)
#include "test-peer-packet.moc"
