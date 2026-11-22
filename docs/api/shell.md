# shell.* API v0

Status: draft, api_version 0. Transport: pocketipc, socket `shell.sock`.
Served by the PocketOS shell process. Intended for the `pos` CLI, tests and
developer tooling, not for applications.

## Methods

- `shell.info`: `api_version`, `apps` (array of `{id, name, page_of?}`;
  `page_of` names the app this one is a page of - System, of Settings, DS
  §47 - which has no launcher cell and whose way back is that app), `current`
  (open app id or `"home"`), `background`: array of `{app, label, help}` -
  work an app keeps running with no screen of its own over it, as the app
  said (app.h `pocketos_shell_set_background`, DS §51; RIFT's session:
  `{"app":"rift","label":"RIFT","help":"RIFT active in background"}`) -
  and `background_mark`: whether the status cluster's mark for it is up,
  `display`: `{width, height, backend}` - the
  logical size this run lays out in, 568x1232 or 1232x568 - and the
  orientation fields `shell.rotation` returns; `chrome`: the status chrome
  in force for the current screen (DS §30, §36) - `policy`: `"cluster"` (the
  status cluster in the top-right corner: the launcher, Controls and every
  app that does not declare NONE) or `"none"` (a fullscreen app);
  `content_y` and `content_h`: the content area, which starts at the top
  edge under every policy; `cluster`: `{shown, x, y, w, h, clock, reserve_x,
  reserve_w}` - the cluster as drawn now (shown over a fullscreen app only
  while the lock is engaged), whether it holds the clock (not on the
  launcher, Controls or the lock, which show the time large), and the widest
  box it can take on this screen, which the rows under it keep clear of;
  `chip`: `{h, content_h, line_h, y, text}` - the radio chip as drawn; and,
  with an app open, `header`: `{y, h, content_x2, pad_left, pad_right,
  hint_x2, hint, body_y}` - the shell's app header and where the app's body
  starts. `launcher.time`, `launcher.date`: `{x, y, w, h}` - where the
  launcher's time and date labels are.
- `shell.open` params `{id}`: opens an app. Error 2 for unknown id.
- `shell.home`: closes the current app and shows the launcher.
- `shell.action` params `{action}`: runs one hardware action by name,
  exactly as the keyboard base's key that carries it does
  (ui/shell/hw_actions.h, docs/hardware/HARDWARE_CONTROLS.md): `home`,
  `back`, `settings`, `terminal`, `wave`, `vision`, `rift`, `screenshot`,
  `volume_up`, `volume_down`, `brightness_up`, `brightness_down`,
  `keyboard_light_up`, `keyboard_light_down`. Result `{action, result,
  value?, current}`: `result` is `done`, `noop` (already there - an app is
  never opened twice - or at a level's bound, which never wraps),
  `refused` (the lock or an alert is up and the action would navigate) or
  `unavailable` (no such app in this build, no such control on this board, a
  screenshot still being written); `value` is the level after a level
  action. Any other name is error 2. `back` from a page of another app
  (`page_of`, System) opens that app (Settings), as the header's back slab
  does (DS §47). `pos call shell shell.action action=back`.
- `shell.key` params `{raw: [bytes]}` or `{code}`: feeds raw keyboard-base
  controller events (bit 7 press, bits 0-6 the matrix code; `code` is a
  press and its release) through the physical key's own path - the key map,
  the modifiers, the stream into the focused field, and the actions. For the
  bench and the tests: the way to exercise a key nobody can press remotely.
  1 to 32 bytes 1..255, or a code 1..127; anything else is error 2. Result
  `{taken, current}`. `pos call shell shell.key code=64` (F8).
- `shell.info` also carries `hardware`: `{keyboard: {present, caps,
  delivered, reserved, actions}, leds: {available, caps, mic, camera,
  failures}, microphone, camera, keyboard_light, screenshot: {busy, saved,
  last}}` - the keyboard base as the driver sees it, the indicator LEDs as
  last written, whether a capture stream and a camera node are open now
  (what the LEDs show), the keyboard light's level (-1 without one), and
  F7's captures.
- `shell.screenshot` params `{path}`: renders the current screen to a PNG at
  `path` (on the device filesystem). Error 4 if it cannot be written. Needs
  LV_USE_SNAPSHOT, which the device's LVGL does not have (KNOWN_ISSUES); F7
  (`shell.action action=screenshot`) captures on the device instead, from
  the DRM plane, into `<state>/screenshots/screenshot-<time>.png`.
- `shell.theme` params `{theme?, mode?}`: selects a Design System theme id
  (`ice`, `brass`, `olive`, `slate`, `carbon`) and/or display mode
  (`normal`, `outdoor`, `night`) live, no restart. An unknown id or mode is
  not an error: the shell falls back to `ice` + `normal` (DS §8) and the
  result carries `fallback: true` with a `reason`. Result: `{theme, mode,
  fallback, reason}`. `shell.info` also reports `theme` and `mode`.
- `shell.text_size` params `{size?}`: reads, or with `size` sets, the
  system-wide text size (DS §46): `small` (the default), `medium` or
  `large`. Applied live: every shared style takes its font at once, and on
  the shell's next pass the launcher, Controls and the app header are laid
  out again; an app open at the time is opened again at the new size (the
  Settings app's own control changes it without reopening Settings). Stored
  as `text_size` in settings.conf, so a restart or a reboot keeps it.
  Result: `{size, pending}` - `pending` while the shell's re-layout has not
  run yet. Anything but the three names is error 2 and changes nothing;
  error 4 when it was applied but could not be stored. A stored value that
  is not one of the three starts the shell at `small` with a warning and is
  left in the file. Event `shell.text_size` `{size}` on every set.
  `shell.info` also reports `text_size`. `pos call shell shell.text_size
  size=large`.
- `shell.audit`: measures what is on the screen now (`pocketui_audit.h`):
  text cut off or shortened with "...", controls clipped where nothing
  scrolls to them, meaningful objects over each other, objects with no size.
  Result: `{current, text_size, landscape, objects, labels, count: {clipped,
  truncated, overlap, zero}, issues: [{kind, path, text, x, y, w, h, other,
  other_path?, other_text?}]}` (at most 200 issues listed; `count` has them
  all). Read-only; for the text-size gate on the device. `pos call shell
  shell.audit`.

- `shell.brightness` params `{percent?}`: reads, or with `percent` sets, the
  panel brightness (docs/hardware/DISPLAY_BRIGHTNESS.md). Result: `{supported,
  percent, min, max, step, device}`; `percent` and `device` are `null` when
  the display has no brightness control or the level cannot be read, and
  `percent` can be below `min` when something other than PocketOS set it.
  `percent` must be an integer in `min..max` (10..100): anything else is
  error 2, with nothing applied. Error 6 when the display has no brightness
  control, error 4 when the device refused the write. A level is persisted
  (`display_brightness` in settings.conf) only once the device accepted it,
  and the shell applies the stored level at start. `pos shell brightness
  [10..100]`.

- `shell.volume` params `{percent?, muted?}`: reads, or sets, the system
  volume (ui/shell/volume.h). Result: `{available, percent, muted,
  effective, min, max, step}`. `percent` is 10..100 in steps of 10 (default
  100, the level validated on unit A) and is kept while muted; `effective` is
  what playback uses, 0 while muted. `available` is false when the kernel
  lists no sound card. Both params are checked before either is applied:
  anything else is error 2 with nothing changed; error 4 when the value could
  not be stored. Stored as `audio_volume` and `audio_muted` in settings.conf,
  read at start. The speaker has no mixer volume (AUDIO_HARDWARE_MAP §7), so
  this is a digital gain pocketaudio applies to every sample (0 dB at 100 %,
  -27 dB at 10 %, under the unchanged -12 dBFS ceiling); the Wave app passes
  it to pos-wave (`--volume-percent`) and sends nothing while muted.
  `pos shell volume [10..100|mute|unmute]`.

- `shell.power` params `{screen_off_s?, auto_lock_s?, lock_at_start?}`
  (DS §52.4, ui/shell/power_policy.h): reads, or sets, Power & Sleep.
  `screen_off_s` is one of 0 (never), 30, 60, 120, 300, 600; `auto_lock_s`
  one of 0, 60, 120, 300, 600, 1800; `lock_at_start` a boolean (settings key
  `lock_screen`, read at the next start). All params are checked before any
  is applied: anything else is error 2 with nothing changed; error 4 when a
  value could not be stored. Stored as `screen_off_s` and `auto_lock_s`. A set
  counts as activity, so the new time runs from the call. Result, and
  `shell.info.power`: `{screen: "on"|"off", screen_off_s, auto_lock_s,
  lock_at_start, held, idle_ms, screen_offs, sleep: "unavailable"}` - `held`
  while an alarm rings or Video, Camera, Vision or DeskBuddy is in front (no
  timer runs then), `idle_ms` since the last touch, key or acting request,
  `screen_offs` since start. Screen off is a black cover; the touch or key
  that wakes it does nothing else. `pos call shell shell.power
  screen_off_s=60`. `key` (the board's power key, DS §56,
  ui/shell/shell_evkey.h): `{enabled, connected, device, state:
  "up"|"down"|"long"|"swallow", long_ms, shorts, longs, ignored, lost,
  swallowed, opens, losses, menu, menu_opens, menu_note}` - `device` the
  evdev node in use (found by KEY_POWER, or `POCKETOS_POWER_KEY_DEVICE`;
  `none` turns the key off), `ignored` repeats and unpaired events, `lost`
  presses ended by lost input, `swallowed` presses spent on waking the
  screen, `losses` times the device went away, `menu` whether the power menu
  is open and `menu_note` the line under its buttons. `shell.info.back_key`
  is the BOOT key (DS §57: short press Back, a hold of `long_ms` Home) in the
  same shape without the menu fields: found by KEY_BACK, or
  `POCKETOS_BACK_KEY_DEVICE` (`none` turns it off).

- `shell.timezone` params `{zone?}` (DS §52.7, ui/shell/tz_zones.h): reads,
  or with `zone` (an IANA name from the list Settings offers) sets, the time
  zone the shell shows local time in, applied at once and stored as
  `timezone`. Error 2 for a name not in the list, error 4 when it could not
  be stored (nothing applied then). Result, and `shell.info.timezone`:
  `{zone, place, offset, stored}`. Event `shell.timezone` `{zone}` on every
  set. `pos call shell shell.timezone zone=Europe/Oslo`.

- `shell.debug_overlay` params `{enabled?}` (DS §52.6,
  ui/shell/shell_overlay.h): reads, or sets, the developer overlay, stored as
  `debug_overlay`. Error 2 for anything but a boolean, error 4 when it could
  not be stored. Result, and `shell.info.debug_overlay`: `{enabled, alive,
  text, refreshes, period_ms}` - `alive` whether its object and timer exist
  (never while off), `text` the line on show ("" while off).

- `shell.notify` params `{title, body?, source?, app?}` (ui/shell/shell_notice.h,
  app.h `pocketos_shell_notify`): a notice - a banner at the top of whatever
  screen is showing, gone by itself after five seconds - for something that
  has just happened. `title` is required; `body` is a preview (two lines of it
  are shown), `source` the caption over them (default `DOORS`), and `app` the
  id of the app a tap opens (nothing when absent; nothing while the lock is
  engaged). Error 2 when `title` is missing or a field is not a string. Not
  stored and not replayed: one at a time, at most four waiting. Result, and
  `shell.info.notice`: `{visible, posted, dropped, title, body}` - the newest
  one shown. `pos call shell shell.notify title="Backup finished" source=SYSD`.
  RIFT raises one for a new direct or channel message (docs/apps/RIFT.md,
  "Message banners").

- `shell.info` also carries `title` with an app open: the header's title, the
  app's name or the page it is on (app.h `pocketos_shell_set_title`).

Requests that act like a person - every method but `shell.info`,
`shell.audit`, `shell.screenshot`, `shell.subscribe`, `shell.unsubscribe`,
`shell.notify`, `shell.lock`, `shell.key` and `shell.tap` - count as activity for Power &
Sleep and wake a dark screen first. `shell.key` and `shell.tap` take the
physical key's and the finger's own path: while the screen is off they only
wake it.

- `shell.rotation` params `{mode?}`: reads, or with `mode` stores, the
  rotation mode (`automatic`, `portrait`, `landscape`; settings key
  `display_rotation`). The display is rotated when it is opened, so a change
  is applied by the shell opening it again: about a second after the call (a
  settle window) it re-executes itself in place, keeping its pid, and comes
  back on the launcher. Result: `{rotation_mode, rotation_mode_valid,
  rotation, orientation, next_rotation, next_orientation, applying, keyboard,
  bench_override, portrait_app}`: `rotation`/`orientation` are this run
  (degrees, `portrait`/`landscape`), `next_*` what the stored mode (or the
  `--rotation` one in a run given it, which every restart keeps) gives with
  the keyboard as it is now and the open app's hold, `applying` whether those
  differ and the shell is therefore about to restart itself, `keyboard`
  `unknown`/`absent`/`present` (`ui/shell/shell_kbd.c` probes the keyboard
  base at start-up and watches for one being attached or removed),
  `bench_override` whether `POCKETOS_DRM_ROTATION` decided instead, and
  `portrait_app` the id of the open app that runs in portrait only and holds
  the display there (DS §53; `timber`), or null. While an app holds it, a
  stored mode waits until the app closes. Any other `mode` is error 2;
  error 4 when it could not be stored. `pos call shell shell.rotation
  mode=landscape`.

- `shell.lock`: engages the lock screen (DS §31.4). Result `{locked}`. The
  lock is accidental-input protection and identity, not security: there is
  no code, and every service keeps running under it.
- `shell.unlock` params `{animate?}`: opens it; with `animate: true` the
  door sequence plays (about 0.8 s), otherwise it opens at once. Result
  `{locked}` (still `true` while an animated open runs). `shell.open` and
  `shell.home` also open the device, without the sequence: a bench or a
  script asking for an app is acting for the owner.
- `shell.controls` params `{show?}`: shows DOORS Controls over the launcher
  (DS §31.5), or with `show: false` closes it. Result `{controls}`; `show`
  that is not a boolean is error 2.
- `shell.folder` params `{id}`: opens the launcher folder `id` ("apps",
  "utilities", "games"; DS §39, §42, §47) at home - going home first from an app or
  Controls - or, with `id: ""`, goes back to the launcher's own page (from
  a folder, or from the favorite picker). Result `{folder}`, the open
  folder's id or null. An id that is not a string is error 2; a folder
  that does not exist or has no app installed is error 2 and changes
  nothing. `pos call shell shell.folder id=games`.
- `shell.favorite` params `{slot, id}` or `{slot, pick: true}`: the
  launcher's favorites (DS §42), slots 1 to 3. `id` an installed app's id
  gives the slot that app, `id: ""` clears it; either is kept in
  settings.conf as `launcher_favorite_<slot>` (cleared: the key is
  removed) and closes an open picker. `pick: true` opens the slot's picker
  at home, going home first from an app or Controls. Result `{favorites,
  picker}` as in `shell.info`. A slot that is not 1, 2 or 3, an id that is
  not a string, an app that is not installed, an app another slot holds
  (an app is a favorite once) and a picker with nothing to offer are error
  2 and change nothing. For the bench and the tests; a person long-presses
  the slot. `pos call shell shell.favorite slot=1 id=rift`.
- `shell.info` also carries the DOORS environment: `lock` `{locked, opening,
  engaged, opened}` (counts since start), `launcher` `{groups, apps,
  icons_art, icons_fallback, cell_width, scrolls, controls, cells, folders,
  folder, hub, hub_lobby, home_cells, folder_cells, favorite_cells, favorites_set,
  favorites, picker, focus, focus_shown, keys}` - `cells` is every app's
  own cell on the page showing (the launcher's, the open folder's, or the
  picker's), `[{id, x, y, w, h}]`, never a favorite's; `folders` is every
  folder, `[{id, name, apps, x, y, w, h}]`, with the rectangle while its
  cell is on the screen; `folder` the open one or null; `hub` whether the
  open folder's page is the Games Hub (DS §60: Games, with its nine games
  and its art; `cells` are then the posters) and `hub_lobby` `{x, y, w, h}`,
  the Multiplayer Lobby preview while the hub shows; `home_cells` the
  launcher's app and folder cells and `folder_cells` how many of them are
  folders; `favorite_cells` the favorite slots before them (3),
  `favorites_set` how many hold an installed app, `favorites`
  `[{slot, id, stored, x, y, w, h}]` (`id` the installed app or null,
  `stored` what settings.conf holds or null, the rectangle while the
  launcher's page shows); `picker` the slot whose picker is open, or null;
  `focus` the id the keys are on ("favorite-N" on a slot, "clear" on the
  picker's Clear), `focus_shown` whether its mark is drawn, `keys` whether
  the launcher has the keys - and `art` `{dir, background, files_read,
  bytes_held}` (ui/shell/art.h).
  `pos call shell shell.lock`, `pos call shell shell.unlock animate=true`.

- `shell.tap` params `{x, y, hold_ms?}` or `{text, hold_ms?}`, in the
  simulator only (`POCKETOS_SHELL_TEST_HOOKS`; the panel's build has no such
  method): a finger on the screen through a pointer of the simulator's own,
  pressed for `hold_ms` (default 80, up to 5000 for a long press) and lifted.
  With `text`, the clickable object holding the first shown label reading
  exactly that is scrolled into view and tapped in its middle. Result
  `{tapped, x, y}`; no such text, no point or a tap still running is error 2.
  For the suites: `pos call shell shell.tap text=System`.

## Methods served by an app

An app inside the shell may serve methods of its own on this socket while it
is in front (`ui/shell/app.h`: `ipc_prefix`, `ipc_request`, `ipc_status`,
`ipc_opens`; the rules are `ui/shell/app_ipc.c`). **One app of the product
serves one method:** PacketShark's `capture.status` (below). The only other
is the probe of the simulator's tests (`ui/shell/app_probe.c`, present only
when `POCKETOS_TEST_PROBE` is set in a simulator build), which is what
`tests/shell_app_methods_test.sh` asks.

- **Names.** An app's methods all begin with its prefix: lower-case words
  ending in a dot. A prefix is never in the shell's own names (`shell.`) and
  never overlaps another app's. The shell checks every app's declaration when
  it starts and exits with status 2, naming the fault, rather than serve an
  unsound one. A name under no app's prefix is error 1, as before.
- **In front** means the app is the open one with neither the lock nor an
  alarm over it. A dark screen that is not locked, Controls or the power menu
  over the app do not change that. Then
  the request goes to the app's `ipc_request` and its answer is the answer:
  a result, or one of pocketipc's error codes with the app's own words. A
  code that is not pocketipc's is error 4.
- **Not in front.** A method the app declared in `ipc_status` is answered by
  the shell with `{"available": false}` and nothing else. Every other method
  is error 3, "<App> is not open": a request to do something is never
  answered as if it had been done.
- **Opening.** A method the app declared in `ipc_opens` opens the app first,
  as a person opening it would (the screen wakes), and is then answered by
  it. It is refused with error 3 while the unit is locked or an alarm shows,
  and never opens the lock (unlike `shell.open`); error 5, with nothing
  touched, when the display would have to be turned for the app first (a
  portrait-only app in landscape: the shell would restart, and the caller
  with it). **Only the probe
  declares one.** Which method of a real app may open it from outside the
  unit is a decision of its own (ADR-016 amendment 1, proposed).
- **Waking.** Apart from an opening method that is let through, an app method
  neither wakes the screen nor counts as activity.
- **Time.** A handler runs on the LVGL thread and must answer from memory. It
  has a budget of 2 ms (`APP_IPC_BUDGET_US`). The shell measures every call,
  counts and logs the ones over it, and reports them; it does not interrupt
  a handler, which cannot be done safely.
- **Size.** A result is at most 8192 bytes serialised
  (`APP_IPC_RESULT_MAX`), for every app; a larger one is replaced by error 4.
- **Events.** An app in front (so not under the lock or an alarm) may raise
  events under its own prefix
  (`pocketos_shell_app_event`), delivered like the shell's own to
  `shell.subscribe` listeners. Any other name is dropped and counted.

`shell.info` carries `app_ipc`: `budget_us`, `result_max`, `calls`, `slow`,
`worst_us`, `worst_method`, `too_large`, `closed_status`, `closed_refused`,
`opened`, `open_refused`, `events`, `events_dropped` - counts since the
shell started.

### `capture.status` (PacketShark)

The state of PacketShark's capture and its aggregate figures, read from the
model the app already holds (`apps/packetshark/psk_remote.[ch]`). It reads
and nothing else: it starts no capture and no helper, sends the helper
nothing, changes nothing on the screen and logs nothing. It takes no
parameters.

**Reach.** This is a method on the unit's own socket. Nothing passes it to a
paired phone: gatewayd knows no `capture.*` name, and letting a companion
read it is ADR-016 amendment 1, which is proposed and not accepted.

While PacketShark is not in front (closed, behind the lock, under an alarm)
the answer is `{"available": false}` and nothing else (also in front, should
the app have failed to start). In front:

```json
{"available": true, "state": "capturing", "reason": "none", "iface": "eth0", "keep": "headers",
 "elapsed_ms": 61250, "limit_s": 900, "seen": 48211, "stored": 48211, "held": 0,
 "kernel_drops": 0, "iface_drops": 0, "shed": 0, "bytes": 31022764, "pps": 812, "bps": 4120000,
 "fill_permille": 310,
 "classes": {"tcp": 39120, "udp": 5210, "dns": 1630, "dhcp": 4, "arp": 61, "icmp": 12, "ipv6": 2101, "other": 73},
 "findings": [0, 3, 0, 1, 0, 0],
 "diag": {"dns_queries": 815, "dns_retries": 4, "dns_answered": 800, "dns_late": 5, "dns_noreply": 6,
          "dns_waiting": 0, "dns_nxdomain": 2, "dns_servfail": 0, "dns_refused": 0,
          "dns_rtt_sum_us": 18400000, "dns_rtt_max_us": 950000, "tcp_attempts": 40, "tcp_connected": 38,
          "tcp_timed": 37, "tcp_hs_sum_us": 1900000, "tcp_hs_max_us": 120000, "tcp_refused": 1,
          "tcp_aborted": 2},
 "requested_by_companion": false}
```

- `state` is what the screen shows: `idle` (setup, and START tapped but the
  helper not yet capturing), `capturing`, `hold`, `saving`, `stopped`, or
  `ended` (the panel of a capture whose helper ended badly; the figures are
  then the last session's).
- `reason`: `none`; for `stopped` and `saving` why it stopped, `user`,
  `limit`, `link`, `end` (a replayed file ended) or `error` (the source
  failed); for `ended`, `helper_hung` (it stopped answering and was killed)
  or `helper_crashed` (it is gone in any other way).
- In `idle` there is no capture: `iface` is empty, `keep` is `headers` and
  every figure is zero, whatever was chosen on the setup screen.
- `iface` is the capture's interface when its name is `eth`, `wlan`, `usb`
  or `synth` and one or two digits; any other name is sent as `""` (a name
  can be chosen, and the kernel's `enx...`/`wlx...` names are a hardware
  address).
- `keep` (`headers` or `full`) and `limit_s` (0: none) are what the capture
  command asked for, kept when it was sent.
- Figures are zero until the helper's first figures arrive;
  `fill_permille` is never over 1000.
- `pps` and `bps` are zero unless the state is `capturing` or `hold`.
- `classes` counts frames by PacketShark's eight classes, `findings` its six
  findings in their order (retransmission, TCP reset, DNS failure,
  unreachable, unanswered SYN, unanswered DNS query), `diag` totals and times
  of the DNS and TCP diagnosis, all since START.
- Every figure is a whole number, a count, a rate, a size or a time, never
  over 999,999,999,999,999. `requested_by_companion` is always `false`: no
  companion can ask for a capture.

**What is never in it:** a packet or a byte of one, a row of the list, the
inspected packet, an address, a host or DNS name, a port, a packet's
sequence number (the diagnosis' "slowest answer" is a packet, and stays), a
saved capture's name, the helper's version or its own words. The answer is
made from a structure that has room for none of these, and
`tests/packetshark_lint.sh` holds the list of the model's members that may
be read into it and the list of every key and word it can carry.

`docs/api/gateway.md` section 9 (on the companion branches) is the proposed
contract for a phone and is not changed by this. It differs in `reason`: it
lists `left`, `refused` and `timeout`, which belong to a start request and to
events and do not exist here, and it does not list `user`, `end` and `error`.
The two are to be made one when the later steps are decided.

No other `capture.*` method exists. In front, any other name under the
prefix is error 1; not in front it is error 3, "PacketShark is not open".
No method opens PacketShark, and it raises no event. Tests:
`tests/psk_remote_test.c`, `tests/capture_status_shell_test.sh`,
`tests/packetshark_app_test.c`.

## Events

`shell.app` `{current}` when the visible app changes, `shell.theme`
`{theme, mode}` after a theme or mode change, and `shell.rotation` (the
`shell.rotation` result) when the stored mode or the keyboard's presence
changes, to subscribed clients (`shell.subscribe` / `shell.unsubscribe`).

## Security

Anyone who can connect to the socket can drive the UI. v0 relies on socket
permissions (root only). Screenshot paths are not restricted.
