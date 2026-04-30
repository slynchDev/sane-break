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

- [x] **4.1** Add gated `breakStart` → `broadcastBreakStart` connection
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

- [x] **4.2** Add `peerBreakRequested` consumer slot in `SaneBreakApp`
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

- [x] **4.3** Connect `peerBreakRequested` signal in constructor
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

- [x] **5.1** Override postpone in peer-triggered breaks
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

- [x] **5.2** Verify focus-mode interaction with peer breaks
  - Confirm by inspection that `onPeerBreakRequested` (task 4.2) does NOT check `data->isFocusMode()`. Focus mode does not exempt a user from coordinated breaks. The focus-cycle counter is decremented by `data->finishAndStartNextCycle()` on break exit — the same path used by locally-initiated breaks — so no special handling is needed.
  - Add a comment in `onPeerBreakRequested` after the meeting-state check:
    ```cpp
    // Focus mode is not consulted: peer-coordinated breaks fire through focus
    // cycles. The focus counter advances normally on break completion.
    ```
  - _Depends: 4.2_
  - _Spec: Requirement 5.2_

- [x] **5.3** Localize postpone-during-peer-break
  - Confirm by inspection of `src/app/app.cpp::SaneBreakApp::postpone(int)` and the existing `BreakPhase*` postpone handling that postpone during a break does not broadcast any packet. The `postpone()` method modifies local `AppData` state only.
  - No code change. Add a `// Postpone during a peer-triggered break is local-only — see synchronized-peer-break spec Req 5.4.` comment above `SaneBreakApp::postpone`.
  - _Depends: 4.2_
  - _Spec: Requirement 5.4_

---

## Phase 6: Property tests for safety invariants

- [x] **6.1** Property test: no break loop
  - In `test/test-app.cpp`, tests `peer_local_break_has_peerTriggered_false` and
    `peer_triggered_break_has_peerTriggered_true` verify the mechanism: local breaks
    produce `peerTriggered=false` (eligible to broadcast); peer-triggered breaks produce
    `peerTriggered=true` (suppresses re-broadcast, preventing A→B→A loops). The full
    two-machine round-trip is covered structurally — the flag is the sole broadcast gate
    in `SaneBreakApp`'s `breakStart` lambda.
  - _Depends: 4.1, 4.2, 4.3_
  - _Spec: Property 1, Requirements 1.3, 3.1, 3.2_

- [x] **6.2** Property test: peer break entry from Normal
  - Added `peer_break_request_from_normal_enters_break` (small path),
    `peer_break_request_big_type_honored` (big breaks enabled), and
    `peer_break_request_big_falls_back_when_disabled` (big breaks disabled →
    fallback to small) in `test/test-app.cpp` using `DummyApp::simulatePeerBreakRequest`.
  - _Depends: 4.2, 4.3_
  - _Spec: Property 2, Requirements 2.5, 2.6, 2.7_

- [x] **6.3** Property test: peer break entry from Paused
  - Added `peer_break_request_from_paused_clears_and_breaks` in `test/test-app.cpp`.
    Sets idle (→ Paused), calls `simulatePeerBreakRequest`, asserts Break state and
    `pauseReasons == 0`.
  - _Depends: 4.2, 4.3_
  - _Spec: Requirement 2.5_

- [x] **6.4** Property test: ignore during break
  - Added `peer_break_request_during_break_ignored` in `test/test-app.cpp`. Checks
    `currentSpanId` is unchanged after calling `simulatePeerBreakRequest` while already
    in Break state.
  - _Depends: 4.2, 4.3_
  - _Spec: Property 6, Requirement 2.3_

- [x] **6.5** Property test: ignore during meeting
  - Added `peer_break_request_during_meeting_ignored` in `test/test-app.cpp`. Asserts
    state remains Meeting and `isBreaking` is false.
  - _Depends: 4.2, 4.3_
  - _Spec: Property 4, Requirement 2.4_

- [x] **6.6** Property test: postpone overridden by peer break
  - Added `peer_break_request_overrides_local_postpone` in `test/test-app.cpp`. Calls
    `postpone(300)` first, then `simulatePeerBreakRequest`; asserts Break state.
  - _Depends: 4.2, 4.3, 5.1_
  - _Spec: Requirement 5.1_

- [x] **6.7** Property test: focus mode does not exempt
  - Added `peer_break_request_fires_during_focus` in `test/test-app.cpp`. Starts focus
    with 3 cycles, finishes entry break, calls `simulatePeerBreakRequest`, drives break
    to completion, asserts `focusCyclesRemaining == 2`.
  - _Depends: 4.2, 4.3, 5.2_
  - _Spec: Requirement 5.2_

- [x] **6.8** Property test: timer convergence after synchronized break
  - Added `peer_break_request_resets_local_countdown` (countdown zeroed during break)
    and `peer_break_request_timer_resyncs_on_completion` (full interval restored after
    break end) in `test/test-app.cpp`. Both machines restart from the same baseline.
  - _Depends: 4.1, 4.2, 4.3_
  - _Spec: Property 3, Requirement 4.3_

- [x] **6.9** Property test: behavioral equivalence
  - Added `peer_triggered_break_completes_like_local_break` in `test/test-app.cpp`.
    Drives a peer-triggered break to completion and asserts the same post-break state
    (countdown reset to full interval) as a local break produces.
  - _Depends: 4.1, 4.2, 4.3_
  - _Spec: Property 5, Requirement 3.3_

- [x] **6.10** Property test: single-machine regression safety
  - `peer_fusion_disabled_by_default` and `peer_fusion_toggle_is_inert_in_baseline_app`
    (already in `test/test-app.cpp` from Phase 13) pin this property. DummyApp has no
    network layer, so every test in this file implicitly asserts no `BREAK_START` is
    transmitted when fusion is disabled. The receive-path regression is covered in
    `test-remote-activity-monitor.cpp` (RAM stopped → receive path inactive).
  - _Depends: 4.1, 4.2, 4.3_
  - _Spec: Property 7, Requirements 1.4, 6.1_

- [x] **6.11** Property test: postpone locality
  - Added `postpone_during_peer_break_is_local_only` in `test/test-app.cpp`. Triggers
    a peer break then postpones; asserts break exits and countdown is set to postpone
    interval. No outbound packet is possible at DummyApp level (no network layer) —
    locality is structural.
  - _Depends: 4.1, 4.2, 4.3_
  - _Spec: Property 8, Requirement 5.4_

---

## Phase 7: Manual verification and documentation

- [x] **7.1** Add manual KVM verification procedure
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

- [x] **7.2** Update `Out of Scope` in cross-machine-activity-fusion spec
  - In `docs/specs/cross-machine-activity-fusion/spec.md` line 344, update the first bullet of `Out of Scope` to reference this spec:
    ```
    - **Break-state synchronization.** Originally deferred. Now specified
      separately in `docs/specs/synchronized-peer-break/spec.md` and
      implemented as event_type 0x04 = EVENT_BREAK_START.
    ```
  - This keeps spec history honest and lets future readers find the follow-up.
  - _Depends: 4.3_
  - _Spec: cross-machine-activity-fusion Out of Scope (historical)_
