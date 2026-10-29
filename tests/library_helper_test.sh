#!/bin/bash
# pos-library against a real Kiwix archive (docs/apps/LIBRARY.md
# "Validation"): the protocol the Library screen speaks, a search that returns
# a real article, an article with headings, links in and out of the
# collection and decoded pictures, the collection's metadata, every way of
# not having a collection (none, missing, not a ZIM, cut short), the ways the
# helper leaves (quit, the shell closing its end, SIGTERM), and all of it
# again with no network at all (a network namespace with no interface) and
# no network system call made.
#
#   tests/library_helper_test.sh tools/library/pos-library
#
# Needs LIBRARY_ARCHIVE: a real .zim with a full-text index; the words below
# fit wikipedia_en_climate-change_maxi_2026-10 (docs/apps/library/CONTENT.md)
# and can be changed with LIBRARY_QUERY / LIBRARY_ARTICLE / LIBRARY_LINK.
#
# Copyright (c) 2026 PocketOS authors.
# SPDX-License-Identifier: Apache-2.0
set -u
HELPER=${1:?usage: library_helper_test.sh pos-library}
ARCHIVE=${LIBRARY_ARCHIVE:?set LIBRARY_ARCHIVE to a .zim file}
QUERY=${LIBRARY_QUERY:-greenhouse effect}
ARTICLE=${LIBRARY_ARTICLE:-Greenhouse_effect}
LINK=${LIBRARY_LINK:-https://library.invalid/Greenhouse_gas}
ORIGIN=https://library.invalid/
failed=0
check() { if [ "$2" = "1" ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi; }
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
mkdir -m 0700 "$T/img"
export POCKETOS_LOG_DIR="$T/log" POCKETOS_CONFIG_DIR="$T/cfg" HOME="$T/home"
mkdir -p "$T/log" "$T/cfg" "$T/home"

# One session: the requests on stdin (one per line, "-" pauses), every line
# the helper wrote, until it leaves or 60 s pass. $1: the output file.
session() { # <out> [prefix command...] -- requests come from stdin
    local out=$1
    shift
    timeout 60 "$@" "$HELPER" session --archive "$ARCHIVE" --images "$T/img" >"$out" 2>"$out.err"
}
req() { # seq url
    printf 'open\t%s\t528\ti\t%s\n' "$1" "$2"
}
idle_after() { # <file> <seq>: the helper finished seq
    grep -q "^idle	$2\$" "$1" && echo 1 || echo 0
}
page_of() { # <file> <seq>: the page lines for seq, through its end
    awk -v s="$2" -F'\t' '$1 == "page" && $2 == s { on = 1 } on { print } $1 == "end" && $2 == s { on = 0 }' "$1"
}

journey() { # <name> [prefix command...]
    local name=$1
    shift
    local out="$T/$name.out"
    {
        req 1 "${ORIGIN}~library/search?q=$(printf '%s' "$QUERY" | sed 's/ /%20/g')"
        sleep 3
        req 2 "${ORIGIN}${ARTICLE}"
        sleep 5
        req 3 "$LINK"
        sleep 4
        req 4 "${ORIGIN}~library/about"
        sleep 1
        req 5 "${ORIGIN}No_such_article_in_this_collection"
        sleep 1
        req 6 "${ORIGIN}~library/main"
        sleep 4
        printf 'quit\n'
        sleep 1
    } | session "$out" "$@"
    local rc=$?
    check "$name: the helper says hello with zim and WebP pictures" \
        "$(head -1 "$out" | grep -qE '^hello	1	zim,(fulltext|titles),img:[a-z,]*webp' && echo 1 || echo 0)"
    page_of "$out" 1 >"$T/$name.search"
    check "$name: a search returns a page that links to the article" \
        "$(grep -qE "^link	[0-9]+	1	${ORIGIN}${ARTICLE}\$" "$T/$name.search" && echo 1 || echo 0)"
    check "$name: and says which search ran" \
        "$(grep -qE '^run	.*(Searched the titles and the full text|Title search only)' "$T/$name.search" && echo 1 || echo 0)"
    page_of "$out" 2 >"$T/$name.article"
    check "$name: the article arrives at its own address" \
        "$(grep -qE "^page	2	[0-9]+	200	${ORIGIN}${ARTICLE}	" "$T/$name.article" && echo 1 || echo 0)"
    check "$name: with headings (h1 and h2) and text" \
        "$(grep -qE '^block	1	1	-1' "$T/$name.article" && grep -qE '^block	1	2	-1' "$T/$name.article" &&
           [ "$(grep -c '^run	' "$T/$name.article")" -gt 100 ] && echo 1 || echo 0)"
    check "$name: links into the collection are followable" \
        "$(grep -qE "^link	[0-9]+	1	${ORIGIN}" "$T/$name.article" && echo 1 || echo 0)"
    check "$name: links out of it are sent as not followable, never as followable" \
        "$(grep -qE '^link	[0-9]+	0	https?://[^l]' "$T/$name.article" &&
           ! grep -E '^link	[0-9]+	1	' "$T/$name.article" | grep -vq "	${ORIGIN}" && echo 1 || echo 0)"
    local px
    px=$(grep -E '^pixels	2	' "$out" | head -1)
    check "$name: a picture of the article is decoded to RGB565 ($(echo "$px" | cut -f4,5 --output-delimiter=x))" \
        "$([ -n "$px" ] && echo 1 || echo 0)"
    check "$name: pictures not shown say why" \
        "$(grep -vE '^nopixels	2	[0-9]+	.+' "$out" | grep -qE '^nopixels	2' && echo 0 || echo 1)"
    check "$name: an internal link opens its article" \
        "$(page_of "$out" 3 | grep -qE "^page	3	[0-9]+	200	${LINK}	" && echo 1 || echo 0)"
    page_of "$out" 4 >"$T/$name.about"
    check "$name: about: the collection's title, date and source" \
        "$(grep -q 'Title: ' "$T/$name.about" && grep -qE 'Date: [0-9]{4}-[0-9]{2}-[0-9]{2}' "$T/$name.about" &&
           grep -q 'Source: ' "$T/$name.about" && echo 1 || echo 0)"
    check "$name: an entry the collection lacks: notfound" \
        "$(grep -qE '^fail	5	notfound	' "$out" && echo 1 || echo 0)"
    check "$name: the main page opens at its real address" \
        "$(page_of "$out" 6 | grep -qE "^page	6	[0-9]+	200	${ORIGIN}[^~]" && echo 1 || echo 0)"
    check "$name: every request ends idle" \
        "$(for s in 1 2 3 4 5 6; do idle_after "$out" $s; done | grep -c 1 | grep -qx 6 && echo 1 || echo 0)"
    check "$name: quit ends the helper cleanly" "$([ $rc = 0 ] && echo 1 || echo 0)"
}

# ---- the reading journey, then again with no network at all -------------------------------
journey normal
if unshare -rn true 2>/dev/null; then
    journey offline unshare -rn
else
    echo "skip offline: no network namespaces here"
fi
if command -v strace >/dev/null; then
    {
        req 1 "${ORIGIN}~library/search?q=$(printf '%s' "$QUERY" | sed 's/ /%20/g')"
        req 2 "${ORIGIN}${ARTICLE}"
        sleep 6
        printf 'quit\n'
    } | timeout 60 strace -f -qq -e trace=socket,connect,sendto,bind -o "$T/strace" \
        "$HELPER" session --archive "$ARCHIVE" --images "$T/img" >"$T/strace.out" 2>/dev/null
    check "no network system call while searching and reading (strace: socket, connect, sendto, bind)" \
        "$([ -s "$T/strace.out" ] && grep -q '^pixels	2' "$T/strace.out" && ! grep -qE 'socket|connect|sendto|bind' "$T/strace" &&
           echo 1 || echo 0)"
fi
check "the picture files are the session's, 0600, and exactly w*h*2 bytes" \
    "$(f=$(grep -E '^pixels	2	' "$T/normal.out" | head -1); n=$(echo "$f" | cut -f6); w=$(echo "$f" | cut -f4); h=$(echo "$f" | cut -f5);
       [ -f "$T/img/$n" ] && [ "$(stat -c %a "$T/img/$n")" = 600 ] && [ "$(stat -c %s "$T/img/$n")" = $((w * h * 2)) ] &&
       echo 1 || echo 0)"

# ---- no collection, or a bad one -----------------------------------------------------------
not_a_collection() { # <name> <expected words> [env...]
    local name=$1 words=$2
    shift 2
    local out="$T/$name.out"
    { req 1 "${ORIGIN}~library/main"; sleep 1; printf 'quit\n'; } |
        env "$@" timeout 20 "$HELPER" session --images "$T/img" >"$out" 2>/dev/null
    check "$name: answered as no collection ($words)" \
        "$(grep -E '^fail	1	archive	' "$out" | grep -q "$words" && idle_after "$out" 1 | grep -q 1 && echo 1 || echo 0)"
}
not_a_collection "no Library folder" "there is no Library folder"
mkdir -p "$T/home/Library"
not_a_collection "an empty Library folder" "there is no .zim archive in"
not_a_collection "a configured file that is not there" "No such file" POCKETOS_LIBRARY_ARCHIVE="$T/gone.zim"
head -c 200000 /dev/urandom >"$T/home/Library/noise.zim"
not_a_collection "a .zim of random bytes" "not a readable ZIM archive"
rm -f "$T/home/Library/noise.zim"
head -c 1048576 "$ARCHIVE" >"$T/home/Library/cut.zim"
not_a_collection "an archive cut short" "not a readable ZIM archive"
rm -f "$T/home/Library/cut.zim"
ln -s "$ARCHIVE" "$T/home/Library/real.zim"
{ req 1 "${ORIGIN}~library/about"; sleep 2; printf 'quit\n'; } |
    timeout 20 "$HELPER" session >"$T/found.out" 2>/dev/null
check "the first .zim in \$HOME/Library is the collection when nothing else names one" \
    "$(grep -qE '^page	1	' "$T/found.out" && echo 1 || echo 0)"
printf 'archive=%s\n' "$ARCHIVE" >"$T/cfg/library.conf"
rm -f "$T/home/Library/real.zim"
{ req 1 "${ORIGIN}~library/about"; sleep 2; printf 'quit\n'; } |
    timeout 20 "$HELPER" session >"$T/conf.out" 2>/dev/null
check "or the archive= line of <config>/library.conf" "$(grep -qE '^page	1	' "$T/conf.out" && echo 1 || echo 0)"
{ req 1 "https://example.com/"; sleep 1; printf 'quit\n'; } |
    timeout 20 "$HELPER" session --archive "$ARCHIVE" >"$T/web.out" 2>/dev/null
check "an address outside the collection is refused, not fetched" \
    "$(grep -qE '^fail	1	url	' "$T/web.out" && echo 1 || echo 0)"

# ---- leaving ---------------------------------------------------------------------------------
( sleep 1; exec 1>&- ) | timeout 10 "$HELPER" session --archive "$ARCHIVE" >/dev/null 2>&1
check "the shell closing its end ends the helper" "$([ $? = 0 ] && echo 1 || echo 0)"
mkfifo "$T/in"
"$HELPER" session --archive "$ARCHIVE" <"$T/in" >/dev/null 2>&1 &
pid=$!
exec 7>"$T/in"   # the shell's end, held open
sleep 1
kill -TERM "$pid" 2>/dev/null
sleep 1
check "SIGTERM ends the helper" "$(kill -0 "$pid" 2>/dev/null && echo 0 || echo 1)"
kill -KILL "$pid" 2>/dev/null
exec 7>&-

echo "library_helper_test: $failed failure(s)"
exit $((failed > 0))
