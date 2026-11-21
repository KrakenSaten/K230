#!/bin/bash
# PocketFleet persistence in the running shell: a match is stored after a
# resolved turn, a stored match is offered again, a finished one is not, and
# every failure path (missing, unwritable, damaged, foreign) leaves the app
# usable with no persistence rather than blocking it.
#
# It also runs fleet_app_test, which is the app itself under a real pointer
# device in both orientations (the layout, the boards and the tap path).
#
# Requires: SHELL_BIN (the CMake-built pocketos-shell). Headless via SDL's
# dummy driver, exactly like tests/shell_ipc_test.sh.
set -u
SHELL_BIN=${SHELL_BIN:?set SHELL_BIN to the pocketos-shell binary}
# Built beside the shell by ui/shell/CMakeLists.txt (host builds).
FLEET_APP_TEST=${FLEET_APP_TEST:-$(dirname "$SHELL_BIN")/fleet_app_test}
export SDL_VIDEODRIVER=dummy
POCKETOS_RUNTIME_DIR=$(mktemp -d)
POCKETOS_LOG_DIR=$(mktemp -d)
POCKETOS_CONFIG_DIR=$(mktemp -d)
POCKETOS_STATE_DIR=$(mktemp -d)
export POCKETOS_RUNTIME_DIR POCKETOS_LOG_DIR POCKETOS_CONFIG_DIR POCKETOS_STATE_DIR
OUT=$(mktemp -d)
SAVE="$POCKETOS_STATE_DIR/fleet/save.v1"
SAVE_BYTES=898
failed=0

check() { if [ "$2" = "1" ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi; }
has() { if printf '%s' "$3" | grep -q -- "$2"; then echo "ok   $1"; else
        echo "FAIL $1: expected '$2' in:"; printf '%s\n' "$3" | tail -5; failed=$((failed + 1)); fi; }
hasnt() { if printf '%s' "$3" | grep -q -- "$2"; then
        echo "FAIL $1: unexpected '$2'"; failed=$((failed + 1)); else echo "ok   $1"; fi; }

# Run the shell once on a chosen PocketFleet screen and return its log.
run() { # <screen|-> <png>
    if [ "$1" = "-" ]; then unset POCKETFLEET_SCREEN; else export POCKETFLEET_SCREEN="$1"; fi
    "$SHELL_BIN" --open fleet --screenshot "$OUT/$2.png" --exit-after-ms 900 \
        >"$OUT/$2.log" 2>&1
    cat "$OUT/$2.log"
}

# 0. The app under a finger, in both orientations. A missing test binary is a
#    failure, not a skip.
#    At every text size (DS §46.4): Fleet keeps its type, so its screens stay
#    the one screen they are at Small - every control in view with nothing
#    scrolled - and the whole test holds at Medium and Large as it does at
#    Small.
if [ -x "$FLEET_APP_TEST" ]; then
    for size in small medium large; do
        log=$(POCKETUI_TEST_TEXT_SIZE=$size "$FLEET_APP_TEST" 2>&1); rc=$?
        printf '%s
' "$log" | grep -E '^FAIL|^     .* is in view|fleet_app_test:' | sed "s/^/[$size] /"
        check "the app, the layout and the tap path, text size $size" "$([ "$rc" = "0" ] && echo 1 || echo 0)"
    done
else
    echo "FAIL fleet_app_test binary missing: $FLEET_APP_TEST"; failed=$((failed + 1))
fi

# 1. A resolved turn stores the match.
log=$(run battle a)
check "battle screenshot rendered" "$([ -s "$OUT/a.png" ] && echo 1 || echo 0)"
check "a resolved turn wrote a save" "$([ -f "$SAVE" ] && echo 1 || echo 0)"
check "the save is one blob" \
      "$([ "$(wc -c < "$SAVE" 2>/dev/null)" = "$SAVE_BYTES" ] && echo 1 || echo 0)"
check "no temporary file left" "$([ -f "$SAVE.tmp" ] && echo 0 || echo 1)"
hasnt "no error while saving" 'ERROR' "$log"

# 2. The next launch finds it and offers to continue.
log=$(run - b)
has "the stored match is offered" 'resumable match' "$log"

# 3. A finished match is not offered again: the slot is cleared.
log=$(run result c)
check "a finished match clears the save" "$([ -f "$SAVE" ] && echo 0 || echo 1)"
log=$(run - d)
hasnt "nothing is offered after a win" 'resumable match' "$log"

# 4. A damaged save must never block the app.
mkdir -p "$(dirname "$SAVE")"
printf 'this is not a PocketFleet save at all' > "$SAVE"
log=$(run - e)
has "a damaged save is rejected" 'rejected, starting fresh' "$log"
check "the app still rendered" "$([ -s "$OUT/e.png" ] && echo 1 || echo 0)"
hasnt "a damaged save is not an error" 'ERROR' "$log"

# 5. A save of the right size but wrong content (a version or checksum change
#    would look like this) is rejected the same way.
head -c "$SAVE_BYTES" /dev/zero | tr '\0' 'x' > "$SAVE"
log=$(run - f)
has "a foreign save of the right size is rejected" 'rejected, starting fresh' "$log"
check "the app still rendered" "$([ -s "$OUT/f.png" ] && echo 1 || echo 0)"

# 6. An unwritable directory switches persistence off for the session.
rm -f "$SAVE"
chmod 0555 "$(dirname "$SAVE")"
if [ -w "$(dirname "$SAVE")" ]; then
    echo "skip unwritable-directory checks (running as root)"
else
    log=$(run battle g)
    has "an unwritable directory is reported" 'continuing without persistence' "$log"
    check "the app still rendered" "$([ -s "$OUT/g.png" ] && echo 1 || echo 0)"
    hasnt "losing persistence is not an error" 'ERROR' "$log"
fi
chmod 0755 "$(dirname "$SAVE")"

# 7. The paced reply: a shot taken through the FIRE path leaves a timer
#    pending. It must settle on its own, and it must also settle when the app
#    is torn down before the timer fires, without touching freed state.
rm -f "$SAVE"
log=$(run battle_paced h)
check "a paced turn completes" "$([ -f "$SAVE" ] && echo 1 || echo 0)"
hasnt "no error while pacing a turn" 'ERROR\|Assert\|assert' "$log"
rm -f "$SAVE"
export POCKETFLEET_SCREEN=battle_paced
"$SHELL_BIN" --open fleet --exit-after-ms 150 >"$OUT/i.log" 2>&1
check "teardown during a paced turn exits cleanly" "$([ $? = 0 ] && echo 1 || echo 0)"
hasnt "teardown during a paced turn is clean" 'ERROR\|Assert\|assert' "$(cat "$OUT/i.log")"
check "an interrupted turn is still stored" "$([ -f "$SAVE" ] && echo 1 || echo 0)"

# 8. Reduced motion must not change the outcome of a turn, only its pacing.
printf 'reduced_motion=1\n' > "$POCKETOS_CONFIG_DIR/settings.conf"
rm -f "$SAVE"
log=$(run battle_paced j)
check "a turn completes with reduced motion" "$([ -f "$SAVE" ] && echo 1 || echo 0)"
hasnt "no error with reduced motion" 'ERROR\|Assert\|assert' "$log"
rm -f "$POCKETOS_CONFIG_DIR/settings.conf"

# 9. Every screen renders in landscape too, with no fault in the log: the
#    layout is the app's own, but the shell is what hosts it.
rm -f "$SAVE"
for screen in - deploy battle battle_paced result; do
    name="land-$screen"
    if [ "$screen" = "-" ]; then unset POCKETFLEET_SCREEN; else export POCKETFLEET_SCREEN="$screen"; fi
    "$SHELL_BIN" --open fleet --rotation landscape \
        --screenshot "$OUT/$name.png" --exit-after-ms 900 >"$OUT/$name.log" 2>&1
    check "$screen renders in landscape" "$([ -s "$OUT/$name.png" ] && echo 1 || echo 0)"
    hasnt "no fault on $screen in landscape" 'ERROR\|Assert\|assert' "$(cat "$OUT/$name.log")"
done
unset POCKETFLEET_SCREEN

# 10. Multiplayer in the running shell (docs/apps/FLEET_MULTIPLAYER.md), against
#     the virtual opponent: every state renders in both shapes with no fault,
#     a live match is stored as one match.v1 of its fixed size and nothing is
#     left half written, the real link with no mesh service says so and stores
#     nothing, and a damaged match.v1 blocks nothing. FLEET_SHOTS_DIR, when set,
#     receives the screenshots under the names docs/design/shots uses.
MATCH="$POCKETOS_STATE_DIR/fleet/match.v1"
MATCH_BYTES=655
rm -f "$SAVE" "$MATCH"
for screen in lobby mp_invited mp_deploy mp_battle mp_waiting mp_lost mp_result mp_chat; do
    case $screen in
        mp_invited) fake="invite=1000" ;;
        mp_waiting) fake="delay=1500" ;;
        *) fake="think=1500" ;;
    esac
    for shape in portrait landscape; do
        name="mp-$screen-$shape"
        rm -f "$MATCH"
        POCKETFLEET_MP_FAKE="$fake" POCKETFLEET_SCREEN="$screen" \
            "$SHELL_BIN" --open fleet ${shape:+--rotation $shape} \
            --screenshot "$OUT/$name.png" --exit-after-ms 1500 >"$OUT/$name.log" 2>&1
        check "$screen renders ($shape)" "$([ -s "$OUT/$name.png" ] && echo 1 || echo 0)"
        hasnt "no fault on $screen ($shape)" 'ERROR\|Assert\|assert' "$(cat "$OUT/$name.log")"
        if [ -n "${FLEET_SHOTS_DIR:-}" ]; then
            suffix=""; [ "$shape" = "landscape" ] && suffix="-landscape"
            cp "$OUT/$name.png" "$FLEET_SHOTS_DIR/fleet-mp-${screen#mp_}$suffix.png"
        fi
    done
done
rm -f "$MATCH"
POCKETFLEET_MP_FAKE="think=1500" POCKETFLEET_SCREEN=mp_battle \
    "$SHELL_BIN" --open fleet --exit-after-ms 1500 >"$OUT/mp-store.log" 2>&1
check "a match under way is stored" "$([ -f "$MATCH" ] && echo 1 || echo 0)"
check "as one blob of its fixed size" \
      "$([ "$(wc -c < "$MATCH" 2>/dev/null)" = "$MATCH_BYTES" ] && echo 1 || echo 0)"
check "with no temporary file left" "$([ -f "$MATCH.tmp" ] && echo 0 || echo 1)"
log=$(run - mp-reopen)
hasnt "a saved match does not stand in for the solo save" 'resumable match' "$log"
rm -f "$MATCH"
unset POCKETFLEET_MP_FAKE
log=$(run lobby mp-noservice)
check "with no mesh service the lobby still renders" "$([ -s "$OUT/mp-noservice.png" ] && echo 1 || echo 0)"
check "and nothing is stored" "$([ -f "$MATCH" ] && echo 0 || echo 1)"
hasnt "no mesh service is not an error" 'ERROR' "$log"
mkdir -p "$(dirname "$MATCH")"
printf 'not a match' > "$MATCH"
log=$(run - mp-damaged)
check "a damaged match.v1 blocks nothing" "$([ -s "$OUT/mp-damaged.png" ] && echo 1 || echo 0)"
has "and is reported" 'cannot be read\|no match to resume' "$log"
rm -f "$MATCH"
unset POCKETFLEET_SCREEN

# 11. The local network in the running shell (docs/apps/FLEET_MULTIPLAYER.md,
#     "The local network in the app"), against a scripted peerd
#     (tests/fake-peerd): Command offers both transports and opening Fleet
#     asks peerd nothing; the lobby on the network renders in both shapes
#     with this device's address and the players peerd holds; an address is
#     asked and answered, and one nobody is at is said to be so; closing ends
#     this device's visibility; no match file is written by any of it; and
#     with no peerd the network is not offered at all. A missing fake-peerd
#     is a failure, not a skip.
FAKE_PEERD=${FAKE_PEERD:-$(cd "$(dirname "$0")" && pwd)/fake-peerd}
LAN_MATCH="$POCKETOS_STATE_DIR/fleet/match-lan.v1"
PEERD_LOG="$OUT/peerd.log"
BOB=b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2
CAROL=c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3
LAN_NODES="[{\"key\":\"$BOB\",\"name\":\"Bob\",\"address\":\"192.168.1.41\",\"heard_ms_ago\":1000,\"kept\":false,\"proved\":true,\"route\":\"lan\"},{\"key\":\"$CAROL\",\"name\":\"Carol\",\"kept\":true,\"proved\":false,\"route\":\"lan\"}]"
PEERD_PID=""
peerd_start() { # <nodes json>
    rm -f "$PEERD_LOG"
    FAKE_PEERD_NODES="$1" FAKE_PEERD_NODES_AFTER="$LAN_NODES" FAKE_PEERD_PROBE="192.168.1.41=$BOB" \
        FAKE_PEERD_LOG="$PEERD_LOG" FAKE_PEERD_LIFE_MS=120000 "$FAKE_PEERD" &
    PEERD_PID=$!
    for _ in $(seq 100); do [ -S "$POCKETOS_RUNTIME_DIR/peerd.sock" ] && break; sleep 0.05; done
}
peerd_stop() {
    [ -n "$PEERD_PID" ] && kill "$PEERD_PID" 2>/dev/null && wait "$PEERD_PID" 2>/dev/null
    PEERD_PID=""
    rm -f "$POCKETOS_RUNTIME_DIR/peerd.sock"
}
lan_shot() { # <screen|-> <name> <shape> <ms>
    if [ "$1" = "-" ]; then unset POCKETFLEET_SCREEN; else export POCKETFLEET_SCREEN="$1"; fi
    "$SHELL_BIN" --open fleet --rotation "$3" --screenshot "$OUT/$2.png" --exit-after-ms "$4" \
        >"$OUT/$2.log" 2>&1
    check "$2 renders" "$([ -s "$OUT/$2.png" ] && echo 1 || echo 0)"
    hasnt "no fault on $2" 'ERROR\|Assert\|assert' "$(cat "$OUT/$2.log")"
    if [ -n "${FLEET_SHOTS_DIR:-}" ]; then cp "$OUT/$2.png" "$FLEET_SHOTS_DIR/fleet-$2.png"; fi
}
rm -f "$SAVE" "$MATCH" "$LAN_MATCH"
if [ -x "$FAKE_PEERD" ]; then
    peerd_start "$LAN_NODES"
    check "a scripted peerd is listening" "$([ -S "$POCKETOS_RUNTIME_DIR/peerd.sock" ] && echo 1 || echo 0)"
    for shape in portrait landscape; do
        lan_shot - "lan-command-$shape" "$shape" 900
    done
    check "opening Fleet on a device with peerd asks it nothing" \
          "$([ -s "$PEERD_LOG" ] && echo 0 || echo 1)"
    for shape in portrait landscape; do
        lan_shot lan_lobby "lan-lobby-$shape" "$shape" 900
    done
    has "the lobby asked to be visible" 'peer.set_visible|{"visible":true}' "$(cat "$PEERD_LOG" 2>/dev/null)"
    has "and listed the players" 'peer.nodes|' "$(cat "$PEERD_LOG" 2>/dev/null)"
    sleep 0.3
    has "closing the shell ended the visibility" 'peer.set_visible|{"visible":false}' \
        "$(cat "$PEERD_LOG" 2>/dev/null)"
    hasnt "nothing was sent to anybody" 'peer.app_send|' "$(cat "$PEERD_LOG" 2>/dev/null)"
    peerd_stop
    peerd_start "[]"
    for shape in portrait landscape; do
        POCKETFLEET_LAN_ASK=192.168.1.99 lan_shot lan_ask "lan-no-answer-$shape" "$shape" 900
    done
    for shape in portrait landscape; do
        POCKETFLEET_LAN_ASK=192.168.1.41 lan_shot lan_ask "lan-answered-$shape" "$shape" 900
    done
    has "an address was asked" 'peer.probe|{"address":"192.168.1.41"}' "$(cat "$PEERD_LOG" 2>/dev/null)"
    peerd_stop
    peerd_start "[]"
    lan_shot lan_silent "lan-silent-portrait" portrait 900
    lan_shot lan_silent "lan-silent-landscape" landscape 900
    peerd_stop
    check "none of it wrote a match file" \
          "$([ -f "$LAN_MATCH" ] || [ -f "$MATCH" ] && echo 0 || echo 1)"
else
    echo "FAIL fake-peerd binary missing: $FAKE_PEERD (make tests/fake-peerd)"; failed=$((failed + 1))
fi
# A match in hand on LoRa, on a device that has both: Command shows the
# network disabled beside RESUME MATCH. (The virtual opponent stands in for
# both transports; "lan" in its spec is what offers the network.)
rm -f "$MATCH" "$LAN_MATCH"
POCKETFLEET_MP_FAKE="think=1500,lan" POCKETFLEET_SCREEN=mp_battle \
    "$SHELL_BIN" --open fleet --exit-after-ms 1500 >"$OUT/lan-held-make.log" 2>&1
check "a LoRa match is in hand" "$([ -f "$MATCH" ] && echo 1 || echo 0)"
for shape in portrait landscape; do
    POCKETFLEET_MP_FAKE="think=1500,lan" lan_shot - "lan-held-$shape" "$shape" 900
done
check "showing Command touched neither save" \
      "$([ "$(wc -c < "$MATCH" 2>/dev/null)" = "$MATCH_BYTES" ] && [ ! -f "$LAN_MATCH" ] && echo 1 || echo 0)"
rm -f "$MATCH" "$LAN_MATCH"
# No peerd on the device: the network is not offered, and asking for its
# lobby opens the one there is.
lan_shot lan_lobby "lan-absent" portrait 900
check "with no peerd nothing is stored either" \
      "$([ -f "$LAN_MATCH" ] || [ -f "$MATCH" ] && echo 0 || echo 1)"
unset POCKETFLEET_SCREEN

rm -rf "$POCKETOS_RUNTIME_DIR" "$POCKETOS_LOG_DIR" "$POCKETOS_CONFIG_DIR" \
       "$POCKETOS_STATE_DIR" "$OUT"
echo "fleet_shell_test: $failed failure(s)"
exit $((failed > 0))
