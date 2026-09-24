/*
 * Camera's state machine. See camera_state.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "camera_state.h"

#include <stdio.h>
#include <string.h>

const char *camera_state_name(enum camera_state s)
{
    switch (s) {
    case CAMERA_INIT: return "CAMERA_INIT";
    case CAMERA_PREVIEW: return "CAMERA_PREVIEW";
    case CAMERA_CAPTURING: return "CAMERA_CAPTURING";
    case CAMERA_REVIEW: return "CAMERA_REVIEW";
    case CAMERA_ERROR: return "CAMERA_ERROR";
    case CAMERA_NO_DEVICE: return "CAMERA_NO_DEVICE";
    }
    return "?";
}

void camera_model_init(struct camera_model *m)
{
    memset(m, 0, sizeof(*m));
    m->state = CAMERA_INIT;
    /* GC2093's only mode until the helper says otherwise. */
    m->still_w = 1920;
    m->still_h = 1080;
}

static void panel(struct camera_model *m, const char *title, const char *detail)
{
    snprintf(m->title, sizeof(m->title), "%s", title);
    snprintf(m->detail, sizeof(m->detail), "%s", detail ? detail : "");
}

static void note(struct camera_model *m, const char *text, int64_t now)
{
    snprintf(m->note, sizeof(m->note), "%s", text);
    m->note_until = now + CAMERA_NOTE_MS;
}

/* Into ERROR with a title and a sentence. Whatever was going on stops. */
static void fail(struct camera_model *m, const char *title, const char *detail)
{
    m->state = CAMERA_ERROR;
    m->live = false;
    m->stalled = false;
    m->saving = false;
    m->confirm_delete = false;
    m->deleting = false;
    panel(m, title, detail);
}

static void to_preview(struct camera_model *m)
{
    m->state = CAMERA_PREVIEW;
    panel(m, "Waiting for the picture", "");
    m->live = false;
    m->stalled = false;
    m->saving = false;
    m->confirm_delete = false;
    m->deleting = false;
}

unsigned camera_model_open(struct camera_model *m)
{
    camera_model_init(m);
    panel(m, "Starting the camera", "");
    return CAMERA_DO_START_SESSION;
}

unsigned camera_model_retry(struct camera_model *m)
{
    bool have_last = m->have_last;
    bool review_picture = m->review_picture;
    char last[CAMERA_NAME_MAX];

    if (m->state != CAMERA_ERROR && m->state != CAMERA_NO_DEVICE) {
        return CAMERA_DO_NOTHING;
    }
    /* A retry is a fresh camera, not a fresh visit: the last photo's picture
     * is still in the app, and still reviewable. */
    snprintf(last, sizeof(last), "%s", m->last_name);
    camera_model_open(m);
    m->have_last = have_last;
    m->review_picture = review_picture;
    snprintf(m->last_name, sizeof(m->last_name), "%s", last);
    return CAMERA_DO_START_SESSION;
}

static void exited(struct camera_model *m, const struct camera_event *ev)
{
    char detail[96];

    if (m->state == CAMERA_ERROR || m->state == CAMERA_NO_DEVICE) {
        return; /* it said why before it left */
    }
    switch (ev->reason) {
    case CAMERA_EXIT_HUNG:
        fail(m, "Camera not responding", "The camera stopped responding and was closed.");
        break;
    case CAMERA_EXIT_PROTOCOL:
        fail(m, "Camera error", "The camera helper sent something invalid and was closed.");
        break;
    case CAMERA_EXIT_CRASHED:
        snprintf(detail, sizeof(detail), "The camera helper crashed (signal %d).",
                 ev->value - 128);
        fail(m, "Camera error", detail);
        break;
    default:
        snprintf(detail, sizeof(detail), "The camera closed unexpectedly (exit %d).", ev->value);
        fail(m, "Camera error", detail);
        break;
    }
}

static void helper_error(struct camera_model *m, const char *text)
{
    char detail[CAMERA_EVENT_TEXT_MAX + 32];

    if (strncmp(text, "busy", 4) == 0) {
        fail(m, "Camera busy", "Another program is using the camera.");
    } else if (strncmp(text, "exec", 4) == 0) {
        snprintf(detail, sizeof(detail), "The camera helper could not be started: %s", text + 4);
        fail(m, "Camera unavailable", detail);
    } else {
        snprintf(detail, sizeof(detail), "The camera could not be opened: %s", text);
        fail(m, "Camera unavailable", detail);
    }
}

unsigned camera_model_event(struct camera_model *m, const struct camera_event *ev, int64_t now)
{
    char detail[CAMERA_EVENT_TEXT_MAX + 32];

    switch (ev->kind) {
    case CAMERA_EV_READY:
        if (m->state != CAMERA_INIT) {
            return CAMERA_DO_NOTHING;
        }
        to_preview(m);
        m->simulated = ev->simulated;
        if (ev->w && ev->h) {
            m->still_w = ev->w;
            m->still_h = ev->h;
        }
        m->photos = (uint32_t)ev->value;
        return CAMERA_DO_START_PREVIEW;

    case CAMERA_EV_NODEVICE:
        m->state = CAMERA_NO_DEVICE;
        m->live = false;
        snprintf(detail, sizeof(detail), "No camera could be found: %s.", ev->text);
        panel(m, "No camera", detail);
        return CAMERA_DO_NOTHING;

    case CAMERA_EV_ERROR:
        if (m->state != CAMERA_ERROR && m->state != CAMERA_NO_DEVICE) {
            helper_error(m, ev->text);
        }
        return CAMERA_DO_NOTHING;

    case CAMERA_EV_LOST:
        snprintf(detail, sizeof(detail), "The camera went away: %s.", ev->text);
        fail(m, "Camera lost", detail);
        return CAMERA_DO_NOTHING;

    case CAMERA_EV_EXITED:
        exited(m, ev);
        return CAMERA_DO_NOTHING;

    case CAMERA_EV_FRAME:
        if (m->state == CAMERA_PREVIEW) {
            m->live = true;
            m->stalled = false;
        }
        return CAMERA_DO_NOTHING;

    case CAMERA_EV_STALL:
        if (m->state == CAMERA_PREVIEW) {
            m->stalled = true;
        }
        return CAMERA_DO_NOTHING;

    case CAMERA_EV_SAVING:
        if (m->state == CAMERA_CAPTURING) {
            m->saving = true;
        }
        return CAMERA_DO_NOTHING;

    case CAMERA_EV_CAPTURED:
        if (m->state != CAMERA_CAPTURING) {
            return CAMERA_DO_NOTHING;
        }
        m->state = CAMERA_REVIEW;
        m->saving = false;
        m->confirm_delete = false;
        m->deleting = false;
        m->photos = (uint32_t)ev->value;
        snprintf(m->review_name, sizeof(m->review_name), "%s", ev->name);
        snprintf(m->last_name, sizeof(m->last_name), "%s", ev->name);
        m->have_last = true;
        m->review_picture = ev->w > 0;
        m->note[0] = '\0';
        panel(m, "Photo saved", ev->name);
        return ev->w > 0 ? CAMERA_DO_TAKE_REVIEW : CAMERA_DO_NOTHING;

    case CAMERA_EV_CAPFAIL:
        if (m->state != CAMERA_CAPTURING) {
            return CAMERA_DO_NOTHING;
        }
        to_preview(m);
        switch (ev->reason) {
        case CAMERA_CAPFAIL_NOSPACE:
            note(m, "Storage is full: the photo was not taken", now);
            break;
        case CAMERA_CAPFAIL_QUOTA:
            note(m, "The photo limit is reached: delete some first", now);
            break;
        case CAMERA_CAPFAIL_DEVICE:
            note(m, "The camera could not take the photo", now);
            break;
        default:
            note(m, "The photo could not be saved", now);
            break;
        }
        return CAMERA_DO_START_PREVIEW;

    case CAMERA_EV_DELETED:
        if (m->state != CAMERA_REVIEW) {
            return CAMERA_DO_NOTHING;
        }
        m->photos = (uint32_t)ev->value;
        if (strcmp(ev->name, m->last_name) == 0) {
            m->have_last = false;
            m->review_picture = false;
            m->last_name[0] = '\0';
        }
        m->review_name[0] = '\0';
        to_preview(m);
        note(m, "Photo deleted", now);
        return CAMERA_DO_START_PREVIEW;

    case CAMERA_EV_DELFAIL:
        if (m->state == CAMERA_REVIEW) {
            m->confirm_delete = false;
            m->deleting = false;
            note(m, "The photo could not be deleted", now);
        }
        return CAMERA_DO_NOTHING;

    case CAMERA_EV_STOPPED:
    case CAMERA_EV_MALFORMED:
    default:
        return CAMERA_DO_NOTHING;
    }
}

unsigned camera_model_shutter(struct camera_model *m)
{
    if (m->state != CAMERA_PREVIEW || !m->live || m->stalled) {
        return CAMERA_DO_NOTHING;
    }
    m->state = CAMERA_CAPTURING;
    m->saving = false;
    m->note[0] = '\0';
    return CAMERA_DO_CAPTURE;
}

unsigned camera_model_keep(struct camera_model *m)
{
    if (m->state != CAMERA_REVIEW || m->deleting) {
        return CAMERA_DO_NOTHING;
    }
    to_preview(m);
    return CAMERA_DO_START_PREVIEW;
}

unsigned camera_model_delete(struct camera_model *m)
{
    if (m->state != CAMERA_REVIEW || m->deleting || !m->review_name[0]) {
        return CAMERA_DO_NOTHING;
    }
    m->confirm_delete = true;
    return CAMERA_DO_NOTHING;
}

unsigned camera_model_delete_confirm(struct camera_model *m)
{
    if (m->state != CAMERA_REVIEW || !m->confirm_delete || m->deleting) {
        return CAMERA_DO_NOTHING;
    }
    m->deleting = true;
    return CAMERA_DO_DELETE;
}

unsigned camera_model_delete_cancel(struct camera_model *m)
{
    if (m->state == CAMERA_REVIEW && !m->deleting) {
        m->confirm_delete = false;
    }
    return CAMERA_DO_NOTHING;
}

unsigned camera_model_show_last(struct camera_model *m)
{
    if (m->state != CAMERA_PREVIEW || !m->have_last || !m->review_picture) {
        return CAMERA_DO_NOTHING;
    }
    m->state = CAMERA_REVIEW;
    m->confirm_delete = false;
    m->deleting = false;
    m->live = false;
    m->stalled = false;
    snprintf(m->review_name, sizeof(m->review_name), "%s", m->last_name);
    return CAMERA_DO_STOP_PREVIEW;
}

bool camera_model_tick(struct camera_model *m, int64_t now)
{
    if (m->note[0] && now >= m->note_until) {
        m->note[0] = '\0';
        return true;
    }
    return false;
}

void camera_model_screen(const struct camera_model *m, struct camera_screen *out)
{
    memset(out, 0, sizeof(*out));
    out->status = m->note;
    out->hint = m->simulated ? "SIMULATED" : "";
    switch (m->state) {
    case CAMERA_INIT:
        out->show_panel = true;
        out->show_shutter = true;
        break;
    case CAMERA_PREVIEW:
        out->show_picture = m->live;
        out->show_panel = !m->live;
        out->show_shutter = true;
        out->shutter_enabled = m->live && !m->stalled;
        out->show_last = m->have_last && m->review_picture;
        if (!m->note[0] && m->stalled) {
            out->status = "Waiting for the camera...";
        }
        break;
    case CAMERA_CAPTURING:
        out->show_picture = m->live;
        out->show_panel = !m->live;
        out->show_shutter = true;
        out->status = m->saving ? "Saving the photo..." : "Taking the photo...";
        break;
    case CAMERA_REVIEW:
        out->show_picture = m->review_picture;
        out->picture_is_review = true;
        out->show_panel = !m->review_picture;
        out->show_review_buttons = !m->confirm_delete;
        out->show_confirm = m->confirm_delete;
        if (m->deleting) {
            out->status = "Deleting...";
        } else if (m->confirm_delete) {
            out->status = "Delete this photo?";
        } else if (!m->note[0]) {
            out->status = m->review_name;
        }
        break;
    case CAMERA_ERROR:
    case CAMERA_NO_DEVICE:
        out->show_panel = true;
        out->show_retry = true;
        break;
    }
}
