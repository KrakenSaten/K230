#!/bin/bash
# Automatic rotation following the keyboard, in the running shell.
#
# The provider is the one thing that cannot run here - a PC has no keyboard
# base board - so the simulator's shell reads its presence from a file
# (POCKETOS_TEST_KEYBOARD_FILE, honoured only in the test-hook build) and this
# rewrites it the way mating and unmating the base rewrites the bus. Everything
# after that is the production path: the debounce in kbd_presence, the settle
# window and the restart in place in shell.c, and the policy in orientation.c.
#
# The restart is checked by pid: the shell re-executes itself, so the process
# that comes back landscape is the same process that went in portrait, which is
# what keeps pos-supervise from seeing an exit.
#
# Requires: SHELL_BIN (the CMake-built simulator) and pos (make all). Run from
# the repository root.
set -u
SHELL_BIN=${SHELL_BIN:?set SHELL_BIN to the pocketos-shell binary}
POS=${POS:-tools/pos/pos}
cd "$(dirname "$0")/.." || exit 1
export SDL_VIDEODRIVER=dummy
OUT=$(mktemp -d)
failed=0
check() { if [ "$2" = "1" ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi; }

KBD=$OUT/keyboard
say() { printf '%s\n' "$1" > "$KBD"; }          # what the "base board" is doing
export POCKETOS_TEST_KEYBOARD_FILE=$KBD

fresh() {
    export POCKETOS_RUNTIME_DIR=$(mktemp -d -p "$OUT") POCKETOS_LOG_DIR=$(mktemp -d -p "$OUT")
    export POCKETOS_CONFIG_DIR=$(mktemp -d -p "$OUT") POCKETOS_STATE_DIR=$(mktemp -d -p "$OUT")
}
info() { "$POS" shell info 2>/dev/null | tr -d ' \t\n' | grep -oE '"display":\{[^}]*\}'; }
orientation() { info | grep -oE '"orientation":"[a-z]+"' | cut -d'"' -f4; }
keyboard() { info | grep -oE '"keyboard":"[a-z]+"' | cut -d'"' -f4; }
shell_pid() { cat "$POCKETOS_RUNTIME_DIR/pid" 2>/dev/null; }

start_shell() { # [args]
    "$SHELL_BIN" "$@" >"$POCKETOS_LOG_DIR/run.log" 2>&1 &
    SP=$!
    echo "$SP" > "$POCKETOS_RUNTIME_DIR/pid"
    for _ in $(seq 1 50); do [ -S "$POCKETOS_RUNTIME_DIR/shell.sock" ] && break; sleep 0.1; done
    sleep 0.4
}
stop_shell() { kill "$SP" 2>/dev/null; wait "$SP" 2>/dev/null; }
# The watch ticks once a second, the debounce needs two agreeing ticks and the
# settle window is 800 ms, so a change is applied inside four seconds. Waiting
# for the orientation rather than a fixed sleep keeps the test honest about
# which one it got.
await() { # <orientation> <seconds>
    local want=$1 n=$(( $2 * 5 ))
    while [ "$n" -gt 0 ]; do
        [ "$(orientation)" = "$want" ] && return 0
        n=$((n - 1)); sleep 0.2
    done
    return 1
}
restarts() { grep -c 'restarting in place' "$POCKETOS_LOG_DIR/run.log" 2>/dev/null; true; }

# ---- 1. boot, with and without a keyboard ----------------------------------
fresh; say present; start_shell
check "boot with the keyboard attached: landscape at once, and it says so" \
    "$([ "$(orientation)" = landscape ] && [ "$(keyboard)" = present ] &&
       grep -q 'rotation mode automatic (default), keyboard present: rotation 270, 1232x568' \
            "$POCKETOS_LOG_DIR/run.log" && echo 1 || echo 0)"
check "and it did not have to restart to get there ($(restarts) restarts)" "$([ "$(restarts)" = 0 ] && echo 1 || echo 0)"
stop_shell

fresh; say absent; start_shell
check "boot with no keyboard: portrait" \
    "$([ "$(orientation)" = portrait ] && [ "$(keyboard)" = absent ] && echo 1 || echo 0)"
stop_shell

fresh; say nonsense; start_shell
check "boot with a provider that cannot tell: unknown, and portrait" \
    "$([ "$(orientation)" = portrait ] && [ "$(keyboard)" = unknown ] && echo 1 || echo 0)"
stop_shell

# ---- 2. attached and removed while Doors runs -------------------------------
fresh; say absent; start_shell
before=$(shell_pid)
say present
check "a keyboard attached in Automatic turns the display to landscape" "$(await landscape 8 && echo 1 || echo 0)"
check "by restarting the shell in place: same process, no exit for the supervisor" \
    "$([ "$(shell_pid)" = "$before" ] && kill -0 "$before" 2>/dev/null && echo 1 || echo 0)"
check "and the restart is logged once ($(restarts))" "$([ "$(restarts)" = 1 ] && echo 1 || echo 0)"
check "the new run says it opened landscape because of the keyboard" \
    "$(grep -cE 'rotation mode automatic \((default|stored)\), keyboard present: rotation 270, 1232x568' \
            "$POCKETOS_LOG_DIR/run.log" | grep -qx 1 && echo 1 || echo 0)"
check "touch follows the display, not the old orientation" \
    "$(info | grep -q '"width":1232,"height":568' && echo 1 || echo 0)"
sleep 3
check "and nothing restarts again while the keyboard stays attached ($(restarts))" \
    "$([ "$(restarts)" = 1 ] && [ "$(orientation)" = landscape ] && echo 1 || echo 0)"

say absent
check "removing it returns to portrait" "$(await portrait 8 && echo 1 || echo 0)"
check "still the same process ($(restarts) restarts in all)" \
    "$([ "$(shell_pid)" = "$before" ] && [ "$(restarts)" = 2 ] && echo 1 || echo 0)"
stop_shell

# ---- 3. a forced mode ignores the keyboard ----------------------------------
fresh; say absent
printf 'display_rotation=portrait\n' > "$POCKETOS_CONFIG_DIR/settings.conf"
start_shell
say present
sleep 5
check "forced Portrait with a keyboard attached: still portrait, no restart" \
    "$([ "$(orientation)" = portrait ] && [ "$(keyboard)" = present ] && [ "$(restarts)" = 0 ] && echo 1 || echo 0)"
check "and the shell knows the keyboard is there, it just does not act on it" \
    "$(grep -q 'keyboard present: rotation 0' "$POCKETOS_LOG_DIR/run.log" && echo 1 || echo 0)"
stop_shell

fresh; say present
printf 'display_rotation=landscape\n' > "$POCKETOS_CONFIG_DIR/settings.conf"
start_shell
check "forced Landscape starts landscape" "$([ "$(orientation)" = landscape ] && echo 1 || echo 0)"
say absent
sleep 5
check "removing the keyboard leaves forced Landscape alone" \
    "$([ "$(orientation)" = landscape ] && [ "$(keyboard)" = absent ] && [ "$(restarts)" = 0 ] && echo 1 || echo 0)"
stop_shell

# ---- 4. noise, and detection that fails -------------------------------------
fresh; say absent; start_shell
for _ in 1 2 3 4 5 6 7 8 9 10; do say present; sleep 0.2; say absent; sleep 0.2; done
check "a base board bouncing on its contacts never turns the display" \
    "$([ "$(orientation)" = portrait ] && [ "$(restarts)" = 0 ] && echo 1 || echo 0)"
say present
check "and once it settles, the display turns once" \
    "$(await landscape 8 && [ "$(restarts)" = 1 ] && echo 1 || echo 0)"
# The provider losing its source is not a keyboard being removed, but it
# resolves the same way in Automatic: portrait, which is what unknown means.
rm -f "$KBD"
check "detection failing publishes unknown and falls back to portrait" "$(await portrait 8 && echo 1 || echo 0)"
check "and says unknown rather than claiming the keyboard is gone" \
    "$([ "$(keyboard)" = unknown ] && echo 1 || echo 0)"
say present
check "recovery turns it back" "$(await landscape 8 && echo 1 || echo 0)"
check "each of those is one restart and no more ($(restarts))" "$([ "$(restarts)" = 3 ] && echo 1 || echo 0)"
stop_shell

# ---- 5. what a restart costs and keeps --------------------------------------
fresh; say absent; start_shell
"$POS" app start notes >/dev/null 2>&1; sleep 0.5
say present
check "with an app open, the display still turns" "$(await landscape 8 && echo 1 || echo 0)"
check "and Doors comes back on the launcher, as Settings says it will" \
    "$("$POS" app list 2>/dev/null | grep -q ' open$' && echo 0 || echo 1)"
check "the app was closed the ordinary way, not killed" \
    "$(grep -q 'close app notes' "$POCKETOS_LOG_DIR/shell.log" && echo 1 || echo 0)"
check "no ERROR anywhere in the run" \
    "$(grep -qE ' ERROR |assert' "$POCKETOS_LOG_DIR/run.log" "$POCKETOS_LOG_DIR/shell.log" && echo 0 || echo 1)"
stop_shell

# ---- 6. the mode, changed while a keyboard is attached ----------------------
fresh; say present; start_shell
check "Automatic with a keyboard is landscape" "$([ "$(orientation)" = landscape ] && echo 1 || echo 0)"
"$POS" call shell shell.rotation mode=portrait >/dev/null 2>&1
check "choosing Portrait overrides the keyboard and applies itself" "$(await portrait 8 && echo 1 || echo 0)"
"$POS" call shell shell.rotation mode=automatic >/dev/null 2>&1
check "choosing Automatic again gives the keyboard back its say" "$(await landscape 8 && echo 1 || echo 0)"
stop_shell

# ---- 7. the lock across keyboard-driven restarts ----------------------------
# The case the lock exists for: the device lies locked in a pocket and the
# base is mated or removed. Every restart must bring the lock back as it was.
# locked() is empty when the shell does not answer, which fails every check.
locked() { "$POS" shell info 2>/dev/null | tr -d ' \t\n' | grep -oE '"locked":(true|false)' | head -1 | cut -d: -f2; }
fresh; say absent; start_shell
before=$(shell_pid)
check "a cold start in Automatic is locked" "$([ "$(locked)" = true ] && echo 1 || echo 0)"
say present
check "locked, the base mated: the display turns" "$(await landscape 8 && echo 1 || echo 0)"
check "and the device is still locked, in the same process" \
    "$([ "$(locked)" = true ] && [ "$(shell_pid)" = "$before" ] && kill -0 "$before" 2>/dev/null && echo 1 || echo 0)"
say absent
check "locked, the base removed: portrait, still locked" "$(await portrait 8 && [ "$(locked)" = true ] && echo 1 || echo 0)"
say present
check "mated again: landscape, still locked, three restarts" \
    "$(await landscape 8 && [ "$(locked)" = true ] && [ "$(restarts)" = 3 ] && echo 1 || echo 0)"
"$POS" call shell shell.unlock >/dev/null 2>&1
check "unlocked by hand" "$([ "$(locked)" = false ] && echo 1 || echo 0)"
say absent
check "open, the base removed: portrait, still open" "$(await portrait 8 && [ "$(locked)" = false ] && echo 1 || echo 0)"
say present
check "open, mated again: landscape, still open, five restarts" \
    "$(await landscape 8 && [ "$(locked)" = false ] && [ "$(restarts)" = 5 ] && echo 1 || echo 0)"
stop_shell

rm -rf "$OUT"
echo "auto_rotation_shell_test: $failed failure(s)"
exit $((failed > 0))
