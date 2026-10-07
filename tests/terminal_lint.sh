#!/bin/bash
# Terminal (docs/apps/TERMINAL.md): the rules the code has to keep that a
# behavioural test cannot see at once.
#
# Copyright (c) 2026 PocketOS authors.
# SPDX-License-Identifier: Apache-2.0
set -u
cd "$(dirname "$0")/.." || exit 1
failed=0
check() { if [ "$2" = "1" ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi; }
T=apps/terminal

check "Terminal is in the shell's registry" \
    "$(grep -q '&app_terminal' ui/shell/shell.c && echo 1 || echo 0)"
check "a top-level launcher app, in ESSENTIALS and no folder (DS §47)" \
    "$(grep -qE '\{ "terminal", HOME_GROUP_ESSENTIALS, HOME_HUE_[A-Z]+, HOME_FOLDER_NONE \}' ui/shell/home_layout.c &&
       echo 1 || echo 0)"
check "its icon mask is compiled in" \
    "$(grep -q 'const lv_image_dsc_t pos_app_icon_terminal' ui/pocketui/pos_app_icons.c && echo 1 || echo 0)"
check "and its portal icon is in the manifest" \
    "$(grep -q 'ui/assets/doors/icon-terminal.bin' ui/assets/doors/MANIFEST.txt && [ -s ui/assets/doors/icon-terminal.bin ] &&
       echo 1 || echo 0)"
check "the shell builds every Terminal source" \
    "$(for f in terminal_app term_session term_screen term_keys term_pty; do
           grep -q "apps/terminal/$f.c" ui/shell/CMakeLists.txt || { echo 0; exit; }; done; echo 1)"

# One place starts processes, and it is the PTY's.
hits=$(grep -nE '\b(fork|execv[pe]*|execl[pe]*|posix_spawn[p]?|system|popen)\s*\(' $T/*.c | grep -v "^$T/term_pty.c:")
check "only term_pty.c starts a process" "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits"
check "the master is non-blocking and close-on-exec from the moment it is opened" \
    "$(grep -q 'open("/dev/ptmx", O_RDWR | O_NOCTTY | O_CLOEXEC | O_NONBLOCK)' $T/term_pty.c && echo 1 || echo 0)"
check "the program leads its own session with the PTY as its terminal" \
    "$(grep -q 'setsid()' $T/term_pty.c && grep -q 'TIOCSCTTY' $T/term_pty.c && echo 1 || echo 0)"
check "the program dies with the Doors shell" \
    "$(grep -q 'PR_SET_PDEATHSIG, SIGKILL' $T/term_pty.c && echo 1 || echo 0)"
check "every signal is back to its default in the program" \
    "$(grep -q 'for (sig = 1; sig < NSIG; sig++)' $T/term_pty.c && grep -q 'sigprocmask(SIG_SETMASK, &none' $T/term_pty.c &&
       echo 1 || echo 0)"
check "the end is seen without reaping, so the session id stays reserved" \
    "$(grep -q 'WEXITED | WNOHANG | WNOWAIT' $T/term_pty.c && echo 1 || echo 0)"

# Nothing on the LVGL thread's path waits, except the bounded close.
hits=$(grep -nE '\b(sleep|usleep|nanosleep|poll|select|pause)\s*\(|waitpid\([^)]*, 0\)' \
           $T/terminal_app.c $T/term_session.c $T/term_screen.c $T/term_keys.c)
check "the app, the session, the screen and the keys never wait" "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits"
check "the screen allocates only in its init" \
    "$([ "$(grep -cE '\b(malloc|calloc|realloc)\(' $T/term_screen.c)" = "1" ] && echo 1 || echo 0)"

# The raw key path is the Terminal's alone (pos_input.h).
hits=$(grep -rln 'pos_input_set_raw_target' apps ui | grep -vE '^(ui/pocketui/pos_input\.[ch]|apps/terminal/terminal_app\.c)$')
check "only the Terminal asks for raw keys" "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits"
check "the Terminal gives the raw key path back when it closes" \
    "$(grep -q 'pos_input_set_raw_target(NULL)' $T/terminal_app.c && echo 1 || echo 0)"

# The session outlives the screen (TERMINAL.md): its one timer is made where
# a session starts and nowhere else, and the Doors shell ends it before it
# exits or re-executes, so an exec never leaves its processes to nobody.
check "the session's timer is made only where a session starts" \
    "$([ "$(grep -c 'lv_timer_create(' $T/terminal_app.c)" = "1" ] &&
       awk '/^static void session_start\(/,/^}/' $T/terminal_app.c | grep -q 'lv_timer_create(' && echo 1 || echo 0)"
check "the Doors shell ends the session on its way out" \
    "$(grep -q '\.shutdown = terminal_shutdown' $T/terminal_app.c && grep -q 'apps\[i\]->shutdown()' ui/shell/shell.c &&
       echo 1 || echo 0)"

check "make test runs the Terminal suites and this lint" \
    "$(grep -q 'TERMINAL_TEST_RUN' Makefile && grep -q 'bash tests/terminal_lint.sh' Makefile && echo 1 || echo 0)"
check "the test binaries are ignored" \
    "$(for b in term_screen_test term_keys_test term_session_test; do
           grep -qx "/tests/$b" .gitignore || { echo 0; exit; }; done; echo 1)"

echo "terminal_lint: $failed failure(s)"
exit $((failed > 0))
