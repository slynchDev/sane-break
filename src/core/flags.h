// Sane Break is a gentle break reminder that helps you avoid mindlessly skipping breaks
// Copyright (C) 2024-2026 Sane Break developers
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include <QFlags>

enum class PauseReason {
  Idle = 1 << 0,
  OnBattery = 1 << 1,
  AppOpen = 1 << 2,
  Sleep = 1 << 3,
  UnknownMonitor = 1 << 4,
  // Phase 14 — cross-peer meeting awareness. A peer on the LAN has
  // entered a meeting; suppress local break scheduling until the peer
  // exits or its MEETING state ages out of the unreachable window.
  // Cleared on peerMeetingChanged(false).
  PeerMeeting = 1 << 5,
};
Q_DECLARE_FLAGS(PauseReasons, PauseReason)
Q_DECLARE_OPERATORS_FOR_FLAGS(PauseReasons)

enum class BreakType { Small, Big };
Q_DECLARE_METATYPE(BreakType)
