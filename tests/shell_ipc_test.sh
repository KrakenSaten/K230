#!/bin/bash
# Headless shell test: drive the SDL simulator (dummy video driver) through
# shell.* with pos, and check screenshots and app switching.
# Requires: SHELL_BIN (CMake-built pocketos-shell), radiod and pos built.
set -u
SHELL_BIN=${SHELL_BIN:?set SHELL_BIN to the pocketos-shell binary}
RADIOD=${RADIOD:-services/radiod/radiod}
POS=${POS:-tools/pos/pos}
export POCKETOS_RUNTIME_DIR POCKETOS_LOG_DIR POCKETOS_CONFIG_DIR SDL_VIDEODRIVER=dummy
POCKETOS_RUNTIME_DIR=$(mktemp -d)
POCKETOS_LOG_DIR=$(mktemp -d)
POCKETOS_CONFIG_DIR=$(mktemp -d)
OUT=$(mktemp -d)
# seed a stored selection: it must be applied before the first frame
printf 'theme=olive
display_mode=night
' > "$POCKETOS_CONFIG_DIR/settings.conf"
failed=0
check_empty() { if [ -z "$2" ]; then echo "ok   $1"; else echo "FAIL $1: unexpected output:"; printf '%s\n' "$2" | head -10; failed=$((failed + 1)); fi; }
check() { if printf '%s' "$3" | grep -q -- "$2"; then echo "ok   $1"; else echo "FAIL $1: expected '$2' in:"; printf '%s\n' "$3" | head -10; failed=$((failed + 1)); fi; }

"$RADIOD" --backend mock >/dev/null 2>&1 & RP=$!
"$SHELL_BIN" >"$OUT/shell.log" 2>&1 & SP=$!
for _ in $(seq 1 50); do [ -S "$POCKETOS_RUNTIME_DIR/shell.sock" ] && break; sleep 0.1; done
[ -S "$POCKETOS_RUNTIME_DIR/shell.sock" ] || { echo "FAIL shell socket missing"; cat "$OUT/shell.log"; kill $RP $SP; exit 1; }

out=$("$POS" app list)
check "app list has radio" '^radio ' "$out"
check "app list has system" '^system ' "$out"
check "app list has fleet" '^fleet ' "$out"
out=$("$POS" shell info)
check "shell info current home" '"current":[[:space:]]*"home"' "$out"
check "stored theme applied at start" '"theme":[[:space:]]*"olive"' "$out"
check "stored mode applied at start" '"mode":[[:space:]]*"night"' "$out"
check "shell info display" '"width":[[:space:]]*568' "$out"

out=$("$POS" app start radio 2>&1); check_empty "app start radio" "$out"
sleep 0.5
out=$("$POS" app list); check "radio marked open" '^radio .*open' "$out"
out=$("$POS" shell screenshot "$OUT/radio.png"); check "screenshot saved" 'saved' "$out"
check "screenshot is a PNG" 'PNG' "$(head -c 8 "$OUT/radio.png" | tr -d '\0')"

# Theme engine (DS v0.1 §8): live switch, info reporting, fallback
out=$("$POS" shell theme brass outdoor 2>&1); check "theme switch brass/outdoor" 'theme brass mode outdoor' "$out"
out=$("$POS" shell info); check "info reports theme" '"theme":[[:space:]]*"brass"' "$out"; check "info reports mode" '"mode":[[:space:]]*"outdoor"' "$out"
"$POS" shell screenshot "$OUT/radio-brass-outdoor.png" >/dev/null
check "themed screenshot differs from ice" '1' "$(cmp -s "$OUT/radio.png" "$OUT/radio-brass-outdoor.png" && echo 0 || echo 1)"
out=$("$POS" shell theme neon 2>&1); check "unknown theme falls back" 'fallback' "$out"
out=$("$POS" shell info); check "fallback theme is ice" '"theme":[[:space:]]*"ice"' "$out"; check "fallback mode is normal" '"mode":[[:space:]]*"normal"' "$out"
out=$("$POS" shell theme ice dusk 2>&1); check "unknown mode falls back" 'fallback' "$out"
out=$("$POS" shell theme slate 2>&1); check "theme only keeps mode" 'theme slate mode normal' "$out"
check "selection persisted" 'theme=slate' "$(cat "$POCKETOS_CONFIG_DIR/settings.conf")"
check "mode persisted" 'display_mode=normal' "$(cat "$POCKETOS_CONFIG_DIR/settings.conf")"
"$POS" shell theme neon >/dev/null 2>&1
check "fallback leaves stored value untouched" 'theme=slate' "$(cat "$POCKETOS_CONFIG_DIR/settings.conf")"
out=$("$POS" app start bogus 2>&1); check "unknown app rejected" 'code 2' "$out"
out=$("$POS" app home 2>&1); check_empty "app home" "$out"
out=$("$POS" shell info); check "back at home" '"current":[[:space:]]*"home"' "$out"

# radiod dies under a running shell (bench 2026-09-07: the shell used to die
# with SIGPIPE on its next status poll). The shell must keep its pid, keep
# answering, and poll the restarted radiod again on its own.
kill -KILL $RP; wait $RP 2>/dev/null
sleep 2.5   # at least two one-second status polls against the dead socket
check "shell survives radiod dying" '1' "$(kill -0 $SP 2>/dev/null && echo 1 || echo 0)"
out=$("$POS" shell info); check "shell IPC still answers with radiod gone" '"current":[[:space:]]*"home"' "$out"
"$RADIOD" --backend mock --verbose >"$OUT/radiod2.log" 2>&1 & RP=$!
for _ in $(seq 1 50); do [ -S "$POCKETOS_RUNTIME_DIR/radiod.sock" ] && break; sleep 0.1; done
# the status poll reconnects on its next tick; wait for the first poll to land
# in the new radiod's log (up to 10 s) rather than trusting a fixed sleep
for _ in $(seq 1 100); do grep -q 'radio.status' "$OUT/radiod2.log" 2>/dev/null && break; sleep 0.1; done
check "shell pid unchanged after radiod restart" '1' "$(kill -0 $SP 2>/dev/null && echo 1 || echo 0)"
check "shell reconnected and polls the new radiod" 'radio.status' "$(cat "$OUT/radiod2.log")"
out=$("$POS" radio status); check "radio status recovered through the new radiod" '"state":[[:space:]]*"rx"' "$out"

# radiod alive but not answering. Before the status poll had a deadline this
# was the worse case of the two: nothing crashed, so the supervisor saw a
# healthy pair of processes while the shell sat in a blocking read forever.
kill -STOP $RP
STOPPED_AT=$(date +%s)
sleep 3   # three status polls that now time out instead of blocking
check "shell survives a wedged radiod" '1' "$(kill -0 $SP 2>/dev/null && echo 1 || echo 0)"
out=$("$POS" shell info 2>&1)
check "shell still answers while radiod is wedged" '"current":[[:space:]]*"home"' "$out"
check "shell answered promptly" '1' "$([ $(( $(date +%s) - STOPPED_AT )) -lt 15 ] && echo 1 || echo 0)"
check "the timeout is logged once radiod stops answering" 'timed out' "$(cat "$OUT/shell.log")"
kill -CONT $RP
for _ in $(seq 1 100); do
    out=$("$POS" shell info 2>&1)
    printf '%s' "$out" | grep -q '"current"' && break
    sleep 0.1
done
check "shell recovers when radiod answers again" '"current":[[:space:]]*"home"' "$out"

# The same wedge with the Radio app open (unit A, M5, 2026-09-08): the app's
# tick asked radiod for info, status and stats every second without a
# deadline, so the LVGL thread blocked on the first tick and the panel, touch
# and the shell's own socket were dead until radiod answered again. The peer
# here accepts every request (the kernel completes the connection) and never
# replies. `timeout` bounds the client so a regression fails instead of
# hanging the suite.
out=$("$POS" app start radio 2>&1); check_empty "app start radio for the wedge" "$out"
sleep 1.5   # at least one radio tick against a healthy radiod first
kill -STOP $RP
STOPPED_AT=$(date +%s)
sleep 3     # three ticks: status poll plus three app queries each, all bounded
check "shell survives a wedged radiod with the Radio app open" '1' "$(kill -0 $SP 2>/dev/null && echo 1 || echo 0)"
out=$(timeout 5 "$POS" shell info 2>&1); rc=$?
check "shell answers within 5 s with the Radio app open and radiod wedged" '1' "$([ $rc -eq 0 ] && echo 1 || echo 0)"
check "the Radio app is still the current app" '"current":[[:space:]]*"radio"' "$out"
check "shell answered promptly with the Radio app open" '1' "$([ $(( $(date +%s) - STOPPED_AT )) -lt 15 ] && echo 1 || echo 0)"

# Kept stopped for longer (unit A, 2026-09-08, v0.0.4): every timed-out
# request dropped its connection, but the kernel keeps each one queued in
# radiod's listen backlog (16) until radiod accepts it, so after a few
# seconds the backlog is full and the shell's next reconnect blocked in
# connect() with no deadline at all (wchan unix_wait_for_peer) until SIGCONT.
# The backlog fills here the same way; the shell must keep answering.
SOCK="$POCKETOS_RUNTIME_DIR/radiod.sock"
for _ in $(seq 1 250); do [ "$(grep -c "$SOCK" /proc/net/unix)" -ge 17 ] && break; sleep 0.1; done
queued=$(grep -c "$SOCK" /proc/net/unix)
check "radiod's listen backlog is full (queued connections: $queued)" '1' "$([ "$queued" -ge 17 ] && echo 1 || echo 0)"
sleep 2     # a few more ticks against the full backlog
wchan=$(cat "/proc/$SP/wchan" 2>/dev/null)
check "shell is not blocked in connect() on the full backlog (wchan: ${wchan:-?})" '1' "$([ "$wchan" != "unix_wait_for_peer" ] && echo 1 || echo 0)"
out=$(timeout 5 "$POS" shell info 2>&1); rc=$?
check "shell answers within 5 s with radiod's backlog full" '1' "$([ $rc -eq 0 ] && echo 1 || echo 0)"
check "the Radio app is still current with the backlog full" '"current":[[:space:]]*"radio"' "$out"
out=$(timeout 5 "$POS" app home 2>&1); rc=$?
check "shell takes a command while radiod is wedged" '1' "$([ $rc -eq 0 ] && echo 1 || echo 0)"
kill -CONT $RP
for _ in $(seq 1 100); do
    out=$(timeout 5 "$POS" shell info 2>&1)
    printf '%s' "$out" | grep -q '"current":[[:space:]]*"home"' && break
    sleep 0.1
done
check "shell is home and radiod answers again after the app wedge" '"current":[[:space:]]*"home"' "$out"
out=$(timeout 5 "$POS" radio status 2>&1); check "radiod itself recovered" '"state":[[:space:]]*"rx"' "$out"

kill $SP; wait $SP 2>/dev/null; kill $RP; wait $RP 2>/dev/null
# SIGTERM leaves through the normal exit path: the app is destroyed and the
# listening socket is unlinked. Before that, /etc/init.d/S90pocketos-shell stop
# left a stale shell.sock behind and this test had to delete it by hand.
check "SIGTERM removes the listening socket" '1' \
      "$([ ! -e "$POCKETOS_RUNTIME_DIR/shell.sock" ] && echo 1 || echo 0)"
check "clean stop is logged" 'stopping on signal' "$(cat "$OUT/shell.log")"
check_empty "shell log has no errors" "$(grep -i 'error\|assert' "$OUT/shell.log" || true)"
# a corrupt stored value must not prevent start and must fall back (DS §8)
printf 'theme=zzz
display_mode=normal
' > "$POCKETOS_CONFIG_DIR/settings.conf"
"$SHELL_BIN" >"$OUT/shell2.log" 2>&1 & SP=$!
for _ in $(seq 1 50); do [ -S "$POCKETOS_RUNTIME_DIR/shell.sock" ] && break; sleep 0.1; done
out=$("$POS" shell info); check "corrupt stored theme falls back to ice" '"theme":[[:space:]]*"ice"' "$out"
check "fallback is logged" 'fallback' "$(cat "$OUT/shell2.log")"
check "corrupt value left in file" 'theme=zzz' "$(cat "$POCKETOS_CONFIG_DIR/settings.conf")"
kill $SP; wait $SP 2>/dev/null
rm -rf "$POCKETOS_RUNTIME_DIR" "$POCKETOS_LOG_DIR" "$POCKETOS_CONFIG_DIR" "$OUT"
echo "shell_ipc_test: $failed failure(s)"
exit $((failed > 0))
