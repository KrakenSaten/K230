/*
 * PocketOS logical key input. See pos_input.h.
 *
 * Delivery is one press and one release per key, which is what LVGL's keypad
 * processing turns into exactly one LV_EVENT_KEY on the focused object: it
 * sends the key on the released-to-pressed edge only. A key held down is
 * never modelled here. Auto-repeat belongs to the source that can see a
 * finger or a switch still held - the touch keyboard's Backspace (§17.3) and
 * later the physical keyboard's driver - and reaches this queue as repeated
 * pushes, so the stream stays a sequence of discrete keys.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "pos_input.h"

#include <string.h>

/* ---- the vocabulary boundary ------------------------------------------ *
 *
 * DS §17.4 promises the stream speaks Unicode code points. LVGL's device
 * layer does not: lv_indev_data_t.key carries a printable character as its
 * UTF-8 *bytes*, copied into the uint32_t in memory order (lv_sdl_keyboard.c
 * does exactly that, and lv_textarea_add_char reads it back the same way).
 * The two agree for ASCII, which is why this only shows up the moment a
 * character above U+007F is typed - and C9 has just put æ ø å on the symbol
 * layer, so it would have shown up in M4 as mojibake.
 *
 * The conversions therefore live here, at the boundary, and nothing above
 * pos_input sees the packing. They are written byte-wise rather than with
 * shifts so they are correct on either endianness, which is also how LVGL
 * moves these bytes. LV_KEY_* constants are all below 0x80, so both
 * conversions leave them untouched.
 *
 * LVGL's own helpers for this live in lv_text_private.h, which is not part
 * of the installed API the device build compiles against; twenty lines here
 * cost less than depending on a private header in the Buildroot sysroot. */

static pos_key_t encode(pos_key_t cp)
{
    uint8_t b[4] = { 0, 0, 0, 0 };
    pos_key_t out = 0;

    if (cp < 0x80u) {
        return cp;
    }
    if (cp < 0x800u) {
        b[0] = (uint8_t)(0xC0u | (cp >> 6));
        b[1] = (uint8_t)(0x80u | (cp & 0x3Fu));
    } else if (cp < 0x10000u) {
        b[0] = (uint8_t)(0xE0u | (cp >> 12));
        b[1] = (uint8_t)(0x80u | ((cp >> 6) & 0x3Fu));
        b[2] = (uint8_t)(0x80u | (cp & 0x3Fu));
    } else if (cp <= 0x10FFFFu) {
        b[0] = (uint8_t)(0xF0u | (cp >> 18));
        b[1] = (uint8_t)(0x80u | ((cp >> 12) & 0x3Fu));
        b[2] = (uint8_t)(0x80u | ((cp >> 6) & 0x3Fu));
        b[3] = (uint8_t)(0x80u | (cp & 0x3Fu));
    } else {
        return cp; /* not a character; hand it on untouched */
    }
    memcpy(&out, b, sizeof(out));
    return out;
}

static pos_key_t decode(pos_key_t packed)
{
    uint8_t b[4];
    unsigned need;
    pos_key_t cp;
    unsigned i;

    memcpy(b, &packed, sizeof(b));
    if (b[0] < 0x80u) {
        return b[0];
    }
    if ((b[0] & 0xE0u) == 0xC0u) {
        need = 1;
        cp = b[0] & 0x1Fu;
    } else if ((b[0] & 0xF0u) == 0xE0u) {
        need = 2;
        cp = b[0] & 0x0Fu;
    } else if ((b[0] & 0xF8u) == 0xF0u) {
        need = 3;
        cp = b[0] & 0x07u;
    } else {
        return packed; /* not UTF-8 we recognise; hand it on untouched */
    }
    for (i = 1; i <= need; i++) {
        if ((b[i] & 0xC0u) != 0x80u) {
            return packed;
        }
        cp = (cp << 6) | (b[i] & 0x3Fu);
    }
    return cp;
}

static lv_group_t *group;        /* what apps focus into */
static lv_indev_t *indev;        /* the one device that delivers the stream */
static lv_group_t *source_group; /* private: holds sink, never app objects */
static lv_obj_t *source_sink;    /* receives adopted sources' keys */

static pos_key_t queue[POS_INPUT_QUEUE];
static unsigned head; /* monotonic; index is head % POS_INPUT_QUEUE */
static unsigned tail;
static bool release_pending;
static pos_key_t last_key;

unsigned pos_input_queued(void)
{
    return head - tail;
}

/* One key per press/release pair. continue_reading keeps LVGL draining
 * within the same lv_timer_handler pass, so a fast typist does not wait a
 * read period per character. */
static void read_cb(lv_indev_t *dev, lv_indev_data_t *data)
{
    (void)dev;

    if (release_pending) {
        release_pending = false;
        data->key = last_key;
        data->state = LV_INDEV_STATE_RELEASED;
        data->continue_reading = head != tail;
        return;
    }
    if (head == tail) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }
    last_key = encode(queue[tail % POS_INPUT_QUEUE]);
    tail++;
    data->key = last_key;
    data->state = LV_INDEV_STATE_PRESSED;
    release_pending = true;
    data->continue_reading = true;
}

/* An adopted source delivered a key to the sink. It arrives in LVGL's packed
 * form, so it is decoded back to a code point before it joins the queue: the
 * queue holds one vocabulary, whoever filled it. */
static void sink_key_cb(lv_event_t *e)
{
    const uint32_t *key = lv_event_get_param(e);

    if (key) {
        pos_input_push_key(decode(*key));
    }
}

void pos_input_init(void)
{
    if (group) {
        return;
    }
    head = tail = 0;
    release_pending = false;
    last_key = 0;

    group = lv_group_create();
    indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_read_cb(indev, read_cb);
    lv_indev_set_group(indev, group);
}

void pos_input_deinit(void)
{
    if (indev) {
        lv_indev_delete(indev);
        indev = NULL;
    }
    if (source_sink) {
        lv_obj_delete(source_sink);
        source_sink = NULL;
    }
    if (source_group) {
        lv_group_delete(source_group);
        source_group = NULL;
    }
    if (group) {
        lv_group_delete(group);
        group = NULL;
    }
    head = tail = 0;
    release_pending = false;
}

bool pos_input_push_key(pos_key_t key)
{
    if (!group) {
        return false;
    }
    if (head - tail >= POS_INPUT_QUEUE) {
        return false; /* drop the newest; what was typed first still arrives */
    }
    queue[head % POS_INPUT_QUEUE] = key;
    head++;
    return true;
}

bool pos_input_add_source(lv_indev_t *src)
{
    if (!src || !group || lv_indev_get_type(src) != LV_INDEV_TYPE_KEYPAD) {
        return false;
    }
    if (src == indev) {
        return false; /* the delivering device is not a source of itself */
    }
    if (!source_group) {
        source_group = lv_group_create();
        /* The sink is never seen: it sits on the system layer at 1 px with
         * no style. It exists because LVGL delivers keypad input to the
         * focused object of a group and to nothing else, so a source needs
         * an object to talk to before its keys can be forwarded. */
        source_sink = lv_obj_create(lv_layer_sys());
        lv_obj_remove_style_all(source_sink);
        lv_obj_set_size(source_sink, 1, 1);
        lv_obj_add_flag(source_sink, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(source_sink, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(source_sink, sink_key_cb, LV_EVENT_KEY, NULL);
        lv_group_add_obj(source_group, source_sink);
        lv_group_focus_obj(source_sink);
    }
    lv_indev_set_group(src, source_group);
    return true;
}

lv_group_t *pos_input_group(void)
{
    return group;
}

lv_indev_t *pos_input_indev(void)
{
    return indev;
}

void pos_input_add_obj(lv_obj_t *obj)
{
    if (group && obj) {
        lv_group_add_obj(group, obj);
    }
}

void pos_input_focus(lv_obj_t *obj)
{
    if (group && obj) {
        lv_group_focus_obj(obj);
    }
}

lv_obj_t *pos_input_focused(void)
{
    return group ? lv_group_get_focused(group) : NULL;
}
