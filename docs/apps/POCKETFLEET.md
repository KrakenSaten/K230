# PocketFleet

A tactical naval game for PocketOS: Battleship on the 568 x 1232 panel,
local single player. Phase 1 only — no networking of any kind.

Status: phases P1 to P10 complete on branch `pocketfleet-engine`. Not merged.

## Layers

```text
apps/fleet/engine/   rules, RNG, AI, save codec, save file. Pure C, no LVGL.
apps/fleet/ui/       view model (no LVGL), local widgets, grid, four screens.
apps/fleet/fleet_app.c   the shell app entry, screen ownership, navigation.
```

The dependency runs one way: `fleet_app` → `ui/*` → `fleet_view` → `engine/*`.
The engine never calls up. It is built by the root `Makefile` so it is unit
tested natively, and again by `ui/shell/CMakeLists.txt` into the shell binary.

`tests/fleet_lint.sh` enforces the structure: the engine and the view model
contain no LVGL, `fleet_store.c` is the only file that touches the
filesystem, and `fleet_ai.[ch]` may not include `fleet_rules.h` or name
`struct fleet_board` / `struct fleet_game`.

## The no-cheat contract

The AI is handed its own shots and the results that were announced for them,
and nothing else. The hidden types are not visible in `fleet_ai.h`, so
reading the player's layout is impossible rather than discouraged.
`tests/fleet_ai_test.c` replays every game using only the announcements, with
no board present at all, and requires the shot sequence to match exactly.

Mean shots to clear the fleet, 300 games per level:
Recruit 95.4, Officer 69.5, Commander 51.3, Admiral 46.0.

## Determinism

A match is reproduced by `(seed, difficulty, the ordered list of player
actions)`. Two RNG streams are derived from the seed so the opponent's
decisions do not shift when the player uses auto-deploy. The save carries
both streams and the AI's record, so a resumed match makes exactly the
decisions it would have made.

## Approved deviations

### D1 — dense grid touch target (approved by the product owner, 2026-09-05)

DS §7 requires a 64 px minimum touch target. Ten columns in the 528 px
content width give 48 px cells; 64 px would need 640 px. The approved
interaction is **aim-then-confirm**:

- a tap on the target grid only moves the crosshair, which is harmless and
  correctable;
- the shot is committed by the FIRE button, full width and 64 px tall, and
  by nothing else;
- FIRE is not armed unless the crosshair sits on a square that has not
  already been fired at.

The global 64 px rule is unchanged. On the Deploy screen a tap places the
selected ship directly: placing is reversible, and the irreversible action
there is CONFIRM DEPLOYMENT, which is again a 64 px button.

### D2 — app-owned save file (approved by the product owner, 2026-09-05)

PocketOS has no `storage.*` service, and the shell's settings store is for
short non-secret preferences rather than game state. PocketFleet writes
`$POCKETOS_STATE_DIR/fleet/save.v1`, default
`/var/lib/pocketos/fleet/save.v1`, entirely inside `fleet_store.c`. That
`/var/lib` is writable on the K230 is DOCUMENTED from the Buildroot
defconfig and remains ASSUMED until hardware confirms it.

### D3 — no hero type on the Result screen (WITHDRAWN 2026-09-05)

Superseded by the platform: `POS_STYLE_HERO_40` exists, and the Result
heading uses it.

### D4 — disabled buttons are drawn as secondary (WITHDRAWN 2026-09-05)

Superseded by the platform: `POS_STYLE_BUTTON_DISABLED` exists, and
`fleet_button_set_enabled()` swaps the primary role for it.

### D5 — own hulls are outlined, not filled

DS §9's segmented meter suggests a bright fill for an "on" block. A ship on
your own grid uses `surface_raised` with a 2 px `text_secondary` border
instead. `surface_raised` is one RGB565 step from `surface` (DS feasibility
H1), so fill alone does not separate a hull from water, and DS §1 limits
large lit areas on the AMOLED. The border does the work.

### D6 — `accent_secondary` is unused

DS caveat C6 says to keep it for two-series charts and not to invent uses.
PocketFleet does not use it. The two series on the grid are carried by
`radio_tx` and `radio_rx`, which the DS guarantees are distinct hues, never
green or red.

## Colour contract

PocketFleet invents no colours. `tests/fleet_theme_test.c` states every
foreground/background pair the grid actually draws and holds all five themes
in all three modes to a floor. Worst ratio observed across the whole matrix:

| Pair | Tokens | Worst |
| --- | --- | --- |
| marks on your fire | `text_on_accent` on `radio_tx` | 3.34 |
| marks on incoming fire | `text_on_accent` on `radio_rx` | 3.68 |
| miss dot on a spent square | `text_secondary` on `bg` | 3.12 |
| grid captions | `text_secondary` on `bg` | 3.12 |
| own hull border on its fill | `text_secondary` on `surface_raised` | 2.80 |
| crosshair on open water | `focus` on `surface` | 3.17 |
| sweep over open water | `accent_primary` on `surface` | 3.17 |
| spent square border | `line` on `bg` | 1.26 |
| spent square against open water | `bg` on `surface` | 1.05 |

The last row is deliberately near 1: a spent square is told from open water
by its dot and its border, never by its fill. This is DS §2's rule that
colour never carries meaning alone, applied to the two states the panel
cannot separate on its own.

Cell language, with a shape behind every colour:

| State | Fill | Structure | Mark |
| --- | --- | --- | --- |
| open water | `surface` | — | — |
| aimed | `surface` | 2 px `focus` | — |
| spent, nothing there | `bg` | 1 px `line` | `text_secondary` dot |
| your hit | `radio_tx` | — | solid square |
| ship you sank | `radio_tx` | 2 px border | cross |
| your ship, afloat | `surface_raised` | 2 px `text_secondary` | — |
| enemy hit on you | `radio_rx` | — | solid square |
| your ship, sunk | `radio_rx` | 2 px border | cross |

`status_error` is never used in the game. It stays reserved for error state.

## Persistence behaviour

- The match is stored after every resolved turn and as soon as a deployment
  is confirmed.
- A finished match clears the slot, so Resume only ever offers an engagement
  that can still be played. Starting a new one supersedes the old.
- An absent, unreadable, damaged or rules-impossible save means there is
  nothing to resume: the app opens on a fresh match and logs a warning. The
  file is left in place rather than deleted, matching the settings store's
  fallback policy.
- The first failed write switches persistence off for the session rather
  than retrying every turn. The Command screen then says the engagement
  lasts for the session only, in `status_warn`: the game plays perfectly
  well without persistence, only Resume is gone.
- Writes are atomic (temp file, fsync, rename), so a power loss leaves
  either the previous save or the new one.
- The save holds a board layout and a shot history. Nothing in it is secret.
  Its FNV-1a checksum detects accidental corruption and is not a
  cryptographic digest; the rules validation, not the checksum, is what
  keeps an impossible match out of the game.

## Motion

| Event | Motion | Timing |
| --- | --- | --- |
| target grid idle | rotating radius with a four-step fading tail | 80 ms step, 6°, ~4.8 s per turn |
| shot resolves | ring on the cell widens and fades | 320 ms |
| opponent replies | paced after your own result | 420 ms |

Nothing blocks: the pause is a timer, taps keep moving the crosshair, and
FIRE is simply not armed while the enemy is firing. Leaving the screen or
closing the app settles a pending reply at once, so a turn is never left
half played. Only the grid is invalidated, never the screen.

With reduced motion there is no sweep, no ring and no pause: the whole
exchange is one instant state change (DS §12). The preference comes from the
platform accessor `pocketos_shell_reduced_motion()`, read once when the app
opens.

The grids are registered with `pos_theme_watch()`, so the theme engine
repaints them on a theme or mode change; a custom-drawn object is not
covered by `lv_obj_report_style_change`. The app falls back to subscribing
to `pos_event_theme_changed()` itself if the engine's watch table is full,
which is the contract that function documents.

## Development aid

`POCKETFLEET_SCREEN=command|deploy|battle|battle_paced|result` opens the app
on that screen from a fixed seed, so the simulator can render each one for
review the way the shell's own `--screenshot` does. It is inert unless set.
`battle_paced` takes a shot through the FIRE path so a headless run
exercises the paced reply and its teardown.

## Screenshots

`docs/design/shots/fleet-<screen>-<theme>[-<mode>].png`, four screens across
all five themes in Normal, plus `ice` in Outdoor and Night, and
`fleet-battle-reduced-motion.png` as the A/B for the sweep.

Regenerate one with:

```bash
POCKETFLEET_SCREEN=battle pocketos-shell --open fleet --theme ice --mode normal \
    --screenshot docs/design/shots/fleet-battle-ice.png --exit-after-ms 1000
```

## Tests

| Test | Covers |
| --- | --- |
| `tests/fleet_rng_test` | reproducibility, zero-seed guard, unbiased draws |
| `tests/fleet_rules_test` | deployment, firing, turn order, win, determinism |
| `tests/fleet_ai_test` | no-cheat replay, legality, termination, strategy, strength order |
| `tests/fleet_save_test` | codec round trips, impossible saves, resume equivalence, the file |
| `tests/fleet_theme_test` | the colour contract above, five themes by three modes |
| `tests/fleet_lint.sh` | the structural rules |
| `tests/fleet_shell_test.sh` | persistence and pacing in the running shell |
