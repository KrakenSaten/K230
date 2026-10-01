/*
 * RIFT's own small components, built from LVGL primitives and token roles
 * (docs/design/rift/HANDOFF.md §8). RIFT owns no colour: everything here
 * either adds a PocketUI role style or reads a Doors token through
 * pos_theme_color() inside a draw callback, which is the sanctioned way to
 * paint something LVGL styles cannot (pos_styles.h, tests/style_lint.sh).
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef RIFT_WIDGETS_H
#define RIFT_WIDGETS_H

#include "rift_format.h"

#include "lvgl.h"
#include "pos_styles.h"

/* Sizes from the handoff §4, which RIFT-DEV-1 approved for this app only:
 * a 36 px row selects and never acts, and everything that navigates or
 * acts stays at 56 or more. */
#define RIFT_ROW_H 36
#define RIFT_GROUP_H 24
#define RIFT_HEADER_ROW_H 28
#define RIFT_TOUCH_H 56
#define RIFT_PRIMARY_H 64
#define RIFT_PAD 20
#define RIFT_PANE_PAD 16
#define RIFT_STRIP_W 104
#define RIFT_STRIP_W_WIDE 150
#define RIFT_GLYPH_BOX 12 /* the 8 px glyph plus room for the 2 px self ring */
#define RIFT_CAPTION_H 18 /* one line of Mono 14, the caption role's type */
/* How far a panel's caption rises above the panel's own box: it is centred on
 * the top rule, so half its line, plus the 3 px that put the rule through the
 * middle of the capitals rather than along their tops (rift_panel). A parent
 * whose top edge a captioned panel sits on leaves this much room. */
#define RIFT_CAPTION_OVERHANG (RIFT_CAPTION_H / 2 + 3)

/* The same heights at the text size in force (DS §46). A caption line is
 * whatever the caption role's font draws, so a panel's caption, a group
 * label and a header row grow with it, and the room left for a caption's
 * rise grows too. Never below the constants above, which they equal at
 * Small. Measured on every call: the text size can change while RIFT is
 * open, and the next build of a view takes the new one. */
int32_t rift_caption_h(void);
int32_t rift_caption_overhang(void);
int32_t rift_group_h(void);
int32_t rift_header_row_h(void);

/* The link glyph of handoff §6: colour never carries it alone, so every
 * caller prints the state word beside one of these. */
enum rift_glyph {
    RIFT_GLYPH_DIRECT = 0, /* filled radio_rx */
    RIFT_GLYPH_RELAYED,    /* hollow text_secondary */
    RIFT_GLYPH_UNKNOWN,    /* dashed text_muted */
    RIFT_GLYPH_STALE,      /* filled text_muted */
    RIFT_GLYPH_SELF,       /* filled text_primary inside a focus ring */
    RIFT_GLYPH_CHANNEL,    /* a "#" in text_secondary (handoff §6) */
};

lv_obj_t *rift_glyph_create(lv_obj_t *parent);
void rift_glyph_set(lv_obj_t *glyph, enum rift_glyph kind);

/* The hop strip: the path at a glance, compressed by rift_strip_build().
 * The numeric hop count is never inside it - it belongs in its own column
 * (handoff §7) - so this draws the shape and nothing else. */
lv_obj_t *rift_strip_create(lv_obj_t *parent, int32_t width);
void rift_strip_set(lv_obj_t *strip, const struct rift_path *path, int stale);
void rift_strip_set_width(lv_obj_t *strip, int32_t width);

/* A hairline panel with its caption cut into the top rule (handoff §4).
 * Returns the panel; its content goes in as children. caption may be NULL. */
lv_obj_t *rift_panel(lv_obj_t *parent, const char *caption);

/* The activity pulse: how lately something was heard from (rift_pulse_of),
 * as three small dots - three filled for NOW, two RECENT, one QUIET, none
 * STALE, and nothing at all for never heard. Dots in a row and not bars of
 * rising height, because this is not a signal meter and must not read as
 * one: it is an age, bucketed, and the age itself is printed beside it
 * wherever it appears. NOW fills in radio_rx, the token for RX activity;
 * the rest in text_secondary, the empty dots in text_muted. */
#define RIFT_PULSE_W 16
lv_obj_t *rift_pulse_create(lv_obj_t *parent);
void rift_pulse_set(lv_obj_t *pulse, enum rift_pulse level);
/* What a pulse is showing; RIFT_PULSE_NONE for anything that is not one. */
enum rift_pulse rift_pulse_get(lv_obj_t *pulse);

/* A 24 px group label: "HEARD < 12 H · 12". */
lv_obj_t *rift_group_label(lv_obj_t *parent, const char *text);

/* The unread pill (handoff §6): Mono 14/500 in text_on_accent on a radio_rx
 * fill, padding 0 5, radius 2. The colours are POS_STYLE_CHIP_RX's, which
 * are exactly those two tokens; the chip's own 36 px geometry is not, so
 * the size, radius and padding are set here. A count of 0 hides it - a pill
 * reading "0" would be a badge saying there is nothing to read. */
lv_obj_t *rift_unread_pill(lv_obj_t *parent);
void rift_unread_pill_set(lv_obj_t *pill, int count);

/* One hairline rule across the parent. */
lv_obj_t *rift_rule(lv_obj_t *parent);

/* A vertical rule in one of the token roles, for the 2 px rule beside a
 * message body (handoff §6: own messages carry it on the right in
 * accent_primary, received on the left in text_secondary, or radio_rx when
 * the peer was heard direct).
 *
 * It exists because none of the shared fill roles carries text_secondary or
 * text_muted as a *fill*, and a colour written here would be RIFT owning a
 * colour. It reads the token in a draw callback, which is the sanctioned
 * way (pos_styles.h, tests/style_lint.sh). */
enum rift_tone {
    RIFT_TONE_ACCENT = 0,
    RIFT_TONE_RX,
    RIFT_TONE_SECONDARY,
    RIFT_TONE_MUTED,
    RIFT_TONE_NONE, /* keeps its place in the row and draws nothing */
};

/* The identity mark of a list row (DS §37.3): a rule this wide at the row's
 * left edge, in the identity accent of who the row is - a chat node, a
 * room, a channel, a conversation - and nothing (RIFT_TONE_NONE) for a
 * repeater or a sensor, which keeps the columns of every row in line. */
#define RIFT_IDENT_W 3

lv_obj_t *rift_vrule(lv_obj_t *parent, int32_t width);
void rift_vrule_set(lv_obj_t *rule, enum rift_tone tone);
/* The rule in an identity accent (DS §37, pos_identity_hue): the mark of
 * who a row or a message is. `index` is a hash and is wrapped to the
 * palette. rift_vrule_set puts a tone back. */
void rift_vrule_set_identity(lv_obj_t *rule, uint32_t index);

/* A 56 px action in a row of them. A disabled action takes the DS §9
 * treatment and no focus, and says why in its own caption elsewhere. In a
 * row the actions share its width equally, whatever their words need (flex
 * grow), so a row of four has room for about ten characters each in
 * portrait: tests/rift_app_test.c checks that every word fits its button. */
lv_obj_t *rift_action(lv_obj_t *parent, const char *text, int primary, int enabled,
                      lv_event_cb_t cb, void *user);
/* Enable or disable an action made by rift_action, in the treatment it was
 * made with. A disabled action takes no clicks, so its callback cannot fire;
 * whatever says why belongs in a caption beside it (DS §2, §9). */
void rift_action_set_enabled(lv_obj_t *button, int primary, int enabled);

/* A label in a dense row: caption type, one line, clipped rather than
 * wrapped, and given an exact width so the columns line up. */
lv_obj_t *rift_cell(lv_obj_t *parent, enum pos_style_role role, int32_t width,
                    lv_text_align_t align);

/* Set a cell's text, shortened with an ellipsis when it does not fit the
 * width the cell has now.
 *
 * LVGL's own LV_LABEL_LONG_DOT would do this, and is not used: it shortens
 * by rewriting the label's own buffer and putting the removed characters
 * back on the next size change, which is a trade this list cannot make - its
 * rows are rebuilt and re-measured whenever the selection or the orientation
 * moves, and a name is remote text nobody here chose the length of. The cell
 * keeps LV_LABEL_LONG_CLIP and the text is cut here, on a character
 * boundary, measured in the font the cell is actually drawn in.
 *
 * The same text fitted to the same width again is not measured again: the
 * cell remembers (in its user data, which a cell has no other use for) a
 * fingerprint of the last text and width it was fitted to. */
void rift_cell_set_text_fit(lv_obj_t *cell, const char *text);
/* The same, fitted to room px rather than to the width the cell has now -
 * for a row that is being filled before its layout has settled, where the
 * caller knows the width the cell is about to have. room <= 0 falls back to
 * the cell's own width. */
void rift_cell_set_text_fit_room(lv_obj_t *cell, const char *text, int32_t room);
/* Set a label's text only when it differs from what it already says.
 * Setting a label re-measures it and re-lays out its row even when the text
 * is the same, and a list repaints every second with most of its rows
 * unchanged. */
void rift_label_set(lv_obj_t *label, const char *text);

/* The width text takes in the cell's own font. */
int32_t rift_cell_text_width(lv_obj_t *cell, const char *text);

#endif
