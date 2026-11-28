#!/bin/bash
# Marks and their evidence through the real helper (N1.0,
# docs/apps/PACKETSHARK.md §4d), tools/capture/pos-capture:
#
#   - the five fixtures of tests/ppc_evid_fixtures.h, written as pcap files
#     by tests/ppc_evid_mkfixtures and replayed with this device's MAC named,
#     a mark set once the replay has ended: the evidence lines are the ones
#     committed in tests/data/packetshark/evid-<name>.expected, and the same
#     on a second replay (replay consistency);
#   - the same fixture replayed without the MAC: direction is unknown, and
#     nothing is then called this device's own or expected here;
#   - on the synthetic source, live: `mark` is answered at once, capture
#     goes on, the window after a mark is open and grows, a mark on HOLD
#     says so and tells the seconds held, STOP closes the window, a mark
#     after STOP stands where the capture stopped, a mark that is not held
#     is refused, and the ninth mark replaces the first;
#   - evidence rows name packets the list can still show;
#   - the log names no address the fixtures carry;
#   - what the review of PR #188 found (2026-11-25): a count-only burst far
#     shorter than a tick is told in its window; a mark set after STOP has
#     the wall clock of STOP; a capture filter a source does not apply is
#     not reported as applied; and, with tests/pos-capture-testhooks, an
#     analysis that could not start is reported and nothing of its classes
#     is observed, while the shipped helper has no such switch.
#
# A pcap file holds frames and times only: a fixture's own mark, its HOLD and
# the frames it reports lost are played by tests/ppc_evid_test.c, not here.
#
# Usage: pos_capture_evid_test.sh <pos-capture> <ppc_evid_mkfixtures> [<pos-capture-testhooks>]
# PACKETSHARK_UPDATE_EXPECTED=1 rewrites the expected files instead of
# comparing (for a reviewed change of output).
#
# Copyright (c) 2026 PocketOS authors.
# SPDX-License-Identifier: Apache-2.0
set -u
cd "$(dirname "$0")/.." || exit 1
HELPER=$(readlink -f "${1:?pos-capture}")
MKFIX=$(readlink -f "${2:?ppc_evid_mkfixtures}")
HOOKS=""
[ -n "${3:-}" ] && HOOKS=$(readlink -f "$3")
DATA=tests/data/packetshark
failed=0
check() { if [ "$2" = "1" ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi; }

T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
export POCKETOS_RUNTIME_DIR="$T/run" POCKETOS_LOG_DIR="$T/log" POCKETOS_LOG_STDERR=0
export POCKETOS_CAPTURES_DIR="$T/captures-none" # under /tmp: refused, nothing made (F1 hardening)
unset POCKETOS_CAPTURE_SOURCE POCKETOS_CAPTURE_SELF_MAC
mkdir -p "$T/run" "$T/log" "$T/fx"
SELF=02:00:5e:00:00:10

"$MKFIX" "$T/fx" > /dev/null
check "the fixtures are written" "$([ "$?" = "0" ] && [ "$(ls "$T"/fx/evid-*.pcap 2>/dev/null | wc -l)" = "5" ] && echo 1 || echo 0)"

# A fixture replayed to its end, then marked, then asked for its evidence.
# The wall clock of the mark is the one figure that is not the fixture's:
# it is the eighth field of ev.begin and the last of mark, cut to 0.
replay() { # <pcap> <out>
    { printf 'start file0 headers 0 all 0\n'; sleep 0.4; printf 'mark\nevid 1 0\n'; sleep 0.2; printf 'quit\n'; } |
        "$HELPER" session --source "file:$1" > "$2" 2>&1
}
evidence() { # <session output>: its mark and evidence lines, the wall clock cut out
    grep -P '^(mark|ev\.[a-z]+)\t' "$1" |
        awk -F'\t' 'BEGIN { OFS = "\t" } $1 == "mark" { $7 = 0 } $1 == "ev.begin" { $9 = 0 } { print }'
}

for f in "$T"/fx/evid-*.pcap; do
    name=$(basename "$f" .pcap)
    POCKETOS_CAPTURE_SELF_MAC=$SELF replay "$f" "$T/$name.out"
    evidence "$T/$name.out" > "$T/$name.ev"
    POCKETOS_CAPTURE_SELF_MAC=$SELF replay "$f" "$T/$name.out2"
    evidence "$T/$name.out2" > "$T/$name.ev2"
    if [ "${PACKETSHARK_UPDATE_EXPECTED:-0}" = "1" ]; then
        cp "$T/$name.ev" "$DATA/$name.expected"
        echo "note wrote $DATA/$name.expected"
        continue
    fi
    if cmp -s "$T/$name.ev" "$DATA/$name.expected"; then
        check "$name: the evidence is the expected one" 1
    else
        check "$name: the evidence is the expected one" 0
        diff "$DATA/$name.expected" "$T/$name.ev" | head -10
    fi
    check "$name: a second replay gives the same evidence, line for line" \
        "$([ -s "$T/$name.ev" ] && cmp -s "$T/$name.ev" "$T/$name.ev2" && echo 1 || echo 0)"
done

# What the lines say, beyond being the same every time. A replay's clock
# starts at its first frame, and its mark stands at its last, so the window
# before is the file's last 60 s. Fields, counted from the word:
#   ev.obs  2 side 3 kind 4 flags 5 t_ms 6 seq 7 alive 8 mac 9 ip 10 ip2 11 a 12 b
#           flags: 1 own, 2 broadcast, 4 reply expected here, 8 routine
#   ev.win  2 side 3 flags 4 span 5 watched 6 held 7 shed 8 lost 9 kept 10 more
#           11 DISCOVER 12 OFFER 13 REQUEST 14 ACK 15 NAK ... 21 PROBE 22 ANNOUNCE
obs() { grep -P "^ev\.obs\t" "$T/evid-$1.ev"; }
win0() { awk -F'\t' '$1 == "ev.win" && $2 == 0' "$T/evid-$1.ev"; }
check "dns-local: the query is this device's own with its reply expected here (kind 15, flags 5)" \
    "$(obs dns-local | awk -F'\t' '$3 == 15 && $4 == 5 && $6 == 3 && $7 == 2 && $9 == "192.0.2.53" && $10 == "192.0.2.16" && $11 == 4660 { ok = 1 } END { exit !ok }' &&
       echo 1 || echo 0)"
check "dns-local: the answer is paired, to this device, 31 ms after the query (kind 17, flags 1, 31000 us)" \
    "$(obs dns-local | awk -F'\t' '$3 == 17 && $4 == 1 && $6 == 4 && $12 == 31000 { ok = 1 } END { exit !ok }' && echo 1 || echo 0)"
check "dhcp-repeat: every DISCOVER is a broadcast with no reply expected here, and no OFFER or ACK is counted" \
    "$([ "$(obs dhcp-repeat | awk -F'\t' '$3 == 1' | wc -l)" -ge 3 ] &&
       obs dhcp-repeat | awk -F'\t' '$3 == 1 && $4 != 2 { bad = 1 } END { exit bad }' &&
       win0 dhcp-repeat | awk -F'\t' '$11 >= 3 && $12 == 0 && $14 == 0 { ok = 1 } END { exit !ok }' && echo 1 || echo 0)"
check "unseen-reply: DISCOVER and REQUEST seen, no OFFER or ACK counted, the probe and the announcement seen" \
    "$(win0 unseen-reply | awk -F'\t' '$11 == 1 && $12 == 0 && $13 == 1 && $14 == 0 && $21 == 1 && $22 == 1 && $9 == 4 { ok = 1 } END { exit !ok }' &&
       echo 1 || echo 0)"
check "loss-event: the announcements, the change, the two MACs and the NAK are all rows with their packets" \
    "$([ "$(obs loss-event | wc -l)" = "6" ] &&
       obs loss-event | awk -F'\t' '$3 == 14 && $6 == 3 && $8 == "02:00:5e:00:00:41" && $9 == "192.0.2.77" { ok = 1 } END { exit !ok }' &&
       obs loss-event | awk -F'\t' '$3 == 5 && $4 == 2 && $6 == 4 && $8 == "02:00:5e:00:00:44" && $10 == "192.0.2.1" { ok = 1 } END { exit !ok }' &&
       echo 1 || echo 0)"
check "every row's packet is still whole in the buffer (alive 2)" \
    "$(cat "$T"/evid-*.ev | awk -F'\t' '$1 == "ev.obs" && $7 != 2 { bad = 1 } END { exit bad }' && echo 1 || echo 0)"
check "a mark on a replay that has ended stands where it ended, its window after empty and cut (flags 2)" \
    "$(grep -qP '^mark\t1\t1\t88000\tstopped\t7\t0$' "$T/evid-dhcp-repeat.ev" &&
       grep -qP '^ev\.win\t1\t2\t0\t0\t0\t0\t0\t0\t0\t' "$T/evid-dhcp-repeat.ev" && echo 1 || echo 0)"

# Without this device's MAC a replayed file has no direction: nothing is
# then called its own, and no reply is said to be expected here.
replay "$T/fx/evid-dns-local.pcap" "$T/nodir.out"
evidence "$T/nodir.out" > "$T/nodir.ev"
check "without the MAC, direction is unknown: no observation is own or has its reply expected here" \
    "$([ "$(grep -cP '^ev\.obs\t' "$T/nodir.ev")" = "4" ] &&
       grep -P '^ev\.obs\t' "$T/nodir.ev" | awk -F'\t' '($4 % 2) == 1 || (int($4 / 4) % 2) == 1 { bad = 1 } END { exit bad }' &&
       echo 1 || echo 0)"

# ---- live, on the synthetic source ---------------------------------------------------
# 2,000 frames a second with DNS, DHCP and ARP exchanges among them.
{
    printf 'start synth0 headers 0 all 0\n'; sleep 2.5
    printf 'mark\n'; sleep 1.2
    printf 'evid 1 1\n'; sleep 1.3
    printf 'evid 2 1\n'; sleep 0.2
    printf 'hold 1\n'; sleep 2.2
    printf 'mark\n'; sleep 0.2
    printf 'evid 3 0\n'; sleep 0.2
    printf 'hold 0\n'; sleep 1.2
    printf 'stop\n'; sleep 0.3
    printf 'evid 4 1\nevid 5 2\nmark\nevid 6 3\nevid 7 9\n'; sleep 0.3
    printf 'window 8 tail 3 0\nquit\n'
} | "$HELPER" session --source synth:2000:1 > "$T/live" 2>&1

field() { awk -F'\t' -v w="$2" -v n="$3" -v k="$4" '$1 == w && (n == "" || $2 == n) { print $k }' "$1" | tail -1; }
ev_of() { awk -F'\t' -v r="$2" '$1 == "ev.begin" { on = ($2 == r) } on { print } $1 == "ev.end" { on = 0 }' "$1"; }
ev_of "$T/live" 1 > "$T/e1"; ev_of "$T/live" 2 > "$T/e2"; ev_of "$T/live" 3 > "$T/e3"
ev_of "$T/live" 4 > "$T/e4"; ev_of "$T/live" 5 > "$T/e5"; ev_of "$T/live" 6 > "$T/e6"
m1_at=$(awk -F'\t' '$1 == "mark" && $2 == 1 { print $4 }' "$T/live")
check "live: mark is answered with its id, time, state and newest packet" \
    "$(grep -qP '^mark\t1\t1\t\d+\tcapturing\t\d+\t\d+$' "$T/live" && [ "${m1_at:-0}" -ge 2000 ] && [ "$m1_at" -le 3500 ] &&
       echo 1 || echo 0)"
check "live: the capture goes on after a mark (packets keep arriving, the state does not change)" \
    "$([ "$(grep -cP '^state\tcapturing' "$T/live")" -ge 1 ] && [ "$(field "$T/live" stats "" 3)" -gt 9000 ] && echo 1 || echo 0)"
s1=$(awk -F'\t' '$1 == "ev.win" && $2 == 1 { print $4 }' "$T/e1"); s2=$(awk -F'\t' '$1 == "ev.win" && $2 == 1 { print $4 }' "$T/e2")
f1=$(awk -F'\t' '$1 == "ev.win" && $2 == 1 { print $3 }' "$T/e1")
check "live: the window after is open (flags 4) and grows between two answers (${s1:-?} ms, then ${s2:-?} ms)" \
    "$([ "${f1:-0}" = "4" ] && [ "${s1:-0}" -ge 900 ] && [ "${s2:-0}" -gt "${s1:-0}" ] && echo 1 || echo 0)"
b1=$(awk -F'\t' '$1 == "ev.win" && $2 == 0 { print $4 " " $3 }' "$T/e1")
check "live: the window before a mark 2.5 s after START is cut at START ($b1)" \
    "$([ "$(echo "$b1" | cut -d' ' -f2)" = "1" ] && [ "$(echo "$b1" | cut -d' ' -f1)" = "$m1_at" ] && echo 1 || echo 0)"
check "live: observations are counted and kept on both sides, each row with a packet number" \
    "$([ "$(grep -cP '^ev\.obs\t0\t' "$T/e2")" -ge 3 ] && [ "$(grep -cP '^ev\.obs\t1\t' "$T/e2")" -ge 3 ] &&
       grep -P '^ev\.obs\t' "$T/e2" | awk -F'\t' '$6 < 1 { bad = 1 } END { exit bad }' && echo 1 || echo 0)"
check "live: a mark on HOLD says so, and its window before tells the seconds held" \
    "$(grep -qP '^mark\t2\t1\t\d+\thold\t' "$T/live" && [ "$(awk -F'\t' '$1 == "ev.win" && $2 == 0 { print $6 }' "$T/e3")" -ge 2 ] &&
       echo 1 || echo 0)"
check "live: nothing is observed on HOLD: the window after the HOLD mark has no row from the held seconds" \
    "$([ "$(grep -cP '^ev\.obs\t1\t' "$T/e3")" = "0" ] && echo 1 || echo 0)"
check "live: STOP closes the windows: mark 1's is cut (flags 2), no longer open" \
    "$([ "$(awk -F'\t' '$1 == "ev.win" && $2 == 1 { print $3 }' "$T/e4")" = "2" ] && echo 1 || echo 0)"
check "live: mark 2's window after tells its held seconds too" \
    "$([ "$(awk -F'\t' '$1 == "ev.win" && $2 == 1 { print $6 }' "$T/e5")" -ge 1 ] && echo 1 || echo 0)"
stop_at=$(awk -F'\t' '$1 == "ev.begin" { print $10 }' "$T/e6")
check "live: a mark after STOP stands where the capture stopped, its window after empty and cut" \
    "$(grep -qP '^mark\t3\t1\t\d+\tstopped\t' "$T/live" && [ "$(awk -F'\t' '$1 == "mark" && $2 == 3 { print $4 }' "$T/live")" = "$stop_at" ] &&
       grep -qP '^ev\.win\t1\t2\t0\t' "$T/e6" && echo 1 || echo 0)"
check "live: a mark that is not held is refused, and the helper goes on answering" \
    "$(grep -qP '^error\tnomark\t' "$T/live" && grep -qP '^rows\.end\t8$' "$T/live" && echo 1 || echo 0)"
# A row's packet is one the inspector can open by its number. The synthetic
# source is deterministic, so a first session finds a row's packet and a
# second, the same, asks for it.
{
    printf 'start synth0 headers 0 all 0\n'; sleep 1.5
    printf 'stop\n'; sleep 0.2
    printf 'mark\nevid 1 0\n'; sleep 0.3
} | "$HELPER" session --source synth:2000:1 > "$T/pre" 2>&1
seq=$(grep -P '^ev\.obs\t0\t' "$T/pre" | tail -1 | cut -f6)
{
    printf 'start synth0 headers 0 all 0\n'; sleep 1.5
    printf 'stop\n'; sleep 0.2
    printf 'mark\nevid 1 0\n'; sleep 0.2
    printf 'inspect 2 %s 0\n' "${seq:-1}"; sleep 0.3
    printf 'quit\n'
} | "$HELPER" session --source synth:2000:1 > "$T/insp" 2>&1
check "an evidence row's packet opens in the inspector by its number (packet ${seq:-?})" \
    "$([ -n "$seq" ] && grep -qP "^pkt\.begin\t2\t$seq\t" "$T/insp" && grep -qP '^pkt\.end\t2$' "$T/insp" && echo 1 || echo 0)"

# Nine marks: the first is no longer held.
{
    printf 'start synth0 headers 0 all 0\n'; sleep 0.6
    for i in 1 2 3 4 5 6 7 8 9; do printf 'mark\n'; sleep 0.05; done
    printf 'evid 1 1\nevid 2 2\nevid 3 0\nquit\n'; sleep 0.2
} | "$HELPER" session --source synth:500:1 > "$T/nine" 2>&1
check "the ninth mark replaces the first: mark 9 says the oldest held is 2, and mark 1 is refused" \
    "$(grep -qP '^mark\t9\t2\t' "$T/nine" && grep -qP '^error\tnomark\t' "$T/nine" &&
       grep -qP '^ev\.begin\t2\t2\t2\t9\t' "$T/nine" && grep -qP '^ev\.begin\t3\t9\t2\t9\t' "$T/nine" && echo 1 || echo 0)"
{ printf 'mark\nevid 1 0\nquit\n'; } | "$HELPER" session --source synth:100:1 > "$T/idle" 2>&1
check "before START there is nothing to mark: both are refused, and nothing is captured" \
    "$([ "$(grep -cP '^error\tstate\t' "$T/idle")" = "2" ] && ! grep -qP '^state\tcapturing' "$T/idle" && echo 1 || echo 0)"

# ---- the review of PR #188 (2026-11-25) ------------------------------------------------------
# Fields of ev.begin: 2 req 3 id 4 first 5 last 6 at 7 state 8 seq 9 wall 10 now
#   11 filter 12 unanalysed (hex mask: 4 DNS, 8 DHCP, 10 ARP) 13 lost at the mark
#   (- not known) 14 from 15 to: the readings around it (- none)
# Fields of ev.win: 2 side 3 flags 4 span 5 watched 6 held 7 shed 8 lost; counts
#   DHCP 11-18, ARP 19-24, DNS 25-28; 29 lost near its far end 30 losses not
#   known since (- known).
win_sum() { # <evidence> <side> <from> <to>: the counts of fields from..to
    awk -F'\t' -v s="$2" -v a="$3" -v b="$4" '$1 == "ev.win" && $2 == s { for (i = a; i <= b; i++) n += $i } END { print n + 0 }' "$1"
}

# P1, shedding: 20,000 frames due at once 1.5 s in. The helper falls behind
# (50 full batches), only counts for a few milliseconds and recovers; the
# window before a mark at 3 s must say a second was counted only.
{
    printf 'start synth0 headers 0 all 0\n'; sleep 3.0
    printf 'mark\nevid 1 0\n'; sleep 0.3
    printf 'quit\n'
} | "$HELPER" session --source synth:2000:1:burst=20000,1500 > "$T/burst" 2>&1
ev_of "$T/burst" 1 > "$T/eb"
on=$(grep -nP '^note\tshed\t1$' "$T/burst" | head -1 | cut -d: -f1)
off=$(grep -nP '^note\tshed\t0$' "$T/burst" | head -1 | cut -d: -f1)
check "burst: the helper fell behind and recovered (note shed 1, then shed 0)" \
    "$([ -n "$on" ] && [ -n "$off" ] && [ "$off" -gt "$on" ] && echo 1 || echo 0)"
[ -n "$on" ] && [ -n "$off" ] &&
    echo "     $(sed -n "${on},${off}p" "$T/burst" | grep -cP '^stats\t') stats line(s), sent every 250 ms, fell inside the burst"
shed0=$(awk -F'\t' '$1 == "ev.win" && $2 == 0 { print $7 }' "$T/eb")
check "burst: the window before the mark tells a second counted only (${shed0:-?}), not every frame seen" \
    "$([ "${shed0:-0}" -ge 1 ] && echo 1 || echo 0)"

# P2, the wall clock of a mark set after STOP: the one of STOP, seconds
# later as minutes later (tests/ppc_evid_test.c sets one five minutes on).
{
    printf 'start synth0 headers 0 all 0\n'; sleep 1.2
    printf 'stop\n'; sleep 0.3
    date +%s > "$T/stop_at"; sleep 3.1; date +%s > "$T/mark_at"
    printf 'mark\nevid 1 0\n'; sleep 0.3
    printf 'quit\n'
} | "$HELPER" session --source synth:2000:1 > "$T/late" 2>&1
ev_of "$T/late" 1 > "$T/el"
wall=$(awk -F'\t' '$1 == "mark" { print $7 }' "$T/late")
stop_at=$(cat "$T/stop_at"); mark_at=$(cat "$T/mark_at")
check "after STOP: a mark set ${mark_at:-?}-${stop_at:-?} s later has the wall clock of STOP (${wall:-?}), on both lines" \
    "$([ -n "$wall" ] && [ "$wall" -ge $((stop_at - 2)) ] && [ "$wall" -le "$stop_at" ] && [ $((mark_at - wall)) -ge 3 ] &&
       [ "$(awk -F'\t' '$1 == "ev.begin" { print $9 }' "$T/el")" = "$wall" ] && echo 1 || echo 0)"
check "after STOP: and stands where the capture stopped on the session's clock" \
    "$(grep -qP '^mark\t1\t1\t\d+\tstopped\t' "$T/late" &&
       [ "$(awk -F'\t' '$1 == "mark" { print $4 }' "$T/late")" = "$(awk -F'\t' '$1 == "ev.begin" { print $10 }' "$T/el")" ] &&
       echo 1 || echo 0)"

# P1, the capture filter: only a live source applies one. The synthetic
# source asked for DNS AND DHCP passes ARP all the same: the evidence must
# not claim ARP was kept out, and counts it.
{
    printf 'start synth0 headers 0 names 0\n'; sleep 2.0
    printf 'mark\nevid 1 0\n'; sleep 0.3
    printf 'quit\n'
} | "$HELPER" session --source synth:2000:1 > "$T/names" 2>&1
ev_of "$T/names" 1 > "$T/en"
check "filter: asked of a source that applies none, it is reported as all (field 11), and ARP is counted" \
    "$([ "$(awk -F'\t' '$1 == "ev.begin" { print $11 }' "$T/en")" = "all" ] && [ "$(win_sum "$T/en" 0 19 24)" -gt 0 ] &&
       echo 1 || echo 0)"
check "filter: every analysis running is reported as such (field 12 is 0), with nothing lost at the mark (field 13)" \
    "$(awk -F'\t' '$1 == "ev.begin" && $12 == "0" && $13 == "0" { ok = 1 } END { exit !ok }' "$T/en" && echo 1 || echo 0)"

# P1, an analysis that could not start: the test-hooks helper runs without
# the DNS table, the network's tables, or both.
if [ -n "$HOOKS" ] && [ -x "$HOOKS" ]; then
    for no in dns net dns,net; do
        {
            printf 'start synth0 headers 0 all 0\n'; sleep 2.0
            printf 'mark\nevid 1 0\n'; sleep 0.3
            printf 'quit\n'
        } | POS_CAPTURE_TEST_NO_ANALYZER=$no "$HOOKS" session --source synth:2000:1 > "$T/no-$no" 2>&1
        ev_of "$T/no-$no" 1 > "$T/eno-$no"
    done
    mask() { awk -F'\t' '$1 == "ev.begin" { print $12 }' "$1"; }
    check "no DNS analysis: reported as 4 (DNS); no DNS observed; DHCP and ARP observed as ever" \
        "$([ "$(mask "$T/eno-dns")" = "4" ] && [ "$(win_sum "$T/eno-dns" 0 25 28)" = "0" ] &&
           [ "$(win_sum "$T/eno-dns" 0 11 24)" -gt 0 ] && echo 1 || echo 0)"
    check "no DHCP and ARP analysis: reported as 18 (DHCP, ARP); none of them observed; DNS observed" \
        "$([ "$(mask "$T/eno-net")" = "18" ] && [ "$(win_sum "$T/eno-net" 0 11 24)" = "0" ] &&
           [ "$(win_sum "$T/eno-net" 0 25 28)" -gt 0 ] && echo 1 || echo 0)"
    check "no analysis at all: reported as 1c (all three), nothing observed, and the capture and its mark go on" \
        "$([ "$(mask "$T/eno-dns,net")" = "1c" ] && [ "$(win_sum "$T/eno-dns,net" 0 11 28)" = "0" ] &&
           grep -qP '^mark\t1\t1\t' "$T/no-dns,net" && grep -qP '^ev\.end\t1$' "$T/no-dns,net" && echo 1 || echo 0)"
    {
        printf 'start synth0 headers 0 all 0\n'; sleep 1.5
        printf 'mark\nevid 1 0\n'; sleep 0.3
        printf 'quit\n'
    } | POS_CAPTURE_TEST_NO_ANALYZER=dns,net POS_CAPTURE_TEST_STATS_FAIL=0,0 "$HELPER" session --source synth:2000:1 \
        > "$T/no-shipped" 2>&1
    ev_of "$T/no-shipped" 1 > "$T/eno-shipped"
    check "the shipped helper has no such switches: the same environment changes nothing (field 12 is 0, losses known)" \
        "$([ "$(mask "$T/eno-shipped")" = "0" ] && [ "$(win_sum "$T/eno-shipped" 0 25 28)" -gt 0 ] &&
           awk -F'\t' '$1 == "ev.begin" && $13 == "0" && $14 == "-" { ok = 1 } END { exit !ok }' "$T/eno-shipped" &&
           [ "$(awk -F'\t' '$1 == "ev.win" && $30 != "-"' "$T/eno-shipped" | wc -l)" = "0" ] && echo 1 || echo 0)"

    # The follow-up review of PR #188 (2026-11-25): what is lost is known only
    # where the counters were read. The test-hooks helper fails the readings
    # from <from> ms after START to <to> ms, the ones after <lost> higher.
    # Those times are on the monotonic clock from START; the evidence's are
    # on the capture's own clock, from its first frame, a few milliseconds
    # later (more under the sanitizers): the bounds below allow 100 ms.
    hd() { awk -F'\t' -v k="$2" '$1 == "ev.begin" { print $k }' "$1"; }
    wn() { awk -F'\t' -v s="$2" -v k="$3" '$1 == "ev.win" && $2 == s { print $k }' "$1"; }
    # Failing over the mark, and back a second after it.
    {
        printf 'start synth0 headers 0 all 0\n'; sleep 2.0
        printf 'mark\nevid 1 0\n'; sleep 1.7
        printf 'evid 2 1\n'; sleep 0.3
        printf 'quit\n'
    } | POS_CAPTURE_TEST_STATS_FAIL=1000,3000,40 "$HOOKS" session --source synth:2000:1 > "$T/sf" 2>&1
    ev_of "$T/sf" 1 > "$T/esf1"; ev_of "$T/sf" 2 > "$T/esf2"
    sf_from=$(hd "$T/esf1" 14); sf_to=$(hd "$T/esf2" 15); sf_at=$(hd "$T/esf1" 6)
    check "stats failing at the mark: its losses are not known (field 13 -) since the last good reading (${sf_from:-?} ms)" \
        "$([ "$(hd "$T/esf1" 13)" = "-" ] && [ "${sf_from:-0}" -ge 400 ] && [ "$sf_from" -le 1000 ] &&
           [ "$(hd "$T/esf1" 15)" = "-" ] && grep -qP '^mark\t1\t1\t\d+\tcapturing\t' "$T/sf" && echo 1 || echo 0)"
    check "and neither window counts a loss or says its losses are known: both not known since then (field 30)" \
        "$([ "$(wn "$T/esf1" 0 30)" = "$sf_from" ] && [ "$(wn "$T/esf1" 1 30)" = "$sf_from" ] &&
           [ "$(wn "$T/esf1" 0 8)" = "0" ] && [ "$(wn "$T/esf1" 1 8)" = "0" ] && echo 1 || echo 0)"
    check "recovered a second later: the 40 are counted at the mark, between the readings (${sf_from:-?} < ${sf_at:-?} < ${sf_to:-?} ms)" \
        "$([ "$(hd "$T/esf2" 13)" = "40" ] && [ "$(hd "$T/esf2" 14)" = "$sf_from" ] && [ "${sf_at:-0}" -gt "$sf_from" ] &&
           [ "${sf_to:-0}" -gt "$sf_at" ] && [ "$sf_to" -ge 2900 ] && [ "$sf_to" -le 3700 ] && echo 1 || echo 0)"
    check "and in neither window: both count none, both now known" \
        "$([ "$(wn "$T/esf2" 0 8)" = "0" ] && [ "$(wn "$T/esf2" 1 8)" = "0" ] && [ "$(wn "$T/esf2" 0 30)" = "-" ] &&
           [ "$(wn "$T/esf2" 1 30)" = "-" ] && [ "$(wn "$T/esf2" 0 29)" = "0" ] && [ "$(wn "$T/esf2" 1 29)" = "0" ] &&
           echo 1 || echo 0)"
    # Three readings in a row failing, all before the mark.
    {
        printf 'start synth0 headers 0 all 0\n'; sleep 2.5
        printf 'mark\nevid 1 0\n'; sleep 0.3
        printf 'quit\n'
    } | POS_CAPTURE_TEST_STATS_FAIL=500,1200,25 "$HOOKS" session --source synth:2000:1 > "$T/sr" 2>&1
    ev_of "$T/sr" 1 > "$T/esr"
    check "repeated failures over before the mark: their 25 are the window before's, nothing at the mark, all known" \
        "$([ "$(hd "$T/esr" 13)" = "0" ] && [ "$(hd "$T/esr" 14)" = "-" ] && [ "$(wn "$T/esr" 0 8)" = "25" ] &&
           [ "$(wn "$T/esr" 1 8)" = "0" ] && [ "$(wn "$T/esr" 0 30)" = "-" ] && [ "$(wn "$T/esr" 1 30)" = "-" ] &&
           echo 1 || echo 0)"
    # STOP while the counters cannot be read: never known, nor later.
    {
        printf 'start synth0 headers 0 all 0\n'; sleep 1.8
        printf 'mark\n'; sleep 0.6
        printf 'stop\n'; sleep 0.3
        printf 'evid 1 1\n'; sleep 1.2
        printf 'mark\nevid 2 1\nevid 3 2\n'; sleep 0.3
        printf 'quit\n'
    } | POS_CAPTURE_TEST_STATS_FAIL=1000,0 "$HOOKS" session --source synth:2000:1 > "$T/ss" 2>&1
    ev_of "$T/ss" 1 > "$T/ess1"; ev_of "$T/ss" 2 > "$T/ess2"; ev_of "$T/ss" 3 > "$T/ess3"
    ss_from=$(hd "$T/ess1" 14)
    check "STOP while failing: the mark's losses stay not known (since ${ss_from:-?} ms), its window after cut and not known" \
        "$([ "$(hd "$T/ess1" 13)" = "-" ] && [ "${ss_from:-0}" -ge 400 ] && [ "$ss_from" -le 1000 ] &&
           [ "$(wn "$T/ess1" 1 3)" = "2" ] && [ "$(wn "$T/ess1" 1 30)" = "$ss_from" ] &&
           [ "$(wn "$T/ess1" 0 30)" = "$ss_from" ] && echo 1 || echo 0)"
    check "and so they stay after STOP: no reading is made, none is invented" \
        "$(diff <(grep -vP '^ev\.begin' "$T/ess1" | sed 's/^\(ev\.[a-z]*\)\t[0-9]*/\1/') \
                <(grep -vP '^ev\.begin' "$T/ess2" | sed 's/^\(ev\.[a-z]*\)\t[0-9]*/\1/') > /dev/null &&
           [ "$(hd "$T/ess2" 13)" = "-" ] && [ "$(hd "$T/ess2" 14)" = "$ss_from" ] && echo 1 || echo 0)"
    check "a mark set after STOP stands in the same spell: its losses are not known either" \
        "$(grep -qP '^mark\t2\t1\t\d+\tstopped\t' "$T/ss" && [ "$(hd "$T/ess3" 13)" = "-" ] &&
           [ "$(hd "$T/ess3" 14)" = "$ss_from" ] && [ "$(wn "$T/ess3" 0 30)" = "$ss_from" ] && echo 1 || echo 0)"
else
    echo "note: no pos-capture-testhooks given: the analyses that could not start are not tried here"
fi

check "the helper's log names no address or name the fixtures carry, and says a mark was set" \
    "$([ -s "$T/log/pos-capture.log" ] && ! grep -qE '192\.0\.2\.|02:00:5e|example\.net' "$T/log/pos-capture.log" &&
       grep -q 'capture: mark 1 at ' "$T/log/pos-capture.log" && echo 1 || echo 0)"

echo "pos_capture_evid_test: $failed failure(s)"
exit $((failed > 0))
