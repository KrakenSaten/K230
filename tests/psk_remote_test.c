/*
 * What of a capture may be read from outside PacketShark
 * (apps/packetshark/psk_remote.h): the `capture.status` answer, made from a
 * model that is filled to the brim with what must never be in it.
 *
 * What is checked: setup has no figure, whatever the model remembers; a
 * running, held, saving and stopped capture and one whose helper ended, each
 * with its state, its reason and its figures; the answer has exactly the
 * keys the contract lists and no other, at any depth; the only texts in it
 * are four enumerated or checked words; nothing of a row, a packet, an
 * address, a name, a file, a sequence number or the helper's own words
 * reaches it; an interface name that is more than a kernel name is dropped;
 * figures are whole numbers however large; the largest answer fits the
 * shell's limit for an app's answer; the state event and the word for a
 * capture that was left; and when the two events are due: a state once per
 * change and never more than four a second, figures once a second while a
 * capture runs and once after it stops, never before their state, and
 * attempts that fail are spaced like the rest.
 *
 * Copyright (c) 2026 Doors authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "psk_remote.h"

#include <cjson/cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failed;
static int checks;
static struct psk_model M;

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

#define POISON "POISON-192.0.2.77-secret.example"
#define MARK 7777777u

/* A text into a field of any size, cut where the field ends. */
static void put(char *dst, size_t n, const char *text)
{
    size_t len = strlen(text);

    if (len >= n) {
        len = n - 1;
    }
    memcpy(dst, text, len);
    dst[len] = 0;
}

/* A model in which everything that is not allowed out says so. */
static void poison(void)
{
    unsigned i;

    memset(&M, 0, sizeof(M));
    M.running = true;
    M.hello = true;
    M.had_capture = true;
    put(M.version, sizeof(M.version), POISON);
    put(M.backend, sizeof(M.backend), "POISON-backend");
    put(M.caps, sizeof(M.caps), POISON);
    put(M.err_code, sizeof(M.err_code), "POISON-code");
    put(M.err_text, sizeof(M.err_text), POISON);
    put(M.save_name, sizeof(M.save_name), "POISON-capture.pcap");
    put(M.save_fail, sizeof(M.save_fail), "POISON-fail");
    M.save = PSK_SAVE_DONE;
    M.save_packets = MARK;
    M.save_total = MARK;
    M.save_bytes = MARK;
    M.speed = MARK;
    M.exit_value = (int)MARK;
    M.stats_ms = MARK;
    M.err_ms = MARK;
    M.row_count = PSK_ROW_CACHE;
    M.rows_first_seq = MARK;
    M.rows_last_seq = MARK;
    M.rows_match = MARK;
    M.want_req = MARK;
    for (i = 0; i < PSK_ROW_CACHE; i++) {
        M.rows[i].seq = MARK;
        M.rows[i].len = MARK;
        put(M.rows[i].src, sizeof(M.rows[i].src), POISON);
        put(M.rows[i].dst, sizeof(M.rows[i].dst), POISON);
        put(M.rows[i].info, sizeof(M.rows[i].info), POISON);
        put(M.rows[i].word, sizeof(M.rows[i].word), "POISON");
    }
    M.have_pkt = true;
    M.pkt.head.seq = MARK;
    M.pkt.head.wire_len = MARK;
    put(M.pkt.src.addr, sizeof(M.pkt.src.addr), POISON);
    put(M.pkt.dst.addr, sizeof(M.pkt.dst.addr), POISON);
    put(M.pkt.src.filter, sizeof(M.pkt.src.filter), "host:" POISON);
    M.pkt.nfields = 1;
    put(M.pkt.field[0].text, sizeof(M.pkt.field[0].text), POISON);
    M.pkt.nhex = 1;
    put(M.pkt.hex[0].hex, sizeof(M.pkt.hex[0].hex), "50 4f 49 53 4f 4e");
    put(M.pkt.hex[0].ascii, sizeof(M.pkt.hex[0].ascii), "POISON");
    M.pkt_next = M.pkt;
    M.pins = MARK;
    M.pin_seq = MARK;
    M.marks_match = MARK;
    M.marks_mask = MARK;
    for (i = 0; i < PSK_BINS; i++) {
        M.bins[i][0] = MARK;
        M.bin_held[i] = MARK;
    }
    M.bin_next = MARK;
    /* Counters the answer carries get small, telling values; the ones it
     * does not carry get the mark. */
    M.have_stats = true;
    M.c.elapsed_ms = 61250;
    M.c.seen = 48211;
    M.c.stored = 48000;
    M.c.held = 11;
    M.c.kdrop = 3;
    M.c.ifdrop = 2;
    M.c.shed = 1;
    M.c.bytes = 31022764;
    M.c.pps = 812;
    M.c.bps = 4120000;
    M.c.fill_permille = 310;
    M.c.first_seq = MARK;
    M.c.last_seq = MARK;
    M.c.match = MARK;
    M.c.ui_skipped = MARK;
    for (i = 0; i < PPC_CLASS_COUNT; i++) {
        M.c.cls[i] = 100 + i;
    }
    for (i = 0; i < PPC_SIGNAL_COUNT; i++) {
        M.c.sig[i] = 20 + i;
    }
    M.have_diag = true;
    M.d.dns_queries = 815;
    M.d.dns_retries = 4;
    M.d.dns_answered = 800;
    M.d.dns_late = 5;
    M.d.dns_noreply = 6;
    M.d.dns_waiting = 7;
    M.d.dns_orphan = MARK;
    M.d.dns_unjudged = MARK;
    M.d.dns_nxdomain = 8;
    M.d.dns_servfail = 9;
    M.d.dns_refused = 10;
    M.d.dns_other = MARK;
    M.d.dns_rtt_sum_us = 18400000;
    M.d.dns_rtt_max_us = 950000;
    M.d.dns_rtt_max_seq = MARK;
    M.d.tcp_attempts = 40;
    M.d.tcp_connected = 38;
    M.d.tcp_timed = 37;
    M.d.tcp_hs_max_us = 120000;
    M.d.tcp_hs_sum_us = 1900000;
    M.d.tcp_hs_max_seq = MARK;
    M.d.tcp_refused = 1;
    M.d.tcp_aborted = 2;
    M.d.tcp_rst_closed = MARK;
    M.d.tcp_rst_unseen = MARK;
    M.d.tcp_segments = MARK;
    M.d.tcp_retrans = MARK;
    M.d.tcp_retrans_acked = MARK;
    M.d.tcp_retrans_unseen = MARK;
    M.d.tcp_syn_again = MARK;
    M.d.tcp_ooo = MARK;
    M.d.tcp_midstream = MARK;
    M.d.tcp_flows = MARK;
    M.d.tcp_oneway = MARK;
    M.d.tcp_replaced = MARK;
    M.d.blinded = MARK;
    M.state = PPC_STATE_CAPTURING;
    M.reason = PPC_REASON_NONE;
    put(M.iface, sizeof(M.iface), "eth0");
}

static cJSON *answer(enum psk_remote_phase phase, bool full, uint32_t limit_s)
{
    struct psk_remote_figures f;

    psk_remote_take(&M, phase, full, limit_s, false, &f);
    return psk_remote_json(&f);
}

static double n(const cJSON *o, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);

    return cJSON_IsNumber(v) ? v->valuedouble : -1;
}

static const char *s(const cJSON *o, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);

    return cJSON_IsString(v) ? v->valuestring : "(none)";
}

static const char *const top_keys[] = {
    "available", "state", "reason", "iface", "keep", "elapsed_ms", "limit_s", "seen", "stored", "held",
    "kernel_drops", "iface_drops", "shed", "bytes", "pps", "bps", "fill_permille", "classes", "findings",
    "diag", "requested_by_companion", NULL,
};
static const char *const string_keys[] = { "state", "reason", "iface", "keep", NULL };

static int in_list(const char *const *list, const char *key)
{
    for (; *list; list++) {
        if (strcmp(*list, key) == 0) {
            return 1;
        }
    }
    return 0;
}

/* The answer is of the contract's shape and of nothing more. */
static int shape_ok(const cJSON *o)
{
    const cJSON *it;
    const cJSON *sub;
    int count = 0;
    unsigned i;

    cJSON_ArrayForEach(it, o)
    {
        if (!it->string || !top_keys[count] || strcmp(it->string, top_keys[count]) != 0) {
            printf("     key %d is %s\n", count, it->string ? it->string : "(null)");
            return 0;
        }
        count++;
        if (strcmp(it->string, "available") == 0 || strcmp(it->string, "requested_by_companion") == 0) {
            if (!cJSON_IsBool(it)) {
                return 0;
            }
        } else if (in_list(string_keys, it->string)) {
            if (!cJSON_IsString(it)) {
                return 0;
            }
        } else if (strcmp(it->string, "classes") == 0 || strcmp(it->string, "diag") == 0) {
            int is_cls = strcmp(it->string, "classes") == 0;

            if (!cJSON_IsObject(it)) {
                return 0;
            }
            i = 0;
            cJSON_ArrayForEach(sub, it)
            {
                const char *want = is_cls ? psk_remote_class_name(i) : psk_remote_diag_name(i);

                if (!want || strcmp(sub->string, want) != 0 || !cJSON_IsNumber(sub)) {
                    return 0;
                }
                i++;
            }
            if (i != (is_cls ? (unsigned)PPC_CLASS_COUNT : (unsigned)PSK_REMOTE_DIAG)) {
                return 0;
            }
        } else if (strcmp(it->string, "findings") == 0) {
            if (!cJSON_IsArray(it) || cJSON_GetArraySize(it) != PPC_SIGNAL_COUNT) {
                return 0;
            }
            cJSON_ArrayForEach(sub, it)
            {
                if (!cJSON_IsNumber(sub)) {
                    return 0;
                }
            }
        } else if (!cJSON_IsNumber(it)) {
            return 0;
        }
    }
    return top_keys[count] == NULL;
}

/* No word, address or number of what must stay is in the text. */
static int clean(const char *text)
{
    char mark[16];

    snprintf(mark, sizeof(mark), "%u", MARK);
    return !strstr(text, "POISON") && !strstr(text, "192.0.2") && !strstr(text, "secret") &&
           !strstr(text, "example") && !strstr(text, ".pcap") && !strstr(text, mark);
}

static int all_zero(const cJSON *o)
{
    const cJSON *it;
    const cJSON *sub;

    cJSON_ArrayForEach(it, o)
    {
        if (cJSON_IsNumber(it) && it->valuedouble != 0) {
            return 0;
        }
        if (cJSON_IsObject(it) || cJSON_IsArray(it)) {
            cJSON_ArrayForEach(sub, it)
            {
                if (sub->valuedouble != 0) {
                    return 0;
                }
            }
        }
    }
    return 1;
}

int main(void)
{
    cJSON *o;
    char *text;
    unsigned i;
    static const enum psk_exit exits[] = { PSK_EXIT_CRASHED, PSK_EXIT_PROTOCOL, PSK_EXIT_UNEXPECTED, PSK_EXIT_START };

    /* ---- setup: no capture --------------------------------------------------- */
    poison();
    o = answer(PSK_REMOTE_IDLE, true, 3600);
    text = cJSON_PrintUnformatted(o);
    check("setup: available, idle, no reason", cJSON_IsTrue(cJSON_GetObjectItem(o, "available")) &&
                                                   strcmp(s(o, "state"), "idle") == 0 &&
                                                   strcmp(s(o, "reason"), "none") == 0);
    check("setup: no interface, HEADERS, no limit and every figure zero, whatever the model remembers",
          strcmp(s(o, "iface"), "") == 0 && strcmp(s(o, "keep"), "headers") == 0 && all_zero(o));
    check("setup: the shape of the contract", shape_ok(o));
    check("setup: nothing of the model's packets", clean(text));
    cJSON_free(text);
    cJSON_Delete(o);

    /* ---- capturing ----------------------------------------------------------- */
    o = answer(PSK_REMOTE_CAPTURE, false, 900);
    text = cJSON_PrintUnformatted(o);
    check("capturing: state, no reason, the interface, HEADERS, the limit",
          strcmp(s(o, "state"), "capturing") == 0 && strcmp(s(o, "reason"), "none") == 0 &&
              strcmp(s(o, "iface"), "eth0") == 0 && strcmp(s(o, "keep"), "headers") == 0 && n(o, "limit_s") == 900);
    check("capturing: the counters, as the model holds them",
          n(o, "elapsed_ms") == 61250 && n(o, "seen") == 48211 && n(o, "stored") == 48000 && n(o, "held") == 11 &&
              n(o, "kernel_drops") == 3 && n(o, "iface_drops") == 2 && n(o, "shed") == 1 &&
              n(o, "bytes") == 31022764 && n(o, "pps") == 812 && n(o, "bps") == 4120000 &&
              n(o, "fill_permille") == 310);
    {
        const cJSON *cls = cJSON_GetObjectItem(o, "classes");
        const cJSON *found = cJSON_GetObjectItem(o, "findings");
        const cJSON *d = cJSON_GetObjectItem(o, "diag");
        int ok = 1;

        for (i = 0; i < PPC_CLASS_COUNT; i++) {
            ok = ok && n(cls, psk_remote_class_name(i)) == 100 + i;
        }
        check("capturing: the eight classes by name, in the protocol's order",
              ok && n(cls, "tcp") == 100 && n(cls, "other") == 107);
        ok = 1;
        for (i = 0; i < PPC_SIGNAL_COUNT; i++) {
            ok = ok && cJSON_GetArrayItem(found, (int)i)->valuedouble == 20 + i;
        }
        check("capturing: the six findings' totals", ok);
        check("capturing: the diagnosis' totals and times",
              n(d, "dns_queries") == 815 && n(d, "dns_retries") == 4 && n(d, "dns_answered") == 800 &&
                  n(d, "dns_late") == 5 && n(d, "dns_noreply") == 6 && n(d, "dns_waiting") == 7 &&
                  n(d, "dns_nxdomain") == 8 && n(d, "dns_servfail") == 9 && n(d, "dns_refused") == 10 &&
                  n(d, "dns_rtt_sum_us") == 18400000 && n(d, "dns_rtt_max_us") == 950000 &&
                  n(d, "tcp_attempts") == 40 && n(d, "tcp_connected") == 38 && n(d, "tcp_timed") == 37 &&
                  n(d, "tcp_hs_sum_us") == 1900000 && n(d, "tcp_hs_max_us") == 120000 && n(d, "tcp_refused") == 1 &&
                  n(d, "tcp_aborted") == 2);
        check("capturing: and not which packet was the slowest",
              !cJSON_GetObjectItem(d, "dns_rtt_max_seq") && !cJSON_GetObjectItem(d, "tcp_hs_max_seq"));
    }
    check("capturing: the shape of the contract", shape_ok(o));
    check("capturing: no row, packet, address, name, file, sequence number or helper text", clean(text));
    check("capturing: no capture was asked for by a companion",
          cJSON_IsFalse(cJSON_GetObjectItem(o, "requested_by_companion")));
    cJSON_free(text);
    cJSON_Delete(o);

    o = answer(PSK_REMOTE_CAPTURE, true, 0);
    check("FULL and no limit are said as they were chosen", strcmp(s(o, "keep"), "full") == 0 && n(o, "limit_s") == 0);
    cJSON_Delete(o);

    /* ---- hold, saving, stopped ----------------------------------------------- */
    M.state = PPC_STATE_HOLD;
    M.reason = PPC_REASON_USER; /* a reason only counts once it has stopped */
    o = answer(PSK_REMOTE_CAPTURE, false, 900);
    check("hold: its state, no reason, its rates",
          strcmp(s(o, "state"), "hold") == 0 && strcmp(s(o, "reason"), "none") == 0 && n(o, "pps") == 812);
    cJSON_Delete(o);
    {
        static const enum ppc_reason why[] = { PPC_REASON_USER, PPC_REASON_LIMIT, PPC_REASON_LINK, PPC_REASON_END,
                                               PPC_REASON_ERROR };
        static const char *const word[] = { "user", "limit", "link", "end", "error" };
        int ok = 1;

        M.state = PPC_STATE_STOPPED;
        for (i = 0; i < 5; i++) {
            M.reason = why[i];
            o = answer(PSK_REMOTE_CAPTURE, false, 900);
            ok = ok && strcmp(s(o, "state"), "stopped") == 0 && strcmp(s(o, "reason"), word[i]) == 0;
            cJSON_Delete(o);
        }
        check("stopped: why, in the helper's five words", ok);
    }
    M.reason = PPC_REASON_LIMIT;
    o = answer(PSK_REMOTE_CAPTURE, false, 900);
    text = cJSON_PrintUnformatted(o);
    check("stopped: the totals stay, the rates are zero",
          n(o, "seen") == 48211 && n(o, "bytes") == 31022764 && n(o, "pps") == 0 && n(o, "bps") == 0);
    check("stopped: shape and nothing more", shape_ok(o) && clean(text));
    cJSON_free(text);
    cJSON_Delete(o);
    M.state = PPC_STATE_SAVING;
    o = answer(PSK_REMOTE_CAPTURE, false, 900);
    text = cJSON_PrintUnformatted(o);
    check("saving: its state, and not the file's name", strcmp(s(o, "state"), "saving") == 0 && clean(text));
    cJSON_free(text);
    cJSON_Delete(o);
    M.state = (enum ppc_state)99;
    o = answer(PSK_REMOTE_CAPTURE, false, 900);
    check("a state this build does not know is idle, with no figure", strcmp(s(o, "state"), "idle") == 0 && all_zero(o));
    cJSON_Delete(o);
    M.state = PPC_STATE_IDLE;
    o = answer(PSK_REMOTE_CAPTURE, false, 900);
    check("a model that is idle is idle, whatever the screen thought",
          strcmp(s(o, "state"), "idle") == 0 && all_zero(o) && strcmp(s(o, "iface"), "") == 0);
    cJSON_Delete(o);

    /* ---- the helper ended ------------------------------------------------------ */
    M.state = PPC_STATE_CAPTURING;
    M.running = false;
    {
        int ok = 1;

        for (i = 0; i < 4; i++) {
            M.exit = exits[i];
            o = answer(PSK_REMOTE_ENDED, false, 900);
            ok = ok && strcmp(s(o, "state"), "ended") == 0 && strcmp(s(o, "reason"), "helper_crashed") == 0;
            cJSON_Delete(o);
        }
        check("ended: a helper that is gone, however it went", ok);
    }
    M.exit = PSK_EXIT_HUNG;
    o = answer(PSK_REMOTE_ENDED, false, 900);
    text = cJSON_PrintUnformatted(o);
    check("ended: a helper that stopped answering", strcmp(s(o, "reason"), "helper_hung") == 0);
    check("ended: the last session's totals, no rate", n(o, "seen") == 48211 && n(o, "pps") == 0 && n(o, "bps") == 0);
    check("ended: shape, and not the helper's exit value or its last words", shape_ok(o) && clean(text));
    cJSON_free(text);
    cJSON_Delete(o);

    /* ---- the interface name ---------------------------------------------------- */
    {
        static const char *const good[] = { "eth0", "eth1", "wlan0", "wlan12", "usb0", "synth0" };
        /* The kernel's predictable names for an adapter are its hardware
         * address; a chosen name can say anything. */
        static const char *const bad[] = { "enx001122aabbcc", "wlx001122aabbcc", "Johns-iPhone", "br-lan",
                                           "eth0.100", "0eth", "eth 0", "a/b", "", "ETH0", "eth", "eth123",
                                           "eth0a", "ethx", "e", "lo", "tun0", "abcdefghijklmno", "wlan0-1",
                                           "caf\xc3\xa9" };
        int ok = 1;

        M.state = PPC_STATE_CAPTURING;
        for (i = 0; i < sizeof(good) / sizeof(good[0]); i++) {
            snprintf(M.iface, sizeof(M.iface), "%s", good[i]);
            o = answer(PSK_REMOTE_CAPTURE, false, 900);
            ok = ok && strcmp(s(o, "iface"), good[i]) == 0;
            cJSON_Delete(o);
        }
        check("an interface name that says only its kind and number is passed on", ok);
        ok = 1;
        for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
            snprintf(M.iface, sizeof(M.iface), "%s", bad[i]);
            o = answer(PSK_REMOTE_CAPTURE, false, 900);
            ok = ok && strcmp(s(o, "iface"), "") == 0 && n(o, "seen") == 48211;
            cJSON_Delete(o);
        }
        check("a name that says more than that is not, and the figures still are", ok);
        /* A name with no end: the check reads at most eight bytes of the
         * sixteen, whatever follows them. */
        memcpy(M.iface, "synth01234567890", sizeof(M.iface));
        o = answer(PSK_REMOTE_CAPTURE, false, 900);
        check("a name with no end is refused", strcmp(s(o, "iface"), "") == 0);
        cJSON_Delete(o);
        memcpy(M.iface, "eth0\0garbage!!!!", sizeof(M.iface));
        o = answer(PSK_REMOTE_CAPTURE, false, 900);
        check("and what lies after a name's end is not part of it", strcmp(s(o, "iface"), "eth0") == 0);
        cJSON_Delete(o);
        snprintf(M.iface, sizeof(M.iface), "%s", "eth0");
    }

    /* ---- figures before the first stats line -------------------------------------- */
    M.have_stats = false;
    M.have_diag = false;
    o = answer(PSK_REMOTE_CAPTURE, false, 900);
    check("capturing before the first figures: the state and the interface, zeros",
          strcmp(s(o, "state"), "capturing") == 0 && strcmp(s(o, "iface"), "eth0") == 0 && n(o, "limit_s") == 900 &&
              n(o, "seen") == 0 && n(cJSON_GetObjectItem(o, "diag"), "dns_queries") == 0);
    cJSON_Delete(o);

    /* ---- the largest answer --------------------------------------------------------- */
    M.have_stats = true;
    M.have_diag = true;
    memset(&M.c, 0xff, sizeof(M.c));
    memset(&M.d, 0xff, sizeof(M.d));
    snprintf(M.iface, sizeof(M.iface), "%s", "abcdefghijklmno");
    o = answer(PSK_REMOTE_CAPTURE, true, 0xffffffffu);
    text = cJSON_PrintUnformatted(o);
    {
        const cJSON *it;
        const cJSON *sub;
        int ok = 1;

        cJSON_ArrayForEach(it, o)
        {
            if (cJSON_IsNumber(it)) {
                ok = ok && it->valuedouble <= (double)PSK_REMOTE_MAX && it->valuedouble >= 0;
            }
            if (cJSON_IsObject(it) || cJSON_IsArray(it)) {
                cJSON_ArrayForEach(sub, it)
                {
                    ok = ok && sub->valuedouble <= (double)PSK_REMOTE_MAX && sub->valuedouble >= 0;
                }
            }
        }
        check("no figure is over the largest that prints whole", ok);
        check("and none is printed with an exponent or a fraction",
              !strstr(text, "e+") && !strstr(text, "E+") && !strchr(text, '.'));
    }
    check("fill is never over 1000", n(o, "fill_permille") == 1000);
    printf("     the largest answer is %zu bytes\n", strlen(text));
    check("the largest answer fits the shell's limit for an app's answer (8192 bytes)", strlen(text) <= 8192);
    check("and is still of the contract's shape", shape_ok(o));
    cJSON_free(text);
    cJSON_Delete(o);

    /* ---- the edges of the function itself --------------------------------------------- */
    {
        struct psk_remote_figures f;

        memset(&f, 0xff, sizeof(f));
        psk_remote_take(NULL, PSK_REMOTE_CAPTURE, true, 900, false, &f);
        o = psk_remote_json(&f);
        check("no model: idle, zeros", strcmp(s(o, "state"), "idle") == 0 && all_zero(o) &&
                                          strcmp(s(o, "keep"), "headers") == 0);
        cJSON_Delete(o);
    }
    check("a state or reason out of range has a word of the list",
          strcmp(psk_remote_state_name(99), "idle") == 0 && strcmp(psk_remote_reason_name(99), "none") == 0);
    check("the lists end", !psk_remote_diag_name(PSK_REMOTE_DIAG) && !psk_remote_class_name(PPC_CLASS_COUNT));

    /* ---- the capture.state event ------------------------------------------------------- */
    poison();
    {
        struct psk_remote_figures f;
        const cJSON *it;
        static const char *const keys[] = { "state", "reason", "iface", "keep", "limit_s" };
        int ok = 1;

        M.state = PPC_STATE_STOPPED;
        M.reason = PPC_REASON_LIMIT;
        psk_remote_take(&M, PSK_REMOTE_CAPTURE, true, 300, false, &f);
        o = psk_remote_state_json(&f);
        text = cJSON_PrintUnformatted(o);
        i = 0;
        cJSON_ArrayForEach(it, o)
        {
            ok = ok && i < 5 && strcmp(it->string, keys[i]) == 0;
            i++;
        }
        check("the state event: five keys, the state and what the capture was started as, no figure",
              ok && i == 5 && strcmp(s(o, "state"), "stopped") == 0 && strcmp(s(o, "reason"), "limit") == 0 &&
                  strcmp(s(o, "iface"), "eth0") == 0 && strcmp(s(o, "keep"), "full") == 0 && n(o, "limit_s") == 300);
        check("the state event: nothing of the model's packets", clean(text));
        cJSON_free(text);
        cJSON_Delete(o);

        psk_remote_left(&f);
        o = psk_remote_json(&f);
        check("left: ended, left, the totals and no rate",
              strcmp(s(o, "state"), "ended") == 0 && strcmp(s(o, "reason"), "left") == 0 && n(o, "seen") == 48211 &&
                  n(o, "pps") == 0 && n(o, "bps") == 0 && shape_ok(o));
        cJSON_Delete(o);
    }

    /* ---- when the events are due ---------------------------------------------------------- */
    {
        struct psk_remote_emit e;
        struct psk_remote_figures f;
        struct psk_remote_figures g;
        int64_t t;
        int states = 0;
        int stats = 0;
        int ahead = 0;
        unsigned due;

        memset(&f, 0, sizeof(f));
        psk_remote_emit_init(&e);
        check("at first the state is due, whatever the clock says",
              psk_remote_due(&e, &f, 0) == PSK_REMOTE_DUE_STATE && psk_remote_due(&e, &f, 5) == PSK_REMOTE_DUE_STATE);
        psk_remote_tried(&e, PSK_REMOTE_DUE_STATE, &f, 0, true);
        check("setup, once said, is not said again and has no figures", psk_remote_due(&e, &f, 1) == 0 &&
                                                                          psk_remote_due(&e, &f, 100000) == 0);

        f.state = PSK_RS_CAPTURING;
        memcpy(f.iface, "eth0", 5);
        f.limit_s = 900;
        check("a change within the gap waits, and no figures go before it", psk_remote_due(&e, &f, 100) == 0);
        check("after the gap the state is due, not the figures", psk_remote_due(&e, &f, 250) == PSK_REMOTE_DUE_STATE);
        psk_remote_tried(&e, PSK_REMOTE_DUE_STATE, &f, 250, false);
        check("a state that could not be raised is due again after the gap, and only then",
              psk_remote_due(&e, &f, 300) == 0 && psk_remote_due(&e, &f, 500) == PSK_REMOTE_DUE_STATE);
        f.state = PSK_RS_HOLD;
        psk_remote_tried(&e, PSK_REMOTE_DUE_STATE, &f, 500, true);
        check("what is raised is the state as it is by then, and its figures follow",
              e.state == PSK_RS_HOLD && psk_remote_due(&e, &f, 501) == PSK_REMOTE_DUE_STATS);
        psk_remote_tried(&e, PSK_REMOTE_DUE_STATS, &f, 501, true);
        check("then figures once a second while held or capturing",
              psk_remote_due(&e, &f, 1400) == 0 && psk_remote_due(&e, &f, 1501) == PSK_REMOTE_DUE_STATS);
        psk_remote_tried(&e, PSK_REMOTE_DUE_STATS, &f, 1501, false);
        check("figures that could not be raised wait a second like any others", psk_remote_due(&e, &f, 1800) == 0 &&
                                                                                  psk_remote_due(&e, &f, 2501) != 0);

        f.state = PSK_RS_STOPPED;
        f.reason = PSK_RR_USER;
        check("a stop is a change", psk_remote_due(&e, &f, 2600) == PSK_REMOTE_DUE_STATE);
        psk_remote_tried(&e, PSK_REMOTE_DUE_STATE, &f, 2600, true);
        check("its totals are owed once", psk_remote_due(&e, &f, 2601) == PSK_REMOTE_DUE_STATS);
        psk_remote_tried(&e, PSK_REMOTE_DUE_STATS, &f, 2601, false);
        check("owed totals that could not be raised are tried again after the gap",
              psk_remote_due(&e, &f, 2700) == 0 && psk_remote_due(&e, &f, 2851) == PSK_REMOTE_DUE_STATS);
        psk_remote_tried(&e, PSK_REMOTE_DUE_STATS, &f, 2851, true);
        check("and a stopped capture then says nothing, however long", psk_remote_due(&e, &f, 2852) == 0 &&
                                                                         psk_remote_due(&e, &f, 9000000) == 0);
        g = f;
        g.reason = PSK_RR_LIMIT;
        check("a reason alone is a change", psk_remote_due(&e, &g, 4000) == PSK_REMOTE_DUE_STATE);
        g = f;
        g.keep_full = true;
        check("so is what the capture is kept as", psk_remote_due(&e, &g, 4000) == PSK_REMOTE_DUE_STATE);
        g = f;
        g.seen = 99;
        g.bytes = 99;
        check("a figure alone is not", psk_remote_due(&e, &g, 4000) == 0);

        /* A helper that changes its state on every poll (33 ms) for ten
         * seconds, and a shell that passes everything on. */
        psk_remote_emit_init(&e);
        memset(&f, 0, sizeof(f));
        for (t = 0; t < 10000; t += 33) {
            f.state = (t / 33) % 2 ? PSK_RS_HOLD : PSK_RS_CAPTURING;
            due = psk_remote_due(&e, &f, t);
            /* Figures are due only for the very state that was raised last. */
            ahead += due == PSK_REMOTE_DUE_STATS && (!e.announced || e.state != f.state);
            states += due == PSK_REMOTE_DUE_STATE;
            stats += due == PSK_REMOTE_DUE_STATS;
            if (due) {
                psk_remote_tried(&e, due, &f, t, true);
            }
        }
        printf("     a flapping helper, 10 s: %d state events, %d stats events\n", states, stats);
        check("a flapping helper makes at most four state events a second, and as few figures",
              states >= 20 && states <= 41 && stats <= 41);
        check("and its figures never carry a state that was not raised", ahead == 0);

        /* A steady capture for a minute. */
        psk_remote_emit_init(&e);
        f.state = PSK_RS_CAPTURING;
        states = stats = 0;
        for (t = 0; t < 60000; t += 33) {
            due = psk_remote_due(&e, &f, t);
            states += due == PSK_REMOTE_DUE_STATE;
            stats += due == PSK_REMOTE_DUE_STATS;
            if (due) {
                psk_remote_tried(&e, due, &f, t, true);
            }
        }
        check("a steady capture: its state once, its figures once a second", states == 1 && stats >= 57 && stats <= 61);

        /* A locked unit for a minute: attempts are spaced too. */
        psk_remote_emit_init(&e);
        states = 0;
        for (t = 0; t < 60000; t += 33) {
            due = psk_remote_due(&e, &f, t);
            states += due != 0;
            if (due) {
                psk_remote_tried(&e, due, &f, t, false);
            }
        }
        check("while nothing can be raised: at most four attempts a second, no figures", states >= 200 && states <= 241 &&
                                                                                           !e.announced);
        psk_remote_emit_init(&e);
        psk_remote_tried(&e, PSK_REMOTE_DUE_STATE, &f, 4000000000ll, true);
        psk_remote_tried(&e, PSK_REMOTE_DUE_STATS, &f, 4000000000ll, true);
        check("a clock that starts again from zero does not stop the figures", psk_remote_due(&e, &f, 5) ==
                                                                                 PSK_REMOTE_DUE_STATS);
    }

    /* ---- a capture started from a start request's sheet, and a request's outcomes ---------- */
    poison();
    {
        struct psk_remote_figures f;

        psk_remote_take(&M, PSK_REMOTE_CAPTURE, false, 300, true, &f);
        o = psk_remote_json(&f);
        text = cJSON_PrintUnformatted(o);
        check("a capture started from a request's sheet says so, and is otherwise as any other",
              cJSON_IsTrue(cJSON_GetObjectItem(o, "requested_by_companion")) && n(o, "seen") == 48211 &&
                  n(o, "limit_s") == 300 && clean(text));
        cJSON_free(text);
        cJSON_Delete(o);
        psk_remote_take(&M, PSK_REMOTE_IDLE, false, 300, true, &f);
        o = psk_remote_json(&f);
        check("setup never says it", cJSON_IsFalse(cJSON_GetObjectItem(o, "requested_by_companion")));
        cJSON_Delete(o);
        check("a request's two outcomes have their words",
              strcmp(psk_remote_reason_name(PSK_RR_REFUSED), "refused") == 0 &&
                  strcmp(psk_remote_reason_name(PSK_RR_TIMEOUT), "timeout") == 0 &&
                  strcmp(psk_remote_reason_name(PSK_RR_LEFT), "left") == 0 &&
                  strcmp(psk_remote_reason_name(PSK_RR_COUNT), "none") == 0);
    }

    printf("psk_remote_test: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
