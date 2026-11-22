#!/bin/bash
# Methods served by an app on shell.sock (ui/shell/app.h ipc_*, app_ipc.h,
# docs/api/shell.md): the hook itself, with the simulator's probe app
# (ui/shell/app_probe.c) as the one app that serves anything.
#
# Requires: SHELL_BIN (the CMake-built simulator, pocketos-shell), python3.
# Run from the repository root.
#
# Copyright (c) 2026 Doors authors.
# SPDX-License-Identifier: Apache-2.0
set -u
SHELL_BIN=${SHELL_BIN:?set SHELL_BIN to the pocketos-shell binary}
CALL="python3 tests/pocketipc_call.py"
export POCKETOS_RUNTIME_DIR POCKETOS_LOG_DIR POCKETOS_CONFIG_DIR SDL_VIDEODRIVER=dummy
POCKETOS_RUNTIME_DIR=$(mktemp -d)
POCKETOS_LOG_DIR=$(mktemp -d)
POCKETOS_CONFIG_DIR=$(mktemp -d)
OUT=$(mktemp -d)
SOCK="$POCKETOS_RUNTIME_DIR/shell.sock"
failed=0
SP=
LP=
# Whatever way this ends, no shell and no listener is left behind.
cleanup() {
    [ -n "$LP" ] && kill "$LP" 2>/dev/null
    [ -n "$SP" ] && kill "$SP" 2>/dev/null
    rm -rf "$POCKETOS_RUNTIME_DIR" "$POCKETOS_LOG_DIR" "$POCKETOS_CONFIG_DIR" "$OUT"
}
trap cleanup EXIT
check() { if printf '%s' "$3" | grep -q -- "$2"; then echo "ok   $1"; else echo "FAIL $1: expected '$2' in:"; printf '%s\n' "$3" | head -6 | cut -c1-300; failed=$((failed + 1)); fi; }
is() { if [ "$2" = "$3" ]; then echo "ok   $1"; else echo "FAIL $1: expected '$2', got '$3'"; failed=$((failed + 1)); fi; }
call() { $CALL "$SOCK" "$@" 2>&1; }
# One number of shell.info's app_ipc object, or of its top level.
stat() { call shell.info | sed 's/^OK //' | python3 -c "import json,sys; d=json.load(sys.stdin); print(d['app_ipc'].get('$1', d.get('$1')))"; }
current() { call shell.info | sed 's/^OK //' | python3 -c "import json,sys; print(json.load(sys.stdin)['current'])"; }
locked() { call shell.info | sed 's/^OK //' | python3 -c "import json,sys; print(json.load(sys.stdin)['lock']['locked'])"; }
start_shell() { # $1: POCKETOS_TEST_PROBE value, or empty for none
    rm -f "$SOCK"
    if [ -n "$1" ]; then POCKETOS_TEST_PROBE="$1" "$SHELL_BIN" >"$OUT/shell.log" 2>&1 & else "$SHELL_BIN" >"$OUT/shell.log" 2>&1 & fi
    SP=$!
    for _ in $(seq 1 80); do [ -S "$SOCK" ] && return 0; kill -0 $SP 2>/dev/null || return 1; sleep 0.1; done
    return 1
}
stop_shell() { [ -n "$SP" ] && kill $SP 2>/dev/null; wait $SP 2>/dev/null; SP=; }

# ---- 1. a shell with no app that serves anything: nothing changed ------------
start_shell "" || { echo "FAIL the shell did not start"; cat "$OUT/shell.log" | tail -5; exit 1; }
check "without the probe its names do not exist" '^ERR 1 unknown method' "$(call probe.status)"
check "nor does any other name outside shell." '^ERR 1 unknown method' "$(call nobody.status)"
check "an unknown shell method is still unknown" '^ERR 1 unknown method' "$(call shell.nothing)"
is "no handler was called" 0 "$(stat calls)"
is "the budget is reported" 2000 "$(stat budget_us)"
is "the limit is reported" 8192 "$(stat result_max)"
check "the probe is in no app list" '^0$' "$(call shell.info | grep -c '"id": *"probe"')"
stop_shell

# ---- 2. what the product's apps serve: PacketShark its status (H2); none opens itself
is "PacketShark and the probe declare a prefix, and no other app" "apps/packetshark/psk_app.c ui/shell/app_probe.c" \
   "$(grep -rlE '\.ipc_prefix[[:space:]]*=' apps ui --include='*.c' | sort | tr '\n' ' ' | sed 's/ $//')"
is "PacketShark (its start request, H4) and the probe declare an opening method, and no other app" "apps/packetshark/psk_app.c ui/shell/app_probe.c" \
   "$(grep -rlE '\.ipc_opens[[:space:]]*=' apps ui --include='*.c' | sort | tr '\n' ' ' | sed 's/ $//')"
is "the probe's methods are compiled only with the simulator's hooks" 1 \
   "$(sed -n '/^#if defined(POCKETOS_SHELL_TEST_HOOKS) && POCKETOS_SHELL_TEST_HOOKS$/,/^#else$/p' ui/shell/app_probe.c | grep -c 'static const struct pocketos_app app_probe')"
is "and nothing of the probe is outside that block" 0 \
   "$(sed -n '/^#else$/,$p' ui/shell/app_probe.c | grep -c -E 'getenv|ipc_prefix|probe_request')"
is "the card build is still compiled without the hooks" 1 "$(grep -c 'POCKETOS_SHELL_TEST_HOOKS' ui/shell/CMakeLists.txt)"

# ---- 3. the probe, closed ------------------------------------------------------
start_shell 1 || { echo "FAIL the shell did not start with the probe"; tail -5 "$OUT/shell.log"; exit 1; }
# A shell starts behind its lock, where no app is in front and none can be opened.
is "locked at start" True "$(locked)"
check "at start an opening method is refused" '^ERR 3 the unit is locked or showing an alarm$' "$(call probe.open)"
check "unlock, as a person at the unit" '"locked": false' "$(call shell.unlock)"
is "the probe is not in the app list either" 0 "$(call shell.info | grep -c '"id": *"probe"')"
is "home at start" home "$(current)"
check "a status method of a closed app says it is not there" '^OK {"available": false}$' "$(call probe.status)"
check "any other method of a closed app is an error, not an answer" '^ERR 3 Probe is not open' "$(call probe.echo '{"text": "hi"}')"
check "also one the app would not know" '^ERR 3 Probe is not open' "$(call probe.zzz)"
check "a name under no prefix is unknown" '^ERR 1 unknown method' "$(call probes.status)"
check "a shell name is the shell's" '^OK ' "$(call shell.info)"
is "nothing of that opened it" home "$(current)"
is "no handler ran for a closed app" 0 "$(stat calls)"
is "the status answer was counted" 1 "$(stat closed_status)"
is "the refusals were counted" 2 "$(stat closed_refused)"

# ---- 4. an opening method opens it; the others then reach it -----------------
check "an opening method opens the app and is answered by it" '^OK {"created": 1, "text": "knock"}$' "$(call probe.open '{"text": "knock"}')"
is "the probe is in front" probe "$(current)"
is "the opening was counted" 1 "$(stat opened)"
check "status, from the app now" '"available": true' "$(call probe.status)"
check "a method with parameters" '^OK {"created": 1, "text": "hi"}$' "$(call probe.echo '{"text": "hi"}')"
check "an opening method of an open app is just a method" '^OK {"created": 1, "text": ""}$' "$(call probe.open)"
is "and opens nothing twice" 1 "$(stat opened)"
check "the app's own refusal keeps its code and words" '^ERR 3 the probe says no$' "$(call probe.fail)"
check "a method the app does not know" '^ERR 1 unknown method$' "$(call probe.zzz)"
check "success without a result is an empty object" '^OK {}$' "$(call probe.nothing)"
check "an error code that is not pocketipc's is the app's fault" '^ERR 4 the app answered with an unknown error$' "$(call probe.wrong)"
check "the shell still answers its own" '"current": "probe"' "$(call shell.info)"

# ---- 5. the limit ------------------------------------------------------------------
check "a result under the limit passes" '^OK {"fill": "xxx' "$(call probe.big '{"bytes": 8000}')"
is "nothing was too large yet" 0 "$(stat too_large)"
check "a result over the limit is an error, not a truncation" '^ERR 4 the app.s answer is too large$' "$(call probe.big '{"bytes": 8200}')"
check "far over, the same" '^ERR 4 ' "$(call probe.big '{"bytes": 20000}')"
is "both were counted" 2 "$(stat too_large)"
check "and the shell goes on" '^OK {"created": 1, "text": "after"}$' "$(call probe.echo '{"text": "after"}')"

# ---- 6. time: measured and reported, never cut short ---------------------------
# Counted against what was there before: a loaded host may have made an
# earlier, honest handler slow, and that is not what is asked here.
slow0=$(stat slow)
check "a handler that takes too long is still answered" '^OK {"slept_ms": 30}$' "$(call probe.slow '{"ms": 30}')"
is "and counted" $((slow0 + 1)) "$(stat slow)"
is "and named" probe.slow "$(stat worst_method)"
check "with its time" '^1$' "$([ "$(stat worst_us)" -ge 30000 ] && echo 1 || echo 0)"
check "and logged" 'app method probe.slow took' "$(cat "$OUT/shell.log")"
check "the count of calls moved with it" '^1$' "$([ "$(stat calls)" -ge 1 ] && echo 1 || echo 0)"

# ---- 7. events: the app's own names only -------------------------------------------
rm -f "$OUT/ready"
$CALL "$SOCK" --listen 3 shell.subscribe "$OUT/ready" >"$OUT/events.txt" 2>&1 & LP=$!
for _ in $(seq 1 50); do [ -f "$OUT/ready" ] && break; sleep 0.1; done
check "an event method" '^OK {}$' "$(call probe.event)"
wait $LP
LP=
check "the app's event reaches a listener" '^EVENT probe.ping {"asked": ' "$(cat "$OUT/events.txt")"
is "exactly one event of the probe's" 1 "$(grep -c '^EVENT probe' "$OUT/events.txt")"
is "it could not raise the shell's" 0 "$(grep -c '^EVENT shell.app' "$OUT/events.txt")"
is "one raised" 1 "$(stat events)"
is "two dropped" 2 "$(stat events_dropped)"

# ---- 8. the lock: a locked unit has no app in front, and nothing opens it ------
check "lock" '"locked": true' "$(call shell.lock)"
check "status of an app under the lock" '^OK {"available": false}$' "$(call probe.status)"
check "a method of an app under the lock" '^ERR 3 Probe is not open' "$(call probe.echo)"
before=$(stat open_refused)
check "an opening method is refused while locked" '^ERR 3 the unit is locked or showing an alarm$' "$(call probe.open)"
is "the refusal was counted" $((before + 1)) "$(stat open_refused)"
is "and it did not open the lock" True "$(locked)"
ev0=$(stat events)
check "an app under the lock is not asked to raise anything" '^ERR 3 Probe is not open' "$(call probe.event)"
is "so no event left it" "$ev0" "$(stat events)"
check "unlock" '"locked": false' "$(call shell.unlock)"
check "the app is there again" '"available": true' "$(call probe.status)"

# ---- 9. closed again, and opened over another app ----------------------------------
check "home" '^OK ' "$(call shell.home)"
check "closed again" '^OK {"available": false}$' "$(call probe.status)"
check "another app" '"current": "notes"' "$(call shell.open '{"id": "notes"}')"
check "still closed with another app in front" '^ERR 3 Probe is not open' "$(call probe.echo)"
check "the opening method takes its place" '^OK {"created": 2, "text": "again"}$' "$(call probe.open '{"text": "again"}')"
is "the probe is in front again" probe "$(current)"
check "the shell is alive" '^1$' "$(kill -0 $SP 2>/dev/null && echo 1 || echo 0)"
stop_shell

# ---- 10. unsound declarations: the shell does not start ----------------------------
for bad in shell overlap norequest list both shape; do
    rm -f "$SOCK"
    POCKETOS_TEST_PROBE=$bad timeout 20 "$SHELL_BIN" >"$OUT/bad-$bad.log" 2>&1
    rc=$?
    is "declaration '$bad': the shell exits with 2" 2 "$rc"
    check "declaration '$bad': it says why" 'app methods: ' "$(cat "$OUT/bad-$bad.log")"
    is "declaration '$bad': no socket was opened" 0 "$([ -S "$SOCK" ] && echo 1 || echo 0)"
done
check "the shell's own names cannot be taken" 'is in the shell.s own names' "$(cat "$OUT/bad-shell.log")"
check "two apps cannot share names" 'overlap' "$(cat "$OUT/bad-overlap.log")"
check "a prefix needs a handler" 'without ipc_request' "$(cat "$OUT/bad-norequest.log")"
check "a listed method must be the app's own" 'is not under its prefix' "$(cat "$OUT/bad-list.log")"
check "a method is status or opening, not both" 'both a status and an opening method' "$(cat "$OUT/bad-both.log")"
check "a prefix has a shape" 'lower-case words ending in a dot' "$(cat "$OUT/bad-shape.log")"

echo "shell_app_methods_test: $failed failure(s)"
exit $((failed > 0))
