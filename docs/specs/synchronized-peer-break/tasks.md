# Synchronized Peer Break - Implementation Tasks

> Auto-generated from spec.md. Each task maps to a spec section.
> Mark tasks: `[ ]` pending, `[~]` in progress, `[x]` done, `[-]` skipped, `[!]` blocked.

---

## Phase 1: Wire format extension

- [x] **1.1** Add `EVENT_BREAK_START` event type constant
  - Extend the `EventType` enum in `src/lib/peer-packet.h` with `EVENT_BREAK_START = 0x04`. Place it after `EVENT_MEETING_TRANSITION = 0x03`. No other constants change.
  - _Spec: Requirement 1.1_

- [x] **1.2** Add `BreakKind` payload enum
  - In `src/lib/peer-packet.h`, after the existing `MeetingState` enum, add:
    ```cpp
    enum BreakKind : uint8_t {
      BREAK_SMALL = 0x00,
      BREAK_BIG = 0x01,
    };
    ```
    The name `BreakKind` (rather than `BreakType`) avoids colliding with the existing `enum class BreakType { Small, Big }` in `src/core/flags.h`. The two enums map 1:1: `BreakType::Small ↔ BREAK_SMALL`, `BreakType::Big ↔ BREAK_BIG`. The mapping helpers live in task 2.1.
  - _Spec: Requirement 1.1, Glossary (Break_Type_Byte)_

- [x] **1.3** Extend packet decoder with `BREAK_START` payload validation
  - In `src/lib/peer-packet.cpp::decodePacket`, add a case for `event_type == EVENT_BREAK_START`: require `payload_length == 1` AND payload byte ∈ {`BREAK_SMALL`, `BREAK_BIG`}. Drop silently (populate `whyDropped` at debug level only) on any mismatch. Place the case alongside the existing `EVENT_ACTIVITY` and `EVENT_IDLE_TRANSITION` validation arms.
  - _Depends: 1.1, 1.2_
  - _Spec: Requirement 1.1_

- [x] **1.4** Unit tests for `BREAK_START` packet round-trip and decoder validation
  - In `tests/test-peer-packet.cpp`, add cases mirroring the existing `round_trip_meeting_transition_*` and `decode_fails_on_meeting_transition_*` patterns:
    - `round_trip_break_start_small`: encode `Packet{eventType=EVENT_BREAK_START, payload={BREAK_SMALL}}`, decode, assert equality
    - `round_trip_break_start_big`: same with `BREAK_BIG`
    - `decode_fails_on_break_start_wrong_payload_size`: payload of 0 bytes and 2 bytes both rejected
    - `decode_fails_on_break_start_invalid_state_byte`: payload byte `0x02` rejected
    - `decode_fails_on_break_start_with_corrupted_hmac`: HMAC tampering rejected (re-uses existing tamper helper)
  - _Depends: 1.3_
  - _Spec: Requirement 1.1_

---

## Phase 2: RemoteActivityMonitor broadcast and receive

- [x] **2.1** Add `BreakType ↔ BreakKind` mapping helpers
  - In `src/lib/remote-activity-monitor.cpp` (or a small inline helper at the top of the file), add `static uint8_t breakTypeToWire(BreakType t)` and `static BreakType breakTypeFromWire(uint8_t b)` returning `BreakType::Small` for `BREAK_SMALL` and `BreakType::Big` for any other value (defensive default — invalid values are already filtered out by the decoder per 1.3, but if one slips through we treat it as a small break, the less-disruptive choice).
  - _Depends: 1.2_
  - _Spec: Requirement 1.2, Constraints_

- [x] **2.2** Add `buildBreakStartPacket` builder
  - In `src/lib/remote-activity-monitor.h`, declare:
    ```cpp
    QByteArray buildBreakStartPacket(const QDateTime& now, uint8_t kind) const;
    ```
    Implement in `src/lib/remote-activity-monitor.cpp` exactly like `buildMeetingTransitionPacket`: construct a `Packet` with `eventType = EVENT_BREAK_START`, `payload = QByteArray(1, kind)`, fresh nonce, supplied timestamp; call `encodePacket(p, m_secret)`. Return empty `QByteArray` if `m_secret` is empty (caller treats as no-op).
  - _Depends: 1.1, 1.2, 1.3_
  - _Spec: Requirement 1.2_

- [x] **2.3** Add `broadcastBreakStart(BreakType)` slot
  - In `src/lib/remote-activity-monitor.h`, add to the `public slots:` block (next to `broadcastMeetingStart` / `broadcastMeetingEnd`):
    ```cpp
    void broadcastBreakStart(BreakType type);
    ```
    Implement in `src/lib/remote-activity-monitor.cpp` to mirror `broadcastMeetingStart`:
    1. If `!m_running` or `m_secret.isEmpty()`, return without doing anything.
    2. Compute `now = QDateTime::currentDateTimeUtc()`.
    3. Call `buildBreakStartPacket(now, breakTypeToWire(type))` to get the bytes.
    4. If non-empty, call `sendToAllInterfaces(bytes, now)`.
    5. The function MUST NOT inspect `peerFusionEnabled` directly — that gate lives on the connect path in `SaneBreakApp`. (Same pattern as the meeting broadcast.)
  - Include `core/flags.h` in `remote-activity-monitor.cpp` if not already pulled in transitively (for `BreakType`).
  - _Depends: 1.1, 2.1, 2.2_
  - _Spec: Requirements 1.2, 1.4_

- [x] **2.4** Add `peerBreakRequested(BreakType)` signal
  - In `src/lib/remote-activity-monitor.h`, add to the `signals:` block (after `peerMeetingChanged`):
    ```cpp
    void peerBreakRequested(BreakType type);
    ```
    No implementation needed — Qt MOC generates the body. The signal is emitted in task 2.5.
  - _Depends: 2.1_
  - _Spec: Requirement 2.1_

- [x] **2.5** Wire `BREAK_START` receive path in `handleReceivedDatagram`
  - In `src/lib/remote-activity-monitor.cpp::handleReceivedDatagram`, after the existing `EVENT_MEETING_TRANSITION` arm, add a new arm for `event_type == EVENT_BREAK_START`:
    1. Apply the same loopback guard as other event types: if `decoded->senderUuid == m_senderUuid`, return without emitting (already filtered at the top of the function via the existing self-traffic drop, but the structure of the existing meeting arm shows the placement).
    2. Map the payload byte via `breakTypeFromWire(payload[0])`.
    3. Emit `peerBreakRequested(type)`.
    - Unlike `EVENT_MEETING_TRANSITION`, `BREAK_START` packets do NOT mutate `PeerState` fields. The signal is fire-and-forget — there is no aggregate state to maintain (no "is any peer in a break" tracking). A break is a single edge-triggered event, not a sustained state.
    - Do NOT update `lastSeenActive`, `lastSeenIdle`, `lastSeenMeetingTransition`, or `lastEventType`. The `BREAK_START` packet is not evidence of activity or idleness; it is a coordination signal only.
  - _Depends: 1.1, 2.1, 2.4_
  - _Spec: Requirements 2.1, 3.4_

- [x] **2.6** Unit tests for broadcast and receive
  - In `tests/test-remote-activity-monitor.cpp`, add:
    - `broadcast_break_start_emits_signed_packet`: call `broadcastBreakStart(BreakType::Small)`, capture sent bytes via the existing test transmit hook, decode, assert `eventType == EVENT_BREAK_START` and payload byte == `BREAK_SMALL`.
    - `broadcast_break_start_skipped_when_not_running`: with `m_running == false`, `broadcastBreakStart` produces no transmit.
    - `receive_break_start_emits_peer_break_requested`: inject a valid `BREAK_START` packet via `handleReceivedDatagram`, assert `peerBreakRequested` was emitted exactly once with the expected `BreakType`.
    - `receive_break_start_does_not_mutate_peer_active_state`: inject a `BREAK_START` packet, assert `peerActivityChanged` is NOT emitted, `lastSeenActive`/`lastSeenIdle` of any tracked peer are unchanged.
    - `receive_break_start_loopback_dropped`: inject a packet with `senderUuid == m_senderUuid`, assert no `peerBreakRequested` emission.
    - `receive_break_big_emits_big_break_type`: payload byte `BREAK_BIG` → signal carries `BreakType::Big`.
  - _Depends: 2.3, 2.5_
  - _Spec: Requirements 1.2, 2.1, 3.4_

---

## Phase 3: AppStateBreak peerTriggered flag

- [x] **3.1** Add `peerTriggered` field to `AppStateBreak`
  - In `src/core/app-states.h::AppStateBreak`, add:
    ```cpp
    bool peerTriggered = false;
    ```
    as a public member (siblings with `data`). Default-initialized to `false` so locally-constructed `AppStateBreak` instances behave exactly as today. Peer-triggered construction (task 4.2) sets it to `true`.
  - _Spec: Requirement 3.1, Glossary (Local_Break, Peer_Break)_

- [x] **3.2** Verify `peerTriggered` does not alter break behavior
  - Add a comment block in `src/core/app-states.cpp` immediately above `AppStateBreak::enter` documenting that `peerTriggered` is read ONLY by `SaneBreakApp` to gate the broadcast path, and that all break-state behavior (phase transitions, force-break, sounds, DB spans, end processing) is independent of this flag.
  - No code changes — this task exists to anchor the invariant during code review.
  - _Depends: 3.1_
  - _Spec: Requirement 3.3_

---

## Phase 4: SaneBreakApp wiring

- [ ] **4.1** Add gated `breakStart` → `broadcastBreakStart` connection
  - In `src/app/app.cpp::SaneBreakApp::SaneBreakApp` constructor, inside the existing `if (m_ram) { ... }` block (next to the `breakStart` → `resetAttribution` connect at lines 75-76), add a second handler for `breakStart` that broadcasts a `BREAK_START` packet only when the current break was locally initiated. Implementation:
    ```cpp
    connect(this, &AppContext::breakStart, m_ram, [this]() {
      if (!preferences->peerFusionEnabled->get()) return;
      auto* breakState = dynamic_cast<AppStateBreak*>(m_currentState.get());
      if (!breakState || breakState->peerTriggered) return;
      m_ram->broadcastBreakStart(data->breakType());
    });
    ```
    The `peerFusionEnabled` check matches the `start()` gate at line 162. The `dynamic_cast` is safe because `breakStart` is emitted from `AppStateBreak::enter()` — the current state is guaranteed to be an `AppStateBreak`, but the cast also defends against any future emission path. `data->breakType()` returns the type assigned to the current break.
  - _Depends: 2.3, 3.1_
  - _Spec: Requirements 1.3, 1.4, 3.2_

- [ ] **4.2** Add `peerBreakRequested` consumer slot in `SaneBreakApp`
  - In `src/app/app.h::SaneBreakApp`, declare:
    ```cpp
    private slots:
      void onPeerBreakRequested(BreakType type);
    ```
  - In `src/app/app.cpp`, implement `SaneBreakApp::onPeerBreakRequested(BreakType type)`:
    1. If current state is `AppStateBreak` (already breaking) → return.
    2. If current state is `AppStateMeeting` → return.
    3. If current state is `AppStatePaused` → call `data->clearPauseReasons()` to drop all pause flags before transitioning.
    4. Set the upcoming break's type:
       - If `type == BreakType::Big` AND `data->effectiveBigBreakEnabled()`, call `data->makeNextBreakBig()`.
       - Otherwise leave as-is (small is the default; if big is disabled, treat the peer's "big" as small — log at debug level: "peer requested big break but big breaks disabled locally; using small").
    5. Call `data->earlyBreak()` to zero the local countdown so the tray no longer shows stale "next break in N min" text during the transition.
    6. Construct an `AppStateBreak` with `peerTriggered = true`, then call `transitionTo(std::move(breakState))`. Note: `transitionTo` takes `unique_ptr<AppState>`, so write `auto breakState = std::make_unique<AppStateBreak>(); breakState->peerTriggered = true; transitionTo(std::move(breakState));`.
    7. The resulting `AppStateBreak::enter()` will emit `breakStart()` synchronously; the gated handler in 4.1 sees `peerTriggered == true` and skips the broadcast.
  - _Depends: 2.4, 3.1, 4.1_
  - _Spec: Requirements 2.2, 2.3, 2.4, 2.5, 2.6, 2.7, 4.2_

- [ ] **4.3** Connect `peerBreakRequested` signal in constructor
  - In `src/app/app.cpp::SaneBreakApp::SaneBreakApp` constructor, inside the existing `if (m_ram) { ... }` block, add:
    ```cpp
    connect(m_ram, &peer::RemoteActivityMonitor::peerBreakRequested, this,
            &SaneBreakApp::onPeerBreakRequested);
    ```
    Place it after the existing `peerMeetingChanged` connect (around line 90) so the wiring order mirrors the spec's logical order: meetings, then breaks.
  - _Depends: 2.4, 4.2_
  - _Spec: Requirements 2.1, 2.2_

---

## Phase 5: Postpone, focus, and meeting interactions

- [ ] **5.1** Override postpone in peer-triggered breaks
  - In `src/app/app.cpp::SaneBreakApp::onPeerBreakRequested` (task 4.2), do NOT short-circuit on `data->isPostponing()`. Postpone defers the LOCAL timer expiry but cannot block a peer-initiated break. The user's intent is RSI protection across machines; postponing on machine A cannot make machine B's "you've been typing for 60 minutes" signal disappear.
  - Add a comment in `onPeerBreakRequested` immediately before the state checks documenting this:
    ```cpp
    // Peer-initiated breaks override postpone state. Postpone is a local
    // affordance to delay the LOCAL timer; it does not negate the peer's
    // signal that aggregate typing has hit the configured limit.
    ```
  - No additional code — the existing `onPeerBreakRequested` skeleton from 4.2 already does not consult `isPostponing()`.
  - _Depends: 4.2_
  - _Spec: Requirement 5.1_

- [ ] **5.2** Verify focus-mode interaction with peer breaks
  - Confirm by inspection that `onPeerBreakRequested` (task 4.2) does NOT check `data->isFocusMode()`. Focus mode does not exempt a user from coordinated breaks. The focus-cycle counter is decremented by `data->finishAndStartNextCycle()` on break exit — the same path used by locally-initiated breaks — so no special handling is needed.
  - Add a comment in `onPeerBreakRequested` after the meeting-state check:
    ```cpp
    // Focus mode is not consulted: peer-coordinated breaks fire through focus
    // cycles. The focus counter advances normally on break completion.
    ```
  - _Depends: 4.2_
  - _Spec: Requirement 5.2_

- [ ] **5.3** Localize postpone-during-peer-break
  - Confirm by inspection of `src/app/app.cpp::SaneBreakApp::postpone(int)` and the existing `BreakPhase*` postpone handling that postpone during a break does not broadcast any packet. The `postpone()` method modifies local `AppData` state only.
  - No code change. Add a `// Postpone during a peer-triggered break is local-only — see synchronized-peer-break spec Req 5.4.` comment above `SaneBreakApp::postpone`.
  - _Depends: 4.2_
  - _Spec: Requirement 5.4_

---

## Phase 6: Property tests for safety invariants

- [ ] **6.1** Property test: no break loop
  - In `tests/test-property-mapping.cpp` (or a new `tests/test-synchronized-peer-break.cpp` if the existing file is large), add `peer_break_does_not_loop`:
    1. Build two in-process `RemoteActivityMonitor` + `SaneBreakApp` pairs sharing a synthetic `sendToAllInterfaces` bridge that delivers each TX to the peer's `handleReceivedDatagram`.
    2. Trigger a local timer expiry on machine A → `AppStateBreak::enter` → `breakStart` signal.
    3. Capture all `BREAK_START` packets transmitted in both directions.
    4. Assert exactly ONE `BREAK_START` packet was transmitted (from A to B). B's `peerTriggered=true` break must not produce a second packet.
  - _Depends: 4.1, 4.2, 4.3_
  - _Spec: Property 1, Requirements 1.3, 3.1, 3.2_

- [ ] **6.2** Property test: peer break entry from Normal
  - Add `peer_break_request_from_normal_enters_break`:
    1. Local in `AppStateNormal`, no postpone, no focus.
    2. Inject a `BREAK_START` packet via `handleReceivedDatagram`.
    3. Assert local state transitions to `AppStateBreak` within the same event-loop turn.
    4. Assert `data->breakType()` matches the packet's `BreakKind`.
    5. Assert `peerTriggered == true` on the resulting `AppStateBreak`.
  - _Depends: 4.2, 4.3_
  - _Spec: Property 2, Requirements 2.5, 2.6, 2.7_

- [ ] **6.3** Property test: peer break entry from Paused
  - Add `peer_break_request_from_paused_clears_and_breaks`:
    1. Local in `AppStatePaused` with `PauseReason::Idle` set.
    2. Inject a `BREAK_START` packet.
    3. Assert local state transitions to `AppStateBreak` (not via `Normal`).
    4. Assert `data->pauseReasons() == 0` (cleared before transition).
  - _Depends: 4.2, 4.3_
  - _Spec: Requirement 2.5_

- [ ] **6.4** Property test: ignore during break
  - Add `peer_break_request_during_break_ignored`:
    1. Local already in `AppStateBreak` (e.g., locally triggered).
    2. Inject a `BREAK_START` packet.
    3. Assert `breakStart` signal was NOT re-emitted, span ID unchanged, no UI re-entry.
  - _Depends: 4.2, 4.3_
  - _Spec: Property 6, Requirement 2.3_

- [ ] **6.5** Property test: ignore during meeting
  - Add `peer_break_request_during_meeting_ignored`:
    1. Local in `AppStateMeeting` (synthesize via `startMeeting(600, "test")`).
    2. Inject a `BREAK_START` packet.
    3. Assert local state remains `AppStateMeeting`. No break window shown.
  - _Depends: 4.2, 4.3_
  - _Spec: Property 4, Requirement 2.4_

- [ ] **6.6** Property test: postpone overridden by peer break
  - Add `peer_break_request_overrides_local_postpone`:
    1. Local in `AppStateNormal` with `data->isPostponing() == true`.
    2. Inject a `BREAK_START` packet.
    3. Assert local state transitions to `AppStateBreak` (postpone does not block).
  - _Depends: 4.2, 4.3, 5.1_
  - _Spec: Requirement 5.1_

- [ ] **6.7** Property test: focus mode does not exempt
  - Add `peer_break_request_fires_during_focus`:
    1. Local in `AppStateNormal` with `data->isFocusMode() == true`, `focusCyclesRemaining == 3`.
    2. Inject a `BREAK_START` packet.
    3. Assert state transitions to `AppStateBreak`.
    4. Drive the break to completion via `finishAndStartNextCycle()`; assert `focusCyclesRemaining` decremented to `2`.
  - _Depends: 4.2, 4.3, 5.2_
  - _Spec: Requirement 5.2_

- [ ] **6.8** Property test: timer convergence after synchronized break
  - Add `synchronized_break_resyncs_timers`:
    1. Two in-process app instances A and B with their `secondsToNextBreak` initialized to differ by 5 minutes (A at 60s, B at 360s — simulating drift).
    2. Drive A's timer to 0; A enters break, broadcasts `BREAK_START`, B enters break.
    3. Complete the break on both machines (drive `finishAndStartNextCycle()`).
    4. Assert `A.secondsToNextBreak == B.secondsToNextBreak` within ±1s.
  - _Depends: 4.1, 4.2, 4.3_
  - _Spec: Property 3, Requirement 4.3_

- [ ] **6.9** Property test: behavioral equivalence
  - Add `peer_triggered_break_indistinguishable_from_local`:
    1. Run a locally-initiated break to completion, capture: phase sequence, opened span IDs, sound calls, force-break counter behavior, exit path.
    2. Reset the app, run a peer-triggered break of the same type to completion, capture the same observables.
    3. Assert the two captures are equal modulo absolute timestamps and the absence of an outbound `BREAK_START` in run 2.
  - _Depends: 4.1, 4.2, 4.3_
  - _Spec: Property 5, Requirement 3.3_

- [ ] **6.10** Property test: single-machine regression safety
  - Add `peer_fusion_disabled_does_not_send_break_start` and `peer_fusion_disabled_ignores_received_break_start`:
    1. With `peerFusionEnabled == false`, drive a local break to completion. Assert no `BREAK_START` packet transmitted.
    2. With `peerFusionEnabled == false`, inject a `BREAK_START` datagram via `handleReceivedDatagram`. Assert no state change (RemoteActivityMonitor is stopped, the receive path is not active).
  - _Depends: 4.1, 4.2, 4.3_
  - _Spec: Property 7, Requirements 1.4, 6.1_

- [ ] **6.11** Property test: postpone locality
  - Add `postpone_during_peer_break_does_not_broadcast`:
    1. Trigger a peer-initiated break on machine B.
    2. User chooses postpone from B's break window.
    3. Assert no packet transmitted from B to A as a result of postpone.
    4. Assert A's break-cycle position is unchanged.
  - _Depends: 4.1, 4.2, 4.3_
  - _Spec: Property 8, Requirement 5.4_

---

## Phase 7: Manual verification and documentation

- [ ] **7.1** Add manual KVM verification procedure
  - Append a section to `docs/specs/cross-machine-activity-fusion/STATUS.md` (or create a new `docs/specs/synchronized-peer-break/STATUS.md`) documenting the manual smoke test:
    1. Both machines running peer fusion + synchronized break.
    2. Set break interval to 2 minutes for fast iteration.
    3. Type continuously on machine A for 2 minutes.
    4. When A's break fires, switch KVM to B within 5 seconds.
    5. Verify B is already in a break window (not still showing a countdown).
    6. Postpone on B; verify A is NOT affected (A's break still showing on its display).
    7. Repeat with `BREAK_BIG` (set `Big break every` to 1 small break for fast iteration).
  - _Depends: 4.3_
  - _Spec: All requirements (manual integration test)_

- [ ] **7.2** Update `Out of Scope` in cross-machine-activity-fusion spec
  - In `docs/specs/cross-machine-activity-fusion/spec.md` line 344, update the first bullet of `Out of Scope` to reference this spec:
    ```
    - **Break-state synchronization.** Originally deferred. Now specified
      separately in `docs/specs/synchronized-peer-break/spec.md` and
      implemented as event_type 0x04 = EVENT_BREAK_START.
    ```
  - This keeps spec history honest and lets future readers find the follow-up.
  - _Depends: 4.3_
  - _Spec: cross-machine-activity-fusion Out of Scope (historical)_
