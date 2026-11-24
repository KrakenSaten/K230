/*
 * PacketShark's network intelligence in words. See psk_net.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "psk_net.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define DOT " \xc2\xb7 "

/* ---- small things ----------------------------------------------------------------------- */

static const char *plural(uint32_t n)
{
    return n == 1 ? "" : "S";
}

/* More text after what is there, never past the buffer. */
static void more(char *buf, size_t buflen, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

static void more(char *buf, size_t buflen, const char *fmt, ...)
{
    size_t o = strlen(buf);
    va_list ap;

    if (o + 1 >= buflen) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(buf + o, buflen - o, fmt, ap);
    va_end(ap);
}

/* One more part of a line of parts, after a middle dot when it is not the
 * first. */
static void part(char *buf, size_t buflen, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

static void part(char *buf, size_t buflen, const char *fmt, ...)
{
    size_t o = strlen(buf);
    va_list ap;

    if (o && o + sizeof(DOT) < buflen) {
        memcpy(buf + o, DOT, sizeof(DOT));
        o += sizeof(DOT) - 1;
    }
    if (o + 1 >= buflen) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(buf + o, buflen - o, fmt, ap);
    va_end(ap);
}

static const char *count(uint32_t n, char *buf, size_t buflen)
{
    return psk_fmt_count(n, buf, buflen);
}

/* A time into the capture: "00:42". */
static const char *at(uint64_t us, char *buf, size_t buflen)
{
    return psk_fmt_elapsed(us / 1000u, buf, buflen);
}

const char *psk_fmt_lease(uint32_t s, char *buf, size_t buflen)
{
    if (s < 120u) {
        snprintf(buf, buflen, "%u S", (unsigned)s);
    } else if (s < 2u * 3600u) {
        snprintf(buf, buflen, "%u MIN", (unsigned)(s / 60u));
    } else if (s < 2u * 86400u) {
        snprintf(buf, buflen, "%u H", (unsigned)(s / 3600u));
    } else {
        snprintf(buf, buflen, "%u D", (unsigned)(s / 86400u));
    }
    return buf;
}

static const char *lease_lower(uint32_t s, char *buf, size_t buflen)
{
    char *p;

    psk_fmt_lease(s, buf, buflen);
    for (p = buf; *p; p++) {
        if (*p >= 'A' && *p <= 'Z') {
            *p = (char)(*p - 'A' + 'a');
        }
    }
    return buf;
}

static struct psk_diag_line *add_line(struct psk_diag_line *out, unsigned *n, unsigned max, unsigned sev)
{
    struct psk_diag_line *l;

    if (*n >= max) {
        return NULL;
    }
    l = &out[(*n)++];
    memset(l, 0, sizeof(*l));
    l->sev = (uint8_t)sev;
    return l;
}

static void go_open(struct psk_diag_line *l, uint32_t seq)
{
    if (seq) {
        l->go = PSK_GO_OPEN;
        l->seq = seq;
    }
}

static void go_host(struct psk_diag_line *l, unsigned cls, const char *host)
{
    l->go = PSK_GO_HOST;
    l->classes = (uint8_t)(1u << cls);
    snprintf(l->host, sizeof(l->host), "%s", host ? host : "");
}

/* ---- severities ------------------------------------------------------------------------- */

static unsigned dhcp_sev(const struct psk_model *m)
{
    const struct ppc_net_totals *t = &m->nt;

    if (t->dhcp_no_offer || t->dhcp_no_ack || t->dhcp_nak || t->dhcp_decline || t->dhcp_servers >= 2) {
        return PSK_SEV_WARN;
    }
    return t->dhcp_late ? PSK_SEV_NOTE : PSK_SEV_OK;
}

static unsigned arp_sev(const struct psk_model *m)
{
    const struct ppc_net_totals *t = &m->nt;
    uint32_t i;

    if (t->arp_conflicts || t->arp_flips) {
        return PSK_SEV_WARN;
    }
    for (i = 0; i < m->net.nevents; i++) {
        const struct ppc_evt_net_event *e = &m->net.evt[i];

        if (e->sev == PPC_NS_WARN && (e->kind == PPC_NK_ARP_ROUTER_MOVED || e->kind == PPC_NK_ARP_NO_REPLY)) {
            return PSK_SEV_WARN;
        }
    }
    return t->arp_moved || t->arp_noreply ? PSK_SEV_NOTE : PSK_SEV_OK;
}

/* ---- the summaries ---------------------------------------------------------------------- */

unsigned psk_net_summary(const struct psk_model *m, unsigned kind, char *buf, size_t buflen)
{
    const struct ppc_net_totals *t = &m->nt;
    char a[24];
    char b[24];

    if (!buf || buflen == 0) {
        return PSK_SEV_OK;
    }
    buf[0] = '\0';
    if (kind == PSK_DIAG_DHCP) {
        uint32_t msgs = t->dhcp_discover + t->dhcp_offer + t->dhcp_request + t->dhcp_ack + t->dhcp_nak +
                        t->dhcp_decline + t->dhcp_release + t->dhcp_inform;

        if (!m->have_net || msgs == 0) {
            snprintf(buf, buflen, "NO DHCP SEEN YET");
            return PSK_SEV_OK;
        }
        /* What needs a look first, so a narrow line still shows it. */
        if (t->dhcp_servers >= 2) {
            part(buf, buflen, "%s SERVERS", count(t->dhcp_servers, a, sizeof(a)));
        }
        if (t->dhcp_no_offer + t->dhcp_no_ack) {
            part(buf, buflen, "%s NO ANSWER", count(t->dhcp_no_offer + t->dhcp_no_ack, a, sizeof(a)));
        }
        if (t->dhcp_nak) {
            part(buf, buflen, "%s REFUSED", count(t->dhcp_nak, a, sizeof(a)));
        }
        if (t->dhcp_decline) {
            part(buf, buflen, "%s DECLINED", count(t->dhcp_decline, a, sizeof(a)));
        }
        part(buf, buflen, "%s OF %s ACKNOWLEDGED", count(t->dhcp_complete, a, sizeof(a)), count(t->dhcp_tx, b, sizeof(b)));
        if (t->dhcp_servers == 1) {
            part(buf, buflen, "1 SERVER");
        }
        return dhcp_sev(m);
    }
    if (kind == PSK_DIAG_ARP) {
        if (!m->have_net || t->arp_requests + t->arp_replies + t->arp_other == 0) {
            snprintf(buf, buflen, "NO ARP SEEN YET");
            return PSK_SEV_OK;
        }
        if (t->arp_conflicts) {
            part(buf, buflen, "%s CONFLICT%s", count(t->arp_conflicts, a, sizeof(a)), plural(t->arp_conflicts));
        }
        if (t->arp_flips) {
            part(buf, buflen, "%s DUPLICATE CLAIM%s", count(t->arp_flips, a, sizeof(a)), plural(t->arp_flips));
        }
        if (t->arp_moved) {
            part(buf, buflen, "%s MOVED", count(t->arp_moved, a, sizeof(a)));
        }
        part(buf, buflen, "%s ADDRESS%s", count(t->arp_binds, a, sizeof(a)), t->arp_binds == 1 ? "" : "ES");
        part(buf, buflen, "%s REQUEST%s", count(t->arp_requests, a, sizeof(a)), plural(t->arp_requests));
        return arp_sev(m);
    }
    /* The overview. */
    if (!m->have_net || t->hosts == 0) {
        snprintf(buf, buflen, "NO DEVICE SEEN YET");
        return PSK_SEV_OK;
    }
    if (t->events_warn) {
        part(buf, buflen, "%s TO CHECK", count(t->events_warn, a, sizeof(a)));
    }
    part(buf, buflen, "%s DEVICE%s", count(t->hosts, a, sizeof(a)), plural(t->hosts));
    if (t->dhcp_servers) {
        part(buf, buflen, "%s DHCP SERVER%s", count(t->dhcp_servers, a, sizeof(a)), plural(t->dhcp_servers));
    }
    if (t->events_note) {
        part(buf, buflen, "%s NOTE%s", count(t->events_note, a, sizeof(a)), plural(t->events_note));
    }
    return t->events_warn ? PSK_SEV_WARN : t->events_note ? PSK_SEV_NOTE : PSK_SEV_OK;
}

/* ---- the DHCP sheet --------------------------------------------------------------------- */

static unsigned dhcp_lines(const struct psk_model *m, struct psk_diag_line *out, unsigned max)
{
    const struct ppc_net_totals *t = &m->nt;
    uint32_t msgs = t->dhcp_discover + t->dhcp_offer + t->dhcp_request + t->dhcp_ack + t->dhcp_nak + t->dhcp_decline +
                    t->dhcp_release + t->dhcp_inform;
    struct psk_diag_line *l;
    unsigned n = 0;
    uint32_t i;
    char a[24];
    char b[24];
    char c[24];
    char d[24];

    l = add_line(out, &n, max, PSK_SEV_OK);
    if (l && msgs == 0) {
        snprintf(l->text, sizeof(l->text), "NO DHCP SEEN");
        snprintf(l->why, sizeof(l->why),
                 "No DHCP message has passed this interface. Devices ask when they join the network and when half "
                 "their lease has run; a renewal goes straight to the server and is not seen from a switch port.");
    } else if (l) {
        snprintf(l->text, sizeof(l->text), "%s OF %s EXCHANGE%s ACKNOWLEDGED", count(t->dhcp_complete, a, sizeof(a)),
                 count(t->dhcp_tx, b, sizeof(b)), plural(t->dhcp_tx));
        snprintf(l->why, sizeof(l->why),
                 "Seen: %s DISCOVER, %s OFFER, %s REQUEST, %s ACK. An exchange is the messages of one transaction id "
                 "and client. One seen only in part is not a failure: the rest may be unicast.",
                 count(t->dhcp_discover, a, sizeof(a)), count(t->dhcp_offer, b, sizeof(b)),
                 count(t->dhcp_request, c, sizeof(c)), count(t->dhcp_ack, d, sizeof(d)));
        go_host(l, PPC_CLASS_DHCP, "");
    }
    for (i = 0; i < m->net.nservers; i++) {
        const struct ppc_evt_net_srv *s = &m->net.srv[i];

        /* Every server after the first is the one to look at. */
        l = add_line(out, &n, max, i == 0 ? PSK_SEV_OK : PSK_SEV_WARN);
        if (!l) {
            break;
        }
        snprintf(l->text, sizeof(l->text), "SERVER %s", s->ip);
        part(l->text, sizeof(l->text), "%s OFFER%s", count(s->offers, a, sizeof(a)), plural(s->offers));
        part(l->text, sizeof(l->text), "%s ACK%s", count(s->acks, a, sizeof(a)), plural(s->acks));
        if (s->naks) {
            part(l->text, sizeof(l->text), "%s NAK%s", count(s->naks, a, sizeof(a)), plural(s->naks));
        }
        snprintf(l->why, sizeof(l->why), "Its frames come from %s%s.", s->mac[0] ? s->mac : "an unknown MAC",
                 s->relayed ? ", a relay passing them on" : "");
        if (s->router[0] || s->dns[0] || s->mask[0] || s->lease_s) {
            more(l->why, sizeof(l->why), " It gives");
            if (s->router[0]) {
                more(l->why, sizeof(l->why), " router %s,", s->router);
            }
            if (s->dns[0]) {
                more(l->why, sizeof(l->why), " DNS %s,", s->dns);
            }
            if (s->mask[0]) {
                more(l->why, sizeof(l->why), " mask %s,", s->mask);
            }
            if (s->lease_s) {
                more(l->why, sizeof(l->why), " leases of %s,", lease_lower(s->lease_s, a, sizeof(a)));
            }
            l->why[strlen(l->why) - 1] = '.';
        }
        if (i > 0) {
            more(l->why, sizeof(l->why), " A second server: meant, or a mistake? Compare what each gives.");
        }
        go_open(l, s->seq);
    }
    if (t->dhcp_servers > m->net.nservers && m->have_net_lists && (l = add_line(out, &n, max, PSK_SEV_WARN)) != NULL) {
        snprintf(l->text, sizeof(l->text), "%s MORE SERVER%s", count(t->dhcp_servers - m->net.nservers, a, sizeof(a)),
                 plural(t->dhcp_servers - m->net.nservers));
        snprintf(l->why, sizeof(l->why), "More servers answered than this sheet lists.");
    }
    for (i = 0; i < m->net.nleases && i < 3; i++) {
        const struct ppc_evt_net_lease *ls = &m->net.lease[i];

        l = add_line(out, &n, max, PSK_SEV_OK);
        if (!l) {
            break;
        }
        snprintf(l->text, sizeof(l->text), "%s LEASED TO %s", ls->ip, ls->mac[0] ? ls->mac : "A CLIENT");
        snprintf(l->why, sizeof(l->why), "At %s into the capture", at(ls->rel_us, a, sizeof(a)));
        if (ls->lease_s) {
            more(l->why, sizeof(l->why), ", for %s", lease_lower(ls->lease_s, b, sizeof(b)));
        }
        if (ls->server[0]) {
            more(l->why, sizeof(l->why), ", by %s", ls->server);
        }
        if (ls->name[0]) {
            more(l->why, sizeof(l->why), ". The client calls itself %s", ls->name);
        }
        more(l->why, sizeof(l->why), ".");
        go_open(l, ls->seq);
    }
    if (t->dhcp_no_offer && (l = add_line(out, &n, max, PSK_SEV_WARN)) != NULL) {
        snprintf(l->text, sizeof(l->text), "%s DISCOVER NOT ANSWERED IN 10 S", count(t->dhcp_no_offer, a, sizeof(a)));
        snprintf(l->why, sizeof(l->why),
                 "No OFFER was seen, and it would have been: the request was this device's own or asked for a "
                 "broadcast reply. No server reachable, no address left, or the answer was lost. Not proof of a fault.");
    }
    if (t->dhcp_no_ack && (l = add_line(out, &n, max, PSK_SEV_WARN)) != NULL) {
        snprintf(l->text, sizeof(l->text), "%s REQUEST%s NOT ANSWERED IN 10 S", count(t->dhcp_no_ack, a, sizeof(a)),
                 plural(t->dhcp_no_ack));
        snprintf(l->why, sizeof(l->why),
                 "No ACK or NAK was seen to a REQUEST whose answer would have passed here. The server may be slow or "
                 "gone, or the answer was lost. Not proof of a fault.");
    }
    if (t->dhcp_nak && (l = add_line(out, &n, max, PSK_SEV_WARN)) != NULL) {
        snprintf(l->text, sizeof(l->text), "%s REFUSED BY A SERVER (NAK)", count(t->dhcp_nak, a, sizeof(a)));
        snprintf(l->why, sizeof(l->why),
                 "A server told a client that it may not have the address it asked for: a lease from another network, "
                 "an address given to someone else, or two servers that disagree. The client starts again.");
    }
    if (t->dhcp_decline && (l = add_line(out, &n, max, PSK_SEV_WARN)) != NULL) {
        snprintf(l->text, sizeof(l->text), "%s DECLINED BY THE CLIENT", count(t->dhcp_decline, a, sizeof(a)));
        snprintf(l->why, sizeof(l->why),
                 "A client found the address it was given already in use and gave it back. Its own check, so a "
                 "confirmed duplicate address: something holds it without the server knowing.");
    }
    if (t->dhcp_unseen && (l = add_line(out, &n, max, PSK_SEV_NOTE)) != NULL) {
        snprintf(l->text, sizeof(l->text), "%s REQUEST%s NOT JUDGED", count(t->dhcp_unseen, a, sizeof(a)),
                 plural(t->dhcp_unseen));
        snprintf(l->why, sizeof(l->why),
                 "Their answers go straight to the device that asked and do not pass a switch port, so none seen "
                 "here says nothing about them.");
    }
    if (t->dhcp_late && (l = add_line(out, &n, max, PSK_SEV_NOTE)) != NULL) {
        snprintf(l->text, sizeof(l->text), "%s ANSWERED LATE", count(t->dhcp_late, a, sizeof(a)));
        snprintf(l->why, sizeof(l->why), "An answer came after its request had been called unanswered: a slow server.");
    }
    if (t->dhcp_waiting && (l = add_line(out, &n, max, PSK_SEV_NOTE)) != NULL) {
        bool live = m->state == PPC_STATE_CAPTURING;

        snprintf(l->text, sizeof(l->text), "%s %s", count(t->dhcp_waiting, a, sizeof(a)),
                 live ? "WAITING FOR AN ANSWER" : "NOT ANSWERED WHEN IT STOPPED");
        snprintf(l->why, sizeof(l->why), "%s",
                 live ? "Asked less than 10 s ago." : "Still waiting when the capture stopped or was held: not judged.");
    }
    return n;
}

/* ---- the ARP sheet ---------------------------------------------------------------------- */

static unsigned arp_lines(const struct psk_model *m, struct psk_diag_line *out, unsigned max)
{
    const struct ppc_net_totals *t = &m->nt;
    struct psk_diag_line *l;
    unsigned n = 0;
    char a[24];
    char b[24];
    char c[24];

    l = add_line(out, &n, max, PSK_SEV_OK);
    if (l && t->arp_requests + t->arp_replies + t->arp_other == 0) {
        snprintf(l->text, sizeof(l->text), "NO ARP SEEN");
        snprintf(l->why, sizeof(l->why),
                 "No ARP message has passed this interface. On a quiet network they come every few minutes, as "
                 "devices refresh what they know of each other.");
    } else if (l) {
        snprintf(l->text, sizeof(l->text), "%s ADDRESS%s" DOT "%s REQUEST%s" DOT "%s REPL%s",
                 count(t->arp_binds, a, sizeof(a)), t->arp_binds == 1 ? "" : "ES", count(t->arp_requests, b, sizeof(b)),
                 plural(t->arp_requests), count(t->arp_replies, c, sizeof(c)), t->arp_replies == 1 ? "Y" : "IES");
        snprintf(l->why, sizeof(l->why),
                 "Each address is paired with the MAC that last claimed it as an ARP sender. Replies go straight to "
                 "the device that asked, so few replies beside many requests is normal on a switch port.");
        go_host(l, PPC_CLASS_ARP, "");
    }
    if (t->arp_conflicts && (l = add_line(out, &n, max, PSK_SEV_WARN)) != NULL) {
        snprintf(l->text, sizeof(l->text), "%s ADDRESS CONFLICT%s SEEN", count(t->arp_conflicts, a, sizeof(a)),
                 plural(t->arp_conflicts));
        snprintf(l->why, sizeof(l->why),
                 "A device probed for an address before using it, and another claimed that address within 10 s. The "
                 "conflict the probe exists to find (RFC 5227): confirmed by the exchange itself.");
    }
    if (t->arp_flips && (l = add_line(out, &n, max, PSK_SEV_WARN)) != NULL) {
        snprintf(l->text, sizeof(l->text), "%s TIME%s TWO MACS CLAIMED ONE ADDRESS", count(t->arp_flips, a, sizeof(a)),
                 plural(t->arp_flips));
        snprintf(l->why, sizeof(l->why),
                 "An address went back to the MAC it had just left, within a minute. A potential duplicate address: "
                 "two stations using it at once. Failover pairs and forged ARP look the same from here.");
    }
    if (t->arp_moved && (l = add_line(out, &n, max, PSK_SEV_NOTE)) != NULL) {
        snprintf(l->text, sizeof(l->text), "%s ADDRESS MOVE%s", count(t->arp_moved, a, sizeof(a)), plural(t->arp_moved));
        snprintf(l->why, sizeof(l->why),
                 "An address was claimed by another MAC than before. Usually harmless: a lease given to another "
                 "device, a replaced adapter, a failover. The network overview lists each one.");
    }
    if (t->arp_noreply && (l = add_line(out, &n, max, PSK_SEV_NOTE)) != NULL) {
        snprintf(l->text, sizeof(l->text), "%s OF THIS DEVICE'S REQUESTS UNANSWERED", count(t->arp_noreply, a, sizeof(a)));
        snprintf(l->why, sizeof(l->why),
                 "This device asked for an address three times or more and saw no reply: nothing on this link holds "
                 "it, or its holder is down. Other devices' requests are not judged: their replies do not pass here.");
    }
    if (t->arp_gratuitous && (l = add_line(out, &n, max, PSK_SEV_OK)) != NULL) {
        snprintf(l->text, sizeof(l->text), "%s GRATUITOUS", count(t->arp_gratuitous, a, sizeof(a)));
        snprintf(l->why, sizeof(l->why),
                 "A device announcing its own address, when it joins or its address changes. Normal; one that changes "
                 "an address's MAC is listed as a move.");
    }
    if (t->arp_probes && (l = add_line(out, &n, max, PSK_SEV_OK)) != NULL) {
        snprintf(l->text, sizeof(l->text), "%s PROBE%s", count(t->arp_probes, a, sizeof(a)), plural(t->arp_probes));
        snprintf(l->why, sizeof(l->why),
                 "Requests with no sender address: a device checking that an address is free before it uses it. "
                 "Normal.");
    }
    if (t->arp_other && (l = add_line(out, &n, max, PSK_SEV_NOTE)) != NULL) {
        snprintf(l->text, sizeof(l->text), "%s WITH ANOTHER OPCODE", count(t->arp_other, a, sizeof(a)));
        snprintf(l->why, sizeof(l->why), "Neither a request nor a reply (RARP, InARP and the like): counted, not judged.");
    }
    return n;
}

unsigned psk_net_lines(const struct psk_model *m, unsigned kind, struct psk_diag_line *out, unsigned max)
{
    if (kind == PSK_DIAG_DHCP) {
        return dhcp_lines(m, out, max);
    }
    if (kind == PSK_DIAG_ARP) {
        return arp_lines(m, out, max);
    }
    return 0;
}

const char *psk_net_confidence(const struct psk_model *m, unsigned kind, char *buf, size_t buflen)
{
    const struct ppc_net_totals *t = &m->nt;
    uint64_t drops = m->c.kdrop + m->c.ifdrop;
    char iface[PPC_IFACE_MAX];
    char n[24];
    char el[24];
    size_t i;

    (void)kind;
    snprintf(iface, sizeof(iface), "%s", m->iface[0] ? m->iface : "THE INTERFACE");
    for (i = 0; iface[i]; i++) {
        if (iface[i] >= 'a' && iface[i] <= 'z') {
            iface[i] = (char)(iface[i] - 'a' + 'A');
        }
    }
    snprintf(buf, buflen,
             "Read from what %s saw in %s. Passive: nothing was sent to check. A switch port shows broadcasts and "
             "this device's own traffic, so this is evidence, not the whole network.",
             iface, psk_fmt_elapsed(m->c.elapsed_ms, el, sizeof(el)));
    if (drops) {
        more(buf, buflen, " The kernel dropped %s %s.", psk_fmt_count(drops, n, sizeof(n)),
             drops == 1 ? "packet" : "packets");
    }
    if (t->blinded) {
        more(buf, buflen, " Packets passed unjudged %u time%s (HOLD, or too fast to store): what waited then was let go.",
             (unsigned)t->blinded, t->blinded == 1 ? "" : "s");
    }
    if (t->hosts_replaced) {
        more(buf, buflen, " The device table was full: %s forgotten.", psk_fmt_count(t->hosts_replaced, n, sizeof(n)));
    }
    if (t->events_lost) {
        more(buf, buflen, " %s older event%s made room for newer ones.", psk_fmt_count(t->events_lost, n, sizeof(n)),
             t->events_lost == 1 ? "" : "s");
    }
    return buf;
}

/* ---- one event -------------------------------------------------------------------------- */

const char *psk_net_event_title(unsigned kind)
{
    static const char *const titles[PPC_NK_COUNT] = {
        "?",
        "SECOND DHCP SERVER ANSWERING",
        "DHCP SERVER REFUSED A REQUEST",
        "CLIENT DECLINED ITS ADDRESS",
        "DHCP DISCOVER NOT ANSWERED",
        "DHCP REQUEST NOT ANSWERED",
        "VERY SHORT DHCP LEASE",
        "ADDRESS CONFLICT SEEN",
        "TWO DEVICES CLAIM ONE ADDRESS",
        "ADDRESS MOVED TO ANOTHER MAC",
        "ROUTER'S ADDRESS AT ANOTHER MAC",
        "NO ARP REPLY FOR AN ADDRESS",
        "DEVICE ASKING FOR MANY ADDRESSES",
        "ARP SENDER IS NOT THE FRAME'S SOURCE",
    };

    return kind < PPC_NK_COUNT ? titles[kind] : "?";
}

static int is_dhcp(unsigned kind)
{
    return kind >= PPC_NK_DHCP_SERVERS && kind <= PPC_NK_DHCP_SHORT_LEASE;
}

/* What the capture saw, with the addresses, and what it may mean. A part of
 * a sentence that needs an address the event does not carry says so in
 * words. */
static void event_words(const struct ppc_evt_net_event *e, char *seen, size_t sn, char *means, size_t mn)
{
    const char *ip = e->ip[0] ? e->ip : "an address";
    const char *ip2 = e->ip2[0] ? e->ip2 : "another address";
    const char *mac = e->mac[0] ? e->mac : "a device";
    const char *mac2 = e->mac2[0] ? e->mac2 : "another device";

    switch (e->kind) {
    case PPC_NK_DHCP_SERVERS:
        snprintf(seen, sn, "DHCP answers came from %s (frames from %s) as well as from %s.", ip, mac, ip2);
        snprintf(means, mn,
                 "Two servers can be a pair set up to share the work, or one nobody meant to have: a router plugged "
                 "in by mistake, a virtual machine, or an attack. The DHCP sheet shows what each gives. Not proof of "
                 "a fault.");
        break;
    case PPC_NK_DHCP_NAK:
        snprintf(seen, sn, "Server %s answered NAK to the client %s.", ip, mac);
        snprintf(means, mn,
                 "The client asked for an address this server does not give it: a lease from another network, an "
                 "address handed to someone else, or two servers that disagree. The client starts again with "
                 "DISCOVER.");
        break;
    case PPC_NK_DHCP_DECLINE:
        snprintf(seen, sn, "The client %s declined %s%s%s.", mac, ip, e->ip2[0] ? ", given by " : "",
                 e->ip2[0] ? e->ip2 : "");
        snprintf(means, mn,
                 "The client checked the address with ARP and found it in use. That is the client's own, confirmed "
                 "report of a duplicate: something holds the address without the server knowing.");
        break;
    case PPC_NK_DHCP_NO_OFFER:
        snprintf(seen, sn, "The client %s asked for a DHCP server and no OFFER was seen within 10 s.", mac);
        snprintf(means, mn,
                 "Judged only because the answer would have passed here (this device's own request, or one that "
                 "asked for a broadcast reply). No server reachable, no address left to give, or the answer was "
                 "lost. Not proof of a fault.");
        break;
    case PPC_NK_DHCP_NO_ACK:
        snprintf(seen, sn, "The client %s asked%s%s for %s and no ACK or NAK was seen within 10 s.", mac,
                 e->ip2[0] ? " server " : "", e->ip2[0] ? e->ip2 : "", ip);
        snprintf(means, mn,
                 "Judged only because the answer would have passed here. The server may be slow or gone, or the "
                 "answer was lost. Not proof of a fault.");
        break;
    case PPC_NK_DHCP_SHORT_LEASE:
        snprintf(seen, sn, "Server %s gave %s the address %s for less than two minutes.", ip, mac, ip2);
        snprintf(means, mn,
                 "Unusual outside test and guest networks: the client must renew constantly. Worth a look at the "
                 "server's settings; not a fault by itself.");
        break;
    case PPC_NK_ARP_CONFLICT:
        snprintf(seen, sn, "%s probed for %s before using it, and %s claimed that address within 10 s.", mac2, ip, mac);
        snprintf(means, mn,
                 "This is the conflict the probe exists to find (RFC 5227): the address is already in use, and the "
                 "probing device should not take it. Confirmed by the exchange itself.");
        break;
    case PPC_NK_ARP_DUPLICATE:
        snprintf(seen, sn, "%s was claimed by %s and by %s in turn, within a minute of each other.", ip, mac, mac2);
        snprintf(means, mn,
                 "A potential duplicate address: two stations appear to use it at once, which breaks connections for "
                 "both. Also seen with failover pairs and a device on two interfaces. Forged ARP that redirects "
                 "traffic looks the same.");
        break;
    case PPC_NK_ARP_MOVED:
        snprintf(seen, sn, "%s was at %s and is now claimed by %s.", ip, mac2, mac);
        snprintf(means, mn,
                 "Usually harmless: a lease given to another device, a replaced adapter, a failover. A problem only "
                 "if the old device still uses the address; then both claims keep coming and it is listed as a "
                 "duplicate.");
        break;
    case PPC_NK_ARP_ROUTER_MOVED:
        snprintf(seen, sn, "%s, which DHCP names as the router, was at %s and is now claimed by %s.", ip, mac2, mac);
        snprintf(means, mn,
                 "A replaced or failed-over router does this once. It is also what ARP spoofing looks like: a device "
                 "answering for the router to see everyone's traffic. Check that the new MAC is the router's.");
        break;
    case PPC_NK_ARP_NO_REPLY:
        snprintf(seen, sn, "This device asked who has %s three times or more and saw no reply.", ip);
        snprintf(means, mn,
                 "Nothing on this link holds the address, or its holder is down or filtered. When it is the router's "
                 "address, nothing beyond this network can be reached.");
        break;
    case PPC_NK_ARP_SWEEP:
        snprintf(seen, sn, "%s%s%s%s sent ARP requests for 32 or more different addresses within 10 s.", mac,
                 e->ip[0] ? " (" : "", e->ip, e->ip[0] ? ")" : "");
        snprintf(means, mn,
                 "A scanner, a monitoring or inventory system, or a busy router refilling its table. Not a fault; "
                 "worth knowing who it is.");
        break;
    case PPC_NK_ARP_SENDER:
        snprintf(seen, sn, "A frame from %s carried an ARP message that claims %s for %s.", mac, ip, mac2);
        snprintf(means, mn,
                 "Seen with proxy ARP, bridges and some access points, which answer for others. A forged ARP message "
                 "can look like this too.");
        break;
    default:
        snprintf(seen, sn, "An event this version has no words for.");
        means[0] = '\0';
        break;
    }
}

unsigned psk_net_event_lines(const struct ppc_evt_net_event *e, struct psk_diag_line *out, unsigned max)
{
    struct psk_diag_line *l;
    unsigned n = 0;
    char means[sizeof(l->why)];
    char a[24];
    char b[24];
    char c[24];

    if (!e || e->kind == PPC_NK_NONE || e->kind >= PPC_NK_COUNT) {
        return 0;
    }
    l = add_line(out, &n, max, e->sev == PPC_NS_WARN ? PSK_SEV_WARN : PSK_SEV_NOTE);
    if (!l) {
        return n;
    }
    snprintf(l->text, sizeof(l->text), "%s", psk_net_event_title(e->kind));
    event_words(e, l->why, sizeof(l->why), means, sizeof(means));
    if (means[0] && (l = add_line(out, &n, max, PSK_SEV_NOTE)) != NULL) {
        snprintf(l->text, sizeof(l->text), "WHAT IT MAY MEAN");
        snprintf(l->why, sizeof(l->why), "%s", means);
    }
    l = add_line(out, &n, max, PSK_SEV_NOTE);
    if (l) {
        snprintf(l->text, sizeof(l->text), "EVIDENCE" DOT "PACKET #%s", count(e->seq, a, sizeof(a)));
        if (e->count > 1) {
            snprintf(l->why, sizeof(l->why),
                     "Seen %s times, first at %s and last at %s into the capture. The packet is the latest that "
                     "showed it.",
                     count(e->count, a, sizeof(a)), at(e->first_us, b, sizeof(b)), at(e->last_us, c, sizeof(c)));
        } else {
            snprintf(l->why, sizeof(l->why), "Seen once, at %s into the capture. The packet is the one that showed it.",
                     at(e->last_us, b, sizeof(b)));
        }
        go_open(l, e->seq);
    }
    l = add_line(out, &n, max, PSK_SEV_NOTE);
    if (l) {
        if (is_dhcp(e->kind)) {
            snprintf(l->text, sizeof(l->text), "ALL DHCP PACKETS");
            snprintf(l->why, sizeof(l->why), "The exchange around it, in PACKETS.");
            go_host(l, PPC_CLASS_DHCP, "");
        } else {
            const char *host = e->ip[0] ? e->ip : e->mac;

            snprintf(l->text, sizeof(l->text), "ARP PACKETS OF %s", host[0] ? host : "THE CAPTURE");
            snprintf(l->why, sizeof(l->why), "Every ARP message that names it, in PACKETS: who asked, who claimed, when.");
            go_host(l, PPC_CLASS_ARP, host);
        }
    }
    return n;
}

const struct ppc_evt_net_event *psk_net_event_same(const struct psk_model *m, const struct ppc_evt_net_event *e)
{
    uint32_t i;

    for (i = 0; e && i < m->net.nevents; i++) {
        const struct ppc_evt_net_event *o = &m->net.evt[i];

        /* The same finding: its kind, when it was first seen, and whom it
         * is about (its latest MACs may have changed since). */
        if (o->kind == e->kind && o->first_us == e->first_us && strcmp(o->ip, e->ip) == 0) {
            return o;
        }
    }
    return NULL;
}

/* ---- the findings, as one list ---------------------------------------------------------- */

static void find_event(struct psk_find *f, const struct ppc_evt_net_event *e)
{
    char a[24];

    memset(f, 0, sizeof(*f));
    f->sev = e->sev == PPC_NS_WARN ? PSK_SEV_WARN : PSK_SEV_NOTE;
    f->kind = PSK_DIAG_FINDING;
    f->ev = *e;
    snprintf(f->text, sizeof(f->text), "%s", psk_net_event_title(e->kind));
    if (e->ip[0]) {
        part(f->sub, sizeof(f->sub), "%s", e->ip);
    }
    if (e->mac[0]) {
        part(f->sub, sizeof(f->sub), "%s", e->mac);
    }
    part(f->sub, sizeof(f->sub), "%s", at(e->last_us, a, sizeof(a)));
    if (e->count > 1) {
        part(f->sub, sizeof(f->sub), "%s TIMES", count(e->count, a, sizeof(a)));
    }
}

unsigned psk_findings(const struct psk_model *m, struct psk_find *out, unsigned max)
{
    static const char *const proto[PSK_DIAG_KINDS] = { "DNS", "TCP" };
    struct psk_diag_line lines[PSK_DIAG_LINES];
    unsigned n = 0;
    unsigned kind;
    uint32_t i;

    for (i = 0; i < m->net.nevents && n < max; i++) {
        if (m->net.evt[i].sev == PPC_NS_WARN) {
            find_event(&out[n++], &m->net.evt[i]);
        }
    }
    for (kind = 0; kind < PSK_DIAG_KINDS && m->have_diag; kind++) {
        unsigned k = psk_diag_lines(m, kind, lines, PSK_DIAG_LINES);
        unsigned j;

        for (j = 0; j < k && n < max; j++) {
            struct psk_find *f;

            if (lines[j].sev != PSK_SEV_WARN) {
                continue;
            }
            f = &out[n++];
            memset(f, 0, sizeof(*f));
            f->sev = PSK_SEV_WARN;
            f->kind = (uint8_t)kind;
            snprintf(f->text, sizeof(f->text), "%s" DOT "%s", proto[kind], lines[j].text);
            snprintf(f->sub, sizeof(f->sub), "FROM THE %s DIAGNOSIS", proto[kind]);
        }
    }
    for (i = 0; i < m->net.nevents && n < max; i++) {
        if (m->net.evt[i].sev != PPC_NS_WARN) {
            find_event(&out[n++], &m->net.evt[i]);
        }
    }
    return n;
}

/* ---- the devices ------------------------------------------------------------------------ */

void psk_net_host_lines(const struct psk_model *m, unsigned i, char *l1, size_t n1, char *l2, size_t n2)
{
    const struct ppc_evt_net_host *h;
    char a[24];
    char b[24];
    unsigned share;

    l1[0] = '\0';
    l2[0] = '\0';
    if (i >= m->net.nhosts) {
        return;
    }
    h = &m->net.host[i];
    part(l1, n1, "%s", h->ip[0] ? h->ip : h->mac);
    if (h->flags & PPC_NH_SELF) {
        part(l1, n1, "THIS DEVICE");
    }
    if (h->flags & PPC_NH_ROUTER) {
        part(l1, n1, "ROUTER");
    }
    if (h->flags & PPC_NH_DHCP) {
        part(l1, n1, "DHCP SERVER");
    }
    if (h->name[0]) {
        part(l1, n1, "%s", h->name);
    }
    part(l2, n2, "%s", h->ip[0] ? h->mac : "NO IPV4 ADDRESS SEEN");
    part(l2, n2, "%s PKT", count(h->pkts, a, sizeof(a)));
    part(l2, n2, "%s", psk_fmt_bytes(h->bytes, b, sizeof(b)));
    share = psk_net_host_share(m, i);
    if (share < 5u) {
        part(l2, n2, "<1%%");
    } else {
        part(l2, n2, "%u%%", (share + 5u) / 10u);
    }
}

unsigned psk_net_host_share(const struct psk_model *m, unsigned i)
{
    uint64_t total = m->c.bytes;
    uint64_t own;

    if (i >= m->net.nhosts) {
        return 0;
    }
    own = m->net.host[i].bytes;
    if (total < own) {
        total = own; /* the list is a moment newer than the counters */
    }
    return total ? (unsigned)(own * 1000u / total) : 0;
}

const char *psk_net_host_filter(const struct psk_model *m, unsigned i)
{
    if (i >= m->net.nhosts) {
        return "";
    }
    return m->net.host[i].ip[0] ? m->net.host[i].ip : m->net.host[i].mac;
}

const char *psk_net_mix(const struct psk_model *m, char *buf, size_t buflen)
{
    uint32_t share[PPC_CLASS_COUNT];
    uint8_t done[PPC_CLASS_COUNT] = { 0 };
    unsigned i;
    unsigned k;

    buf[0] = '\0';
    for (i = 0; i < PPC_CLASS_COUNT; i++) {
        share[i] = psk_model_class_share_permille(m, i);
    }
    for (k = 0; k < PPC_CLASS_COUNT; k++) {
        int best = -1;

        for (i = 0; i < PPC_CLASS_COUNT; i++) {
            if (!done[i] && m->c.cls[i] && (best < 0 || share[i] > share[best])) {
                best = (int)i;
            }
        }
        if (best < 0) {
            break;
        }
        done[best] = 1;
        if (share[best] < 5u) {
            part(buf, buflen, "%s <1%%", psk_class_word((unsigned)best));
        } else {
            part(buf, buflen, "%s %u%%", psk_class_word((unsigned)best), (unsigned)((share[best] + 5u) / 10u));
        }
    }
    if (!buf[0]) {
        snprintf(buf, buflen, "NO PACKET YET");
    }
    return buf;
}
