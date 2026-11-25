#!/bin/bash
# PacketShark in the running app (tests/packetshark_app_test.c), and in the
# real shell: registered, on the launcher table, kept awake, opened from
# --open in both orientations on the synthetic source, drawn, quiet, and with
# no helper left when it closes.
#
# Requires: SHELL_BIN (the CMake-built pocketos-shell) and make's
# tools/capture/pos-capture. PACKETSHARK_SHOTS=<dir> keeps the screenshots.
# With make's tests/ppc_evid_mkfixtures built, MARK's evidence is also shown
# for the replayed fixtures (N1.0). PACKETSHARK_ONLY=n1 runs MARK and its
# evidence alone, in both shapes, and nothing of the rest.
#
# Copyright (c) 2026 PocketOS authors.
# SPDX-License-Identifier: Apache-2.0
set -u
SHELL_BIN=${SHELL_BIN:?set SHELL_BIN to the pocketos-shell binary}
cd "$(dirname "$0")/.." || exit 1
failed=0
check() { if [ "$2" = "1" ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi; }

HELPER=$(pwd)/tools/capture/pos-capture
if [ ! -x "$HELPER" ]; then
    echo "FAIL build the helper first: make tools/capture/pos-capture"
    exit 1
fi

# The evidence fixtures as pcap files, for the app test to replay (N1.0).
FIX=""
if [ -x tests/ppc_evid_mkfixtures ]; then
    FIX=$(mktemp -d)
    ./tests/ppc_evid_mkfixtures "$FIX" >/dev/null || FIX=""
fi
BIN=${PACKETSHARK_APP_TEST:-$(dirname "$SHELL_BIN")/packetshark_app_test}
if [ -x "$BIN" ]; then
    log=$(PACKETSHARK_HELPER="$HELPER" PACKETSHARK_FIXTURES="$FIX" SDL_VIDEODRIVER=dummy timeout 900 "$BIN" 2>&1); rc=$?
    printf '%s\n' "$log" | grep -E '^FAIL|packetshark_app_test|^note|^     '
    check "PacketShark end to end, tapped, in portrait and landscape, and every fault" \
        "$([ "$rc" = "0" ] && echo 1 || echo 0)"
    printf '%s\n' "$log" | grep -E '^\[(Warn|Error)\]' | head -3
    check "and LVGL warned about nothing while it ran" \
        "$(printf '%s\n' "$log" | grep -qE '^\[(Warn|Error)\]' && echo 0 || echo 1)"
else
    echo "FAIL packetshark_app_test binary missing: $BIN"; failed=$((failed + 1))
fi
[ -n "$FIX" ] && rm -rf "$FIX"
if [ -n "${PACKETSHARK_ONLY:-}" ]; then
    # One part of the app test was asked for: the shell's half is not it.
    echo "packetshark_shell_test ($PACKETSHARK_ONLY): $failed failed"
    exit $((failed > 0))
fi

if grep -aq '/usr/bin/pos-capture' "$SHELL_BIN"; then
    ORIENTATIONS="portrait landscape"
else
    ORIENTATIONS=""
    echo "SKIP the real-shell half: $SHELL_BIN was configured with -DPOCKETOS_WITH_PACKETSHARK=OFF"
fi
for o in $ORIENTATIONS; do
    RUN=$(mktemp -d); LOGD=$(mktemp -d); CFG=$(mktemp -d); STATE=$(mktemp -d)
    SDL_VIDEODRIVER=dummy POCKETOS_RUNTIME_DIR="$RUN" POCKETOS_LOG_DIR="$LOGD" \
    POCKETOS_CONFIG_DIR="$CFG" POCKETOS_STATE_DIR="$STATE" \
    POCKETOS_CAPTURE_HELPER="$HELPER" POCKETOS_CAPTURE_SOURCE=synth:200:1 \
        timeout 60 "$SHELL_BIN" --rotation $o --open packetshark --screenshot "$LOGD/packetshark.png" \
        --exit-after-ms 2500 >"$LOGD/out" 2>&1
    rc=$?
    check "$o: the shell opens PacketShark and draws it" \
        "$([ "$rc" = "0" ] && [ -s "$LOGD/packetshark.png" ] && echo 1 || echo 0)"
    check "$o: PacketShark reports itself open" \
        "$(grep -qh 'open app packetshark' "$LOGD/out" "$LOGD/shell.log" 2>/dev/null && echo 1 || echo 0)"
    check "$o: and logs no fault" "$(grep -qE ' ERROR |assert' "$LOGD/out" "$LOGD/shell.log" 2>/dev/null &&
                                    echo 0 || echo 1)"
    hits=$(grep -hE ' WARN |\[Warn\]' "$LOGD/out" "$LOGD/shell.log" 2>/dev/null | grep -v 'radiod unavailable')
    check "$o: and no warning but the simulator's missing radiod" "$([ -z "$hits" ] && echo 1 || echo 0)"
    [ -n "$hits" ] && echo "$hits" | head -3
    [ -n "${PACKETSHARK_SHOTS:-}" ] && [ -s "$LOGD/packetshark.png" ] &&
        cp "$LOGD/packetshark.png" "$PACKETSHARK_SHOTS/packetshark-shell-$o.png"
    check "$o: opening the app starts no capture (no helper before START)" \
        "$([ ! -e "$LOGD/pos-capture.log" ] && echo 1 || echo 0)"
    check "$o: no helper outlives the shell" "$(pgrep -f "$HELPER session" >/dev/null && echo 0 || echo 1)"
    rm -rf "$RUN" "$LOGD" "$CFG" "$STATE"
done

check "PacketShark is in the shell's registry, on by default (POCKETOS_WITH_PACKETSHARK ON)" \
    "$(grep -q '#define OPTIONAL_PACKETSHARK , &app_packetshark' ui/shell/shell.c &&
       grep -q 'OPTIONAL_APPS .*OPTIONAL_PACKETSHARK' ui/shell/shell.c &&
       grep -q 'option(POCKETOS_WITH_PACKETSHARK .* ON)' ui/shell/CMakeLists.txt && echo 1 || echo 0)"
check "on the launcher: the APPS folder, in the network colour" \
    "$(grep -q '{ "packetshark", HOME_GROUP_FOLDERS, HOME_HUE_NETWORK, HOME_FOLDER_APPS }' ui/shell/home_layout.c &&
       echo 1 || echo 0)"
check "the screen stays on while it is in front" \
    "$(grep -E 'awake_apps\[\] *=' ui/shell/shell.c | grep -q '"packetshark"' && echo 1 || echo 0)"
check "fullscreen (DS §30.8)" "$(grep -q '.chrome = POCKETOS_CHROME_NONE' apps/packetshark/psk_app.c && echo 1 || echo 0)"
check "the simulator alone defaults to the synthetic source" \
    "$([ "$(grep -c 'PSK_SOURCE_DEFAULT="synth:200:1"' ui/shell/CMakeLists.txt)" = 1 ] && echo 1 || echo 0)"

echo "packetshark_shell_test: $failed failure(s)"
exit $((failed > 0))
