# DeskBuddy personality: captures and manual check

The personality slice is described in [../DESKBUDDY.md](../DESKBUDDY.md)
("Personality (Companion)"); ideas for later slices are in
[PERSONALITY_NEXT.md](PERSONALITY_NEXT.md).

## What these pictures are

**Every picture here is a capture of the real application on the host. There
are no mockups.** None was taken on a K230. All were made from a build of
commit `55ec6d8` (a clean clone, CMake reporting "Doors 0.3.6 build 55ec6d8").

| File | How it was made |
| --- | --- |
| `shell-portrait.png`, `shell-landscape.png` | The real shell (`pocketos-shell`, SDL dummy display, `--open deskbuddy --rotation portrait|landscape --screenshot`, `DESKBUDDY_VISION=none`): the screen as opened, with the shell's header. |
| `demo-portrait.png`, `demo-landscape.png` | Animated PNGs. The real DeskBuddy code and LVGL, hosted by the app test's harness (`tests/deskbuddy_app_test.c` with `$DESKBUDDY_DEMO_DIR`), driven by a synthetic pointer; the app's body only (the harness has no shell header). Assembled by `tools/design/deskbuddy_demo.py`, which halves the size and **draws a grey ring where the finger is pressed - an overlay, not part of the app**. |
| `still-*.png` | Single frames of the same capture at full size, without the ring. |

The portrait animation, 35 s (quiet minutes skipped, not filmed):

| From | What happens |
| --- | --- |
| 0 s | idle: blinks, a glance, a breath |
| 1.6 s | a tap beside the right eye: startled, then a look that way |
| 3.5 s | four quick taps: "hey", then a brief huff, a squint, a slow blink |
| 8 s | a slow stroke below the eyes: pleased |
| 11.6 s | FEED: the biscuit appears, the eyes follow it |
| 12.7 s | dragged to the mouth: eyes wide, mouth open; let go: chewing, a smile |
| 17 s | REST: a yawn, asleep (WAKE on the button), breathing slowly |
| 21.7 s | a tap: it stirs awake |
| 24 s | a minute alone: drowsy, slow blinks, a nod |
| 28.5 s | two minutes alone: asleep; a stroke wakes it |

The landscape animation: FEED, a tap on the biscuit, it floats to the mouth
and is eaten.

| Still | Shows |
| --- | --- |
| `still-poke.png` | looking toward a poke on the right |
| `still-annoyed.png` | the brief huff |
| `still-pet.png` | pleased after a stroke |
| `still-snack.png` | watching the snack |
| `still-snack-mouth.png` | the snack at its mouth |
| `still-eat.png` | chewing |
| `still-yawn.png` | the yawn before REST |
| `still-asleep.png` | asleep (WAKE on the button) |
| `still-drowsy.png` | drowsy after a minute alone |
| `still-landscape-snack.png` | landscape, watching the snack |
| `still-landscape-eat.png` | landscape, after eating |

## Manual check (the same for every candidate)

Portrait, Companion (BUDDY), a unit or the SDL simulator, no camera needed
(`DESKBUDDY_VISION=none` on the simulator). Times are approximate.

1. Open DeskBuddy. Watch 10 s: blinks, a glance now and then, a slow breath.
   Besides the eyes: FEED / REST, the mode row and, on a unit without the
   vision models, the existing NO VISION YET note.
2. Tap once just right of the right eye. It reacts toward that side and is
   calm again within about 2 s. Repeat a few times: the reaction varies.
3. Tap the face four times within 2 s. A short annoyed look (glare, a
   shake, a flat mouth), then a squint and a slow blink back to calm within
   about 3 s. Tap again within 6 s: only a blink, no second huff.
4. Stroke slowly back and forth below the eyes for about 1 s: pleased (^ ^,
   a smile). Keep stroking: it stays pleased, it does not restart. A fast
   flick across the face: nothing.
5. Tap FEED, then REST, SET and DONE, BUDDY: none of these pokes it.
6. FEED: a biscuit appears in a lower corner and the eyes follow it. Drag it
   half way and let go: it floats back. Drag it to just below the eyes: the
   mouth opens; let go: it chews and smiles.
7. FEED, then tap the biscuit (or GIVE): it floats to the mouth and is
   eaten. FEED again, then Back: it is put away. FEED and leave it 20 s: it
   goes away by itself, with no fuss.
8. REST: a yawn, then asleep; the button says WAKE. Tap the face: it stirs
   (lids first), then looks about.
9. Leave it untouched: after 1 min heavy lids and slow blinks, after 2 min
   asleep. A stroke wakes it gently.
10. With a biscuit out, turn to landscape (attach the keyboard, or the
    rotation setting). The biscuit is back in its place; drag it to the
    mouth.
11. GUARD and NIGHT: no FEED / REST; both behave as before (a tap in NIGHT
    opens the dim eyes for a few seconds).
12. Turn on reduced motion (Settings): reactions still change the face, but
    nothing shakes, hops, breathes or chews.
13. Start dragging the biscuit and close the app mid-drag; reopen: calm, no
    biscuit out, nothing remembered.

Keyboard (Companion): F is FEED / GIVE, Enter gives, R is REST / WAKE, Esc
puts the biscuit away.
