# RIFT repeater control - hardware gate (unit B, PARTIAL: CLOCK defect found and fixed)

**Build on unit B since 2026-10-05 16:17 UTC:** doors-shell and meshcored
`b79e297` (md5 `bcfba16a…` / `b2bddbb2…`). The rollback below still holds the
binaries found before the first deploy.

**Before that, 15:28-16:17 UTC:** doors-shell and meshcored
`6fe7db0` (riscv64, md5 `018903a8…` / `cf85c25f…`), hot-swapped from a clean
clone's cross-build. **As found before:** doors-shell md5 `16522b79…`,
meshcored `2fcc0f9a…` (build "unknown", release 0.3.0 / BUILD_ID 6b26f06 on
the card) - kept in `/root/rollback-rift-repeater/` with copies of
meshcored's state files; `RESTORE.sh` there puts both binaries back. After
the deploy: meshcored online, 298 nodes, no session, no scan, radiod
`tx_packets` 0 before and after; RIFT opens with REPEATERS 0-HOP and SCAN
0-HOP enabled. Nothing was transmitted.

Branch `feat/rift-repeater-control`. Prepared 2026-10-05. **Nothing here has
been on a radio.** Host evidence: `tests/meshcored_repeater_test.cpp` (a
whole meshcored runtime against a test repeater built from upstream's
`simple_repeater` handlers, plain and ASan/UBSan), `tests/rift_ipc_test.c`,
`tests/rift_model_test.c`, `tests/rift_app_test.c`; screens in
`docs/apps/RIFT.md`, "Repeater control".

Not run because no repeater password and no repeater set aside for testing
were given. Every step that transmits is marked **TX**; each is one packet at
the unit's configured power, and none is resent on a timer.

**Write the unit's build at the top of the results** (doors-shell and
meshcored md5 and BUILD_ID) before step 1, as every gate sheet does.

## What changes on the unit

| Binary | Why |
| --- | --- |
| `/usr/sbin/meshcored` | `mesh.discover`, `mesh.discovered`, `mesh.remote_login` / `_request` / `_cli` / `_logout` / `_session`; events `mesh.discover`, `mesh.remote` |
| `/usr/bin/doors-shell` | RIFT: ACTIVITY's REPEATERS 0-HOP panel, the repeater page |

Both must change together: a RIFT from this branch on an older meshcored
says "This meshcored has no repeater discovery" and offers no login. State
files are not touched; the session is memory only. Rollback: the two binaries
as found.

## Needed from the owner

- A repeater the owner controls, in direct range of the unit, and its
  **admin** password (and, for step 6, its guest password if one is set).
  Typed on the unit only; never put in a script, a log or this sheet.
- The unit's clock set (NTP) before step 4: the repeater refuses a login
  whose timestamp is not newer than the last it saw from this node.

## Steps

1. **No TX.** Open RIFT. ACTIVITY shows REPEATERS 0-HOP, SCAN 0-HOP enabled,
   "NOT SCANNED YET". `doors call meshcored mesh.remote_session` →
   `{"active":false}`.
2. **TX (one zero-hop control packet).** Press SCAN 0-HOP. Expect SCANNING
   with a 30 s countdown; the repeater in range listed within that time with
   RSSI/SNR. Press SCAN again during the window: nothing happens (meshcored
   log: one transmit). Expect only repeaters in direct range; a repeater
   known to be reachable only through another must **not** be listed.
   Record the list against `mesh.nodes` `advert_hops` for the same nodes.
3. **No TX.** Open the repeater's row. The page shows its key, LATEST SCAN,
   both SNRs and HELD (if its advert has been heard; if NOT HELD, LOGIN is
   disabled - wait for its advert and record that).
4. **TX.** Log in with a **wrong** password. Expect "No answer to the login"
   after 20-30 s and the field empty. Nothing else should change on the
   repeater.
5. **TX.** Log in with the admin password. Expect "Logged in as admin" and
   the repeater's clock. The password field is empty; check
   `grep -ri <password> /var/lib/pocketos/log/` finds nothing.
6. **TX, read-only.** STATUS, NEIGHBOURS, VERSION in turn: each answered
   within its wait, values plausible against the repeater's own display or
   the MeshCore app; NEIGHBOURS names held nodes. Press a second button while
   one waits: nothing is sent.
7. **TX, read-only.** VER, CLOCK, NEIGHBORS: each answered in the transcript.
8. **No TX.** Type `reboot`, SEND: a confirmation; CANCEL. Type `erase`,
   SEND: "Not sent". Type `get guest.password`, SEND: "Not sent". The
   meshcored log shows no command transmitted for any of the three.
   **Do not confirm `reboot`, `set ...` or any radio change.**
9. **No TX.** LOGOUT: the page shows Not logged in; `mesh.remote_session`
   inactive. Log in again (TX), then press Home: `mesh.remote_session`
   inactive. Log in again (TX), then `/etc/init.d/S65meshcored restart`:
   RIFT reconnects with no session shown.
10. Optional, guest password: login says "Logged in as guest"; STATUS works;
    the console says commands are for admin only and sends nothing.

## Known before the run

- A wrong password and a repeater that did not hear the login look the same
  (upstream sends nothing for either).
- A repeater that answers with the 48-byte RepeaterStats may show receive
  airtime and receive errors read from the reply's zero padding (the reply is
  padded to the AES block; nothing in it says how long the struct was). Note
  firmware versions where the values look like 0.
- Logout is local: the repeater keeps this node in its access list.

## Findings, 2026-10-05 (unit B, repeater BREVDUE `5997dbcb…`, direct, RSSI -31)

Run by the owner, then reproduced: SCAN 0-HOP, open, login worked; **after
CLOCK every repeater button stayed grey and no time appeared.**

- **Defect (fixed in `b79e297`):** meshcored sized a direct request's wait
  from MeshCore's packed path byte. This mesh uses 2-byte path hashes, so a
  zero-hop route is `0x40`, which was read as 65 hops: the login waited
  146.9 s and CLOCK 104.5 s ("WAITING UP TO 105 S"), all buttons disabled.
  It did end, as a timeout, after that. Upstream counts `path_len & 63`.
- **CLOCK itself is correct:** sent as `TXT_TYPE_CLI_DATA` "clock", answered
  by CLI data with no tag, matched by sender. Over the air it was answered
  and matched in about 2 s (by flood when the repeater had no route back,
  and direct afterwards); one press got no answer at all (radiod RX capture:
  nothing from the repeater), which is ordinary loss of a single packet and
  ends as a timeout.
- **After the fix, on air:** login (blank password, as this node was already
  in BREVDUE's access list) answered as admin; CLOCK "COMMAND ANSWERED" in
  about 1.5 s with every button usable at once; STATUS straight after it
  answered (battery 4.30 V, uptime 51m 30s); a direct command's wait is
  21.3 s. Logged out afterwards; no session left.
- **Seen, not changed:** a session ended by another client (`doors call
  meshcored mesh.remote_logout`) raises no event, so an open RIFT page goes
  on showing it logged in until its next request is refused.
- **Steps still to run:** wrong password (step 4) and the confirmation and
  refusal checks of step 8 with the owner present; step 9's restart.
