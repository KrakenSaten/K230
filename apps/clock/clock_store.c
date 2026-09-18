/*
 * PocketClock persistence. See clock_store.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "clock_store.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define STORE_PATH_MAX 256
/* Longest legal line: the keyword, four small numbers and a full label. */
#define STORE_LINE_MAX 128
/* A settings file far larger than this is not one of ours. */
#define STORE_MAX_LINES (CLOCK_MAX_ALARMS + 8)

static char dir_buf[STORE_PATH_MAX];

const char *clock_store_dir(void)
{
    const char *base = getenv("POCKETOS_STATE_DIR");

    snprintf(dir_buf, sizeof(dir_buf), "%s/%s",
             (base && *base) ? base : CLOCK_STORE_DEFAULT_DIR, CLOCK_STORE_SUBDIR);
    return dir_buf;
}

int clock_store_path(char *out, size_t out_len)
{
    int n = snprintf(out, out_len, "%s/%s", clock_store_dir(), CLOCK_STORE_FILE);

    return (n < 0 || (size_t)n >= out_len) ? -1 : 0;
}

/* Create every missing component of the store directory. Best effort: a
 * failure surfaces as a failed write, which the caller already handles. */
static void make_dirs(const char *dir)
{
    char work[STORE_PATH_MAX];
    size_t i;

    snprintf(work, sizeof(work), "%s", dir);
    for (i = 1; work[i]; i++) {
        if (work[i] != '/') {
            continue;
        }
        work[i] = '\0';
        mkdir(work, 0755);
        work[i] = '/';
    }
    mkdir(work, 0755);
}

int clock_label_is_storable(const char *label)
{
    size_t i;

    if (!label) {
        return 1; /* no label is perfectly storable */
    }
    if (strlen(label) >= CLOCK_LABEL_MAX) {
        return 0;
    }
    for (i = 0; label[i]; i++) {
        unsigned char c = (unsigned char)label[i];

        /* Anything below space would either end the line early or come back
         * as something else. Bytes above 0x7F are UTF-8 continuation and
         * lead bytes, which survive a line intact and are kept. */
        if (c < 0x20 || c == 0x7F) {
            return 0;
        }
    }
    return 1;
}

/* ---- loading ----------------------------------------------------------- */

/* One unsigned field, with no strtol surprises: a field that is not entirely
 * digits is a broken file, not a zero. */
static int parse_uint(const char *s, const char **end, long *out)
{
    long v = 0;
    int digits = 0;

    while (*s == ' ') {
        s++;
    }
    while (*s >= '0' && *s <= '9') {
        if (v > 100000000L) {
            return -1;
        }
        v = v * 10 + (*s - '0');
        s++;
        digits++;
    }
    if (digits == 0) {
        return -1;
    }
    *out = v;
    *end = s;
    return 0;
}

static int parse_alarm(const char *rest, struct clock_engine *e)
{
    long enabled, hour, minute, repeat;
    const char *p = rest;
    int index;

    if (parse_uint(p, &p, &enabled) != 0 || parse_uint(p, &p, &hour) != 0 ||
        parse_uint(p, &p, &minute) != 0 || parse_uint(p, &p, &repeat) != 0) {
        return -1;
    }
    if (enabled > 1 || hour > 23 || minute > 59 || repeat >= CLOCK_REPEAT_COUNT) {
        return -1;
    }
    if (*p == ' ') {
        p++;
    }
    if (!clock_label_is_storable(p)) {
        return -1;
    }
    /* NULL for "now": a stored alarm carries no opinion about today, and the
     * engine settles that on its first valid reading instead. */
    index = clock_alarm_add(e, (int)hour, (int)minute,
                            (enum clock_repeat)repeat, *p ? p : NULL, NULL);
    if (index < 0) {
        return -1;
    }
    if (!enabled) {
        clock_alarm_set_enabled(e, index, false, NULL);
    }
    return 0;
}

int clock_store_load(struct clock_engine *e)
{
    char path[STORE_PATH_MAX];
    char line[STORE_LINE_MAX];
    struct clock_engine work;
    FILE *f;
    int lines = 0;
    int rc = -1;

    if (!e || clock_store_path(path, sizeof(path)) != 0) {
        return -1;
    }
    f = fopen(path, "rb");
    if (!f) {
        return errno == ENOENT ? 1 : -1;
    }

    /* Built up beside the real engine, so a file that turns out to be
     * damaged half way through cannot leave three of five alarms behind. */
    clock_engine_init(&work);

    if (!fgets(line, sizeof(line), f)) {
        goto out;
    }
    line[strcspn(line, "\r\n")] = '\0';
    if (strcmp(line, CLOCK_STORE_MAGIC) != 0) {
        goto out;
    }

    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);

        if (++lines > STORE_MAX_LINES) {
            goto out;
        }
        /* A line that filled the buffer without a newline was truncated, and
         * a truncated label is not the label that was saved. */
        if (len + 1 >= sizeof(line) && line[len - 1] != '\n') {
            goto out;
        }
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] == '\0') {
            continue;
        }
        if (strncmp(line, "alarm ", 6) == 0) {
            if (parse_alarm(line + 6, &work) != 0) {
                goto out;
            }
        } else if (strncmp(line, "timer ", 6) == 0) {
            const char *p = line + 6;
            long seconds;

            if (parse_uint(p, &p, &seconds) != 0 || *p != '\0') {
                goto out;
            }
            /* Zero is a legal stored duration: it means none was ever set.
             * Anything the engine would refuse is a file we do not trust. */
            if (seconds > 0 && !clock_timer_set(&work, 0, 0, (int)seconds)) {
                goto out;
            }
        } else {
            goto out; /* an unknown keyword is a file we do not understand */
        }
    }
    if (ferror(f)) {
        goto out;
    }

    /* Only the persisted parts are taken across; the stopwatch, the ringing
     * state and every fired_day belong to this run. */
    memcpy(e->alarms, work.alarms, sizeof(e->alarms));
    e->alarm_count = work.alarm_count;
    e->timer = work.timer;
    rc = 0;
out:
    fclose(f);
    return rc;
}

/* ---- saving ------------------------------------------------------------ */

int clock_store_save(const struct clock_engine *e)
{
    char path[STORE_PATH_MAX];
    char tmp[STORE_PATH_MAX + 8];
    FILE *f;
    int i;

    if (!e || clock_store_path(path, sizeof(path)) != 0) {
        return -1;
    }
    if ((size_t)snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= sizeof(tmp)) {
        return -1;
    }
    make_dirs(clock_store_dir());
    f = fopen(tmp, "wb");
    if (!f) {
        return -1;
    }
    fprintf(f, "%s\n", CLOCK_STORE_MAGIC);
    /* The duration that was set, never the remaining time of a countdown:
     * one is a setting, the other is elapsed time and does not survive. */
    fprintf(f, "timer %lld\n", (long long)(e->timer.duration_ms / 1000));
    for (i = 0; i < e->alarm_count; i++) {
        const struct clock_alarm *a = &e->alarms[i];
        const char *label = clock_label_is_storable(a->label) ? a->label : "";

        fprintf(f, "alarm %d %u %u %u %s\n", a->enabled ? 1 : 0,
                (unsigned)a->hour, (unsigned)a->minute, (unsigned)a->repeat,
                label);
    }
    if (ferror(f) || fflush(f) != 0 || fsync(fileno(f)) != 0) {
        fclose(f);
        unlink(tmp);
        return -1;
    }
    fclose(f);
    /* rename() is atomic on the same filesystem: a reader sees the previous
     * alarms or these, never a half-written set. An interrupted save leaves
     * only the temporary, which the next save overwrites. */
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

/* ---- the restart handoff ----------------------------------------------- */

/* Magic, mono, wall, alarms, sw, timer, ring, eight alarms and twenty laps,
 * and a little room over. A file longer than this is not one of ours. */
#define HANDOFF_MAX_LINES (7 + CLOCK_MAX_ALARMS + CLOCK_MAX_LAPS)

static char handoff_dir_buf[STORE_PATH_MAX];

const char *clock_handoff_dir(void)
{
    const char *base = getenv("POCKETOS_RUNTIME_DIR");

    snprintf(handoff_dir_buf, sizeof(handoff_dir_buf), "%s/%s",
             (base && *base) ? base : CLOCK_HANDOFF_DEFAULT_DIR, CLOCK_HANDOFF_SUBDIR);
    return handoff_dir_buf;
}

int clock_handoff_path(char *out, size_t out_len)
{
    int n = snprintf(out, out_len, "%s/%s", clock_handoff_dir(), CLOCK_HANDOFF_FILE);

    return (n < 0 || (size_t)n >= out_len) ? -1 : 0;
}

int clock_handoff_clear(void)
{
    char path[STORE_PATH_MAX];

    if (clock_handoff_path(path, sizeof(path)) != 0) {
        return -1;
    }
    if (unlink(path) != 0 && errno != ENOENT) {
        return -1;
    }
    return 0;
}

/* Is there anything here that only this run knows? A shell whose clock has
 * done nothing at all - no alarm, no stopwatch, no countdown, and no real
 * wall clock yet - writes no handoff, which is the reason an exit that has
 * nothing to hand on costs nothing.
 *
 * The last of the five is the one that is not obvious. wall_was_valid, and
 * with it every alarm's fired_day, is what the engine works out the first
 * time it sees a real wall clock: every alarm whose minute has gone by today
 * is marked as done, because it cannot have rung while the board did not
 * know the time (clock_engine.c). That is right after a boot without an RTC
 * and wrong 300 ms after an exec, where it would swallow an alarm due in the
 * minute the restart happened to land in. So a run with alarms that already
 * knows what time it is has something to say, even when nothing has rung. */
static int handoff_worth_writing(const struct clock_engine *e)
{
    int i;

    if (e->sw.state != CLOCK_SW_IDLE || e->sw.accumulated_ms != 0 ||
        e->sw.lap_count != 0) {
        return 1;
    }
    if (e->timer.state != CLOCK_TIMER_IDLE) {
        return 1;
    }
    if (e->ringing != CLOCK_RING_NONE) {
        return 1;
    }
    if (e->alarm_count > 0 && e->wall_was_valid) {
        return 1;
    }
    for (i = 0; i < e->alarm_count; i++) {
        if (e->alarms[i].snooze_until != 0) {
            return 1; /* a snooze is monotonic, and needs no wall clock */
        }
    }
    return 0;
}

int clock_handoff_save(const struct clock_engine *e, const struct clock_now *now)
{
    char path[STORE_PATH_MAX];
    char tmp[STORE_PATH_MAX + 8];
    FILE *f;
    int i;

    if (!e || !now || clock_handoff_path(path, sizeof(path)) != 0) {
        return -1;
    }
    if (!handoff_worth_writing(e)) {
        /* Nothing to say, and the last thing said must not be left standing:
         * a stopwatch that has since been reset would otherwise come back. */
        return clock_handoff_clear() == 0 ? 1 : -1;
    }
    if ((size_t)snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= sizeof(tmp)) {
        return -1;
    }
    make_dirs(clock_handoff_dir());
    f = fopen(tmp, "wb");
    if (!f) {
        return -1;
    }
    fprintf(f, "%s\n", CLOCK_HANDOFF_MAGIC);
    /* The reading this was written at, so the next start can tell that the
     * monotonic clock has not gone backwards in between. */
    fprintf(f, "mono %lld\n", (long long)now->mono_ms);
    fprintf(f, "wall %d\n", e->wall_was_valid ? 1 : 0);
    /* The alarm list this describes, by length: the handoff names alarms by
     * position, so a list that is not the one it was written against is a
     * handoff that cannot be applied. */
    fprintf(f, "alarms %d\n", e->alarm_count);
    for (i = 0; i < e->alarm_count; i++) {
        fprintf(f, "alarm %d %lld %lld\n", i,
                (long long)e->alarms[i].snooze_until,
                (long long)e->alarms[i].fired_day);
    }
    fprintf(f, "sw %u %lld %lld %d\n", (unsigned)e->sw.state,
            (long long)e->sw.started_mono, (long long)e->sw.accumulated_ms,
            e->sw.lap_count);
    /* One lap to a line: twenty of them on one would outrun the reader
     * buffer, and a line the reader has to truncate is a file it refuses. */
    for (i = 0; i < e->sw.lap_count && i < CLOCK_MAX_LAPS; i++) {
        fprintf(f, "lap %lld\n", (long long)e->sw.laps[i]);
    }
    fprintf(f, "timer %u %lld %lld %lld\n", (unsigned)e->timer.state,
            (long long)e->timer.duration_ms, (long long)e->timer.deadline_mono,
            (long long)e->timer.remaining_ms);
    fprintf(f, "ring %u %d\n", (unsigned)e->ringing, e->ringing_alarm);
    if (ferror(f) || fflush(f) != 0 || fsync(fileno(f)) != 0) {
        fclose(f);
        unlink(tmp);
        return -1;
    }
    fclose(f);
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

/* ---- taking the handoff ------------------------------------------------ */

/* One signed field. Same contract as parse_uint: a field that is not a whole
 * number is a broken file, not a zero. */
static int parse_i64(const char *s, const char **end, long long *out)
{
    long long v = 0;
    int digits = 0;
    int negative = 0;

    while (*s == ' ') {
        s++;
    }
    if (*s == '-') {
        negative = 1;
        s++;
    }
    while (*s >= '0' && *s <= '9') {
        if (v > 900000000000000LL) {
            return -1;
        }
        v = v * 10 + (*s - '0');
        s++;
        digits++;
    }
    if (digits == 0) {
        return -1;
    }
    *out = negative ? -v : v;
    *end = s;
    return 0;
}

/* Every field of a line, and no more: a line with something left on the end
 * is a line this reader does not understand. */
static int parse_fields(const char *p, long long *out, int count)
{
    int i;

    for (i = 0; i < count; i++) {
        if (parse_i64(p, &p, &out[i]) != 0) {
            return -1;
        }
    }
    while (*p == ' ') {
        p++;
    }
    return *p == '\0' ? 0 : -1;
}

/* What the reader has seen, so a file missing a line it needs - or carrying
 * one twice - is refused rather than half-applied. */
struct handoff_seen {
    int mono, wall, alarms, sw, timer, ring;
    int alarm_lines;
    int lap_lines;
};

/* Every state below is read as a signed number and kept as an unsigned one,
 * so each is checked at both ends of its range. A negative that was only
 * compared with the top would come back as 255 - a state no rule in
 * clock_engine.c has an answer for. */
static int handoff_apply_sw(struct clock_engine *w, const long long *v)
{
    if (v[0] < CLOCK_SW_IDLE || v[0] > CLOCK_SW_PAUSED || v[1] < 0 || v[2] < 0 ||
        v[3] < 0 || v[3] > CLOCK_MAX_LAPS) {
        return -1;
    }
    /* An idle stopwatch is a reset one: nothing started, nothing on the
     * clock, no laps. */
    if (v[0] == CLOCK_SW_IDLE && (v[1] != 0 || v[2] != 0 || v[3] != 0)) {
        return -1;
    }
    w->sw.state = (uint8_t)v[0];
    w->sw.started_mono = (int64_t)v[1];
    w->sw.accumulated_ms = (int64_t)v[2];
    w->sw.lap_count = (int)v[3];
    return 0;
}

static int handoff_apply_timer(struct clock_engine *w, const long long *v)
{
    if (v[0] < CLOCK_TIMER_IDLE || v[0] > CLOCK_TIMER_EXPIRED || v[1] < 0 ||
        v[1] > (long long)CLOCK_TIMER_MAX_SECONDS * 1000 || v[2] < 0 || v[3] < 0) {
        return -1;
    }
    /* A countdown that is doing anything at all has a duration, and never
     * more left on it than it was set for. */
    if (v[0] != CLOCK_TIMER_IDLE && v[1] == 0) {
        return -1;
    }
    if (v[3] > v[1]) {
        return -1;
    }
    if (v[0] == CLOCK_TIMER_EXPIRED && v[3] != 0) {
        return -1;
    }
    w->timer.state = (uint8_t)v[0];
    w->timer.duration_ms = (int64_t)v[1];
    w->timer.deadline_mono = (int64_t)v[2];
    w->timer.remaining_ms = (int64_t)v[3];
    return 0;
}

static int handoff_apply_ring(struct clock_engine *w, const long long *v)
{
    if (v[0] < CLOCK_RING_NONE || v[0] > CLOCK_RING_TIMER) {
        return -1;
    }
    if (v[0] == CLOCK_RING_ALARM) {
        if (v[1] < 0 || v[1] >= w->alarm_count) {
            return -1;
        }
    } else if (v[1] != -1) {
        return -1;
    }
    w->ringing = (uint8_t)v[0];
    w->ringing_alarm = (int)v[1];
    return 0;
}

static int handoff_read(FILE *f, struct clock_engine *w, const struct clock_now *now)
{
    char line[STORE_LINE_MAX];
    struct handoff_seen seen;
    long long v[4];
    int lines = 0;

    memset(&seen, 0, sizeof(seen));
    if (!fgets(line, sizeof(line), f)) {
        return -1;
    }
    line[strcspn(line, "\r\n")] = '\0';
    if (strcmp(line, CLOCK_HANDOFF_MAGIC) != 0) {
        return -1;
    }

    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);

        if (++lines > HANDOFF_MAX_LINES) {
            return -1;
        }
        if (len + 1 >= sizeof(line) && line[len - 1] != '\n') {
            return -1; /* truncated, so not the line that was written */
        }
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] == '\0') {
            continue;
        }
        if (strncmp(line, "mono ", 5) == 0) {
            if (seen.mono++ || parse_fields(line + 5, v, 1) != 0 || v[0] < 0) {
                return -1;
            }
            /* The boot-scoping argument, in one line. /run is a tmpfs and
             * starts empty, so this should be impossible; if it ever is not,
             * a monotonic clock that has gone backwards is a different boot,
             * and every instant in this file means nothing. */
            if (now->mono_ms < v[0]) {
                return -1;
            }
        } else if (strncmp(line, "wall ", 5) == 0) {
            if (seen.wall++ || parse_fields(line + 5, v, 1) != 0 || v[0] < 0 || v[0] > 1) {
                return -1;
            }
            w->wall_was_valid = v[0] != 0;
        } else if (strncmp(line, "alarms ", 7) == 0) {
            if (seen.alarms++ || parse_fields(line + 7, v, 1) != 0) {
                return -1;
            }
            /* The list has to be the one this was written against. It always
             * is - nothing can edit the alarms between the exit that wrote
             * this and the start that reads it - and saying so here is what
             * keeps a snooze on the alarm it belongs to. */
            if (v[0] != w->alarm_count) {
                return -1;
            }
        } else if (strncmp(line, "alarm ", 6) == 0) {
            if (!seen.alarms || parse_fields(line + 6, v, 3) != 0) {
                return -1;
            }
            if (v[0] != seen.alarm_lines || v[0] >= w->alarm_count) {
                return -1; /* in order, one each, and no more than there are */
            }
            if (v[1] < 0 || v[2] < CLOCK_DAY_NEVER) {
                return -1;
            }
            w->alarms[v[0]].snooze_until = (int64_t)v[1];
            w->alarms[v[0]].fired_day = (int64_t)v[2];
            seen.alarm_lines++;
        } else if (strncmp(line, "sw ", 3) == 0) {
            if (seen.sw++ || parse_fields(line + 3, v, 4) != 0 ||
                handoff_apply_sw(w, v) != 0) {
                return -1;
            }
        } else if (strncmp(line, "lap ", 4) == 0) {
            if (!seen.sw || seen.lap_lines >= w->sw.lap_count ||
                parse_fields(line + 4, v, 1) != 0 || v[0] < 0) {
                return -1;
            }
            w->sw.laps[seen.lap_lines++] = (int64_t)v[0];
        } else if (strncmp(line, "timer ", 6) == 0) {
            if (seen.timer++ || parse_fields(line + 6, v, 4) != 0 ||
                handoff_apply_timer(w, v) != 0) {
                return -1;
            }
        } else if (strncmp(line, "ring ", 5) == 0) {
            if (seen.ring++ || parse_fields(line + 5, v, 2) != 0 ||
                handoff_apply_ring(w, v) != 0) {
                return -1;
            }
        } else {
            return -1; /* an unknown keyword is a file we do not understand */
        }
    }
    if (ferror(f)) {
        return -1;
    }
    /* Every line this format has, exactly once, and every alarm and lap the
     * counts promised. A handoff missing a line is not a handoff. */
    if (seen.mono != 1 || seen.wall != 1 || seen.alarms != 1 || seen.sw != 1 ||
        seen.timer != 1 || seen.ring != 1) {
        return -1;
    }
    if (seen.alarm_lines != w->alarm_count || seen.lap_lines != w->sw.lap_count) {
        return -1;
    }
    /* Cross-field, once everything is in: a stopwatch cannot have started
     * after the reading this was written at. */
    if (w->sw.state == CLOCK_SW_RUNNING && w->sw.started_mono > now->mono_ms) {
        return -1;
    }
    return 0;
}

/* A countdown whose deadline went by while the process was being replaced is
 * over, and comes back over. Letting it back in RUNNING would put a deadline
 * in the past on screen for as long as it took the next step to notice. The
 * ringing that follows is the engine on its next step, exactly as it would
 * have been if nothing had restarted - and a snooze that came due in the same
 * gap needs nothing here for the same reason. */
static void handoff_settle(struct clock_engine *w, const struct clock_now *now)
{
    if (w->timer.state == CLOCK_TIMER_RUNNING && now->mono_ms >= w->timer.deadline_mono) {
        w->timer.state = CLOCK_TIMER_EXPIRED;
        w->timer.remaining_ms = 0;
    }
}

int clock_handoff_load(struct clock_engine *e, const struct clock_now *now)
{
    char path[STORE_PATH_MAX];
    struct clock_engine work;
    FILE *f;
    int rc;

    if (!e || !now || clock_handoff_path(path, sizeof(path)) != 0) {
        return -1;
    }
    f = fopen(path, "rb");
    if (!f) {
        return errno == ENOENT ? 1 : -1;
    }

    /* Built on a copy of the engine the settings file has already filled, so
     * a handoff that turns out to be damaged half way through cannot leave
     * the stopwatch running and the countdown missing. */
    work = *e;
    rc = handoff_read(f, &work, now);
    fclose(f);
    /* Consumed either way: one exit hands off to one start, and a handoff
     * that could not be read must not be refused again at every start for
     * the rest of the boot. */
    unlink(path);
    if (rc != 0) {
        return -1;
    }
    handoff_settle(&work, now);
    *e = work;
    return 0;
}
