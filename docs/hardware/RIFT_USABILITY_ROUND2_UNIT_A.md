# RIFT usability round 2 (PR #124) on unit A, 2026-11-04

Unit A: CH342 console on COM5 of the build PC, wlan0 `88:3b:dc:b7:9e:c7`. The owner
asked for this unit to be used (it is the one showing the ACTIVITY warning); unit B
(`…9e:d3`) was not inspected. Three kinds of evidence are kept apart below:
what the device itself showed, what the owner observed, and host tests.

Nothing was transmitted, no setting was changed, and meshcored and radiod were
not restarted (pids 388 and 267 before and after).

## 1. Device evidence

### Before the install (read-only diagnosis)

- **Builds:** `doors-shell` Doors 0.3.9 build `ccc2c04a` (draft #119), sha256
  `15842a91…`. Its RIFT model, window and IPC code is identical to master's at the
  time. `meshcored` 0.3.8 `bc889b0d`, up 5.2 days.
- **RIFT session:** open since 2026-11-01 21:13, kept in the background since.
- **meshcored store** (aggregated on the device, no text, names or keys):
  - 22 direct messages, ids 477–504, received at monotonic 195,421–197,665 s, before
    that session started (~199,500 s);
  - 64 channel messages, ids 1124–1188; the ring is full.
  - The same 22 direct ids at 18:11 and 18:19 UTC.
- **`app_rx` = 0** for the whole meshcored run: no `mesh.app` event reached RIFT.
- **Diagnostics that weren't available:** `shell.screenshot` fails on this unit
  ("screenshot failed", code 4), and RIFT logs no counter. So the counter itself
  is the owner's reading (section 2).

### The install

- **Built from:** the PR head `081530fc` (the commit the host tests ran on;
  master's later commits are README only). The source tree diffed identical to
  the commit. Shell only, cross-built against the existing SDK output (DRM,
  sysroot LVGL), `BUILD_ID` `081530fc`; sha256 `6ce9752b…`. No clean SDK build.
- **Runtime:**
  - every `NEEDED` library is present on the unit;
  - `LD_BIND_NOW` symbol resolution against the unit's own libraries: 0
    unresolved;
  - `/etc/default/doors-shell` and `/var/lib/pocketos` were not touched.
- **Transfer:** gzip and base64 over the serial console, verified by sha256.
- **Backup and rollback:**
  - a hard link to the replaced binary: `/root/rollback-rift-p124/backup/doors-shell`,
    hash checked against `OLD.sha256`;
  - **rollback:** `sh /root/rollback-rift-p124/RESTORE.sh`. It stops the shell only,
    puts the backup back, starts it, and prints `RESTORE: OK` after `sha256sum -c`
    passes and exactly one shell is running.
- **After the install:**
  - `/usr/bin/doors-shell: OK` (sha256 check);
  - one `doors-shell` under one `pos-supervise`;
  - `shell.info` reports 0.3.9 / `081530fc`, with an app list identical to the
    one before the install.
  - The shell log since start has one warning, the pre-existing "favorite 2
    holds meshscope", which the old shell logged too.
  - RIFT opened with `rift: open, meshcored connected`.
  - The display runs at rotation 270 (landscape), with the keyboard base present.
- **Not shown by the device:** that stored DMs appear and that refreshes keep the
  counter at 0. RIFT exposes neither, and a counter of 0 right after a restart
  proves nothing about the full-window case (a new session starts at 0).

### Incidents during the install (no lasting effect)

- **Console logout.** The first transfer's receiver (`cat`) never started. The
  base64 lines reached the root shell as over-long, harmless words, and the
  closing Ctrl-D logged the console out. The owner logged back in.
  - Fix: the receiver is now `head -n <lines>`, which needs no Ctrl-D, and a
    probe line goes first, so the sender stops if the shell answers it.
- **Truncated command.** A long one-line install command hit the console's line
  editor limit and waited at a continuation prompt. It was cancelled with
  Ctrl-C before anything ran, and replaced by short-line scripts sent as files
  (checksums matched).

## 2. Owner observations

- **ACTIVITY count before the install:** 52,184, then 53,350, then 53,372.
  - 53,350 → 53,372 is +22, one 30 s refresh;
  - 52,184 → 53,350 is +1,166 = 53 × 22;
  - +22 is the number of stored direct messages that RIFT's full window had
    evicted (section 1).
- **After the install:** "no auto capital on physical keyboard. Landscape mode
  is not better."
- **Not reported:** DMs listed in COMMS, the top tabs, the emoji button and
  picker, and channel names without "?". They aren't recorded here as passed or
  failed.

The capital finding matches the design of #105: only the portrait touch
keyboard is offered an automatic capital, and a physical keyboard types what its
own Shift and Caps say (`docs/apps/RIFT.md`, "Automatic capitals"). PR #124
does not change that. On unit A, which is used in landscape with the keyboard
base, a new message therefore never starts with an automatic capital.

That is the cause of issue 2 on this unit, and it is not fixed by PR #124. A
physical-keyboard offer (a pending capital for the first letter, cancelled by a
tap of Shift, as on the touch keyboard) is a design change to #105, for the
owner to decide.

## 3. Host tests (PR head `081530fc`, from the PR)

- `rift_app_test`, carbon theme: 2282 checks, 0 failures.
  - Includes the capital after SEND, after DONE and reopened, on a DM and a
    channel, against the scripted meshcored.
  - Includes the strip and emoji-button geometry at Small, Medium and Large in
    both orientations.
  - Under Neon, 2 "faces alike" checks fail, as they do on master.
- The full-window case (`test_channel_flood_keeps_direct` in `rift_comms_test`)
  is the regression for the counter. The device cannot show it right after an
  install.
- Other RIFT model, IPC, emoji and keyboard tests, `rift_lint.sh` and
  `style_lint.sh`: pass.

## Result

- The PR shell is installed on unit A and left there; nothing essential failed.
- The counter cause on this unit is the window eviction and re-offer that the PR
  fixes (host-tested). Confirming it on the device needs the window to fill
  again, which this check did not wait for.
- Issue 2 on this unit is the physical keyboard, which PR #124 does not
  address.
