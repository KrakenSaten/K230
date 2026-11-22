/*
 * PacketShark: a packet analyzer for Doors (docs/apps/PACKETSHARK.md;
 * ADR-016 and DS §59 accepted). Milestones M3 to M9: Ethernet capture from
 * the launcher - setup, START, live counters, HOLD, STOP, the error state;
 * the traffic graph (M4); the packet list with its filter and sort (M5); the
 * inspector and pins (M6); findings (M7); SAVE and the saved captures (M8);
 * the wide shape and the keys (M9); Wi-Fi in managed mode (M10).
 *
 * The shell is a viewport (DESIGN_DECISIONS D1, accepted): the capture
 * socket, the decoder and every packet are in pos-capture, one helper per
 * START, started here through psk_session and polled from an LVGL timer
 * without ever blocking. The app keeps counters and at most a screen of
 * text. Leaving the app ends the helper, and with it the capture: nothing
 * is written to storage unless SAVE was confirmed on a stopped capture
 * (D10, accepted; ADR-016 rule 9). The screen stays on while the app
 * is in front (shell.c awake_apps, D23), so a capture never runs behind a
 * dark panel.
 *
 * The interfaces come from sysd (system.status.network, docs/api/system.md),
 * asked when the app opens and every PSK_IFACE_POLL_MS while the setup view
 * shows. An interface with a carrier can be chosen: Ethernet with a cable,
 * Wi-Fi while it is associated (M10; managed mode, the frames the driver
 * hands up as Ethernet). The joined network's name comes from netd
 * (wifi.status, docs/api/network.md), asked only while a Wi-Fi interface
 * has a carrier. Nothing here changes an interface.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "psk_app.h"

#include "pocketlog/pocketlog.h"
#include "pos_input.h"
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

bool psk_iface_usable(const struct psk_iface *f)
{
    return f->carrier;
}

static bool capture_name(const char *n)
{
    return strncmp(n, "eth", 3) == 0 || strncmp(n, "en", 2) == 0 || strncmp(n, "usb", 3) == 0 ||
           strncmp(n, "wlan", 4) == 0;
}

/* The network netd's Wi-Fi interface has joined, onto that interface's
 * entry. Display text only (netd sanitises it); nothing when netd does not
 * answer or is not connected. */
static void read_ssid(struct psk_app *a)
{
    char err[96];
    cJSON *st = shell_ipc_call_timeout("netd", "wifi.status", NULL, SHELL_IPC_UI_TIMEOUT_MS, err, sizeof(err));
    const cJSON *ifn = st ? cJSON_GetObjectItemCaseSensitive(st, "interface") : NULL;
    const cJSON *state = st ? cJSON_GetObjectItemCaseSensitive(st, "state") : NULL;
    const cJSON *ssid = st ? cJSON_GetObjectItemCaseSensitive(st, "ssid") : NULL;
    int i;

    if (cJSON_IsString(ifn) && cJSON_IsString(ssid) && ssid->valuestring[0] && cJSON_IsString(state) &&
        (strcmp(state->valuestring, "connected") == 0 || strcmp(state->valuestring, "obtaining_ip") == 0)) {
        for (i = 0; i < a->nifs; i++) {
            if (a->ifs[i].wifi && a->ifs[i].carrier && strcmp(a->ifs[i].name, ifn->valuestring) == 0) {
                snprintf(a->ifs[i].ssid, sizeof(a->ifs[i].ssid), "%s", ssid->valuestring);
            }
        }
    }
    cJSON_Delete(st);
}

static void read_ifaces(struct psk_app *a)
{
    char keep[PPC_IFACE_MAX] = "";
    char err[96];
    cJSON *st;
    const cJSON *net;
    const cJSON *it;
    bool wifi_up = false;
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
        a->ifs[0].ssid[0] = '\0';
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
            wifi_up |= f->wifi && f->carrier;
        }
        cJSON_Delete(st);
        if (wifi_up) {
            read_ssid(a);
        }
    }
    /* The interface last tapped while it is usable (a Wi-Fi that drops and
     * rejoins is chosen again); else keep the choice while it stays usable;
     * otherwise the first usable interface, so "plug in, open, START" is
     * two taps (PRODUCT_SPEC F1). sysd lists names sorted: eth before wlan. */
    a->sel = -1;
    for (i = 0; i < a->nifs; i++) {
        if (psk_iface_usable(&a->ifs[i]) && strcmp(a->ifs[i].name, a->chosen) == 0) {
            a->sel = i;
        }
    }
    for (i = 0; i < a->nifs && a->sel < 0; i++) {
        if (psk_iface_usable(&a->ifs[i]) && strcmp(a->ifs[i].name, keep) == 0) {
            a->sel = i;
        }
    }
    for (i = 0; i < a->nifs && a->sel < 0; i++) {
        if (psk_iface_usable(&a->ifs[i])) {
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
    if (psk_session_view_diag(a->session, a->view_id, a->sort, a->classes, a->host, a->port, a->pinned_only,
                              a->sigmask, a->diagf) != 0) {
        LOG_DEBUG("packetshark: the view could not be sent");
    }
    a->window_due = true;
    a->window_inflight = false;
    a->marks_due = true;
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
    /* The rail's marks: where the view's findings are (M7). Asked for when
     * the view changes and, while packets arrive, once a second; a view
     * that shows only findings needs none. */
    if (!a->sigmask && (a->marks_due || (m->state == PPC_STATE_CAPTURING &&
                                         now - a->marks_sent_ms >= PSK_MARKS_REFRESH_MS))) {
        int32_t h = lv_obj_get_height(a->pk_rail);
        uint32_t buckets = h > 0 ? (uint32_t)h / 12u : 0;

        if (buckets > PPC_MARK_BUCKETS) {
            buckets = PPC_MARK_BUCKETS;
        }
        if (buckets && psk_session_marks(a->session, a->model, buckets) == 0) {
            a->marks_due = false;
            a->marks_sent_ms = now;
        }
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
    psk_set_view_signals(a, classes, host, port, pinned, 0);
}

void psk_set_view_signals(struct psk_app *a, uint8_t classes, const char *host, uint16_t port, bool pinned,
                          unsigned signals)
{
    psk_set_view_diag(a, classes, host, port, pinned, signals, 0);
}

void psk_set_view_diag(struct psk_app *a, uint8_t classes, const char *host, uint16_t port, bool pinned,
                       unsigned signals, unsigned diag)
{
    char keep[PPC_HOST_TEXT];

    /* host may be a->host itself (the sort changes nothing else). */
    snprintf(keep, sizeof(keep), "%s", host ? host : "");
    a->classes = classes ? classes : PPC_CLASSES_ALL;
    snprintf(a->host, sizeof(a->host), "%s", keep);
    a->port = port;
    a->pinned_only = pinned;
    a->sigmask = (uint8_t)(signals & PPC_SIG_FINDINGS_ALL);
    a->diagf = (uint8_t)(diag < PPC_DIAG_COUNT ? diag : 0);
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
    psk_set_view_diag(a, a->classes, a->host, a->port, a->pinned_only, a->sigmask, a->diagf);
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

/* A finding's way to its packets (M7): the list shows them alone. */
void psk_show_signal(struct psk_app *a, unsigned index)
{
    if (index < PPC_SIGNAL_COUNT) {
        psk_set_view_signals(a, PPC_CLASSES_ALL, "", 0, false, 1u << index);
    }
    psk_select_tab(a, PSK_TAB_PACKETS);
}

void psk_diag_go(struct psk_app *a, const struct psk_diag_line *l)
{
    if (!l || l->go == PSK_GO_NONE || !psk_capture_present(a)) {
        return;
    }
    psk_dsheet_hide(a);
    if (l->go == PSK_GO_LIST) {
        psk_set_view_diag(a, PPC_CLASSES_ALL, "", 0, false, l->signals, l->diag);
        psk_select_tab(a, PSK_TAB_PACKETS);
    } else if (l->seq) {
        /* The packet alone, in the inspector: the list shows everything, so
         * the arrows walk its neighbours. */
        psk_set_view_diag(a, PPC_CLASSES_ALL, "", 0, false, 0, 0);
        psk_open_packet(a, l->seq, 0);
    }
}

bool psk_inspect_visible(const struct psk_app *a)
{
    return a->cur == PSK_TAB_INSPECT || (a->wide && a->cur == PSK_TAB_PACKETS);
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
    a->key_nav = false;
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

/* The packet a key moved to stays among the rows shown: when it is not,
 * the list is held with it near the middle. */
static void keep_selected_in_view(struct psk_app *a)
{
    const struct psk_model *m = a->model;
    uint32_t rank = m->pkt.head.rank;
    uint32_t vis = (uint32_t)(a->vis > 0 ? a->vis : 1);

    if (!m->have_pkt || rank >= m->pkt.head.match || a->sort == 2) {
        return; /* not in the view, or an order a rank says little about */
    }
    if (rank >= a->top_rank && rank < a->top_rank + vis && !a->follow) {
        return;
    }
    a->follow = false;
    a->top_rank = rank > vis / 2u ? rank - vis / 2u : 0;
    a->top_seq = 0;
    a->jump = PSK_JUMP_RANK;
    a->jump_at = a->top_rank;
    a->window_due = true;
    a->window_inflight = false;
}

/* ---- the saved captures (M8) --------------------------------------------------------------- */

void psk_saves_refresh(struct psk_app *a)
{
    a->nsaves = psk_saves_list(a->saves, PSK_SAVES_ROWS, &a->saves_count, &a->saves_bytes);
    if (psk_ssheet_shown(a)) {
        psk_ssheet_paint(a);
    }
}

void psk_saved_delete_ask(struct psk_app *a, int index)
{
    if (index < 0 || index >= a->nsaves) {
        return;
    }
    snprintf(a->del_name, sizeof(a->del_name), "%s", a->saves[index].name);
    psk_sheet_show(a, PSK_SHEET_DELETE);
}

/* What the poll brought, beyond painting. */
static void react(struct psk_app *a, unsigned changed)
{
    struct psk_model *m = a->model;

    if (a->pending_start && m->hello && m->state == PPC_STATE_IDLE) {
        const struct psk_iface *f = a->sel >= 0 ? &a->ifs[a->sel] : NULL;

        a->pending_start = false;
        /* What this capture is asked for, kept as it is sent (the setup
         * rows take no tap from START on, psk_option). */
        a->run_full = a->keep == PPC_KEEP_FULL;
        a->run_limit_s = psk_limit_min[a->limit_idx] * 60u;
        if (!f || psk_session_capture(a->session, f->name, a->run_full ? PPC_KEEP_FULL : PPC_KEEP_HEADERS, 0,
                                      a->cfilter, a->run_limit_s) != 0) {
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
        if (a->key_nav) {
            keep_selected_in_view(a);
        }
    }
    if ((changed & PSK_CHANGED_SAVE) && m->save != PSK_SAVE_RUNNING) {
        if (m->save == PSK_SAVE_DONE) {
            LOG_INFO("packetshark: the capture was saved");
        } else {
            LOG_WARN("packetshark: the capture was not saved (%s)", m->save_fail);
        }
        psk_saves_refresh(a);
    }
    if (changed & PSK_CHANGED_PINS) {
        pins_changed(a);
    }
    if ((changed & (PSK_CHANGED_DIAG | PSK_CHANGED_STATE | PSK_CHANGED_EXITED)) && psk_dsheet_shown(a)) {
        psk_dsheet_paint(a);
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
static int psk_back(void *priv);

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

/* ---- what the app says of its capture to the shell's listeners ------------------------------ */

/* The figures of what the screen shows (psk_remote.h says what can be in
 * them): the error panel, a capture, or setup. */
static void remote_figures(const struct psk_app *a, struct psk_remote_figures *f)
{
    enum psk_remote_phase phase =
        a->failed_view ? PSK_REMOTE_ENDED : psk_capture_present(a) ? PSK_REMOTE_CAPTURE : PSK_REMOTE_IDLE;

    psk_remote_take(a->model, phase, a->run_full, a->run_limit_s, f);
}

/* capture.state on a change and capture.stats once a second while a capture
 * runs: at most one event a poll, as psk_remote_due() says (it bounds them).
 * The shell passes an event on only while this app is in front; one it did
 * not pass on is tried again. */
static void announce(struct psk_app *a)
{
    struct psk_remote_figures f;
    int64_t now = psk_now();
    unsigned due;
    cJSON *data;

    if (!pocketos_shell_app_in_front()) {
        /* Locked, or an alarm over the app: nothing would be passed on, so
         * nothing is made. What changed meanwhile is due when it is back. */
        return;
    }
    remote_figures(a, &f);
    due = psk_remote_due(&a->emit, &f, now);
    if (!due) {
        return;
    }
    data = due == PSK_REMOTE_DUE_STATE ? psk_remote_state_json(&f) : psk_remote_json(&f);
    psk_remote_tried(&a->emit, due, &f, now,
                     data && pocketos_shell_app_event(due == PSK_REMOTE_DUE_STATE ? "capture.state" : "capture.stats",
                                                      data));
}

static void on_poll(lv_timer_t *t)
{
    struct psk_app *a = lv_timer_get_user_data(t);
    int64_t t0 = now_us();
    unsigned changed = psk_session_poll(a->session, a->model, psk_now());

    react(a, changed);
    announce(a);
    if (a->back_pending) {
        /* The app's own back slab (wide): one level back, as the Back
         * action; at the top level the shell goes home. */
        a->back_pending = false;
        if (!psk_back(a)) {
            pocketos_shell_go_home();
            return;
        }
    }
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
    if (t == PSK_TAB_INSPECT && a->wide) {
        t = PSK_TAB_PACKETS; /* the inspector is beside the list */
    }
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
    if (a->starting) {
        return; /* START has been tapped: what it asked for is settled */
    }
    if (index >= 0 && index < a->nifs && psk_iface_usable(&a->ifs[index])) {
        a->sel = index;
        snprintf(a->chosen, sizeof(a->chosen), "%s", a->ifs[index].name);
        psk_live_paint(a, PSK_CHANGED_STATE);
    }
}

void psk_option(struct psk_app *a, int which)
{
    if (a->starting) {
        /* START has been tapped. A row changed now would show a choice the
         * capture was not started with: the rows wait until it is capturing
         * (and gone from the screen) or back on setup. */
        return;
    }
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
    psk_saves_refresh(a);
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

bool psk_action_offered(const struct psk_app *a, enum psk_action act)
{
    enum ppc_state s = a->model->state;

    if (!psk_capture_present(a)) {
        return false;
    }
    switch (act) {
    case PSK_ACT_HOLD:
        return s == PPC_STATE_CAPTURING;
    case PSK_ACT_RESUME:
        return s == PPC_STATE_HOLD;
    case PSK_ACT_STOP:
        return s == PPC_STATE_CAPTURING || s == PPC_STATE_HOLD;
    default:
        return false;
    }
}

/* HOLD, RESUME and STOP: the one way they are done, for the capture bar's
 * buttons and for capture.hold and capture.stop alike. The helper is told;
 * the state changes when it answers. 0, or -1 when it did not take the
 * line. */
static int capture_command(struct psk_app *a, enum psk_action act)
{
    int rc;

    switch (act) {
    case PSK_ACT_HOLD:
        rc = psk_session_hold(a->session, true);
        break;
    case PSK_ACT_RESUME:
        rc = psk_session_hold(a->session, false);
        break;
    case PSK_ACT_STOP:
        rc = psk_session_stop(a->session);
        break;
    default:
        return -1;
    }
    psk_live_paint(a, PSK_CHANGED_STATE);
    return rc;
}

void psk_do(struct psk_app *a, enum psk_action act)
{
    switch (act) {
    case PSK_ACT_START:
        start_capture(a);
        break;
    case PSK_ACT_HOLD:
    case PSK_ACT_RESUME:
    case PSK_ACT_STOP:
        capture_command(a, act);
        return;
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
        if (a->model->state == PPC_STATE_STOPPED && a->model->save != PSK_SAVE_RUNNING) {
            psk_sheet_show(a, PSK_SHEET_SAVE);
        }
        return;
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
        if (!a->starting) {
            a->keep = PPC_KEEP_FULL;
            psk_paint_options(a);
        }
        break;
    case PSK_SHEET_NOLIMIT:
        if (!a->starting) {
            a->limit_idx = PSK_LIMITS - 1;
            psk_paint_options(a);
        }
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
    case PSK_SHEET_SAVE:
        /* The helper writes the file; the poll brings its progress. */
        if (psk_session_save(a->session) != 0) {
            LOG_WARN("packetshark: the save could not be asked for");
        }
        psk_live_paint(a, PSK_CHANGED_STATE);
        break;
    case PSK_SHEET_DELETE:
        if (psk_saves_delete(a->del_name) != 0) {
            LOG_WARN("packetshark: a saved capture could not be deleted");
        } else {
            LOG_INFO("packetshark: a saved capture was deleted");
        }
        a->del_name[0] = '\0';
        psk_saves_refresh(a);
        psk_live_paint(a, PSK_CHANGED_STATE);
        break;
    default:
        break;
    }
}

/* ---- layout ------------------------------------------------------------------------------- */

static void layout(struct psk_app *a)
{
    struct pos_insets in;
    const lv_area_t *box;
    int32_t w;
    int32_t h;
    bool wide;

    if (!pocketui_layout_begin(&a->guard, a->frame, &in)) {
        return;
    }
    box = &a->guard.area;
    /* At the display's top edge - landscape, where the shell builds no
     * header above PacketShark (DS §37.2, §59.7) - the app's top row is the
     * screen's top row: it starts at the edge and keeps clear of the rounded
     * corners sideways, as RIFT's strip does (rift_app.c). */
    a->top_inset_l = 0;
    a->top_inset_r = 0;
    if (box->y1 <= 0 && in.top > 0) {
        const struct pos_display_geometry *g = pocketui_display_geometry();
        struct pos_insets bar = pos_display_bar_insets(g, POS_EDGE_TOP);
        int32_t left = bar.left - box->x1 - in.left;
        int32_t right = bar.right - (g->width - 1 - box->x2) - in.right;

        /* What the row still needs past the frame's own side margins. */
        in.top = bar.top;
        a->top_inset_l = left > 0 ? left : 0;
        a->top_inset_r = right > 0 ? right : 0;
    }
    /* Nothing inside the panel's unsafe corners (DS §21.3). */
    lv_obj_set_style_pad_left(a->frame, in.left, 0);
    lv_obj_set_style_pad_right(a->frame, in.right, 0);
    lv_obj_set_style_pad_top(a->frame, in.top, 0);
    lv_obj_set_style_pad_bottom(a->frame, in.bottom, 0);
    /* The shape is chosen from the room, not from the orientation: a
     * landscape body with a keyboard sheet over it may not have it. */
    w = lv_area_get_width(box) - in.left - in.right;
    h = lv_area_get_height(box) - in.top - in.bottom;
    wide = w > h && w >= PSK_WIDE_MIN_W;
    {
        /* Wide: the body's top padding is given up, so the top row starts
         * at the display's edge as the shell's header would (RIFT zeroes
         * its root's padding for the same reason); tall keeps it. The
         * frame's size changes with it and this runs once more. */
        lv_obj_t *root = lv_obj_get_parent(a->frame);
        int32_t want = wide ? 0 : a->root_pad_top;

        if (lv_obj_get_style_pad_top(root, LV_PART_MAIN) != want) {
            lv_obj_set_style_pad_top(root, want, 0);
            a->relayout = true;
        }
    }
    if (wide != a->wide || !a->shaped) {
        a->wide = wide;
        a->shaped = true;
        psk_shape(a);
    } else if (a->wide) {
        lv_obj_set_style_pad_left(a->top, a->top_inset_l, 0);
        lv_obj_set_style_pad_right(a->top, a->top_inset_r, 0);
    }
}

static void on_frame_size(lv_event_t *e)
{
    struct psk_app *a = lv_event_get_user_data(e);

    a->relayout = true;
}

/* ---- keys (M9) ----------------------------------------------------------------------------- *
 *
 * A keyboard does what the screen's own controls do, and no more: no key
 * starts a capture, confirms a sheet or deletes anything (ADR-016 rule 1:
 * an explicit START on the app's own screen).
 *
 *   Esc            one level back, as the Back action
 *   1 2 3, Tab     LIVE, PACKETS, INSPECT; Tab goes to the next
 *   Space          HOLD or RESUME
 *   S              STOP
 *   F              the filter sheet
 *   Up, Down       PACKETS: the previous or next packet of the list;
 *                  INSPECT: the previous or next field
 *   Left, Right    INSPECT (and the wide shape): the previous or next packet
 *   Home, End      PACKETS: the oldest end; the newest (FOLLOW)
 *   Enter          PACKETS: open the selected packet in INSPECT;
 *                  INSPECT: FILTER BY the selected field
 *   P              PIN or unpin the inspected packet
 */

/* The packet before or after the inspected one in the list's order; with
 * none inspected yet, the first row shown. The tab does not change. */
static void key_packet(struct psk_app *a, int step)
{
    uint32_t seq = a->insp_seq;
    int i;

    if (!psk_capture_present(a)) {
        return;
    }
    if (!seq || !a->model->have_pkt) {
        step = 0;
        for (i = 0; i < a->vis && i < PSK_LIST_ROWS && !seq; i++) {
            seq = a->lrow[i].seq;
        }
    }
    if (!seq) {
        return;
    }
    if (a->follow) {
        psk_set_follow(a, false);
    }
    if (psk_session_inspect(a->session, a->model, seq, step) == 0) {
        a->insp_seq = seq;
        a->insp_note[0] = '\0';
        a->key_nav = true;
        if (step == 0) {
            a->sel_seq = seq;
            psk_list_paint(a);
        }
        psk_paint_tabs(a);
    }
}

static void key_field(struct psk_app *a, int step)
{
    const struct psk_model *m = a->model;
    int n = m->have_pkt ? (int)m->pkt.nfields : 0;
    int want = a->insp_field;
    int tries;

    /* Only fields of an open layer are on screen. */
    for (tries = 0; tries < n; tries++) {
        want = want < 0 ? (step > 0 ? 0 : n - 1) : want + step;
        if (want < 0 || want >= n) {
            return;
        }
        if (a->layer_open[m->pkt.field[want].layer]) {
            a->insp_field = -1;
            psk_select_field(a, want);
            if (a->field[want].row) {
                lv_obj_scroll_to_view_recursive(a->field[want].row, LV_ANIM_OFF);
            }
            return;
        }
    }
}

bool psk_key(struct psk_app *a, uint32_t key)
{
    const struct psk_model *m = a->model;
    bool capture = psk_capture_present(a);
    bool running = capture && (m->state == PPC_STATE_CAPTURING || m->state == PPC_STATE_HOLD);
    bool sheet = a->sheet_kind != PSK_SHEET_NONE || psk_fsheet_shown(a) || psk_ssheet_shown(a) || psk_dsheet_shown(a);

    if (key == LV_KEY_ESC) {
        a->back_pending = true; /* handled by the poll, outside this event */
        return true;
    }
    if (sheet) {
        return false; /* a sheet is answered on the screen */
    }
    if (key >= 'A' && key <= 'Z') {
        key += 'a' - 'A';
    }
    switch (key) {
    case '1':
        psk_select_tab(a, PSK_TAB_LIVE);
        return true;
    case '2':
        psk_select_tab(a, PSK_TAB_PACKETS);
        return true;
    case '3':
        psk_select_tab(a, PSK_TAB_INSPECT);
        return true;
    case LV_KEY_NEXT:
        psk_select_tab(a, a->cur == PSK_TAB_LIVE ? PSK_TAB_PACKETS
                          : a->cur == PSK_TAB_PACKETS && !a->wide && a->insp_seq ? PSK_TAB_INSPECT : PSK_TAB_LIVE);
        return true;
    case ' ':
        if (running) {
            psk_do(a, m->state == PPC_STATE_HOLD ? PSK_ACT_RESUME : PSK_ACT_HOLD);
        }
        return running;
    case 's':
        if (running) {
            psk_do(a, PSK_ACT_STOP);
        }
        return running;
    case 'f':
        if (capture) {
            psk_do(a, PSK_ACT_FILTER);
        }
        return capture;
    case 'p':
        if (capture && psk_inspect_visible(a)) {
            psk_toggle_pin(a);
            return true;
        }
        return false;
    case LV_KEY_UP:
    case LV_KEY_DOWN:
        if (a->cur == PSK_TAB_INSPECT) {
            key_field(a, key == LV_KEY_DOWN ? 1 : -1);
            return true;
        }
        if (a->cur == PSK_TAB_PACKETS) {
            key_packet(a, key == LV_KEY_DOWN ? 1 : -1);
            return true;
        }
        return false;
    case LV_KEY_LEFT:
    case LV_KEY_RIGHT:
        if (psk_inspect_visible(a)) {
            key_packet(a, key == LV_KEY_RIGHT ? 1 : -1);
            return true;
        }
        return false;
    case LV_KEY_HOME:
        if (capture && a->cur == PSK_TAB_PACKETS) {
            psk_list_scroll_to(a, 0, true);
            return true;
        }
        return false;
    case LV_KEY_END:
        if (capture && a->cur == PSK_TAB_PACKETS) {
            if (running && a->sort != 2) {
                psk_set_follow(a, true);
            } else {
                psk_list_scroll_to(a, UINT32_MAX, true);
            }
            return true;
        }
        return false;
    case LV_KEY_ENTER:
        if (a->cur == PSK_TAB_INSPECT || (a->wide && a->cur == PSK_TAB_PACKETS && a->insp_field >= 0)) {
            psk_do(a, PSK_ACT_FILTER_BY);
            return true;
        }
        if (a->cur == PSK_TAB_PACKETS && a->insp_seq) {
            psk_select_tab(a, PSK_TAB_INSPECT);
            return true;
        }
        return false;
    default:
        return false;
    }
}

static void on_key(lv_event_t *e)
{
    psk_key(lv_event_get_user_data(e), lv_event_get_key(e));
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
    psk_remote_emit_init(&a->emit);
    a->cfilter = PPC_CFILTER_ALL;
    a->simulated = psk_session_default_source() != NULL;
    a->cur = PSK_TAB_LIVE;
    a->classes = PPC_CLASSES_ALL;
    a->follow = true;
    a->vis = 1;
    a->insp_field = -1;
    a->row_h = PSK_ROW_H;
    a->marks_due = true;

    a->root_pad_top = lv_obj_get_style_pad_top(root, LV_PART_MAIN);
    a->frame = psk_box(root);
    lv_obj_set_size(a->frame, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(a->frame, LV_FLEX_FLOW_COLUMN);
    psk_live_build(a);
    psk_sheet_build(a);
    psk_fsheet_build(a);
    psk_ssheet_build(a);
    psk_dsheet_build(a);
    psk_saves_refresh(a);
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
    /* Keys (M9): the frame is the app's one focusable object. */
    lv_obj_add_event_cb(a->frame, on_key, LV_EVENT_KEY, a);
    pos_input_add_obj(a->frame);
    pos_input_focus(a->frame);
    lv_obj_update_layout(a->frame);
    layout(a);
    lv_obj_update_layout(a->frame);
    if (a->relayout) {
        /* The wide shape moved the frame to the top edge: its insets again. */
        a->relayout = false;
        layout(a);
        lv_obj_update_layout(a->frame);
    }
    psk_live_paint(a, ~0u);
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
    lv_obj_remove_event_cb_with_user_data(a->frame, on_key, a);
    if (lv_display_get_default()) {
        lv_display_remove_event_cb_with_user_data(lv_display_get_default(), on_refresh, a);
    }
    if (a->timer) {
        lv_timer_delete(a->timer);
    }
    if (psk_capture_present(a)) {
        /* A capture goes with the app: say so, once, with no figure, past
         * the emitter's pacing (it is the last word). Only a capture that
         * is there now: not setup, not one whose helper already ended. If
         * nobody can be told (the unit is locked) it is not kept. */
        struct psk_remote_figures f;
        cJSON *data;

        remote_figures(a, &f);
        psk_remote_left(&f);
        data = psk_remote_state_json(&f);
        if (data) {
            pocketos_shell_app_event("capture.state", data);
        }
    }
    /* Leaving the app ends the capture and discards its packets (ADR-016
     * rule 5): at most PSK_DESTROY_GRACE_MS + PSK_KILL_REAP_MS. A save in
     * hand is cut short; the helper removes its part file. */
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
    if (psk_ssheet_shown(a)) {
        psk_ssheet_hide(a);
        return 1;
    }
    if (psk_dsheet_shown(a)) {
        psk_dsheet_hide(a);
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
    if (psk_model_has_packets(a->model) && !a->discarding && !a->failed_view) {
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
    /* M7 to M9 */
    if (strcmp(name, "signals") == 0) {
        return a->signals;
    }
    if (strncmp(name, "sig", 3) == 0 && (i = name[3] - '0') >= 0 && i < PPC_SIGNAL_COUNT) {
        return a->sig_cell[i];
    }
    if (strcmp(name, "saves") == 0) {
        return a->saves_row;
    }
    if (strncmp(name, "save", 4) == 0 && (i = name[4] - '0') >= 0 && i < PSK_SAVES_ROWS) {
        return a->ss_row[i];
    }
    if (strcmp(name, "top") == 0) {
        return a->top;
    }
    if (strcmp(name, "back") == 0) {
        return a->back;
    }
    if (strcmp(name, "inspect") == 0) {
        return a->page[PSK_TAB_INSPECT];
    }
    if (strncmp(name, "finding", 7) == 0 && (i = name[7] - '0') >= 0 && i < PSK_LAYERS) {
        return a->layer[i].finding;
    }
    /* M11.1 */
    if (strncmp(name, "diag", 4) == 0 && (i = name[4] - '0') >= 0 && i < PSK_DIAG_KINDS) {
        return a->diag_row[i];
    }
    if (strcmp(name, "dsheet") == 0) {
        return a->dsheet;
    }
    if (strncmp(name, "dline", 5) == 0 && (i = name[5] - '0') >= 0 && i < PSK_DIAG_LINES) {
        return a->ds_row[i];
    }
    if (strncmp(name, "detail", 6) == 0 && (i = name[6] - '0') >= 0 && i < PSK_LAYERS) {
        return a->layer[i].detail;
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

/* ---- the methods served on the shell's socket (app.h ipc_*, docs/api/shell.md) -------------- */

/* The shell calls these only while this app is in front (open, not locked, no
 * alarm). Not in front it answers capture.status with {"available": false}
 * itself and refuses every other capture.* name.
 *
 *   capture.status  reads
 *   capture.hold    HOLD and RESUME, as the capture bar's buttons
 *   capture.stop    STOP, as the capture bar's button
 *
 * Nothing here starts a capture, discards or saves one, or opens the app (no
 * ipc_opens): those stay with a finger on this screen (ADR-016 rule 1). */
static const char *const psk_ipc_status[] = { "capture.status", NULL };

/* capture.status: the capture's state and aggregate figures, from the model
 * this app already holds. It reads; it sends the helper nothing, starts
 * nothing, changes nothing on the screen. */
static int psk_ipc_read(const struct psk_app *a, cJSON **result, const char **message)
{
    struct psk_remote_figures f;

    if (!a || !a->model) {
        /* The app could not be made: it is not there to be read. */
        *result = cJSON_CreateObject();
        if (!*result || !cJSON_AddBoolToObject(*result, "available", false)) {
            cJSON_Delete(*result);
            *result = NULL;
            *message = "out of memory";
            return POCKETIPC_ERR_BACKEND;
        }
        return 0;
    }
    remote_figures(a, &f);
    *result = psk_remote_json(&f);
    if (!*result) {
        *message = "out of memory";
        return POCKETIPC_ERR_BACKEND;
    }
    return 0;
}

/* A sheet is up over the capture bar: a confirmation, the filter, the saved
 * captures or a diagnosis. */
static bool sheet_up(const struct psk_app *a)
{
    return a->sheet_kind != PSK_SHEET_NONE || psk_fsheet_shown(a) || psk_ssheet_shown(a) || psk_dsheet_shown(a);
}

/* capture.hold {on} and capture.stop: what the button of the same name does,
 * through the same function, and only when the capture bar offers it. The
 * answer is the state as it is now and whether the helper was told; the
 * change itself arrives as it does on the screen, when the helper answers
 * (capture.state). Asking for the state a capture is already in is no
 * fault and does nothing. */
static int psk_ipc_command(struct psk_app *a, const char *method, const cJSON *params, cJSON **result,
                           const char **message)
{
    struct psk_remote_figures f;
    enum psk_action act;
    bool told;
    cJSON *o;

    if (!a || !a->model) {
        *message = "PacketShark is not ready";
        return POCKETIPC_ERR_POLICY;
    }
    if (strcmp(method, "capture.stop") == 0) {
        act = PSK_ACT_STOP;
    } else {
        const cJSON *on = cJSON_IsObject(params) ? cJSON_GetObjectItemCaseSensitive(params, "on") : NULL;

        if (!cJSON_IsBool(on)) {
            *message = "on must be true or false";
            return POCKETIPC_ERR_INVALID_PARAMS;
        }
        act = cJSON_IsTrue(on) ? PSK_ACT_HOLD : PSK_ACT_RESUME;
    }
    if (!psk_action_offered(a, PSK_ACT_STOP)) {
        /* Setup, starting, stopped, saving, a helper that ended, a discard
         * on its way: there is no running capture to hold or stop. */
        *message = "nothing is capturing";
        return POCKETIPC_ERR_POLICY;
    }
    if (sheet_up(a)) {
        /* A sheet covers the capture bar: no finger and no key reaches
         * HOLD or STOP now (psk_key), so no request does either. */
        *message = "a sheet is open on the unit";
        return POCKETIPC_ERR_POLICY;
    }
    remote_figures(a, &f);
    told = psk_action_offered(a, act);
    o = cJSON_CreateObject();
    if (!o || !cJSON_AddStringToObject(o, "state", psk_remote_state_name(f.state)) ||
        !cJSON_AddBoolToObject(o, "changing", told)) {
        cJSON_Delete(o);
        *message = "out of memory";
        return POCKETIPC_ERR_BACKEND;
    }
    if (told && capture_command(a, act) != 0) {
        cJSON_Delete(o);
        *message = "the capture helper did not take it";
        return POCKETIPC_ERR_BACKEND;
    }
    *result = o;
    return 0;
}

static int psk_ipc_request(void *priv, const char *method, const cJSON *params, cJSON **result,
                           const char **message)
{
    if (strcmp(method, "capture.status") == 0) {
        return psk_ipc_read(priv, result, message);
    }
    if (strcmp(method, "capture.hold") == 0 || strcmp(method, "capture.stop") == 0) {
        return psk_ipc_command(priv, method, params, result, message);
    }
    *message = "unknown method";
    return POCKETIPC_ERR_UNKNOWN_METHOD;
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
    /* Landscape draws its own top row, with a back slab of its own, the
     * state and the capture's buttons (DS §59.7, mockup 08; M9). */
    .header = POCKETOS_HEADER_NONE_LANDSCAPE,
    .ipc_prefix = "capture.",
    .ipc_request = psk_ipc_request,
    .ipc_status = psk_ipc_status,
};
