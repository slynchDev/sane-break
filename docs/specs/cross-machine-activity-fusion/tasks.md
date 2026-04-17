# Cross-Machine Activity Fusion - Implementation Tasks

> Auto-generated from spec.md. Each task maps to a spec section.
> Mark tasks: `[ ]` pending, `[~]` in progress, `[x]` done, `[-]` skipped, `[!]` blocked.

---

## Phase 1: Foundations

- [x] **1.1** Add `Qt6::Network` to build
  - Add `find_package(Qt6 COMPONENTS Network REQUIRED)` and link `Qt6::Network` to the `sane-lib` target in `src/lib/CMakeLists.txt`. Required for `QUdpSocket`. `QMessageAuthenticationCode`, `QHostInfo`, and `QUuid` are in Qt Core — no additional linkage needed.
  - _Spec: Constraints_

- [x] **1.2** Declare peer preferences in `SanePreferences`
  - Add six `Setting<T>*` fields to `src/core/preferences.h` with exact INI keys and defaults: `peerFusionEnabled` (bool `peer/fusion-enabled`, default `false`), `peerListenPort` (int `peer/listen-port`, default `45454`, range 1024-65535), `peerActiveWindowSeconds` (int `peer/active-window-seconds`, default `15`, range 5-120), `peerUnreachableWindowSeconds` (int `peer/unreachable-window-seconds`, default `60`, range 15-600), `peerHeartbeatIntervalSeconds` (int `peer/heartbeat-interval-seconds`, default `5`, range 1-30), `peerBroadcastInterfaces` (QStringList `peer/broadcast-interfaces`, default empty). Instantiate in the `SanePreferences` constructor body in `src/core/preferences.cpp`. Range validation happens in the preferences UI layer, not here.
  - _Spec: Requirement 7.1_

- [x] **1.3** Define wire-format constants and `Packet` struct
  - Create `src/lib/peer-packet.h` with: `static constexpr char kMagic[4] = {'S', 'B', 'P', 'A'};` (raw bytes — do NOT use the `uint32_t kMagic = 'SBPA'` multi-character-literal form; its value is implementation-defined per the C++ standard and naive integer serialization would be host-endian-dependent, which would make little-endian and big-endian peers unable to talk to each other). Add `kVersion = 0x01`, `kKeyIdV1 = 0x00`, `kMaxPacketBytes = 512`, `kMaxHostnameBytes = 63`, `kMinPacketBytes` (header bytes), event-type enum values (`ACTIVITY = 0x01`, `IDLE_TRANSITION = 0x02`), and state enum values (`STATE_IDLE = 0x00`, `STATE_ACTIVE = 0x01`). Define a `Packet` struct with fields for `senderUuid`, `hostname`, `timestamp`, `nonce`, `eventType`, and a `QByteArray payload`. No encode/decode logic yet — that lives in task 2.2/2.3.
  - _Spec: Requirements 4.1-4.8_

- [x] **1.4** Add `breakStart()` / `breakEnd()` signals to `AppContext`
  - Add two Qt signals `void breakStart();` and `void breakEnd();` to `AppContext` in `src/core/app-states.h`. Emit `breakStart` at the top of `AppStateBreak::enter()` (right after `openCurrentSpan("break", ...)`) and `breakEnd` at the end of `AppStateBreak::exit()` (right after `closeCurrentSpan()`). These signals let `RemoteActivityMonitor` reset per-peer attribution counters atomically on break transitions, without coupling `AppStateBreak` to peer logic.
  - _Spec: Requirement 5.1_

- [x] **1.5** Make `SystemIdleTime::isIdle()` virtual
  - Change `bool isIdle()` in `src/core/idle-time.h:35` from a non-virtual inline getter to `virtual bool isIdle() { return m_isIdle; }`. No behavior change for existing callers. Required so `EffectiveIdleTime` (task 5.1) can override `isIdle()` to return the fused `localIdle && !anyPeerActive` value consumed synchronously by `AppStateBreak::enter()` and `BreakPhaseFullScreen::tick()`.
  - _Spec: Requirement 2.5_

---

## Phase 2: Crypto and serialization

- [x] **2.1** Secret file loader with permissions auto-repair
  - New file `src/lib/peer-secret.{h,cpp}` exposing `QByteArray loadPeerSecret(QString* outError)`. Resolve path from env `SANE_BREAK_PEER_SECRET_FILE`, default `~/.secrets/sane-break-peer`. Read file, strip optional trailing newline, require exactly 64 hex characters, hex-decode to a 32-byte `QByteArray`. On permissions wider than `0600`: attempt `QFile::setPermissions(ReadOwner|WriteOwner)`; if that fails, populate `outError` and return empty. On missing/unreadable/malformed: populate `outError` with a single user-visible message naming the resolved path and return empty. Caller treats empty result as "fusion disabled, single-machine mode".
  - _Spec: Requirements 3.1, 3.5, 3.6_

- [x] **2.2** Packet encoder
  - Add `QByteArray encodePacket(const Packet& p, const QByteArray& secret)` in `src/lib/peer-packet.cpp`. Serialize fields in order: magic(4) | version(1) | key_id(1) | sender_uuid(16) | hostname_len(1) | hostname(0..63 UTF-8) | timestamp(8 BE) | nonce(8) | event_type(1) | payload_length(2 BE) | payload(variable). Then append `QMessageAuthenticationCode::hash(body, secret, QCryptographicHash::Sha256)` (32 bytes). All multi-byte integers big-endian. Caller stamps `key_id = kKeyIdV1 = 0x00`.
  - _Depends: 1.3, 2.1_
  - _Spec: Requirements 3.1, 4.1-4.8, 4.13_

- [x] **2.3** Packet decoder with validation
  - Add `std::optional<Packet> decodePacket(const QByteArray& bytes, const QByteArray& secret, QString* whyDropped)` in `src/lib/peer-packet.cpp`. Validate in order and drop silently (populate `whyDropped` at debug level only) on: total size out of `[kMinPacketBytes, kMaxPacketBytes]`, magic mismatch, version != 0x01, key_id != 0x00, hostname_len > 63, total size implied by payload_length disagrees with buffer length, unknown event_type. Additionally validate event-type-specific payload shape per Req 4.7/4.8: if `event_type == ACTIVITY` require `payload_length == 4`; if `event_type == IDLE_TRANSITION` require `payload_length == 1` AND the payload byte ∈ {0x00, 0x01}. Verify HMAC with `QMessageAuthenticationCode` + constant-time byte compare. Timestamp-window and replay checks are NOT decoder responsibilities — they live in the caller (task 3.3) against a live clock. Return the decoded `Packet` only if all checks pass.
  - _Depends: 1.3, 2.1_
  - _Spec: Requirements 3.2, 4.1, 4.4, 4.7-4.13_

- [x] **2.4** Replay ring buffer
  - Add `PeerReplayBuffer` (fixed-capacity 256 entries of `{QByteArray sender_uuid, uint64_t nonce}`) in `src/lib/peer-packet.{h,cpp}`. Methods: `bool contains(const QByteArray& uuid, uint64_t nonce) const`, `void insert(...)`. Oldest-out eviction. Caller also enforces the timestamp window check (`|now - ts| > 30s` → drop; `now + 5s < ts` → drop) before consulting the buffer.
  - _Spec: Requirements 3.3, 3.4_

- [x] **2.5** Unit tests for packet crypto and replay buffer
  - Add `test/test-peer-packet.cpp` and register in `test/CMakeLists.txt`. Test cases: encode then decode round-trips cleanly; decode fails for magic mismatch, wrong version byte, `key_id=0x01`, `hostname_len=100`, truncated buffer, `payload_length` overflowing buffer, unknown event_type, ACTIVITY with `payload_length=3` or `=5`, IDLE_TRANSITION with `payload_length=0` or `=2` or state byte `0x02`/`0xff`, tampered HMAC. Replay buffer: first insert wins, duplicate rejected, 257th insert evicts the oldest. Timestamp-window tests (too old, too new) belong in task 3.6, not here — the decoder does not own timestamp validation. Property 2 / Property 9 / Property 10 anchor these tests.
  - _Depends: 2.2, 2.3, 2.4_
  - _Spec: Requirements 3.2, 3.4, 4.7, 4.8, 4.9-4.12; Properties 2, 10_

---

## Phase 3: RemoteActivityMonitor core (peer state, sockets)

- [x] **3.1** `RemoteActivityMonitor` class skeleton
  - New `src/lib/remote-activity-monitor.{h,cpp}`. Subclass `QObject`. Constructor signature `RemoteActivityMonitor(SanePreferences* prefs, SystemIdleTime* localIdle, QObject* parent = nullptr)` — `localIdle` MUST be the raw local `SystemIdleTime`, NOT an `EffectiveIdleTime` facade; wiring the facade here creates a peer-feedback loop (peer active → facade reports active → our heartbeat re-broadcasts → peer sees us active forever). Owns a `QByteArray m_secret`, `QByteArray m_senderUuid` (fresh `QUuid::createUuid().toRfc4122()` per process), `PeerReplayBuffer m_replay`, and a map `QHash<QByteArray, PeerState> m_peers` where key is sender_uuid. Define `enum class PeerLastState { Active, Idle }` and `struct PeerState { QString hostname; QDateTime lastSeenActive; QDateTime lastSeenIdle; PeerLastState lastState; int activeSecondsSinceLastBreak; }`. `lastState` is the normalized state (Active/Idle) — distinct from the raw `event_type` byte — so the three packet shapes (ACTIVITY, IDLE_TRANSITION{active}, IDLE_TRANSITION{idle}) all collapse cleanly into this enum at update time (task 3.3). Expose signals: `void peerActivityChanged(bool anyPeerActive);`. Provide `void start()` / `void stop()` lifecycle methods. `stop()` SHALL clear `m_peers`, cancel the offline-cleanup timer (task 3.5) and the heartbeat timer (task 4.1), close any open sockets (task 3.2), and — if the cached aggregate was true — emit `peerActivityChanged(false)` so `EffectiveIdleTime` drops back to pass-through semantics.
  - _Depends: 1.2, 1.3, 2.1, 2.4_
  - _Spec: Requirements 2.1-2.2, 3.4, 7.3_

- [x] **3.2** UDP socket setup with interface allowlist
  - Store per-interface tuples `struct InterfaceSocket { QUdpSocket* socket; QHostAddress localAddr; QHostAddress subnetBroadcast; int interfaceIndex; }` in a `QList<InterfaceSocket>`. Also maintain `QSet<int> m_allowedInterfaceIndexes` for ingress filtering (task 3.3). In `RemoteActivityMonitor::start()`, for each interface name in `preferences->peerBroadcastInterfaces->get()`:
    1. `QNetworkInterface iface = QNetworkInterface::interfaceFromName(name);` — if invalid or down, log a user-visible warning and skip this entry (partial failure is OK; other interfaces still work).
    2. Pick the first IPv4 `QNetworkAddressEntry` from `iface.addressEntries()`; skip if none. Store `entry.ip()` as `localAddr`, `entry.broadcast()` as `subnetBroadcast` (e.g., `192.168.1.255` for a `192.168.1.0/24` subnet), and `iface.index()` as `interfaceIndex`; insert the index into `m_allowedInterfaceIndexes`. `QHostAddress::Broadcast` (`255.255.255.255`) is NOT used on egress — Linux routes it via the default route regardless of socket binding, which would leak packets onto whichever interface matches the default route, defeating the allowlist.
    3. Create a `QUdpSocket` and `bind(QHostAddress::AnyIPv4, peerListenPort, QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)`. Binding to `Any` is required on Linux to receive packets addressed to `255.255.255.255` or the subnet-directed broadcast; binding to `localAddr` would silently drop those packets on most kernels. Note: `AnyIPv4` means the socket receives broadcasts arriving on every NIC; the allowlist is enforced on ingress inside task 3.3 via `QNetworkDatagram::interfaceIndex()` matched against `m_allowedInterfaceIndexes`.
    4. Connect `readyRead` to `onDatagramReady()` (task 3.3).
  - If the allowlist is empty, `start()` is a no-op. If every interface fails to bind, log the cumulative failure and leave the list empty (graceful degradation per Requirement 11.4).
  - _Depends: 3.1_
  - _Spec: Requirements 1.5-1.6, 2.1, 11.4_

- [x] **3.3** Datagram dispatch: receive → interface filter → decode → verify → update peer
  - Implement `onDatagramReady()` using `QObject::sender()` to identify which socket fired, then loop `while (sock->hasPendingDatagrams())` calling `QNetworkDatagram dg = sock->receiveDatagram()` (not `readDatagram`, which discards the arrival-interface metadata). Per Req 2.1 ingress filtering: if `dg.interfaceIndex()` is not in `m_allowedInterfaceIndexes` (task 3.2), drop silently — a packet arriving on a non-allowlisted NIC (corporate VPN, guest Wi-Fi, tenant bridge) must not mutate peer state even if its HMAC is valid. Then `decodePacket(dg.data(), m_secret)`; drop silently on failure. If `packet.senderUuid == m_senderUuid`, drop (self-loopback per Req 1.7). Timestamp-window check (`|now - ts| > 30s` OR `ts > now + 5s`) + replay-buffer check; drop on either failure. Insert into replay buffer. Look up or create `PeerState` for `packet.senderUuid`; update `hostname` (first-seen) and normalize the event into `PeerLastState`:
    - `event_type == ACTIVITY` → `lastSeenActive = now; lastState = Active`
    - `event_type == IDLE_TRANSITION` AND `payload state byte == 0x01` → `lastSeenActive = now; lastState = Active`
    - `event_type == IDLE_TRANSITION` AND `payload state byte == 0x00` → `lastSeenIdle = now; lastState = Idle`
  - Call `recomputeAnyPeerActive()` to emit `peerActivityChanged` if the aggregate transitioned.
  - _Depends: 2.3, 2.4, 3.1, 3.2_
  - _Spec: Requirements 1.7, 2.1-2.4, 3.2-3.4_

- [x] **3.4** `anyPeerActive` aggregation logic
  - Private `bool computeAnyPeerActive() const`: iterate `m_peers`, return true iff at least one peer has `lastSeenActive` within `peerActiveWindowSeconds` AND `lastState == PeerLastState::Active`. `recomputeAnyPeerActive()` computes the new value, compares to cached previous, and emits `peerActivityChanged(newValue)` on transition only.
  - _Depends: 3.1_
  - _Spec: Requirements 2.3, 2.4_

- [x] **3.5** Peer offline cleanup + periodic aggregate recompute
  - QTimer with 1-second interval started in `start()` (the same 1 s cadence as the attribution tick in 6.1 — implementations MAY share one timer). On every tick: (a) iterate `m_peers`, remove entries whose `max(lastSeenActive, lastSeenIdle)` is older than `peerUnreachableWindowSeconds`; (b) unconditionally call `recomputeAnyPeerActive()`. The unconditional recompute bounds staleness of the `peerActiveWindowSeconds` → idle transition to 1 second; without it, a peer that simply stops broadcasting would keep the aggregate stuck at `true` until it crosses the `peerUnreachableWindowSeconds` threshold (up to 45 s stale with default settings, blocking local `idleStart` emission).
  - _Depends: 3.1, 3.4_
  - _Spec: Requirements 2.3, 2.4, 2.8, 11.2_

- [x] **3.6** Unit tests for peer state dispatch
  - Add tests in `test/test-remote-activity-monitor.cpp`: valid ACTIVITY / IDLE_TRANSITION{active} / IDLE_TRANSITION{idle} each update peer state and `lastState` correctly; ACTIVITY → IDLE_TRANSITION{active} → IDLE_TRANSITION{idle} sequence drives the aggregate through Active→Active→Idle transitions at the right edges; self-packet drops; duplicate nonce drops; timestamp too old (`ts = now - 31s`) drops; timestamp too new (`ts = now + 6s`) drops; ingress interface filter drops packets whose `QNetworkDatagram::interfaceIndex()` is not in `m_allowedInterfaceIndexes`; `peerActivityChanged` fires only on true→false or false→true transitions; silent peer's aggregate flips within 1 s of crossing `peerActiveWindowSeconds`; peer offline timeout removes and flips aggregate. Property 1 (active fusion monotonicity) and Property 4 (self-traffic immunity) anchor specific tests here.
  - _Depends: 3.3, 3.4, 3.5_
  - _Spec: Requirements 1.7, 2.1-2.4, 2.8, 3.3; Properties 1, 4_

---

## Phase 4: RemoteActivityMonitor outbound

- [x] **4.1** Heartbeat timer + ACTIVITY emission
  - In `RemoteActivityMonitor::start()`, start a `QTimer` with interval `peerHeartbeatIntervalSeconds * 1000`. On timeout, if the local `SystemIdleTime` reports `!isIdle()`, build an ACTIVITY packet (payload = `event_count` accumulated since last emit; implementation may simply set to `1` if no tick counter exists), encode via `encodePacket`, and for each `InterfaceSocket` tuple from task 3.2 call `socket->writeDatagram(bytes, subnetBroadcast, peerListenPort)`. Using the per-interface subnet-directed broadcast (e.g., `192.168.1.255`) ensures the kernel routes the packet via that specific interface; `QHostAddress::Broadcast` (`255.255.255.255`) would route via the default route regardless of socket binding and defeat the interface allowlist. Reset the counter after emit. No emission when `peerFusionEnabled` is false OR `peerBroadcastInterfaces` empty.
  - _Depends: 3.2, 2.2_
  - _Spec: Requirements 1.1, 1.5, 1.6_

- [x] **4.2** IDLE_TRANSITION emission on local idle edges
  - Connect the raw local `SystemIdleTime` (injected via 3.1's ctor) `idleStart` → send IDLE_TRANSITION{state=idle}; connect `idleEnd` → send IDLE_TRANSITION{state=active}. Reuses the same encode + per-interface subnet-broadcast path as 4.1. Must be the raw timer, not the `EffectiveIdleTime` facade (per 3.1 feedback-loop note).
  - _Depends: 4.1_
  - _Spec: Requirements 1.3, 1.4_

- [x] **4.3** Send-failure backoff logging
  - When `writeDatagram` returns -1 for an interface, log at debug level at most once per interface per 60 seconds. Do not disable the socket; keep retrying on subsequent ticks.
  - _Depends: 4.1_
  - _Spec: Requirement 11.5_

- [x] **4.4** Unit tests for outbound emission
  - Tests: heartbeat fires at T-second cadence while non-idle; no emission when idle locally; idleStart/idleEnd edges emit exactly one IDLE_TRANSITION each with the right state byte; self-packets round-trip are ignored by the local receiver (regression for Property 4). Verify packet field values via `decodePacket`.
  - _Depends: 4.1, 4.2_
  - _Spec: Requirements 1.1, 1.3, 1.4, 1.7; Property 4_

---

## Phase 5: EffectiveIdleTime facade

- [x] **5.1** `EffectiveIdleTime` class declaration
  - New `src/lib/effective-idle-time.{h,cpp}`. Subclass `SystemIdleTime`. Constructor takes `SystemIdleTime* wrapped` (the real OS idle watcher) and `RemoteActivityMonitor* peers`. Forward `setWatchAccuracy`, `setMinIdleTime`, `startWatching`, `stopWatching` to `wrapped`. Override `isIdle()` to return the fused value `wrapped->isIdle() && !m_anyPeerActive` (requires task 1.5 making the base `isIdle()` virtual). This keeps synchronous callers of `app->idleTimer->isIdle()` in `AppStateBreak::enter()` (`app-states.cpp:209`) and `BreakPhaseFullScreen::tick()` (`app-states.cpp:311`) consistent with the fused signal edges. Track `bool m_anyPeerActive` updated from `peers->peerActivityChanged`.
  - _Depends: 1.5, 3.4_
  - _Spec: Requirements 2.5, 2.6_

- [x] **5.2** Fused `idleStart` / `idleEnd` emission
  - Cache last emitted state (`bool m_effectiveIdle = false`). On any of three triggers (wrapped idleStart, wrapped idleEnd, peerActivityChanged), recompute `effective = wrapped->isIdle() && !m_anyPeerActive`. If `effective != m_effectiveIdle`: update cache; emit `idleStart` or `idleEnd` accordingly. Never emit a duplicate edge.
  - _Depends: 5.1_
  - _Spec: Requirement 2.5_

- [x] **5.3** Unit tests for EffectiveIdleTime
  - Dummy `SystemIdleTime` + stub `RemoteActivityMonitor`. Cases: local idle + no peers → idleStart fires; local idle + peer active → no idleStart; local idle + peer active transitions to peer idle → idleStart fires; local active → idleEnd regardless of peers; duplicate edges suppressed. Property 1 (active fusion monotonicity) anchors the tests.
  - _Depends: 5.2_
  - _Spec: Requirements 2.4-2.6; Property 1_

---

## Phase 6: Per-peer attribution counters

- [ ] **6.1** `activeSecondsSinceLastBreak` counters
  - In `RemoteActivityMonitor`, add `int m_localActiveSecondsSinceLastBreak = 0` and per-peer `activeSecondsSinceLastBreak` in `PeerState`. Start a 1-second tick timer in `start()`. On each tick: if local is non-idle, increment the local counter by 1; for each peer considered active (same rule as `computeAnyPeerActive`), increment that peer's counter by 1.
  - _Depends: 3.1, 3.4_
  - _Spec: Requirement 5.2_

- [ ] **6.2** Reset counters on `AppContext::breakStart`
  - `AppDependencies` wiring (Phase 9) connects `AppContext::breakStart` to `RemoteActivityMonitor::resetAttribution()`. `resetAttribution()` zeros the local counter and every peer's counter atomically on the GUI thread.
  - _Depends: 1.4, 6.1_
  - _Spec: Requirements 5.1, 5.2_

- [ ] **6.3** `activityBreakdown()` method
  - Public method returning `struct ActivityBreakdown { QList<HostActivity> hosts; int totalActiveSeconds; }` where `HostActivity { QString label; int activeSeconds; int sharePercent; }`. Label is peer hostname (fallback: first 8 hex chars of sender_uuid) for peers and `QHostInfo::localHostName()` for local. Callable from the GUI thread; reads in-memory counters only, no I/O.
  - _Depends: 6.1_
  - _Spec: Requirements 5.2, 5.6_

- [ ] **6.4** Unit tests for attribution
  - Tests: counters tick correctly for local + per-peer active seconds; breakStart resets all to 0; `activityBreakdown()` totals match per-host sum within ±1 s (Property 5); unknown-hostname peer gets a uuid-prefix label.
  - _Depends: 6.2, 6.3_
  - _Spec: Requirements 5.1-5.3, 5.6; Property 5_

- [ ] **6.5** Peer status accessor for live indicator
  - Add `QList<PeerStatus> peerStatuses() const` to `RemoteActivityMonitor` returning `struct PeerStatus { QString hostLabel; QDateTime lastSeenActive; QDateTime lastSeenIdle; bool isActive; }` per peer (plus optionally a local entry). Used by the preferences window's live indicator (task 10.3). Distinct from `activityBreakdown()`, which stays attribution-focused (label / activeSeconds / sharePercent). Reads only in-memory peer state; no I/O.
  - _Depends: 3.1, 3.3_
  - _Spec: Requirement 7.2_

---

## Phase 7: Tray peer-breakdown tooltip

- [ ] **7.1** Peer-breakdown line composer
  - Add helper in `src/app/tray.cpp` (or a new `src/app/peer-tooltip.{h,cpp}`) that formats `activityBreakdown()` output into a human string like `r16 68% · hp 32% · 47m total`. When fewer than 2 peers are present or `peerFusionEnabled == false` or no peers seen, return empty string. When minutes < 1, format as `XXs total`.
  - _Depends: 6.3_
  - _Spec: Requirements 5.3-5.5_

- [ ] **7.2** 2-second throttle scoped to peer line
  - Cache the last-rendered peer line + a `QElapsedTimer`. In `StatusTrayWindow::update()`, rebuild the peer line only if the cached value is older than 2 seconds; otherwise reuse. Countdown portion continues to refresh at the existing 1 Hz cadence unaffected.
  - _Depends: 7.1_
  - _Spec: Requirement 5.4; Constraints_

- [ ] **7.3** Wire RemoteActivityMonitor into tray construction
  - `StatusTrayWindow` gains a `RemoteActivityMonitor*` (optional, nullable). `SaneBreakApp::create()` passes it. When null, the peer line is always empty; when non-null, it composes per 7.1/7.2.
  - _Depends: 7.1, 7.2_
  - _Spec: Requirements 5.3, 5.5_

- [ ] **7.4** Manual tooltip verification
  - Manual check: run two instances on the same LAN with `peerFusionEnabled=true` and a common interface allowlisted; verify the tooltip on each shows the other peer's hostname and share percent updating at most every 2 s; tooltip countdown updates every 1 s.
  - _Depends: 7.3, 9.1_
  - _Spec: Requirement 5_

---

## Phase 8: DB schema migration + host attribution (parallel to 2-7)

- [ ] **8.1** Migration runner keyed on `PRAGMA user_version`
  - Add `QSqlError BreakDatabase::migrate()` in `src/core/db.cpp`. Query `PRAGMA user_version`. If `< 2`: `BEGIN`, `ALTER TABLE spans ADD COLUMN host TEXT`, `UPDATE spans SET host = ? WHERE host IS NULL` bound with `QHostInfo::localHostName()`, `PRAGMA user_version = 2`, `COMMIT`. On failure: rollback, return error. Do NOT use SQL `DEFAULT <expression>` — SQLite accepts only constant literals there.
  - _Spec: Requirement 6.1; Constraints_

- [ ] **8.2** Fresh-install schema stamps `user_version = 2`
  - In the `ensureDb()` path where the `spans`/`events` tables are created for the first time, include `host TEXT` in the CREATE TABLE, and after successful creation run `PRAGMA user_version = 2`. Ensures Property 6 (migration idempotence) holds for new installs.
  - _Spec: Requirement 6.2; Property 6_

- [ ] **8.3** Stamp `host` on every opened span
  - Modify `BreakDatabase::openSpan(...)` to insert `QHostInfo::localHostName()` into the `host` column. No behavior change for existing callers.
  - _Depends: 8.1, 8.2_
  - _Spec: Requirement 6.3_

- [ ] **8.4** Graceful degradation on migration failure
  - If `migrate()` returns an error: set an internal `m_readOnlyMode` flag, surface one user-visible error dialog on startup, and make `openSpan` / `closeSpan` / `logEvent` no-op (debug-log-only) for the session. Break scheduler and UI continue. Next startup retries migration.
  - _Depends: 8.1, 8.3_
  - _Spec: Requirements 6.6, 6.7_

- [ ] **8.5** Per-host aggregation query for stats
  - Add `QList<HostUsageStats> queryDailyUsageByHost(QDate from, QDate to)` returning per-day per-host active seconds. Reuse the existing span-aggregation logic with `GROUP BY date, host`.
  - _Depends: 8.1_
  - _Spec: Requirement 6.5_

- [ ] **8.6** Stats window per-host breakdown row
  - Render the per-host aggregation beneath the existing daily total in `src/app/stats-window.cpp`. Display as `{hostname}: {HH:MM:SS}` per host per day.
  - _Depends: 8.5_
  - _Spec: Requirement 6.5_

- [ ] **8.7** DB migration unit tests
  - Add to `test/test-db.cpp`: fresh DB starts at `user_version = 2` and has `host` column; DB at `user_version = 0` migrates to 2 and backfills `host` with local hostname; migration is idempotent (second call is no-op); read-only DB triggers `m_readOnlyMode` and skips span writes without crashing.
  - _Depends: 8.2, 8.3, 8.4_
  - _Spec: Requirements 6.1, 6.2, 6.6; Properties 6, 11_

---

## Phase 9: AppDependencies wiring + runtime toggle

- [x] **9.1** Construct `RemoteActivityMonitor` in `SaneBreakApp::create()`
  - In `src/app/app.cpp` (`SaneBreakApp::create()`), construct the raw idle timer via `createIdleTimer(parent)` and store it in a local `SystemIdleTime* rawIdleTimer`. Then `RemoteActivityMonitor* ram = new RemoteActivityMonitor(preferences, rawIdleTimer, parent)` — passing the raw timer (not the facade built in 9.2) to prevent the peer-feedback loop documented in task 3.1. Add a `RemoteActivityMonitor*` field to `AppDependencies` in `src/core/app.h`. Populate it in the deps struct. Call `ram->start()` after deps are constructed if `preferences->peerFusionEnabled->get()` is true.
  - Implementation note: the `ram*` field in `AppDependencies` is forward-declared (`namespace peer { class RemoteActivityMonitor; }`) so that `sane-core` does not acquire a link-time dependency on `sane-lib`. All peer-specific wiring lives in `SaneBreakApp` (which is part of `sane-gui` and already links both libraries).
  - _Depends: Phase 3 complete, Phase 4 complete, 1.2_
  - _Spec: Requirement 7.7; Constraints_

- [x] **9.2** Wire `EffectiveIdleTime` into `AppDependencies::idleTimer`
  - `EffectiveIdleTime` is always constructed (wrapping `rawIdleTimer` and `ram`) and always assigned to `AppDependencies::idleTimer`, regardless of `peerFusionEnabled`. When fusion is disabled (ram stopped), the facade degrades to pass-through because `m_anyPeerActive` stays `false`. This keeps the dependency graph fixed and satisfies 9.3's "no second AppDependencies code path" requirement. `rawIdleTimer` is referenced by BOTH `ram` (as its local idle source) and `EffectiveIdleTime` (as its wrapped timer) — single raw instance, two consumers.
  - _Depends: 9.1, 5.2_
  - _Spec: Requirements 2.7, 7.3_

- [x] **9.3** Runtime toggle of `peerFusionEnabled`
  - `SaneBreakApp` connects `preferences->peerFusionEnabled->changed` to `onPeerFusionToggled()`, which calls `ram->start()` or `ram->stop()` based on the new value. `stop()`'s contract (clear `m_peers`, emit `peerActivityChanged(false)`) causes `EffectiveIdleTime` to fall through to pass-through within the same event-loop turn — no restart required, no second `AppDependencies` code path.
  - _Depends: 9.1, 9.2, 3.1_
  - _Spec: Requirement 7.7_

- [x] **9.4** Runtime rebind on port / interface change
  - `SaneBreakApp` connects both `peerListenPort` and `peerBroadcastInterfaces` `changed` signals to `onPeerBindingChanged()`, which calls `ram->stop()` then `ram->start()`. If `ram->isRunning()` is false afterward, the handler reverts both preferences to their last known-good snapshot (captured after each successful start) and re-calls `start()`. A guard flag prevents recursion when the revert fires `changed` again. Tray-level warning surfacing is deferred to Phase 10 (preferences UI / live indicator); a `qWarning` is logged in the interim.
  - _Depends: 9.1, 9.3_
  - _Spec: Requirement 7.8_

- [!] **9.5** Wire break-transition signals
  - Connect `AppContext::breakStart` → `ram->resetAttribution()` (from task 6.2). Verify `breakStart` fires on every break entry path (normal tick expiry, EndMeetingBreakNow, BigBreakNow, etc.) by inspecting `AppStateBreak::enter()` — since the signal is emitted there, coverage is automatic.
  - **Blocked**: depends on Phase 6 task 6.2 (`RemoteActivityMonitor::resetAttribution()` method definition). Once Phase 6 lands, the wiring is a single `connect(this, &AppContext::breakStart, m_ram, &peer::RemoteActivityMonitor::resetAttribution)` line in the `if (m_ram)` block of `SaneBreakApp::SaneBreakApp`.
  - _Depends: 9.1, 6.2, 1.4_
  - _Spec: Requirements 5.1, 5.2_

- [x] **9.6** Integration tests for wiring
  - `test/test-app.cpp` gains `peer_fusion_disabled_by_default` (pins the Property 7 default) and `peer_fusion_runtime_toggle_null_ram_safe` (verifies the handler short-circuits when no RAM is wired, matching `DummyApp`). `test/test-effective-idle-time.cpp` gains `ram_stop_after_peer_active_restores_local_idle` and `ram_stop_with_no_peers_is_no_op_for_facade` — these cover the mid-session toggle path (ram stop → facade pass-through within one event-loop turn) that the SaneBreakApp handler drives. Full-stack SaneBreakApp toggle coverage is deferred to Phase 13's property-test suite.
  - _Depends: 9.3, 9.5_
  - _Spec: Requirements 7.7, 11.1, 11.3; Property 7_

---

## Phase 10: Preferences UI (parallel to 3-9 after 1.2)

- [ ] **10.1** Add "Peer fusion" section to `src/app/pref-window.ui`
  - New `QGroupBox` labeled "Peer fusion". Contains (in order): `QCheckBox` for `peerFusionEnabled`, `QSpinBox` for `peerListenPort` (1024-65535), three `QSpinBox` for the three window/heartbeat settings (with ranges from Req 7.1), a `QPlainTextEdit` for the `peerBroadcastInterfaces` allowlist (one per line; empty means disabled — displayed as a visible "Peer fusion is disabled — add at least one interface below" hint), a read-only `QLabel` displaying the resolved secret file path, and a `QLabel` with the metadata-leak note ("Packets contain this machine's hostname and activity timing and are broadcast to the selected interfaces").
  - _Depends: 1.2_
  - _Spec: Requirements 7.2, 7.5_

- [ ] **10.2** Wire preference controllers for Peer fusion section
  - In `src/app/pref-window.cpp`, register controllers for each of the six settings using the existing `PrefController` pattern. The resolved secret path label reads `$SANE_BREAK_PEER_SECRET_FILE` (default `~/.secrets/sane-break-peer`).
  - _Depends: 10.1_
  - _Spec: Requirement 7.2_

- [ ] **10.3** Live peer indicator in preferences window
  - Add a `QListWidget` under the Peer fusion settings. Populated on a 2 s `QTimer` from `RemoteActivityMonitor::peerStatuses()` (task 6.5). Each row shows `{hostLabel} — active 3 s ago` or `{hostLabel} — idle 12 s ago`, computed from `lastSeenActive` / `lastSeenIdle` relative to `QDateTime::currentDateTimeUtc()`. Hidden when `peerFusionEnabled == false`.
  - _Depends: 10.2, 6.5_
  - _Spec: Requirement 7.2_

---

## Phase 11: workstation-work provisioning (parallel — different repo)

- [ ] **11.1** `install.sh` generates the peer secret if absent
  - In `~/git/workstation-work/install.sh`, before the sane-break build block: `mkdir -p ~/.secrets && chmod 0700 ~/.secrets`. If `~/.secrets/sane-break-peer` does not exist, generate inside a subshell-scoped umask so the file never exists at a wider-than-0600 mode (do NOT use `> file && chmod 0600 file`, which leaves the file world-readable for a brief race window): `(umask 177 && openssl rand -hex 32 > ~/.secrets/sane-break-peer)`. Print a numbered instruction block ending with the race-free remote-copy command: `ssh user@otherhost 'install -m 600 /dev/null ~/.secrets/sane-break-peer' && scp -p ~/.secrets/sane-break-peer user@otherhost:~/.secrets/sane-break-peer` (pre-creates the remote file at 0600, then `scp -p` preserves mode on transfer).
  - _Spec: Requirement 8.1_

- [ ] **11.2** `install.sh` repairs wider-than-0600 permissions
  - If `~/.secrets/sane-break-peer` exists with mode wider than `0600`: `chmod 0600` and echo a warning.
  - _Depends: 11.1_
  - _Spec: Requirement 8.2_

- [ ] **11.3** `scripts/doctor.sh` asserts the secret file
  - After the existing `compare_file` / `check_command` blocks in `~/git/workstation-work/scripts/doctor.sh`, add assertions: file exists; is a regular file owned by `$USER`; mode is `0600`; content is exactly 64 hex characters (optional trailing newline). On any failure, print `Run scripts/ws deploy && install.sh to (re)generate the peer secret.`.
  - _Spec: Requirement 8.3_

- [ ] **11.4** `configs/sane-break/SaneBreak.ini` adds `[peer]` section
  - Append to `~/git/workstation-work/configs/sane-break/SaneBreak.ini`:
    ```
    [peer]
    fusion-enabled=true
    broadcast-interfaces=<iface>
    ```
    where `<iface>` is the host's chosen broadcast interface (r16's LAN NIC). Deployed to `~/.config/SaneBreak/SaneBreak.ini` automatically via the existing `scripts/ws deploy` flow.
  - _Spec: Requirement 8.5_

- [ ] **11.5** Verify `ws deploy` carries the new keys end-to-end
  - Manual: on r16, `scripts/ws deploy --pull`, confirm `~/.config/SaneBreak/SaneBreak.ini` contains the `[peer]` section with the expected keys. Run `scripts/ws doctor` — all assertions pass.
  - _Depends: 11.1, 11.3, 11.4_
  - _Spec: Requirement 10.1, 10.3_

- [ ] **11.6** Verify `ws deploy` does NOT touch the secret file
  - Manual negative test guarding Reqs 10.2 and 10.4. Capture baseline: `BEFORE=$(stat --format='%Y %i %s' ~/.secrets/sane-break-peer) && BEFORE_SHA=$(sha256sum ~/.secrets/sane-break-peer | awk '{print $1}')`. Run `scripts/ws deploy --pull` (including any install.sh subcommand path). Re-capture: `AFTER=$(stat --format='%Y %i %s' ~/.secrets/sane-break-peer) && AFTER_SHA=$(sha256sum ~/.secrets/sane-break-peer | awk '{print $1}')`. Assert `BEFORE == AFTER` AND `BEFORE_SHA == AFTER_SHA` — mtime, inode, size, and content all unchanged. Any difference means a deploy code path accidentally touched the secret; investigate and fix before shipping.
  - _Depends: 11.1, 11.5_
  - _Spec: Requirements 10.2, 10.4_

---

## Phase 12: workstation-personal provisioning (parallel — different repo)

- [ ] **12.1** Switch `install.sh` clone to `slynchDev/sane-break`
  - In `~/git/workstation-personal/install.sh`, change the `git clone https://github.com/AllanChain/sane-break.git` line to `git clone -b meeting-aware https://github.com/slynchDev/sane-break.git`. Mirror any branch-pinning logic used by the work repo.
  - _Spec: Requirement 9.1_

- [ ] **12.2** Unify checkout path with workstation-work
  - Pick one path (`~/git/sane-break` to match work, or `~/src/sane-break` — the work repo uses `~/git/sane-break`, so standardize on that). Update the clone target, the "Building sane-break" `cd` line, the rebuild-skip `if [ -d ~/... ]` check, and any `scripts/doctor.sh` references in `~/git/workstation-personal/` consistently in this single task.
  - _Depends: 12.1_
  - _Spec: Requirement 9.2_

- [ ] **12.3** Mirror 11.1 secret generation in `workstation-personal/install.sh`
  - Same subshell-scoped `(umask 177 && openssl rand -hex 32 > ~/.secrets/sane-break-peer)` generation plus the `ssh ... install -m 600 ... && scp -p` copy instruction as task 11.1. Do not use the naive `> file && chmod 0600 file` pattern — it creates a race window where the secret is briefly world-readable.
  - _Spec: Requirement 9.3_

- [ ] **12.4** Mirror 11.2 permissions repair
  - Same `chmod 0600` + warning behavior.
  - _Depends: 12.3_
  - _Spec: Requirement 9.4_

- [ ] **12.5** Mirror 11.3 `scripts/doctor.sh` assertion
  - Same four-part assertion.
  - _Spec: Requirement 9.5_

- [ ] **12.6** `configs/sane-break/SaneBreak.ini` `[peer]` section
  - Same `fusion-enabled=true` + host-specific `broadcast-interfaces` value for hp/x220 profiles.
  - _Spec: Requirement 9.6_

- [ ] **12.7** Verify `ws deploy` does NOT touch the secret file (workstation-personal)
  - Mirror of task 11.6 for the personal repo: capture `stat`+`sha256sum` of `~/.secrets/sane-break-peer` before and after `scripts/ws deploy --pull` on hp or x220; assert byte-identical and mtime-identical.
  - _Depends: 12.3, 12.5_
  - _Spec: Requirements 10.2, 10.4_

---

## Phase 13: Property-test integration suite

- [ ] **13.1** Active fusion monotonicity (Property 1)
  - Two-instance integration test: instance A emits ACTIVITY packets; instance B's local `SystemIdleTime` is idle. Assert B's `EffectiveIdleTime` never emits `idleStart` while A is active.
  - _Depends: 9.6, 5.3_
  - _Spec: Property 1; Requirements 2.3-2.5_

- [ ] **13.2** Spoof rejection (Property 2)
  - Test: craft packets with invalid HMAC, invalid `key_id`, wrong magic, replayed nonce. Assert receiver state is byte-identical to the "packet never arrived" baseline — no peer counter change, no `peerActivityChanged` emission, no DB write.
  - _Depends: 2.5, 3.6_
  - _Spec: Property 2; Requirements 3.2-3.4, 4.4, 4.13_

- [ ] **13.3** Secret-file safety (Property 3)
  - Test: run the app with the secret file variously missing, empty, malformed hex, 63 chars, 65 chars, mode `0644` and owned by root (simulated via mock `QFile::permissions`). Assert `peerFusionEnabled` is effectively disabled and break scheduler behavior matches baseline.
  - _Depends: 2.1, 9.6_
  - _Spec: Property 3; Requirements 3.5, 3.6_

- [ ] **13.4** Self-traffic immunity (Property 4)
  - Test: instance emits a packet; the same instance's receive socket observes it (simulated by injecting via `writeDatagram(loopback)`). Assert counters, `peerActivityChanged`, and tooltip remain unchanged.
  - _Depends: 3.6, 4.4_
  - _Spec: Property 4; Requirement 1.7_

- [ ] **13.5** Attribution fidelity (Property 5)
  - Test: advance time with a mix of local activity and simulated peer ACTIVITY packets; call `activityBreakdown()` just before a simulated `breakStart`; assert sum of per-host `activeSeconds` equals `totalActiveSeconds` within ±1 s and no host exceeds the interval length.
  - _Depends: 6.4_
  - _Spec: Property 5; Requirements 5.1-5.3_

- [ ] **13.6** Single-machine regression safety (Property 7)
  - Test: run every existing `test-app.cpp` scenario with `peerFusionEnabled=true` but no peers ever received. Every assertion must still pass — byte-identical behavior to baseline.
  - _Depends: 9.6_
  - _Spec: Property 7; Requirement 11.1_

- [ ] **13.7** Replay bound (Property 9)
  - Test: capture a valid packet; replay after 30 s (timestamp-expired); replay within 30 s with same nonce. Both drops leave state unchanged.
  - _Depends: 2.5_
  - _Spec: Property 9; Requirements 3.3, 3.4_

- [ ] **13.8** Wire-format field bounding (Property 10)
  - Test: craft packet with `hostname_len = 100`, `hostname_len = 64`, `payload_length` implying overrun, `key_id = 0xFF`. Each drops silently; peer state unchanged; replay buffer unchanged.
  - _Depends: 2.5_
  - _Spec: Property 10; Requirements 4.4, 4.10-4.12_

- [ ] **13.9** Migration failure containment (Property 11)
  - Test: simulate migration failure (read-only DB file); assert app starts, scheduler runs, break transitions fire, `logEvent` / `openSpan` / `closeSpan` silently skip without crashing; on next restart with writable DB, migration succeeds.
  - _Depends: 8.7_
  - _Spec: Property 11; Requirements 6.6, 6.7_

- [ ] **13.10** Manual KVM-switch end-to-end
  - Manual procedure documented in a sibling `test/manual-kvm-switch.md`: start sane-break on r16 and hp, connect via KVM, type on r16 for a full break cycle, switch to hp mid-cycle, type on hp, confirm the break triggers on whichever machine is visible at break time and that the combined cycle length matches the configured `smallEvery`. Verify the tray tooltip on each machine shows the peer's share.
  - _Depends: 11.5, 12.6_
  - _Spec: Introduction; Requirements 2, 5_

- [ ] **13.11** Provisioning determinism (Property 8)
  - Manual matrix test of `scripts/ws doctor` (in both workstation-work and workstation-personal repos) against the Property 8 "healthy iff" conditions. Cases:
    - (a) install.sh just generated the secret on this host → expect PASS
    - (b) user hand-populated with a 64-hex-character value at mode 0600 before running install.sh → expect PASS
    - (c) file missing entirely → expect FAIL with a remediation hint referencing the install-script generator
    - (d) file present but mode `0644` → expect FAIL
    - (e) file present but 32 hex chars (too short) or 66 chars (too long) → expect FAIL
    - (f) file present but contains non-hex characters (e.g., "hello world" padded to 64 bytes) → expect FAIL
    - (g) file present but owned by a different user (simulate via `sudo chown`) → expect FAIL
  - _Depends: 11.3, 12.5_
  - _Spec: Property 8; Requirements 8.1, 8.3, 9.3, 9.5_
