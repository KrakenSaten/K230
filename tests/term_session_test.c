/*
 * Terminal: a real shell on a real PTY (apps/terminal/term_session.c and
 * term_pty.c), driven the way the app drives it - keys in, bounded pumps,
 * the screen read back - with no LVGL.
 *
 * The shell is /bin/sh (TERM_TEST_SHELL overrides): dash on the build host,
 * BusyBox ash on the device, so the same binary cross-built runs there too.
 * Every check is about behaviour the user would see or the system would
 * keep: text the shell computed (not the echo of what was typed), the line
 * discipline acting on the keys, a running command interrupted, the shell's
 * end announced and a new one started, and after every close no process of
 * the session, no child and no descriptor left behind - including a job
 * that ignores SIGHUP, and a command still flooding the screen.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#define _GNU_SOURCE
#include "term_session.h"

#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define PROMPT "TPROMPT> "

static int failed;
static int checks;
static const char *shell;

static void check(const char *what, int ok)
{
    checks++;
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) {
        failed++;
    }
}

static void sleep_ms(int ms)
{
    struct timespec d = { ms / 1000, (long)(ms % 1000) * 1000000L };

    nanosleep(&d, NULL);
}

static int count_fds(void)
{
    DIR *d = opendir("/proc/self/fd");
    struct dirent *e;
    int n = 0;

    while (d && (e = readdir(d)) != NULL) {
        if (e->d_name[0] != '.') {
            n++;
        }
    }
    if (d) {
        closedir(d);
    }
    return n - 1; /* the directory's own */
}

static bool no_child(void)
{
    return waitpid(-1, NULL, WNOHANG) < 0 && errno == ECHILD;
}

/* A process of session sid whose name is comm, or -1. */
static pid_t find_in_session(pid_t sid, const char *comm)
{
    DIR *d = opendir("/proc");
    struct dirent *e;
    pid_t found = -1;

    while (d && found < 0 && (e = readdir(d)) != NULL) {
        char path[64];
        char buf[512];
        char name[64];
        int session;
        FILE *f;

        if (e->d_name[0] < '0' || e->d_name[0] > '9') {
            continue;
        }
        snprintf(path, sizeof(path), "/proc/%d/stat", atoi(e->d_name));
        f = fopen(path, "r");
        if (!f) {
            continue;
        }
        if (fgets(buf, sizeof(buf), f)) {
            char *open = strchr(buf, '(');
            char *close = strrchr(buf, ')');

            if (open && close && close > open && (size_t)(close - open - 1) < sizeof(name)) {
                memcpy(name, open + 1, (size_t)(close - open - 1));
                name[close - open - 1] = '\0';
                if (sscanf(close + 1, " %*c %*d %*d %d", &session) == 1 && session == sid &&
                    strcmp(name, comm) == 0) {
                    found = (pid_t)atoi(e->d_name);
                }
            }
        }
        fclose(f);
    }
    if (d) {
        closedir(d);
    }
    return found;
}

/* The bytes a process has written, by its own account. */
static unsigned long long wchar_of(pid_t pid)
{
    char path[64];
    char line[128];
    unsigned long long v = 0;
    FILE *f;

    snprintf(path, sizeof(path), "/proc/%d/io", (int)pid);
    f = fopen(path, "r");
    while (f && fgets(line, sizeof(line), f)) {
        if (sscanf(line, "wchar: %llu", &v) == 1) {
            break;
        }
    }
    if (f) {
        fclose(f);
    }
    return v;
}

/* The CPU time a process has used, in clock ticks (utime + stime). A
 * writer blocked on a full PTY uses none; an unblocked `yes` uses all it
 * can get. Unlike /proc/<pid>/io, every kernel has it. */
static unsigned long long cpu_of(pid_t pid)
{
    char path[64];
    char buf[512];
    unsigned long long ut = 0;
    unsigned long long st = 0;
    const char *close;
    FILE *f;

    snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
    f = fopen(path, "r");
    if (f && fgets(buf, sizeof(buf), f) && (close = strrchr(buf, ')')) != NULL) {
        /* after ")": state ppid pgrp session tty tpgid flags minflt cminflt
         * majflt cmajflt utime stime */
        sscanf(close + 1, " %*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu", &ut, &st);
    }
    if (f) {
        fclose(f);
    }
    return ut + st;
}

/* ---- reading the screen -------------------------------------------------- */

/* Every stored line, oldest first, through f; stops when f says so. */
static bool any_line(const struct term_session *s, bool (*f)(const char *line, const void *arg), const void *arg)
{
    char text[TERM_MAX_COLS * 3 + 1];
    int back = term_screen_scrollback(&s->screen);
    int k;
    int r;

    for (k = back; k >= 1; k--) {
        term_screen_row_text(&s->screen, k, 0, text, sizeof(text));
        if (f(text, arg)) {
            return true;
        }
    }
    for (r = 0; r < s->screen.rows; r++) {
        term_screen_row_text(&s->screen, 0, r, text, sizeof(text));
        if (f(text, arg)) {
            return true;
        }
    }
    return false;
}

static bool line_has(const char *line, const void *arg)
{
    return strstr(line, arg) != NULL;
}

/* A line that is the text, after any prompts: keys typed ahead are echoed
 * before the shell prints its next prompt, so a command's output can share
 * a row with the prompts, as on any terminal. */
static bool line_is(const char *line, const void *arg)
{
    static const char prompt[] = "TPROMPT> ";

    while (strncmp(line, prompt, sizeof(prompt) - 1) == 0) {
        line += sizeof(prompt) - 1;
    }
    return strcmp(line, arg) == 0;
}

static bool shows(const struct term_session *s, const char *text)
{
    return any_line(s, line_has, text);
}

static bool shows_line(const struct term_session *s, const char *text)
{
    return any_line(s, line_is, text);
}

static bool until(struct term_session *s, bool (*done)(const struct term_session *, const char *),
                  const char *text, int ms)
{
    int64_t end = term_pty_now_ms() + ms;

    while (term_pty_now_ms() < end) {
        term_session_pump(s, TERM_PUMP_BUDGET, term_pty_now_ms());
        if (done(s, text)) {
            return true;
        }
        sleep_ms(5);
    }
    if (done(s, text)) {
        return true;
    }
    {
        char line[TERM_MAX_COLS * 3 + 1];
        int r;

        printf("     waited for \"%s\"; phase %d, pty %d, the screen:\n", text ? text : "the end", s->phase,
               s->pty.state);
        for (r = 0; r < s->screen.rows; r++) {
            term_screen_row_text(&s->screen, 0, r, line, sizeof(line));
            if (line[0]) {
                printf("     |%s|\n", line);
            }
        }
    }
    return false;
}

static bool wait_for(struct term_session *s, const char *text, int ms)
{
    return until(s, shows, text, ms);
}

static bool wait_line(struct term_session *s, const char *text, int ms)
{
    return until(s, shows_line, text, ms);
}

static bool ended(const struct term_session *s, const char *unused)
{
    (void)unused;
    return s->phase == TERM_SESSION_ENDED;
}

static void pump_ms(struct term_session *s, int ms)
{
    int64_t end = term_pty_now_ms() + ms;

    while (term_pty_now_ms() < end) {
        term_session_pump(s, TERM_PUMP_BUDGET, term_pty_now_ms());
        sleep_ms(5);
    }
}

/* ---- typing -------------------------------------------------------------- */

static void key(struct term_session *s, enum term_key_kind kind, uint32_t cp, unsigned mods)
{
    struct term_key k = { kind, cp, mods };

    term_session_key(s, &k);
}

static void type(struct term_session *s, const char *text)
{
    const unsigned char *p = (const unsigned char *)text;

    while (*p) {
        uint32_t c = *p++;

        if (c == '\r') {
            key(s, TERM_KEY_ENTER, 0, 0);
            continue;
        }
        if (c >= 0xC0) { /* two-byte UTF-8 is all the tests type */
            c = ((c & 0x1F) << 6) | (*p++ & 0x3F);
        }
        key(s, TERM_KEY_CHAR, c, 0);
    }
}

static void ctrl(struct term_session *s, char c)
{
    key(s, TERM_KEY_CHAR, (uint32_t)c, TERM_MOD_CTRL);
}

/* A fresh session at the prompt, or a failed check and false. */
static bool open_at_prompt(struct term_session *s, int cols, int rows)
{
    if (term_session_open(s, shell, false, NULL, cols, rows) != 0) {
        check("the screen could be allocated", 0);
        return false;
    }
    if (!wait_for(s, "TPROMPT>", 5000)) {
        char text[TERM_MAX_COLS * 3 + 1];
        int r;

        check("the shell prints its prompt", 0);
        printf("     phase %d, pty state %d, %lu bytes in\n", s->phase, s->pty.state, s->bytes_in);
        for (r = 0; r < s->screen.rows; r++) {
            term_screen_row_text(&s->screen, 0, r, text, sizeof(text));
            if (text[0]) {
                printf("     |%s|\n", text);
            }
        }
        term_session_close(s);
        return false;
    }
    return true;
}

/* ---- the cases ----------------------------------------------------------- */

static void test_basics(void)
{
    struct term_session s;
    pid_t sid;

    if (!open_at_prompt(&s, 80, 24)) {
        return;
    }
    check("a shell starts on the PTY and prints its prompt", s.phase == TERM_SESSION_RUNNING);
    sid = s.pty.pid;
    check("it leads a session of its own", getsid(sid) == sid && getsid(0) != sid);

    type(&s, "echo out-$((6*7))\r");
    check("a command runs, and what it printed appears", wait_line(&s, "out-42", 3000));

    type(&s, "echo abX");
    key(&s, TERM_KEY_BACKSPACE, 0, 0);
    type(&s, "c\r");
    check("Backspace erases the character before it", wait_line(&s, "abc", 3000));

    /* Read by od, not by the shell, so a line editor that completes on Tab
     * (BusyBox ash does) cannot get between the key and the bytes. */
    type(&s, "od -c\r");
    pump_ms(&s, 200);
    key(&s, TERM_KEY_UP, 0, 0);
    key(&s, TERM_KEY_ESC, 0, 0);
    key(&s, TERM_KEY_TAB, 0, 0);
    key(&s, TERM_KEY_ENTER, 0, 0);
    ctrl(&s, 'd');
    check("an arrow, Escape and Tab reach the program as ESC sequences and HT",
          wait_for(&s, "033   [   A 033  \\t  \\n", 3000));

    type(&s, "echo \"[$TERM]\" \"[${SSH_CLIENT}]\"\r");
    check("the program is told TERM=vt100 and sees no SSH session", wait_line(&s, "[vt100] []", 3000));

    type(&s, "stty size\r");
    check("the program knows the terminal's size", wait_line(&s, "24 80", 3000));
    term_session_resize(&s, 100, 30);
    type(&s, "stty size\r");
    check("and its new size after a resize", wait_line(&s, "30 100", 3000));

    type(&s, "echo \xc3\xa6\xc3\xb8\xc3\xa5\r");
    check("UTF-8 goes through the PTY both ways", wait_line(&s, "\xc3\xa6\xc3\xb8\xc3\xa5", 3000));

    type(&s, "printf '\\033[1;31mRED\\033[0m\\n'\r");
    check("the program's colours reach the screen", wait_line(&s, "RED", 3000));
    {
        int r;
        bool red = false;

        for (r = 0; r < s.screen.rows; r++) {
            const struct term_cell *l = term_screen_view_row(&s.screen, 0, r);

            if (l && l[0].ch == 'R' && l[1].ch == 'E' && (l[0].fg & TERM_COLOR_MASK) == 1 &&
                (l[0].fg & TERM_ATTR_BOLD)) {
                red = true;
            }
        }
        check("as bold red cells", red);
    }

    type(&s, "sleep 20\r");
    pump_ms(&s, 300);
    {
        int64_t t0 = term_pty_now_ms();

        ctrl(&s, 'c');
        type(&s, "echo rc=$?\r");
        check("Ctrl+C interrupts a running command", wait_line(&s, "rc=130", 3000));
        check("at once", term_pty_now_ms() - t0 < 2000);
    }

    ctrl(&s, 'd');
    check("Ctrl+D at an empty prompt ends the shell", until(&s, ended, NULL, 3000));
    check("the screen says it ended", wait_for(&s, "Shell exited. Press Enter to start a new one.", 1000));
    check("the shell was reaped", no_child());
    check("nothing of its session is left", term_pty_session_count(sid) == 0);
    check("the PTY was let go", s.pty.fd < 0);
    type(&s, "x");
    check("a key other than Enter does not restart it", s.phase == TERM_SESSION_ENDED && s.starts == 1);
    key(&s, TERM_KEY_ENTER, 0, 0);
    check("Enter starts a new shell", s.phase == TERM_SESSION_RUNNING && s.starts == 2);
    type(&s, "echo again\r");
    check("which works", wait_line(&s, "again", 3000));
    term_session_close(&s);
    check("closing leaves no child", no_child());
}

static void test_endings(void)
{
    struct term_session s;
    pid_t sid;

    if (!open_at_prompt(&s, 80, 24)) {
        return;
    }
    type(&s, "exit 3\r");
    check("a shell that exits with a status", until(&s, ended, NULL, 3000));
    check("says which", wait_for(&s, "Shell exited with status 3.", 1000));
    term_session_close(&s);

    if (!open_at_prompt(&s, 80, 24)) {
        return;
    }
    type(&s, "kill -9 $$\r");
    check("a shell that is killed", until(&s, ended, NULL, 3000));
    check("says by what", wait_for(&s, "Shell ended by signal 9.", 1000));
    check("and was reaped", no_child());
    term_session_close(&s);

    /* A background job, and one that ignores SIGHUP: the shell's end is
     * their end too, the second by SIGKILL after the grace. */
    if (!open_at_prompt(&s, 80, 24)) {
        return;
    }
    sid = s.pty.pid;
    type(&s, "sleep 1000 &\r");
    type(&s, "(trap '' HUP; exec sleep 1001) &\r");
    type(&s, "echo started\r");
    wait_line(&s, "started", 3000);
    check("the jobs run in the terminal's session", term_pty_session_count(sid) == 3);
    type(&s, "exit\r");
    check("the shell exits with jobs still running", until(&s, ended, NULL, 3000));
    check("no job of its session outlives it", term_pty_session_count(sid) == 0);
    check("the one that ignored SIGHUP needed SIGKILL", s.pty.leftovers_killed >= 1);
    term_session_close(&s);
    check("and nothing is left to reap", no_child());

    /* A program that is not there. */
    if (term_session_open(&s, "/nonexistent/shell", false, NULL, 80, 24) == 0) {
        check("a shell that cannot run is reported on the screen", wait_for(&s, "cannot run /nonexistent/shell", 3000));
        check("and ends as such", until(&s, ended, NULL, 3000) && wait_for(&s, "status 127", 1000));
        term_session_close(&s);
    }
}

static void test_close_while_busy(void)
{
    struct term_session s;
    int64_t t0;
    pid_t sid;
    int64_t took;

    if (!open_at_prompt(&s, 80, 24)) {
        return;
    }
    sid = s.pty.pid;
    type(&s, "yes flood\r");
    pump_ms(&s, 300);
    check("a flood is running", s.bytes_in > 10000);
    t0 = term_pty_now_ms();
    term_session_close(&s);
    took = term_pty_now_ms() - t0;
    printf("note closing during a flood took %lld ms\n", (long long)took);
    check("closing during a flood returns within its bound",
          took <= TERM_PTY_HUP_GRACE_MS + TERM_PTY_KILL_REAP_MS + 100);
    check("the flood and the shell are gone", term_pty_session_count(sid) == 0);
    check("reaped", no_child());

    /* A job that ignores SIGHUP, and a stopped one, when the terminal is
     * closed under a running shell. */
    if (!open_at_prompt(&s, 80, 24)) {
        return;
    }
    sid = s.pty.pid;
    type(&s, "(trap '' HUP; exec sleep 1002) &\r");
    type(&s, "sleep 1003\r");
    pump_ms(&s, 200);
    ctrl(&s, 'z');
    type(&s, "echo stopped\r");
    wait_line(&s, "stopped", 3000);
    term_session_close(&s);
    check("closing ends a job that ignores SIGHUP and a stopped one", term_pty_session_count(sid) == 0);
    check("reaped", no_child());
}

static void test_flood(void)
{
    struct term_session s;
    size_t most = 0;
    int64_t end;
    long polls = 0;

    if (!open_at_prompt(&s, 80, 24)) {
        return;
    }
    /* 50,000 lines as fast as the program can write them. */
    type(&s, "seq 1 50000; echo seq-done\r");
    end = term_pty_now_ms() + 30000;
    while (term_pty_now_ms() < end && !shows_line(&s, "seq-done")) {
        size_t n = term_session_pump(&s, TERM_PUMP_BUDGET, term_pty_now_ms());

        most = n > most ? n : most;
        polls++;
    }
    check("50,000 lines all arrive", shows_line(&s, "seq-done") && shows_line(&s, "50000"));
    check("no pump ever took more than its budget", most <= TERM_PUMP_BUDGET);
    printf("note %lu bytes in %ld pumps, the largest %zu\n", s.bytes_in, polls, most);
    check("every byte seq wrote was read (ONLCR adds a CR per line)", s.bytes_in >= 288894 + 50000);
    check("the scrollback stayed at its bound", term_screen_scrollback(&s.screen) == TERM_SCROLLBACK);
    type(&s, "echo still-here\r");
    check("the shell answers after the flood", wait_line(&s, "still-here", 3000));

    /* Not reading: the program is held by the kernel's buffer, and nothing
     * accumulates here. What `yes` wrote while nobody read is its own
     * count of bytes written (/proc/<pid>/io), which an unblocked yes
     * raises by hundreds of megabytes a second. */
    type(&s, "yes held\r");
    pump_ms(&s, 200);
    {
        pid_t yes = find_in_session(s.pty.pid, "yes");
        long hz = sysconf(_SC_CLK_TCK);
        unsigned long long w0;
        unsigned long long w1;
        unsigned long long c0;
        unsigned long long c1;

        check("the flooding program is found", yes > 0);
        pump_ms(&s, 50);
        w0 = wchar_of(yes);
        c0 = cpu_of(yes);
        sleep_ms(1000);
        w1 = wchar_of(yes);
        c1 = cpu_of(yes);
        printf("note unread for 1 s, yes used %llu ticks of CPU (of %ld) and wrote %llu bytes%s\n", c1 - c0, hz,
               w1 - w0, w0 ? "" : " (no I/O accounting on this kernel)");
        /* Blocked: well under a tenth of the second on the CPU, and when
         * the kernel counts it, under a megabyte written. */
        check("a program that out-writes the screen is held back by the PTY",
              yes > 0 && (c1 - c0) * 10 < (unsigned long long)hz && (w0 == 0 || w1 - w0 < 1024u * 1024u));
    }
    /* And Ctrl+C still gets through the flood. */
    pump_ms(&s, 100);
    ctrl(&s, 'c');
    type(&s, "echo after-flood\r");
    check("Ctrl+C stops a flood", wait_line(&s, "after-flood", 5000));
    term_session_close(&s);
}

static void test_churn(void)
{
    struct term_session s;
    int fds0 = count_fds();
    int i;
    int good = 0;

    for (i = 0; i < 25; i++) {
        if (term_session_open(&s, shell, false, NULL, 80, 24) != 0) {
            break;
        }
        if (i % 2 == 0 && wait_for(&s, "TPROMPT>", 5000)) {
            type(&s, "sleep 100 &\r");
            pump_ms(&s, 20);
        }
        term_session_close(&s);
        good++;
    }
    check("25 opens and closes, some at once, some with a job running", good == 25);
    check("leave no descriptor behind", count_fds() == fds0);
    check("and no child", no_child());
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    shell = getenv("TERM_TEST_SHELL");
    if (!shell || !*shell) {
        shell = "/bin/sh";
    }
    /* The environment the shell inherits: a known prompt, no rc file, and
     * an SSH variable the terminal must not pass on. */
    setenv("PS1", PROMPT, 1);
    unsetenv("ENV");
    setenv("SSH_CLIENT", "10.0.0.1 22 22", 1);
    signal(SIGPIPE, SIG_IGN);

    test_basics();
    test_endings();
    test_close_while_busy();
    test_flood();
    test_churn();
    check("no child is left at the end", no_child());
    printf("term_session_test: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
