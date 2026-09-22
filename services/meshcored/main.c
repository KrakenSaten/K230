/*
 * meshcored: the MeshCore protocol service. See docs/services/MESHCORED.md
 * and docs/api/mesh.md.
 *
 * The loop is the whole design. One poll waits on two things at once - this
 * service's own clients and the radiod connection - and one turn of the
 * MeshCore runtime follows every wait. There is no busy poll, no thread and
 * no blocking sleep in the path a packet travels: the wait's timeout is the
 * protocol tick, and it collapses to zero while there is a received frame
 * still to hand over, so a burst is drained without the daemon going deaf to
 * its own clients meanwhile.
 *
 * What keeps running when things go away, which is what a protocol service
 * is for:
 *
 *   no client connected      the runtime ticks, receives, learns nodes and
 *                            answers messages exactly as before; clients are
 *                            observers of a daemon, not owners of it.
 *   a client disconnects     its subscription goes with it and nothing else
 *                            changes.
 *   radiod restarts          the connection is remade, the lease asked for
 *                            again, the profile applied again, and the
 *                            protocol core - identity, nodes, paths,
 *                            duplicate table - is never torn down.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#define _GNU_SOURCE
#include "mcd.h"

#include "mcd_util.h"
#include "radio_link.h"

#include "pocketlog/pocketlog.h"
#include "pocketpaths.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* The protocol tick. MeshCore's own scheduling works in hundreds of
 * milliseconds - a delayed ACK is 200 ms, a CAD retry 200 ms - so 50 ms is
 * fine grained enough to be invisible to it and coarse enough to cost
 * nothing on a handheld. The wait is a poll, not a sleep: a client request
 * or a radio event wakes it immediately. */
#define MCD_TICK_MS 50

/* How often the node table is written when it has changed. Writing on every
 * change would put a file write in the packet path; a node learned and then
 * lost to a power cut within this window is re-learned from the next advert. */
#define MCD_PERSIST_INTERVAL_MS 10000

static volatile sig_atomic_t stop_requested;

static void on_signal(int sig)
{
    (void)sig;
    stop_requested = 1;
}

uint64_t mcd_mono_ms(void)
{
    struct timespec ts;

    /* CLOCK_MONOTONIC throughout, for the reason docs/api/radio.md gives:
     * this board starts at 1970 on every boot and jumps by decades when the
     * network comes up, so an interval measured on the wall clock across
     * that step is wrong by a generation. */
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

const char *mcd_state_name(enum mcd_state s)
{
    switch (s) {
    case MCD_STARTING: return "starting";
    case MCD_WAITING_FOR_RADIOD: return "waiting_for_radiod";
    case MCD_WAITING_FOR_LEASE: return "waiting_for_lease";
    case MCD_CONFIGURING: return "configuring";
    case MCD_ONLINE: return "online";
    case MCD_DEGRADED: return "degraded";
    case MCD_ERROR: return "error";
    case MCD_STATE_COUNT: break;
    }
    return "unknown";
}

void mcd_profile_defaults(struct mcd_profile *p)
{
    p->frequency_mhz = MCD_DEFAULT_FREQUENCY_MHZ;
    p->bandwidth_khz = MCD_DEFAULT_BANDWIDTH_KHZ;
    p->spreading_factor = MCD_DEFAULT_SPREADING_FACTOR;
    p->coding_rate = MCD_DEFAULT_CODING_RATE;
    p->sync_word = MCD_DEFAULT_SYNC_WORD;
    p->preamble_length = MCD_DEFAULT_PREAMBLE;
    p->tx_power_dbm = MCD_DEFAULT_TX_POWER_DBM;
    p->crc = true;
}

int mcd_profile_validate(const struct mcd_profile *p, char *err, size_t errlen)
{
    if (!(p->frequency_mhz >= 137.0 && p->frequency_mhz <= 1020.0)) {
        snprintf(err, errlen, "frequency_mhz %g is outside any LoRa band", p->frequency_mhz);
        return -1;
    }
    if (!(p->bandwidth_khz >= 7.8 && p->bandwidth_khz <= 500.0)) {
        snprintf(err, errlen, "bandwidth_khz %g is outside 7.8 to 500", p->bandwidth_khz);
        return -1;
    }
    if (p->spreading_factor < 5 || p->spreading_factor > 12) {
        snprintf(err, errlen, "spreading_factor %d is outside 5 to 12", p->spreading_factor);
        return -1;
    }
    if (p->coding_rate < 5 || p->coding_rate > 8) {
        snprintf(err, errlen, "coding_rate %d is outside 5 to 8", p->coding_rate);
        return -1;
    }
    if (p->sync_word < 0 || p->sync_word > 0xFF) {
        snprintf(err, errlen, "sync_word %d is outside 0 to 255", p->sync_word);
        return -1;
    }
    if (p->preamble_length < 1 || p->preamble_length > 65535) {
        snprintf(err, errlen, "preamble_length %d is outside 1 to 65535", p->preamble_length);
        return -1;
    }
    /* The SX1262's own range. radiod's region guard is stricter and is the
     * one that decides; this refuses a value no transceiver could take, at
     * start-up, rather than at the first configure. */
    if (p->tx_power_dbm < -9 || p->tx_power_dbm > 22) {
        snprintf(err, errlen, "tx_power_dbm %d is outside -9 to 22", p->tx_power_dbm);
        return -1;
    }
    return 0;
}

void mcd_backoff_reset(struct mcd_backoff *b)
{
    b->delay_ms = MCD_BACKOFF_MIN_MS;
    b->next_attempt_ms = 0;
}

void mcd_backoff_failed(struct mcd_backoff *b, uint64_t now_ms)
{
    if (b->delay_ms == 0) {
        b->delay_ms = MCD_BACKOFF_MIN_MS;
    }
    b->next_attempt_ms = now_ms + b->delay_ms;
    if (b->delay_ms < MCD_BACKOFF_MAX_MS) {
        b->delay_ms *= 2;
        if (b->delay_ms > MCD_BACKOFF_MAX_MS) {
            b->delay_ms = MCD_BACKOFF_MAX_MS;
        }
    }
}

bool mcd_backoff_due(const struct mcd_backoff *b, uint64_t now_ms)
{
    return now_ms >= b->next_attempt_ms;
}

/* ---- state and events --------------------------------------------------- */

void mcd_broadcast(struct mcd *d, cJSON *event)
{
    if (!event) {
        return;
    }
    if (d->server) {
        pocketipc_server_broadcast(d->server, event);
    } else {
        cJSON_Delete(event);
    }
}

void mcd_set_state(struct mcd *d, enum mcd_state s, const char *reason)
{
    bool changed = (d->state != s);

    snprintf(d->state_reason, sizeof(d->state_reason), "%s", reason ? reason : "");
    if (!changed) {
        return;
    }
    LOG_INFO("state %s -> %s (%s)", mcd_state_name(d->state), mcd_state_name(s),
             d->state_reason);
    d->state = s;
    d->state_since_ms = mcd_mono_ms();
    mcd_broadcast(d, mcd_event_state(d));
}

/* ---- the runtime's hooks ------------------------------------------------ */

static int hook_tx_submit(void *user, const uint8_t *bytes, int len, uint64_t *submit_id)
{
    struct mcd *d = user;

    /* The runtime is created before the radiod connection, and it cannot
     * transmit until that connection has told it the radio is online - so
     * this is unreachable today. It is here because the ordering is the only
     * thing that makes it so, and a daemon should not be one refactor away
     * from dereferencing nothing. */
    if (!d->link) {
        return -1;
    }
    return mcd_link_submit_tx(d->link, bytes, len, submit_id);
}

static void hook_on_node(void *user, const struct mcd_node *n, const char *reason)
{
    struct mcd *d = user;

    mcd_broadcast(d, mcd_event_node(n, reason));
}

static void hook_on_message(void *user, const struct mcd_message *m)
{
    struct mcd *d = user;

    mcd_broadcast(d, mcd_event_message(m));
}

static void hook_on_channel(void *user, const struct mcd_channel *c, const char *reason)
{
    struct mcd *d = user;

    mcd_broadcast(d, mcd_event_channel(c, reason));
}

static void hook_on_frame(void *user, const struct mcd_rx_meta *meta, int bytes,
                          const char *outcome)
{
    struct mcd *d = user;

    mcd_broadcast(d, mcd_event_activity_rx(meta, bytes, outcome));
}

static void runtime_log(int level, const char *line)
{
    /* mcport's levels are pocketlog's, in pocketlog's order, which is why
     * this is a cast and not a table. */
    pocketlog_write((enum pocketlog_level)level, "%s", line);
}

/* ---- the loop ----------------------------------------------------------- */

static int run(struct mcd *d)
{
    uint64_t last_persist = mcd_mono_ms();

    while (!stop_requested) {
        int radio_fd = mcd_link_fd(d->link);
        int radio_ready = 0;
        int timeout = MCD_TICK_MS;
        uint64_t now;

        /* A received frame still waiting to be handed over means there is
         * work to do now. The wait collapses, the turn below delivers one
         * frame, and the loop comes back here - so a burst is drained one
         * frame per turn while this service's own clients keep being served
         * between them. */
        if (mcd_runtime_rx_pending(d->rt)) {
            timeout = 0;
        }
        if (pocketipc_server_poll_fd(d->server, timeout, radio_fd, &radio_ready) < 0) {
            if (errno == EINTR) {
                continue;
            }
            LOG_ERROR("poll: %s", strerror(errno));
            return 1;
        }
        if (radio_ready) {
            mcd_link_readable(d->link);
        }
        now = mcd_mono_ms();
        mcd_link_step(d->link, now);
        mcd_runtime_tick(d->rt);

        if (mcd_runtime_dirty(d->rt) && now - last_persist >= MCD_PERSIST_INTERVAL_MS) {
            mcd_runtime_persist(d->rt);
            last_persist = now;
        }
    }
    return 0;
}

/* ---- start-up ----------------------------------------------------------- */

static void usage(FILE *out)
{
    fprintf(out,
            "usage: meshcored [--radiod-socket NAME] [--socket-name NAME]\n"
            "                 [--state-dir DIR] [--name NODE-NAME]\n"
            "                 [--tx-power-dbm N] [--frequency-mhz F] [--bandwidth-khz F]\n"
            "                 [--spreading-factor N] [--coding-rate N] [--sync-word N]\n"
            "                 [--preamble N] [--no-crc] [--verbose]\n"
            "\n"
            "The MeshCore protocol service. It talks only to radiod; it opens no SPI\n"
            "device and drives no GPIO.\n"
            "\n"
            "Defaults are the MeshCore profile proven on air by the accepted P0 gate:\n"
            "  %g MHz, %g kHz, SF%d, CR 4/%d, sync 0x%02X, preamble %d, CRC on, %d dBm.\n"
            "Regional policy (duty cycle, ERP, sub-band) is the operator's and radiod's\n"
            "region guard, not this service's.\n"
            "\n"
            "State:   $POCKETOS_STATE_DIR/meshcored (default %s/meshcored)\n"
            "Sockets: $POCKETOS_RUNTIME_DIR or %s\n",
            MCD_DEFAULT_FREQUENCY_MHZ, MCD_DEFAULT_BANDWIDTH_KHZ,
            MCD_DEFAULT_SPREADING_FACTOR, MCD_DEFAULT_CODING_RATE,
            MCD_DEFAULT_SYNC_WORD, MCD_DEFAULT_PREAMBLE, MCD_DEFAULT_TX_POWER_DBM,
            POCKETOS_STATE_DIR_DEFAULT, POCKETIPC_DEFAULT_DIR);
}

static int arg_int(const char *s, long lo, long hi, long *out)
{
    char *end;
    long v;

    if (s == NULL || *s == '\0') {
        return -1;
    }
    errno = 0;
    v = strtol(s, &end, 0);
    if (errno != 0 || *end != '\0' || v < lo || v > hi) {
        return -1;
    }
    *out = v;
    return 0;
}

static int arg_double(const char *s, double *out)
{
    char *end;
    double v;

    if (s == NULL || *s == '\0') {
        return -1;
    }
    errno = 0;
    v = strtod(s, &end);
    if (errno != 0 || *end != '\0') {
        return -1;
    }
    *out = v;
    return 0;
}

int main(int argc, char **argv)
{
    struct mcd d;
    struct mcd_runtime_config rcfg;
    struct mcd_runtime_hooks hooks;
    char err[256] = "";
    const char *state_base;
    int i;
    int rc;

    memset(&d, 0, sizeof(d));
    d.state_lock_fd = -1;
    snprintf(d.cfg.socket_name, sizeof(d.cfg.socket_name), "%s", MCD_SERVICE_NAME);
    snprintf(d.cfg.radiod_socket, sizeof(d.cfg.radiod_socket), "radiod");
    mcd_profile_defaults(&d.cfg.profile);

    pocketlog_init(MCD_SERVICE_NAME);
    pocketlog_install_crash_handler();

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        long v;

        if (strcmp(a, "--radiod-socket") == 0 && i + 1 < argc) {
            snprintf(d.cfg.radiod_socket, sizeof(d.cfg.radiod_socket), "%s", argv[++i]);
        } else if (strcmp(a, "--socket-name") == 0 && i + 1 < argc) {
            snprintf(d.cfg.socket_name, sizeof(d.cfg.socket_name), "%s", argv[++i]);
        } else if (strcmp(a, "--state-dir") == 0 && i + 1 < argc) {
            snprintf(d.cfg.state_dir, sizeof(d.cfg.state_dir), "%s", argv[++i]);
        } else if (strcmp(a, "--name") == 0 && i + 1 < argc) {
            snprintf(d.cfg.node_name, sizeof(d.cfg.node_name), "%s", argv[++i]);
            d.cfg.node_name_given = true;
        } else if (strcmp(a, "--tx-power-dbm") == 0 && i + 1 < argc) {
            if (arg_int(argv[++i], -100, 100, &v) != 0) {
                LOG_ERROR("--tx-power-dbm needs an integer, got '%s'", argv[i]);
                return 2;
            }
            d.cfg.profile.tx_power_dbm = (int)v;
        } else if (strcmp(a, "--frequency-mhz") == 0 && i + 1 < argc) {
            if (arg_double(argv[++i], &d.cfg.profile.frequency_mhz) != 0) {
                LOG_ERROR("--frequency-mhz needs a number, got '%s'", argv[i]);
                return 2;
            }
        } else if (strcmp(a, "--bandwidth-khz") == 0 && i + 1 < argc) {
            if (arg_double(argv[++i], &d.cfg.profile.bandwidth_khz) != 0) {
                LOG_ERROR("--bandwidth-khz needs a number, got '%s'", argv[i]);
                return 2;
            }
        } else if (strcmp(a, "--spreading-factor") == 0 && i + 1 < argc) {
            if (arg_int(argv[++i], 0, 20, &v) != 0) {
                LOG_ERROR("--spreading-factor needs an integer, got '%s'", argv[i]);
                return 2;
            }
            d.cfg.profile.spreading_factor = (int)v;
        } else if (strcmp(a, "--coding-rate") == 0 && i + 1 < argc) {
            if (arg_int(argv[++i], 0, 20, &v) != 0) {
                LOG_ERROR("--coding-rate needs an integer, got '%s'", argv[i]);
                return 2;
            }
            d.cfg.profile.coding_rate = (int)v;
        } else if (strcmp(a, "--sync-word") == 0 && i + 1 < argc) {
            if (arg_int(argv[++i], -1, 4096, &v) != 0) {
                LOG_ERROR("--sync-word needs an integer, got '%s'", argv[i]);
                return 2;
            }
            d.cfg.profile.sync_word = (int)v;
        } else if (strcmp(a, "--preamble") == 0 && i + 1 < argc) {
            if (arg_int(argv[++i], 0, 100000, &v) != 0) {
                LOG_ERROR("--preamble needs an integer, got '%s'", argv[i]);
                return 2;
            }
            d.cfg.profile.preamble_length = (int)v;
        } else if (strcmp(a, "--no-crc") == 0) {
            d.cfg.profile.crc = false;
        } else if (strcmp(a, "--verbose") == 0) {
            d.cfg.verbose = true;
            pocketlog_set_level(POCKETLOG_DEBUG);
        } else if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            usage(stdout);
            return 0;
        } else {
            usage(stderr);
            return 2;
        }
    }

    if (mcd_profile_validate(&d.cfg.profile, err, sizeof(err)) != 0) {
        LOG_ERROR("radio profile rejected: %s", err);
        return 2;
    }
    if (d.cfg.state_dir[0] == '\0') {
        state_base = pocketos_state_dir();
        snprintf(d.cfg.state_dir, sizeof(d.cfg.state_dir), "%s/%s", state_base,
                 MCD_SERVICE_NAME);
    }

    /* One process per node, before anything reads the identity or takes the
     * socket: a second meshcored - a hand-started one beside the supervised
     * one, say - would otherwise be a second radio with the same identity,
     * rewriting the same node table, and its listen would unlink the first
     * one's socket from under every client. It leaves with 3, which
     * pos-supervise treats like any failure: backoff, then crash loop. */
    {
        bool busy = false;

        d.state_lock_fd = mcd_runtime_lock_state_dir(d.cfg.state_dir, &busy, err, sizeof(err));
        if (d.state_lock_fd < 0) {
            LOG_ERROR("%s", err);
            pocketlog_close();
            return busy ? 3 : 1;
        }
    }

    d.start_ms = mcd_mono_ms();
    d.state = MCD_STARTING;
    snprintf(d.state_reason, sizeof(d.state_reason), "starting");
    d.state_since_ms = d.start_ms;

    /* The protocol core's own logging goes to pocketlog, so MeshCore's
     * warnings land in the same file as everything else this service says
     * rather than on a stderr nobody reads. */
    mcd_runtime_set_log_sink(runtime_log);

    memset(&hooks, 0, sizeof(hooks));
    hooks.tx_submit = hook_tx_submit;
    hooks.on_node = hook_on_node;
    hooks.on_message = hook_on_message;
    hooks.on_channel = hook_on_channel;
    hooks.on_frame = hook_on_frame;
    hooks.user = &d;

    memset(&rcfg, 0, sizeof(rcfg));
    rcfg.state_dir = d.cfg.state_dir;
    rcfg.node_name = d.cfg.node_name_given ? d.cfg.node_name : NULL;
    rcfg.verbose = d.cfg.verbose;

    d.rt = mcd_runtime_create(&rcfg, &hooks, err, sizeof(err));
    if (!d.rt) {
        /* A corrupt identity file lands here, and it stops the service on
         * purpose. Generating a replacement would make this a different node
         * to every peer that knows it and would destroy the only copy of a
         * key nothing else holds. */
        LOG_ERROR("cannot start the MeshCore runtime: %s", err);
        close(d.state_lock_fd);
        pocketlog_close();
        return 1;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    d.link = mcd_link_new(&d);
    if (!d.link) {
        LOG_ERROR("out of memory");
        mcd_runtime_destroy(d.rt);
        close(d.state_lock_fd);
        pocketlog_close();
        return 1;
    }

    d.server = pocketipc_server_new(d.cfg.socket_name, mcd_handle_request, &d);
    if (!d.server) {
        LOG_ERROR("cannot listen: %s", strerror(errno));
        mcd_link_free(d.link);
        mcd_runtime_destroy(d.rt);
        close(d.state_lock_fd);
        pocketlog_close();
        return 1;
    }
    LOG_INFO("listening on %s, state in %s, radiod socket %s",
             pocketipc_server_path(d.server), d.cfg.state_dir, d.cfg.radiod_socket);
    mcd_set_state(&d, MCD_WAITING_FOR_RADIOD, "looking for radiod");

    rc = run(&d);

    LOG_INFO("shutting down (rc=%d)", rc);
    mcd_link_shutdown(d.link);
    /* The node table is written on the way out whether or not the interval
     * had come round, so a clean stop never loses what this session learned. */
    if (mcd_runtime_dirty(d.rt)) {
        mcd_runtime_persist(d.rt);
    }
    pocketipc_server_free(d.server);
    mcd_link_free(d.link);
    mcd_runtime_destroy(d.rt);
    /* Last, after the node table is written: the next process may take the
     * directory the moment this is released. */
    close(d.state_lock_fd);
    pocketlog_close();
    return rc;
}
