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

# A service this process stands in for, in the state file pos-supervise would
# have written for it (tools/supervise/pos-supervise). The pid file beside it
# is what the init scripts use to stop the daemon; sysd reads only the state.
#
# started_uptime_s has to be this shell's real start time, not a made-up
# number: sysd checks that the process holding child_pid is still the one the
# supervisor started, by comparing it with field 22 of /proc/<pid>/stat. Taken
# the same way the reader takes it - everything after the last ')' is field 3,
# so starttime is the twentieth - because comm may contain spaces and
# parentheses and cannot be counted past from the left.
shell_start_s() {
    _ticks=$(sed -e 's/^.*) //' "/proc/$$/stat" | cut -d' ' -f20)
    echo $(( _ticks / $(getconf CLK_TCK) ))
}
cat > "$POCKETOS_RUNTIME_DIR/radiod.state" <<EOF
state_version=1
name=radiod
supervisor_pid=$$
child_pid=$$
running=1
crashloop=0
last_exit_code=
restarts=0
backoff_s=
started_uptime_s=$(shell_start_s)
updated_uptime_s=$(shell_start_s)
EOF
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
# build machine with no /etc/doors-release or /etc/pocketos-release.
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
check "a service that has not exited has a null exit code" '"last_exit_code":[[:space:]]*null' "$out"
check "its restart count is a number" '"restarts":[[:space:]]*0' "$out"

# Writer to reader. Everything above uses a state file this script wrote, and
# tests/sysd_services_test.c uses ones it invents, so both halves of the
# contract are tested against fixtures and neither would notice a key renamed
# in pos-supervise. Here the real supervisor writes the file and the real sysd
# reads it. `entry` is the JSON object that follows the service's name.
SUPERVISE=${SUPERVISE:-tools/supervise/pos-supervise}
entry_of() { # <name> <status json>
    printf '%s' "$2" | tr -d ' \t\n' | sed "s/.*\"name\":\"$1\"//" | cut -c1-120
}
sh "$SUPERVISE" livesvc sleep 30 >/dev/null 2>&1 &
LIVE_SUP=$!
# Wait for the field this test actually reads, not merely for the file to
# exist. pos-supervise writes its state once before starting the child, with
# child_pid empty, and again once the child is up. Waiting on `-s` caught the
# first of those under load: child_pid came back empty, the expected string
# became "pid":, and the suite failed intermittently - which is what made the
# release gate unreliable rather than any behaviour of sysd.
for _ in $(seq 1 50); do
    LIVE_CHILD=$(sed -n 's/^child_pid=//p' "$POCKETOS_RUNTIME_DIR/livesvc.state" 2>/dev/null)
    [ -n "$LIVE_CHILD" ] && break
    sleep 0.1
done
entry=$(entry_of livesvc "$("$POS" system status)")
check "sysd lists a service a real pos-supervise is watching" '"pid"' "$entry"
check "sysd reports the pid the supervisor actually wrote" "\"pid\":${LIVE_CHILD}," "$entry"
check "sysd reports it running" '"running":true' "$entry"
check "sysd reports no crash loop" '"crashloop":false' "$entry"
check "sysd reports a null exit code before the first exit" '"last_exit_code":null' "$entry"
check "sysd reports the restart count" '"restarts":0' "$entry"
kill -TERM $LIVE_SUP 2>/dev/null
wait $LIVE_SUP 2>/dev/null
entry=$(entry_of livesvc "$("$POS" system status)")
check "a stopped service keeps its entry" '"name":"livesvc"' \
      "$(printf '%s' "$("$POS" system status)" | tr -d ' \t\n')"
check "a stopped service reports a null pid" '"pid":null' "$entry"
check "a stopped service reports not running" '"running":false' "$entry"
rm -f "$POCKETOS_RUNTIME_DIR/livesvc.state"

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

# ---- system.reboot and system.poweroff -----------------------------------
#
# Run against tests/sysd-testhooks, the only build whose power actions can be
# pointed at something harmless. The shipped sysd cannot: it is compiled
# without the hook and does not carry the variable names at all, which is
# checked below rather than by asking it to reboot this machine.
TESTHOOKS=${TESTHOOKS:-tests/sysd-testhooks}
ACT="$POCKETOS_RUNTIME_DIR/actions"
printf '%s\n' '#!/bin/sh' "echo reboot >> \"$ACT\"" > "$POCKETOS_RUNTIME_DIR/rec-reboot"
printf '%s\n' '#!/bin/sh' "echo poweroff >> \"$ACT\"" > "$POCKETOS_RUNTIME_DIR/rec-poweroff"
chmod 0755 "$POCKETOS_RUNTIME_DIR/rec-reboot" "$POCKETOS_RUNTIME_DIR/rec-poweroff"

power_start() { # <socket name>
    rm -f "$ACT"
    SYSD_REBOOT_COMMAND="$POCKETOS_RUNTIME_DIR/rec-reboot" \
    SYSD_POWEROFF_COMMAND="$POCKETOS_RUNTIME_DIR/rec-poweroff" \
        "$TESTHOOKS" --socket-name "$1" > "$POCKETOS_RUNTIME_DIR/$1.out" 2>&1 &
    POWER_PID=$!
    for _ in $(seq 1 50); do [ -S "$POCKETOS_RUNTIME_DIR/$1.sock" ] && break; sleep 0.1; done
}
power_stop() {
    kill $POWER_PID 2>/dev/null
    wait $POWER_PID 2>/dev/null
}
wait_action() { # up to 4 s for the action to run
    for _ in $(seq 1 40); do [ -s "$ACT" ] && return 0; sleep 0.1; done
    return 1
}
actions() { cat "$ACT" 2>/dev/null; }
action_count() { [ -e "$ACT" ] && wc -l < "$ACT" | tr -d ' ' || echo 0; }

# 1. A valid reboot: accepted, replied to before anything happens, then run
# once. The reply arriving while the action file is still absent is the whole
# reply-before-act guarantee, observed rather than assumed.
power_start pwr1
out=$("$POS" call pwr1 system.reboot 2>&1)
immediately=$([ -e "$ACT" ] && echo yes || echo no)
check "system.reboot is accepted" '"action":[[:space:]]*"reboot"' "$out"
check "the reply arrives before the action runs" '^no$' "$immediately"
check "the action then runs" '^reboot$' "$(wait_action && actions)"
sleep 1
check "the action ran exactly once" '^1$' "$(action_count)"
check "system.reboot chose the reboot command" '^reboot$' "$(actions)"
power_stop

# 2. The other action picks the other command.
power_start pwr2
out=$("$POS" call pwr2 system.poweroff 2>&1)
check "system.poweroff is accepted" '"action":[[:space:]]*"poweroff"' "$out"
check "system.poweroff chose the poweroff command" '^poweroff$' "$(wait_action && actions)"
sleep 1
check "the poweroff action ran exactly once" '^1$' "$(action_count)"
power_stop

# 3. One at a time: a second request while an action is pending is refused,
# and the refusal names what is already pending.
power_start pwr3
"$POS" call pwr3 system.reboot >/dev/null 2>&1
out=$("$POS" call pwr3 system.poweroff 2>&1)
check "poweroff while a reboot is pending is refused" 'code 5' "$out"
check "the refusal names the pending action" 'reboot already pending' "$out"
wait_action
sleep 1
check "the conflicting request added no second action" '^1$' "$(action_count)"
check "only the first action ran" '^reboot$' "$(actions)"
power_stop

# 4. The same the other way round.
power_start pwr4
"$POS" call pwr4 system.poweroff >/dev/null 2>&1
out=$("$POS" call pwr4 system.reboot 2>&1)
check "reboot while a poweroff is pending is refused" 'code 5' "$out"
check "the refusal names the pending poweroff" 'poweroff already pending' "$out"
wait_action
sleep 1
check "only the poweroff ran" '^poweroff$' "$(actions)"
check "and it ran once" '^1$' "$(action_count)"
power_stop

# 5. Requests that are not understood must never act: parameters on a method
# that takes none, and a method that does not exist.
power_start pwr5
out=$("$POS" call pwr5 system.reboot force=true 2>&1)
check "parameters on system.reboot are refused" 'code 2' "$out"
check "the refusal says why" 'takes no parameters' "$out"
out=$("$POS" call pwr5 system.bogus 2>&1)
check "an unknown method is still code 1" 'code 1' "$out"
out=$("$POS" call pwr5 system.poweroff spurious=1 2>&1)
check "parameters on system.poweroff are refused" 'code 2' "$out"
sleep 1
check "nothing that was refused ran" '^0$' "$(action_count)"
check "the service is still serving after refusing" '"kernel"' "$("$POS" call pwr5 system.info 2>&1)"
check "system.status is unaffected by the power methods" '"services"' \
      "$("$POS" call pwr5 system.status 2>&1)"
power_stop

# 6. A client that asks and immediately goes away has not been told the
# machine is about to reboot, so the machine must not reboot. The reply write
# fails with EPIPE, sysd drops the client, and no action is recorded.
power_start pwr6
python3 - "$POCKETOS_RUNTIME_DIR/pwr6.sock" <<'PY' || true
import json, socket, struct, sys
body = json.dumps({"id": 1, "method": "system.reboot"}).encode()
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect(sys.argv[1])
s.sendall(struct.pack(">I", len(body)) + body)
s.close()
PY
sleep 1
check "a request whose reply could not be delivered runs nothing" '^0$' "$(action_count)"
check "and the service is still up afterwards" '"kernel"' "$("$POS" call pwr6 system.info 2>&1)"
power_stop

# 7. A stop request outranks an action that has not started. The interesting
# case is not a signal early in the delay - the loop simply ends - but one
# that arrives in the very iteration the delay expires, where the pending
# check runs before the loop condition is tested again. SIGSTOP freezes sysd
# inside the delay, the delay then passes while it is frozen, and SIGTERM is
# delivered on SIGCONT: it resumes into exactly that iteration.
power_start pwr7
"$POS" call pwr7 system.reboot >/dev/null 2>&1
kill -STOP $POWER_PID 2>/dev/null
frozen_in_time=$([ -e "$ACT" ] && echo no || echo yes)
check "sysd was frozen before its delay expired" '^yes$' "$frozen_in_time"
sleep 1
kill -TERM $POWER_PID 2>/dev/null
kill -CONT $POWER_PID 2>/dev/null
wait $POWER_PID 2>/dev/null
sleep 1
check "a stop delivered as the delay expires drops the action" '^0$' "$(action_count)"

# 8. The hook is a build option, not an environment switch. The shipped
# binary is not asked to reboot this machine to prove it; it is checked for
# the only thing that could make it redirectable.
check "the shipped sysd carries no reboot override" '^0$' \
      "$(strings services/sysd/sysd | grep -c SYSD_REBOOT_COMMAND)"
check "the shipped sysd carries no poweroff override" '^0$' \
      "$(strings services/sysd/sysd | grep -c SYSD_POWEROFF_COMMAND)"
check "the shipped sysd carries the real command" '^1$' \
      "$(strings services/sysd/sysd | grep -c '^/sbin/reboot$')"
check "the test build does carry the override" '^1$' \
      "$(strings "$TESTHOOKS" | grep -c SYSD_REBOOT_COMMAND)"

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

# ---- system.logs / system.crashes (field-level checks: tests/sysd_logs_test.c) ----
out=$("$POS" call sysd system.logs 2>&1)
check "system.logs reads sysd's own log" '"source":[[:space:]]*"sysd"' "$out"
check "system.logs says the directory is there" '"available":[[:space:]]*true' "$out"
printf '2026-09-24T10:00:00.000Z radiod WARN  receive recovery failed: -5\n2026-09-24T10:00:01.000Z radiod ERROR state error\n' \
    > "$POCKETOS_LOG_DIR/radiod.log"
out=$("$POS" call sysd system.logs level=error source=radiod 2>&1)
check "system.logs filters by level and source" '"message":[[:space:]]*"state error"' "$out"
absent "and leaves the warning out" 'receive recovery' "$out"
out=$("$POS" call sysd system.logs level=loud 2>&1)
check "system.logs refuses an unknown level (code 2)" 'code 2' "$out"
out=$("$POS" call sysd system.logs limit=500 2>&1)
check "system.logs refuses a limit above the bound (code 2)" 'code 2' "$out"
out=$("$POS" call sysd system.crashes 2>&1)
check "system.crashes with none" '"total":[[:space:]]*0' "$out"
printf 'Doors crash report\nversion: 0.0.12\nbuild: abc\nprocess: radiod\npid: 7\nsignal: 11\nbacktrace:\n/usr/sbin/radiod(+0x10)[0x1]\n' \
    > "$POCKETOS_LOG_DIR/crash-radiod-1790000000-7.txt"
out=$("$POS" call sysd system.crashes 2>&1)
check "system.crashes lists a report" '"process":[[:space:]]*"radiod"' "$out"
check "with its signal" '"signal_name":[[:space:]]*"SIGSEGV"' "$out"
out=$("$POS" call sysd system.crashes x=1 2>&1)
check "system.crashes takes no params (code 2)" 'code 2' "$out"
out=$("$POS" call sysd system.status 2>&1)
check "system.status carries a bluetooth object" '"bluetooth"' "$out"
check "and the power source" '"source"' "$out"

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
