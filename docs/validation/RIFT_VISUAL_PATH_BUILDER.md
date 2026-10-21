# RIFT Visual Path Builder — UI validation record

Date: 2026-10-21
Branch: `feat/rift-visual-path-builder`, stacked on PR #100
(`feat/meshcored-explicit-path-p0`).
Base: `4baac72ff235f41ee86b7381ed1277e6f3090d8a` (PR #100 head on that date).
Status: **SIMULATOR AND SCRIPTED-SERVICE VALIDATED, K230 CROSS-BUILT. NOT
RUN ON A UNIT. NO RADIO WAS USED.**

Design reference: the Visual Path Builder proposal (PR #99,
`docs/apps/rift/VISUAL_PATH_BUILDER.md` on that branch). Service contract:
`docs/api/mesh.md` as changed by PR #100 (`mesh.info` `capabilities`,
`mesh.path_preview`, `mesh.send` `via`). User-facing description:
[docs/apps/RIFT.md, "Chosen route"](../apps/RIFT.md#chosen-route-the-route-builder).

## Two kinds of evidence, kept apart

| | what | class | where |
| --- | --- | --- | --- |
| PR #100 | meshcored host tests, K230 cross-build, no-TX checks on unit B, and a zero-relay explicit DIRECT message to a stock MeshCore companion delivered and ACKed **on the air** | VERIFIED (hardware, 2026-10-21) | [RIFT_EXPLICIT_PATH_P0.md](RIFT_EXPLICIT_PATH_P0.md) |
| this branch | RIFT's UI and client against the **scripted** `tests/fake-meshcored`, in the LVGL simulator and on the host; K230 cross-build | VERIFIED (simulator / mock only) | below |
| neither | forwarding through one or more physical repeaters on a route chosen in RIFT | **NOT VERIFIED** | "One bench session", below |

Nothing in this branch was installed on a unit, and no test here can
transmit: the scripted service has no radio, and it logs every `mesh.send`
and `mesh.path_preview` it is asked for so the tests read what would have
gone on the air from the request itself.

## What was validated (host and simulator, 2026-10-21)

Host (WSL Ubuntu 22.04, gcc 11.4, `-Wall -Wextra -Werror`):

| test | result |
| --- | --- |
| `tests/rift_route_test` (new) | 100 checks, 0 failures |
| `tests/rift_ipc_test` (47 new explicit-route checks) | 373 checks, 0 failures |
| `tests/rift_model_test` | 370, 0 |
| `tests/rift_comms_test` | 345, 0 |
| `tests/rift_format_test` | 145, 0 |
| `tests/rift_reliability_test` | 94, 0 |
| `tests/rift_rxlog_test` | 71, 0 |
| `tests/rift_notify_test` | 156, 0 |
| `tests/rift_tiles_test` | 72, 0 |
| `tests/rift_lint.sh` (9 new rules) | 0 failures |

Simulator (SDL dummy driver, LVGL 59dc7e4, theme carbon):

| test | result |
| --- | --- |
| `rift_app_test`, route builder session alone (`RIFT_APP_TEST_ONLY=route`) | 106 checks, 0 failures, 23 screenshots (after `3c04c2f`) |
| `rift_app_test`, every session | 1980 checks, 0 failures |
| `tests/rift_shell_test.sh` (real shell, scripted service, both orientations, no service, kept session, same pixels twice) | 0 failures |

Covered by those, against the scripted service:

- SET ROUTE from the conversation, recipient named and fixed; MAP becomes
  the builder.
- Repeaters tapped on their markers join in order by whole key; a chat node,
  the recipient and a duplicate are refused with the reason on screen.
- RELAYS lists the eligible repeaters, including one with no location,
  which is added there, listed, and never drawn or bridged by a line.
- Numbers on markers, numbers in the list and the chain text agree; lines
  only between consecutive placed waypoints; the recipient's leg dashed
  until checked.
- Cut from a picked row (`REMOVE 2-3`), UNDO, CLEAR.
- The service's refusal shown in its words; USE ROUTE disabled.
- CANCEL: back to the conversation, the typed text intact, nothing staged,
  nothing sent.
- USE ROUTE, then SEND: the service's send log holds exactly
  `<recipient>|<text>|via=<relay 1>,<relay 2>` once; the composer empties
  only after acceptance; the message reads `CHOSEN ROUTE, 2 RELAYS` and is
  not offered RESEND.
- Two presses in one pass: one send.
- AUTO: the next send has no `via` member at all, as before.
- The empty route: `via=` (an empty array), never AUTO.
- A route refused at SEND: `Not sent: route refused: …`, nothing logged as
  sent, the text kept.
- Previews alone produce no `mesh.send` (method log).
- An edit while a preview is out: the late answer is not applied.
- The service going away during a check: `nothing was sent`, not "unknown";
  during a draft preview: unanswered, not valid.
- An older service (no capabilities): never asked for a preview, never
  given `via`, nothing sent; SET ROUTE disabled with the reason. A restart
  from a capable service into an older one keeps the staged route refused,
  not sent AUTO.
- Small, Medium and Large in portrait and landscape, and HDMI 1280x720: no
  caption or button word clipped, the panel inside the body, actions in
  reach.
- A route chosen for one conversation is kept when another conversation
  gets one (a stage per conversation, eight at once; a ninth USE ROUTE is
  refused with the reason, never replacing another's).
- Existing behaviour: the landscape console still shows 14 whole messages
  (DS §37.2 check), MAP with 1000 nodes still 25 objects, every earlier
  `rift_app_test` session unchanged.

Also run, and not part of this branch:

- **Sanitizers:** `rift_route_test`, `rift_ipc_test`, `rift_reliability_test`
  and `rift_format_test` under ASan + UBSan: 0 failures, no report.
- **Neon (PR #98, on master, not in PR #100's base):** this branch merged
  with master `dc9108a` in a throwaway worktree (one conflict in
  `tests/rift_app_test.c`, both sides adding a function before `main`;
  resolved by keeping both) and the route session run with
  `RIFT_THEME=neon`: 100 checks, 0 failures. The list rows were moved from
  the button fill to a slab because of what that showed. When this PR is
  retargeted to master, expect that same one-hunk conflict.
- **Bounded review:** one review pass of the diff found two defects, both
  fixed in `45153ce`: a route staged on one conversation was dropped when
  another conversation staged one (its next SEND would have gone AUTO), and
  a failed `mesh.send` write after the check was reported as "may have been
  sent" and dropped the connection twice.

### Screenshots

Rendered by `rift_app_test` with `RIFT_SHOTS_DIR`; the PR shows them. Node
names, keys and coordinates are synthetic fixtures.

`route-portrait-comms-auto`, `route-portrait-selected`,
`route-portrait-repeaters`, `route-portrait-unlocated`,
`route-portrait-refused`, `route-portrait-staged`, `route-portrait-sent`,
`route-portrait-send-refused`, `route-portrait-disconnected`,
`route-portrait-unsupported`, `route-{portrait,landscape}-{small,medium,large}`,
`route-{portrait,landscape}-comms-staged`, `route-hdmi-1280x720`.

## K230 cross-build (2026-10-21)

- Source: `git archive 29cff06` (no `.git`, `BUILD_ID` written); the same build of `6f9d555` and `45153ce` also passed.
- Toolchain: Xuantie-900 gcc 14.1.1 (V3.0.2 B-20250410) through the SDK's
  Buildroot `toolchainfile.cmake`; BSP `bb831ab`, SDK `22d02c6` (the pins
  in docs/BUILD_ENVIRONMENT.md).
- `-DPOCKETOS_DISPLAY=drm -DPOCKETOS_LVGL_MODE=sysroot`, sysroot the copy
  staged for the Neon kit (`liblvgl.so.9.5.0` sha256 `4f761c44…`,
  `LV_USE_QRCODE 1`). The SDK's own sysroot LVGL is built without the QR
  widget, which `apps/system` needs on this base; that is not this
  branch's change.
- Result: `pocketos-shell` RISC-V PIE, **0 warnings**, rc 0, 46 route
  symbols linked; sha256 `d668bd2b7d986721225591ea1004109e7bd204867e0ad0d2860f6dd01e55e944`.
- Not packaged, not installed, not run on a unit.

## Hardware claims

- No pin, register or device path is touched by this branch. RIFT talks
  only to meshcored over pocketipc.
- Map positions are the nodes' **claimed** advert locations (DOCUMENTED,
  docs/api/mesh.md); this device's adverts carry none (VERIFIED earlier for
  MAP). The builder never invents a position.
- That a route checked by `mesh.path_preview` will be forwarded by the
  chosen repeaters is **ASSUMED** until the bench session below.

## Hardware checks

### 1. Owner UI check on unit B (non-transmitting) - DONE 2026-10-21

**Build and kit.** Local integration commit `ec38999` (master `dc9108a`
with Neon + this PR at `a045e49` + PR #100 `4baac72`; not pushed),
cross-built 0 warnings, stripped shell `990cae23…`. Every LVGL symbol it
uses is exported by the installed Neon LVGL `2c4fd81b…`, so it ran on it.
Kit `rift-vpb-ec38999` copied to `/root` with the owner's approval, every
file hash-checked on the unit. `rift-start.sh` checks passed: installed
shell `86d7b017…` (Neon, f398ecb), LVGL `2c4fd81b…`, running
`/usr/sbin/meshcored` `c0af8f32…` (PR #100 candidate); settings backed up
(`538f31ae…`: neon, landscape, Large). The test shell (pid 3337) owned
the panel; meshcored advertised both capabilities `true`.

**Owner's observations (on the panel, by hand):**

- COMMS had no M5 conversation: meshcored keeps no history across a
  restart and held 0 messages. Opened from NODES → `Mstr_m5` → MESSAGE.
  DETAILS showed `ROUTE AUTO` with SET ROUTE enabled.
- SET ROUTE opened MAP as the ROUTE BUILDER: `TO Mstr_m5`,
  `EXPLICIT DIRECT · NO RELAY`, `CHECKED BY MESHCORED · NOTHING SENT`.
- Final check: tapped SET ROUTE, then CANCEL (finger on both buttons).

**Automated evidence (keyboard-base keys injected with `shell.key`
through the shell's own key path; captures by `shell.action screenshot`,
read from the panel; no `shell.tap` exists in the device build):**

| capture | input | what the panel showed |
| --- | --- | --- |
| 00 | - | builder for Mstr_m5, no relay, `0 PATH BYTES · NOTHING SENT`, basemap on |
| 04 | Right, Enter, Down, Enter, Left | `2 RELAYS`: 1 SE Skalle-Per (numbered on its marker), 2 NO-1263 Prinsdal (no location: in the list only, said on the map); **REFUSED BY MESHCORED: a relay hash matches another known repeater** (1-byte hashes); USE ROUTE disabled |
| 05 | Backspace | `1 RELAY`, `CHECKED BY MESHCORED · 1 PATH BYTE · NOTHING SENT`, USE ROUTE enabled |
| 06 | Right, Down ×6, Enter, Left | 1 SE Skalle-Per → 2 Nordpuls Solar: line 1→2 on the map, chain `YOU › SE Skalle-Per › Nordpuls Solar › Mstr_m5`, `2 PATH BYTES · NOTHING SENT` (no line to the M5: it has no location) |
| 07 | Backspace ×2 | back to no relay; UNDO and CLEAR disabled |
| 08 | Esc | builder closed, conversation `ROUTE AUTO`, nothing staged; composer kept its 🙂 |
| 09 | letters | known draft `🙂rute` in the composer (no Enter) |
| 10 | (owner: SET ROUTE, CANCEL) | conversation `ROUTE AUTO`, composer still `🙂rute` |

Transmit counters (service read, not UI evidence): `tx_submitted 2`,
`tx_ok 2`, `tx_failed 0`, `tx_unknown 0`, `sent_flood 1`, `sent_direct 1`
before, during and after - unchanged. No SEND, no Enter in the composer,
no advert.

**Restore.** `rift-restore.sh` first ended `rc=1`: its settings check ran
`sha256sum -c` from the wrong directory and skipped the copy (the live
file was already byte-identical to the backup). Fixed in the kit
(`d6a3cf98…`), re-run: **`RESTORE: OK`** - installed Neon shell
`86d7b017…` supervised on the panel, settings `538f31ae…`, meshcored the
same file and process (pid 388, never restarted).

**Not covered by this check:** map marker taps, CLEAR and cut-from-here
by finger (no tap automation on the device; covered in the simulator),
turning the unit and changing text size (simulator only).

**Found on the device, fixed in `3c04c2f` (simulator-verified, K230
cross-built 0 warnings; not re-run on the unit):**

1. Landscape, Large text: the list started below the visible part of the
   panel body, and the arrow-key cursor did not scroll its row into view
   (`lv_obj_scroll_to_view` on the row scrolled only its non-scrolling
   parent). Now scrolled through the body; switching lists brings the
   list's first row into view. `rift_app_test` checks it by key.
2. An emoji in a node name (`SE Skalle-Per 🐵`) showed as a box in the
   route chain line: the builder's labels and the conversation's route
   line now use RIFT's colour-emoji font. MAP's own marker names, drawn on
   its canvas and not changed by this PR, still show such an emoji as a
   box.

### 2. Physical RF acceptance (separate) - PENDING, not waived

The backend evidence is PR #100's on-air pass: an explicit zero-relay
DIRECT message delivered to and acknowledged by the M5 companion
([RIFT_EXPLICIT_PATH_P0.md](RIFT_EXPLICIT_PATH_P0.md)). It is reused, not
repeated. What remains is only what that pass cannot show:

- **The UI's own send, on the device** (uncovered: the composer path into
  `rift_ipc_send_explicit` against the real service). USE DIRECT to the M5
  in direct range, SEND once: delivered and ACKed, the message reading
  `CHOSEN ROUTE, NO RELAY`, no RESEND offered. One transmission.
- **Forwarding with a non-empty `via`** (uncovered: no relay has carried a
  route chosen here). Only when one MeshCore repeater is available: a
  one-relay route to the M5, SEND once, the repeater forwarding it once.
  The M5 does not need to be out of direct range for this; the record
  says whether the delivered copy came through the relay. Not a
  prerequisite for the UI or for merging the UI.

Two repeaters, reversed routes and an out-of-range placement are not
required.
