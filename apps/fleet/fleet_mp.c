/*
 * PocketFleet multiplayer in the app: the link, the session, the timer that
 * drives them, and the steering of the screens by the match's phase
 * (docs/apps/FLEET_MULTIPLAYER.md). See fleet_app.h.
 *
 * What opening the app does here: pick a link and read the saved match file.
 * Nothing more - no connection to the mesh service and no packet - until the
 * player opens Multiplayer or presses Resume (fleet_session.h).
 *
 * The link is meshcored's (fleet_link_mesh.c) unless POCKETFLEET_MP_FAKE asks
 * for the virtual opponent (fleet_link_loop.h), a development aid in the
 * spirit of POCKETFLEET_SCREEN: with it set, one simulator shell plays a whole
 * match against the real protocol and nothing reaches any radio.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "fleet_app.h"

#include "app.h"
#include "engine/fleet_ai.h"
#include "engine/fleet_store.h"
#include "link/fleet_link_loop.h"
#include "link/fleet_link_mesh.h"
#include "link/fleet_session.h"
#include "pocketlog/pocketlog.h"
#include "ui/fleet_view_mp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <time.h>

/* Often enough that an answer is on the screen as it arrives; the session
 * does nothing on a tick with nothing due. */
#define MP_POLL_MS 100

int64_t fleet_app_now(struct fleet_app *app)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000 + (app ? app->mp_clock_offset : 0);
}

static int mp_save(void *user, const uint8_t *blob, size_t n)
{
    (void)user;
    if (fleet_store_match_save(blob, n) != 0) {
        LOG_WARN("fleet: cannot write %s, this match lasts for the session only",
                 fleet_store_match_path());
        return -1;
    }
    return 0;
}

static int mp_entropy(void *user, void *buf, size_t n)
{
    uint8_t *p = buf;
    size_t got = 0;

    (void)user;
    while (got < n) {
        ssize_t r = getrandom(p + got, n - got, 0);

        if (r <= 0) {
            return -1;
        }
        got += (size_t)r;
    }
    return 0;
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
    return fleet_view_mp_status(app->mp->ready ? &app->mp->m : NULL, link,
                                fleet_app_peer(app, peer, sizeof(peer)), buf, n);
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

void fleet_mp_create(struct fleet_app *app)
{
    const char *fake = getenv("POCKETFLEET_MP_FAKE");
    uint8_t blob[FLEET_STORE_MATCH_MAX];
    int n;

    if (fake && *fake) {
        struct fleet_link_loop_cfg cfg;

        fleet_link_loop_defaults(&cfg);
        fleet_link_loop_parse(fake, &cfg);
        app->link = fleet_link_loop_open(&cfg);
        LOG_INFO("fleet: multiplayer against the virtual opponent (%s)", fake);
    } else {
        app->link = fleet_link_mesh_open(NULL);
    }
    app->mp = calloc(1, sizeof(*app->mp));
    if (!app->link || !app->mp) {
        free(app->mp);
        app->mp = NULL;
        return;
    }
    fleet_session_init(app->mp, app->link, mp_save, mp_entropy, app);
    n = fleet_store_match_load(blob, sizeof(blob));
    if (n > 0) {
        fleet_session_set_saved(app->mp, blob, (size_t)n, 0);
        if (summarise_saved(app, blob, (size_t)n) != 0) {
            /* Left in place, as save.v1 is: it is the only evidence. */
            LOG_WARN("fleet: %s cannot be read; no match to resume", fleet_store_match_path());
        }
    } else if (n < 0) {
        fleet_session_set_saved(app->mp, NULL, 0, 1);
        LOG_WARN("fleet: %s cannot be read; no match to resume", fleet_store_match_path());
    }
    app->mp_timer = lv_timer_create(on_mp_tick, MP_POLL_MS, app);
}

void fleet_mp_destroy(struct fleet_app *app)
{
    if (app->mp_timer) {
        lv_timer_delete(app->mp_timer);
        app->mp_timer = NULL;
    }
    if (app->link) {
        app->link->ops->close(app->link->ctx);
        app->link = NULL;
    }
    free(app->mp);
    app->mp = NULL;
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

    if (strcmp(want, "lobby") != 0 && strncmp(want, "mp_", 3) != 0) {
        return 0;
    }
    if (!app->mp) {
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
