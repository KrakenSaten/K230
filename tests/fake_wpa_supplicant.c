/*
 * A stand-in for wpa_supplicant 2.11's control interface, for
 * tests/netd_test.sh. It speaks the same datagram protocol on the same
 * socket path (ctrl_interface=DIR from the -c file, one socket per -i
 * interface), accepts the commands netd sends and produces the events
 * wpa_supplicant produces, with the radio replaced by a scenario file:
 *
 *   bss <bssid> <freq> <signal> <flags> <ssid hex or ->   a network in range
 *   hidden <bssid> <freq> <signal> <flags> <ssid hex>     in range, SSID not broadcast
 *   key <ssid hex> <passphrase hex>                        the passphrase that works
 *   reject <ssid hex>                                      the AP refuses association
 *   sae 1                                                  the driver reports SAE
 *   late_scan                                              SCAN_RESULTS is answered late
 *   busy <CMD> <n> <ms>                                    stalls once, after <CMD> and n more
 *
 * busy models a supplicant whose one thread is stuck in a slow driver call
 * (a scan started by the first ENABLE_NETWORK, after a runtime restart on
 * unit A): once per process, after it has answered the first <CMD> and then
 * n more commands, it reads nothing for <ms> ms, then works through what
 * queued up in order - late replies to addresses that have moved on, and
 * networks added by an ADD_NETWORK whose answer nobody got. Recorded as
 * "busy for <ms> ms after <cmd>" and "busy over".
 *
 * late_scan models a supplicant that stalls on SCAN_RESULTS: the answer is
 * held, and sent to whoever asked just before the NEXT command is answered -
 * the ordering in which a late reply lands while the client is waiting for
 * something else. Every time, not when the timing happens to fall that way.
 * Recorded as "late SCAN_RESULTS sent before <cmd>: delivered|undeliverable";
 * undeliverable means the address it was sent to no longer exists.
 *
 * The file ($FAKE_WPA_SCENARIO) is read again at every scan and every join
 * attempt, so a test can change the world while netd runs. What the fake
 * saw is appended to $FAKE_WPA_RECORD, one fact per line, and never contains
 * a passphrase: only whether the one received matched.
 *
 * The parsing rules copied from wpa_supplicant are the ones netd depends on:
 * a quoted string value runs from the first to the last '"' on the line, an
 * unquoted ssid is hex, a passphrase must be 8..63 characters.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE
#include "wifi_parse.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define MAX_NET 32
#define MAX_BSS 32
#define MAX_SUBS 8

struct net {
    int used;
    struct wifi_ssid ssid;
    char key_mgmt[32];
    char psk[64];
    int have_psk;
    int scan_ssid;
    int enabled;
    int temp_disabled;
};

struct bss {
    char bssid[18];
    int freq;
    int signal;
    char flags[96];
    struct wifi_ssid ssid;
    int hidden;
};

static struct net nets[MAX_NET];
static int next_id;
static struct bss bsss[MAX_BSS];
static int bss_count;
static int sae;
static int late_scan;
/* busy: the scenario's trigger, and how far this process has got with it. */
static char busy_cmd[32];
static int busy_after;
static int busy_ms;
static int busy_left = -1;
static int busy_done;
/* A SCAN_RESULTS answer being held (late_scan), and who asked for it. */
static char late_text[8192];
static struct sockaddr_un late_to;
static socklen_t late_tolen;
static int late_held;
static char path[108];
static int sock = -1;
static struct sockaddr_un subs[MAX_SUBS];
static socklen_t sub_len[MAX_SUBS];
static int sub_count;
static volatile sig_atomic_t stop;

static char state[32] = "DISCONNECTED";
static int cur_id = -1;
static int cur_bss = -1;
static int user_disconnected;
static int selected = -1;
static long attempt_at;
static long scan_done_at;
static int not_found_sent;

static long now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void record(const char *fmt, ...)
{
    const char *file = getenv("FAKE_WPA_RECORD");
    FILE *f;
    va_list ap;

    if (!file || !(f = fopen(file, "a"))) {
        return;
    }
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

static void on_term(int sig)
{
    (void)sig;
    stop = 1;
}

static void load_scenario(void)
{
    const char *file = getenv("FAKE_WPA_SCENARIO");
    char line[512];
    FILE *f;

    bss_count = 0;
    sae = 0;
    late_scan = 0;
    busy_ms = 0;
    if (!file || !(f = fopen(file, "r"))) {
        return;
    }
    while (fgets(line, sizeof(line), f)) {
        char kind[16], bssid[32], flags[96], hex[80];
        int freq, signal;

        if (sscanf(line, "%15s", kind) != 1) {
            continue;
        }
        if ((strcmp(kind, "bss") == 0 || strcmp(kind, "hidden") == 0) && bss_count < MAX_BSS &&
            sscanf(line, "%*s %31s %d %d %95s %79s", bssid, &freq, &signal, flags, hex) == 5) {
            struct bss *b = &bsss[bss_count];

            memset(b, 0, sizeof(*b));
            snprintf(b->bssid, sizeof(b->bssid), "%.17s", bssid);
            b->freq = freq;
            b->signal = signal;
            snprintf(b->flags, sizeof(b->flags), "%s", flags);
            if (strcmp(hex, "-") != 0) {
                wifi_ssid_from_hex(hex, &b->ssid);
            }
            b->hidden = strcmp(kind, "hidden") == 0;
            bss_count++;
        } else if (strcmp(kind, "sae") == 0) {
            sae = 1;
        } else if (strcmp(kind, "late_scan") == 0) {
            late_scan = 1;
        } else if (strcmp(kind, "busy") == 0 &&
                   sscanf(line, "%*s %31s %d %d", busy_cmd, &busy_after, &busy_ms) != 3) {
            busy_ms = 0;
        }
    }
    fclose(f);
}

/* key <ssid hex> <passphrase hex> / reject <ssid hex> for this SSID. */
static int scenario_lookup(const char *kind, const struct wifi_ssid *ssid, char *out, size_t n)
{
    const char *file = getenv("FAKE_WPA_SCENARIO");
    char line[512];
    char want[WIFI_SSID_HEX_MAX];
    FILE *f;
    int found = 0;

    wifi_ssid_to_hex(ssid, want, sizeof(want));
    if (!file || !(f = fopen(file, "r"))) {
        return 0;
    }
    while (!found && fgets(line, sizeof(line), f)) {
        char k[16], hex[80], value[200];
        int fields = sscanf(line, "%15s %79s %199s", k, hex, value);

        if (fields >= 2 && strcmp(k, kind) == 0 && strcmp(hex, want) == 0) {
            found = 1;
            if (out && fields == 3) {
                size_t i;
                size_t len = strlen(value) / 2;

                for (i = 0; i < len && i + 1 < n; i++) {
                    unsigned v;

                    sscanf(value + i * 2, "%2x", &v);
                    out[i] = (char)v;
                }
                out[i < n ? i : n - 1] = '\0';
            }
        }
    }
    fclose(f);
    return found;
}

static void send_event(const char *fmt, ...)
{
    char msg[512];
    va_list ap;
    int i;

    msg[0] = '<';
    msg[1] = '3';
    msg[2] = '>';
    va_start(ap, fmt);
    vsnprintf(msg + 3, sizeof(msg) - 3, fmt, ap);
    va_end(ap);
    for (i = 0; i < sub_count; i++) {
        sendto(sock, msg, strlen(msg), MSG_DONTWAIT, (struct sockaddr *)&subs[i], sub_len[i]);
    }
}

static struct net *net_by_id(const char *s)
{
    char *end;
    long id = strtol(s, &end, 10);

    if (end == s || id < 0 || id >= MAX_NET || !nets[id].used) {
        return NULL;
    }
    return &nets[id];
}

static void ssid_escaped(const struct wifi_ssid *s, char *out, size_t n)
{
    size_t o = 0;
    uint8_t i;

    for (i = 0; i < s->len && o + 5 < n; i++) {
        uint8_t c = s->bytes[i];

        if (c == '"' || c == '\\') {
            out[o++] = '\\';
            out[o++] = (char)c;
        } else if (c >= 32 && c < 127) {
            out[o++] = (char)c;
        } else {
            o += (size_t)snprintf(out + o, n - o, "\\x%02x", c);
        }
    }
    out[o] = '\0';
}

static void disconnect(int reason, int local)
{
    if (cur_id >= 0) {
        send_event("CTRL-EVENT-DISCONNECTED bssid=%s reason=%d%s", bsss[cur_bss].bssid, reason,
                   local ? " locally_generated=1" : "");
    }
    cur_id = -1;
    cur_bss = -1;
    snprintf(state, sizeof(state), "DISCONNECTED");
}

/* One join attempt at the chosen network. */
static void attempt(void)
{
    struct net *n = NULL;
    char esc[160];
    char want[64];
    int id = selected;
    int i;

    load_scenario();
    if (id < 0) {
        for (i = 0; i < MAX_NET; i++) {
            if (nets[i].used && nets[i].enabled && !nets[i].temp_disabled) {
                id = i;
                break;
            }
        }
    }
    if (id < 0 || !nets[id].used || nets[id].temp_disabled || !nets[id].enabled) {
        return;
    }
    n = &nets[id];
    {
        /* Like wpa_supplicant, the strongest BSS of the network is joined. */
        int best = -1;

        for (i = 0; i < bss_count; i++) {
            if (wifi_ssid_equal(&bsss[i].ssid, &n->ssid) && (!bsss[i].hidden || n->scan_ssid) &&
                (best < 0 || bsss[i].signal > bsss[best].signal)) {
                best = i;
            }
        }
        i = best < 0 ? bss_count : best;
    }
    ssid_escaped(&n->ssid, esc, sizeof(esc));
    if (i == bss_count) {
        send_event("CTRL-EVENT-NETWORK-NOT-FOUND ");
        not_found_sent++;
        attempt_at = now_ms() + 700;
        return;
    }
    if (scenario_lookup("reject", &n->ssid, NULL, 0)) {
        n->temp_disabled = 1;
        send_event("CTRL-EVENT-SSID-TEMP-DISABLED id=%d ssid=\"%s\" auth_failures=1 duration=10 reason=CONN_FAILED",
                   id, esc);
        record("rejected %d", id);
        return;
    }
    if (strcmp(n->key_mgmt, "NONE") != 0) {
        if (!n->have_psk || !scenario_lookup("key", &n->ssid, want, sizeof(want)) ||
            strcmp(want, n->psk) != 0) {
            wifi_ssid_to_hex(&n->ssid, want, sizeof(want));
            record("psk_bad %s", want);
            snprintf(state, sizeof(state), "4WAY_HANDSHAKE");
            n->temp_disabled = 1;
            send_event("CTRL-EVENT-SSID-TEMP-DISABLED id=%d ssid=\"%s\" auth_failures=1 duration=10 reason=WRONG_KEY",
                       id, esc);
            snprintf(state, sizeof(state), "DISCONNECTED");
            return;
        }
        wifi_ssid_to_hex(&n->ssid, want, sizeof(want));
        record("psk_ok %s key_mgmt=%s", want, n->key_mgmt);
    } else {
        wifi_ssid_to_hex(&n->ssid, want, sizeof(want));
        record("open_ok %s", want);
    }
    cur_id = id;
    cur_bss = i;
    selected = -1;
    snprintf(state, sizeof(state), "COMPLETED");
    send_event("CTRL-EVENT-CONNECTED - Connection to %s completed [id=%d id_str=]", bsss[i].bssid, id);
}

/* wpa_supplicant's string rule: "quoted" runs to the last quote, else hex. */
static int parse_value(const char *v, char *out, size_t n, int *quoted)
{
    size_t len = strlen(v);

    *quoted = 0;
    if (v[0] == '"') {
        const char *last = strrchr(v, '"');

        if (last == v || last[1] != '\0') {
            return -1;
        }
        len = (size_t)(last - v - 1);
        if (len >= n) {
            return -1;
        }
        memcpy(out, v + 1, len);
        out[len] = '\0';
        *quoted = 1;
        return (int)len;
    }
    if (len >= n) {
        return -1;
    }
    memcpy(out, v, len + 1);
    return (int)len;
}

static void reply(const struct sockaddr_un *to, socklen_t tolen, const char *text)
{
    sendto(sock, text, strlen(text), MSG_DONTWAIT, (struct sockaddr *)to, tolen);
}

static void handle(char *cmd, const struct sockaddr_un *from, socklen_t fromlen)
{
    char out[8192];
    char *arg;
    int i;

    if (late_held) {
        ssize_t r = sendto(sock, late_text, strlen(late_text), MSG_DONTWAIT,
                           (struct sockaddr *)&late_to, late_tolen);

        late_held = 0;
        record("late SCAN_RESULTS sent before %.*s: %s", (int)strcspn(cmd, " "), cmd,
               r >= 0 ? "delivered" : "undeliverable");
    }

    if (strncmp(cmd, "SET_NETWORK ", 12) == 0) {
        char *id_s = cmd + 12;
        char *field = strchr(id_s, ' ');
        char *value = field ? strchr(field + 1, ' ') : NULL;
        struct net *n;
        char buf[128];
        int quoted;
        int len;

        if (!field || !value) {
            reply(from, fromlen, "FAIL\n");
            return;
        }
        *field++ = '\0';
        *value++ = '\0';
        n = net_by_id(id_s);
        len = parse_value(value, buf, sizeof(buf), &quoted);
        if (!n || len < 0) {
            reply(from, fromlen, "FAIL\n");
            return;
        }
        if (strcmp(field, "ssid") == 0) {
            int rc = quoted ? wifi_ssid_from_text(buf, &n->ssid) : wifi_ssid_from_hex(buf, &n->ssid);

            record("set ssid %s", rc == 0 ? (quoted ? "text" : buf) : "invalid");
            reply(from, fromlen, rc == 0 ? "OK\n" : "FAIL\n");
        } else if (strcmp(field, "psk") == 0) {
            int ok = quoted ? (len >= 8 && len <= 63) : (len == 64);

            if (ok) {
                snprintf(n->psk, sizeof(n->psk), "%s", buf);
                n->have_psk = 1;
            }
            record("set psk %s", ok ? "accepted" : "refused");
            explicit_bzero(buf, sizeof(buf));
            explicit_bzero(value, strlen(value));
            reply(from, fromlen, ok ? "OK\n" : "FAIL\n");
        } else if (strcmp(field, "key_mgmt") == 0 || strcmp(field, "scan_ssid") == 0 ||
                   strcmp(field, "ieee80211w") == 0) {
            if (strcmp(field, "key_mgmt") == 0) {
                snprintf(n->key_mgmt, sizeof(n->key_mgmt), "%s", buf);
            } else if (strcmp(field, "scan_ssid") == 0) {
                n->scan_ssid = atoi(buf);
            }
            record("set %s %s", field, buf);
            reply(from, fromlen, "OK\n");
        } else {
            record("set unknown %s", field);
            reply(from, fromlen, "FAIL\n");
        }
        return;
    }
    record("cmd %.*s", (int)strcspn(cmd, " "), cmd);
    arg = strchr(cmd, ' ');
    if (arg) {
        *arg++ = '\0';
    }
    if (strcmp(cmd, "PING") == 0) {
        reply(from, fromlen, "PONG\n");
    } else if (strcmp(cmd, "ATTACH") == 0) {
        if (sub_count < MAX_SUBS) {
            subs[sub_count] = *from;
            sub_len[sub_count++] = fromlen;
        }
        reply(from, fromlen, "OK\n");
    } else if (strcmp(cmd, "DETACH") == 0) {
        reply(from, fromlen, "OK\n");
    } else if (strcmp(cmd, "GET_CAPABILITY") == 0) {
        reply(from, fromlen, sae ? "NONE WPA-PSK WPA-EAP SAE" : "NONE WPA-PSK WPA-EAP");
    } else if (strcmp(cmd, "SCAN") == 0) {
        if (scan_done_at) {
            reply(from, fromlen, "FAIL-BUSY\n");
        } else {
            scan_done_at = now_ms() + 300;
            reply(from, fromlen, "OK\n");
            send_event("CTRL-EVENT-SCAN-STARTED ");
        }
    } else if (strcmp(cmd, "SCAN_RESULTS") == 0) {
        int o;

        load_scenario();
        o = snprintf(out, sizeof(out), "bssid / frequency / signal level / flags / ssid\n");
        for (i = 0; i < bss_count && o < (int)sizeof(out) - 200; i++) {
            char esc[160];

            if (bsss[i].hidden) {
                esc[0] = '\0';
            } else {
                ssid_escaped(&bsss[i].ssid, esc, sizeof(esc));
            }
            o += snprintf(out + o, sizeof(out) - (size_t)o, "%s\t%d\t%d\t%s\t%s\n", bsss[i].bssid,
                          bsss[i].freq, bsss[i].signal, bsss[i].flags, esc);
        }
        if (late_scan) {
            snprintf(late_text, sizeof(late_text), "%s", out);
            memcpy(&late_to, from, sizeof(late_to));
            late_tolen = fromlen;
            late_held = 1;
            record("late SCAN_RESULTS held");
            return;
        }
        reply(from, fromlen, out);
    } else if (strcmp(cmd, "ADD_NETWORK") == 0) {
        int id = -1;

        for (i = 0; i < MAX_NET; i++) {
            if (!nets[(next_id + i) % MAX_NET].used) {
                id = (next_id + i) % MAX_NET;
                break;
            }
        }
        if (id < 0) {
            reply(from, fromlen, "FAIL\n");
            return;
        }
        next_id = id + 1;
        memset(&nets[id], 0, sizeof(nets[id]));
        nets[id].used = 1;
        snprintf(nets[id].key_mgmt, sizeof(nets[id].key_mgmt), "WPA-PSK");
        snprintf(out, sizeof(out), "%d\n", id);
        reply(from, fromlen, out);
    } else if (strcmp(cmd, "REMOVE_NETWORK") == 0 && arg) {
        struct net *n = net_by_id(arg);

        if (!n) {
            reply(from, fromlen, "FAIL\n");
            return;
        }
        if (cur_id == n - nets) {
            disconnect(3, 1);
        }
        if (selected == n - nets) {
            selected = -1;
        }
        explicit_bzero(n, sizeof(*n));
        reply(from, fromlen, "OK\n");
    } else if ((strcmp(cmd, "ENABLE_NETWORK") == 0 || strcmp(cmd, "SELECT_NETWORK") == 0) && arg) {
        struct net *n = net_by_id(arg);

        if (!n) {
            reply(from, fromlen, "FAIL\n");
            return;
        }
        n->enabled = 1;
        n->temp_disabled = 0;
        if (cmd[0] == 'S') {
            for (i = 0; i < MAX_NET; i++) {
                if (&nets[i] != n && nets[i].used) {
                    nets[i].enabled = 0;
                }
            }
            if (cur_id >= 0 && cur_id != n - nets) {
                disconnect(3, 1);
            }
            selected = (int)(n - nets);
            user_disconnected = 0;
            not_found_sent = 0;
            snprintf(state, sizeof(state), "SCANNING");
            attempt_at = now_ms() + 300;
        } else if (cur_id < 0 && !user_disconnected && !attempt_at) {
            attempt_at = now_ms() + 300;
        }
        reply(from, fromlen, "OK\n");
    } else if (strcmp(cmd, "DISABLE_NETWORK") == 0 && arg) {
        struct net *n = net_by_id(arg);

        if (n) {
            n->enabled = 0;
        }
        reply(from, fromlen, n ? "OK\n" : "FAIL\n");
    } else if (strcmp(cmd, "DISCONNECT") == 0) {
        user_disconnected = 1;
        selected = -1;
        disconnect(3, 1);
        reply(from, fromlen, "OK\n");
    } else if (strcmp(cmd, "RECONNECT") == 0) {
        user_disconnected = 0;
        attempt_at = now_ms() + 300;
        reply(from, fromlen, "OK\n");
    } else if (strcmp(cmd, "LIST_NETWORKS") == 0) {
        int o = snprintf(out, sizeof(out), "network id / ssid / bssid / flags\n");

        for (i = 0; i < MAX_NET && o < (int)sizeof(out) - 200; i++) {
            char esc[160];

            if (!nets[i].used) {
                continue;
            }
            ssid_escaped(&nets[i].ssid, esc, sizeof(esc));
            o += snprintf(out + o, sizeof(out) - (size_t)o, "%d\t%s\tany\t%s\n", i, esc,
                          cur_id == i ? "[CURRENT]" : nets[i].enabled ? "" : "[DISABLED]");
        }
        reply(from, fromlen, out);
    } else if (strcmp(cmd, "STATUS") == 0) {
        if (cur_id >= 0) {
            char esc[160];

            ssid_escaped(&nets[cur_id].ssid, esc, sizeof(esc));
            snprintf(out, sizeof(out),
                     "bssid=%s\nfreq=%d\nssid=%s\nid=%d\nmode=station\npairwise_cipher=CCMP\n"
                     "group_cipher=CCMP\nkey_mgmt=WPA2-PSK\nwpa_state=COMPLETED\naddress=02:00:00:00:00:01\n",
                     bsss[cur_bss].bssid, bsss[cur_bss].freq, esc, cur_id);
        } else {
            snprintf(out, sizeof(out), "wpa_state=%s\naddress=02:00:00:00:00:01\n", state);
        }
        reply(from, fromlen, out);
    } else if (strcmp(cmd, "SIGNAL_POLL") == 0) {
        if (cur_id >= 0) {
            snprintf(out, sizeof(out), "RSSI=%d\nLINKSPEED=65\nNOISE=9999\nFREQUENCY=%d\n",
                     bsss[cur_bss].signal, bsss[cur_bss].freq);
            reply(from, fromlen, out);
        } else {
            reply(from, fromlen, "FAIL\n");
        }
    } else if (strcmp(cmd, "TERMINATE") == 0) {
        reply(from, fromlen, "OK\n");
        stop = 1;
    } else {
        reply(from, fromlen, "UNKNOWN COMMAND\n");
    }
}

/* After each answered command: the busy scenario's countdown, and the stall
 * itself. Stalling means not reading the socket at all - commands queue up
 * in the kernel as they would behind a blocked wpa_supplicant - but SIGTERM
 * still ends it, so netd's restart path can be exercised too. */
static void busy_step(const char *verb)
{
    long until;

    if (busy_ms <= 0 || busy_done) {
        return;
    }
    if (busy_left < 0) {
        if (strcmp(verb, busy_cmd) != 0) {
            return;
        }
        busy_left = busy_after;
    } else {
        busy_left--;
    }
    if (busy_left > 0) {
        return;
    }
    busy_done = 1;
    record("busy for %d ms after %s", busy_ms, verb);
    until = now_ms() + busy_ms;
    while (!stop && now_ms() < until) {
        struct timespec ts = { 0, 20 * 1000000L };

        nanosleep(&ts, NULL);
    }
    record("busy over");
}

int main(int argc, char **argv)
{
    const char *iface = NULL;
    const char *conf = NULL;
    char dir[96] = "";
    char line[256];
    struct sockaddr_un addr;
    FILE *f;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
            iface = argv[++i];
        } else if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            conf = argv[++i];
        } else if (strcmp(argv[i], "-D") == 0 && i + 1 < argc) {
            i++;
        }
    }
    if (!iface || !conf || !(f = fopen(conf, "r"))) {
        fprintf(stderr, "fake_wpa_supplicant: need -i and a readable -c\n");
        return 1;
    }
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "ctrl_interface=", 15) == 0) {
            snprintf(dir, sizeof(dir), "%.*s", (int)strcspn(line + 15, "\n"), line + 15);
        }
    }
    fclose(f);
    if (!dir[0]) {
        fprintf(stderr, "fake_wpa_supplicant: no ctrl_interface in %s\n", conf);
        return 1;
    }
    record("started iface=%s pid=%d", iface, (int)getpid());
    if (getenv("FAKE_WPA_FAIL_START")) {
        record("failing start");
        return 1;
    }
    mkdir(dir, 0770);
    snprintf(path, sizeof(path), "%s/%s", dir, iface);
    unlink(path);
    sock = socket(AF_UNIX, SOCK_DGRAM, 0);
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
    if (sock < 0 || bind(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("fake_wpa_supplicant: bind");
        return 1;
    }
    signal(SIGTERM, on_term);
    signal(SIGINT, on_term);
    load_scenario();

    while (!stop) {
        struct pollfd p = { .fd = sock, .events = POLLIN };
        long now;

        if (poll(&p, 1, 50) > 0) {
            char cmd[4096];
            struct sockaddr_un from;
            socklen_t fromlen = sizeof(from);
            ssize_t n = recvfrom(sock, cmd, sizeof(cmd) - 1, 0, (struct sockaddr *)&from, &fromlen);

            if (n > 0) {
                char verb[32];

                cmd[n] = '\0';
                snprintf(verb, sizeof(verb), "%.*s", (int)strcspn(cmd, " "), cmd);
                handle(cmd, &from, fromlen);
                explicit_bzero(cmd, sizeof(cmd));
                busy_step(verb);
            }
        }
        now = now_ms();
        if (scan_done_at && now >= scan_done_at) {
            scan_done_at = 0;
            send_event("CTRL-EVENT-SCAN-RESULTS ");
        }
        if (attempt_at && now >= attempt_at && cur_id < 0 && !user_disconnected) {
            attempt_at = 0;
            attempt();
        }
    }
    send_event("CTRL-EVENT-TERMINATING ");
    record("terminated");
    unlink(path);
    return 0;
}
