#!/bin/bash
# Headless shell test: drive the SDL simulator (dummy video driver) through
# shell.* with pos, and check screenshots and app switching.
# Requires: SHELL_BIN (CMake-built pocketos-shell), radiod and pos built.
set -u
SHELL_BIN=${SHELL_BIN:?set SHELL_BIN to the pocketos-shell binary}
RADIOD=${RADIOD:-services/radiod/radiod}
POS=${POS:-tools/pos/pos}
export POCKETOS_RUNTIME_DIR POCKETOS_LOG_DIR SDL_VIDEODRIVER=dummy
POCKETOS_RUNTIME_DIR=$(mktemp -d)
POCKETOS_LOG_DIR=$(mktemp -d)
OUT=$(mktemp -d)
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
out=$("$POS" shell info)
check "shell info current home" '"current":[[:space:]]*"home"' "$out"
check "shell info display" '"width":[[:space:]]*568' "$out"

out=$("$POS" app start radio 2>&1); check_empty "app start radio" "$out"
sleep 0.5
out=$("$POS" app list); check "radio marked open" '^radio .*open' "$out"
out=$("$POS" shell screenshot "$OUT/radio.png"); check "screenshot saved" 'saved' "$out"
check "screenshot is a PNG" 'PNG' "$(head -c 8 "$OUT/radio.png" | tr -d '\0')"

out=$("$POS" app start bogus 2>&1); check "unknown app rejected" 'code 2' "$out"
out=$("$POS" app home 2>&1); check_empty "app home" "$out"
out=$("$POS" shell info); check "back at home" '"current":[[:space:]]*"home"' "$out"

kill $SP; wait $SP 2>/dev/null; kill $RP; wait $RP 2>/dev/null
check_empty "shell log has no errors" "$(grep -i 'error\|assert' "$OUT/shell.log" || true)"
rm -rf "$POCKETOS_RUNTIME_DIR" "$POCKETOS_LOG_DIR" "$OUT"
echo "shell_ipc_test: $failed failure(s)"
exit $((failed > 0))
