#!/bin/bash
# pos-supervise tests: crash-loop detection and clean stop on SIGTERM.
set -u
SUP=${SUP:-tools/supervise/pos-supervise}
export POCKETOS_RUNTIME_DIR POCKETOS_LOG_DIR
POCKETOS_RUNTIME_DIR=$(mktemp -d)
POCKETOS_LOG_DIR=$(mktemp -d)
failed=0
check() { if [ "$2" -eq 1 ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi; }
# One value out of a state file (tools/supervise/pos-supervise). An empty
# value and an absent key both come back empty, which is what "the supervisor
# does not know" means here.
state() { # <name> <key>
    sed -n "s/^$2=//p" "$POCKETOS_RUNTIME_DIR/$1.state" 2>/dev/null
}
no_temp_files() { # no half-written state left behind by any test above
    [ -z "$(ls "$POCKETOS_RUNTIME_DIR"/*.tmp.* 2>/dev/null)" ] && echo 1 || echo 0
}

# pos-supervise is POSIX sh and cannot link against core/pocketpaths.h, so it
# repeats two of the platform's directory defaults. They must not drift.
HDR=$(dirname "$0")/../core/pocketpaths.h
hdr_default() { sed -n "s/^#define $1 \"\\(.*\\)\"$/\\1/p" "$HDR"; }
sup_default() { sed -n "s/^[A-Z_]*=\\\${$1:-\\([^}]*\\)}$/\\1/p" "$SUP"; }
check "pos-supervise runtime default matches pocketpaths.h" \
      $([ -n "$(hdr_default POCKETOS_RUNTIME_DIR_DEFAULT)" ] &&
        [ "$(sup_default POCKETOS_RUNTIME_DIR)" = "$(hdr_default POCKETOS_RUNTIME_DIR_DEFAULT)" ] && echo 1 || echo 0)
check "pos-supervise log default matches pocketpaths.h" \
      $([ -n "$(hdr_default POCKETOS_LOG_DIR_DEFAULT)" ] &&
        [ "$(sup_default POCKETOS_LOG_DIR)" = "$(hdr_default POCKETOS_LOG_DIR_DEFAULT)" ] && echo 1 || echo 0)

# 1. A service that exits immediately must trip the crash-loop guard.
start=$(date +%s)
POS_SUPERVISE_MAX_RESTARTS=1 sh "$SUP" flaky sh -c 'exit 3' >/dev/null 2>&1
rc=$?
elapsed=$(( $(date +%s) - start ))
check "crash loop exits 1" $([ $rc -eq 1 ] && echo 1 || echo 0)
check "crash loop marker written" $([ -f "$POCKETOS_RUNTIME_DIR/flaky.crashloop" ] && echo 1 || echo 0)
check "crash loop detected quickly (<10s)" $([ $elapsed -lt 10 ] && echo 1 || echo 0)
check "supervise log mentions rc=3" $(grep -q 'rc=3' "$POCKETOS_LOG_DIR/supervise-flaky.log" && echo 1 || echo 0)
check "pid file removed" $([ ! -f "$POCKETOS_RUNTIME_DIR/flaky.pid" ] && echo 1 || echo 0)
# The state file is what sysd reads; the marker and the pid file stay for the
# init scripts and the bring-up checklist.
check "crash loop state file written" \
      $([ -f "$POCKETOS_RUNTIME_DIR/flaky.state" ] && echo 1 || echo 0)
check "state format version recorded" $([ "$(state flaky state_version)" = "1" ] && echo 1 || echo 0)
check "state names the service" $([ "$(state flaky name)" = "flaky" ] && echo 1 || echo 0)
check "state says crash loop" $([ "$(state flaky crashloop)" = "1" ] && echo 1 || echo 0)
check "state says not running" $([ "$(state flaky running)" = "0" ] && echo 1 || echo 0)
check "state has no child pid" $([ -z "$(state flaky child_pid)" ] && echo 1 || echo 0)
check "state records the last exit code" $([ "$(state flaky last_exit_code)" = "3" ] && echo 1 || echo 0)
check "state counts the restarts" $([ "$(state flaky restarts)" = "2" ] && echo 1 || echo 0)
check "no temp state file left behind" $(no_temp_files)

# 1b. The init script's supervisor pid file (POS_SUPERVISE_PIDFILE) goes with
# a crash loop. Nothing else is there to remove it, and left behind it names
# a pid that may be reused: the next start would refuse and the next stop
# would signal a stranger (v0.3.0 cold review item 2). The init scripts write
# it from $!, as here. The supervised command must not inherit the variable.
SUPFILE=$POCKETOS_RUNTIME_DIR/flaky2-supervise.pid
POS_SUPERVISE_PIDFILE=$SUPFILE POS_SUPERVISE_MAX_RESTARTS=1 \
    sh "$SUP" flaky2 sh -c "env > '$POCKETOS_RUNTIME_DIR/flaky2.env'; exit 3" >/dev/null 2>&1 &
p=$!
echo "$p" > "$SUPFILE"
wait "$p"
rc=$?
check "crash loop with a supervisor pid file still exits 1" $([ $rc -eq 1 ] && echo 1 || echo 0)
check "crash loop removes the supervisor pid file" $([ ! -e "$SUPFILE" ] && echo 1 || echo 0)
check "the supervised command does not inherit POS_SUPERVISE_PIDFILE" \
      $([ -s "$POCKETOS_RUNTIME_DIR/flaky2.env" ] &&
        ! grep -q '^POS_SUPERVISE_PIDFILE=' "$POCKETOS_RUNTIME_DIR/flaky2.env" && echo 1 || echo 0)
# A file that no longer names this supervisor - rewritten by a later start -
# is somebody else's and stays.
POS_SUPERVISE_PIDFILE=$SUPFILE POS_SUPERVISE_MAX_RESTARTS=1 sh "$SUP" flaky3 sh -c 'exit 3' >/dev/null 2>&1 &
p=$!
echo "999999" > "$SUPFILE"
wait "$p"
check "a supervisor pid file naming another pid is left alone" \
      $([ "$(cat "$SUPFILE" 2>/dev/null)" = "999999" ] && echo 1 || echo 0)
rm -f "$SUPFILE"

# 2. A long-running service: SIGTERM to the supervisor stops the child too.
POS_SUPERVISE_PIDFILE=$POCKETOS_RUNTIME_DIR/steady-supervise.pid sh "$SUP" steady sleep 100 >/dev/null 2>&1 &
SUPPID=$!
echo "$SUPPID" > "$POCKETOS_RUNTIME_DIR/steady-supervise.pid"
sleep 1
child=$(cat "$POCKETOS_RUNTIME_DIR/steady.pid" 2>/dev/null)
check "child pid recorded" $([ -n "$child" ] && kill -0 "$child" 2>/dev/null && echo 1 || echo 0)
check "state names the same child" $([ "$(state steady child_pid)" = "$child" ] && echo 1 || echo 0)
check "state names the supervisor" $([ "$(state steady supervisor_pid)" = "$SUPPID" ] && echo 1 || echo 0)
check "state says running" $([ "$(state steady running)" = "1" ] && echo 1 || echo 0)
check "state says no crash loop" $([ "$(state steady crashloop)" = "0" ] && echo 1 || echo 0)
check "state counts no restarts yet" $([ "$(state steady restarts)" = "0" ] && echo 1 || echo 0)
check "state has no exit code before the first exit" \
      $([ -z "$(state steady last_exit_code)" ] && echo 1 || echo 0)
check "state has no backoff while the child runs" \
      $([ -z "$(state steady backoff_s)" ] && echo 1 || echo 0)
check "state records when the child started" \
      $([ -n "$(state steady started_uptime_s)" ] && echo 1 || echo 0)
kill -TERM $SUPPID
wait $SUPPID 2>/dev/null
sleep 0.5
check "child stopped on SIGTERM" $(kill -0 "$child" 2>/dev/null && echo 0 || echo 1)
check "steady pid file removed" $([ ! -f "$POCKETOS_RUNTIME_DIR/steady.pid" ] && echo 1 || echo 0)
check "steady supervisor pid file removed on a clean stop" \
      $([ ! -f "$POCKETOS_RUNTIME_DIR/steady-supervise.pid" ] && echo 1 || echo 0)
check "no crash loop marker for steady" $([ ! -f "$POCKETOS_RUNTIME_DIR/steady.crashloop" ] && echo 1 || echo 0)
# The state file outlives the supervisor: a service that was stopped is a fact
# worth reporting, and /run is a tmpfs so it goes at the next boot anyway.
check "state file survives a clean stop" \
      $([ -f "$POCKETOS_RUNTIME_DIR/steady.state" ] && echo 1 || echo 0)
check "shutdown state says not running" $([ "$(state steady running)" = "0" ] && echo 1 || echo 0)
check "shutdown state clears the child pid" \
      $([ -z "$(state steady child_pid)" ] && echo 1 || echo 0)
check "shutdown state is not a crash loop" $([ "$(state steady crashloop)" = "0" ] && echo 1 || echo 0)

# 3. A service that takes time to leave (the shell releasing the panel on the
# K230): the supervisor must not be gone before the child is. Before this,
# the trapped SIGTERM returned from `wait` at once, the supervisor logged
# "stopped" and exited in tens of milliseconds with the child still
# shutting down, and the init script SIGKILLed the child it found still
# running (unit A, M6, 2026-09-08: "Stopping pocketos-shell: OK (forced)").
SLOW="$POCKETOS_RUNTIME_DIR/slow.sh"
printf '%s\n' '#!/bin/sh' 'trap "sleep 1; exit 0" TERM INT' 'while :; do sleep 0.2; done' > "$SLOW"
chmod 0755 "$SLOW"
sh "$SUP" slow "$SLOW" >/dev/null 2>&1 &
SUPPID=$!
sleep 1
child=$(cat "$POCKETOS_RUNTIME_DIR/slow.pid" 2>/dev/null)
check "slow child pid recorded" $([ -n "$child" ] && kill -0 "$child" 2>/dev/null && echo 1 || echo 0)
t0=$(date +%s%N)
kill -TERM $SUPPID
wait $SUPPID 2>/dev/null
waited_ms=$(( ($(date +%s%N) - t0) / 1000000 ))
check "supervisor outlives a child that takes time to leave (>=900 ms, got ${waited_ms} ms)" \
      $([ "$waited_ms" -ge 900 ] && echo 1 || echo 0)
check "child is gone the moment the supervisor is" $(kill -0 "$child" 2>/dev/null && echo 0 || echo 1)
check "slow stop is logged after the child left" \
      $(grep -q 'stopped' "$POCKETOS_LOG_DIR/supervise-slow.log" && echo 1 || echo 0)
check "slow pid file removed" $([ ! -f "$POCKETOS_RUNTIME_DIR/slow.pid" ] && echo 1 || echo 0)

# 4. A service that keeps dying, read continuously while it does. The state
# file is written at every transition, so this is where a reader would catch a
# half-written one if writes were not a temp file plus a rename. Every read has
# to be a whole file: the first line is the version and the last is the
# timestamp the writer puts there last.
BOUNCE="$POCKETOS_RUNTIME_DIR/bounce.sh"
printf '%s\n' '#!/bin/sh' 'sleep 0.2' 'exit 7' > "$BOUNCE"
chmod 0755 "$BOUNCE"
POS_SUPERVISE_MAX_RESTARTS=20 sh "$SUP" bouncy "$BOUNCE" >/dev/null 2>&1 &
SUPPID=$!
reads=0
partial=0
saw_backoff=0
saw_running=0
top_restarts=0
n=0
while [ $n -lt 60 ]; do
    n=$((n + 1))
    txt=$(cat "$POCKETOS_RUNTIME_DIR/bouncy.state" 2>/dev/null)
    if [ -n "$txt" ]; then
        reads=$((reads + 1))
        case "$(printf '%s\n' "$txt" | head -1)" in
        state_version=1) ;;
        *) partial=$((partial + 1)) ;;
        esac
        case "$(printf '%s\n' "$txt" | tail -1)" in
        updated_uptime_s=*) ;;
        *) partial=$((partial + 1)) ;;
        esac
        [ -n "$(state bouncy backoff_s)" ] && saw_backoff=1
        [ "$(state bouncy running)" = "1" ] && saw_running=1
        r=$(state bouncy restarts)
        [ -n "$r" ] && [ "$r" -gt "$top_restarts" ] && top_restarts=$r
    fi
    sleep 0.1
done
kill -TERM $SUPPID 2>/dev/null
wait $SUPPID 2>/dev/null
check "the state file was readable throughout ($reads reads)" $([ "$reads" -ge 20 ] && echo 1 || echo 0)
check "no partial state was ever observed" $([ "$partial" -eq 0 ] && echo 1 || echo 0)
check "a running child was observed" $([ "$saw_running" -eq 1 ] && echo 1 || echo 0)
check "a backoff was observed while waiting to restart" $([ "$saw_backoff" -eq 1 ] && echo 1 || echo 0)
check "the restart count advanced (reached $top_restarts)" $([ "$top_restarts" -ge 1 ] && echo 1 || echo 0)
check "the child's exit code was recorded" $([ "$(state bouncy last_exit_code)" = "7" ] && echo 1 || echo 0)
check "no crash loop while under the restart limit" \
      $([ "$(state bouncy crashloop)" = "0" ] && echo 1 || echo 0)
check "no temp state file left behind by the churn" $(no_temp_files)

# 5. The pid file names a live child or does not exist. It used to survive the
# child that wrote it and sit there for the whole backoff, up to 30 s, while
# the init scripts read it to decide what to signal; a pid reused in that
# window would have been SIGTERMed and then SIGKILLed in the daemon's place.
BOUNCE2="$POCKETOS_RUNTIME_DIR/bounce2.sh"
printf '%s\n' '#!/bin/sh' 'exit 9' > "$BOUNCE2"
chmod 0755 "$BOUNCE2"
POS_SUPERVISE_MAX_RESTARTS=20 sh "$SUP" pidgone "$BOUNCE2" >/dev/null 2>&1 &
SUPPID=$!
n=0
while [ -z "$(state pidgone backoff_s)" ] && [ $n -lt 60 ]; do n=$((n + 1)); sleep 0.1; done
check "the supervisor reached a backoff" $([ -n "$(state pidgone backoff_s)" ] && echo 1 || echo 0)
check "the pid file is absent during the backoff" \
      $([ ! -e "$POCKETOS_RUNTIME_DIR/pidgone.pid" ] && echo 1 || echo 0)
check "the state file agrees there is no child" \
      $([ -z "$(state pidgone child_pid)" ] && echo 1 || echo 0)
check "the state file still says not running" $([ "$(state pidgone running)" = "0" ] && echo 1 || echo 0)
check "the exit code of the child that died is kept" \
      $([ "$(state pidgone last_exit_code)" = "9" ] && echo 1 || echo 0)
kill -TERM $SUPPID 2>/dev/null
wait $SUPPID 2>/dev/null
check "the pid file is still absent after the stop" \
      $([ ! -e "$POCKETOS_RUNTIME_DIR/pidgone.pid" ] && echo 1 || echo 0)

# 6. SIGTERM during a backoff must stop the supervisor at once. The backoff is
# waited for with `wait` on a backgrounded sleep, which POSIX says a trapped
# signal ends immediately; a foreground `sleep 30` left the shell blocked, the
# init script escalated after 3 s, and the stop was reported "OK (forced)".
# setsid puts the supervisor in its own process group so that anything it
# leaves behind - an orphaned sleep above all - can be seen after it exits.
HAVE_SETSID=0
command -v setsid >/dev/null 2>&1 && HAVE_SETSID=1
launch() { # <name> — start the supervisor, echo the pid it recorded
    if [ "$HAVE_SETSID" -eq 1 ]; then
        setsid sh "$SUP" "$1" "$BOUNCE2" >/dev/null 2>&1 &
    else
        sh "$SUP" "$1" "$BOUNCE2" >/dev/null 2>&1 &
    fi
    _n=0
    while [ -z "$(state "$1" supervisor_pid)" ] && [ $_n -lt 60 ]; do _n=$((_n + 1)); sleep 0.1; done
    state "$1" supervisor_pid
}
wait_backoff() { # <name> <seconds> — wait until the recorded backoff reaches it
    _n=0
    while [ $_n -lt 200 ]; do
        _b=$(state "$1" backoff_s)
        [ -n "$_b" ] && [ "$_b" -ge "$2" ] && return 0
        _n=$((_n + 1)); sleep 0.1
    done
    return 1
}
stop_promptly() { # <name> <pid> — SIGTERM and report how many ms it took to go
    _t0=$(date +%s%N)
    kill -TERM "$2" 2>/dev/null
    _n=0
    while kill -0 "$2" 2>/dev/null && [ $_n -lt 100 ]; do _n=$((_n + 1)); sleep 0.1; done
    echo $(( ($(date +%s%N) - _t0) / 1000000 ))
}

for want in 4 8; do
    name="backoff$want"
    pid=$(launch "$name")
    if wait_backoff "$name" "$want"; then
        got=$(state "$name" backoff_s)
        check "reached a ${want}s backoff (recorded ${got}s)" 1
        ms=$(stop_promptly "$name" "$pid")
        check "SIGTERM during a ${got}s backoff stops in under 1 s (took ${ms} ms)" \
              $([ "$ms" -lt 1000 ] && echo 1 || echo 0)
        check "the supervisor is gone after a ${got}s backoff stop" \
              $(kill -0 "$pid" 2>/dev/null && echo 0 || echo 1)
        check "it wrote its shutdown state rather than being killed (${want}s)" \
              $([ "$(state "$name" running)" = "0" ] && [ -z "$(state "$name" child_pid)" ] && echo 1 || echo 0)
        check "no pid file left after the ${want}s backoff stop" \
              $([ ! -e "$POCKETOS_RUNTIME_DIR/$name.pid" ] && echo 1 || echo 0)
        if [ "$HAVE_SETSID" -eq 1 ]; then
            check "no orphan sleep or child left from the ${want}s backoff" \
                  $([ -z "$(pgrep -g "$pid" 2>/dev/null)" ] && echo 1 || echo 0)
        else
            echo "note: setsid missing; orphan check skipped for ${want}s"
        fi
    else
        check "reached a ${want}s backoff" 0
    fi
done
check "no temp state file left behind by the backoff stops" $(no_temp_files)

rm -rf "$POCKETOS_RUNTIME_DIR" "$POCKETOS_LOG_DIR"
echo "supervise_test: $failed failure(s)"
exit $((failed > 0))
