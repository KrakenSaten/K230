<!-- Copyright (c) 2026 Doors authors. SPDX-License-Identifier: Apache-2.0 -->
# DOORS card games: redesign proposal

Blackjack, Poker (Texas Hold'em) and Solitaire (Klondike).
Status: **PROPOSED, awaiting the owner's choice of direction.** Nothing in
this branch changes the product: it holds this document, mockups and the
script that draws them. Baseline: master `a5d18bfc` (2026-11-22).

| Where | What |
| --- | --- |
| `mockups/board-directions.png` | the three directions, one Blackjack hand each |
| `mockups/board-before-after.png` | today's simulator captures beside the recommended direction |
| `mockups/*.png` (+ `.svg`) | twelve screens at panel pixels (568 x 1232 or 1232 x 568) |
| `mockups/deck-sheet.png` | the deck at its three sizes, chips, seats, buttons |
| `current/*.png` | 17 captures of today's games from the SDL simulator, this baseline |
| `tools/gen_mockups.py`, `tools/cg_kit.py` | draws and renders every mockup |

Evidence classes used below: **VERIFIED** (read in source or measured and
recorded in the repo), **DOCUMENTED** (stated in repo docs), **ASSUMED**
(my estimate; to be measured before it is relied on).

The mockups are design pictures drawn as SVG and rendered by a browser. They
are not simulator output and nothing in them has run on LVGL or on a unit.

---

## 1. Assessment of the current games

All three were read in source and captured from a fresh simulator build
(`current/`). The engines are the strong part: pure C, integer chips,
allocation-free, heavily tested (Blackjack 558 checks, Solitaire 507, Poker
over every five-card combination). The presentation is where they fall short.

**Shared weaknesses**

1. **No motion at all.** Each action replaces the picture. The docs say so
   ("there is no timer or animation"). A dealt card, a dealer's draw, a
   revealed flop and a card sent home all appear at once, so cause and effect
   are read from captions.
2. **The table is a rectangle of felt in a frame.** One 128 px felt tile
   borrowed from Timber, tiled flat, inside a hairline panel on a black page.
   There is no light, no edge and no object on it besides cards. Chips, the
   subject of two of the three games, are never drawn.
3. **Cards are placeholders.** One big pip or a letter in the UI font; courts
   are a bare "J", "Q", "K"; the back is a brown panel with one diamond. Suits
   are built from discs and triangles, which shows at 128 px.
4. **The interface is system chrome.** HUD boxes, mono captions and the
   theme's accent button make each game read as a settings page with a game
   in the middle. About 30 % of the portrait screen is chrome.
5. **Three copies.** Solitaire and Blackjack carry verbatim copies of the
   card byte, RNG, suit shapes, renderer and palette (`sol_*` / `bj_*`), and
   Poker includes Blackjack's headers directly. Both app docs already list
   what to extract.
6. **No sound** in any of them, although the platform has an effects helper
   that Relay Nine and RotorTerror use.

**Blackjack** (`current/blackjack-*.png`)
- Half the felt is empty; the only thing in it is `+75` after a round.
- Betting is two buttons that say `BET −10` / `BET +10`; the bet is a number
  in a box. No chips.
- The dealer's play is not shown: stand, and the final hand is simply there.
- A natural, a win, a push and a bust look alike apart from one word.
- No statistics, no split (the lint forbids the word).

**Poker** (`current/poker-*.png`), the weakest of the three
- Opponents are a line of text ("LINUS BB 980") over two small card backs.
  No seat, no identity, no stack, no bet on the table.
- The pot is the string `Flop | POT 80`. Side pots are not shown at all; a
  split is explained as "Multiple winners; payouts at seats."
- Eight permanent buttons, of which three (ALL IN, NEXT HAND, RESTART) are
  rarely valid, plus a raise stepper that moves one big blind per press, so a
  pot-sized raise from 40 to 240 is ten presses.
- All three opponents run the same policy (`poker_ai.c`): identical
  thresholds, sixteen equity trials (noisy), one action exactly every second.
- The session is lost on a restart, which includes every rotation and every
  keyboard attach; the app is portrait only for that reason.
- Showdown: the winning five cards are not identified.

**Solitaire** (`current/solitaire-*.png`)
- 68 px cards with the bottom 40 % of the screen unused in portrait.
- Select-then-place only; no one-tap move, no drag, no undo, no hint, no
  auto-complete, draw-1 only, no timer or score.
- The win is four kings and the caption "SOLVED IN 212 MOVES".
- A selected run is a 3 px orange outline; legal destinations are not shown.

## 2. Three directions

One Blackjack hand in each: `mockups/board-directions.png`.

### A. Lamplight (recommended)

A card table under one lamp, in a dark room.

- **Identity.** The cloth fills the whole screen and falls off to true black
  at the edges, which on this AMOLED panel is free and is where the depth
  comes from. Each game has its own cloth, taken from its Games Hub poster so
  the hub and the game read as one thing: claret for Blackjack, indigo for
  Hold'em, bottle green for Klondike. No frame, no HUD boxes.
- **Cards.** Warm ivory stock with a soft edge and a cast shadow; a large
  condensed index; true pip layouts at table size; double-ended court cards
  built from a display serif letter and a small crown mark inside a ruled
  frame; an ace in a hairline ring. At column size (72 px) the face reduces
  to one index and one large mark, which is what stays legible in a fan.
- **Backs.** Lattice on a deep colour with the DOORS arch in a gold ring;
  four colourways (midnight, claret, bottle, indigo).
- **Table.** Information is printed on the cloth in a muted gold ink, the way
  a real layout prints its rules: "BLACKJACK PAYS 3 TO 2", "DEALER STANDS ON
  ALL 17s" (both true of the engine). Chips are objects: clay chips with edge
  inserts in four denominations, stacked.
- **Type.** IBM Plex Sans Condensed for numbers and actions, IBM Plex Mono for
  small letterspaced labels (both product families), Fraunces italic for the
  few big moments ("Blackjack", "Solved.", "Linus takes it.").
- **Motion.** Physical and short: cards travel from the shoe, turn over,
  chips slide. 120 to 280 ms, eased out, never more than a few objects at
  once.
- **K230 fit.** Good. Everything is a blit from RAM over a prepared cloth
  buffer; grain doubles as dither against RGB565 banding. See section 8.
- **Complexity.** Medium-high. The art pipeline and the motion runner are
  new; layouts are redone.

### B. Imprint

A printed programme: paper, ink and one vermilion.

- **Identity.** The screen is a page: a serif headline, hairline rules,
  folios ("No. 13"), totals as very large numerals in the margin.
- **Cards.** Flat cotton paper, one giant serif numeral, two small suit marks.
  Backs in vermilion pinstripe.
- **Table.** None. Ruled sections on warm black.
- **Type.** Fraunces throughout with mono small caps.
- **Motion.** Restrained: slides and cross-fades.
- **K230 fit.** Excellent; nearly flat colour and text, the cheapest to draw.
- **Complexity.** Low-medium.
- **Why not.** It is the most original of the three and the cheapest, but it
  gives up the table. Poker without seats around a table and Blackjack
  without chips lose the thing the brief asks for most, and Solitaire's
  seven columns leave no room for the page layout that carries the look.

### C. Signal

An abstract digital tabletop on pure black.

- **Identity.** No materials. The table is a ring of light in the game's
  colour; the bet is the largest thing on screen.
- **Cards.** White tiles, huge condensed rank, one large suit, four-colour
  suits (blue diamonds, green clubs). Backs are a dark grid with a ring.
- **Type.** Condensed and mono only.
- **Motion.** Cinematic: ring sweeps, iris transitions.
- **K230 fit.** Good for static frames; full-screen transitions are the most
  expensive kind of motion on a software renderer.
- **Complexity.** Medium.
- **Why not.** The best legibility of the three, but it sits closest to the
  "excessive neon" the brief rules out, and it is the least like cards.

### Recommendation

**A, Lamplight**, with two things taken from the others: C's four-colour
suits as an optional deck setting (a real aid at 72 px), and B's discipline
about type, which is why Lamplight has no gold gradients, bevels or glows on
text. It gives the largest visible change from today, it gives Blackjack and
Poker a real table and Solitaire a calm one, and its cost at run time is
blits.

## 3. Lamplight in detail

`mockups/deck-sheet.png` shows every part at panel pixels.

| Part | Specification |
| --- | --- |
| Card sizes | 124 x 174 (Blackjack hands), 108 x 151 (your hole cards), 82 x 115 (board), 72 x 101 (Klondike portrait), 96 x 134 (Klondike landscape), 48 to 66 (opponents) |
| Stock | ivory `#faf4e4` to `#ebe1c9`, edge `#b9ad92`, radius 7.5 % of width |
| Inks | black `#1a1714`, red `#b3261e`; optional four-colour: diamonds `#2f6fe0`, clubs `#1e9b62` |
| Print ink | gold `#d9b66a` at 25 to 80 % |
| Cloth | Blackjack `#7d2130` to `#3a0d16`; Hold'em `#40377c` to `#1c183c`; Klondike `#226347` to `#0e2f22`; all to black |
| Chips | 10 slate blue, 50 green, 100 black and gold, 500 violet |
| Buttons | 16 px radius; primary ivory with ink text; secondary 1.5 px ivory outline; every one at least 64 px tall, with its key printed small in the corner |
| Suits | four paths (in `cg_kit.py`), no font glyph |

The palette is fixed in every theme, as the Games Hub's is (DS §60.2), and
Night dims the scene as it does today. That needs a design-system amendment:
see decisions.

## 4. Blackjack

Screens: `blackjack-bet.png`, `blackjack-play.png`, `blackjack-win.png`.

**Layout.** Dealer at the top with a total badge; the printed arc; the bet
circle with the actual chips; your hand with a larger total badge (ivory, or
gold on 21); BANK and BET on one ruled line; three actions. The felt's empty
half is gone because the bet lives there.

**Betting.** The bet is a stack. Tap a rack chip (10, 50, 100) to add it,
CLEAR, MAX, and the existing −10 / +10 for the keyboard. Arrow keys, Enter
and the letters keep today's meaning. The engine's 10 to 200 range and step
are unchanged, so every chip action is a sequence of the existing
`bj_bet_up` / `bj_bet_down`.

**Flow and motion** (all skipped under reduced motion)

| Moment | Motion | Time |
| --- | --- | --- |
| Deal | four cards from the shoe corner, one after another; hole card stays down | 4 x 140 ms |
| Hit | one card travels and settles; total badge counts up | 180 ms |
| Double | a second stack slides out beside the bet, the one card lands turned sideways | 300 ms |
| Stand | hole card turns over (squash to edge, swap, open); dealer draws one card every 450 ms | varies |
| Win | chips slide from the dealer to the bet circle, then both to BANK, which counts up | 500 ms |
| Blackjack | the band in `blackjack-win.png` wipes in; the losing hand dims | 350 ms |
| Lose / bust | the stack slides away to the dealer; your cards dim | 300 ms |
| Push | the stack nudges and returns | 200 ms |

The engine settles a round in one call. The view replays the difference
between the state before and after as a short script; the engine is never
paused mid-round and the save still holds only positions the rules reached.

**Kept exactly:** rules, shoe, payouts, save file v1, every key.

**Added (valuable, not essential):** session statistics in a separate file
(hands, won, blackjacks, best run, biggest win) on the result line and a
STATS sheet; a rules sheet behind the menu.

**Evaluated**
- *Split* is the feature players will miss most. It changes the engine (two
  hands, per-hand stakes), `bj_game_valid`, the save format (v2 with v1 still
  read) and the lint that forbids it. Recommended as its own later PR, after
  the redesign ships, so the visual work never touches the rules.
- *Configurable rules* (dealer hits soft 17, 6:5): not recommended. One
  honest ruleset printed on the cloth is part of the design.
- *Insurance, surrender, side bets:* no.

## 5. Poker

Screens: `poker-turn.png`, `poker-showdown.png`, `poker-turn-landscape.png`.

**Table.** A rail and an indigo cloth with a betting line. Seats sit on the
rail; cards, bets and the pot are on the cloth. Nothing else is in the room.

**Opponents.** Each has a medallion, a colour, a stack and a way of playing.
The names are today's; the emblems are small in-jokes, not likenesses.

| Seat | Emblem | Colour | Style | Tell in the presentation |
| --- | --- | --- | --- | --- |
| Ada | engine cog | verdigris | tight and exact: folds marginal hands, bets when the price is right | acts at an even pace |
| Linus | kernel (a seed) | ember | loose and aggressive: raises often, bluffs sometimes | acts fast, raises bigger |
| Grace | the moth | lilac | patient: calls, slow-plays strong hands | pauses longer before big calls |

**Reading the table.** The active seat's ring lights. Each seat's last
action is a tag in its colour (RAISE 80, CALL 80, FOLD); a folded seat dims
and loses its cards. Bets are chips in front of the seat with the amount. The
dealer button is an object that moves. Blinds are tagged on the first round.
The street is named with four dots. Side pots are separate stacks, each
labelled with its amount and who can win it; an all-in seat carries an ALL IN
tag and its cards turn face up when the board runs out.

**Your hand.** Larger, slightly fanned hole cards; a plain-words label of
what you hold now ("PAIR OF QUEENS"), from the existing evaluator on your own
cards and the board; the price in one line ("40 TO CALL · POT ODDS 6 TO 1").

**Betting controls.** Three actions, always in the same place: FOLD, CALL (or
CHECK), RAISE, each showing its amount. Above them four presets (MIN, ½ POT,
POT, ALL IN) and a stepper with a track for fine control. ALL IN, NEXT HAND
and RESTART leave the permanent grid: all-in is a preset, NEXT HAND replaces
the row after a hand, restart moves to the menu. Keys are unchanged; 1 to 4
pick a preset.

**Showdown.** Hands turn over in order; the five winning cards rise and the
rest dim; the winner's seat lights and the pot slides to it; one sentence
says what happened and why. The next hand deals itself after a few seconds
unless the table is touched (off under reduced motion; Enter always works).

**AI.** Three changes, none of which gives a seat information it should not
have.
1. *Personality as parameters*: fold threshold, raise threshold, raise
   frequency, raise size and bluff rate per seat instead of one set of
   constants.
2. *Better estimates*: 64 to 128 equity trials instead of 16, and position
   and stack depth in the pre-flop score. About 10 000 seven-card evaluations
   at most, once per decision (ASSUMED affordable; to be timed on a unit).
3. *Its own random stream*. Today `poker_ai` draws its samples from
   `g->rng`, the generator that shuffles the next deck. It reads no hidden
   card, but giving the AI a separate generator makes a hand replayable and
   makes "the AI cannot see the deck" checkable by the compiler: the AI
   function would take a view struct holding only its own hole cards, the
   board and the public betting state, instead of the whole game.
   The existing rule tests stay as they are; a new test asserts the AI's
   decision is unchanged when every hidden card is swapped.

Pacing moves from the 1 Hz shell tick to a one-shot timer of 0.6 to 2.5 s
that depends on the seat and the size of the decision.

**Persistence and landscape.** A `poker/game.v1` save written like the other
two (atomic, validated by a new `poker_game_valid`, saved between actions),
so the table survives rotation, keyboard attach and restart. With that in
place the app can declare every orientation and use the landscape layout in
the mockup.

**Not available:** tactile feedback. The hardware baseline lists no
vibration motor and the platform has no haptic API (DOCUMENTED). Presses get
a visual response and a sound instead.

## 6. Solitaire

Screens: `solitaire-play.png`, `solitaire-win.png`,
`solitaire-play-landscape.png`.

**Table.** Full-bleed green cloth; 72 px cards in portrait (68 today), 96 px
in landscape with the stock and waste down the left edge and the foundations
at the right, which gives the columns the full height.

**Interaction**
- *One tap moves.* Tapping a card sends it to the best legal place
  (foundation first, then the longest build). If there are several, the card
  lifts and the places light, as in the mockup. Today's pick-up and put-down
  remains for the keyboard and as the fallback.
- *Lit destinations.* A card in hand lights every pile that takes it.
- *Drag* in portrait, as an option: the tableau never scrolls there, so a
  drag is free to mean "move". Landscape keeps tap, because a drag scrolls
  long columns. To be judged on glass before it is promised.
- *Undo and redo*, unlimited within a deal. The game state is about 100
  bytes, so the history is a ring of whole positions; no inverse-move logic
  to get wrong.
- *Hint*: the next useful move, shown by lifting the card and lighting the
  place. Never plays for you.
- *Finish*: appears once every card is face up; plays the rest home.
- *Draw three* as a deal option, with the waste fanned.
- *Time, score, moves* on one line; standard Klondike scoring.
- *Statistics*: deals, solved, best time, fewest moves, streak.

**Motion.** Cards travel (160 ms), a turned card flips, a refused move
shakes once, the stock recycles as a sweep. The win is a cascade from the
foundations, then "Solved." with the numbers; any key or tap ends it.

**Engine impact.** Undo, hint, finish, timer, score and statistics are
additions around the rules. Draw three changes `sol_draw` and needs a
variant byte in the save (v2, v1 still read).

## 7. Sound

Short synthesized effects through the existing effects helper
(`pos-audio-effects`, `core/audio_effects`, used by Relay Nine and
RotorTerror; VERIFIED present, the card effects are new): card slide, card
turn, chip set down, chip sweep, a soft knock for check, a two-note figure
for a win, a longer one for blackjack and a solved deal. Quiet by default,
follow the system volume and mute, and one switch per game in its menu. No
music.

## 8. Technical feasibility

**The platform, as recorded in the repo**

| Fact | Class |
| --- | --- |
| 568 x 1232 AMOLED, RGB565, software rendering, one draw unit | VERIFIED |
| Linux sees one hart; 1 GiB RAM, about 930 MB available at idle | DOCUMENTED |
| LVGL 9.5 refresh 33 ms; apps animate from their own `lv_timer` (Timber 40 ms, Radar 50 ms) | VERIFIED |
| Relay Nine holds about 30 fps full-scene at 72 to 92 % CPU; Radar 17 % | DOCUMENTED |
| No image cache, no shadow cache, matrix transforms off on the device | DOCUMENTED |
| `pocketos_shell_set_fullscreen()`, `pocketos_shell_reduced_motion()` exist | VERIFIED |
| Runtime art is `.bin` (RGB565 / RGB565A8 / A8) under an 11 MiB budget | VERIFIED |

**Rendering approach**

- *Cards: composed once, then blitted.* Pre-rendering 52 faces at three
  sizes with alpha is about 6 MB on the card, more than half the art budget.
  Instead ship a small kit (suit masks, rank glyphs and court art as A8 /
  RGB565A8, about 150 to 300 kB, ASSUMED) and compose the faces the game
  needs into a RAM atlas when it opens: about 1.1 MB for Klondike's 52 at
  72 px, less for the others. Drawing a card is then one image blit, cheaper
  than today's rectangles, triangles, discs and text per card.
- *Cloth: one buffer.* The light falloff and grain are computed once into a
  screen-sized RGB565 buffer (1.4 MB RAM) with ordered dither, from a tiny
  light map. Every redraw of the table starts with a copy from it.
- *Shadows are sprites*, one A8 image per card size. LVGL's runtime shadows
  are not used (no cache on the device).
- *Motion redraws only what moved.* A small runner owns a list of tweens and
  one 33 ms timer that exists only while something is moving; each frame
  invalidates the old and new bounds of the moving objects. Idle cost is
  zero, as today. Budget: at most a quarter of the panel per frame, at most
  six moving objects, target 30 fps and under 35 % CPU while moving
  (ASSUMED; the first milestone measures it).
- *Turning a card* is a horizontal squash of an image, not a 3D transform.
- *Tilted cards* (your two hole cards, the win cascade) are the one thing
  the device's renderer does not do cheaply. The two hole cards are rendered
  tilted once per hand into their own sprites; the cascade falls straight on
  the device if rotation proves too slow.
- *Procedural or pre-rendered?* Both, at different times: art authored as
  vectors in the repo (as the hub posters are), reduced to masks at build
  time, composed at app start, blitted at run time. No vector work per
  frame, no ThorVG dependency, no new library.

**Lifecycle and rules:** unchanged create / tick / destroy; timers are
deleted in `destroy`; saves keep their format unless a section above says
otherwise; engines stay LVGL-free; touch and one logical key stream as now.
No networking, purchases or real money, as now.

**Risks**

| Risk | Mitigation |
| --- | --- |
| Frame rate with staging copies whole frames to scanout | measure first (milestone 0); every animation has an instant form already required for reduced motion |
| Banding of dark gradients in RGB565 | dither and grain in the cloth buffer; judged on the panel, not the simulator |
| 72 px card legibility and 34 px strips under a finger | deck sheet is at panel pixels; needs a look on glass before the deck is frozen |
| Cards no longer follow the text-size setting | the fixed index is about today's Medium to Large; owner decision |
| Fixed palette breaks "apps use tokens" | amendment modelled on DS §60.2; owner decision |
| Landscape top safe area (50 px) | the two landscape mockups predate a safe-area pass; layout to be adjusted |
| Court-card art drawn by script | the frame, crown and letter are a system; the three court faces deserve a hand pass before freezing |
| Fraunces is a new typeface | used in art and three or four fixed phrases only; SIL OFL 1.1; needs a notices row |

## 9. Shared components

A first-party `games/cards` library (name to be agreed), replacing the three
copies:

| Component | Today | Proposal |
| --- | --- | --- |
| Card byte, rank/suit helpers, RNG | `sol_*`, `bj_*` copies; Poker includes `bj_*` | one module; reuse |
| Shoe (N decks) | Blackjack only | shared, Klondike and Hold'em as one deck |
| Deck renderer and atlas | two copies of shape drawing | rebuild |
| Cloth, shadows, chips | none | new |
| Motion runner, sound wrapper | none | new |
| Table buttons, tags, badges | DS buttons | new, one style file |
| Rules, geometry, input model, saves | per game | stay per game |

Engines and their tests are reused whole. Views (`*_view.c`) are reworked for
the new geometry; table widgets and card drawing are rebuilt.

## 10. Roadmap

**Essential for the redesign**

| # | Milestone | PRs | Done when |
| --- | --- | --- | --- |
| 0 | Spike: atlas, cloth buffer, one animated deal behind a review switch | 1 draft | frame time, CPU and RSS measured on a unit (needs the owner's go-ahead for a unit) |
| 1 | Shared library | 2: (a) card/RNG/shoe extraction with no visual change, (b) deck art pipeline, renderer, cloth, chips, DS amendment | all existing game suites pass unchanged |
| 2 | Klondike, static | 1 | new table, deck, layout, lit destinations, both orientations |
| 3 | Motion runner and sound wrapper; Klondike motion | 1 | reduced motion verified instant |
| 4 | Blackjack | 2: table and chip betting; motion and result states | save v1 round-trips unchanged |
| 5 | Hold'em table, seats, controls | 2: table and seats; controls and showdown | engine suite unchanged |
| 6 | Hub posters redrawn from the new deck; docs; hardware gate | 1 | owner's look on glass |

**Valuable gameplay**

| Item | Game | Engine change |
| --- | --- | --- |
| One-tap move, undo/redo, hint, finish, timer, score, stats | Klondike | none to the rules |
| Statistics, rules sheet | Blackjack | none |
| Save file, rotation survival, landscape | Hold'em | adds `poker_game_valid` |
| AI personalities, own RNG stream, view-only AI input, pacing | Hold'em | AI module only |

**Optional, later**

Split (Blackjack, save v2); draw three (Klondike, save v2); drag in
portrait; four-colour deck; alternative backs and cloths; a tournament mode
with rising blinds; daily deal.

Each PR is draft until its tests pass and it has been built; nothing is
merged or put on a unit without the owner's word.

## 11. Decisions for the owner

1. **Direction:** A (Lamplight), B (Imprint) or C (Signal), or a mix.
2. **Fixed palette in every theme**, Night dimming, as the hub: accept as a
   design-system amendment?
3. **Full screen** with the game's own back mark, instead of the shell's
   header row.
4. **Cards stop following the text-size setting.**
5. **Fraunces** (SIL OFL 1.1) for art and headline phrases.
6. **Sound** on by default at a low level, or off until switched on.
7. **Split** in Blackjack: later PR, or never.
8. **A unit for milestone 0** measurements (A or B), when the time comes.

## 12. What was and was not done

- Built: the SDL simulator from this baseline in WSL (`make all` and the
  shell target, both clean) and 17 captures of the current games.
- Rendered: twelve mockups and two boards with Edge headless.
- Not done: no product code changed, nothing cross-built, nothing run on
  Unit A or B, no frame rate or memory figure measured for the proposal
  (every such figure above is marked ASSUMED), no tests run beyond the build.
