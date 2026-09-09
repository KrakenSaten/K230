#!/bin/bash
# PocketOS init-script tests (M2).
#
# The boot path had no automated test: S60radiod and S90pocketos-shell were
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
S60_SRC=$OVERLAY/etc/init.d/S60radiod
S90_SRC=$OVERLAY/etc/init.d/S90pocketos-shell
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
    for f in "$S50_SRC" "$S60_SRC" "$S90_SRC"; do
        check "image-critical: $(basename "$f") recorded 100755" \
              $([ "$(mode_of "$f")" = "100755" ] && echo 1 || echo 0)
    done
    # Overlay data files must NOT be executable.
    for f in $OVERLAY/etc/default/telnet $OVERLAY/etc/pocketos/settings.conf; do
        check "image-critical: $(basename "$f") recorded 100644" \
              $([ "$(mode_of "$f")" = "100644" ] && echo 1 || echo 0)
    done
    # Hygiene: scripts meant to be run directly by a developer or operator.
    for f in tools/supervise/pos-supervise tools/hwcheck/hwcheck.sh \
             tools/design/gen_fonts.sh platforms/k230/scripts/apply_to_sdk.sh \
             platforms/k230/scripts/build_image.sh platforms/k230/scripts/deploy.sh; do
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
make_daemon "$ROOT/usr/bin/pocketos-shell" "$ROOT/shell.env"

# Rewrite the absolute paths of an init script into the fake root. /dev/null is
# deliberately left alone.
# The runtime directory is rewritten first and only where a separator (space
# or "=") precedes it: "/var/run/pocketos-shell-supervise.pid" contains the literal string
# "/run/pocketos", so an unanchored rule would rewrite the middle of the shell
# service's pid file path and the script would then write it nowhere.
rewrite() { # <source> <destination>
    sed -e "s#\([[:space:]=]\)/run/pocketos#\1$ROOT/run/pocketos#g" \
        -e "s#/usr/bin/#$ROOT/usr/bin/#g" \
        -e "s#/usr/sbin/#$ROOT/usr/sbin/#g" \
        -e "s#/var/run#$ROOT/var/run#g" \
        -e "s#/var/lib/pocketos#$ROOT/var/lib/pocketos#g" \
        -e "s#/etc/default#$ROOT/etc/default#g" \
        -e "s#/dev/dri/card0#$ROOT/dev/dri/card0#g" \
        -e "s#HOME=/root #HOME=$ROOT/root #g" \
        "$1" > "$2"
    chmod 0755 "$2"
}
rewrite "$REPO/$S50_SRC" "$ROOT/etc/init.d/S50sysd"
rewrite "$REPO/$S60_SRC" "$ROOT/etc/init.d/S60radiod"
rewrite "$REPO/$S90_SRC" "$ROOT/etc/init.d/S90pocketos-shell"

# The rewrite must be complete: any surviving system path would make the test
# lie about what it exercised (or touch the host).
leaked=$(grep -nE '(^|[^A-Za-z0-9_/])/(etc|var|usr|run)/' "$ROOT/etc/init.d/S50sysd" \
                  "$ROOT/etc/init.d/S60radiod" \
                  "$ROOT/etc/init.d/S90pocketos-shell" | grep -v "$ROOT" | grep -v '^\s*#')
check "path rewrite left no system path behind" $([ -z "$leaked" ] && echo 1 || echo 0)
[ -n "$leaked" ] && echo "$leaked" | head -5

# A rule that matches inside an already-rewritten path produces "$ROOT/var$ROOT/run/..."
doubled=$(grep -n "$ROOT[^ ]*$ROOT" "$ROOT/etc/init.d/S50sysd" "$ROOT/etc/init.d/S60radiod" \
               "$ROOT/etc/init.d/S90pocketos-shell")
check "path rewrite did not nest one prefix inside another" $([ -z "$doubled" ] && echo 1 || echo 0)
[ -n "$doubled" ] && echo "$doubled" | head -5

S50="$ROOT/etc/init.d/S50sysd"
S60="$ROOT/etc/init.d/S60radiod"
S90="$ROOT/etc/init.d/S90pocketos-shell"

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

# ---- S90pocketos-shell --------------------------------------------------

out=$("$S90" start 2>&1)
check "S90 is disabled by default" $(contains "$out" "disabled")
check "S90 disabled starts nothing" $([ ! -e "$ROOT/shell.env" ] && echo 1 || echo 0)

printf 'ENABLE=1\n' > "$ROOT/etc/default/pocketos-shell"
printf 'ENABLE=1\n' > "$ROOT/etc/default/k230_phone_ui"
out=$("$S90" start 2>&1)
check "S90 refuses while the vendor launcher is enabled" $(contains "$out" "owns the panel")
check "S90 refusal names the launcher switch" $(contains "$out" "/etc/default/k230_phone_ui")
check "S90 refusal starts nothing" $([ ! -e "$ROOT/shell.env" ] && echo 1 || echo 0)

printf 'ENABLE=0\n' > "$ROOT/etc/default/k230_phone_ui"
sleep 600 &
LAUNCHER=$!
echo "$LAUNCHER" > "$ROOT/var/run/k230_phone_ui.pid"
out=$("$S90" start 2>&1)
check "S90 refuses while the vendor launcher is running" $(contains "$out" "is running")
check "S90 running-launcher refusal starts nothing" $([ ! -e "$ROOT/shell.env" ] && echo 1 || echo 0)
kill "$LAUNCHER" 2>/dev/null
rm -f "$ROOT/var/run/k230_phone_ui.pid"

printf 'ENABLE=1\nPOCKETOS_DRM_ROTATION=180\n' > "$ROOT/etc/default/pocketos-shell"
out=$("$S90" start 2>&1)
check "S90 starts when it owns the panel" $(contains "$out" "OK")
check "S90 starts the shell" $(wait_for "$ROOT/shell.env" && echo 1 || echo 0)
SHPID=$(pidof_file "$ROOT/var/run/pocketos-shell-supervise.pid")
check "S90 start writes the supervise pid file" $([ -n "$SHPID" ] && echo 1 || echo 0)
check "S90 start leaves the supervisor running" $(alive "$SHPID" && echo 1 || echo 0)
check "S90 start records the daemon pid in the runtime dir" \
      $([ -s "$ROOT/run/pocketos/pocketos-shell.pid" ] && echo 1 || echo 0)
check "S90 exports the persistent log directory" \
      $(grep -q "^POCKETOS_LOG_DIR=$ROOT/var/lib/pocketos/log$" "$ROOT/shell.env" && echo 1 || echo 0)
check "S90 exports the vendor staging default" \
      $(grep -q '^K230_LVGL_DRM_STAGING=1$' "$ROOT/shell.env" && echo 1 || echo 0)
check "S90 exports a bench override that was set" \
      $(grep -q '^POCKETOS_DRM_ROTATION=180$' "$ROOT/shell.env" && echo 1 || echo 0)
check "S90 does not export a bench override that was not set" \
      $(grep -q '^POCKETOS_TOUCH_CALIB=' "$ROOT/shell.env" && echo 0 || echo 1)

out=$("$S90" stop 2>&1)
check "S90 stop reports OK" $(contains "$out" "OK")
check "S90 stop removes the supervise pid file" \
      $([ ! -f "$ROOT/var/run/pocketos-shell-supervise.pid" ] && echo 1 || echo 0)
check "S90 stop ends the supervisor" $(wait_gone "$SHPID" && echo 1 || echo 0)

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

echo "initscript_test: $failed failure(s)"
exit $((failed > 0))
