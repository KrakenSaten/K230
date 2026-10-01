# Launcher app groups and System in Settings: hardware gate

**Not run.** Written with the change on `feat/launcher-app-groups` (DS §47);
no unit was touched while it was made. Fill the build identity in first.

**Unit and build:** unit ___, `doors shell info` `build` = ______ (expect the
branch tip), `apps` = 26 with `system` `page_of: settings`. Deploy: the
`doors-shell` binary and `/usr/share/doors/ui/icon-apps.bin` (the one new art
file); nothing else changes on the device. Keep the shell and `settings.conf`
the unit carries now for the rollback, and note them here.

The structure, the folders' contents, the favorites' resolution, Back's
targets and the layout audit at Small, Medium and Large were all checked in
the simulator (`tests/launcher_groups_shell_test.sh` and the suites in DS
§47.6). What only the panel shows is below.

| # | Step | Expect | Result |
| --- | --- | --- | --- |
| G1 | Unlock; look at the launcher in **portrait** | Three rows under the time: three favorites; Terminal, RIFT, Browser, Settings; Apps, Utilities, Games. No System cell anywhere. **No scroll**: the footer (Lock, Controls) is on screen without a swipe. The Apps cell's icon reads as its own (the package's Apps glyph, Apps colour) | |
| G2 | Turn to **landscape** (Settings > Rotation, or attach the keyboard base) | All ten cells on one row, left to right, centred; labels whole; no scroll; nothing under the status cluster or in a rounded corner | |
| G3 | Tap **Apps** | DeskBuddy, MP3, Photo, Radio, Video, Vision, Wave, Zabbix, in that order (portrait: two rows of four; landscape: one row). Open one, come home with the back slab: back in Apps. Back slab: the launcher's page, the focus mark on Apps after a key | |
| G4 | Tap **Utilities** | Clock, Calendar, Calculator, Notes, Files, Recorder, Camera. Open Calculator, back: in Utilities | |
| G5 | Tap **Games** | Fleet, Radar, Timber, Solitaire, Blackjack, 2048. Open one, back: in Games | |
| G6 | **Favorites**: long-press slot 1, choose Photo (from inside Apps); tap the slot | The picker offers apps from every folder and **no System**. The slot shows Photo; a tap opens Photo directly; its back slab lands on the launcher's page (not in Apps). A shell restart keeps the slot | |
| G7 | Favorites kept from before (if the unit has one): note what `launcher_favorite_1..3` held before the deploy | Each still shows its app and opens it; one that held System opens System, whose back slab goes to Settings | |
| G8 | Open **Settings**; scroll to the end | Last panel SYSTEM, one row "System" with its line and a chevron, under Appearance (landscape: the right column) | |
| G9 | Tap the **System** row | System Status opens; the header says "System"; everything on it as before (identity, vitals, storage, network, services, radio, Diagnostics, Restart, Power off) | |
| G10 | System's **back slab** | Settings (at the top of its page) | |
| G11 | Settings' **back slab** | The launcher's page | |
| G12 | Global **Back** (`doors call shell shell.action action=back` - no key of the base carries Back yet, DS §44.2): from System; from System's Diagnostics; from Settings; from an open folder | Diagnostics closes first; then System -> Settings; Settings -> launcher; folder -> launcher's page | |
| G13 | **Home** (`action=home`, or the base's Home key) from System, from inside a folder app | The launcher's own page every time | |
| G14 | The base's **Settings**, **Terminal**, **RIFT** keys (F-row, DS §44.1) | Each opens its app; Terminal still takes raw input | |
| G15 | Controls: **About DOORS**, then **Power** | Both open System; Back from it is Settings | |
| G16 | Settings > Appearance > Text size **Medium**, then **Large**: look at the launcher (both orientations if practical), each folder, the favorites' picker, Settings' System row, System | Labels whole, nothing clipped or overlapping, no new scroll on the launcher's page; then back to the owner's size | |
| G17 | **Repeated use**: 20 rounds of Settings -> System -> back -> back, and 20 folder open/close rounds | No shell restart (`pidof doors-shell` unchanged), RSS flat after the first rounds, `doors shell info` `art.bytes_held` back to its value at the launcher's page, no new crash report, no `ERROR` in the shell log | |

**Restore** what the unit carried before (shell binary, `icon-apps.bin`
removed, `settings.conf` byte-identical) and record it at the top of this
sheet, or note why the unit keeps the branch.
