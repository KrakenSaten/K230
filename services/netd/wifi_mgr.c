/*
 * netd's Wi-Fi manager. See wifi_mgr.h and docs/api/network.md.
 *
 * Logging rule for this file: SSIDs, BSSIDs and states may be logged; a
 * passphrase never is, and neither is any control-interface command that
 * can carry one (only its verb and field name are).
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#define _GNU_SOURCE
#include "wifi_mgr.h"

#include "pocketipc/pocketipc.h"
#include "pocketlog/pocketlog.h"
#include "pocketpaths.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CTRL_TIMEOUT_MS 300
#define STOP_GRACE_MS 3000
/* How long one step may spend handing saved networks over before it leaves
 * the rest to the next step (each command is bounded by CTRL_TIMEOUT_MS). */
#define SYNC_STEP_BUDGET_MS 500
/* After a sync cut short by no answer: 0.5, 1, 2, then every 4 s. */
#define SYNC_RETRY_MS 500
#define SYNC_RETRY_MAX_MS 4000

/* ctrl_ok() and supp_add() results below 0. saved_id[] uses the same values. */
#define CTRL_NO_ANSWER (-1) /* busy or gone: the command may still be carried out later, or never */
#define CTRL_REFUSED (-2)   /* it answered, and not with OK */

static const char *const state_names[] = {
    [WIFI_STATE_UNAVAILABLE] = "unavailable",
    [WIFI_STATE_OFF] = "off",
    [WIFI_STATE_STARTING] = "starting",
    [WIFI_STATE_DISCONNECTED] = "disconnected",
    [WIFI_STATE_CONNECTING] = "connecting",
    [WIFI_STATE_OBTAINING_IP] = "obtaining_ip",
    [WIFI_STATE_CONNECTED] = "connected",
    [WIFI_STATE_FAILED] = "failed",
};

static const char *const reason_names[] = {
    [WIFI_REASON_NONE] = NULL,
    [WIFI_REASON_NO_INTERFACE] = "no_interface",
    [WIFI_REASON_INTERFACE_BUSY] = "interface_busy",
    [WIFI_REASON_AUTH_FAILED] = "auth_failed",
    [WIFI_REASON_NOT_FOUND] = "not_found",
    [WIFI_REASON_ASSOC_FAILED] = "assoc_failed",
    [WIFI_REASON_TIMEOUT] = "timeout",
    [WIFI_REASON_DHCP_FAILED] = "dhcp_failed",
    [WIFI_REASON_SUPPLICANT_FAILED] = "supplicant_failed",
};

const char *wifi_state_name(enum wifi_state s)
{
    return (size_t)s < sizeof(state_names) / sizeof(state_names[0]) ? state_names[s] : "unknown";
}

const char *wifi_reason_name(enum wifi_reason r)
{
    return (size_t)r < sizeof(reason_names) / sizeof(reason_names[0]) ? reason_names[r] : NULL;
}

/* ---- children that were asked to stop ------------------------------------ *
 *
 * Stopping is asynchronous so no request handler waits for a process to
 * leave: the child gets SIGTERM, is remembered here, reaped by netd's loop
 * through wifi_mgr_child_exited(), and SIGKILLed if it overstays. */

#define STOPPING_MAX 8
static struct {
    pid_t pid;
    long deadline_ms;
} stopping[STOPPING_MAX];

static void stop_async(pid_t pid, long now_ms)
{
    int i;

    if (pid <= 0) {
        return;
    }
    kill(pid, SIGTERM);
    for (i = 0; i < STOPPING_MAX; i++) {
        if (stopping[i].pid == 0) {
            stopping[i].pid = pid;
            stopping[i].deadline_ms = now_ms + STOP_GRACE_MS;
            return;
        }
    }
    /* No slot: wait for it here rather than lose track of it. */
    netd_stop_child(pid, STOP_GRACE_MS);
}

static int stopping_count(void)
{
    int i;
    int n = 0;

    for (i = 0; i < STOPPING_MAX; i++) {
        n += stopping[i].pid != 0;
    }
    return n;
}

static void stopping_step(long now_ms)
{
    int i;

    for (i = 0; i < STOPPING_MAX; i++) {
        if (stopping[i].pid != 0 && now_ms >= stopping[i].deadline_ms) {
            LOG_WARN("wifi: child %d ignored SIGTERM, killing it", (int)stopping[i].pid);
            kill(stopping[i].pid, SIGKILL);
            stopping[i].deadline_ms = now_ms + STOP_GRACE_MS;
        }
    }
}

/* ---- control interface ----------------------------------------------------- */

static long mono_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* The verb and, for SET_NETWORK, the field: everything that may be logged
 * about a command. */
static void cmd_label(const char *cmd, char *out, size_t n)
{
    size_t words = strncmp(cmd, "SET_NETWORK ", 12) == 0 ? 3 : 1;
    size_t i = 0;
    size_t seen = 0;

    while (cmd[i] && i + 1 < n) {
        if (cmd[i] == ' ' && ++seen >= words) {
            break;
        }
        out[i] = cmd[i];
        i++;
    }
    out[i] = '\0';
}

static int ctrl(struct wifi_mgr *m, const char *cmd, char *reply, size_t n)
{
    char label[48];
    int r;

    if (m->req.fd < 0) {
        errno = ENOTCONN;
        return -1;
    }
    r = wpa_ctrl_request(&m->req, cmd, reply, n, CTRL_TIMEOUT_MS);
    cmd_label(cmd, label, sizeof(label));
    if (r < 0) {
        LOG_WARN("wifi: %s: no answer from wpa_supplicant: %s", label, strerror(errno));
        m->ctrl_failures++;
        /* A busy supplicant still carries the command out when it gets to
         * it - an ADD_NETWORK whose id never arrived, a late SELECT_NETWORK.
         * The next sync looks at what it actually holds. */
        m->sweep_due = 1;
        return -1;
    }
    m->ctrl_failures = 0;
    m->ctrl_answered_ms = mono_ms();
    LOG_DEBUG("wifi: %s -> %.*s", label, (int)strcspn(reply, "\n"), reply);
    return r;
}

/* 0, CTRL_NO_ANSWER or CTRL_REFUSED. */
static int ctrl_ok(struct wifi_mgr *m, const char *cmd)
{
    char reply[64];

    if (ctrl(m, cmd, reply, sizeof(reply)) < 0) {
        return CTRL_NO_ANSWER;
    }
    return strncmp(reply, "OK", 2) == 0 ? 0 : CTRL_REFUSED;
}

static void ctrl_close(struct wifi_mgr *m)
{
    wpa_ctrl_close(&m->req);
    wpa_ctrl_close(&m->ev);
    m->ctrl_ready = 0;
}

/* A passphrase is cleared with explicit_bzero so the compiler cannot drop it;
 * the rest of the entry is ordinary data. */
static void wipe_saved(struct wifi_saved *s)
{
    explicit_bzero(s->passphrase, sizeof(s->passphrase));
    memset(&s->ssid, 0, sizeof(s->ssid));
    s->security = WIFI_SEC_OPEN;
    s->hidden = 0;
}

/* ---- association bookkeeping ------------------------------------------------ */

static void clear_association(struct wifi_mgr *m)
{
    m->wpa_state[0] = '\0';
    m->completed = 0;
    m->cur_id = -1;
    memset(&m->cur_ssid, 0, sizeof(m->cur_ssid));
    m->bssid[0] = '\0';
    m->freq_mhz = 0;
    m->signal_dbm = 0;
    m->have_signal = 0;
    m->ipv4[0] = '\0';
}

static void set_state(struct wifi_mgr *m, enum wifi_state s, enum wifi_reason r)
{
    if (m->state != s || m->reason != r) {
        char ssid[WIFI_SSID_TEXT_MAX];

        wifi_ssid_to_text(&m->cur_ssid, ssid, sizeof(ssid));
        if (s == WIFI_STATE_FAILED || s == WIFI_STATE_UNAVAILABLE) {
            LOG_WARN("wifi: %s -> %s%s%s", wifi_state_name(m->state), wifi_state_name(s),
                     r ? " " : "", r ? wifi_reason_name(r) : "");
        } else {
            LOG_INFO("wifi: %s -> %s%s%s%s", wifi_state_name(m->state), wifi_state_name(s),
                     ssid[0] ? " \"" : "", ssid, ssid[0] ? "\"" : "");
        }
    }
    m->state = s;
    m->reason = r;
}

/* ---- the store ---------------------------------------------------------------- */

static int store_save(struct wifi_mgr *m)
{
    if (m->store_load == WIFI_STORE_UNREADABLE) {
        /* It exists and could not be read: what it holds is unknown, so it
         * is never replaced by what this process happens to have. */
        LOG_ERROR("wifi: not writing %s/%s: it could not be read at start", m->store_dir,
                  WIFI_STORE_FILE);
        m->store_write_failed = 1;
        return -1;
    }
    if (m->store_set_aside_pending) {
        if (wifi_store_set_aside(m->store_dir) == 0) {
            LOG_WARN("wifi: the damaged store was kept as %s/%s", m->store_dir, WIFI_STORE_DAMAGED_FILE);
        } else if (errno != ENOENT) {
            LOG_ERROR("wifi: cannot move the damaged store aside: %s; not overwriting it",
                      strerror(errno));
            m->store_write_failed = 1;
            return -1;
        }
        m->store_set_aside_pending = 0;
    }
    if (wifi_store_save(m->store_dir, &m->store) < 0) {
        LOG_ERROR("wifi: cannot write %s/%s: %s", m->store_dir, WIFI_STORE_FILE, strerror(errno));
        m->store_write_failed = 1;
        return -1;
    }
    m->store_write_failed = 0;
    return 0;
}

/* ---- wpa_supplicant networks -------------------------------------------------- */

static const char *key_mgmt_for(enum wifi_security s, int sae_capable)
{
    switch (wifi_join_method(s, sae_capable)) {
    case WIFI_JOIN_OPEN:
        return "NONE";
    case WIFI_JOIN_SAE:
        return s == WIFI_SEC_WPA2_WPA3 ? "WPA-PSK SAE" : "SAE";
    case WIFI_JOIN_PSK:
        return "WPA-PSK";
    default:
        return NULL;
    }
}

/* Add one network. select: SELECT_NETWORK (join it now, others paused)
 * instead of ENABLE_NETWORK. Returns the supplicant id, CTRL_NO_ANSWER (try
 * again later) or CTRL_REFUSED (no point trying again). */
static int supp_add(struct wifi_mgr *m, const struct wifi_saved *n, int select)
{
    char reply[64];
    char cmd[160];
    char ssid_hex[WIFI_SSID_HEX_MAX];
    const char *km = key_mgmt_for(n->security, m->sae_capable);
    char *end;
    long id;
    int rc;

    if (!km) {
        return CTRL_REFUSED;
    }
    if (ctrl(m, "ADD_NETWORK", reply, sizeof(reply)) < 0) {
        return CTRL_NO_ANSWER;
    }
    id = strtol(reply, &end, 10);
    if (end == reply || (*end != '\0' && *end != '\n') || id < 0 || id > 100000) {
        LOG_WARN("wifi: ADD_NETWORK gave no id");
        return CTRL_REFUSED;
    }
    wifi_ssid_to_hex(&n->ssid, ssid_hex, sizeof(ssid_hex));
    snprintf(cmd, sizeof(cmd), "SET_NETWORK %ld ssid %s", id, ssid_hex);
    if ((rc = ctrl_ok(m, cmd)) < 0) {
        goto fail;
    }
    snprintf(cmd, sizeof(cmd), "SET_NETWORK %ld key_mgmt %s", id, km);
    if ((rc = ctrl_ok(m, cmd)) < 0) {
        goto fail;
    }
    if (strcmp(km, "NONE") != 0) {
        /* The passphrase has already passed wifi_passphrase_valid: printable
         * ASCII only, so it cannot end the command or add a line.
         * wpa_supplicant takes everything between the first and the last
         * quote, so a quote inside it is kept as a character. */
        snprintf(cmd, sizeof(cmd), "SET_NETWORK %ld psk \"%s\"", id, n->passphrase);
        rc = ctrl_ok(m, cmd);
        explicit_bzero(cmd, sizeof(cmd));
        if (rc < 0) {
            goto fail;
        }
        if (strcmp(km, "SAE") == 0 || strcmp(km, "WPA-PSK SAE") == 0) {
            snprintf(cmd, sizeof(cmd), "SET_NETWORK %ld ieee80211w %d", id,
                     strcmp(km, "SAE") == 0 ? 2 : 1);
            if ((rc = ctrl_ok(m, cmd)) < 0) {
                goto fail;
            }
        }
    }
    if (n->hidden) {
        snprintf(cmd, sizeof(cmd), "SET_NETWORK %ld scan_ssid 1", id);
        if ((rc = ctrl_ok(m, cmd)) < 0) {
            goto fail;
        }
    }
    snprintf(cmd, sizeof(cmd), "%s %ld", select ? "SELECT_NETWORK" : "ENABLE_NETWORK", id);
    if ((rc = ctrl_ok(m, cmd)) < 0) {
        goto fail;
    }
    return (int)id;
fail:
    /* A supplicant that did not answer would only make REMOVE_NETWORK wait
     * out another timeout; the half-made entry is left to the sweep (ctrl()
     * has set sweep_due), which removes it once the supplicant answers. */
    if (rc == CTRL_REFUSED) {
        snprintf(cmd, sizeof(cmd), "REMOVE_NETWORK %ld", id);
        ctrl_ok(m, cmd);
    }
    return rc;
}

static void supp_remove(struct wifi_mgr *m, int id)
{
    char cmd[40];

    if (id >= 0) {
        snprintf(cmd, sizeof(cmd), "REMOVE_NETWORK %d", id);
        ctrl_ok(m, cmd);
    }
}

/* SELECT_NETWORK pauses every other network; this undoes that. */
static void enable_saved(struct wifi_mgr *m)
{
    char cmd[40];
    int i;

    for (i = 0; i < m->store.count; i++) {
        if (m->saved_id[i] >= 0) {
            snprintf(cmd, sizeof(cmd), "ENABLE_NETWORK %d", m->saved_id[i]);
            ctrl_ok(m, cmd);
        }
    }
}

static int is_saved_id(const struct wifi_mgr *m, int id)
{
    int i;

    for (i = 0; id >= 0 && i < m->store.count; i++) {
        if (m->saved_id[i] == id) {
            return 1;
        }
    }
    return 0;
}

/* Remove every network the supplicant holds that netd does not track: what
 * commands that got no answer still did when a busy supplicant got to them.
 * Called with no join pending, so the saved networks are all netd tracks.
 * The supplicant reads its one socket in order, so a LIST_NETWORKS answered
 * in time means everything sent before it has been carried out. Returns 0,
 * or CTRL_NO_ANSWER. */
static int supp_sweep(struct wifi_mgr *m)
{
    int ids[64];
    char cmd[40];
    char *reply = malloc(WPA_CTRL_REPLY_MAX);
    int count;
    int i;

    if (!reply) {
        return CTRL_NO_ANSWER;
    }
    if (ctrl(m, "LIST_NETWORKS", reply, WPA_CTRL_REPLY_MAX) < 0) {
        free(reply);
        return CTRL_NO_ANSWER;
    }
    count = wifi_parse_network_ids(reply, ids, (int)(sizeof(ids) / sizeof(ids[0])));
    free(reply);
    if (count < 0) {
        /* It answered, with something else: nothing to act on. */
        LOG_WARN("wifi: LIST_NETWORKS gave no list");
        m->sweep_due = 0;
        return 0;
    }
    for (i = 0; i < count; i++) {
        if (is_saved_id(m, ids[i])) {
            continue;
        }
        snprintf(cmd, sizeof(cmd), "REMOVE_NETWORK %d", ids[i]);
        if (ctrl_ok(m, cmd) == CTRL_NO_ANSWER) {
            return CTRL_NO_ANSWER;
        }
        LOG_INFO("wifi: removed network %d, which wpa_supplicant set up after netd stopped waiting",
                 ids[i]);
    }
    /* A full list may have had more; the next sync looks again. */
    m->sweep_due = count == (int)(sizeof(ids) / sizeof(ids[0]));
    return 0;
}

static void sync_later(struct wifi_mgr *m, long now_ms)
{
    long wait = (long)SYNC_RETRY_MS << (m->sync_retries < 3 ? m->sync_retries : 3);

    if (wait > SYNC_RETRY_MAX_MS) {
        wait = SYNC_RETRY_MAX_MS;
    }
    m->sync_retries++;
    m->sync_at_ms = now_ms + wait;
    LOG_WARN("wifi: wpa_supplicant is busy; saved networks are handed over again in %ld ms", wait);
}

/* Hand wpa_supplicant the saved networks it does not hold yet: all of them
 * once its control interface is up, and afterwards whatever a busy
 * supplicant did not take. A command without an answer ends the pass and a
 * later step tries again, so a supplicant that is busy for a few seconds
 * (it was, after a runtime restart on unit A) costs a delay, not the saved
 * networks. A network it refuses is not offered again until it restarts.
 * One pass spends about SYNC_STEP_BUDGET_MS at most, plus one command. */
static void supp_sync(struct wifi_mgr *m, long now_ms)
{
    long budget_end = mono_ms() + SYNC_STEP_BUDGET_MS;
    int i;

    /* A join in progress has paused every other network (SELECT_NETWORK);
     * ENABLE_NETWORK now would undo that. Its end re-enables them, and the
     * next pass adds what is missing. */
    if (m->pending || now_ms < m->sync_at_ms) {
        return;
    }
    if (m->sweep_due && supp_sweep(m) < 0) {
        sync_later(m, now_ms);
        return;
    }
    for (i = 0; i < m->store.count; i++) {
        char ssid[WIFI_SSID_TEXT_MAX];
        int id;

        if (m->saved_id[i] != CTRL_NO_ANSWER) {
            continue;
        }
        if (mono_ms() >= budget_end) {
            return; /* the next step goes on */
        }
        id = supp_add(m, &m->store.net[i], 0);
        if (id == CTRL_NO_ANSWER) {
            sync_later(m, now_ms);
            return;
        }
        m->saved_id[i] = id;
        if (id == CTRL_REFUSED) {
            wifi_ssid_to_text(&m->store.net[i].ssid, ssid, sizeof(ssid));
            LOG_WARN("wifi: saved network \"%s\" could not be added", ssid);
        }
    }
    if (m->sync_retries) {
        LOG_INFO("wifi: saved networks handed to wpa_supplicant after %d retr%s", m->sync_retries,
                 m->sync_retries == 1 ? "y" : "ies");
        m->sync_retries = 0;
    }
}

/* ---- the DHCP client ------------------------------------------------------------ */

static void dhcp_start(struct wifi_mgr *m, long now_ms)
{
    const char *argv[] = { m->sys.udhcpc, "-f", "-R", "-i", m->sys.iface, "-t", "5", "-T", "2",
                           "-A", "10", "-s", m->sys.dhcp_script, NULL };

    m->dhcp_pid = netd_spawn(argv);
    m->dhcp_retry_at_ms = now_ms + 5000;
    if (m->dhcp_pid < 0) {
        LOG_ERROR("wifi: cannot start %s: %s", m->sys.udhcpc, strerror(errno));
        m->dhcp_pid = 0;
        return;
    }
    LOG_INFO("wifi: DHCP client started on %s (pid %d)", m->sys.iface, (int)m->dhcp_pid);
}

static void dhcp_stop(struct wifi_mgr *m, long now_ms)
{
    if (m->dhcp_pid > 0) {
        /* udhcpc -R releases the lease and deconfigures the address on exit. */
        stop_async(m->dhcp_pid, now_ms);
        LOG_INFO("wifi: DHCP client stopped");
    }
    m->dhcp_pid = 0;
    m->ipv4[0] = '\0';
}

/* ---- wpa_supplicant itself ------------------------------------------------------ */

static void cancel_pending(struct wifi_mgr *m, enum wifi_reason latch)
{
    char ssid[WIFI_SSID_TEXT_MAX];
    int slot = m->pending_replaced_slot;

    if (!m->pending) {
        return;
    }
    wifi_ssid_to_text(&m->pending_net.ssid, ssid, sizeof(ssid));
    if (m->ctrl_ready) {
        supp_remove(m, m->pending_id);
        /* The saved entry this attempt displaced goes back as it was. */
        if (slot >= 0 && slot < m->store.count && m->saved_id[slot] < 0) {
            m->saved_id[slot] = supp_add(m, &m->store.net[slot], 0);
        }
        enable_saved(m);
    }
    if (latch != WIFI_REASON_NONE) {
        LOG_WARN("wifi: joining \"%s\" failed: %s", ssid, wifi_reason_name(latch));
    }
    wipe_saved(&m->pending_net);
    m->pending = 0;
    m->pending_id = -1;
    m->pending_not_found = 0;
    m->pending_replaced_slot = -1;
    m->latched = latch;
}

static void supp_teardown(struct wifi_mgr *m, long now_ms, int iface_down)
{
    int i;

    dhcp_stop(m, now_ms);
    if (m->pending) {
        wipe_saved(&m->pending_net);
        m->pending = 0;
        m->pending_replaced_slot = -1;
    }
    if (m->ctrl_ready) {
        char reply[16];

        wpa_ctrl_request(&m->req, "TERMINATE", reply, sizeof(reply), CTRL_TIMEOUT_MS);
    }
    ctrl_close(m);
    if (m->supp_pid > 0) {
        stop_async(m->supp_pid, now_ms);
        LOG_INFO("wifi: wpa_supplicant stopped");
    }
    m->supp_pid = 0;
    for (i = 0; i < WIFI_STORE_MAX; i++) {
        m->saved_id[i] = -1;
    }
    m->scanning = 0;
    clear_association(m);
    if (iface_down && netd_iface_set_up(&m->sys, 0) < 0) {
        LOG_WARN("wifi: cannot bring %s down: %s", m->sys.iface, strerror(errno));
    }
}

static int write_conf(struct wifi_mgr *m)
{
    char text[NETD_PATH_MAX + 96];
    int fd;
    int len;
    int rc = 0;

    len = snprintf(text, sizeof(text),
                   "# Generated by netd at start; holds no network and no secret.\n"
                   "ctrl_interface=%s\nupdate_config=0\nap_scan=1\n", m->ctrl_dir);
    fd = open(m->conf_path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) {
        return -1;
    }
    if (write(fd, text, (size_t)len) != len) {
        rc = -1;
    }
    if (close(fd) != 0) {
        rc = -1;
    }
    return rc;
}

static void supp_failed(struct wifi_mgr *m, long now_ms, const char *why)
{
    long backoff;

    m->supp_failures++;
    backoff = 1000L << (m->supp_failures < 5 ? m->supp_failures : 5);
    m->supp_retry_at_ms = now_ms + backoff;
    LOG_WARN("wifi: wpa_supplicant %s (failure %d of %d)", why, m->supp_failures,
             WIFI_SUPPLICANT_MAX_RESTARTS);
}

static void supp_start(struct wifi_mgr *m, long now_ms)
{
    const char *argv[] = { m->sys.wpa_supplicant, "-i", m->sys.iface, "-D", "nl80211,wext",
                           "-c", m->conf_path, NULL };
    char sock[NETD_PATH_MAX + IFNAMSIZ + 2];
    struct stat sb;

    if (pocketos_mkdir_p(m->run_dir, 0700) < 0) {
        LOG_ERROR("wifi: cannot create %s: %s", m->run_dir, strerror(errno));
        supp_failed(m, now_ms, "could not be prepared");
        return;
    }
    /* A socket left by a supplicant nobody owns any more (netd was killed and
     * its child lingered): ask it to go, then clear the path. A live one was
     * already refused as foreign before this point. */
    snprintf(sock, sizeof(sock), "%s/%s", m->ctrl_dir, m->sys.iface);
    if (lstat(sock, &sb) == 0) {
        struct wpa_ctrl old;
        char reply[16];

        if (wpa_ctrl_open(&old, m->ctrl_dir, m->sys.iface, m->run_dir) == 0) {
            wpa_ctrl_request(&old, "TERMINATE", reply, sizeof(reply), CTRL_TIMEOUT_MS);
            wpa_ctrl_close(&old);
        }
        unlink(sock);
    }
    if (write_conf(m) < 0) {
        LOG_ERROR("wifi: cannot write %s: %s", m->conf_path, strerror(errno));
        supp_failed(m, now_ms, "could not be configured");
        return;
    }
    m->supp_pid = netd_spawn(argv);
    if (m->supp_pid < 0) {
        m->supp_pid = 0;
        supp_failed(m, now_ms, "could not be started");
        return;
    }
    m->supp_started_ms = now_ms;
    m->ctrl_ready = 0;
    LOG_INFO("wifi: wpa_supplicant started on %s (pid %d)", m->sys.iface, (int)m->supp_pid);
}

static int ctrl_connect(struct wifi_mgr *m, long now_ms)
{
    char reply[256];
    int i;

    if (wpa_ctrl_open(&m->req, m->ctrl_dir, m->sys.iface, m->run_dir) < 0) {
        return -1;
    }
    if (wpa_ctrl_open(&m->ev, m->ctrl_dir, m->sys.iface, m->run_dir) < 0 ||
        wpa_ctrl_attach(&m->ev, CTRL_TIMEOUT_MS) < 0 ||
        wpa_ctrl_request(&m->req, "PING", reply, sizeof(reply), CTRL_TIMEOUT_MS) < 0 ||
        strncmp(reply, "PONG", 4) != 0) {
        ctrl_close(m);
        return -1;
    }
    m->sae_capable = 0;
    if (wpa_ctrl_request(&m->req, "GET_CAPABILITY key_mgmt", reply, sizeof(reply),
                         CTRL_TIMEOUT_MS) > 0) {
        char *save = NULL;
        char *tok;

        for (tok = strtok_r(reply, " \n", &save); tok; tok = strtok_r(NULL, " \n", &save)) {
            if (strcmp(tok, "SAE") == 0) {
                m->sae_capable = 1;
            }
        }
    }
    LOG_INFO("wifi: control interface ready; WPA3-SAE %s by the driver",
             m->sae_capable ? "supported" : "not supported");
    m->ctrl_ready = 1;
    m->ctrl_failures = 0;
    m->ctrl_answered_ms = mono_ms();
    /* The saved networks are handed over by supp_sync(), in this step and,
     * when the supplicant is busy, later ones. Anything it already holds is
     * swept first: nothing, from the configuration netd writes, unless this
     * is a reconnect to a supplicant that kept running. */
    for (i = 0; i < WIFI_STORE_MAX; i++) {
        m->saved_id[i] = CTRL_NO_ANSWER;
    }
    m->sweep_due = 1;
    m->sync_at_ms = now_ms;
    m->sync_retries = 0;
    return 0;
}

/* ---- polling and events ------------------------------------------------------------ */

static void fetch_scan_results(struct wifi_mgr *m, long now_ms)
{
    char *reply = malloc(WPA_CTRL_REPLY_MAX);

    if (!reply) {
        return;
    }
    if (ctrl(m, "SCAN_RESULTS", reply, WPA_CTRL_REPLY_MAX) >= 0) {
        m->bss_count = wifi_parse_scan_results(reply, m->bss, WIFI_SCAN_MAX, &m->hidden_count);
        m->scan_done_ms = now_ms;
        m->scan_failed = 0;
        LOG_DEBUG("wifi: scan results: %d BSS, %d hidden", m->bss_count, m->hidden_count);
    }
    m->scanning = 0;
    free(reply);
}

static void poll_status(struct wifi_mgr *m, long now_ms)
{
    char reply[2048];
    char value[128];
    int was_completed = m->completed;

    if (ctrl(m, "STATUS", reply, sizeof(reply)) < 0) {
        return;
    }
    if (wifi_kv_get(reply, "wpa_state", m->wpa_state, sizeof(m->wpa_state)) < 0) {
        m->wpa_state[0] = '\0';
    }
    m->completed = strcmp(m->wpa_state, "COMPLETED") == 0;
    if (!m->completed) {
        char keep[32];

        snprintf(keep, sizeof(keep), "%s", m->wpa_state);
        clear_association(m);
        snprintf(m->wpa_state, sizeof(m->wpa_state), "%s", keep);
        return;
    }
    if (wifi_kv_get_int(reply, "id", &m->cur_id) < 0) {
        m->cur_id = -1;
    }
    if (wifi_kv_get(reply, "ssid", value, sizeof(value)) < 0 ||
        wifi_ssid_decode_printf(value, &m->cur_ssid) < 0) {
        memset(&m->cur_ssid, 0, sizeof(m->cur_ssid));
    }
    if (wifi_kv_get(reply, "bssid", m->bssid, sizeof(m->bssid)) < 0) {
        m->bssid[0] = '\0';
    }
    if (wifi_kv_get_int(reply, "freq", &m->freq_mhz) < 0) {
        m->freq_mhz = 0;
    }
    m->have_signal = 0;
    if (ctrl(m, "SIGNAL_POLL", reply, sizeof(reply)) >= 0 &&
        wifi_kv_get_int(reply, "RSSI", &m->signal_dbm) == 0 && m->signal_dbm < 0) {
        m->have_signal = 1;
    }
    if (!was_completed) {
        char ssid[WIFI_SSID_TEXT_MAX];

        m->assoc_since_ms = now_ms;
        m->dhcp_retry_at_ms = 0;
        wifi_ssid_to_text(&m->cur_ssid, ssid, sizeof(ssid));
        LOG_INFO("wifi: associated with \"%s\" (%s, %d MHz)", ssid, m->bssid, m->freq_mhz);
    }
}

static void handle_event(struct wifi_mgr *m, const char *msg, long now_ms)
{
    struct wifi_event ev;

    wifi_parse_event(msg, &ev);
    switch (ev.type) {
    case WIFI_EV_SCAN_RESULTS:
        fetch_scan_results(m, now_ms);
        break;
    case WIFI_EV_SCAN_FAILED:
        m->scanning = 0;
        m->scan_failed = 1;
        break;
    case WIFI_EV_CONNECTED:
    case WIFI_EV_DISCONNECTED:
        m->status_due_ms = now_ms;
        break;
    case WIFI_EV_WRONG_KEY:
        if (m->pending && (ev.network_id < 0 || ev.network_id == m->pending_id)) {
            cancel_pending(m, WIFI_REASON_AUTH_FAILED);
        } else if (is_saved_id(m, ev.network_id)) {
            /* A saved network whose passphrase has changed. wpa_supplicant
             * keeps retrying on its own backoff; say why nothing happens. */
            m->latched = WIFI_REASON_AUTH_FAILED;
        }
        break;
    case WIFI_EV_CONN_FAILED:
    case WIFI_EV_ASSOC_REJECT:
    case WIFI_EV_AUTH_REJECT:
        if (m->pending && (ev.network_id < 0 || ev.network_id == m->pending_id)) {
            cancel_pending(m, WIFI_REASON_ASSOC_FAILED);
        }
        break;
    case WIFI_EV_NETWORK_NOT_FOUND:
        if (m->pending && ++m->pending_not_found >= 2) {
            cancel_pending(m, WIFI_REASON_NOT_FOUND);
        }
        break;
    case WIFI_EV_TERMINATING:
        ctrl_close(m);
        break;
    default:
        break;
    }
}

static void drain_events(struct wifi_mgr *m, long now_ms)
{
    char buf[1024];
    int r;
    int budget = 64;

    while (m->ctrl_ready && budget-- > 0 && (r = wpa_ctrl_recv(&m->ev, buf, sizeof(buf))) != 0) {
        if (r < 0) {
            LOG_WARN("wifi: event connection to wpa_supplicant lost: %s", strerror(errno));
            ctrl_close(m);
            return;
        }
        handle_event(m, buf, now_ms);
    }
}

/* Store the joined network and give its supplicant entry a slot, keeping
 * saved_id[] in step with wifi_store_put's own reordering. */
static void join_succeeded(struct wifi_mgr *m)
{
    char ssid[WIFI_SSID_TEXT_MAX];
    int at = wifi_store_find(&m->store, &m->pending_net.ssid);
    int i;

    if (at < 0) {
        if (m->store.count < WIFI_STORE_MAX) {
            at = m->store.count;
        } else {
            at = WIFI_STORE_MAX - 1;
            supp_remove(m, m->saved_id[at]); /* the oldest network makes room */
        }
    } else if (m->saved_id[at] >= 0 && m->saved_id[at] != m->pending_id) {
        supp_remove(m, m->saved_id[at]);
    }
    if (wifi_store_put(&m->store, &m->pending_net) == 0) {
        for (i = at; i > 0; i--) {
            m->saved_id[i] = m->saved_id[i - 1];
        }
        m->saved_id[0] = m->pending_id;
        store_save(m);
    }
    wifi_ssid_to_text(&m->pending_net.ssid, ssid, sizeof(ssid));
    LOG_INFO("wifi: joined \"%s\"%s", ssid,
             m->store_write_failed ? ", but it could not be remembered" : " and remembered it");
    m->pending_replaced_slot = -1;
    wipe_saved(&m->pending_net);
    m->pending = 0;
    m->pending_id = -1;
    m->pending_not_found = 0;
    m->latched = WIFI_REASON_NONE;
    enable_saved(m);
}

static void derive_state(struct wifi_mgr *m, long now_ms)
{
    if (m->pending) {
        set_state(m, WIFI_STATE_CONNECTING, WIFI_REASON_NONE);
    } else if (m->completed) {
        m->latched = WIFI_REASON_NONE;
        if (m->ipv4[0]) {
            set_state(m, WIFI_STATE_CONNECTED, WIFI_REASON_NONE);
        } else if (now_ms - m->assoc_since_ms >= m->dhcp_timeout_ms) {
            set_state(m, WIFI_STATE_FAILED, WIFI_REASON_DHCP_FAILED);
        } else {
            set_state(m, WIFI_STATE_OBTAINING_IP, WIFI_REASON_NONE);
        }
    } else if (m->latched != WIFI_REASON_NONE) {
        set_state(m, WIFI_STATE_FAILED, m->latched);
    } else if (strcmp(m->wpa_state, "AUTHENTICATING") == 0 ||
               strcmp(m->wpa_state, "ASSOCIATING") == 0 ||
               strcmp(m->wpa_state, "ASSOCIATED") == 0 ||
               strcmp(m->wpa_state, "4WAY_HANDSHAKE") == 0 ||
               strcmp(m->wpa_state, "GROUP_HANDSHAKE") == 0) {
        set_state(m, WIFI_STATE_CONNECTING, WIFI_REASON_NONE);
    } else {
        set_state(m, WIFI_STATE_DISCONNECTED, WIFI_REASON_NONE);
    }
}

/* ---- lifecycle ---------------------------------------------------------------------- */

int wifi_mgr_init(struct wifi_mgr *m, const struct wifi_mgr_config *cfg)
{
    int too_open = 0;
    int i;

    memset(m, 0, sizeof(*m));
    memset(stopping, 0, sizeof(stopping));
    netd_sys_init(&m->sys, cfg->iface);
    snprintf(m->store_dir, sizeof(m->store_dir), "%s", cfg->store_dir);
    snprintf(m->run_dir, sizeof(m->run_dir), "%s", cfg->run_dir);
    if (snprintf(m->ctrl_dir, sizeof(m->ctrl_dir), "%s/wpa", cfg->run_dir) >= (int)sizeof(m->ctrl_dir) ||
        snprintf(m->conf_path, sizeof(m->conf_path), "%s/wpa_supplicant.conf", cfg->run_dir) >=
            (int)sizeof(m->conf_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    m->connect_timeout_ms = cfg->connect_timeout_ms > 0 ? cfg->connect_timeout_ms
                                                         : WIFI_CONNECT_TIMEOUT_MS_DEFAULT;
    m->dhcp_timeout_ms = cfg->dhcp_timeout_ms > 0 ? cfg->dhcp_timeout_ms : WIFI_DHCP_TIMEOUT_MS_DEFAULT;
    m->state = WIFI_STATE_STARTING;
    m->req.fd = -1;
    m->ev.fd = -1;
    m->pending_id = -1;
    m->pending_replaced_slot = -1;
    for (i = 0; i < WIFI_STORE_MAX; i++) {
        m->saved_id[i] = -1;
    }
    clear_association(m);

    m->store_load = wifi_store_load(m->store_dir, &m->store, &too_open);
    switch (m->store_load) {
    case WIFI_STORE_LOADED:
        LOG_INFO("wifi: store loaded: Wi-Fi %s, %d saved network(s)", m->store.enabled ? "on" : "off",
                 m->store.count);
        if (too_open) {
            LOG_WARN("wifi: %s/%s was readable by others; its mode is corrected on the next write",
                     m->store_dir, WIFI_STORE_FILE);
            store_save(m);
        }
        break;
    case WIFI_STORE_ABSENT:
        LOG_INFO("wifi: no store yet: Wi-Fi off, no saved networks");
        break;
    case WIFI_STORE_DAMAGED:
        LOG_ERROR("wifi: %s/%s is damaged; starting with Wi-Fi off and no saved networks, "
                  "and the file is kept aside at the next change", m->store_dir, WIFI_STORE_FILE);
        m->store_set_aside_pending = 1;
        break;
    default:
        /* Not damaged, just unreadable now: store_save() never replaces it. */
        LOG_ERROR("wifi: %s/%s cannot be read: %s; starting with Wi-Fi off", m->store_dir,
                  WIFI_STORE_FILE, strerror(errno));
        break;
    }
    return 0;
}

void wifi_mgr_shutdown(struct wifi_mgr *m)
{
    int i;

    supp_teardown(m, 0, 0);
    for (i = 0; i < STOPPING_MAX; i++) {
        if (stopping[i].pid) {
            netd_stop_child(stopping[i].pid, STOP_GRACE_MS);
            stopping[i].pid = 0;
        }
    }
    wifi_store_wipe(&m->store);
}

int wifi_mgr_event_fd(const struct wifi_mgr *m)
{
    return m->ctrl_ready ? m->ev.fd : -1;
}

void wifi_mgr_child_exited(struct wifi_mgr *m, pid_t pid, int status, long now_ms)
{
    int i;

    for (i = 0; i < STOPPING_MAX; i++) {
        if (stopping[i].pid == pid) {
            stopping[i].pid = 0;
            return;
        }
    }
    if (pid == m->supp_pid) {
        m->supp_pid = 0;
        ctrl_close(m);
        dhcp_stop(m, now_ms);
        if (m->pending) {
            wipe_saved(&m->pending_net);
            m->pending = 0;
            m->pending_replaced_slot = -1;
            m->latched = WIFI_REASON_SUPPLICANT_FAILED;
        }
        for (i = 0; i < WIFI_STORE_MAX; i++) {
            m->saved_id[i] = -1;
        }
        clear_association(m);
        supp_failed(m, now_ms, WIFEXITED(status) ? "exited" : "was killed");
    } else if (pid == m->dhcp_pid) {
        LOG_WARN("wifi: DHCP client exited (status %d)", WIFEXITED(status) ? WEXITSTATUS(status) : -1);
        m->dhcp_pid = 0;
    }
}

void wifi_mgr_step(struct wifi_mgr *m, long now_ms)
{
    stopping_step(now_ms);
    if (!netd_iface_present(&m->sys)) {
        if (m->supp_pid) {
            LOG_WARN("wifi: %s disappeared", m->sys.iface);
            supp_teardown(m, now_ms, 0);
        }
        set_state(m, WIFI_STATE_UNAVAILABLE, WIFI_REASON_NO_INTERFACE);
        return;
    }
    if (!m->store.enabled) {
        if (m->supp_pid) {
            supp_teardown(m, now_ms, 1);
        }
        m->latched = WIFI_REASON_NONE;
        set_state(m, WIFI_STATE_OFF, WIFI_REASON_NONE);
        return;
    }
    if (!m->supp_pid) {
        if (m->foreign_checked_ms == 0 || now_ms - m->foreign_checked_ms >= 2000) {
            m->foreign_pid = netd_foreign_supplicant(&m->sys, 0);
            m->foreign_checked_ms = now_ms;
            if (m->foreign_pid) {
                LOG_WARN("wifi: wpa_supplicant pid %d, not started by netd, controls %s",
                         (int)m->foreign_pid, m->sys.iface);
            }
        }
        if (m->foreign_pid) {
            set_state(m, WIFI_STATE_UNAVAILABLE, WIFI_REASON_INTERFACE_BUSY);
            return;
        }
        if (m->supp_failures >= WIFI_SUPPLICANT_MAX_RESTARTS) {
            set_state(m, WIFI_STATE_FAILED, WIFI_REASON_SUPPLICANT_FAILED);
            return;
        }
        /* An old supplicant still on its way out holds the socket path. */
        if (now_ms < m->supp_retry_at_ms || stopping_count() > 0) {
            set_state(m, WIFI_STATE_STARTING, WIFI_REASON_NONE);
            return;
        }
        supp_start(m, now_ms);
        set_state(m, WIFI_STATE_STARTING, WIFI_REASON_NONE);
        return;
    }
    if (!m->ctrl_ready) {
        if (ctrl_connect(m, now_ms) == 0) {
            m->status_due_ms = now_ms;
        } else if (now_ms - m->supp_started_ms > WIFI_SUPPLICANT_START_MS) {
            LOG_WARN("wifi: wpa_supplicant never opened its control interface");
            stop_async(m->supp_pid, now_ms);
            m->supp_pid = 0;
            supp_failed(m, now_ms, "did not start");
        }
        if (!m->ctrl_ready) {
            set_state(m, WIFI_STATE_STARTING, WIFI_REASON_NONE);
            return;
        }
    }
    /* Failures are forgiven only after a minute of running, so a supplicant
     * that dies right after it answers still reaches the restart limit. */
    if (m->supp_failures && now_ms - m->supp_started_ms > 60000) {
        m->supp_failures = 0;
    }
    drain_events(m, now_ms);
    if (!m->ctrl_ready) {
        return;
    }
    supp_sync(m, now_ms);
    if (now_ms >= m->status_due_ms) {
        poll_status(m, now_ms);
        m->status_due_ms = now_ms + ((m->pending || (m->completed && !m->ipv4[0])) ? 500 : 1000);
        if (m->ctrl_failures >= 3 &&
            mono_ms() - m->ctrl_answered_ms >= WIFI_SUPPLICANT_UNRESPONSIVE_MS) {
            LOG_WARN("wifi: wpa_supplicant stopped answering; restarting it");
            supp_teardown(m, now_ms, 0);
            supp_failed(m, now_ms, "stopped answering");
            return;
        }
    }
    if (m->completed && m->dhcp_pid == 0 && now_ms >= m->dhcp_retry_at_ms) {
        dhcp_start(m, now_ms);
    } else if (!m->completed && m->dhcp_pid > 0) {
        dhcp_stop(m, now_ms);
    }
    if (m->completed) {
        if (netd_iface_ipv4(&m->sys, m->ipv4, sizeof(m->ipv4)) < 0) {
            m->ipv4[0] = '\0';
        }
    }
    if (m->pending) {
        if (m->completed && m->cur_id == m->pending_id) {
            join_succeeded(m);
        } else if (now_ms >= m->pending_deadline_ms) {
            cancel_pending(m, m->pending_not_found ? WIFI_REASON_NOT_FOUND : WIFI_REASON_TIMEOUT);
        }
    }
    if (m->scanning && now_ms - m->scan_started_ms >= WIFI_SCAN_TIMEOUT_MS) {
        m->scanning = 0;
        m->scan_failed = 1;
    }
    derive_state(m, now_ms);
}

/* ---- requests -------------------------------------------------------------------------- */

static int refuse_unless_ready(struct wifi_mgr *m, char *err, size_t n)
{
    if (m->state == WIFI_STATE_UNAVAILABLE) {
        snprintf(err, n, "Wi-Fi is unavailable (%s)", wifi_reason_name(m->reason));
        return POCKETIPC_ERR_UNSUPPORTED;
    }
    if (!m->store.enabled) {
        snprintf(err, n, "Wi-Fi is off");
        return POCKETIPC_ERR_POLICY;
    }
    if (!m->ctrl_ready) {
        snprintf(err, n, "Wi-Fi is starting; try again shortly");
        return POCKETIPC_ERR_BUSY;
    }
    return 0;
}

static void add_ssid(cJSON *o, const char *text_key, const char *hex_key, const struct wifi_ssid *s)
{
    char text[WIFI_SSID_TEXT_MAX];
    char hex[WIFI_SSID_HEX_MAX];

    if (s->len == 0) {
        cJSON_AddNullToObject(o, text_key);
        cJSON_AddNullToObject(o, hex_key);
        return;
    }
    wifi_ssid_to_text(s, text, sizeof(text));
    wifi_ssid_to_hex(s, hex, sizeof(hex));
    cJSON_AddStringToObject(o, text_key, text);
    cJSON_AddStringToObject(o, hex_key, hex);
}

static const char *store_word(const struct wifi_mgr *m)
{
    if (m->store_load == WIFI_STORE_UNREADABLE) {
        return "unreadable";
    }
    if (m->store_write_failed) {
        return "unwritable";
    }
    return m->store_set_aside_pending ? "damaged" : "ok";
}

int wifi_mgr_status(struct wifi_mgr *m, cJSON **result, char *err, size_t n)
{
    cJSON *o = cJSON_CreateObject();
    const char *reason = wifi_reason_name(m->reason);
    int slot;

    (void)err;
    (void)n;
    cJSON_AddNumberToObject(o, "api_version", 0);
    cJSON_AddStringToObject(o, "interface", m->sys.iface);
    cJSON_AddBoolToObject(o, "available", m->state != WIFI_STATE_UNAVAILABLE);
    cJSON_AddBoolToObject(o, "enabled", m->store.enabled);
    cJSON_AddStringToObject(o, "state", wifi_state_name(m->state));
    if (reason) {
        cJSON_AddStringToObject(o, "reason", reason);
    } else {
        cJSON_AddNullToObject(o, "reason");
    }
    if (m->pending) {
        add_ssid(o, "ssid", "ssid_hex", &m->pending_net.ssid);
    } else {
        add_ssid(o, "ssid", "ssid_hex", &m->cur_ssid);
    }
    if (m->completed && m->bssid[0]) {
        cJSON_AddStringToObject(o, "bssid", m->bssid);
    } else {
        cJSON_AddNullToObject(o, "bssid");
    }
    if (m->completed && m->freq_mhz > 0) {
        cJSON_AddNumberToObject(o, "frequency_mhz", m->freq_mhz);
    } else {
        cJSON_AddNullToObject(o, "frequency_mhz");
    }
    if (m->completed && m->have_signal) {
        cJSON_AddNumberToObject(o, "signal_dbm", m->signal_dbm);
        cJSON_AddNumberToObject(o, "signal_bars", wifi_signal_bars(m->signal_dbm));
    } else {
        cJSON_AddNullToObject(o, "signal_dbm");
        cJSON_AddNullToObject(o, "signal_bars");
    }
    if (m->completed && m->ipv4[0]) {
        cJSON_AddStringToObject(o, "ipv4", m->ipv4);
    } else {
        cJSON_AddNullToObject(o, "ipv4");
    }
    slot = m->completed ? wifi_store_find(&m->store, &m->cur_ssid) : -1;
    if (slot >= 0) {
        cJSON_AddStringToObject(o, "security", wifi_security_name(m->store.net[slot].security));
    } else {
        cJSON_AddNullToObject(o, "security");
    }
    cJSON_AddBoolToObject(o, "scanning", m->scanning);
    cJSON_AddNumberToObject(o, "saved_count", m->store.count);
    cJSON_AddStringToObject(o, "store", store_word(m));
    if (m->ctrl_ready) {
        cJSON_AddBoolToObject(o, "wpa3_supported", m->sae_capable);
    } else {
        cJSON_AddNullToObject(o, "wpa3_supported");
    }
    *result = o;
    return 0;
}

int wifi_mgr_set_enabled(struct wifi_mgr *m, int on, long now_ms, cJSON **result, char *err,
                         size_t n)
{
    int was = m->store.enabled;

    if (m->store_load == WIFI_STORE_UNREADABLE) {
        snprintf(err, n, "the Wi-Fi store could not be read, so it is not being replaced");
        return POCKETIPC_ERR_BACKEND;
    }
    if (on != was) {
        m->store.enabled = on;
        if (store_save(m) < 0) {
            m->store.enabled = was;
            snprintf(err, n, "could not save the Wi-Fi setting");
            return POCKETIPC_ERR_BACKEND;
        }
        LOG_INFO("wifi: turned %s", on ? "on" : "off");
    }
    if (on) {
        m->supp_failures = 0;
        m->supp_retry_at_ms = 0;
        m->foreign_checked_ms = 0;
    }
    m->latched = WIFI_REASON_NONE;
    wifi_mgr_step(m, now_ms);
    return wifi_mgr_status(m, result, err, n);
}

int wifi_mgr_scan(struct wifi_mgr *m, long now_ms, cJSON **result, char *err, size_t n)
{
    char reply[64];
    int code = refuse_unless_ready(m, err, n);

    if (code) {
        return code;
    }
    if (ctrl(m, "SCAN", reply, sizeof(reply)) < 0) {
        snprintf(err, n, "wpa_supplicant did not answer");
        return POCKETIPC_ERR_BACKEND;
    }
    if (strncmp(reply, "OK", 2) != 0 && strncmp(reply, "FAIL-BUSY", 9) != 0) {
        snprintf(err, n, "the scan could not be started");
        return POCKETIPC_ERR_BACKEND;
    }
    /* FAIL-BUSY: a scan is already running, and its results will do. */
    m->scanning = 1;
    m->scan_failed = 0;
    m->scan_started_ms = now_ms;
    *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(*result, "scanning", 1);
    return 0;
}

int wifi_mgr_networks(struct wifi_mgr *m, long now_ms, cJSON **result, char *err, size_t n)
{
    struct wifi_network nets[WIFI_SCAN_MAX];
    int count = wifi_networks_from_bss(m->bss, m->bss_count, nets, WIFI_SCAN_MAX);
    cJSON *o = cJSON_CreateObject();
    cJSON *list = cJSON_CreateArray();
    int i;

    (void)err;
    (void)n;
    cJSON_AddBoolToObject(o, "scanning", m->scanning);
    cJSON_AddBoolToObject(o, "scan_failed", m->scan_failed);
    if (m->scan_done_ms) {
        cJSON_AddNumberToObject(o, "age_s", (double)((now_ms - m->scan_done_ms) / 1000));
    } else {
        cJSON_AddNullToObject(o, "age_s");
    }
    cJSON_AddNumberToObject(o, "hidden_count", m->hidden_count);
    for (i = 0; i < count; i++) {
        cJSON *e = cJSON_CreateObject();
        enum wifi_join join = wifi_join_method(nets[i].security, m->sae_capable);

        add_ssid(e, "ssid", "ssid_hex", &nets[i].ssid);
        cJSON_AddNumberToObject(e, "signal_dbm", nets[i].signal_dbm);
        cJSON_AddNumberToObject(e, "signal_bars", wifi_signal_bars(nets[i].signal_dbm));
        cJSON_AddNumberToObject(e, "frequency_mhz", nets[i].freq_mhz);
        cJSON_AddStringToObject(e, "security", wifi_security_name(nets[i].security));
        cJSON_AddBoolToObject(e, "supported", join != WIFI_JOIN_UNSUPPORTED);
        cJSON_AddBoolToObject(e, "needs_passphrase", join == WIFI_JOIN_PSK || join == WIFI_JOIN_SAE);
        cJSON_AddBoolToObject(e, "saved", wifi_store_find(&m->store, &nets[i].ssid) >= 0);
        cJSON_AddBoolToObject(e, "connected",
                              m->completed && wifi_ssid_equal(&m->cur_ssid, &nets[i].ssid));
        cJSON_AddItemToArray(list, e);
    }
    cJSON_AddItemToObject(o, "networks", list);
    *result = o;
    return 0;
}

static const struct wifi_bss *strongest_bss(const struct wifi_mgr *m, const struct wifi_ssid *ssid)
{
    const struct wifi_bss *best = NULL;
    int i;

    for (i = 0; i < m->bss_count; i++) {
        if (wifi_ssid_equal(&m->bss[i].ssid, ssid) &&
            (!best || m->bss[i].signal_dbm > best->signal_dbm)) {
            best = &m->bss[i];
        }
    }
    return best;
}

int wifi_mgr_connect(struct wifi_mgr *m, const struct wifi_ssid *ssid, const char *passphrase,
                     int hidden, const char *security_decl, int allow_open, long now_ms,
                     cJSON **result, char *err, size_t n)
{
    struct wifi_saved net;
    enum wifi_security sec;
    enum wifi_join join;
    char text[WIFI_SSID_TEXT_MAX];
    int code = refuse_unless_ready(m, err, n);
    int slot;

    if (code) {
        return code;
    }
    if (ssid->len == 0 || wifi_ssid_is_hidden(ssid)) {
        snprintf(err, n, "ssid must be 1..32 bytes");
        return POCKETIPC_ERR_INVALID_PARAMS;
    }
    if (hidden) {
        if (!security_decl || wifi_security_parse(security_decl, &sec) < 0) {
            snprintf(err, n, "a hidden network needs security: open, wpa, wpa2 or wpa3");
            return POCKETIPC_ERR_INVALID_PARAMS;
        }
    } else {
        const struct wifi_bss *b = strongest_bss(m, ssid);

        if (!b) {
            snprintf(err, n, "network not in the last scan; scan again, or pass hidden=true");
            return POCKETIPC_ERR_INVALID_PARAMS;
        }
        sec = b->security;
    }
    join = wifi_join_method(sec, m->sae_capable);
    if (join == WIFI_JOIN_UNSUPPORTED) {
        switch (sec) {
        case WIFI_SEC_WPA3_SAE:
            snprintf(err, n, "WPA3-only networks are not supported by this Wi-Fi driver");
            break;
        case WIFI_SEC_WEP:
            snprintf(err, n, "WEP networks are not supported: WEP is not secure");
            break;
        case WIFI_SEC_ENTERPRISE:
            snprintf(err, n, "enterprise (802.1X) networks are not supported");
            break;
        default:
            snprintf(err, n, "%s networks are not supported", wifi_security_name(sec));
            break;
        }
        return POCKETIPC_ERR_UNSUPPORTED;
    }
    memset(&net, 0, sizeof(net));
    net.ssid = *ssid;
    net.security = sec;
    net.hidden = hidden ? 1 : 0;
    if (join == WIFI_JOIN_OPEN) {
        if (!allow_open) {
            snprintf(err, n, "this network is open (not encrypted); pass allow_open=true to join it");
            return POCKETIPC_ERR_POLICY;
        }
        if (passphrase && passphrase[0]) {
            snprintf(err, n, "an open network takes no passphrase");
            return POCKETIPC_ERR_INVALID_PARAMS;
        }
    } else if ((!passphrase || !passphrase[0]) && (slot = wifi_store_find(&m->store, ssid)) >= 0 &&
               wifi_security_needs_passphrase(m->store.net[slot].security)) {
        /* A saved network is joined again with the passphrase it was saved
         * with; nobody has to type it twice. */
        memcpy(net.passphrase, m->store.net[slot].passphrase, sizeof(net.passphrase));
    } else {
        if (!passphrase || !passphrase[0]) {
            snprintf(err, n, "this network needs a passphrase");
            return POCKETIPC_ERR_INVALID_PARAMS;
        }
        if (wifi_passphrase_valid(passphrase) < 0) {
            snprintf(err, n, "passphrase must be 8..63 printable ASCII characters");
            return POCKETIPC_ERR_INVALID_PARAMS;
        }
        memcpy(net.passphrase, passphrase, strlen(passphrase) + 1);
    }

    /* One attempt at a time: a new one replaces an undecided one. */
    cancel_pending(m, WIFI_REASON_NONE);
    m->latched = WIFI_REASON_NONE;
    slot = wifi_store_find(&m->store, ssid);
    if (slot >= 0 && m->saved_id[slot] >= 0) {
        supp_remove(m, m->saved_id[slot]);
        m->saved_id[slot] = -1;
        m->pending_replaced_slot = slot;
    }
    m->pending_id = supp_add(m, &net, 1);
    if (m->pending_id < 0) {
        wipe_saved(&net);
        if (slot >= 0 && m->saved_id[slot] < 0) {
            m->saved_id[slot] = supp_add(m, &m->store.net[slot], 0);
        }
        m->pending_replaced_slot = -1;
        enable_saved(m);
        snprintf(err, n, "wpa_supplicant refused the network");
        return POCKETIPC_ERR_BACKEND;
    }
    m->pending_net = net;
    wipe_saved(&net);
    m->pending = 1;
    m->pending_not_found = 0;
    m->pending_deadline_ms = now_ms + m->connect_timeout_ms;
    m->status_due_ms = now_ms;
    dhcp_stop(m, now_ms);
    wifi_ssid_to_text(ssid, text, sizeof(text));
    LOG_INFO("wifi: joining \"%s\" (%s%s)", text, wifi_security_name(sec), hidden ? ", hidden" : "");
    derive_state(m, now_ms);

    *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(*result, "accepted", 1);
    add_ssid(*result, "ssid", "ssid_hex", ssid);
    cJSON_AddStringToObject(*result, "security", wifi_security_name(sec));
    return 0;
}

int wifi_mgr_disconnect(struct wifi_mgr *m, long now_ms, cJSON **result, char *err, size_t n)
{
    int code = refuse_unless_ready(m, err, n);

    if (code) {
        return code;
    }
    cancel_pending(m, WIFI_REASON_NONE);
    if (ctrl_ok(m, "DISCONNECT") < 0) {
        snprintf(err, n, "wpa_supplicant did not disconnect");
        return POCKETIPC_ERR_BACKEND;
    }
    LOG_INFO("wifi: disconnected by request");
    m->latched = WIFI_REASON_NONE;
    dhcp_stop(m, now_ms);
    clear_association(m);
    m->status_due_ms = now_ms;
    derive_state(m, now_ms);
    return wifi_mgr_status(m, result, err, n);
}

int wifi_mgr_forget(struct wifi_mgr *m, const struct wifi_ssid *ssid, cJSON **result, char *err,
                    size_t n)
{
    char text[WIFI_SSID_TEXT_MAX];
    int slot = wifi_store_find(&m->store, ssid);
    int i;

    if (slot < 0) {
        snprintf(err, n, "not a saved network");
        return POCKETIPC_ERR_INVALID_PARAMS;
    }
    if (m->ctrl_ready) {
        supp_remove(m, m->saved_id[slot]);
    }
    for (i = slot; i < WIFI_STORE_MAX - 1; i++) {
        m->saved_id[i] = m->saved_id[i + 1];
    }
    m->saved_id[WIFI_STORE_MAX - 1] = -1;
    wifi_store_remove(&m->store, ssid);
    wifi_ssid_to_text(ssid, text, sizeof(text));
    if (store_save(m) < 0) {
        LOG_WARN("wifi: forgot \"%s\" for now, but the store could not be written", text);
    } else {
        LOG_INFO("wifi: forgot \"%s\"", text);
    }
    *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(*result, "forgotten", 1);
    cJSON_AddBoolToObject(*result, "persisted", !m->store_write_failed);
    cJSON_AddNumberToObject(*result, "saved_count", m->store.count);
    return 0;
}

int wifi_mgr_saved(struct wifi_mgr *m, cJSON **result, char *err, size_t n)
{
    cJSON *o = cJSON_CreateObject();
    cJSON *list = cJSON_CreateArray();
    int i;

    (void)err;
    (void)n;
    for (i = 0; i < m->store.count; i++) {
        cJSON *e = cJSON_CreateObject();

        add_ssid(e, "ssid", "ssid_hex", &m->store.net[i].ssid);
        cJSON_AddStringToObject(e, "security", wifi_security_name(m->store.net[i].security));
        cJSON_AddBoolToObject(e, "hidden", m->store.net[i].hidden);
        cJSON_AddItemToArray(list, e);
    }
    cJSON_AddItemToObject(o, "networks", list);
    cJSON_AddStringToObject(o, "store", store_word(m));
    *result = o;
    return 0;
}
