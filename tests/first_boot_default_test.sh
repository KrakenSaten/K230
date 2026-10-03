#!/bin/bash
# The first boot of a freshly flashed card is Doors', not the vendor
# launcher's.
#
# Up to v0.3.0 the patched vendor launcher (S99zz_k230_phone_ui) was on and
# S90doors-shell off unless /etc/default said otherwise, and no image ships
# those files, so every fresh card booted into the LILYGO launcher until
# someone wrote them by hand. There is no first-boot flag involved: the
# LILYGO BSP removes the SDK's /first_boot_flag and S00resizemmc. The fix is
# the two defaults, checked here:
#   - the real [3b/5] block of apply_to_sdk.sh, run on a launcher script,
#     leaves it off by default and on only with ENABLE=1 in its file (also
#     when the script was patched by an older apply, with the old default);
#   - S90doors-shell starts with no settings file at all, and still yields
#     to the launcher when a unit switches it back on.
# The block runs on a stand-in with the vendor script's two anchor lines, and
# on the vendor script itself when a vendor checkout is present
# ($POCKETOS_VENDOR_DIR or vendor/T-Display-K230).
#
# Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
set -u
cd "$(dirname "$0")/.." || exit 1
REPO=$(pwd)
APPLY=platforms/k230/scripts/apply_to_sdk.sh
S90_SRC=platforms/k230/rootfs_overlay/etc/init.d/S90doors-shell
failed=0
check() { if [ "$2" = "1" ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi; }
contains() { case "$1" in *"$2"*) echo 1 ;; *) echo 0 ;; esac; }

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# ---- the launcher switch, as apply_to_sdk.sh writes it -------------------

# The block itself, from its echo to the next step's, so a change to it is
# what gets tested rather than a copy of it.
sed -n '/^echo "\[3b\/5\]/,/^echo "\[3c\/5\]/p' "$APPLY" | sed '$d' > "$TMP/block.sh"
check "the panel-switch block is found in apply_to_sdk.sh" \
    "$(grep -q 'S99zz_k230_phone_ui' "$TMP/block.sh" && echo 1 || echo 0)"

# The vendor script's shape where the block anchors: DRM_NODE= at the top,
# the "Starting" printf at the head of start().
stand_in() {
    cat > "$1" <<'EOF'
#!/bin/sh
DAEMON=/root/app/k230_phone_ui/k230_phone_ui
PIDFILE=/var/run/k230_phone_ui.pid
DRM_NODE=/dev/dri/card0

start()
{
	printf "Starting k230_phone_ui: "

	if [ ! -x "$DAEMON" ]; then
		echo "missing"
		return 0
	fi
	echo "OK"
}

case "$1" in
	start) start ;;
esac
exit 0
EOF
}

# patch <script>: run the block on it the way apply_to_sdk.sh does.
patch_launcher() {
    local sdk="$TMP/sdk"
    rm -rf "$sdk"
    mkdir -p "$sdk/buildroot-overlay/board/canaan/k230-soc/rootfs_overlay/etc/init.d"
    cp "$1" "$sdk/buildroot-overlay/board/canaan/k230-soc/rootfs_overlay/etc/init.d/S99zz_k230_phone_ui"
    ( SDK_DIR="$sdk"; set -e; . "$TMP/block.sh" ) > "$TMP/apply.log" 2>&1 || return 1
    cp "$sdk/buildroot-overlay/board/canaan/k230-soc/rootfs_overlay/etc/init.d/S99zz_k230_phone_ui" "$1"
}

# run_launcher <script> <root>: start it with /etc/default moved under <root>.
run_launcher() {
    sed "s#/etc/default/#$2/etc/default/#g" "$1" > "$2/S99"
    sh "$2/S99" start 2>&1
}

launcher_cases() { # <label> <script>
    local label="$1" s="$2" root="$TMP/root-$1" out
    mkdir -p "$root/etc/default"
    if ! patch_launcher "$s"; then
        check "$label: the block patches the launcher script" 0
        sed 's/^/    /' "$TMP/apply.log"
        return
    fi
    check "$label: the block patches the launcher script" 1
    check "$label: the default it writes is off" \
        "$(grep -q '^ENABLE=0$' "$s" && ! grep -q '^ENABLE=1$' "$s" && echo 1 || echo 0)"
    out=$(run_launcher "$s" "$root")
    check "$label: with no settings file the launcher does not start" \
        "$(contains "$out" "disabled (")"
    printf 'ENABLE=1\n' > "$root/etc/default/k230_phone_ui"
    out=$(run_launcher "$s" "$root")
    check "$label: ENABLE=1 in its file still brings it back" \
        "$([ "$(contains "$out" "disabled (")" = 0 ] && echo 1 || echo 0)"
    printf 'ENABLE=0\n' > "$root/etc/default/k230_phone_ui"
    out=$(run_launcher "$s" "$root")
    check "$label: ENABLE=0 in its file keeps it down" "$(contains "$out" "disabled (")"
    cp "$s" "$s.once"
    patch_launcher "$s"
    check "$label: a second apply changes nothing" \
        "$(cmp -s "$s" "$s.once" && echo 1 || echo 0)"
}

stand_in "$TMP/S99.stand-in"
launcher_cases stand-in "$TMP/S99.stand-in"

# A copy an older apply left patched carries the old default, ENABLE=1.
stand_in "$TMP/S99.old"
patch_launcher "$TMP/S99.old"
sed -i 's/^ENABLE=0$/ENABLE=1/' "$TMP/S99.old"
patch_launcher "$TMP/S99.old"
check "an older apply's default (on) is corrected to off" \
    "$(grep -q '^ENABLE=0$' "$TMP/S99.old" && ! grep -q '^ENABLE=1$' "$TMP/S99.old" && echo 1 || echo 0)"

VENDOR_S99="${POCKETOS_VENDOR_DIR:-$REPO/vendor/T-Display-K230}/k230_launcher/rootfs_overlay/etc/init.d/S99zz_k230_phone_ui"
if [ -f "$VENDOR_S99" ]; then
    cp "$VENDOR_S99" "$TMP/S99.vendor"
    launcher_cases vendor "$TMP/S99.vendor"
else
    echo "skip the vendor launcher script itself (no vendor checkout at ${VENDOR_S99%/k230_launcher/*})"
fi

# ---- S90doors-shell with no settings file ---------------------------------

ROOT="$TMP/s90"
mkdir -p "$ROOT/etc/default" "$ROOT/usr/bin" "$ROOT/run/pocketos" "$ROOT/var/run" \
         "$ROOT/var/lib/pocketos/log" "$ROOT/dev/dri"
: > "$ROOT/dev/dri/card0"
# A supervisor stand-in that records it was started, then waits to be stopped.
printf '#!/bin/sh\nenv > "%s/shell.env"\nexec sleep 30\n' "$ROOT" > "$ROOT/usr/bin/pos-supervise"
printf '#!/bin/sh\nexit 0\n' > "$ROOT/usr/bin/doors-shell"
chmod 0755 "$ROOT/usr/bin/pos-supervise" "$ROOT/usr/bin/doors-shell"
sed -e "s#/etc/default/#$ROOT/etc/default/#g" -e "s#/etc/init.d/#$ROOT/etc/init.d/#g" \
    -e "s#/usr/bin/#$ROOT/usr/bin/#g" -e "s#/var/run/#$ROOT/var/run/#g" \
    -e "s#/run/pocketos#$ROOT/run/pocketos#g" -e "s#/var/lib/pocketos#$ROOT/var/lib/pocketos#g" \
    -e "s#/dev/dri/card0#$ROOT/dev/dri/card0#g" \
    "$S90_SRC" > "$ROOT/S90"
check "the S90 copy has every path moved into the test root" \
    "$(grep -v '^\s*#' "$ROOT/S90" | grep -E '(^|[ "=])/(etc|usr|var|run|dev)/' | grep -vq "$ROOT" && echo 0 || echo 1)"

s90() { sh "$ROOT/S90" "$@" 2>&1; }
started() { local n=0; while [ ! -s "$ROOT/shell.env" ] && [ "$n" -lt 30 ]; do sleep 0.1; n=$((n + 1)); done; [ -s "$ROOT/shell.env" ]; }

out=$(s90 start)
check "with no settings file at all, S90 starts the shell (a fresh card boots Doors)" \
    "$([ "$(contains "$out" "OK")" = 1 ] && started && echo 1 || echo 0)"
s90 stop >/dev/null; rm -f "$ROOT/shell.env"

printf 'ENABLE=1\n' > "$ROOT/etc/default/k230_phone_ui"
out=$(s90 start)
check "a unit that switches the launcher back on keeps it: S90 yields" \
    "$(contains "$out" "owns the panel")"
check "and starts nothing" "$([ ! -e "$ROOT/shell.env" ] && echo 1 || echo 0)"
rm -f "$ROOT/etc/default/k230_phone_ui"

printf 'ENABLE=0\n' > "$ROOT/etc/default/doors-shell"
out=$(s90 start)
check "a unit that switches the shell off keeps it off" \
    "$([ "$(contains "$out" "disabled")" = 1 ] && [ ! -e "$ROOT/shell.env" ] && echo 1 || echo 0)"
rm -f "$ROOT/etc/default/doors-shell"

echo "first_boot_default_test: $failed failure(s)"
exit $((failed > 0))
