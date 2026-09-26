# RIFT

The mesh client for Doors: what this node is, what state its radio service is
in, which nodes it has heard, and how a packet would get to one of them.

**Status:** on master: phase 1 (ACTIVITY, NODES — unit A bench gate PASS
2026-09-19, `docs/hardware/RIFT_PHASE1_BENCH_GATE.md`), phase 2 (COMMS —
unit A re-gate PASS 2026-09-20) and channels (on-air gate A–D PASS
2026-09-21, `docs/hardware/RIFT_CHANNELS_GATE.md`).

Also on master: the RIFT improvements - the ADVERT buttons, forgetting a node
or its route, the per-message ACK deadlines in meshcored and the screen-space
changes described below (unit A gate PASS 2026-09-22,
`docs/hardware/RIFT_IMPROVEMENTS_GATE.md`); 256 retained nodes, newest first
(unit A gate PASS 2026-09-22, `docs/hardware/MESH_NODE_CAPACITY_256_GATE.md`);
and, host-tested only (2026-09-23), channel conversations keyed by the
channel rather than the slot, so a reused slot does not inherit an old
channel's history and a reply from a left channel is refused. Every gate
above ran bench-deployed builds; none ran from a flashed v0.0.11 image.
Known limits are in docs/KNOWN_ISSUES.md ("RIFT, the mesh client").

On `feat/rift-ui-next`, **host-tested only** (2026-09-26, no hardware yet):
the whole node table (256) in a virtual NODES list, denser COMMS and threads,
an activity measure on every node and conversation, and a DM sound behind a
setting - which in this build is silent, because Doors has no notification
sound for an app to ask for ("The DM sound", below).

RIFT puts a packet on the air in two places, both at a reader's press, and
the whole of "What it is not" below is about where that is allowed to happen
and what it is not allowed to claim.

The design is `docs/design/rift/HANDOFF.md`, approved 2026-09-19; that
package is the contract and this phase implements part of it.

## What it is

Four sections, in the design's fixed order: **ACTIVITY · NODES · COMMS ·
NET**. RIFT draws the first three.

- **ACTIVITY** — the radio service's state and the reason for it in the
  service's own words; on one line, whether radiod is connected and what
  state it reported, whether the lease is held and whether the node can
  transmit; how many nodes the service holds, which build of meshcored is
  answering, and the service's own traffic count (`RX 2627 · TX 6 OK`, with
  failed, unknown and no-receive-after transmits named only when there were
  any). **THIS DEVICE** — this node's name, hash and key, and the two
  **ADVERT** buttons (below). Then the nodes heard most recently, and the raw
  frame feed. A full node table is said here in words, with what to do about
  it.
- **NODES** — every node the service holds (all 256 its table can), as
  36 px rows: link glyph, name, role, hop strip, hop count, RSSI, SNR
  (landscape), last heard and the activity pulse (below). Grouped into
  heard within 12 h, not heard for longer, and never heard. The list is
  virtual: a pool of rows is placed over whatever part of it is on screen,
  so 256 nodes cost what a screenful does. A row selects and
  does nothing else; the selection expands in place into the state line, the
  path written out, the signal, and a 56 px action bar: **MESSAGE** and
  **DETAIL ›**. **DETAIL** pushes a screen that opens with its actions —
  **MESSAGE**, **RE-ROUTE**, **FORGET** — then the link state, the hop
  ladder, the path changes RIFT has seen, and the identity. In landscape the
  same detail is the pane beside the list.
- **COMMS** — the conversations as 36 px rows: link glyph, name, the newest
  message as a preview, the unread pill, when the other side was last heard
  from with its activity pulse, and how that peer is reached. In portrait
  the list is as tall as its rows - up to five while a thread is open, and
  all but the composer's room while none is - rather than a fixed 268 px.
  Choosing one opens its thread: the messages oldest first, each a body, a
  2 px rule on the side that says which of you said it, and **one** caption
  — on the body's own line when both fit, which for a short message is one
  line in all — how long ago, then the service's own word for its state:
  `4m · DELIVERED · ACK 41 s`, `1m · RECEIVED · −88 dBm · SNR 6.5`,
  `SENT · FLOOD`, `NO ACK`, `FAILED`. Under it the composer. The unread count
  rides on the COMMS tab, so it is visible from the other sections. A
  conversation that does not exist yet is started from **NODES**: select a
  node and press **MESSAGE**, from the row's action bar or the detail's, and
  COMMS opens on that peer with the composer live and the thread honestly
  empty — no placeholder message is invented to make it look begun.
- **NET** keeps its place in the navigation and says it is not in this build.
  An empty view would read as a quiet mesh.

### Advert, forget, re-route

- **ADVERT NEAR / ADVERT MESH** (ACTIVITY, THIS DEVICE) send one signed
  self-advert, zero-hop or flooded (`mesh.advert`, `zero_hop`). Zero-hop is
  heard in direct range and repeated by nobody, at the airtime of one packet.
  Nothing in RIFT or meshcored adverts on its own. The caption under the
  buttons says `ZERO-HOP ADVERT · ACCEPTED 12s AGO` — **accepted**, because
  that is what the service's answer means; how the transmit went is the
  activity feed's, in the service's own words. The buttons are there only
  while the service says its radio can send, and one advert at a time.
- **FORGET** (the node's detail) asks the service to drop the node
  (`mesh.node_remove`): its contact, its learned route, its last advert. It
  asks first — a DS §17.5 confirmation, Cancel first and accented, saying
  what it costs — and nothing is sent until the confirmation is pressed. A
  confirmation belongs to the node it was asked about and is dropped if the
  selection moves; any other way out — another section, closing the detail,
  turning the panel — is Cancel, so nobody comes back to a FORGET left armed.
  The node comes back when it next adverts; until then no
  message can be sent to it. MeshCore's contact table holds 256 and evicts
  nothing on its own, so this is what makes room when a new node's adverts
  are being turned away — which on unit A blocked a direct message to a new
  peer until the state file was moved aside by hand
  (`docs/hardware/RIFT_CHANNELS_GATE.md`). The warning that the table is
  full counts adverts turned away **since the last node this app saw
  forgotten** (by this app or, through the `mesh.node` event, another client):
  the service's `nodes_unretained` only grows, and once room has been made the
  earlier refusals no longer say the table is full.
- **RE-ROUTE** asks the service to forget only the learned route
  (`mesh.node_reset_path`), so the next message floods and the reply teaches
  a new one: the remedy for a node that has moved and whose direct messages
  go unacknowledged. One press; it transmits nothing, and is offered only
  when there is a route to forget.

Each says what became of it where it was pressed, in the service's words when
it refuses (`FORGET · NOT DONE: …`), and a request whose connection went
before it was answered says there was no answer and that it may or may not
have happened (`FORGET · NO ANSWER: …`). A message is the same: `Not sent`
only when the service said no, `No answer` when the connection went first.

### What the screen gives the data

The command line is there only when it holds something: the landscape
composer (landscape COMMS with a conversation open), or the line that says
the service is not answering. Elsewhere its 56 px go to the section: of the
378 px a landscape body has under its strip, that is 15 % more list. The
landscape key hints moved to the strip's right caption beside the counts —
`↑↓ SELECT · ENTER MESSAGE · 5 KNOWN · 3 FRESH · MAX 8 HOPS` — and Enter in
landscape NODES opens the selected node's conversation (it used to be named
`ENTER DETAIL` and do nothing, the detail being already on screen).

Footers are shown only for what the rows cannot say themselves: no nodes,
cached data, a forgotten node, a full table, a channel fault. The counts they
used to repeat are in the group labels. A thread's note likewise: a failed or
pending send, why the composer cannot send, or an empty thread — which is
where the history's one caveat, that it does not survive the service's
restart, is said. The landscape route pane keeps the delivery tally and that
caveat.

A list keeps its place: a rebuild — a new node, a re-ordering by last heard, a
selection — used to put the list back at its top (`lv_obj_clean` does), which
on a live mesh happened every few seconds under whoever was reading. Neither
list rebuilds on a re-ordering any more: NODES rebinds its pool of rows,
COMMS rebinds its rows in place, and a thread appends the new message's row
and drops the oldest - a reader scrolled back into a thread is left there
when a message arrives, and only a reader at its end is taken to it. The
selected row is scrolled into view when the selection moves, so the arrows
never walk it off the pane. A thread is read at its end again whenever its
pane changes height — the landscape composer appearing, the portrait
keyboard — so the newest message is not the one a height change hides.

Panel captions are drawn whole. They are centred on the panel's top rule, so
half of each lies above the panel, and LVGL clipped every one of them at the
rule — on unit A as well (`docs/hardware/shots/rift-phase1-unitA-activity-
2055.png`).

**COMMS holds direct conversations and channels in one list**, as the design
asks, with the `#` glyph on a channel row. A channel is a conversation like
any other here — one list, one thread, one read mark, one unread count, one
composer — and the places it is *not* are the places the protocol differs:

- **Nothing acknowledges a channel message.** A MeshCore group frame is
  flooded and unacknowledged, so an outgoing one reaches `sent_flood` and
  stays there. Its caption says `SENT · FLOOD · NO ACK ON CHANNELS` rather
  than leaving a permanent `SENT` to be read as a delivery that has not turned
  up, and the tally counts channel sends apart from delivered and no-ack
  instead of reporting `0 DELIVERED`, which would read as a failure.
- **A sender's name on a channel is a claim.** It comes out of the message
  payload and nothing signs it; anyone holding the key can send any name. It
  is drawn with a trailing `?` rather than the way a `peer_name` is, because a
  `peer_name` arrived with a public key behind it. The body is printed
  without the `<name>: ` MeshCore writes into the payload - taken off only
  when it is exactly the name the service parsed - so the name is said once,
  as a claim, and a reader's own line on a channel is what they typed.
- **A channel has no route.** The route column says `FLOOD`, and the landscape
  route pane says a channel is a shared key rather than drawing a hop chain
  that does not exist.
- **RIFT cannot join or leave one.** `mesh.channel_add` takes a pre-shared
  key, and there is nowhere on a RIFT screen to type one; adding it would be
  this app growing a key-entry surface nobody asked for. The app holds no key,
  derives no channel from a name, and draws only channels `mesh.channels`
  reported. `tests/rift_lint.sh` checks each of those.

Landscape (1232 × 568) is a recomposition, not a rotation: NODES becomes the
list beside the selected node's detail, ACTIVITY becomes two columns, COMMS
becomes list, thread and route in three panes with the command line as the
composer, and the extra SNR column appears. Nothing exists in landscape that
portrait cannot show. The split is chosen from the room the app is given, not
from the orientation, so a body shrunk by a keyboard sheet keeps the single
column.

## What it is not

- **It transmits in two places, and never on its own.** `mesh.send` and
  `mesh.advert` are the only two methods in the API that put a packet on the
  air. Each is named once, in `rift_ipc.c`, and written by one function.
  `mesh.send` is reached only from the composer, by a reader pressing SEND
  on text a reader typed; `mesh.advert` only from the two ADVERT buttons'
  handler. Opening a screen, a snapshot, a period expiring and a reconnect
  all still put nothing on the air. `tests/rift_lint.sh` checks each link in
  both chains; `tests/rift_ipc_test.c` checks it from the other end, by
  recording every method the service was asked for over a whole run, every
  body it was asked to send and every advert. Forgetting a node or its route
  transmits nothing, and is held to the same rule: named once, reached only
  from the detail's buttons, and forgetting only from the confirmation.
- **It does not decide that a message arrived.** A message's state is copied
  from the service's word and never chosen here: accepted is not transmitted,
  transmitted is not acknowledged, and nothing turns `sent_flood` into
  delivered. While a request is in flight the thread says "Sending", which
  claims only that.
- **Not a channel *manager*.** It shows the channels the service holds and
  writes to them. Joining one takes a key, which is the service's business and
  not a screen's. See above.
- **Not a contact manager either.** It forgets a node, or its route, when a
  reader asks; it does not add, import, export, favourite or rename one.
- Not a network map: no rings, no relay load, no inferred links. That is NET.
- No command parser. The approved design makes the command line permanent
  chrome with `/msg`, `/join` and the rest behind it; without a parser it
  held a line of key hints, so it is now drawn only as the landscape composer
  or to say the service is not answering, and the section has the room the
  rest of the time. The commands are a later phase, and would bring the line
  back as what the design meant it to be.
- It stores the reader's own choices and nothing else - today one, the DM
  sound - opens no device, links no radio or protocol library, plays no
  sound itself, and owns no colour.

## What it shows, and what it refuses to show

meshcored leaves out what nobody measured (docs/api/mesh.md), and this app
keeps it left out.

| On screen | Means |
| --- | --- |
| `?` in `text_muted` | the service could report this and has not: an RSSI nobody measured, a hop nobody named, an age with no timestamp behind it |
| `—` | the value cannot exist at all: an end-to-end RSSI over relays |
| `DIR` | zero relays. Never `0` |
| `NO PATH` | no route back is known. Not zero hops |
| `NEVER HEARD` | no timestamp at all, which is not the same as heard long ago |
| "cached" | meshcored is not answering and what is on screen is the last thing it said |

A node heard in the future - meshcored's monotonic clock ahead of this app's,
which can only be a fault - is shown as an unknown age and never as a time to
come.

## The path

The full hop list is kept and never truncated; only the drawing compresses.
More than four relays draws the first two, a `+n` cell and the last one, and
the numeric hop count is always printed in its own column beside it.

MeshCore packs a whole number of bytes per hop, so a path divides exactly by
its hop count. When it does not - the path was clipped at `MCD_MAX_PATH`, or
the count and the bytes disagree - the hops are real but none of them can be
named, and they are drawn as unknown rather than sliced into plausible
pieces. The state line then says so: `RELAYED · 3 HOPS · 3 UNKNOWN HOPS`.

A hop hash is named only when exactly one node in the cache matches it. Two
nodes sharing a first byte resolve to nothing, for the same reason
`mesh.node` refuses an ambiguous prefix: a guessed name on a hop is a wrong
route drawn confidently.

The path changes RIFT has seen are its own record, and they now survive the
periodic node snapshot. A snapshot replaces everything the service says
about a node - an absent signal is absent, not the last one an event
carried - and keeps what RIFT observed of it: the last three paths and how
many events named it. It used to replace those too, so the "path changes"
panel was emptied every 20 s and could hardly ever show a change. A route
that changed while no event reached RIFT - across a reconnect - is recorded
from the snapshot that shows it. A reply to RIFT's own `mesh.node` question
updates the node without being counted as an event about it.

## Activity: how lately it was heard from

Every node row and every conversation row carries its **last heard** age and
the same age as a three-dot **pulse**. The pulse is that age bucketed, and
nothing else - no signal, no hop count, no count of frames, no guess about
a link:

| Pulse | Word | The newest observation is |
| --- | --- | --- |
| three dots, in `radio_rx` | `NOW` | 5 minutes old or less |
| two | `RECENT` | an hour old or less |
| one | `QUIET` | 12 hours old or less (`RIFT_STALE_MS`, the same boundary NODES groups on) |
| none filled | `STALE` | older than 12 hours |
| nothing drawn | `?` | there is none, or its stamp is in the future (a clock fault) |

What counts as an observation is what the API already reports:

- **a node**: `last_heard_mono_ms`, which meshcored stamps on an advert, a
  direct message and peer data from that node (`mesh_runtime.cpp`, `stamp()`
  - DOCUMENTED from the service's source);
- **a direct conversation**: the later of its peer's `last_heard_mono_ms`
  (while the node is held) and the newest *incoming* message;
- **a channel**: the newest incoming message on it.

Nothing this device sent counts: sending proves nothing about who is
listening. Dots in a row and not bars of rising height, placed after the age
and away from RSSI and SNR, so the pulse does not read as a signal meter.
The word is written out in a selected node's expansion (`HEARD 11m · RECENT`).
It is computed when a row is drawn, from stamps already held - there is no
background work and no history kept for it.

The mesh as a whole gets two counts: the landscape strip says how many nodes
are `NOW` (`5 KNOWN · 2 NOW · 3 FRESH · MAX 8 HOPS`), and MESH ACTIVITY says
how many frames the service reported in the last five minutes, by direction
(`LAST 5 MIN · RX 12 · TX 1`). That count is taken from the 48 frames RIFT
holds, and says `48+` when all 48 are that recent and there may have been
more. It is a count of frames and not a measure of the link.

## The DM sound

A short sound when a **direct message genuinely arrives**, behind a setting
on ACTIVITY (**NOTIFY · Sound for a new DM**, `ON` / `OFF`, on by default).
Which messages count is decided once, in the model, where the message is
filed (`rift_model_apply_live_message`, `rift_arrivals.c`). All five must hold:

1. it arrived as a live `mesh.message` **event** - never from a
   `mesh.messages` snapshot, which is history however recent: the one taken on
   opening, and the one taken after every reconnect;
2. it is **incoming and direct** - not this device's own, not a channel's;
3. its id is **new to the window** - an id already held is a state change or
   the same event again;
4. its id is **above every id this run has shown**, live or in a snapshot -
   anything at or below is history coming round again. The mark goes with the
   window when the service restarts, because the ids start again from 1;
5. its peer, sender timestamp and text are **not those of one of the last
   eight arrivals** - a sender's retry, which meshcored records as a new
   message with a new id (docs/KNOWN_ISSUES.md). Only when the sender's
   timestamp is known.

Then the policy (`rift_notify.c`): nothing while the setting is off, nothing
while Doors is muted (`pocketos_shell_volume_effective()` is 0) or there is
no sound to play, and at most **one sound in 10 s** however many arrive - a
burst of twenty is one sound, and nothing is queued to play later. An arrival
the setting, the volume or the gap kept quiet is dropped, never replayed when
they change. The sound is played at the system volume and stopped when RIFT
closes.

**In this build the sound is silent, and the switch says so.** Apps never
touch the sound card (ADR-002), and the one exception - pocketaudio driven by
a per-operation `pos-wave` helper - is Wave's and is not to be extended
(ADR-004). ADR-004 names system sounds as the point at which audio moves to
a platform owner, and that decision is not RIFT's. So the sound goes through
a backend seam (`rift_sound.h`, in the shape of PocketClock's
`clock_alert.h`), whose built-in backend has no sound and says: *No system
notification sound in this build of Doors: a new direct message is shown, not
heard.* Everything above is built and host-tested against a fake backend; the
setting is stored and honoured, so the day a backend is registered the sound
works and nothing else changes.

The setting lives in `$POCKETOS_STATE_DIR/rift/prefs.v1` (`dm_sound=0|1`,
settings.conf's `key=value` format), the app-owned-store pattern Fleet, Radar
and Timber use; the shell's `settings.conf` is the shell's and no app writes
it. A file that cannot be read leaves the default; one that cannot be written
keeps the choice for the session and the switch says it is not saved.

### Shared requirements left for integration

Not implemented here, because each is a platform API rather than RIFT's:

- **A notification sound an app can ask for** - for example
  `pocketos_shell_play_sound(POCKETOS_SOUND_MESSAGE)` in `app.h`: short,
  non-blocking, at the system volume, silent while muted, with the shell (or
  an `audiod`, ADR-004 Option A) owning the card. Needs the ADR-004 revisit.
  RIFT's side is one backend of about twenty lines in `rift_sound.c`'s shape.
- **Optionally, an app-preference store** (`pocketos_shell_pref_get/set(app,
  key)` over settings.conf), if the product owner would rather apps did not
  each keep a file. RIFT would move `dm_sound` over and drop `rift_store.c`.
- **The viewport after the chrome work lands.** Every RIFT layout is sized
  from the body it is given (NODES' pool, COMMS' portrait list cap); none
  assumes the header or status bar height. After the chrome refactor, re-run
  `tests/rift_shell_test.sh` in both orientations and look at the shots.

## Scale

What RIFT holds, and what it builds for it:

| | Held | Built |
| --- | --- | --- |
| Nodes | 256 (`RIFT_MAX_NODES`, all of meshcored's table; was 64) | rows for the screen only: 31 in portrait, 35 at most after turning (`rift_nodes_rows_built`) |
| Messages | 256 (`RIFT_MAX_MESSAGES`; was 96) | a thread's newest 64 (`RIFT_THREAD_ROWS`; was 32), "136 EARLIER" for the rest |
| Conversations | 64 (`RIFT_MAX_CONVERSATIONS`; was 32) | a row each, rebound in place |
| Hops on a detail's ladder | all 63 MeshCore allows (was 16) | a rung each, built when the path changes |

With more peers in the window than the list has rows, the list keeps the
conversations spoken in most recently. It used to keep the ones held longest
and leave out whoever had just spoken - a defect this work found. A path chain
too long to write whole keeps both ends and says how many hops it left out of
the middle (`… +41 …`) instead of being cut off before its target.

A repaint no longer re-sets text a row already shows or re-fits a name
already fitted to the same width. On the host (not the board), a steady
repaint of NODES with 256 nodes went from 6.2 ms to 0.2 ms, and of COMMS
with a 200-message thread open from 13.5 ms to 1.4 ms; a full-screen redraw
is about 3 ms either way (`tests/rift_app_test.c` prints both on every run).
The C908 is slower by a factor nobody has measured.

## How it talks to meshcored

One connection, asynchronous throughout (`apps/rift/rift_ipc.c`).

Requests are written and forgotten; the reply is matched by id when it turns
up. That is not a flourish - it is what keeps two promises at once. Nothing
blocks the LVGL thread, because there is no call to block in: the only
bounded wait in the module is the connect, and it is shorter than one frame.
And nothing is lost, because `pocketipc_call` discards events while it waits
for its reply, so a client that subscribes *and* calls on one connection
drops whatever arrives during a call.

Consumed: `mesh.info`, `mesh.status`, `mesh.identity`, `mesh.nodes`,
`mesh.node`, `mesh.channels`, `mesh.messages`, `mesh.send`, `mesh.advert`,
`mesh.node_remove`, `mesh.node_reset_path`, `mesh.subscribe`,
`mesh.unsubscribe`, and the `mesh.state`, `mesh.node` (including its
`removed` reason, which takes the node off the list rather than applying it),
`mesh.channel`, `mesh.activity` and `mesh.message` events.

`mesh.message` is raised three times over for one message — when it arrives
or is sent, and again whenever its state changes. All three go through one
path keyed on the message id, which is what makes the third arrival an update
to one row rather than a third copy of it, and why a message with no id is
refused outright: there would be nothing to match the next one against. A
`mesh.messages` snapshot merges the same way, so a snapshot taken after
events have already delivered some of the same messages updates them instead
of doubling them.

An id identifies a message **within one run of the service**, and not across
two: meshcored hands them out from 1 on every run and keeps no messages over
one. So the message window is emptied, with the read marks that are ids in
the same space, whenever the run changes — the way the node list has been
replaced outright by every snapshot from the start. The run is told from the
one before it by `mesh.status`'s `uptime_s` read against this app's own
monotonic clock: a smaller uptime than the last is certain, and a run start
that has moved forward by more than two seconds catches the short-lived run a
smaller uptime would miss. Not on every reconnect: a socket can go without
the process behind it going, and this window of 256 can hold more of one kind
than either of the service's two rings of 64, so emptying on a reconnect
would throw away messages the service can no longer supply. Found on unit A
on 2026-09-21, during part D of the channels gate, as four messages on the
panel against the three the service held — and not a channel fault: the id
counter has always been one counter and has always started again.

Sending is two halves that may arrive in either order. `mesh.send` answers
with a `message_id`; `mesh.message` carries the message itself. Nothing is
created from the reply — inventing a local copy and reconciling it later is
how one message becomes two — so whichever arrives first makes the single
row, under the service's id. A submission whose socket goes away before it is
answered says that it does not know whether the message was sent, because
nothing on this side does.

When the service goes away the nodes stay, marked as cached, and the client
reconnects with a backoff doubling from 0.5 s to 5 s - meshcored's own
ceiling is 30 s, which is right for a daemon and wrong for a screen somebody
is looking at. The next connection re-reads the snapshot, so a node the
service has forgotten stops being shown here.

A malformed event is refused whole, counted, and never half-applied, and it
does not drop the connection: one bad event costs one event, and a peer on
the air must not be able to disconnect this app from its own service.

## Structure

| File | |
| --- | --- |
| `rift_model.c/.h` | what is known and how sure it is: the bounded node cache, the activity ring, the service state and its counters. No LVGL |
| `rift_messages.c` | the model's second translation unit, over the same struct: the message window, the conversations, how far each has been read, the submission in flight, and which run of the service the ids in all of it came from. No LVGL |
| `rift_channels.c` | the channel table mesh.channels reports and the mesh.channel events that change it. No LVGL |
| `rift_actions.c` | an advert, forgetting a node, forgetting a route: asked, then answered or refused. No LVGL |
| `rift_arrivals.c` | which filed messages are a direct message that has genuinely just arrived: the DM sound's five conditions. No LVGL |
| `rift_notify.c/.h` | the DM sound's policy: the setting, the platform, one sound per 10 s. No LVGL, no clock of its own |
| `rift_sound.c/.h` | the seam the sound goes through, and the built-in backend that has none. No LVGL |
| `rift_store.c/.h` | the reader's preferences file, and nothing about the mesh. No LVGL |
| `rift_dm_sound.c` | the app's side of the DM sound: the setting, whether anything could be heard, the pass after every socket read |
| `rift_order.c` | read-only questions over the node cache: list order, how many are fresh, which name a hop gets. No LVGL |
| `rift_json.h` | the four readers every part parses the API with, so all apply the same rule: absent is not zero |
| `rift_format.c/.h` | every string the screens print about nodes and paths, and the path arithmetic. No LVGL, no cJSON, no I/O |
| `rift_format_msg.c` | the same for messages and requests: states, the one-line caption, the preview, the channel body, what became of an advert or a node change |
| `rift_ipc.c/.h` | the meshcored connection, the framing and the reconnect. No LVGL |
| `rift_app.c/.h` | chrome, sections, layout and lifecycle |
| `ui/rift_widgets.c` | the link glyph, the hop strip, the panel with its caption in the rule, the action bar |
| `ui/rift_activity.c` | ACTIVITY |
| `ui/rift_nodes.c` | the node list - virtual: the layout of every line, and a pool of rows bound to the part on screen - the selection and the landscape split |
| `ui/rift_node_row.c` | one node row: built once, filled from a node, given the selection's look and, in portrait, its expansion |
| `ui/rift_detail.c` | the selected node in full: one builder for the landscape pane and the portrait DETAIL screen, so the two cannot drift |
| `ui/rift_comms.c` | the conversation list, the three panes and the landscape route pane |
| `ui/rift_thread.c` | the open conversation: the header, the messages and the portrait composer. One builder for both orientations, as `rift_detail.c` is for NODES |

## Tests

| | |
| --- | --- |
| `tests/rift_format_test.c` | 92 checks: ages, signal, hop columns, state words, path compression, the inline chain, the ladder, UTF-8 names |
| `tests/rift_model_test.c` | 213 checks: the initial snapshot, duplicate and update events, missing telemetry, malformed input, the bounded cache, the service going away and coming back, which run of the service answered, ordering; the path history and event count surviving a snapshot while the service's values are replaced, a reply that is not an event, a removal that is not an update, the traffic counters, the table-full count since the last forget (and a new run counting from nothing), a route change dated when it was seen, and the advert and node-change state machine with NOT DONE kept apart from NO ANSWER |
| `tests/rift_comms_test.c` | 298 checks: the conversations and their order, duplicate and state-change events, unread and what clears it, the thread window, every state caption, telemetry that was never measured, the bounded message cache, the service restarting under the cache and the reconnect that is not a restart, the send state machine, what `mesh.send` will take, remote text nobody here chose the length of, and the channel body without its sender prefix and the one-line caption |
| `tests/rift_ipc_test.c` | 205 checks against a real socket and a scripted service in a child process: connect, snapshot, events, refusals, the service disappearing, reconnect, one whole service replaced by another with an id space that starts again, the proof that nothing the app does on its own transmits or adverts, the send lifecycle, adverts asked for and refused, and forgetting a node or its route - answered, refused in the service's words, and unanswered when the service dies |
| `tests/rift_app_test.c` | 422 checks under a real LVGL pointer device: the chrome, all three sections, the row that only selects, the pushed detail and its FORGET confirmation (cancelled by leaving the section, closing the detail or turning the panel), Enter on a node that has gone, the table-full warning clearing once room is made, a thread's No answer and Not sent, both landscape splits, the composer, the unread pill, the command line present only when it holds something, a long list keeping its place and its selection in view, the newest message in view above the landscape composer, every panel caption drawn whole, every action's word inside its button, the ADVERT buttons, and open/leave/open again three times over. Then the activity pulse on real rows; the DM sound end to end against a fake backend (history silent, one sound per arrival, none for a repeat, the reader's own, a channel, a retry or a snapshot, one for a burst, the switch on ACTIVITY stored and honoured, muted, no backend, stopped on close, kept across opening, not saved when the store cannot be written); and scale - 256 nodes with the rows built bounded by the screen in both orientations, the selection kept by key across a re-ordering and a removal, 64 conversations re-ordered without a row rebuilt, a 200-message thread moved along without a rebuild and a reader in its history left there, a hundred arrivals in one pass. Prints what a repaint costs. Writes the screenshots |
| `tests/rift_notify_test.c` | 93 checks: which direct messages are arrivals (history on opening, the same event twice, the reader's own, a channel, a reconnect's snapshot, an id below the highest, a sender's retry under a new id, no timestamp, a malformed message, a new run starting its ids again), the sound policy (a burst is one sound, nothing queued, off, muted, the gap from the last sound, a clock stepping back), the sound seam, the preferences file, the activity buckets and what a conversation is heard from, frames in the last five minutes, a 63-hop chain too long to write whole, and a list of more peers than it holds keeping the newest |
| `tests/rift_shell_test.sh` | the app test, then the real shell opening RIFT in both orientations with a scripted meshcored on a real socket, then with no service at all, then the same fixtures twice for the same pixels |
| `tests/rift_lint.sh` | the boundaries: what transmits and from where (send and advert), what changes a node and from where, no colour, no device, one store holding nothing about the mesh, a DM arrival decided in one place from live events only, the sound asked for in one place and only when the policy says so, no sound device and no helper process, the sound stopped on close, no monolith, and the gaps this build leaves |

`tests/fake-meshcored` is a scripted stand-in for the service, built by the
root Makefile and never installed. A negative `last_heard_mono_ms` in its
fixtures means "this long ago", so an age on screen does not depend on how
long the host has been up. Its `uptime_s` grows with its own process, the way
the real service's does, and a script says what it started at — so two of
them, one after the other, are two runs of a service and not one.

## Known gaps

1. **No launcher icon.** DS §20 wants an A8 tint mask on every launcher tile.
   The approved design package carries the RIFT mark as a design sheet
   (`docs/design/rift/shots/identity-sheet.png`), not as the png-32 artwork
   `tools/design/gen_app_icons.py` generates masks from, so the launcher
   draws the text icon. `tests/app_icons_test.sh` names RIFT as the one
   allowed exception and fails if a second app joins it or if RIFT quietly
   gains one without the generator being told.
2. **The last hop is not named.** The design's `LAST HOP −88 · RPT-NORD`
   needs to know which hop transmitted the frame that was heard. `mesh.*`
   reports the signal of the frame and not its sender, so the detail says the
   signal belongs to the last hop and does not say which one.
3. **Per-hop uncertainty is coarser than the design's.** The design marks an
   individual hop whose hash is missing from an advert path. The API gives a
   hop count and an opaque byte string, so this build can say that a path
   could not be divided into hops - all of them unknown - but not that one
   hop in the middle is.
4. **The chrome is Doors's.** The design's landscape chrome merges the header
   and the section strip into one 56 px row and shrinks the back slab. The
   status bar and the back slab are Doors-owned and RIFT changes nothing
   there, so the app lays out in the body it is given. RIFT is fullscreen
   (DS §30.8, 2026-09-24): there is no status bar above the header in either
   orientation, and the body takes the height it gave back: 56 px of strip, and
   the command line only while it is the landscape composer or says the
   service is not answering. The counts the design puts in the header's
   right caption are at the end of the section strip in landscape, beside the
   key hints; portrait's group labels carry them.
5. **Path history is only what RIFT saw.** There is no history before the app
   opened, and the panel is headed "PATH CHANGES SEEN BY RIFT" so it is not
   read as the service's record. It holds the last three paths, from events
   and snapshots alike, for as long as the app is open.
6. **A channel row is drawn before anything is said on it.** The model does
   not call a channel with no messages a conversation — it holds no messages,
   and inventing history is what this app must not do — but a joined channel
   that stayed invisible until somebody spoke would be a channel nobody could
   be the first to speak on. So COMMS adds a row per channel the service
   reported, with no preview, no unread and a total of zero, which is exactly
   what it is.
   Channels have been **on a radio**, in both directions, against a real
   MeshCore peer:
   [docs/hardware/RIFT_CHANNELS_GATE.md](../hardware/RIFT_CHANNELS_GATE.md)
   (unit A, 2026-09-21, **PASS**). That gate also read every string on this
   page off the unit's own DRM plane rather than off a description — the `#`
   glyph and `FLOOD` on a channel row, `CHANNEL · HASH 9a · FLOOD` in the
   header, the claim marker on a received sender's name, and
   `SENT · FLOOD · NO ACK ON CHANNELS` under an outgoing one — and confirmed
   that the words `DELIVERED` and `ACKED` appear nowhere on a channel.
7. **A message carries no route.** The design's per-message caption is
   `RECEIVED · PATH 9 · 1 UNKNOWN HOP`. The API carries a path on a *node*
   and not on a message, so the route is drawn in the thread header and the
   landscape route pane, where it is the peer's current path and is true,
   rather than under each line where it would be a guess about the frame that
   carried it. A message's own RSSI and SNR *are* reported, and are shown.
8. **No attempt count and no return path.** The design's
   `NO ACK · 3 ATTEMPTS · LAST 12:47` and `DELIVERED · ACK 41 s · RETURN PATH
   9 HOPS` need how many times MeshCore tried and which way the ACK came
   back. The API reports neither, so the captions stop at `NO ACK` and
   `DELIVERED · ACK 41 s`. The delivery tally in the landscape route pane is
   RIFT's own arithmetic over the messages it still holds, and is worded as
   that rather than as the service's count.
9. **Unread is this session's.** Nothing persists it, and meshcored's message
   store does not survive its own restart (`"persistent": false`), so
   "unread" means "arrived while RIFT was open and has not been drawn". The
   first snapshot of a session is marked read, because it happened before
   this app was watching and it has no way to know what was read then; a
   snapshot after a *reconnect* is not, because what arrived while the
   connection was down is genuinely unread. A read mark is a conversation's
   last read id, so the marks go when the message window does — everything a
   restarted service holds arrived while this app was not watching, and is
   unread by the same rule.
10. **The message window is bounded.** The newest 256 are kept, and a
    conversation's unread count, preview and tally are derived from what is
    still held. A thread longer than the pane says how many are earlier
    rather than implying there are none.
11. **A forgotten node comes back only by its own advert.** There is nothing
    else that can bring it back: the service keeps no copy, and RIFT asks for
    none. That is what the confirmation says. Forgetting is also the only
    way to make room in a full table - there is no favourite to protect a
    node and no automatic eviction - which is upstream's policy, unchanged.
12. **Adverts carry no position.** `mesh.advert` builds the advert with the
    node's name only; MeshCore can add a location, and this service has no
    location to add and no setting for one.
13. **No trace, no path discovery, no repeater login.** MeshCore has all
    three, and each transmits and needs a request/response the service does
    not yet match. They are not in the API (docs/api/mesh.md, "Not in v0").
14. **The DM sound is silent in this build.** There is no platform sound
    for an app to ask for, and RIFT does not open the card itself (ADR-002,
    ADR-004). The switch says so. See "The DM sound".
15. **A sender's retry is still shown as a second message.** meshcored
    records one per attempt (docs/KNOWN_ISSUES.md) and RIFT shows what the
    service recorded; only the sound recognises the retry and stays quiet.
16. **The activity measure has no history.** It is the age of the newest
    observation and says nothing about how often a node is heard; a rate would
    need timestamps RIFT does not keep. RSSI and SNR stay in their own columns
    and are never folded into it.

## What needs hardware

For `feat/rift-ui-next`: the unit A integration gate **passed on
2026-09-26** on build `d512ba9` (rebased onto master 140843e, with the
compact status cluster):
[docs/hardware/RIFT_UI_NEXT_GATE.md](../hardware/RIFT_UI_NEXT_GATE.md).
Both orientations and rotation both ways, a real mesh of 241 nodes scrolled
end to end, selection and detail, COMMS threads, the DM sound switch stored
across a reopen, and one real DM from unit B counted once. Still open: a
multi-hop path on a board (the bench mesh had none beyond 0 hops), 256
nodes, and a finger on the glass. The DM sound cannot be heard until the
platform has a sound for it; when it does, check one sound per arrival, none
for the history on opening or after a reconnect, one for a burst.

For the phases on master:

The unit A gate **passed on 2026-09-22**, with one portrait check inconclusive
and minor follow-ups recorded (the RE-ROUTE and FORGET captions outlive what
they say):
[docs/hardware/RIFT_IMPROVEMENTS_GATE.md](../hardware/RIFT_IMPROVEMENTS_GATE.md),
which also holds the payload's hashes and every command. What it had to see,
on the panel and in the service's counters:

1. **ADVERT NEAR and ADVERT MESH** each put exactly one advert on the air -
   `tx_submitted` up by one, `mesh.activity` `tx` `ok` - and a peer in range
   learns this node from the zero-hop one (a T-Deck's node list, or its
   advert decoded with `tools/meshcore-frame parse`); a peer two hops away
   learns it from the flooded one and not the zero-hop one.
2. **FORGET** on the T-Deck peer: gone from the list, from `mesh.nodes` and
   from `state.v1` after a meshcored restart; a message to it refused; back
   in the list at its next advert; a direct message to it acknowledged again
   after the peer has adverted.
3. **RE-ROUTE** on a peer with a learned route: the next message goes
   `sent_flood`, the ACK returns, and the route is learned again.
4. **Two direct messages in flight**, the first to a peer that is switched off
   and the second to one that answers: the second `acked`, the first `no_ack`
   at its own deadline and not before (about 12 s flood, 6 s direct plus
   airtime) - the case the service used to leave `sent_*` for ever.
5. The **screens** read off the DRM plane with `ffmpeg -f kmsgrab`, both
   orientations: panel captions whole, no command line on NODES and
   ACTIVITY, the landscape composer with the newest message above it, the
   four detail actions' words inside their buttons in portrait, and a list of
   more than a screenful keeping its place while the mesh re-orders it.
