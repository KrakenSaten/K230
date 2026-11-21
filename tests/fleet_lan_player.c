/*
 * One PocketFleet player on the local network without a screen, for the
 * end-to-end test (tests/fleet_lan_e2e_test.sh): the real session, the real
 * LAN link and the real match state machine, talking to a real peerd over
 * its socket, with PocketFleet's AI where the player's finger would be. The
 * sibling of tests/fleet_mp_player.c, which does the same over the mesh.
 *
 *   fleet_lan_player --role host|guest --save FILE [--peer NAME | --address A]
 *                    [--seed N] [--timeout S] [--crash-at-ply N]
 *                    [--forfeit-at-ply N] [--chat]
 *
 * peerd is the one whose socket is in $POCKETOS_RUNTIME_DIR.
 *
 * The host invites the player peerd lists as --peer, or - for a network that
 * carries no announcements - asks the device at --address who it is and
 * invites whoever answers. The guest accepts the first invitation. Each
 * deploys a random fleet and fires the AI's shot on its turn. The match is
 * saved to --save exactly as the app saves it - before any packet it produced
 * may leave - before, as the app does, though to a file of the test's own -
 * and a player started on a save resumes that match, which is how the test
 * restarts one mid-match.
 *
 * It is seen on the network as the app is (apps/fleet/fleet_mp.c,
 * follow_visibility): while it is "in the lobby" - no match begun - and
 * while its own invitation or acceptance waits for an answer; from Deploy on
 * it is hidden, and the match is played with the kept opponent alone. A
 * player resuming a save is never visible at all, which is stricter than
 * the app (whose lobby is on screen for a moment before RESUME).
 *
 * --crash-at-ply N leaves with _exit(3) once N plies are resolved: no close,
 * no unsubscribe, as a crash would.
 *
 * --forfeit-at-ply N forfeits once N plies are resolved, as FORFEIT in the
 * lobby does: the match ends with an END the opponent has to acknowledge.
 *
 * --chat talks through the battle, a line every 2 s whenever one is not
 * already waiting, "<role> line <n>, Fleet over the network".
 *
 * Output, one line each: "READY <key>", "LISTED <n> after_ms=<t>" (with
 * --peer: when the first player was listed; with --address: how many were
 * after seven seconds of listening, before anything is asked), "ASKED
 * <address>", "ANSWERED", "INVITED <name>",
 * "ACCEPTED", "DEPLOYED", "HIDDEN" (once it stops asking to be seen),
 * "RESUMED ...", "PLY <n>", and at the end "DONE role=.. outcome=.. ...".
 * DONE comes only once no END is owed any more, and says whether a forfeit's
 * END was answered (end_acked=1) or given up. After DONE it keeps answering
 * until SIGTERM, so a peer still waiting for its last answer gets it. Exit 0
 * when the match finished, 2 on a timeout, 4 when stopped before the end.
 *
 * Copyright (c) 2026 Doors authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "fleet_link_peer.h"
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
 * this one, never half of each. The file is the test's own: Fleet's store,
 * and which file a transport's save goes to, are not what this exercises
 * (tests/fleet_save_test, tests/fleet_app_test). */
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

/* From a generator seeded with --seed and the process id: this is a test,
 * and the commitment salt's secrecy is not what it is testing. */
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
    fprintf(stderr, "usage: fleet_lan_player --role host|guest --save FILE "
                    "[--peer NAME | --address A] [--seed N] [--timeout S] "
                    "[--crash-at-ply N] [--forfeit-at-ply N] [--chat]\n");
    exit(1);
}

/* The opponent's lines still held: each theirs, whole, once, and in the
 * order said. Returns 1 when all are. */
static int chat_lines_ok(const struct fleet_chat *c, const char *peer_role, int *in_order)
{
    char want[16];
    unsigned last = 0;
    int ok = 1;
    int i;
    int j;

    snprintf(want, sizeof(want), "%s line ", peer_role);
    *in_order = 1;
    for (i = 0; i < c->count; i++) {
        const struct fleet_chat_line *l = &c->line[i];
        unsigned n;

        if (l->mine) {
            continue;
        }
        if (strncmp(l->text, want, strlen(want)) != 0 ||
            sscanf(l->text + strlen(want), "%u", &n) != 1 ||
            !strstr(l->text, ", Fleet over the network")) {
            ok = 0;
            continue;
        }
        for (j = i + 1; j < c->count; j++) {
            if (!c->line[j].mine && strcmp(c->line[j].text, l->text) == 0) {
                ok = 0;
            }
        }
        if (n <= last) {
            *in_order = 0;
        }
        last = n;
    }
    return ok;
}

int main(int argc, char **argv)
{
    const char *role = NULL;
    const char *peer = NULL;
    const char *address = NULL;
    uint32_t seed = 1;
    int timeout_s = 600;
    int crash_at = 0;
    int forfeit_at = 0;
    int forfeited = 0;
    int end_owed = 0;
    int end_acked = 0;
    int talk = 0;
    unsigned said = 0;
    int64_t next_say = 0;
    struct player pl;
    struct fleet_session s;
    struct fleet_link *link;
    uint8_t blob[FLEET_MATCH_SAVE_SIZE];
    int have_blob = 0;
    int resumed = 0;
    int announced = 0;
    int listed_said = 0;
    int answered_said = 0;
    int hidden_said = 0;
    int last_ply = -1;
    int64_t start;
    int64_t next_invite = 0;
    int64_t next_ask = 0;
    int i;

    memset(&pl, 0, sizeof(pl));
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--role") && i + 1 < argc) {
            role = argv[++i];
        } else if (!strcmp(argv[i], "--peer") && i + 1 < argc) {
            peer = argv[++i];
        } else if (!strcmp(argv[i], "--address") && i + 1 < argc) {
            address = argv[++i];
        } else if (!strcmp(argv[i], "--save") && i + 1 < argc) {
            pl.save_path = argv[++i];
        } else if (!strcmp(argv[i], "--seed") && i + 1 < argc) {
            seed = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (!strcmp(argv[i], "--timeout") && i + 1 < argc) {
            timeout_s = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--crash-at-ply") && i + 1 < argc) {
            crash_at = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--forfeit-at-ply") && i + 1 < argc) {
            forfeit_at = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--chat")) {
            talk = 1;
        } else {
            usage();
        }
    }
    if (!role || !pl.save_path || (strcmp(role, "host") && strcmp(role, "guest")) ||
        (!strcmp(role, "host") && !peer && !address)) {
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
    link = fleet_link_peer_open(NULL);
    if (!link) {
        return 1;
    }
    /* As the app: nothing is asked for on the first connection that the
     * player did not ask for. A player on a save goes straight to its match. */
    fleet_link_peer_set_visible(link, !have_blob);
    fleet_session_init(&s, link, save, entropy, &pl);
    fleet_session_set_saved(&s, have_blob ? blob : NULL, have_blob ? sizeof(blob) : 0, 0);
    start = mono_ms();
    fleet_session_engage(&s, start);

    for (;;) {
        int64_t now = mono_ms();
        struct fleet_match *m = &s.m;
        unsigned rx_before;
        int row;
        int col;

        if (stopping) {
            /* Told to go before the match was over: as the app closes. */
            link->ops->close(link->ctx);
            return 4;
        }
        if (now - start > (int64_t)timeout_s * 1000) {
            struct fleet_link_peer_info info;

            memset(&info, 0, sizeof(info));
            (void)fleet_link_peer_info(link, &info);
            printf("TIMEOUT phase=%s plies=%d link=%d trouble=%d kept=%d\n",
                   s.ready ? fleet_match_phase_name((enum fleet_mp_phase)m->phase) : "none",
                   s.ready ? m->resolved : 0, (int)link->ops->state(link->ctx), (int)info.trouble,
                   info.kept);
            link->ops->close(link->ctx);
            return 2;
        }
        if (s.ready) {
            /* The app's rule (fleet_mp.c, follow_visibility), with "the lobby
             * is on screen" standing for "no match has begun". */
            int in_lobby = !have_blob && m->phase <= FLEET_MP_ACCEPTING;

            fleet_link_peer_set_visible(link, in_lobby);
            if (!in_lobby && !hidden_said) {
                hidden_said = 1;
                printf("HIDDEN\n");
            }
        }
        rx_before = s.ready ? m->stats.rx : 0;
        fleet_session_poll(&s, now);
        if (s.ready && end_owed && !m->end_unacked) {
            /* The END is no longer owed: answered, if a packet of the match
             * came in on the poll that settled it; given up after its three
             * tries otherwise. */
            end_owed = 0;
            end_acked = m->stats.rx > rx_before;
        }
        if (s.ready && m->end_unacked) {
            end_owed = 1;
        }
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
                uint8_t asked_key[FLEET_KEY_BYTES];
                int have_asked = 0;
                int n = link->ops->peers(link->ctx, peers, FLEET_LINK_PEERS);
                int k;

                if (address) {
                    enum fleet_link_peer_probe_state ps = fleet_link_peer_probe_state(link);

                    if (!listed_said && now - start >= 7000) {
                        /* Seven seconds of listening first - three of
                         * peerd's announcements and three of the link's
                         * looks at its list, and the six seconds the lobby
                         * itself waits - so "nobody was listed" means it. */
                        listed_said = 1;
                        printf("LISTED %d after_ms=%lld\n", n, (long long)(now - start));
                    }
                    if (ps == FLEET_PEER_PROBE_ANSWERED &&
                        fleet_link_peer_probe_key(link, asked_key) == 0) {
                        have_asked = 1;
                        if (!answered_said) {
                            answered_said = 1;
                            printf("ANSWERED\n");
                        }
                    } else if (listed_said && ps != FLEET_PEER_PROBE_PENDING && now >= next_ask) {
                        next_ask = now + 4000;
                        if (fleet_link_peer_probe(link, address) == 0) {
                            printf("ASKED %s\n", address);
                        }
                    }
                } else if (!listed_said && n > 0) {
                    listed_said = 1;
                    printf("LISTED %d after_ms=%lld\n", n, (long long)(now - start));
                }
                for (k = 0; k < n; k++) {
                    if (address ? have_asked && !memcmp(peers[k].key, asked_key, FLEET_KEY_BYTES)
                                : !strcmp(peers[k].name, peer)) {
                        fleet_session_invite(&s, peers[k].key, peers[k].name, now);
                        printf("INVITED %s\n", peers[k].name);
                        break;
                    }
                }
                next_invite = now + 300;
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
            if (forfeit_at && !forfeited && m->resolved >= forfeit_at) {
                forfeited = 1;
                if (fleet_session_forfeit(&s, now) == 0) {
                    end_owed = m->end_unacked != 0;
                    printf("FORFEITED at ply %d\n", m->resolved);
                }
                break;
            }
            if (talk && now >= next_say && fleet_chat_pending(&m->chat) == 0) {
                char text[64];

                snprintf(text, sizeof(text), "%s line %u, Fleet over the network", role, ++said);
                fleet_session_chat_send(&s, text, now);
                next_say = now + 2000;
            }
            if (fleet_match_my_turn(m) && choose(m, seed, &row, &col) == 0) {
                fleet_session_fire(&s, row, col, now);
            }
            break;
        case FLEET_MP_DONE: {
            int in_order = 1;
            int lines_ok = chat_lines_ok(&m->chat, strcmp(role, "host") ? "host" : "guest",
                                         &in_order);

            if (m->end_unacked) {
                break;      /* over for us; the END is still owed its answer */
            }
            printf("DONE role=%s outcome=%s end=%d by_me=%d end_acked=%d verify=%d plies=%d "
                   "digest=%08x sent=%u rx=%u profile=%s gov_waits=%u resyncs=%u dup_replies=%u violations=%u saves=%u "
                   "chat_said=%u chat_tx=%u chat_shown=%u chat_dup=%u chat_failed=%u chat_ok=%d "
                   "chat_order=%d\n",
                   role, outcome_name(m->outcome), m->end_reason, m->end_by_me, end_acked,
                   m->verify, m->resolved,
                   fleet_match_digest(m, m->resolved), m->stats.tx, m->stats.rx,
                   m->profile ? m->profile->name : "?", m->stats.gov_waits, m->stats.resyncs,
                   m->stats.dup_replies, m->stats.violations, pl.saves, said, m->stats.chat_tx,
                   m->stats.chat_rx, m->stats.chat_dup, m->stats.chat_failed, lines_ok, in_order);
            /* Stay, so a peer still waiting for our last answer gets it,
             * until the test stops us (or the timeout does). */
            while (!stopping && mono_ms() - start <= (int64_t)timeout_s * 1000) {
                fleet_session_poll(&s, mono_ms());
                usleep(10000);
            }
            link->ops->close(link->ctx);
            return 0;
        }
        default:
            break;
        }
        usleep(10000);
    }
}
