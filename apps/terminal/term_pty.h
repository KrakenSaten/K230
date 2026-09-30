/*
 * Terminal: one shell on one pseudo-terminal, owned by the Doors shell.
 *
 * term_pty_start() opens a PTY pair, forks, and runs the program as the
 * leader of a new session with the PTY's slave as its controlling terminal:
 * job control, Ctrl+C and the line discipline are the kernel's, exactly as
 * over SSH. The master is non-blocking and close-on-exec, and never leaves
 * this process.
 *
 * Nothing here blocks except term_pty_close(), and that for a bounded time.
 * Reads return what is there; writes that the PTY will not take now wait in
 * a bounded queue (TERM_PTY_OUTQ) and the rest is counted and dropped.
 *
 * Lifecycle. The program's end is noticed by term_pty_poll() with a
 * non-reaping wait, so its pid - and with it the session id - stays reserved
 * while the rest of the session is cleared: everything still in the session
 * gets SIGHUP (and SIGCONT), and whatever is left after TERM_PTY_HUP_GRACE_MS
 * gets SIGKILL; only then is the leader reaped. term_pty_close() does the
 * same at once and waits for it within its bounds. So a session never
 * outlives its terminal, and the pid that names it cannot be reused while it
 * is being cleared. A process that left the session on purpose (setsid) is
 * not the terminal's and is left alone.
 *
 * If the Doors shell dies instead, the kernel closes the master: the program
 * gets SIGKILL (PR_SET_PDEATHSIG) and its foreground job SIGHUP.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef TERM_PTY_H
#define TERM_PTY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* Keys waiting for a program that does not read. A person cannot type this
 * much ahead; a paste could, and loses the excess. */
#define TERM_PTY_OUTQ 4096

/* How long the session has between SIGHUP and SIGKILL, once its program
 * ended or the terminal was closed. */
#define TERM_PTY_HUP_GRACE_MS 300
/* How long term_pty_close() then waits for the leader to be reapable. */
#define TERM_PTY_KILL_REAP_MS 500

enum term_pty_state {
    TERM_PTY_IDLE = 0, /* nothing started, or closed */
    TERM_PTY_RUNNING,  /* the program runs */
    TERM_PTY_ENDING,   /* it ended; the session is being cleared */
    TERM_PTY_ENDED,    /* all gone and reaped; the master may still hold output */
};

struct term_pty_spawn {
    const char *path; /* the program */
    bool login;       /* argv[0] "-name": a login shell reads /etc/profile */
    const char *home; /* HOME and the working directory; NULL: inherited, "/" */
    const char *term; /* TERM */
    int cols, rows;
};

struct term_pty {
    enum term_pty_state state;
    int fd;    /* the master, or -1 */
    pid_t pid; /* the program, the session's leader, or -1 */
    bool eof;  /* the master said no more */
    int exit_code;   /* how it ended: exit status, or -1 */
    int exit_signal; /* or the signal that killed it, else 0 */
    int64_t deadline_ms;
    uint8_t outq[TERM_PTY_OUTQ];
    size_t outq_len;
    unsigned long input_dropped;
    unsigned leftovers_killed; /* session members that needed SIGKILL */
};

void term_pty_init(struct term_pty *p);

/* Start the program. Returns 0, or -1 with a reason in err (the PTY could
 * not be had, or fork failed). A program that cannot be executed is not an
 * error here: it says so on the terminal and ends with status 127. */
int term_pty_start(struct term_pty *p, const struct term_pty_spawn *sp, char *err, size_t errlen);

/* Up to cap bytes of output. >0 bytes read, 0 nothing now, -1 no more will
 * come (the master is closed or reported the end). */
long term_pty_read(struct term_pty *p, uint8_t *buf, size_t cap);

/* Queue bytes for the program and write what the PTY takes now. Returns 0,
 * or -1 when nothing runs or some of it had to be dropped. */
int term_pty_send(struct term_pty *p, const uint8_t *data, size_t n);
/* Write what waits in the queue, as far as the PTY takes it. */
void term_pty_flush(struct term_pty *p);

/* Tell the program the terminal's size (it gets SIGWINCH). */
void term_pty_resize(struct term_pty *p, int cols, int rows);

/* Notice the program's end and clear its session, a step at a time. Never
 * blocks. */
void term_pty_poll(struct term_pty *p, int64_t now_ms);

/* Let the master go once everything has been read (ENDED only). */
void term_pty_release(struct term_pty *p);

/* End it now: close the master, SIGHUP the session, SIGKILL what remains
 * after TERM_PTY_HUP_GRACE_MS, reap the leader within TERM_PTY_KILL_REAP_MS.
 * Leaves the pty IDLE. Safe in any state. */
void term_pty_close(struct term_pty *p);

/* Live (not zombie) processes in session sid. For the lifecycle above and
 * for tests that want to see that nothing was left behind. */
int term_pty_session_count(pid_t sid);

int64_t term_pty_now_ms(void);

#endif
