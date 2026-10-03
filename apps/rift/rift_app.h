/*
 * RIFT: the mesh client for Doors.
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
 * RIFT draws all four sections of the approved design: ACTIVITY, NODES,
 * COMMS and NET - the hop rings of handoff §7, from the node cache
 * (rift_net.h for the placement, ui/rift_netview.c for the drawing).
 *
 * COMMS holds direct conversations and the channels the service reported,
 * in one list; a channel is a conversation keyed "#<slot>" (rift_model.h).
 *
 * This app transmits in two places, each reached only by a reader's press.
 * mesh.send is written only by rift_ipc_send_message, reached only from the
 * composer, on text a reader typed. mesh.advert is written only by
 * rift_ipc_send_advert, reached only from ACTIVITY's two ADVERT buttons.
 * Nothing automatic can reach either: opening a screen, a snapshot, a period
 * expiring and a reconnect all still put nothing on the air.
 * tests/rift_lint.sh checks each link, and tests/rift_ipc_test.c proves it
 * from the service's side.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef RIFT_APP_H
#define RIFT_APP_H

#include "rift_format.h"
#include "rift_ipc.h"
#include "rift_model.h"
#include "rift_notify.h"
#include "rift_store.h"
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
/* How long the list says a node was forgotten. The node has left the list,
 * and with it the detail that would have said so. */
#define RIFT_ACTION_NOTE_MS 30000

struct rift_nodes;
struct rift_activity_view;
struct rift_comms;
struct rift_find;
struct rift_net_view;
struct rift_manage;
struct rift_device;
struct rift_session_view;

/* The app's id in the shell's registry, and the name its session is marked
 * by in the status cluster. */
#define RIFT_APP_ID "rift"
/* The status cluster's word for a session running behind another screen
 * (app.h pocketos_shell_set_background, DS §51), and what it means. */
#define RIFT_BACKGROUND_LABEL "RIFT"
#define RIFT_BACKGROUND_HELP "RIFT active in background"

/* One block, in two halves (DS §51).
 *
 * RIFT's session outlives its screen. Leaving the app with Back or Home
 * takes the screen away and nothing else: the meshcored connection, its
 * subscription, the model built from it - activity, traffic, messages, read
 * marks - and where the reader was stay, and the pump timer goes on taking
 * the mesh in. The next open builds a screen over the same block. The
 * session ends only when it is asked to: CLOSE RIFT on ACTIVITY (after a
 * confirmation), or the Doors shell stopping or re-executing (app.h
 * shutdown).
 *
 * Everything from the top down to `section` is the screen's: LVGL objects,
 * the screens' private blocks and the layout's bookkeeping. Leaving clears
 * exactly that range (rift_app.c screen_forget), so a pointer into a deleted
 * screen cannot survive into the next open. From `section` on is the
 * session's. A new field goes in the half it belongs to. */
struct rift_app {
    /* ---- the screen: built by every open, cleared by every leave ---- */
    lv_obj_t *root;   /* the shell's body */
    lv_obj_t *frame;  /* exactly the body's content box */
    lv_obj_t *strip;  /* the section strip */
    lv_obj_t *back;   /* the strip's back slab, landscape only (DS §37.2) */
    lv_obj_t *tab[RIFT_SEC_COUNT];
    lv_obj_t *tab_rule[RIFT_SEC_COUNT];
    lv_obj_t *tab_label[RIFT_SEC_COUNT];
    lv_obj_t *tab_pill[RIFT_SEC_COUNT];
    lv_obj_t *content;
    /* The command line: present only as the landscape composer, or to say
     * the service is not answering (cmd_status). Otherwise hidden, and the
     * section above it has the room. */
    lv_obj_t *cmdline;
    lv_obj_t *cmd_status;
    /* The strip's right caption: the landscape key hints and counts. */
    lv_obj_t *cmd_hint;
    /* The app's one key sink: 1 px, outside the command line, never hidden. */
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
    /* The shape changed and the chrome - the strip, the command line and
     * its field - has heights to take for it. Done from the timer, never
     * from the layout pass that noticed: those are the frame's own flex
     * children, and resizing them inside the frame's size-changed event
     * kept LVGL laying the frame out for ever (found on the turn back to
     * portrait with a thousand nodes, 2026-09-28). */
    int chrome_pending;
    /* Esc in the composer asks for the list's focus back. Like the refresh,
     * it is done from the timer and not from the key handler: changing the
     * group's focus from inside the event LVGL is dispatching does not
     * stick. */
    int focus_list_pending;
    /* A conversation was opened: its composer - the command line's field in
     * landscape, the thread's in portrait - takes the keys as soon as it
     * can, so typing needs no tap first. From the timer, for the reason
     * above, and only from the list's key sink or the other composer: a
     * dialog or a form that holds the focus keeps it. */
    int focus_composer_pending;
    /* The composer has the focus because a conversation was opened, not
     * because a reader went to it with TAB: Esc on it, empty, is then the
     * list's Esc (back to ACTIVITY), as it was before the composer took the
     * focus by itself. */
    int composer_auto;
    int wide; /* the landscape split is on */
    int32_t body_w;
    int32_t body_h;
    /* The strip is the screen's top row (landscape: no shell header above
     * it), and how far its ends keep in from the rounded top corners. */
    int strip_at_top;
    int32_t strip_inset_left;
    int32_t strip_inset_right;
    unsigned drawn_revision;
    int64_t last_repaint_ms;

    struct rift_nodes *nodes;
    struct rift_activity_view *activity;
    struct rift_comms *comms;
    struct rift_find *find;
    struct rift_net_view *net;
    struct rift_manage *manage;
    struct rift_device *device;
    struct rift_session_view *session;
    lv_obj_t *activity_root;
    lv_obj_t *nodes_root;
    lv_obj_t *comms_root;
    lv_obj_t *net_root;
    /* The screen this app listens on for the theme-changed event (the
     * colour-emoji styles follow the text size there, and the frame is
     * repainted); removed on destroy, because the screen outlives the app. */
    lv_obj_t *theme_host;

    /* ---- the session: from here to the end, kept while RIFT is left ---- */
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
    /* Landscape COMMS: the details pane (route, signal, tally) beside the
     * thread is shown only while this is set - a tap on the thread's header
     * turns it over - so the thread has the width the rest of the time
     * (DS §37.2). Portrait has no such pane. */
    int details_open;
    /* NODES' find bar (ui/rift_find.c): what a reader typed, and whether the
     * list shows only the repeaters heard zero-hop. A view of the list and
     * nothing more - neither changes a node, and neither is stored. */
    char node_query[RIFT_QUERY_MAX];
    int node_zero_hop;

    struct rift_model model;
    struct rift_ipc ipc;

    /* The reader's own choices (rift_store.h) and the DM sound they govern
     * (rift_notify.h, rift_sound.h). prefs_saved is 0 after a change that
     * could not be written: the choice holds until RIFT closes, and the
     * screen says so. */
    struct rift_prefs prefs;
    int prefs_saved;
    struct rift_notify notify;

    /* The session's timer, not the screen's: with no screen it still reads
     * the socket, so the subscription is never left to back up (pocketipc
     * disconnects a subscriber that stops reading) and nothing that arrives
     * while RIFT is left is lost. */
    lv_timer_t *pump;
    /* CLOSE RIFT was confirmed: the next destroy ends the session rather
     * than keeping it. */
    int ending;
    /* How many times a screen was built over this session (for the tests
     * and the log: 1 is a fresh open). */
    unsigned opens;
};

/* Chrome, for the screens. */
void rift_app_select(struct rift_app *a, const char *key);
/* Open a conversation with this peer, in COMMS. Selecting a conversation
 * never sends anything; it chooses where the composer would write. */
void rift_app_open_conversation(struct rift_app *a, const char *key);
void rift_app_open_detail(struct rift_app *a, int open);
void rift_app_show_section(struct rift_app *a, enum rift_section section);
/* Landscape COMMS: show or hide the details pane beside the thread. */
void rift_app_toggle_details(struct rift_app *a);
/* Whether the landscape command line is the composer right now. */
int rift_app_composer_live(const struct rift_app *a);
/* The composers and the keys (rift_focus.c). attach follows both fields'
 * keys and focus, once per screen built; composer gives the open
 * conversation's field the focus when focus_composer_pending asks and
 * nothing else that should keep the keys has them - called from the timer,
 * never from inside an LVGL event. */
void rift_focus_attach(struct rift_app *a);
void rift_focus_composer(struct rift_app *a);
/* Everything on screen, from the model as it stands. Cheap enough to call
 * on every repaint: the screens update labels in place and only rebuild
 * when the set of rows itself changed. */
void rift_app_refresh(struct rift_app *a);
/* The selected node, or NULL when nothing is selected or the selection has
 * left the cache (a snapshot that no longer holds it). */
const struct rift_node *rift_app_selected(const struct rift_app *a);
/* meshcored's monotonic clock as this app reads it. */
int64_t rift_app_now(const struct rift_app *a);

/* CLOSE RIFT, confirmed: end the session - the connection given back, the
 * model released, the status cluster's RIFT gone - and go home. Ends only
 * what is RIFT's: meshcored and the radio are not touched. */
void rift_app_end(struct rift_app *a);
/* The session, whether or not a screen is over it; NULL when RIFT is not
 * running at all. For the tests and the shell's own bookkeeping. */
struct rift_app *rift_app_session(void);
/* Whether the session is running with no screen over it. */
int rift_app_in_background(void);

/* rift_background.c, for rift_app.c's create and destroy. */
/* A new block: the model, the connection (not yet attempted), the reader's
 * stored choices, ACTIVITY. Not the session until adopted. */
struct rift_app *rift_bg_new(void);
void rift_bg_adopt(struct rift_app *a);
/* End the session, if there is one. */
void rift_bg_end(void);
/* Clear the screen's half of the block (everything before `section`). */
void rift_bg_forget_screen(struct rift_app *a);

/* The DM sound's setting: applied at once, and stored. */
void rift_app_set_dm_sound(struct rift_app *a, int on);
/* Whether a sound could be heard now: a backend that can play one, and
 * Doors not muted. The setting is a separate question. */
int rift_app_can_sound(const struct rift_app *a);
/* After every pass at the socket: a sound, if a direct message has just
 * arrived and the setting, the platform and the policy all say yes. */
void rift_app_notify_pass(struct rift_app *a, int64_t now);

/* The glyph a node's row and its captions carry. */
enum rift_glyph rift_app_glyph(const struct rift_node *n, int64_t now_ms);
/* Name a hop hash from the cache, for the path chain and ladder. */
const char *rift_app_resolve(const char *hop_id, void *user);

#endif
