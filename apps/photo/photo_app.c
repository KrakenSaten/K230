/*
 * Photo: the photo library on its own - the photos Camera took, as a grid of
 * thumbnails, one photo fitted to the screen with what is known about it,
 * NEWER and OLDER, DELETE (confirmed), EXPORT to Files, and a slideshow.
 *
 * Camera takes the photos; Photo is where they are looked at. There is one
 * library and one gallery: this app hosts Camera's own gallery screen
 * (apps/camera/camera_gallery_screen.c, its model camera_gallery.c) in its
 * body, standalone, rather than a second photo browser. It never opens a
 * camera. The gallery's library helper, `pos-camera library` (ADR-006's
 * process boundary, sealed shared memory, watchdog and death signal), reads,
 * decodes and deletes every file; nothing here touches one. The LVGL thread
 * only ever makes non-blocking calls, but for leaving, which gives the
 * helper GALLERY_LEAVE_GRACE_MS before it is killed.
 *
 * FULLSCREEN (DS section 30.8, as Camera): the app declares NONE; the shell's
 * header carries the back slab, the only way out. There is no CAMERA button:
 * the shell has no way for one app to open another, so taking a photo means
 * going home and opening Camera (docs/apps/PHOTO.md).
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "app.h"
#include "camera_gallery_screen.h"
#include "camera_session.h"
#include "pocketui.h"

#include <stdlib.h>

/* As Camera's timer: pictures are copied out of the shared memory in the
 * tick that learns of them. */
#define PHOTO_POLL_MS 33

struct photo_app {
    struct camera_session session;
    struct gallery_ui *gallery;
    lv_timer_t *timer;
};

static void on_poll(lv_timer_t *t)
{
    struct photo_app *a = lv_timer_get_user_data(t);

    /* Standalone: the gallery never asks for the camera, so the answer is
     * always false and there is nothing to leave for. */
    (void)gallery_ui_poll(a->gallery);
}

static void *photo_create(lv_obj_t *root)
{
    struct photo_app *a = calloc(1, sizeof(*a));

    if (!a) {
        return NULL;
    }
    camera_session_init(&a->session);
    a->gallery = gallery_ui_create(root, &a->session);
    if (!a->gallery) {
        free(a);
        return NULL;
    }
    gallery_ui_set_standalone(a->gallery, true);
    pocketos_shell_set_status_hint("");
    a->timer = lv_timer_create(on_poll, PHOTO_POLL_MS, a);
    /* Starts the library helper and lists the photos. */
    gallery_ui_enter(a->gallery);
    return a;
}

static void photo_destroy(void *priv)
{
    struct photo_app *a = priv;

    if (!a) {
        return;
    }
    if (a->timer) {
        lv_timer_delete(a->timer);
    }
    /* The helper first (bounded), then every picture; the objects go with
     * the app's root, which the shell deletes. */
    gallery_ui_leave(a->gallery);
    camera_session_abandon(&a->session, 0); /* nothing left to end, as a rule */
    gallery_ui_destroy(a->gallery);
    free(a);
}

LV_IMAGE_DECLARE(pos_app_icon_photo);

const struct pocketos_app app_photo = {
    .id = "photo",
    .name = "Photo",
    /* The launcher draws the Doors icon (DS section 20): the icon extension's
     * Gallery, a framed picture. */
    .icon = LV_SYMBOL_IMAGE,
    .icon_mask = &pos_app_icon_photo,
    .create = photo_create,
    .tick = NULL,
    .destroy = photo_destroy,
    /* Fullscreen (DS section 30.8): no status bar, the pictures take the
     * height, as in Camera's gallery. */
    .chrome = POCKETOS_CHROME_NONE,
};
