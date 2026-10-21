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
| `rift_app_test`, route builder session alone (`RIFT_APP_TEST_ONLY=route`) | 100 checks, 0 failures, 19 screenshots |
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

## One bench session (the remaining hardware checks)

One sitting, one shell build of this branch on unit B (the PR #100
meshcored already installed there), the M5 companion as recipient, and two
MeshCore repeaters (R1, R2) placed so that the companion is **not** in
direct range of unit B but R1 → R2 → companion is. Record each with
`mesh.messages`, the service log, the repeaters' own logs or a third
receiver, and a DRM capture of the panel. Restore the installed shell
afterwards.

1. **Install and open** the shell build; RIFT shows `ROUTE AUTO` and SET
   ROUTE enabled (capabilities read from the real meshcored). Opening the
   builder, tapping, CLEAR/UNDO and previews transmit nothing: `tx_ok`
   unchanged, no `mesh.activity` `tx`.
2. **Two-relay route:** select R1 then R2 on the map (or RELAYS), USE
   ROUTE, SEND. The service log shows one explicit DIRECT with the two
   relay hashes in that order; R1 and R2 each forward once; the companion
   receives the text and its ACK returns: the message turns `DELIVERED`
   with `CHOSEN ROUTE, 2 RELAYS`.
3. **Order matters:** the same route reversed (R2, R1): sent as asked; the
   result (likely no ACK) is recorded, not interpreted as a fault of the UI.
4. **Zero relays from the UI:** USE DIRECT to the companion in direct range:
   delivered and ACKed (repeats PR #100's on-air result through the new UI).
5. **Refusal on the device:** a route through a node the service will
   refuse (forget R2 in NODES, then SEND on a staged route through it):
   `Not sent: route refused …`, `tx_ok` unchanged, the text kept.
6. **AUTO unchanged:** AUTO, SEND: behaves exactly as before this branch
   (flood or learned direct, ACK as usual).
7. **No RESEND:** on an explicit message that ended NO ACK, the actions
   offer no RESEND.
8. **Screens on the glass:** portrait and landscape at Small / Medium /
   Large on the panel, and the HDMI output at 1280x720, with a finger:
   marker taps land on the intended repeater; nothing clipped.
9. **Service restart mid-send:** restart meshcored with the unit's service
   manager between SEND and its answer: the note says nothing was sent (the
   route was still being checked) or that it may or may not have been (the
   send was written), and nothing is sent again on AUTO.
