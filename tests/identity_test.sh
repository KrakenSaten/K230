#!/bin/bash
# The Doors release and CLI identity (ADR-005 Phase 2), and compatibility with
# what PocketOS installed before it.
#
#   - `make install` writes one release file, /etc/doors-release, and makes
#     /etc/pocketos-release a symlink to it: two names, one file, so the two
#     cannot drift apart.
#   - The command-line tool is installed once, as /usr/bin/doors, with
#     /usr/bin/pos a symlink to it. Invoked as doors it says Doors; invoked as
#     pos it prints what pos printed through v0.0.9, byte for byte where this
#     test pins it. The pos-* helpers keep their names.
#   - The third-party notices, the only shared data, live in /usr/share/doors;
#     /usr/share/pocketos stays a directory with a link to them in it.
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

check "/usr/bin/doors is the CLI binary, mode 0755" \
    "$(yes_if eval 'regular "$F/usr/bin/doors" && mode_is "$F/usr/bin/doors" 755 && cmp -s "$F/usr/bin/doors" tools/pos/pos')"
check "/usr/bin/pos is a symlink to doors in the same directory" \
    "$(yes_if link_is "$F/usr/bin/pos" doors)"
missing=""
for h in pos-hwcheck pos-spixfer pos-wave pos-supervise; do
    regular "$F/usr/bin/$h" || missing="$missing $h"
done
check "the pos-* helpers keep their names${missing:+ (missing:$missing)}" "$(yes_if [ -z "$missing" ])"
check "and have no doors-* twins" "$(yes_if [ -z "$(find "$F" -name 'doors-*' ! -name doors-release)" ])"

check "/usr/share/doors/THIRD_PARTY_NOTICES.txt is the notices file, mode 0644" \
    "$(yes_if eval 'regular "$F/usr/share/doors/THIRD_PARTY_NOTICES.txt" && mode_is "$F/usr/share/doors/THIRD_PARTY_NOTICES.txt" 644 && cmp -s "$F/usr/share/doors/THIRD_PARTY_NOTICES.txt" THIRD_PARTY_NOTICES.txt')"
check "/usr/share/pocketos stays a directory, not a link" \
    "$(yes_if eval '[ -d "$F/usr/share/pocketos" ] && [ ! -L "$F/usr/share/pocketos" ]')"
check "holding THIRD_PARTY_NOTICES.txt as a link to ../doors/THIRD_PARTY_NOTICES.txt" \
    "$(yes_if link_is "$F/usr/share/pocketos/THIRD_PARTY_NOTICES.txt" ../doors/THIRD_PARTY_NOTICES.txt)"
check "and nothing else lives in either shared directory but the shell's art" \
    "$(yes_if [ "$(find "$F/usr/share" -mindepth 2 ! -path "$F/usr/share/doors/ui*" | LC_ALL=C sort | tr '\n' ' ')" = \
        "$F/usr/share/doors/THIRD_PARTY_NOTICES.txt $F/usr/share/pocketos/THIRD_PARTY_NOTICES.txt " ])"
art_ok=1
for a in ui/assets/doors/*.bin; do
    t="$F/usr/share/doors/ui/$(basename "$a")"
    { regular "$t" && mode_is "$t" 644 && cmp -s "$t" "$a"; } || art_ok=0
done
check "/usr/share/doors/ui holds exactly the committed DOORS art, mode 0644" \
    "$(yes_if eval '[ "$art_ok" = 1 ] && [ "$(ls "$F/usr/share/doors/ui" | wc -l)" = "$(ls ui/assets/doors/*.bin | wc -l)" ]')"

# ---- over a PocketOS-era tree, and then again ---------------------------
U="$TMP/upgrade"
mkdir -p "$U/etc" "$U/usr/bin" "$U/usr/share/pocketos"
printf '0.0.9\nBUILD_ID=c6cf41b\n' > "$U/etc/pocketos-release"
printf '#!/bin/sh\necho pos 0.0.9\n' > "$U/usr/bin/pos"
chmod 0755 "$U/usr/bin/pos"
printf 'PocketOS third-party notices, as v0.0.9 shipped them\n' > "$U/usr/share/pocketos/THIRD_PARTY_NOTICES.txt"
install_into "$U"; rc=$?
check "make install over a PocketOS-era tree succeeds" "$(yes_if [ $rc -eq 0 ])"
check "the old regular release file became the symlink" \
    "$(yes_if link_is "$U/etc/pocketos-release" doors-release)"
check "the old pos binary became the symlink to doors" "$(yes_if link_is "$U/usr/bin/pos" doors)"
check "the old notices copy became the link, so it cannot go stale" \
    "$(yes_if eval 'link_is "$U/usr/share/pocketos/THIRD_PARTY_NOTICES.txt" ../doors/THIRD_PARTY_NOTICES.txt && cmp -s "$U/usr/share/pocketos/THIRD_PARTY_NOTICES.txt" THIRD_PARTY_NOTICES.txt')"
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
    case "$(realpath -m "$l")" in "$(realpath "$F")"/*) ;; *) bad="$bad ${l#"$F"/}(outside)" ;; esac
done < <(find "$F" -type l)
check "every link is relative and resolves inside the tree${bad:+ (not:$bad)}" "$(yes_if [ -z "$bad" ])"

# ---- doors and pos, run from the installed tree -------------------------
DOORS="$F/usr/bin/doors"
POSL="$F/usr/bin/pos"
out() { "$@" 2>&1; }   # stdout and stderr, whatever the exit code
check "doors version says Doors" \
    "$(yes_if [ "$(out "$DOORS" version)" = "Doors $VERSION (build $BUILD)" ])"
check "pos version is unchanged" \
    "$(yes_if [ "$(out "$POSL" version)" = "pos $VERSION (build $BUILD)" ])"
check "the build output run by its own path answers as pos" \
    "$(yes_if [ "$(out tools/pos/pos version)" = "pos $VERSION (build $BUILD)" ])"
mkdir -p "$TMP/bin"
ln -s "$DOORS" "$TMP/bin/pos-old"
check "any name other than doors answers as pos" \
    "$(yes_if [ "$(out "$TMP/bin/pos-old" version)" = "pos $VERSION (build $BUILD)" ])"
check "doors on PATH, called by its bare name, says Doors" \
    "$(yes_if [ "$(PATH="$F/usr/bin:$PATH" out doors version)" = "Doors $VERSION (build $BUILD)" ])"
check "pos on PATH, called by its bare name, is unchanged" \
    "$(yes_if [ "$(PATH="$F/usr/bin:$PATH" out pos version)" = "pos $VERSION (build $BUILD)" ])"

# pos's own lines, as v0.0.9 printed them. Scripts and bench sheets quote these.
pins_ok=1
pin() { # <expected line> <command...>
    local want=$1; shift
    out "$POSL" "$@" | grep -qxF -- "$want" || { pins_ok=0; echo "     pos $*: no line '$want'"; }
}
pin "usage: pos <command> [subcommand]" help
pin "  version               print pos version" help
pin "  radio <command>       talk to radiod (pos radio help)" help
pin "usage: pos radio <command>" radio help
pin "usage: pos wifi <command>" wifi bogus
pin "  connect <ssid> [options]    join; passphrase from stdin (pos wifi connect help)" wifi bogus
pin "usage: pos call <service> <method> [key=value ...]" call
pin "  e.g. pos call sysd system.info" call
pin "usage: pos app list | start <id> | home" app bogus
pin "usage: pos shell info | screenshot <path.png> | theme <id> [normal|outdoor|night]" shell
pin "                 | brightness [10..100]" shell
pin "usage: pos logs                     list log files" logs --bogus
pin "       pos logs <name> [-n LINES]   tail a service log (default 50 lines)" logs --bogus
pin "       pos logs --crash <file>      print a crash report" logs --bogus
check "pos usage text is unchanged from v0.0.9" "$pins_ok"

# doors prints the same text under its own name. The shell usage's second
# line is indented to stay under the first, two columns further for doors.
same_ok=1
for args in "help" "radio help" "wifi bogus" "call" "app bogus" "logs --bogus"; do
    # shellcheck disable=SC2086
    want=$(out "$POSL" $args | sed -E 's/\bpos\b/doors/g')
    # shellcheck disable=SC2086
    [ "$(out "$DOORS" $args)" = "$want" ] || { same_ok=0; echo "     doors $args differs from pos $args"; }
done
check "doors prints pos's usage text with its own name" "$same_ok"
check "doors shell usage keeps its continuation line aligned" \
    "$(yes_if [ "$(out "$DOORS" shell)" = "$(printf '%s\n%s' \
        'usage: doors shell info | screenshot <path.png> | theme <id> [normal|outdoor|night]' \
        '                   | brightness [10..100]')" ])"
check "doors and pos exit with the same status on a usage error" \
    "$(yes_if [ "$("$DOORS" radio help >/dev/null 2>&1; echo $?)" = "$("$POSL" radio help >/dev/null 2>&1; echo $?)" ])"

# Errors name the tool the way it was called.
E="$TMP/empty-run"
mkdir -p "$E"
errs_ok=1
expect() { # <name> <expected substring> <command...>
    local name=$1 want=$2 bin; shift 2
    [ "$name" = doors ] && bin=$DOORS || bin=$POSL
    POCKETOS_RUNTIME_DIR="$E" POCKETOS_LOG_DIR="$E/nolog" out "$bin" "$@" | grep -qF -- "$want" ||
        { errs_ok=0; echo "     $name $*: no '$want'"; }
}
for n in doors pos; do
    expect $n "$n system: cannot connect to sysd" system status
    expect $n "$n radio: cannot connect to $E/radiod.sock" radio info
    expect $n "$n wifi: cannot connect to $E/netd.sock" wifi status
    expect $n "$n: cannot connect to $E/shell.sock" app list
    expect $n "$n logs: cannot open $E/nolog" logs
    expect $n "$n call: expected key=value, got nokv" call sysd system.info nokv
    expect $n "$n: brightness takes a whole percentage" shell brightness x
done
check "error messages name doors or pos, as invoked" "$errs_ok"

# The release line of `system info`, against the three layouts a card can
# have. The tool reads the real /etc, so a private mount namespace lends it a
# scratch one; a host that cannot make one skips this part and says so.
if unshare -rm true 2>/dev/null; then
    ETC="$TMP/etc"
    run_info() { # <layout> <name>: the release line doors/pos print for that layout
        rm -rf "$ETC"; mkdir -p "$ETC"
        [ -f /etc/ld.so.cache ] && cp /etc/ld.so.cache "$ETC/"
        case "$1" in
            old) printf '0.0.9\nBUILD_ID=c6cf41b\n' > "$ETC/pocketos-release" ;;
            new) printf '0.0.10\nBUILD_ID=d00r5aa\n' > "$ETC/doors-release" ;;
            link) printf '0.0.10\nBUILD_ID=d00r5aa\n' > "$ETC/doors-release"
                  ln -s doors-release "$ETC/pocketos-release" ;;
        esac
        unshare -rm sh -c 'mount --bind "$1" /etc && exec "$2" system info' sh "$ETC" "$F/usr/bin/$2" 2>&1 |
            grep -E '^(doors|pocketos) '
    }
    line() { printf '%-16s %s' "$1" "$2"; }
    check "only /etc/pocketos-release: doors system info reads it" \
        "$(yes_if [ "$(run_info old doors)" = "$(line doors '0.0.9 (build c6cf41b)')" ])"
    check "only /etc/pocketos-release: pos system info reads it, under its old label" \
        "$(yes_if [ "$(run_info old pos)" = "$(line pocketos '0.0.9 (build c6cf41b)')" ])"
    check "only /etc/doors-release: doors system info reads it" \
        "$(yes_if [ "$(run_info new doors)" = "$(line doors '0.0.10 (build d00r5aa)')" ])"
    check "only /etc/doors-release: pos system info reads it" \
        "$(yes_if [ "$(run_info new pos)" = "$(line pocketos '0.0.10 (build d00r5aa)')" ])"
    check "doors-release plus the compatibility link: one line, from the new file" \
        "$(yes_if [ "$(run_info link doors)" = "$(line doors '0.0.10 (build d00r5aa)')" ])"
    check "and pos reads the same" \
        "$(yes_if [ "$(run_info link pos)" = "$(line pocketos '0.0.10 (build d00r5aa)')" ])"
else
    echo "note: no private mount namespace here (unshare -rm); the system info release-line checks were skipped."
    echo "      The reader itself is covered by tests/paths_test.c and tests/pocketsys_test.c."
fi

# ---- deploy.sh carries what the image carries ---------------------------
DEPLOY=platforms/k230/scripts/deploy.sh
checked=$(sed -n '/^for f in /,/; do$/p' "$DEPLOY" | tr ' \\;' '\n\n\n' | grep -E '^(usr|etc)/')
sent=$(sed -n '/^tar -C "\${T}"/,/SSH\[@\]/p' "$DEPLOY" | tr ' \\' '\n\n' | grep -E '^(usr|etc)/')
miss_checked=""
miss_sent=""
# A directory in either list carries every file under it (usr/share/doors/ui,
# the shell's art: tar sends a directory whole).
covered() { # <list> <path>
    local d=$2
    while :; do
        printf '%s\n' "$1" | grep -qxF -- "$d" && return 0
        case "$d" in */*) d=${d%/*} ;; *) return 1 ;; esac
    done
}
while IFS= read -r p; do
    covered "$checked" "$p" || miss_checked="$miss_checked $p"
    covered "$sent" "$p" || miss_sent="$miss_sent $p"
done < <(tree_paths "$F")
check "deploy.sh checks for every path make install creates${miss_checked:+ (missing:$miss_checked)}" \
    "$(yes_if [ -z "$miss_checked" ])"
check "deploy.sh sends every path make install creates${miss_sent:+ (missing:$miss_sent)}" \
    "$(yes_if [ -z "$miss_sent" ])"

echo "identity_test: $failed failure(s)"
exit $((failed > 0))
