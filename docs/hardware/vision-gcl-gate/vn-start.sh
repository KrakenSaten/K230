#!/bin/sh
# Copyright (c) 2026 PocketOS authors.
# SPDX-License-Identifier: Apache-2.0
# PR #112 Vision device session: hand the panel from the installed,
# supervised doors-shell to this kit's doors-shell and pos-vision (a
# protocol-3 pair, POCKETOS_VISION_HELPER), opened in Vision.
#   vn-start.sh <label> <mode> <detector> <rotation>
#     mode: detect|color|trace|...  detector: r0|upstream  rotation: portrait|landscape
#
# The test shell reads and writes only the kit's copies: POCKETOS_CONFIG_DIR
# is ./cfg (a copy of /etc/pocketos) and POCKETOS_STATE_DIR ./state (copies
# of Vision's and DeskBuddy's directories), so no installed setting, Vision
# setting or DeskBuddy preference is written. Nothing under /usr, /etc or
# /var/lib is changed. The first start records the baseline vn-restore.sh
# checks against. Running it again replaces the test shell.
set -u
K=$(cd "$(dirname "$0")" && pwd)
cd "$K" || exit 1
LABEL=${1:?label}; MODE=${2:?mode}; DET=${3:?detector}; ROT=${4:?rotation}
fail() { echo "START: $*; nothing further changed" >&2; exit 1; }

echo "== kit"
sha256sum -c SHA256SUMS || fail "kit files do not match SHA256SUMS"

echo "== baseline"
mkdir -p backup log shots cfg state
if [ ! -s backup/installed.sha256 ]; then
	# Another session owning the panel or the camera is not ours to interrupt.
	for p in $("$K/vn-status.sh" panel-owners); do
		[ "$(readlink /proc/$p/exe)" = /usr/bin/doors-shell ] || fail "panel held by $p $(readlink /proc/$p/exe)"
	done
	[ -z "$(pidof pos-vision pos-camera)" ] || fail "a camera helper is running: $(pidof pos-vision pos-camera)"
	sha256sum /usr/bin/doors-shell /usr/bin/pos-vision /usr/lib/liblvgl.so.9.5.0 \
		/usr/share/doors/vision/*.kmodel > backup/installed.sha256 || fail "cannot hash the installed files"
	for f in /etc/pocketos/settings.conf /var/lib/pocketos/vision/settings.v1 /var/lib/pocketos/vision/owner.v1 \
	         /var/lib/pocketos/deskbuddy/prefs.v1 /var/lib/pocketos/deskbuddy/guard.v1; do
		if [ -e "$f" ]; then echo "present $(sha256sum "$f")"; else echo "absent $f"; fi
	done > backup/preserved.txt
	find /etc/pocketos /var/lib/pocketos -type f | sort | xargs sha256sum > backup/state-before.txt 2>/dev/null
	echo "$(pidof meshcored) $(pidof radiod)" > backup/services-before.txt
	cp -a /etc/pocketos/. cfg/
	for d in vision deskbuddy; do [ -d /var/lib/pocketos/$d ] && cp -a /var/lib/pocketos/$d state/; done
	sync
fi
cat backup/preserved.txt

echo "== the scenario's settings (kit copies only)"
mkdir -p state/vision
if [ -s state/vision/settings.v1 ]; then
	sed -e "s/^mode=.*/mode=$MODE/" -e "s/^detector=.*/detector=$DET/" state/vision/settings.v1 > state/vision/settings.v1.tmp
else
	printf 'mode=%s\ndetector=%s\n' "$MODE" "$DET" > state/vision/settings.v1.tmp
fi
grep -q '^mode=' state/vision/settings.v1.tmp || echo "mode=$MODE" >> state/vision/settings.v1.tmp
grep -q '^detector=' state/vision/settings.v1.tmp || echo "detector=$DET" >> state/vision/settings.v1.tmp
mv state/vision/settings.v1.tmp state/vision/settings.v1
chmod 600 state/vision/settings.v1
grep -v '^display_rotation=' cfg/settings.conf > cfg/settings.conf.tmp; echo "display_rotation=$ROT" >> cfg/settings.conf.tmp
mv cfg/settings.conf.tmp cfg/settings.conf
grep -E '^(mode|detector)=' state/vision/settings.v1; grep display_rotation cfg/settings.conf

echo "== stop the shell on the panel"
"$K/vn-stop-test.sh"
if [ -n "$(pidof doors-shell)" ]; then
	/etc/init.d/S90doors-shell stop || fail "S90doors-shell stop failed"
	sleep 1
fi
[ -z "$(pidof doors-shell pocketos-shell)" ] || fail "a shell is still running: $(pidof doors-shell pocketos-shell)"
ps | grep '[p]os-supervise doors-shell' && fail "the doors-shell supervisor is still running"
[ -z "$(pidof pos-vision)" ] || fail "a pos-vision is still running: $(pidof pos-vision)"

echo "== start the test shell ($LABEL)"
env HOME=/root K230_LVGL_DRM_STAGING=1 POCKETOS_LOG_DIR="$K/log" POCKETOS_LOG_STDERR=0 \
	POCKETOS_CONFIG_DIR="$K/cfg" POCKETOS_STATE_DIR="$K/state" POCKETOS_VISION_HELPER="$K/pos-vision" \
	setsid "$K/doors-shell" --open vision > "$K/log/stdio-$LABEL.log" 2>&1 < /dev/null &
sleep 4
P=""
for p in $(pidof doors-shell); do [ "$(readlink /proc/$p/exe)" = "$K/doors-shell" ] && P=$p; done
[ -n "$P" ] || { echo "START: the test shell exited; log follows"; tail -30 "$K/log/stdio-$LABEL.log"; echo "Run ./vn-restore.sh"; exit 1; }
echo "$P" > "$K/test-shell.pid"
"$K/vn-status.sh"
echo "START: test shell $P is on the panel in Vision ($MODE, $DET, $ROT). Restore with ./vn-restore.sh"
