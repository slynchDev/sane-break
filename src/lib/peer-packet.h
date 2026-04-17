// Sane Break is a gentle break reminder that helps you avoid mindlessly skipping breaks
// Copyright (C) 2026 Sane Break developers
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QByteArray>
#include <QString>
#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace peer {

// Raw on-wire bytes. Do NOT use the `uint32_t kMagic = 'SBPA'` multi-character
// literal form — its value is implementation-defined in C++ and naive integer
// serialization would be host-endian-dependent, making little-endian and
// big-endian peers unable to talk to each other.
inline constexpr std::array<char, 4> kMagic = {'S', 'B', 'P', 'A'};

inline constexpr uint8_t kVersion = 0x01;
inline constexpr uint8_t kKeyIdV1 = 0x00;

inline constexpr int kMaxPacketBytes = 512;
inline constexpr int kMaxHostnameBytes = 63;

// Header bytes (all fields except the variable-length hostname and payload):
//   magic(4) + version(1) + key_id(1) + sender_uuid(16) + hostname_len(1)
// + timestamp(8) + nonce(8) + event_type(1) + payload_length(2) + hmac(32)
inline constexpr int kHeaderBytes = 4 + 1 + 1 + 16 + 1 + 8 + 8 + 1 + 2 + 32;
inline constexpr int kMinPacketBytes = kHeaderBytes;  // zero-length hostname and payload

enum EventType : uint8_t {
  EVENT_ACTIVITY = 0x01,
  EVENT_IDLE_TRANSITION = 0x02,
};

enum State : uint8_t {
  STATE_IDLE = 0x00,
  STATE_ACTIVE = 0x01,
};

struct Packet {
  QByteArray senderUuid;   // 16 bytes (RFC4122)
  QString hostname;        // up to 63 UTF-8 bytes
  qint64 timestamp = 0;    // Unix seconds
  quint64 nonce = 0;       // random 8 bytes
  uint8_t eventType = 0;   // one of EventType
  QByteArray payload;      // event-type-specific
};

// Serialize `p` with the provided 32-byte HMAC-SHA256 secret.
// Always stamps `kKeyIdV1 = 0x00`. Returns the full datagram including
// HMAC trailer. The payload is written as-is; callers are responsible
// for matching event-type-specific payload shapes (task 2.2).
QByteArray encodePacket(const Packet& p, const QByteArray& secret);

// Decode + verify a received datagram. On any validation failure (size,
// magic, version, key_id, hostname_len, payload_length mismatch, unknown
// event_type, event-type-specific payload shape, HMAC), returns nullopt
// and populates `whyDropped` at debug level. Timestamp and replay checks
// are NOT done here — they live in the caller against a live clock.
std::optional<Packet> decodePacket(const QByteArray& bytes,
                                   const QByteArray& secret,
                                   QString* whyDropped = nullptr);

// Fixed-capacity ring buffer of (sender_uuid, nonce) pairs for replay
// protection. Oldest-out eviction. 256 entries fits the expected 2–8 host
// working set with bursty headroom.
class PeerReplayBuffer {
 public:
  explicit PeerReplayBuffer(int capacity = 256);
  bool contains(const QByteArray& senderUuid, quint64 nonce) const;
  void insert(const QByteArray& senderUuid, quint64 nonce);
  int size() const { return m_size; }
  int capacity() const { return m_capacity; }

 private:
  struct Entry {
    QByteArray senderUuid;
    quint64 nonce = 0;
  };
  std::vector<Entry> m_buffer;
  int m_capacity;
  int m_nextIndex = 0;
  int m_size = 0;
};

}  // namespace peer
