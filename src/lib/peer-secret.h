// Sane Break is a gentle break reminder that helps you avoid mindlessly skipping breaks
// Copyright (C) 2026 Sane Break developers
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QByteArray>
#include <QString>

namespace peer {

// Load the 32-byte HMAC-SHA256 shared secret.
//
// Path resolution: `$SANE_BREAK_PEER_SECRET_FILE` env var if set,
// otherwise `~/.secrets.d/sane-break-peer`.
//
// Behavior:
// - The file may have a trailing newline; strip it before hex-decoding.
// - Require exactly 64 hex characters; decode to a 32-byte QByteArray.
// - If permissions are wider than 0600 (any group/other bits set), attempt
//   `chmod 0600`. If chmod fails, populate `outError` and return empty —
//   caller must run in single-machine mode.
// - If file missing, unreadable, zero-length, not 64 hex, or malformed,
//   populate `outError` and return empty.
//
// Returns an empty QByteArray on any failure; caller treats empty as
// "fusion disabled, single-machine mode".
QByteArray loadPeerSecret(QString* outError = nullptr);

// Resolve the secret file path for display (pref window, doctor scripts).
// Exposed separately so the UI can show the resolved path without loading.
QString resolvedPeerSecretPath();

}  // namespace peer
