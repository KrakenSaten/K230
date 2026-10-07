/*
 * Terminal: a screen with a shell behind it. See term_session.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "term_session.h"

#include <stdio.h>
#include <string.h>

/* The screen's own voice, in reverse video so it cannot be mistaken for
 * the shell's output: a line of its own, then the pen as a program would
 * find it on a new terminal. */
static void say(struct term_session *s, const char *text)
{
    static const char before[] = "\r\n\033[0m\033[7m";
    static const char after[] = "\033[0m\r\n";

    term_screen_feed(&s->screen, (const uint8_t *)before, sizeof(before) - 1);
    term_screen_feed(&s->screen, (const uint8_t *)text, strlen(text));
    term_screen_feed(&s->screen, (const uint8_t *)after, sizeof(after) - 1);
}

/* Whatever the last program left set - a scroll region, application cursor
 * keys, no autowrap, a hidden cursor, the line-drawing set - is the new
 * shell's to set again, not to inherit. The text stays where it is. */
static void soft_reset(struct term_session *s)
{
    static const char seq[] = "\033[0m\033[?25h\033[?1l\033[?7h\033[4l\033(B\033)B\017";
    int cx = s->screen.cx;
    int cy = s->screen.cy;

    term_screen_feed(&s->screen, (const uint8_t *)seq, sizeof(seq) - 1);
    s->screen.top = 0;
    s->screen.bottom = s->screen.rows - 1;
    s->screen.cx = cx;
    s->screen.cy = cy;
}

static void start(struct term_session *s)
{
    struct term_pty_spawn sp;
    char line[sizeof(s->err) + 48];

    memset(&sp, 0, sizeof(sp));
    sp.path = s->path;
    sp.login = s->login;
    sp.home = s->home[0] ? s->home : NULL;
    sp.term = TERM_TYPE;
    sp.cols = s->screen.cols;
    sp.rows = s->screen.rows;
    s->starts++;
    if (term_pty_start(&s->pty, &sp, s->err, sizeof(s->err)) != 0) {
        snprintf(line, sizeof(line), "Terminal: %s. Press Enter to try again.", s->err);
        say(s, line);
        s->phase = TERM_SESSION_ENDED;
        return;
    }
    s->err[0] = '\0';
    s->phase = TERM_SESSION_RUNNING;
}

int term_session_open(struct term_session *s, const char *path, bool login, const char *home, int cols,
                      int rows)
{
    memset(s, 0, sizeof(*s));
    term_pty_init(&s->pty);
    if (term_screen_init(&s->screen, cols, rows) != 0) {
        s->phase = TERM_SESSION_OFF;
        return -1;
    }
    snprintf(s->path, sizeof(s->path), "%s", path ? path : "/bin/sh");
    snprintf(s->home, sizeof(s->home), "%s", home ? home : "");
    s->login = login;
    start(s);
    return 0;
}

static void send(struct term_session *s, const uint8_t *data, size_t n)
{
    if (n && term_pty_send(&s->pty, data, n) == 0) {
        s->bytes_out += n;
    }
}

static void announce_end(struct term_session *s)
{
    char line[96];

    if (s->pty.exit_signal > 0) {
        snprintf(line, sizeof(line), "Shell ended by signal %d. Press Enter to start a new one.",
                 s->pty.exit_signal);
    } else if (s->pty.exit_code > 0) {
        snprintf(line, sizeof(line), "Shell exited with status %d. Press Enter to start a new one.",
                 s->pty.exit_code);
    } else {
        snprintf(line, sizeof(line), "Shell exited. Press Enter to start a new one.");
    }
    soft_reset(s);
    say(s, line);
    s->phase = TERM_SESSION_ENDED;
}

size_t term_session_pump(struct term_session *s, size_t budget, int64_t now_ms)
{
    uint8_t buf[TERM_READ_CHUNK];
    uint8_t rep[TERM_REPLY_MAX];
    size_t taken = 0;
    long n = 0;
    size_t r;

    if (s->phase == TERM_SESSION_OFF || s->pty.state == TERM_PTY_IDLE) {
        return 0;
    }
    while (taken < budget) {
        size_t want = budget - taken < sizeof(buf) ? budget - taken : sizeof(buf);

        n = term_pty_read(&s->pty, buf, want);
        if (n <= 0) {
            break;
        }
        term_screen_feed(&s->screen, buf, (size_t)n);
        taken += (size_t)n;
    }
    s->bytes_in += taken;
    r = term_screen_take_reply(&s->screen, rep, sizeof(rep));
    if (r && s->pty.state == TERM_PTY_RUNNING) {
        send(s, rep, r);
    }
    term_pty_poll(&s->pty, now_ms);
    /* The end is announced once everything the session wrote has been
     * read: after the reaping, a read that finds nothing (or the end) means
     * there is nothing more to come. */
    if (s->pty.state == TERM_PTY_ENDED && s->phase == TERM_SESSION_RUNNING && taken < budget && n <= 0) {
        n = term_pty_read(&s->pty, buf, sizeof(buf));
        if (n > 0) {
            term_screen_feed(&s->screen, buf, (size_t)n);
            s->bytes_in += (size_t)n;
            taken += (size_t)n;
        } else {
            term_pty_release(&s->pty);
            announce_end(s);
        }
    }
    return taken;
}

void term_session_key(struct term_session *s, const struct term_key *k)
{
    uint8_t out[TERM_KEY_MAX];
    size_t n;

    if (!k || s->phase == TERM_SESSION_OFF) {
        return;
    }
    if (s->phase == TERM_SESSION_ENDED) {
        if (k->kind == TERM_KEY_ENTER) {
            term_pty_close(&s->pty);
            soft_reset(s);
            start(s);
        }
        return;
    }
    n = term_key_encode(k, s->screen.app_cursor_keys, out);
    send(s, out, n);
}

void term_session_resize(struct term_session *s, int cols, int rows)
{
    if (s->phase == TERM_SESSION_OFF) {
        return;
    }
    term_screen_resize(&s->screen, cols, rows);
    term_pty_resize(&s->pty, s->screen.cols, s->screen.rows);
}

void term_session_close(struct term_session *s)
{
    term_pty_close(&s->pty);
    if (s->phase != TERM_SESSION_OFF) {
        term_screen_free(&s->screen);
    }
    s->phase = TERM_SESSION_OFF;
}
