/*
 * Host tests for PacketShark's network intelligence in the shell (M12.1):
 * the net lines of the protocol both ways, the model that takes them, and
 * the words apps/packetshark/psk_net.c makes of them - above all that a
 * sentence never says more than the capture saw.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "psk_model.h"
#include "psk_net.h"

#include <stdio.h>
#include <string.h>

static int failures;
static int checks;

static void check(const char *what, int ok)
{
    checks++;
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) {
        failures++;
    }
}

static struct psk_model M;

/* A line through the parser and into the model, as the session does. */
static unsigned feed(const char *line)
{
    struct ppc_evt e;

    if (ppc_evt_parse(line, &e) == PPC_EVT_BAD) {
        return 0x80000000u;
    }
    return psk_model_apply(&M, &e, 1000);
}

static void totals(const struct ppc_net_totals *t)
{
    char line[PPC_LINE_MAX];

    ppc_evt_format_net(line, sizeof(line), t);
    feed(line);
}

static struct ppc_evt_net_event event(unsigned kind, unsigned sev, const char *ip, const char *mac, const char *ip2,
                                      const char *mac2)
{
    struct ppc_evt_net_event e;

    memset(&e, 0, sizeof(e));
    e.kind = (uint8_t)kind;
    e.sev = (uint8_t)sev;
    e.count = 1;
    e.seq = 100 + kind;
    e.first_us = 40000000ull + kind;
    e.last_us = 42000000ull;
    snprintf(e.ip, sizeof(e.ip), "%s", ip);
    snprintf(e.mac, sizeof(e.mac), "%s", mac);
    snprintf(e.ip2, sizeof(e.ip2), "%s", ip2);
    snprintf(e.mac2, sizeof(e.mac2), "%s", mac2);
    return e;
}

/* Whether a sentence is whole: it ends as a sentence does and was not cut
 * by its buffer. */
static int whole(const char *s, size_t cap)
{
    size_t n = strlen(s);

    return n > 0 && n < cap - 1 && (s[n - 1] == '.' || s[n - 1] == '?');
}

static void test_protocol(void)
{
    struct ppc_net_totals t;
    struct ppc_evt e;
    struct ppc_cmd c;
    char line[PPC_LINE_MAX];
    struct ppc_evt_net_host h;
    struct ppc_evt_net_srv s;
    struct ppc_evt_net_lease l;
    struct ppc_evt_net_event v;
    uint32_t *f = (uint32_t *)&t;
    unsigned i;

    for (i = 0; i < PPC_NET_FIELDS; i++) {
        f[i] = 1000u + i;
    }
    check("the totals line is written", ppc_evt_format_net(line, sizeof(line), &t) > 0);
    check("and read back, every figure in its place",
          ppc_evt_parse(line, &e) == PPC_EVT_NET && memcmp(&e.u.net, &t, sizeof(t)) == 0);
    for (i = 0; i < PPC_NET_FIELDS; i++) {
        f[i] = 0xffffffffu;
    }
    check("the largest totals line fits a line", ppc_evt_format_net(line, sizeof(line), &t) > 0 &&
                                                     strlen(line) < PPC_LINE_MAX && ppc_evt_parse(line, &e) == PPC_EVT_NET);
    check("a totals line with a figure missing is refused", ppc_evt_parse("net\t1\t2\t3\n", &e) == PPC_EVT_BAD);

    check("the request is written and read", ppc_cmd_format_net(line, sizeof(line), 77) > 0 &&
                                                 ppc_cmd_parse(line, &c) == PPC_CMD_NET && c.req == 77);
    check("a request without its number is refused", ppc_cmd_parse("net", &c) == PPC_CMD_BAD);

    memset(&h, 0, sizeof(h));
    snprintf(h.mac, sizeof(h.mac), "02:00:5e:10:00:02");
    snprintf(h.ip, sizeof(h.ip), "192.0.2.20");
    h.flags = PPC_NH_IP | PPC_NH_ROUTER;
    h.pkts = 48417;
    h.bytes = 31400000000ull;
    snprintf(h.name, sizeof(h.name), "desk-pc");
    check("a device is written and read back",
          ppc_evt_format_net_host(line, sizeof(line), &h) > 0 && ppc_evt_parse(line, &e) == PPC_EVT_NET_HOST &&
              memcmp(&e.u.net_host, &h, sizeof(h)) == 0);
    h.ip[0] = '\0';
    h.name[0] = '\0';
    check("one without address or name too",
          ppc_evt_format_net_host(line, sizeof(line), &h) > 0 && ppc_evt_parse(line, &e) == PPC_EVT_NET_HOST &&
              e.u.net_host.ip[0] == '\0' && e.u.net_host.name[0] == '\0');
    snprintf(h.ip, sizeof(h.ip), "192.0.2.x");
    check("an address that is not one is not put on a line", ppc_evt_format_net_host(line, sizeof(line), &h) < 0);
    check("nor taken from one",
          ppc_evt_parse("net.host\t02:00:5e:10:00:02\t<b>\t0\t1\t60\t-\n", &e) == PPC_EVT_BAD &&
              ppc_evt_parse("net.host\tnot-a-mac\t-\t0\t1\t60\t-\n", &e) == PPC_EVT_BAD &&
              ppc_evt_parse("net.host\t02:00:5e:10:00:02\t-\t99\t1\t60\t-\n", &e) == PPC_EVT_BAD);
    check("a name is made printable ASCII",
          ppc_evt_parse("net.host\t02:00:5e:10:00:02\t-\t0\t1\t60\ta\x01\xc3\xa5z\n", &e) == PPC_EVT_NET_HOST &&
              strcmp(e.u.net_host.name, "a...z") == 0);
    check("a name too long for its field is refused",
          ppc_evt_parse("net.host\t02:00:5e:10:00:02\t-\t0\t1\t60\taaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n", &e) == PPC_EVT_BAD);

    memset(&s, 0, sizeof(s));
    snprintf(s.ip, sizeof(s.ip), "192.0.2.1");
    snprintf(s.mac, sizeof(s.mac), "02:00:5e:10:00:fe");
    s.offers = 3;
    s.acks = 4;
    s.naks = 1;
    s.seq = 900;
    s.lease_s = 86400;
    snprintf(s.router, sizeof(s.router), "192.0.2.1");
    snprintf(s.mask, sizeof(s.mask), "255.255.255.0");
    check("a server is written and read back, a missing option as none",
          ppc_evt_format_net_srv(line, sizeof(line), &s) > 0 && ppc_evt_parse(line, &e) == PPC_EVT_NET_SRV &&
              memcmp(&e.u.net_srv, &s, sizeof(s)) == 0 && e.u.net_srv.dns[0] == '\0');

    memset(&l, 0, sizeof(l));
    snprintf(l.ip, sizeof(l.ip), "192.0.2.120");
    snprintf(l.mac, sizeof(l.mac), "02:00:5e:20:00:10");
    snprintf(l.server, sizeof(l.server), "192.0.2.1");
    l.lease_s = 3600;
    l.rel_us = 42000000;
    l.seq = 901;
    check("a lease is written and read back",
          ppc_evt_format_net_lease(line, sizeof(line), &l) > 0 && ppc_evt_parse(line, &e) == PPC_EVT_NET_LEASE &&
              memcmp(&e.u.net_lease, &l, sizeof(l)) == 0);

    v = event(PPC_NK_ARP_DUPLICATE, PPC_NS_WARN, "192.0.2.77", "02:00:5e:30:00:a1", "", "02:00:5e:30:00:a2");
    check("an event is written and read back",
          ppc_evt_format_net_evt(line, sizeof(line), &v) > 0 && ppc_evt_parse(line, &e) == PPC_EVT_NET_EVT &&
              memcmp(&e.u.net_evt, &v, sizeof(v)) == 0);
    v.kind = PPC_NK_COUNT;
    check("an event of a kind there is not is neither written nor read",
          ppc_evt_format_net_evt(line, sizeof(line), &v) < 0 &&
              ppc_evt_parse("net.evt\t99\t1\t1\t5\t0\t0\t-\t-\t-\t-\n", &e) == PPC_EVT_BAD &&
              ppc_evt_parse("net.evt\t0\t1\t1\t5\t0\t0\t-\t-\t-\t-\n", &e) == PPC_EVT_BAD &&
              ppc_evt_parse("net.evt\t1\t2\t1\t5\t0\t0\t-\t-\t-\t-\n", &e) == PPC_EVT_BAD);
}

static void answer(uint32_t req, int end)
{
    char line[PPC_LINE_MAX];
    struct ppc_evt_net_host h;
    struct ppc_evt_net_srv s;
    struct ppc_evt_net_lease l;
    struct ppc_evt_net_event v;

    ppc_evt_format_net_begin(line, sizeof(line), req);
    feed(line);
    memset(&h, 0, sizeof(h));
    snprintf(h.mac, sizeof(h.mac), "02:00:5e:10:00:02");
    snprintf(h.ip, sizeof(h.ip), "192.0.2.1");
    h.flags = PPC_NH_IP | PPC_NH_ROUTER | PPC_NH_DHCP;
    h.pkts = 1204;
    h.bytes = 600000;
    ppc_evt_format_net_host(line, sizeof(line), &h);
    feed(line);
    memset(&h, 0, sizeof(h));
    snprintf(h.mac, sizeof(h.mac), "02:00:5e:10:00:01");
    h.flags = PPC_NH_SELF;
    h.pkts = 300;
    h.bytes = 200000;
    snprintf(h.name, sizeof(h.name), "doors");
    ppc_evt_format_net_host(line, sizeof(line), &h);
    feed(line);
    memset(&s, 0, sizeof(s));
    snprintf(s.ip, sizeof(s.ip), "192.0.2.1");
    snprintf(s.mac, sizeof(s.mac), "02:00:5e:10:00:02");
    s.offers = 3;
    s.acks = 4;
    s.seq = 900;
    s.lease_s = 86400;
    snprintf(s.router, sizeof(s.router), "192.0.2.1");
    snprintf(s.dns, sizeof(s.dns), "192.0.2.53");
    snprintf(s.mask, sizeof(s.mask), "255.255.255.0");
    ppc_evt_format_net_srv(line, sizeof(line), &s);
    feed(line);
    snprintf(s.ip, sizeof(s.ip), "192.0.2.254");
    snprintf(s.mac, sizeof(s.mac), "02:00:5e:10:00:fd");
    s.offers = 1;
    s.acks = 0;
    s.seq = 950;
    ppc_evt_format_net_srv(line, sizeof(line), &s);
    feed(line);
    memset(&l, 0, sizeof(l));
    snprintf(l.ip, sizeof(l.ip), "192.0.2.120");
    snprintf(l.mac, sizeof(l.mac), "02:00:5e:20:00:10");
    snprintf(l.server, sizeof(l.server), "192.0.2.1");
    snprintf(l.name, sizeof(l.name), "desk-pc");
    l.lease_s = 86400;
    l.rel_us = 42000000;
    l.seq = 901;
    ppc_evt_format_net_lease(line, sizeof(line), &l);
    feed(line);
    v = event(PPC_NK_DHCP_SERVERS, PPC_NS_WARN, "192.0.2.254", "02:00:5e:10:00:fd", "192.0.2.1", "");
    ppc_evt_format_net_evt(line, sizeof(line), &v);
    feed(line);
    v = event(PPC_NK_ARP_MOVED, PPC_NS_NOTE, "192.0.2.40", "02:00:5e:30:00:b2", "", "02:00:5e:30:00:b1");
    v.count = 3;
    ppc_evt_format_net_evt(line, sizeof(line), &v);
    feed(line);
    if (end) {
        ppc_evt_format_net_end(line, sizeof(line), req);
        feed(line);
    }
}

static void test_model(void)
{
    struct ppc_net_totals t;

    psk_model_init(&M);
    feed("state\tcapturing\tnone\teth0\t100\t1\n");
    memset(&t, 0, sizeof(t));
    t.hosts = 14;
    totals(&t);
    check("the totals reach the model", M.have_net && M.nt.hosts == 14 && !M.have_net_lists);
    answer(5, 1);
    check("an answer nobody asked for is not taken", !M.have_net_lists && M.net.nhosts == 0);
    M.want_net = 7;
    answer(6, 1);
    check("nor the answer to an older request", !M.have_net_lists);
    answer(7, 0);
    check("half an answer is never shown", !M.have_net_lists && M.net.nhosts == 0);
    answer(7, 1);
    check("the whole answer to the newest request is",
          M.have_net_lists && M.net.nhosts == 2 && M.net.nservers == 2 && M.net.nleases == 1 && M.net.nevents == 2);
    {
        char line[PPC_LINE_MAX];
        struct ppc_evt_net_host h;
        unsigned i;

        M.want_net = 8;
        ppc_evt_format_net_begin(line, sizeof(line), 8);
        feed(line);
        memset(&h, 0, sizeof(h));
        snprintf(h.mac, sizeof(h.mac), "02:00:5e:10:00:02");
        ppc_evt_format_net_host(line, sizeof(line), &h);
        for (i = 0; i < 40; i++) {
            feed(line);
        }
        ppc_evt_format_net_end(line, sizeof(line), 8);
        feed(line);
        check("more devices than the model has room for are left out, not written past it",
              M.net.nhosts == PPC_NET_WIRE_HOSTS);
    }
    psk_model_helper_stopped(&M, PSK_EXIT_CRASHED, 139, 2000);
    check("the lists go with the helper: they name packets it held", !M.have_net_lists && M.net.nhosts == 0);
    M.want_net = 9;
    answer(9, 1);
    feed("state\tidle\tnone\t-\t0\t1\n");
    feed("state\tcapturing\tnone\teth0\t100\t1\n");
    check("a new capture starts with nothing known", !M.have_net && !M.have_net_lists && M.nt.hosts == 0);
}

static void test_words(void)
{
    struct ppc_net_totals t;
    struct psk_diag_line dl[PSK_DIAG_LINES];
    char b[128];
    char note[512];
    unsigned n;
    unsigned i;
    int ok;

    psk_model_init(&M);
    feed("state\tcapturing\tnone\teth0\t100\t1\n");
    check("before anything was seen: OK, and it says so",
          psk_diag_summary(&M, PSK_DIAG_DHCP, b, sizeof(b)) == PSK_SEV_OK && strcmp(b, "NO DHCP SEEN YET") == 0 &&
              psk_diag_summary(&M, PSK_DIAG_ARP, b, sizeof(b)) == PSK_SEV_OK && strcmp(b, "NO ARP SEEN YET") == 0 &&
              psk_diag_summary(&M, PSK_DIAG_NET, b, sizeof(b)) == PSK_SEV_OK && strcmp(b, "NO DEVICE SEEN YET") == 0);
    n = psk_diag_lines(&M, PSK_DIAG_DHCP, dl, PSK_DIAG_LINES);
    check("the DHCP sheet then has one line that explains why", n == 1 && strcmp(dl[0].text, "NO DHCP SEEN") == 0 &&
                                                                    whole(dl[0].why, sizeof(dl[0].why)));
    n = psk_diag_lines(&M, PSK_DIAG_ARP, dl, PSK_DIAG_LINES);
    check("and the ARP sheet", n == 1 && strcmp(dl[0].text, "NO ARP SEEN") == 0 && whole(dl[0].why, sizeof(dl[0].why)));

    memset(&t, 0, sizeof(t));
    t.hosts = 14;
    t.arp_requests = 310;
    t.arp_replies = 4;
    t.arp_binds = 14;
    t.arp_gratuitous = 2;
    t.dhcp_discover = 3;
    t.dhcp_offer = 3;
    t.dhcp_request = 3;
    t.dhcp_ack = 3;
    t.dhcp_tx = 3;
    t.dhcp_complete = 3;
    t.dhcp_servers = 1;
    totals(&t);
    check("a healthy DHCP reads OK", psk_diag_summary(&M, PSK_DIAG_DHCP, b, sizeof(b)) == PSK_SEV_OK &&
                                         strcmp(b, "3 OF 3 ACKNOWLEDGED \xc2\xb7 1 SERVER") == 0);
    check("a healthy ARP reads OK", psk_diag_summary(&M, PSK_DIAG_ARP, b, sizeof(b)) == PSK_SEV_OK &&
                                        strcmp(b, "14 ADDRESSES \xc2\xb7 310 REQUESTS") == 0);
    check("and the overview", psk_diag_summary(&M, PSK_DIAG_NET, b, sizeof(b)) == PSK_SEV_OK &&
                                  strcmp(b, "14 DEVICES \xc2\xb7 1 DHCP SERVER") == 0);
    t.arp_moved = 1;
    t.events_note = 1;
    totals(&t);
    check("an address that moved is a note, not a warning",
          psk_diag_summary(&M, PSK_DIAG_ARP, b, sizeof(b)) == PSK_SEV_NOTE &&
              psk_diag_summary(&M, PSK_DIAG_NET, b, sizeof(b)) == PSK_SEV_NOTE && strstr(b, "1 NOTE") != NULL);
    t.dhcp_unseen = 5;
    totals(&t);
    check("requests that could not be judged leave DHCP OK", psk_diag_summary(&M, PSK_DIAG_DHCP, b, sizeof(b)) == PSK_SEV_OK);

    t.dhcp_servers = 2;
    t.dhcp_no_offer = 1;
    t.dhcp_nak = 1;
    t.dhcp_decline = 1;
    t.dhcp_no_ack = 1;
    t.dhcp_late = 1;
    t.dhcp_waiting = 1;
    t.dhcp_tx = 5;
    t.arp_conflicts = 1;
    t.arp_flips = 2;
    t.arp_noreply = 1;
    t.arp_probes = 3;
    t.arp_other = 1;
    t.events_warn = 4;
    t.blinded = 1;
    t.hosts_replaced = 2;
    t.events_lost = 3;
    totals(&t);
    M.want_net = 3;
    answer(3, 1);
    check("what needs a look comes first on the DHCP line",
          psk_diag_summary(&M, PSK_DIAG_DHCP, b, sizeof(b)) == PSK_SEV_WARN && strncmp(b, "2 SERVERS", 9) == 0 &&
              strstr(b, "2 NO ANSWER") && strstr(b, "1 REFUSED") && strstr(b, "1 DECLINED"));
    check("and on the ARP line", psk_diag_summary(&M, PSK_DIAG_ARP, b, sizeof(b)) == PSK_SEV_WARN &&
                                     strncmp(b, "2 DUPLICATE CLAIMS \xc2\xb7 1 PROBED IN USE", 36) == 0);
    check("and on the overview's", psk_diag_summary(&M, PSK_DIAG_NET, b, sizeof(b)) == PSK_SEV_WARN &&
                                       strncmp(b, "4 TO CHECK", 10) == 0);

    n = psk_diag_lines(&M, PSK_DIAG_DHCP, dl, PSK_DIAG_LINES);
    ok = n >= 10;
    for (i = 0; i < n; i++) {
        ok &= whole(dl[i].why, sizeof(dl[i].why)) && strlen(dl[i].text) < sizeof(dl[i].text) - 1;
        if (!whole(dl[i].why, sizeof(dl[i].why))) {
            printf("     cut: %s / %s\n", dl[i].text, dl[i].why);
        }
    }
    check("every line of the full DHCP sheet is a whole sentence", ok);
    check("with two servers each is to be looked at, whichever was seen first; each opens its latest packet",
          strncmp(dl[1].text, "SERVER 192.0.2.1 ", 17) == 0 && dl[1].sev == PSK_SEV_WARN && dl[1].go == PSK_GO_OPEN &&
              dl[1].seq == 900 && strncmp(dl[2].text, "SERVER 192.0.2.254", 18) == 0 && dl[2].sev == PSK_SEV_WARN &&
              dl[2].seq == 950);
    check("a server's line says what it gives",
          strstr(dl[1].why, "router 192.0.2.1") && strstr(dl[1].why, "DNS 192.0.2.53") &&
              strstr(dl[1].why, "mask 255.255.255.0") && strstr(dl[1].why, "leases of 24 h"));
    check("a lease names the address, the client, the time and the name, and opens its ACK",
          strcmp(dl[3].text, "192.0.2.120 LEASED TO 02:00:5e:20:00:10") == 0 && dl[3].go == PSK_GO_OPEN &&
              dl[3].seq == 901 && strstr(dl[3].why, "for 24 h") && strstr(dl[3].why, "desk-pc") &&
              strstr(dl[3].why, "00:42"));
    check("the summary line lists every DHCP packet", dl[0].go == PSK_GO_HOST &&
                                                          dl[0].classes == (1u << PPC_CLASS_DHCP) && dl[0].host[0] == '\0');
    {
        int unanswered = 0;
        int unjudged = 0;

        for (i = 0; i < n; i++) {
            if (strstr(dl[i].text, "NOT ANSWERED IN 10 S")) {
                unanswered += dl[i].sev == PSK_SEV_WARN && strstr(dl[i].why, "Not proof of a fault") != NULL;
            }
            if (strstr(dl[i].text, "NOT JUDGED")) {
                unjudged += dl[i].sev == PSK_SEV_NOTE;
            }
        }
        check("an unanswered request is a warning that says it is not proof", unanswered == 2);
        check("requests whose answer cannot pass here are a note, never called unanswered", unjudged == 1);
    }

    n = psk_diag_lines(&M, PSK_DIAG_ARP, dl, PSK_DIAG_LINES);
    ok = n == 8;
    for (i = 0; i < n; i++) {
        ok &= whole(dl[i].why, sizeof(dl[i].why)) && strlen(dl[i].text) < sizeof(dl[i].text) - 1;
        if (!whole(dl[i].why, sizeof(dl[i].why))) {
            printf("     cut: %s / %s\n", dl[i].text, dl[i].why);
        }
    }
    check("every line of the full ARP sheet is a whole sentence", ok);
    check("two MACs for one address is a warning, a probed address in use and a move are notes, gratuitous ARP and probes "
          "as they should be",
          dl[1].sev == PSK_SEV_WARN && strstr(dl[1].text, "TWO MACS") && dl[2].sev == PSK_SEV_NOTE &&
              strstr(dl[2].text, "PROBED") && strstr(dl[2].why, "switches probe") &&
              dl[3].sev == PSK_SEV_NOTE && strstr(dl[3].text, "MOVE") && dl[5].sev == PSK_SEV_OK &&
              strstr(dl[5].text, "GRATUITOUS") && dl[6].sev == PSK_SEV_OK && strstr(dl[6].text, "PROBE"));

    psk_diag_confidence(&M, PSK_DIAG_NET, note, sizeof(note));
    check("the note says it is passive, partial, and what was let go",
          strstr(note, "ETH0") && strstr(note, "Passive") && strstr(note, "evidence, not the whole network") &&
              strstr(note, "unjudged 1 time ") && strstr(note, "2 forgotten") && strstr(note, "3 older events") &&
              whole(note, sizeof(note)));
}

static void test_events(void)
{
    struct psk_diag_line dl[PSK_DIAG_LINES];
    struct ppc_evt_net_event e;
    unsigned kind;
    unsigned n;
    unsigned i;
    int all_whole = 1;
    int four = 1;
    int no_overclaim = 1;
    int addressed = 1;

    for (kind = 1; kind < PPC_NK_COUNT; kind++) {
        e = event(kind, kind == PPC_NK_ARP_MOVED ? PPC_NS_NOTE : PPC_NS_WARN, "192.0.2.77", "02:00:5e:30:00:a1",
                  "192.0.2.1", "02:00:5e:30:00:a2");
        n = psk_net_event_lines(&e, dl, PSK_DIAG_LINES);
        four &= n == 4 && strcmp(dl[0].text, psk_net_event_title(kind)) == 0 && strcmp(dl[1].text, "WHAT IT MAY MEAN") == 0;
        for (i = 0; i < n; i++) {
            all_whole &= whole(dl[i].why, sizeof(dl[i].why)) && strlen(dl[i].text) < sizeof(dl[i].text) - 1;
            if (!whole(dl[i].why, sizeof(dl[i].why))) {
                printf("     cut (%u): %s\n", kind, dl[i].why);
            }
        }
        /* What was seen names an address or a MAC of the event. */
        addressed &= strstr(dl[0].why, "192.0.2.77") || strstr(dl[0].why, "02:00:5e:30:00:a1");
        /* Only what the protocol itself reports is called confirmed: a
         * client's DECLINE. */
        if (kind != PPC_NK_DHCP_DECLINE) {
            no_overclaim &= strstr(dl[0].why, "onfirmed") == NULL && strstr(dl[1].why, "onfirmed") == NULL;
        }
        /* And nothing is called an attack: at most what one looks like. */
        no_overclaim &= strstr(dl[0].why, "attack") == NULL && strstr(dl[0].why, "spoof") == NULL;
        four &= dl[2].go == PSK_GO_OPEN && dl[2].seq == e.seq && dl[3].go == PSK_GO_HOST;
    }
    check("every event has four lines: what was seen, what it may mean, its packet, the packets around it", four);
    check("every one of them a whole sentence", all_whole);
    check("what was seen names the event's addresses", addressed);
    check("only a client's DECLINE is called confirmed; nothing is called an attack", no_overclaim);

    e = event(PPC_NK_ARP_DUPLICATE, PPC_NS_WARN, "192.0.2.77", "02:00:5e:30:00:a1", "", "02:00:5e:30:00:a2");
    n = psk_net_event_lines(&e, dl, PSK_DIAG_LINES);
    check("a duplicate is called potential and says what else it could be",
          dl[0].sev == PSK_SEV_WARN && strstr(dl[1].why, "potential duplicate") && strstr(dl[1].why, "failover") &&
              strstr(dl[0].why, "02:00:5e:30:00:a1") && strstr(dl[0].why, "02:00:5e:30:00:a2"));
    check("its packets are the ARP messages that name the address",
          dl[3].classes == (1u << PPC_CLASS_ARP) && strcmp(dl[3].host, "192.0.2.77") == 0 &&
              strcmp(dl[3].text, "ARP PACKETS OF 192.0.2.77") == 0);
    check("seen once says so, with its time", strstr(dl[2].why, "Seen once, at 00:42") != NULL);
    e.count = 12;
    psk_net_event_lines(&e, dl, PSK_DIAG_LINES);
    check("seen again says how often, since when and until when",
          strstr(dl[2].why, "Seen 12 times, first at 00:40 and last at 00:42") != NULL);
    e = event(PPC_NK_DHCP_NO_OFFER, PPC_NS_WARN, "", "02:00:5e:20:00:10", "", "");
    psk_net_event_lines(&e, dl, PSK_DIAG_LINES);
    check("an unanswered DISCOVER says why it could be judged, and that it is not proof",
          strstr(dl[1].why, "Judged only because") && strstr(dl[1].why, "Not proof of a fault") &&
              dl[3].classes == (1u << PPC_CLASS_DHCP) && dl[3].host[0] == '\0');
    e = event(PPC_NK_ARP_ROUTER_MOVED, PPC_NS_WARN, "192.0.2.1", "02:00:5e:66:00:01", "", "02:00:5e:10:00:fe");
    psk_net_event_lines(&e, dl, PSK_DIAG_LINES);
    check("the router at another MAC names both readings: a replaced router, or what spoofing looks like",
          strstr(dl[1].why, "replaced or failed-over router") && strstr(dl[1].why, "looks like"));
    memset(&e, 0, sizeof(e));
    check("an event of no kind has no lines", psk_net_event_lines(&e, dl, PSK_DIAG_LINES) == 0 &&
                                                  psk_net_event_lines(NULL, dl, PSK_DIAG_LINES) == 0);
}

static void test_findings(void)
{
    struct psk_find f[16];
    struct ppc_net_totals t;
    struct ppc_evt_net_event e;
    char l1[128];
    char l2[128];
    char b[160];
    unsigned n;

    psk_model_init(&M);
    feed("state\tcapturing\tnone\teth0\t100\t1\n");
    check("no finding before anything was seen", psk_findings(&M, f, 16) == 0);
    memset(&t, 0, sizeof(t));
    t.hosts = 2;
    totals(&t);
    M.want_net = 3;
    answer(3, 1);
    /* A DNS diagnosis with one warning line: a query nobody answered. */
    feed("stats\t60000\t1504\t1504\t0\t0\t0\t0\t800000\t10\t100\t1\t1504\t1504\t10\t0\t900\t300\t100\t50\t100\t54\t0\t0\t0\t0\t0"
         "\t0\t0\t1\n");
    feed("diag\t4\t0\t3\t0\t1\t0\t0\t0\t0\t0\t0\t0\t60000\t30000\t88\t0\t0\t0\t0\t0\t0\t0\t0\t0\t0\t0\t0\t0\t0\t0\t0\t0\t0\t0"
         "\t0\t0\n");
    n = psk_findings(&M, f, 16);
    check("the events and the DNS diagnosis's warning are one list, warnings first, notes last",
          n == 3 && f[0].sev == PSK_SEV_WARN && f[0].kind == PSK_DIAG_FINDING && f[1].sev == PSK_SEV_WARN &&
              f[1].kind == PSK_DIAG_DNS && f[2].sev == PSK_SEV_NOTE && f[2].kind == PSK_DIAG_FINDING);
    check("an event's row: its title, then its addresses, its time and how often",
          strcmp(f[0].text, "SECOND DHCP SERVER ANSWERING") == 0 &&
              strcmp(f[0].sub, "192.0.2.254 \xc2\xb7 00:42") == 0 &&
              strcmp(f[2].sub, "192.0.2.40 \xc2\xb7 00:42 \xc2\xb7 3 TIMES") == 0);
    check("a diagnosis's row says where it is from", strcmp(f[1].text, "DNS \xc2\xb7 1 NOT ANSWERED WITHIN 5 S") == 0 &&
                                                         strcmp(f[1].sub, "FROM THE DNS DIAGNOSIS") == 0);
    check("cut to what was asked for", psk_findings(&M, f, 1) == 1);

    e = f[2].ev;
    e.count = 1;
    snprintf(e.mac, sizeof(e.mac), "02:00:5e:30:00:00");
    check("an event is found again by its kind, address and first time, whatever else changed",
          psk_net_event_same(&M, &e) == &M.net.evt[1] && psk_net_event_same(&M, &e)->count == 3);
    e.first_us++;
    check("and not when it is another one", psk_net_event_same(&M, &e) == NULL);

    psk_net_host_lines(&M, 0, l1, sizeof(l1), l2, sizeof(l2));
    check("a device's lines: address and roles, then MAC, frames, bytes and share",
          strcmp(l1, "192.0.2.1 \xc2\xb7 ROUTER \xc2\xb7 DHCP SERVER") == 0 &&
              strcmp(l2, "02:00:5e:10:00:02 \xc2\xb7 1 204 PKT \xc2\xb7 600 KB \xc2\xb7 75%") == 0);
    psk_net_host_lines(&M, 1, l1, sizeof(l1), l2, sizeof(l2));
    check("one without an address is named by its MAC, and says so",
          strcmp(l1, "02:00:5e:10:00:01 \xc2\xb7 THIS DEVICE \xc2\xb7 doors") == 0 &&
              strncmp(l2, "NO IPV4 ADDRESS SEEN \xc2\xb7 300 PKT", 31) == 0);
    check("its packets are listed by its address; without one it offers no list (a MAC does not filter IP packets)",
          strcmp(psk_net_host_filter(&M, 0), "192.0.2.1") == 0 && psk_net_host_filter(&M, 1)[0] == '\0' &&
              psk_net_host_filter(&M, 9)[0] == '\0');
    check("the share is of the session's bytes, in tenths of a percent",
          psk_net_host_share(&M, 0) == 750 && psk_net_host_share(&M, 1) == 250 && psk_net_host_share(&M, 9) == 0);
    psk_net_host_lines(&M, 9, l1, sizeof(l1), l2, sizeof(l2));
    check("a device there is not has no lines", l1[0] == '\0' && l2[0] == '\0');

    check("the classes' shares, largest first", strncmp(psk_net_mix(&M, b, sizeof(b)), "TCP 60% \xc2\xb7 UDP 20%", 18) == 0);
    psk_model_init(&M);
    check("and none before the first packet", strcmp(psk_net_mix(&M, b, sizeof(b)), "NO PACKET YET") == 0);

    check("lease times read as people say them",
          strcmp(psk_fmt_lease(45, b, sizeof(b)), "45 S") == 0 && strcmp(psk_fmt_lease(1800, b, sizeof(b)), "30 MIN") == 0 &&
              strcmp(psk_fmt_lease(86400, b, sizeof(b)), "24 H") == 0 &&
              strcmp(psk_fmt_lease(604800, b, sizeof(b)), "7 D") == 0);
}

int main(void)
{
    test_protocol();
    test_model();
    test_words();
    test_events();
    test_findings();
    printf("psk_net_test: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
