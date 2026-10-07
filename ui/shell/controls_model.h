/*
 * DOORS Controls, the decisions without LVGL: what each tile says, what a tap
 * on the radio does (the antenna question before every off-to-on), and where
 * every panel goes in both orientations. controls.c draws what this decides;
 * tests/controls_model_test.c covers it on the host, including that no two
 * panels overlap and all of them fit the content area.
 *
 * Sources, each a provider that already exists (nothing here reads a device):
 *
 *   Radio      radiod's state as the status bar last polled it ("off" is the
 *              owner's switch, radio.set_enabled), and on a unit not yet set
 *              up for RIFT sysd's radio_setup.status (docs/api/system.md)
 *   Bluetooth  system.status.bluetooth.controllers (sysd)
 *   Battery    system.status.power (sysd)
 *   Volume     the shell's own setting (volume.h)
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef DOORS_CONTROLS_MODEL_H
#define DOORS_CONTROLS_MODEL_H

#include <cjson/cJSON.h>
#include <stdbool.h>
#include <stdint.h>

#define CONTROLS_LINE_MAX 48

/* ---- the radio tile and its switch -------------------------------------- */

enum controls_radio_tap {
    CONTROLS_TAP_NOTHING = 0, /* radiod is not answering: nothing to switch */
    CONTROLS_TAP_ASK_ANTENNA, /* off: ask about the antenna first */
    CONTROLS_TAP_SWITCH_OFF,  /* on in any way (rx, tx, idle, error): off at once */
    CONTROLS_TAP_ASK_SETUP    /* not set up for RIFT: the setup question first */
};

/* radiod's state word as the status bar has it (NULL: not answering) to the
 * tile's value line and its "on" dot. */
const char *controls_radio_text(const char *state);
bool controls_radio_dot(const char *state);
/* Whether the radio is switched on, as far as radiod says: anything but
 * "off" is on (an error is a radio that is on and failing). */
bool controls_radio_on(const char *state);

/* The confirmation that stands between a tap and a radio switched on. The
 * backend never asks (radio.set_enabled has no such step); this is the
 * explicit user action's alone, so no restart or recovery can raise it. */
enum controls_confirm {
    CONTROLS_CONFIRM_NONE = 0,
    CONTROLS_CONFIRM_ANTENNA,
    CONTROLS_CONFIRM_SETUP    /* the setup question, which is also the antenna's */
};

struct controls_radio_flow {
    enum controls_confirm confirm;
};

/* A tap on the radio tile. Returns what to do now; ASK_ANTENNA also opens
 * the question (flow->confirm). A tap while the question is open does
 * nothing. */
enum controls_radio_tap controls_radio_tapped(struct controls_radio_flow *flow, const char *state);
/* The answer. Returns true when the radio is to be switched on now: only
 * for Enable on an open question. Both close the question. */
bool controls_radio_answer(struct controls_radio_flow *flow, bool enable);
/* Controls hidden, or the shell leaving it: an open question is cancelled,
 * which leaves the radio off. */
void controls_radio_dismiss(struct controls_radio_flow *flow);

/* ---- setting a unit up for RIFT (sysd's radio_setup) ------------------------ */

/* A fresh card runs radiod on the mock and meshcored disabled; the tile then
 * offers the setup instead of switching the mock: the SX1262, switched on,
 * and MeshCore started, all after one question that is also the antenna
 * question. sysd does the work and either finishes it or puts the unit back
 * as it was. */
struct controls_setup {
    bool known;   /* sysd answered radio_setup.status */
    bool needed;  /* not on the SX1262, or meshcored not enabled */
    bool running;
    bool done;    /* the last setup finished */
    bool failed;  /* the last setup failed; error says why */
    char error[200];
};

/* radio_setup.status's result (NULL: sysd did not answer) into out. */
void controls_setup_parse(const cJSON *status, struct controls_setup *out);
/* A tap on the radio tile, knowing the setup (NULL: unknown). A unit that
 * needs the setup is asked the setup question (flow->confirm SETUP); while
 * one runs a tap does nothing; otherwise as controls_radio_tapped. */
enum controls_radio_tap controls_radio_tapped_setup(struct controls_radio_flow *flow, const char *state,
                                                    const struct controls_setup *setup);
/* The answer to the setup question. Returns true when the setup is to start
 * now: only for Set up on an open setup question. Both close it. */
bool controls_radio_setup_answer(struct controls_radio_flow *flow, bool start);
/* The setup question's body, with the last failure when there was one. */
void controls_setup_body(const struct controls_setup *setup, char *out, int len);

#define CONTROLS_SETUP_TITLE "Set up the LoRa radio for RIFT?"
#define CONTROLS_SETUP_BODY "This switches to the SX1262 radio, turns it on and starts MeshCore. Connect an " \
                            "antenna first: transmitting without one may damage the RF output stage."
#define CONTROLS_SETUP_START "Set up radio"
#define CONTROLS_SETUP_RUNNING "Setting up"
#define CONTROLS_SETUP_DONE_TITLE "The LoRa radio is set up."
#define CONTROLS_SETUP_DONE_BODY "RIFT can reach the mesh now."
#define CONTROLS_SETUP_FAILED_TITLE "The radio could not be set up."
#define CONTROLS_NOTICE_OK "OK"

#define CONTROLS_ANTENNA_TITLE "Connect an antenna before enabling the radio."
#define CONTROLS_ANTENNA_BODY "Transmitting without an antenna may damage the RF output stage."
#define CONTROLS_ANTENNA_CANCEL "Cancel"
#define CONTROLS_ANTENNA_ENABLE "Enable radio"

/* ---- Bluetooth and battery, from system.status ------------------------ */

/* status: sysd's system.status result, or NULL when sysd did not answer. */
void controls_bluetooth_text(const cJSON *status, char *out, int len);
bool controls_bluetooth_available(const cJSON *status);
void controls_battery_text(const cJSON *status, char *out, int len);
/* The dot: charging, or on external power with a battery. */
bool controls_battery_dot(const cJSON *status);

/* ---- volume -------------------------------------------------------------- */

void controls_volume_text(bool available, bool muted, int percent, char *out, int len);

/* ---- layout --------------------------------------------------------------- */

struct controls_rect {
    int32_t x, y, w, h;
};

enum controls_tile {
    CONTROLS_TILE_WIFI,
    CONTROLS_TILE_BLUETOOTH,
    CONTROLS_TILE_RADIO,
    CONTROLS_TILE_BATTERY,
    CONTROLS_TILE_ROTATION,
    CONTROLS_TILE_MODE,
    CONTROLS_TILE_COUNT
};

struct controls_layout {
    int32_t margin;
    struct controls_rect back;
    struct controls_rect header;   /* title and subtitle */
    struct controls_rect tile[CONTROLS_TILE_COUNT];
    struct controls_rect brightness;
    struct controls_rect volume;
    struct controls_rect list;     /* Settings, Mesh messages, About DOORS */
    int32_t list_row_h;
    struct controls_rect lock;
    struct controls_rect power;
    struct controls_rect dialog;   /* the antenna question, centred */
};

/* What Controls is laid out in. The content area starts at the top edge
 * (there is no status bar, DS §36), so the header row lies where the panel's
 * rounded top corners are and where the status cluster sits:
 *   inset_top_left/right: what the top edge's corners take at each end
 *                         (pos_display_bar_insets(POS_EDGE_TOP)); the side
 *                         margin is raised to the larger of the two;
 *   keepout:              the status cluster's widest box, in content
 *                         coordinates; nothing is placed over it. w == 0:
 *                         no cluster. */
struct controls_frame {
    bool landscape;
    int32_t width;
    int32_t height;
    int32_t inset_top_left;
    int32_t inset_top_right;
    struct controls_rect keepout;
};

/* The layout for a frame. Returns 0, or -1 when the area is too small to
 * hold it without overlap - with each other or with the keep-out box (the
 * caller still gets the best effort). */
int controls_layout(const struct controls_frame *f, struct controls_layout *out);

/* Whether two rectangles share any pixel. */
bool controls_rects_overlap(const struct controls_rect *a, const struct controls_rect *b);

#endif
