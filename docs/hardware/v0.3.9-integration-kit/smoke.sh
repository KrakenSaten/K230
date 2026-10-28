#!/bin/sh
# Copyright (c) 2026 PocketOS authors.
# SPDX-License-Identifier: Apache-2.0
# Short automatic smoke check of the integration build, no radio use (RIFT is
# not opened; radiod and meshcored are only checked to keep their processes).
# Opens Vision, DeskBuddy and Relay Nine through the shell's IPC, switches
# between them, and checks the camera helper hand-over and cleanup.
set -u
K=$(cd "$(dirname "$0")" && pwd)
cd "$K" || exit 1
mkdir -p log
L=/var/lib/pocketos/log
rc=0
ok() { echo "ok   $1"; }
bad() { echo "FAIL $1"; rc=1; }
c() { doors call shell "$@" 2>&1; }
helpers() { echo "$(pidof pos-vision) | $(pidof pos-camera) | $(pidof pos-audio-effects)"; }
mark=$(date +%s)
for f in doors-shell pos-vision; do wc -l < "$L/$f.log" 2>/dev/null > "log/$f.lines" || echo 0 > "log/$f.lines"; done
SVC="$(pidof meshcored) $(pidof radiod) $(pidof netd) $(pidof sysd)"
SH=$(pidof doors-shell)

echo "== identity"
c shell.info | head -c 600; echo
P=$(pidof doors-shell); strings /proc/$P/exe 2>/dev/null | grep -qx 3ccef4e2 && ok "running shell is build 3ccef4e2" || bad "running shell build"

echo "== Vision"
c shell.open id=vision > /dev/null; sleep 8
V=$(pidof pos-vision)
[ -n "$V" ] && ok "Vision started pos-vision $V ($(tr '\0' ' ' < /proc/$V/cmdline | cut -c1-160))" || bad "Vision: no pos-vision"
grep -E "hello|proto|refus" "$L/pos-vision.log" 2>/dev/null | tail -2

echo "== Vision -> DeskBuddy (helper hand-over)"
c shell.open id=deskbuddy > /dev/null; sleep 8
n=$(pidof pos-vision | wc -w)
[ "$n" -le 1 ] && ok "at most one pos-vision after the switch ($(pidof pos-vision))" || bad "$n pos-vision processes"
[ "$(pidof pos-vision)" != "$V" ] || [ -z "$V" ] && ok "Vision's helper ended" || bad "Vision's helper $V outlived Vision"

echo "== DeskBuddy -> Home"
c shell.home > /dev/null; sleep 3
[ -z "$(pidof pos-vision pos-camera)" ] && ok "no camera helper after Home" || bad "camera helper left: $(helpers)"

echo "== Relay Nine"
c shell.open id=neon-outpost > /dev/null; sleep 4
c shell.audit > log/relay-audit.json 2>&1
[ -n "$(pidof pos-audio-effects)" ] && ok "pos-audio-effects runs while Relay Nine is open" || echo "note: no pos-audio-effects (sound may be off)"
c shell.home > /dev/null; sleep 3
[ -z "$(pidof pos-audio-effects)" ] && ok "pos-audio-effects stopped after Home" || bad "pos-audio-effects left: $(pidof pos-audio-effects)"

echo "== Vision open/close x3"
for i in 1 2 3; do
	c shell.open id=vision > /dev/null; sleep 6; [ -n "$(pidof pos-vision)" ] || bad "cycle $i: no pos-vision"
	c shell.home > /dev/null; sleep 3; [ -z "$(pidof pos-vision)" ] || bad "cycle $i: pos-vision left"
done
ok "three Vision cycles done"

echo "== after"
[ "$(pidof doors-shell)" = "$SH" ] && ok "same shell process $SH (no restart, no crash)" || bad "shell changed: $SH -> $(pidof doors-shell)"
ps | grep -q '[p]os-supervise doors-shell' && ok "shell supervised" || bad "shell not supervised"
[ "$(pidof meshcored) $(pidof radiod) $(pidof netd) $(pidof sysd)" = "$SVC" ] && ok "other services: same processes" || bad "another service changed"
[ -z "$(pidof pos-vision pos-camera pos-audio-effects)" ] && ok "no helper left" || bad "helpers left: $(helpers)"
grep VmRSS /proc/$SH/status
for f in doors-shell pos-vision; do
	s=$(cat log/$f.lines); tail -n +$((s + 1)) "$L/$f.log" 2>/dev/null > "log/$f.new"
	e=$(grep -ciE " (error|fatal)|crash|version|refus" "log/$f.new")
	echo "$f.log: $(wc -l < log/$f.new) new lines, $e error/version lines"; grep -iE " (error|fatal)|crash|version|refus" "log/$f.new" | head -6
done
[ $rc -eq 0 ] && echo "SMOKE: OK" || echo "SMOKE: FAILED (rc=$rc)"
exit $rc
