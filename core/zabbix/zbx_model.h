/*
 * The Zabbix viewer's data model: what a monitoring terminal keeps of a
 * Zabbix server between two refreshes, and nothing more.
 *
 * Everything here is bounded. A server with ten thousand hosts and two
 * thousand open problems still costs this model the same fixed number of
 * bytes: the helper (tools/zabbix/pos_zabbix.c) asks the server for at most
 * ZBX_*_FETCH records, sorts what came back, and keeps the ZBX_*_MAX most
 * important ones. Totals beyond what is kept are carried as numbers, with a
 * flag saying whether they are exact, so a screen can say "100 of 1 734"
 * instead of pretending the list is complete.
 *
 * Strings are fixed arrays, cut at a UTF-8 character boundary, never
 * allocated. Nothing in this file allocates at all.
 *
 * Pure C, no LVGL, no JSON, no network: the helper fills it from the API
 * (zbx_api.c), the line protocol carries it to the app (zbx_proto.c), and
 * the app draws it. The same header on both sides of the socket.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef POCKETOS_ZBX_MODEL_H
#define POCKETOS_ZBX_MODEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---- limits --------------------------------------------------------------- */

/* Kept and shown. */
#define ZBX_PROBLEM_MAX 100
#define ZBX_HOST_MAX 200
#define ZBX_DETAIL_PROBLEM_MAX 20
#define ZBX_ITEM_MAX 12
/* Asked for per refresh (the API's own "limit"). Sorting happens on what
 * arrives, so the kept set is the most severe / most important of these. */
#define ZBX_PROBLEM_FETCH 500
#define ZBX_HOST_FETCH 500
#define ZBX_ITEM_FETCH 200

/* String sizes, terminator included. Zabbix allows longer names (host 128,
 * trigger 255); longer ones are cut and end in "...". */
#define ZBX_ID_MAX 24           /* Zabbix ids are unsigned 64-bit decimals */
#define ZBX_HOST_NAME_MAX 64
#define ZBX_PROBLEM_NAME_MAX 128
#define ZBX_ITEM_NAME_MAX 64
#define ZBX_VALUE_MAX 40
#define ZBX_UNITS_MAX 16
#define ZBX_TEXT_MAX 96         /* error texts, labels */
#define ZBX_VERSION_MAX 16
#define ZBX_URL_MAX 256

/* ---- severity --------------------------------------------------------------- */

/* Zabbix trigger severities, in the API's own numbering. */
enum zbx_severity {
    ZBX_SEV_NONE = -1,          /* no problem at all */
    ZBX_SEV_NOT_CLASSIFIED = 0,
    ZBX_SEV_INFORMATION = 1,
    ZBX_SEV_WARNING = 2,
    ZBX_SEV_AVERAGE = 3,
    ZBX_SEV_HIGH = 4,
    ZBX_SEV_DISASTER = 5,
};
#define ZBX_SEV_COUNT 6

/* "DISASTER", "HIGH", "AVERAGE", "WARNING", "INFO", "N/C"; "OK" for NONE. */
const char *zbx_severity_word(int severity);
/* The full name for a detail line: "Not classified" ... "Disaster", "None". */
const char *zbx_severity_name(int severity);
/* Clamp anything the wire or the API said into -1..5. */
int zbx_severity_clamp(long v);

/* ---- hosts ---------------------------------------------------------------- */

/* Whether the server can reach a host, from its interfaces' "available"
 * (0 unknown, 1 available, 2 unavailable) and, from Zabbix 6.4, the agent's
 * active checks ("active_available", the same three values). */
enum zbx_avail {
    ZBX_AVAIL_UNKNOWN = 0,      /* nothing has been checked, or no interface */
    ZBX_AVAIL_UP = 1,
    ZBX_AVAIL_DOWN = 2,
};

const char *zbx_avail_word(enum zbx_avail a); /* "UP", "DOWN", "UNKNOWN" */

/* Combine one more interface state into a host's: any unavailable interface
 * makes the host DOWN, all-available makes it UP, anything else leaves it as
 * it was. Start from ZBX_AVAIL_UNKNOWN with seen_up = false. */
enum zbx_avail zbx_avail_combine(enum zbx_avail host, long iface_available, bool *seen_up);

struct zbx_host {
    char hostid[ZBX_ID_MAX];
    char name[ZBX_HOST_NAME_MAX];
    enum zbx_avail avail;
    bool maintenance;
    int problems;               /* open problems on this host among those fetched */
    int8_t max_severity;        /* ZBX_SEV_NONE when problems == 0 */
};

/* ---- problems --------------------------------------------------------------- */

struct zbx_problem {
    char eventid[ZBX_ID_MAX];
    char objectid[ZBX_ID_MAX];  /* the trigger */
    char hostid[ZBX_ID_MAX];    /* the first host of the trigger, or "" */
    char host[ZBX_HOST_NAME_MAX];
    char name[ZBX_PROBLEM_NAME_MAX];
    int8_t severity;
    bool acknowledged;
    bool suppressed;
    int64_t clock;              /* when it started, Unix seconds (server time) */
};

/* ---- latest values ------------------------------------------------------------- */

struct zbx_item {
    char itemid[ZBX_ID_MAX];
    char name[ZBX_ITEM_NAME_MAX];
    char value[ZBX_VALUE_MAX];
    char units[ZBX_UNITS_MAX];
    int64_t clock;              /* lastclock, 0 when never collected */
};

/* ---- the sets the helper sends ------------------------------------------------ */

/* One refresh of the problem list: the ZBX_PROBLEM_MAX most important open
 * problems, most severe first, newest first within a severity. */
struct zbx_problem_set {
    struct zbx_problem p[ZBX_PROBLEM_MAX];
    int count;                  /* kept */
    int total;                  /* open problems on the server */
    bool total_exact;           /* false: the server had more than it was asked for */
    int sev_count[ZBX_SEV_COUNT];
    bool sev_exact;             /* the per-severity counts cover every open problem */
    int unacknowledged;         /* among those fetched */
    int64_t ref_clock;          /* the server's time when this was read, Unix seconds */
};

struct zbx_host_set {
    struct zbx_host h[ZBX_HOST_MAX];
    int count;                  /* kept */
    int total;                  /* monitored hosts on the server */
    bool total_exact;
    int fetched;                /* the ones availability was counted over */
    bool down_exact;            /* down covers every monitored host, fetched or not */
    int down;
    int unknown;
    int maintenance;
    int64_t ref_clock;
};

/* One host, opened. */
struct zbx_detail {
    struct zbx_host host;
    struct zbx_problem p[ZBX_DETAIL_PROBLEM_MAX];
    int problem_count;
    struct zbx_item item[ZBX_ITEM_MAX];
    int item_count;
    int64_t ref_clock;
};

/* The OVERVIEW numbers, derived from the newest of each set. */
struct zbx_overview {
    int hosts_total;
    bool hosts_exact;           /* unknown and maintenance cover every host */
    int hosts_down;
    bool down_exact;            /* down does */
    int hosts_unknown;
    int hosts_maintenance;
    int problems_total;
    bool problems_exact;
    int unacknowledged;
    int sev_count[ZBX_SEV_COUNT];
    int8_t max_severity;        /* ZBX_SEV_NONE: no open problem */
};

/* ---- operations ------------------------------------------------------------------ */

/* Copy src into dst of dstlen bytes, never splitting a UTF-8 character, and
 * turn control characters (tab and newline included) into spaces, so any
 * string that went through here is safe on one protocol line and one label.
 * When it does not fit, the end is "..." (or as much of it as fits).
 * Returns true when the whole string fitted. */
bool zbx_copy_text(char *dst, size_t dstlen, const char *src);

/* Order two numeric id strings ("1234" < "12345"); ids that are not decimal
 * compare as text after every decimal one. */
int zbx_id_cmp(const char *a, const char *b);

/* Sort problems most severe first, then newest (clock, then eventid). */
void zbx_problems_sort(struct zbx_problem *p, int n);
/* Hosts that need attention first: most severe problem, then DOWN, UNKNOWN,
 * UP, then maintenance last, then by name without case. */
void zbx_hosts_sort(struct zbx_host *h, int n);

/* Recount unacknowledged and per-severity counts over the kept problems.
 * For a set built from everything the server has (total_exact), this makes
 * the counts exact. */
void zbx_problem_set_count(struct zbx_problem_set *s);

/* Set every host's problem count and worst severity from a list of
 * problems. Problems match hosts by hostid; a problem with no host counts
 * for none. */
void zbx_hosts_apply(struct zbx_host *h, int nh, const struct zbx_problem *p, int np);

/* Recount down/unknown/maintenance over the kept hosts. */
void zbx_host_set_count(struct zbx_host_set *s);

/* The OVERVIEW numbers from the two sets; either may be NULL (not read yet). */
void zbx_overview_build(struct zbx_overview *o, const struct zbx_problem_set *problems,
                        const struct zbx_host_set *hosts);

/* How long ago, for a list: "now", "42s", "5m", "3h 12m", "2d 4h", "61d".
 * Unknown (clock <= 0 or in the future by more than a minute): "--". */
void zbx_format_age(char *out, size_t len, int64_t now, int64_t clock);

#endif
