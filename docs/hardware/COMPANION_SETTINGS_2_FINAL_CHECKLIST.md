# Companion Settings 2.0: final checks before merge

This is the remainder of `COMPANION_SETTINGS_2_GATE.md`. Section 11.2 holds
everything that already passed on 2026-11-27, and that evidence is reused
here, not repeated. Mark each check PASS or FAIL with the time (UTC).

**The state these checks start from:**
- Unit B runs build `d189c2c1`, the tested build, and needs no reinstall.
  This PR's code is the same at the current head `0d1174d5` (gate 11.3).
- Unit B is on the Flip8's hotspot (10.129.188.199), paired with the Flip8,
  whose phone key starts `C5D4 DCF0 CC0D 6E4D`.
- The Flip8's app is APK `ec50c2c1…`, which is the head's app code.
- The hotspot path is VERIFIED (PR #193). Skullerud Park cannot be used:
  phone and unit could not reach each other there, cause not proven.

**Rules:**
- Only the owner touches Unit B's screen and decides each step.
- No RIFT send is part of this checklist.

## F0. Build host, before the hardware (not a repeat: the merged head is new)

This is the one binary in which master's #191 meets this PR's Settings
pages. The Android app needs nothing: its tree is identical to PR #193's,
built and tested there.

- [x] **PASS, 2026-11-27, 13:13 to 13:22Z,** at `83c7e8a1` (the code of
  `0d1174d5`):
  - **Where:** a fresh `git archive` in WSL (`~/work/cs2-f0`), with the same
    toolchain, sysroot, host dependencies and LVGL as the 2026-11-25 runs.
    Logs are in `C:\K230-artifacts\companion-settings-2\f0-83c7e8a1\`.
  - `make packetshark-test`: rc 0. Every suite has 0 failures, `packetshark_lint` included.
  - `tests/settings_view_test`: 300 checks, 0 failures; built with
    `-Wall -Wextra`, no warnings.
  - Simulator build of `settings_app_test`, `packetshark_app_test` and
    `pocketos-shell`: rc 0.
  - `settings_app_test`: 1987 checks, 0 failures.
  - `tests/packetshark_shell_test.sh` (the PacketShark app test and the real
    shell; #191 extended it): 0 failures.
  - riscv64 cross-build of doors-shell (DRM, sysroot LVGL, configured as for
    `d189c2c1`): rc 0. The binary carries build id `83c7e8a1`, sha256
    `54d4b85c…a034`.
  - **Warnings:** none in a file changed since `d189c2c1`, and none in
    `apps/settings` or `apps/packetshark`'s changed files. The only warning in
    a file of this PR is `tests/settings_app_test.c:1718`
    (`-Wformat-truncation`, test code). It was already there on 2026-11-25.

## F1. Reboot with a paired phone (gate A9 and U5)

Why it is mandatory: S5 rebooted with no phone paired, so nothing listened
afterwards (`no_clients`). A phone reconnecting through the new S70gatewayd
has not been seen.

- [ ] With the gateway on and the app Open, reboot Unit B. Let it rejoin the
  hotspot; that took 2 min 25 s once today.
- [ ] Expected:
  - the gateway is on, with the same unit key (`7810 B639 41A1 1FE9`);
  - the Flip8 is in the unit's list with the same permissions;
  - the app is Open again **by itself**: no new pairing, same pin ("Try now"
    may shorten the back-off);
  - over SSH or serial, if reachable: `gateway.status` `listening: true` on
    the hotspot address.

## F2. Take a permission away from a connected phone (gate A7)

Why it is mandatory: it is a security control of this PR's page. Only giving
a permission has been seen on hardware (gate 11.2).

- [ ] On the Flip8's page on Unit B, switch **PacketShark control** off while
  the app is Open.
- [ ] Expected: the app's PacketShark room shows "Capture control: Not
  granted" from its next request, and no start request is taken.
- [ ] Switch it back on: "Granted" again.

## F3. Removal (gate A8)

Why it is mandatory: it is the revocation control of this PR's page.

- [ ] REMOVE PHONE on the Flip8's page, and confirm.
- [ ] Expected:
  - the app says **"Removed: The unit says this phone was removed"**;
  - the Flip8 is no longer in the unit's list;
  - "Try now" does not get the phone back in.

## F4. Pairing again, with the key confirmation's NO (gate A2)

Why it is mandatory: it is the pairing's safeguard. Today only the YES path
was recorded. This step also restores the pairing after F3.

- [ ] PAIR A PHONE, then scan with the Flip8.
- [ ] Leave **"Same key on the phone"** at NO and tap ALLOW: nothing pairs.
- [ ] Compare the unit's key with the app's "This phone's key", set YES, then
  ALLOW.
- [ ] Expected:
  - paired with Device status, Read RIFT and Send RIFT messages;
  - PacketShark and Vision off;
  - the list shows the Flip8 `connected`, and the app is Open.

## Not mandatory for this merge

Each of these is optional for the merge, for the reason given:
- A10, gateway OFF/ON with the phone open:
  - switching off ends gatewayd and closes 47231 (U4, VERIFIED);
  - the app treats a lost session as "AWAY" and reconnects (VERIFIED today).
- A3, refusing; A6, two phones; an old code after cancel or expiry:
  - cancel and expiry closed the window (VERIFIED);
  - `pair_decide` is bound to the confirmed key (host-tested).
- U1, the panel in portrait, landscape and Large, and U2's error texts: no
  security effect; the simulator screenshots stand.
- A11 and U9 with a phone: the upgrade kept the key, state, password and
  sshd_config (VERIFIED).
- A12 to A15 beyond what passed today (RIFT history and live traffic, Vision,
  PacketShark HOLD, STOP and figures): this PR does not change those
  features. RIFT reading and a capture start request already went through
  Unit B's gateway (VERIFIED). A13, each send, is only ever on the owner's
  word.
- A password SSH login attempt by the owner: `sshd -T` says root logs in by
  key only (VERIFIED).
- The fresh-card test (gate 10.3) belongs before a **release**.

## After

- [ ] Write F0 to F4 into gate section 11.2.
- [ ] Unit B: keep the build, or run `RESET_COMPANION.sh` or `RESTORE.sh`
  (gate 6), and move it back to its usual network. Both are the owner's
  choice.
