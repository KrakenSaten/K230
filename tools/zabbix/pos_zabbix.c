/*
 * pos-zabbix: the only Doors program that talks to a Zabbix server.
 *
 *   pos-zabbix session [--fake SCENARIO] [--config FILE] [--secret FILE]
 *       The Zabbix app's helper (docs/apps/ZABBIX.md). stdin and stdout are
 *       a socketpair to the shell; the protocol is core/zabbix/zbx_proto.h.
 *       It lives exactly as long as the Zabbix screen: it leaves on `quit`,
 *       when the shell closes its end, on SIGTERM, and - through
 *       PR_SET_PDEATHSIG, set by the session before exec - when the shell
 *       dies.
 *   pos-zabbix check [--fake SCENARIO] [--config FILE] [--secret FILE] [--detail HOSTID]
 *       One round (version, problems, hosts, and a host when asked), the
 *       protocol lines on stdout. Exit 0 when the server answered, 1 when
 *       it did not, 2 for usage or configuration. For the bench.
 *   pos-zabbix set-secret token|password [--secret FILE]
 *       Store the API token (or password) read from ONE line of stdin, in a
 *       0600 file under a 0700 directory. Never from argv: a token in argv
 *       is in every process listing and in the shell's history.
 *   pos-zabbix clear-secret [--secret FILE]
 *   pos-zabbix transport
 *       "curl" when this build can reach a real server, "none" when it has
 *       only the fake.
 *   pos-zabbix scenarios
 *       The fake backend's scenario names.
 *
 * The backend is the real one (libcurl) unless --fake is given, the
 * configuration says mode=fake, or $POCKETOS_ZABBIX_BACKEND is "fake" (the
 * simulator's default); $POCKETOS_ZABBIX_FAKE then names the scenario.
 *
 * Nothing here blocks for longer than one request's timeout (timeout_s,
 * 3..30 s). A request that ignores it - a resolver stuck in the kernel - is
 * what the session's watchdog and SIGKILL are for.
 *
 * Exit codes: 0 done, 1 check failed, 2 usage or setup.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#define _GNU_SOURCE
#include "pocketlog/pocketlog.h"
#include "zabbix/zbx_client.h"
#include "zabbix/zbx_config.h"
#include "zabbix/zbx_fake.h"
#include "zabbix/zbx_http.h"
#include "zabbix/zbx_proto.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t stop;

static void on_signal(int sig)
{
    (void)sig;
    stop = 1;
}

static int64_t mono_ms(void *user)
{
    struct timespec ts;

    (void)user;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int usage(void)
{
    fprintf(stderr,
            "usage: pos-zabbix session|check [--fake SCENARIO] [--config FILE] [--secret FILE]"
            " [--detail HOSTID]\n"
            "       pos-zabbix set-secret token|password [--secret FILE]   (value on stdin)\n"
            "       pos-zabbix clear-secret [--secret FILE]\n"
            "       pos-zabbix transport | scenarios\n");
    return 2;
}

struct opts {
    const char *fake;
    const char *config;
    const char *secret;
    const char *detail;
};

static int parse_opts(int argc, char **argv, int first, struct opts *o)
{
    int i;

    memset(o, 0, sizeof(*o));
    for (i = first; i < argc; i++) {
        if (i + 1 >= argc) {
            return -1;
        }
        if (strcmp(argv[i], "--fake") == 0) {
            o->fake = argv[++i];
        } else if (strcmp(argv[i], "--config") == 0) {
            o->config = argv[++i];
        } else if (strcmp(argv[i], "--secret") == 0) {
            o->secret = argv[++i];
        } else if (strcmp(argv[i], "--detail") == 0) {
            o->detail = argv[++i];
        } else {
            return -1;
        }
    }
    return 0;
}

/* The configuration this run uses: the files, then the fake if anything
 * asks for it. A configuration error leaves an unconfigured config and the
 * reason in err, which the app shows. */
static void load_config(const struct opts *o, struct zbx_config *cfg, char *err, size_t errlen)
{
    char conf[ZBX_PATH_MAX];
    char secret[ZBX_PATH_MAX];
    const char *env = getenv("POCKETOS_ZABBIX_BACKEND");
    const char *scenario = o->fake;

    zbx_config_paths(conf, sizeof(conf), secret, sizeof(secret));
    if (zbx_config_load(cfg, o->config ? o->config : conf, o->secret ? o->secret : secret, err,
                        errlen) != 0) {
        LOG_WARN("zabbix: configuration refused: %s", err);
    }
    if (!scenario && env && strcmp(env, "fake") == 0) {
        scenario = getenv("POCKETOS_ZABBIX_FAKE");
        if (!scenario || !*scenario) {
            scenario = cfg->fake ? cfg->scenario : ZBX_FAKE_DEFAULT;
        }
    }
    if (scenario) {
        if (!zbx_fake_known(scenario)) {
            snprintf(err, errlen, "unknown fake scenario %s", scenario);
            cfg->configured = false;
            return;
        }
        cfg->fake = true;
        snprintf(cfg->scenario, sizeof(cfg->scenario), "%s", scenario);
    }
    if (cfg->fake) {
        cfg->configured = true;
        err[0] = '\0';
        snprintf(cfg->url_shown, sizeof(cfg->url_shown), "fake://%s", cfg->scenario);
        if (!cfg->label[0]) {
            snprintf(cfg->label, sizeof(cfg->label), "Demo server");
        }
    }
}

/* ---- session ---------------------------------------------------------------------- */

struct session {
    struct zbx_config cfg;
    struct zbx_fake fake;
    struct zbx_transport tr;
    struct zbx_client client;
    char in[ZBX_LINE_MAX];
    size_t in_len;
    bool overlong;
};

static int open_transport(struct session *s, char *err, size_t errlen)
{
    if (s->cfg.fake) {
        zbx_fake_init(&s->fake, s->cfg.scenario);
        s->fake.realtime = true;
        zbx_transport_fake(&s->tr, &s->fake);
        return 0;
    }
    if (!s->cfg.configured) {
        zbx_transport_fake(&s->tr, &s->fake); /* never called: nothing is due */
        return 0;
    }
    if (zbx_transport_curl(&s->tr, err, errlen) != 0) {
        return -1;
    }
    return 0;
}

/* One command line. Returns false when the helper should leave. */
static bool command(struct session *s, char *line)
{
    struct zbx_cmd cmd;

    switch (zbx_cmd_parse(line, &cmd)) {
    case ZBX_CMD_QUIT:
        return false;
    case ZBX_CMD_SCENARIO:
        if (s->cfg.fake && zbx_fake_set_scenario(&s->fake, cmd.word) == 0) {
            snprintf(s->cfg.scenario, sizeof(s->cfg.scenario), "%.15s", cmd.word);
            snprintf(s->client.cfg.scenario, sizeof(s->client.cfg.scenario), "%.15s", cmd.word);
            LOG_INFO("zabbix: fake scenario %s", cmd.word);
            zbx_proto_hello(stdout, true, cmd.word);
            zbx_client_reset(&s->client);
        }
        return true;
    case ZBX_CMD_REFRESH:
    case ZBX_CMD_DETAIL:
        zbx_client_command(&s->client, &cmd);
        return true;
    case ZBX_CMD_NONE:
    default:
        return true; /* a word from a newer app */
    }
}

/* Read what the app sent, after poll() said there is something. Returns
 * false at end of input.
 *
 * stdin and stdout are one socket, so the descriptor is never made
 * non-blocking: O_NONBLOCK is a property of the open file, and on stdout it
 * made a burst of a few hundred lines (a large estate's sets) fail with
 * EAGAIN once the socket buffer filled - stdio then flagged an error and the
 * helper left as if the app had gone. Instead each read here is
 * non-blocking on its own (MSG_DONTWAIT); on a pipe (the bench, by hand)
 * that is not a socket, and exactly one read is made, which poll() said
 * will not block. */
static bool read_commands(struct session *s)
{
    char buf[512];
    bool socket = true;
    int reads = 0;

    for (;;) {
        ssize_t n;
        ssize_t i;

        if (socket) {
            n = recv(STDIN_FILENO, buf, sizeof(buf), MSG_DONTWAIT);
            if (n < 0 && errno == ENOTSOCK) {
                socket = false;
                continue;
            }
        } else if (reads > 0) {
            return true;
        } else {
            n = read(STDIN_FILENO, buf, sizeof(buf));
        }
        reads++;
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return true;
        }
        if (n <= 0) {
            return false;
        }
        for (i = 0; i < n; i++) {
            if (buf[i] == '\n') {
                s->in[s->in_len] = '\0';
                if (!s->overlong && !command(s, s->in)) {
                    return false;
                }
                s->in_len = 0;
                s->overlong = false;
            } else if (s->in_len + 1 < sizeof(s->in)) {
                s->in[s->in_len++] = buf[i];
            } else {
                s->overlong = true;
            }
        }
    }
}

static int run_session(const struct opts *o)
{
    static struct session s;
    char err[ZBX_TEXT_MAX] = "";
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN); /* a closed socket is an EPIPE, and we leave */

    load_config(o, &s.cfg, err, sizeof(err));
    if (open_transport(&s, err, sizeof(err)) != 0) {
        s.cfg.configured = false;
    }
    if (zbx_client_init(&s.client, &s.cfg, &s.tr, stdout, mono_ms, NULL) != 0) {
        return 2;
    }
    zbx_config_forget_secret(&s.cfg); /* the client holds the one copy */
    zbx_proto_hello(stdout, s.client.cfg.fake, s.client.cfg.fake ? s.client.cfg.scenario : "-");
    zbx_client_start(&s.client);
    if (!s.client.cfg.configured && err[0]) {
        /* Say why: the app shows it on STATUS. */
        zbx_proto_state(stdout, ZBX_CONN_UNCONFIGURED, 0, 0, ZBX_ERR_CONFIG, err);
    }
    LOG_INFO("zabbix: session start, %s, %s", s.client.cfg.fake ? "fake" : s.tr.name ? s.tr.name : "-",
             s.client.cfg.configured ? "configured" : "not configured");

    while (!stop) {
        struct pollfd p = { .fd = STDIN_FILENO, .events = POLLIN };
        int wait = zbx_client_step(&s.client);
        int pr;

        if (ferror(stdout)) {
            break; /* the app is gone */
        }
        if (stop) {
            break;
        }
        pr = poll(&p, 1, wait);
        if (pr < 0 && errno != EINTR) {
            break;
        }
        if (pr > 0 && !read_commands(&s)) {
            break;
        }
    }
    zbx_proto_bye(stdout);
    zbx_client_free(&s.client);
    if (s.tr.close) {
        s.tr.close(&s.tr);
    }
    LOG_INFO("zabbix: session end");
    return 0;
}

/* ---- check ----------------------------------------------------------------------- */

static int run_check(const struct opts *o)
{
    static struct session s;
    char err[ZBX_TEXT_MAX] = "";
    int rc;

    load_config(o, &s.cfg, err, sizeof(err));
    if (!s.cfg.configured) {
        fprintf(stderr, "pos-zabbix: not configured%s%s\n", err[0] ? ": " : "", err);
        return 2;
    }
    if (open_transport(&s, err, sizeof(err)) != 0) {
        fprintf(stderr, "pos-zabbix: %s\n", err);
        return 2;
    }
    s.fake.realtime = false;
    if (zbx_client_init(&s.client, &s.cfg, &s.tr, stdout, mono_ms, NULL) != 0) {
        return 2;
    }
    zbx_config_forget_secret(&s.cfg);
    if (o->detail) {
        struct zbx_cmd cmd = { .kind = ZBX_CMD_DETAIL };

        snprintf(cmd.word, sizeof(cmd.word), "%s", o->detail);
        zbx_client_command(&s.client, &cmd);
    }
    zbx_proto_hello(stdout, s.client.cfg.fake, s.client.cfg.fake ? s.client.cfg.scenario : "-");
    zbx_client_start(&s.client);
    zbx_client_step(&s.client);
    rc = s.client.state == ZBX_CONN_ONLINE ? 0 : 1;
    zbx_client_free(&s.client);
    if (s.tr.close) {
        s.tr.close(&s.tr);
    }
    return rc;
}

/* ---- the secret -------------------------------------------------------------------- */

static int run_set_secret(const char *kind, const struct opts *o, bool clear)
{
    char path[ZBX_PATH_MAX];
    char value[ZBX_SECRET_MAX + 2];
    char err[ZBX_TEXT_MAX];
    size_t n;
    int rc;

    zbx_config_paths(NULL, 0, path, sizeof(path));
    if (o->secret) {
        snprintf(path, sizeof(path), "%s", o->secret);
    }
    if (clear) {
        rc = zbx_config_write_secret(path, "token", "", err, sizeof(err));
    } else {
        if (!fgets(value, sizeof(value), stdin)) {
            fprintf(stderr, "pos-zabbix: nothing on stdin\n");
            return 2;
        }
        n = strlen(value);
        while (n > 0 && (value[n - 1] == '\n' || value[n - 1] == '\r')) {
            value[--n] = '\0';
        }
        rc = zbx_config_write_secret(path, kind, value, err, sizeof(err));
        explicit_bzero(value, sizeof(value));
    }
    if (rc != 0) {
        fprintf(stderr, "pos-zabbix: %s\n", err);
        return 2;
    }
    fprintf(stderr, "pos-zabbix: %s %s\n", clear ? "removed" : "stored", path);
    return 0;
}

int main(int argc, char **argv)
{
    struct opts o;
    const char *const *sc;

    if (argc < 2) {
        return usage();
    }
    if (strcmp(argv[1], "transport") == 0) {
        /* What this build can reach a real server with: "curl" or "none". */
        printf("%s\n", zbx_http_available() ? "curl" : "none");
        return 0;
    }
    if (strcmp(argv[1], "scenarios") == 0) {
        for (sc = zbx_fake_scenarios(); *sc; sc++) {
            printf("%s\n", *sc);
        }
        return 0;
    }
    if (strcmp(argv[1], "set-secret") == 0) {
        if (argc < 3 || (strcmp(argv[2], "token") != 0 && strcmp(argv[2], "password") != 0) ||
            parse_opts(argc, argv, 3, &o) != 0) {
            return usage();
        }
        return run_set_secret(argv[2], &o, false);
    }
    if (strcmp(argv[1], "clear-secret") == 0) {
        if (parse_opts(argc, argv, 2, &o) != 0) {
            return usage();
        }
        return run_set_secret("token", &o, true);
    }
    if (parse_opts(argc, argv, 2, &o) != 0) {
        return usage();
    }
    pocketlog_init("pos-zabbix");
    if (strcmp(argv[1], "session") == 0) {
        return run_session(&o);
    }
    if (strcmp(argv[1], "check") == 0) {
        return run_check(&o);
    }
    return usage();
}
