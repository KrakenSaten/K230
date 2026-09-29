/*
 * The scripted vision provider. See db_vision_mock.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "db_vision_mock.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define TOKEN_MAX 48
/* Far enough for any demo, short enough that at_ms + gaps never overflows. */
#define STEP_MS_MAX (24LL * 3600 * 1000)

static int parse_step(const char *tok, struct db_mock_step *out)
{
    char buf[TOKEN_MAX];
    char *colon;
    char *at;
    char *end;
    long long ms;
    long conf = DB_CONF_NONE;

    if (strlen(tok) >= sizeof(buf)) {
        return -1;
    }
    strcpy(buf, tok);
    colon = strchr(buf, ':');
    if (!colon || colon == buf) {
        return -1;
    }
    *colon = '\0';
    errno = 0;
    ms = strtoll(buf, &end, 10);
    if (errno || *end || ms < 0 || ms > STEP_MS_MAX) {
        return -1;
    }
    at = strchr(colon + 1, '@');
    if (at) {
        *at = '\0';
        errno = 0;
        conf = strtol(at + 1, &end, 10);
        if (errno || end == at + 1 || *end || conf < 0 || conf > 1000) {
            return -1;
        }
    }
    if (db_vision_kind_parse(colon + 1, &out->kind) != 0) {
        return -1;
    }
    out->at_ms = ms;
    out->confidence_pm = (int16_t)conf;
    return 0;
}

int db_vision_mock_load(struct db_vision_mock *m, const char *script)
{
    struct db_vision_mock next;
    const char *p = script;

    if (!m) {
        return -1;
    }
    memset(&next, 0, sizeof(next));
    memset(m, 0, sizeof(*m));
    if (!script) {
        return -1;
    }
    while (*p) {
        char tok[TOKEN_MAX];
        size_t n;

        p += strspn(p, ", \t\n");
        n = strcspn(p, ", \t\n");
        if (n == 0) {
            break;
        }
        if (n >= sizeof(tok)) {
            return -1;
        }
        memcpy(tok, p, n);
        tok[n] = '\0';
        p += n;
        if (strcmp(tok, "loop") == 0) {
            next.loop = true;
            continue;
        }
        if (next.loop || next.n == DB_MOCK_MAX_STEPS || parse_step(tok, &next.step[next.n]) != 0 ||
            (next.n > 0 && next.step[next.n].at_ms < next.step[next.n - 1].at_ms)) {
            return -1; /* "loop" must be last; steps never go back in time */
        }
        next.n++;
    }
    if (next.n == 0) {
        return -1;
    }
    *m = next;
    return m->n;
}

static void emit(enum db_vision_kind kind, int16_t conf, int64_t at, struct db_vision_queue *q)
{
    struct db_vision_event ev = { .kind = kind,
                                  .face = kind == DB_VISION_OWNER_RECOGNIZED || kind == DB_VISION_UNKNOWN_PERSON,
                                  .confidence_pm = conf,
                                  .mono_ms = at };

    db_vision_queue_push(q, &ev);
}

void db_vision_mock_inject(struct db_vision_mock *m, enum db_vision_kind kind, int64_t now_ms,
                           struct db_vision_queue *q)
{
    (void)m;
    emit(kind, DB_CONF_NONE, now_ms, q);
}

static int mock_start(void *ctx, int64_t now_ms, struct db_vision_queue *q)
{
    struct db_vision_mock *m = ctx;

    (void)q;
    if (!m) {
        return -1;
    }
    m->t0 = now_ms;
    m->next = 0;
    m->running = true;
    return 0;
}

static int64_t mock_poll(void *ctx, int64_t now_ms, struct db_vision_queue *q)
{
    struct db_vision_mock *m = ctx;
    int guard = 0;

    if (!m || !m->running || m->n == 0) {
        return -1;
    }
    /* Every step that is due, in order, but never more than a queue's worth
     * in one poll: a clock that jumped far ahead must not spin here. */
    while (guard++ < DB_VISION_QUEUE_CAP) {
        if (m->next == m->n) {
            if (!m->loop) {
                return -1;
            }
            m->t0 += m->step[m->n - 1].at_ms + DB_MOCK_LOOP_GAP_MS;
            m->next = 0;
        }
        if (m->t0 + m->step[m->next].at_ms > now_ms) {
            break;
        }
        emit(m->step[m->next].kind, m->step[m->next].confidence_pm, m->t0 + m->step[m->next].at_ms, q);
        m->next++;
    }
    return m->next < m->n ? m->t0 + m->step[m->next].at_ms
                          : (m->loop ? m->t0 + m->step[m->n - 1].at_ms + DB_MOCK_LOOP_GAP_MS : -1);
}

static void mock_stop(void *ctx)
{
    struct db_vision_mock *m = ctx;

    if (m) {
        m->running = false;
    }
}

const struct db_vision_provider_ops db_vision_mock_ops = {
    .name = "mock",
    .start = mock_start,
    .poll = mock_poll,
    .stop = mock_stop,
};
