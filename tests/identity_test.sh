#!/bin/bash
# The Doors release identity (ADR-005 Phase 2), and compatibility with what
# PocketOS installed before it.
#
#   - `make install` writes one release file, /etc/doors-release, and makes
#     /etc/pocketos-release a symlink to it: two names, one file, so the two
#     cannot drift apart.
#   - Installing over a PocketOS-era tree, where the old names are regular
#     files, ends in the same layout, and installing twice changes nothing.
#   - No installed directory is a symlink and every link resolves inside the
#     tree: BusyBox tar on the board cannot replace a directory with a link,
#     and a bench deploy unpacks the tree over /.
#   - deploy.sh checks for, and sends, every path `make install` creates.
#
# The readers' side (new name first, old name when the new one is absent) is
# unit-tested in tests/paths_test.c and, through system.info, in
# tests/pocketsys_test.c.
#
# Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
set -u
cd "$(dirname "$0")/.." || exit 1
failed=0
check() { if [ "$2" = "1" ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi; }

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
VERSION=$(cat VERSION)
BUILD=$(cat BUILD_ID 2>/dev/null || git rev-parse --short HEAD 2>/dev/null || echo unknown)

# A sub-make of its own, like tests/build_outputs_test.sh: the parent's
# MAKEFLAGS would bring -j and a jobserver this one cannot use.
install_into() { # <destdir>
    MAKEFLAGS= MAKELEVEL= make -s --no-print-directory install DESTDIR="$1" PREFIX=/usr \
        > "$TMP/install.log" 2>&1
}
yes_if() { if "$@"; then echo 1; else echo 0; fi; }
link_is() { [ -L "$1" ] && [ "$(readlink "$1")" = "$2" ]; }
regular() { [ -f "$1" ] && [ ! -L "$1" ]; }
mode_is() { [ "$(stat -c %a "$1" 2>/dev/null)" = "$2" ]; }
tree_paths() { (cd "$1" && find . \( -type f -o -type l \) | sed 's#^\./##' | LC_ALL=C sort); }

# ---- a fresh install -----------------------------------------------------
F="$TMP/fresh"
install_into "$F"; rc=$?
check "make install into an empty DESTDIR succeeds" "$(yes_if [ $rc -eq 0 ])"
[ $rc -eq 0 ] || tail -20 "$TMP/install.log" | sed 's/^/     /'

check "/etc/doors-release is a regular file, mode 0644" \
    "$(yes_if eval 'regular "$F/etc/doors-release" && mode_is "$F/etc/doors-release" 644')"
check "it holds the bare version on line 1 and BUILD_ID below it ($VERSION, $BUILD)" \
    "$(yes_if [ "$(cat "$F/etc/doors-release" 2>/dev/null)" = "$(printf '%s\nBUILD_ID=%s' "$VERSION" "$BUILD")" ])"
check "/etc/pocketos-release is a symlink to doors-release in the same directory" \
    "$(yes_if link_is "$F/etc/pocketos-release" doors-release)"
check "both names read the same bytes" \
    "$(yes_if cmp -s "$F/etc/doors-release" "$F/etc/pocketos-release")"
check "the tree holds exactly one release file" \
    "$(yes_if [ "$(find "$F" -name '*-release' -type f | wc -l)" = 1 ])"

# ---- over a PocketOS-era tree, and then again ---------------------------
U="$TMP/upgrade"
mkdir -p "$U/etc"
printf '0.0.9\nBUILD_ID=c6cf41b\n' > "$U/etc/pocketos-release"
install_into "$U"; rc=$?
check "make install over a PocketOS-era tree succeeds" "$(yes_if [ $rc -eq 0 ])"
check "the old regular release file became the symlink" \
    "$(yes_if link_is "$U/etc/pocketos-release" doors-release)"
check "and no second copy of the release metadata is left" \
    "$(yes_if [ "$(find "$U" -name '*-release' -type f | wc -l)" = 1 ])"
install_into "$U"; rc=$?
check "a second install succeeds" "$(yes_if [ $rc -eq 0 ])"
check "and leaves the same paths and links as a fresh install" \
    "$(yes_if [ "$(tree_paths "$U")" = "$(tree_paths "$F")" ])"
same_links=1
while IFS= read -r p; do
    if [ -L "$F/$p" ]; then
        link_is "$U/$p" "$(readlink "$F/$p")" || same_links=0
    else
        regular "$U/$p" || same_links=0
    fi
done < <(tree_paths "$F")
check "with every link and every regular file of the same kind" "$same_links"

# ---- links the board can unpack -----------------------------------------
check "no installed directory is a symlink" \
    "$(yes_if [ -z "$(find "$F" -type l -xtype d)" ])"
bad=""
while IFS= read -r l; do
    t=$(readlink "$l")
    case "$t" in /*) bad="$bad ${l#"$F"/}(absolute)" ;; esac
    [ -e "$l" ] || bad="$bad ${l#"$F"/}(dangling)"
done < <(find "$F" -type l)
check "every link is relative and resolves inside the tree${bad:+ (not:$bad)}" "$(yes_if [ -z "$bad" ])"

# ---- deploy.sh carries what the image carries ---------------------------
DEPLOY=platforms/k230/scripts/deploy.sh
checked=$(sed -n '/^for f in /,/; do$/p' "$DEPLOY" | tr ' \\;' '\n\n\n' | grep -E '^(usr|etc)/')
sent=$(sed -n '/^tar -C "\${T}"/,/SSH\[@\]/p' "$DEPLOY" | tr ' \\' '\n\n' | grep -E '^(usr|etc)/')
miss_checked=""
miss_sent=""
while IFS= read -r p; do
    printf '%s\n' "$checked" | grep -qxF -- "$p" || miss_checked="$miss_checked $p"
    printf '%s\n' "$sent" | grep -qxF -- "$p" || miss_sent="$miss_sent $p"
done < <(tree_paths "$F")
check "deploy.sh checks for every path make install creates${miss_checked:+ (missing:$miss_checked)}" \
    "$(yes_if [ -z "$miss_checked" ])"
check "deploy.sh sends every path make install creates${miss_sent:+ (missing:$miss_sent)}" \
    "$(yes_if [ -z "$miss_sent" ])"

echo "identity_test: $failed failure(s)"
exit $((failed > 0))
