/*
 * RIFT's words: every string the screens print, decided here.
 *
 * No LVGL, no cJSON, no I/O. The parts of a mesh screen that go wrong are
 * the parts that say something the data does not support - a hop count
 * printed as 0 because nobody measured it, an RSSI attributed to a node
 * nine relays away, a path drawn through hops nobody named, a remote name
 * cut in the middle of a UTF-8 character - so they are decided in pure C
 * and tested without a display (tests/rift_format_test.c).
 *
 * Two rules from the design handoff (docs/design/rift/HANDOFF.md §6, §7):
 *
 *   - "?" when the backend could report a value and has not; U+2014 when
 *     the value cannot exist at all, such as an end-to-end RSSI over
 *     relays. Never 0, never a bar, never "good".
 *   - The full hop list is kept and never truncated; only the *rendering*
 *     compresses, and the numeric hop count is always printed beside it.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef RIFT_FORMAT_H
#define RIFT_FORMAT_H

#include "rift_model.h"

#include <stddef.h>
#include <stdint.h>

/* The characters the product fonts carry (tools/design/gen_fonts.sh covers
 * 0x20-0x7E, 0xA0-0xFF, 0x2013-0x2026, 0x2039-0x203A, 0x2190-0x2212), named
 * once so no screen spells a code point itself. */
#define RIFT_SEP " \xC2\xB7 "          /* U+00B7 middle dot, spaced */
#define RIFT_ARROW " \xE2\x80\xBA "    /* U+203A single right guillemet */
#define RIFT_ELLIPSIS "\xE2\x80\xA6"   /* U+2026 */
#define RIFT_EMDASH "\xE2\x80\x94"     /* U+2014: the value cannot exist */
#define RIFT_MINUS "\xE2\x88\x92"      /* U+2212: a real minus, not a hyphen */
#define RIFT_UNKNOWN "?"               /* the value could be known and is not */

/* Copy at most out_len-1 bytes of src, never splitting a UTF-8 character,
 * and always NUL-terminating. Remote names arrive from meshcored already
 * well-formed (docs/api/mesh.md, "Remote text"); this keeps them that way
 * when they are longer than the field that holds them. Returns the bytes
 * written. src may be NULL, which writes "". */
size_t rift_utf8_copy(char *out, size_t out_len, const char *src);

/* As rift_utf8_copy, but a string that had to be cut ends in U+2026, so a
 * shortened name is visibly shortened rather than silently wrong. */
size_t rift_utf8_ellipsis(char *out, size_t out_len, const char *src);

/* ---- ages -------------------------------------------------------------- */

#define RIFT_AGE_MAX 8

/* "12s", "6m", "2h", "5d", ">99d", or "?" when the time is not known. A
 * negative age - meshcored's monotonic clock ahead of ours, which can only
 * be a fault - is "?" and never a time in the future. */
void rift_fmt_age(int64_t age_ms, int known, char *out, size_t out_len);
/* The same interval as two fields for the detail panel's big value: "12"
 * and "min". value or unit may be NULL. Unknown gives "?" and "". */
void rift_fmt_age_split(int64_t age_ms, int known, char *value, size_t value_len, char *unit,
                        size_t unit_len);

/* ---- activity ------------------------------------------------------------
 *
 * How lately something was heard from, as one of four words. It is the age
 * of the newest thing RIFT has actually observed from that subject, bucketed,
 * and nothing else: no signal, no count, no guess about a link. Signal is
 * the RSSI and SNR columns' business, and the two are never combined.
 *
 *   NOW      heard within the last 5 minutes
 *   RECENT   within the last hour
 *   QUIET    within the last 12 hours (RIFT_STALE_MS, the NODES boundary)
 *   STALE    longer ago than that
 *   ?        never heard, or an age this app cannot trust (a stamp in the
 *            future) - unknown, which is not the same as stale
 *
 * What "heard" is depends on the subject, and is the observation the API
 * already reports: a node's last_heard_mono_ms (meshcored stamps it on an
 * advert, a direct message and peer data from that node); a direct
 * conversation's peer, the same, or its newest incoming message when the
 * node has left the cache; a channel's newest incoming message. Nothing
 * this device sent counts: sending proves nothing about who is listening. */
enum rift_pulse {
    RIFT_PULSE_NONE = 0,
    RIFT_PULSE_STALE,
    RIFT_PULSE_QUIET,
    RIFT_PULSE_RECENT,
    RIFT_PULSE_NOW,
};

#define RIFT_PULSE_NOW_MS (5 * 60 * 1000LL)
#define RIFT_PULSE_RECENT_MS (60 * 60 * 1000LL)

enum rift_pulse rift_pulse_of(int64_t age_ms, int known);
/* "NOW", "RECENT", "QUIET", "STALE", or RIFT_UNKNOWN. */
const char *rift_pulse_word(enum rift_pulse p);

/* ---- signal ------------------------------------------------------------ */

#define RIFT_SIGNAL_MAX 12

/* RSSI as a whole number of dBm with a real minus sign, or "?". */
void rift_fmt_rssi(double dbm, int known, char *out, size_t out_len);
/* SNR to one decimal, or "?". */
void rift_fmt_snr(double db, int known, char *out, size_t out_len);

/* ---- a node's link ----------------------------------------------------- */

enum rift_link rift_link_of(const struct rift_node *n);
/* Not heard for longer than RIFT_STALE_MS. A node never heard at all is not
 * stale; it is unheard, which the list groups separately. */
int rift_node_is_stale(const struct rift_node *n, int64_t now_ms);

/* The hop column: "DIR" for direct, the relay count for relayed, "?" when
 * no route back is known. Never a bare 0 (handoff §6). */
#define RIFT_HOPS_MAX 6
void rift_fmt_hops(const struct rift_node *n, char *out, size_t out_len);

/* The state line, in the fixed order state · hop count · uncertainty:
 * "DIRECT", "RELAYED · 8 HOPS", "RELAYED · 8 HOPS · 2 UNKNOWN HOPS",
 * "NO PATH". */
#define RIFT_STATE_MAX 64
void rift_fmt_state(const struct rift_node *n, char *out, size_t out_len);

/* What a node is called on screen: its name when it has one, else its hash
 * in the form the API gives it. Never empty. */
#define RIFT_LABEL_MAX RIFT_NAME_MAX
void rift_fmt_label(const struct rift_node *n, char *out, size_t out_len);

/* MeshCore's ADV_TYPE_*: "chat", "repeater", "room", "sensor". NULL when
 * the type was not reported or is not one this build knows - an unknown
 * number is not turned into a word. */
const char *rift_type_word(int type, int have_type);
/* The short role tag beside a name in a row: "RPT" for a repeater, "ROOM",
 * "SENS", or NULL for a plain chat node and for an unknown type. */
const char *rift_type_tag(int type, int have_type);

/* Identity accents (DS §37). What is somebody gets a stable hue: a chat
 * node, a room, a channel, and a channel sender by the name it claims. A
 * repeater or a sensor is infrastructure and gets none; nor does a node
 * whose type was never reported, because a guess at who someone is would
 * be a colour that changes when the answer comes. The hue is a hash of
 * what identifies them - the public key, the channel's on-air hash, the
 * claimed name - reduced by the caller to the palette's size, so the same
 * one is the same colour on every screen and every reader's device. */
int rift_ident_for_type(int type, int have_type);
/* FNV-1a over the bytes, so a key and a name hash the same way anywhere. */
uint32_t rift_ident_hash(const char *text);

/* A public key as the detail screen shows it: four groups of four hex
 * digits, an ellipsis, and the last four - "3F9A C21E 7D04 … 88B1". A key
 * that is not 64 hex characters is printed as far as it goes rather than
 * padded. */
#define RIFT_KEY_SHORT_MAX 40
void rift_fmt_key_short(const char *key, char *out, size_t out_len);

/* ---- the path ---------------------------------------------------------- */

/* A hop's identifier is the hash bytes MeshCore routes on, as hex. The
 * path may carry more than one byte per hop (Packet::pathHashSize), so this
 * is not always two characters. */
#define RIFT_HOP_ID_MAX 17
#define RIFT_MAX_HOPS 64

struct rift_hop {
    char id[RIFT_HOP_ID_MAX];
    int have_id;
};

struct rift_path {
    int known;         /* a route back is known at all */
    int direct;        /* zero relays */
    int hops;          /* relay hops, as the service reported them */
    int identified;    /* relay hops this path carries an identifier for */
    int unknown;       /* hops - identified, never negative */
    int count;         /* entries written in hop[] */
    int bytes_per_hop; /* 0 when the path could not be divided into hops */
    struct rift_hop hop[RIFT_MAX_HOPS];
};

/* Take a node's path apart.
 *
 * The API gives a hop count and an opaque byte string (docs/api/mesh.md,
 * mesh.nodes), and MeshCore packs pathHashSize() bytes per hop. When the
 * string divides exactly by the hop count, every hop has an identifier.
 * When it does not - the path was clipped at MCD_MAX_PATH, or the two
 * disagree - the hops are real but none of them can be named, and they are
 * reported as unknown rather than sliced into plausible pieces. Returns 0,
 * or -1 for input this function will not take (a path_hex that is not an
 * even-length hex string). */
int rift_path_parse(const struct rift_node *n, struct rift_path *out);

/* The hop strip (handoff §7): self, the relays, the target. More than four
 * relays compresses to the first two, a "+n" cell and the last one; the
 * numeric hop count is printed separately and is never compressed. */
enum rift_cell {
    RIFT_CELL_SELF = 0,
    RIFT_CELL_RELAY,     /* a relay this path names */
    RIFT_CELL_UNKNOWN,   /* a relay this path does not name */
    RIFT_CELL_MORE,      /* "+n" */
    RIFT_CELL_TARGET,
};

struct rift_strip_cell {
    enum rift_cell kind;
    int more;            /* RIFT_CELL_MORE only: how many are folded in */
    int hop;             /* relay index, 0-based; -1 for self, target, more */
};

/* self + two relays + "+n" + one relay + target. */
#define RIFT_STRIP_MAX 6
/* Compress once there are more relays than this (handoff §7). */
#define RIFT_STRIP_RELAYS_MAX 4

int rift_strip_build(const struct rift_path *p, struct rift_strip_cell *out, int max);

/* The inline chain: "K230 › RPT-NORD › 7f › ? › HYTTA". A hop the path
 * does not name is "?", never a plausible hash. resolve may be NULL; it is
 * asked for a name for a hop identifier and may answer NULL.
 *
 * A chain too long for out - a path of 63 hops, each named - keeps both
 * ends and says how many hops it left out of the middle ("… +41 …"),
 * rather than being cut off before its target. The ladder on the node's
 * detail lists every hop. */
#define RIFT_CHAIN_MAX 1024
typedef const char *(*rift_resolve_fn)(const char *hop_id, void *user);
void rift_path_chain(const char *self_label, const struct rift_path *p, const char *target_label,
                     rift_resolve_fn resolve, void *user, char *out, size_t out_len);

/* One line of the portrait hop ladder / landscape hop table:
 *   index 0            self
 *   1..hops            a relay
 *   hops + 1           the target
 * Writes the hop's label (a resolved name, a hash, or "?") and its kind.
 * Returns 0, or -1 when index is past the end of the path. */
struct rift_ladder_row {
    int index;
    enum rift_cell kind;
    char label[RIFT_NAME_MAX];
    char note[RIFT_TEXT_MAX];
};

int rift_path_ladder_row(const struct rift_path *p, int index, const char *self_label,
                         const char *target_label, rift_resolve_fn resolve, void *user,
                         struct rift_ladder_row *out);

/* ---- messages ----------------------------------------------------------- */

/* The state word of a message, in the vocabulary of the handoff §6 rather
 * than the API's: DELIVERED is what an ACK means to a reader, where the API
 * calls it "acked". SENT is never DELIVERED - accepted is not transmitted
 * and transmitted is not acknowledged (docs/api/mesh.md).
 *
 * A state this build does not know prints the service's own word rather
 * than the nearest one this build has. */
#define RIFT_MSG_STATE_MAX 24
void rift_fmt_msg_state(const struct rift_message *msg, char *out, size_t out_len);

/* Whether a message's caption is a warning: no ACK, failed, and a state
 * word this build does not recognise. Colour never carries it alone - the
 * word says it too - but the caption takes status_warn when this is set
 * (handoff §5). */
int rift_msg_is_warn(const struct rift_message *msg);

/* The whole caption under a message body, in the fixed order
 * state · evidence:
 *
 *   "RECEIVED"                     nothing was measured
 *   "RECEIVED \xC2\xB7 \xE2\x88\x9288 dBm \xC2\xB7 SNR 7.3"
 *   "DELIVERED \xC2\xB7 ACK 41 s"
 *   "SENT \xC2\xB7 FLOOD"          submitted, no acknowledgement yet
 *   "NO ACK"
 *   "FAILED"
 *
 * The design also puts a per-message hop count and route in this caption
 * ("RECEIVED \xC2\xB7 PATH 9"). The API carries no path on a message, only on a
 * node, so this does not print one: the route belongs to the thread header
 * and the route pane, where it is the peer's current path and is true. */
#define RIFT_MSG_CAPTION_MAX 96
void rift_fmt_msg_caption(const struct rift_message *msg, char *out, size_t out_len);

/* How long the ACK took, as "41 s" / "1m 04s", or "" when it has not been
 * acknowledged or the service reported no stamps to measure between. */
#define RIFT_ACK_MAX 12
void rift_fmt_ack(const struct rift_message *msg, char *out, size_t out_len);

/* A conversation row's preview: the newest message on one line, prefixed
 * "you: " when this device sent it. Newlines and tabs - the two control
 * characters the API lets through - become spaces, because a row is one
 * line and a body that wrapped would push the columns after it off the
 * row. */
#define RIFT_PREVIEW_MAX 96
void rift_fmt_preview(const struct rift_message *msg, char *out, size_t out_len);

/* What was said, without the "<sender>: " MeshCore writes into a channel
 * payload - taken off only when it is exactly the sender_name the service
 * parsed out of it. A direct message's text, and a channel payload that does
 * not start that way, come back whole. Never NULL. */
const char *rift_msg_body(const struct rift_message *msg);

/* The one caption line under a message body, in the fixed order
 * age · [claimed sender] · state · evidence:
 *
 *   "4m · DELIVERED · ACK 41 s"
 *   "1m · RECEIVED · −88 dBm · SNR 6.5"
 *   "45s · HYTTA? · RECEIVED"          a channel line names who it claims
 *
 * A direct thread names nobody here: its header names the peer, and the side
 * of the rule says which of the two a line is. */
#define RIFT_MSG_META_MAX (RIFT_AGE_MAX + RIFT_NAME_MAX + RIFT_MSG_CAPTION_MAX + 16)
void rift_fmt_msg_meta(const struct rift_message *msg, int64_t now_ms, char *out,
                       size_t out_len);
/* The same, with the claimed sender apart from the rest: who gets "HYTTA?"
 * (or UNNAMED) for an incoming channel message and "" otherwise; out gets
 * "age · caption". The thread draws who in the sender's identity accent
 * (DS §37.3), so it has to be its own label. */
void rift_fmt_msg_meta_split(const struct rift_message *msg, int64_t now_ms, char *who,
                             size_t who_len, char *out, size_t out_len);

/* What became of the last advert or node change a reader asked for:
 * "ZERO-HOP ADVERT · ASKED…", "FLOOD ADVERT · ACCEPTED 12s AGO",
 * "ROUTE FORGOTTEN 3s AGO · NEXT MESSAGE FLOODS", "… · NOT DONE: <why>".
 * An advert that was answered is ACCEPTED, never SENT: the service queued
 * it, and the transmit's outcome is the activity feed's. "" when there is
 * nothing to report. */
#define RIFT_ACTION_TEXT_MAX (RIFT_NAME_MAX + RIFT_TEXT_MAX + 48)
void rift_fmt_action(const struct rift_action_state *s, int64_t now_ms, char *out,
                     size_t out_len);

/* Is this text one mesh.send will take? The service refuses a body that is
 * empty, longer than 160 bytes, or carries a control character other than
 * newline and tab, and answers with an error (docs/api/mesh.md). This is
 * the same rule applied before the request is written, so the composer can
 * say why rather than sending something it knows will be refused.
 *
 * Returns 0 when the text is sendable. Otherwise -1, and why holds a
 * sentence for the reader. */
int rift_send_text_check(const char *text, char *why, size_t why_len);

/* The bytes a body would take on the air, which is what the 160-byte limit
 * counts - not characters. */
size_t rift_send_text_bytes(const char *text);

#endif
