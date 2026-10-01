/*
 * The small parts ACTIVITY's management panels are built from - rows, a
 * wrapping line, a choice drawn as chosen, a text field with the Doors touch
 * keyboard - so ui/rift_manage.c (channels) and ui/rift_device.c (this
 * node's name and path hash size) are built the same way.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef RIFT_FORM_H
#define RIFT_FORM_H

#include "rift_app.h"

#define RIFT_FORM_GAP 12

/* A row of the given height: its children side by side, centred. */
lv_obj_t *rift_form_row(lv_obj_t *parent, int32_t height);
/* A column as tall as what is in it. */
lv_obj_t *rift_form_column(lv_obj_t *parent);
/* A label the width of its parent that wraps. */
lv_obj_t *rift_form_text(lv_obj_t *parent, enum pos_style_role role);
void rift_form_show(lv_obj_t *obj, int on);
/* A choice among several drawn as chosen (primary) or not (secondary), as
 * the NOTIFY switch and NODES' ZERO-HOP are. Words and a caption beside it
 * always say what it is; the look only agrees. */
void rift_form_chosen(lv_obj_t *button, int on);
/* What a field holds that somebody could have meant: no character the text
 * area took in as a key (an Esc, a tab). */
void rift_form_typed(lv_obj_t *field, char *out, size_t out_len);
/* A one-line field at most max_chars long, with the touch keyboard for a
 * finger in portrait and put away on Done. */
lv_obj_t *rift_form_field(struct rift_app *app, lv_obj_t *parent, const char *placeholder,
                          uint32_t max_chars);
/* Whether a field is in the Doors focus group. A field is made out of it,
 * and is put in only while the form that holds it is open: a field in a
 * closed form is one TAB would walk into, and one LVGL focuses - and scrolls
 * ACTIVITY to - when it is the first object the group gets. Changed only on
 * a change, because re-adding moves the focus (lvgl-layout gotcha 4). */
void rift_form_field_live(lv_obj_t *field, int live);
/* Whether the service can be asked anything now. Whether its radio can send
 * does not matter: nothing asked from these panels is a packet. */
int rift_form_service_ready(const struct rift_app *app);

#endif
