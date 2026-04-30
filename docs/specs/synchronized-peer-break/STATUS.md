# Synchronized Peer Break — Status

## Implementation Status

All phases complete. See `tasks.md` for task-by-task detail.

| Phase | Description | Status |
|-------|-------------|--------|
| 1 | Wire format extension (`EVENT_BREAK_START`) | ✅ |
| 2 | RemoteActivityMonitor broadcast and receive | ✅ |
| 3 | `AppStateBreak::peerTriggered` flag | ✅ |
| 4 | SaneBreakApp wiring | ✅ |
| 5 | Postpone, focus, and meeting interactions | ✅ |
| 6 | Property tests for safety invariants | ✅ |
| 7 | Documentation | ✅ |

## Manual KVM Verification Procedure

Use this procedure to verify the feature end-to-end on real hardware before
shipping. It requires two machines running sane-break with peer fusion enabled
and the shared secret provisioned.

### Setup

1. On both machines, set **Small break every** to **2 minutes** (Settings →
   Schedule) so breaks fire quickly during testing.
2. Set **Big break every** to **1 small break** on both machines so a big break
   fires after the first small break — allows testing `BREAK_BIG` in the same
   session.
3. Ensure peer fusion is enabled on both machines and the status indicator in
   the tray shows both machines active.

### Test 1: Small break synchronization

1. Type continuously on **machine A** for 2 minutes.
2. Machine A's break timer fires → the break window appears on A.
3. **Within 5 seconds**, switch the KVM to machine B.
4. **Expected:** Machine B is already showing the break window, not a countdown.
5. Wait out the break on both machines (or close them via Escape if testing only
   the trigger, not the break duration).

### Test 2: Postpone locality

1. Repeat Test 1 so both machines are in a break simultaneously.
2. On **machine B**, postpone the break (click Postpone in the break window or
   system tray).
3. **Expected:** Machine B's break window closes; machine A's break window
   remains open and is unaffected.
4. Switch KVM back to A; verify A is still breaking normally.

### Test 3: Big break synchronization

1. Let the small break from Test 1 complete naturally.
2. Since **Big break every** is set to 1, the next break should be a big break.
3. Type on machine A until the big break fires.
4. **Expected:** Machine B also enters a big break window (same longer duration).

### Test 4: Meeting exemption

1. On machine A, start a meeting via the tray icon (any duration).
2. Force a break on machine B via **Break now** in the tray — machine B
   broadcasts `BREAK_START`.
3. **Expected:** Machine A remains in meeting mode; the break signal from B does
   not interrupt A's meeting.

### Test 5: Peer fusion disabled regression

1. Disable peer fusion on machine B (Settings → Network → uncheck Peer Fusion).
2. Let machine A's timer fire naturally.
3. **Expected:** Machine A enters a break normally. Machine B does not receive or
   react to the `BREAK_START` packet (B's countdown continues uninterrupted).

### Restoring defaults

After testing, reset the break interval to the preferred production values
(typically 20–30 minutes for small, 3–4 small breaks before big).
