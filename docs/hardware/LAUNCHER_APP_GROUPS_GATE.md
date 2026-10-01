# Launcher app groups and System in Settings: hardware gate

**Unit B carries this branch now** (left so the owner can look at it):
`doors-shell` from `feat/launcher-app-groups` `11ab0d9` (stripped, md5
`a0642593…`; `doors shell info` `build` `11ab0d9`, 26 apps, `system`
`page_of: settings`) and the one new art file
`/usr/share/doors/ui/icon-apps.bin` (md5 `53871b03…`). Userspace is
master `749f4f1` (`/etc/doors-release`), untouched. `settings.conf` is
byte-identical to how it was found (the owner's `text_size=large`,
favorites Video and RIFT, rotation `automatic`). Rotation automatic gives
landscape: the keyboard base is attached.

**Rollback:** `/root/rollback-appgroups/RESTORE.sh` puts back what the unit
carried before the gate - `doors-shell` `89297c0` (text size, md5
`6f5115e4…`), no `icon-apps.bin`, `settings.conf` as found - and restarts
the shell service. Not run.

**The gate below ran on `11ab0d9`** on unit B, 2026-10-01 18:57-19:20 UTC,
by Claude over SSH (Wi-Fi, 192.168.10.187): a hot swap of the shell, no
flash. Fingers were injected on the panel's touch device (multitouch slot
0, as the GT9895 reports one: taps, drags and long presses), keys through
the keyboard base's own key path (`shell.key`), Back and Home as
`shell.action` (no key of the base carries Back yet, DS §44.2), pictures by
kmsgrab from the panel. Tooling, logs and captures:
`C:\K230\out\app-groups-gate` (`g*`, `t.sh`, `g.sh`, `act.sh`, `keys.sh`,
`gate_touch.py`; `logs/`, `caps/`).

## Results: PASS (all seventeen steps)

| # | Result |
| --- | --- |
| G1 | **PASS.** Portrait at Large: three rows - favorites (Video, RIFT, Add); Terminal, RIFT, Browser, Settings; Apps, Utilities, Games. No System cell. No scroll: Lock and Controls on screen. 124 px cells, every place drawn from its art, layout audit clean (`caps/g1-portrait-large.png`). The Apps cell reads as one of the family (four squares, the Apps colour) |
| G2 | **PASS.** Landscape at Large: all ten cells on one row, centred, 108 px cells, labels whole, nothing under the status cluster or in a corner, no scroll, audit clean (`g1-landscape-large.png`) |
| G3 | **PASS.** By finger, both orientations: Apps holds DeskBuddy, MP3, Photo, Radio, Video, Vision, Wave, Zabbix in that order (landscape one row, portrait two rows of four, labels whole at Large). Photo, MP3 and Vision opened from it came home to Apps; Apps' back slab to the launcher's page (`g3-apps-*.png`) |
| G4 | **PASS.** Utilities: Clock, Calendar, Calculator, Notes, Files, Recorder, Camera; Calculator opened from it came home to it (`g4-utilities-*.png`) |
| G5 | **PASS.** Games: Fleet, Radar, Timber, Solitaire, Blackjack, 2048; Radar opened from it came home to it (`g5-games-*.png`) |
| G6 | **PASS.** A long press (900 ms) on the empty slot 3 opened "Favorite 3": 23 apps from every folder, no System, not Video or RIFT (held by slots 1 and 2), labels whole at Large (`g6-picker.png`). Photo chosen: the slot shows it, a tap opens Photo directly, its back slab lands on the launcher's page, not Apps. A shell restart kept all three. Slot 3 cleared afterwards |
| G7 | **PASS.** The owner's own favorite Video, stored before the change and now in Apps, still shows and opens Video directly; Back lands on the launcher's page. A favorite holding System (set through `shell.favorite`, as one stored before would be - the unit had none): shown, a tap opens System, Back is Settings, then home |
| G8 | **PASS.** Settings' last panel, SYSTEM, under Appearance (landscape: the right column), reached by finger scrolls; "System" with "About this device, status, diagnostics, restart and power" wrapped whole at Large and a chevron; clear of the corners in portrait (`g8-settings-*-system-row.png`) |
| G9 | **PASS.** A tap on the row opens System in both orientations; the header reads "System"; its content as before - vitals, storage, network, services, identity, Diagnostics, Restart, Power off (`g9-system-*.png`, `g12-system-actions.png`). Restart and Power off were not touched |
| G10 | **PASS.** System's back slab: Settings, at the top of its page (`g10-back-to-settings.png`) |
| G11 | **PASS.** Settings' back slab: the launcher's page |
| G12 | **PASS.** Back from System's Diagnostics closes Diagnostics (System stays); Back from System: Settings; from Settings: home; at home: `noop`; from an open folder: the launcher's page. **Note:** System shows "STALE 7s" for a moment after Diagnostics closes - System's own rule (its status poll pauses while Diagnostics is open), not this branch |
| G13 | **PASS.** Home from System, and from MP3 opened inside Apps: the launcher's own page, no folder open |
| G14 | **PASS.** Through the base's key path: F2 Settings, F8 Terminal, F9 RIFT, LILYGO Terminal, F1 home. Typing into Terminal was not exercised (unchanged by the branch) |
| G15 | **PASS.** Controls' About DOORS row and its Power button both open System; Back from it is Settings |
| G16 | **PASS.** Small, Medium and Large, portrait and landscape: the launcher's page, Apps, Utilities, Games, the picker and Settings with nothing clipped, overlapping or sizeless (`shell.audit`), the launcher never scrolls, Back from System is Settings at every size (`g16-*.png`). System's one "truncated" at every size is its Model value ("Canaan CanMV-K23…") shortened by its key/value row - DS §46.8's known limit, not this branch. The unit was returned to the owner's Large |
| G17 | **PASS.** 20 Settings -> System -> back -> back rounds by finger (scroll, tap the row, back slab or Back alternately) and 21 folder open/close rounds by finger: the same shell process throughout (no restart), RSS 15,280 kB before and after, 13 fds, `art.bytes_held` 1,676,032 before and after, no crash report, no `ERROR` in the shell log since the deploy |

Two rotations (to portrait and back to automatic) were in-place restarts as
designed (DS §21.4), each unlocked over IPC after it came back.
