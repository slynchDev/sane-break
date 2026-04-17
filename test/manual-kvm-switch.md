# Manual KVM-switch end-to-end test

Covers Phase 13 task 13.10 (manual procedure) and exercises the
user-facing goal of the cross-machine-activity-fusion feature: a single
break schedule that accounts for combined work time across two
workstations sharing monitors through a KVM switch.

## Prerequisites

- Two machines (r16 and hp, per the current workstation profiles).
- Both running the `meeting-aware` fork of sane-break.
- `~/.secrets/sane-break-peer` populated with the same 64-hex-character
  secret on both machines, at mode 0600.
- Each host's `peer/broadcast-interfaces` points at the LAN NIC.
- `peer/fusion-enabled=true` in both machines' `SaneBreak.ini`.
- A KVM switch wired so a single keyboard and monitor set can be
  toggled between r16 and hp.

## Procedure

1. Launch sane-break on both r16 and hp. Confirm each tray icon is
   present and the countdown is active.
2. Switch the KVM to r16. Type continuously for `smallEvery / 2`
   seconds. Do not idle.
3. Switch the KVM to hp without pausing. Type continuously for another
   `smallEvery / 2` seconds.
4. Observe which machine shows the break prompt at the end of the
   combined cycle. Exactly one machine should break, and the total
   elapsed time between the last break and this one should be close
   to the configured `smallEvery`.
5. Hover the tray icon on each machine. The tooltip should contain a
   peer-breakdown line like `r16 52% · peer-hp 48% · XXm total`, with
   the split roughly reflecting where the active keystrokes landed.
6. Let one machine go idle for at least `peerUnreachableWindowSeconds`
   while the other stays active. The idle machine's peer row drops out
   of the tooltip on the active machine within that window.

## Expected observations

- Breaks fire once per cycle, not twice (once per host).
- Neither machine prematurely schedules a break because the other was
  being used on the KVM — this was the pre-fusion bug.
- Tooltip split matches intuition from keystroke distribution.
- Replaying the same cycle with `peerFusionEnabled=false` on both
  machines should reproduce the pre-fusion behavior: each machine
  schedules its own break independently based only on its own idle.

## Recording

Note the following per run for future regression reference:

- `smallEvery` setting
- Time spent actively on each machine during the cycle
- Which machine fired the break
- Tooltip snapshot at break time
- Any anomalies (duplicate breaks, missed breaks, wrong peer %)
