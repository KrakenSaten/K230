/*
 * RIFT's meshcored client, against a real socket and a scripted service.
 *
 * What is proved here is the part that cannot be proved with a mock in the
 * same process: that the client connects, reads a snapshot, subscribes,
 * keeps events that arrive while a request is outstanding, survives the
 * service disappearing under it, reconnects with a backoff and re-reads the
 * snapshot, and refuses rubbish without dropping the connection over it.
 *
 * It also proves a negative that matters more than any of them: this client
 * calls nothing that transmits. Opening RIFT must not put a packet on the
 * air, and the service here records every method it is asked for.
 *
 * No LVGL. The fake service runs in a child process, so the socket, the
 * framing and the disconnection are all real.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "rift_ipc.h"

#include "fake_meshcored.h"
#include "rift_format.h"
#include "rift_keys.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int failed;
static int checks;

static void check(const char *what, int ok)
{
    checks++;
    if (!ok) {
        failed++;
        printf("FAIL %s\n", what);
    }
}

static void text_is(const char *what, const char *got, const char *want)
{
    checks++;
    if (!got || strcmp(got, want) != 0) {
        failed++;
        printf("FAIL %s: got \"%s\", want \"%s\"\n", what, got ? got : "(null)", want);
    }
}

#define KEY_A "a19ac21e7d0411223344556677889900aabbccddeeff001122334455667788b1"
#define KEY_B "b2cafe1e7d0411223344556677889900aabbccddeeff001122334455667788b2"

#define NODES_TWO                                                                        \
    "[{\"public_key\":\"" KEY_A "\",\"node_hash\":\"a1\",\"name\":\"OSLO-01\",\"type\":1," \
    "\"path_known\":true,\"hops\":0,\"direct\":true,\"last_heard_mono_ms\":1000,"          \
    "\"last_rssi_dbm\":-71.0,\"last_snr_db\":9.5},"                                        \
    "{\"public_key\":\"" KEY_B "\",\"node_hash\":\"b2\",\"name\":\"HYTTA\",\"type\":2,"     \
    "\"path_known\":true,\"hops\":3,\"direct\":false,\"path_hex\":\"a1c2d3\","              \
    "\"last_heard_mono_ms\":500}]"

/* How long any one wait may take before it is called a failure. Every wait
 * below ends as soon as what it waits for has happened, so this is headroom,
 * not a delay: a sanitised build, a loaded host and a reconnect that misses
 * once and backs off all fit inside it, and a passing run never gets near it. */
#define WAIT_MS 15000

/* How long a scripted service runs if nobody stops it. Every section stops
 * its own; this only has to outlast the most waits one section makes (six),
 * each at its longest, so that no service ends under a check still waiting
 * for it. */
#define FAKE_LIFE_MS (8 * WAIT_MS)

#define KEY_C "c3beef1e7d0411223344556677889900aabbccddeeff001122334455667788b3"

static int64_t now_ms(void);
static int spin(struct rift_ipc *c, int budget_ms,
                int (*done)(const struct rift_ipc *, const struct rift_model *),
                const struct rift_model *m);
static int have_snapshot(const struct rift_ipc *c, const struct rift_model *m);
static int opening_answered(const struct rift_ipc *c, const struct rift_model *m);
static int is_down(const struct rift_ipc *c, const struct rift_model *m);
static int round_trip(struct rift_ipc *c, struct rift_model *m);

/* The lines a log file holds, each compared against want[] in order; returns
 * how many lines there were, or -1 when the file could not be read. */
static int log_lines(const char *path, const char *const *want, int want_n, int *matched)
{
    char line[256];
    int n = 0;
    FILE *f = fopen(path, "r");

    *matched = 1;
    if (!f) {
        return -1;
    }
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\n")] = '\0';
        if (n >= want_n || strcmp(line, want[n]) != 0) {
            *matched = 0;
        }
        n++;
    }
    fclose(f);
    return n;
}

static int advert_settled(const struct rift_ipc *c, const struct rift_model *m)
{
    (void)c;
    return !m->advert.active;
}

static int node_op_settled(const struct rift_ipc *c, const struct rift_model *m)
{
    (void)c;
    return !m->node_op.active;
}

/* An advert, on request and only then, against a real socket. */
static void test_adverts(const char *runtime)
{
    static struct rift_model m;
    static struct rift_ipc c;
    struct fake_meshcored_script script;
    char adverts[600];
    int matched = 0;
    pid_t pid;

    snprintf(adverts, sizeof(adverts), "%s/adverts", runtime);
    unlink(adverts);
    memset(&script, 0, sizeof(script));
    script.state = "online";
    script.nodes_json = NODES_TWO;
    script.advert_log = adverts;
    script.life_ms = FAKE_LIFE_MS;
    pid = fake_meshcored_spawn(&script);
    check("a service that takes adverts is running",
          pid > 0 && fake_meshcored_wait_ready(WAIT_MS));

    rift_model_init(&m);
    rift_ipc_init(&c, &m, RIFT_SERVICE);
    spin(&c, WAIT_MS, opening_answered, &m);
    /* The service handles requests in order and logs an advert as it takes
     * it, so once a later request is answered, any advert written while
     * connecting is in the log. */
    check("the service answered a later request too", round_trip(&c, &m));
    check("connecting and reading adverts nothing",
          log_lines(adverts, NULL, 0, &matched) <= 0);

    check("a zero-hop advert is asked for", rift_ipc_send_advert(&c, 1) == 0);
    check("and is in flight until the service answers", m.advert.active);
    check("a second, while it is, is refused and not written", rift_ipc_send_advert(&c, 0) == -1);
    spin(&c, WAIT_MS, advert_settled, &m);
    check("the service's answer settles it as accepted",
          m.advert.done && !m.advert.failed && m.advert.kind == RIFT_ACTION_ADVERT_NEAR);
    check("a flooded one is asked for next", rift_ipc_send_advert(&c, 0) == 0);
    spin(&c, WAIT_MS, advert_settled, &m);
    check("and settled the same way",
          m.advert.done && m.advert.kind == RIFT_ACTION_ADVERT_MESH);
    {
        static const char *const want[] = { "zero_hop", "flood" };

        check("the service was asked for exactly those two, in that order, as asked",
              log_lines(adverts, want, 2, &matched) == 2 && matched);
    }
    rift_ipc_close(&c);
    fake_meshcored_stop(pid);

    /* A service that refuses: the reader sees its words, not a success. */
    memset(&script, 0, sizeof(script));
    script.state = "online";
    script.nodes_json = NODES_TWO;
    script.refuse_advert = 1;
    script.life_ms = FAKE_LIFE_MS;
    pid = fake_meshcored_spawn(&script);
    check("a service that refuses adverts is running",
          pid > 0 && fake_meshcored_wait_ready(WAIT_MS));
    rift_model_init(&m);
    rift_ipc_init(&c, &m, RIFT_SERVICE);
    spin(&c, WAIT_MS, opening_answered, &m);
    rift_ipc_send_advert(&c, 1);
    spin(&c, WAIT_MS, advert_settled, &m);
    check("a refused advert is not called accepted", m.advert.failed && !m.advert.done);
    check("and carries the service's reason",
          strstr(m.advert.error, "the radio is not available") != NULL);
    rift_ipc_close(&c);
    fake_meshcored_stop(pid);

    /* Nobody there. */
    rift_model_init(&m);
    rift_ipc_init(&c, &m, "meshcored-that-is-not-there");
    check("an advert with no connection is refused", rift_ipc_send_advert(&c, 0) == -1);
    check("and says so", m.advert.failed && !m.advert.active);
    rift_ipc_close(&c);
}

/* Forgetting a node, and forgetting a route. Nothing transmits; what is
 * checked is that the change is asked for once, for the node named, and that
 * what the list shows afterwards is the service's answer. */
static void test_node_changes(const char *runtime)
{
    static struct rift_model m;
    static struct rift_ipc c;
    struct fake_meshcored_script script;
    char methods[600];
    const struct rift_node *n;
    pid_t pid;

    snprintf(methods, sizeof(methods), "%s/node-methods", runtime);
    unlink(methods);
    memset(&script, 0, sizeof(script));
    script.state = "online";
    script.nodes_json = NODES_TWO;
    script.method_log = methods;
    script.life_ms = FAKE_LIFE_MS;
    pid = fake_meshcored_spawn(&script);
    check("a service holding two nodes is running",
          pid > 0 && fake_meshcored_wait_ready(WAIT_MS));
    rift_model_init(&m);
    rift_ipc_init(&c, &m, RIFT_SERVICE);
    spin(&c, WAIT_MS, opening_answered, &m);
    check("both nodes are listed", m.node_count == 2);

    check("a prefix is not a node this app will ask to change",
          rift_ipc_reset_path(&c, "a19a", "OSLO-01") == -1 && m.node_op.failed);
    check("the route to a node is forgotten on request", rift_ipc_reset_path(&c, KEY_A, "OSLO-01") == 0);
    spin(&c, WAIT_MS, node_op_settled, &m);
    n = rift_model_find(&m, KEY_A);
    check("the answer settles it", m.node_op.done && m.node_op.kind == RIFT_ACTION_RESET_PATH);
    check("and the node is shown with no route", n && !n->path_known);

    check("a node is forgotten on request", rift_ipc_forget_node(&c, KEY_B, "HYTTA") == 0);
    spin(&c, WAIT_MS, node_op_settled, &m);
    check("the answer settles it", m.node_op.done && m.node_op.kind == RIFT_ACTION_FORGET);
    text_is("naming the node it was about", m.node_op.label, "HYTTA");
    check("and the node is gone from the list", rift_model_find(&m, KEY_B) == NULL &&
                                                    m.node_count == 1);
    /* The next snapshot, asked for and answered: a fixed wait passed this
     * whenever the answer had not arrived yet. */
    check("and stays gone in the next snapshot",
          round_trip(&c, &m) && rift_model_find(&m, KEY_B) == NULL);

    rift_ipc_forget_node(&c, KEY_C, "nobody");
    spin(&c, WAIT_MS, node_op_settled, &m);
    check("a node the service does not hold is refused in its words",
          m.node_op.failed && strstr(m.node_op.error, "no node") != NULL);
    rift_ipc_close(&c);
    fake_meshcored_stop(pid);
    {
        /* Asked for once each, and never on the way in or out. */
        char line[128];
        int removes = 0;
        int resets = 0;
        FILE *f = fopen(methods, "r");

        while (f && fgets(line, sizeof(line), f)) {
            line[strcspn(line, "\n")] = '\0';
            removes += strcmp(line, "mesh.node_remove") == 0;
            resets += strcmp(line, "mesh.node_reset_path") == 0;
        }
        if (f) {
            fclose(f);
        }
        check("the service was asked to forget twice - HYTTA, and the one it did not hold",
              removes == 2);
        check("and to forget a route once: the prefix never left this app", resets == 1);
    }

    /* A service that takes the request and dies before it answers. */
    memset(&script, 0, sizeof(script));
    script.state = "online";
    script.nodes_json = NODES_TWO;
    script.node_ops_silent = 1;
    script.life_ms = FAKE_LIFE_MS;
    pid = fake_meshcored_spawn(&script);
    check("a service that will not answer is running",
          pid > 0 && fake_meshcored_wait_ready(WAIT_MS));
    rift_model_init(&m);
    rift_ipc_init(&c, &m, RIFT_SERVICE);
    spin(&c, WAIT_MS, opening_answered, &m);
    rift_ipc_forget_node(&c, KEY_B, "HYTTA");
    /* It has taken the forget once it answers a request made after it, and
     * the forget is still unanswered: then it goes. Stopped here, not left to
     * a short life_ms, which could run out before a slow client had even
     * asked - and then this would be a different case. */
    check("the service took the forget and has not answered it",
          round_trip(&c, &m) && m.node_op.active);
    fake_meshcored_stop(pid);
    spin(&c, WAIT_MS, is_down, &m);
    check("a forget nobody answered is not called done",
          !m.node_op.done && !m.node_op.active && m.node_op.failed);
    check("it says it may or may not have happened",
          strstr(m.node_op.error, "may or may not") != NULL);
    check("and the node is still shown, as cached", rift_model_find(&m, KEY_B) != NULL && m.stale);
    rift_ipc_close(&c);
    fake_meshcored_stop(pid);
}

static int manage_settled(const struct rift_ipc *c, const struct rift_model *m)
{
    (void)c;
    return !m->manage_op.active;
}

/* Does this block of memory hold these bytes anywhere? A key the client was
 * handed must not outlive the request it went out in. */
static int holds_bytes(const void *block, size_t len, const char *what)
{
    size_t n = strlen(what);
    const unsigned char *b = block;
    size_t i;

    for (i = 0; n && i + n <= len; i++) {
        if (memcmp(b + i, what, n) == 0) {
            return 1;
        }
    }
    return 0;
}

/* ---- repeater control (rift_ipc_repeater.c) ---------------------------- */

#define REPEATER_B                                                                         \
    "{\"public_key\":\"" KEY_B "\",\"node_hash\":\"b2\",\"name\":\"HYTTA\",\"known\":true," \
    "\"type\":2,\"their_snr_db\":5.5,\"snr_db\":8.25,\"rssi_dbm\":-71}"

static int rep_idle(const struct rift_ipc *c, const struct rift_model *m)
{
    (void)c;
    return !rift_rep_busy(&m->repeater);
}

static int rep_listed(const struct rift_ipc *c, const struct rift_model *m)
{
    (void)c;
    return m->repeater.scan.count >= 1 && !m->repeater.scan.asking;
}

static int rep_logged(const struct rift_ipc *c, const struct rift_model *m)
{
    (void)c;
    return rift_rep_logged_in(&m->repeater, KEY_B);
}

static int rep_status(const struct rift_ipc *c, const struct rift_model *m)
{
    (void)c;
    return m->repeater.status.have;
}

static int rep_transcript(const struct rift_ipc *c, const struct rift_model *m)
{
    (void)c;
    return m->repeater.line_count >= 3;
}

/* How many lines of the log start with this. */
static int log_count(const char *path, const char *prefix)
{
    FILE *f = fopen(path, "r");
    char line[512];
    int n = 0;

    if (!f) {
        return 0;
    }
    while (fgets(line, sizeof(line), f)) {
        n += strncmp(line, prefix, strlen(prefix)) == 0;
    }
    fclose(f);
    return n;
}

static int log_has(const char *path, const char *text)
{
    FILE *f = fopen(path, "r");
    char line[512];
    int found = 0;

    if (!f) {
        return 0;
    }
    while (!found && fgets(line, sizeof(line), f)) {
        found = strstr(line, text) != NULL;
    }
    fclose(f);
    return found;
}

static void test_repeater_control(const char *runtime)
{
    static struct rift_model m;
    static struct rift_ipc c;
    struct fake_meshcored_script script;
    char rlog[600];
    char pw[RIFT_REP_PASSWORD_MAX + 1];
    pid_t pid;
    size_t i;
    int zero;

    snprintf(rlog, sizeof(rlog), "%s/remote", runtime);
    unlink(rlog);
    memset(&script, 0, sizeof(script));
    script.state = "online";
    script.nodes_json = NODES_TWO;
    script.repeater_json = REPEATER_B;
    script.remote_log = rlog;
    script.life_ms = FAKE_LIFE_MS;
    pid = fake_meshcored_spawn(&script);
    check("a service with repeater control is running",
          pid > 0 && fake_meshcored_wait_ready(WAIT_MS));
    rift_model_init(&m);
    rift_ipc_init(&c, &m, RIFT_SERVICE);
    spin(&c, WAIT_MS, opening_answered, &m);
    check("the service answered a later request too", round_trip(&c, &m));
    check("connecting ends a session an earlier RIFT left, and reads the scan list",
          log_count(rlog, "mesh.remote_logout|") == 1 && log_count(rlog, "mesh.discovered|") == 1);
    check("and asks nothing that transmits", log_count(rlog, "mesh.discover|") == 0 &&
                                                 log_count(rlog, "mesh.remote_login|") == 0);

    /* SCAN 0-HOP, pressed three times. */
    check("a scan is asked for", rift_ipc_scan_repeaters(&c) == 0);
    check("pressed again while it is asked, nothing is written", rift_ipc_scan_repeaters(&c) == -1);
    spin(&c, WAIT_MS, rep_listed, &m);
    check("the repeater that answered is listed, heard directly in this round",
          m.repeater.scan.count == 1 && m.repeater.scan.found[0].current &&
              strcmp(m.repeater.scan.found[0].key, KEY_B) == 0);
    check("pressed while the round is open, nothing is written either",
          rift_ipc_scan_repeaters(&c) == -1);
    round_trip(&c, &m);
    check("three presses were one request", log_count(rlog, "mesh.discover|") == 1);

    /* A wrong password: the service reports the wait running out. */
    snprintf(pw, sizeof(pw), "%s", "wrong-one");
    check("a login is asked for", rift_ipc_repeater_login(&c, KEY_B, pw, sizeof(pw)) == 0);
    zero = 1;
    for (i = 0; i < sizeof(pw); i++) {
        zero &= pw[i] == '\0';
    }
    check("and the caller's password buffer is wiped at once", zero);
    spin(&c, WAIT_MS, rep_idle, &m);
    check("no answer is a timeout, not a login", m.repeater.login == RIFT_REP_LOGIN_TIMEOUT &&
                                                     !rift_rep_logged_in(&m.repeater, KEY_B));
    check("and the password reached the service in the request",
          log_has(rlog, "\"password\":\"wrong-one\""));
    check("but is held nowhere in the model or the client",
          !holds_bytes(&m, sizeof(m), "wrong-one") && !holds_bytes(&c, sizeof(c), "wrong-one"));

    snprintf(pw, sizeof(pw), "%s", "hunter2");
    rift_ipc_repeater_login(&c, KEY_B, pw, sizeof(pw));
    spin(&c, WAIT_MS, rep_logged, &m);
    check("the right password is a login, as admin", rift_rep_logged_in(&m.repeater, KEY_B) &&
                                                       m.repeater.admin);
    check("with no copy of it kept", !holds_bytes(&m, sizeof(m), "hunter2") &&
                                         !holds_bytes(&c, sizeof(c), "hunter2"));

    check("status is asked for", rift_ipc_repeater_ask(&c, KEY_B, RIFT_REP_STATUS) == 0);
    check("a second request before it is answered is refused and not written",
          rift_ipc_repeater_ask(&c, KEY_B, RIFT_REP_NEIGHBOURS) == -1);
    spin(&c, WAIT_MS, rep_status, &m);
    check("status is what the repeater answered", m.repeater.status.have &&
                                                      m.repeater.status.battery_mv == 4012 &&
                                                      m.repeater.status.have_dups &&
                                                      !m.repeater.status.have_rx_air);
    check("exactly one request was written", log_count(rlog, "mesh.remote_request|") == 1);

    check("a read-only command is sent", rift_ipc_repeater_cli(&c, KEY_B, "ver") == 0);
    spin(&c, WAIT_MS, rep_transcript, &m);
    check("and its answer is the transcript", m.repeater.line_count == 3 &&
                                                  strcmp(rift_rep_line(&m.repeater, 0), "> ver") == 0);
    check("a command naming a password is never written",
          rift_ipc_repeater_cli(&c, KEY_B, "password hunter3") == -1 &&
              !log_has(rlog, "hunter3"));
    check("nor is erase", rift_ipc_repeater_cli(&c, KEY_B, "erase") == -1 &&
                              !log_has(rlog, "\"command\":\"erase\""));

    check("logout is asked for", rift_ipc_repeater_logout(&c) == 0 && !m.repeater.active);
    round_trip(&c, &m);
    check("and the service was told", log_count(rlog, "mesh.remote_logout|") == 2);

    /* Logged in again, and then the service goes away. */
    snprintf(pw, sizeof(pw), "%s", "hunter2");
    rift_ipc_repeater_login(&c, KEY_B, pw, sizeof(pw));
    spin(&c, WAIT_MS, rep_logged, &m);
    fake_meshcored_stop(pid);
    spin(&c, WAIT_MS, is_down, &m);
    check("a service that went away took the session with it",
          !m.repeater.active && !rift_rep_logged_in(&m.repeater, KEY_B) &&
              !rift_rep_busy(&m.repeater));
    check("the scan list stays, no longer current",
          m.repeater.scan.count == 1 && !m.repeater.scan.found[0].current);
    rift_ipc_close(&c);
    unlink(rlog);

    /* A meshcored from before repeater control. */
    memset(&script, 0, sizeof(script));
    script.state = "online";
    script.nodes_json = NODES_TWO;
    script.no_remote = 1;
    script.life_ms = FAKE_LIFE_MS;
    pid = fake_meshcored_spawn(&script);
    check("an older service is running", pid > 0 && fake_meshcored_wait_ready(WAIT_MS));
    rift_model_init(&m);
    rift_ipc_init(&c, &m, RIFT_SERVICE);
    spin(&c, WAIT_MS, opening_answered, &m);
    round_trip(&c, &m);
    check("its unknown methods say the feature is not there, not that it failed",
          m.repeater.scan.unsupported && m.repeater.unsupported);
    check("and SCAN is then not written at all", rift_ipc_scan_repeaters(&c) == -1);
    rift_ipc_close(&c);
    fake_meshcored_stop(pid);
}

#define SITE_CHANNEL \
    "[{\"channel\":0,\"name\":\"SITE\",\"channel_hash\":\"8c\",\"key_bits\":128," \
    "\"text_limit\":147,\"ack_expected\":false}]"

/* Managing the node: a channel joined and left, a rename, a path hash size.
 * None transmits; each is asked for once, on request, and the client keeps
 * no key it was handed. */
static void test_management(const char *runtime)
{
    static struct rift_model m;
    static struct rift_ipc c;
    struct fake_meshcored_script script;
    char manage[600];
    char key[RIFT_KEY_B64_MAX];
    char line[512];
    int lines = 0;
    FILE *f;
    pid_t pid;

    snprintf(manage, sizeof(manage), "%s/manage", runtime);
    unlink(manage);
    memset(&script, 0, sizeof(script));
    script.state = "online";
    script.nodes_json = NODES_TWO;
    script.channels_json = SITE_CHANNEL;
    script.manage_log = manage;
    script.life_ms = FAKE_LIFE_MS;
    pid = fake_meshcored_spawn(&script);
    check("a service that can be managed is running", pid > 0 && fake_meshcored_wait_ready(WAIT_MS));
    rift_model_init(&m);
    rift_ipc_init(&c, &m, RIFT_SERVICE);
    spin(&c, WAIT_MS, opening_answered, &m);
    check("the service answered a later request too", round_trip(&c, &m));
    check("the path hash size is read on connecting",
          m.have_path_hash && m.path_hash_bytes == 1 && !m.path_hash_unsupported);
    check("and where the name came from", m.self_name_source == RIFT_NAME_SOURCE_STORED &&
                                              m.self_name_max == 31);
    f = fopen(manage, "r");
    check("connecting changed nothing", f == NULL);
    if (f) {
        fclose(f);
    }

    check("a hashtag key is derived", rift_hashtag_key("#test", key, sizeof(key)) == 0);
    check("a channel is joined on request", rift_ipc_channel_add(&c, "#test", key) == 0);
    check("one change at a time: a second is refused and not written",
          rift_ipc_set_name(&c, "Other") == -1);
    spin(&c, WAIT_MS, manage_settled, &m);
    check("the answer settles it", m.manage_op.done && m.manage_op.kind == RIFT_ACTION_CHANNEL_ADD);
    text_is("under the name it was asked for", m.manage_op.label, "#test");
    check("and the channel is listed", round_trip(&c, &m) && m.channel_count == 2 &&
                                           rift_model_channel(&m, 1) &&
                                           strcmp(rift_model_channel(&m, 1)->name, "#test") == 0);
    check("the model holds no key", !holds_bytes(&m, sizeof(m), key));
    check("nor does the client", !holds_bytes(&c, sizeof(c), key));

    rift_ipc_channel_add(&c, "#again", key);
    spin(&c, WAIT_MS, manage_settled, &m);
    check("the same key twice is refused in the service's words",
          m.manage_op.failed && strstr(m.manage_op.error, "already a channel") != NULL);

    check("a channel is left on request", rift_ipc_channel_remove(&c, 1, "#test") == 0);
    spin(&c, WAIT_MS, manage_settled, &m);
    check("which settles it", m.manage_op.done && m.manage_op.kind == RIFT_ACTION_CHANNEL_REMOVE &&
                                  m.manage_op.value == 1);
    check("and only that channel is gone", round_trip(&c, &m) && m.channel_count == 1 &&
                                               rift_model_channel(&m, 0) &&
                                               !rift_model_channel(&m, 1));

    check("the node is renamed on request", rift_ipc_set_name(&c, "K230-\xC3\x98st") == 0);
    spin(&c, WAIT_MS, manage_settled, &m);
    check("and the identity is the service's answer",
          m.manage_op.done && strcmp(m.self_name, "K230-\xC3\x98st") == 0);

    check("2-byte path hashes are asked for", rift_ipc_set_path_hash(&c, 2) == 0);
    spin(&c, WAIT_MS, manage_settled, &m);
    check("and the size is the service's answer", m.manage_op.done && m.path_hash_bytes == 2);
    rift_ipc_set_path_hash(&c, 4);
    spin(&c, WAIT_MS, manage_settled, &m);
    check("a size the service will not take is refused, and the size stays",
          m.manage_op.failed && m.path_hash_bytes == 2);
    rift_ipc_close(&c);
    fake_meshcored_stop(pid);

    f = fopen(manage, "r");
    while (f && fgets(line, sizeof(line), f)) {
        lines++;
        if (lines == 1) {
            line[strcspn(line, "\n")] = '\0';
            text_is("the join carried the name and the derived key, and nothing else", line,
                    "mesh.channel_add|{\"name\":\"#test\",\"key\":\"nNj88ipHMztZHZaiuEi3Pw==\"}");
        }
    }
    if (f) {
        fclose(f);
    }
    check("six changes were asked for, each once: no second, nothing on its own", lines == 6);

    /* A name set by the operator's configuration. */
    memset(&script, 0, sizeof(script));
    script.state = "online";
    script.nodes_json = NODES_TWO;
    script.name_pinned = 1;
    script.no_path_hash = 1;
    script.life_ms = FAKE_LIFE_MS;
    pid = fake_meshcored_spawn(&script);
    check("a service with a pinned name and no path hash setting is running",
          pid > 0 && fake_meshcored_wait_ready(WAIT_MS));
    rift_model_init(&m);
    rift_ipc_init(&c, &m, RIFT_SERVICE);
    spin(&c, WAIT_MS, opening_answered, &m);
    check("the service answered a later request too", round_trip(&c, &m));
    check("a pinned name says so", m.self_name_source == RIFT_NAME_SOURCE_CONFIG);
    check("a service with no path hash setting is told apart from one not yet asked",
          m.path_hash_unsupported && !m.have_path_hash);
    check("and that is not a malformed event or a lost connection",
          rift_ipc_connected(&c) && m.events_malformed == 0);
    /* A configured name is renamed like any other, and saved. */
    rift_ipc_set_name(&c, "Other");
    spin(&c, WAIT_MS, manage_settled, &m);
    check("a configured name is renamed on request",
          m.manage_op.done && !m.manage_op.failed && strcmp(m.self_name, "Other") == 0);
    check("and the answer says it was saved: nothing is shown as unsaved",
          !m.manage_op.unsaved);
    check("the name is the stored one afterwards",
          m.self_name_source == RIFT_NAME_SOURCE_STORED);
    {
        char caption[RIFT_ACTION_TEXT_MAX];

        rift_fmt_action(&m.manage_op, rift_mono_ms(), caption, sizeof(caption));
        check("the caption is the plain renamed one",
              strstr(caption, "RENAMED") != NULL && strstr(caption, "NOT SAVED") == NULL);
    }
    /* The Public channel is known by the service's word about its key. */
    check("the well-known Public key is joined under a local name",
          rift_ipc_channel_add(&c, "torget", "izOH6cXN6mrJ5e26oRXNcg==") == 0);
    spin(&c, WAIT_MS, manage_settled, &m);
    check("and a channel merely called Public, with another key",
          rift_ipc_channel_add(&c, "Public", "AAECAwQFBgcICQoLDA0ODw==") == 0);
    spin(&c, WAIT_MS, manage_settled, &m);
    check("the list is read again", round_trip(&c, &m) && m.channel_count == 2);
    {
        int publics = 0;
        int i;

        for (i = 0; i < m.channel_count; i++) {
            if (m.channels[i].is_public) {
                publics++;
                check("the Public channel is the one holding the well-known key",
                      strcmp(m.channels[i].name, "torget") == 0);
            }
        }
        check("exactly one channel is the Public one, and not by its name", publics == 1);
    }
    rift_ipc_close(&c);
    fake_meshcored_stop(pid);

    /* Renamed for this run, and not written. */
    memset(&script, 0, sizeof(script));
    script.state = "online";
    script.nodes_json = NODES_TWO;
    script.rename_unsaved = 1;
    script.life_ms = FAKE_LIFE_MS;
    pid = fake_meshcored_spawn(&script);
    check("a service that cannot save a name is running",
          pid > 0 && fake_meshcored_wait_ready(WAIT_MS));
    rift_model_init(&m);
    rift_ipc_init(&c, &m, RIFT_SERVICE);
    spin(&c, WAIT_MS, opening_answered, &m);
    rift_ipc_set_name(&c, "Fleeting");
    spin(&c, WAIT_MS, manage_settled, &m);
    check("a rename it took but did not save is in use",
          m.manage_op.done && strcmp(m.self_name, "Fleeting") == 0);
    check("and is recorded as unsaved", m.manage_op.unsaved);
    {
        char caption[RIFT_ACTION_TEXT_MAX];

        rift_fmt_action(&m.manage_op, rift_mono_ms(), caption, sizeof(caption));
        check("the caption says NOT SAVED and that the old name returns",
              strstr(caption, "NOT SAVED") != NULL && strstr(caption, "OLD NAME RETURNS") != NULL);
        check("and does not read as an ordinary rename",
              strstr(caption, "PEERS SEE IT") == NULL);
    }
    script.rename_unsaved = 0;
    rift_ipc_close(&c);
    fake_meshcored_stop(pid);

    /* Taken and never answered. */
    memset(&script, 0, sizeof(script));
    script.state = "online";
    script.nodes_json = NODES_TWO;
    script.manage_silent = 1;
    script.life_ms = FAKE_LIFE_MS;
    pid = fake_meshcored_spawn(&script);
    check("a service that will not answer is running", pid > 0 && fake_meshcored_wait_ready(WAIT_MS));
    rift_model_init(&m);
    rift_ipc_init(&c, &m, RIFT_SERVICE);
    spin(&c, WAIT_MS, opening_answered, &m);
    rift_ipc_channel_remove(&c, 0, "SITE");
    check("the service took the leave and has not answered it",
          round_trip(&c, &m) && m.manage_op.active);
    fake_meshcored_stop(pid);
    spin(&c, WAIT_MS, is_down, &m);
    check("a leave nobody answered is not called done, and says it may have happened",
          !m.manage_op.done && m.manage_op.failed && m.manage_op.unknown &&
              strstr(m.manage_op.error, "may or may not") != NULL);
    rift_ipc_close(&c);

    rift_model_init(&m);
    rift_ipc_init(&c, &m, "meshcored-that-is-not-there");
    check("a join with no connection is refused", rift_ipc_channel_add(&c, "#x", key) == -1);
    check("and says nothing was changed",
          m.manage_op.failed && strstr(m.manage_op.error, "nothing was changed") != NULL);
    rift_ipc_close(&c);
}

static int64_t now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Poll the client the way the app's timer does, until `done` or the budget
 * runs out. Returns the milliseconds spent. */
static int spin(struct rift_ipc *c, int budget_ms, int (*done)(const struct rift_ipc *,
                                                               const struct rift_model *),
                const struct rift_model *m)
{
    int64_t start = now_ms();

    for (;;) {
        int64_t now = now_ms();

        rift_ipc_poll(c, now);
        if (done && done(c, m)) {
            return (int)(now_ms() - start);
        }
        if (now - start >= budget_ms) {
            return (int)(now - start);
        }
        usleep(2000);
    }
}

static int have_snapshot(const struct rift_ipc *c, const struct rift_model *m)
{
    (void)c;
    return m->snapshot_valid && m->have_identity && m->have_info && m->have_status;
}

/* Is a request of this kind still waiting for its answer? */
static int outstanding(const struct rift_ipc *c, enum rift_req what)
{
    int i;

    for (i = 0; i < RIFT_MAX_PENDING; i++) {
        if (c->pending[i].what == what) {
            return 1;
        }
    }
    return 0;
}

/* Connected, and everything the client asked for on connecting has been
 * answered. This, not have_snapshot, is what a check about the channel list
 * or the message history waits for: both are asked for after the node list,
 * so a wait that ends on the node list can end before either has arrived. */
static int opening_answered(const struct rift_ipc *c, const struct rift_model *m)
{
    (void)m;
    return c->fd >= 0 && !outstanding(c, RIFT_REQ_SUBSCRIBE) && !outstanding(c, RIFT_REQ_INFO) &&
           !outstanding(c, RIFT_REQ_IDENTITY) && !outstanding(c, RIFT_REQ_STATUS) &&
           !outstanding(c, RIFT_REQ_NODES) && !outstanding(c, RIFT_REQ_CHANNELS) &&
           !outstanding(c, RIFT_REQ_MESSAGES);
}

static int have_events(const struct rift_ipc *c, const struct rift_model *m)
{
    (void)m;
    return c->events_in >= 3;
}

static int have_one_event(const struct rift_ipc *c, const struct rift_model *m)
{
    (void)m;
    return c->events_in >= 1;
}

/* The five scripted events of "a service that answers", and the junk frame
 * the service sends after them. */
static int have_all_events(const struct rift_ipc *c, const struct rift_model *m)
{
    (void)m;
    return c->events_in >= 5 && c->bad_frames >= 1;
}

/* What the next wait on events_reached waits for: how many events the client
 * has read, in all. Set just before the wait. */
static unsigned events_target;

static int events_reached(const struct rift_ipc *c, const struct rift_model *m)
{
    (void)m;
    return c->events_in >= events_target;
}

static int is_down(const struct rift_ipc *c, const struct rift_model *m)
{
    (void)m;
    return c->fd < 0;
}

/* A second connection, and everything asked for on it answered. Not the
 * model's validity flags: rift_ipc_close leaves messages_valid standing from
 * the first connection, so a wait on it could end before the second
 * connection's history had been read. */
static int is_up_again(const struct rift_ipc *c, const struct rift_model *m)
{
    return c->connects >= 2 && opening_answered(c, m);
}

static int node_answered(const struct rift_ipc *c, const struct rift_model *m)
{
    (void)m;
    return c->fd >= 0 && !outstanding(c, RIFT_REQ_NODE);
}

static int nodes_answered(const struct rift_ipc *c, const struct rift_model *m)
{
    (void)m;
    return c->fd >= 0 && !outstanding(c, RIFT_REQ_NODES);
}

static int send_answered(const struct rift_ipc *c, const struct rift_model *m)
{
    (void)m;
    return c->fd >= 0 && !outstanding(c, RIFT_REQ_SEND);
}

/* The reply to a send and the mesh.message the service raises beside it.
 * Both, because the two may arrive in either order and a count of messages
 * taken after only one of them cannot tell one message from two. */
static int send_reported(const struct rift_ipc *c, const struct rift_model *m)
{
    return send_answered(c, m) && events_reached(c, m);
}

/* A round trip: the node list, asked for and answered. For a check that
 * something did NOT arrive, or was not done: the service handles requests in
 * order and writes an event raised beside a reply straight after that reply,
 * so once the answer to a later request is in, it has taken every earlier
 * request and anything it had to say about one has been read too. A fixed
 * wait could only say that nothing had arrived yet. Returns 1 when the
 * answer came. */
static int round_trip(struct rift_ipc *c, struct rift_model *m)
{
    if (rift_ipc_request_nodes(c) != 0) {
        return 0;
    }
    spin(c, WAIT_MS, nodes_answered, m);
    return nodes_answered(c, m);
}

/* Is there a message holding this text? A message the service has forgotten
 * and this side has not is what the defect looked like on the panel. */
static int holds_text(const struct rift_model *m, const char *want)
{
    int i;

    for (i = 0; i < m->msg_count; i++) {
        if (strcmp(m->msg[i].text, want) == 0) {
            return 1;
        }
    }
    return 0;
}

int main(void)
{
    char runtime[] = "/tmp/rift_ipc_test.XXXXXX";
    struct rift_model m;
    struct rift_ipc c;
    char methods[600];
    pid_t pid;

    if (!mkdtemp(runtime)) {
        printf("FAIL could not make a runtime directory\n");
        return 1;
    }
    setenv("POCKETOS_RUNTIME_DIR", runtime, 1);
    snprintf(methods, sizeof(methods), "%s/methods.txt", runtime);

    /* ---- no service at all ------------------------------------------------ */
    rift_model_init(&m);
    rift_ipc_init(&c, &m, RIFT_SERVICE);
    check("a client with nowhere to connect starts down", !rift_ipc_connected(&c));
    rift_ipc_poll(&c, now_ms());
    check("and one pass leaves it down", !rift_ipc_connected(&c));
    check("the service is reported absent, not unknown", m.state == RIFT_SVC_ABSENT);
    check("and the app is told why", c.last_error[0] != '\0');
    check("nothing was invented to show", m.node_count == 0 && !m.snapshot_valid);
    {
        /* A poll before the backoff has expired must not hammer the socket:
         * the first attempt failed, so the next one waits. Every failed
         * attempt sets the time of the next one, so an unchanged time is the
         * proof that none was made. (This compared c.connects, which counts
         * only connections that succeeded - 0 before and 0 after, whatever
         * the client did - and so could not fail.) */
        int64_t next = c.next_attempt_ms;
        int64_t t = now_ms();
        int i;

        for (i = 0; i < 50; i++) {
            rift_ipc_poll(&c, t);
        }
        check("polling harder does not retry harder", c.next_attempt_ms == next);
    }
    rift_ipc_close(&c);

    /* ---- no service yet, and then the service appears ---------------------- */
    /* Unit A after a boot with no meshcored: the shell up first and RIFT open
     * on a service that is not there. The client has to keep looking without
     * hammering the socket, find the service by itself when it starts,
     * subscribe once, and apply each event once. */
    {
        static const char *const late_events[] = {
            "mesh.node|{\"reason\":\"path\",\"node\":{\"public_key\":\"" KEY_B "\","
            "\"node_hash\":\"b2\",\"name\":\"HYTTA\",\"type\":2,\"path_known\":true,"
            "\"hops\":5,\"direct\":false,\"path_hex\":\"a1c2d3e4f5\","
            "\"last_heard_mono_ms\":1400}}",
            NULL,
        };
        struct fake_meshcored_script script;
        /* Thirty seconds of polling every 10 ms, on a clock that ends a
         * second ago, so the next attempt it leaves falls due in real time
         * shortly after the service below appears. */
        int64_t base = now_ms() - 31000;
        int64_t last_next = -1;
        unsigned tries = 0;
        int subscribes = 0;
        char line[128];
        FILE *f;
        pid_t pid;
        int k;

        unlink(methods);
        rift_model_init(&m);
        rift_ipc_init(&c, &m, RIFT_SERVICE);
        for (k = 0; k <= 3000; k++) {
            rift_ipc_poll(&c, base + (int64_t)k * 10);
            if (c.next_attempt_ms != last_next) {
                tries++;
                last_next = c.next_attempt_ms;
            }
        }
        /* 0.5, 1, 2, 4, then 5 s apart: nine tries in thirty seconds. */
        check("thirty seconds with no service is a handful of tries, not a storm",
              tries >= 3 && tries <= 10);
        check("the interval stops growing at its ceiling", c.backoff_ms == RIFT_BACKOFF_MAX_MS);
        check("and the service is reported absent throughout",
              m.state == RIFT_SVC_ABSENT && !rift_ipc_connected(&c) && c.connects == 0);

        memset(&script, 0, sizeof(script));
        script.state = "online";
        script.nodes_json = NODES_TWO;
        script.events = late_events;
        script.life_ms = FAKE_LIFE_MS;
        script.method_log = methods;
        pid = fake_meshcored_spawn(&script);
        check("the service comes up after the client", fake_meshcored_wait_ready(WAIT_MS));
        /* The next attempt falls due up to the backoff ceiling (5 s) after
         * the service appears, and one refused while it is still starting
         * waits that long again: WAIT_MS covers both. */
        spin(&c, WAIT_MS, have_snapshot, &m);
        check("the client finds it on its own", rift_ipc_connected(&c) && have_snapshot(&c, &m));
        check("with one connection, not several", c.connects == 1);
        check("and says so: absent no longer", m.state != RIFT_SVC_ABSENT && !m.stale);
        spin(&c, WAIT_MS, have_one_event, &m);
        /* Everything the service wrote before it answered a later request
         * has arrived once that answer has, a second copy of the event
         * included. */
        check("the service answered a later request too", round_trip(&c, &m));
        check("the one event it raised arrives exactly once", c.events_in == 1);
        /* Applied once and filed once. Not "hops == 5": the fake's node
         * snapshot is fixed text, so whether it is read before or after the
         * event decides the hop count - which a sanitised run's timing
         * reordered - while a real service's snapshot already says what its
         * event said. The counter and the row count do not depend on that. */
        check("and it is applied exactly once, to the row it names, with no second row",
              m.events_applied == 1 && m.events_malformed == 0 &&
                  rift_model_find(&m, KEY_B) != NULL && m.node_count == 2);
        f = fopen(methods, "r");
        while (f && fgets(line, sizeof(line), f)) {
            line[strcspn(line, "\n")] = '\0';
            subscribes += strcmp(line, "mesh.subscribe") == 0;
        }
        if (f) {
            fclose(f);
        }
        check("it subscribed exactly once", subscribes == 1);
        rift_ipc_close(&c);
        fake_meshcored_stop(pid);
        unlink(methods);
    }

    /* ---- a service that answers -------------------------------------------- */
    {
        static const char *const events[] = {
            "mesh.state|{\"state\":\"online\",\"reason\":\"receiving\",\"mono_ms\":1200}",
            "mesh.activity|{\"kind\":\"rx\",\"payload_type\":\"advert\",\"bytes\":48,"
            "\"mono_ms\":1300,\"rssi_dbm\":-88.0,\"snr_db\":6.0}",
            /* The same node again, with a longer path: an update, not a
             * second row. */
            "mesh.node|{\"reason\":\"path\",\"node\":{\"public_key\":\"" KEY_B "\","
            "\"node_hash\":\"b2\",\"name\":\"HYTTA\",\"type\":2,\"path_known\":true,"
            "\"hops\":5,\"direct\":false,\"path_hex\":\"a1c2d3e4f5\","
            "\"last_heard_mono_ms\":1400}}",
            /* Rubbish, in the two shapes a subscriber actually sees. */
            "mesh.node|{\"reason\":\"discovered\",\"node\":{\"name\":\"no key\"}}",
            "mesh.nonsense|{\"whatever\":1}",
            NULL
        };
        struct fake_meshcored_script script;

        memset(&script, 0, sizeof(script));
        script.state = "online";
        script.reason = "receiving";
        script.nodes_json = NODES_TWO;
        script.events = events;
        script.junk_frame = "{\"hello\":\"this is not a message\"}";
        script.life_ms = FAKE_LIFE_MS;
        script.method_log = methods;

        pid = fake_meshcored_spawn(&script);
        check("the fake service came up", fake_meshcored_wait_ready(WAIT_MS));

        rift_model_init(&m);
        rift_ipc_init(&c, &m, RIFT_SERVICE);
        spin(&c, WAIT_MS, have_snapshot, &m);
        check("the client connects", rift_ipc_connected(&c));
        check("and reads the whole opening set", have_snapshot(&c, &m));
        check("the node list is the service's", m.node_count == 2);
        text_is("with this device's name", m.self_name, "K230-A");
        text_is("and the service's protocol", m.protocol, "meshcore");
        check("nothing is stale while it is answering", !m.stale);

        spin(&c, WAIT_MS, have_events, &m);
        check("events arrive", c.events_in >= 3);
        check("a state event is applied", m.state == RIFT_SVC_ONLINE);
        check("an activity event is kept", m.activity_total >= 1);
        /* Applied once, to the node it names. Not "hops == 5": these events
         * start as soon as the client subscribes, so they interleave with the
         * answers to what it asks next, and the node snapshot is fixed text -
         * whether it or the event lands last decides the hop count, and a
         * sanitised run's timing reorders them. A real service's snapshot
         * already says what its event said. The first three events are the
         * good ones, so all three applied and none twice is a count the order
         * cannot change. What an event does to a row is
         * tests/rift_model_test.c's to prove; one landing on top of a
         * snapshot over a socket is the channels section's. */
        check("a node event is applied to the node it names",
              m.events_applied == 3 && rift_model_find(&m, KEY_B) != NULL);
        check("and does not add a row", m.node_count == 2);

        /* The two bad events and the junk frame, which come last. */
        spin(&c, WAIT_MS, have_all_events, &m);
        check("a malformed event is refused", m.events_malformed >= 2);
        check("and refusing it did not drop the connection", rift_ipc_connected(&c));
        check("a frame that is neither event nor reply is counted", c.bad_frames >= 1);
        check("and that did not drop the connection either", rift_ipc_connected(&c));
        check("the connection has not been remade behind our back", c.connects == 1);

        /* One node, asked for by name. */
        check("a single node can be asked for", rift_ipc_request_node(&c, KEY_A) == 0);
        /* A channel is not a node. Asking meshcored about one would be a
         * question it refuses - it is not a hex key - and the refusal would
         * sit in the client's last error, which is what the command line
         * shows once the service goes away. Refused before it is written. */
        {
            char chan[RIFT_KEY_HEX];

            rift_channel_key(0, "8c", "SITE", chan, sizeof(chan));
            check("but a channel is not a node and is never asked about",
                  rift_ipc_request_node(&c, chan) == -1);
        }
        check("nor one that cannot be identified", rift_ipc_request_node(&c, "#7:?") == -1);
        check("nor the bare slot older builds used", rift_ipc_request_node(&c, "#0") == -1);
        spin(&c, WAIT_MS, node_answered, &m);
        check("and the answer updates the row it belongs to",
              node_answered(&c, &m) && m.node_count == 2);

        /* A prefix that matches nothing is refused by the service, and the
         * refusal is reported rather than swallowed. */
        {
            unsigned errors = c.errors_in;

            rift_ipc_request_node(&c, "ffff");
            spin(&c, WAIT_MS, node_answered, &m);
            check("a refusal from the service is counted", c.errors_in == errors + 1);
            check("and said in words", strstr(c.last_error, "mesh.node") != NULL);
            check("without dropping the connection", rift_ipc_connected(&c));
        }

        check("every request outstanding has been answered",
              c.replies_in >= c.requests_out - 1);
        check("and nothing was refused for want of room", c.requests_refused == 0);
        rift_ipc_close(&c);
        fake_meshcored_stop(pid);
    }

    /* ---- the service goes away under a live client -------------------------- */
    {
        struct fake_meshcored_script script;

        memset(&script, 0, sizeof(script));
        script.state = "online";
        script.nodes_json = NODES_TWO;
        /* Short-lived because it is stopped below, once the client has read
         * it. A life_ms of its own could end before a slow client had. */
        script.life_ms = FAKE_LIFE_MS;
        script.method_log = methods;

        pid = fake_meshcored_spawn(&script);
        check("a short-lived service came up", fake_meshcored_wait_ready(WAIT_MS));
        rift_model_init(&m);
        rift_ipc_init(&c, &m, RIFT_SERVICE);
        spin(&c, WAIT_MS, have_snapshot, &m);
        check("the client read its snapshot", m.node_count == 2);

        fake_meshcored_stop(pid);
        spin(&c, WAIT_MS, is_down, &m);
        check("the client notices the service has gone", !rift_ipc_connected(&c));
        check("and says so", m.state == RIFT_SVC_ABSENT);
        /* The nodes stay, marked as what they are. An empty list would say
         * something less true than a stale one. */
        check("what it knew is still shown", m.node_count == 2);
        check("marked cached", m.stale);
        check("and the snapshot is no longer valid", !m.snapshot_valid);
        check("the backoff has grown past its floor", c.backoff_ms > RIFT_BACKOFF_MIN_MS);

        /* ---- and comes back -------------------------------------------------- */
        {
            struct fake_meshcored_script again;

            memset(&again, 0, sizeof(again));
            again.state = "degraded";
            again.reason = "the radio is not receiving";
            /* One node fewer: the snapshot replaces, so the one the service
             * has forgotten must go. */
            again.nodes_json = "[{\"public_key\":\"" KEY_A "\",\"node_hash\":\"a1\","
                               "\"name\":\"OSLO-01\",\"path_known\":false,"
                               "\"last_heard_mono_ms\":2000}]";
            again.life_ms = FAKE_LIFE_MS;
            again.method_log = methods;
            pid = fake_meshcored_spawn(&again);
            check("the service comes back", fake_meshcored_wait_ready(WAIT_MS));
            spin(&c, WAIT_MS, is_up_again, &m);
            check("the client reconnects on its own", c.connects >= 2);
            check("re-reads the snapshot", m.snapshot_valid);
            check("which replaces what it had", m.node_count == 1);
            check("so a node the service has forgotten is gone", rift_model_find(&m, KEY_B) ==
                                                                     NULL);
            check("nothing is cached any more", !m.stale);
            check("and the new state is the one it is in now", m.state == RIFT_SVC_DEGRADED);
            check("the backoff went back to its floor", c.backoff_ms == RIFT_BACKOFF_MIN_MS);
            rift_ipc_close(&c);
            fake_meshcored_stop(pid);
        }
    }

    /* ---- the service is restarted under a live client ------------------------ */
    /* The one part D of the channels gate on unit A found, 2026-09-21:
     * meshcored hands out message ids from 1 on every run and keeps no
     * messages across one, so a client that merged a snapshot by id ended up
     * showing a history blended from two sessions - four messages on the
     * panel against the three the service held.
     *
     * Two whole processes, a real socket and a real reconnect, because that
     * is the only way the ordering is real: the status that says the run
     * changed and the snapshot that refills the cache arrive on the same
     * connection, in that order, with events in between.
     *
     * It has nothing to do with channels. This is a direct-only client and a
     * direct-only service. */
    {
        struct fake_meshcored_script first;
        struct fake_meshcored_script second;

        memset(&first, 0, sizeof(first));
        first.state = "online";
        first.uptime_s = 3600;
        first.nodes_json = NODES_TWO;
        first.messages_json =
            "[{\"id\":1,\"direction\":\"in\",\"peer_public_key\":\"" KEY_B "\","
            "\"peer_name\":\"HYTTA\",\"text\":\"before one\",\"state\":\"received\","
            "\"mono_ms\":-9000},"
            "{\"id\":2,\"direction\":\"in\",\"peer_public_key\":\"" KEY_B "\","
            "\"peer_name\":\"HYTTA\",\"text\":\"before two\",\"state\":\"received\","
            "\"mono_ms\":-8000},"
            "{\"id\":3,\"direction\":\"in\",\"peer_public_key\":\"" KEY_A "\","
            "\"peer_name\":\"OSLO-01\",\"text\":\"before three\",\"state\":\"received\","
            "\"mono_ms\":-7000}]";
        first.life_ms = FAKE_LIFE_MS;
        first.method_log = methods;

        pid = fake_meshcored_spawn(&first);
        check("a service that has been up an hour is running",
              pid > 0 && fake_meshcored_wait_ready(WAIT_MS));
        rift_model_init(&m);
        rift_ipc_init(&c, &m, RIFT_SERVICE);
        spin(&c, WAIT_MS, opening_answered, &m);
        check("its history is read", m.messages_valid && m.msg_count == 3);
        check("and it is the run the client thinks it is on", m.svc_restarts == 0);

        fake_meshcored_stop(pid);
        spin(&c, WAIT_MS, is_down, &m);
        check("the client notices it has gone", !rift_ipc_connected(&c));
        check("and keeps what it was shown, marked cached", m.msg_count == 3 && m.stale);

        /* The replacement: a new process, seconds old, with an id space that
         * has started again. Two messages, under ids 1 and 2, which are not
         * the ids 1 and 2 the client is holding - one of them was even in a
         * different conversation. */
        memset(&second, 0, sizeof(second));
        second.state = "online";
        second.uptime_s = 2;
        second.nodes_json = NODES_TWO;
        second.messages_json =
            "[{\"id\":1,\"direction\":\"in\",\"peer_public_key\":\"" KEY_A "\","
            "\"peer_name\":\"OSLO-01\",\"text\":\"after one\",\"state\":\"received\","
            "\"mono_ms\":-2000},"
            "{\"id\":2,\"direction\":\"in\",\"peer_public_key\":\"" KEY_A "\","
            "\"peer_name\":\"OSLO-01\",\"text\":\"after two\",\"state\":\"received\","
            "\"mono_ms\":-1000}]";
        second.life_ms = FAKE_LIFE_MS;
        second.method_log = methods;
        pid = fake_meshcored_spawn(&second);
        check("a service that has just come up takes its place",
              pid > 0 && fake_meshcored_wait_ready(WAIT_MS));
        spin(&c, WAIT_MS, is_up_again, &m);
        check("the client reconnects and reads again", c.connects >= 2 && m.messages_valid);
        check("the restart is noticed", m.svc_restarts == 1);
        check("and the client holds exactly what the service holds", m.msg_count == 2);
        check("nothing the service has forgotten is still on this side",
              !holds_text(&m, "before one") && !holds_text(&m, "before two") &&
                  !holds_text(&m, "before three"));
        check("the new ids are the new messages, not old ones rewritten",
              holds_text(&m, "after one") && holds_text(&m, "after two"));
        check("what was dropped is counted", m.msgs_forgotten == 3);
        /* The read marks went with the messages. Had they stayed, the marks
         * from the old id space would have covered both new ids and the
         * conversation would have looked read. */
        check("what arrived while the client was away is unread",
              rift_model_unread_total(&m) == 2);

        rift_ipc_close(&c);
        fake_meshcored_stop(pid);
    }

    /* ---- and a reconnect that is not a restart ------------------------------- */
    /* The socket can go without the process behind it going. This cache of 96
     * can hold more of one kind than either of meshcored's two rings of 64,
     * so a client that emptied on every reconnect would throw away messages
     * the service can no longer supply. Only a changed run empties it. */
    {
        struct fake_meshcored_script script;

        memset(&script, 0, sizeof(script));
        script.state = "online";
        script.uptime_s = 4000;
        script.nodes_json = NODES_TWO;
        script.messages_json =
            "[{\"id\":1,\"direction\":\"in\",\"peer_public_key\":\"" KEY_B "\","
            "\"peer_name\":\"HYTTA\",\"text\":\"still here\",\"state\":\"received\","
            "\"mono_ms\":-3000}]";
        /* Two clients, so the service outlives the first one going. */
        script.serve_clients = 2;
        script.life_ms = FAKE_LIFE_MS;
        script.method_log = methods;
        pid = fake_meshcored_spawn(&script);
        check("a long-lived service is running",
              pid > 0 && fake_meshcored_wait_ready(WAIT_MS));

        rift_model_init(&m);
        rift_ipc_init(&c, &m, RIFT_SERVICE);
        spin(&c, WAIT_MS, opening_answered, &m);
        check("the history is read", m.msg_count == 1);

        /* The client's socket goes; the service does not. */
        rift_ipc_close(&c);
        spin(&c, WAIT_MS, is_up_again, &m);
        check("the client comes back to the same run", c.connects >= 2 && m.svc_restarts == 0);
        check("so nothing was forgotten", m.msgs_forgotten == 0);
        check("and what it was holding is still held",
              m.msg_count == 1 && holds_text(&m, "still here"));

        rift_ipc_close(&c);
        fake_meshcored_stop(pid);
    }

    /* ---- a service that refuses ---------------------------------------------- */
    {
        struct fake_meshcored_script script;

        memset(&script, 0, sizeof(script));
        script.state = "error";
        script.reason = "radiod refused the profile";
        script.refuse_nodes = 1;
        script.life_ms = FAKE_LIFE_MS;
        script.method_log = methods;
        pid = fake_meshcored_spawn(&script);
        check("a refusing service came up", fake_meshcored_wait_ready(WAIT_MS));
        rift_model_init(&m);
        rift_ipc_init(&c, &m, RIFT_SERVICE);
        spin(&c, WAIT_MS, opening_answered, &m);
        check("the client stays connected to a service that refuses one method",
              rift_ipc_connected(&c));
        check("the refusal is counted", c.errors_in >= 1);
        check("no node list is claimed", !m.snapshot_valid && m.node_count == 0);
        check("but the state it is in is still read", m.state == RIFT_SVC_ERROR);
        text_is("with the reason", m.reason, "radiod refused the profile");
        rift_ipc_close(&c);
        fake_meshcored_stop(pid);
    }

    /* ---- open, close, open again --------------------------------------------- */
    {
        struct fake_meshcored_script script;
        int round;

        memset(&script, 0, sizeof(script));
        script.state = "online";
        script.nodes_json = NODES_TWO;
        script.life_ms = FAKE_LIFE_MS;
        script.method_log = methods;
        pid = fake_meshcored_spawn(&script);
        check("a service for the restart rounds came up", fake_meshcored_wait_ready(WAIT_MS));
        for (round = 0; round < 3; round++) {
            rift_model_init(&m);
            rift_ipc_init(&c, &m, RIFT_SERVICE);
            spin(&c, WAIT_MS, have_snapshot, &m);
            check("each time the app opens it reads the service fresh", m.node_count == 2);
            /* Closing gives the subscription back and closes the socket; a
             * second close must be safe, because a failed create reaches
             * the same path. */
            rift_ipc_close(&c);
            rift_ipc_close(&c);
            check("and closing leaves nothing open", c.fd < 0);
        }
        fake_meshcored_stop(pid);
    }

    /* ---- the negative that matters -------------------------------------------- */
    /* Not proved by reading the client's source, but by what the service
     * was asked for over the whole of this run. Opening RIFT must not put a
     * packet on the air. */
    {
        char line[128];
        int saw_send = 0;
        int saw_advert = 0;
        int saw_repeater_tx = 0;
        int saw_unexpected = 0;
        int saw_subscribe = 0;
        int saw_unsubscribe = 0;
        int lines = 0;
        FILE *f = fopen(methods, "r");

        check("the service recorded what it was asked for", f != NULL);
        while (f && fgets(line, sizeof(line), f)) {
            line[strcspn(line, "\n")] = '\0';
            lines++;
            if (strcmp(line, "mesh.send") == 0) {
                saw_send = 1;
            } else if (strcmp(line, "mesh.advert") == 0) {
                saw_advert = 1;
            } else if (strcmp(line, "mesh.subscribe") == 0) {
                saw_subscribe = 1;
            } else if (strcmp(line, "mesh.unsubscribe") == 0) {
                saw_unsubscribe = 1;
            } else if (strcmp(line, "mesh.discover") == 0 ||
                       strcmp(line, "mesh.remote_login") == 0 ||
                       strcmp(line, "mesh.remote_request") == 0 ||
                       strcmp(line, "mesh.remote_cli") == 0) {
                saw_repeater_tx = 1;
            } else if (strcmp(line, "mesh.info") != 0 && strcmp(line, "mesh.status") != 0 &&
                       strcmp(line, "mesh.identity") != 0 && strcmp(line, "mesh.nodes") != 0 &&
                       strcmp(line, "mesh.node") != 0 && strcmp(line, "mesh.messages") != 0 &&
                       strcmp(line, "mesh.channels") != 0 && strcmp(line, "mesh.path_hash") != 0 &&
                       /* Repeater control's two connect-time requests: ending a
                        * session an earlier RIFT left, and reading the scan
                        * list. Neither transmits. */
                       strcmp(line, "mesh.remote_logout") != 0 &&
                       strcmp(line, "mesh.discovered") != 0) {
                saw_unexpected = 1;
                printf("     unexpected method: %s\n", line);
            }
        }
        if (f) {
            fclose(f);
        }
        check("it was asked for something", lines > 0);
        /* Everything above this point is what the app does on its own:
         * connect, read a snapshot, subscribe, lose the service, reconnect.
         * None of it may transmit. Phase 2 can send, but only when a reader
         * asks it to, which is the section after this one. */
        check("nothing the app does on its own asks the service to send", !saw_send);
        check("or to advert", !saw_advert);
        check("or to scan for repeaters, log in to one, or ask or command one", !saw_repeater_tx);
        check("only the methods this phase consumes were used", !saw_unexpected);
        check("the subscription was taken", saw_subscribe);
        check("and given back rather than merely dropped", saw_unsubscribe);
        unlink(methods);
    }


    /* ---- sending, against a real socket ---------------------------------- */
    /* Everything above proved the app does not transmit on its own. This
     * proves that when it does, the message goes out once, its outcome comes
     * from the service, and a refusal reaches the reader. */
    {
        char sends[600];
        struct fake_meshcored_script script;
        pid_t pid;

        snprintf(sends, sizeof(sends), "%s/sends", runtime);
        unlink(sends);
        unlink(methods);
        memset(&script, 0, sizeof(script));
        script.state = "online";
        script.nodes_json = NODES_TWO;
        script.messages_json =
            "[{\"id\":1,\"direction\":\"in\",\"peer_public_key\":\"" KEY_B "\","
            "\"peer_name\":\"HYTTA\",\"text\":\"er du der?\",\"state\":\"received\","
            "\"mono_ms\":-4000}]";
        script.send_log = sends;
        script.method_log = methods;
        script.life_ms = FAKE_LIFE_MS;
        pid = fake_meshcored_spawn(&script);
        check("a service that takes messages is running", pid > 0);
        check("and its socket is there", fake_meshcored_wait_ready(WAIT_MS));

        rift_model_init(&m);
        rift_ipc_init(&c, &m, "meshcored");
        spin(&c, WAIT_MS, opening_answered, &m);
        check("the history was read with the snapshot", m.messages_valid && m.msg_count == 1);
        check("and the service said it does not keep it", !m.messages_persistent);
        check("nothing from that first snapshot is unread", rift_model_unread_total(&m) == 0);

        events_target = c.events_in + 1;
        check("a message is sent", rift_ipc_send_message(&c, KEY_B, "kommer nå") == 0);
        /* The reply and the event may arrive in either order; either way the
         * message must exist exactly once, under the service's id. */
        spin(&c, WAIT_MS, send_reported, &m);
        check("the submission is finished", !rift_model_sending(&m));
        check("the service gave it an id", m.outbox.message_id > 0);
        check("and exactly one message was added", m.msg_count == 2);
        {
            const struct rift_message *thread[8];
            int n = rift_model_thread(&m, KEY_B, thread, 8, NULL);

            check("the thread holds both", n == 2);
            check("ours is outgoing", n == 2 && thread[1]->dir == RIFT_MSG_OUT);
            check("with the service's state, not ours",
                  n == 2 && thread[1]->state == RIFT_MSG_SENT_FLOOD);
            check("and it is not shown as delivered",
                  n == 2 && thread[1]->state != RIFT_MSG_ACKED);
        }

        /* What went on the air is what the reader typed, once. */
        {
            FILE *f = fopen(sends, "r");
            char line[256];
            int lines = 0;

            while (f && fgets(line, sizeof(line), f)) {
                line[strcspn(line, "\n")] = '\0';
                lines++;
                text_is("the service was asked to send exactly what was typed", line,
                        KEY_B "|kommer nå");
            }
            if (f) {
                fclose(f);
            }
            check("once, not twice", lines == 1);
        }

        rift_ipc_close(&c);
        fake_meshcored_stop(pid);
        unlink(sends);
        unlink(methods);
    }

    /* ---- channels, against a real socket ---------------------------------- */
    /* The whole path: the channel list read on connect, a channel message
     * arriving as an event, a message sent to a channel rather than a node,
     * and the list re-read after the service has gone and come back. */
    {
        char sends[600];
        struct fake_meshcored_script script;
        pid_t pid;
        char site[RIFT_KEY_HEX];
        static const char *const chan_events[] = {
            "mesh.message|{\"message\":{\"id\":5,\"direction\":\"in\",\"kind\":\"channel\","
            "\"channel\":0,\"channel_name\":\"SITE\",\"channel_hash\":\"8c\","
            "\"sender_name\":\"HYTTA\",\"text\":\"HYTTA: all clear\","
            "\"state\":\"received\",\"ack_expected\":false,\"mono_ms\":-1000}}",
            "mesh.channel|{\"reason\":\"added\",\"channel\":{\"channel\":3,"
            "\"name\":\"LATE\",\"channel_hash\":\"2a\",\"key_bits\":256,"
            "\"text_limit\":147}}",
            NULL,
        };

        snprintf(sends, sizeof(sends), "%s/sends", runtime);
        unlink(sends);
        unlink(methods);
        memset(&script, 0, sizeof(script));
        script.state = "online";
        script.nodes_json = NODES_TWO;
        script.channels_json =
            "[{\"channel\":0,\"name\":\"SITE\",\"channel_hash\":\"8c\",\"key_bits\":256,"
            "\"text_limit\":147,\"ack_expected\":false}]";
        script.events = chan_events;
        /* "Added later" means after the list was read. Raised as soon as the
         * client subscribed, the added channel could land before the list,
         * which is fixed text without it and would replace it. */
        script.events_after_snapshot = 1;
        script.send_log = sends;
        script.method_log = methods;
        script.life_ms = FAKE_LIFE_MS;
        pid = fake_meshcored_spawn(&script);
        check("a service with a channel is running", pid > 0);
        check("and its socket is there", fake_meshcored_wait_ready(WAIT_MS));

        rift_model_init(&m);
        rift_ipc_init(&c, &m, "meshcored");
        spin(&c, WAIT_MS, opening_answered, &m);
        check("the channel list was read on connect", m.channels_valid);
        check("and holds the one the service has", m.channel_count >= 1);
        check("in the slot it named", rift_model_channel(&m, 0) != NULL);
        rift_channel_key(0, "8c", "SITE", site, sizeof(site));
        check("with the limit the service gave, not the API's 160",
              rift_model_text_limit(&m, site) == 147);

        /* The event that arrives on the channel, and the channel added. */
        events_target = 2;
        spin(&c, WAIT_MS, events_reached, &m);
        {
            const struct rift_message *thread[8];
            int n = rift_model_thread(&m, site, thread, 8, NULL);

            check("a channel message arrived as an event", n == 1);
            check("as a channel message", n == 1 && thread[0]->is_channel);
            check("with the sender's claimed name",
                  n == 1 && thread[0]->have_sender_name &&
                      strcmp(thread[0]->sender_name, "HYTTA") == 0);
            check("and nothing that could acknowledge it",
                  n == 1 && !thread[0]->ack_expected);
            /* Deliberately no unread assertion here. Whatever is in the cache
             * when the first mesh.messages reply lands is seeded as read -
             * nothing has been drawn yet, so "unread since you last looked"
             * is not a question that has an answer (rift_model.h,
             * messages_seeded) - and a real service raises this event
             * whenever it likes, so which side of that reply it falls on is
             * not a property of channels. This service holds it until after
             * the reply only so the channel added below lands after the
             * list. tests/rift_comms_test.c settles the unread behaviour
             * deterministically instead. */
            {
                struct rift_conv conv[RIFT_MAX_CONVERSATIONS];

                check("and it is in the conversation list",
                      rift_model_conversations(&m, conv, RIFT_MAX_CONVERSATIONS) >= 1);
            }
        }
        /* A channel added while the client was connected turns up without a
         * snapshot being asked for. */
        check("a channel added later arrives as an event",
              rift_model_channel(&m, 3) != NULL);

        /* Sending to a channel. */
        check("a message is sent to the channel",
              rift_ipc_send_message(&c, site, "pa vei") == 0);
        spin(&c, WAIT_MS, send_answered, &m);
        check("the submission finished", !rift_model_sending(&m));
        check("the service gave it an id", m.outbox.message_id > 0);
        /* What went on the air, and to where: a channel slot, not a node. */
        {
            FILE *f = fopen(sends, "r");
            char line[256];
            int lines = 0;

            while (f && fgets(line, sizeof(line), f)) {
                line[strcspn(line, "\n")] = '\0';
                lines++;
                text_is("the service was asked to send to the channel", line, "#0|pa vei");
            }
            if (f) {
                fclose(f);
            }
            check("once, not twice", lines == 1);
        }
        /* A body too long for the channel is refused before anything is
         * written: the limit is the service's, and it is shorter than a
         * direct message's because this node's name travels inside. */
        {
            char too_long[200];
            FILE *f;
            char line[256];
            int lines = 0;

            memset(too_long, 'x', sizeof(too_long));
            too_long[148] = '\0';
            check("a body over the channel's limit is refused",
                  rift_ipc_send_message(&c, site, too_long) == -1);
            check("and the reader is told why", m.outbox.failed && m.outbox.error[0]);
            f = fopen(sends, "r");
            while (f && fgets(line, sizeof(line), f)) {
                lines++;
            }
            if (f) {
                fclose(f);
            }
            check("nothing more was written to the service", lines == 1);
        }
        /* A conversation key for a different channel in the same slot - the
         * old channel's, after it was left and the slot taken - is refused
         * here too, with nothing written: mesh.send names only the slot, so
         * the service could not tell the difference. */
        {
            char other[RIFT_KEY_HEX];
            FILE *f;
            char line[256];
            int lines = 0;

            rift_channel_key(0, "77", "OLD", other, sizeof(other));
            check("a key for another channel in the slot is refused",
                  rift_ipc_send_message(&c, other, "reply") == -1);
            check("and the reader is told it is not joined",
                  m.outbox.failed && strstr(m.outbox.error, "not joined") != NULL);
            f = fopen(sends, "r");
            while (f && fgets(line, sizeof(line), f)) {
                lines++;
            }
            if (f) {
                fclose(f);
            }
            check("and nothing was written to the service", lines == 1);
        }

        rift_ipc_close(&c);
        fake_meshcored_stop(pid);
        unlink(sends);
        unlink(methods);
    }

    /* ---- a channel list that changes across a reconnect -------------------- */
    {
        struct fake_meshcored_script script;
        pid_t pid;

        memset(&script, 0, sizeof(script));
        script.state = "online";
        script.nodes_json = NODES_TWO;
        script.channels_json =
            "[{\"channel\":0,\"name\":\"SITE\",\"channel_hash\":\"8c\",\"key_bits\":256,"
            "\"text_limit\":147},"
            "{\"channel\":1,\"name\":\"OPS\",\"channel_hash\":\"4d\",\"key_bits\":128,"
            "\"text_limit\":147}]";
        script.serve_clients = 1;
        script.life_ms = FAKE_LIFE_MS;
        pid = fake_meshcored_spawn(&script);
        check("a service with two channels came up", fake_meshcored_wait_ready(WAIT_MS));

        rift_model_init(&m);
        rift_ipc_init(&c, &m, "meshcored");
        spin(&c, WAIT_MS, opening_answered, &m);
        check("both channels were read", m.channel_count == 2);

        fake_meshcored_stop(pid);
        spin(&c, WAIT_MS, is_down, &m);
        check("the service went away", m.state == RIFT_SVC_ABSENT);
        /* The channels stay on screen and stop being current. Blanking them
         * would say something less true than a stale list does. */
        check("the channels are still shown", m.channel_count == 2);
        check("but are no longer current", !m.channels_valid);
        {
            struct fake_meshcored_script again;
            pid_t pid2;

            memset(&again, 0, sizeof(again));
            again.state = "online";
            again.nodes_json = NODES_TWO;
            /* One of them has been left while nobody was watching. */
            again.channels_json =
                "[{\"channel\":1,\"name\":\"OPS\",\"channel_hash\":\"4d\",\"key_bits\":128,"
                "\"text_limit\":147}]";
            again.life_ms = FAKE_LIFE_MS;
            pid2 = fake_meshcored_spawn(&again);
            check("the service comes back", fake_meshcored_wait_ready(WAIT_MS));
            spin(&c, WAIT_MS, is_up_again, &m);
            check("the channel list was re-read", m.channels_valid);
            check("and is now what the service holds", m.channel_count == 1);
            /* The one that was left is gone rather than lingering as
             * somewhere to write that nothing would carry. */
            check("the channel that was left is not offered",
                  rift_model_channel(&m, 0) == NULL);
            check("and the one still held is", rift_model_channel(&m, 1) != NULL);
            rift_ipc_close(&c);
            fake_meshcored_stop(pid2);
        }
    }

    /* ---- the first message of a conversation ------------------------------ */
    /* NODES -> MESSAGE opens a conversation that holds nothing. Sending from
     * there is the case where there is no history to hide a mistake in: the
     * service must be asked exactly once, and exactly one message must
     * exist afterwards - the service's, not one this app made up. */
    {
        char sends[600];
        struct fake_meshcored_script script;
        pid_t pid;

        snprintf(sends, sizeof(sends), "%s/sends-new", runtime);
        unlink(sends);
        memset(&script, 0, sizeof(script));
        script.state = "online";
        script.nodes_json = NODES_TWO;
        script.send_log = sends;
        script.life_ms = FAKE_LIFE_MS;
        pid = fake_meshcored_spawn(&script);
        check("a service with no message history is running",
              pid > 0 && fake_meshcored_wait_ready(WAIT_MS));

        rift_model_init(&m);
        rift_ipc_init(&c, &m, "meshcored");
        spin(&c, WAIT_MS, opening_answered, &m);
        check("and it holds none", m.messages_valid && m.msg_count == 0);
        check("so the peer has no conversation yet",
              rift_model_thread(&m, KEY_A, NULL, 0, NULL) == 0);

        events_target = c.events_in + 1;
        check("the first message is written", rift_ipc_send_message(&c, KEY_A, "first") == 0);
        spin(&c, WAIT_MS, send_reported, &m);
        check("exactly one message exists afterwards", m.msg_count == 1);
        check("and it is the service's, under the service's id", m.msg[0].id > 0);
        check("nothing was invented before the service answered", m.msgs_duplicate == 0);
        {
            FILE *f = fopen(sends, "r");
            char line[256];
            int lines = 0;

            while (f && fgets(line, sizeof(line), f)) {
                line[strcspn(line, "\n")] = '\0';
                lines++;
                text_is("the service was asked to send just that", line, KEY_A "|first");
            }
            if (f) {
                fclose(f);
            }
            check("once", lines == 1);
        }
        rift_ipc_close(&c);
        fake_meshcored_stop(pid);
        unlink(sends);
    }

    /* ---- a service that refuses the send --------------------------------- */
    {
        struct fake_meshcored_script script;
        pid_t pid;

        memset(&script, 0, sizeof(script));
        script.state = "degraded";
        script.nodes_json = NODES_TWO;
        script.refuse_send = 1;
        script.life_ms = FAKE_LIFE_MS;
        pid = fake_meshcored_spawn(&script);
        check("a service that will not send is running", pid > 0);
        check("and is answering", fake_meshcored_wait_ready(WAIT_MS));

        rift_model_init(&m);
        rift_ipc_init(&c, &m, "meshcored");
        spin(&c, WAIT_MS, opening_answered, &m);
        check("the send is written", rift_ipc_send_message(&c, KEY_B, "hallo") == 0);
        spin(&c, WAIT_MS, send_answered, &m);
        check("and comes back refused", m.outbox.failed);
        check("with the service's own words, not ours",
              strstr(m.outbox.error, "the radio is not available") != NULL);
        check("the service answered a later request too", round_trip(&c, &m));
        check("nothing was added to the thread", m.msg_count == 0);
        check("and nothing is left in flight", !rift_model_sending(&m));

        rift_ipc_close(&c);
        fake_meshcored_stop(pid);
    }

    /* ---- a service that accepts and then says nothing --------------------- */
    /* An id handed out and never spoken of again. The app must not decide
     * for itself what became of it. */
    {
        struct fake_meshcored_script script;
        pid_t pid;

        memset(&script, 0, sizeof(script));
        script.state = "online";
        script.nodes_json = NODES_TWO;
        script.send_is_silent = 1;
        script.life_ms = FAKE_LIFE_MS;
        pid = fake_meshcored_spawn(&script);
        check("a silent service is running", pid > 0 && fake_meshcored_wait_ready(WAIT_MS));

        rift_model_init(&m);
        rift_ipc_init(&c, &m, "meshcored");
        spin(&c, WAIT_MS, opening_answered, &m);
        rift_ipc_send_message(&c, KEY_B, "into the quiet");
        spin(&c, WAIT_MS, send_answered, &m);
        check("the submission ended, because the reply came", !rift_model_sending(&m));
        check("it was not a failure", !m.outbox.failed);
        check("the service answered a later request too", round_trip(&c, &m));
        check("but no message exists, because none was ever reported", m.msg_count == 0);

        rift_ipc_close(&c);
        fake_meshcored_stop(pid);
    }

    /* ---- sending with nobody there ---------------------------------------- */
    {
        rift_model_init(&m);
        rift_ipc_init(&c, &m, "meshcored-that-is-not-there");
        check("a send with no connection is refused",
              rift_ipc_send_message(&c, KEY_B, "hello?") == -1);
        check("and says so rather than failing silently", m.outbox.failed);
        check("nothing was queued", !rift_model_sending(&m));
        rift_ipc_close(&c);
    }

    test_adverts(runtime);
    test_node_changes(runtime);
    test_management(runtime);
    test_repeater_control(runtime);
    {
        char path[700];

        snprintf(path, sizeof(path), "%s/adverts", runtime);
        unlink(path);
        snprintf(path, sizeof(path), "%s/node-methods", runtime);
        unlink(path);
    }
    rmdir(runtime);
    printf("rift_ipc_test: %d checks, %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
