#!/bin/bash
# The top-left way back takes a tap anywhere in its corner (DS §48), in the
# running simulator shell, by real taps through the simulator's own pointer
# (shell.tap, test hooks only):
#
#   1. pocketui_back_test (built beside the shell): the slab drawn as before,
#      the corner target's edges to the pixel, the body's first row, the
#      pressed state, a slide off the corner, Enter on the focused slab;
#   2. every app opened in portrait and landscape (and landscape with unit
#      A's 50 px top corners): its header's back slab is where it was, a tap
#      on the screen's top-left pixel, the strip left of the slab, the row
#      above it, the header's foot under it and the last column of the reach
#      right of it each go back; a tap one pixel past the reach, on the
#      title, or on the body's first pixel row does not;
#   3. Settings -> System: the corner goes back to Settings, then home;
#   4. a launcher folder and Controls: the corner comes back to the
#      launcher's page; the title beside it does not;
#   5. thirty corner rounds in a row, and no fault in the shell's log.
#
# Requires SHELL_BIN (the CMake-built pocketos-shell, SDL, with its test
# hooks) and pos (make all).
set -u
SHELL_BIN=${SHELL_BIN:?set SHELL_BIN to the pocketos-shell binary}
POS=${POS:-tools/pos/pos}
cd "$(dirname "$0")/.." || exit 1
export SDL_VIDEODRIVER=dummy
OUT=$(mktemp -d)
failed=0
check() { if [ "$2" = "1" ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi; }

REACH=8 # POCKETUI_BACK_REACH
# Every app with the shell's header in both orientations; RIFT and Video draw
# their own top row in landscape (app.h `header`) and are taken in portrait.
APPS="settings calculator clock calendar notes files terminal camera recorder photo mp3 wave zabbix browser
      fleet radar timber solitaire blackjack 2048 deskbuddy radio vision"
PORTRAIT_ONLY="rift video"

fresh() {
    export POCKETOS_RUNTIME_DIR=$(mktemp -d -p "$OUT") POCKETOS_LOG_DIR=$(mktemp -d -p "$OUT")
    export POCKETOS_CONFIG_DIR=$(mktemp -d -p "$OUT") POCKETOS_STATE_DIR=$(mktemp -d -p "$OUT")
}
start_shell() { # [args]
    "$SHELL_BIN" --theme ice --mode normal "$@" >"$POCKETOS_LOG_DIR/run.log" 2>&1 &
    SP=$!
    for _ in $(seq 1 60); do [ -S "$POCKETOS_RUNTIME_DIR/shell.sock" ] && break; sleep 0.1; done
    sleep 0.4
}
stop_shell() { kill "$SP" 2>/dev/null; wait "$SP" 2>/dev/null; }
info() { "$POS" shell info 2>/dev/null; }
field() { info | python3 -c "import json,sys; d=json.load(sys.stdin); print(json.dumps(d$1))" 2>/dev/null; }
call() { "$POS" call shell "$@" >/dev/null 2>&1; }
current() { field '["current"]' | tr -d '"'; }
folder() { field '["launcher"]["folder"]' | tr -d '"'; }
controls() { field '["launcher"]["controls"]'; }
tap() { call shell.tap x="$1" y="$2"; sleep 0.35; }
no_fault() { ! grep -qE ' ERROR |assert' "$POCKETOS_LOG_DIR/shell.log" "$POCKETOS_LOG_DIR/run.log"; }
# The header's geometry: "<pad_left> <y> <h> <body_y>", the slab at
# (pad_left, y + 8), 72 x 56 (shell.c app_open, DS §7).
header() {
    info | python3 -c "
import json, sys
h = json.load(sys.stdin)['chrome']['header']
print(int(h['pad_left']), int(h['y']), int(h['h']), int(h['body_y'])) if h['present'] else print('')" 2>/dev/null
}
open_app() { "$POS" app start "$1" >/dev/null 2>&1; sleep 0.4; } # a string id: "2048" too

# Every corner point and every point just outside, for the app open now.
# <tag> <app> <where back leads>
corner_round() {
    local tag=$1 app=$2 to=$3 g pl hy hh by x2 y2 x y ok_in=0 ok_out=0 n_in=0 n_out=0 bad=""
    g=$(header)
    if [ -z "$g" ]; then
        check "$tag $app: the shell's header is there" 0
        return
    fi
    set -- $g
    pl=$1 hy=$2 hh=$3 by=$4
    x2=$((pl + 71 + REACH)) y2=$((hy + 8 + 55 + REACH))
    check "$tag $app: the reach ends at the header's foot ($y2, header $hy+$hh)" \
        "$([ "$y2" = $((hy + hh - 1)) ] && echo 1 || echo 0)"
    for xy in "0 0" "$((pl / 2)) $((hy + 36))" "$((pl + 36)) $((hy + 2))" "$((pl + 36)) $y2" \
              "0 $y2" "$x2 $((hy + 36))" "$((pl + 36)) $((hy + 36))"; do
        set -- $xy
        n_in=$((n_in + 1))
        tap "$1" "$2"
        if [ "$(current)" = "$to" ]; then ok_in=$((ok_in + 1)); else bad="$bad in($1,$2)"; fi
        open_app "$app"
    done
    for xy in "$((x2 + 1)) $((hy + 36))" "$((pl + 72 + 16 + 12)) $((hy + 36))" "$((pl + 36)) $by" "0 $by"; do
        set -- $xy
        n_out=$((n_out + 1))
        tap "$1" "$2"
        if [ "$(current)" = "$app" ]; then ok_out=$((ok_out + 1)); else bad="$bad out($1,$2)"; open_app "$app"; fi
    done
    check "$tag $app: the slab at ($pl, $((hy + 8))) and $n_in corner taps go back to $to ($ok_in)$bad" \
        "$([ "$ok_in" = "$n_in" ] && [ "$pl" -ge 20 ] && echo 1 || echo 0)"
    check "$tag $app: $n_out taps just outside stay in $app ($ok_out)" \
        "$([ "$ok_out" = "$n_out" ] && echo 1 || echo 0)"
}

# ---- 1. the unit test built beside the shell ------------------------------------------
BIN=$(dirname "$SHELL_BIN")/pocketui_back_test
if [ -x "$BIN" ]; then
    log=$("$BIN" 2>&1); rc=$?
    printf '%s\n' "$log" | grep -E '^FAIL|^[a-z_]+_test:'
    check "pocketui_back_test passes" "$([ "$rc" = 0 ] && echo 1 || echo 0)"
else
    check "pocketui_back_test binary present ($BIN)" 0
fi

# ---- 2.-5. the running shell ------------------------------------------------------------
for run in portrait landscape landscape50; do
    fresh
    case $run in
        portrait) start_shell --rotation portrait --no-lock; apps="$APPS $PORTRAIT_ONLY" ;;
        landscape) start_shell --rotation landscape --no-lock; apps=$APPS ;;
        landscape50) POCKETOS_SAFE_CORNERS=50,30,30,50 start_shell --rotation landscape --no-lock; apps="settings calculator notes files" ;;
    esac
    check "$run: the shell answers" "$([ "$(current)" = home ] && echo 1 || echo 0)"

    # 2. every app
    for a in $apps; do
        open_app "$a"
        if [ "$(current)" != "$a" ]; then
            check "$run $a: opens" 0
            continue
        fi
        corner_round "$run" "$a" home
        call shell.home; sleep 0.3
    done

    # 3. Settings -> System -> Settings -> home
    open_app settings
    call shell.tap text="System"; sleep 0.5
    sys=$(current)
    tap 0 0
    back1=$(current)
    tap 0 0
    check "$run: Settings -> System, the corner back to Settings, again home ($sys $back1 $(current))" \
        "$([ "$sys" = system ] && [ "$back1" = settings ] && [ "$(current)" = home ] && echo 1 || echo 0)"

    # 4. a folder and Controls
    # The folder page's slab is at the launcher's margin (home_layout.c: 28
    # portrait, 36 landscape), row 8; its title 16 px after it.
    case $run in portrait) m=28 ;; *) m=36 ;; esac
    for f in utilities games apps; do
        call shell.folder id=$f; sleep 0.4
        opened=$(folder)
        tap $((m + 72 + REACH)) 36 # one pixel past the reach
        tap $((m + 72 + 16 + 20)) 36 # the title
        kept=$(folder)
        tap 0 0
        check "$run: folder $f: past the reach and the title keep it, the corner comes back to the launcher ($opened $kept $(folder))" \
            "$([ "$opened" = "$f" ] && [ "$kept" = "$f" ] && [ "$(folder)" = null ] && [ "$(current)" = home ] && echo 1 || echo 0)"
    done
    call shell.controls; sleep 0.4
    shown=$(controls)
    tap 2 70
    check "$run: Controls: the corner closes it to the launcher ($shown $(controls) $(current))" \
        "$([ "$shown" = true ] && [ "$(controls)" = false ] && [ "$(current)" = home ] && echo 1 || echo 0)"

    # 5. thirty rounds
    ok=0
    for k in $(seq 1 30); do
        open_app calculator
        tap $((k % 20)) $((k % 70))
        [ "$(current)" = home ] && ok=$((ok + 1))
    done
    check "$run: thirty corner rounds end home ($ok)" "$([ "$ok" = 30 ] && echo 1 || echo 0)"
    check "$run: the shell is still the one started, no fault in its log" \
        "$(kill -0 "$SP" 2>/dev/null && no_fault && echo 1 || echo 0)"
    stop_shell
done

rm -rf "$OUT"
echo "back_corner_shell_test: $failed failed"
exit $((failed > 0))
