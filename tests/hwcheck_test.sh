#!/bin/bash
# pos-hwcheck --lora on the host with stubbed gpio/spi tools: the probe must
# refuse while the SX1262 lines are owned (exit 3, no gpioset started), must
# finish within a bounded time when the lines are free, must read the sync
# word, and must never leave a gpioset process behind (bench defect B2/B3,
# 2026-09-07: the real gpioset waits forever without a terminal).
set -u
HW=${HW:-tools/hwcheck/hwcheck.sh}
T=$(mktemp -d)
STUB="$T/bin"; mkdir -p "$STUB"
failed=0
check() { if [ "$2" = "1" ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi; }

# stubs: gpioinfo reports owned or free lines (STUB_OWNED), gpioset holds
# forever like libgpiod 2 does and records its pid, gpioget answers, spi-pipe
# returns the SX1262 reset defaults for the two probe commands.
cat > "$STUB/gpioinfo" <<'EOF'
#!/bin/sh
if [ "${STUB_OWNED:-0}" = "1" ]; then c=" consumer=radiod"; else c=""; fi
for l in 5 19 20 12; do printf '\tline  %2d:\tunnamed         \tinput%s\n' "$l" "$c"; done
EOF
cat > "$STUB/gpioset" <<'EOF'
#!/bin/sh
echo $$ >> "$STUB_PIDS"
while :; do sleep 1; done
EOF
cat > "$STUB/gpioget" <<'EOF'
#!/bin/sh
echo '"19"=inactive'
EOF
cat > "$STUB/spi-pipe" <<'EOF'
#!/bin/sh
n=6; while [ $# -gt 0 ]; do [ "$1" = "-b" ] && n=$2; shift; done
cat > /dev/null
if [ "$n" = "6" ]; then printf '\252\252\252\252\024\044'; else printf '\252\052'; fi
EOF
chmod +x "$STUB"/*
export STUB_PIDS="$T/gpioset.pids"; : > "$STUB_PIDS"
export POCKETOS_SX1262_SPI="$T/spidev"; : > "$POCKETOS_SX1262_SPI"
export PATH="$STUB:$PATH"

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

# 3. Lines free: the probe runs, reads 14 24, finishes in bounded time, leaves nothing behind.
start=$(date +%s)
STUB_OWNED=0 bash "$HW" --lora "$T/out3" > "$T/run3.txt" 2>&1; rc=$?
elapsed=$(( $(date +%s) - start ))
check "free lines: exit code 0" "$([ $rc -eq 0 ] && echo 1 || echo 0)"
check "free lines: sync word 14 24 read" "$(grep -q '14 24' "$T/run3.txt" && echo 1 || echo 0)"
check "free lines: VERIFIED line printed" "$(grep -q 'SX126x answers' "$T/run3.txt" && echo 1 || echo 0)"
check "free lines: finished within 15 s" "$([ $elapsed -lt 15 ] && echo 1 || echo 0)"
check "free lines: gpioset was used (power and reset)" "$([ "$(wc -l < "$STUB_PIDS")" -ge 2 ] && echo 1 || echo 0)"
left=0; while read -r p; do kill -0 "$p" 2>/dev/null && left=$((left + 1)); done < "$STUB_PIDS"
check "free lines: no gpioset process left behind" "$([ $left -eq 0 ] && echo 1 || echo 0)"
check "free lines: report says none left" "$(grep -q 'no gpioset process left behind' "$T/run3.txt" && echo 1 || echo 0)"

# 4. Interrupted probe: killing hwcheck mid-run must still take its gpioset children down.
: > "$STUB_PIDS"
STUB_OWNED=0 bash "$HW" --lora "$T/out4" > "$T/run4.txt" 2>&1 & HP=$!
for _ in $(seq 1 50); do [ -s "$STUB_PIDS" ] && break; sleep 0.1; done
sleep 0.1; kill -TERM $HP 2>/dev/null; wait $HP 2>/dev/null; sleep 0.5
left=0; while read -r p; do kill -0 "$p" 2>/dev/null && left=$((left + 1)); done < "$STUB_PIDS"
check "interrupted probe: no gpioset process left behind" "$([ $left -eq 0 ] && echo 1 || echo 0)"

# 5. Plain inventory needs none of the tools and is unaffected by owned lines.
PATH="$(echo "$PATH" | sed "s|$STUB:||")" STUB_OWNED=1 bash "$HW" "$T/out5" > "$T/run5.txt" 2>&1; rc=$?
check "plain inventory exits 0 without gpio tools" "$([ $rc -eq 0 ] && echo 1 || echo 0)"

while read -r p; do kill "$p" 2>/dev/null; done < "$STUB_PIDS"
rm -rf "$T"
echo "hwcheck_test: $failed failure(s)"
exit $((failed > 0))
