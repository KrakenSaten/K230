# shell.* API v0

Status: draft, api_version 0. Transport: pocketipc, socket `shell.sock`.
Served by the PocketOS shell process. Intended for the `pos` CLI, tests and
developer tooling, not for applications.

## Methods

- `shell.info`: `api_version`, `apps` (array of `{id, name, page_of?}`;
  `page_of` names the app this one is a page of - System, of Settings, DS
  §47 - which has no launcher cell and whose way back is that app), `current`
  (open app id or `"home"`), `display`: `{width, height, backend}` - the
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

- `shell.rotation` params `{mode?}`: reads, or with `mode` stores, the
  rotation mode (`automatic`, `portrait`, `landscape`; settings key
  `display_rotation`). The display is rotated when it is opened, so a change
  is applied by the shell opening it again: about a second after the call (a
  settle window) it re-executes itself in place, keeping its pid, and comes
  back on the launcher. Result: `{rotation_mode, rotation_mode_valid,
  rotation, orientation, next_rotation, next_orientation, applying, keyboard,
  bench_override}`: `rotation`/`orientation` are this run (degrees,
  `portrait`/`landscape`), `next_*` what the stored mode gives with the
  keyboard as it is now, `applying` whether those differ and the shell is
  therefore about to restart itself, `keyboard` `unknown`/`absent`/`present`
  (`ui/shell/shell_kbd.c` probes the keyboard base at start-up and watches for
  one being attached or removed), and `bench_override` whether
  `POCKETOS_DRM_ROTATION` decided instead. Any other `mode` is error 2;
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
  folder, home_cells, folder_cells, favorite_cells, favorites_set,
  favorites, picker, focus, focus_shown, keys}` - `cells` is every app's
  own cell on the page showing (the launcher's, the open folder's, or the
  picker's), `[{id, x, y, w, h}]`, never a favorite's; `folders` is every
  folder, `[{id, name, apps, x, y, w, h}]`, with the rectangle while its
  cell is on the screen; `folder` the open one or null; `home_cells` the
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

## Events

`shell.app` `{current}` when the visible app changes, `shell.theme`
`{theme, mode}` after a theme or mode change, and `shell.rotation` (the
`shell.rotation` result) when the stored mode or the keyboard's presence
changes, to subscribed clients (`shell.subscribe` / `shell.unsubscribe`).

## Security

Anyone who can connect to the socket can drive the UI. v0 relies on socket
permissions (root only). Screenshot paths are not restricted.
