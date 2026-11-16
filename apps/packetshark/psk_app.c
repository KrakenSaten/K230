/*
 * PacketShark: a packet analyzer for Doors (docs/apps/PACKETSHARK.md;
 * ADR-016 and DS §59 accepted). Milestone M3: Ethernet capture from the
 * launcher - setup, START, live counters, HOLD, STOP, the error state.
 *
 * The shell is a viewport (DESIGN_DECISIONS D1, accepted): the capture
 * socket, the decoder and every packet are in pos-capture, one helper per
 * START, started here through psk_session and polled from an LVGL timer
 * without ever blocking. The app keeps counters and at most a screen of
 * text. Leaving the app ends the helper, and with it the capture: nothing
 * is written to storage (D10, accepted). The screen stays on while the app
 * is in front (shell.c awake_apps, D23), so a capture never runs behind a
 * dark panel.
 *
 * The interfaces come from sysd (system.status.network, docs/api/system.md),
 * asked when the app opens and every PSK_IFACE_POLL_MS while the setup view
 * shows. Only an Ethernet interface with a carrier can be chosen; Wi-Fi is
 * listed and not selectable in this version (D18).
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "psk_app.h"

#include "pocketlog/pocketlog.h"
#include "shell_ipc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const uint32_t psk_limit_min[PSK_LIMITS] = { 5, 15, 60, 0 };
#define LIMIT_DEFAULT 1 /* 15 minutes (ADR-016 rule 6) */

int64_t psk_now(void)
{
    return (int64_t)lv_tick_get();
}

/* ---- interfaces ------------------------------------------------------------------------ */

static bool usable(const struct psk_iface *f)
{
    return !f->wifi && f->carrier;
}

static bool capture_name(const char *n)
{
    return strncmp(n, "eth", 3) == 0 || strncmp(n, "en", 2) == 0 || strncmp(n, "usb", 3) == 0 ||
           strncmp(n, "wlan", 4) == 0;
}

static void read_ifaces(struct psk_app *a)
{
    char keep[PPC_IFACE_MAX] = "";
    char err[96];
    cJSON *st;
    const cJSON *net;
    const cJSON *it;
    int i;

    if (a->sel >= 0 && a->sel < a->nifs) {
        snprintf(keep, sizeof(keep), "%s", a->ifs[a->sel].name);
    }
    a->nifs = 0;
    if (a->simulated) {
        /* The simulator, or a replay: one stand-in, always ready. */
        snprintf(a->ifs[0].name, sizeof(a->ifs[0].name), "synth0");
        a->ifs[0].synthetic = true;
        a->ifs[0].up = true;
        a->ifs[0].carrier = true;
        a->ifs[0].wifi = false;
        a->ifs[0].ipv4[0] = '\0';
        a->nifs = 1;
    } else {
        st = shell_ipc_call_timeout("sysd", "system.status", NULL, SHELL_IPC_UI_TIMEOUT_MS, err, sizeof(err));
        net = st ? cJSON_GetObjectItemCaseSensitive(st, "network") : NULL;
        cJSON_ArrayForEach(it, net)
        {
            const cJSON *name = cJSON_GetObjectItemCaseSensitive(it, "name");
            const cJSON *op = cJSON_GetObjectItemCaseSensitive(it, "operstate");
            const cJSON *carrier = cJSON_GetObjectItemCaseSensitive(it, "carrier");
            const cJSON *ip = cJSON_GetObjectItemCaseSensitive(it, "ipv4");
            struct psk_iface *f;

            if (a->nifs >= PSK_IFACES || !cJSON_IsString(name) || !ppc_iface_ok(name->valuestring) ||
                !capture_name(name->valuestring)) {
                continue;
            }
            f = &a->ifs[a->nifs++];
            memset(f, 0, sizeof(*f));
            snprintf(f->name, sizeof(f->name), "%s", name->valuestring);
            f->wifi = strncmp(f->name, "wlan", 4) == 0;
            f->up = cJSON_IsString(op) && strcmp(op->valuestring, "down") != 0;
            f->carrier = cJSON_IsTrue(carrier);
            if (cJSON_IsString(ip)) {
                snprintf(f->ipv4, sizeof(f->ipv4), "%s", ip->valuestring);
            }
        }
        cJSON_Delete(st);
    }
    /* Keep the choice while it stays usable; otherwise the first usable
     * interface, so "plug in, open, START" is two taps (PRODUCT_SPEC F1). */
    a->sel = -1;
    for (i = 0; i < a->nifs; i++) {
        if (usable(&a->ifs[i]) && strcmp(a->ifs[i].name, keep) == 0) {
            a->sel = i;
        }
    }
    for (i = 0; i < a->nifs && a->sel < 0; i++) {
        if (usable(&a->ifs[i])) {
            a->sel = i;
        }
    }
    a->next_ifs_ms = psk_now() + PSK_IFACE_POLL_MS;
}

/* ---- the helper ------------------------------------------------------------------------- */

static void start_capture(struct psk_app *a)
{
    struct psk_session_config cfg = { NULL, NULL };
    char err[128];

    if (a->sel < 0 || a->starting) {
        return;
    }
    a->notice[0] = '\0';
    if (psk_session_active(a->session)) {
        /* A spent helper (a stopped capture) is never reused: a helper
         * captures once (ADR-016 rule 17). */
        psk_session_abandon(a->session, PSK_DESTROY_GRACE_MS);
    }
    if (psk_session_start(a->session, &cfg, psk_now(), err, sizeof(err)) != 0) {
        LOG_WARN("packetshark: could not start the helper: %s", err);
        snprintf(a->notice, sizeof(a->notice), "THE CAPTURE HELPER COULD NOT START");
        return;
    }
    psk_model_helper_started(a->model, psk_now());
    a->starting = true;
    a->pending_start = true;
}

static const char *refusal(const char *code)
{
    static const struct {
        const char *code;
        const char *text;
    } map[] = {
        { "busy", "ANOTHER CAPTURE IS RUNNING" },
        { "nodev", "THE INTERFACE IS GONE" },
        { "down", "THE INTERFACE IS DOWN" },
        { "perm", "NOT PERMITTED TO CAPTURE" },
        { "notether", "THIS INTERFACE CANNOT BE CAPTURED" },
        { "nobackend", "THIS BUILD CANNOT CAPTURE LIVE TRAFFIC" },
        { "filter", "THE CAPTURE FILTER WAS REFUSED" },
        { "nomem", "NOT ENOUGH MEMORY FOR THE CAPTURE" },
    };
    unsigned i;

    for (i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (strcmp(code, map[i].code) == 0) {
            return map[i].text;
        }
    }
    return "THE CAPTURE COULD NOT START";
}

/* What the poll brought, beyond painting. */
static void react(struct psk_app *a, unsigned changed)
{
    struct psk_model *m = a->model;

    if (a->pending_start && m->hello && m->state == PPC_STATE_IDLE) {
        const struct psk_iface *f = a->sel >= 0 ? &a->ifs[a->sel] : NULL;

        a->pending_start = false;
        if (!f || psk_session_capture(a->session, f->name, a->keep, 0, a->cfilter,
                                      psk_limit_min[a->limit_idx] * 60u) != 0) {
            snprintf(a->notice, sizeof(a->notice), "THE CAPTURE COULD NOT START");
            a->starting = false;
            psk_session_discard(a->session);
        }
    }
    if (a->starting && m->had_capture && m->state != PPC_STATE_IDLE) {
        a->starting = false;
        LOG_INFO("packetshark: capturing on %s", m->iface);
    }
    if ((changed & PSK_CHANGED_ERROR) && a->starting) {
        /* The helper refused to start: say why on the setup screen and let
         * the idle helper go. */
        snprintf(a->notice, sizeof(a->notice), "%s", refusal(m->err_code));
        LOG_WARN("packetshark: capture refused (%s)", m->err_code);
        a->starting = false;
        a->pending_start = false;
        psk_session_discard(a->session);
    }
    if (changed & PSK_CHANGED_EXITED) {
        bool bad = m->exit != PSK_EXIT_NORMAL;

        a->discarding = false;

        if (a->starting || a->pending_start) {
            a->starting = false;
            a->pending_start = false;
            if (bad && !a->notice[0]) {
                snprintf(a->notice, sizeof(a->notice), "%s",
                         m->exit == PSK_EXIT_START ? "THE CAPTURE HELPER IS NOT INSTALLED"
                                                   : "THE CAPTURE HELPER DID NOT ANSWER");
            }
        }
        if (bad && m->had_capture) {
            char why[48];

            /* The packets went with it (D10). Said on screen, logged here
             * without a word of what was captured. */
            a->failed_view = true;
            LOG_WARN("packetshark: the capture helper ended: %s",
                     psk_exit_text(m->exit, m->exit_value, why, sizeof(why)));
        }
        if (a->sheet_kind == PSK_SHEET_DISCARD || a->sheet_kind == PSK_SHEET_NEW ||
            a->sheet_kind == PSK_SHEET_LEAVE) {
            psk_sheet_hide(a);
        }
        read_ifaces(a);
    }
}

static void layout(struct psk_app *a);

static void on_poll(lv_timer_t *t)
{
    struct psk_app *a = lv_timer_get_user_data(t);
    unsigned changed = psk_session_poll(a->session, a->model, psk_now());

    react(a, changed);
    if (a->relayout) {
        /* Never inside a layout pass: the size event only marks it. */
        a->relayout = false;
        layout(a);
    }
    if (a->view == PSK_VIEW_SETUP && !a->starting && psk_now() >= a->next_ifs_ms) {
        int before = a->sel;
        int nbefore = a->nifs;

        read_ifaces(a);
        if (before != a->sel || nbefore != a->nifs) {
            changed |= PSK_CHANGED_STATE;
        }
        psk_paint_ifaces(a);
    }
    if (changed) {
        psk_live_paint(a, changed);
    }
}

/* ---- actions ------------------------------------------------------------------------------ */

void psk_select_tab(struct psk_app *a, enum psk_tab t)
{
    if (t == PSK_TAB_INSPECT) {
        return; /* nothing can be opened in this version */
    }
    a->cur = t;
    psk_paint_tabs(a);
}

void psk_select_iface(struct psk_app *a, int index)
{
    if (index >= 0 && index < a->nifs && usable(&a->ifs[index])) {
        a->sel = index;
        psk_live_paint(a, PSK_CHANGED_STATE);
    }
}

void psk_option(struct psk_app *a, int which)
{
    switch (which) {
    case 0:
        if (a->keep == PPC_KEEP_FULL) {
            a->keep = PPC_KEEP_HEADERS;
        } else {
            psk_sheet_show(a, PSK_SHEET_FULL);
            return;
        }
        break;
    case 1:
        if (psk_limit_min[(a->limit_idx + 1) % PSK_LIMITS] == 0) {
            psk_sheet_show(a, PSK_SHEET_NOLIMIT);
            return;
        }
        a->limit_idx = (a->limit_idx + 1) % PSK_LIMITS;
        break;
    case 2:
        a->cfilter = (enum ppc_cfilter)((a->cfilter + 1) % PPC_CFILTER_COUNT);
        break;
    default:
        return; /* promiscuous mode: not offered in this version */
    }
    psk_paint_options(a);
}

static void back_to_setup(struct psk_app *a)
{
    a->failed_view = false;
    a->starting = false;
    a->pending_start = false;
    /* FULL and no limit last one capture (ADR-016 rule 7, ARCHITECTURE §9). */
    a->keep = PPC_KEEP_HEADERS;
    if (psk_limit_min[a->limit_idx] == 0) {
        a->limit_idx = LIMIT_DEFAULT;
    }
    read_ifaces(a);
    psk_live_paint(a, PSK_CHANGED_STATE);
}

void psk_do(struct psk_app *a, enum psk_action act)
{
    switch (act) {
    case PSK_ACT_START:
        start_capture(a);
        break;
    case PSK_ACT_HOLD:
        psk_session_hold(a->session, true);
        break;
    case PSK_ACT_RESUME:
        psk_session_hold(a->session, false);
        break;
    case PSK_ACT_STOP:
        psk_session_stop(a->session);
        break;
    case PSK_ACT_DISCARD:
        psk_sheet_show(a, PSK_SHEET_DISCARD);
        return;
    case PSK_ACT_NEW:
        if (a->failed_view || !psk_model_has_packets(a->model)) {
            back_to_setup(a);
            return;
        }
        psk_sheet_show(a, PSK_SHEET_NEW);
        return;
    case PSK_ACT_FILTER:
    case PSK_ACT_SAVE:
    case PSK_ACT_NONE:
    default:
        return;
    }
    psk_live_paint(a, PSK_CHANGED_STATE);
}

void psk_sheet_answer(struct psk_app *a, bool go)
{
    enum psk_sheet kind = a->sheet_kind;

    psk_sheet_hide(a);
    if (!go) {
        return;
    }
    switch (kind) {
    case PSK_SHEET_FULL:
        a->keep = PPC_KEEP_FULL;
        psk_paint_options(a);
        break;
    case PSK_SHEET_NOLIMIT:
        a->limit_idx = PSK_LIMITS - 1;
        psk_paint_options(a);
        break;
    case PSK_SHEET_DISCARD:
    case PSK_SHEET_NEW:
        /* The helper frees its packets and leaves; the poll sees it go and
         * the setup view returns. Never a wait on the LVGL thread. */
        psk_session_discard(a->session);
        a->discarding = psk_session_active(a->session);
        back_to_setup(a);
        break;
    case PSK_SHEET_LEAVE:
        pocketos_shell_go_home();
        break;
    default:
        break;
    }
}

/* ---- layout ------------------------------------------------------------------------------- */

static void layout(struct psk_app *a)
{
    struct pos_insets in;

    if (!pocketui_layout_begin(&a->guard, a->frame, &in)) {
        return;
    }
    /* Nothing inside the panel's unsafe corners (DS §21.3). */
    lv_obj_set_style_pad_left(a->frame, in.left, 0);
    lv_obj_set_style_pad_right(a->frame, in.right, 0);
    lv_obj_set_style_pad_top(a->frame, in.top, 0);
    lv_obj_set_style_pad_bottom(a->frame, in.bottom, 0);
}

static void on_frame_size(lv_event_t *e)
{
    struct psk_app *a = lv_event_get_user_data(e);

    a->relayout = true;
}

/* ---- the app ------------------------------------------------------------------------------ */

static void *psk_create(lv_obj_t *root)
{
    struct psk_app *a = calloc(1, sizeof(*a));

    if (!a) {
        return NULL;
    }
    a->session = malloc(sizeof(*a->session));
    a->model = malloc(sizeof(*a->model));
    if (!a->session || !a->model) {
        free(a->session);
        free(a->model);
        free(a);
        return NULL;
    }
    psk_session_init(a->session);
    psk_model_init(a->model);
    a->sel = -1;
    a->keep = PPC_KEEP_HEADERS;
    a->limit_idx = LIMIT_DEFAULT;
    a->cfilter = PPC_CFILTER_ALL;
    a->simulated = psk_session_default_source() != NULL;
    a->cur = PSK_TAB_LIVE;

    a->frame = psk_box(root);
    lv_obj_set_size(a->frame, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(a->frame, LV_FLEX_FLOW_COLUMN);
    psk_live_build(a);
    psk_sheet_build(a);
    read_ifaces(a);
    psk_paint_tabs(a);
    psk_live_paint(a, ~0u);
    a->timer = lv_timer_create(on_poll, PSK_POLL_MS, a);
    lv_obj_add_event_cb(a->frame, on_frame_size, LV_EVENT_SIZE_CHANGED, a);
    lv_obj_update_layout(a->frame);
    layout(a);
    return a;
}

static void psk_destroy(void *priv)
{
    struct psk_app *a = priv;

    if (!a) {
        return;
    }
    lv_obj_remove_event_cb_with_user_data(a->frame, on_frame_size, a);
    if (a->timer) {
        lv_timer_delete(a->timer);
    }
    /* Leaving the app ends the capture and discards its packets (ADR-016
     * rule 5): at most PSK_DESTROY_GRACE_MS + PSK_KILL_REAP_MS. */
    if (psk_session_active(a->session)) {
        LOG_INFO("packetshark: closed, the capture helper is ended");
    }
    psk_session_abandon(a->session, PSK_DESTROY_GRACE_MS);
    free(a->session);
    free(a->model);
    free(a);
}

/* The Back action (UI_UX.md §4.3): a sheet closes; a capture with packets in
 * memory asks before it is discarded; otherwise the shell goes home. */
static int psk_back(void *priv)
{
    struct psk_app *a = priv;

    if (!a) {
        return 0;
    }
    if (a->sheet_kind != PSK_SHEET_NONE) {
        psk_sheet_hide(a);
        return 1;
    }
    if (a->cur != PSK_TAB_LIVE) {
        psk_select_tab(a, PSK_TAB_LIVE);
        return 1;
    }
    if (psk_model_has_packets(a->model)) {
        psk_sheet_show(a, PSK_SHEET_LEAVE);
        return 1;
    }
    return 0;
}

LV_IMAGE_DECLARE(pos_app_icon_packetshark);

const struct pocketos_app app_packetshark = {
    .id = "packetshark",
    .name = "PacketShark",
    .icon = LV_SYMBOL_WIFI,
    /* First-party, drawn in the icon extension's line language: a fin over a
     * baseline that runs on as stepped bars (docs/design/doors-app-icons,
     * the concept the owner accepted with DS §59.8). */
    .icon_mask = &pos_app_icon_packetshark,
    .create = psk_create,
    .tick = NULL,
    .destroy = psk_destroy,
    /* Fullscreen (DS §30.8): no status cluster competes with the state hint
     * in the header. */
    .chrome = POCKETOS_CHROME_NONE,
    .back = psk_back,
    /* The header's back slab leaves the app; Back asks first (above). */
    .back_slab_in_app = true,
};
