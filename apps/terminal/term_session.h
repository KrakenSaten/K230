/*
 * Terminal: a screen with a shell behind it.
 *
 * The part of the Terminal that is neither pixels nor processes: it moves
 * output from the PTY into the screen, the screen's reports and the typed
 * keys back into the PTY, and says when the shell has ended. The app calls
 * term_session_pump() from its timer and term_session_key() for each key;
 * both return at once.
 *
 * Output is taken in bounded slices: at most `budget` bytes per pump. What
 * the program writes beyond that waits in the kernel's PTY buffer, and when
 * that is full the program's own write blocks - the program is slowed to
 * the speed the screen can take, and no memory grows on this side. Keys are
 * written the moment they arrive, whatever the output is doing, so Ctrl+C
 * reaches a program that floods the screen.
 *
 * When the shell ends, the screen says how, and Enter starts a new one on
 * the same screen.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef TERM_SESSION_H
#define TERM_SESSION_H

#include "term_keys.h"
#include "term_pty.h"
#include "term_screen.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Output taken per pump, and per read. At the app's 30 ms tick that is up
 * to about half a megabyte a second: far more than can be read, and little
 * enough that one tick's parsing never holds the LVGL thread for long. */
#define TERM_PUMP_BUDGET 16384
#define TERM_READ_CHUNK 4096

/* What the Terminal tells the program it is. term_screen.h is the subset
 * it keeps to, and VT100 is the terminal type that asks for no more. */
#define TERM_TYPE "vt100"

enum term_session_phase {
    TERM_SESSION_OFF = 0, /* no screen */
    TERM_SESSION_RUNNING, /* a shell is on the PTY */
    TERM_SESSION_ENDED,   /* it ended, and the screen said so: Enter restarts */
};

struct term_session {
    enum term_session_phase phase;
    struct term_screen screen;
    struct term_pty pty;
    char path[256];
    char home[256];
    bool login;
    unsigned long bytes_in;  /* output taken from the program */
    unsigned long bytes_out; /* keys and reports sent to it */
    unsigned starts;
    char err[160];
};

/* Make the screen and start the shell. path is the program; login makes it
 * a login shell; home is its HOME and working directory (NULL: inherited).
 * Returns 0, or -1 when the screen's memory could not be had. A shell that
 * would not start is not an error: the screen says so and Enter retries. */
int term_session_open(struct term_session *s, const char *path, bool login, const char *home, int cols,
                      int rows);

/* Take up to budget bytes of output into the screen, send the screen's
 * reports, notice the shell's end. Returns the bytes taken. Never blocks. */
size_t term_session_pump(struct term_session *s, size_t budget, int64_t now_ms);

/* A key. While the shell runs it is sent at once; after it ended, Enter
 * starts a new shell and every other key is ignored. */
void term_session_key(struct term_session *s, const struct term_key *k);

/* The screen's new size, told to the program too. */
void term_session_resize(struct term_session *s, int cols, int rows);

/* End the shell and its session (term_pty_close's bounds) and free the
 * screen. Safe to call twice. */
void term_session_close(struct term_session *s);

#endif
