# shell.* API v0

Status: draft, api_version 0. Transport: pocketipc, socket `shell.sock`.
Served by the PocketOS shell process. Intended for the `pos` CLI, tests and
developer tooling, not for applications.

## Methods

- `shell.info`: `api_version`, `apps` (array of `{id, name}`), `current`
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
- `shell.screenshot` params `{path}`: renders the current screen to a PNG at
  `path` (on the device filesystem). Error 4 if it cannot be written.
- `shell.theme` params `{theme?, mode?}`: selects a Design System theme id
  (`ice`, `brass`, `olive`, `slate`, `carbon`) and/or display mode
  (`normal`, `outdoor`, `night`) live, no restart. An unknown id or mode is
  not an error: the shell falls back to `ice` + `normal` (DS §8) and the
  result carries `fallback: true` with a `reason`. Result: `{theme, mode,
  fallback, reason}`. `shell.info` also reports `theme` and `mode`.

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
- `shell.info` also carries the DOORS environment: `lock` `{locked, opening,
  engaged, opened}` (counts since start), `launcher` `{groups, apps,
  icons_art, icons_fallback, cell_width, scrolls, controls, cells}` -
  `cells` is every app's cell on the screen, `[{id, x, y, w, h}]` - and `art`
  `{dir, background, files_read, bytes_held}` (ui/shell/art.h).
  `pos call shell shell.lock`, `pos call shell shell.unlock animate=true`.

## Events

`shell.app` `{current}` when the visible app changes, `shell.theme`
`{theme, mode}` after a theme or mode change, and `shell.rotation` (the
`shell.rotation` result) when the stored mode or the keyboard's presence
changes, to subscribed clients (`shell.subscribe` / `shell.unsubscribe`).

## Security

Anyone who can connect to the socket can drive the UI. v0 relies on socket
permissions (root only). Screenshot paths are not restricted.
