#!/usr/bin/env bash
# What companion.* in sysd may and may not do, held in its source (ADR-019
# amendment 2, services/sysd/sysd_companion.h):
#
#   1. It runs programs only through the ops' run(), with an argv vector -
#      never system(), popen() or a shell - and only these: S70gatewayd start
#      and stop, sshd -t -f, S50sshd reload.
#   2. Its paths are compiled in. Only the test-hooks object reads a root from
#      the environment, and the shipped sysd does not contain that name.
#   3. The password reaches no log: no LOG_*, printf or dprintf line names it.
#      sysd logs the method of a request and never its parameters.
#   4. gatewayd never names companion.*: no phone reaches the switch or the
#      password through the gateway's allowlist.
#
# Copyright (c) 2026 Doors authors.
# SPDX-License-Identifier: Apache-2.0
set -u
cd "$(dirname "$0")/.."

failed=0
checks=0
ok()   { checks=$((checks + 1)); echo "ok   $*"; }
fail() { checks=$((checks + 1)); failed=$((failed + 1)); echo "FAIL $*"; }

SRC=services/sysd/sysd_companion.c
MAIN=services/sysd/main.c

# ---- 1. what it runs ----------------------------------------------------------
if grep -nE '\b(system|popen|execl|execlp|execvp|execve|posix_spawn)[[:space:]]*\(' "$SRC" >/dev/null; then
    fail "$SRC starts a program other than through run(): $(grep -nE '\b(system|popen|execl|execlp|execvp|execve|posix_spawn)[[:space:]]*\(' "$SRC" | head -3)"
else
    ok "no system(), popen() or exec in $SRC: programs run through the ops' run() only"
fi
if grep -n '/bin/sh' "$SRC" >/dev/null; then
    fail "$SRC names a shell"
else
    ok "no shell in $SRC"
fi
argvs=$(grep -oE 'argv\[\] = \{[^}]*\}|stop\[\] = \{[^}]*\}' "$SRC" | sed 's/[[:space:]]\+/ /g' | sort -u)
allowed=$(printf '%s\n' \
    'argv[] = { c->paths.init, "start", NULL }' \
    'argv[] = { c->paths.init, "stop", NULL }' \
    'argv[] = { p->sshd, "-t", "-f", tmp, NULL }' \
    'argv[] = { p->sshd_init, "reload", NULL }' \
    'stop[] = { c->paths.init, "stop", NULL }' | sort -u)
if [ "$argvs" = "$allowed" ]; then
    ok "the only commands: S70gatewayd start|stop, sshd -t -f, S50sshd reload"
else
    fail "the commands in $SRC are not the allowed four:"
    diff <(echo "$allowed") <(echo "$argvs") | sed 's/^/     /'
fi

# ---- 2. paths -------------------------------------------------------------------
if [ "$(grep -c 'getenv' "$SRC")" -gt 0 ] && ! awk '/getenv/ && !inhook {bad=1} /^#ifdef SYSD_TEST_HOOKS/ {inhook=1} /^#endif/ {inhook=0} END {exit bad}' "$SRC"; then
    fail "$SRC reads the environment outside SYSD_TEST_HOOKS"
else
    ok "the environment is read only under SYSD_TEST_HOOKS"
fi
if [ -x services/sysd/sysd ]; then
    if strings services/sysd/sysd | grep -q SYSD_TEST_COMPANION_ROOT; then
        fail "the shipped sysd contains SYSD_TEST_COMPANION_ROOT"
    else
        ok "the shipped sysd does not contain SYSD_TEST_COMPANION_ROOT"
    fi
else
    echo "     (services/sysd/sysd not built: its strings are not checked)"
fi

# ---- 3. the password in no log -----------------------------------------------------
# The words inside string literals do not count, the identifiers outside do.
named=$(for f in "$SRC" "$MAIN"; do sed -E 's/"([^"\\]|\\.)*"/""/g' "$f" | grep -nE '(LOG_[A-Z]+|printf|say|puts)[[:space:]]*\(.*\b(password|pw|v->valuestring)\b' | grep -vE '\bpassword_[a-z]' | sed "s|^|$f:|"; done)
if [ -n "$named" ]; then
    fail "a log or print line names the password: $(echo "$named" | head -3)"
else
    ok "no log or print line in sysd names the password"
fi
if grep -nE 'LOG_[A-Z]+\(.*params' "$MAIN" >/dev/null; then
    fail "sysd logs request parameters"
else
    ok "sysd logs no request's parameters"
fi
if grep -c 'explicit_bzero(password' "$SRC" | grep -qE '^[4-9]|^[1-9][0-9]'; then
    ok "the password is wiped on every path of companion.set_password and the job"
else
    fail "too few explicit_bzero(password...) in $SRC"
fi

# ---- 4. the gateway does not reach it ----------------------------------------------
if grep -rn '"companion\.' services/gatewayd >/dev/null; then
    fail "gatewayd names a companion.* method: $(grep -rn '"companion\.' services/gatewayd | head -2)"
else
    ok "gatewayd names no companion.* method"
fi

echo "sysd companion lint: ${checks} checks, ${failed} failed"
[ "$failed" -eq 0 ]
