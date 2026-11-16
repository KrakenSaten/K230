/*
 * PacketShark's helper client (apps/packetshark/psk_session.h) against the
 * real helper (tools/capture/pos-capture) on the synthetic and replay
 * sources, and against stand-ins that misbehave.
 *
 *   psk_session_test <path to pos-capture>
 *
 * What is checked: the hello; a capture starts, counts and bins; HOLD
 * counts without storing; a window brings rows in order, only for the
 * newest request; STOP stops counting; DISCARD ends the helper normally;
 * a replayed file ends with every frame seen (T1); a second capture while
 * one runs is refused (the lock); a capture that cannot open its interface
 * is refused and leaves the helper idle; a helper that crashes is reported
 * with its signal; one that stops answering while capturing is killed
 * within the watchdog; one that never says hello is killed at the deadline;
 * a missing helper; protocol damage (over-long and unparsable lines) kills
 * it; abandoning a capturing helper takes at most grace + reap; twenty
 * capture cycles leave no child and no descriptor behind.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE
#include "psk_saves.h"
#include "psk_session.h"

#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int failed;
static int checks;
static const char *helper;
static char dir[128];

static void check(const char *what, int ok)
{
    checks++;
    if (!ok) {
        failed++;
        printf("FAIL %s\n", what);
    } else {
        printf("ok   %s\n", what);
    }
}

static int64_t now_ms(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void sleep_ms(int ms)
{
    struct timespec d = { ms / 1000, (ms % 1000) * 1000000L };

    nanosleep(&d, NULL);
}

/* Poll every 20 ms until cond holds or ms pass; the condition is re-read. */
#define POLL_UNTIL(s, m, cond, ms)                                                                 \
    do {                                                                                           \
        int64_t end_ = now_ms() + (ms);                                                            \
        while (now_ms() < end_) {                                                                  \
            psk_session_poll((s), (m), now_ms());                                                  \
            if (cond) {                                                                            \
                break;                                                                             \
            }                                                                                      \
            sleep_ms(20);                                                                          \
        }                                                                                          \
    } while (0)

static int start(struct psk_session *s, struct psk_model *m, const char *h, const char *source)
{
    struct psk_session_config cfg = { h, source };
    char err[128];

    psk_session_init(s);
    psk_model_init(m);
    if (psk_session_start(s, &cfg, now_ms(), err, sizeof(err)) != 0) {
        printf("     start: %s\n", err);
        return -1;
    }
    psk_model_helper_started(m, now_ms());
    return 0;
}

static int open_fds(void)
{
    DIR *d = opendir("/proc/self/fd");
    struct dirent *e;
    int n = 0;

    while (d && (e = readdir(d)) != NULL) {
        if (e->d_name[0] != '.') {
            n++;
        }
    }
    if (d) {
        closedir(d);
    }
    return n;
}

static int no_child(void)
{
    return waitpid(-1, NULL, WNOHANG) < 0 && errno == ECHILD;
}

static void test_capture(void)
{
    static struct psk_session s;
    static struct psk_model m;
    uint64_t seen;
    uint64_t held;
    uint32_t i;
    int ordered = 1;

    check("the helper starts", start(&s, &m, helper, "synth:400:1") == 0);
    POLL_UNTIL(&s, &m, m.hello && m.state == PPC_STATE_IDLE, 3000);
    check("hello, idle", m.hello && m.state == PPC_STATE_IDLE && m.backend[0]);
    check("capture", psk_session_capture(&s, "synth0", PPC_KEEP_HEADERS, 0, PPC_CFILTER_ALL, 900) == 0);
    POLL_UNTIL(&s, &m, m.have_stats && m.c.seen > 300 && m.bin_next >= 1, 3000);
    check("capturing: counters rise and a second is binned",
          m.state == PPC_STATE_CAPTURING && m.c.seen > 300 && m.c.stored == m.c.seen && m.bin_next >= 1 &&
              strcmp(m.iface, "synth0") == 0);
    check("the helper reported its privilege drop", m.caps[0] != '\0');
    printf("     caps: %s\n", m.caps);
    check("window: the newest 20 rows", psk_session_window(&s, &m, 0, 19, 0) == 0);
    POLL_UNTIL(&s, &m, m.row_count == 20 && !m.rows_open, 2000);
    for (i = 1; i < m.row_count; i++) {
        ordered = ordered && m.rows[i].seq == m.rows[i - 1].seq + 1 && m.rows[i].rel_us >= m.rows[i - 1].rel_us;
    }
    check("20 rows in order, each with a protocol word", m.row_count == 20 && ordered && m.rows[0].word[0]);

    /* M5: a view (TCP only, the newest first), a window of it, a packet of
     * it inspected, its neighbour, a pin. */
    check("a view: TCP, the newest first", psk_session_view(&s, 5, 1, 1u << PPC_CLASS_TCP, "", 0, false) == 0);
    check("a window of the view", psk_session_window_at(&s, &m, PPC_ANCHOR_TAIL, 0, 0, 9) == 0);
    POLL_UNTIL(&s, &m, m.rows_filter_id == 5 && m.row_count == 10 && !m.rows_open, 2000);
    ordered = m.row_count == 10;
    for (i = 0; i < m.row_count; i++) {
        ordered = ordered && m.rows[i].cls == PPC_CLASS_TCP && (i == 0 || m.rows[i].seq < m.rows[i - 1].seq);
    }
    check("ten TCP rows, the newest first, from rank 0", ordered && m.rows_first_rank == 0 && m.rows_match > 10 &&
                                                            m.rows_match < m.c.last_seq);
    {
        uint32_t want = m.rows[3].seq;
        uint32_t next = m.rows[4].seq;

        check("inspect one", psk_session_inspect(&s, &m, want, 0) == 0);
        POLL_UNTIL(&s, &m, m.have_pkt && m.pkt.head.seq == want, 2000);
        check("its head, layers, fields and hex", m.have_pkt && m.pkt.head.seq == want && m.pkt.head.rank < m.pkt.head.match &&
                                                      m.pkt.nlayers >= 4 && m.pkt.nfields > 10 && m.pkt.nhex > 0 &&
                                                      strcmp(m.pkt.layer[3].name, "TCP") == 0 &&
                                                      m.pkt.src.filter[0]);
        check("its neighbour in the view's order", psk_session_inspect(&s, &m, want, 1) == 0);
        POLL_UNTIL(&s, &m, m.have_pkt && m.pkt.head.seq == next, 2000);
        /* Ranks move as packets arrive in a newest-first view: the seq is the test. */
        check("is the next row", m.pkt.head.seq == next);
        check("pin it", psk_session_pin(&s, next, true) == 0);
        POLL_UNTIL(&s, &m, m.pins == 1, 2000);
        check("pinned", m.pins == 1 && m.pin_seq == next && (m.pkt.head.flags & PPC_ROWF_PINNED));
        check("a pinned view", psk_session_view(&s, 6, 0, PPC_CLASSES_ALL, NULL, 0, true) == 0 &&
                                   psk_session_window_at(&s, &m, PPC_ANCHOR_RANK, 0, 0, 9) == 0);
        POLL_UNTIL(&s, &m, m.rows_filter_id == 6 && !m.rows_open, 2000);
        check("holds that packet alone, marked", m.row_count == 1 && m.rows[0].seq == next &&
                                                     (m.rows[0].flags & PPC_ROWF_PINNED));
        psk_session_view(&s, 7, 0, PPC_CLASSES_ALL, NULL, 0, false);
    }
    check("hold", psk_session_hold(&s, true) == 0);
    POLL_UNTIL(&s, &m, m.state == PPC_STATE_HOLD, 2000);
    {
        /* The stats line that came before the state line counted frames
         * stored up to then; take the first one written after it. */
        int64_t after = m.stats_ms;

        POLL_UNTIL(&s, &m, m.stats_ms > after, 2000);
    }
    seen = m.c.stored;
    sleep_ms(600);
    POLL_UNTIL(&s, &m, m.c.held > 100, 2000);
    check("HOLD counts without storing", m.state == PPC_STATE_HOLD && m.c.held > 100 && m.c.stored == seen &&
                                             m.c.seen == m.c.stored + m.c.held + m.c.shed);
    check("resume", psk_session_hold(&s, false) == 0);
    POLL_UNTIL(&s, &m, m.state == PPC_STATE_CAPTURING && m.c.stored > seen + 50, 2000);
    check("storing again", m.state == PPC_STATE_CAPTURING && m.c.stored > seen + 50);
    held = m.c.held;
    check("stop", psk_session_stop(&s) == 0);
    POLL_UNTIL(&s, &m, m.state == PPC_STATE_STOPPED, 2000);
    seen = m.c.seen;
    sleep_ms(500);
    psk_session_poll(&s, &m, now_ms());
    check("stopped by the user: nothing more is counted",
          m.state == PPC_STATE_STOPPED && m.reason == PPC_REASON_USER && m.c.seen == seen && m.c.held == held);
    check("the packets are still there", psk_model_has_packets(&m) && m.c.last_seq >= m.c.first_seq);
    check("discard", psk_session_discard(&s) == 0);
    POLL_UNTIL(&s, &m, !s.running, 2000);
    check("the helper leaves normally", !s.running && m.exit == PSK_EXIT_NORMAL && no_child());
}

/* Findings and SAVE through the session, and the saved captures as the app
 * lists and deletes them (M7, M8). */
static void test_findings_and_save(void)
{
    static struct psk_session s;
    static struct psk_model m;
    struct psk_saved list[4];
    char caps[300];
    char cwd[200];
    char path[400];
    char cmd[400];
    uint32_t count = 0;
    uint64_t bytes = 0;
    uint32_t i;
    int n;
    int ok;
    FILE *f;

    /* Never under /tmp (ADR-016 rule 9): beside where the test runs. */
    snprintf(caps, sizeof(caps), "%s/psk-session-captures-%ld", getcwd(cwd, sizeof(cwd)) ? cwd : ".", (long)getpid());
    setenv("POCKETOS_CAPTURES_DIR", caps, 1);
    check("no folder yet: an empty list", psk_saves_list(list, 4, &count, &bytes) == 0 && count == 0);

    start(&s, &m, helper, "synth:2000:1");
    POLL_UNTIL(&s, &m, m.hello, 3000);
    psk_session_capture(&s, "synth0", PPC_KEEP_HEADERS, 0, PPC_CFILTER_ALL, 900);
    POLL_UNTIL(&s, &m, m.c.sig[0] > 2 && m.c.sig[1] > 2 && m.c.sig[2] > 2 && m.c.sig[3] > 2, 4000);
    check("findings are counted in the stats", m.c.sig[0] > 2 && m.c.sig[1] > 2 && m.c.sig[2] > 2 && m.c.sig[3] > 2);
    check("a view of retransmissions", psk_session_view_signals(&s, 3, 0, PPC_CLASSES_ALL, NULL, 0, false,
                                                                 PPC_SIG_RETRANS) == 0 &&
                                           psk_session_window(&s, &m, 0, 9, 0) == 0);
    POLL_UNTIL(&s, &m, m.rows_filter_id == 3 && m.row_count > 0 && !m.rows_open, 2000);
    ok = m.row_count > 1;
    for (i = 0; i < m.row_count; i++) {
        ok = ok && (m.rows[i].signal & PPC_SIG_RETRANS) && strncmp(m.rows[i].info, "Retransmission ", 15) == 0;
    }
    check("every row is one and says so", ok);
    if (m.row_count > 0) {
        uint32_t want = m.rows[m.row_count - 1u].seq;

        psk_session_inspect(&s, &m, want, 0);
        POLL_UNTIL(&s, &m, m.have_pkt && m.pkt.head.seq == want, 2000);
        check("the inspected packet carries its finding under its TCP layer, naming the packet it repeats",
              m.have_pkt && m.pkt.nfindings == 1 && m.pkt.finding[0].layer < m.pkt.nlayers &&
                  strcmp(m.pkt.layer[m.pkt.finding[0].layer].name, "TCP") == 0 &&
                  strncmp(m.pkt.finding[0].text, "RETRANSMISSION OF #", 19) == 0 &&
                  strstr(m.pkt.finding[0].text, "MS EARLIER") != NULL);
    }
    check("marks: asked for the whole capture", psk_session_view(&s, 4, 0, PPC_CLASSES_ALL, NULL, 0, false) == 0 &&
                                                    psk_session_marks(&s, &m, 40) == 0);
    POLL_UNTIL(&s, &m, m.marks_view == 4 && m.marks_buckets == 40, 2000);
    check("and answered: 40 parts, some with findings", m.marks_view == 4 && m.marks_buckets == 40 && m.marks_mask != 0 &&
                                                            (m.marks_mask >> 40) == 0);

    check("save while capturing is refused", psk_session_save(&s) == 0);
    POLL_UNTIL(&s, &m, strcmp(m.err_code, "state") == 0, 2000);
    check("with a reason, and nothing written", strcmp(m.err_code, "state") == 0 && m.save == PSK_SAVE_NONE &&
                                                    psk_saves_list(list, 4, &count, NULL) == 0);
    psk_session_stop(&s);
    POLL_UNTIL(&s, &m, m.state == PPC_STATE_STOPPED, 2000);
    check("save a stopped capture", psk_session_save(&s) == 0);
    POLL_UNTIL(&s, &m, m.save == PSK_SAVE_DONE || m.save == PSK_SAVE_FAILED, 6000);
    check("saved: every packet, a name the helper made, stopped again",
          m.save == PSK_SAVE_DONE && m.save_packets == m.save_total && m.save_packets > 1000 &&
              ppc_save_name_ok(m.save_name) && m.state == PPC_STATE_STOPPED && m.save_bytes > 24);
    n = psk_saves_list(list, 4, &count, &bytes);
    check("the app lists it: its name and its size", n == 1 && count == 1 && strcmp(list[0].name, m.save_name) == 0 &&
                                                         list[0].bytes == m.save_bytes && bytes == m.save_bytes);
    psk_session_discard(&s);
    POLL_UNTIL(&s, &m, !s.running, 2000);
    check("the file outlives the helper", psk_saves_list(list, 4, &count, NULL) == 1 && no_child());

    /* What is not a capture the helper made is neither listed nor deleted. */
    snprintf(path, sizeof(path), "%s/notes.txt", caps);
    f = fopen(path, "w");
    if (f) {
        fclose(f);
    }
    snprintf(path, sizeof(path), "%s/capture-99999999-999999.pcap", caps);
    check("a link named like a capture", symlink("/etc/hostname", path) == 0);
    n = psk_saves_list(list, 4, &count, NULL);
    check("another file and a link are not listed", n == 1 && count == 1);
    check("nor deleted: a path, another name, a link, a name that is not there",
          psk_saves_delete("../psk_session_test") == -1 && psk_saves_delete("notes.txt") == -1 &&
              psk_saves_delete("capture-99999999-999999.pcap") == -1 && psk_saves_delete("capture-1.pcap") == -1 &&
              psk_saves_delete(NULL) == -1 && access(path, F_OK) == 0);
    check("the capture is deleted by its name", psk_saves_delete(list[0].name) == 0 &&
                                                    psk_saves_list(list, 4, &count, NULL) == 0 && count == 0);
    {
        char b[24];

        check("ages", strcmp(psk_saves_age(1800000000, 1800000030, b, sizeof(b)), "JUST NOW") == 0 &&
                          strcmp(psk_saves_age(1800000000, 1800000600, b, sizeof(b)), "10 MIN AGO") == 0 &&
                          strcmp(psk_saves_age(1800000000, 1800007200, b, sizeof(b)), "2 H AGO") == 0 &&
                          strcmp(psk_saves_age(1800000000, 1800259200, b, sizeof(b)), "3 D AGO") == 0 &&
                          strcmp(psk_saves_age(100, 1800000000, b, sizeof(b)), "") == 0);
    }
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", caps);
    if (system(cmd) != 0) {
        printf("note: could not remove %s\n", caps);
    }
    unsetenv("POCKETOS_CAPTURES_DIR");
}

static void test_replay_and_lock(const char *pcap)
{
    static struct psk_session a;
    static struct psk_session b;
    static struct psk_model ma;
    static struct psk_model mb;
    char src[300];

    snprintf(src, sizeof(src), "file:%s", pcap);
    start(&a, &ma, helper, src);
    POLL_UNTIL(&a, &ma, ma.hello, 3000);
    psk_session_capture(&a, "file0", PPC_KEEP_HEADERS, 0, PPC_CFILTER_ALL, 0);
    POLL_UNTIL(&a, &ma, ma.state == PPC_STATE_STOPPED && ma.have_stats && ma.c.seen == 7, 3000);
    check("a replay ends by itself with every frame seen (T1)",
          ma.state == PPC_STATE_STOPPED && ma.reason == PPC_REASON_END && ma.c.seen == 7 && ma.c.stored == 7);
    psk_session_abandon(&a, PSK_DESTROY_GRACE_MS);

    start(&a, &ma, helper, "synth:100:2");
    start(&b, &mb, helper, "synth:100:3");
    POLL_UNTIL(&a, &ma, ma.hello, 3000);
    POLL_UNTIL(&b, &mb, mb.hello, 3000);
    psk_session_capture(&a, "synth0", PPC_KEEP_HEADERS, 0, PPC_CFILTER_ALL, 0);
    POLL_UNTIL(&a, &ma, ma.state == PPC_STATE_CAPTURING, 2000);
    psk_session_capture(&b, "synth0", PPC_KEEP_HEADERS, 0, PPC_CFILTER_ALL, 0);
    POLL_UNTIL(&b, &mb, mb.err_code[0], 2000);
    check("a second capture while one runs is refused: busy",
          strcmp(mb.err_code, "busy") == 0 && mb.state == PPC_STATE_IDLE);
    psk_session_stop(&a);
    POLL_UNTIL(&a, &ma, ma.state == PPC_STATE_STOPPED, 2000);
    mb.err_code[0] = '\0';
    psk_session_capture(&b, "synth0", PPC_KEEP_HEADERS, 0, PPC_CFILTER_ALL, 0);
    POLL_UNTIL(&b, &mb, mb.state == PPC_STATE_CAPTURING, 2000);
    check("STOP releases the lock: the other one may capture now", mb.state == PPC_STATE_CAPTURING);
    psk_session_capture(&b, "synth0", PPC_KEEP_HEADERS, 0, PPC_CFILTER_ALL, 0);
    POLL_UNTIL(&b, &mb, mb.err_code[0], 2000);
    check("a helper captures once: a second start is refused", strcmp(mb.err_code, "state") == 0);
    psk_session_abandon(&a, PSK_DESTROY_GRACE_MS);
    psk_session_abandon(&b, PSK_DESTROY_GRACE_MS);

    start(&a, &ma, helper, "");
    POLL_UNTIL(&a, &ma, ma.hello, 3000);
    psk_session_capture(&a, "nosuch0", PPC_KEEP_HEADERS, 0, PPC_CFILTER_ALL, 0);
    POLL_UNTIL(&a, &ma, ma.err_code[0], 2000);
    check("an interface that cannot be opened is refused, the helper stays idle",
          (strcmp(ma.err_code, "nodev") == 0 || strcmp(ma.err_code, "nobackend") == 0 ||
           strcmp(ma.err_code, "perm") == 0) &&
              ma.state == PPC_STATE_IDLE && a.running);
    printf("     %s: %s\n", ma.err_code, ma.err_text);
    psk_session_abandon(&a, PSK_DESTROY_GRACE_MS);
    check("no helper left", no_child());
}

static void test_faults(void)
{
    static struct psk_session s;
    static struct psk_model m;
    char t[48];
    char script[256];
    FILE *f;
    int64_t t0;

    /* A crash: the synthetic source raises SIGSEGV after 100 frames. */
    start(&s, &m, helper, "synth:500:1:crash=100");
    POLL_UNTIL(&s, &m, m.hello, 3000);
    psk_session_capture(&s, "synth0", PPC_KEEP_HEADERS, 0, PPC_CFILTER_ALL, 0);
    POLL_UNTIL(&s, &m, !s.running, 3000);
    check("a crash is reported with its signal", !s.running && m.exit == PSK_EXIT_CRASHED && m.exit_value == 139);
    printf("     %s\n", psk_exit_text(m.exit, m.exit_value, t, sizeof(t)));

    /* Silent while capturing: stopped with SIGSTOP. */
    start(&s, &m, helper, "synth:100:1");
    POLL_UNTIL(&s, &m, m.hello, 3000);
    psk_session_capture(&s, "synth0", PPC_KEEP_HEADERS, 0, PPC_CFILTER_ALL, 0);
    POLL_UNTIL(&s, &m, m.state == PPC_STATE_CAPTURING, 2000);
    kill(s.pid, SIGSTOP);
    t0 = now_ms();
    POLL_UNTIL(&s, &m, !s.running, PSK_SILENCE_MS + 2000);
    check("silent while capturing: killed by the watchdog", !s.running && m.exit == PSK_EXIT_HUNG &&
                                                                now_ms() - t0 >= PSK_SILENCE_MS - 300 &&
                                                                now_ms() - t0 < PSK_SILENCE_MS + 1000);

    /* Never says hello: a stand-in that only waits. */
    snprintf(script, sizeof(script), "%s/mute.sh", dir);
    f = fopen(script, "w");
    if (f) {
        fprintf(f, "#!/bin/sh\nexec sleep 30\n");
        fclose(f);
        chmod(script, 0700);
    }
    start(&s, &m, script, NULL);
    t0 = now_ms();
    POLL_UNTIL(&s, &m, !s.running, PSK_HELLO_MS + 2000);
    check("no hello: killed at the deadline", !s.running && m.exit == PSK_EXIT_HUNG &&
                                                   now_ms() - t0 >= PSK_HELLO_MS - 100);

    /* Not there. */
    start(&s, &m, "/nonexistent/pos-capture", NULL);
    POLL_UNTIL(&s, &m, !s.running, 2000);
    check("a missing helper", !s.running && m.exit == PSK_EXIT_START);

    /* Lines that do not parse, then one that never ends. */
    snprintf(script, sizeof(script), "%s/bad.sh", dir);
    f = fopen(script, "w");
    if (f) {
        fprintf(f, "#!/bin/sh\nprintf 'hello\\t1\\tx\\ty\\n'\nwhile :; do printf 'stats\\tx\\n'; sleep 0.02; done\n");
        fclose(f);
        chmod(script, 0700);
    }
    start(&s, &m, script, NULL);
    POLL_UNTIL(&s, &m, !s.running, 4000);
    check("unparsable lines: killed", !s.running && m.exit == PSK_EXIT_PROTOCOL);
    f = fopen(script, "w");
    if (f) {
        fprintf(f, "#!/bin/sh\nwhile :; do printf 'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa'; done\n");
        fclose(f);
    }
    start(&s, &m, script, NULL);
    POLL_UNTIL(&s, &m, !s.running, 3000);
    check("an over-long line: killed", !s.running && m.exit == PSK_EXIT_PROTOCOL);
    unlink(script);

    /* Abandon while capturing. */
    start(&s, &m, helper, "synth:2000:1");
    POLL_UNTIL(&s, &m, m.hello, 3000);
    psk_session_capture(&s, "synth0", PPC_KEEP_HEADERS, 0, PPC_CFILTER_ALL, 0);
    POLL_UNTIL(&s, &m, m.state == PPC_STATE_CAPTURING && m.c.seen > 0, 2000);
    t0 = now_ms();
    psk_session_abandon(&s, PSK_DESTROY_GRACE_MS);
    t0 = now_ms() - t0;
    check("abandon while capturing: gone within 500 ms", t0 <= PSK_DESTROY_GRACE_MS + PSK_KILL_REAP_MS &&
                                                            !s.running && no_child());
    printf("     abandon took %lld ms\n", (long long)t0);
}

static void test_cycles(void)
{
    static struct psk_session s;
    static struct psk_model m;
    int before = open_fds();
    int i;
    int ok = 1;

    for (i = 0; i < 20; i++) {
        start(&s, &m, helper, "synth:300:1");
        POLL_UNTIL(&s, &m, m.hello, 3000);
        psk_session_capture(&s, "synth0", PPC_KEEP_HEADERS, 0, PPC_CFILTER_ALL, 0);
        POLL_UNTIL(&s, &m, m.state == PPC_STATE_CAPTURING && m.c.seen > 0, 2000);
        ok = ok && m.state == PPC_STATE_CAPTURING;
        if (i % 2) {
            psk_session_stop(&s);
            POLL_UNTIL(&s, &m, m.state == PPC_STATE_STOPPED, 2000);
            psk_session_discard(&s);
            POLL_UNTIL(&s, &m, !s.running, 2000);
            ok = ok && !s.running && m.exit == PSK_EXIT_NORMAL;
        } else {
            psk_session_abandon(&s, PSK_DESTROY_GRACE_MS);
        }
    }
    check("twenty capture cycles, each capturing", ok);
    check("and no child or descriptor left behind", no_child() && open_fds() == before);
    printf("     descriptors %d before, %d after\n", before, open_fds());
}

int main(int argc, char **argv)
{
    char cmd[300];

    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGPIPE, SIG_IGN);
    if (argc != 2 || access(argv[1], X_OK) != 0) {
        printf("usage: psk_session_test <pos-capture>\n");
        return 2;
    }
    helper = argv[1];
    snprintf(dir, sizeof(dir), "/tmp/psk-session-%ld", (long)getpid());
    mkdir(dir, 0700);
    setenv("POCKETOS_RUNTIME_DIR", dir, 1);
    setenv("POCKETOS_LOG_DIR", dir, 1);
    setenv("POCKETOS_LOG_STDERR", "0", 1);
    unsetenv("POCKETOS_CAPTURE_SOURCE");
    test_capture();
    test_findings_and_save();
    test_replay_and_lock(getenv("PSK_SELFTEST_PCAP") ? getenv("PSK_SELFTEST_PCAP")
                                                     : "tests/data/packetshark/selftest.pcap");
    test_faults();
    test_cycles();
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
    if (system(cmd) != 0) {
        printf("note: could not remove %s\n", dir);
    }
    printf("psk_session_test: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
