/*
 * Camera's gallery screen: the LVGL side of camera_gallery.h, living in the
 * Camera app's body beside the camera's own screen, and in the Photo app's
 * body on its own (apps/photo/photo_app.c: standalone, no camera at all).
 *
 * The Camera app owns one helper session at a time (camera_session.h). Going
 * to the gallery ends the camera's helper - the camera is closed while photos
 * are browsed - and gallery_ui_enter() starts `pos-camera library` on the same
 * session; gallery_ui_leave() ends it again before the camera reopens. Every
 * file is read, decoded, exported and deleted by that helper; this file only
 * copies finished pictures out of the shared memory into buffers it owns, as
 * camera_app.c does for the preview.
 *
 * Memory: one page of thumbnails, the photo view's picture, and - only while
 * the slideshow runs - its shown and prepared pictures. All of it is freed
 * when the gallery is left.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef CAMERA_GALLERY_SCREEN_H
#define CAMERA_GALLERY_SCREEN_H

#include "camera_session.h"
#include "lvgl.h"

#include <stdbool.h>

struct gallery_ui;

/* Build the gallery's objects under root, hidden. The session is the app's. */
struct gallery_ui *gallery_ui_create(lv_obj_t *root, struct camera_session *session);
/* The Photo app's gallery: no CAMERA action (camera_gallery_set_standalone),
 * so gallery_ui_poll() never asks for the camera. Before gallery_ui_enter();
 * kept across leave and enter. */
void gallery_ui_set_standalone(struct gallery_ui *u, bool standalone);
/* Show it and start the library helper (the camera's helper must be gone). */
void gallery_ui_enter(struct gallery_ui *u);
/* The app's timer, while the gallery is shown. Never blocks. True once the
 * owner has asked to go back to the camera. */
bool gallery_ui_poll(struct gallery_ui *u);
/* End the library helper (bounded, as camera_session_abandon), free every
 * picture, hide. */
void gallery_ui_leave(struct gallery_ui *u);
bool gallery_ui_active(const struct gallery_ui *u);
/* After gallery_ui_leave(); the objects themselves go with the app's root. */
void gallery_ui_destroy(struct gallery_ui *u);

#endif
