/*
 * PacketShark: a packet analyzer for Doors (docs/apps/PACKETSHARK.md;
 * ADR-016 and DS §59 accepted). Milestones M3 to M6: Ethernet capture from
 * the launcher - setup, START, live counters, HOLD, STOP, the error state;
 * the traffic graph (M4); the packet list with its filter and sort (M5); the
 * inspector and pins (M6).
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
#include <time.h>

const uint32_t psk_limit_min[PSK_LIMITS] = { 5, 15, 60, 0 };
#define LIMIT_DEFAULT 1 /* 15 minutes (ADR-016 rule 6) */

#define COST_LOG_MS 60000

int64_t psk_now(void)
{
    return (int64_t)lv_tick_get();
}

static int64_t now_us(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
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

/* ---- the list and the inspector (M5, M6) ---------------------------------------------- */

bool psk_capture_present(const struct psk_app *a)
{
    return psk_model_has_packets(a->model) && !a->discarding && !a->failed_view;
}

/* The list's place, back to the newest packets and no packet open: a new
 * capture, or none. The filter stays as it was chosen. */
static void reset_list(struct psk_app *a)
{
    a->follow = a->sort != 2;
    a->top_rank = 0;
    a->top_seq = 0;
    a->jump = PSK_JUMP_NONE;
    a->sel_seq = 0;
    a->insp_seq = 0;
    a->insp_shown = 0;
    a->insp_note[0] = '\0';
    a->window_due = true;
    a->window_inflight = false;
    a->window_last_seq = 0;
    if (a->cur == PSK_TAB_INSPECT) {
        a->cur = PSK_TAB_PACKETS;
    }
}

static void send_view(struct psk_app *a)
{
    a->view_id++;
    if (psk_session_view(a->session, a->view_id, a->sort, a->classes, a->host, a->port, a->pinned_only) != 0) {
        LOG_DEBUG("packetshark: the view could not be sent");
    }
    a->window_due = true;
    a->window_inflight = false;
}

static void send_window(struct psk_app *a)
{
    uint32_t vis = (uint32_t)(a->vis > 0 ? a->vis : 1);
    uint32_t before;
    uint32_t after;
    enum ppc_anchor anchor;
    uint64_t at = 0;

    if (a->follow) {
        anchor = PPC_ANCHOR_TAIL;
        before = vis - 1u;
        after = vis - 1u;
    } else {
        /* Rows above and below the top one, so a short drag is painted
         * from what is here while the next window is on its way. */
        before = vis;
        after = 2u * vis - 1u;
        switch (a->jump) {
        case PSK_JUMP_RANK:
            anchor = PPC_ANCHOR_RANK;
            at = a->jump_at;
            break;
        case PSK_JUMP_TIME:
            anchor = PPC_ANCHOR_US;
            at = a->jump_at;
            before = 0;
            break;
        default:
            if (a->top_seq) {
                anchor = PPC_ANCHOR_SEQ;
                at = a->top_seq;
            } else {
                anchor = PPC_ANCHOR_RANK;
                at = a->top_rank;
            }
            break;
        }
    }
    if (before > PPC_WINDOW_MAX - 1u) {
        before = PPC_WINDOW_MAX - 1u;
    }
    if (after > PPC_WINDOW_MAX - 1u - before) {
        after = PPC_WINDOW_MAX - 1u - before;
    }
    if (psk_session_window_at(a->session, a->model, anchor, at, before, after) == 0) {
        a->window_inflight = true;
        a->window_sent_ms = psk_now();
        a->window_last_seq = a->model->c.last_seq;
        a->window_due = false;
    }
}

/* Ask for a window when the list needs one: shown, and something moved -
 * its place, its filter, new packets while it follows them, or a second
 * gone by while it is held (the ranks shift as the buffer turns). One
 * request at a time. */
static void list_poll(struct psk_app *a)
{
    const struct psk_model *m = a->model;
    int64_t now = psk_now();
    bool due = a->window_due;

    if (!psk_capture_present(a) || a->cur != PSK_TAB_PACKETS) {
        return;
    }
    if (a->retry_ms && now < a->retry_ms) {
        return;
    }
    a->retry_ms = 0;
    if (a->window_inflight && now - a->window_sent_ms < 600) {
        return;
    }
    if (!due && a->follow && m->c.last_seq != a->window_last_seq && now - a->window_sent_ms >= PSK_FOLLOW_REFRESH_MS) {
        due = true;
    }
    if (!due && !a->follow && m->state != PPC_STATE_STOPPED && now - a->window_sent_ms >= PSK_HELD_REFRESH_MS) {
        due = true;
    }
    if (due) {
        send_window(a);
    }
}

static uint32_t max_top(const struct psk_app *a)
{
    uint32_t match = a->model->rows_match;

    return match > (uint32_t)a->vis ? match - (uint32_t)a->vis : 0;
}

static void top_seq_from_rows(struct psk_app *a)
{
    const struct psk_model *m = a->model;
    int64_t k = (int64_t)a->top_rank - (int64_t)m->rows_first_rank;

    if (k >= 0 && k < (int64_t)m->row_count) {
        a->top_seq = m->rows[k].seq;
    }
}

/* A window arrived: where the list now starts. */
static void rows_arrived(struct psk_app *a)
{
    const struct psk_model *m = a->model;
    uint32_t vis = (uint32_t)(a->vis > 0 ? a->vis : 1);

    a->window_inflight = false;
    if (m->rows_filter_id != a->view_id) {
        a->window_due = true; /* rows of a filter that has since changed */
        return;
    }
    if (a->dragging) {
        /* The finger leads: keep its place, take what rows came. */
        top_seq_from_rows(a);
    } else if (a->follow) {
        a->top_rank = m->rows_first_rank;
        if (a->sort == 0 && m->row_count > vis) {
            a->top_rank += m->row_count - vis;
        }
        top_seq_from_rows(a);
    } else {
        a->top_rank = m->rows_anchor;
        if (a->top_rank > max_top(a)) {
            a->top_rank = max_top(a);
        }
        a->jump = PSK_JUMP_SEQ;
        top_seq_from_rows(a);
    }
}

void psk_set_view(struct psk_app *a, uint8_t classes, const char *host, uint16_t port, bool pinned)
{
    a->classes = classes ? classes : PPC_CLASSES_ALL;
    snprintf(a->host, sizeof(a->host), "%s", host ? host : "");
    a->port = port;
    a->pinned_only = pinned;
    a->follow = a->sort != 2;
    a->top_rank = 0;
    a->top_seq = 0;
    a->jump = a->follow ? PSK_JUMP_NONE : PSK_JUMP_RANK;
    a->jump_at = 0;
    send_view(a);
    psk_list_paint(a);
}

void psk_set_sort(struct psk_app *a, unsigned sort)
{
    a->sort = (uint8_t)(sort % 3u);
    psk_set_view(a, a->classes, a->host, a->port, a->pinned_only);
}

void psk_set_follow(struct psk_app *a, bool on)
{
    if (a->sort == 2) {
        on = false; /* the length order has no newest end */
    }
    if (on == a->follow) {
        return;
    }
    a->follow = on;
    if (on) {
        a->jump = PSK_JUMP_NONE;
    } else {
        top_seq_from_rows(a);
        a->jump = PSK_JUMP_SEQ;
    }
    a->window_due = true;
    a->window_inflight = false;
    psk_list_paint(a);
    psk_live_paint(a, PSK_CHANGED_STATE);
}

void psk_show_second(struct psk_app *a, int64_t second)
{
    if (second >= 0 && a->sort != 2) {
        a->follow = false;
        a->jump = PSK_JUMP_TIME;
        a->jump_at = (uint64_t)second * 1000000u;
        a->window_due = true;
        a->window_inflight = false;
    }
    psk_select_tab(a, PSK_TAB_PACKETS);
}

void psk_show_class(struct psk_app *a, unsigned cls)
{
    if (cls < PPC_CLASS_COUNT) {
        psk_set_view(a, (uint8_t)(1u << cls), "", 0, false);
    }
    psk_select_tab(a, PSK_TAB_PACKETS);
}

void psk_list_scroll_to(struct psk_app *a, uint32_t rank, bool final)
{
    bool live = a->model->state == PPC_STATE_CAPTURING || a->model->state == PPC_STATE_HOLD;

    if (rank > max_top(a)) {
        rank = max_top(a);
    }
    if (a->follow) {
        a->follow = false;
        psk_live_paint(a, PSK_CHANGED_STATE);
    }
    if (rank != a->top_rank || final) {
        a->top_rank = rank;
        a->top_seq = 0;
        top_seq_from_rows(a);
        a->jump = PSK_JUMP_RANK;
        a->jump_at = rank;
        a->window_due = true;
        psk_list_paint(a);
    }
    if (final && live && ((a->sort == 0 && rank >= max_top(a)) || (a->sort == 1 && rank == 0))) {
        /* Back at the newest end: the list follows again. */
        psk_set_follow(a, true);
    } else if (final) {
        psk_live_paint(a, PSK_CHANGED_STATE);
    }
}

void psk_open_packet(struct psk_app *a, uint32_t seq, int step)
{
    if (!seq || !psk_capture_present(a)) {
        return;
    }
    if (step == 0 && a->follow) {
        /* Opening a packet holds the list where it is, so it is there on
         * the way back. */
        psk_set_follow(a, false);
    }
    if (psk_session_inspect(a->session, a->model, seq, step) != 0) {
        return;
    }
    a->insp_seq = seq;
    a->insp_note[0] = '\0';
    if (step == 0) {
        a->sel_seq = seq;
        psk_list_paint(a);
        psk_select_tab(a, PSK_TAB_INSPECT);
    }
}

void psk_select_field(struct psk_app *a, int index)
{
    a->insp_field = a->insp_field == index ? -1 : index;
    psk_inspect_paint(a);
    psk_live_paint(a, PSK_CHANGED_STATE);
}

void psk_filter_by(struct psk_app *a, const char *token)
{
    uint8_t classes = PPC_CLASSES_ALL;
    char host[PPC_HOST_TEXT] = "";
    uint16_t port = 0;

    if (psk_filter_token(token, &classes, host, sizeof(host), &port) != 0) {
        return;
    }
    /* The list shows what the field stands for, and only that. */
    psk_set_view(a, classes, host, port, false);
    psk_select_tab(a, PSK_TAB_PACKETS);
}

void psk_toggle_pin(struct psk_app *a)
{
    const struct psk_model *m = a->model;

    if (m->have_pkt) {
        psk_session_pin(a->session, m->pkt.head.seq, !(m->pkt.head.flags & PPC_ROWF_PINNED));
    }
}

static void pins_changed(struct psk_app *a)
{
    struct psk_model *m = a->model;
    uint32_t i;

    a->insp_note[0] = '\0';
    for (i = 0; i < m->row_count; i++) {
        if (m->rows[i].seq == m->pin_seq) {
            if (m->pin_on) {
                m->rows[i].flags |= PPC_ROWF_PINNED;
            } else {
                m->rows[i].flags &= (uint8_t)~PPC_ROWF_PINNED;
            }
        }
    }
    if (a->pinned_only) {
        a->window_due = true;
    }
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
        /* A new helper knows no view: tell it the list's. */
        reset_list(a);
        send_view(a);
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
    if ((changed & PSK_CHANGED_ERROR) && !a->starting) {
        if (strcmp(m->err_code, "busy") == 0) {
            /* The helper's queue was full: ask again shortly. */
            a->window_inflight = false;
            a->window_due = true;
            a->retry_ms = psk_now() + 150;
        } else if (strcmp(m->err_code, "gone") == 0 && a->insp_seq) {
            snprintf(a->insp_note, sizeof(a->insp_note), "! THE PACKET HAS LEFT THE BUFFER");
        } else if (strcmp(m->err_code, "pins") == 0) {
            snprintf(a->insp_note, sizeof(a->insp_note), "! EVERY PIN IS TAKEN · UNPIN ONE FIRST");
        }
    }
    if (changed & PSK_CHANGED_ROWS) {
        rows_arrived(a);
    }
    if (changed & PSK_CHANGED_PACKET) {
        a->insp_note[0] = '\0';
        a->insp_seq = m->pkt.head.seq;
        a->sel_seq = m->pkt.head.seq;
    }
    if (changed & PSK_CHANGED_PINS) {
        pins_changed(a);
    }
    if (changed & PSK_CHANGED_EXITED) {
        bool bad = m->exit != PSK_EXIT_NORMAL;

        reset_list(a);

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

/* A display refresh while the app is open: render and flush (B5). */
static void on_refresh(lv_event_t *e)
{
    struct psk_app *a = lv_event_get_user_data(e);

    if (lv_event_get_code(e) == LV_EVENT_REFR_START) {
        a->frame_start_us = now_us();
    } else if (a->frame_start_us) {
        int64_t d = now_us() - a->frame_start_us;

        if (d > a->worst_frame_us) {
            a->worst_frame_us = d;
        }
        a->frame_start_us = 0;
    }
}

static void log_cost(struct psk_app *a)
{
    if (psk_now() < a->next_cost_log_ms) {
        return;
    }
    a->next_cost_log_ms = psk_now() + COST_LOG_MS;
    if (a->worst_poll_us || a->worst_frame_us) {
        LOG_DEBUG("packetshark: worst poll %lld us, worst frame %lld us in the last minute",
                  (long long)a->worst_poll_us, (long long)a->worst_frame_us);
        a->worst_poll_us = 0;
        a->worst_frame_us = 0;
    }
}

static void on_poll(lv_timer_t *t)
{
    struct psk_app *a = lv_timer_get_user_data(t);
    int64_t t0 = now_us();
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
        psk_live_fit(a);
    }
    psk_list_fit(a);
    list_poll(a);
    t0 = now_us() - t0;
    if (t0 > a->worst_poll_us) {
        a->worst_poll_us = t0;
    }
    log_cost(a);
}

/* ---- actions ------------------------------------------------------------------------------ */

void psk_select_tab(struct psk_app *a, enum psk_tab t)
{
    if (t == PSK_TAB_INSPECT && (a->insp_seq == 0 || !psk_capture_present(a))) {
        return; /* no packet has been opened */
    }
    a->cur = t;
    psk_paint_tabs(a);
    if (t == PSK_TAB_PACKETS) {
        a->window_due = true;
        psk_list_fit(a);
    }
    psk_live_paint(a, PSK_CHANGED_STATE);
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
        psk_fsheet_show(a, a->cur == PSK_TAB_LIVE);
        return;
    case PSK_ACT_LIVE:
        psk_set_follow(a, true);
        return;
    case PSK_ACT_FILTER_BY:
        if (a->model->have_pkt && a->insp_field >= 0 && (uint32_t)a->insp_field < a->model->pkt.nfields) {
            psk_filter_by(a, a->model->pkt.field[a->insp_field].filter);
        }
        return;
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
    a->classes = PPC_CLASSES_ALL;
    a->follow = true;
    a->vis = 1;
    a->insp_field = -1;

    a->frame = psk_box(root);
    lv_obj_set_size(a->frame, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(a->frame, LV_FLEX_FLOW_COLUMN);
    psk_live_build(a);
    psk_sheet_build(a);
    psk_fsheet_build(a);
    read_ifaces(a);
    psk_paint_tabs(a);
    psk_live_paint(a, ~0u);
    a->timer = lv_timer_create(on_poll, PSK_POLL_MS, a);
    a->next_cost_log_ms = psk_now() + COST_LOG_MS;
    if (lv_display_get_default()) {
        lv_display_add_event_cb(lv_display_get_default(), on_refresh, LV_EVENT_REFR_START, a);
        lv_display_add_event_cb(lv_display_get_default(), on_refresh, LV_EVENT_REFR_READY, a);
    }
    lv_obj_add_event_cb(a->frame, on_frame_size, LV_EVENT_SIZE_CHANGED, a);
    lv_obj_update_layout(a->frame);
    layout(a);
    psk_live_fit(a);
    return a;
}

static void psk_destroy(void *priv)
{
    struct psk_app *a = priv;

    if (!a) {
        return;
    }
    lv_obj_remove_event_cb_with_user_data(a->frame, on_frame_size, a);
    if (lv_display_get_default()) {
        lv_display_remove_event_cb_with_user_data(lv_display_get_default(), on_refresh, a);
    }
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
    if (psk_fsheet_shown(a)) {
        psk_fsheet_hide(a);
        return 1;
    }
    if (a->cur == PSK_TAB_INSPECT) {
        psk_select_tab(a, PSK_TAB_PACKETS);
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

/* ---- test seams ----------------------------------------------------------------------------- */

lv_obj_t *psk_app_part(void *priv, const char *name)
{
    struct psk_app *a = priv;
    int i;

    if (!a || !name) {
        return NULL;
    }
    if (strcmp(name, "traffic") == 0) {
        return a->traffic;
    }
    if (strcmp(name, "graph") == 0) {
        return a->graph;
    }
    if (strcmp(name, "protocols") == 0) {
        return a->protos;
    }
    if (strcmp(name, "readout") == 0) {
        return a->readout;
    }
    if (strcmp(name, "session") == 0) {
        /* value label -> its cell -> the row -> the panel */
        return lv_obj_get_parent(lv_obj_get_parent(lv_obj_get_parent(a->cell[0])));
    }
    if (strcmp(name, "status") == 0) {
        return lv_obj_get_parent(a->chip);
    }
    if (strncmp(name, "tab", 3) == 0 && (i = name[3] - '0') >= 0 && i < PSK_TAB_COUNT) {
        return a->tab[i];
    }
    if (strncmp(name, "bar", 3) == 0 && (i = name[3] - '0') >= 0 && i < 3) {
        return a->btn[i];
    }
    if (strncmp(name, "proto", 5) == 0 && (i = name[5] - '0') >= 0 && i < PPC_CLASS_COUNT) {
        return a->proto_cell[i];
    }
    /* The figures' labels: SESSION's, LAST SESSION's, each class's rate and share. */
    if (strncmp(name, "cell", 4) == 0 && (i = name[4] - '0') >= 0 && i < 4) {
        return a->cell[i];
    }
    if (strncmp(name, "last", 4) == 0 && (i = name[4] - '0') >= 0 && i < 4) {
        return a->last[i];
    }
    if (strncmp(name, "rate", 4) == 0 && (i = name[4] - '0') >= 0 && i < PPC_CLASS_COUNT) {
        return a->proto_rate[i];
    }
    if (strncmp(name, "share", 5) == 0 && (i = name[5] - '0') >= 0 && i < PPC_CLASS_COUNT) {
        return a->proto_share[i];
    }
    /* M5, M6 */
    if (strcmp(name, "list") == 0) {
        return a->pk_list;
    }
    if (strcmp(name, "rail") == 0) {
        return a->pk_rail;
    }
    if (strcmp(name, "filter") == 0) {
        return a->pk_filter;
    }
    if (strcmp(name, "sort") == 0) {
        return a->pk_sort;
    }
    if (strcmp(name, "follow") == 0) {
        return a->pk_follow;
    }
    if (strcmp(name, "prev") == 0) {
        return a->in_prev;
    }
    if (strcmp(name, "next") == 0) {
        return a->in_next;
    }
    if (strcmp(name, "pin") == 0) {
        return a->in_pin;
    }
    if (strcmp(name, "hex") == 0) {
        return a->in_hex;
    }
    if (strncmp(name, "end", 3) == 0 && (i = name[3] - '0') >= 0 && i < 2) {
        return a->in_end[i];
    }
    if (strncmp(name, "row", 3) == 0 && (i = atoi(name + 3)) >= 0 && i < PSK_LIST_ROWS) {
        return a->lrow[i].row;
    }
    if (strncmp(name, "layer", 5) == 0 && (i = name[5] - '0') >= 0 && i < PSK_LAYERS) {
        return a->layer[i].row;
    }
    if (strncmp(name, "field", 5) == 0 && (i = atoi(name + 5)) >= 0 && i < PSK_FIELDS) {
        return a->field[i].row;
    }
    return NULL;
}

void psk_app_cost(void *priv, int64_t *worst_poll_us, int64_t *worst_frame_us, int reset)
{
    struct psk_app *a = priv;

    if (!a) {
        return;
    }
    if (worst_poll_us) {
        *worst_poll_us = a->worst_poll_us;
    }
    if (worst_frame_us) {
        *worst_frame_us = a->worst_frame_us;
    }
    if (reset) {
        a->worst_poll_us = 0;
        a->worst_frame_us = 0;
    }
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
