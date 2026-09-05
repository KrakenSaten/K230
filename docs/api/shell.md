# shell.* API v0

Status: draft, api_version 0. Transport: pocketipc, socket `shell.sock`.
Served by the PocketOS shell process. Intended for the `pos` CLI, tests and
developer tooling, not for applications.

## Methods

- `shell.info`: `api_version`, `apps` (array of `{id, name}`), `current`
  (open app id or `"home"`), `display` (`{width, height, backend}`).
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

## Events

`shell.app` `{current}` when the visible app changes, and `shell.theme`
`{theme, mode}` after a theme or mode change, to subscribed clients
(`shell.subscribe` / `shell.unsubscribe`).

## Security

Anyone who can connect to the socket can drive the UI. v0 relies on socket
permissions (root only). Screenshot paths are not restricted.
