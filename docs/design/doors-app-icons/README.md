# First-party Doors app icons

These are launcher icons for apps that none of the supplied packages has an
icon for. They are drawn in the line language of the Doors icon extension
(`../brand/doors-icon-extension`, read-only):

- a 24-unit `viewBox="0 0 24 24"`;
- 1.5-unit strokes with square caps and miter joins, in `currentColor`;
- no fill.

They are drawn in the repository, not supplied by the designer, and say so
wherever they are used. `tests/app_icons_test.sh` keeps them apart from the
owner-supplied artwork.

| File | Icon | App |
| --- | --- | --- |
| `svg/zabbix.svg` | a screen with a heartbeat trace and a stand: monitoring. Wave's waveform sits between two signal arcs; this one is framed and has a single sharp spike | Zabbix (DS §35.4, CONNECTIONS; chosen by the owner 2026-09-26) |
| `svg/vision.svg` | an eye: the almond outline with a round pupil, the plainest sign for seeing (drawn for the Vision prototype, docs/apps/VISION.md; DEVICE after Recorder, in the `ai` hue; place and icon for the owner to confirm) | Vision |
| `svg/browser.svg` | a globe: a circle with one meridian and the equator, the usual sign for the web (kept as light as the other icons: two more parallels made it the densest mask of all) | Browser (docs/apps/BROWSER.md, CONNECTIONS after Zabbix, in the network colour; confirmed by the owner 2026-09-27) |
| `svg/solitaire.svg` | a column of cards: the top edges of two cards above a whole one with a diamond pip, as a Klondike column fans | PG Solitaire (docs/apps/PGSOLITAIRE.md; PLAY after Timber, in the games colour; place and icon for the owner to confirm) |
| `svg/blackjack.svg` | a card with a diamond pip and a chip beside it: cards against the house (play chips only) | PG Blackjack (docs/apps/PGBLACKJACK.md; PLAY after Solitaire, games colour; for the owner to confirm) |
| `svg/2048.svg` | four tiles in a 2 x 2 grid, with gaps wide enough to stay apart at 32 px | PG 2048 (docs/apps/PG2048.md; PLAY after Blackjack, games colour; for the owner to confirm) |

**How the files are made:**

- **The SVG** is the master.
- **The PNGs.** `tools/design/render_app_icon.py svg/<name>.svg .` writes
  `png-24/` and `png-32/`, white on transparent like the extension's own
  exports, with the whole canvas scaled (strokes 1.5 and 2 px).
  - They are drawn by `tools/design/svgraster.py`, which draws every stroke
    with round caps; `svgraster.py` explains why the icon families accept
    that.
  - The output is deterministic, and `tests/app_icons_test.sh` checks that
    the committed PNGs are exactly what the SVG gives.
- **The mask.** `tools/design/gen_app_icons.py` turns `png-32/<name>.png`
  into the app's A8 mask in `ui/pocketui/pos_app_icons.c`.
- **The portal icon.** `tools/design/gen_doors_ui.py` draws the SVG into the
  B portal frame in the app's launcher colour, as
  `ui/assets/doors/icon-<name>.bin`.
