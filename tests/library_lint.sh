#!/bin/bash
# The Library's boundaries (docs/apps/LIBRARY.md), held statically. Runs
# with every `make test`; needs no libzim.
set -u
cd "$(dirname "$0")/.." || exit 1
failed=0
check() { if [ "$2" -eq 1 ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi; }
code() { grep -vE '^[[:space:]]*(/\*|\*|//)' "$@" 2>/dev/null | sed 's|/\*.*\*/||'; }

H=tools/library/pos_library.cpp
APP=apps/library/library_app.c

# ---- the archive stays in the helper -------------------------------------------------------
hits=$(grep -rlE '#include <zim/' apps ui core tools --include='*.c' --include='*.cpp' --include='*.h' | tr '\n' ' ')
check "only pos-library reads an archive: libzim is included nowhere else ($hits)" \
    "$([ "$hits" = "$H " ] && echo 1 || echo 0)"
check "the shell never links libzim or the helper" \
    "$(grep -vE '^[[:space:]]*#' ui/shell/CMakeLists.txt | grep -qE 'libzim|-lzim|pos_library' && echo 0 || echo 1)"
hits=$(code $APP | grep -nE 'lv_[a-z_]+\(|web_html|web_image|\b(open|fopen|opendir|fork|exec[lv]p?e?)\(')
check "the app is a profile: no LVGL of its own, no parsing, no files, no processes" \
    "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -5

# ---- offline, by construction --------------------------------------------------------------
hits=$(code $H | grep -nE 'curl|openssl|web_fetch|getaddrinfo|gethostbyname|\bsocket\(|\bconnect\(|\bbind\(')
check "the helper has no network code: no HTTP client, no socket, no name lookup" \
    "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -5
check "and its build links no fetcher: web_fetch is not among its objects" \
    "$(sed -n '/^POS_LIBRARY_OBJS :=/,/[^\\]$/p' Makefile | grep -qE 'web_fetch|WEB_HELPER_OBJS|WEB_FETCH' && echo 0 || echo 1)"
check "links that leave the collection are sent as not followable" \
    "$(sed -n '/^static void mark_outside_links/,/^}/p' $H | grep -q 'supported = false' && echo 1 || echo 0)"
check "addresses of the collection are https://library.invalid/ (.invalid never resolves, RFC 6761)" \
    "$(grep -q '#define WEB_LIBRARY_ORIGIN "https://library.invalid/"' core/web/web_library.h && echo 1 || echo 0)"
check "the helper refuses any address that is not the collection's" \
    "$(sed -n '/^static bool url_entry/,/^}/p' $H | grep -q 'strcmp(u.host, WEB_LIBRARY_HOST) != 0' && echo 1 || echo 0)"

# ---- the screen's process rules (Browser's) ---------------------------------------------------
check "the helper refuses a picture directory others can reach, and writes its pictures 0600" \
    "$(grep -q '(st.st_mode & 077) != 0' $H && grep -q 'O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600' $H && echo 1 || echo 0)"
check "a helper of another app gets no fake network and no CA file" \
    "$(sed -n '/^static unsigned start_helper/,/^}/p' apps/browser/browser_app.c | grep -q 'a->p->helper_env' && echo 1 || echo 0)"
hits=$(code $H | grep -nE 'LOG_[A-Z]+\(.*\b(url|path|q|words|open_url)\b')
check "no log line carries an address, an entry or a search" "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -5
hits=$(code $H | grep -nE '\b(system|popen|fork|execl|execlp|execvp|execle|execv)\(')
check "the helper starts nothing" "$([ -z "$hits" ] && echo 1 || echo 0)"

# ---- a prototype: nothing ships ------------------------------------------------------------------
check "Library is out of the shell unless asked for" \
    "$(grep -q 'option(POCKETOS_WITH_LIBRARY "Build the Library prototype into the shell" OFF)' ui/shell/CMakeLists.txt &&
       echo 1 || echo 0)"
check "pos-library is not in \$(BINS), not installed, not in the image's package" \
    "$(! sed -n '/^BINS :=/,/[^\\]$/p' Makefile | grep -q pos-library && ! grep -q 'pos-library' platforms/k230/package/pocketos/pocketos.mk &&
       ! grep -qE 'install .*pos-library' Makefile && echo 1 || echo 0)"
hits=$(git ls-files '*.zim' 2>/dev/null)
check "no archive is in git" "$([ -z "$hits" ] && echo 1 || echo 0)"

echo "library_lint: $failed failure(s)"
exit $((failed > 0))
