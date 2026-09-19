/*
 * RIFT: the mesh client for Doors, phase 1.
 *
 * This header is the app's internal contract - the chrome owns it, the
 * three screen modules read it - and nothing outside apps/rift includes it.
 * The shell sees only the struct pocketos_app at the bottom of rift_app.c.
 *
 * The split is deliberate and is the point of the arrangement:
 *
 *   rift_model    what is known, and how sure it is        no LVGL
 *   rift_format   every string that is printed             no LVGL
 *   rift_ipc      the meshcored connection                 no LVGL
 *   rift_app      chrome, sections, layout, lifecycle      LVGL
 *   ui/rift_*     one screen each                          LVGL
 *
 * Phase 2 draws ACTIVITY, NODES and COMMS. NET keeps its place in the
 * navigation - the four sections are the approved design and removing one
 * would be a different design - and says plainly that it is not in this
 * build rather than showing an empty view that looks like a quiet mesh.
 *
 * COMMS is direct conversations only. The approved design merges channels
 * into the same list; the radio service has none - MAX_GROUP_CHANNELS is
 * undefined in protocols/meshcore, so upstream's channel code is not
 * compiled, and docs/api/mesh.md lists group channels under "Not in v0" -
 * so this build says so instead of drawing a channel nobody could speak on.
 *
 * This app transmits in exactly one place. mesh.send is written only by
 * rift_ipc_send_message, reached only from the composer, reached only by a
 * reader pressing SEND on text a reader typed. Nothing automatic can reach
 * it: opening a screen, a snapshot, a period expiring and a reconnect all
 * still put nothing on the air. mesh.advert is not called from anywhere in
 * apps/rift, and tests/rift_lint.sh checks both of those.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef RIFT_APP_H
#define RIFT_APP_H

#include "rift_format.h"
#include "rift_ipc.h"
#include "rift_model.h"
#include "rift_widgets.h"

#include "lvgl.h"
#include "pocketui.h"

/* The four sections, in the fixed order of the handoff §3. */
enum rift_section {
    RIFT_SEC_ACTIVITY = 0,
    RIFT_SEC_NODES,
    RIFT_SEC_COMMS,
    RIFT_SEC_NET,
    RIFT_SEC_COUNT,
};

/* How often the client takes a pass at its socket. Not the shell's
 * once-a-second tick: a subscriber that reads once a second is a subscriber
 * pocketipc disconnects under a burst (docs/api/pocketipc.md, backpressure),
 * and an app that only redraws on the tick shows a mesh a second late. */
#define RIFT_POLL_MS 100
/* Ages move on their own, so the screen is repainted at least this often
 * even when nothing arrived. */
#define RIFT_REPAINT_MS 1000

/* The landscape split needs room for a list and a context pane side by
 * side. Below this the app stays in its single column, whatever the shape
 * of the box: a 1232 px landscape body splits, a keyboard-shrunk one may
 * not, and the rule is the room rather than the orientation (DS §21.3). */
#define RIFT_SPLIT_MIN_W 900
#define RIFT_CONTEXT_W 552

struct rift_nodes;
struct rift_activity_view;
struct rift_comms;

struct rift_app {
    lv_obj_t *root;   /* the shell's body */
    lv_obj_t *frame;  /* exactly the body's content box */
    lv_obj_t *strip;  /* the section strip */
    lv_obj_t *tab[RIFT_SEC_COUNT];
    lv_obj_t *tab_rule[RIFT_SEC_COUNT];
    lv_obj_t *tab_label[RIFT_SEC_COUNT];
    lv_obj_t *tab_pill[RIFT_SEC_COUNT];
    lv_obj_t *content;
    lv_obj_t *cmdline;
    lv_obj_t *cmd_hint;
    lv_obj_t *keysink;
    /* The landscape composer. The command line *is* the composer in
     * landscape (handoff §8), and the command line is chrome, so the field
     * lives here rather than in the COMMS screen; portrait has its own, in
     * the thread, where the design puts it. */
    lv_obj_t *composer;
    lv_obj_t *cmd_send_hint; /* "TO HYTTA · FLOOD · ENTER SEND · ESC CLEAR" */
    int composer_focused;

    struct pocketui_layout_guard layout_guard;
    /* A refresh is owed from outside a layout pass.
     *
     * The size-changed handler runs *inside* LVGL's layout update, and
     * lv_obj_update_layout() is a no-op while one is running (it takes a
     * mutex and returns). Anything that has to measure a settled width -
     * every name and preview fitted to its column - therefore cannot be
     * done from there: it would measure whatever the pass had reached so
     * far. So the layout asks for a refresh, and the timer does it, outside
     * the pass, where a layout can actually be forced. */
    int refresh_pending;
    /* Esc in the composer asks for the list's focus back. Like the refresh,
     * it is done from the timer and not from the key handler: changing the
     * group's focus from inside the event LVGL is dispatching does not
     * stick. */
    int focus_list_pending;
    int wide; /* the landscape split is on */
    int32_t body_w;
    int32_t body_h;

    enum rift_section section;
    /* Portrait only: the pushed DETAIL screen is up. In landscape the same
     * content is the right pane and nothing is pushed. */
    int detail_open;
    char selected[RIFT_KEY_HEX];
    int have_selected;
    /* The open conversation, which is a different selection from the
     * selected node: a reader can be reading one node's detail and writing
     * to another, and collapsing the two would move one when they moved the
     * other. */
    char conv[RIFT_KEY_HEX];
    int have_conv;

    struct rift_model model;
    struct rift_ipc ipc;

    lv_timer_t *pump;
    unsigned drawn_revision;
    int64_t last_repaint_ms;

    struct rift_nodes *nodes;
    struct rift_activity_view *activity;
    struct rift_comms *comms;
    lv_obj_t *activity_root;
    lv_obj_t *nodes_root;
    lv_obj_t *comms_root;
    lv_obj_t *placeholder;
    /* Set only when pos_theme_watch's table was full and this app had to
     * listen for the theme event itself; it is removed from the screen on
     * destroy, because the screen outlives the app. */
    lv_obj_t *theme_host;
};

/* Chrome, for the screens. */
void rift_app_select(struct rift_app *a, const char *key);
/* Open a conversation with this peer, in COMMS. Selecting a conversation
 * never sends anything; it chooses where the composer would write. */
void rift_app_open_conversation(struct rift_app *a, const char *key);
void rift_app_open_detail(struct rift_app *a, int open);
void rift_app_show_section(struct rift_app *a, enum rift_section section);
/* Everything on screen, from the model as it stands. Cheap enough to call
 * on every repaint: the screens update labels in place and only rebuild
 * when the set of rows itself changed. */
void rift_app_refresh(struct rift_app *a);
/* The selected node, or NULL when nothing is selected or the selection has
 * left the cache (a snapshot that no longer holds it). */
const struct rift_node *rift_app_selected(const struct rift_app *a);
/* meshcored's monotonic clock as this app reads it. */
int64_t rift_app_now(const struct rift_app *a);

/* The glyph a node's row and its captions carry. */
enum rift_glyph rift_app_glyph(const struct rift_node *n, int64_t now_ms);
/* Name a hop hash from the cache, for the path chain and ladder. */
const char *rift_app_resolve(const char *hop_id, void *user);

#endif
