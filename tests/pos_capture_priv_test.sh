#!/bin/bash
# The capture helper's least privilege end to end (F1 hardening; ADR-016
# rule 17 amended), through the real helper as the shell drives it.
#
#   tests/pos_capture_priv_test.sh <pos-capture> <pos-capture-testhooks>
#
# As root (the unit's case; `sudo` or `wsl -u root` on a host):
#   - the helper says it switched identity, and /proc agrees while it
#     captures: uid and gid 65532 in every slot, no supplementary groups,
#     every capability set empty, no_new_privs, not dumpable;
#   - the descriptors it holds are its own: stdin, stdout, stderr, the lock,
#     the log, the capture source and the captures folder, nothing inherited
#     from whoever started it;
#   - MARK and its evidence, HOLD and STOP still work;
#   - SAVE writes one 0600 file of the identity into a 0700 folder of the
#     identity, no part file left, readable back; a capture already in the
#     folder stays root's;
#   - the helper still dies with its parent: the parent-death signal, which
#     the identity change clears, is set again;
#   - a failed privilege reduction fails closed: the capture is refused
#     with `error privilege`, no frame is counted, the lock is free again;
#   - the system call filter (amendment 3) is on while it captures: the
#     helper says so and /proc shows Seccomp 2, with or without root; a
#     forced failure of its check fails closed the same way; live, the
#     capture socket's counters are still read (the evidence knows its
#     losses).
# Without root, the helper has no identity to leave: it says so, captures
# and saves as before, and the root-only checks are skipped.
#
# Copyright (c) 2026 PocketOS authors.
# SPDX-License-Identifier: Apache-2.0
set -u
cd "$(dirname "$0")/.." || exit 1
HELPER=$(readlink -f "${1:?pos-capture}")
HOOKS=$(readlink -f "${2:?pos-capture-testhooks}")
failed=0
check() { if [ "$2" = "1" ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi; }
skip() { echo "skip $1"; }
ID=65532

T=$(mktemp -d)
# SAVE refuses /tmp: the captures folder lives under out/ (not on a shared
# Windows drive when run as root: it must accept chown).
SVP="$PWD/out/capture-priv-save-$$"
SV="$SVP/Captures"
OTHER="$PWD/out/capture-priv-other-$$"
cleanup() {
    [ -n "${HPID:-}" ] && kill "$HPID" 2>/dev/null
    rm -rf "$T" "$SVP" "$OTHER"
}
trap cleanup EXIT
mkdir -p "$T/run" "$T/log"
export POCKETOS_RUNTIME_DIR="$T/run" POCKETOS_LOG_DIR="$T/log" POCKETOS_LOG_STDERR=0
unset POCKETOS_CAPTURE_SOURCE
root=0
[ "$(id -u)" = "0" ] && root=1

# ---- a capture, MARK, HOLD, STOP and SAVE through the helper --------------------------------
mkdir -p "$SV"
if [ "$root" = "1" ]; then
    : > "$SV/capture-19990101-000000.pcap"
    chmod 0600 "$SV/capture-19990101-000000.pcap"
    chmod 0700 "$SV"
fi
mkfifo "$T/in"
if [ "$root" = "1" ]; then
    # a high inherited descriptor, past the 1024 a fixed loop would close
    ulimit -n 4096 2>/dev/null
    exec 1500< /dev/null 2>/dev/null
fi
POCKETOS_CAPTURES_DIR="$SV" "$HELPER" session --source synth:2000:1 < "$T/in" > "$T/out" 2>&1 &
HPID=$!
exec 1500<&- 2>/dev/null
exec 4> "$T/in"
printf 'start synth0 headers 0 all 0\n' >&4
for _ in $(seq 1 50); do grep -qP '^state\tcapturing' "$T/out" && break; sleep 0.1; done
sleep 0.5
st="/proc/$HPID/status"
if [ "$root" = "1" ]; then
    check "the helper says it switched to pos-capture" \
        "$(grep -qP "^note\tidentity\tswitched uid=$ID gid=$ID groups=0 netns=own$" "$T/out" && echo 1 || echo 0)"
    check "it has a network namespace of its own" \
        "$([ -n "$(readlink "/proc/$HPID/ns/net")" ] && [ "$(readlink "/proc/$HPID/ns/net")" != "$(readlink /proc/self/ns/net)" ] &&
           echo 1 || echo 0)"
    check "an inherited descriptor above 1023 did not survive" "$([ ! -e "/proc/$HPID/fd/1500" ] && echo 1 || echo 0)"
    check "/proc agrees while it captures: uid and gid $ID in every slot, no groups" \
        "$(grep -qP "^Uid:\t$ID\t$ID\t$ID\t$ID$" "$st" && grep -qP "^Gid:\t$ID\t$ID\t$ID\t$ID$" "$st" &&
           grep -qP '^Groups:\s*$' "$st" && echo 1 || echo 0)"
    check "every capability set is empty, no_new_privs is set" \
        "$(grep -qP '^CapInh:\t0+$' "$st" && grep -qP '^CapPrm:\t0+$' "$st" && grep -qP '^CapEff:\t0+$' "$st" &&
           grep -qP '^CapBnd:\t0+$' "$st" && grep -qP '^NoNewPrivs:\t1$' "$st" && echo 1 || echo 0)"
    # not dumpable: /proc/<pid>/ files of a non-dumpable process belong to root
    check "it is not dumpable (its /proc entries are root's)" \
        "$([ "$(stat -c %u "/proc/$HPID/environ")" = "0" ] && echo 1 || echo 0)"
    fds=$(ls -l "/proc/$HPID/fd" 2>/dev/null | awk 'NR > 1 { print $NF }' | sort | tr '\n' ' ')
    nfd=$(ls "/proc/$HPID/fd" | wc -l)
    check "it holds only its own descriptors: stdio, lock, log, source, captures folder ($nfd: $fds)" \
        "$([ "$nfd" -le 8 ] && echo "$fds" | grep -q capture.lock && echo "$fds" | grep -q pos-capture.log &&
           echo "$fds" | grep -q "$SV" && echo 1 || echo 0)"
else
    check "without root the helper says it had no identity to leave" \
        "$(grep -qP "^note\tidentity\tunprivileged uid=$(id -u) " "$T/out" && echo 1 || echo 0)"
    skip "the identity, capability and descriptor checks: run as root"
fi
check "capabilities dropped, as before" "$(grep -qP '^note\tcaps\tdropped' "$T/out" && echo 1 || echo 0)"
check "the system call filter is on: the helper says so, and /proc shows Seccomp 2 while it captures" \
    "$(grep -qP '^note\tseccomp\tfilter=2 receive-only=source$' "$T/out" && grep -qP '^Seccomp:\t2$' "$st" &&
       echo 1 || echo 0)"
printf 'mark\n' >&4
sleep 0.4
printf 'evid 7 0\n' >&4
sleep 0.4
printf 'hold 1\n' >&4
sleep 0.3
printf 'hold 0\n' >&4
sleep 0.3
printf 'stop\n' >&4
sleep 0.4
printf 'save\n' >&4
for _ in $(seq 1 60); do grep -qP '^saved\t(done|fail)\t' "$T/out" && break; sleep 0.1; done
check "MARK and its evidence work after the drop" \
    "$(grep -qP '^mark\t1\t' "$T/out" && grep -qP '^ev\.begin\t7\t1\t' "$T/out" && grep -qP '^ev\.end\t7$' "$T/out" &&
       echo 1 || echo 0)"
check "HOLD and STOP work" \
    "$(grep -P '^state\t' "$T/out" | cut -f2 | tr '\n' ' ' | grep -q 'capturing hold capturing stopped' && echo 1 || echo 0)"
name=$(grep -P '^saved\tdone\t' "$T/out" | head -1 | cut -f6)
check "SAVE writes its file ($name)" "$([ -n "$name" ] && [ -f "$SV/$name" ] && echo 1 || echo 0)"
check "the file is 0600 and the folder 0700, no part file left" \
    "$([ -n "$name" ] && [ "$(stat -c %a "$SV/$name")" = "600" ] && [ "$(stat -c %a "$SV")" = "700" ] &&
       [ -z "$(find "$SV" -name '*.part')" ] && echo 1 || echo 0)"
check "the file reads back" \
    "$([ -n "$name" ] && "$HELPER" decode "$SV/$name" | tail -1 | grep -qE '^# frames=[1-9]' && echo 1 || echo 0)"
if [ "$root" = "1" ]; then
    check "file and folder belong to pos-capture; the capture already there stays root's" \
        "$([ "$(stat -c %u:%g "$SV/$name")" = "$ID:$ID" ] && [ "$(stat -c %u:%g "$SV")" = "$ID:$ID" ] &&
           [ "$(stat -c %u "$SV/capture-19990101-000000.pcap")" = "0" ] && echo 1 || echo 0)"
fi
printf 'quit\n' >&4
exec 4>&-
wait "$HPID" 2>/dev/null
HPID=

# ---- the helper dies with its parent --------------------------------------------------------
if [ "$root" = "1" ]; then
    rm -f "$T/in2"
    mkfifo "$T/in2"
    # A parent of its own, which is killed; the helper's stdin stays open
    # through fd 5, so only the parent-death signal can end it.
    bash -c 'POCKETOS_CAPTURES_DIR="$1" "$2" session --source synth:500:1 < "$3" > "$4" 2>&1 & echo $! > "$5"; wait' \
        _ "$SV" "$HELPER" "$T/in2" "$T/out2" "$T/hpid" &
    PARENT=$!
    exec 5> "$T/in2"
    printf 'start synth0 headers 0 all 0\n' >&5
    for _ in $(seq 1 50); do grep -qP '^note\tidentity\tswitched' "$T/out2" 2>/dev/null && break; sleep 0.1; done
    child=$(cat "$T/hpid" 2>/dev/null)
    kill -9 "$PARENT"
    gone=0
    for _ in $(seq 1 30); do
        if ! kill -0 "$child" 2>/dev/null; then gone=1; break; fi
        sleep 0.1
    done
    check "the helper dies with its parent after the identity change (parent-death signal set again)" "$gone"
    exec 5>&-
    [ "$gone" = "0" ] && kill "$child" 2>/dev/null
else
    skip "the parent-death check: run as root"
fi

# ---- a failed reduction fails closed --------------------------------------------------------
{ printf 'start synth0 headers 0 all 0\n'; sleep 0.6; printf 'stats\nquit\n'; } 2>/dev/null |
    POS_CAPTURE_TEST_DROP_FAIL=1 POCKETOS_CAPTURES_DIR="$SV" "$HOOKS" session --source synth:2000:1 \
        > "$T/fail" 2>&1
rc=$?
check "a failed privilege reduction refuses the capture" \
    "$(grep -qP '^error\tprivilege\t' "$T/fail" && ! grep -qP '^state\tcapturing' "$T/fail" && echo 1 || echo 0)"
check "and the helper ends at once (exit 3), answering nothing more, so no frame is processed" \
    "$([ "$rc" = "3" ] && ! grep -qP '^stats\t' "$T/fail" && echo 1 || echo 0)"
check "the log says why, without a packet" \
    "$(grep -q 'privilege reduction failed' "$T/log/pos-capture.log" && echo 1 || echo 0)"
{ printf 'start synth0 headers 0 all 0\n'; sleep 0.3; printf 'quit\n'; } |
    POCKETOS_CAPTURES_DIR="$SV" "$HELPER" session --source synth:500:1 > "$T/after" 2>&1
check "the lock is free again: the next capture starts" \
    "$(grep -qP '^state\tcapturing' "$T/after" && echo 1 || echo 0)"
{ printf 'start synth0 headers 0 all 0\n'; sleep 0.6; printf 'stats\nquit\n'; } 2>/dev/null |
    POS_CAPTURE_TEST_DROP_FAIL=2 POCKETOS_CAPTURES_DIR="$SV" "$HOOKS" session --source synth:2000:1 \
        > "$T/ffail" 2>&1
rc=$?
check "a failed system call filter refuses the capture the same way (error privilege, exit 3, no frame)" \
    "$(grep -qP '^error\tprivilege\t' "$T/ffail" && ! grep -qP '^state\tcapturing' "$T/ffail" && [ "$rc" = "3" ] &&
       ! grep -qP '^stats\t' "$T/ffail" && grep -q 'system call filter failed' "$T/log/pos-capture.log" &&
       echo 1 || echo 0)"

# ---- the live capture path after the drop (optional) -----------------------------------------
# POS_CAPTURE_PRIV_LIVE=<interface>, as root, with a helper built with
# PACKETSHARK_PCAP=1: a real capture socket opened as root, then read as
# pos-capture in a network namespace of its own; its kernel counters read;
# promiscuous mode never set; a capture saved.
if [ "$root" = "1" ] && [ -n "${POS_CAPTURE_PRIV_LIVE:-}" ] && [ "$("$HELPER" backend)" = "pcap" ]; then
    IF=$POS_CAPTURE_PRIV_LIVE
    rm -rf "$SV"
    mkfifo "$T/in3"
    POCKETOS_CAPTURES_DIR="$SV" "$HELPER" session < "$T/in3" > "$T/live" 2>&1 &
    LPID=$!
    exec 6> "$T/in3"
    printf 'start %s headers 0 all 0\n' "$IF" >&6
    for _ in $(seq 1 50); do grep -qP '^state\tcapturing' "$T/live" && break; sleep 0.1; done
    flags0=$(cat "/sys/class/net/$IF/flags")
    lsec=$(grep -P '^Seccomp:' "/proc/$LPID/status" | cut -f2)
    ping -c 3 -W 1 "$(ip -4 route | awk '/default/ { print $3; exit }')" > /dev/null 2>&1
    sleep 2
    flags1=$(cat "/sys/class/net/$IF/flags")
    printf 'mark\n' >&6
    sleep 0.6
    printf 'evid 9 0\n' >&6
    sleep 0.4
    printf 'stop\n' >&6
    sleep 0.5
    printf 'save\n' >&6
    for _ in $(seq 1 60); do grep -qP '^saved\t(done|fail)\t' "$T/live" && break; sleep 0.1; done
    printf 'quit\n' >&6
    exec 6>&-
    wait "$LPID" 2>/dev/null
    seen=$(grep -P '^stats\t' "$T/live" | tail -1 | cut -f3)
    check "live on $IF: the capture switched identity and network namespace and kept reading frames ($seen)" \
        "$(grep -qP "^note\tidentity\tswitched uid=$ID gid=$ID groups=0 netns=own$" "$T/live" &&
           [ "${seen:-0}" -gt 0 ] && echo 1 || echo 0)"
    check "live: promiscuous mode was never set on $IF (flags $flags0, $flags1)" \
        "$(( (flags0 & 0x100) == 0 && (flags1 & 0x100) == 0 ))"
    check "live: the filter is on (Seccomp $lsec) and the socket's drop counters are read (the evidence knows its losses)" \
        "$([ "$lsec" = "2" ] && grep -qP '^ev\.begin\t9\t.*\t0\t-\t-$' "$T/live" &&
           ! grep -P '^ev\.win\t' "$T/live" | grep -qvP '\t-$' && echo 1 || echo 0)"
    lname=$(grep -P '^saved\tdone\t' "$T/live" | head -1 | cut -f6)
    check "live: the capture is saved as pos-capture and reads back ($lname)" \
        "$([ -n "$lname" ] && [ "$(stat -c %u "$SV/$lname")" = "$ID" ] &&
           "$HELPER" decode "$SV/$lname" | tail -1 | grep -qE '^# frames=[1-9]' && echo 1 || echo 0)"
    check "live: the helper stopped cleanly, no error" "$(! grep -qP '^error\t' "$T/live" && echo 1 || echo 0)"
else
    skip "the live capture path: set POS_CAPTURE_PRIV_LIVE=<interface> and run as root with a PACKETSHARK_PCAP=1 helper"
fi

# ---- a folder that is not a Captures folder is not handed over ------------------------------
if [ "$root" = "1" ]; then
    mkdir -p "$OTHER"
    { printf 'start synth0 headers 0 all 0\n'; sleep 0.4; printf 'stop\nsave\n'; sleep 0.5; printf 'quit\n'; } |
        POCKETOS_CAPTURES_DIR="$OTHER" "$HELPER" session --source synth:500:1 > "$T/other" 2>&1
    check "a root-owned folder not named Captures is refused (SAVE says nodir) and stays root's" \
        "$(grep -qP '^saved\tfail\t0\t\d+\t0\tnodir$' "$T/other" && [ "$(stat -c %u "$OTHER")" = "0" ] && echo 1 || echo 0)"
else
    skip "the folder hand-over check: run as root"
fi

echo "pos_capture_priv_test: $failed failure(s)"
exit $((failed > 0))
