#!/bin/bash
# Settings in the running app, and the shell's side of it.
#
# Requires: SHELL_BIN (the CMake-built pocketos-shell).
set -u
SHELL_BIN=${SHELL_BIN:?set SHELL_BIN to the pocketos-shell binary}
cd "$(dirname "$0")/.." || exit 1
failed=0
check() { if [ "$2" = "1" ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi; }

BIN=${SETTINGS_APP_TEST:-$(dirname "$SHELL_BIN")/settings_app_test}
if [ -x "$BIN" ]; then
    log=$(SDL_VIDEODRIVER=dummy "$BIN" 2>&1); rc=$?
    printf '%s\n' "$log" | grep -E '^FAIL|settings_app_test:'
    check "Settings end to end, tapped and typed into" "$([ "$rc" = "0" ] && echo 1 || echo 0)"
else
    echo "FAIL settings_app_test binary missing: $BIN"; failed=$((failed + 1))
fi

check "the shell knows about Settings" "$(grep -q '&app_settings' ui/shell/shell.c && echo 1 || echo 0)"
check "and builds it" "$(grep -q 'apps/settings/settings_app.c' ui/shell/CMakeLists.txt && echo 1 || echo 0)"
bash tests/settings_lint.sh >/dev/null 2>&1
check "the Settings lint passes" "$([ $? = 0 ] && echo 1 || echo 0)"

RUN=$(mktemp -d); LOGD=$(mktemp -d); CFG=$(mktemp -d); STATE=$(mktemp -d)
SDL_VIDEODRIVER=dummy POCKETOS_RUNTIME_DIR="$RUN" POCKETOS_LOG_DIR="$LOGD" \
POCKETOS_CONFIG_DIR="$CFG" POCKETOS_STATE_DIR="$STATE" POCKETOS_TEST_SYSFS_ROOT="$RUN/nosys" \
    "$SHELL_BIN" --open settings --exit-after-ms 1500 >"$LOGD/out" 2>&1
rc=$?
check "the shell opens Settings with no netd running" "$([ "$rc" = "0" ] && echo 1 || echo 0)"
check "and logs no fault" "$(grep -qE ' ERROR |assert' "$LOGD/out" && echo 0 || echo 1)"
check "Settings reports itself open" \
    "$(grep -q 'open app settings' "$LOGD/shell.log" "$LOGD/out" 2>/dev/null && echo 1 || echo 0)"
check "opening Settings writes nothing to the app state directory" \
    "$([ -z "$(ls -A "$STATE" 2>/dev/null)" ] && echo 1 || echo 0)"
check "nor to the settings store" "$([ ! -e "$CFG/settings.conf" ] && echo 1 || echo 0)"
rm -rf "$RUN" "$LOGD" "$CFG" "$STATE"

echo "settings_shell_test: $failed failure(s)"
exit $((failed > 0))
