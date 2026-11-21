/*
 * PocketFleet multiplayer in the app: the link, the session, the timer that
 * drives them, and the steering of the screens by the match's phase
 * (docs/apps/FLEET_MULTIPLAYER.md). See fleet_app.h.
 *
 * What opening the app does here: pick a link and read the saved match files.
 * Nothing more - no connection to any service and no packet - until the
 * player opens Multiplayer or presses Resume (fleet_session.h).
 *
 * There are two transports and one session (docs/decisions/ADR-018): LoRa,
 * through meshcored (fleet_link_mesh.c), and the local network, through
 * peerd (fleet_link_peer.c). Each has its own save, and a save is only ever
 * handed to the session of its own transport: the file decides, before a
 * byte of it is decoded. The session is on LoRa unless the local network's
 * save holds the match in hand, and moves only when the player picks the
 * other one with no match in hand on either. The local network is offered
 * only on a device where peerd's socket exists (or its save holds a match):
 * elsewhere Fleet is exactly what it was.
 *
 * POCKETFLEET_MP_FAKE asks for the virtual opponent instead
 * (fleet_link_loop.h), a development aid in the spirit of POCKETFLEET_SCREEN:
 * with it set, one simulator shell plays a whole match against the real
 * protocol and nothing reaches any radio or network. "lan" in it offers the
 * local network too, played by the same virtual opponent on the LAN profile.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "fleet_app.h"

#include "app.h"
#include "engine/fleet_ai.h"
#include "engine/fleet_store.h"
#include "link/fleet_link_loop.h"
#include "link/fleet_link_mesh.h"
#include "link/fleet_link_peer.h"
#include "link/fleet_session.h"
#include "pocketlog/pocketlog.h"
#include "ui/fleet_view_mp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <time.h>
#include <unistd.h>

/* Often enough that an answer is on the screen as it arrives; the session
 * does nothing on a tick with nothing due. */
#define MP_POLL_MS 100

int64_t fleet_app_now(struct fleet_app *app)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000 + (app ? app->mp_clock_offset : 0);
}

/* The one place a transport becomes a save file. */
static enum fleet_store_transport store_of(enum fleet_mp_transport t)
{
    return t == FLEET_MP_LAN ? FLEET_STORE_LAN : FLEET_STORE_MESH;
}

static int mp_save(void *user, const uint8_t *blob, size_t n)
{
    struct fleet_app *app = user;
    enum fleet_store_transport store = store_of((enum fleet_mp_transport)app->mp_transport);

    if (fleet_store_match_save_for(store, blob, n) != 0) {
        LOG_WARN("fleet: cannot write %s, this match lasts for the session only",
                 fleet_store_match_path_for(store));
        return -1;
    }
    return 0;
}

int fleet_app_entropy(void *buf, size_t n)
{
    uint8_t *p = buf;
    size_t got = 0;

    while (got < n) {
        ssize_t r = getrandom(p + got, n - got, 0);

        if (r <= 0) {
            return -1;
        }
        got += (size_t)r;
    }
    return 0;
}

static int mp_entropy(void *user, void *buf, size_t n)
{
    (void)user;
    return fleet_app_entropy(buf, n);
}

const char *fleet_app_peer(struct fleet_app *app, char *buf, size_t n)
{
    if (!app || !app->mp || !app->mp->ready) {
        snprintf(buf, n, "%s", app && app->mp_saved_peer[0] ? app->mp_saved_peer : "");
        return buf;
    }
    return fleet_session_peer_name(app->mp, buf, n);
}

struct fleet_board *fleet_app_deploy_board(struct fleet_app *app)
{
    return app->mode == FLEET_MODE_MULTI ? &app->mp_fleet : &app->game.board[FLEET_SIDE_PLAYER];
}

int fleet_app_mp_status(struct fleet_app *app, char *buf, size_t n)
{
    enum fleet_link_state link = FLEET_LINK_CONNECTING;
    char peer[40];

    if (!app || !app->mp) {
        return -1;
    }
    if (app->link && app->link->ops->state) {
        link = app->link->ops->state(app->link->ctx);
    }
    return fleet_view_mp_status_on(app->mp->ready ? &app->mp->m : NULL, link,
                                   fleet_app_peer(app, peer, sizeof(peer)),
                                   app->mp_transport == FLEET_MP_LAN, buf, n);
}

/* The header says where the match stands. Read afresh every time from the
 * match and the link, so it cannot drift from them. */
static void refresh_status(struct fleet_app *app)
{
    char status[64];

    if (app->mode == FLEET_MODE_MULTI && app->current != FLEET_SCREEN_COMMAND &&
        app->mp && app->mp->engaged && fleet_app_mp_status(app, status, sizeof(status)) == 0) {
        pocketos_shell_set_status_hint(status);
    }
}

static void refresh_current(struct fleet_app *app)
{
    switch (app->current) {
    case FLEET_SCREEN_COMMAND:
        fleet_screen_command_refresh(app);
        return;
    case FLEET_SCREEN_DEPLOY:
        fleet_screen_deploy_refresh(app);
        break;
    case FLEET_SCREEN_BATTLE:
        fleet_screen_battle_refresh(app);
        break;
    case FLEET_SCREEN_RESULT:
        fleet_screen_result_refresh(app);
        break;
    case FLEET_SCREEN_LOBBY:
        fleet_screen_lobby_refresh(app);
        break;
    case FLEET_SCREEN_CHAT:
        fleet_screen_chat_refresh(app);
        break;
    default:
        break;
    }
    refresh_status(app);
}

/* Put the player on the screen the match's phase belongs to, when the phase
 * moves on by itself: the invitation accepted, the match over. A phase the
 * player moved by pressing something has already been shown. */
static void steer(struct fleet_app *app)
{
    const struct fleet_match *m = &app->mp->m;
    uint8_t was = app->mp_phase;

    app->mp_phase = m->phase;
    if (app->mode != FLEET_MODE_MULTI || was == m->phase) {
        return;
    }
    if (m->phase == FLEET_MP_DEPLOY && was < FLEET_MP_DEPLOY) {
        fleet_board_clear(&app->mp_fleet);
        fleet_screen_deploy_enter(app);
        fleet_app_show(app, FLEET_SCREEN_DEPLOY);
    } else if (m->phase == FLEET_MP_BATTLE && app->current == FLEET_SCREEN_DEPLOY) {
        fleet_screen_battle_enter(app);
        fleet_app_show(app, FLEET_SCREEN_BATTLE);
    } else if ((m->phase == FLEET_MP_REVEAL || m->phase == FLEET_MP_DONE) &&
               (app->current == FLEET_SCREEN_BATTLE || app->current == FLEET_SCREEN_DEPLOY)) {
        fleet_app_show(app, FLEET_SCREEN_RESULT);
    } else if (m->phase <= FLEET_MP_ACCEPTING && app->current != FLEET_SCREEN_COMMAND &&
               app->current != FLEET_SCREEN_LOBBY && was >= FLEET_MP_DEPLOY) {
        fleet_app_show(app, FLEET_SCREEN_LOBBY);
    }
}

void fleet_app_mp_changed(struct fleet_app *app)
{
    unsigned rev;

    if (!app || !app->mp) {
        return;
    }
    rev = fleet_session_revision(app->mp);
    if (app->mp->ready && app->mp->identity_changed) {
        app->mp_stale[app->mp_transport] = 1;
    }
    if (app->mp->ready && app->mp_revision == 0) {
        /* The match was only just read, when our key became known: nothing
         * moved by itself, so the player stays where they chose to be - in
         * the lobby with the match in hand - rather than being steered from
         * "no match yet" to the match's phase. */
        app->mp_phase = app->mp->m.phase;
    } else if (app->mp->ready) {
        steer(app);
    }
    app->mp_revision = rev;
    refresh_current(app);
}

static void on_mp_tick(lv_timer_t *t)
{
    struct fleet_app *app = lv_timer_get_user_data(t);

    if (!app->mp) {
        return;
    }
    fleet_session_poll(app->mp, fleet_app_now(app));
    if (fleet_session_revision(app->mp) != app->mp_revision) {
        fleet_app_mp_changed(app);
    } else {
        /* The link's own state and the peers it hears change without the
         * match changing; a second's staleness is fine for those. */
        static unsigned ticks;

        if (++ticks % 10 == 0) {
            if (app->current == FLEET_SCREEN_LOBBY || app->current == FLEET_SCREEN_BATTLE ||
                app->current == FLEET_SCREEN_CHAT) {
                refresh_current(app);
            } else {
                refresh_status(app);
            }
        }
    }
    if (app->current == FLEET_SCREEN_CHAT) {
        fleet_screen_chat_tick(app);
    } else if (app->current == FLEET_SCREEN_LOBBY) {
        fleet_screen_lobby_tick(app);
    }
}

/* What a saved match is, without the node key that decoding it properly
 * needs - which is not asked for until the player engages. The blob names
 * its own key, so it can be read against that for the Command screen. */
static int summarise_saved(struct fleet_app *app, const uint8_t *blob, size_t n)
{
    struct fleet_match *probe;
    int ok;

    app->mp_saved = 0;
    if (n != FLEET_MATCH_SAVE_SIZE) {
        return -1;
    }
    probe = malloc(sizeof(*probe));
    if (!probe) {
        return 0;
    }
    fleet_match_init(probe, blob + 6, 1);
    ok = fleet_match_save_decode(probe, blob, n) == 0;
    if (ok && probe->phase >= FLEET_MP_DEPLOY && probe->phase <= FLEET_MP_REVEAL) {
        app->mp_saved = 1;
        snprintf(app->mp_saved_peer, sizeof(app->mp_saved_peer), "%s",
                 probe->peer_name[0] ? probe->peer_name : "your opponent");
    }
    free(probe);
    return ok ? 0 : -1;
}

/* Whether this device can play over the local network at all. */
static int lan_available(void)
{
    const char *fake = getenv("POCKETFLEET_MP_FAKE");

    if (fake && *fake) {
        struct fleet_link_loop_cfg cfg;

        fleet_link_loop_defaults(&cfg);
        fleet_link_loop_parse(fake, &cfg);
        return cfg.lan != 0;
    }
    return fleet_link_peer_present(NULL);
}

static struct fleet_link *open_link(enum fleet_mp_transport t)
{
    const char *fake = getenv("POCKETFLEET_MP_FAKE");

    if (fake && *fake) {
        struct fleet_link_loop_cfg cfg;

        fleet_link_loop_defaults(&cfg);
        fleet_link_loop_parse(fake, &cfg);
        /* The profile follows the transport, never the spec: a match is only
         * ever saved and read by its own transport (ADR-018 point 3). */
        cfg.lan = t == FLEET_MP_LAN;
        LOG_INFO("fleet: multiplayer against the virtual opponent (%s)", fake);
        return fleet_link_loop_open(&cfg);
    }
    return t == FLEET_MP_LAN ? fleet_link_peer_open(NULL) : fleet_link_mesh_open(NULL);
}

/* The session, on one transport: its link, and that transport's save. Opens
 * no connection and sends nothing. */
static int open_session(struct fleet_app *app, enum fleet_mp_transport t)
{
    enum fleet_store_transport store = store_of(t);
    uint8_t blob[FLEET_STORE_MATCH_MAX];
    int n;

    app->mp_transport = (uint8_t)t;
    app->mp_saved = 0;
    app->mp_saved_peer[0] = '\0';
    app->mp_revision = 0;
    app->mp_phase = FLEET_MP_IDLE;
    app->mp_forfeit_armed = 0;
    app->link = open_link(t);
    app->mp = calloc(1, sizeof(*app->mp));
    if (!app->link || !app->mp) {
        if (app->link) {
            app->link->ops->close(app->link->ctx);
            app->link = NULL;
        }
        free(app->mp);
        app->mp = NULL;
        return -1;
    }
    fleet_session_init(app->mp, app->link, mp_save, mp_entropy, app);
    n = fleet_store_match_load_for(store, blob, sizeof(blob));
    if (n > 0) {
        fleet_session_set_saved(app->mp, blob, (size_t)n, 0);
        if (summarise_saved(app, blob, (size_t)n) != 0) {
            /* Left in place, as save.v1 is: it is the only evidence. */
            LOG_WARN("fleet: %s cannot be read; no match to resume",
                     fleet_store_match_path_for(store));
        }
    } else if (n < 0) {
        fleet_session_set_saved(app->mp, NULL, 0, 1);
        LOG_WARN("fleet: %s cannot be read; no match to resume",
                 fleet_store_match_path_for(store));
    }
    return 0;
}

static void close_session(struct fleet_app *app)
{
    if (app->link) {
        app->link->ops->close(app->link->ctx);
        app->link = NULL;
    }
    free(app->mp);
    app->mp = NULL;
}

/* Whether a transport's save holds a match between Deploy and Reveal. The
 * blob is read under its own identity and handed to nobody. */
static int save_in_hand(enum fleet_mp_transport t)
{
    uint8_t blob[FLEET_STORE_MATCH_MAX];
    int n = fleet_store_match_load_for(store_of(t), blob, sizeof(blob));

    return n > 0 && fleet_match_save_in_hand(blob, (size_t)n, NULL, 0);
}

/* One match per device (ADR-008 decision 4, ADR-018 point 4): the session is
 * told whether the other transport's save holds one, and then begins none. */
static void read_other(struct fleet_app *app)
{
    enum fleet_mp_transport other =
        app->mp_transport == FLEET_MP_LAN ? FLEET_MP_MESH : FLEET_MP_LAN;

    app->mp_other_in_hand = (uint8_t)(save_in_hand(other) && !app->mp_stale[other]);
    if (app->mp) {
        fleet_session_set_other_in_hand(app->mp, app->mp_other_in_hand);
    }
}

void fleet_mp_create(struct fleet_app *app)
{
    enum fleet_mp_transport t = FLEET_MP_MESH;
    int lan_hand = save_in_hand(FLEET_MP_LAN);

    /* The transport is the one whose save holds the match in hand. Both
     * holding one cannot arise through Fleet; if the files say so anyway,
     * LoRa's is offered first and the other waits its turn. */
    if (lan_hand && !save_in_hand(FLEET_MP_MESH)) {
        t = FLEET_MP_LAN;
    }
    app->mp_lan_offered = (uint8_t)(lan_hand || lan_available());
    if (open_session(app, t) != 0) {
        return;
    }
    read_other(app);
    app->mp_timer = lv_timer_create(on_mp_tick, MP_POLL_MS, app);
}

void fleet_mp_destroy(struct fleet_app *app)
{
    if (app->mp_timer) {
        lv_timer_delete(app->mp_timer);
        app->mp_timer = NULL;
    }
    close_session(app);
}

int fleet_app_mp_waiting(const struct fleet_app *app)
{
    /* Where there is only LoRa this is mp_saved, as it always was. */
    return app && app->mp_saved && !(app->mp_lan_offered && app->mp_stale[app->mp_transport]);
}

int fleet_app_mp_choice(const struct fleet_app *app)
{
    const struct fleet_session *s = app ? app->mp : NULL;

    if (!s || !app->mp_lan_offered || fleet_app_mp_waiting(app) || app->mp_other_in_hand) {
        return 0;
    }
    return !s->ready || s->m.phase == FLEET_MP_IDLE || s->m.phase == FLEET_MP_DONE;
}

void fleet_app_multiplayer_on(struct fleet_app *app, enum fleet_mp_transport transport)
{
    if (!app || !app->mp) {
        return;
    }
    if (transport != (enum fleet_mp_transport)app->mp_transport && fleet_app_mp_choice(app)) {
        enum fleet_mp_transport was = (enum fleet_mp_transport)app->mp_transport;

        /* The session leaves the transport it was on: its link is closed
         * (and on the network that ends this device's visibility at once),
         * and nothing of it is carried over but what its own save holds. */
        close_session(app);
        if (open_session(app, transport) != 0 && open_session(app, was) != 0) {
            fleet_app_show(app, FLEET_SCREEN_COMMAND);
            return;
        }
        read_other(app);
        fleet_screen_lobby_enter(app);
    }
    fleet_app_multiplayer(app);
}

void fleet_app_multiplayer(struct fleet_app *app)
{
    if (!app || !app->mp) {
        return;
    }
    app->mode = FLEET_MODE_MULTI;
    fleet_session_engage(app->mp, fleet_app_now(app));
    app->mp_phase = app->mp->ready ? app->mp->m.phase : FLEET_MP_IDLE;
    fleet_app_show(app, FLEET_SCREEN_LOBBY);
}

void fleet_app_mp_resume(struct fleet_app *app)
{
    const struct fleet_match *m;

    if (!app || !app->mp) {
        return;
    }
    app->mode = FLEET_MODE_MULTI;
    fleet_session_engage(app->mp, fleet_app_now(app));
    if (!app->mp->ready) {
        fleet_app_show(app, FLEET_SCREEN_LOBBY);
        return;
    }
    m = &app->mp->m;
    fleet_session_resume(app->mp, fleet_app_now(app));
    app->mp_saved = 0;
    app->mp_phase = m->phase;
    switch (m->phase) {
    case FLEET_MP_DEPLOY:
        if (!m->committed) {
            fleet_board_clear(&app->mp_fleet);
            fleet_screen_deploy_enter(app);
            fleet_app_show(app, FLEET_SCREEN_DEPLOY);
            break;
        }
        /* fall through */
    case FLEET_MP_COMMITTED:
    case FLEET_MP_BATTLE:
        fleet_screen_battle_enter(app);
        fleet_app_show(app, FLEET_SCREEN_BATTLE);
        break;
    case FLEET_MP_REVEAL:
    case FLEET_MP_DONE:
        fleet_app_show(app, FLEET_SCREEN_RESULT);
        break;
    default:
        fleet_app_show(app, FLEET_SCREEN_LOBBY);
        break;
    }
}

void fleet_app_chat(struct fleet_app *app)
{
    if (!app || app->mode != FLEET_MODE_MULTI || !app->mp || !app->mp->ready) {
        return;
    }
    fleet_screen_chat_enter(app);
    fleet_app_show(app, FLEET_SCREEN_CHAT);
}

void fleet_app_chat_back(struct fleet_app *app)
{
    const struct fleet_match *m;

    if (!app || !app->mp || !app->mp->ready) {
        if (app) {
            fleet_app_show(app, FLEET_SCREEN_LOBBY);
        }
        return;
    }
    m = &app->mp->m;
    app->mp_phase = m->phase;
    switch (m->phase) {
    case FLEET_MP_DEPLOY:
        if (!m->committed) {
            fleet_app_show(app, FLEET_SCREEN_DEPLOY);
            break;
        }
        /* fall through */
    case FLEET_MP_COMMITTED:
    case FLEET_MP_BATTLE:
        fleet_screen_battle_resume(app);
        fleet_app_show(app, FLEET_SCREEN_BATTLE);
        break;
    case FLEET_MP_REVEAL:
    case FLEET_MP_DONE:
        fleet_app_show(app, FLEET_SCREEN_RESULT);
        break;
    default:
        fleet_app_show(app, FLEET_SCREEN_LOBBY);
        break;
    }
}

/* ---- development aid ------------------------------------------------------ */

static void skip(struct fleet_app *app, int64_t ms)
{
    int64_t end = app->mp_clock_offset + ms;

    while (app->mp_clock_offset < end) {
        app->mp_clock_offset += 100;
        fleet_session_poll(app->mp, fleet_app_now(app));
    }
    fleet_app_mp_changed(app);
}

/* The real link, given real time: whatever answers as peerd is another
 * process. For a screenshot only - the shell takes it after its first tick,
 * and the lobby should by then be showing what peerd said. */
static void settle(struct fleet_app *app, int ms)
{
    int t;

    for (t = 0; t < ms; t += 10) {
        usleep(10000);
        fleet_session_poll(app->mp, fleet_app_now(app));
        if (t % 100 == 0) {
            fleet_screen_lobby_refresh(app);
        }
    }
    fleet_app_mp_changed(app);
}

/* Our side's shot, chosen as the AI would from our own answers. */
static int debug_fire(struct fleet_app *app)
{
    const struct fleet_match *m = &app->mp->m;
    struct fleet_ai ai;
    struct fleet_rng rng;
    int row;
    int col;
    int k;

    fleet_ai_init(&ai, FLEET_COMMANDER);
    for (k = 1; k <= m->resolved; k++) {
        if (fleet_match_shooter(k) == m->role) {
            fleet_ai_observe(&ai, m->log_cell[k] / FLEET_GRID, m->log_cell[k] % FLEET_GRID,
                             (enum fleet_shot_result)fleet_res_outcome(m->log_res[k]),
                             fleet_res_ship(m->log_res[k]));
        }
    }
    fleet_rng_seed(&rng, 31u + m->resolved);
    if (fleet_ai_next_shot(&ai, &rng, &row, &col) != 0) {
        return -1;
    }
    return fleet_session_fire(app->mp, row, col, fleet_app_now(app));
}

int fleet_mp_debug(struct fleet_app *app, const char *want)
{
    struct fleet_link_peer peer;
    struct fleet_rng rng;
    int plies = 0;
    int i;

    if (strcmp(want, "lobby") != 0 && strncmp(want, "mp_", 3) != 0 &&
        strncmp(want, "lan_", 4) != 0) {
        return 0;
    }
    if (!app->mp) {
        return 1;
    }
    if (strncmp(want, "lan_", 4) == 0) {
        /* The local network's lobby, against whatever answers as peerd:
         * as it opens, after an address has been asked, and after the six
         * seconds of silence. */
        fleet_app_multiplayer_on(app, FLEET_MP_LAN);
        if (getenv("POCKETFLEET_MP_FAKE") || app->mp_transport != FLEET_MP_LAN) {
            return 1;
        }
        settle(app, 600);
        if (strcmp(want, "lan_ask") == 0 && getenv("POCKETFLEET_LAN_ASK")) {
            fleet_screen_lobby_ask(app, getenv("POCKETFLEET_LAN_ASK"));
            settle(app, 900);
        } else if (strcmp(want, "lan_silent") == 0) {
            app->mp_clock_offset += 7000;
            settle(app, 100);
        }
        return 1;
    }
    if (!getenv("POCKETFLEET_MP_FAKE")) {
        /* The real link: the lobby says what the mesh service is doing. */
        if (strcmp(want, "lobby") == 0) {
            fleet_app_multiplayer(app);
        }
        return 1;
    }
    fleet_app_multiplayer(app);
    skip(app, 500);
    if (strcmp(want, "lobby") == 0) {
        return 1;
    }
    if (strcmp(want, "mp_invited") == 0) {
        for (i = 0; i < 600 && app->mp->m.phase != FLEET_MP_INVITED; i++) {
            skip(app, 100);
        }
        return 1;
    }
    if (app->link->ops->peers(app->link->ctx, &peer, 1) != 1) {
        return 1;
    }
    fleet_session_invite(app->mp, peer.key, peer.name, fleet_app_now(app));
    for (i = 0; i < 600 && app->mp->m.phase != FLEET_MP_DEPLOY; i++) {
        skip(app, 100);
    }
    if (strcmp(want, "mp_deploy") == 0) {
        fleet_rng_seed(&rng, 4242);
        fleet_board_autoplace(&app->mp_fleet, &rng);
        fleet_board_unplace(&app->mp_fleet, FLEET_SHIP_DESTROYER);
        fleet_screen_deploy_refresh(app);
        return 1;
    }
    fleet_rng_seed(&rng, 4242);
    fleet_board_autoplace(&app->mp_fleet, &rng);
    fleet_session_deploy(app->mp, &app->mp_fleet, fleet_app_now(app));
    fleet_screen_battle_enter(app);
    fleet_app_show(app, FLEET_SCREEN_BATTLE);
    if (strcmp(want, "mp_result") == 0) {
        plies = 400;
    } else {
        plies = 18;
    }
    for (i = 0; i < 20000 && app->mp->m.resolved < plies &&
                app->mp->m.phase != FLEET_MP_DONE; i++) {
        if (fleet_match_my_turn(&app->mp->m)) {
            debug_fire(app);
        }
        skip(app, 100);
    }
    for (i = 0; i < 600 && strcmp(want, "mp_result") == 0 && app->mp->m.phase != FLEET_MP_DONE; i++) {
        skip(app, 100);
    }
    for (i = 0; i < 600 && !fleet_match_my_turn(&app->mp->m) &&
                app->mp->m.phase == FLEET_MP_BATTLE; i++) {
        skip(app, 100);
    }
    if (strcmp(want, "mp_waiting") == 0 || strcmp(want, "mp_lost") == 0) {
        if (strcmp(want, "mp_lost") == 0) {
            fleet_link_loop_set_cut(app->link, 1);
        }
        debug_fire(app);
        skip(app, 200);
        if (strcmp(want, "mp_lost") == 0) {
            skip(app, 10 * 60000);
        }
        fleet_app_mp_changed(app);
        return 1;
    }
    if (strcmp(want, "mp_battle") == 0) {
        fleet_screen_battle_aim(app, 4, 6);
    }
    if (strcmp(want, "mp_chat") == 0) {
        /* A few lines each way, each given the time the governor wants. */
        fleet_session_chat_send(app->mp, "Good luck, captain.", fleet_app_now(app));
        skip(app, 20000);
        fleet_link_loop_say(app->link, "You too. Fire away!");
        skip(app, 20000);
        fleet_session_chat_send(app->mp, "Sk\xc3\xa5l! F\xc3\xb8rste treff var mitt.",
                                fleet_app_now(app));
        skip(app, 20000);
        fleet_link_loop_say(app->link, "Not for long.");
        skip(app, 20000);
        fleet_app_chat(app);
    }
    fleet_app_mp_changed(app);
    return 1;
}
