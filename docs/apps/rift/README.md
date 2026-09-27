# RIFT — what it draws

Generated, not drawn by hand: every image here comes out of the test suite
from fixed fixtures, and the same fixtures twice give the same pixels
(`tests/rift_shell_test.sh` checks that). They are here so the screens can be
compared with `docs/design/rift/shots/` without a board.

```sh
SHELL_BIN=<build>/pocketos-shell SHOTS_DIR=docs/apps/rift \
    bash tests/rift_shell_test.sh
```

`RIFT_THEME` and `RIFT_MODE` choose the theme; these were taken in carbon /
normal, which is the theme the design package's own exports use. The other
four themes and the Outdoor and Night modes are the same screens in different
tokens — RIFT names no colour of its own (`tests/style_lint.sh`).

| File | Source | |
| --- | --- | --- |
| `portrait-no-service.png` | `rift_app_test` | ACTIVITY with no meshcored at all, which is what a unit that has not enabled the service shows (meshcored ships disabled); the command line appears only to say so |
| `portrait-activity.png` | `rift_app_test` | ACTIVITY: the service state and why, radiod and the lease on one line, the service's traffic count, this device with the two ADVERT buttons, the nodes heard most recently, the raw feed |
| `portrait-nodes.png` | `rift_app_test` | NODES: 36 px rows, the three groups, the hop strip, `DIR`, and `?` where nothing was measured; no footer and no command line when neither has anything to say |
| `portrait-nodes-selected.png` | `rift_app_test` | a row selected — expanded in place into the state line, the path written out, the signal and a 56 px action bar: MESSAGE, DETAIL |
| `portrait-node-detail.png` | `rift_app_test` | the pushed DETAIL screen, actions first — ‹ NODES, MESSAGE, RE-ROUTE, FORGET — then link state, the hop ladder, identity |
| `portrait-forget-confirm.png` | `rift_app_test` | FORGET asking first: the DS §17.5 confirmation in place of the action bar, Cancel first and accented |
| `portrait-comms-list.png` | `rift_app_test` | COMMS: the conversations, their previews and their routes |
| `portrait-comms-thread.png` | `rift_app_test` | a conversation open: each message a body and one caption line — age, state, evidence — and the field with a SEND as wide as its word |
| `portrait-comms-channels.png` | `rift_app_test` | two joined channels as rows beside the conversations, `#` glyph and `FLOOD` |
| `portrait-comms-channel-thread.png` | `rift_app_test` | a channel thread: the body without the sender's prefix, the claimed sender marked `?`, and `NO ACK ON CHANNELS` under ours |
| `landscape-nodes.png` | `rift_app_test` | the landscape split: the list beside the selected node's detail, whose actions are under its title; the key hints in the strip |
| `landscape-activity.png` | `rift_app_test` | ACTIVITY in two columns, THIS DEVICE and its ADVERT buttons at the head of the right one |
| `landscape-comms.png` | `rift_app_test` | COMMS in three panes — list, thread, route — with the command line as the composer and the newest message above it |
| `landscape-comms-channel.png` | `rift_app_test` | the same for a channel: no route to draw, and a tally that counts what was sent and claims no delivery |
| `landscape-comms-long.png` | `rift_app_test` | a 200-message thread turned: the 28 px header and 2 px between messages of DS §37.2 - 14 whole messages above the composer where there were 11 - and the identity marks on the conversation rows |
| `portrait-activity-traffic.png` | `rift_app_test` | MESH ACTIVITY scrolled to its graph: the last twenty minutes a bar each, MSG over ADV over OTHER, the legend in words, and the feed under it |
| `landscape-nodes-256.png` | `rift_app_test` | NODES with every node the cache holds (a thousand now, the name is the file's history), the identity mark on every chat node, and the detail pane beside them |
| `portrait-comms-many.png` | `rift_app_test` | every conversation the list holds (128) with nothing open: the list takes the height |
| `portrait-comms-long.png` | `rift_app_test` | the 200-message thread in portrait, unchanged by §37: 27 whole messages, the same as before |
| `portrait-activity-notify.png` | `rift_app_test` | the NOTIFY panel's switch turned off, and its line saying what that means |
| `shell-portrait.png` | the real shell | the whole stack: Doors status bar, app header, RIFT, and a scripted meshcored on the other end of a real socket |
| `shell-landscape.png` | the real shell | the same, turned |

The `rift_app_test` orientations are drawn without the status bar: that bar is
the shell's, and the shell is not in that test. The two `shell-*` images have
it, and their clock reads the wall time, so those two are the only frames here
that are not identical between runs.

Ages, not times of day. The design's mocks read `11:32`; this board has no
clock that survives a power cut (`docs/hardware/T-DISPLAY-K230.md`) and a
message's own `timestamp` is the *sender's* clock (`docs/api/mesh.md`), so
neither is a local wall time RIFT could honestly print. Every interval on
these screens is measured on the monotonic clock both ends of the socket
share.

Nothing in these was taken from hardware. Nothing that produced them
transmitted: the fixtures are read, the screens with a composer have an empty
one, and the ADVERT and FORGET presses the test makes before these are taken
go to a client with no service behind it, which refuses them and says so.
