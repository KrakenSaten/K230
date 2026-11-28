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
# no file but its lock and its log (S5); the five findings are counted, a
# view shows them alone and the rail's marks say where they are (M7); SAVE
# writes one 0600 .pcap in a 0700 folder, only from a stopped capture, never
# under /tmp, and the file reads back packet for packet (M8); the bench runs.
#
# PACKETSHARK_UPDATE_EXPECTED=1 rewrites the expected files from the
# decoder's output instead of comparing (for a reviewed change of output).
#
# Copyright (c) 2026 PocketOS authors.
# SPDX-License-Identifier: Apache-2.0
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
# Since F1 hardening a capture prepares its captures folder at START, while
# it may still reach it. Under /tmp that is refused, so sessions that do not
# save leave nothing, and none touches the home folder of whoever runs this.
export POCKETOS_CAPTURES_DIR="$T/captures-none"
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
check "hello first" "$(head -1 "$T/s1" | grep -qP '^hello\t6\t' && echo 1 || echo 0)"
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

# ---- the list and the inspector (M5, M6) --------------------------------------------
{
    printf 'view 1 time ff - 0 0\nstart synth0 headers 0 all 0\n'; sleep 1.2
    printf 'view 2 time 1 - 0 0\nwindow 2 tail 4 0\n'; sleep 0.2
    printf 'view 3 len ff - 0 0\nwindow 3 rank:0 0 4\n'; sleep 0.2
    printf 'view 4 newest ff - 443 0\nwindow 4 tail 0 4\n'; sleep 0.2
    printf 'view 5 time ff 192.0.2.53 0 0\nwindow 5 tail 2 0\n'; sleep 0.2
    printf 'view 6 time ff 999.1.1.1 0 0\n'
    printf 'inspect 7 5 0\npin 5 1\nview 8 time ff - 0 1\nwindow 8 tail 3 0\n'; sleep 0.2
    printf 'inspect 9 999999 0\nstop\n'; sleep 0.3
    printf 'quit\n'
} | "$HELPER" session --source synth:500:1 > "$T/s2" 2>&1
check "view and window: TCP only (classes 1)" \
    "$(awk -F'\t' '/^rows.begin\t2\t/{on=1;next} /^rows.end\t2/{on=0} on && $1=="row" && $6!=0 {bad=1} END{print bad?0:1}' "$T/s2")"
check "the length order: largest first" \
    "$(awk -F'\t' '/^rows.begin\t3\t/{on=1;next} /^rows.end\t3/{on=0} on && $1=="row" {if (prev && $8>prev) bad=1; prev=$8; n++} END{print (n==5 && !bad)?1:0}' "$T/s2")"
check "port 443, the newest first" \
    "$(awk -F'\t' '/^rows.begin\t4\t/{on=1;next} /^rows.end\t4/{on=0} on && $1=="row" {if (prev && $2>prev) bad=1; if ($12 !~ /443/) bad=1; prev=$2; n++} END{print (n==5 && !bad)?1:0}' "$T/s2")"
check "a host filter: every row has it at one end" \
    "$(awk -F'\t' '/^rows.begin\t5\t/{on=1;next} /^rows.end\t5/{on=0} on && $1=="row" {if ($10!="192.0.2.53" && $11!="192.0.2.53") bad=1; n++} END{print (n==3 && !bad)?1:0}' "$T/s2")"
check "a host that is not an address is refused" "$(grep -qP '^error\tbadarg\ta host' "$T/s2" && echo 1 || echo 0)"
check "inspect: the head, the ends, layers, fields, hex, the end" \
    "$(grep -qP '^pkt.begin\t7\t5\t' "$T/s2" && grep -qP '^pkt.ends\t' "$T/s2" && grep -qP '^layer\t0\t-\tFrame' "$T/s2" && grep -qP '^field\t' "$T/s2" && grep -qP '^hex\t0\t' "$T/s2" && grep -qP '^pkt.end\t7$' "$T/s2" && echo 1 || echo 0)"
check "pin, and the pinned view holds that packet alone, flagged" \
    "$(grep -qP '^pinned\t5\t1\t1$' "$T/s2" && awk -F'\t' '/^rows.begin\t8\t/{on=1;next} /^rows.end\t8/{on=0} on && $1=="row" {n++; if ($2!=5 || $13!=1) bad=1} END{print (n==1 && !bad)?1:0}' "$T/s2")"
check "a packet that was never there is gone" "$(grep -qP '^error\tgone\t' "$T/s2" && echo 1 || echo 0)"
check "no line from the inspector carries a tab inside a field or a stray byte" \
    "$(LC_ALL=C grep -P '^(layer|field|hex|pkt)' "$T/s2" | LC_ALL=C grep -qP '[\x00-\x08\x0b-\x1f\x7f]' && echo 0 || echo 1)"

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

# ---- findings, marks and SAVE (M7, M8) ----------------------------------------------
SV="$PWD/out/packetshark-save-test"
rm -rf "$SV"
{
    printf 'start synth0 headers 0 all 0\n'; sleep 6.2 # past the DNS wait (5 s)
    printf 'view 2 time ff - 0 0 2\nwindow 2 tail 3 0\n'; sleep 0.2
    printf 'view 3 time ff - 0 0 1f\nwindow 3 tail 30 0\nmarks 4 32\n'; sleep 0.2
    printf 'view 6 time ff - 0 0 1\nwindow 6 tail 3 0\n'; sleep 0.2
    printf 'view 7 time ff - 0 0 10\nwindow 7 tail 3 0\n'; sleep 0.2
    printf 'view 5 time ff - 0 0\nmarks 5 32\nsave\n'; sleep 0.2
    printf 'stop\n'; sleep 0.3
    printf 'save\n'; sleep 1.5
    printf 'save\n'; sleep 1.5
    printf 'stats\nquit\n'
} | POCKETOS_CAPTURES_DIR="$SV" "$HELPER" session --source synth:2000:1 > "$T/s3" 2>&1
sig=$(grep -P '^stats\t' "$T/s3" | tail -1 | awk -F'\t' '{ print $(NF-5), $(NF-4), $(NF-3), $(NF-2), $(NF-1), $NF }')
check "retransmissions, resets, failed lookups, unreachables, unanswered SYNs and DNS queries are counted ($sig)" \
    "$(echo "$sig" | awk '{ print ($1 > 0 && $2 > 0 && $3 > 0 && $4 > 0 && $5 > 0 && $6 > 0) ? 1 : 0 }')"
# M11.1: the diag line (struct ppc_diag's order): DNS answered, timed and
# not seen; TCP handshakes completed and timed, refusals.
dg=$(grep -P '^diag\t' "$T/s3" | tail -1)
check "a diag line every second while capturing, and one on stop ($(grep -cP '^diag\t' "$T/s3"))" \
    "$([ "$(grep -cP '^diag\t' "$T/s3")" -ge 4 ] && [ "$(echo "$dg" | awk -F'\t' '{ print NF }')" = "37" ] && echo 1 || echo 0)"
check "DNS transactions: answered and timed, a few unanswered, failures by code ($(echo "$dg" | cut -f2-16))" \
    "$(echo "$dg" | awk -F'\t' '{ print ($4 > 50 && $6 > 0 && $10 > 0 && $14 > 0 && $15 > 0 && $16 > 0) ? 1 : 0 }')"
check "TCP connections: handshakes completed and timed, refusals ($(echo "$dg" | cut -f17-24))" \
    "$(echo "$dg" | awk -F'\t' '{ print ($17 > 50 && $18 > 40 && $19 > 40 && $21 > 0 && $23 > 0) ? 1 : 0 }')"
check "a view of resets: every row is one" \
    "$(awk -F'\t' '$1 == "rows.begin" { on = ($2 == 2) } on && $1 == "row" { n++; if (int($9 / 2) % 2 != 1) bad++ } END { print (n == 4 && !bad) ? 1 : 0 }' "$T/s3")"
check "a view of every finding: only marked rows" \
    "$(awk -F'\t' '$1 == "rows.begin" { on = ($2 == 3) } on && $1 == "row" { n++; if ($9 == 0) bad++ } END { print (n > 10 && !bad) ? 1 : 0 }' "$T/s3")"
check "a retransmission and an unanswered SYN say so in their rows" \
    "$(awk -F'\t' '$1 == "rows.begin" { v = $2 } $1 == "row" && v == 6 && $12 ~ /^Retransmission / { r++ } $1 == "row" && v == 7 && $12 ~ /^No answer / { s++ } END { print (r && s) ? 1 : 0 }' "$T/s3")"
check "marks: none to add to a view of findings, some in the whole capture" \
    "$(grep -qP '^marks\t4\t3\t\d+\t32\t0$' "$T/s3" && grep -P '^marks\t5\t5\t\d+\t32\t[0-9a-f]+$' "$T/s3" | grep -qvP '\t0$' && echo 1 || echo 0)"
check "save is refused while the capture runs" "$(grep -qP '^error\tstate\tonly a stopped' "$T/s3" && echo 1 || echo 0)"
check "saving, then stopped again, twice" \
    "$(grep -P '^state\t' "$T/s3" | cut -f2 | tr '\n' ' ' | grep -q 'capturing stopped saving stopped saving stopped' && echo 1 || echo 0)"
names=$(grep -P '^saved\tdone\t' "$T/s3" | cut -f6 | tr '\n' ' ')
first=$(echo "$names" | cut -d' ' -f1)
npk=$(grep -P '^saved\tdone\t' "$T/s3" | head -1 | cut -f3)
check "two saves, two files, each named by the helper ($names)" \
    "$([ "$(echo "$names" | wc -w)" = "2" ] && [ "$(echo "$names" | tr ' ' '\n' | sort -u | grep -c .)" = "2" ] &&
       echo "$names" | tr ' ' '\n' | grep . | grep -qvE '^capture-[0-9-]+\.pcap$'; [ "$?" = "1" ] && echo 1 || echo 0)"
check "the folder is 0700 and the file 0600" \
    "$([ "$(stat -c %a "$SV")" = "700" ] && [ "$(stat -c %a "$SV/$first")" = "600" ] && echo 1 || echo 0)"
check "no part file is left" "$([ -z "$(find "$SV" -name '*.part' | head -1)" ] && echo 1 || echo 0)"
check "the saved file reads back packet for packet ($npk)" \
    "$("$HELPER" decode "$SV/$first" --keep full | tail -1 | grep -q "^# frames=$npk " && [ "${npk:-0}" -gt 1000 ] && echo 1 || echo 0)"
if command -v tcpdump > /dev/null 2>&1; then
    check "and tcpdump reads it too" "$([ "$(tcpdump -nn -r "$SV/$first" 2> /dev/null | wc -l)" = "$npk" ] && echo 1 || echo 0)"
fi
{ printf 'start synth0 headers 0 all 0\n'; sleep 0.4; printf 'stop\nsave\n'; sleep 0.3; printf 'quit\n'; } |
    POCKETOS_CAPTURES_DIR="$T/captures" "$HELPER" session --source synth:500:1 > "$T/s4" 2>&1
check "nothing is saved under /tmp" \
    "$(case "$T" in /tmp/*) grep -qP '^saved\tfail\t0\t\d+\t0\tnodir$' "$T/s4" && [ ! -e "$T/captures" ] && echo 1 || echo 0 ;; *) echo 1 ;; esac)"
{ printf 'start synth0 headers 0 all 0\n'; sleep 0.4; printf 'stop\nsave\n'; printf 'quit\n'; } |
    POCKETOS_CAPTURES_DIR="$SV/cut" "$HELPER" session --source synth:20000:1 > "$T/s5" 2>&1
check "a save cut short by quit leaves no part file" "$([ -z "$(find "$SV" -name '*.part' | head -1)" ] && echo 1 || echo 0)"
check "the log says a capture was saved, and not where or what" \
    "$(grep -q 'capture: saved [0-9]* packets' "$T/log/pos-capture.log" && ! grep -q 'capture-[0-9]' "$T/log/pos-capture.log" && echo 1 || echo 0)"
rm -rf "$SV"

# ---- the bench --------------------------------------------------------------------
"$HELPER" bench 20000 > "$T/bench" 2>&1
check "the bench runs" "$(grep -qE '^bench decode_store_account_ns_per_frame=[0-9]+' "$T/bench" && echo 1 || echo 0)"
grep '^bench' "$T/bench" | sed 's/^/     /'

echo "pos_capture_test: $failed failure(s)"
exit $((failed > 0))
