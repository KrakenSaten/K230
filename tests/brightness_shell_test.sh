#!/bin/bash
# Display brightness in the running shell: startup restore, shell.brightness
# over pocketipc, persistence, an invalid stored value, a failing device and a
# display with no control. The HAL itself is tests/brightness_test.c; this is
# the shell's side of it, against a fake sysfs tree the simulator build is
# allowed to read (POCKETOS_SHELL_TEST_HOOKS, SDL only).
#
# Requires: SHELL_BIN (CMake-built pocketos-shell, SDL) and `make all` (pos).
set -u
SHELL_BIN=${SHELL_BIN:?set SHELL_BIN to the pocketos-shell binary}
POS=${POS:-tools/pos/pos}
cd "$(dirname "$0")/.." || exit 1
failed=0
check() { if [ "$2" = "1" ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi; }
has() { printf '%s' "$1" | grep -q -- "$2" && echo 1 || echo 0; }

export SDL_VIDEODRIVER=dummy
TMP=$(mktemp -d)
SP=""
cleanup() { [ -n "$SP" ] && kill "$SP" 2>/dev/null; wait 2>/dev/null; rm -rf "$TMP"; }
trap cleanup EXIT

# <case name> : fresh runtime/config/log dirs, a sysfs root, and a running shell
start_shell() { # <sysfs root or "">
    export POCKETOS_RUNTIME_DIR="$TMP/run.$1" POCKETOS_LOG_DIR="$TMP/log.$1" POCKETOS_STATE_DIR="$TMP/state.$1"
    mkdir -p "$POCKETOS_RUNTIME_DIR" "$POCKETOS_LOG_DIR" "$POCKETOS_STATE_DIR"
    if [ -n "$2" ]; then export POCKETOS_TEST_SYSFS_ROOT="$2"; else export POCKETOS_TEST_SYSFS_ROOT="$TMP/nosysfs"; fi
    "$SHELL_BIN" >"$POCKETOS_LOG_DIR/out" 2>&1 & SP=$!
    for _ in $(seq 1 50); do [ -S "$POCKETOS_RUNTIME_DIR/shell.sock" ] && break; sleep 0.1; done
    [ -S "$POCKETOS_RUNTIME_DIR/shell.sock" ]
}
stop_shell() { kill "$SP" 2>/dev/null; wait "$SP" 2>/dev/null; SP=""; }
fake_panel() { # <root> <max> <level>
    mkdir -p "$1/class/backlight/rm69a10"
    printf '%s\n' "$2" > "$1/class/backlight/rm69a10/max_brightness"
    printf '%s\n' "$3" > "$1/class/backlight/rm69a10/brightness"
}
level() { tr -d '\n' < "$1/class/backlight/rm69a10/brightness"; }

# The hook exists only in the simulator: the shipped (drm) build is compiled
# without it, so nothing in the environment can redirect the real panel.
check "the sysfs test hook is compiled into SDL builds only" \
    "$(sed -n '/if(POCKETOS_DISPLAY STREQUAL "sdl")/,/^else()/p' ui/shell/CMakeLists.txt |
       grep -q 'POCKETOS_SHELL_TEST_HOOKS=1' &&
       [ "$(grep -c 'POCKETOS_SHELL_TEST_HOOKS' ui/shell/CMakeLists.txt)" = "1" ] && echo 1 || echo 0)"
check "and the shell reads the hook only under that define" \
    "$(sed -n '/^static const char \*sysfs_root/,/^}/p' ui/shell/shell.c |
       grep -q 'POCKETOS_SHELL_TEST_HOOKS' && echo 1 || echo 0)"

# ---- a panel with nothing stored: left exactly as booted ---------------------
export POCKETOS_CONFIG_DIR="$TMP/cfg.fresh"; mkdir -p "$POCKETOS_CONFIG_DIR"
fake_panel "$TMP/sys.fresh" 255 254
start_shell fresh "$TMP/sys.fresh"; check "shell starts with a fresh panel" "$([ $? = 0 ] && echo 1 || echo 0)"
check "nothing stored leaves the boot level untouched" "$([ "$(level "$TMP/sys.fresh")" = "254" ] && echo 1 || echo 0)"
out=$("$POS" shell brightness 2>&1)
check "pos shell brightness reads 100 % from raw 254" "$(has "$out" 'brightness 100% (rm69a10)')"
out=$("$POS" call shell shell.brightness 2>&1)
check "shell.brightness reports supported" "$(has "$out" '"supported":[[:space:]]*true')"
range="$(has "$out" '"min":[[:space:]]*10,')$(has "$out" '"max":[[:space:]]*100,')$(has "$out" '"step":[[:space:]]*10,')"
check "and the range 10..100 step 10" "$([ "$range" = "111" ] && echo 1 || echo 0)"

out=$("$POS" shell brightness 40 2>&1)
check "set 40 answers 40" "$(has "$out" 'brightness 40%')"
check "40 % reaches the panel as raw 102" "$([ "$(level "$TMP/sys.fresh")" = "102" ] && echo 1 || echo 0)"
check "and is persisted" "$(grep -qx 'display_brightness=40' "$POCKETOS_CONFIG_DIR/settings.conf" && echo 1 || echo 0)"

out=$("$POS" shell brightness 5 2>&1)
check "below the floor is refused over IPC" "$(has "$out" 'code 2')"
check "and changes nothing" "$([ "$(level "$TMP/sys.fresh")" = "102" ] && echo 1 || echo 0)"
out=$("$POS" shell brightness 0 2>&1)
check "0 is refused, the panel never goes dark by request" "$(has "$out" 'code 2')"
out=$("$POS" shell brightness 101 2>&1)
check "above 100 is refused" "$(has "$out" 'code 2')"
out=$("$POS" call shell shell.brightness percent=55.5 2>&1)
check "a fraction is refused" "$(has "$out" 'code 2')"
out=$("$POS" call shell shell.brightness percent=high 2>&1)
check "a word is refused" "$(has "$out" 'code 2')"
out=$("$POS" shell brightness abc 2>&1); rc=$?
check "the CLI refuses a non-number before calling" "$([ "$rc" = "2" ] && echo 1 || echo 0)"
check "stored value unchanged by every refusal" \
    "$(grep -qx 'display_brightness=40' "$POCKETOS_CONFIG_DIR/settings.conf" && echo 1 || echo 0)"
stop_shell

# ---- startup restore ----------------------------------------------------------
fake_panel "$TMP/sys.fresh" 255 254
start_shell restore "$TMP/sys.fresh"; check "shell restarts" "$([ $? = 0 ] && echo 1 || echo 0)"
for _ in $(seq 1 30); do [ "$(level "$TMP/sys.fresh")" = "102" ] && break; sleep 0.1; done
check "the stored 40 % is applied at start" "$([ "$(level "$TMP/sys.fresh")" = "102" ] && echo 1 || echo 0)"
check "the restore is logged" "$(grep -q 'brightness: rm69a10 restored to 40%' "$POCKETOS_LOG_DIR/shell.log" "$POCKETOS_LOG_DIR/out" 2>/dev/null && echo 1 || echo 0)"
stop_shell

# ---- an invalid stored value is ignored, logged and left alone ----------------
export POCKETOS_CONFIG_DIR="$TMP/cfg.bad"; mkdir -p "$POCKETOS_CONFIG_DIR"
printf 'display_brightness=3\n' > "$POCKETOS_CONFIG_DIR/settings.conf"
fake_panel "$TMP/sys.bad" 255 254
start_shell bad "$TMP/sys.bad"; check "shell starts with an invalid stored value" "$([ $? = 0 ] && echo 1 || echo 0)"
sleep 0.3
check "an out-of-range stored value is not applied" "$([ "$(level "$TMP/sys.bad")" = "254" ] && echo 1 || echo 0)"
check "it is logged as a warning" "$(grep -q 'stored display_brightness=3 is not 10..100' "$POCKETOS_LOG_DIR/shell.log" "$POCKETOS_LOG_DIR/out" 2>/dev/null && echo 1 || echo 0)"
check "and the stored value is left untouched" "$(grep -qx 'display_brightness=3' "$POCKETOS_CONFIG_DIR/settings.conf" && echo 1 || echo 0)"
stop_shell
printf 'display_brightness=seventy\n' > "$POCKETOS_CONFIG_DIR/settings.conf"
start_shell bad2 "$TMP/sys.bad"; sleep 0.3
check "a non-numeric stored value is not applied" "$([ "$(level "$TMP/sys.bad")" = "254" ] && echo 1 || echo 0)"
check "and the shell still answers" "$(has "$("$POS" shell brightness 2>&1)" 'brightness 100%')"
stop_shell

# ---- the device refusing the write ------------------------------------------
export POCKETOS_CONFIG_DIR="$TMP/cfg.fail"; mkdir -p "$POCKETOS_CONFIG_DIR"
mkdir -p "$TMP/sys.fail/class/backlight/rm69a10/brightness"
printf '255\n' > "$TMP/sys.fail/class/backlight/rm69a10/max_brightness"
start_shell fail "$TMP/sys.fail"; check "shell starts with a device that cannot be written" "$([ $? = 0 ] && echo 1 || echo 0)"
out=$("$POS" shell brightness 60 2>&1)
check "a failing write is reported as a backend error" "$(has "$out" 'code 4')"
check "and nothing is persisted" "$(grep -q 'display_brightness' "$POCKETOS_CONFIG_DIR/settings.conf" 2>/dev/null && echo 0 || echo 1)"
check "the shell survives it" "$(kill -0 "$SP" 2>/dev/null && echo 1 || echo 0)"
stop_shell

# ---- no brightness control at all (the simulator, the HDMI variant) ------------
export POCKETOS_CONFIG_DIR="$TMP/cfg.none"; mkdir -p "$POCKETOS_CONFIG_DIR"
printf 'display_brightness=50\n' > "$POCKETOS_CONFIG_DIR/settings.conf"
start_shell none ""; check "shell starts with no backlight device" "$([ $? = 0 ] && echo 1 || echo 0)"
out=$("$POS" shell brightness 2>&1)
check "pos reports unsupported" "$(has "$out" 'unsupported')"
out=$("$POS" call shell shell.brightness 2>&1)
check "shell.brightness says supported false" "$(has "$out" '"supported":[[:space:]]*false')"
check "with a null percent and device" "$(has "$out" '"percent":[[:space:]]*null')"
out=$("$POS" shell brightness 60 2>&1)
check "setting it is refused as unsupported" "$(has "$out" 'code 6')"
check "and the stored preference is kept for a panel that has one" \
    "$(grep -qx 'display_brightness=50' "$POCKETOS_CONFIG_DIR/settings.conf" && echo 1 || echo 0)"
check "no ERROR logged on a display without control" \
    "$(grep -q ' ERROR ' "$POCKETOS_LOG_DIR/shell.log" "$POCKETOS_LOG_DIR/out" 2>/dev/null && echo 0 || echo 1)"
stop_shell

echo "brightness_shell_test: $failed failure(s)"
exit $((failed > 0))
