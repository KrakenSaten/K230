#!/bin/bash
# An app that runs in portrait only (app.h `orientation`; Timber), in the
# running shell: opened in landscape the display turns to portrait before the
# app is created, nothing turns it while the app is open, and every way out
# gives the mode back - by the shell's one restart in place, never by touching
# the stored mode.
#
# Every turn is checked by pid (the shell re-executes itself, so the process
# that comes back is the one that went) and counted from the log, so a loop
# shows up as a count that keeps climbing.
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
say() { printf '%s\n' "$1" > "$KBD"; }
export POCKETOS_TEST_KEYBOARD_FILE=$KBD

fresh() { # [stored settings line]
    export POCKETOS_RUNTIME_DIR=$(mktemp -d -p "$OUT") POCKETOS_LOG_DIR=$(mktemp -d -p "$OUT")
    export POCKETOS_CONFIG_DIR=$(mktemp -d -p "$OUT") POCKETOS_STATE_DIR=$(mktemp -d -p "$OUT")
    [ -n "${1:-}" ] && printf '%s\n' "$1" > "$POCKETOS_CONFIG_DIR/settings.conf"
    true
}
stored() { grep -E '^display_rotation=' "$POCKETOS_CONFIG_DIR/settings.conf" 2>/dev/null | cut -d= -f2; }
info() { "$POS" shell info 2>/dev/null | tr -d ' \t\n'; }
orientation() { info | grep -oE '"display":\{[^}]*\}' | grep -oE '"orientation":"[a-z]+"' | cut -d'"' -f4; }
current() { info | grep -oE '"current":"[^"]*"' | head -1 | cut -d'"' -f4; }
folder() { "$POS" shell info 2>/dev/null | python3 -c "import json,sys; print(json.load(sys.stdin)['launcher']['folder'] or '')" 2>/dev/null; }
held() { info | grep -oE '"portrait_app":("[a-z0-9]*"|null)' | cut -d: -f2 | tr -d '"'; }
call() { "$POS" call shell "$@" >/dev/null 2>&1; }
start_shell() { # [args]
    "$SHELL_BIN" --no-lock "$@" >"$POCKETOS_LOG_DIR/run.log" 2>&1 &
    SP=$!
    for _ in $(seq 1 50); do [ -S "$POCKETOS_RUNTIME_DIR/shell.sock" ] && break; sleep 0.1; done
    sleep 0.4
}
stop_shell() { kill "$SP" 2>/dev/null; wait "$SP" 2>/dev/null; }
# A turn is a restart: the socket goes and comes back. Wait for the
# orientation and the open app rather than for a fixed time.
await() { # <orientation> <current> <seconds>
    local n=$(( $3 * 5 ))
    while [ "$n" -gt 0 ]; do
        [ "$(orientation)" = "$1" ] && [ "$(current)" = "$2" ] && return 0
        n=$((n - 1)); sleep 0.2
    done
    return 1
}
restarts() { grep -c 'restarting in place' "$POCKETOS_LOG_DIR/run.log" 2>/dev/null; true; }
alive() { kill -0 "$SP" 2>/dev/null; }
no_fault() { ! grep -qE ' ERROR |assert' "$POCKETOS_LOG_DIR/run.log" "$POCKETOS_LOG_DIR/shell.log" 2>/dev/null; }

# ---- 1. the declaration ------------------------------------------------------
check "Timber declares portrait only, and it is the only app that does" \
    "$([ "$(grep -rl 'POCKETOS_APP_ORIENTATION_PORTRAIT' apps | sort | tr '\n' ' ')" = "apps/timber/timber_app.c " ] &&
       echo 1 || echo 0)"
check "no app writes the rotation mode or rotates the display itself" \
    "$(grep -rqE 'pocketos_shell_set_rotation_mode|lv_display_set_rotation|orientation_hold' apps/timber && echo 0 || echo 1)"

# ---- 2. opened in landscape, left by every way out ---------------------------
say absent
fresh display_rotation=landscape
start_shell
pid=$SP
check "forced Landscape starts landscape, on the launcher" \
    "$([ "$(orientation)" = landscape ] && [ "$(current)" = home ] && [ "$(restarts)" = 0 ] && echo 1 || echo 0)"

"$POS" app start timber >/dev/null 2>&1
check "opening Timber turns the display to portrait, with Timber open" "$(await portrait timber 8 && echo 1 || echo 0)"
check "by one restart in place: same process ($(restarts) restart)" \
    "$([ "$(restarts)" = 1 ] && alive && echo 1 || echo 0)"
check "Timber was created once, in the portrait run, never sideways" \
    "$([ "$(grep -c 'open app timber$' "$POCKETOS_LOG_DIR/shell.log")" = 1 ] &&
       grep -q 'open app timber again after the restart' "$POCKETOS_LOG_DIR/shell.log" &&
       grep -q 'rotation mode landscape (stored), keyboard absent: rotation 0, 568x1232' "$POCKETOS_LOG_DIR/run.log" &&
       echo 1 || echo 0)"
check "the shell says Timber holds it, and the stored mode is still landscape" \
    "$([ "$(held)" = timber ] && [ "$(stored)" = landscape ] && echo 1 || echo 0)"

# Nothing turns it while Timber is open: not the mode, not the keyboard.
call shell.rotation mode=automatic
say present
sleep 4
check "inside Timber neither Automatic with a keyboard nor the keyboard turns it ($(restarts))" \
    "$([ "$(orientation)" = portrait ] && [ "$(current)" = timber ] && [ "$(restarts)" = 1 ] && echo 1 || echo 0)"
call shell.rotation mode=landscape
sleep 2
check "nor choosing Landscape: stored, and waiting ($(restarts))" \
    "$([ "$(orientation)" = portrait ] && [ "$(restarts)" = 1 ] && [ "$(stored)" = landscape ] && echo 1 || echo 0)"
say absent

"$POS" app home >/dev/null 2>&1
check "Home: back to landscape, on the launcher" "$(await landscape home 8 && echo 1 || echo 0)"
check "one more restart, same process, and the hold is gone ($(restarts))" \
    "$([ "$(restarts)" = 2 ] && alive && [ "$(held)" = null ] && [ "$(stored)" = landscape ] && echo 1 || echo 0)"

"$POS" app start timber >/dev/null 2>&1
check "reopened: portrait again" "$(await portrait timber 8 && [ "$(restarts)" = 3 ] && echo 1 || echo 0)"
call shell.action action=back
check "Back: landscape again" "$(await landscape home 8 && [ "$(restarts)" = 4 ] && echo 1 || echo 0)"

"$POS" app start timber >/dev/null 2>&1
await portrait timber 8 >/dev/null
# The header's back slab: the screen's top-left corner in portrait (DS §48).
call shell.tap x=40 y=40
check "the back slab: landscape again ($(restarts))" \
    "$(await landscape home 8 && [ "$(restarts)" = 6 ] && echo 1 || echo 0)"

"$POS" app start timber >/dev/null 2>&1
await portrait timber 8 >/dev/null
call shell.action action=home
check "the Home action: landscape again ($(restarts))" \
    "$(await landscape home 8 && [ "$(restarts)" = 8 ] && echo 1 || echo 0)"

"$POS" app start timber >/dev/null 2>&1
await portrait timber 8 >/dev/null
"$POS" app start notes >/dev/null 2>&1
check "straight from Timber to another app: that app, in landscape ($(restarts))" \
    "$(await landscape notes 8 && [ "$(restarts)" = 10 ] && echo 1 || echo 0)"
"$POS" app home >/dev/null 2>&1; sleep 0.3

# Opened from its folder, it comes back to the folder.
call shell.folder id=games; sleep 0.3
"$POS" app start timber >/dev/null 2>&1
await portrait timber 8 >/dev/null
"$POS" app home >/dev/null 2>&1
check "opened from GAMES and left: landscape, in GAMES ($(restarts))" \
    "$(await landscape home 8 && [ "$(folder)" = games ] && [ "$(restarts)" = 12 ] && echo 1 || echo 0)"

sleep 3
check "nothing restarts on its own afterwards: no loop ($(restarts))" \
    "$([ "$(restarts)" = 12 ] && [ "$(orientation)" = landscape ] && alive && echo 1 || echo 0)"
check "the stored mode was never touched by any of it" "$([ "$(stored)" = landscape ] && echo 1 || echo 0)"
check "the app was closed the ordinary way every time" \
    "$([ "$(grep -c 'close app timber' "$POCKETOS_LOG_DIR/shell.log")" = 6 ] && echo 1 || echo 0)"
check "no ERROR anywhere in the run" "$(no_fault && echo 1 || echo 0)"
stop_shell

# ---- 3. the mode changed while Timber held the display -----------------------
fresh display_rotation=landscape
start_shell
"$POS" app start timber >/dev/null 2>&1
await portrait timber 8 >/dev/null
call shell.rotation mode=portrait
sleep 1
"$POS" app home >/dev/null 2>&1
sleep 2
check "Portrait chosen inside Timber: leaving it stays portrait, no further restart ($(restarts))" \
    "$([ "$(orientation)" = portrait ] && [ "$(current)" = home ] && [ "$(restarts)" = 1 ] &&
       [ "$(stored)" = portrait ] && echo 1 || echo 0)"
stop_shell

# ---- 4. already portrait: nothing to turn ------------------------------------
fresh display_rotation=portrait
start_shell
"$POS" app start timber >/dev/null 2>&1; sleep 0.5
check "in Portrait, Timber opens at once, no restart" \
    "$([ "$(current)" = timber ] && [ "$(orientation)" = portrait ] && [ "$(restarts)" = 0 ] && echo 1 || echo 0)"
"$POS" app home >/dev/null 2>&1; sleep 1
check "and leaves without one" "$([ "$(current)" = home ] && [ "$(restarts)" = 0 ] && echo 1 || echo 0)"
stop_shell

# ---- 5. the lock does not close Timber or turn the display -------------------
fresh display_rotation=landscape
start_shell
"$POS" app start timber >/dev/null 2>&1
await portrait timber 8 >/dev/null
call shell.lock; sleep 1
check "locked with Timber open: still portrait, still open, no restart" \
    "$([ "$(orientation)" = portrait ] && [ "$(current)" = timber ] && [ "$(restarts)" = 1 ] && echo 1 || echo 0)"
call shell.unlock; sleep 0.3
"$POS" app home >/dev/null 2>&1
check "unlocked and left: landscape" "$(await landscape home 8 && [ "$(restarts)" = 2 ] && echo 1 || echo 0)"
stop_shell

# ---- 6. --open: once, at a cold start ----------------------------------------
fresh display_rotation=landscape
start_shell --open timber
check "--open timber in Landscape starts portrait, without a restart" \
    "$([ "$(orientation)" = portrait ] && [ "$(current)" = timber ] && [ "$(restarts)" = 0 ] &&
       grep -q 'display: timber runs in portrait only; the landscape mode' "$POCKETOS_LOG_DIR/run.log" && echo 1 || echo 0)"
"$POS" app home >/dev/null 2>&1
check "left: landscape, and --open does not open it again" "$(await landscape home 8 && echo 1 || echo 0)"
sleep 3
check "no loop ($(restarts) restart)" \
    "$([ "$(restarts)" = 1 ] && [ "$(current)" = home ] && alive && echo 1 || echo 0)"
check "no ERROR in the --open run" "$(no_fault && echo 1 || echo 0)"
stop_shell

# ---- 7. a run given --rotation goes back to that mode, not the stored one ----
fresh
start_shell --rotation landscape
"$POS" app start timber >/dev/null 2>&1
check "--rotation landscape (nothing stored): Timber turns it to portrait" "$(await portrait timber 8 && echo 1 || echo 0)"
"$POS" app home >/dev/null 2>&1
check "and leaving it comes back to the run's landscape ($(restarts) restarts)" \
    "$(await landscape home 8 && [ "$(restarts)" = 2 ] && [ -z "$(stored)" ] && echo 1 || echo 0)"
stop_shell

# ---- 8. the bench override wins over the hold --------------------------------
fresh display_rotation=landscape
POCKETOS_DRM_ROTATION=270 start_shell
"$POS" app start timber >/dev/null 2>&1; sleep 1
check "POCKETOS_DRM_ROTATION=270: Timber opens as the bench says, no restart" \
    "$([ "$(current)" = timber ] && [ "$(orientation)" = landscape ] && [ "$(restarts)" = 0 ] && echo 1 || echo 0)"
stop_shell

rm -rf "$OUT"
echo "app_orientation_shell_test: $failed failure(s)"
exit $((failed > 0))
