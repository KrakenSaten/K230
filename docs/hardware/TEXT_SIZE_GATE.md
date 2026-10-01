# Text size and readability - hardware gate (DS §46)

**Not run.** Everything on branch `feat/text-size-accessibility` was checked
in the simulator only (DS §46.8); no unit was touched while it was built.
This sheet is what only the panel and a finger can answer. State the unit
and the build at the top when it is run (see the other gate sheets).

## Deploy

Hot-swap is enough: `doors-shell` and the regenerated portal icons
(`/usr/share/doors/ui/icon-*.bin`, 29 files, with `MANIFEST.txt`). Nothing
else changes on the unit. Keep a rollback copy of both first. The stored
size lives in `/etc/pocketos/settings.conf` as `text_size`; remove the line
to return to Small.

`doors call shell shell.text_size size=medium` (or `large`, `small`) changes
it from SSH; `doors call shell shell.audit` measures the screen on show
(`count` should read 0 clipped, 0 overlap, 0 zero; `truncated` lists any
"..." the launcher used).

## Steps

| # | Step | Pass when |
| --- | --- | --- |
| T1 | Small against the current DOORS: launcher (portrait and landscape), Settings, an app header, Controls, the lock | Text, spacing and places are those of master. Expected differences, and only these: muted notes a little brighter (§46.7), the portal icons' glyphs heavier and their colour stripe stronger, the press/focus mark 3 px |
| T2 | Settings -> Appearance -> Text size: MEDIUM | The three choices show their own sizes; the change is immediate, Settings stays open where it was, nothing jumps or blinks twice |
| T3 | Medium, reading at arm's length with and without glasses: launcher, Settings, System, a list (Files), a dialog (Files delete, System power) | Noticeably easier to read than Small; nothing cut, nothing over anything |
| T4 | Large, the same screens | Readable without effort; the launcher still recognisably DOORS; a long name in a landscape launcher cell may end in "..." (DeskBuddy), nothing else |
| T5 | Launcher portrait and landscape at Large: favorites picker, GAMES, UTILITIES | Every cell named; the folder pages scroll if they must |
| T6 | Dialogs and confirmations at Large: Controls' antenna question, Files delete, System power, Photo delete, an alarm firing (Clock) | Every button whole, every line of the question readable |
| T7 | Camera at Large | The preview is not covered more than at Small; the controls stay on screen |
| T8 | Photo at Large: a photo, the three lines, DELETE and its question | The picture is not covered; the lines readable |
| T9 | Vision at Large: DETECT and COUNT | Overlay labels stay compact (they do not follow the size); boxes and trails not covered; the strip and controls on screen |
| T10 | DeskBuddy at Large | Companion text readable; the panel fits |
| T11 | RIFT at Large: ACTIVITY, NODES, COMMS with a thread open | Panel captions sit on their rules and are whole; rows readable; the composer reachable with the keyboard up |
| T12 | Fleet at Large, landscape: COMMAND (the difficulty bar goes two by two), a match with YOUR TURN / WAITING, chat | The board keeps its size; the status words whole |
| T13 | Browser, Files, Notes at Large | The app's own chrome follows the size; page text, file contents and note text are the app's (§46.4) |
| T14 | Terminal at Large | The chrome and keys row follow the size; the terminal grid keeps its columns and rows (§46.4, not bound) |
| T15 | Touch at Large: the launcher cells, Settings' rows and buttons, the keyboard | No harder to hit than at Small; nothing needs a second tap |
| T16 | Icons on the launcher at Normal, Night and Outdoor | Each portal reads as its app at a glance; nothing glows or looks cartoonish |
| T17 | Switch Small -> Medium -> Large -> Small ten times from Settings, then from SSH with Settings open, with RIFT open, with a folder open | Each change lands; Small at the end is T1's Small; `doors shell info` stays answering; no warning in shell.log but the stored-size line |
| T18 | Memory and CPU: `VmRSS` of `doors-shell` at Small after boot, after T17, and after an hour on the launcher | Within a few hundred kB of each other; no growth across switches (fonts are not loaded, only mapped); CPU idle on the launcher as at Small |
| T19 | Reboot at Large | Comes back at Large: lock, launcher and Settings all Large; Settings shows LARGE accented |
| T20 | `doors call shell shell.audit` on each of T3-T13's screens at Large | 0 clipped, 0 overlap, 0 zero (record any truncated list) |
