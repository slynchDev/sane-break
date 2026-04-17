# Cross-machine activity fusion — branch status

> Companion to `spec.md` and `tasks.md`. Describes the branch's goals, what
> currently works, the known gaps, and what still needs to be done outside
> this repo before the feature can actually ship to the user's machines.

## Goal

The `cross-machine-activity-fusion` branch exists to solve a specific
workflow: the user runs two workstations (r16 for work, hp/x220 for
personal) and switches between them with a hardware KVM. Each sane-break
instance tracks its own idle state via OS idle APIs, so typing on the
KVM-focused machine while the other sits unused is invisible to the
unfocused machine — which then falsely believes the user is idle.

Consequences before this branch:
- Breaks drift out of sync between machines.
- A break can fire on a machine that hasn't been touched in 45 minutes
  because that machine's counter has been paused ("idle"), while the
  actively-used machine's counter has raced ahead.
- Idle-based pause-and-resume logic misfires: one machine pauses on
  idle, resumes when you swap KVM back, and may reset the cycle
  entirely — even though you never actually stopped working.

The branch fuses idle state across machines so all participating
instances share a single "is the user active anywhere" view, and
attributes per-host work seconds for the tray tooltip and stats window.

## Branch lineage

- Branched from `meeting-aware` at `933fdf0 Harden auto-meeting detection
  and toggle handling`. Full access to `AppStateMeeting`, audio-stream
  meeting detection, and all meeting-aware pause/resume logic.
- Merge-base with `main` is much older (`00254ccf`). `main` does not
  have meeting-aware at all.
- Branch tip: `2253658 test(cross-machine-activity-fusion): add
  property-test suite` (at time of writing).

## How it works at runtime

### Packet flow

Each instance:
1. Sends **ACTIVITY** heartbeats every `peerHeartbeatIntervalSeconds`
   (default 5 s) while the raw local idle timer reports non-idle.
2. Sends **IDLE_TRANSITION** edges the moment the raw local idle timer
   flips (active→idle or idle→active).
3. Listens on a bound UDP socket per allowlisted interface. Every
   packet is HMAC-SHA256 verified against the shared secret and dropped
   silently if it fails any of: magic / version / key_id / hostname
   length / payload length / structural HMAC / timestamp window / replay
   nonce / ingress interface allowlist.

### Fused idle state

`EffectiveIdleTime` wraps the raw local `SystemIdleTime` plus the
`RemoteActivityMonitor` and exposes the fused value

    isIdle() = wrapped->isIdle() AND NOT anyPeerActive

to the rest of the app via the existing `AppDependencies::idleTimer`
slot. Consumers (`AppStateBreak::enter`, `BreakPhaseFullScreen::tick`,
etc.) are unchanged — they see a single idle signal that already
incorporates peer state.

`anyPeerActive` is true iff at least one peer has sent an ACTIVITY
heartbeat or an IDLE_TRANSITION{active} within the last
`peerActiveWindowSeconds` (default 15 s) and has not subsequently sent
an IDLE_TRANSITION{idle}. The aggregate is recomputed every 1 s so a
silently-disappeared peer drops out of the aggregate within 1 s of
crossing the active window.

### Attribution

`RemoteActivityMonitor` maintains a 1 s tick counter per peer (and for
local) tracking active seconds since the last break. `AppContext::breakStart`
is connected to `ram->resetAttribution()` so the counters zero atomically
on every break entry path (normal tick expiry, `BigBreakNow`,
`EndMeetingBreakNow`, focus-mode entry).

The tray tooltip on each machine shows

    r16 68% · peer-hp 32% · 47m total

updated at most once every 2 s (independent of the 1 Hz countdown
refresh). The stats window gains a per-day per-host row below the daily
total when a given day has spans from more than one host.

### Runtime controls

The preferences window's Pause tab gains a "Peer fusion" group with:
- Enable checkbox
- Listen port (1024-65535)
- Active / unreachable / heartbeat window spinboxes
- Broadcast-interfaces plain-text allowlist (one per line)
- Read-only resolved-secret-path label
- Metadata-leak notice
- Live-indicator list (`{host} — active 3s ago` / `idle 12s ago`,
  refreshing every 2 s)

Toggling `peerFusionEnabled` at runtime starts/stops the monitor in
place without rebuilding the dependency graph. Changing the port or
interface list rebinds sockets and reverts to the last known-good
binding if the new one fails.

## Wire format

Every packet:

    magic(4='SBPA') | version(1) | key_id(1) | sender_uuid(16)
    | hostname_len(1) | hostname(0..63 UTF-8)
    | timestamp(8 BE) | nonce(8 BE)
    | event_type(1) | payload_length(2 BE) | payload
    | hmac(32, SHA256 over all bytes above)

Event types:
- `ACTIVITY` (0x01): payload is 4-byte big-endian activity counter
- `IDLE_TRANSITION` (0x02): payload is 1 byte `{0x00=idle, 0x01=active}`

Cap: 512 bytes per packet, 63-byte hostname. Smaller than any plausible
MTU; not fragmentable.

## Security model

- **Secret**: 32 bytes, stored hex-encoded (64 chars) at
  `~/.secrets/sane-break-peer` with mode 0600. Env override via
  `SANE_BREAK_PEER_SECRET_FILE`. Loader auto-repairs wider-than-0600
  permissions, rejects malformed hex, and returns empty on any failure
  (fusion then runs in single-machine pass-through mode).
- **Integrity + auth**: HMAC-SHA256 over the full packet body, constant-
  time compared on receive. Forged or tampered packets produce zero
  state mutation.
- **Replay**: 30 s timestamp window (with 5 s forward clock-skew
  tolerance) plus a 256-entry `(sender_uuid, nonce)` ring buffer. Any
  duplicate or stale packet exits before touching peer state.
- **Metadata confinement**: `peerBroadcastInterfaces` is an allowlist,
  not a default-to-all. Egress uses per-interface subnet-directed
  broadcast (e.g. `192.168.1.255`), not `255.255.255.255`, so kernel
  routing can't leak packets onto non-allowlisted NICs. Ingress is
  filtered via `QNetworkDatagram::interfaceIndex()` against the same
  allowlist, so a packet that arrives on a corporate VPN or guest Wi-Fi
  is dropped even if its HMAC is valid.
- **Feedback-loop immunity**: `RemoteActivityMonitor` takes the RAW
  `SystemIdleTime`, not the `EffectiveIdleTime` facade, so a peer
  reporting active cannot cause us to rebroadcast "active" indefinitely.

## Current state

### Completed and on the branch

11 of 13 phases from `tasks.md` land on this branch:

| Phase | Content |
|-------|---------|
| 1 | Foundations: preferences, wire-format constants, breakStart/breakEnd signals, virtual isIdle |
| 2 | Crypto + serialization: secret loader, encode/decode, replay buffer |
| 3 | RemoteActivityMonitor peer state + socket bind with interface allowlist |
| 4 | Outbound heartbeat + idle-edge emission |
| 5 | EffectiveIdleTime facade |
| 6 | Per-host attribution counters + resetAttribution + activityBreakdown |
| 7 | Tray peer-breakdown tooltip with 2 s throttle |
| 8 | DB schema v2: `host` column + migration + per-host query + stats row |
| 9 | AppDependencies wiring: ram construction, facade injection, runtime toggle, rebind-with-revert, breakStart connect |
| 10 | Preferences UI for the six peer settings + live-indicator list |
| 13 | Property-test mapping + migration-failure test + manual KVM procedure doc |

Test coverage: 7 test binaries, ~175 tests, 100% pass at `HEAD=2253658`.
Release build produces a working `sane-break` binary.

### Not on the branch, still deferred

- **Phase 11** (workstation-work integration)
- **Phase 12** (workstation-personal integration)
- **13.11** (provisioning determinism matrix) — exercised from the
  workstation repos, not from here

## Known gap: no cross-peer meeting awareness

The branch fuses idle state only. The wire format has exactly two event
types (ACTIVITY, IDLE_TRANSITION); there is no `EVENT_MEETING_TRANSITION`.
Meeting-aware pause is applied purely locally by each instance from its
own audio-stream detection.

### Consequence

During a Zoom call on r16 with hp running peer-fusion:

1. r16 detects the meeting → `AppStateMeeting` → local breaks suspended.
2. r16 keeps broadcasting ACTIVITY (notes, clicks) throughout the call.
3. hp sees peer activity → `EffectiveIdleTime` suppresses idle → hp's
   countdown ticks down normally.
4. Over a 45-minute call hp fires 2-3 ghost break cycles that the user
   never sees (KVM focused on r16). Cycle counter advances on hp;
   big-break scheduling shifts.
5. Call ends, r16 runs `endMeetingBreakLater(smallEvery)` and starts a
   fresh cycle. hp is deep into "about to fire".
6. User swaps KVM to hp to process meeting notes. hp fires a break
   within seconds — ambushing the user right as they sit down to focus.

### Where it does *not* bite

- Single-machine usage.
- Both machines simultaneously in active keyboard use (no meeting).
- Passive listening meetings with no keyboard activity — r16 goes idle
  naturally, hp sees no peer activity, both pause in sync.
- Meetings shorter than `smallEvery` — the ghost cycle doesn't have
  time to fire.

### Shape of a fix (not built)

Additive on top of the current design. No rebase or API break:

1. New event type `EVENT_MEETING_TRANSITION = 0x03`, payload 1 byte
   `{0x00=ended, 0x01=started}`.
2. `AppContext::meetingStart` / `meetingEnd` signals → wire to
   `RemoteActivityMonitor::broadcastMeetingTransition(...)` alongside
   the existing breakStart connect.
3. `PeerState::inMeeting` bool.
4. New aggregate `anyPeerInMeeting()` signal. Consumer options:
   - `AppContext` treats it as a new `PauseReason::PeerMeeting` (cleanest,
     reuses the existing pause machinery which also happens to reset
     the cycle on long pauses).
   - Or: `EffectiveIdleTime` reports "idle" whenever any peer is in a
     meeting, effectively freezing the non-meeting peer's countdown.
     Simpler but less semantically clean.
5. Tests: peer-meeting-transitions-across, self-meeting-doesn't-broadcast-
   back (feedback-loop immunity), meeting-start-then-disconnect-peer-
   recovers-after-unreachable-window.

Roughly Phase 6 in size. Would live as a Phase 14 if resumed.

### User-level mitigations without code

- Tray-toggle peer fusion off before a call. Requires remembering.
- Run `autoMeetingOnApp=true` on both machines with the same meeting app
  installed. Usually impractical because the call only happens on one.

## Not yet integrated into workstation repos

The in-repo work above runs but does not get provisioned onto the user's
machines. What's missing, split by repo:

### `~/git/workstation-work` (r16)

- `install.sh`: race-free secret generation
  (`(umask 177 && openssl rand -hex 32 > ~/.secrets/sane-break-peer)`)
  + chmod 0600 repair if the file exists with wider mode.
- `scripts/doctor.sh`: four-part assertion (exists, owned by `$USER`,
  mode 0600, content is 64 hex chars).
- `configs/sane-break/SaneBreak.ini`: add a `[peer]` section with
  `fusion-enabled=true` and `broadcast-interfaces=<lan-nic>`.
- `profiles/r16.conf`: no new fields needed (the ini carries the
  interface name; the secret path is fixed).

### `~/git/workstation-personal` (hp, x220)

Additional work vs the work repo:

- `install.sh` currently clones `https://github.com/AllanChain/sane-break`
  (upstream) into `~/src/sane-break`. Needs to switch to
  `https://github.com/slynchDev/sane-break@meeting-aware` and unify the
  checkout path with work at `~/git/sane-break`.
- Mirror the work repo's secret-generation + chmod-repair logic, or
  auto-fetch the secret from r16 via `scp` (the same pattern the
  existing slack-notification-relay uses for its bearer token and TLS
  cert).
- Mirror the doctor assertions.
- Add the `[peer]` section to the personal `SaneBreak.ini` with each
  host's own broadcast interface.

### Suggested provisioning pattern

The workstation repos already ship a working cross-machine secret-
provisioning flow for the slack-notification-relay (see
`docs/specs/slack-notification-relay/spec.md` in workstation-work):

- r16 generates the secret (interactive `scripts/provision-relay-token.sh`)
- hp/x220 auto-fetch via `scp r16.slynch.org:.secrets.d/...` using the
  host's existing SSH key
- doctor asserts presence + mode + content shape
- TLS CA cert gets the same `scp`-based fetch with trust-store install

Peer-fusion's secret is structurally simpler (just 32 bytes, no TLS, no
bearer token), so the same pattern reduces cleanly. No 1Password, no
manual copy, just `scp` once during install. The ntfy relay does NOT
encrypt its payload (TLS + bearer only), whereas peer-fusion HMAC-signs
every packet with the shared secret — different threat models.

## Building and testing the branch

The branch builds cleanly in a git worktree so the `meeting-aware`
checkout in the main working directory isn't disturbed:

    git worktree add /tmp/sane-break-cmaf cross-machine-activity-fusion
    cmake -S /tmp/sane-break-cmaf -B /tmp/sane-break-cmaf/build \
        -DCMAKE_BUILD_TYPE=Release -DTESTING=ON
    cmake --build /tmp/sane-break-cmaf/build -j4
    cmake --build /tmp/sane-break-cmaf/build --target check -j4

Produces `/tmp/sane-break-cmaf/build/sane-break` (~1.5 MB) and runs 7
test binaries with ~175 assertions. All pass.

For live smoke-testing without the workstation-repo provisioning:

    export SANE_BREAK_PEER_SECRET_FILE=/tmp/sbp-secret
    (umask 177 && openssl rand -hex 32 > /tmp/sbp-secret)
    # Then edit ~/.config/SaneBreak/SaneBreak.ini and add:
    #   [peer]
    #   fusion-enabled=true
    #   broadcast-interfaces=<your LAN NIC name>
    #   listen-port=45454

Without those three pieces (secret file, `fusion-enabled=true`,
non-empty `broadcast-interfaces`) the feature is dormant and the app
behaves byte-identically to `meeting-aware`. Property 7 in the spec
pins this and the `peer_fusion_disabled_by_default` test enforces it in
CI.

## Status line

Feature-complete within this repo, test-covered, verified-building.
Two gaps prevent real-world use: (1) no cross-peer meeting awareness —
causes ambush breaks after meetings, described above; (2) not
provisioned into the workstation repos — so even if the feature were
perfect, it wouldn't run on any of the user's actual machines yet.
