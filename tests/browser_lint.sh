#!/bin/bash
# Browser's boundaries (docs/apps/BROWSER.md, ADR-009 accepted), held statically.
set -u
cd "$(dirname "$0")/.." || exit 1
failed=0
check() { if [ "$2" -eq 1 ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi; }
code() { grep -vE '^[[:space:]]*(/\*|\*|//)' "$@" 2>/dev/null | sed 's|/\*.*\*/||'; }

A=apps/browser
C=core/web
H=tools/browser/pos_browser.c
APP=$A/browser_app.c

# ---- layering ------------------------------------------------------------------
hits=$(grep -lE 'lvgl|lv_obj|lv_label|lv_timer' $C/*.[ch] $H $A/browser_session.[ch] $A/browser_view.[ch] 2>/dev/null)
check "the reader, the helper, the session and the view are free of LVGL" "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits"
# The shell links the address rules, the document, the protocol, the list and
# the remembered state - nothing that reads a byte a web server sent.
hits=$(grep -oE 'core/web/[a-z_]+\.c' ui/shell/CMakeLists.txt | sort -u | tr '\n' ' ')
check "the shell links only web_doc, web_history, web_proto, web_store and web_url of core/web ($hits)" \
    "$([ "$hits" = "core/web/web_doc.c core/web/web_history.c core/web/web_proto.c core/web/web_store.c core/web/web_url.c " ] &&
       echo 1 || echo 0)"
hits=$(code $A/*.c $A/*.h | grep -nE 'curl|openssl|png\.h|jpeglib|web_html\.h|web_fetch\.h|web_image\.h')
check "the app never includes HTML parsing, fetching, TLS or picture decoding" "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -5
hits=$(code $A/*.c | grep -nE '\b(connect|getaddrinfo|gethostbyname|bind|accept)\(')
check "the app opens no network socket (only the helper's socketpair)" "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -5
hits=$(code $APP $A/browser_view.c | grep -nE '\b(fopen|open|openat|unlink|rename|mkdir|opendir|fork|exec[lv]p?e?)\(')
check "the screen and the view never touch files or start processes (the store and the session do)" \
    "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -5
hits=$(grep -rlE '\bfork\(' $A $C tools/browser --include='*.c' | sort | tr '\n' ' ')
check "only the session starts a process ($hits)" "$([ "$hits" = "$A/browser_session.c " ] && echo 1 || echo 0)"
hits=$(code $C/*.c tools/browser/*.c $A/*.c | grep -nE '\b(system|popen|execl|execlp|execvp|execle)\(')
check "no shell is ever run, and nothing is looked up on PATH" "$([ -z "$hits" ] && echo 1 || echo 0)"
hits=$(grep -lE '#include <curl/' $C/*.c $C/*.h tools/browser/*.c $A/*.c 2>/dev/null | tr '\n' ' ')
check "only web_fetch_curl.c speaks libcurl ($hits)" "$([ "$hits" = "$C/web_fetch_curl.c " ] && echo 1 || echo 0)"
hits=$(grep -lE '#include <(png|jpeglib)\.h>' $C/*.c tools/browser/*.c $A/*.c 2>/dev/null | tr '\n' ' ')
check "only web_image.c decodes pictures ($hits)" "$([ "$hits" = "$C/web_image.c " ] && echo 1 || echo 0)"

# ---- what is fetched -------------------------------------------------------------
check "http and https are the only schemes that are ever fetched (the enum has no other)" \
    "$(sed -n '/^enum web_scheme {/,/};/p' $C/web_url.h | grep -oE 'WEB_SCHEME_[A-Z]+' | tr '\n' ' ' |
       grep -qx 'WEB_SCHEME_NONE WEB_SCHEME_HTTP WEB_SCHEME_HTTPS WEB_SCHEME_ABOUT ' && echo 1 || echo 0)"
check "libcurl itself is held to http and https, redirects included" \
    "$(grep -q 'CURLOPT_PROTOCOLS_STR, "http,https"' $C/web_fetch_curl.c &&
       grep -q 'CURLOPT_REDIR_PROTOCOLS_STR, "http,https"' $C/web_fetch_curl.c && echo 1 || echo 0)"
check "libcurl never follows a redirect itself: web_fetch applies the rules" \
    "$(grep -q 'CURLOPT_FOLLOWLOCATION, 0L' $C/web_fetch_curl.c && ! grep -qE 'FOLLOWLOCATION, 1' $C/*.c && echo 1 || echo 0)"
check "an https page never redirects to http" \
    "$(grep -q 'cur.scheme == WEB_SCHEME_HTTPS && to.scheme == WEB_SCHEME_HTTP' $C/web_fetch.c && echo 1 || echo 0)"

# ---- TLS --------------------------------------------------------------------------
check "certificates and host names are always verified" \
    "$(grep -q 'CURLOPT_SSL_VERIFYPEER, 1L' $C/web_fetch_curl.c && grep -q 'CURLOPT_SSL_VERIFYHOST, 2L' $C/web_fetch_curl.c &&
       echo 1 || echo 0)"
hits=$(code $C/*.c tools/browser/*.c $A/*.c | grep -nE 'VERIFYPEER, *0|VERIFYHOST, *[01]L|CURLOPT_SSL_OPTIONS|CURLOPT_PROXY_SSL|verify *= *false')
check "there is no switch that turns verification off" "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -5
check "a CA file comes only from the simulator's test hooks, never from the device's environment" \
    "$(grep -B3 'getenv("POCKETOS_BROWSER_CA_FILE")' $APP | grep -q 'POCKETOS_SHELL_TEST_HOOKS' && echo 1 || echo 0)"

# ---- privacy ------------------------------------------------------------------------
check "cookies stay in memory: no cookie jar is ever written" \
    "$(grep -q 'CURLOPT_COOKIEFILE, ""' $C/web_fetch_curl.c && ! grep -q 'COOKIEJAR' $C/*.c && echo 1 || echo 0)"
hits=$(code $H $A/*.c | grep -nE 'LOG_[A-Z]+\(.*\b(url|open_url|loading_url|doc_url|field|typed)\b')
check "no log line carries an address (only its scheme and host)" "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -5
check "the remembered pages are 0600 in a 0700 directory, written without following links" \
    "$(grep -q 'pocketos_mkdir_p(dir, 0700)' $C/web_store.c && grep -q 'O_NOFOLLOW | O_CLOEXEC, 0600' $C/web_store.c &&
       grep -q 'fchmod(fd, 0600)' $C/web_store.c && echo 1 || echo 0)"
check "the picture directory is the session's own (mkdtemp, 0700) and its files 0600" \
    "$(grep -q 'mkdtemp(tmpl)' $A/browser_session.c && grep -q 'O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600' $H && echo 1 || echo 0)"
check "the helper refuses a picture directory others can reach" \
    "$(grep -q '(st.st_mode & 077) != 0' $H && echo 1 || echo 0)"

# ---- lifetimes --------------------------------------------------------------------------
check "the helper leaves with the shell (PR_SET_PDEATHSIG) and holds none of its descriptors" \
    "$(grep -q 'PR_SET_PDEATHSIG, SIGTERM' $A/browser_session.c && grep -q 'SOCK_CLOEXEC' $A/browser_session.c &&
       grep -q 'CHILD_FD_SCAN_MAX' $A/browser_session.c && echo 1 || echo 0)"
check "leaving the app abandons the helper and frees the page's pictures before their objects could draw" \
    "$(sed -n '/^static void browser_destroy/,/^}/p' $APP | grep -q 'browser_session_abandon' &&
       sed -n '/^static void browser_destroy/,/^}/p' $APP | grep -n 'clear_page\|browser_view_free' | head -1 | grep -q clear_page &&
       echo 1 || echo 0)"
check "the timer only polls: no blocking read on the LVGL thread" \
    "$(code $A/browser_session.c | grep -E '\brecv\(' | grep -qv MSG_DONTWAIT && echo 0 || echo 1)"

# ---- the image -----------------------------------------------------------------------------
check "the package builds the real fetcher and the decoders" \
    "$(grep -q 'BROWSER_CURL=1 BROWSER_IMAGES=1' platforms/k230/package/pocketos/pocketos.mk && echo 1 || echo 0)"
check "and depends on libcurl, jpeg and libpng (all already in the image)" \
    "$(grep -E '^POCKETOS_DEPENDENCIES' platforms/k230/package/pocketos/pocketos.mk | grep -q 'libcurl' &&
       grep -E '^POCKETOS_DEPENDENCIES' platforms/k230/package/pocketos/pocketos.mk | grep -qw 'jpeg' &&
       grep -E '^POCKETOS_DEPENDENCIES' platforms/k230/package/pocketos/pocketos.mk | grep -qw 'libpng' && echo 1 || echo 0)"
check "pos-browser is installed" "$(grep -q 'tools/browser/pos-browser $(DESTDIR)$(PREFIX)/bin/pos-browser' Makefile && echo 1 || echo 0)"
check "Browser is on the launcher's page, in ESSENTIALS (DS §47)" \
    "$(grep -q '{ "browser", HOME_GROUP_ESSENTIALS, HOME_HUE_NETWORK, HOME_FOLDER_NONE }' ui/shell/home_layout.c && echo 1 || echo 0)"

echo "browser_lint: $failed failure(s)"
exit $((failed > 0))
