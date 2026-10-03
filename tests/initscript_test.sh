#!/bin/bash
# PocketOS init-script tests (M2).
#
# The boot path had no automated test: S60radiod and S90doors-shell were
# only ever checked with `sh -n`. This exercises their control flow against a
# fake root, plus the file modes that decide whether they run at all on the
# device.
#
# How the scripts are exercised: each one is copied and its absolute paths are
# rewritten to point into a temporary root. The rewrite is mechanical and is
# itself asserted (no un-rewritten /etc, /var, /usr, /run or /dev/dri path may
# survive), so what runs is the real control flow with different path
# constants. The supervisor is the real tools/supervise/pos-supervise; only
# the daemons are stubs, and they record their environment so the tests can
# check what the init script exported.
#
# File modes: Buildroot copies platforms/k230/rootfs_overlay with `rsync -a`,
# so the mode recorded in git is the mode in the image. The other scripts are
# installed with an explicit `install -m 0755` by the Makefile, so their modes
# are hygiene rather than correctness; both are checked, and the distinction
# is stated in the output. The check reads `git ls-files --stage`, not the
# filesystem: on a DrvFs (WSL /mnt/c) checkout every file reports as
# executable, so `test -x` there would pass no matter what git records.
#
# Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
set -u
cd "$(dirname "$0")/.." || exit 1
REPO=$(pwd)
OVERLAY=platforms/k230/rootfs_overlay
S50_SRC=$OVERLAY/etc/init.d/S50sysd
S55_SRC=$OVERLAY/etc/init.d/S55netd
S60_SRC=$OVERLAY/etc/init.d/S60radiod
S65_SRC=$OVERLAY/etc/init.d/S65meshcored
S90_SRC=$OVERLAY/etc/init.d/S90doors-shell
failed=0

check() { # <label> <0|1>
    if [ "$2" -eq 1 ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi
}
contains() { # <haystack> <needle>
    case "$1" in *"$2"*) echo 1 ;; *) echo 0 ;; esac
}

# ---- file modes ---------------------------------------------------------

# Prefer what git records, which is what a fresh clone and `git archive` will
# produce. Where git cannot answer (the synced Buildroot package tree has no
# .git), fall back to the filesystem, but only after proving the filesystem
# keeps modes at all: DrvFs reports every file as executable.
MODE_SOURCE=none
if git ls-files --stage -- "$S60_SRC" 2>/dev/null | grep -q .; then
    MODE_SOURCE=git
else
    # The probe must live on the filesystem holding the repository, not in
    # $TMPDIR: a WSL checkout under /mnt/c has its sources on DrvFs (every
    # file reports as executable) while /tmp is ext4, so probing /tmp would
    # answer for the wrong filesystem.
    probe=./.mode_probe.$$
    if : > "$probe" 2>/dev/null; then
        chmod 0644 "$probe"
        if [ ! -x "$probe" ]; then
            chmod 0755 "$probe"
            [ -x "$probe" ] && MODE_SOURCE=fs
        fi
        rm -f "$probe"
    fi
fi

mode_of() {
    case $MODE_SOURCE in
    git) git ls-files --stage -- "$1" 2>/dev/null | awk '{print $1}' ;;
    fs)  [ -x "$1" ] && echo 100755 || echo 100644 ;;
    esac
}

if [ "$MODE_SOURCE" = none ]; then
    echo "note: no git index and the filesystem does not keep modes; mode checks skipped"
else
    echo "note: file modes read from $MODE_SOURCE"
    # Image-critical: these modes are copied into the rootfs by Buildroot and
    # BusyBox rcS runs `$i start`, which needs the executable bit.
    for f in "$S50_SRC" "$S55_SRC" "$S60_SRC" "$S65_SRC" "$S90_SRC"; do
        check "image-critical: $(basename "$f") recorded 100755" \
              $([ "$(mode_of "$f")" = "100755" ] && echo 1 || echo 0)
    done
    # Overlay data files must NOT be executable.
    for f in $OVERLAY/etc/default/telnet $OVERLAY/etc/pocketos/settings.conf $OVERLAY/logo.xrgb; do
        check "image-critical: $(basename "$f") recorded 100644" \
              $([ "$(mode_of "$f")" = "100644" ] && echo 1 || echo 0)
    done
    # Hygiene: scripts meant to be run directly by a developer or operator.
    for f in tools/supervise/pos-supervise tools/hwcheck/hwcheck.sh \
             tools/design/gen_fonts.sh platforms/k230/scripts/apply_to_sdk.sh \
             platforms/k230/scripts/build_image.sh platforms/k230/scripts/deploy.sh \
             platforms/k230/scripts/deploy_unit.sh; do
        check "hygiene: $f recorded 100755" \
              $([ "$(mode_of "$f")" = "100755" ] && echo 1 || echo 0)
    done
    nonexec=""
    for f in tests/*.sh tests/hw/*.sh; do
        [ "$(mode_of "$f")" = "100755" ] || nonexec="$nonexec $f"
    done
    check "hygiene: every tests/*.sh recorded 100755" \
          $([ -z "$nonexec" ] && echo 1 || echo 0)
    [ -n "$nonexec" ] && echo "     not executable:$nonexec"
fi

# ---- fake root ----------------------------------------------------------

ROOT=$(mktemp -d)
# Kill anything this test started before removing its root, so a failure part
# way through cannot leave a supervisor and a stub daemon behind.
cleanup() {
    pkill -f "$ROOT/usr/bin/pos-supervise" 2>/dev/null
    sleep 0.3
    pkill -9 -f "$ROOT/usr/" 2>/dev/null
    rm -rf "$ROOT"
}
trap cleanup EXIT
mkdir -p "$ROOT/usr/bin" "$ROOT/usr/sbin" "$ROOT/var/run" "$ROOT/var/lib/pocketos/log" \
         "$ROOT/run/pocketos" "$ROOT/etc/default" "$ROOT/etc/init.d" "$ROOT/dev/dri" "$ROOT/root"
: > "$ROOT/dev/dri/card0"
export POCKETOS_RUNTIME_DIR="$ROOT/run/pocketos"

cp "$REPO/tools/supervise/pos-supervise" "$ROOT/usr/bin/pos-supervise"
chmod 0755 "$ROOT/usr/bin/pos-supervise"

# A well-behaved daemon: records the environment its init script gave it, then
# waits for SIGTERM. It deliberately does not exec: a process that execs sleep
# loses the argv the tests identify it by, and would then be counted as gone
# while it is still running.
make_daemon() { # <path> <envfile>
    cat > "$1" <<EOD
#!/bin/sh
env > "$2"
trap 'exit 0' TERM INT
while :; do sleep 0.2; done
EOD
    chmod 0755 "$1"
}
make_daemon "$ROOT/usr/sbin/radiod" "$ROOT/radiod.env"
make_daemon "$ROOT/usr/sbin/sysd" "$ROOT/sysd.env"
make_daemon "$ROOT/usr/bin/doors-shell" "$ROOT/shell.env"
# netd's stub also records its arguments: the interface comes from the init
# script, and that is what is being checked.
cat > "$ROOT/usr/sbin/netd" <<EOD
#!/bin/sh
env > "$ROOT/netd.env"
echo "\$*" > "$ROOT/netd.args"
trap 'exit 0' TERM INT
while :; do sleep 0.2; done
EOD
chmod 0755 "$ROOT/usr/sbin/netd"

# Rewrite the absolute paths of an init script into the fake root. /dev/null is
# deliberately left alone.
# The runtime directory is rewritten first and only where a separator (space
# or "=") precedes it: "/var/run/doors-shell-supervise.pid" contains the literal string
# "/run/pocketos", so an unanchored rule would rewrite the middle of the shell
# service's pid file path and the script would then write it nowhere.
rewrite() { # <source> <destination>
    sed -e "s#\([[:space:]=]\)/run/pocketos#\1$ROOT/run/pocketos#g" \
        -e "s#/usr/bin/#$ROOT/usr/bin/#g" \
        -e "s#/usr/sbin/#$ROOT/usr/sbin/#g" \
        -e "s#/var/run#$ROOT/var/run#g" \
        -e "s#/var/lib/pocketos#$ROOT/var/lib/pocketos#g" \
        -e "s#/etc/default#$ROOT/etc/default#g" \
        -e "s#/etc/init.d/#$ROOT/etc/init.d/#g" \
        -e "s#/dev/dri/card0#$ROOT/dev/dri/card0#g" \
        -e "s#HOME=/root #HOME=$ROOT/root #g" \
        "$1" > "$2"
    chmod 0755 "$2"
}
rewrite "$REPO/$S50_SRC" "$ROOT/etc/init.d/S50sysd"
rewrite "$REPO/$S55_SRC" "$ROOT/etc/init.d/S55netd"
rewrite "$REPO/$S60_SRC" "$ROOT/etc/init.d/S60radiod"
rewrite "$REPO/$S65_SRC" "$ROOT/etc/init.d/S65meshcored"
rewrite "$REPO/$S90_SRC" "$ROOT/etc/init.d/S90doors-shell"

# The rewrite must be complete: any surviving system path would make the test
# lie about what it exercised (or touch the host).
leaked=$(grep -nE '(^|[^A-Za-z0-9_/])/(etc|var|usr|run)/' "$ROOT/etc/init.d/S50sysd" \
                  "$ROOT/etc/init.d/S55netd" "$ROOT/etc/init.d/S60radiod" \
                  "$ROOT/etc/init.d/S65meshcored" \
                  "$ROOT/etc/init.d/S90doors-shell" | grep -v "$ROOT" | grep -v '^\s*#')
check "path rewrite left no system path behind" $([ -z "$leaked" ] && echo 1 || echo 0)
[ -n "$leaked" ] && echo "$leaked" | head -5

# A rule that matches inside an already-rewritten path produces "$ROOT/var$ROOT/run/..."
doubled=$(grep -n "$ROOT[^ ]*$ROOT" "$ROOT/etc/init.d/S50sysd" "$ROOT/etc/init.d/S55netd" \
               "$ROOT/etc/init.d/S60radiod" \
               "$ROOT/etc/init.d/S65meshcored" \
               "$ROOT/etc/init.d/S90doors-shell")
check "path rewrite did not nest one prefix inside another" $([ -z "$doubled" ] && echo 1 || echo 0)
[ -n "$doubled" ] && echo "$doubled" | head -5

S50="$ROOT/etc/init.d/S50sysd"
S60="$ROOT/etc/init.d/S60radiod"
S65="$ROOT/etc/init.d/S65meshcored"
S90="$ROOT/etc/init.d/S90doors-shell"

alive() { [ -n "${1:-}" ] && kill -0 "$1" 2>/dev/null; }
pidof_file() { [ -s "$1" ] && cat "$1" || echo ""; }
wait_for() { # <file> — up to 5 s
    n=0
    while [ ! -e "$1" ] && [ $n -lt 50 ]; do sleep 0.1; n=$((n + 1)); done
    [ -e "$1" ]
}
# The supervisor carries the daemon's path in its own argv, so a plain
# pgrep -f for the daemon matches both. Count the ones that are not the
# supervisor.
count_daemons() { pgrep -af "$ROOT/usr/sbin/radiod" 2>/dev/null | grep -vc 'pos-supervise'; }
count_sysd() { pgrep -af "$ROOT/usr/sbin/sysd" 2>/dev/null | grep -vc 'pos-supervise'; }
count_supervisors() { pgrep -af "$ROOT/usr/bin/pos-supervise" 2>/dev/null | wc -l; }
wait_gone() { # <pid> — up to 5 s
    n=0
    while [ -n "${1:-}" ] && kill -0 "$1" 2>/dev/null && [ $n -lt 50 ]; do sleep 0.1; n=$((n + 1)); done
    kill -0 "${1:-}" 2>/dev/null && return 1 || return 0
}
wait_daemons() { # <expected count> — up to 5 s
    n=0
    while [ "$(count_daemons)" -ne "$1" ] && [ $n -lt 50 ]; do sleep 0.1; n=$((n + 1)); done
    [ "$(count_daemons)" -eq "$1" ]
}
wait_sysd() { # <expected count> — up to 5 s
    n=0
    while [ "$(count_sysd)" -ne "$1" ] && [ $n -lt 50 ]; do sleep 0.1; n=$((n + 1)); done
    [ "$(count_sysd)" -eq "$1" ]
}

# ---- S60radiod ----------------------------------------------------------

out=$("$S60" start 2>&1)
check "S60 start reports OK" $(contains "$out" "OK")
check "S60 start names backend, region and power" $(contains "$out" "(mock, EU868, 2 dBm)")
SUPPID=$(pidof_file "$ROOT/var/run/radiod-supervise.pid")
check "S60 start writes the supervise pid file" $([ -n "$SUPPID" ] && echo 1 || echo 0)
check "S60 start leaves the supervisor running" $(alive "$SUPPID" && echo 1 || echo 0)
check "S60 start starts the daemon" $(wait_for "$ROOT/radiod.env" && echo 1 || echo 0)
check "S60 start records the daemon pid in the runtime dir" \
      $([ -s "$ROOT/run/pocketos/radiod.pid" ] && echo 1 || echo 0)
check "S60 exports the persistent log directory" \
      $(grep -q "^POCKETOS_LOG_DIR=$ROOT/var/lib/pocketos/log$" "$ROOT/radiod.env" && echo 1 || echo 0)
check "S60 exports POCKETOS_LOG_STDERR=0" \
      $(grep -q '^POCKETOS_LOG_STDERR=0$' "$ROOT/radiod.env" && echo 1 || echo 0)
check "S60 creates the stdio log" $([ -f "$ROOT/var/lib/pocketos/log/radiod.stdio.log" ] && echo 1 || echo 0)

out=$("$S60" start 2>&1)
check "S60 start is idempotent" $(contains "$out" "already running")
check "S60 does not start a second daemon" $(wait_daemons 1 && echo 1 || echo 0)

out=$("$S60" stop 2>&1)
check "S60 stop reports OK" $(contains "$out" "OK")
check "S60 stop removes the supervise pid file" \
      $([ ! -f "$ROOT/var/run/radiod-supervise.pid" ] && echo 1 || echo 0)
check "S60 stop ends the supervisor" $(wait_gone "$SUPPID" && echo 1 || echo 0)
check "S60 stop ends the daemon" $(wait_daemons 0 && echo 1 || echo 0)

out=$("$S60" stop 2>&1)
check "S60 stop when not running says so" $(contains "$out" "not running")

# The stdio log of the previous run is kept as .1 when the next one starts.
rm -f "$ROOT/radiod.env"
printf 'previous boot\n' > "$ROOT/var/lib/pocketos/log/radiod.stdio.log"
printf 'RADIOD_BACKEND=sx1262\nRADIOD_TX_POWER_DBM=7\n' > "$ROOT/etc/default/radiod"
out=$("$S60" start 2>&1)
check "S60 honours /etc/default/radiod" $(contains "$out" "(sx1262, EU868, 7 dBm)")
check "S60 rotates the previous stdio log to .1" \
      $(grep -q 'previous boot' "$ROOT/var/lib/pocketos/log/radiod.stdio.log.1" 2>/dev/null && echo 1 || echo 0)
"$S60" stop >/dev/null 2>&1
wait_daemons 0
rm -f "$ROOT/etc/default/radiod" "$ROOT/radiod.env"

out=$("$S60" restart 2>&1)
check "S60 restart starts the service" $(wait_for "$ROOT/radiod.env" && echo 1 || echo 0)
check "S60 restart leaves exactly one daemon" $(wait_daemons 1 && echo 1 || echo 0)
check "S60 restart leaves exactly one supervisor" \
      $([ "$(count_supervisors)" -eq 1 ] && echo 1 || echo 0)
"$S60" stop >/dev/null 2>&1
wait_daemons 0

# A daemon that takes a second to leave after SIGTERM, as the shell does on
# the K230 while it releases the panel. stop must report a plain OK: the
# supervisor stays until the daemon is gone, so the init script never finds
# a daemon "still running" that is merely on its way out. Before the
# supervisor waited, this produced "OK (forced)" and a SIGKILL (unit A, M6).
cat > "$ROOT/usr/sbin/radiod" <<EOD
#!/bin/sh
trap 'sleep 1; exit 0' TERM INT
env > "$ROOT/radiod.env"
while :; do sleep 0.2; done
EOD
chmod 0755 "$ROOT/usr/sbin/radiod"
rm -f "$ROOT/radiod.env"
"$S60" start >/dev/null 2>&1
wait_for "$ROOT/radiod.env"
wait_daemons 1
started=$(date +%s)
out=$("$S60" stop 2>&1)
elapsed=$(( $(date +%s) - started ))
check "S60 stop reports OK for a daemon that takes time to leave" $(contains "$out" "OK")
check "S60 stop does not force a daemon that is leaving" $([ "$(contains "$out" "forced")" -eq 0 ] && echo 1 || echo 0)
check "S60 stop of a slow daemon leaves nothing behind" \
      $([ "$(count_daemons)" -eq 0 ] && [ "$(count_supervisors)" -eq 0 ] && echo 1 || echo 0)
check "S60 stop of a slow daemon is prompt" $([ "$elapsed" -le 5 ] && echo 1 || echo 0)

# A daemon that ignores SIGTERM. The supervisor waits for it, so it never
# leaves either; the init script must notice, escalate to the daemon itself,
# and not return until nothing is left (the next start would otherwise be the
# second one, still holding whatever the first owns).
cat > "$ROOT/usr/sbin/radiod" <<EOD
#!/bin/sh
trap '' TERM INT
env > "$ROOT/radiod.env"
while :; do sleep 0.2; done
EOD
chmod 0755 "$ROOT/usr/sbin/radiod"
rm -f "$ROOT/radiod.env"
"$S60" start >/dev/null 2>&1
wait_for "$ROOT/radiod.env"
wait_daemons 1
started=$(date +%s)
out=$("$S60" stop 2>&1)
elapsed=$(( $(date +%s) - started ))
check "S60 stop returns when the daemon ignores SIGTERM" $(contains "$out" "OK")
check "S60 stop reports that it had to force" $(contains "$out" "forced")
check "S60 stop leaves no daemon behind" $([ "$(count_daemons)" -eq 0 ] && echo 1 || echo 0)
check "S60 stop leaves no supervisor behind" $([ "$(count_supervisors)" -eq 0 ] && echo 1 || echo 0)
check "S60 stop is bounded" $([ "$elapsed" -le 20 ] && echo 1 || echo 0)
make_daemon "$ROOT/usr/sbin/radiod" "$ROOT/radiod.env"
rm -f "$ROOT/radiod.env"

# ---- S65meshcored ------------------------------------------------------
#
# Until 2026-09-22 this script had no test at all, and unit A showed what that
# cost: meshcored installed by hand, no init script, nothing at boot. What is
# exercised here is the whole lifecycle through the real pos-supervise - the
# opt-in, start, the refusals, stop, restart, stale pid files, a meshcored no
# init script started, a crash and a crash loop.

# A well-behaved stand-in that also records its arguments, which carry the
# per-unit settings.
make_meshcored() {
    cat > "$ROOT/usr/sbin/meshcored" <<EOD
#!/bin/sh
env > "$ROOT/meshcored.env"
echo "\$*" > "$ROOT/meshcored.args"
trap 'exit 0' TERM INT
while :; do sleep 0.2; done
EOD
    chmod 0755 "$ROOT/usr/sbin/meshcored"
}
make_meshcored
count_mcd() { pgrep -af "$ROOT/usr/sbin/meshcored" 2>/dev/null | grep -vc 'pos-supervise'; }
wait_mcd() { # <expected count> - up to 5 s
    n=0
    while [ "$(count_mcd)" -ne "$1" ] && [ $n -lt 50 ]; do sleep 0.1; n=$((n + 1)); done
    [ "$(count_mcd)" -eq "$1" ]
}
MCD_SUP_PIDFILE="$ROOT/var/run/meshcored-supervise.pid"
MCD_STATE="$ROOT/run/pocketos/meshcored.state"
mcd_state() { sed -n "s/^$1=//p" "$MCD_STATE" 2>/dev/null; }

out=$("$S65" start 2>&1); rc=$?
check "S65 is disabled by default" $(contains "$out" "disabled (set MESHCORED_ENABLE=1")
check "S65 disabled start exits 0 (nothing was asked of it)" $([ "$rc" -eq 0 ] && echo 1 || echo 0)
check "S65 disabled starts nothing" \
      $([ ! -e "$ROOT/meshcored.env" ] && [ ! -s "$MCD_SUP_PIDFILE" ] && echo 1 || echo 0)

printf 'MESHCORED_ENABLE=1\n' > "$ROOT/etc/default/meshcored"
out=$("$S65" start 2>&1); rc=$?
check "S65 enabled start reports OK" $(contains "$out" "OK")
check "S65 enabled start exits 0" $([ "$rc" -eq 0 ] && echo 1 || echo 0)
SUPPID=$(pidof_file "$MCD_SUP_PIDFILE")
check "S65 start leaves a supervisor running" $(alive "$SUPPID" && echo 1 || echo 0)
check "S65 start starts meshcored" $(wait_for "$ROOT/meshcored.env" && echo 1 || echo 0)
check "S65 start runs exactly one meshcored" $(wait_mcd 1 && echo 1 || echo 0)
check "S65 start records the daemon pid for stop" \
      $([ -s "$ROOT/run/pocketos/meshcored.pid" ] && echo 1 || echo 0)
check "S65 status is visible: the supervisor's state file says running" \
      $([ "$(mcd_state running)" = "1" ] && [ "$(mcd_state crashloop)" = "0" ] && echo 1 || echo 0)
check "S65 passes the bench-safe transmit power" \
      $(grep -q -- '--tx-power-dbm 2' "$ROOT/meshcored.args" && echo 1 || echo 0)
check "S65 passes no --name when none is set (the stored one is kept)" \
      $(grep -q -- '--name' "$ROOT/meshcored.args" && echo 0 || echo 1)
check "S65 exports the persistent log directory" \
      $(grep -q "^POCKETOS_LOG_DIR=$ROOT/var/lib/pocketos/log$" "$ROOT/meshcored.env" && echo 1 || echo 0)
check "S65 exports POCKETOS_LOG_STDERR=0" \
      $(grep -q '^POCKETOS_LOG_STDERR=0$' "$ROOT/meshcored.env" && echo 1 || echo 0)
check "S65 creates the state directory 0700 (it holds a private key)" \
      $([ "$(stat -c %a "$ROOT/var/lib/pocketos/meshcored" 2>/dev/null)" = "700" ] && echo 1 || echo 0)

out=$("$S65" start 2>&1)
check "S65 start is idempotent" $(contains "$out" "already running")
check "S65 does not start a second meshcored" $(wait_mcd 1 && echo 1 || echo 0)

out=$("$S65" stop 2>&1); rc=$?
check "S65 stop reports OK" $(contains "$out" "OK")
check "S65 stop exits 0" $([ "$rc" -eq 0 ] && echo 1 || echo 0)
check "S65 stop ends the supervisor" $(wait_gone "$SUPPID" && echo 1 || echo 0)
check "S65 stop ends meshcored" $(wait_mcd 0 && echo 1 || echo 0)
check "S65 stop leaves no supervise pid file" $([ ! -e "$MCD_SUP_PIDFILE" ] && echo 1 || echo 0)
check "S65 stop leaves no daemon pid file" $([ ! -e "$ROOT/run/pocketos/meshcored.pid" ] && echo 1 || echo 0)
check "S65 stop leaves a state file that says stopped, not running" \
      $([ "$(mcd_state running)" = "0" ] && [ -z "$(mcd_state child_pid)" ] && echo 1 || echo 0)

out=$("$S65" stop 2>&1); rc=$?
check "S65 stop when stopped says not running" $(contains "$out" "not running")
check "S65 repeated stop is safe (exit 0)" $([ "$rc" -eq 0 ] && echo 1 || echo 0)

rm -f "$ROOT/meshcored.env"
out=$("$S65" restart 2>&1)
check "S65 restart starts the service" $(wait_for "$ROOT/meshcored.env" && echo 1 || echo 0)
check "S65 restart leaves exactly one meshcored" $(wait_mcd 1 && echo 1 || echo 0)
out=$("$S65" restart 2>&1)
check "S65 restart of a running service leaves exactly one meshcored" $(wait_mcd 1 && echo 1 || echo 0)
check "and exactly one supervisor for it" \
      $([ "$(pgrep -af "pos-supervise meshcored" 2>/dev/null | grep -c "$ROOT")" -eq 1 ] && echo 1 || echo 0)
"$S65" stop >/dev/null 2>&1
wait_mcd 0

# Stale pid files from a previous boot or a killed supervisor: pids that name
# nothing. They must neither block the start nor be mistaken for a service.
# The pid of a process that has already exited, taken without signalling
# anything: a background job killed before it has exec'd is still this shell,
# and would run this script's EXIT trap - which removes $ROOT.
dead=$(sh -c 'echo $$')
echo "$dead" > "$MCD_SUP_PIDFILE"; echo "$dead" > "$ROOT/run/pocketos/meshcored.pid"
out=$("$S65" stop 2>&1)
check "S65 stop with only stale pid files says not running" $(contains "$out" "not running")
check "and removes both, so a reused pid is never taken for meshcored" \
      $([ ! -e "$MCD_SUP_PIDFILE" ] && [ ! -e "$ROOT/run/pocketos/meshcored.pid" ] && echo 1 || echo 0)
echo "$dead" > "$MCD_SUP_PIDFILE"; echo "$dead" > "$ROOT/run/pocketos/meshcored.pid"
rm -f "$ROOT/meshcored.env"
out=$("$S65" start 2>&1)
check "S65 start is not blocked by stale pid files" $(contains "$out" "OK")
check "and starts exactly one meshcored" $(wait_mcd 1 && echo 1 || echo 0)
"$S65" stop >/dev/null 2>&1
wait_mcd 0

# An orphan: the supervisor killed outright, its daemon still running. stop
# must ask the daemon to leave (meshcored writes its node table on SIGTERM)
# rather than SIGKILL it, and leave no child pid file behind for a reused pid.
cat > "$ROOT/usr/sbin/meshcored" <<EOD
#!/bin/sh
env > "$ROOT/meshcored.env"
trap 'echo term > "$ROOT/mcd.term"; exit 0' TERM INT
while :; do sleep 0.2; done
EOD
chmod 0755 "$ROOT/usr/sbin/meshcored"
rm -f "$ROOT/meshcored.env" "$ROOT/mcd.term"
"$S65" start >/dev/null 2>&1
wait_for "$ROOT/meshcored.env"
kill -9 "$(pidof_file "$MCD_SUP_PIDFILE")" 2>/dev/null
sleep 0.3
check "S65: with its supervisor killed, the daemon is an orphan still running" \
      $([ "$(count_mcd)" -eq 1 ] && [ -s "$ROOT/run/pocketos/meshcored.pid" ] && echo 1 || echo 0)
out=$("$S65" stop 2>&1)
check "S65 stop asks an orphaned meshcored to leave rather than killing it" \
      $([ -e "$ROOT/mcd.term" ] && [ "$(contains "$out" "forced")" -eq 0 ] && echo 1 || echo 0)
check "and leaves no daemon and no child pid file" \
      $([ "$(count_mcd)" -eq 0 ] && [ ! -e "$ROOT/run/pocketos/meshcored.pid" ] && echo 1 || echo 0)
make_meshcored
rm -f "$ROOT/mcd.term" "$ROOT/meshcored.env"

# Enabled and not installable: loud, and a non-zero exit.
mv "$ROOT/usr/sbin/meshcored" "$ROOT/usr/sbin/meshcored.away"
out=$("$S65" start 2>&1); rc=$?
check "S65 enabled with no meshcored binary fails loudly" $(contains "$out" "FAILED: enabled, but")
check "and exits non-zero" $([ "$rc" -ne 0 ] && echo 1 || echo 0)
mv "$ROOT/usr/sbin/meshcored.away" "$ROOT/usr/sbin/meshcored"
mv "$ROOT/usr/bin/pos-supervise" "$ROOT/usr/bin/pos-supervise.away"
out=$("$S65" start 2>&1); rc=$?
check "S65 with no pos-supervise refuses rather than run meshcored unsupervised" \
      $(contains "$out" "pos-supervise is not installed")
check "and exits non-zero" $([ "$rc" -ne 0 ] && echo 1 || echo 0)
check "and started nothing" $(wait_mcd 0 && echo 1 || echo 0)
mv "$ROOT/usr/bin/pos-supervise.away" "$ROOT/usr/bin/pos-supervise"

# A meshcored no init script started - unit A's bench state: started by hand
# with nohup, no pid file. A real ELF (a copy of sleep) under the daemon's
# path, so /proc/<pid>/exe names it exactly as it would on the unit.
cp "$(command -v sleep)" "$ROOT/usr/sbin/meshcored"
"$ROOT/usr/sbin/meshcored" 600 &
hand=$!
sleep 0.3
out=$("$S65" start 2>&1); rc=$?
check "S65 refuses to start beside a meshcored it did not start" \
      $(contains "$out" "already running unsupervised (pid $hand)")
check "and exits non-zero" $([ "$rc" -ne 0 ] && echo 1 || echo 0)
check "and did not start a supervisor" $([ ! -s "$MCD_SUP_PIDFILE" ] && echo 1 || echo 0)
out=$("$S65" stop 2>&1); rc=$?
check "S65 stop ends a meshcored it did not start" $(contains "$out" "stopped unsupervised pid $hand")
check "and exits 0" $([ "$rc" -eq 0 ] && echo 1 || echo 0)
check "and it is gone" $(wait_gone "$hand" && echo 1 || echo 0)
wait "$hand" 2>/dev/null
make_meshcored

# A crash, then a clean run: the supervisor restarts it and says so.
cat > "$ROOT/usr/sbin/meshcored" <<EOD
#!/bin/sh
if [ ! -e "$ROOT/mcd.crashed" ]; then : > "$ROOT/mcd.crashed"; exit 1; fi
env > "$ROOT/meshcored.env"
trap 'exit 0' TERM INT
while :; do sleep 0.2; done
EOD
chmod 0755 "$ROOT/usr/sbin/meshcored"
rm -f "$ROOT/meshcored.env" "$ROOT/mcd.crashed"
"$S65" start >/dev/null 2>&1
check "S65: a meshcored that crashes is restarted by the supervisor" \
      $(n=0; while [ ! -e "$ROOT/meshcored.env" ] && [ $n -lt 50 ]; do sleep 0.1; n=$((n + 1)); done; \
        [ -e "$ROOT/meshcored.env" ] && echo 1 || echo 0)
check "and the state file counts the restart and the exit code" \
      $([ "$(mcd_state restarts)" = "1" ] && [ "$(mcd_state last_exit_code)" = "1" ] \
        && [ "$(mcd_state running)" = "1" ] && echo 1 || echo 0)
"$S65" stop >/dev/null 2>&1
wait_mcd 0

# A meshcored that always crashes ends in a crash loop, reported the way every
# other supervised service reports it. The restart limit is lowered for the
# test through the supervisor's own knob; the backoff is the real one.
printf '#!/bin/sh\nexit 3\n' > "$ROOT/usr/sbin/meshcored"
chmod 0755 "$ROOT/usr/sbin/meshcored"
POS_SUPERVISE_MAX_RESTARTS=2 "$S65" start >/dev/null 2>&1
n=0
while [ "$(mcd_state crashloop)" != "1" ] && [ $n -lt 150 ]; do sleep 0.1; n=$((n + 1)); done
check "S65: a meshcored that keeps crashing ends in a crash loop" \
      $([ "$(mcd_state crashloop)" = "1" ] && echo 1 || echo 0)
check "with the marker the bring-up checklist names" \
      $([ -s "$ROOT/run/pocketos/meshcored.crashloop" ] && echo 1 || echo 0)
check "its last exit code recorded" $([ "$(mcd_state last_exit_code)" = "3" ] && echo 1 || echo 0)
check "and no daemon or pid file left behind" \
      $([ "$(count_mcd)" -eq 0 ] && [ ! -e "$ROOT/run/pocketos/meshcored.pid" ] && echo 1 || echo 0)
out=$("$S65" stop 2>&1); rc=$?
check "S65 stop after a crash loop is safe" $([ "$rc" -eq 0 ] && echo 1 || echo 0)
make_meshcored
rm -f "$ROOT/etc/default/meshcored" "$ROOT/meshcored.env" "$ROOT/meshcored.args" "$ROOT/mcd.crashed"

# ---- S90doors-shell --------------------------------------------------

# On by default (a fresh card boots Doors; tests/first_boot_default_test.sh),
# so off is a unit's own setting.
printf 'ENABLE=0\n' > "$ROOT/etc/default/doors-shell"
out=$("$S90" start 2>&1)
check "S90 switched off in its settings file is disabled" $(contains "$out" "disabled")
check "S90 disabled starts nothing" $([ ! -e "$ROOT/shell.env" ] && echo 1 || echo 0)
rm -f "$ROOT/etc/default/doors-shell"

printf 'ENABLE=1\n' > "$ROOT/etc/default/pocketos-shell"
printf 'ENABLE=1\n' > "$ROOT/etc/default/k230_phone_ui"
# Doors images carry no launcher; a unit on an older image still has one, and
# then its switch decides.
printf '#!/bin/sh\nexit 0\n' > "$ROOT/etc/init.d/S99zz_k230_phone_ui"
chmod 0755 "$ROOT/etc/init.d/S99zz_k230_phone_ui"
out=$("$S90" start 2>&1)
check "S90 refuses while an installed vendor launcher is enabled" $(contains "$out" "owns the panel")
check "S90 refusal names the launcher switch" $(contains "$out" "/etc/default/k230_phone_ui")
check "S90 refusal starts nothing" $([ ! -e "$ROOT/shell.env" ] && echo 1 || echo 0)
rm -f "$ROOT/etc/init.d/S99zz_k230_phone_ui"

printf 'ENABLE=0\n' > "$ROOT/etc/default/k230_phone_ui"
sleep 600 &
LAUNCHER=$!
echo "$LAUNCHER" > "$ROOT/var/run/k230_phone_ui.pid"
out=$("$S90" start 2>&1)
check "S90 refuses while the vendor launcher is running" $(contains "$out" "is running")
check "S90 running-launcher refusal starts nothing" $([ ! -e "$ROOT/shell.env" ] && echo 1 || echo 0)
kill "$LAUNCHER" 2>/dev/null
rm -f "$ROOT/var/run/k230_phone_ui.pid"

printf 'ENABLE=1\nPOCKETOS_DRM_ROTATION=180\nPOCKETOS_SAFE_CORNERS=24,24,24,24\n' > "$ROOT/etc/default/pocketos-shell"
# A switch left at ENABLE=1 from an older image, with no launcher installed:
# it decides nothing, or the panel would stay dark.
printf 'ENABLE=1\n' > "$ROOT/etc/default/k230_phone_ui"
out=$("$S90" start 2>&1)
rm -f "$ROOT/etc/default/k230_phone_ui"
check "S90 starts when it owns the panel" $(contains "$out" "OK")
check "S90 ignores a leftover launcher switch with no launcher installed" \
      $(contains "$out" "vendor launcher not installed")
check "S90 starts the shell" $(wait_for "$ROOT/shell.env" && echo 1 || echo 0)
SHPID=$(pidof_file "$ROOT/var/run/doors-shell-supervise.pid")
check "S90 start writes the supervise pid file" $([ -n "$SHPID" ] && echo 1 || echo 0)
check "S90 start leaves the supervisor running" $(alive "$SHPID" && echo 1 || echo 0)
check "S90 start records the daemon pid in the runtime dir" \
      $([ -s "$ROOT/run/pocketos/doors-shell.pid" ] && echo 1 || echo 0)
check "S90 exports the persistent log directory" \
      $(grep -q "^POCKETOS_LOG_DIR=$ROOT/var/lib/pocketos/log$" "$ROOT/shell.env" && echo 1 || echo 0)
check "S90 exports the vendor staging default" \
      $(grep -q '^K230_LVGL_DRM_STAGING=1$' "$ROOT/shell.env" && echo 1 || echo 0)
check "S90 exports a bench override that was set" \
      $(grep -q '^POCKETOS_DRM_ROTATION=180$' "$ROOT/shell.env" && echo 1 || echo 0)
check "S90 exports the safe-area corner override when set" \
      $(grep -q '^POCKETOS_SAFE_CORNERS=24,24,24,24$' "$ROOT/shell.env" && echo 1 || echo 0)
check "S90 does not export a bench override that was not set" \
      $(grep -q '^POCKETOS_TOUCH_CALIB=' "$ROOT/shell.env" && echo 0 || echo 1)

out=$("$S90" stop 2>&1)
check "S90 stop reports OK" $(contains "$out" "OK")
check "S90 stop removes the supervise pid file" \
      $([ ! -f "$ROOT/var/run/doors-shell-supervise.pid" ] && echo 1 || echo 0)
check "S90 stop ends the supervisor" $(wait_gone "$SHPID" && echo 1 || echo 0)

# ---- Phase 3: one identity, one settings file, one shell -----------------
#
# ADR-005 Phase 3 renamed the service. What is checked here is everything that
# can go wrong while a unit is half-way between the two names: which settings
# file is read (never both), and whether a second shell can start beside this
# one.

NEWCONF="$ROOT/etc/default/doors-shell"
OLDCONF="$ROOT/etc/default/pocketos-shell"
OLD_INIT="$ROOT/etc/init.d/S90pocketos-shell"
OLD_DAEMON="$ROOT/usr/bin/pocketos-shell"
OLD_SUP_PID="$ROOT/var/run/pocketos-shell-supervise.pid"
OLD_CHILD_PID="$ROOT/run/pocketos/pocketos-shell.pid"
shell_started() { wait_for "$ROOT/shell.env" && echo 1 || echo 0; }
clear_run() { rm -f "$ROOT/shell.env"; }

# The identity itself. The supervisor's name decides the runtime files and the
# supervisor log, which is what sysd reads to build the service row.
check "the overlay carries the doors-shell service and only it" \
      $([ -f "$REPO/$S90_SRC" ] && [ ! -e "$REPO/$OVERLAY/etc/init.d/S90pocketos-shell" ] && echo 1 || echo 0)
check "it supervises under that name" \
      $(grep -q 'NAME=doors-shell' "$REPO/$S90_SRC" && echo 1 || echo 0)
check "it runs /usr/bin/doors-shell" \
      $(grep -q 'DAEMON=/usr/bin/doors-shell' "$REPO/$S90_SRC" && echo 1 || echo 0)
check "its runtime files carry the new name" \
      $(grep -q 'PIDFILE=/var/run/doors-shell-supervise.pid' "$REPO/$S90_SRC" &&
        grep -q 'CHILD_PIDFILE=/run/pocketos/doors-shell.pid' "$REPO/$S90_SRC" && echo 1 || echo 0)
check "the socket, logs and directories are unchanged" \
      $(grep -q 'LOGDIR=/var/lib/pocketos/log' "$REPO/$S90_SRC" &&
        grep -q 'shell.stdio.log' "$REPO/$S90_SRC" && echo 1 || echo 0)

# Settings: whole file or nothing.
rm -f "$NEWCONF" "$OLDCONF"; clear_run
printf 'ENABLE=1\nPOCKETOS_SAFE_CORNERS=11,11,11,11\n' > "$OLDCONF"
out=$("$S90" start 2>&1)
check "with only the PocketOS-era settings file, that file is used" \
      $([ "$(shell_started)" = 1 ] && contains "$out" "settings from" && echo 1 || echo 0)
check "and the output names it" $(contains "$out" "etc/default/pocketos-shell")
check "and its settings take effect" \
      $(grep -q '^POCKETOS_SAFE_CORNERS=11,11,11,11$' "$ROOT/shell.env" && echo 1 || echo 0)
"$S90" stop >/dev/null 2>&1; clear_run

printf 'ENABLE=1\nPOCKETOS_SAFE_CORNERS=22,22,22,22\n' > "$NEWCONF"
out=$("$S90" start 2>&1)
wait_for "$ROOT/shell.env" || true
check "with both files, the doors-shell one is used" \
      $(grep -q '^POCKETOS_SAFE_CORNERS=22,22,22,22$' "$ROOT/shell.env" 2>/dev/null && echo 1 || echo 0)
check "and it says the old one was ignored rather than merged" \
      $(contains "$out" "ignored, not merged")
check "nothing from the old file leaks in" \
      $(grep -q '11,11,11,11' "$ROOT/shell.env" 2>/dev/null && echo 0 || echo 1)
"$S90" stop >/dev/null 2>&1; clear_run

# ENABLE belongs to that file too: a unit switched off in the new file stays
# off however the old one reads.
printf 'ENABLE=0\n' > "$NEWCONF"
out=$("$S90" start 2>&1)
check "ENABLE=0 in the new file wins over ENABLE=1 in the old one" \
      $([ "$(contains "$out" "disabled")" = 1 ] && [ ! -e "$ROOT/shell.env" ] && echo 1 || echo 0)
check "and the refusal names the file it read" $(contains "$out" "etc/default/doors-shell")

rm -f "$OLDCONF"
printf 'ENABLE=1\n' > "$NEWCONF"
out=$("$S90" start 2>&1)
check "with only the new file, it starts" $([ "$(shell_started)" = 1 ] && echo 1 || echo 0)
"$S90" stop >/dev/null 2>&1; clear_run

# One shell. A PocketOS-era service that is still installed and enabled would
# start at the next boot beside this one: two DRM owners, two shell.sock
# owners. The refusal is the guarantee, and it names what to remove.
printf 'ENABLE=1\n' > "$OLDCONF"
cp "$ROOT/usr/bin/doors-shell" "$OLD_DAEMON"
cp "$S90" "$OLD_INIT"
out=$("$S90" start 2>&1)
check "it refuses to start beside an installed, enabled PocketOS-era service" \
      $([ ! -e "$ROOT/shell.env" ] && [ "$(contains "$out" "not started")" = 1 ] && echo 1 || echo 0)
check "the refusal names the init script to remove" $(contains "$out" "S90pocketos-shell")
"$S90" start >/dev/null 2>&1
check "the refusal is a failure, not a quiet success" $([ $? -ne 0 ] && echo 1 || echo 0)
check "and it removed nothing itself" \
      $([ -e "$OLD_INIT" ] && [ -e "$OLD_DAEMON" ] && echo 1 || echo 0)

# Installed but disabled is not a second service; it may still be there while a
# migration finishes, and the start reports it rather than refusing.
printf 'ENABLE=0\n' > "$OLDCONF"
out=$("$S90" start 2>&1)
check "an installed but disabled PocketOS-era service does not block the start" \
      $([ "$(shell_started)" = 1 ] && echo 1 || echo 0)
check "but it is reported" $(contains "$out" "still installed but disabled")
"$S90" stop >/dev/null 2>&1; clear_run
rm -f "$OLD_INIT"

# A PocketOS-era shell that is actually running is the dangerous case,
# whatever the files say.
sleep 600 &
OLDSH=$!
echo "$OLDSH" > "$OLD_CHILD_PID"
out=$("$S90" start 2>&1)
check "it refuses while a PocketOS-era shell is running" \
      $([ ! -e "$ROOT/shell.env" ] && [ "$(contains "$out" "is running")" = 1 ] && echo 1 || echo 0)
kill "$OLDSH" 2>/dev/null; wait "$OLDSH" 2>/dev/null
rm -f "$OLD_CHILD_PID"

# The same shell with no pid file at all: a daemon whose supervisor was killed
# still holds DRM and the socket, and only /proc knows about it. The stand-in
# is a real executable copied to the old name - a shell script would show its
# interpreter in /proc/<pid>/exe, which is not what the sweep reads.
cp "$(command -v sleep)" "$OLD_DAEMON" 2>/dev/null && chmod 0755 "$OLD_DAEMON"
"$OLD_DAEMON" 600 &
ORPHAN=$!
sleep 0.5
out=$("$S90" start 2>&1)
check "it refuses while a PocketOS-era shell runs with no pid file" \
      $([ ! -e "$ROOT/shell.env" ] && [ "$(contains "$out" "is running")" = 1 ] && echo 1 || echo 0)
kill "$ORPHAN" 2>/dev/null; wait "$ORPHAN" 2>/dev/null
rm -f "$OLD_DAEMON"

# Stale runtime files from the old identity are not a running shell. A reboot
# clears /run, but a deploy that removed the service must not leave the new one
# refusing for ever.
printf 'stale\n' > "$OLD_CHILD_PID"
printf '99999999\n' > "$OLD_SUP_PID"
: > "$ROOT/run/pocketos/pocketos-shell.state"
: > "$ROOT/run/pocketos/pocketos-shell.crashloop"
out=$("$S90" start 2>&1)
check "stale PocketOS-era pid, state and crashloop files do not block the start" \
      $([ "$(shell_started)" = 1 ] && echo 1 || echo 0)
check "the new supervisor wrote its state under the new name" \
      $([ -s "$ROOT/run/pocketos/doors-shell.state" ] && echo 1 || echo 0)
check "the state file names this service" \
      $(grep -qx 'name=doors-shell' "$ROOT/run/pocketos/doors-shell.state" && echo 1 || echo 0)
check "the supervisor log carries the new name" \
      $([ -f "$ROOT/var/lib/pocketos/log/supervise-doors-shell.log" ] && echo 1 || echo 0)
"$S90" stop >/dev/null 2>&1
rm -f "$OLD_CHILD_PID" "$OLD_SUP_PID" "$OLD_DAEMON" \
      "$ROOT/run/pocketos/pocketos-shell.state" "$ROOT/run/pocketos/pocketos-shell.crashloop"
clear_run

# ---- the orientation apply, under the new service name ------------------
#
# The display milestone applies a rotation by re-executing the shell in place:
# same pid, so pos-supervise sees no exit and counts no restart. Renaming the
# service must not turn that into a restart, or worse into a crash-loop count,
# so the daemon here does exactly what the shell does.
cat > "$ROOT/usr/bin/doors-shell" <<EOD
#!/bin/sh
if [ -z "\${REEXEC_DONE:-}" ]; then
	echo \$\$ > "$ROOT/reexec.first"
	REEXEC_DONE=1 exec "\$0" "\$@"
fi
echo \$\$ > "$ROOT/reexec.second"
env > "$ROOT/shell.env"
trap 'exit 0' TERM INT
while :; do sleep 0.2; done
EOD
chmod 0755 "$ROOT/usr/bin/doors-shell"
rm -f "$ROOT/reexec.first" "$ROOT/reexec.second"
printf 'ENABLE=1\n' > "$NEWCONF"
"$S90" start >/dev/null 2>&1
wait_for "$ROOT/reexec.second" || true
sleep 1
FIRST=$(pidof_file "$ROOT/reexec.first"); SECOND=$(pidof_file "$ROOT/reexec.second")
CHILD=$(pidof_file "$ROOT/run/pocketos/doors-shell.pid")
check "an orientation apply keeps the same process" \
      $([ -n "$FIRST" ] && [ "$FIRST" = "$SECOND" ] && echo 1 || echo 0)
check "the supervisor's child pid is still that process" \
      $([ -n "$CHILD" ] && [ "$CHILD" = "$SECOND" ] && echo 1 || echo 0)
check "the supervisor counted no restart" \
      $(grep -qx 'restarts=0' "$ROOT/run/pocketos/doors-shell.state" && echo 1 || echo 0)
check "and no crash loop" \
      $(grep -qx 'crashloop=0' "$ROOT/run/pocketos/doors-shell.state" &&
        [ ! -e "$ROOT/run/pocketos/doors-shell.crashloop" ] && echo 1 || echo 0)
check "the service is still running after it" \
      $(grep -qx 'running=1' "$ROOT/run/pocketos/doors-shell.state" && echo 1 || echo 0)
"$S90" stop >/dev/null 2>&1
make_daemon "$ROOT/usr/bin/doors-shell" "$ROOT/shell.env"
rm -f "$NEWCONF" "$ROOT/reexec.first" "$ROOT/reexec.second"; clear_run
printf 'ENABLE=1\n' > "$OLDCONF"

# ---- S55netd ------------------------------------------------------------
#
# netd's script has S50's discipline and one setting, the interface, from
# /etc/default/netd. Everything started here is stopped again before the S50
# section, which counts supervisors as its own.

S55="$ROOT/etc/init.d/S55netd"
count_netd() { pgrep -af "$ROOT/usr/sbin/netd" 2>/dev/null | grep -vc 'pos-supervise'; }
wait_netd() { # <expected count> — up to 5 s
    n=0
    while [ "$(count_netd)" -ne "$1" ] && [ $n -lt 50 ]; do sleep 0.1; n=$((n + 1)); done
    [ "$(count_netd)" -eq "$1" ]
}

out=$("$S55" start 2>&1)
check "S55 start reports OK" $(contains "$out" "OK")
check "S55 start names the default interface" $(contains "$out" "(wlan0)")
NETPID=$(pidof_file "$ROOT/var/run/netd-supervise.pid")
check "S55 start writes the supervise pid file" $([ -n "$NETPID" ] && echo 1 || echo 0)
check "S55 start leaves the supervisor running" $(alive "$NETPID" && echo 1 || echo 0)
check "S55 start starts the daemon" $(wait_for "$ROOT/netd.args" && echo 1 || echo 0)
check "S55 passes the interface" $(grep -qx -- '--interface wlan0' "$ROOT/netd.args" && echo 1 || echo 0)
check "S55 start records the daemon pid in the runtime dir" \
      $([ -s "$ROOT/run/pocketos/netd.pid" ] && echo 1 || echo 0)
check "S55 exports the persistent log directory" \
      $(grep -q "^POCKETOS_LOG_DIR=$ROOT/var/lib/pocketos/log$" "$ROOT/netd.env" && echo 1 || echo 0)
check "S55 exports POCKETOS_LOG_STDERR=0" \
      $(grep -q '^POCKETOS_LOG_STDERR=0$' "$ROOT/netd.env" && echo 1 || echo 0)
check "S55 exports no netd test hook" \
      $(grep -q '^NETD_TEST_' "$ROOT/netd.env" && echo 0 || echo 1)
out=$("$S55" start 2>&1)
check "S55 start is idempotent" $(contains "$out" "already running")
check "S55 does not start a second daemon" $(wait_netd 1 && echo 1 || echo 0)
out=$("$S55" stop 2>&1)
check "S55 stop reports OK" $(contains "$out" "OK")
check "S55 stop ends the supervisor" $(wait_gone "$NETPID" && echo 1 || echo 0)
check "S55 stop ends the daemon" $(wait_netd 0 && echo 1 || echo 0)
check "S55 stop removes the supervise pid file" \
      $([ ! -f "$ROOT/var/run/netd-supervise.pid" ] && echo 1 || echo 0)
out=$("$S55" stop 2>&1)
check "S55 stop when not running says so" $(contains "$out" "not running")
rm -f "$ROOT/netd.args" "$ROOT/netd.env"
printf 'NETD_INTERFACE=wlan1\n' > "$ROOT/etc/default/netd"
out=$("$S55" restart 2>&1)
check "S55 honours /etc/default/netd" $(contains "$out" "(wlan1)")
check "S55 passes the configured interface" \
      $(wait_for "$ROOT/netd.args" && grep -qx -- '--interface wlan1' "$ROOT/netd.args" && echo 1 || echo 0)
"$S55" stop >/dev/null 2>&1
wait_netd 0
rm -f "$ROOT/etc/default/netd"

# ---- S50sysd ------------------------------------------------------------
#
# Runs last, so the supervisor count belongs to sysd alone. sysd has no
# per-unit configuration: there is nothing to source and nothing to refuse,
# so what is exercised here is the start/stop discipline S60radiod earned on
# unit A (M6) — the supervise pid and the daemon pid are two different facts,
# and stop does not return until both are gone.

out=$("$S50" start 2>&1)
check "S50 start reports OK" $(contains "$out" "OK")
SYSPID=$(pidof_file "$ROOT/var/run/sysd-supervise.pid")
check "S50 start writes the supervise pid file" $([ -n "$SYSPID" ] && echo 1 || echo 0)
check "S50 start leaves the supervisor running" $(alive "$SYSPID" && echo 1 || echo 0)
check "S50 start starts the daemon" $(wait_for "$ROOT/sysd.env" && echo 1 || echo 0)
check "S50 start records the daemon pid in the runtime dir" \
      $([ -s "$ROOT/run/pocketos/sysd.pid" ] && echo 1 || echo 0)
# The pid file is what stop() needs; the state file is what sysd reads. Both
# have to be there, written by the real pos-supervise this test runs.
check "S50 start writes the supervisor state file" \
      $([ -s "$ROOT/run/pocketos/sysd.state" ] && echo 1 || echo 0)
check "S50 state names the running daemon" \
      $([ "$(sed -n 's/^child_pid=//p' "$ROOT/run/pocketos/sysd.state")" = \
          "$(cat "$ROOT/run/pocketos/sysd.pid")" ] && echo 1 || echo 0)
check "S50 state says running" \
      $([ "$(sed -n 's/^running=//p' "$ROOT/run/pocketos/sysd.state")" = "1" ] && echo 1 || echo 0)
check "S50 exports the persistent log directory" \
      $(grep -q "^POCKETOS_LOG_DIR=$ROOT/var/lib/pocketos/log$" "$ROOT/sysd.env" && echo 1 || echo 0)
check "S50 exports POCKETOS_LOG_STDERR=0" \
      $(grep -q '^POCKETOS_LOG_STDERR=0$' "$ROOT/sysd.env" && echo 1 || echo 0)
check "S50 does not export a fake root" \
      $(grep -q '^POCKETSYS_ROOT=' "$ROOT/sysd.env" && echo 0 || echo 1)
check "S50 creates the stdio log" $([ -f "$ROOT/var/lib/pocketos/log/sysd.stdio.log" ] && echo 1 || echo 0)

out=$("$S50" start 2>&1)
check "S50 start is idempotent" $(contains "$out" "already running")
check "S50 does not start a second daemon" $(wait_sysd 1 && echo 1 || echo 0)

out=$("$S50" stop 2>&1)
check "S50 stop reports OK" $(contains "$out" "OK")
check "S50 stop removes the supervise pid file" \
      $([ ! -f "$ROOT/var/run/sysd-supervise.pid" ] && echo 1 || echo 0)
check "S50 stop ends the supervisor" $(wait_gone "$SYSPID" && echo 1 || echo 0)
check "S50 stop ends the daemon" $(wait_sysd 0 && echo 1 || echo 0)
check "S50 stop leaves the state file saying not running" \
      $([ "$(sed -n 's/^running=//p' "$ROOT/run/pocketos/sysd.state")" = "0" ] && echo 1 || echo 0)

out=$("$S50" stop 2>&1)
check "S50 stop when not running says so" $(contains "$out" "not running")

rm -f "$ROOT/sysd.env"
printf 'previous boot\n' > "$ROOT/var/lib/pocketos/log/sysd.stdio.log"
out=$("$S50" restart 2>&1)
check "S50 restart starts the service" $(wait_for "$ROOT/sysd.env" && echo 1 || echo 0)
check "S50 restart leaves exactly one daemon" $(wait_sysd 1 && echo 1 || echo 0)
check "S50 restart leaves exactly one supervisor" \
      $([ "$(count_supervisors)" -eq 1 ] && echo 1 || echo 0)
check "S50 rotates the previous stdio log to .1" \
      $(grep -q 'previous boot' "$ROOT/var/lib/pocketos/log/sysd.stdio.log.1" 2>/dev/null && echo 1 || echo 0)
"$S50" stop >/dev/null 2>&1
wait_sysd 0

# A daemon that ignores SIGTERM: the supervisor waits for it and so never
# leaves either. stop must escalate to the daemon, then to SIGKILL, and not
# return while anything is left holding the socket.
cat > "$ROOT/usr/sbin/sysd" <<EOD
#!/bin/sh
trap '' TERM INT
env > "$ROOT/sysd.env"
while :; do sleep 0.2; done
EOD
chmod 0755 "$ROOT/usr/sbin/sysd"
rm -f "$ROOT/sysd.env"
"$S50" start >/dev/null 2>&1
wait_for "$ROOT/sysd.env"
wait_sysd 1
started=$(date +%s)
out=$("$S50" stop 2>&1)
elapsed=$(( $(date +%s) - started ))
check "S50 stop returns when the daemon ignores SIGTERM" $(contains "$out" "OK")
check "S50 stop reports that it had to force" $(contains "$out" "forced")
check "S50 stop leaves no daemon behind" $([ "$(count_sysd)" -eq 0 ] && echo 1 || echo 0)
check "S50 stop leaves no supervisor behind" $([ "$(count_supervisors)" -eq 0 ] && echo 1 || echo 0)
check "S50 stop is bounded" $([ "$elapsed" -le 20 ] && echo 1 || echo 0)

# A daemon that exits at once leaves the supervisor between restarts. Two
# things used to go wrong there and both are on the path a reboot takes:
# stop() read a pid file still naming the dead child, and it found the
# supervisor asleep in a foreground `sleep`, waited its 3 s, escalated to
# SIGKILL and called a service that was not even running "OK (forced)".
cat > "$ROOT/usr/sbin/sysd" <<EOD
#!/bin/sh
env > "$ROOT/sysd.env"
exit 5
EOD
chmod 0755 "$ROOT/usr/sbin/sysd"
rm -f "$ROOT/sysd.env"
"$S50" start >/dev/null 2>&1
wait_for "$ROOT/sysd.env"
# Wait for a backoff of at least 4 s: stop() allows the supervisor 3 s before
# escalating, so a 1 s backoff would have been survived by the old foreground
# sleep too and would not prove anything.
n=0
b=""
while [ $n -lt 200 ]; do
    b=$(sed -n 's/^backoff_s=//p' "$ROOT/run/pocketos/sysd.state" 2>/dev/null)
    [ -n "$b" ] && [ "$b" -ge 4 ] && break
    n=$((n + 1)); sleep 0.1
done
check "S50 leaves a daemon that keeps exiting in a backoff of 4 s or more (${b}s)" \
      $([ -n "$b" ] && [ "$b" -ge 4 ] && echo 1 || echo 0)
check "S50 backoff has no pid file to signal" \
      $([ ! -e "$ROOT/run/pocketos/sysd.pid" ] && echo 1 || echo 0)
started=$(date +%s%N)
out=$("$S50" stop 2>&1)
elapsed_ms=$(( ($(date +%s%N) - started) / 1000000 ))
check "S50 stop during a backoff reports OK" $(contains "$out" "OK")
check "S50 stop during a backoff is not forced" \
      $([ "$(contains "$out" "forced")" -eq 0 ] && echo 1 || echo 0)
# Bounded by the script's own escalation budget (STOP_TIMEOUT, 3 s): under it
# means wait_gone was satisfied and nothing was SIGKILLed, which is the
# property. The wall clock here also contains stop()'s own `sleep 0.1` polling
# loop, whose granularity stretches under load - 2422 ms was seen in a full
# suite run where the supervisor itself still answered in 104 ms. The precise
# measurement of that lives in tests/supervise_test.sh, which times the signal
# against the process directly.
check "S50 stop during a backoff needs no escalation (${elapsed_ms} ms)" \
      $([ "$elapsed_ms" -lt 3000 ] && echo 1 || echo 0)
check "S50 stop during a backoff leaves no supervisor" \
      $([ "$(count_supervisors)" -eq 0 ] && echo 1 || echo 0)
check "S50 stop during a backoff leaves the state saying not running" \
      $([ "$(sed -n 's/^running=//p' "$ROOT/run/pocketos/sysd.state")" = "0" ] && echo 1 || echo 0)
make_daemon "$ROOT/usr/sbin/sysd" "$ROOT/sysd.env"

# ---- a stop that cannot finish (cold review F9) --------------------------

# Every stop() removed the pid file before deciding whether anything had
# actually stopped, every script ended `exit 0` whatever stop() returned, and
# restart was `stop; start`. A daemon that would not go therefore produced a
# caller told the stop had succeeded, no pid file left to try again with, and
# a second supervisor started on top of the first.
#
# A stop that genuinely cannot finish needs a pid that answers kill -0 and
# ignores every signal, SIGKILL included. That is a zombie: its parent is
# alive and never reaps it. No root, no unkillable process, and nothing that
# can escape this test - killing the parent reaps it.
cat > "$ROOT/usr/bin/make-zombie" <<'EOZ'
#!/bin/sh
# usage: make-zombie <pidfile>. Writes the zombie's pid, then stays alive
# without reaping it, so the pid keeps existing.
sh -c 'exit 0' &
echo $! > "$1"
exec sleep 60
EOZ
chmod 0755 "$ROOT/usr/bin/make-zombie"

zombie_pid=""
zombie_parent=""
start_zombie() {
    rm -f "$ROOT/zombie.pid"
    "$ROOT/usr/bin/make-zombie" "$ROOT/zombie.pid" &
    zombie_parent=$!
    wait_for "$ROOT/zombie.pid" || return 1
    zombie_pid=$(cat "$ROOT/zombie.pid")
    # Wait until it really is a zombie, so kill -0 answering is not just the
    # process still running normally.
    n=0
    while [ $n -lt 50 ]; do
        [ "$(awk '{print $3}' "/proc/$zombie_pid/stat" 2>/dev/null)" = "Z" ] && return 0
        sleep 0.1; n=$((n + 1))
    done
    return 1
}

if start_zombie; then
    check "the unkillable stand-in exists" $(kill -0 "$zombie_pid" 2>/dev/null && echo 1 || echo 0)
    kill -KILL "$zombie_pid" 2>/dev/null
    sleep 0.2
    check "and survives SIGKILL" $(kill -0 "$zombie_pid" 2>/dev/null && echo 1 || echo 0)

    # The scripts wait STOP_TIMEOUT (3 s) three times before giving up, which
    # is 9 s per service of pure waiting. The staged copies get one second
    # instead, and the rewrite is asserted like every other one here.
    for svc in S50sysd S55netd S60radiod S90doors-shell; do
        sed 's/^STOP_TIMEOUT=3$/STOP_TIMEOUT=1/' "$ROOT/etc/init.d/$svc" \
            > "$ROOT/etc/init.d/$svc.quick"
        chmod 0755 "$ROOT/etc/init.d/$svc.quick"
    done
    quick_ok=$(grep -l '^STOP_TIMEOUT=1$' "$ROOT/etc/init.d/S50sysd.quick" \
               "$ROOT/etc/init.d/S55netd.quick" "$ROOT/etc/init.d/S60radiod.quick" \
               "$ROOT/etc/init.d/S90doors-shell.quick" 2>/dev/null | wc -l)
    check "the shortened stop budget reached all four copies" \
          $([ "$quick_ok" -eq 4 ] && echo 1 || echo 0)

    for svc in S50sysd S55netd S60radiod S90doors-shell; do
        case $svc in
        S50sysd)           pidfile="$ROOT/var/run/sysd-supervise.pid" ;;
        S55netd)           pidfile="$ROOT/var/run/netd-supervise.pid" ;;
        S60radiod)         pidfile="$ROOT/var/run/radiod-supervise.pid" ;;
        S90doors-shell) pidfile="$ROOT/var/run/doors-shell-supervise.pid" ;;
        esac
        script="$ROOT/etc/init.d/$svc.quick"

        # The supervisor pid file names something that will not die.
        printf '%s\n' "$zombie_pid" > "$pidfile"
        before=$(count_supervisors)

        out=$("$script" stop 2>&1); rc=$?
        check "$svc stop that cannot finish says so" $(contains "$out" "FAILED")
        check "$svc stop that cannot finish exits non-zero" \
              $([ "$rc" -ne 0 ] && echo 1 || echo 0)
        check "$svc keeps the pid file it could not stop with" \
              $([ "$(cat "$pidfile" 2>/dev/null)" = "$zombie_pid" ] && echo 1 || echo 0)

        out=$("$script" restart 2>&1); rc=$?
        check "$svc restart after a failed stop exits non-zero" \
              $([ "$rc" -ne 0 ] && echo 1 || echo 0)
        check "$svc restart after a failed stop starts nothing" \
              $([ "$(count_supervisors)" -eq "$before" ] && echo 1 || echo 0)
        check "$svc restart after a failed stop does not announce a start" \
              $([ "$(contains "$out" "Starting")" -eq 0 ] && echo 1 || echo 0)

        # A bare start must refuse too: the pid file still names something
        # that is running.
        out=$("$script" start 2>&1)
        check "$svc start on top of it refuses" $(contains "$out" "already running")
        check "$svc start on top of it adds no supervisor" \
              $([ "$(count_supervisors)" -eq "$before" ] && echo 1 || echo 0)

        rm -f "$pidfile"
    done
    kill "$zombie_parent" 2>/dev/null
    wait "$zombie_parent" 2>/dev/null
else
    echo "FAIL could not create the unkillable stand-in"
    failed=$((failed + 1))
fi

# ---- deploy.sh stops before it installs (cold review F9) -----------------

# The remote half is deploy_unit.sh, which deploy.sh copies to the unit and
# runs there; it is run here with its paths moved into a fake root rather than
# restated, since a copy would stop testing the real thing the moment the
# script changed. The init scripts are replaced by ones that always fail, and
# tar by a recorder.
{
    d=$ROOT/deploy
    mkdir -p "$d/etc/init.d" "$d/bin"
    for svc in S50sysd S55netd S60radiod S90doors-shell; do
        printf '#!/bin/sh\necho "Stopping %s: FAILED, still running"\nexit 1\n' "$svc" \
            > "$d/etc/init.d/$svc"
        chmod 0755 "$d/etc/init.d/$svc"
    done
    printf '#!/bin/sh\necho tar-ran >> "%s/tar.log"\n' "$d" > "$d/bin/tar"
    chmod 0755 "$d/bin/tar"
    : > "$d/payload.tar"
    sed -e "s#/etc/init.d#$d/etc/init.d#g" \
        -e "s/^doors version.*//" \
        "$REPO/platforms/k230/scripts/deploy_unit.sh" > "$d/remote.sh"
    check "the remote half of deploy.sh was extracted" \
          $([ -s "$d/remote.sh" ] && grep -q 'could not be stopped' "$d/remote.sh" \
            && echo 1 || echo 0)
    out=$(PATH="$d/bin:$PATH" sh "$d/remote.sh" "$d/payload.tar" 2>&1 </dev/null); rc=$?
    check "deploy stops before it installs, and aborts when a stop fails" \
          $([ "$rc" -ne 0 ] && echo 1 || echo 0)
    check "deploy says nothing was installed" $(contains "$out" "nothing has been installed")
    check "deploy unpacked no files" $([ ! -f "$d/tar.log" ] && echo 1 || echo 0)

    # An archive that is not there: refused before a single service is asked
    # to stop (the stand-ins would say so if one were).
    out=$(PATH="$d/bin:$PATH" sh "$d/remote.sh" "$d/no-such.tar" 2>&1 </dev/null); rc=$?
    check "deploy_unit.sh without its archive stops nothing" \
          $([ "$rc" -ne 0 ] && [ "$(contains "$out" "nothing has been stopped")" = 1 ] &&
            [ "$(contains "$out" "Stopping")" = 0 ] && echo 1 || echo 0)
}

# ---- deploy.sh refuses to start an incomplete installation ---------------
#
# The same remote half, with every path it names moved into a fake root and a
# tar that unpacks a prepared payload there. What is under test: after the
# unpack the unit is asked whether every service arrived whole, and a unit
# holding one half of a service (unit A, 2026-09-22: meshcored without
# S65meshcored) ends the deploy before anything is started.
#
# The /proc sweeps stay real, so the meshcored pattern is renamed to a name
# only this test's own stand-in carries: nothing else on the build host can
# match it, and nothing else can be signalled.
FAKE_MCD="mcdgate$$"
deploy_remote() { # <dir>: writes <dir>/remote.sh, and the archive it is given
    sed -e "s#/var/run#$1/var/run#g" \
        -e "s#\([[:space:]]\)/run/pocketos#\1$1/run/pocketos#g" \
        -e "s#/etc/init.d#$1/etc/init.d#g" \
        -e "s#/usr/sbin/#$1/usr/sbin/#g" \
        -e "s#/usr/bin/#$1/usr/bin/#g" \
        -e "s#\*/meshcored|\*\"/meshcored (deleted)\"#*/$FAKE_MCD|*\"/$FAKE_MCD (deleted)\"#" \
        -e "s/^doors version.*//" \
        "$REPO/platforms/k230/scripts/deploy_unit.sh" > "$1/remote.sh"
    : > "$1/payload.tar"
}
deploy_root() { # <dir> <payload dir>: stop/start stubs that record, and a tar that unpacks <payload>
    mkdir -p "$1/etc/init.d" "$1/bin" "$1/run/pocketos" "$1/var/run"
    for svc in S50sysd S55netd S60radiod S65meshcored S90doors-shell; do
        printf '#!/bin/sh\necho "$1 %s" >> "%s/calls.log"\nexit 0\n' "$svc" "$1" > "$1/etc/init.d/$svc"
        chmod 0755 "$1/etc/init.d/$svc"
    done
    printf '#!/bin/sh\ncp -a "%s"/. "%s"/\n' "$2" "$1" > "$1/bin/tar"
    chmod 0755 "$1/bin/tar"
}
deploy_payload() { # <dir> <deploy dir>: every service whole, as the archive carries it
    mkdir -p "$1/usr/sbin" "$1/usr/bin" "$1/etc/init.d"
    for f in usr/sbin/sysd usr/sbin/netd usr/sbin/radiod usr/sbin/meshcored usr/bin/doors-shell \
             usr/bin/pos-supervise etc/init.d/S50sysd etc/init.d/S55netd etc/init.d/S60radiod \
             etc/init.d/S65meshcored etc/init.d/S90doors-shell; do
        printf '#!/bin/sh\necho "$1 %s" >> "%s/calls.log"\nexit 0\n' "${f##*/}" "$2" > "$1/$f"
        chmod 0755 "$1/$f"
    done
}
{
    d=$ROOT/deploy-incomplete; p=$ROOT/payload-incomplete
    deploy_payload "$p" "$ROOT/deploy-incomplete"; rm -f "$p/etc/init.d/S65meshcored"
    mkdir -p "$d"; deploy_remote "$d"
    check "the remote half was extracted with the completeness check" \
          $(grep -q 'missing or not executable after unpacking' "$d/remote.sh" && echo 1 || echo 0)
    # The stand-in init scripts that exist before the unpack stop cleanly; the
    # payload then lacks S65meshcored, so the one the unit ends up with is the
    # stub from before - removed here, which is unit A's state exactly.
    deploy_root "$d" "$p"; rm -f "$d/etc/init.d/S65meshcored"
    out=$(PATH="$d/bin:$PATH" sh "$d/remote.sh" "$d/payload.tar" 2>&1 </dev/null); rc=$?
    check "deploy refuses a unit left with meshcored and no S65meshcored" $([ "$rc" -ne 0 ] && echo 1 || echo 0)
    check "and names the missing piece" $(contains "$out" "S65meshcored is missing or not executable after unpacking")
    check "and says services were not started" $(contains "$out" "services were NOT started")
    check "and started nothing" $(grep -q '^start' "$d/calls.log" 2>/dev/null && echo 0 || echo 1)

    d=$ROOT/deploy-mode; p=$ROOT/payload-mode
    deploy_payload "$p" "$ROOT/deploy-mode"; chmod 0644 "$p/usr/sbin/meshcored"
    mkdir -p "$d"; deploy_remote "$d"; deploy_root "$d" "$p"
    out=$(PATH="$d/bin:$PATH" sh "$d/remote.sh" "$d/payload.tar" 2>&1 </dev/null); rc=$?
    check "deploy refuses a meshcored binary that arrived not executable" \
          $([ "$rc" -ne 0 ] && [ "$(contains "$out" "usr/sbin/meshcored is missing or not executable")" = 1 ] && echo 1 || echo 0)

    d=$ROOT/deploy-complete; p=$ROOT/payload-complete
    deploy_payload "$p" "$d"
    mkdir -p "$d"; deploy_remote "$d"; deploy_root "$d" "$p"
    # A meshcored no init script started, under the renamed pattern: a real
    # ELF (a copy of sleep), so /proc/<pid>/exe names it.
    mkdir -p "$d/hand"; cp "$(command -v sleep)" "$d/hand/$FAKE_MCD"
    "$d/hand/$FAKE_MCD" 600 & hand=$!
    sleep 0.3
    # radiod's stop records whether the hand-started meshcored was still there:
    # it must already be gone, since it holds radiod's lease.
    printf '#!/bin/sh\n[ "$1" = stop ] && { kill -0 %s 2>/dev/null && echo mcd-alive-at-radiod-stop >> "%s/calls.log"; }\necho "$1 S60radiod" >> "%s/calls.log"\nexit 0\n' \
        "$hand" "$d" "$d" > "$d/etc/init.d/S60radiod"
    chmod 0755 "$d/etc/init.d/S60radiod"
    out=$(PATH="$d/bin:$PATH" sh "$d/remote.sh" "$d/payload.tar" 2>&1 </dev/null); rc=$?
    check "deploy stops a meshcored no init script started, before it unpacks" \
          $(contains "$out" "stopping a meshcored no init script started: $hand")
    check "and it is gone" $(wait_gone "$hand" && echo 1 || echo 0)
    check "before radiod is stopped, the order the two depend in" \
          $(grep -q 'mcd-alive-at-radiod-stop' "$d/calls.log" && echo 0 || echo 1)
    wait "$hand" 2>/dev/null
    check "a complete unpack starts every service" \
          $([ "$rc" -eq 0 ] && grep -q '^start S65meshcored' "$d/calls.log" \
            && grep -q '^start S60radiod' "$d/calls.log" && echo 1 || echo 0)
    check "radiod starts before meshcored, and meshcored before the shell" \
          $(awk '/^start S60radiod/{r=NR} /^start S65meshcored/{m=NR} /^start S90doors-shell/{s=NR} END{exit !(r && m && s && r < m && m < s)}' \
            "$d/calls.log" && echo 1 || echo 0)
    check "meshcored stops before radiod on the way down" \
          $(awk '/^stop S65meshcored/{m=NR} /^stop S60radiod/{r=NR} END{exit !(m && r && m < r)}' \
            "$d/calls.log" && echo 1 || echo 0)

    # meshcored failing to start is reported, and does not keep the shell down.
    d=$ROOT/deploy-mcdfail; p=$ROOT/payload-mcdfail
    deploy_payload "$p" "$d"
    printf '#!/bin/sh\necho "$1 S65meshcored" >> "%s/calls.log"\n[ "$1" = start ] && exit 1\nexit 0\n' "$d" \
        > "$p/etc/init.d/S65meshcored"
    chmod 0755 "$p/etc/init.d/S65meshcored"
    mkdir -p "$d"; deploy_remote "$d"; deploy_root "$d" "$p"
    out=$(PATH="$d/bin:$PATH" sh "$d/remote.sh" "$d/payload.tar" 2>&1 </dev/null); rc=$?
    check "a meshcored that will not start fails the deploy" \
          $([ "$rc" -ne 0 ] && [ "$(contains "$out" "S65meshcored did not start")" = 1 ] && echo 1 || echo 0)
    check "but the shell is started all the same" \
          $(grep -q '^start S90doors-shell' "$d/calls.log" && echo 1 || echo 0)
}

echo "initscript_test: $failed failure(s)"
exit $((failed > 0))
