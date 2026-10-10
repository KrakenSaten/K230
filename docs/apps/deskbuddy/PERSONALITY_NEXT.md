# DeskBuddy: the same character beyond Companion (design note)

**Status: a proposal for later slices. Nothing here is implemented.** It
builds on the personality slice in [../DESKBUDDY.md](../DESKBUDDY.md)
("Personality (Companion)"): one character - eyes, a mouth when it has
something to say, a breath - that every mode shows, and one door in, the
`db_stimulus`. Each section names what it would reuse and what it would
need decided first.

## DeskGuard: honest now, monitoring later

**Now (no camera needed).** GUARD armed is, today, a promise the app keeps
only while it is open and, on a unit without the models, cannot keep at
all: the note says NO VISION YET while the caption says WATCHING THE DESK.
The honest version for a blind unit is an "away from desk" sign, not a
guard:

- ARM becomes AWAY when the provider is blind (`seen` is `UNAVAILABLE`): the
  character turns to a calm "back soon" pose - eyes closed or looking down,
  no glare - with the time it was set, e.g. `AWAY SINCE 14:05`.
- Touch wakes it with the existing `stir`; the first touch after AWAY shows
  `AWAY 1 H 20 MIN` for a few seconds, then disarms. Nothing claims who
  touched it.
- No log line is written blind: the guard log stays a record of what vision
  saw, never of guesses.

**Later (real monitoring).** With a provider, the existing guard states
stay as they are; the character only lends them its face: `GUARD_ARMED` a
slow look about (the idle glance), a visitor the existing wary look, the
owner back the `greet` reaction. Watching with the app closed or the screen
off needs the background lifecycle ADR-002 defers - a decision, not a
character change.

Needs deciding: whether AWAY replaces ARM on blind units or sits beside it;
whether GUARD stays visible at all without a camera.

## NightDesk: sleeping character, clock, alarms, dim

- The character sleeps (`SLEEP`'s closed eyes, its slow breath at the night
  opacity) under the existing large clock; a touch gives `stir` and a sleepy
  look for `DB_NIGHT_PRESENCE_MS`, then it settles again. No reaction plays
  at full brightness at night.
- **Alarms stay the shell's.** NightDesk would only show the next alarm the
  shell already knows (a read-only line such as `ALARM 06:30`), and when the
  shell's alarm rings DeskBuddy is behind it like any app. It never sets,
  snoozes or silences an alarm itself.
- **Dim.** Today Night dims the character by opacity only. A lower panel
  brightness at night is the shell's (its brightness setting), so NightDesk would
  ask for it through an existing setting, or not at all. The global
  screen-off and lock rules are not touched.

Needs deciding: whether Night shows the next alarm at all, and the
brightness question (the shell owns it).

## Character sounds (Off / Touch only / All)

A short, quiet sound per reaction - a "hm?" for a poke, a purr for petting,
a crunch for eating, a yawn - on the existing audio path (core/pocketaudio, as
RIFT's sounds use it).

| Mode | Plays |
| --- | --- |
| Off (the default) | nothing |
| Touch only | sounds for reactions the user caused: poke, pet, eat, stir |
| All | also the spontaneous ones: the yawn when drowsy, a sleepy sigh |

- The system mute and volume always win; nothing plays at night or while
  another app's audio is in front; at most one sound per reaction, and none
  for a beat that is skipped under reduced motion.
- The setting is one more DeskBuddy preference (`sound=off|touch|all` in
  `prefs.v1`), on the SET panel.
- The sounds themselves need a source and a licence (generated in-house, or
  CC0 with the notice the licensing work requires).

Needs deciding: whether a desk companion should make sound at all by
default (proposed: Off), and the sound source.

## Focus sessions and light personalisation

- **Focus.** A FOCUS button in Companion starts a 25-minute session (length
  in SET): the character goes quiet - eyes half-lidded, no spontaneous yawns
  or glances, touches answered with a single blink - and a thin ring or the
  caption counts down. At the end it gives the `greet` reaction once; it
  never nags, repeats or notifies outside the app. Leaving the app ends the
  session; nothing is kept but the preferred length.
- **Personalisation**, a few choices on SET, each one preference:
  - eye shape: round (today) or tall;
  - liveliness: calm / normal, scaling the reaction pauses and the chance of
    the mischievous variants;
  - a name shown on the SET panel only.
  Colours stay the theme's (DS §4): the character never gets a colour of its
  own.

Needs deciding: whether focus belongs in DeskBuddy or in Clock, and which
two or three choices are worth a setting.

## What would carry over unchanged

- `db_stimulus` as the only way in: a future wave or peace sign maps to
  `GREET` (or a new kind with its own reaction), from `DB_SRC_VISION`,
  through the provider boundary that exists today. Nothing in the screen
  changes for it.
- The reaction table: new reactions are new beat lists, not new code paths.
- The gesture classifier and the snack, which have nothing mode-specific in
  them.
