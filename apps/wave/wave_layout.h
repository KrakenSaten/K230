/*
 * Wave's layout and keyboard policy: which shape the screen takes in the room
 * the shell gives it, and when the touch keyboard comes up. Pure C, decided
 * from numbers alone: tests/wave_layout_test.c.
 *
 * THE SHAPES (content boxes on the reference panel, Wave being fullscreen
 * under the DS §36 frame, measured on unit A):
 *
 *   TALL   portrait, 528 x 1106 (528 x 820 with the touch keyboard up).
 *          One column: state and status, the history (which takes whatever
 *          is left and scrolls inside its own card), the preset, LISTEN and
 *          CAPTURE, and the composer row - message field and SEND - at the
 *          foot, next to the thumb and right above the keyboard.
 *
 *   WIDE   landscape, 1192 x 442. Two columns over the composer row: the
 *          state, status and history on the left, a rail of controls on the
 *          right (preset, LISTEN, CAPTURE, byte counter). The composer runs
 *          the full width underneath, with a KEYS button for the touch
 *          keyboard.
 *
 *   STRIP  landscape with the touch keyboard up: the body is 156 px tall.
 *          Only the composer row (field, SEND, HIDE) is shown, 64 px,
 *          because that is all there is room for and all that is needed
 *          while typing.
 *
 * Nothing in any shape makes the body scroll: the history is the one part
 * whose length is unbounded, and it scrolls inside its own card.
 *
 * THE KEYBOARD.
 *
 *   Portrait: tapping the message field brings up the touch keyboard, as in
 *   every other app. Nothing brings it up by itself.
 *
 *   Landscape: the touch keyboard would take three quarters of the body, so
 *   a tap on the field only focuses it - landscape is where the physical
 *   keyboard is (DS §21: automatic rotation turns landscape exactly when one
 *   is attached). The KEYS button is there in every landscape shape, so the
 *   app is still usable without a physical keyboard: KEYS brings the touch
 *   keyboard up, and the same button (then HIDE) puts it away again.
 *
 *   Everywhere: Enter sends; after a send the touch keyboard goes away so
 *   the answer can be seen.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETWAVE_LAYOUT_H
#define POCKETWAVE_LAYOUT_H

/* The rail of controls in WIDE. */
#define WAVE_RAIL_W 400
/* Below this body height only the composer row is shown. */
#define WAVE_STRIP_MAX_H 240
/* The history column in WIDE never gets narrower than this; a landscape body
 * too narrow for it and the rail falls back to TALL. */
#define WAVE_HISTORY_MIN_W 360
/* Buttons in the composer row. */
#define WAVE_SEND_W 132
#define WAVE_KEYS_W 112

enum wave_shape {
    WAVE_SHAPE_TALL = 0,
    WAVE_SHAPE_WIDE,
    WAVE_SHAPE_STRIP
};

struct wave_layout {
    enum wave_shape shape;
    int show_history;           /* state, status and history column */
    int show_rail;              /* preset, LISTEN, CAPTURE, counter */
    int show_keys;              /* the KEYS / HIDE button */
    int field_tap_shows_keyboard;
    const char *keys_label;     /* "KEYS" or "HIDE" */
};

/* w and h: the room the app has (content box). landscape: the shell's
 * orientation for this run. kb_visible: the touch keyboard is up now. */
void wave_layout_choose(struct wave_layout *out, int w, int h, int landscape, int kb_visible);

/* What a tap on KEYS does: 1 show the keyboard, 0 hide it. */
int wave_layout_keys_shows(const struct wave_layout *l, int kb_visible);

#endif
