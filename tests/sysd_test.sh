#!/bin/bash
# End-to-end test: sysd and pos over pocketipc against the real host.
# Run from the repository root after `make all`. The field-level checks are
# in tests/pocketsys_test.c (fake root); this proves the service, the CLI
# and the protocol around them.
set -u

SYSD=${SYSD:-services/sysd/sysd}
POS=${POS:-tools/pos/pos}
export POCKETOS_RUNTIME_DIR POCKETOS_LOG_DIR
POCKETOS_RUNTIME_DIR=$(mktemp -d)
POCKETOS_LOG_DIR=$(mktemp -d)
failed=0

check() { # check <name> <expected-regex> <actual>
    if printf '%s' "$3" | grep -q -- "$2"; then
        echo "ok   $1"
    else
        echo "FAIL $1: expected '$2' in:"; printf '%s\n' "$3" | head -20
        failed=$((failed + 1))
    fi
}
absent() { # absent <name> <unwanted-regex> <actual>
    if printf '%s' "$3" | grep -q -- "$2"; then
        echo "FAIL $1: did not expect '$2' in:"; printf '%s\n' "$3" | head -20
        failed=$((failed + 1))
    else
        echo "ok   $1"
    fi
}

# A service this process stands in for, as pos-supervise would record it.
echo $$ > "$POCKETOS_RUNTIME_DIR/radiod.pid"

"$SYSD" > "$POCKETOS_RUNTIME_DIR/sysd.out" 2>&1 &
SYSD_PID=$!
for _ in $(seq 1 50); do [ -S "$POCKETOS_RUNTIME_DIR/sysd.sock" ] && break; sleep 0.1; done
[ -S "$POCKETOS_RUNTIME_DIR/sysd.sock" ] || { echo "FAIL sysd did not start"; cat "$POCKETOS_RUNTIME_DIR/sysd.out"; exit 1; }

out=$("$POS" call sysd system.info)
check "info api_version" '"api_version":[[:space:]]*0' "$out"
EXPECTED_VERSION=$(cat VERSION)
EXPECTED_BUILD=$(cat BUILD_ID 2>/dev/null || git rev-parse --short HEAD 2>/dev/null || echo unknown)
check "info reports the version" "\"version\":[[:space:]]*\"${EXPECTED_VERSION}\"" "$out"
check "info reports the build" "\"build\":[[:space:]]*\"${EXPECTED_BUILD}\"" "$out"
check "info kernel" '"kernel":[[:space:]]*"' "$out"
check "info hostname" '"hostname":[[:space:]]*"' "$out"
# Present whatever this host carries: a string on a flashed card, null on a
# build machine with no /etc/pocketos-release.
check "info carries release_build" '"release_build":' "$out"

out=$("$POS" system status)
check "status uptime_s" '"uptime_s":[[:space:]]*[0-9]' "$out"
check "status load" '"load":[[:space:]]*\[' "$out"
check "status memory total_kb" '"total_kb":[[:space:]]*[0-9]' "$out"
check "status storage lists /" '"mount":[[:space:]]*"/"' "$out"
check "status network is an array" '"network":[[:space:]]*\[' "$out"
check "status power source" '"source":[[:space:]]*"' "$out"
check "status clock_set is a bool" '"clock_set":[[:space:]]*\(true\|false\)' "$out"
check "status lists the supervised radiod" '"name":[[:space:]]*"radiod"' "$out"
check "status radiod running (this shell holds the pid)" '"running":[[:space:]]*true' "$out"

# CPU utilisation needs two samples a second apart; it is null until then.
sleep 2.5
out=$("$POS" system status)
check "cpu_percent is a number after two samples" '"cpu_percent":[[:space:]]*[0-9]' "$out"

out=$("$POS" call sysd system.bogus 2>&1)
check "unknown method is code 1" 'code 1' "$out"
out=$("$POS" call sysd 2>&1)
check "pos call needs service and method" 'usage: pos call' "$out"
out=$("$POS" call nosuchd system.info 2>&1)
check "pos call reports an unreachable service" 'cannot connect to nosuchd' "$out"

# The fake root is a test-only build option (core/pocketsys.c,
# POCKETSYS_TEST_HOOKS): tests/pocketsys_test honours $POCKETSYS_ROOT, a
# production sysd must not. A service whose whole job is to report what the
# machine is must not be redirectable by whoever sets its environment.
FAKE=$(mktemp -d)
mkdir -p "$FAKE/proc/device-tree"
printf 'FAKE BOARD THAT DOES NOT EXIST\n' > "$FAKE/proc/device-tree/model"
POCKETSYS_ROOT=$FAKE "$SYSD" --socket-name sysdfake > "$POCKETOS_RUNTIME_DIR/fake.out" 2>&1 &
FAKE_PID=$!
for _ in $(seq 1 50); do [ -S "$POCKETOS_RUNTIME_DIR/sysdfake.sock" ] && break; sleep 0.1; done
out=$("$POS" call sysdfake system.info 2>&1)
check "sysd started with POCKETSYS_ROOT still answers" '"kernel":[[:space:]]*"' "$out"
absent "production sysd ignores POCKETSYS_ROOT" 'FAKE BOARD' "$out"
kill $FAKE_PID 2>/dev/null
wait $FAKE_PID 2>/dev/null
rm -rf "$FAKE"

# Protocol robustness: a garbage frame must not take sysd down.
python3 - "$POCKETOS_RUNTIME_DIR/sysd.sock" <<'PY' || true
import socket, sys
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect(sys.argv[1])
s.sendall(b"\x00\x00\x00\x03abc")
s.settimeout(1)
try:
    s.recv(64)
except Exception:
    pass
s.close()
PY
out=$("$POS" call sysd system.info 2>&1)
check "sysd survives an invalid JSON frame" '"kernel"' "$out"

kill $SYSD_PID
wait $SYSD_PID 2>/dev/null
rc=$?
check "sysd exits 0 on SIGTERM" '^0$' "$rc"
[ -S "$POCKETOS_RUNTIME_DIR/sysd.sock" ] && { echo "FAIL socket not removed on exit"; failed=$((failed + 1)); } || echo "ok   socket removed on exit"
check "sysd log names the build" "$EXPECTED_BUILD" "$(cat "$POCKETOS_LOG_DIR/sysd.log")"

"$SYSD" --bogus > "$POCKETOS_RUNTIME_DIR/bad.out" 2>&1; rc=$?
check "unknown option exits 2" '^2$' "$rc"
out=$("$POS" system status 2>&1)
check "pos system status without sysd says so" 'cannot connect to sysd' "$out"

rm -rf "$POCKETOS_RUNTIME_DIR" "$POCKETOS_LOG_DIR"
echo "sysd_test: $failed failure(s)"
exit $((failed > 0))
