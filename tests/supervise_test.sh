#!/bin/bash
# pos-supervise tests: crash-loop detection and clean stop on SIGTERM.
set -u
SUP=${SUP:-tools/supervise/pos-supervise}
export POCKETOS_RUNTIME_DIR POCKETOS_LOG_DIR
POCKETOS_RUNTIME_DIR=$(mktemp -d)
POCKETOS_LOG_DIR=$(mktemp -d)
failed=0
check() { if [ "$2" -eq 1 ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi; }

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

# 2. A long-running service: SIGTERM to the supervisor stops the child too.
sh "$SUP" steady sleep 100 >/dev/null 2>&1 &
SUPPID=$!
sleep 1
child=$(cat "$POCKETOS_RUNTIME_DIR/steady.pid" 2>/dev/null)
check "child pid recorded" $([ -n "$child" ] && kill -0 "$child" 2>/dev/null && echo 1 || echo 0)
kill -TERM $SUPPID
wait $SUPPID 2>/dev/null
sleep 0.5
check "child stopped on SIGTERM" $(kill -0 "$child" 2>/dev/null && echo 0 || echo 1)
check "steady pid file removed" $([ ! -f "$POCKETOS_RUNTIME_DIR/steady.pid" ] && echo 1 || echo 0)
check "no crash loop marker for steady" $([ ! -f "$POCKETOS_RUNTIME_DIR/steady.crashloop" ] && echo 1 || echo 0)

rm -rf "$POCKETOS_RUNTIME_DIR" "$POCKETOS_LOG_DIR"
echo "supervise_test: $failed failure(s)"
exit $((failed > 0))
