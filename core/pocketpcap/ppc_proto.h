/*
 * PacketShark's line protocol between the shell and pos-capture
 * (ARCHITECTURE §3; DESIGN_DECISIONS D2).
 *
 * Transport: one AF_UNIX stream socketpair, the helper's stdin and stdout.
 * Lines of at most PPC_LINE_MAX bytes ending in '\n'. Helper-to-shell lines
 * separate fields with a tab. Shell-to-helper lines may use tabs or spaces,
 * so the helper can be driven by hand from a serial console during bring-up
 * (BusyBox ash completes a typed tab). Text derived from a packet never
 * carries a tab or newline (ppc_format.h sanitizes it).
 *
 * Implemented (M2/M3; view, the window's rank and time anchors, inspect and
 * pin since M5/M6, protocol 2; the view's findings, marks, save and the
 * finding line since M7/M8, protocol 3; the sixth finding, the view's
 * diagnosis, the diag and detail lines since M11.1, protocol 4; the network
 * intelligence, the net command and the net lines since M12.1, protocol 5;
 * marks and their evidence since N1.0, protocol 6):
 *
 *   shell -> helper
 *     start <iface> <headers|full> <promisc 0|1> <cfilter> <limit_s>
 *     hold <0|1>
 *     stop
 *     discard
 *     view <id> <time|newest|len> <classes> <host|-> <port> <pinned 0|1> [<signals> [<diag>]]
 *                                     the list's display filter and sort;
 *                                     classes is a hex mask of enum ppc_class,
 *                                     signals a hex mask of PPC_SIG_* (M7): only
 *                                     packets with one of those findings;
 *                                     diag an enum ppc_diag_code (M11.1): only
 *                                     packets with that diagnosis, 0 any
 *     window <req> <tail|seq:N|rank:N|us:N> <before> <after>
 *                                     at most PPC_WINDOW_MAX rows of the view
 *     inspect <req> <seq> <step -1|0|1>
 *                                     one packet's layers, fields and bytes; a
 *                                     step moves to its neighbour in the view
 *     pin <seq> <0|1>
 *     marks <req> <buckets>           where the view's findings are (M7): the
 *                                     view cut into at most 64 equal parts
 *     save                            write the stopped capture to a file (M8)
 *     net <req>                       the devices, DHCP servers, leases and
 *                                     events as they stand (M12.1)
 *     mark                            mark this moment (N1.0): answered at
 *                                     once by a `mark` line
 *     evid <req> <id>                 a mark's evidence as it stands; id 0 is
 *                                     the newest mark
 *     intel <req> <mark> <pair>       a mark's comparisons (N1.2, protocol 7):
 *                                     pair 1 the minute after against the
 *                                     minute before, pair 0 the minute before
 *                                     against its reference; mark 0 is the
 *                                     newest. Answered by in.begin, two in.vis,
 *                                     five in.cmp and in.end, queued whole
 *     stats                           one snapshot now
 *     quit
 *
 *   helper -> shell
 *     hello <proto> <version> <backend>
 *     state <idle|capturing|hold|saving|stopped> <reason> <iface|-> <speed_mbps> <clock_set>
 *     stats <elapsed_ms> <seen> <stored> <held> <kdrop> <ifdrop> <shed> <bytes> <pps> <bps>
 *           <first_seq> <last_seq> <match> <fill_permille> <ui_skipped> <c0..c7> <s0..s5>
 *     diag <36 figures of struct ppc_diag, in its order>
 *                                     the DNS and TCP diagnosis (M11.1), once
 *                                     a second while capturing, and on stop
 *     net <33 figures of struct ppc_net_totals, in its order>
 *                                     the network intelligence's totals (M12.1),
 *                                     once a second while capturing, on stop,
 *                                     and before each answer below
 *     net.begin <req>
 *     net.host <mac> <ip|-> <flags> <pkts> <bytes> <name|->
 *     net.srv <ip> <mac> <relayed 0|1> <offers> <acks> <naks> <seq> <lease_s>
 *             <router|-> <dns|-> <mask|->
 *     net.lease <ip> <mac> <server> <lease_s> <rel_us> <seq> <name|->
 *     net.evt <kind> <sev> <count> <seq> <first_us> <last_us> <ip|-> <mac|->
 *             <ip2|-> <mac2|->
 *     net.end <req>                   the answer to `net`, queued whole: at
 *                                     most PPC_NET_WIRE_HOSTS devices (most
 *                                     bytes first), _SERVERS, _LEASES (newest
 *                                     first) and _EVENTS (warnings first)
 *     bin <second> <c0..c7> <held>
 *     rows.begin <req> <filter_id> <first_rank> <match> <first_seq> <last_seq> [<anchor_rank>]
 *     row <seq> <rel_us> <gap_us> <dir> <class> <word> <len> <signal> <src> <dst> <info> [<flags>]
 *     rows.end <req>
 *     pkt.begin <req> <seq> <rank> <match> <rel_us> <gap_us> <dir> <class> <word>
 *               <wire_len> <stored> <signal> <flags>
 *     pkt.ends <src> <src_sub> <src_filter> <dst> <dst_sub> <dst_filter>
 *     layer <index> <class|-> <name> <summary>
 *     field <layer> <name> <value> <off|-> <len> <filter|->
 *     hex <off> <hex> <ascii>
 *     pkt.end <req>
 *     pinned <seq> <0|1> <pins>
 *     finding <layer> <text>          inside pkt.begin..pkt.end: a finding of
 *                                     the packet, under that layer (M7)
 *     detail <layer> <text>           inside pkt.begin..pkt.end: what the DNS
 *                                     or TCP diagnosis says of the packet, not
 *                                     a fault (M11.1)
 *     marks <req> <view_id> <match> <buckets> <hex mask>
 *     mark <id> <first> <at_ms> <state> <seq> <wall_s>
 *                                     a mark was set (N1.0): its id, the oldest
 *                                     id still held, when (ms since START),
 *                                     what the capture was doing, the newest
 *                                     packet then, and the wall clock (0 when
 *                                     it is not set)
 *     ev.begin <req> <id> <first> <last> <at_ms> <state> <seq> <wall_s> <now_ms>
 *              <cfilter> <unanalyzed> <edge_lost|-> <edge_from|-> <edge_to|->
 *                                     the head of a mark's evidence: which
 *                                     mark of the ones held, when, and the
 *                                     wall clock then (after STOP: at STOP);
 *                                     the capture filter the source applied
 *                                     (all, ip, names, noself), the classes
 *                                     whose analysis could not start (a hex
 *                                     mask of enum ppc_class among DHCP, ARP
 *                                     and DNS); and when the drop counters
 *                                     could not be read at the mark, the last
 *                                     good reading before it, the first after
 *                                     it, and what that one told was lost in
 *                                     between: "0 - -" when they were read,
 *                                     "- <from> -" while no reading has come
 *     ev.win <side 0|1> <flags> <span_ms> <watched_s> <held_s> <shed_s> <lost>
 *            <kept> <more> <k1..k18> <lost_near> <unknown_from|->
 *                                     one side of the mark: the window before
 *                                     it (0) or after it (1), how much of it was
 *                                     watched, and every observation counted by
 *                                     kind (enum ppc_ev_kind, in its order);
 *                                     lost in a spell without a counter reading
 *                                     that reaches past its far end (apart), and
 *                                     the last good reading before a spell still
 *                                     open in it, whose losses are not known
 *     ev.obs <side> <kind> <flags> <t_ms> <seq> <alive> <mac|-> <ip|-> <ip2|->
 *            <a> <b>                  one observation kept as a row; alive says
 *                                     what the buffer still holds of its packet
 *     ev.end <req>                    the answer to `evid`, queued whole
 *     in.begin <req> <mark> <pair> <at_ms> <now_ms> <final 0|1>
 *                                     the head of a mark's comparisons (N1.2):
 *                                     final once the window and the wait of its
 *                                     last questions have passed, or STOP
 *     in.vis <side 0|1> <from_ms> <to_ms> <watched_s> <held_s> <shed_s> <gap_s>
 *            <frames> <lost_lo> <lost_hi> <loss_known 0|1> <own_in> <own_out>
 *                                     what the capture saw of each window
 *     in.cmp <metric> <verdict> <quality> <reason> <notes> <n0> <n1> <k0> <k1>
 *            <stat0> <stat1> <und0> <und1> <p_e6> <ref1> <ref2> <ref3> <ref4>
 *                                     one comparison, as the engine made it
 *                                     (ppc_intel.h): its words, its counts,
 *                                     and up to four packets as <seq>:<alive>
 *                                     (alive: 0 gone, 1 summary, 2 whole) or -
 *     in.end <req>                    the answer to `intel`, queued whole
 *     saved <run|done|fail> <packets> <total> <bytes> <name|code>
 *                                     SAVE's progress and outcome (M8); name is
 *                                     the file's name, without its folder
 *     error <code> <text>
 *     note <key> <value>
 *     bye
 *
 * Unknown words are ignored on both sides, so an older app and a newer
 * helper (or the reverse) keep working on what they share.
 *
 * Pure C, no LVGL, no allocation (tests/ppc_proto_test.c).
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef PPC_PROTO_H
#define PPC_PROTO_H

#include "ppc_format.h"
#include "ppc_record.h"

#include <stddef.h>
#include <stdint.h>

#define PPC_PROTO_VERSION 7
#define PPC_LINE_MAX 1024
#define PPC_IFACE_MAX 16
#define PPC_WINDOW_MAX 48
#define PPC_TEXT_MAX 160
#define PPC_FIELDS_MAX 40

enum ppc_state {
    PPC_STATE_IDLE = 0,
    PPC_STATE_CAPTURING,
    PPC_STATE_HOLD,
    PPC_STATE_SAVING,
    PPC_STATE_STOPPED,
};

/* Why a session is in its state. */
enum ppc_reason {
    PPC_REASON_NONE = 0,
    PPC_REASON_USER,   /* STOP pressed */
    PPC_REASON_LIMIT,  /* the session's time limit */
    PPC_REASON_LINK,   /* the interface went down or lost its carrier */
    PPC_REASON_END,    /* a replayed file ended */
    PPC_REASON_ERROR,  /* the source failed */
};

/* Capture-filter presets (ARCHITECTURE §7.1). Fixed strings: they compile on
 * the device because they compile in the host test. */
enum ppc_cfilter {
    PPC_CFILTER_ALL = 0,
    PPC_CFILTER_IP,
    PPC_CFILTER_NAMES,
    PPC_CFILTER_NOSELF,
    PPC_CFILTER_COUNT
};

/* Replace what is not printable ASCII with '.', in place. */
void ppc_sanitize(char *s);
/* The same, keeping exactly two well-formed UTF-8 marks the inspector's own
 * words use, the middle dot (U+00B7) and the right arrow (U+2192). Never
 * used on bytes taken from a packet, which ppc_sanitize() makes ASCII. */
void ppc_sanitize_marks(char *s);

const char *ppc_state_name(enum ppc_state s);
const char *ppc_reason_name(enum ppc_reason r);
const char *ppc_cfilter_id(enum ppc_cfilter f);
/* -1 for an unknown id. */
int ppc_cfilter_parse(const char *id);
/* The classes of a mark's evidence (PPC_EV_CLASSES) a preset keeps out of
 * the capture, read from its expression below: NAMES passes DNS and DHCP
 * by their ports and no ARP; IP passes ARP by name; ALL and NOSELF keep
 * every one of them. */
uint8_t ppc_cfilter_excludes(enum ppc_cfilter f);
/* The tcpdump expression of a preset, "" for ALL. self is this device's
 * IPv4 address in dotted form, or NULL when it has none: NOSELF then
 * excludes SSH by port alone. Returns buf. */
const char *ppc_cfilter_expr(enum ppc_cfilter f, const char *self, char *buf, size_t buflen);

/* ---- commands -------------------------------------------------------------------- */

enum ppc_cmd_kind {
    PPC_CMD_NONE = 0, /* an empty line, or a word this helper does not know */
    PPC_CMD_BAD,      /* a known word with bad arguments */
    PPC_CMD_START,
    PPC_CMD_HOLD,
    PPC_CMD_STOP,
    PPC_CMD_DISCARD,
    PPC_CMD_WINDOW,
    PPC_CMD_STATS,
    PPC_CMD_QUIT,
    PPC_CMD_VIEW,
    PPC_CMD_INSPECT,
    PPC_CMD_PIN,
    PPC_CMD_MARKS,
    PPC_CMD_SAVE,
    PPC_CMD_NET, /* M12.1 */
    PPC_CMD_MARK, /* N1.0 */
    PPC_CMD_EVID, /* N1.0 */
    PPC_CMD_INTEL, /* N1.2 */
};

/* Where a window is anchored. */
enum ppc_anchor {
    PPC_ANCHOR_TAIL = 0, /* the newest packet's end of the view */
    PPC_ANCHOR_SEQ,      /* this packet, or its nearest neighbour in the view */
    PPC_ANCHOR_RANK,     /* this position in the view */
    PPC_ANCHOR_US,       /* the first packet at or after this time (us since the first) */
};

#define PPC_HOST_TEXT 48
/* A view's classes mask for every class. */
#define PPC_CLASSES_ALL 0xffu
/* The inspector's hex lines carry this many bytes each. */
#define PPC_HEX_PER_LINE 8u

/* Row and packet flags (the row's last field, pkt.begin's flags). */
#define PPC_ROWF_PINNED 0x01u
#define PPC_ROWF_GONE 0x02u      /* the bytes left the buffer: summary only */
#define PPC_ROWF_TRUNCATED 0x04u
#define PPC_ROWF_MALFORMED 0x08u
#define PPC_ROWF_GRO 0x10u

struct ppc_cmd {
    enum ppc_cmd_kind kind;
    char iface[PPC_IFACE_MAX];
    uint8_t keep;      /* enum ppc_keep */
    uint8_t promisc;
    uint8_t cfilter;   /* enum ppc_cfilter */
    uint32_t limit_s;  /* 0: no limit */
    uint8_t on;        /* hold */
    uint32_t req;
    uint8_t tail;      /* window anchored at the newest packet */
    uint32_t seq;      /* window anchored at this packet; inspect, pin: the packet */
    uint32_t before;
    uint32_t after;
    uint8_t anchor;    /* enum ppc_anchor (tail and seq say it too) */
    uint64_t at;       /* the rank or the microseconds of those anchors */
    /* view */
    uint32_t view_id;
    uint8_t sort;      /* enum ppc_sort (ppc_view.h) */
    uint8_t classes;
    char host[PPC_HOST_TEXT]; /* "" for any */
    uint16_t port;
    uint8_t pinned;
    uint8_t signals;   /* PPC_SIG_* bits, 0 for any packet */
    uint8_t diag;      /* enum ppc_diag_code, 0 for any packet (M11.1) */
    /* inspect */
    int8_t step;
    /* marks */
    uint32_t buckets;
    /* evid (N1.0), intel (N1.2) */
    uint32_t mark_id;  /* 0: the newest mark */
    uint8_t pair;      /* intel: 1 after against before, 0 before against its reference */
};

/* An interface name as the kernel allows it: 1..15 characters, no slash,
 * no space, not "." or "..". */
int ppc_iface_ok(const char *name);

enum ppc_cmd_kind ppc_cmd_parse(const char *line, struct ppc_cmd *c);

/* Command lines for the shell, without the newline. Each returns the length,
 * or -1 when an argument cannot be sent or the buffer is too small. */
int ppc_cmd_format_start(char *buf, size_t n, const char *iface, enum ppc_keep keep, int promisc,
                         enum ppc_cfilter cf, uint32_t limit_s);
int ppc_cmd_format_hold(char *buf, size_t n, int on);
int ppc_cmd_format_window(char *buf, size_t n, uint32_t req, uint32_t seq, uint32_t before,
                          uint32_t after);
int ppc_cmd_format_window_at(char *buf, size_t n, uint32_t req, enum ppc_anchor anchor, uint64_t at,
                             uint32_t before, uint32_t after);
/* sort: 0 time, 1 newest, 2 len (enum ppc_sort). host NULL or "" for any. */
int ppc_cmd_format_view(char *buf, size_t n, uint32_t id, unsigned sort, uint8_t classes, const char *host,
                        uint16_t port, int pinned);
/* The same with the findings to show (PPC_SIG_* bits, 0: any packet). */
int ppc_cmd_format_view_signals(char *buf, size_t n, uint32_t id, unsigned sort, uint8_t classes, const char *host,
                                uint16_t port, int pinned, unsigned signals);
/* The same with a diagnosis to show (enum ppc_diag_code, 0: any packet; M11.1). */
int ppc_cmd_format_view_diag(char *buf, size_t n, uint32_t id, unsigned sort, uint8_t classes, const char *host,
                             uint16_t port, int pinned, unsigned signals, unsigned diag);
int ppc_cmd_format_inspect(char *buf, size_t n, uint32_t req, uint32_t seq, int step);
int ppc_cmd_format_marks(char *buf, size_t n, uint32_t req, uint32_t buckets);
int ppc_cmd_format_pin(char *buf, size_t n, uint32_t seq, int on);
int ppc_cmd_format_net(char *buf, size_t n, uint32_t req);
int ppc_cmd_format_mark(char *buf, size_t n);
int ppc_cmd_format_evid(char *buf, size_t n, uint32_t req, uint32_t id);
int ppc_cmd_format_intel(char *buf, size_t n, uint32_t req, uint32_t id, unsigned pair);

/* ---- events ---------------------------------------------------------------------- */

enum ppc_evt_kind {
    PPC_EVT_NONE = 0, /* a word this app does not know */
    PPC_EVT_BAD,      /* a known word that did not parse */
    PPC_EVT_HELLO,
    PPC_EVT_STATE,
    PPC_EVT_STATS,
    PPC_EVT_BIN,
    PPC_EVT_ROWS_BEGIN,
    PPC_EVT_ROW,
    PPC_EVT_ROWS_END,
    PPC_EVT_ERROR,
    PPC_EVT_NOTE,
    PPC_EVT_BYE,
    PPC_EVT_PKT_BEGIN,
    PPC_EVT_PKT_ENDS,
    PPC_EVT_LAYER,
    PPC_EVT_FIELD,
    PPC_EVT_HEX,
    PPC_EVT_PKT_END,
    PPC_EVT_PINNED,
    PPC_EVT_FINDING,
    PPC_EVT_MARKS,
    PPC_EVT_SAVED,
    PPC_EVT_DIAG,   /* M11.1 */
    PPC_EVT_DETAIL, /* M11.1: u.finding carries it */
    PPC_EVT_NET,       /* M12.1: the totals */
    PPC_EVT_NET_BEGIN, /* u.net_req */
    PPC_EVT_NET_HOST,
    PPC_EVT_NET_SRV,
    PPC_EVT_NET_LEASE,
    PPC_EVT_NET_EVT,
    PPC_EVT_NET_END,   /* u.net_req */
    PPC_EVT_MARK,      /* N1.0: a mark was set */
    PPC_EVT_EV_BEGIN,  /* u.ev_head */
    PPC_EVT_EV_WIN,
    PPC_EVT_EV_OBS,
    PPC_EVT_EV_END,    /* u.ev_req */
    PPC_EVT_IN_BEGIN,  /* N1.2: u.in_head */
    PPC_EVT_IN_VIS,    /* u.in_vis */
    PPC_EVT_IN_CMP,    /* u.in_cmp */
    PPC_EVT_IN_END,    /* u.ev_req */
};

#define PPC_FIELD_NAME 24
#define PPC_FIELD_TEXT 112
#define PPC_FILTER_TEXT 56
#define PPC_HEX_TEXT 32
#define PPC_ASCII_TEXT 12
#define PPC_MARK_BUCKETS 64u
#define PPC_SAVE_NAME 48

/* SAVE's phases (the `saved` line). */
enum ppc_save_phase { PPC_SAVE_RUN = 0, PPC_SAVE_DONE, PPC_SAVE_FAIL };

struct ppc_counters {
    uint64_t elapsed_ms;
    uint64_t seen;
    uint64_t stored;
    uint64_t held;
    uint64_t kdrop;
    uint64_t ifdrop;
    uint64_t shed;
    uint64_t bytes;
    uint64_t pps;
    uint64_t bps;
    uint32_t first_seq;
    uint32_t last_seq;
    uint32_t match;
    uint32_t fill_permille;
    uint64_t ui_skipped;
    uint64_t cls[PPC_CLASS_COUNT];
    uint32_t sig[PPC_SIGNAL_COUNT];
};

/* The DNS and TCP diagnosis (M11.1, docs/apps/PACKETSHARK.md §4b): running
 * totals since START, and what the connection table holds now. The `diag`
 * line carries them in this order, which leaves the struct no padding. */
struct ppc_diag {
    /* DNS transactions (UDP port 53) */
    uint32_t dns_queries;      /* distinct queries */
    uint32_t dns_retries;      /* copies of a query sent again */
    uint32_t dns_answered;     /* answered within PPC_DNS_WAIT_NS */
    uint32_t dns_late;         /* answered after they were called unanswered */
    uint32_t dns_noreply;      /* no answer seen within PPC_DNS_WAIT_NS */
    uint32_t dns_waiting;      /* asked, the wait not over */
    uint32_t dns_orphan;       /* answers whose query this capture did not see */
    uint32_t dns_unjudged;     /* queries forgotten unjudged (frames passed unseen, table full) */
    /* response codes of DNS and LLMNR answers */
    uint32_t dns_nxdomain;
    uint32_t dns_servfail;
    uint32_t dns_refused;
    uint32_t dns_other;        /* any other code but NOERROR */
    /* response time, from the latest copy of the query; over answered + late */
    uint64_t dns_rtt_sum_us;
    uint32_t dns_rtt_max_us;
    uint32_t dns_rtt_max_seq;  /* the slowest answer's packet */
    /* TCP connections */
    uint32_t tcp_attempts;     /* SYNs that began a connection */
    uint32_t tcp_connected;    /* handshakes completed */
    uint32_t tcp_timed;        /* of those, timed from their first SYN */
    uint32_t tcp_hs_max_us;
    uint64_t tcp_hs_sum_us;    /* first SYN to the segment that completed it */
    uint32_t tcp_hs_max_seq;   /* the slowest handshake's completing packet */
    uint32_t tcp_refused;      /* resets answering a SYN */
    uint32_t tcp_aborted;      /* resets of connections open or opening */
    uint32_t tcp_rst_closed;   /* resets after a FIN or an earlier reset */
    uint32_t tcp_rst_unseen;   /* resets of connections never seen */
    uint32_t tcp_segments;     /* segments that carry sequence space, SYNs aside */
    uint32_t tcp_retrans;      /* sent again, the original seen here */
    uint32_t tcp_retrans_acked;/* sent again after the receiver had acknowledged it */
    uint32_t tcp_retrans_unseen;/* filled a gap later than a round trip */
    uint32_t tcp_syn_again;    /* a SYN or SYN-ACK sent again */
    uint32_t tcp_ooo;          /* filled a gap within a round trip: reordering */
    uint32_t tcp_midstream;    /* connections first seen without their handshake */
    uint32_t tcp_flows;        /* connections in the table now */
    uint32_t tcp_oneway;       /* of those, seen in one direction only (three segments or more) */
    uint32_t tcp_replaced;     /* entries replaced for lack of room */
    uint32_t blinded;          /* times frames passed unjudged (HOLD, shedding) */
};
#define PPC_DIAG_FIELDS 36

/* ---- the network intelligence (M12.1, docs/apps/PACKETSHARK.md §4c) ----------- */

/* What a device was seen to be (net.host's flags). */
#define PPC_NH_IP 0x01u      /* its IPv4 address is known, from ARP or a DHCP ACK */
#define PPC_NH_SELF 0x02u    /* this device: it sent the frames the capture calls outgoing */
#define PPC_NH_ROUTER 0x04u  /* a DHCP server named its address as the router */
#define PPC_NH_DHCP 0x08u    /* it answered as a DHCP server */
#define PPC_NH_ALL 0x0fu

/* An event's kind. The comment says what its addresses are; the app's words
 * for each are in apps/packetshark/psk_net.c. Part of the protocol. */
enum ppc_net_kind {
    PPC_NK_NONE = 0,
    PPC_NK_DHCP_SERVERS,     /* a second DHCP server answered: ip it, mac its frame's source, ip2 the first */
    PPC_NK_DHCP_NAK,         /* a server refused a request: ip the server, mac the client */
    PPC_NK_DHCP_DECLINE,     /* a client declined its address as in use: ip it, mac the client, ip2 the server */
    PPC_NK_DHCP_NO_OFFER,    /* a DISCOVER no OFFER was seen to: mac the client */
    PPC_NK_DHCP_NO_ACK,      /* a REQUEST no ACK or NAK was seen to: mac the client, ip asked for, ip2 the server */
    PPC_NK_DHCP_SHORT_LEASE, /* a lease under PPC_NET_SHORT_LEASE_S: ip the server, mac the client, ip2 the address */
    PPC_NK_ARP_CONFLICT,     /* an address claimed during a probe for it: ip, mac the claimant, mac2 the prober */
    PPC_NK_ARP_DUPLICATE,    /* two MACs claim one address in turn: ip, mac, mac2 */
    PPC_NK_ARP_MOVED,        /* an address changed MAC: ip, mac the new, mac2 the old */
    PPC_NK_ARP_ROUTER_MOVED, /* the same, of the router a DHCP server named */
    PPC_NK_ARP_NO_REPLY,     /* this device's ARP requests unanswered: ip asked for */
    PPC_NK_ARP_SWEEP,        /* a station asking for many addresses: mac, ip its own */
    PPC_NK_ARP_SENDER,       /* an ARP sender MAC that is not the frame's source: ip, mac the frame's, mac2 ARP's */
    PPC_NK_COUNT
};

enum ppc_net_sev { PPC_NS_NOTE = 0, PPC_NS_WARN };

/* The totals since START. The `net` line carries them in this order. */
struct ppc_net_totals {
    uint32_t hosts;          /* devices in the table now */
    uint32_t hosts_replaced; /* devices forgotten for lack of room */
    /* ARP */
    uint32_t arp_requests;
    uint32_t arp_replies;
    uint32_t arp_gratuitous; /* of those, sender and target address the same */
    uint32_t arp_probes;     /* requests with no sender address (RFC 5227) */
    uint32_t arp_other;      /* any other opcode */
    uint32_t arp_binds;      /* addresses a MAC has claimed */
    uint32_t arp_moved;      /* times an address changed MAC */
    uint32_t arp_flips;      /* times two MACs claimed one address in turn */
    uint32_t arp_conflicts;  /* claims during a probe */
    uint32_t arp_noreply;    /* this device's requests left unanswered */
    /* DHCP messages */
    uint32_t dhcp_discover;
    uint32_t dhcp_offer;
    uint32_t dhcp_request;
    uint32_t dhcp_ack;
    uint32_t dhcp_nak;
    uint32_t dhcp_decline;
    uint32_t dhcp_release;
    uint32_t dhcp_inform;
    /* DHCP transactions */
    uint32_t dhcp_tx;        /* transactions seen (transaction id and client MAC) */
    uint32_t dhcp_complete;  /* of those, acknowledged */
    uint32_t dhcp_no_offer;  /* a DISCOVER whose OFFER would have been seen here, and was not */
    uint32_t dhcp_no_ack;    /* the same of a REQUEST */
    uint32_t dhcp_unseen;    /* requests not judged: their answer is unicast to another device */
    uint32_t dhcp_unjudged;  /* requests forgotten unjudged (frames passed unseen, table full) */
    uint32_t dhcp_late;      /* answered after they were called unanswered */
    uint32_t dhcp_waiting;   /* requests whose wait is not over */
    uint32_t dhcp_servers;   /* servers seen answering */
    /* events */
    uint32_t events_warn;    /* in the ring now, by severity */
    uint32_t events_note;
    uint32_t events_lost;    /* events that left the ring for newer ones */
    uint32_t blinded;        /* times frames passed unjudged (HOLD, shedding) */
};
#define PPC_NET_FIELDS 33

#define PPC_NET_NAME 20      /* a DHCP host name, cut to fit */
#define PPC_IP_TEXT 16
#define PPC_MAC_TEXT 18
#define PPC_NET_WIRE_HOSTS 8u
#define PPC_NET_WIRE_SERVERS 4u
#define PPC_NET_WIRE_LEASES 6u
#define PPC_NET_WIRE_EVENTS 12u

struct ppc_evt_net_host {
    char mac[PPC_MAC_TEXT];
    char ip[PPC_IP_TEXT];    /* "" when not known */
    uint8_t flags;           /* PPC_NH_* */
    uint32_t pkts;
    uint64_t bytes;
    char name[PPC_NET_NAME];
};

struct ppc_evt_net_srv {
    char ip[PPC_IP_TEXT];
    char mac[PPC_MAC_TEXT];
    uint8_t relayed;
    uint32_t offers;
    uint32_t acks;
    uint32_t naks;
    uint32_t seq;
    uint32_t lease_s;
    char router[PPC_IP_TEXT];
    char dns[PPC_IP_TEXT];
    char mask[PPC_IP_TEXT];
};

struct ppc_evt_net_lease {
    char ip[PPC_IP_TEXT];
    char mac[PPC_MAC_TEXT];
    char server[PPC_IP_TEXT];
    uint32_t lease_s;
    uint64_t rel_us;
    uint32_t seq;
    char name[PPC_NET_NAME];
};

struct ppc_evt_net_event {
    uint8_t kind;            /* enum ppc_net_kind */
    uint8_t sev;             /* enum ppc_net_sev */
    uint32_t count;
    uint32_t seq;
    uint64_t first_us;
    uint64_t last_us;
    char ip[PPC_IP_TEXT];
    char mac[PPC_MAC_TEXT];
    char ip2[PPC_IP_TEXT];
    char mac2[PPC_MAC_TEXT];
};

/* ---- marks and their evidence (N1.0, docs/apps/PACKETSHARK.md §4d) ---------------- */

/* What a packet stated: an observation's kind. Part of the protocol (the
 * `ev.obs` line carries one, the `ev.win` line a count for each after the
 * first, in this order). */
enum ppc_ev_kind {
    PPC_EV_NONE = 0,
    PPC_EV_DHCP_DISCOVER,
    PPC_EV_DHCP_OFFER,
    PPC_EV_DHCP_REQUEST,
    PPC_EV_DHCP_ACK,
    PPC_EV_DHCP_NAK,
    PPC_EV_DHCP_DECLINE,
    PPC_EV_DHCP_RELEASE,
    PPC_EV_DHCP_INFORM,
    PPC_EV_ARP_REQUEST,
    PPC_EV_ARP_REPLY,
    PPC_EV_ARP_PROBE,    /* a request with no sender address (RFC 5227) */
    PPC_EV_ARP_ANNOUNCE, /* sender and target address the same */
    PPC_EV_ARP_CHANGED,  /* an address answered for by another MAC than before */
    PPC_EV_ARP_TWO_MACS, /* two MACs claimed one address in turn, or during a probe */
    PPC_EV_DNS_QUERY,
    PPC_EV_DNS_RETRY,    /* the same query sent again */
    PPC_EV_DNS_ANSWER,
    PPC_EV_DNS_FAILURE,  /* an answer with a response code other than NOERROR */
    PPC_EV_KINDS
};

/* An observation's flags. */
#define PPC_EVF_OWN 0x01u        /* this device sent it, or it was addressed to this device */
#define PPC_EVF_BCAST 0x02u      /* broadcast or multicast: every port of the segment sees it */
#define PPC_EVF_REPLY_HERE 0x04u /* a request whose reply would pass this interface */
#define PPC_EVF_PLAIN 0x08u      /* routine: another station's ordinary ARP request or reply */
#define PPC_EVF_UNPAIRED 0x10u   /* a DNS answer with no matching query to time it by: never
                                  * seen, or no longer remembered (PPC_DNS_KEEP_NS, PPC_DNS_QUERIES) */
#define PPC_EVF_V6 0x20u         /* over IPv6: the addresses are not carried */
#define PPC_EVF_ALL 0x3fu

/* A window's flags. */
#define PPC_EVW_CUT_START 0x01u /* the capture started inside the window */
#define PPC_EVW_CUT_STOP 0x02u  /* the capture stopped inside it */
#define PPC_EVW_OPEN 0x04u      /* the window is still filling */
#define PPC_EVW_HISTORY 0x08u   /* observations had left the history: the counts are of what remained */
#define PPC_EVW_LOSSES_DROPPED 0x10u /* the record of spells without a counter reading did not keep them all */
#define PPC_EVW_ALL 0x1fu

/* What is known of the frames lost around a mark, in neither window. */
enum ppc_ev_edge {
    PPC_EV_EDGE_NONE = 0, /* the counters were read at the mark: every loss is on one side */
    PPC_EV_EDGE_COUNTED,  /* not read at the mark; the next reading told how many, not when */
    PPC_EV_EDGE_UNKNOWN,  /* not read since before the mark: not known yet, or never (STOP) */
};

#define PPC_EV_WINDOW_MS 60000u
#define PPC_EV_KEEP 12u      /* rows kept with a mark, each side */
/* The classes a mark's evidence observes, as a mask of enum ppc_class. */
#define PPC_EV_CLASSES ((uint8_t)(1u << PPC_CLASS_DHCP | 1u << PPC_CLASS_ARP | 1u << PPC_CLASS_DNS))

/* What the buffer still holds of an observation's packet. */
enum ppc_ev_alive {
    PPC_EV_GONE = 0,   /* the packet has left the buffer: its number is all that is left */
    PPC_EV_SUMMARY,    /* its row is there, its bytes have gone */
    PPC_EV_WHOLE,
};

struct ppc_evt_mark {
    uint32_t id;
    uint32_t first;    /* the oldest mark still held */
    uint32_t at_ms;    /* since START */
    uint8_t state;     /* enum ppc_state when it was set */
    uint32_t seq;      /* the newest packet then; 0: none yet */
    uint64_t wall_s;   /* the wall clock then; 0: not set */
};

/* Protocol 7 (N1.2): a mark's comparisons, as ppc_intel made them. */
#define PPC_IN_METRICS 5u /* dns.time, dns.answered, dns.failed, dhcp.discover, arp.request */
#define PPC_IN_REF_MAX 4u

struct ppc_evt_in_head {
    uint32_t req;
    uint32_t mark;     /* the mark's id */
    uint8_t pair;
    uint32_t at_ms;
    uint32_t now_ms;
    uint8_t final;
};

struct ppc_evt_in_vis {
    uint8_t side;
    uint32_t from_ms, to_ms;
    uint32_t watched_s, held_s, shed_s, gap_s;
    uint32_t frames;
    uint32_t lost_lo, lost_hi;
    uint8_t loss_known;
    uint32_t own_in, own_out;
};

struct ppc_evt_in_cmp {
    uint8_t metric;    /* enum ppc_in_metric */
    uint8_t verdict;   /* enum ppc_in_verdict */
    uint8_t quality;   /* enum ppc_in_quality */
    uint8_t reason;    /* enum ppc_in_reason */
    uint8_t notes;     /* PPC_INN_* */
    uint32_t n[2], k[2], stat[2], und[2];
    uint32_t p_e6;
    uint32_t ref[PPC_IN_REF_MAX];   /* 0: none */
    uint8_t alive[PPC_IN_REF_MAX];  /* PPC_EV_GONE .. PPC_EV_WHOLE */
};

struct ppc_evt_ev_head {
    uint32_t req;
    uint32_t id;
    uint32_t first;
    uint32_t last;
    uint32_t at_ms;
    uint8_t state;
    uint32_t seq;
    uint64_t wall_s;
    uint32_t now_ms;   /* the session's clock as the answer was made */
    uint8_t cfilter;   /* enum ppc_cfilter: the capture filter the source applied */
    uint8_t unanalyzed; /* PPC_EV_CLASSES whose analysis could not start */
    uint8_t edge;       /* enum ppc_ev_edge */
    uint32_t edge_from_ms; /* PPC_EV_EDGE_COUNTED, _UNKNOWN: the last good reading before the mark */
    uint32_t edge_to_ms;   /* PPC_EV_EDGE_COUNTED: the first after it */
    uint64_t edge_lost;    /* PPC_EV_EDGE_COUNTED: what it told was lost in between */
};

struct ppc_evt_ev_win {
    uint8_t side;      /* 0 before the mark, 1 after it */
    uint8_t flags;     /* PPC_EVW_* */
    uint32_t span_ms;
    uint32_t watched_s;
    uint32_t held_s;
    uint32_t shed_s;
    uint64_t lost;
    uint32_t kept;
    uint32_t more;
    uint32_t count[PPC_EV_KINDS]; /* [0] is not sent */
    uint64_t lost_near;  /* lost in a spell reaching past the window's far end: apart */
    uint8_t unknown;     /* a spell without a counter reading is open in the window */
    uint32_t unknown_from_ms; /* the last good reading before it */
};

struct ppc_evt_ev_obs {
    uint8_t side;
    uint8_t kind;      /* enum ppc_ev_kind */
    uint8_t flags;     /* PPC_EVF_* */
    uint8_t alive;     /* enum ppc_ev_alive */
    uint32_t t_ms;
    uint32_t seq;
    char mac[PPC_MAC_TEXT];
    char ip[PPC_IP_TEXT];
    char ip2[PPC_IP_TEXT];
    uint32_t a;
    uint32_t b;
};

struct ppc_evt_row {
    uint32_t seq;
    uint64_t rel_us;
    uint64_t gap_us;
    char dir[6];
    uint8_t cls;
    char word[8];
    uint32_t len;
    uint16_t signal;
    char src[PPC_ADDR_TEXT];
    char dst[PPC_ADDR_TEXT];
    char info[PPC_INFO_TEXT];
    uint8_t flags;     /* PPC_ROWF_* */
};

/* pkt.begin: the inspected packet's head. */
struct ppc_evt_pkt {
    uint32_t req;
    uint32_t seq;
    uint32_t rank;     /* its place in the view; match when it is not in it */
    uint32_t match;
    uint64_t rel_us;
    uint64_t gap_us;
    char dir[6];
    uint8_t cls;
    char word[8];
    uint32_t wire_len;
    uint32_t stored;
    uint16_t signal;
    uint8_t flags;     /* PPC_ROWF_* */
};

struct ppc_evt_end {
    char addr[PPC_ADDR_TEXT];
    char sub[PPC_ADDR_TEXT];
    char filter[PPC_FILTER_TEXT];
};

struct ppc_evt {
    enum ppc_evt_kind kind;
    union {
        struct {
            uint32_t proto;
            char version[32];
            char backend[16];
        } hello;
        struct {
            enum ppc_state state;
            enum ppc_reason reason;
            char iface[PPC_IFACE_MAX];
            uint32_t speed;
            uint8_t clock_set;
        } state;
        struct ppc_counters stats;
        struct {
            uint64_t second;
            uint32_t cls[PPC_CLASS_COUNT];
            uint32_t held;
        } bin;
        struct {
            uint32_t req;
            uint32_t filter_id;
            uint32_t first_rank;
            uint32_t match;
            uint32_t first_seq;
            uint32_t last_seq;
            uint32_t anchor_rank; /* the anchor's rank; first_rank when not sent */
        } rows;
        struct ppc_evt_row row;
        struct {
            char code[16];
            char text[PPC_TEXT_MAX];
        } error;
        struct {
            char key[16];
            char value[64];
        } note;
        struct ppc_evt_pkt pkt;
        struct {
            struct ppc_evt_end src;
            struct ppc_evt_end dst;
        } ends;
        struct {
            uint32_t index;
            int cls;           /* -1 for none */
            char name[PPC_FIELD_NAME];
            char text[PPC_FIELD_TEXT];
        } layer;
        struct {
            uint32_t layer;
            char name[PPC_FIELD_NAME];
            char text[PPC_FIELD_TEXT];
            int32_t off;       /* -1 for none */
            uint32_t len;
            char filter[PPC_FILTER_TEXT]; /* "" for none */
        } field;
        struct {
            uint32_t off;
            char hex[PPC_HEX_TEXT];
            char ascii[PPC_ASCII_TEXT];
        } hex;
        struct {
            uint32_t seq;
            uint8_t on;
            uint32_t pins;
        } pinned;
        struct {
            uint32_t layer;
            char text[PPC_FIELD_TEXT];
        } finding;
        struct {
            uint32_t req;
            uint32_t view_id;
            uint32_t match;
            uint32_t buckets;
            uint64_t mask;     /* bit i: a finding in part i of the view */
        } marks;
        struct {
            uint8_t phase;     /* enum ppc_save_phase */
            uint32_t packets;
            uint32_t total;
            uint64_t bytes;
            char name[PPC_SAVE_NAME]; /* the file's name, or the failure's code */
        } saved;
        struct ppc_diag diag;
        struct ppc_net_totals net;
        uint32_t net_req;
        struct ppc_evt_net_host net_host;
        struct ppc_evt_net_srv net_srv;
        struct ppc_evt_net_lease net_lease;
        struct ppc_evt_net_event net_evt;
        struct ppc_evt_mark mark;
        struct ppc_evt_ev_head ev_head;
        struct ppc_evt_ev_win ev_win;
        struct ppc_evt_ev_obs ev_obs;
        uint32_t ev_req;
        struct ppc_evt_in_head in_head;
        struct ppc_evt_in_vis in_vis;
        struct ppc_evt_in_cmp in_cmp;
    } u;
};

enum ppc_evt_kind ppc_evt_parse(const char *line, struct ppc_evt *e);

/* Event lines for the helper, WITH the newline. Each returns the length, or
 * -1 when the buffer is too small (nothing useful written). Free text is
 * sanitized and cut to fit. */
int ppc_evt_format_hello(char *buf, size_t n, const char *version, const char *backend);
int ppc_evt_format_state(char *buf, size_t n, enum ppc_state s, enum ppc_reason r, const char *iface,
                         uint32_t speed, int clock_set);
int ppc_evt_format_stats(char *buf, size_t n, const struct ppc_counters *c);
int ppc_evt_format_bin(char *buf, size_t n, uint64_t second, const uint32_t cls[PPC_CLASS_COUNT],
                       uint32_t held);
int ppc_evt_format_rows_begin(char *buf, size_t n, uint32_t req, uint32_t filter_id,
                              uint32_t first_rank, uint32_t match, uint32_t first_seq,
                              uint32_t last_seq);
int ppc_evt_format_row(char *buf, size_t n, const struct ppc_evt_row *row);
/* The same with the rank the window was anchored at (protocol 2). */
int ppc_evt_format_rows_begin_at(char *buf, size_t n, uint32_t req, uint32_t filter_id, uint32_t first_rank,
                                 uint32_t match, uint32_t first_seq, uint32_t last_seq, uint32_t anchor_rank);
int ppc_evt_format_rows_end(char *buf, size_t n, uint32_t req);
int ppc_evt_format_error(char *buf, size_t n, const char *code, const char *text);
int ppc_evt_format_note(char *buf, size_t n, const char *key, const char *value);
int ppc_evt_format_bye(char *buf, size_t n);
int ppc_evt_format_pkt_begin(char *buf, size_t n, const struct ppc_evt_pkt *p);
int ppc_evt_format_pkt_ends(char *buf, size_t n, const struct ppc_evt_end *src, const struct ppc_evt_end *dst);
/* cls < 0: none. */
int ppc_evt_format_layer(char *buf, size_t n, uint32_t index, int cls, const char *name, const char *text);
/* off < 0: none; filter NULL or "": none. */
int ppc_evt_format_field(char *buf, size_t n, uint32_t layer, const char *name, const char *text, int32_t off,
                         uint32_t len, const char *filter);
int ppc_evt_format_hex(char *buf, size_t n, uint32_t off, const char *hex, const char *ascii);
int ppc_evt_format_pkt_end(char *buf, size_t n, uint32_t req);
int ppc_evt_format_pinned(char *buf, size_t n, uint32_t seq, int on, uint32_t pins);
int ppc_evt_format_finding(char *buf, size_t n, uint32_t layer, const char *text);
int ppc_evt_format_detail(char *buf, size_t n, uint32_t layer, const char *text);
int ppc_evt_format_diag(char *buf, size_t n, const struct ppc_diag *d);
int ppc_evt_format_marks(char *buf, size_t n, uint32_t req, uint32_t view_id, uint32_t match, uint32_t buckets,
                         uint64_t mask);
/* The network intelligence (M12.1). Addresses are text the helper made
 * ("192.0.2.1", "02:00:5e:10:00:01"; "" for none); anything else in their
 * place is refused by the parser. A name is sanitized like any free text. */
int ppc_evt_format_net(char *buf, size_t n, const struct ppc_net_totals *t);
int ppc_evt_format_net_begin(char *buf, size_t n, uint32_t req);
int ppc_evt_format_net_host(char *buf, size_t n, const struct ppc_evt_net_host *h);
int ppc_evt_format_net_srv(char *buf, size_t n, const struct ppc_evt_net_srv *s);
int ppc_evt_format_net_lease(char *buf, size_t n, const struct ppc_evt_net_lease *l);
int ppc_evt_format_net_evt(char *buf, size_t n, const struct ppc_evt_net_event *e);
int ppc_evt_format_net_end(char *buf, size_t n, uint32_t req);
/* Marks and their evidence (N1.0). Addresses are text the helper made, as
 * in the network intelligence's lines. */
int ppc_evt_format_mark(char *buf, size_t n, const struct ppc_evt_mark *m);
int ppc_evt_format_ev_begin(char *buf, size_t n, const struct ppc_evt_ev_head *h);
int ppc_evt_format_ev_win(char *buf, size_t n, const struct ppc_evt_ev_win *w);
int ppc_evt_format_ev_obs(char *buf, size_t n, const struct ppc_evt_ev_obs *o);
int ppc_evt_format_ev_end(char *buf, size_t n, uint32_t req);
int ppc_evt_format_in_begin(char *buf, size_t n, const struct ppc_evt_in_head *h);
int ppc_evt_format_in_vis(char *buf, size_t n, const struct ppc_evt_in_vis *v);
int ppc_evt_format_in_cmp(char *buf, size_t n, const struct ppc_evt_in_cmp *m);
int ppc_evt_format_in_end(char *buf, size_t n, uint32_t req);
int ppc_evt_format_saved(char *buf, size_t n, enum ppc_save_phase phase, uint32_t packets, uint32_t total,
                         uint64_t bytes, const char *name);
/* A saved capture's name as the helper makes it: "capture-" then digits
 * and dashes, ".pcap"; nothing else is ever shown or deleted. */
int ppc_save_name_ok(const char *name);
/* The folder SAVE writes to and the app lists (ADR-016 rule 9):
 * $POCKETOS_CAPTURES_DIR for the simulator and the tests, else
 * $HOME/Captures, which is /root/Captures on the unit. Returns buf, or NULL
 * when it does not fit or names a place under /tmp, where no capture goes. */
const char *ppc_captures_dir(char *buf, size_t n);

#endif
