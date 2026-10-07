/*
 * system.logs and system.crashes. See sysd_logs.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE
#include "sysd_logs.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define NAME_MAX_LEN 48
#define TS_LEN 24 /* 2026-09-04T13:20:01.123Z */

struct entry {
    char ts[TS_LEN + 1];
    char source[NAME_MAX_LEN];
    enum sysd_log_level level;
    char message[SYSD_LOGS_MESSAGE_MAX + 1];
    unsigned long seq;
};

struct collect {
    const struct sysd_logs_query *q;
    struct entry *best;   /* newest first, at most q->limit */
    int count;
    unsigned long seq;
    long skipped;
    long scanned;
    bool older_not_scanned;
};

static const char *const level_words[] = { "debug", "info", "warn", "error" };

/* ---- names ------------------------------------------------------------- */

static bool name_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '_' || c == '-';
}

/* "<base>.log" with a base of name characters only: radiod.log and
 * supervise-radiod.log, not radiod.stdio.log or radiod.log.1. */
static bool log_base(const char *file, char *base, size_t n)
{
    size_t len = strlen(file);
    size_t i;

    if (len < 5 || len - 4 >= n || strcmp(file + len - 4, ".log") != 0) {
        return false;
    }
    for (i = 0; i < len - 4; i++) {
        if (!name_char(file[i])) {
            return false;
        }
    }
    memcpy(base, file, len - 4);
    base[len - 4] = '\0';
    return true;
}

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* ---- text -------------------------------------------------------------- */

/* Length of the valid UTF-8 sequence at s (at most avail bytes), or 0. */
static size_t utf8_len(const unsigned char *s, size_t avail)
{
    size_t need;
    size_t i;

    if (s[0] < 0x80) {
        return 1;
    }
    if (s[0] >= 0xc2 && s[0] <= 0xdf) {
        need = 2;
    } else if (s[0] >= 0xe0 && s[0] <= 0xef) {
        need = 3;
    } else if (s[0] >= 0xf0 && s[0] <= 0xf4) {
        need = 4;
    } else {
        return 0;
    }
    if (need > avail) {
        return 0;
    }
    for (i = 1; i < need; i++) {
        if ((s[i] & 0xc0) != 0x80) {
            return 0;
        }
    }
    return need;
}

/* Copy len bytes of src into dst (size SYSD_LOGS_MESSAGE_MAX + 1): control
 * characters become spaces, invalid UTF-8 becomes '?', and a message that
 * does not fit ends in "..." cut on a character boundary, so the panel
 * never receives half a character or a terminal escape. */
static void clean_message(const char *src, size_t len, char *dst)
{
    const unsigned char *s = (const unsigned char *)src;
    size_t in = 0;
    size_t out = 0;
    const size_t cap = SYSD_LOGS_MESSAGE_MAX;

    while (in < len) {
        size_t l = utf8_len(s + in, len - in);

        if (l == 0) {
            if (out + 1 > cap) {
                break;
            }
            dst[out++] = '?';
            in++;
            continue;
        }
        if (out + l > cap) {
            break;
        }
        if (l == 1) {
            dst[out++] = (s[in] < 0x20 || s[in] == 0x7f) ? ' ' : (char)s[in];
        } else {
            memcpy(dst + out, s + in, l);
            out += l;
        }
        in += l;
    }
    if (in < len) {
        /* Back off to leave room for the ellipsis on a character boundary. */
        while (out > cap - 3) {
            out--;
            while (out > 0 && ((unsigned char)dst[out] & 0xc0) == 0x80) {
                out--;
            }
        }
        memcpy(dst + out, "...", 3);
        out += 3;
    }
    while (out > 0 && dst[out - 1] == ' ') {
        out--;
    }
    dst[out] = '\0';
}

static bool digits(const char *s, int n)
{
    int i;

    for (i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') {
            return false;
        }
    }
    return true;
}

/* "YYYY-MM-DDTHH:MM:SS" at s. */
static bool ts_prefix(const char *s, size_t len)
{
    return len >= 19 && digits(s, 4) && s[4] == '-' && digits(s + 5, 2) && s[7] == '-' &&
           digits(s + 8, 2) && s[10] == 'T' && digits(s + 11, 2) && s[13] == ':' &&
           digits(s + 14, 2) && s[16] == ':' && digits(s + 17, 2);
}

static int level_of(const char *w, size_t n)
{
    if (n == 5 && memcmp(w, "DEBUG", 5) == 0) {
        return SYSD_LOG_DEBUG;
    }
    if (n == 4 && memcmp(w, "INFO", 4) == 0) {
        return SYSD_LOG_INFO;
    }
    if (n == 4 && memcmp(w, "WARN", 4) == 0) {
        return SYSD_LOG_WARN;
    }
    if (n == 5 && memcmp(w, "ERROR", 5) == 0) {
        return SYSD_LOG_ERROR;
    }
    return -1;
}

/* One pocketlog line: "<ts> <name> <LEVEL> <message>". */
static bool parse_pocketlog(const char *line, size_t len, struct entry *e)
{
    const char *p = line + TS_LEN;
    const char *end = line + len;
    const char *w;
    int level;

    if (len < TS_LEN + 2 || !ts_prefix(line, len) || line[19] != '.' || !digits(line + 20, 3) ||
        line[23] != 'Z' || line[24] != ' ') {
        return false;
    }
    memcpy(e->ts, line, TS_LEN);
    e->ts[TS_LEN] = '\0';
    p++;
    while (p < end && *p != ' ') { /* the process name */
        p++;
    }
    while (p < end && *p == ' ') {
        p++;
    }
    w = p;
    while (p < end && *p != ' ') {
        p++;
    }
    level = level_of(w, (size_t)(p - w));
    if (level < 0) {
        return false;
    }
    while (p < end && *p == ' ') {
        p++;
    }
    e->level = (enum sysd_log_level)level;
    clean_message(p, (size_t)(end - p), e->message);
    return true;
}

/* One pos-supervise line: "<ts>Z supervise <name> <what happened>". What
 * happened is the level: a crash loop is an error, an exit is a warning (the
 * service died and was restarted), anything else - "stopped" - is info. */
static bool parse_supervise(const char *line, size_t len, struct entry *e)
{
    static const char tag[] = " supervise ";
    const char *p;

    if (len < 20 + sizeof(tag) || !ts_prefix(line, len) || line[19] != 'Z' ||
        memcmp(line + 20, tag, sizeof(tag) - 1) != 0) {
        return false;
    }
    memcpy(e->ts, line, 19);
    memcpy(e->ts + 19, ".000Z", 6);
    p = line + 20 + sizeof(tag) - 1;
    clean_message(p, (size_t)(line + len - p), e->message);
    if (strstr(e->message, " crash loop")) {
        e->level = SYSD_LOG_ERROR;
    } else if (strstr(e->message, " exited ")) {
        e->level = SYSD_LOG_WARN;
    } else {
        e->level = SYSD_LOG_INFO;
    }
    return true;
}

/* ---- collecting -------------------------------------------------------- */

static bool newer(const struct entry *a, const struct entry *b)
{
    int c = strcmp(a->ts, b->ts);

    return c > 0 || (c == 0 && a->seq > b->seq);
}

/* Keep the newest q->limit entries, sorted newest first. */
static void offer(struct collect *c, struct entry *e)
{
    int i;

    e->seq = ++c->seq;
    if (c->count == c->q->limit && !newer(e, &c->best[c->count - 1])) {
        return;
    }
    i = c->count < c->q->limit ? c->count++ : c->count - 1;
    while (i > 0 && newer(e, &c->best[i - 1])) {
        c->best[i] = c->best[i - 1];
        i--;
    }
    c->best[i] = *e;
}

static void take_line(struct collect *c, const char *source, bool supervise, const char *line,
                      size_t len)
{
    struct entry e;
    bool ok;

    while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == ' ')) {
        len--;
    }
    if (len == 0) {
        return;
    }
    memset(&e, 0, sizeof(e));
    ok = supervise ? parse_supervise(line, len, &e) : parse_pocketlog(line, len, &e);
    if (!ok) {
        c->skipped++;
        return;
    }
    if (e.level < c->q->min_level) {
        return;
    }
    snprintf(e.source, sizeof(e.source), "%s", source);
    offer(c, &e);
}

/* The last `budget` bytes of path, line by line. Returns the bytes read, or
 * -1 when the file cannot be opened. *more is set when older content was
 * left unread. */
static long read_tail(struct collect *c, const char *path, long budget, const char *source,
                      bool supervise, bool *more)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    struct stat st;
    off_t from;
    char *buf;
    ssize_t got;
    size_t start = 0;
    size_t i;

    *more = false;
    if (fd < 0) {
        return -1;
    }
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || budget <= 0) {
        close(fd);
        return 0;
    }
    from = st.st_size > budget ? st.st_size - budget : 0;
    buf = malloc((size_t)budget + 1);
    if (!buf) {
        close(fd);
        return 0;
    }
    got = pread(fd, buf, (size_t)budget, from);
    close(fd);
    if (got <= 0) {
        free(buf);
        return 0;
    }
    if (from > 0) {
        /* Started mid-line: the first line is not a line. */
        *more = true;
        while (start < (size_t)got && buf[start] != '\n') {
            start++;
        }
        start++;
    }
    for (i = start; i < (size_t)got; i++) {
        if (buf[i] == '\n') {
            take_line(c, source, supervise, buf + start, i - start);
            start = i + 1;
        }
    }
    if (start < (size_t)got) { /* a last line still being written */
        take_line(c, source, supervise, buf + start, (size_t)got - start);
    }
    free(buf);
    return (long)got;
}

static void scan_file(struct collect *c, const char *dir, const char *base, cJSON *sources)
{
    char path[640];
    char rotated[648];
    struct stat st;
    long budget = SYSD_LOGS_TAIL_BYTES;
    long got;
    bool supervise = strncmp(base, "supervise-", 10) == 0;
    bool more = false;

    snprintf(path, sizeof(path), "%s/%s.log", dir, base);
    snprintf(rotated, sizeof(rotated), "%s.1", path);
    /* The rotated file is older, so it is read first and only for what the
     * current one leaves of the budget. */
    if (stat(path, &st) == 0 && st.st_size < budget) {
        got = read_tail(c, rotated, budget - (long)st.st_size, base, supervise, &more);
        if (got > 0) {
            c->scanned += got;
            c->older_not_scanned |= more;
        }
    } else if (access(rotated, F_OK) == 0) {
        c->older_not_scanned = true;
    }
    got = read_tail(c, path, budget, base, supervise, &more);
    if (got >= 0) {
        c->scanned += got;
        c->older_not_scanned |= more;
        cJSON_AddItemToArray(sources, cJSON_CreateString(base));
    }
}

int sysd_logs_parse_query(const cJSON *params, struct sysd_logs_query *q, char *err, size_t n)
{
    const cJSON *v;

    q->min_level = SYSD_LOG_DEBUG;
    q->limit = SYSD_LOGS_LIMIT_DEFAULT;
    q->source = NULL;
    if (!params || cJSON_IsNull(params)) {
        return 0;
    }
    if (!cJSON_IsObject(params)) {
        snprintf(err, n, "params must be an object");
        return -1;
    }
    v = cJSON_GetObjectItemCaseSensitive(params, "level");
    if (v) {
        if (!cJSON_IsString(v)) {
            snprintf(err, n, "level must be all, info, warn or error");
            return -1;
        }
        if (strcmp(v->valuestring, "all") == 0) {
            q->min_level = SYSD_LOG_DEBUG;
        } else if (strcmp(v->valuestring, "info") == 0) {
            q->min_level = SYSD_LOG_INFO;
        } else if (strcmp(v->valuestring, "warn") == 0) {
            q->min_level = SYSD_LOG_WARN;
        } else if (strcmp(v->valuestring, "error") == 0) {
            q->min_level = SYSD_LOG_ERROR;
        } else {
            snprintf(err, n, "level must be all, info, warn or error");
            return -1;
        }
    }
    v = cJSON_GetObjectItemCaseSensitive(params, "limit");
    if (v) {
        if (!cJSON_IsNumber(v) || v->valuedouble != (double)(int)v->valuedouble ||
            v->valueint < 1 || v->valueint > SYSD_LOGS_LIMIT_MAX) {
            snprintf(err, n, "limit must be an integer from 1 to %d", SYSD_LOGS_LIMIT_MAX);
            return -1;
        }
        q->limit = v->valueint;
    }
    v = cJSON_GetObjectItemCaseSensitive(params, "source");
    if (v) {
        size_t i;
        char base[NAME_MAX_LEN];

        if (!cJSON_IsString(v) || !v->valuestring[0] ||
            strlen(v->valuestring) >= sizeof(base)) {
            snprintf(err, n, "source must be a log name such as radiod");
            return -1;
        }
        for (i = 0; v->valuestring[i]; i++) {
            if (!name_char(v->valuestring[i])) {
                snprintf(err, n, "source must be a log name such as radiod");
                return -1;
            }
        }
        q->source = v->valuestring;
    }
    return 0;
}

cJSON *sysd_logs_query(const char *log_dir, const struct sysd_logs_query *q)
{
    cJSON *o = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();
    cJSON *sources = cJSON_CreateArray();
    struct collect c;
    DIR *d = opendir(log_dir);
    char *names[SYSD_LOGS_MAX_FILES * 4];
    int nnames = 0;
    int i;

    memset(&c, 0, sizeof(c));
    c.q = q;
    c.best = calloc((size_t)q->limit, sizeof(*c.best));
    if (d && c.best) {
        struct dirent *de;
        char base[NAME_MAX_LEN];

        while ((de = readdir(d)) != NULL &&
               nnames < (int)(sizeof(names) / sizeof(names[0]))) {
            if (log_base(de->d_name, base, sizeof(base)) &&
                (!q->source || strcmp(q->source, base) == 0)) {
                names[nnames] = strdup(base);
                if (names[nnames]) {
                    nnames++;
                }
            }
        }
        /* The same files in the same order on every call. */
        qsort(names, (size_t)nnames, sizeof(names[0]), cmp_str);
        for (i = 0; i < nnames; i++) {
            if (i < SYSD_LOGS_MAX_FILES) {
                scan_file(&c, log_dir, names[i], sources);
            } else {
                c.older_not_scanned = true;
            }
            free(names[i]);
        }
    }
    if (d) {
        closedir(d);
    }
    for (i = 0; i < c.count; i++) {
        cJSON *e = cJSON_CreateObject();

        cJSON_AddStringToObject(e, "ts", c.best[i].ts);
        cJSON_AddStringToObject(e, "source", c.best[i].source);
        cJSON_AddStringToObject(e, "level", level_words[c.best[i].level]);
        cJSON_AddStringToObject(e, "message", c.best[i].message);
        cJSON_AddItemToArray(arr, e);
    }
    free(c.best);
    cJSON_AddBoolToObject(o, "available", d != NULL);
    cJSON_AddItemToObject(o, "entries", arr);
    cJSON_AddNumberToObject(o, "returned", c.count);
    cJSON_AddNumberToObject(o, "skipped", (double)c.skipped);
    cJSON_AddItemToObject(o, "sources", sources);
    cJSON_AddNumberToObject(o, "scanned_bytes", (double)c.scanned);
    cJSON_AddBoolToObject(o, "older_not_scanned", c.older_not_scanned);
    return o;
}

/* ---- crash reports ----------------------------------------------------- */

struct crash {
    char file[NAME_MAX_LEN * 2];
    char process[NAME_MAX_LEN * 2];
    long time;
    long pid;
};

/* crash-<process>-<unixtime>-<pid>.txt, the process name possibly holding
 * dashes itself, so the two numbers are taken from the right. */
static bool crash_name(const char *file, struct crash *cr)
{
    size_t len = strlen(file);
    char tmp[NAME_MAX_LEN * 2];
    char *dash;
    char *end;
    size_t i;

    if (len < 6 + 4 + 4 || len >= sizeof(tmp) || strncmp(file, "crash-", 6) != 0 ||
        strcmp(file + len - 4, ".txt") != 0) {
        return false;
    }
    for (i = 0; i < len; i++) {
        if (!name_char(file[i]) && file[i] != '.') {
            return false;
        }
    }
    memcpy(tmp, file + 6, len - 10);
    tmp[len - 10] = '\0';
    dash = strrchr(tmp, '-');
    if (!dash || !dash[1]) {
        return false;
    }
    cr->pid = strtol(dash + 1, &end, 10);
    if (*end) {
        return false;
    }
    *dash = '\0';
    dash = strrchr(tmp, '-');
    if (!dash || !dash[1] || dash == tmp) {
        return false;
    }
    cr->time = strtol(dash + 1, &end, 10);
    if (*end) {
        return false;
    }
    *dash = '\0';
    snprintf(cr->process, sizeof(cr->process), "%s", tmp);
    memcpy(cr->file, file, len + 1); /* len < sizeof(tmp) == sizeof(cr->file) */
    return true;
}

static int crash_newer_first(const void *a, const void *b)
{
    const struct crash *x = a;
    const struct crash *y = b;

    if (x->time != y->time) {
        return x->time > y->time ? -1 : 1;
    }
    return strcmp(y->file, x->file);
}

static const char *signal_name(long sig)
{
    switch (sig) {
    case SIGSEGV:
        return "SIGSEGV";
    case SIGBUS:
        return "SIGBUS";
    case SIGILL:
        return "SIGILL";
    case SIGFPE:
        return "SIGFPE";
    case SIGABRT:
        return "SIGABRT";
    default:
        return NULL;
    }
}

/* The header of one report, read from its first 2 KiB only. */
static cJSON *crash_json(const char *dir, const struct crash *cr)
{
    cJSON *o = cJSON_CreateObject();
    cJSON *frames = cJSON_CreateArray();
    char path[700];
    char buf[2048];
    char version[64] = "";
    char build[64] = "";
    long sig = -1;
    int nframes = 0;
    bool in_trace = false;
    FILE *f;

    snprintf(path, sizeof(path), "%s/%s", dir, cr->file);
    f = fopen(path, "r");
    if (f) {
        size_t got = fread(buf, 1, sizeof(buf) - 1, f);
        char *line = buf;

        fclose(f);
        buf[got] = '\0';
        while (line && *line) {
            char *nl = strchr(line, '\n');

            if (nl) {
                *nl = '\0';
            } else if (line + strlen(line) == buf + got && got == sizeof(buf) - 1) {
                break; /* cut by the read, not a whole line */
            }
            if (in_trace) {
                if (nframes < SYSD_CRASH_FRAMES && *line) {
                    char clean[SYSD_LOGS_MESSAGE_MAX + 1];

                    clean_message(line, strlen(line), clean);
                    cJSON_AddItemToArray(frames, cJSON_CreateString(clean));
                    nframes++;
                }
            } else if (strncmp(line, "version: ", 9) == 0) {
                clean_message(line + 9, strlen(line + 9) < 60 ? strlen(line + 9) : 60, version);
            } else if (strncmp(line, "build: ", 7) == 0) {
                clean_message(line + 7, strlen(line + 7) < 60 ? strlen(line + 7) : 60, build);
            } else if (strncmp(line, "signal: ", 8) == 0) {
                sig = strtol(line + 8, NULL, 10);
            } else if (strcmp(line, "backtrace:") == 0) {
                in_trace = true;
            }
            line = nl ? nl + 1 : NULL;
        }
    }
    cJSON_AddStringToObject(o, "file", cr->file);
    cJSON_AddStringToObject(o, "process", cr->process);
    cJSON_AddNumberToObject(o, "pid", (double)cr->pid);
    cJSON_AddNumberToObject(o, "time", (double)cr->time);
    if (sig >= 0) {
        const char *name = signal_name(sig);

        cJSON_AddNumberToObject(o, "signal", (double)sig);
        if (name) {
            cJSON_AddStringToObject(o, "signal_name", name);
        } else {
            cJSON_AddNullToObject(o, "signal_name");
        }
    } else {
        cJSON_AddNullToObject(o, "signal");
        cJSON_AddNullToObject(o, "signal_name");
    }
    if (version[0]) {
        cJSON_AddStringToObject(o, "version", version);
    } else {
        cJSON_AddNullToObject(o, "version");
    }
    if (build[0]) {
        cJSON_AddStringToObject(o, "build", build);
    } else {
        cJSON_AddNullToObject(o, "build");
    }
    cJSON_AddItemToObject(o, "frames", frames);
    return o;
}

cJSON *sysd_crashes_list(const char *log_dir)
{
    cJSON *o = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();
    DIR *d = opendir(log_dir);
    struct crash *all = NULL;
    size_t n = 0;
    size_t cap = 0;
    long total = 0;
    size_t i;

    if (d) {
        struct dirent *de;
        struct crash cr;

        /* Every report is counted; only their names are held, and at most
         * 256 of them are sorted - a card with more than that has a problem
         * this page is not going to explain one by one. */
        while ((de = readdir(d)) != NULL) {
            if (!crash_name(de->d_name, &cr)) {
                continue;
            }
            total++;
            if (n == cap) {
                struct crash *grown;

                if (cap >= 256) {
                    continue;
                }
                cap = cap ? cap * 2 : 16;
                grown = realloc(all, cap * sizeof(*all));
                if (!grown) {
                    break;
                }
                all = grown;
            }
            all[n++] = cr;
        }
        closedir(d);
    }
    if (n > 0) {
        qsort(all, n, sizeof(*all), crash_newer_first);
    }
    for (i = 0; i < n && i < SYSD_CRASHES_MAX; i++) {
        cJSON_AddItemToArray(arr, crash_json(log_dir, &all[i]));
    }
    free(all);
    cJSON_AddBoolToObject(o, "available", d != NULL);
    cJSON_AddItemToObject(o, "reports", arr);
    cJSON_AddNumberToObject(o, "total", (double)total);
    return o;
}
