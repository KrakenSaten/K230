#!/bin/bash
# pos-capture end to end, driven through pipes the way a person drives it
# from a serial console (docs/apps/PACKETSHARK.md).
#
#   tests/pos_capture_test.sh <pos-capture> <ppc_mkfixtures>
#
# What is checked: every fixture decodes to its committed expected rows
# (tests/data/packetshark/*.expected), and the host's tcpdump, when there is
# one, counts the same frames in each; HEADERS keeps no byte of a plain-text
# HTTP payload, in the decoder and through a replayed session (S4); a
# session starts, counts, holds, answers a window, stops and leaves; a
# second capture is refused while one runs; SIGTERM ends a capturing helper
# cleanly; a clock step of 56 years leaves relative time continuous (T3);
# the time limit stops a capture; a damaged replay stops with an error; bad
# commands are refused and unknown ones ignored; the helper's log carries no
# address from the packets it saw (S7); a session that is not saved leaves
# no file but its lock and its log (S5); the bench runs.
#
# PACKETSHARK_UPDATE_EXPECTED=1 rewrites the expected files from the
# decoder's output instead of comparing (for a reviewed change of output).
set -u
cd "$(dirname "$0")/.." || exit 1
HELPER=$(readlink -f "${1:?pos-capture}")
MKFIX=$(readlink -f "${2:?ppc_mkfixtures}")
DATA=tests/data/packetshark
failed=0
check() { if [ "$2" = "1" ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi; }

T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
export POCKETOS_RUNTIME_DIR="$T/run" POCKETOS_LOG_DIR="$T/log" POCKETOS_LOG_STDERR=0
unset POCKETOS_CAPTURE_SOURCE
mkdir -p "$T/run" "$T/log" "$T/fx"

# ---- the decoder against its expected rows ------------------------------------------
"$MKFIX" "$T/fx"
check "the fixtures are generated" "$([ "$?" = "0" ] && [ -s "$T/fx/dns.pcap" ] && echo 1 || echo 0)"
cp "$DATA/selftest.pcap" "$T/fx/"
for f in "$T"/fx/*.pcap; do
    name=$(basename "$f" .pcap)
    "$HELPER" decode "$f" > "$T/$name.rows" 2>&1
    if [ "${PACKETSHARK_UPDATE_EXPECTED:-0}" = "1" ]; then
        cp "$T/$name.rows" "$DATA/$name.expected"
        echo "note wrote $DATA/$name.expected"
        continue
    fi
    if cmp -s "$T/$name.rows" "$DATA/$name.expected"; then
        check "$name: the rows are the expected ones" 1
    else
        check "$name: the rows are the expected ones" 0
        diff "$DATA/$name.expected" "$T/$name.rows" | head -10
    fi
    if command -v tcpdump >/dev/null 2>&1; then
        want=$(tcpdump -nn -r "$f" 2>/dev/null | wc -l)
        have=$(grep -vc '^#' "$T/$name.rows")
        check "$name: tcpdump reads the same $want frames" "$([ "$want" = "$have" ] && echo 1 || echo 0)"
    fi
done
check "HEADERS keeps no byte of the HTTP payload (S4)" \
    "$(grep -q hunter2 "$T/http.rows" && echo 0 || echo 1)"
"$HELPER" decode "$T/fx/http.pcap" --keep full | awk -F'\t' '$1==3 {print $8}' > "$T/full"
check "FULL keeps it (and says so in the stored column)" "$([ "$(cat "$T/full")" = "119" ] && echo 1 || echo 0)"
check "the backend is named" "$("$HELPER" backend | grep -qxE 'pcap|none' && echo 1 || echo 0)"

# ---- a session through a pipe -------------------------------------------------------
{
    printf 'start synth0 headers 0 all 0\n'; sleep 1.3
    printf 'stats\nhold 1\n'; sleep 0.6
    printf 'stats\nhold 0\n'; sleep 0.4
    printf 'window 1 tail 4 0\nfly away\nhold 7\nstop\nstats\n'; sleep 0.4
    printf 'quit\n'
} | "$HELPER" session --source synth:500:1 > "$T/s1" 2>&1
rc=$?
check "the session leaves with 0" "$([ "$rc" = "0" ] && echo 1 || echo 0)"
check "hello first" "$(head -1 "$T/s1" | grep -qP '^hello\t1\t' && echo 1 || echo 0)"
check "capturing, hold, capturing, stopped by the user" \
    "$(grep -P '^state\t' "$T/s1" | cut -f2,3 | tr '\t\n' ' ' | grep -q 'idle none capturing none hold none capturing none stopped user' && echo 1 || echo 0)"
check "a window of 5 rows" "$([ "$(grep -cP '^row\t' "$T/s1")" = "5" ] && grep -qP '^rows.end\t1$' "$T/s1" && echo 1 || echo 0)"
check "a bad command is refused, an unknown one ignored" \
    "$([ "$(grep -cP '^error\tbadarg' "$T/s1")" = "1" ] && echo 1 || echo 0)"
last=$(grep -P '^stats\t' "$T/s1" | tail -1)
seen=$(echo "$last" | cut -f3); stored=$(echo "$last" | cut -f4); held=$(echo "$last" | cut -f5); shed=$(echo "$last" | cut -f8)
check "seen = stored + held + shed ($seen = $stored + $held + $shed)" \
    "$([ "$seen" -gt 0 ] && [ "$seen" = "$((stored + held + shed))" ] && [ "$held" -gt 0 ] && echo 1 || echo 0)"
check "at least one bin" "$(grep -qP '^bin\t0\t' "$T/s1" && echo 1 || echo 0)"
check "bye last" "$(tail -1 "$T/s1" | grep -qx bye && echo 1 || echo 0)"

# ---- one capture at a time ----------------------------------------------------------
{ printf 'start synth0 headers 0 all 0\n'; sleep 2; printf 'quit\n'; } |
    "$HELPER" session --source synth:100:1 > "$T/a" 2>&1 &
first=$!
sleep 0.5
{ printf 'start synth0 headers 0 all 0\n'; sleep 0.3; printf 'quit\n'; } |
    "$HELPER" session --source synth:100:2 > "$T/b" 2>&1
check "a second capture is refused while one runs" "$(grep -qP '^error\tbusy' "$T/b" && echo 1 || echo 0)"
wait $first

# ---- SIGTERM while capturing --------------------------------------------------------
mkfifo "$T/in"
"$HELPER" session --source synth:1000:1 < "$T/in" > "$T/t" 2>&1 &
pid=$!
exec 7>"$T/in"
printf 'start synth0 headers 0 all 0\n' >&7
sleep 0.8
t0=$(date +%s%N)
kill -TERM $pid
wait $pid
rc=$?
t1=$(date +%s%N)
exec 7>&-
check "SIGTERM: leaves with 0 within 200 ms, saying bye" \
    "$([ "$rc" = "0" ] && [ $(((t1 - t0) / 1000000)) -lt 200 ] && tail -1 "$T/t" | grep -qx bye && echo 1 || echo 0)"

# ---- a clock step (T3) --------------------------------------------------------------
# The synthetic source steps its wall clock by 56 years after 100 frames.
{ printf 'start synth0 headers 0 all 0\n'; sleep 1.5; printf 'window 2 seq:95 0 10\nquit\n'; } |
    "$HELPER" session --source synth:200:1:step=100,1767225600 > "$T/c" 2>&1
check "the step is noticed" "$(grep -qP '^note\tclock-step\tforward' "$T/c" && echo 1 || echo 0)"
gaps=$(grep -P '^row\t' "$T/c" | cut -f4 | sort -n | tail -1)
check "and no gap between two packets exceeds the real one (max ${gaps:-?} us) (T3)" \
    "$([ -n "$gaps" ] && [ "$gaps" -lt 100000 ] && echo 1 || echo 0)"

# ---- the time limit, a damaged replay -----------------------------------------------
{ printf 'start synth0 headers 0 all 1\n'; sleep 2.3; printf 'quit\n'; } |
    "$HELPER" session --source synth:100:1 > "$T/l" 2>&1
check "the time limit stops the capture" "$(grep -qP '^state\tstopped\tlimit' "$T/l" && echo 1 || echo 0)"
head -c 300 "$T/fx/dns.pcap" > "$T/cut.pcap"
{ printf 'start file0 headers 0 all 0\n'; sleep 0.5; printf 'quit\n'; } |
    "$HELPER" session --source "file:$T/cut.pcap" > "$T/d" 2>&1
check "a damaged replay stops with an error" "$(grep -qP '^state\tstopped\terror' "$T/d" && echo 1 || echo 0)"

# ---- S4 through a session, S7, S5 -------------------------------------------------
{ printf 'start file0 headers 0 all 0\n'; sleep 0.5; printf 'window 3 tail 10 0\nquit\n'; } |
    "$HELPER" session --source "file:$T/fx/http.pcap" > "$T/h" 2>&1
check "a replayed HTTP exchange shows no payload in its rows (S4)" \
    "$(grep -qP '^row\t' "$T/h" && ! grep -q hunter2 "$T/h" && echo 1 || echo 0)"
for f in dns dhcp arp icmp local; do
    { printf 'start file0 headers 0 all 0\n'; sleep 0.3; printf 'quit\n'; } |
        "$HELPER" session --source "file:$T/fx/$f.pcap" > /dev/null 2>&1
done
check "the helper's log names no address, name or payload it saw (S7)" \
    "$([ -s "$T/log/pos-capture.log" ] &&
       ! grep -qE '192\.0\.2\.|198\.51\.100\.|2001:db8|example\.net|02:00:00|hunter2|sensor' "$T/log/pos-capture.log" &&
       echo 1 || echo 0)"
check "and says what it did (lifecycle lines)" \
    "$(grep -q 'capture: start' "$T/log/pos-capture.log" && grep -q 'capture: stopped' "$T/log/pos-capture.log" && echo 1 || echo 0)"
extra=$(find "$T/run" "$T/log" -type f ! -name capture.lock ! -name 'pos-capture.log*' | head -3)
check "nothing written but the lock and the log (S5)" "$([ -z "$extra" ] && echo 1 || echo 0)"
[ -n "$extra" ] && echo "$extra"

# ---- the bench --------------------------------------------------------------------
"$HELPER" bench 20000 > "$T/bench" 2>&1
check "the bench runs" "$(grep -qE '^bench decode_store_account_ns_per_frame=[0-9]+' "$T/bench" && echo 1 || echo 0)"
grep '^bench' "$T/bench" | sed 's/^/     /'

echo "pos_capture_test: $failed failure(s)"
exit $((failed > 0))
