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

kill $SP; wait $SP 2>/dev/null; kill $RP; wait $RP 2>/dev/null
check_empty "shell log has no errors" "$(grep -i 'error\|assert' "$OUT/shell.log" || true)"
# a corrupt stored value must not prevent start and must fall back (DS §8)
printf 'theme=zzz
display_mode=normal
' > "$POCKETOS_CONFIG_DIR/settings.conf"
rm -f "$POCKETOS_RUNTIME_DIR/shell.sock"   # the killed instance leaves its socket file behind
"$SHELL_BIN" >"$OUT/shell2.log" 2>&1 & SP=$!
for _ in $(seq 1 50); do [ -S "$POCKETOS_RUNTIME_DIR/shell.sock" ] && break; sleep 0.1; done
out=$("$POS" shell info); check "corrupt stored theme falls back to ice" '"theme":[[:space:]]*"ice"' "$out"
check "fallback is logged" 'fallback' "$(cat "$OUT/shell2.log")"
check "corrupt value left in file" 'theme=zzz' "$(cat "$POCKETOS_CONFIG_DIR/settings.conf")"
kill $SP; wait $SP 2>/dev/null
rm -rf "$POCKETOS_RUNTIME_DIR" "$POCKETOS_LOG_DIR" "$POCKETOS_CONFIG_DIR" "$OUT"
echo "shell_ipc_test: $failed failure(s)"
exit $((failed > 0))
