#!/bin/bash
# pos-browser over real sockets (docs/apps/BROWSER.md "Validation"): the
# libcurl fetcher against local HTTP and HTTPS servers - pages, redirects
# and their rules, certificates, cookies, cut and damaged answers, the
# session's pictures and STOP, and what reaches the log. The servers are in
# tests/browser_http_test.py; nothing leaves the machine.
#
# The checks need a pos-browser with libcurl ("pos-browser features" starts
# with net) and are SKIPPED, loudly, otherwise - an ordinary host build has
# the fake network only (make BROWSER_CURL=1 BROWSER_IMAGES=1).
#
# Requires: tools/browser/pos-browser, python3, openssl(1).
set -u
cd "$(dirname "$0")/.." || exit 1
H=$(pwd)/tools/browser/pos-browser
if [ ! -x "$H" ]; then
    echo "FAIL build it first: make tools/browser/pos-browser"
    exit 1
fi
for tool in python3 openssl; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "SKIP browser_http_test: no $tool"
        exit 0
    fi
done
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
python3 tests/browser_http_test.py "$H" "$T"
