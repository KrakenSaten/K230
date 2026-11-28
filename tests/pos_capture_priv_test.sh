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
#     with `error privilege`, no frame is counted, the lock is free again.
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
SV="$PWD/out/capture-priv-save-$$"
cleanup() {
    [ -n "${HPID:-}" ] && kill "$HPID" 2>/dev/null
    rm -rf "$T" "$SV"
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
POCKETOS_CAPTURES_DIR="$SV" "$HELPER" session --source synth:2000:1 < "$T/in" > "$T/out" 2>&1 &
HPID=$!
exec 4> "$T/in"
printf 'start synth0 headers 0 all 0\n' >&4
for _ in $(seq 1 50); do grep -qP '^state\tcapturing' "$T/out" && break; sleep 0.1; done
sleep 0.5
st="/proc/$HPID/status"
if [ "$root" = "1" ]; then
    check "the helper says it switched to pos-capture" \
        "$(grep -qP "^note\tidentity\tswitched uid=$ID gid=$ID groups=0$" "$T/out" && echo 1 || echo 0)"
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
{ printf 'start synth0 headers 0 all 0\n'; sleep 0.6; printf 'stats\nquit\n'; } |
    POS_CAPTURE_TEST_DROP_FAIL=1 POCKETOS_CAPTURES_DIR="$SV" "$HOOKS" session --source synth:2000:1 \
        > "$T/fail" 2>&1
check "a failed privilege reduction refuses the capture" \
    "$(grep -qP '^error\tprivilege\t' "$T/fail" && ! grep -qP '^state\tcapturing' "$T/fail" && echo 1 || echo 0)"
check "and no frame was counted" \
    "$(grep -P '^stats\t' "$T/fail" | tail -1 | awk -F'\t' '{ print ($3 == 0) ? 1 : 0 }')"
check "the log says why, without a packet" \
    "$(grep -q 'privilege reduction failed' "$T/log/pos-capture.log" && echo 1 || echo 0)"
{ printf 'start synth0 headers 0 all 0\n'; sleep 0.3; printf 'quit\n'; } |
    POCKETOS_CAPTURES_DIR="$SV" "$HELPER" session --source synth:500:1 > "$T/after" 2>&1
check "the lock is free again: the next capture starts" \
    "$(grep -qP '^state\tcapturing' "$T/after" && echo 1 || echo 0)"

echo "pos_capture_priv_test: $failed failure(s)"
exit $((failed > 0))
