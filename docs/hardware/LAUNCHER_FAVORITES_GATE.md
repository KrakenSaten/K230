# Launcher favorites and Utilities hardware gate (unit B)

The launcher's three favorites and the Utilities folder (DS §42) on a real
K230, under touch, without a reflash.

**Result: PASS, 2026-09-29, on unit B.** Every step was driven remotely:
touches were injected into the touch controller's evdev node as the GT9895
reports a finger (multitouch slot 0; `tests/hw/touch_slot0_tap.py` with a
settable hold, 0.9 s for a long press), and the results were read from
`doors shell info`, `settings.conf`, `shell.log` and screen captures. A
real finger's long press on the glass is still for the owner to try.

## Build under test

- Unit B ran branch `feat/launcher-favorites-utilities`, commit
  **51c46f9**, in landscape 1232 x 568 (portrait for step 6). The shell
  reported `build=51c46f9` for every step.
- Cross-built with the pinned SDK toolchain: the DRM shell
  (`POCKETOS_DISPLAY=drm`, LVGL from the sysroot) and `make all` with
  `-Werror`, 0 first-party warnings.
- Installed by hand, userspace only (no flash, no services touched):
  `/usr/bin/doors-shell` (stripped, md5 ff84d075), and the portal icons the
  24-app shell draws that the unit's v0.2.1 image lacks:
  `icon-utilities.bin` (md5 5b471e51), `icon-deskbuddy.bin`, `icon-mp3.bin`,
  `icon-video.bin`. `pos-mp3` and `pos-video` were not installed; MP3 and
  Video were not opened.
- Unit B before and after: image v0.2.1 (9dc66c2) carrying shell a680c56
  (md5 82084ab7), no `launcher_favorite` keys. Rollback
  `/root/rollback-favorites/RESTORE.sh`, run at the end: shell a680c56
  again, `settings.conf` byte-identical to before (md5 e59f5383), the four
  icons removed.

Tools: `C:\K230\out\favorites-gate` (env.sh, g1-g9, logs, captures in
`caps/`).

## Steps

| # | Step | Result |
| --- | --- | --- |
| 1 | Launcher opens: 24 apps, three empty favorites (dimmed + and "Add") leading the first line, then CONNECTIONS and WORKSPACE (Utilities, DeskBuddy), then PLAY (Games) and DEVICE; 13 app and folder cells, 2 folders; no `launcher_favorite` key | PASS |
| 2 | Long press on empty slot 1: the picker opens with all 24 apps; the release opens nothing | PASS |
| 3 | Tap RIFT in the picker: slot 1 is RIFT, `launcher_favorite_1=rift`; tap slot 1: RIFT opens | PASS |
| 4 | Long press slot 1 (set): the picker with Clear first, still on the launcher (nothing opened); tap Calculator (an app inside Utilities): slot 1 changes, the key follows; tap slot 1: Calculator opens | PASS |
| 5 | Slot 2 Clock, slot 3 Vision by long press; slot 3's picker offers 22 apps, neither Calculator nor Clock (held by slots 1 and 2); long press slot 1 and Clear: empty, its key removed; long press slot 1 and the back slab: left empty | PASS |
| 6 | Shell restart (`S90doors-shell restart`): the same favorites (empty, Clock, Vision) | PASS |
| 7 | Utilities by touch: Calculator, Calendar, Camera, Clock, Files, Notes, Recorder; no Zabbix; Notes opens from it and comes home to it | PASS |
| 8 | Games by touch: the six games, as before; 2048 opens and comes home to Games | PASS |
| 9 | Tap favorite 2: Clock opens | PASS |
| 10 | Portrait (rotation restart): the same favorites as the first row, above CONNECTIONS; a long press sets slot 1 to Files, which a tap opens; back to landscape with the same three | PASS |
| 11 | Churn: 24 rounds of long press/assign/tap-open/clear with both folders opened and closed: `art.bytes_held` 1,841,920 bytes before and after, fds 11 throughout, shell RSS 14.9 -> 17.0 MB in the first batch (the first RIFT open of that shell run) and +28 kB in the last; one shell pid, no crash file, no ERROR in `shell.log` | PASS |

The log records each change (`launcher: favorite 1 is rift`, `... cleared`,
`favorite 1 picker open, 25 app(s) in 9 column(s), scrolls`, `... picker
closed`). The art held grows by one portal icon for each favorite set and
comes back when it is cleared.
