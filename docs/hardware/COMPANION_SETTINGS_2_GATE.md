# Companion Settings 2.0: hardware gate (Unit B)

**Status: build, image and kit ready (`d189c2c1`, section 11); the tests on
the unit wait for the owner.** Nothing has been installed, started, stopped
or changed on any unit; the kit is staged on Unit B. Unit B was otherwise
only read, over key-only SSH and its serial console, on 2026-11-25. Unit A
and PR #186 are out of scope and must stay untouched.

- **What is under test:** draft PR #190 (`claude/companion-settings-2-aad748`),
  ADR-019 amendment 2. The owner accepted it on 2026-11-25 with its three
  decisions and one condition: an activation that does not finish must not
  leave the gateway on.
- **Build:** PR #188 (PacketShark N1.0) merged on 2026-11-25. Master
  `ff95ab99` is merged into the branch, and the validation build is
  `d189c2c1` (section 11.1).
- **Not to be done without the owner present** (owner, 2026-11-25): changing
  Unit B's password or SSH configuration, switching the gateway on, or
  approving a pairing.
- **Evidence classes** are as in docs/hardware/T-DISPLAY-K230.md: VERIFIED
  (seen on the unit or the image), DOCUMENTED (an earlier report or the
  repository says so), ASSUMED (expected, not yet seen).

## 1. Unit B as found (2026-11-25, read-only)

| Item | State | Class |
|---|---|---|
| Release | Doors 0.5.0, `BUILD_ID=b37b644a`. The install of 2026-11-25 deployed it from the official image onto the 0.3.6 card, which keeps that card's kernel, modules and third-party libraries (`C:\K230-artifacts\unit-b-v0.5.0\REPORT.md`) | VERIFIED / DOCUMENTED |
| Addresses | wlan0 `172.16.5.126`, eth0 `172.16.5.220`, both in 172.16.5.0/24. Host key as recorded for `172.16.5.198` (`-o HostKeyAlias=172.16.5.198`) | VERIFIED |
| SSH | Root signs in from the bench PC with its key only (`BatchMode`, `PreferredAuthentications=publickey`). `authorized_keys` has two keys: SHA256:6bkDkuuN... (pocketos-bench-2026-09-08) and the bench PC's SHA256:0e74RVZz... | VERIFIED |
| sshd | `/etc/ssh/sshd_config`: `PermitRootLogin yes` (line 32), `PasswordAuthentication yes`, `PermitEmptyPasswords no`. `sshd -T`: `permitrootlogin yes`, `persourcepenalties` on (an authentication failure locks out the PC's address for a while). `S50sshd` has `reload`; sshd running | VERIFIED |
| Root password | `/etc/passwd` field `x`; root's `/etc/shadow` field **empty**; shadow mode 0600. So today the serial console logs root in with no password, and sshd refuses the empty password | VERIFIED |
| Serial console | `console=ttyS0,115200`; inittab `getty -L console 0 vt100`. On the bench PC, **COM3** (CH342 channel A). A marker written to `/dev/ttyS0` over SSH arrived on COM3, and a carriage return got `Welcome to Buildroot` / `canaan login:`. No credentials were typed | VERIFIED |
| Companion | No `/usr/sbin/gatewayd`, no `/etc/init.d/S70gatewayd`, no `/etc/default/gatewayd`, no gatewayd or companion state under `/var/lib/pocketos`. TCP 47231 is free | VERIFIED |
| Libraries | `/usr/lib`: libcrypt.so.2, libssl.so.3, libcrypto.so.3 (OpenSSL 3.4.1), libcjson.so.1. `/lib/libc.so.6`. `/lib64`, `/lib64/lp64d`, `/usr/lib64` and `/usr/lib64/lp64d` link to them. Copies of these libraries, read with readelf on the PC, define every symbol version that this branch's riscv64 test builds of sysd and gatewayd need: `XCRYPT_2.0` (`crypt_r`, `crypt_gensalt_rn`), `OPENSSL_3.0.0`, `GLIBC_2.27`, `GLIBC_2.33` (`C:\K230-artifacts\companion-settings-2\unitb-libs`) | VERIFIED (for the test builds; check again for the final ones, 4.1) |
| Runtime | sysd, netd, radiod (sx1262), meshcored (`MESHCORED_ENABLE=1`) and doors-shell are running. `/run/pocketos` is 0755 root; its sockets are 0660 root. Root filesystem 9 % used | VERIFIED |
| Not in the image | pos-library, pos-ninjaone, and the GENERAL 365, SEGMENT and INSTANCES models. The kit must leave them alone | DOCUMENTED |
| Old rollback kits | `/root/doors-v0.5.0-b37b644a/RESTORE.sh` exists. It goes back to the state **before** 0.5.0, so it is **not** this gate's rollback | VERIFIED |
| Phone | No Android device on ADB on the bench PC (2026-11-25). The owner's Flip8 (SM-F776B) carries the Companion test build, paired with **Unit A** (`docs/design/companion/M6-STATUS.md`) | VERIFIED / DOCUMENTED |

gatewayd listens on one interface's address only. `S70gatewayd`'s
`GATEWAYD_INTERFACE` is `wlan0` unless the unit's file says otherwise, so the
phone reaches it at 172.16.5.126. eth0 is on the same subnet, and replies have been
seen to leave through wlan0. That is why SSH to .126 works. The phone path has
not been tried (ASSUMED).

## 2. Go / no-go: what must be true before the unit is touched

1. PR #188 is merged, master is merged into PR #190's branch, and the suites in
   3.1 pass on that head. PR #190 stays a draft.
2. **The owner says go for Unit B** in the conversation, for this kit and
   this build. A go for an earlier build does not carry over.
3. The owner picks the phone (8.0) and is at the unit: the Settings taps and
   the password are typed on its own screen.
4. The owner chooses the device password and keeps it somewhere other than
   the unit **before** it is set. Afterwards the serial console asks for it
   (owner decision 2).
5. Key-only SSH is checked again immediately before (5.1), and the serial
   check of section 1 is repeated: marker on COM3, `canaan login:`.
6. The kit verifies on the unit (`sha256sum -c`), and BACKUP.sh has finished
   and been copied to the PC (4.2).

Any "no" stops the gate before INSTALL.sh.

## 3. The validation build (after PR #188)

### 3.1 Synchronize and re-run the focused suites

`git merge origin/master` into `claude/companion-settings-2-aad748` (a merge,
not a rebase: the branch is pushed and has a PR). Then, on the merged head:

- `tests/sysd_companion_test.c`, also under ASan and UBSan; `tests/sysd_companion_lint.sh`
- `make companion-integration-test` (`tests/companion_settings_integration_test.py`, section 6b: the interrupted activation)
- `tests/settings_view_test.c`, `tests/settings_app_test.c`, `tests/settings_lint.sh`
- `tests/initscript_test.sh` (S70gatewayd and its guard), `tests/gatewayd_e2e_test.py`, `tests/gatewayd_lint.sh`
- the packaging suites: `tests/image_contents_test.sh`, `tests/deploy_staging_test.sh`, `tests/package_sync_test.sh`, `tests/notices_test.sh`
- anything PR #188 touched that Companion also touches (shell, Settings, pos-capture's protocol), as the merge shows

Pre-existing failures that are not this PR's, as on 2026-11-25: style_lint
(ui/cards, zabbix), build_deps_test (environment), and the initscript hygiene
check (four fleet/peerd scripts are 0644 on master).

### 3.2 Build

- Run a full `platforms/k230/scripts/build_image.sh` of the merged head
  (k230_pocketos_defconfig, WSL; docs/BUILD_ENVIRONMENT.md). It gives the SD
  image and the target tree. Its last step, `verify_image.sh`, must PASS
  (section 10). A tree from `pocketos-rebuild` alone is not enough: the new
  init script reaches the tree only through a full build.
- **The payload:** the paths of `deploy.sh`'s list, taken from that image's
  root filesystem, as for the 0.5.0 install. The unit then runs exactly what
  the image would ship. Before packing, compare the image's LVGL libraries
  with Unit B's. If they differ, carry them as the 0.5.0 install did.
- **The APK:** `./gradlew :app:testDebugUnitTest :app:assembleDebug` in
  `android/doors-companion` at the same head. Record its sha256 and the
  commit. The app is not changed by PR #190.
- Record the sha256 of every payload file in `new/FILES.sha256`, and the image's
  own sha256.

## 4. The kit: `/root/companion-settings-2-<build>/`

The kit has the same shape as `/root/doors-v0.5.0-b37b644a`
(`C:\K230-artifacts\unit-b-v0.5.0\kit`): `new/` (payload.tar, PATHS.txt,
FILES.sha256, SHA256SUMS), `deploy_unit.sh` (this build's) and
`deploy_unit-0.5.0.sh` (0.5.0's, for RESTORE), BACKUP.sh, INSTALL.sh,
RESTORE.sh and KIT.sha256. It is copied over SSH and checked with
`sha256sum -c`. INSTALL.sh and RESTORE.sh stop netd, so each runs detached
and writes `<name>.log` and `<name>.status`:

```sh
setsid nohup sh INSTALL.sh </dev/null >/dev/null 2>&1 &
```

### 4.1 BACKUP.sh (services up, writes `before/` once, never over it)

As in 0.5.0's, plus what Companion changes outside the payload:

- `files.tar` (every payload path that exists now), `added.txt` (the paths
  the payload adds: `usr/sbin/gatewayd`, `etc/init.d/S70gatewayd`, ...),
  `state.tar` (`/var/lib/pocketos`), `etc.tar` (all of `/etc`), `models.sha256`.
- Exact copies, with sha256 and `stat` (mode, owner) of each: `/etc/shadow`,
  `/etc/passwd`, `/etc/ssh/sshd_config`, `/root/.ssh/authorized_keys`.
- `sshd -T` output; `builds.txt` (sha256 and build stamp of every binary).
- `absent.txt`: each of these that does **not** exist now, so RESTORE
  removes it: `/etc/default/gatewayd`, `/etc/ssh/sshd_config.doors-orig`,
  `/var/lib/pocketos/gatewayd`, `/var/lib/pocketos/companion`, `/etc/.pwd.lock`,
  `/var/lib/pocketos/log/companion.log`, `/var/lib/pocketos/log/gatewayd.log`.
- A check that Unit B's libraries define the symbol versions the **final**
  sysd and gatewayd need, the way section 1 did for the test builds. This is
  run on the PC with the final binaries, before the kit is copied.

`before/` is copied to the PC (`C:\K230-artifacts\companion-settings-2\unit-b-before\`)
and its SHA256SUMS compared, before INSTALL.

### 4.2 INSTALL.sh

1. Verify `new/` and `before/`; refuse if `/etc/default/gatewayd` exists (it
   must not on this unit, section 1).
2. Stop the services in deploy order: S90doors-shell, S70gatewayd (absent:
   skipped), S65meshcored, S60radiod, S55netd, S50sysd. Take
   `state-stopped.tar` once.
3. Unpack the payload with this build's `deploy_unit.sh`. It starts the
   services again: S50sysd, S55netd, S60radiod, S65meshcored, then
   S70gatewayd, then the shell.
4. Check:
   - every installed file matches `new/FILES.sha256`;
   - the Vision models are unchanged;
   - `/etc/shadow`, `/etc/passwd` and `/etc/ssh/sshd_config` are byte for
     byte as in `before/` (the install touches none of them);
   - no `/etc/default/gatewayd`, no gatewayd process, and 47231 not listening:
     S70gatewayd is installed, off;
   - sysd answers (`doors call sysd companion.status`), with `installed:
     true, enabled: false, password_set: false, ssh: "open"`.
5. `install.status` 0, or the number of the failed step.

### 4.3 RESTORE.sh: back to 0.5.0 exactly as found

RESTORE.sh also undoes what Settings did, and does so **in this order**:

1. **The gateway off first**, while its script is there: write
   `GATEWAYD_ENABLE=0`, run `S70gatewayd stop`, then check that no gatewayd
   runs. If `/var/lib/pocketos/companion/activating` exists, it is removed
   after the 0 is written.
2. Stop the services (S90 ... S50).
3. **`/etc/shadow` back**: from `before/`, through a temporary file, fsync,
   rename, mode 0600 root:root, then check its sha256. `/etc/passwd` back
   only if it differs. This puts back root's empty field, as found.
4. **`/etc/ssh/sshd_config` back** the same way. `sshd -t` must pass before
   anything else happens. If it fails, the hardened file stays and the step
   is reported.
5. 0.5.0's `deploy_unit.sh` unpacks `before/files.tar`. Then the paths in
   `added.txt` and `absent.txt` are removed: the gateway binary and script,
   its switch, **the unit's gateway key and its phones**, the activation
   record, the lock file and the two logs (copied into the kit first, as
   evidence).
6. `--state` also puts `/var/lib/pocketos` back from `state-stopped.tar`.
7. `S50sshd reload` last. The services start (0.5.0's order).
8. Check:
   - the shell, sysd and every binary are the ones in `before/builds.txt`;
   - shadow, passwd and sshd_config have the sha256 recorded in `before/`;
   - `sshd -T` says `permitrootlogin yes` again;
   - no gatewayd runs, 47231 is free, and none of the `absent.txt` paths exist.

After a restore, key SSH from the PC and the serial login are checked as in
section 1. The phone's pairing is then for a unit key that no longer exists.
The app shows another key at the address and stops; forgetting the unit there
is the owner's step.

## 5. SSH and serial recovery

### 5.1 SSH

- Every connection uses the key only: `-o BatchMode=yes
  -o PasswordAuthentication=no -o KbdInteractiveAuthentication=no
  -o PreferredAuthentications=publickey -o HostKeyAlias=172.16.5.198`.
  No password is ever tried from the PC, and no raw TCP connection is made to
  port 22. Either would trigger sshd's per-source penalty and lock out the
  PC's address.
- Hardening keeps key login working. `prohibit-password` refuses passwords
  for root and allows keys, and the PC's key is in `authorized_keys`
  (VERIFIED). Before the **first** Settings action that hardens SSH (setting
  the password, 7.2), open a second SSH session and leave it at a shell. A
  running session survives `S50sshd reload` (DOCUMENTED, OpenSSH; ASSUMED on
  B). Within a minute of the action, a **new** key-only connection must work.
  If it does not, use the open session to run RESTORE.sh. Without one, use
  the serial console (5.2).
- sysd itself refuses to set the password or switch the gateway on if
  `sshd -t -f` fails or the reload fails, and changes nothing else then.

### 5.2 Serial console

- COM3, 115200 8N1, CH342 channel A on the bench PC (VERIFIED 2026-11-25).
  Any terminal will do (PuTTY, or PowerShell's `System.IO.Ports.SerialPort`).
  Type long lines in short pieces: long pastes broke on 2026-10-19.
- **Before** the device password is set, `root` logs in with no password.
  **After** it, the serial console asks for the device password (owner
  decision 2). That is why it is recorded beforehand (section 2, item 4).
- From the serial shell the kit runs as from SSH. The serial console does not
  depend on netd, so RESTORE.sh can also run in the foreground there.
- Last resorts, neither tried on Unit B (ASSUMED):
  - take out the microSD card and put the backed-up `/etc/shadow` and
    `/etc/ssh/sshd_config` back from another Linux machine;
  - write the official 0.5.0 image to the card and put the unit's state back
    from the PC's copy of `before/`.

### 5.3 Stop conditions: restore at once

- INSTALL.sh status is not 0.
- sysd, netd, meshcored or the shell is not running a minute after INSTALL,
  or pos-supervise reports a crash loop.
- Key-only SSH fails after any step.
- sshd is not running.
- A gatewayd runs while `companion.status` says `enabled: false`.
- The gateway listens on any address other than wlan0's.
- The panel shows nothing, or Settings does not open.
- Anything the owner calls wrong.

## 6. Order of the session

1. Section 2, then BACKUP.sh, and the copy to the PC.
2. INSTALL.sh, and its checks (4.2).
3. The checks on the unit (section 7) and on the phone (section 8), with the
   owner at the unit.
4. At the end, the owner chooses: keep the build on Unit B (record it in the
   unit's notes; the kit stays for a later restore), or RESTORE.sh.

## 7. Checks on the unit (Settings > Companion, by the owner's finger)

A production shell takes no screenshots, so the panel's look is the owner's
photos and eyes (`docs/design/companion/M6-STATUS.md`). Every state below is
also read back over SSH (`doors call sysd companion.status`, `doors call
gatewayd gateway.status`, `companion.log`, `gatewayd.log`). Results are
PASS, FAIL or NOT TESTED; nothing is claimed from the simulator.

| # | Check | Expected |
|---|---|---|
| U1 | Settings > Companion with no password, in portrait and landscape, and at Large text | Gateway off; the switch cannot be tapped; DEVICE PASSWORD `Not set`, with a note that setting it makes SSH key-only; PAIR A PHONE says why it is not available |
| U2 | Set the device password (8 to 64 characters), with both fields; first try two different ones and a short one | Mismatch and length are said under the fields and nothing is sent. Then "Setting the password...", then "Password set. The serial console asks for it from now on." Over SSH: `sshd_config` now has `PermitRootLogin prohibit-password`, `sshd_config.doors-orig` holds the original, `sshd -T` says `prohibit-password`, and root's shadow field begins with `$6$` (print the first 3 characters only). A **new** key-only SSH connection works. The serial login now asks for a password; the owner checks it at COM3 |
| U3 | Gateway ON | STARTING, then On. `enabled` and `running` are true, gatewayd runs under pos-supervise, 47231 listens on 172.16.5.126 only (`netstat -ltn`), `/var/lib/pocketos/companion/activating` is absent afterwards, and the unit's key is shown |
| U4 | Gateway OFF, then ON again | `GATEWAYD_ENABLE=0` written, gatewayd gone, 47231 closed. Back on, the unit's key is the same |
| U5 | Reboot with the gateway on | It comes back on, with the same key, phones, password and hardened SSH |
| U6 | Reboot with the gateway off | It stays off (S70 log: disabled) |
| U7 | **An interrupted activation at boot** (the owner's condition). With the gateway on, over SSH: write a record from another boot (`printf 'b007-0000 4242\n' > /var/lib/pocketos/companion/activating`, mode 0600), then reboot | At boot S70gatewayd starts nothing ("not started: turning it on was interrupted") although the switch says 1. sysd then writes `GATEWAYD_ENABLE=0`, removes the record, and reports `action: recover`, `error: activation_interrupted`. Settings shows Off and why. Turning it on again works |
| U8 | **An interrupted activation, sysd killed.** With the gateway off, tap ON and kill sysd (`kill -9`) at once, from an SSH session already open (only if the activation still runs; it usually takes well under a second). Otherwise write a record of this boot naming a process group that is gone, and kill sysd | pos-supervise starts sysd again. sysd ends the record's process group if it is alive, writes 0, removes the record and reports `activation_interrupted`. No gatewayd is left running, and Settings says it is off |
| U9 | An upgrade: run INSTALL.sh's unpack again with the same payload (this build's `deploy_unit.sh`, services stopped and started by it) | The switch, the unit's key, the phones and their scopes, the password and the hardened sshd_config are all as before |
| U10 | The words of a failure: switch ON while gatewayd's state is unusable is not tried on the unit. The simulator screenshots stand for those texts (`docs/apps/settings/companion/`) | NOT TESTED on hardware, by design |

U7 and U8 put test files in place by hand. The kit's RESTORE and its
`absent.txt` cover them.

## 8. Android Companion validation

### 8.0 The phone: the owner's choice before the session

The app holds **one** paired unit (the Android README, "Not yet"), and the
Flip8's app is paired with Unit A. The options are:

- **(a)** A second Android phone (min SDK 29) with the test APK. Unit A is
  not touched in any way. Recommended.
- **(b)** On the Flip8: save the installed APK (`adb pull`, as on
  2026-11-24), install this build, forget Unit A in the app, and pair with
  Unit B. Unit A itself is not changed, but it keeps a paired entry for a
  phone that no longer holds its pin. Going back to Unit A later means
  pairing again on Unit A. This needs the owner's explicit yes, because it
  changes how Unit A is used.
- **(c)** The emulator on the bench PC (`C:\Android\toolchain`), on the
  unit's subnet. It covers the protocol, but it is not a physical phone and
  does not count for the "real Android" item.

Record the phone model, its Android version, the APK sha256 and the commit.
Phone screenshots: `adb exec-out screencap -p`.

### 8.1 Pairing and trust

| # | Check | Expected |
|---|---|---|
| A1 | PAIR A PHONE: the QR code in portrait and landscape; the phone scans it | The phone connects; the unit shows "A phone is connecting...", then the phone's name and key (`XXXX XXXX XXXX XXXX`, mono) |
| A2 | **Fingerprint confirmation**: compare the unit's key with the phone's "This phone's key"; first leave "Same key on the phone" at NO | ALLOW cannot be tapped while it is NO. At YES, ALLOW pairs with the shown scopes (Status, RIFT, sending; PacketShark and Vision off, as PAIRING.md §4 says). The phone reads `paired`. On the unit's list: the phone's name, key, `connected` |
| A3 | REFUSE on a second attempt (after Remove, or with phone (c)) | The phone is told it was refused. Nothing is added to the list |
| A4 | **Expiry**: show a code and let it run out (120 s, `Closes in` counts down) | "the code expired", SHOW A NEW CODE. A phone that scans the old code afterwards is not let in |
| A5 | **Cancel**: show a code, CANCEL (also Back and Home) | The window closes (`gateway.pair_cancel`; `pair_closed` in `gateway.status`). The old code no longer works |
| A6 | Optional, with two phones: both scan; the unit confirms one key | `pair_decide` carries the confirmed key. If the other phone is the one waiting, nothing is decided for it ("not the phone that was confirmed"), and the page says so |

### 8.2 Permissions and revocation

| # | Check | Expected |
|---|---|---|
| A7 | On the phone's page on the unit: switch Send RIFT off, then on; then Vision, PacketShark figures and PacketShark control | The connected phone is held to the new set from its next request (`gateway.scopes`). Its rooms say "Not granted" for what was taken. The dependencies hold (sending brings reading; control brings figures) |
| A8 | **Revocation**: REMOVE PHONE (confirm, CANCEL is the accent) | The phone shows that the unit removed it (`gateway.revoked`), and the unit's list no longer has it. Reconnecting does not get in. Pairing again is a new pairing |

### 8.3 Persistence

| # | Check | Expected |
|---|---|---|
| A9 | Reboot the unit while paired | The phone reconnects by itself, with the same pin and no new pairing; scopes as before |
| A10 | Gateway OFF with the phone connected, then ON | The phone treats it as a lost session, not a removal, and reconnects when it is on |
| A11 | After U9 (an upgrade) | As A9 |

### 8.4 RIFT, Vision, PacketShark through the gateway

| # | Check | Expected |
|---|---|---|
| A12 | **RIFT reading**: nodes, channels, history sync; a live channel message from other nodes on the air | The history fills. A live message arrives with the unit's journal. Run, journal and gap marks appear only where they belong |
| A13 | **RIFT sending**: **only with the owner's go for each transmission**, to a channel or node the owner names, because every send is a radio transmission | Accepted, then on the air or delivered, in the app's words. Nothing is sent twice |
| A14 | **Vision**: give `vision` on the unit; open Vision on the unit | The preview starts on the phone with its boxes. STOP on the unit ends it, ALLOW brings it back, leaving Vision ends it, and taking `vision` away ends it at once. Unit B has GENERAL 365, SEGMENT and INSTANCES (Unit A had only R0 on 2026-11-24) |
| A15 | **PacketShark**: give the figures (`capture_view`), then control (`capture_control`). eth0 is cabled on Unit B | **The first run of the app's PacketShark against a real gatewayd** (the Android README says it has run only against a contract-played gateway). The capture's state and figures appear while a capture runs, and the unit marks a companion that reads. HOLD and STOP from the phone (STOP asks twice). A start request raises the sheet on the unit and starts nothing until START is tapped there. Refusing, or 60 s, says refused or timed out. Nothing of the capture is stored on the phone |

### 8.5 After the session

The Flip8, if used (b), keeps the build it was given unless the owner wants
`installed-before.apk` back. Unit B: keep or restore (section 6). The phone's
pairing to a restored Unit B is stale (4.3).

## 9. Evidence

The PC folder `C:\K230-artifacts\companion-settings-2\` holds:

- `unit-b-before/`, the kit, and its logs and status files;
- `companion.log`, `gatewayd.log` and `sysd.log` from the unit;
- `companion.status` and `gateway.status` at each step;
- phone screenshots, the owner's photos of the panel, and the APK and image
  sha256.

The results go into this document, section 11, as PASS, FAIL or NOT TESTED,
with the time of each.

## 10. Image packaging verification before a release

### 10.1 Automatic: `verify_image.sh` (the end of `build_image.sh`)

On the release image it must PASS. The checks Companion added or relies on:

- **gatewayd is a whole service**: `/usr/sbin/gatewayd` and
  `/etc/init.d/S70gatewayd` are both there and executable, the script names
  it as its DAEMON, and its build stamp is the release's
  (`tools/release/check_rootfs.sh`).
- **No per-unit switch**: no `/etc/default/gatewayd` (and no
  `/etc/default/meshcored`).
- **No unit's Companion state** (new): no `/var/lib/pocketos/gatewayd` (a
  shipped unit key would make every unit the same unit), no
  `/var/lib/pocketos/companion`, no `/etc/ssh/sshd_config.doors-orig`.
- **No root password** (new): root's `/etc/shadow` field is empty or locked.
  A shipped hash would be every unit's password.
- **SSH key-only**: `sshd_config` judged `hardened` by
  `tools/release/sshd_root_login.sh`, the rule sysd uses.
- **Every library a binary asks for** (new): each Doors binary's NEEDED
  entries (readelf) are in `/lib` or `/usr/lib`, through links if need be,
  and not empty. That covers libcrypt.so.2 for sysd, and libssl.so.3,
  libcrypto.so.3 and libcjson.so.1 for gatewayd.

Run on the official 0.5.0 image on 2026-11-25 (the branch's gate, an image
from before the amendment): it read the libraries of all six of its binaries
as present. It found no gatewayd state and no root password. It failed on
the two rules 0.5.0 predates (vendor sshd_config; no gatewayd). That is the
expected result, and it shows that the new checks read a real image.

### 10.2 By hand, on the release image (debugfs, read-only)

- [ ] `/etc/init.d/S70gatewayd` is 0755 and has the interrupted-activation
      guard (`grep activating`)
- [ ] `readelf -V` of the image's sysd and gatewayd against the image's own
      libraries: every needed version is defined (as section 1 did with
      Unit B's)
- [ ] `strings` of the image's sysd and gatewayd: no test hooks
      (`SYSD_TEST_COMPANION_ROOT`; gatewayd's `--test-listen`, `--test-port`,
      `--test-shadow`)
- [ ] `THIRD_PARTY_NOTICES.txt` lists libxcrypt and OpenSSL (`tests/notices_test.sh`)
- [ ] The image's sha256 is recorded with the release notes

### 10.3 On hardware: a fresh-card test

The same shape as `V0.4.0_FRESH_CARD_TEST.md`, on **a spare microSD card**:
the unit's own card is taken out and kept unchanged. It needs the owner's
go and the owner's choice of unit (Unit A is out of scope now).

- Flash, then the first boot.
- Settings > Companion: off; password `Not set`; `sshd -T`
  `prohibit-password`; S70gatewayd starts nothing; 47231 closed; no
  `/etc/default/gatewayd`.
- Set the password, switch on, pair, reboot: all of it holds.
- Then deploy the next build over it (an upgrade): all of it still holds.

## 11. Results

### 11.1 Remote preparation, 2026-11-25 (no change to any unit)

PR #188 merged at 19:28Z. Master `ff95ab99` was merged into the branch
(`6b47a760`): the only conflict was the Makefile's `clean` line, and both
sides were kept. Two fixes followed, `469dd1dd` (a test) and `d189c2c1` (the
notices). On the owner's instruction, nothing was installed on Unit B, and
its password, SSH configuration, gateway and pairing were not touched.

**The validation build: `d189c2c1361e0c47e10bd21e49f84262e7b7726b`.**
`apply_to_sdk.sh`, `build_image.sh` and `verify_image.sh` were run in WSL:
- the pinned vendor tree (BSP `bb831ab`, SDK `22d02c6`), clean;
- RadioLib, ggwave, MeshCore and Crypto at their pins, clean;
- the R0 model with the pinned hash;
- `Source worktree : clean`, no override.

The first build, of `6636908d`, was a full one from an empty SDK output, and
took 44 min with one stall: the Kendryte mirror accepted the strace download
and sent nothing. I fetched strace upstream and checked it against
Buildroot's hash, and `~/.wgetrc` now sets `read_timeout = 60`. The builds
of `469dd1dd` and `d189c2c1` reused that output: `apply_to_sdk.sh` removes
the Doors package's build directory, so Doors was rebuilt and the image
reassembled. The kernel and device trees are carried over unchanged.

| Artefact | sha256 |
|---|---|
| `sysimage-sdcard.img` (763,363,328 bytes) | `ed8fc013df5f4981560ea63f1294f34ebfe9cb9aca216e5ffa1ce8b8fd6f7377` |
| `doors-0.5.0-tdisplay-k230-d189c2c1.img.gz` (122,317,610 bytes; VERSION is still 0.5.0, so this is not a release) | `896f4ccf87a432bd0adeb64163547aef854457a28b80e74382e4fe99053589a2` |
| `/usr/sbin/sysd`, `/usr/sbin/gatewayd`, `/usr/bin/doors-shell` in the image (first 16 hex digits) | `d50b867f5848f236`, `eac3b235868889d0`, `131460491af446da` |
| Validation APK (`android/doors-companion`, debug; the app's sources are identical from `6636908d` to `d189c2c1`) | `ebadca8eb3e5c5b1f32290c61f83b1a075779d930793dae1faa432c8788a1e10` |

Everything is in `C:\K230-artifacts\companion-settings-2\`: `image-d189c2c1\`
(image, BUILD_INFO, the gate's output, the packaging check, provenance),
`kit-d189c2c1\` and the APK.

**Image gate (`verify_image.sh`): PASS.** It found:
- the boot files present;
- one shell service, on by default, and no vendor launcher;
- SSH root by key only (`PermitRootLogin prohibit-password`);
- no `/var/lib/pocketos/gatewayd` or `/companion`, no `sshd_config.doors-orig`;
- root's shadow field empty;
- R0 as pinned, and the five refused models absent;
- every service whole and from build `d189c2c1`, gatewayd and S70gatewayd included;
- no `/etc/default/gatewayd` or `/etc/default/meshcored`;
- every library each Doors binary asks for, in the image.

**Packaging, by hand (10.2): PASS.**
- `S70gatewayd` is 0755, carries the interrupted-activation guard, and is
  off unless the unit says otherwise.
- The shipped sysd and gatewayd carry no test-hook strings.
- `readelf -V`: every symbol version sysd and gatewayd need is defined by
  the image's own libraries **and** by Unit B's (`XCRYPT_2.0`,
  `OPENSSL_3.0.0` from OpenSSL 3.4.1, `GLIBC_2.27`, `GLIBC_2.33`).
- `img.gz` unpacks to the image byte for byte.
- One finding, fixed in `d189c2c1`: the notices' list of the Buildroot
  libraries Doors loads named neither libxcrypt (sysd) nor libpcap
  (pos-capture).

**The focused suites on the merged head:** all PASS.
- sysd companion 100 (plus radio 43); gatewayd e2e 200, capture e2e 90;
  companion integration 48; settings_view 300, settings_app 1987; pocketipc.
- `make packetshark-test`.
- image_contents, deploy_staging, package_sync, notices, license_audit,
  build_provenance, splash_image; the settings, gatewayd, sysd_companion and
  packetshark lints.
- `initscript_test`: only the failure already on master (four fleet/peerd
  scripts at 0644).

Found and fixed in `469dd1dd`: the 89-failure `initscript_test` run. The
guard test killed a background job that had not yet exec'd, which ran the
test's EXIT trap and removed its root. Six runs in a row passed after the fix.

**The kit:** `/root/companion-settings-2-d189c2c1/` on Unit B, staged and
checked there (`KIT.sha256`, `new/SHA256SUMS`).
- Payload: deploy.sh's 36 entries from the build's target tree (99 paths,
  96 files, 28.7 MB, the same bytes as the image's), plus the build's LVGL
  libraries, which differ in their bytes from Unit B's.
- RESTORE uses 0.5.0's own `deploy_unit.sh`, sha256 `d3ec0378...` as in the
  0.5.0 kit.
- BACKUP.sh was dry-run once on Unit B: 97 paths saved, gatewayd and
  S70gatewayd recorded as added, 9 Companion paths recorded as absent, exact
  copies of shadow (0600) and sshd_config, and `before/` verifies. It was then
  deleted, so tomorrow's BACKUP is fresh. Unit B's `sshd_config` is 0755 (the
  vendor's) and sysd writes 0644, so the kit puts back and checks mode and
  owner as well as content.
- INSTALL, RESTORE and RESET_COMPANION have not run yet: they change the unit.

**The emulator:** `doors-companion-readme` (Android 16), headless, has the
APK and reaches 172.16.5.126 and .220. Nothing was tried against a gateway,
because none runs.

### 11.2 With the owner present

The install, the rollback test, the checks over SSH after the password is
set (S1 to S8), and the hands-on tests are in
`COMPANION_SETTINGS_2_MANUAL_CHECKLIST.md`. Their results go here.
