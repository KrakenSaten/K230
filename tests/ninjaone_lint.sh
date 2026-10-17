#!/bin/bash
# Copyright (c) 2026 PocketOS authors.
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail
cd "$(dirname "$0")/.."
if rg -n '#include.*(curl|cJSON)|curl_easy|cJSON_' apps/ninjaone; then
    echo 'FAIL: HTTP or JSON entered the shell app'; exit 1
fi
if rg -n '(CURLOPT_SSL_VERIFYPEER, 0|CURLOPT_SSL_VERIFYHOST, 0|CURLOPT_POST,|CURLOPT_CUSTOMREQUEST|client_secret)' tools/ninjaone/*.c tools/ninjaone/provision.py; then
    echo 'FAIL: unsafe TLS, mutation or unsupported authentication'; exit 1
fi
python3 - <<'PY'
from pathlib import Path
s = Path('core/ninjaone/ninja_api.c').read_text()
assert '"/v2/alerts"' in s and '"organizations" : "devices"' in s
oauth = Path('tools/ninjaone/ninja_oauth.h').read_text()
assert '%s/ws/oauth/token' in oauth and 'CURLOPT_POSTFIELDS' in oauth
ui = Path('apps/ninjaone/ninja_app.c').read_text()
assert 'Unofficial NinjaOne client / read-only' in ui
assert '.id = "ninjaone"' in ui and '.icon_mask = &pos_app_icon_ninjaone' in ui
assert '&app_ninjaone' in Path('ui/shell/shell.c').read_text()
assert '{ "ninjaone", HOME_GROUP_FOLDERS, HOME_HUE_TOOLS, HOME_FOLDER_APPS }' in Path('ui/shell/home_layout.c').read_text()
assert 'NINJAONE_CURL=1' in Path('platforms/k230/package/pocketos/pocketos.mk').read_text()
assert 'tools/ninjaone/pos-ninjaone $(DESTDIR)$(PREFIX)/bin/pos-ninjaone' in Path('Makefile').read_text()
assert 'ninjaone' in Path('tools/design/gen_app_icons.py').read_text()
assert Path('ui/assets/doors/icon-ninjaone.bin').stat().st_size == 27660
print('ninjaone_lint: process/TLS/read-only boundaries, registration and icon packaging passed')
PY
