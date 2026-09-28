# PG Blackjack

A Pocket Games title: single-player Blackjack against the dealer, on
PocketTimber's felt, in the same card family as PG Solitaire.
Branch `feature/game-blackjack`, from master `a2e10d1`. Not merged, not
stacked on the Solitaire branch. Physically tested on unit A at `189f7f7`
(2026-09-13, in the temporary combined build `test/pocket-games-triple`
`03a0fef`): PASS.

This is a game with play chips. There is no money, no purchase, no
networking and nothing that leaves the device.

## Ruleset

One ruleset (`apps/blackjack/engine/bj_rules.h`):

- **Shoe:** 2 standard decks (104 cards), shuffled from a seed. Before a round
  is dealt, a shoe with fewer than 26 cards left is replaced by a freshly
  shuffled full one ("NEW SHOE" on the caption). If a shoe ever runs out
  mid-round, every card not on the table is shuffled back in, so no card is
  ever in two places.
- **Values:** 2-10 face value, J Q K 10, ace 1 or 11. One ace counts 11 when
  that does not pass 21 ("soft").
- **Deal:** player, dealer, player, dealer; the dealer's second card face down.
  The bet is taken on the deal.
- **Naturals:** the dealer checks for blackjack at once. Both have one: push.
  Player only: pays **3:2**. Dealer only: the bet is lost before the player
  acts (so a double is never lost to a dealer natural). **No insurance.**
- **Player:** hit or stand. Over 21 busts and loses at once; exactly 21 stands
  by itself. **Double down** on the first two cards only, if the bankroll
  covers a second bet: one card, then stand.
- **Dealer:** reveals, draws while under 17, **stands on all 17s (soft 17
  included)**.
- **Settle:** higher total wins 1:1; equal totals push; a dealer bust pays a
  standing hand.
- **Not in this version:** split, surrender, insurance, side bets, multiple
  hands or seats.

**Chips:** bankroll starts at 1000; bet 10-200 in steps of 10, never above
the bankroll (so 3:2 is always whole chips). Below 10 chips: "OUT OF CHIPS"
and a NEW BANKROLL button. The session, bankroll included, is kept across
leaving and reopening the app (Persistence).

**On screen:** the HUD's two panels read **BANK** (chips not on the table)
and **BET** (the chips on the table during a hand, otherwise the next
round's bet) in DS hero-40 numbers. Between rounds the bet buttons say what
they do: **BET −10** and **BET +10**. A settled round's result is shown large
in the open felt between the hands: `+50` in the success token, `−50` in
`text_secondary`, `PUSH` in `text_primary` - the sign carries the meaning,
the colour only repeats it. Added after the Unit A test (2026-09-13), where
the smaller "BANKROLL"/"BET" title-size HUD and bare − / + buttons were not
clear enough.

## Controls

Keyboard first (one logical key stream, DS 17.4); the buttons do the same
things and never take the focus. The table takes no taps.

| Phase | Keys | Buttons |
| --- | --- | --- |
| Between rounds | Enter, Space, `n`: deal | DEAL (first round) / NEW ROUND (accent) |
| | Left, Down, `-`: bet -10 | BET −10 (off at the minimum) |
| | Right, Up, `+`, `=`: bet +10 | BET +10 (off at the limit) |
| Your hand | `h`, Enter: hit | HIT (accent) |
| | `s`, Space: stand | STAND |
| | `d`: double down | DOUBLE (off unless allowed) |
| Out of chips | Enter, Space, `n` | NEW BANKROLL (accent) |

Letters in either case. A refused action says why ("DOUBLE ONLY ON YOUR FIRST
TWO CARDS", "NOT ENOUGH CHIPS TO DOUBLE", "THE MAXIMUM BET IS 200", ...).
Results are in words on the caption ("BLACKJACK · +15", "DEALER BUSTS · +50",
"PUSH · BET RETURNED", "BUST · −50"); the player's label is also drawn in the
success token after a win, never as the only signal.

## Architecture

```
apps/blackjack/engine/bj_cards.[ch]      card, rank/suit/colour, deck, shuffle  (copy of Solitaire's, bj_ prefix)
apps/blackjack/engine/bj_rng.[ch]        xorshift32                              (copy)
apps/blackjack/engine/bj_rules.[ch]      shoe, hands and totals, deal/hit/stand/double, dealer, settlement, chips, bj_game_valid
apps/blackjack/ui/bj_view.[ch]           pure: screen blocks, table and hand geometry, key map, buttons, HUD, labels, result, captions
apps/blackjack/ui/bj_card_draw.[ch]      LVGL: face (with index layout), back, slot, suits  (copy + one parameter)
apps/blackjack/ui/bj_table_widget.[ch]   LVGL: felt, both hands, labels, the round's result
apps/blackjack/ui/bj_felt.[ch]           the one door to PocketTimber's felt
apps/blackjack/ui/bj_palette.h           card world palette (values generated from art/)
apps/blackjack/art/cards_palette.txt     the card colours, as art (same values as Solitaire's)
apps/blackjack/bj_store.[ch]             the save file: codec, atomic write, validated load (the only file I/O)
apps/blackjack/bj_app.[ch]               LVGL: HUD, table, controls, input plumbing, when to save
```

The rules are pure C with integer chip arithmetic. The view model is also
LVGL-free, so every key, button state, label and caption is host-tested.
`tests/bj_lint.sh` holds the layering and the scope (no split, insurance,
surrender or side bets).

**Branch independence:** the card primitives, suit shapes, renderer, palette
manifest and its CMake generation are **copies** of PG Solitaire's under a
`bj_` prefix, so this branch builds and merges on its own, and both branches
can be merged together without clashing symbols.

### Look

Same family as PG Solitaire: Timber's felt tile through `timber_art_felt()`
(base-tone fallback without Timber's art), a hairline DS panel around it,
cards drawn as shapes in the card world palette (warm paper, deep red, near
black, timber backs with a Timber-wood frame), Night dimming the world by
half `bg`, and every interface element (HUD, labels, the result, caption,
buttons) in tokens. Cards are 128 x 179 px in portrait; hands overlap by 56 % of a card
and tighten when long, so even a 16-card hand fits. Before the first round
the felt shows outline slots for both hands.

### Deviations pending the owner

- **D1 - card world palette**, as PG Solitaire's D1 (docs there): cards need
  paper and red/black ink no theme token provides; kept as art in
  `cards_palette.txt`, generated into the build, Timber's "world is not
  themed" precedent. `style_lint.sh` untouched and passing.
- **App-to-app reuse:** `bj_felt.c` reads PocketTimber's felt, one file,
  lint-enforced, as in Solitaire.
- **D2 - app-owned storage.** The session is written by the app itself
  (`bj_store.c`) under `$POCKETOS_STATE_DIR`, as PocketFleet's approved D2
  (approved for Fleet only), PG 2048 and PG Solitaire do. No platform
  game-state service exists. Refused, Blackjack would go back to a bankroll
  that lasts one session.

No touch-size deviation: Blackjack's only touch targets are 64 px buttons.

## Persistence

Added after the Unit A test (2026-09-13): leaving and reopening resumes the
session **exactly**, a hand in play included.

`$POCKETOS_STATE_DIR/blackjack/game.v1` (default
`/var/lib/pocketos/blackjack/`), `apps/blackjack/bj_store.[ch]`, the app's
only file I/O. 181 bytes little-endian, always:

| Offset | Field | Size |
| --- | --- | --- |
| 0 | magic `PGBJ` | 4 |
| 4 | version 1 | 2 |
| 6 | bankroll | 4 |
| 10 | bet (the next round's) | 4 |
| 14 | stake (on the table this round) | 4 |
| 18 | last result, signed | 4 |
| 22 | rounds | 4 |
| 26 | phase, outcome, doubled, new shoe | 4 |
| 30 | shoe generator state | 4 |
| 34 | shuffles | 4 |
| 38 | shoe: count, 104 card slots | 105 |
| 143 | player: count, 16 card slots | 17 |
| 160 | dealer: count, 16 card slots (the hole card is a card like any other) | 17 |
| 177 | FNV-1a of bytes 0..176 | 4 |

Slots past a count are `FF` and must read back so; a game has exactly one
encoding.

- **Kept:** the whole game. The shoe's cards in order and its generator, so
  a resumed hand gets the same cards it would have got, and a resumed
  session goes on exactly as if it had never been left. Leaving cannot
  change a round, and it cannot move a chip.
- **Not kept:** the last refused command and its caption.
- **When:** a command the rules accept (bet, deal, hit, stand, double, new
  bankroll) marks the game changed; the shell's 1 Hz tick saves it; a round
  that settles is saved at once; closing the app saves. A new session nobody
  touched writes nothing. No write per key.
- **Chips across a crash:** the file always holds a whole position the rules
  reached. Losing power before a save resumes the last saved position, at
  most a tick old - a stake not yet taken still in the bankroll, the same
  cards still in the shoe - so chips are never lost or duplicated; a settled
  round is on disk before its result is shown.
- **Refusal:** wrong length, magic, version, checksum or padding, or a
  position `bj_game_valid()` rejects, is refused whole - nothing restored -
  left in place, shown as "SAVED GAME UNREADABLE · NEW BANKROLL", and
  replaced after the first command. `bj_game_valid()` refuses: a card that
  does not exist, or more often than two decks hold it across shoe and
  table; a bet off the step or out of range, or above the chips between
  rounds; cards, a stake or a result before a round; a live hand that should
  have settled (21 or more, a dealer blackjack, a dealer who has drawn, a
  double) or whose stake is not the bet; a settled round whose outcome the
  hands contradict, whose result is not what the outcome pays on the stake,
  or a double without exactly one extra card; a stuck generator.
  The bankroll itself cannot be checked - any amount is a possible session.
- **Writes:** temp file, fsync, rename. A failed save shows "NOT SAVED ·
  PLAY CONTINUES" (a refusal is still explained first); the next tick tries
  again.
- Review states (`PGBLACKJACK_SCREEN`) never read or write the file.
- Starting over with 1000 chips while chips remain is not offered (NEW
  BANKROLL appears only when out of chips), as before.

## Orientation

Portrait and landscape support is planned for the next platform milestone;
nothing here implements it.

Ready for it: rules and view never see a panel size (lint fails on `568`,
`1232`, `1176`, `1060`). The root places HUD, table and controls from
`bj_view_screen(w, h)` on every size change - stacked when tall, table left
with HUD and controls right when wide. The table sizes cards from both
dimensions (a quarter of the width, half the height less labels) and hands
tighten their overlap to fit, so a wide, short table gets height-bound cards
and still holds the longest hand. Covered by `tests/bj_view_test.c`, never on
glass. The dealer-above-player arrangement is kept in both; a side-by-side
dealer/player arrangement may suit landscape better and would be a view change
only.

## Known limitations

- Card legibility at 128 px and the full-table redraw per action are
  unmeasured (no motion, so no frame-rate concern); the first Unit A test
  (2026-09-13) found the game sound and asked for the clearer BANK/BET and
  save/resume now in place; the second test that evening passed both.
- No motion: the dealer's cards appear at once when the player stands. A
  paced reveal would read better and is future work (it must honour reduced
  motion).
- No statistics. The felt shows a result only where the gap between the
  hands is taller than the number (always in portrait; a short, wide table
  may leave it to the caption).
- Launcher capacity: 12 apps with this branch, which fits; merging another
  game branch too needs a launcher decision.
- Icon `LV_SYMBOL_IMAGE` until the DS icon set exists.
- Review states: `PGBLACKJACK_SCREEN=bet|play|win|blackjack|bust|broke` (fixed
  seeds). Host renders: `docs/design/shots/pgblackjack-*.png`.

## Tests

| Test | What |
| --- | --- |
| `tests/bj_rules_test.c` (119) | totals: soft/hard aces, multiple aces, busts; naturals both ways and pushes; dealer 16/17 boundary including soft 17 and multi-card draws; hit, bust, auto-stand on 21, soft-to-hard; double (win, bust, refused late, refused unfunded); bet limits and bankroll clamping; broke and new bankroll; shoe composition, reshuffle mark, a shoe emptied mid-round refilled without duplication; 400 rounds never over-dealing a card; new-round cleanup; determinism; 20 000 random rounds conserving chips and cards with every outcome seen; `bj_game_valid`: every position of 60 000 random commands (dry shoes included) valid, and each kind of position broken one way at a time refused |
| `tests/bj_view_test.c` (91) | layout tall/wide (HUD stacked beside a wide table), hand geometry for 1-16 cards, hole card, the result area, key map per phase, buttons with their words and enabled states, commands, BANK/BET values in every phase, every label, result and caption, the save notes |
| `tests/bj_store_test.c` (132) | byte layout; exact round trip - same session, same chips, same cards, same bytes again - of a new session, a raised bet, a hand just dealt, a five-card hand in play, all seven outcomes, a doubled win and a doubled loss to nothing, a new bankroll, 300 rounds in, a shoe refilled mid-round; payout state and a negative result in the file; a resumed hand settles as the original and sixty rounds later is still the same session; every one of the 1448 single-bit corruptions refused with nothing restored; truncated, padded, and resealed files of another version, bad phase or outcome, bad flags, stuck generator, bet off step, stake not the bet, a result mid-hand, settled without outcome, counts past their slots, a card past a count, a card that does not exist, a card three times, a busted hand still live; never writing an invalid game; the file: missing, nested directories, no temp left, resume, new session and new bankroll replace, truncated/padded/empty refused and left, unwritable directory, default path |
| `tests/bj_lint.sh` | layering, no colour in C, felt door, copy provenance, input rules, scope; the store as the only file I/O, atomic and validated, saved from the tick |
| `tests/bj_app_test.c` (129) | a stacked shoe played by keys and by taps, each step compared with the rules; bet keys ignored mid-hand; disabled DOUBLE; out of chips; the HUD's BANK and BET words and large numbers; layout and a 16-card hand in three modes; five themes; 3000 random inputs with chips conserved and every position saved; persistence (see the file's header: untouched session writes nothing, tick and settle saves, mid-hand leave and exact resume with the same caption, buttons and focus, playing on per the rules, doubled win, out of chips, new bankroll, damaged save, failed save with play continuing); review states never touching the save; five open/close rounds |
| `tests/bj_shell_test.sh` | runs the app test; launcher rows; generated palette; the real shell opens and closes every review state with no fault, and neither they nor an untouched session write; a damaged save opens cleanly and is left in place |

Writing the rules test found a real defect before it shipped:
`h->card[h->n++] = draw(g)` left it unspecified whether `n` grew before a
mid-round refill counted the table, and GCC grew it first, so the refilled
shoe was one card short. `give()` now draws in its own statement.

## Pocket Cards: what to extract later

Written after both card games, from what actually had to be copied:

- **Shared, verbatim:** the card byte and its rank/suit/colour/rank-text
  helpers; Fisher-Yates over xorshift32; the suit shapes; the card back; the
  empty slot; the card world palette and its CMake generation; the felt door.
- **Shared, with a parameter the second game forced:** the face renderer
  needs an **index layout** - rank beside pip for Solitaire's vertical fans,
  rank over pip for Blackjack's sideways overlaps. Poker hands would use the
  latter.
- **A deck that is really a shoe:** Blackjack needs N decks, reshuffle marks
  and refill-excluding-table; Solitaire needs one deck and no refill. A shared
  module should offer a multi-deck shoe with Solitaire as N = 1.
- **Per game:** piles/hands, rules, geometry (fans vs overlaps), interaction
  model, captions. Poker would add hand ranking, which neither game has.
- **Decide at merge:** a Pocket Games art/"table" module owning the felt (read
  today through Timber) and the palette, so no game reaches into another.
