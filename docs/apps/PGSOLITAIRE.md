# PG Solitaire

A Pocket Games title: Klondike solitaire on PocketTimber's felt.
Integrated for Doors v0.2.x on `feat/games-solitaire-blackjack-2048` (from
v0.2.1 `c9c2105`, 2026-09-28): imported byte for byte from `feature/game-solitaire` `62f5413`
(physically tested on unit A at `040955e` on 2026-09-13, in the temporary
combined build `test/pocket-games-triple` `03a0fef`: PASS), then brought up
to the current frame - see "Doors integration" below. Not merged; not yet
on glass in this form.

## Ruleset

One variant, used everywhere (`apps/solitaire/engine/sol_rules.h`):

- One standard 52-card deck, shuffled from a 32-bit seed (Fisher-Yates over
  xorshift32).
- Deal: seven tableau columns of 1..7 cards, only the top card face up; the
  other 24 form the stock, face down.
- **Draw-1.** A draw turns one stock card onto the waste. When the stock is
  empty the waste is turned over to become the stock again, dealing the same
  cards in the same order. **Passes are unlimited.** Draw-1 was chosen as the
  approachable default the brief preferred; draw-3 is future work.
- Only the waste's top card plays.
- Foundations: one fixed slot per suit (spades, hearts, clubs, diamonds, so the
  row alternates black/red). Each takes its ace, then that suit ascending, one
  card at a time.
- Tableau: a card or a face-up run goes onto a card one rank higher of the
  opposite colour. An empty column takes only a king or a king-led run. Any
  face-up run that descends in alternating colours moves as a whole.
- A move that uncovers a face-down tableau card turns it face up.
- A foundation's top card may come back down onto the tableau.
- Won when all 52 cards are home.
- MOVES counts card moves, draws and turns of the waste.

Not in this version: scoring beyond the move count, undo, auto-complete,
draw-3, timer.

## Controls

Keyboard first, one logical key stream (DS 17.4); touch beside it. There is
no drag: a card is picked up and put down, which the keyboard and a finger do
the same way.

| Key | Touch | Does |
| --- | --- | --- |
| Left / Right | - | cursor along its row: stock, waste, 4 foundations; or 7 columns |
| Up / Down | - | in a column, up/down its face-up cards (chooses how much of a run); Up past the first face-up card goes to the top row, Down from the top row to the column below |
| 1 .. 7 | - | cursor to that column's top card |
| Enter, Space | tap a card | pick up the card and the run below it |
| Enter, Space | tap another pile | put the selection down there (anywhere down a column counts) |
| Enter on the selected card | tap it again | put it back / send it to its foundation if one takes it |
| d | tap the stock | draw one, or turn the waste over |
| f | tap the selected card again | send to its foundation |
| Esc, Backspace | tap the felt | drop the selection |
| n | NEW GAME | new deal; asks first once the deal has a move (n again deals, Enter/Esc keeps playing) |
| Enter, n after a win | NEW GAME | new deal |

The keyboard cursor (four corner brackets in the focus token) appears when a
key is used and hides on the next tap. The selection is a 3 px focus-token
outline around the whole run. They differ in shape, not only colour (DS 2).
Refused moves say why on the caption ("SAME COLOUR · ALTERNATE RED AND BLACK",
"ONLY A KING GOES TO AN EMPTY COLUMN", ...) and keep the selection. Neither
the table nor the buttons take the focus when tapped. Tab is not used
(KNOWN_ISSUES: LV_KEY_NEXT is consumed by LVGL).

## Architecture

```
apps/solitaire/engine/sol_cards.[ch]      card, rank/suit/colour, deck, shuffle   (Pocket Cards candidate)
apps/solitaire/engine/sol_rng.[ch]        xorshift32                              (Pocket Cards candidate)
apps/solitaire/engine/sol_rules.[ch]      Klondike draw-1: deal, pick, move, draw, foundation target, validity
apps/solitaire/ui/sol_view.[ch]           pure: screen blocks, table geometry, fans, hit test,
                                          cursor/selection state machine for keys and taps, captions
apps/solitaire/ui/sol_card_draw.[ch]      LVGL: face, back, slot, suit shapes    (Pocket Cards candidate)
apps/solitaire/ui/sol_table_widget.[ch]   LVGL: felt, piles, selection, cursor; tap -> hit
apps/solitaire/ui/sol_felt.[ch]           the one door to PocketTimber's felt
apps/solitaire/ui/sol_palette.h           card world palette (values generated from art/)
apps/solitaire/art/cards_palette.txt      the card colours, as art
apps/solitaire/sol_app.[ch]               LVGL: HUD, table, controls, input plumbing
```

The rules know piles, not pixels. The view model is also LVGL-free, so the
whole interaction model - every key, every tap, every refusal and caption -
is host-tested, including a 200 000-step random key/tap session that must
never leave the game invalid or the selection pointing at something
unpickable. `tests/sol_lint.sh` enforces the layering.

### Look

- **Felt:** PocketTimber's `felt_tile.png` (128 x 128, base #1B3A2A, fibrous
  noise), tiled over the table exactly as Timber tiles it, through
  `timber_art_felt()`. Not recreated. If the shell is built without Timber's
  art the table fills with the felt's base tone. Like Timber's viewport, the
  felt is confined to a hairline DS panel; everything outside it is chrome in
  tokens.
- **Cards** are world objects, like Timber's wood: drawn as shapes (rounded
  paper, 1 px edge, rank in the app-title role's font, suit built from
  triangles and discs - the product fonts have no suit glyphs), so they are
  sharp at any size the layout gives them. Court cards show their letter large;
  aces a large pip. Backs are dark timber with a frame in Timber's base wood
  tone (#C9A670) and one small diamond - the restrained wood/bronze accent.
- **Colours:** interface marks (selection, cursor, HUD, captions, buttons) are
  tokens. Card paper, ink, backs and slot outlines are the card world palette,
  `apps/solitaire/art/cards_palette.txt`, turned into `sol_palette.c` in the
  build tree by `ui/shell/CMakeLists.txt` (configuring fails if an entry is
  missing). No colour is written in the game's C sources; `style_lint.sh` is
  untouched and passes.
- **Night** dims the world with half of `bg` over the table, the design
  PocketTimber's art review recorded; interface marks draw after it in the
  night tokens.
- 68 x 95 px cards in portrait (seven columns across 528 px); face-down fans
  11 px, face-up fans 31 px (rank and pip always visible). The longest legal
  column (6 down + 13 up) fits without compression.

### Deviations pending the owner

- **D1 - card world palette.** DS 4/8 say apps use tokens only. Playing
  cards need paper, black and red that no theme token provides (the only red
  is `status_error`, reserved for errors by DS 2). This follows PocketTimber's
  approved "the world is not themed" ruling, with the values kept as art
  rather than as C literals. Refused, the fallback would be token paper
  (`text_primary`) and ink (`text_on_accent`) with red suits distinguished by
  shape only.
- **D2 - fanned cards under the 64 px touch minimum.** A face-up card in a
  column exposes a 31 px strip. As PocketFleet's approved D1 does for its
  grid: a tap there only *selects* (harmless, correctable, shown on the
  caption); the committing tap is on a destination, whose target is the whole
  column (68 px wide, full height) or a full 68 x 95 slot. Unmeasured on glass.
- **App-to-app reuse:** `sol_felt.c` calls PocketTimber's `timber_art_felt()`.
  Apps otherwise do not depend on each other (the RNG precedent duplicates
  instead). Chosen because the brief asked for Timber's actual felt, and
  duplicating 49 kB of it per card game was worse. One file, lint-enforced.
- **D3 - app-owned storage.** The saved game is written by the app itself
  (`sol_store.c`) under `$POCKETOS_STATE_DIR`, as PocketFleet's approved D2
  (approved for Fleet only) and PG 2048 do. No platform game-state service
  exists. Refused, Solitaire would go back to no save/resume.

## Persistence

Added after the Unit A test (2026-09-13): leaving and reopening resumes the
same deal.

`$POCKETOS_STATE_DIR/solitaire/game.v1` (default
`/var/lib/pocketos/solitaire/`), `apps/solitaire/sol_store.[ch]`, the app's
only file I/O. 101 bytes little-endian, always, because every card is in
exactly one pile:

| Offset | Field | Size |
| --- | --- | --- |
| 0 | magic `PGSL` | 4 |
| 4 | version 1 | 2 |
| 6 | seed | 4 |
| 10 | moves | 4 |
| 14 | passes (waste turned over) | 4 |
| 18 | won | 1 |
| 19 | 13 piles (stock, waste, 4 foundations, 7 columns): card count, face-down count | 26 |
| 45 | the 52 cards, pile by pile, bottom first | 52 |
| 97 | FNV-1a of bytes 0..96 | 4 |

- **Kept:** the exact game - every pile, card order, face-down counts, so the
  stock and waste (and therefore the next draw) come back as they were;
  seed, moves, passes, won.
- **Not kept:** the cursor, a card in hand, a pending question, a caption. A
  resumed deal starts with nothing selected and the cursor at column 1.
- **When:** a move, draw or new deal marks the game changed; the shell's
  1 Hz tick saves it; a win is saved at once; closing the app saves. Opening
  a fresh deal and leaving untouched writes nothing. No write per input.
- **Resume rules:** a valid unfinished game is resumed. A won game is not: a
  new deal is dealt (and replaces it on the next save). A missing file deals a
  new game.
- **Refusal:** wrong length, magic, version or checksum, or any position
  `sol_game_valid()` rejects (a card missing or twice, counts not adding to
  52, a foundation out of order, a face-down card on top of a column, won with
  cards out) is refused whole - nothing is restored - left in place, shown as
  "SAVED GAME UNREADABLE · NEW DEAL", and replaced by the next save.
- **Writes:** temp file, fsync, rename. A failed save shows "NOT SAVED · PLAY
  CONTINUES"; play continues.
- Review states (`PGSOLITAIRE_SCREEN`) never read or write the file.
- App-owned storage is deviation D3 above.

## Orientation

The shell gives a portrait or a landscape body (DS §21) and the app lays out
from `sol_view_screen(w, h)` on every size change: stacked when tall, table
left with HUD and controls right when wide. Landscape was first seen (in the
simulator) for this integration. One defect was found and fixed: the
caption's block in the 240 px side column was 104 px, so a long caption
("Q OF SPADES +8 SELECTED - TAP FELT TO CANCEL") wrapped over its top edge
and lost its first line. The controls block is now the whole column under the
HUD, bottom-aligned, and the caption wraps there; under a tall table it stays
one line with an ellipsis (a dotted LVGL label only cuts at a fixed height).

Landscape cards are about 85 x 119 px (portrait's are 68 x 95), bound by the table's
height; long columns compress their fan to stay on the felt, so a column of
thirteen face-up cards shows about 22 px of each. Keyboard navigation is by
logical rows and columns and does not change. A landscape arrangement with
the stock, waste and foundations in a column beside the tableau would give
the fans more room; that is a view change only, left as future work.

## Doors integration (2026-09-28)

What changed from the 2026-09-13 branch, and nothing else:

- **Registered** in the shell (`ui/shell/shell.c`) and shown by the launcher
  in PLAY after Fleet, Radar and Timber, in the games colour
  (`ui/shell/home_layout.c`).
- **Fullscreen** (`.chrome = POCKETOS_CHROME_NONE`), like Fleet, Radar and
  Timber: no status cluster over the game; the shell's header keeps the way
  back.
- **Icon**: a first-party launcher mask and portal icon (a column of cards,
  `docs/design/doors-app-icons/svg/solitaire.svg`), drawn like Zabbix's, Browser's
  and Vision's. `.icon` stays only as the text fallback. Place and icon are
  for the owner to confirm.
- **Landscape** (DS §21): see "Orientation". One defect fixed: a long caption lost its first line in the side column.
- **Tests**: the app test hosts the app in the current frame (the chrome
  height from `chrome.h`, not the retired status-bar constant) and gained a
  landscape section (`tests/games_frame.h`: every object inside the body,
  every button at least 64 px tall with its label whole); the shell suite runs
  every review state in both orientations; the lint checks the launcher
  table instead of the v0.0.9 tile grid.
- **Build**: `make games-test` runs the three games' host suites and lints on
  their own; the card palettes come from one CMake step
  (`games_card_palette`).

Lifecycle, checked for this integration: every LVGL object is under the body
the shell deletes; the root is the only object added to the focus group and
goes with it; there is no timer or animation; the table widget's state is freed on its delete event; the app state is one `lv_malloc` freed in `destroy`. The
save is a few hundred bytes, written with `fsync` from the once-a-second tick
only when something changed, at the end of a game and on close - the same
pattern as Notes, Clock and Radar - so the UI thread never waits on more than
one small file.

## Known limitations

- The unit A physical test (2026-09-13) passed by eye, save/resume included.
  Tap accuracy on 31 px strips, card legibility at 68 px, and drawing cost of
  a full table redraw per change remain unmeasured.
- No undo, auto-complete, draw-3 or scoring.
- Launcher: 21 apps with the three games, so the portrait launcher scrolls
  further (PLAY and DEVICE each take a second row). A Games folder is
  proposed separately on `feat/launcher-app-groups`.
- Review states: `PGSOLITAIRE_SCREEN=deal|selected|keyboard|confirm|won` (a
  fixed deal). Host renders: `docs/design/shots/pgsolitaire-*.png`.

## Tests

| Test | What |
| --- | --- |
| `tests/sol_rules_test.c` (96) | cards, 52 unique after shuffle, shuffle uniformity, deal, legal/illegal tableau moves incl. same colour and rank, runs, flips, kings to empty columns, foundations (suit, order, one card, back down), stock/waste incl. identical second pass and unlimited passes, win, 400 random games with no card lost or duplicated, invalid positions |
| `tests/sol_view_test.c` (101) | screen and table layout tall/wide, fan compression, hit testing, cursor navigation, pick/place/refuse/cancel by key, draw/turn/home/new game/confirm/won by key, the same by tap, captions, a 200 000-step random key/tap session |
| `tests/sol_store_test.c` (66) | byte layout; exact round trip of positions across a whole game (passes, a win); stock, waste, face-down counts and all 52 cards once; every single-bit corruption refused with nothing restored; truncated, padded, version 2, duplicate card, face-down top, bad counts and false win refused even when resealed; the file: missing, nested directories, no temp left, replace in place, damaged left in place, unwritable directory; a loaded game plays on exactly as the original; a new deal replaces the old |
| `tests/sol_lint.sh` | the structural rules above, and the store as the only file I/O, atomic and validated |
| `tests/sol_app_test.c` (118) | the app under the real key stream and a real pointer: keys and taps move cards exactly as the rules do, cancel, draw, the question and the win via buttons and keys, focus kept, 1500 random inputs, layout and the longest column in three modes, five themes; persistence: untouched deal writes nothing, tick saves, leaving mid-selection saves, reopening resumes card for card with the interaction reset and plays on per the rules, a new deal replaces the save, a won game is not resumed, a damaged save deals anew and is replaced after play, review states never write; five open/close rounds |
| `tests/sol_shell_test.sh` | runs the app test; launcher rows; generated palette; the real shell opens and closes every review state with no fault and writes nothing; a damaged save opens cleanly and is left in place |

## Pocket Cards: what to extract later

Solitaire is the first card game; Blackjack carries copies under its own
prefix. What looks shared after writing both:

- **Extract:** the card byte (rank/suit/colour, rank text), the 52-card deck
  with the seeded Fisher-Yates shuffle, the suit shapes, and the face/back
  renderer with the card world palette (and its CMake generation). These have
  no game rules in them and were copied verbatim.
- **Keep per game:** piles, rules, fans and hit testing (Solitaire's columns
  and Blackjack's hands have nothing in common), and the interaction model.
- **Decide then:** where the felt lives (a Pocket Games art module both Timber
  and the card games read), and whether a multi-deck shoe (Blackjack) belongs
  in the shared deck. Poker would need hand ranking, which neither game has.
