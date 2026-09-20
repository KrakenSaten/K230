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
| `portrait-no-service.png` | `rift_app_test` | ACTIVITY with no meshcored at all, which is what a unit that has not enabled the service shows. meshcored ships disabled |
| `portrait-activity.png` | `rift_app_test` | ACTIVITY: the service state and why, this device, the nodes heard most recently, the raw feed |
| `portrait-nodes.png` | `rift_app_test` | NODES: 36 px rows, the three groups, the hop strip, `DIR`, and `?` where nothing was measured |
| `portrait-nodes-selected.png` | `rift_app_test` | a row selected — expanded in place into the state line, the path written out, the signal and a 56 px action bar |
| `portrait-node-detail.png` | `rift_app_test` | the pushed DETAIL screen: link state, identity, the hop ladder |
| `portrait-comms-list.png` | `rift_app_test` | COMMS: the conversations, their previews and their routes, over a note saying the list is direct-only because the service has no channels |
| `portrait-comms-thread.png` | `rift_app_test` | a conversation open: the messages with their state captions, and the 56 px field and SEND the touch keyboard comes up for |
| `landscape-nodes.png` | `rift_app_test` | the landscape split: the list beside the selected node's detail |
| `landscape-activity.png` | `rift_app_test` | ACTIVITY in two columns |
| `landscape-comms.png` | `rift_app_test` | COMMS in three panes — list, thread, route — with the command line as the composer |
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
transmitted: the fixtures are read, and the one screen with a composer has an
empty one.
