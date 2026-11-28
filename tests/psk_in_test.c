/*
 * PacketShark's WHEN face, its words and its model (N1.2, DS §59.12):
 * apps/packetshark/psk_in.c and protocol 7 in psk_model.c.
 *
 *   - figures for each comparison, in its own units;
 *   - the evidence line: seen well, seen with limits, not enough evidence
 *     with the engine's reason, notes;
 *   - no problem verdict while automatic verdicts are off (N1.2);
 *   - packet references with their state, gone ones said to have expired;
 *   - the SEEING line: watched, HOLD, losses, losses not known, one-way;
 *   - the model keeps an answer only whole and only for the newest request
 *     of its pair.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "../apps/packetshark/psk_in.h"
#include "../apps/packetshark/psk_model.h"

#include <stdio.h>
#include <string.h>

static int checks;
static int failed;

#define CHECK(what, ok)                                                                                                \
    do {                                                                                                               \
        checks++;                                                                                                      \
        if (ok) {                                                                                                      \
            printf("ok   %s\n", what);                                                                                 \
        } else {                                                                                                       \
            printf("FAIL %s\n", what);                                                                                 \
            failed++;                                                                                                  \
        }                                                                                                              \
    } while (0)

static struct ppc_evt_in_cmp cmp(unsigned metric, unsigned verdict, unsigned quality, unsigned reason)
{
    struct ppc_evt_in_cmp c;

    memset(&c, 0, sizeof(c));
    c.metric = (uint8_t)metric;
    c.verdict = (uint8_t)verdict;
    c.quality = (uint8_t)quality;
    c.reason = (uint8_t)reason;
    return c;
}

static void words(void)
{
    char b[256];
    bool lim;
    bool exp;
    struct ppc_evt_in_cmp c;

    c = cmp(PSK_INM_DNS_TIME, PSK_INV_WORSE, PSK_INQ_GOOD, 0);
    c.n[0] = 60;
    c.n[1] = 25;
    c.stat[0] = 31000;
    c.stat[1] = 120400;
    CHECK("answer time: medians in milliseconds, before and after, and how many answers",
          !strcmp(psk_in_figures(&c, b, sizeof(b)), "MEDIAN 31 MS \xe2\x86\x92 120 MS \xc2\xb7 60 \xe2\x86\x92 25 ANSWERS"));
    CHECK("watched whole, and no word of a verdict while automatic verdicts are off",
          !strcmp(psk_in_evidence(&c, b, sizeof(b), &lim), "WATCHED WHOLE") && !lim && !PSK_IN_VERDICTS &&
              !strcmp(psk_in_verdict(&c, true), ""));
    c = cmp(PSK_INM_DNS_ANSWERED, PSK_INV_CANT_TELL, PSK_INQ_INSUFFICIENT, 9);
    c.n[0] = 60;
    c.k[0] = 60;
    c.n[1] = 50;
    c.k[1] = 40;
    c.notes = 0x02 | 0x04;
    c.und[1] = 2;
    CHECK("answers seen: k of n questions before and after",
          !strcmp(psk_in_figures(&c, b, sizeof(b)), "60 OF 60 \xe2\x86\x92 40 OF 50 QUESTIONS"));
    CHECK("not enough evidence: the engine's reason in words, frames lost, the questions left out, and that an "
          "answer by another path is not seen",
          !strcmp(psk_in_evidence(&c, b, sizeof(b), &lim),
                  "NOT ENOUGH EVIDENCE \xc2\xb7 THE FRAMES LOST COULD ACCOUNT FOR A CHANGE \xc2\xb7 FRAMES LOST \xc2\xb7 2 "
                  "QUESTIONS LEFT OUT \xc2\xb7 AN ANSWER BY ANOTHER PATH IS NOT SEEN HERE") &&
              lim);
    c = cmp(PSK_INM_DNS_ANSWERED, PSK_INV_WORSE, PSK_INQ_GOOD, 0);
    c.n[0] = 66;
    c.k[0] = 66;
    c.n[1] = 50;
    c.k[1] = 31;
    CHECK("watched whole but fewer answers seen: the other path is said, and it counts as a limit",
          !strcmp(psk_in_evidence(&c, b, sizeof(b), &lim),
                  "WATCHED WHOLE \xc2\xb7 AN ANSWER BY ANOTHER PATH IS NOT SEEN HERE") &&
              lim);
    c.k[1] = 50;
    CHECK("as many answers seen as before: no word of another path",
          !strstr(psk_in_evidence(&c, b, sizeof(b), &lim), "ANOTHER PATH"));
    c = cmp(PSK_INM_ARP, PSK_INV_CANT_TELL, PSK_INQ_INSUFFICIENT, 9);
    CHECK("the engine's lost-frames reason on a pair that lost nothing is said as burstiness (finding E1)",
          !strcmp(psk_in_evidence(&c, b, sizeof(b), &lim),
                  "NOT ENOUGH EVIDENCE \xc2\xb7 TOO BURSTY TO TELL A CHANGE FROM ORDINARY VARIATION") &&
              lim);
    c = cmp(PSK_INM_ARP, PSK_INV_NO_CHANGE, PSK_INQ_DEGRADED, 0);
    c.k[0] = 158;
    c.k[1] = 790;
    c.stat[0] = 15800;
    c.stat[1] = 79000;
    c.notes = 0x01;
    CHECK("a rate: a minute's worth before and after, and what was seen",
          !strcmp(psk_in_figures(&c, b, sizeof(b)), "158 \xe2\x86\x92 790 A MINUTE \xc2\xb7 158 \xe2\x86\x92 790 SEEN"));
    CHECK("seen with limits names them",
          !strcmp(psk_in_evidence(&c, b, sizeof(b), &lim), "WATCHED WITH LIMITS \xc2\xb7 NOT ALL OF IT WATCHED") && lim);
    c.stat[0] = 50;
    c.stat[1] = 1250;
    psk_in_figures(&c, b, sizeof(b));
    CHECK("a sparse rate keeps one decimal, a busier one rounds", !strncmp(b, "0.5 \xe2\x86\x92 13 A MINUTE", 18));
    c = cmp(PSK_INM_DNS_FAILED, PSK_INV_PENDING, PSK_INQ_INSUFFICIENT, 0);
    CHECK("a comparison not made yet says so", !strcmp(psk_in_evidence(&c, b, sizeof(b), &lim), "NOT YET COMPARED") && lim);
    c = cmp(PSK_INM_DNS_TIME, PSK_INV_WORSE, PSK_INQ_GOOD, 0);
    c.ref[0] = 812;
    c.alive[0] = 2;
    c.ref[1] = 830;
    c.alive[1] = 0;
    c.ref[3] = 901;
    c.alive[3] = 1;
    CHECK("packet references with their state; a gone one is said to have expired",
          !strcmp(psk_in_refs(&c, b, sizeof(b), &exp),
                  "PACKETS #812 WHOLE \xc2\xb7 #830 GONE (EXPIRED) \xc2\xb7 #901 SUMMARY ONLY") &&
              exp);
    c.alive[1] = 2;
    psk_in_refs(&c, b, sizeof(b), &exp);
    CHECK("no expired reference, none said", !exp);
    memset(&c.ref, 0, sizeof(c.ref));
    CHECK("no reference, no line", !strcmp(psk_in_refs(&c, b, sizeof(b), &exp), "") && !exp);
    {
        unsigned i;
        int names = 1;

        for (i = 0; i < PPC_IN_METRICS; i++) {
            names &= psk_in_metric(i)[0] != '\0';
        }
        CHECK("every metric has a name; none past the five", names && !strcmp(psk_in_metric(5), ""));
    }
}

static struct psk_insight answer(void)
{
    struct psk_insight in;
    unsigned i;

    memset(&in, 0, sizeof(in));
    in.head.req = 3;
    in.head.mark = 1;
    in.head.pair = 1;
    in.head.final = 1;
    in.nvis = 2;
    for (i = 0; i < 2; i++) {
        in.vis[i].side = (uint8_t)i;
        in.vis[i].watched_s = 60;
        in.vis[i].loss_known = 1;
        in.vis[i].own_in = 50;
        in.vis[i].own_out = 60;
    }
    for (i = 0; i < PPC_IN_METRICS; i++) {
        in.cmp[i] = cmp(i, PSK_INV_NO_CHANGE, PSK_INQ_GOOD, 0);
    }
    in.have = (1u << PPC_IN_METRICS) - 1u;
    return in;
}

static void seeing(void)
{
    char b[400];
    bool lim;
    struct psk_insight in = answer();

    CHECK("seeing, both windows watched whole",
          !strcmp(psk_in_seeing(&in, b, sizeof(b), &lim), "BEFORE 60 S WATCHED \xc2\xb7 AFTER 60 S WATCHED") && !lim);
    CHECK("the summary: all observed, all seen well",
          !strcmp(psk_in_summary(&in, b, sizeof(b)), "5 OBSERVED \xc2\xb7 ALL WATCHED WHOLE"));
    in.vis[1].watched_s = 39;
    in.vis[1].held_s = 21;
    in.vis[1].lost_lo = 56;
    in.vis[1].lost_hi = 56;
    psk_in_seeing(&in, b, sizeof(b), &lim);
    CHECK("HOLD and frames lost are said, by window",
          !strcmp(b, "BEFORE 60 S WATCHED \xc2\xb7 AFTER 39 S WATCHED, 21 S ON HOLD, 56 FRAMES LOST") && lim);
    in.vis[0].loss_known = 0;
    in.vis[1].own_in = 0;
    in.vis[1].lost_lo = 10;
    psk_in_seeing(&in, b, sizeof(b), &lim);
    CHECK("losses not known, a bound on losses, and nothing addressed to this device are said",
          strstr(b, "BEFORE 60 S WATCHED, LOSSES NOT KNOWN") && strstr(b, "UP TO 56 FRAMES LOST") &&
              strstr(b, "NOTHING ADDRESSED TO THIS DEVICE SEEN AFTER") && lim);
    in = answer();
    in.cmp[1] = cmp(1, PSK_INV_CANT_TELL, PSK_INQ_INSUFFICIENT, 6);
    in.cmp[4] = cmp(4, PSK_INV_WORSE, PSK_INQ_DEGRADED, 0);
    CHECK("the summary counts the comparisons with limits",
          !strcmp(psk_in_summary(&in, b, sizeof(b)), "5 OBSERVED \xc2\xb7 2 WITH LIMITS"));
    in.head.final = 0;
    CHECK("a window still filling says so",
          !strcmp(psk_in_summary(&in, b, sizeof(b)), "WINDOW STILL FILLING \xc2\xb7 2 WITH LIMITS SO FAR"));
    in.have = 0x0f;
    CHECK("an answer short of a comparison is not whole, and says nothing",
          !psk_in_complete(&in) && !strcmp(psk_in_summary(&in, b, sizeof(b)), ""));
}

static unsigned feed(struct psk_model *m, const char *line)
{
    struct ppc_evt e;

    ppc_evt_parse(line, &e);
    return psk_model_apply(m, &e, 0);
}

static void model(void)
{
    static struct psk_model m;
    char lines[8][PPC_LINE_MAX];
    struct psk_insight in = answer();
    struct ppc_evt_in_head h = in.head;
    unsigned i;
    unsigned ch = 0;

    psk_model_init(&m);
    m.want_in[1] = 3;
    ppc_evt_format_in_begin(lines[0], sizeof(lines[0]), &h);
    ppc_evt_format_in_vis(lines[1], sizeof(lines[1]), &in.vis[0]);
    ppc_evt_format_in_vis(lines[2], sizeof(lines[2]), &in.vis[1]);
    for (i = 0; i < PPC_IN_METRICS; i++) {
        ppc_evt_format_in_cmp(lines[3 + i], sizeof(lines[3 + i]), &in.cmp[i]);
    }
    for (i = 0; i < 8; i++) {
        lines[i][strcspn(lines[i], "\n")] = '\0';
        ch |= feed(&m, lines[i]);
    }
    CHECK("nothing kept before in.end", !m.have_in[1] && !(ch & PSK_CHANGED_INTEL));
    ch = feed(&m, "in.end\t3");
    CHECK("a whole answer to the newest request is kept, for its pair",
          (ch & PSK_CHANGED_INTEL) && m.have_in[1] && !m.have_in[0] && m.insight[1].head.mark == 1 &&
              m.insight[1].nvis == 2 && m.insight[1].have == 0x1f);
    m.want_in[1] = 9;
    for (i = 0; i < 8; i++) {
        feed(&m, lines[i]);
    }
    ch = feed(&m, "in.end\t3");
    CHECK("an answer to an older request is dropped", !(ch & PSK_CHANGED_INTEL));
    h.req = 9;
    ppc_evt_format_in_begin(lines[0], sizeof(lines[0]), &h);
    lines[0][strcspn(lines[0], "\n")] = '\0';
    m.have_in[1] = false;
    for (i = 0; i < 7; i++) {
        feed(&m, lines[i]); /* one comparison short */
    }
    ch = feed(&m, "in.end\t9");
    CHECK("an answer short of a comparison is not kept", !(ch & PSK_CHANGED_INTEL) && !m.have_in[1]);
    psk_model_clear_packets(&m);
    CHECK("a new capture forgets the comparisons", !m.have_in[0] && !m.have_in[1] && m.want_in[1] == 0 && m.in_open == -1);
}

int main(void)
{
    words();
    seeing();
    model();
    printf("psk_in_test: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
