# Companion Settings 2.0: manual tests at the unit (Unit B)

The tests that need the owner's hands, eyes and phone. Everything that could
be done remotely is in `COMPANION_SETTINGS_2_GATE.md` section 11. Mark each
PASS, FAIL or NOT TESTED, with the time. Unit A stays untouched.

**Where Unit B starts:** unchanged, Doors 0.5.0 `b37b644a`. On the owner's
instruction of 2026-11-25, nothing was installed overnight, and root's
password, SSH, the gateway and pairing were left alone. The validation build
is `d189c2c1` (image gate PASS, gate 11.1). Its kit is staged and verified
on the unit at `/root/companion-settings-2-d189c2c1/`. The APK is
`C:\K230-artifacts\companion-settings-2\doors-companion-debug-d189c2c1.apk`
(sha256 `ebadca8e...`).

## 0. Install, with you there (about 5 minutes, over SSH)

- [ ] Key-only SSH works, and COM3 shows `canaan login:` (no login).
- [ ] Unit B still says `BUILD_ID=b37b644a`; in the kit,
      `sha256sum -c KIT.sha256` passes again.
- [ ] Run `sh BACKUP.sh` and copy `before/` to the PC.
- [ ] Run `INSTALL.sh`, detached; `install.status` must be 0.
- [ ] `companion.status`: installed, off, no password, SSH open. No gatewayd
      runs, and 47231 is closed.
- [ ] The rollback, exercised once before anything else: run `RESTORE.sh`
      (status 0: binaries, shadow, sshd and paths back as found), then
      `INSTALL.sh` again. About 3 minutes; it proves the way back works.

## Before the tests at the unit

- [ ] Choose the device password and write it down outside the unit. From
      step 2 on, the serial console asks for it.
- [ ] The Flip8: its Companion app is paired with Unit A, and the app holds
      one unit. Forgetting Unit A in the app is your decision (gate 8.0, b).
      Keep the installed APK first: `adb pull`, as on 2026-11-24.
- [ ] Install the validation APK (above).
- [ ] Before each step that changes something on the unit, a key-only SSH
      connection to 172.16.5.126 works.

## At the unit

| # | Do | Expect |
|---|---|---|
| 1 | Settings > Companion in portrait, landscape and Large text | Off; the switch can't be tapped; password `Not set`, with a note that setting it makes SSH key-only; PAIR A PHONE says why not. Photograph each page |
| 2 | SET PASSWORD: first two different entries, then a short one, then the real password twice | The errors show under the fields. Then "Password set. The serial console asks for it from now on." |
| 3 | Gateway ON by finger | STARTING, then On, with the unit's key |
| 4 | PAIR A PHONE, then scan the QR with the Flip8, in portrait and again in landscape | The phone connects; the unit shows its name and key |
| 5 | **Fingerprint**: leave "Same key on the phone" at NO, try ALLOW; compare the unit's key with the phone's "This phone's key"; YES, ALLOW | ALLOW is dead at NO. Paired with Status, RIFT, sending; the list shows the phone `connected` |
| 6 | A phone's page: switch Send RIFT off and on; switch on Vision, PacketShark figures and PacketShark control | The phone follows from its next request; "Not granted" where a scope is taken; the dependencies hold |
| 7 | RIFT on the phone: history and live channel traffic. **Sending only with your own go, to where you choose** | Messages arrive and the marks are right. A send you make: Accepted, then on the air or delivered |
| 8 | Vision on the unit, with `vision` granted | Picture and boxes on the phone. STOP and ALLOW on the unit work; leaving Vision ends it; switching `vision` off ends it at once |
| 9 | PacketShark: a capture on eth0. From the phone: HOLD, STOP (asks twice), and a start request | Figures on the phone, and the unit marks a reader. The request raises the sheet on the unit, and only START there starts it. Refusing, or 60 s, is said on the phone. **The app's first run against a real unit** |
| 10 | Reboot the unit while paired | The phone reconnects by itself, same pin and scopes |
| 11 | Gateway OFF, then ON, with the phone open | "Lost", not "removed"; it reconnects |
| 12 | REMOVE PHONE (confirm) | The phone says the unit removed it; it can't get back in without pairing again |
| 13 | Optional: show a code and let it expire (120 s); show one and CANCEL; REFUSE a phone | Each ends in words; an old code no longer works |
| 14 | At COM3 (optional): `canaan login:`, then root | It asks for the device password |

## Over SSH while you are there (after step 2, no hands needed)

These were held back overnight because they need a device password or the
gateway on. Results go in section 11 of the gate.

| # | Check | Expect |
|---|---|---|
| S1 | After step 2: `sshd_config`, `sshd_config.doors-orig`, `sshd -T`, root's shadow field (first 3 characters only), a **new** key-only SSH connection | `prohibit-password`; the original kept; `$6$`; key login works |
| S2 | After step 3: `netstat -ltn`; `gateway.status` | Nothing listens with no phone and no window open (`no_clients`). During a pairing window: 172.16.5.126:47231 only, never .220 or 0.0.0.0 |
| S3 | During a window, from the PC: `openssl s_client -tls1_2`; TLS 1.3 without a client key | Both refused; the window stays open |
| S4 | The emulator (bench PC) pairs during a window | The unit shows the emulator's name and key, equal to the app's "This phone's key". **You** decide on the unit: REFUSE, or let it expire |
| S5 | Reboot with the gateway on | On again, with the same unit key and phones; the password and SSH as set |
| S6 | Interrupted activation at boot (gate U7): with the gateway on, a record from another boot, then a reboot | S70 starts nothing; sysd switches it off (`activation_interrupted`); Settings says why |
| S7 | Interrupted activation, sysd killed (gate U8): a record of this boot with a process group that is gone, then sysd killed | pos-supervise restarts sysd; it is switched off; no gatewayd is left |
| S8 | An in-place upgrade: `INSTALL.sh` again | The switch, unit key, phones, password and `sshd_config` byte for byte as before |

## After

- [ ] Choose: keep the build on Unit B, or `RESET_COMPANION.sh` (keeps the
      build, Companion back to first use), or `RESTORE.sh` (back to 0.5.0).
      Run detached; the kit's README says how.
- [ ] The Flip8: keep the validation APK, or reinstall the saved one. Its
      pairing with a restored Unit B is stale; forget it in the app.
- [ ] Send the photos and phone screenshots for the gate's results.
