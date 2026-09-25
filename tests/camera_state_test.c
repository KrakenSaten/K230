/*
 * Camera's state machine on its own: every state, every event in every state
 * it matters in, what the screen shows, and the actions asked for. No
 * processes and no clock but the one passed in.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "camera_state.h"

#include <stdio.h>
#include <string.h>

static int checks;
static int failed;

static void check(const char *name, int ok)
{
    checks++;
    if (ok) {
        printf("ok   %s\n", name);
    } else {
        printf("FAIL %s\n", name);
        failed++;
    }
}

static struct camera_event ev(enum camera_ev_kind kind)
{
    struct camera_event e;

    memset(&e, 0, sizeof(e));
    e.kind = kind;
    return e;
}

static unsigned feed(struct camera_model *m, struct camera_event e)
{
    return camera_model_event(m, &e, 1000);
}

static void ready(struct camera_model *m)
{
    struct camera_event e = ev(CAMERA_EV_READY);

    e.w = 1920;
    e.h = 1080;
    e.value = 7;
    feed(m, e);
}

static struct camera_event captured(const char *name, uint32_t w)
{
    struct camera_event e = ev(CAMERA_EV_CAPTURED);

    snprintf(e.name, sizeof(e.name), "%s", name);
    e.w = w;
    e.h = w ? 939 : 0;
    e.value = 8;
    return e;
}

static void test_happy_path(void)
{
    struct camera_model m;
    struct camera_screen s;
    struct camera_event e;
    unsigned a;

    check("opening starts the helper", camera_model_open(&m) == CAMERA_DO_START_SESSION);
    check("in INIT", m.state == CAMERA_INIT);
    camera_model_screen(&m, &s);
    check("INIT: the panel, a disabled shutter", s.show_panel && !s.show_picture &&
                                                     s.show_shutter && !s.shutter_enabled);
    check("the shutter does nothing in INIT", camera_model_shutter(&m) == CAMERA_DO_NOTHING);

    e = ev(CAMERA_EV_READY);
    e.w = 1920;
    e.h = 1080;
    e.value = 7;
    e.simulated = true;
    a = feed(&m, e);
    check("ready starts the preview", a == CAMERA_DO_START_PREVIEW && m.state == CAMERA_PREVIEW);
    check("and counts the photos", m.photos == 7);
    camera_model_screen(&m, &s);
    check("no picture yet: panel, shutter disabled", s.show_panel && !s.shutter_enabled);
    check("the fake says SIMULATED", strcmp(s.hint, "SIMULATED") == 0);
    check("no shutter before a picture", camera_model_shutter(&m) == CAMERA_DO_NOTHING);

    feed(&m, ev(CAMERA_EV_FRAME));
    camera_model_screen(&m, &s);
    check("a picture: shown, shutter enabled", s.show_picture && !s.show_panel &&
                                                   s.shutter_enabled && !s.picture_is_review);
    check("no last photo yet", !s.show_last);

    feed(&m, ev(CAMERA_EV_STALL));
    camera_model_screen(&m, &s);
    check("stalled: the shutter waits", !s.shutter_enabled &&
                                            strstr(s.status, "Waiting") != NULL);
    check("and a tap does nothing", camera_model_shutter(&m) == CAMERA_DO_NOTHING);
    feed(&m, ev(CAMERA_EV_FRAME));
    check("a picture ends the stall", !m.stalled);

    check("shutter: capture", camera_model_shutter(&m) == CAMERA_DO_CAPTURE &&
                                   m.state == CAMERA_CAPTURING);
    camera_model_screen(&m, &s);
    check("capturing: the shutter is off", s.show_shutter && !s.shutter_enabled &&
                                               strstr(s.status, "Taking") != NULL);
    check("a second tap does nothing", camera_model_shutter(&m) == CAMERA_DO_NOTHING);
    feed(&m, ev(CAMERA_EV_FRAME));
    check("a stray frame does not leave CAPTURING", m.state == CAMERA_CAPTURING);
    feed(&m, ev(CAMERA_EV_SAVING));
    camera_model_screen(&m, &s);
    check("saving says so", strstr(s.status, "Saving") != NULL);

    a = feed(&m, captured("IMG_0008.jpg", 528));
    check("captured: review, take the picture", a == CAMERA_DO_TAKE_REVIEW &&
                                                     m.state == CAMERA_REVIEW);
    check("names the photo", strcmp(m.review_name, "IMG_0008.jpg") == 0 && m.photos == 8);
    camera_model_screen(&m, &s);
    check("review: the review picture and Delete/Keep",
          s.show_picture && s.picture_is_review && s.show_review_buttons && !s.show_confirm &&
              !s.show_shutter);

    check("keep: back to the preview", camera_model_keep(&m) == CAMERA_DO_START_PREVIEW &&
                                           m.state == CAMERA_PREVIEW && !m.live);
    camera_model_screen(&m, &s);
    check("the last photo is offered", s.show_last);
    check("last: review it again", camera_model_show_last(&m) == CAMERA_DO_STOP_PREVIEW &&
                                       m.state == CAMERA_REVIEW &&
                                       strcmp(m.review_name, "IMG_0008.jpg") == 0);

    check("delete asks first", camera_model_delete(&m) == CAMERA_DO_NOTHING && m.confirm_delete);
    camera_model_screen(&m, &s);
    check("confirm: Cancel/Delete and the question", s.show_confirm && !s.show_review_buttons &&
                                                         strstr(s.status, "Delete") != NULL);
    check("cancel", camera_model_delete_cancel(&m) == CAMERA_DO_NOTHING && !m.confirm_delete);
    camera_model_delete(&m);
    check("confirm: delete", camera_model_delete_confirm(&m) == CAMERA_DO_DELETE && m.deleting);
    check("keep is ignored while deleting", camera_model_keep(&m) == CAMERA_DO_NOTHING);
    check("a second confirm does nothing", camera_model_delete_confirm(&m) == CAMERA_DO_NOTHING);
    e = ev(CAMERA_EV_DELETED);
    snprintf(e.name, sizeof(e.name), "IMG_0008.jpg");
    e.value = 7;
    a = feed(&m, e);
    check("deleted: preview again", a == CAMERA_DO_START_PREVIEW && m.state == CAMERA_PREVIEW &&
                                        m.photos == 7);
    camera_model_screen(&m, &s);
    check("says so, and the last photo is gone", strcmp(s.status, "Photo deleted") == 0 &&
                                                     !s.show_last && !m.have_last);
    check("the note expires", camera_model_tick(&m, 1000 + CAMERA_NOTE_MS) && m.note[0] == '\0');
    check("Last does nothing now", camera_model_show_last(&m) == CAMERA_DO_NOTHING);
}

static void test_failures(void)
{
    struct camera_model m;
    struct camera_screen s;
    struct camera_event e;

    camera_model_open(&m);
    e = ev(CAMERA_EV_NODEVICE);
    snprintf(e.text, sizeof(e.text), "camera support is not built in");
    feed(&m, e);
    camera_model_screen(&m, &s);
    check("nodevice: NO_DEVICE, panel, retry", m.state == CAMERA_NO_DEVICE && s.show_panel &&
                                                   s.show_retry && !s.show_shutter);
    check("the reason is said", strstr(m.detail, "not built in") != NULL);
    feed(&m, ev(CAMERA_EV_EXITED));
    check("the helper leaving after it does not change that", m.state == CAMERA_NO_DEVICE);
    check("check again starts over", camera_model_retry(&m) == CAMERA_DO_START_SESSION &&
                                         m.state == CAMERA_INIT);

    camera_model_open(&m);
    e = ev(CAMERA_EV_ERROR);
    snprintf(e.text, sizeof(e.text), "busy the camera is in use");
    feed(&m, e);
    check("busy: ERROR, said as busy", m.state == CAMERA_ERROR &&
                                           strstr(m.title, "busy") != NULL);
    camera_model_open(&m);
    e = ev(CAMERA_EV_ERROR);
    snprintf(e.text, sizeof(e.text), "exec /usr/bin/pos-camera: No such file or directory");
    feed(&m, e);
    check("a missing helper is said", m.state == CAMERA_ERROR &&
                                          strstr(m.detail, "could not be started") != NULL);

    /* The helper hangs, crashes or leaves in every state that has one. */
    {
        static const int reasons[] = { CAMERA_EXIT_HUNG, CAMERA_EXIT_PROTOCOL,
                                       CAMERA_EXIT_CRASHED, CAMERA_EXIT_NORMAL };
        static const char *const words[] = { "responding", "invalid", "crashed", "unexpectedly" };
        int i;

        for (i = 0; i < 4; i++) {
            char name[80];

            camera_model_open(&m);
            ready(&m);
            feed(&m, ev(CAMERA_EV_FRAME));
            camera_model_shutter(&m);
            e = ev(CAMERA_EV_EXITED);
            e.reason = reasons[i];
            e.value = i == 2 ? 128 + 11 : 1;
            feed(&m, e);
            snprintf(name, sizeof(name), "helper gone mid-capture (%s): ERROR", words[i]);
            check(name, m.state == CAMERA_ERROR && strstr(m.detail, words[i]) != NULL);
        }
    }
    camera_model_screen(&m, &s);
    check("ERROR: panel and retry only", s.show_panel && s.show_retry && !s.show_shutter &&
                                             !s.show_review_buttons);
    check("the shutter does nothing in ERROR", camera_model_shutter(&m) == CAMERA_DO_NOTHING);

    camera_model_open(&m);
    ready(&m);
    feed(&m, ev(CAMERA_EV_FRAME));
    e = ev(CAMERA_EV_LOST);
    snprintf(e.text, sizeof(e.text), "no camera");
    feed(&m, e);
    check("lost in PREVIEW: ERROR", m.state == CAMERA_ERROR && strstr(m.title, "lost") != NULL);

    /* Capture failures keep the camera. */
    {
        static const int reasons[] = { CAMERA_CAPFAIL_NOSPACE, CAMERA_CAPFAIL_QUOTA,
                                       CAMERA_CAPFAIL_DEVICE, CAMERA_CAPFAIL_IO };
        static const char *const words[] = { "full", "limit", "could not take", "saved" };
        int i;

        for (i = 0; i < 4; i++) {
            char name[80];
            unsigned a;

            camera_model_open(&m);
            ready(&m);
            feed(&m, ev(CAMERA_EV_FRAME));
            camera_model_shutter(&m);
            e = ev(CAMERA_EV_CAPFAIL);
            e.reason = reasons[i];
            a = camera_model_event(&m, &e, 5000);
            camera_model_screen(&m, &s);
            snprintf(name, sizeof(name), "capfail %s: preview again with a note", words[i]);
            check(name, a == CAMERA_DO_START_PREVIEW && m.state == CAMERA_PREVIEW &&
                            strstr(s.status, words[i]) != NULL);
        }
        check("the note stays for its time", !camera_model_tick(&m, 5000 + CAMERA_NOTE_MS - 1));
        check("and then goes", camera_model_tick(&m, 5000 + CAMERA_NOTE_MS));
    }

    /* A capture with no review picture (the view was not set). */
    camera_model_open(&m);
    ready(&m);
    feed(&m, ev(CAMERA_EV_FRAME));
    camera_model_shutter(&m);
    check("captured without a picture: no take",
          feed(&m, captured("IMG_0001.jpg", 0)) == CAMERA_DO_NOTHING && m.state == CAMERA_REVIEW);
    camera_model_screen(&m, &s);
    check("the panel says saved", s.show_panel && !s.show_picture &&
                                      strstr(m.title, "saved") != NULL);
    camera_model_keep(&m);
    camera_model_screen(&m, &s);
    check("no Last without its picture", !s.show_last);

    /* Delete fails. */
    camera_model_open(&m);
    ready(&m);
    feed(&m, ev(CAMERA_EV_FRAME));
    camera_model_shutter(&m);
    feed(&m, captured("IMG_0002.jpg", 528));
    camera_model_delete(&m);
    camera_model_delete_confirm(&m);
    feed(&m, ev(CAMERA_EV_DELFAIL));
    camera_model_screen(&m, &s);
    check("delfail: still in review, asked nothing, told",
          m.state == CAMERA_REVIEW && !m.confirm_delete && !m.deleting &&
              s.show_review_buttons && strstr(s.status, "could not be deleted") != NULL);

    /* A retry keeps the last photo reviewable. */
    e = ev(CAMERA_EV_EXITED);
    e.reason = CAMERA_EXIT_CRASHED;
    feed(&m, e);
    camera_model_retry(&m);
    ready(&m);
    feed(&m, ev(CAMERA_EV_FRAME));
    camera_model_screen(&m, &s);
    check("after a retry the last photo is still offered", s.show_last &&
                                                                strcmp(m.last_name, "IMG_0002.jpg") == 0);

    /* Events in states they do not belong to are ignored. */
    camera_model_open(&m);
    check("captured in INIT is ignored",
          feed(&m, captured("IMG_0003.jpg", 528)) == CAMERA_DO_NOTHING && m.state == CAMERA_INIT);
    feed(&m, ev(CAMERA_EV_FRAME));
    check("a frame in INIT does not make it live", !m.live);
    ready(&m);
    check("a second ready is ignored", feed(&m, ev(CAMERA_EV_READY)) == CAMERA_DO_NOTHING);
    check("keep outside review does nothing", camera_model_keep(&m) == CAMERA_DO_NOTHING);
    check("delete outside review does nothing", camera_model_delete(&m) == CAMERA_DO_NOTHING &&
                                                    camera_model_delete_confirm(&m) ==
                                                        CAMERA_DO_NOTHING);
    check("retry outside ERROR does nothing", camera_model_retry(&m) == CAMERA_DO_NOTHING);
    check("every state has a name", strcmp(camera_state_name(CAMERA_NO_DEVICE),
                                           "CAMERA_NO_DEVICE") == 0);
}

int main(void)
{
    test_happy_path();
    test_failures();
    printf("camera_state_test: %d checks, %d failure(s)\n", checks, failed);
    return failed > 0;
}
