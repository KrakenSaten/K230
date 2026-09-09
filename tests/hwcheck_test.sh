#!/bin/bash
# pos-hwcheck --lora on the host with stubbed gpio/spi tools: the probe must
# refuse while the SX1262 lines are owned (exit 3, no gpioset started), must
# finish within a bounded time when the lines are free, must reset and wait
# for readiness before touching SPI, must capture the sync-word window and
# never leave a gpioset process behind (bench defect B2/B3, 2026-09-07: the
# real gpioset waits forever without a terminal).
#
# The stubs model the chip's reset lifecycle as unit A showed it (M7,
# 2026-09-08): BUSY stays active until RST is actually driven high, and the
# SPI bus answers ff until then. A probe that releases RST after the low
# pulse and reads anyway (v0.0.3) therefore fails here exactly as it failed
# on the board; the fixed probe drives RST high, waits for BUSY low and a
# standby GetStatus over two consecutive polls, then reads.
#
# ReadRegister (v0.0.6): the probe's frame is byte-for-byte what radiod's
# RadioLib sends (opcode 0x1D, address 0x07 0x40, status NOP, data NOPs,
# sync word at returned offset 4). radiod reads 14 24 through a single
# SPI_IOC_MESSAGE; through spi-pipe unit A returned 24 b4 at offset 4 with
# no 0x14 (v0.0.4/v0.0.5), an SPI-transaction effect the probe cannot
# reframe away. The probe now captures the returned window at 6 and 8 bytes
# and dumps both, and its liveness message rests on GetStatus (the reliable
# signal) when the sync word is not delivered. The stub feeds a modelled
# chip response as a test input: a healthy chip returns 14 24 (happy path),
# and a separate case replays unit A's observed 24 b4 to check the probe's
# reporting. Neither asserts what the board will return; that needs the
# hardware retest.
set -u
HW=${HW:-tools/hwcheck/hwcheck.sh}
T=$(mktemp -d)
STUB="$T/bin"; mkdir -p "$STUB"
failed=0
check() { if [ "$2" = "1" ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi; }

# stubs: gpioinfo reports owned or free lines (STUB_OWNED); gpioset holds a
# line until killed like libgpiod 2 does, recording its pid, its arguments and
# the driven value in $STUB_STATE/<chip>.<line>, and forgetting the value when
# it is stopped (a released line has no driven value); gpioget reports BUSY
# active unless RST is driven high (or always, with STUB_BUSY_STUCK=1);
# spi-pipe answers only while RST is driven high, ff otherwise: GetStatus
# (2 bytes) a2 22 standby, ReadRegister (6 or 8 bytes) the modelled window in
# $STUB_REG6 / $STUB_REG8, recording every register read.
cat > "$STUB/gpioinfo" <<'EOF'
#!/bin/sh
if [ "${STUB_OWNED:-0}" = "1" ]; then c=" consumer=radiod"; else c=""; fi
for l in 5 19 20 12; do printf '\tline  %2d:\tunnamed         \tinput%s\n' "$l" "$c"; done
EOF
cat > "$STUB/gpioset" <<'EOF'
#!/bin/sh
echo $$ >> "$STUB_PIDS"
echo "$*" >> "$STUB_ARGS"
chip=""; line=""; val=""
while [ $# -gt 0 ]; do
    case "$1" in
        --chip) chip=$2; shift ;;
        *=*) line=${1%%=*}; val=${1#*=} ;;
    esac
    shift
done
f="$STUB_STATE/$chip.$line"
echo "$val" > "$f"
trap 'rm -f "$f"; exit 0' TERM INT
while :; do sleep 0.2; done
EOF
cat > "$STUB/gpioget" <<'EOF'
#!/bin/sh
line=""
while [ $# -gt 0 ]; do case "$1" in --chip) shift ;; *) line=$1 ;; esac; shift; done
if [ "$line" = "19" ]; then
    if [ "${STUB_BUSY_STUCK:-0}" != "1" ] && [ "$(cat "$STUB_STATE/gpiochip0.5" 2>/dev/null)" = "1" ]; then
        echo '"19"=inactive'
    else
        echo '"19"=active'
    fi
else
    echo "\"$line\"=inactive"
fi
EOF
cat > "$STUB/spi-pipe" <<'EOF'
#!/bin/sh
echo "$*" >> "$STUB_SPI"
n=6; while [ $# -gt 0 ]; do [ "$1" = "-b" ] && n=$2; shift; done
cat > /dev/null
c=$(cat "$STUB_SPI_COUNT" 2>/dev/null || echo 0); c=$((c + 1)); echo "$c" > "$STUB_SPI_COUNT"
if [ "$(cat "$STUB_STATE/gpiochip0.5" 2>/dev/null)" = "1" ] && [ "${STUB_BUSY_STUCK:-0}" != "1" ]; then
    case "$n" in
        2) printf '\242\042' ;;                                   # GetStatus a2 22 (standby)
        6) echo "$c" >> "$STUB_REG_READS"; printf "${STUB_REG6:-\\242\\242\\242\\242\\024\\044}" ;;
        8) echo "$c" >> "$STUB_REG_READS"; printf "${STUB_REG8:-\\242\\242\\242\\242\\024\\044\\264\\000}" ;;
        *) printf '\242' ;;
    esac
else
    i=0; while [ "$i" -lt "$n" ]; do printf '\377'; i=$((i + 1)); done
fi
EOF
chmod +x "$STUB"/*
export STUB_PIDS="$T/gpioset.pids" STUB_ARGS="$T/gpioset.args" STUB_SPI="$T/spi.calls" STUB_STATE="$T/state"
export STUB_SPI_COUNT="$T/spi.count" STUB_REG_READS="$T/reg.reads"
mkdir -p "$STUB_STATE"; : > "$STUB_PIDS"; : > "$STUB_ARGS"; : > "$STUB_SPI"; : > "$STUB_REG_READS"
export POCKETOS_SX1262_SPI="$T/spidev"; : > "$POCKETOS_SX1262_SPI"
export PATH="$STUB:$PATH"
reset_stubs() { : > "$STUB_PIDS"; : > "$STUB_ARGS"; : > "$STUB_SPI"; : > "$STUB_REG_READS"; rm -f "$STUB_SPI_COUNT" "$STUB_STATE"/*; }
left_behind() { # number of recorded gpioset pids still alive
    n=0; while read -r p; do kill -0 "$p" 2>/dev/null && n=$((n + 1)); done < "$STUB_PIDS"; echo "$n"
}

# 1. Lines owned by radiod: refuse, exit 3, no gpioset started, inventory still written.
STUB_OWNED=1 bash "$HW" --lora "$T/out1" > "$T/run1.txt" 2>&1; rc=$?
check "owned lines: exit code 3" "$([ $rc -eq 3 ] && echo 1 || echo 0)"
check "owned lines: refusal message names radiod" "$(grep -q 'REQUIRES RADIOD STOPPED' "$T/run1.txt" && echo 1 || echo 0)"
check "owned lines: consumer reported" "$(grep -q 'consumer=radiod' "$T/run1.txt" && echo 1 || echo 0)"
check "owned lines: no gpioset was started" "$([ ! -s "$STUB_PIDS" ] && echo 1 || echo 0)"
check "owned lines: read-only inventory still produced" "$(ls "$T"/out1/hwcheck-*/report.txt >/dev/null 2>&1 && echo 1 || echo 0)"

# 2. A radiod with the sx1262 backend running (lines free in the stub): refuse as well.
bash -c 'sleep 30; :' radiod --backend sx1262 & FAKE=$!   # two commands: bash keeps its own cmdline instead of exec'ing sleep
STUB_OWNED=0 bash "$HW" --lora "$T/out2" > "$T/run2.txt" 2>&1; rc=$?
kill $FAKE 2>/dev/null; wait $FAKE 2>/dev/null
check "radiod sx1262 process: exit code 3" "$([ $rc -eq 3 ] && echo 1 || echo 0)"
check "radiod sx1262 process: no gpioset was started" "$([ ! -s "$STUB_PIDS" ] && echo 1 || echo 0)"

# 3. Lines free, healthy chip: RST low, RST driven high, standby on two polls,
#    then the register windows; 14 24 found; bounded; nothing left behind.
reset_stubs
start=$(date +%s)
STUB_OWNED=0 bash "$HW" --lora "$T/out3" > "$T/run3.txt" 2>&1; rc=$?
elapsed=$(( $(date +%s) - start ))
check "free lines: exit code 0" "$([ $rc -eq 0 ] && echo 1 || echo 0)"
check "free lines: RST was driven low" "$(grep -q '5=0' "$STUB_ARGS" && echo 1 || echo 0)"
check "free lines: RST was driven high after the low pulse (not released)" \
      "$([ "$(grep '5=' "$STUB_ARGS" | tail -1)" = "--chip gpiochip0 5=1" ] && echo 1 || echo 0)"
check "free lines: readiness reported (BUSY low, standby status over two polls)" "$(grep -q 'SX126x ready .* ms after reset: BUSY (GPIO19) low, GetStatus chip mode 2' "$T/run3.txt" && echo 1 || echo 0)"
first_reg=$(head -1 "$STUB_REG_READS" 2>/dev/null); first_reg=${first_reg:-0}
check "free lines: the register read waited for two status polls first (SPI transfer #$first_reg, not #1)" "$([ "$first_reg" -ge 3 ] && echo 1 || echo 0)"
check "free lines: both a 6-byte and an 8-byte window were captured" \
      "$(grep -q -- '-b 6' "$STUB_SPI" && grep -q -- '-b 8' "$STUB_SPI" && echo 1 || echo 0)"
check "free lines: both windows dumped in the report" \
      "$(grep -q '6-byte window:' "$T/run3.txt" && grep -q '8-byte window:' "$T/run3.txt" && echo 1 || echo 0)"
check "free lines: sync word 14 24 read" "$(grep -q '14 24' "$T/run3.txt" && echo 1 || echo 0)"
check "free lines: GetStatus read too (a2 22)" "$(grep -q 'a2 22' "$T/run3.txt" && echo 1 || echo 0)"
check "free lines: VERIFIED line printed" "$(grep -q 'SX126x answers' "$T/run3.txt" && echo 1 || echo 0)"
check "free lines: finished within 15 s" "$([ $elapsed -lt 15 ] && echo 1 || echo 0)"
check "free lines: gpioset was used for power, reset low and reset high" "$([ "$(wc -l < "$STUB_PIDS")" -ge 3 ] && echo 1 || echo 0)"
check "free lines: no gpioset process left behind" "$([ "$(left_behind)" -eq 0 ] && echo 1 || echo 0)"
check "free lines: every driven line released" "$([ -z "$(ls -A "$STUB_STATE")" ] && echo 1 || echo 0)"
check "free lines: report says none left" "$(grep -q 'no gpioset process left behind' "$T/run3.txt" && echo 1 || echo 0)"

# 3b. Unit A's observed response: the transaction returns 24 b4 at offset 4
#     with no 0x14 in either window, but GetStatus still reports standby. The
#     probe must exit 5, dump both windows, name it an SPI-transaction effect
#     (not the frame), and report the chip alive from GetStatus.
reset_stubs
STUB_OWNED=0 STUB_REG6='\242\242\242\242\044\264' STUB_REG8='\242\242\242\242\044\264\000\000' \
    bash "$HW" --lora "$T/out3b" > "$T/run3b.txt" 2>&1; rc=$?
check "observed bytes: exit code 5 (sync word gate still fails)" "$([ $rc -eq 5 ] && echo 1 || echo 0)"
check "observed bytes: the 24 b4 window is dumped" "$(grep -q '24 b4' "$T/run3b.txt" && echo 1 || echo 0)"
check "observed bytes: 14 24 is not claimed" "$(grep -q 'VERIFIED' "$T/run3b.txt" && echo 0 || echo 1)"
check "observed bytes: named an SPI-transaction effect, not the frame" "$(grep -q 'SPI-transaction effect' "$T/run3b.txt" && echo 1 || echo 0)"
check "observed bytes: GetStatus standby means the chip is answering" "$(grep -q 'chip in standby' "$T/run3b.txt" && echo 1 || echo 0)"

# 3c. The widening property, on the parser directly: a sync word that lands at
#     offset 5 is inside an 8-byte window but truncated out of a 6-byte one.
w8="a2 a2 a2 a2 a2 14 24 b4"; w6="a2 a2 a2 a2 a2 14"
check "widening: an offset-5 sync word is present in the 8-byte window" "$(case "$w6 | $w8" in *"14 24"*) echo 1 ;; *) echo 0 ;; esac)"
check "widening: the same word is truncated out of a 6-byte-only window" "$(case "$w6" in *"14 24"*) echo 0 ;; *) echo 1 ;; esac)"

# 4. Interrupted probe: killing hwcheck mid-run must still take its gpioset children down.
reset_stubs
STUB_OWNED=0 bash "$HW" --lora "$T/out4" > "$T/run4.txt" 2>&1 & HP=$!
for _ in $(seq 1 50); do [ -s "$STUB_PIDS" ] && break; sleep 0.1; done
sleep 0.1; kill -TERM $HP 2>/dev/null; wait $HP 2>/dev/null; sleep 0.5
check "interrupted probe: no gpioset process left behind" "$([ "$(left_behind)" -eq 0 ] && echo 1 || echo 0)"

# 5. Plain inventory needs none of the tools and is unaffected by owned lines.
PATH="$(echo "$PATH" | sed "s|$STUB:||")" STUB_OWNED=1 bash "$HW" "$T/out5" > "$T/run5.txt" 2>&1; rc=$?
check "plain inventory exits 0 without gpio tools" "$([ $rc -eq 0 ] && echo 1 || echo 0)"

# 6. BUSY stuck high: the probe must give up within its budget, say so, touch
#    the SPI bus not at all, and release everything.
reset_stubs
start=$(date +%s)
STUB_OWNED=0 STUB_BUSY_STUCK=1 bash "$HW" --lora "$T/out6" > "$T/run6.txt" 2>&1; rc=$?
elapsed=$(( $(date +%s) - start ))
check "stuck BUSY: exit code 5" "$([ $rc -eq 5 ] && echo 1 || echo 0)"
check "stuck BUSY: the report names the readiness failure and the RST state" "$(grep -q 'SX126x not ready .* RST driven high' "$T/run6.txt" && echo 1 || echo 0)"
check "stuck BUSY: no SPI read attempted" "$([ ! -s "$STUB_SPI" ] && echo 1 || echo 0)"
check "stuck BUSY: bounded (under 10 s)" "$([ $elapsed -lt 10 ] && echo 1 || echo 0)"
check "stuck BUSY: no gpioset process left behind" "$([ "$(left_behind)" -eq 0 ] && echo 1 || echo 0)"
check "stuck BUSY: every driven line released" "$([ -z "$(ls -A "$STUB_STATE")" ] && echo 1 || echo 0)"

while read -r p; do kill "$p" 2>/dev/null; done < "$STUB_PIDS"
rm -rf "$T"
echo "hwcheck_test: $failed failure(s)"
exit $((failed > 0))
