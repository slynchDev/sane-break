// Sane Break is a gentle break reminder that helps you avoid mindlessly skipping breaks
// Copyright (C) 2026 Sane Break developers
// SPDX-License-Identifier: GPL-3.0-or-later

#include "peer-packet.h"

#include <QCryptographicHash>
#include <QMessageAuthenticationCode>
#include <QtEndian>
#include <algorithm>
#include <cstring>

namespace peer {

namespace {

constexpr int kHmacBytes = 32;
constexpr int kSenderUuidBytes = 16;

// Constant-time byte comparison to prevent HMAC timing attacks.
bool constantTimeEquals(const QByteArray& a, const QByteArray& b) {
  if (a.size() != b.size()) return false;
  unsigned char diff = 0;
  for (int i = 0; i < a.size(); ++i) {
    diff |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]);
  }
  return diff == 0;
}

void appendUint8(QByteArray& buf, uint8_t v) {
  buf.append(static_cast<char>(v));
}

void appendUint16Be(QByteArray& buf, quint16 v) {
  char bytes[2];
  qToBigEndian<quint16>(v, bytes);
  buf.append(bytes, 2);
}

void appendUint64Be(QByteArray& buf, quint64 v) {
  char bytes[8];
  qToBigEndian<quint64>(v, bytes);
  buf.append(bytes, 8);
}

void appendInt64Be(QByteArray& buf, qint64 v) {
  char bytes[8];
  qToBigEndian<qint64>(v, bytes);
  buf.append(bytes, 8);
}

}  // namespace

QByteArray encodePacket(const Packet& p, const QByteArray& secret) {
  // Reject oversized payloads defensively — payload_length is a 2-byte field,
  // and truncating silently would produce a malformed packet that the decoder
  // rejects with a confusing "payload_length mismatch" error. Return an empty
  // QByteArray to signal the encoder-side failure to the caller.
  if (p.payload.size() < 0 || p.payload.size() > 0xFFFF) return {};

  QByteArray body;
  body.reserve(kMaxPacketBytes);

  // magic(4)
  body.append(kMagic.data(), kMagic.size());

  // version(1), key_id(1)
  appendUint8(body, kVersion);
  appendUint8(body, kKeyIdV1);

  // sender_uuid(16) — left-pad/truncate to exactly 16 bytes
  QByteArray uuid = p.senderUuid.leftJustified(kSenderUuidBytes, '\0', true);
  body.append(uuid.constData(), kSenderUuidBytes);

  // hostname_len(1) | hostname(0..63 UTF-8)
  QByteArray hostname = p.hostname.toUtf8().left(kMaxHostnameBytes);
  appendUint8(body, static_cast<uint8_t>(hostname.size()));
  body.append(hostname);

  // timestamp(8 BE) | nonce(8) | event_type(1) | payload_length(2 BE) | payload
  appendInt64Be(body, p.timestamp);
  appendUint64Be(body, p.nonce);
  appendUint8(body, p.eventType);
  appendUint16Be(body, static_cast<quint16>(p.payload.size()));
  body.append(p.payload);

  // HMAC trailer over everything above
  QByteArray mac =
      QMessageAuthenticationCode::hash(body, secret, QCryptographicHash::Sha256);
  body.append(mac);
  return body;
}

std::optional<Packet> decodePacket(const QByteArray& bytes, const QByteArray& secret,
                                   QString* whyDropped) {
  auto drop = [whyDropped](const char* reason) -> std::optional<Packet> {
    if (whyDropped) *whyDropped = QString::fromLatin1(reason);
    return std::nullopt;
  };

  if (bytes.size() < kMinPacketBytes || bytes.size() > kMaxPacketBytes) {
    return drop("size out of range");
  }

  // Authenticate-then-parse: verify HMAC on the full body before touching any
  // attacker-controlled fields. This bounds the pre-auth attack surface to the
  // fixed-size bound check above and the constant-time HMAC compare below.
  QByteArray body = bytes.left(bytes.size() - kHmacBytes);
  QByteArray hmacField = bytes.right(kHmacBytes);
  QByteArray expected =
      QMessageAuthenticationCode::hash(body, secret, QCryptographicHash::Sha256);
  if (!constantTimeEquals(hmacField, expected)) return drop("hmac mismatch");

  int pos = 0;
  auto readBytes = [&](int n) -> QByteArray {
    QByteArray out = bytes.mid(pos, n);
    pos += n;
    return out;
  };

  // magic(4)
  if (std::memcmp(bytes.constData() + pos, kMagic.data(), kMagic.size()) != 0) {
    return drop("magic mismatch");
  }
  pos += kMagic.size();

  // version(1)
  uint8_t version = static_cast<uint8_t>(bytes[pos++]);
  if (version != kVersion) return drop("unknown version");

  // key_id(1)
  uint8_t keyId = static_cast<uint8_t>(bytes[pos++]);
  if (keyId != kKeyIdV1) return drop("unknown key_id");

  // sender_uuid(16)
  QByteArray senderUuid = readBytes(kSenderUuidBytes);

  // hostname_len(1)
  uint8_t hostnameLen = static_cast<uint8_t>(bytes[pos++]);
  if (hostnameLen > kMaxHostnameBytes) return drop("hostname_len out of range");

  // hostname(hostnameLen)
  if (pos + hostnameLen > bytes.size()) return drop("hostname overruns buffer");
  QByteArray hostnameBytes = readBytes(hostnameLen);

  // timestamp(8 BE) + nonce(8 BE) + event_type(1) + payload_length(2 BE)
  if (pos + 8 + 8 + 1 + 2 > bytes.size()) return drop("fixed fields overrun buffer");

  qint64 timestamp = qFromBigEndian<qint64>(bytes.constData() + pos);
  pos += 8;
  quint64 nonce = qFromBigEndian<quint64>(bytes.constData() + pos);
  pos += 8;
  uint8_t eventType = static_cast<uint8_t>(bytes[pos++]);
  quint16 payloadLength = qFromBigEndian<quint16>(bytes.constData() + pos);
  pos += 2;

  if (eventType != EVENT_ACTIVITY && eventType != EVENT_IDLE_TRANSITION) {
    return drop("unknown event_type");
  }

  // The bytes after the payload should be exactly the 32-byte HMAC trailer.
  int expectedRemaining = static_cast<int>(payloadLength) + kHmacBytes;
  if (bytes.size() - pos != expectedRemaining) {
    return drop("payload_length mismatch");
  }

  // Event-type-specific payload shape (Requirements 4.7, 4.8).
  if (eventType == EVENT_ACTIVITY && payloadLength != 4) {
    return drop("bad ACTIVITY payload size");
  }
  if (eventType == EVENT_IDLE_TRANSITION && payloadLength != 1) {
    return drop("bad IDLE_TRANSITION payload size");
  }

  QByteArray payload = readBytes(payloadLength);

  if (eventType == EVENT_IDLE_TRANSITION) {
    uint8_t state = static_cast<uint8_t>(payload[0]);
    if (state != STATE_IDLE && state != STATE_ACTIVE) {
      return drop("bad IDLE_TRANSITION state byte");
    }
  }

  Packet p;
  p.senderUuid = senderUuid;
  p.hostname = QString::fromUtf8(hostnameBytes);
  p.timestamp = timestamp;
  p.nonce = nonce;
  p.eventType = eventType;
  p.payload = payload;
  return p;
}

// ---- PeerReplayBuffer ------------------------------------------------------

PeerReplayBuffer::PeerReplayBuffer(int capacity)
    : m_capacity(std::max(1, capacity)) {
  m_buffer.reserve(static_cast<size_t>(m_capacity));
}

bool PeerReplayBuffer::contains(const QByteArray& senderUuid, quint64 nonce) const {
  for (int i = 0; i < m_size; ++i) {
    const Entry& e = m_buffer[i];
    if (e.nonce == nonce && e.senderUuid == senderUuid) return true;
  }
  return false;
}

void PeerReplayBuffer::insert(const QByteArray& senderUuid, quint64 nonce) {
  Entry entry{senderUuid, nonce};
  if (m_size < m_capacity) {
    m_buffer.push_back(entry);
    m_size = static_cast<int>(m_buffer.size());
    m_nextIndex = m_size % m_capacity;
  } else {
    m_buffer[m_nextIndex] = entry;
    m_nextIndex = (m_nextIndex + 1) % m_capacity;
  }
}

}  // namespace peer
