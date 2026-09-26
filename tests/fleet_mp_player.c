/*
 * One PocketFleet multiplayer player without a screen, for the end-to-end
 * test (tests/fleet_mp_e2e_test.sh): the real session, the real mesh link and
 * the real match state machine, talking to a real meshcored over its socket,
 * with PocketFleet's AI where the player's finger would be.
 *
 *   fleet_mp_player --service NAME --role host|guest --peer NAME --save FILE
 *                   [--seed N] [--timeout S] [--fast] [--crash-at-ply N]
 *
 * The host invites the node the mesh knows as --peer; the guest accepts the
 * first invitation. Each deploys a random fleet and fires the AI's shot on its
 * turn. The match is saved to --save exactly as the app saves it - before any
 * packet it produced may leave - and a player started on a save resumes that
 * match, which is how the test restarts one mid-match.
 *
 * --fast refills the airtime governor's token bucket every turn, so a match
 * runs in under a minute instead of about ten; the hourly budget and every
 * other rule still hold. The real-pace run leaves it off.
 *
 * --crash-at-ply N leaves with _exit(3) once N plies are resolved: no close,
 * no unsubscribe, as a crash would.
 *
 * Output, one line each: "READY <key>", "PLY <n>", and at the end
 * "DONE role=.. outcome=.. end=.. verify=.. plies=.. digest=.. ...". After DONE
 * it keeps answering until SIGTERM, so a peer still waiting for its last
 * answer gets it. Exit 0 when the match finished, 2 on a timeout.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "fleet_link_mesh.h"
#include "fleet_session.h"

#include "fleet_ai.h"
#include "fleet_rng.h"
#include "fleet_rules.h"

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

struct player {
    const char *save_path;
    struct fleet_rng rng;
    unsigned saves;
};

static volatile sig_atomic_t stopping;

static void on_term(int sig)
{
    (void)sig;
    stopping = 1;
}

static int64_t mono_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Written whole and renamed into place, so a crash leaves the last save or
 * this one, never half of each. */
static int save(void *user, const uint8_t *blob, size_t n)
{
    struct player *p = user;
    char tmp[512];
    int fd;
    ssize_t w;

    snprintf(tmp, sizeof(tmp), "%s.tmp", p->save_path);
    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        return -1;
    }
    w = write(fd, blob, n);
    if (w != (ssize_t)n || fsync(fd) != 0) {
        close(fd);
        return -1;
    }
    close(fd);
    if (rename(tmp, p->save_path) != 0) {
        return -1;
    }
    p->saves++;
    return 0;
}

/* Seeded, so a run can be repeated; this is a test, and the commitment salt's
 * secrecy is not what it is testing. */
static int entropy(void *user, void *buf, size_t n)
{
    struct player *p = user;
    uint8_t *b = buf;
    size_t i;

    for (i = 0; i < n; i++) {
        b[i] = (uint8_t)fleet_rng_next(&p->rng);
    }
    return 0;
}

static int choose(const struct fleet_match *m, uint32_t seed, int *row, int *col)
{
    struct fleet_ai ai;
    struct fleet_rng rng;
    int k;

    fleet_ai_init(&ai, FLEET_OFFICER);
    for (k = 1; k <= m->resolved; k++) {
        if (fleet_match_shooter(k) == m->role) {
            uint8_t res = m->log_res[k];

            fleet_ai_observe(&ai, m->log_cell[k] / FLEET_GRID, m->log_cell[k] % FLEET_GRID,
                             (enum fleet_shot_result)fleet_res_outcome(res), fleet_res_ship(res));
        }
    }
    fleet_rng_seed(&rng, seed * 2654435761u + m->resolved * 40503u + 1);
    return fleet_ai_next_shot(&ai, &rng, row, col);
}

static const char *outcome_name(int o)
{
    switch (o) {
    case FLEET_OUTCOME_WIN: return "win";
    case FLEET_OUTCOME_LOSS: return "loss";
    case FLEET_OUTCOME_VOID: return "void";
    default: return "none";
    }
}

static void usage(void)
{
    fprintf(stderr, "usage: fleet_mp_player --service NAME --role host|guest --peer NAME "
                    "--save FILE [--seed N] [--timeout S] [--fast] [--crash-at-ply N]\n");
    exit(1);
}

int main(int argc, char **argv)
{
    const char *service = NULL;
    const char *role = NULL;
    const char *peer = NULL;
    uint32_t seed = 1;
    int timeout_s = 900;
    int fast = 0;
    int crash_at = 0;
    struct player pl;
    struct fleet_session s;
    struct fleet_link *link;
    uint8_t blob[FLEET_MATCH_SAVE_SIZE];
    int have_blob = 0;
    int resumed = 0;
    int announced = 0;
    int last_ply = -1;
    int64_t start;
    int64_t next_invite = 0;
    int i;

    memset(&pl, 0, sizeof(pl));
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--service") && i + 1 < argc) {
            service = argv[++i];
        } else if (!strcmp(argv[i], "--role") && i + 1 < argc) {
            role = argv[++i];
        } else if (!strcmp(argv[i], "--peer") && i + 1 < argc) {
            peer = argv[++i];
        } else if (!strcmp(argv[i], "--save") && i + 1 < argc) {
            pl.save_path = argv[++i];
        } else if (!strcmp(argv[i], "--seed") && i + 1 < argc) {
            seed = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (!strcmp(argv[i], "--timeout") && i + 1 < argc) {
            timeout_s = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--crash-at-ply") && i + 1 < argc) {
            crash_at = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--fast")) {
            fast = 1;
        } else {
            usage();
        }
    }
    if (!service || !role || !pl.save_path || (strcmp(role, "host") && strcmp(role, "guest")) ||
        (!strcmp(role, "host") && !peer)) {
        usage();
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGTERM, on_term);
    signal(SIGPIPE, SIG_IGN);
    fleet_rng_seed(&pl.rng, seed * 7919u + (unsigned)getpid());

    {
        FILE *f = fopen(pl.save_path, "rb");

        if (f) {
            have_blob = fread(blob, 1, sizeof(blob), f) == sizeof(blob);
            fclose(f);
        }
    }
    link = fleet_link_mesh_open(service);
    if (!link) {
        return 1;
    }
    fleet_session_init(&s, link, save, entropy, &pl);
    fleet_session_set_saved(&s, have_blob ? blob : NULL, have_blob ? sizeof(blob) : 0, 0);
    start = mono_ms();
    fleet_session_engage(&s, start);

    for (;;) {
        int64_t now = mono_ms();
        struct fleet_match *m = &s.m;
        int row;
        int col;

        if (now - start > (int64_t)timeout_s * 1000) {
            printf("TIMEOUT phase=%s plies=%d link=%s\n",
                   s.ready ? fleet_match_phase_name((enum fleet_mp_phase)m->phase) : "none",
                   s.ready ? m->resolved : 0,
                   fleet_link_state_text(link->ops->state(link->ctx)));
            link->ops->close(link->ctx);
            return 2;
        }
        if (fast && s.ready) {
            m->tokens = FLEET_GOV_BURST;
        }
        fleet_session_poll(&s, now);
        if (!s.ready || link->ops->state(link->ctx) != FLEET_LINK_UP) {
            usleep(10000);
            continue;
        }
        if (!announced) {
            char hex[FLEET_KEY_BYTES * 2 + 1];
            int k;

            for (k = 0; k < FLEET_KEY_BYTES; k++) {
                snprintf(hex + 2 * k, 3, "%02x", m->self_key[k]);
            }
            printf("READY %s\n", hex);
            announced = 1;
        }
        if (have_blob && !resumed) {
            /* The app's Resume button. */
            resumed = 1;
            if (fleet_match_active(m)) {
                printf("RESUMED phase=%s plies=%d\n",
                       fleet_match_phase_name((enum fleet_mp_phase)m->phase), m->resolved);
                fleet_session_resume(&s, now);
            }
        }
        if (m->resolved != last_ply) {
            last_ply = m->resolved;
            printf("PLY %d\n", last_ply);
            if (crash_at && last_ply >= crash_at && m->phase == FLEET_MP_BATTLE) {
                _exit(3);
            }
        }
        switch (m->phase) {
        case FLEET_MP_IDLE:
            if (!strcmp(role, "host") && now >= next_invite) {
                struct fleet_link_peer peers[FLEET_LINK_PEERS];
                int n = link->ops->peers(link->ctx, peers, FLEET_LINK_PEERS);
                int k;

                for (k = 0; k < n; k++) {
                    if (!strcmp(peers[k].name, peer)) {
                        fleet_session_invite(&s, peers[k].key, peers[k].name, now);
                        printf("INVITED %s\n", peer);
                        break;
                    }
                }
                next_invite = now + 3000;
            }
            break;
        case FLEET_MP_INVITED:
            if (!strcmp(role, "guest")) {
                fleet_session_accept(&s, now);
                printf("ACCEPTED\n");
            }
            break;
        case FLEET_MP_DEPLOY:
            if (!m->committed) {
                struct fleet_board b;
                struct fleet_rng r;

                fleet_board_clear(&b);
                fleet_rng_seed(&r, seed ^ m->sid);
                fleet_board_autoplace(&b, &r);
                fleet_session_deploy(&s, &b, now);
                printf("DEPLOYED\n");
            }
            break;
        case FLEET_MP_BATTLE:
            if (fleet_match_my_turn(m) && choose(m, seed, &row, &col) == 0) {
                fleet_session_fire(&s, row, col, now);
            }
            break;
        case FLEET_MP_DONE:
            printf("DONE role=%s outcome=%s end=%d verify=%d plies=%d digest=%08x sent=%u rx=%u "
                   "tx_airtime_ms=%u gov_waits=%u resyncs=%u dup_replies=%u violations=%u saves=%u\n",
                   role, outcome_name(m->outcome), m->end_reason, m->verify, m->resolved,
                   fleet_match_digest(m, m->resolved), m->stats.tx, m->stats.rx,
                   m->stats.tx_airtime_ms, m->stats.gov_waits, m->stats.resyncs,
                   m->stats.dup_replies, m->stats.violations, pl.saves);
            /* Stay, so a peer still waiting for our last answer gets it,
             * until the test stops us (or the timeout does). */
            while (!stopping && mono_ms() - start <= (int64_t)timeout_s * 1000) {
                fleet_session_poll(&s, mono_ms());
                usleep(10000);
            }
            link->ops->close(link->ctx);
            return 0;
        default:
            break;
        }
        usleep(10000);
    }
}
