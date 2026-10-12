#!/bin/bash
# Copyright (c) 2026 PocketOS authors.
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p work/ninjaone-tests
${CC:-gcc} ${CFLAGS:--O2 -Wall -Wextra -Werror} -Icore -Iapps/ninjaone \
    tests/ninjaone_test.c core/ninjaone/ninja_api.c core/ninjaone/ninja_model.c \
    core/ninjaone/ninja_token.c core/pocketpaths.c apps/ninjaone/ninja_session.c \
    -o work/ninjaone-tests/test -lcjson -lm
POCKETOS_NINJAONE_HELPER="$PWD/tools/ninjaone/pos-ninjaone" work/ninjaone-tests/test
python3 - <<'TOKEN_CLI_TEST'
import os, subprocess, tempfile
from pathlib import Path
with tempfile.TemporaryDirectory() as runtime:
    env = {**os.environ, 'POCKETOS_RUNTIME_DIR': runtime}
    helper = 'tools/ninjaone/pos-ninjaone'
    for raw in (b'bad\nextra\n', b'bad\x00value', b'x'*4097):
        result = subprocess.run([helper, 'set-token'], input=raw, capture_output=True, env=env)
        assert result.returncode and raw not in result.stdout + result.stderr
    result = subprocess.run([helper, 'set-token'], input=b'synthetic-cli-token\n', capture_output=True, env=env)
    assert not result.returncode and not result.stdout
    assert (Path(runtime)/'ninjaone/access-token').read_text() == 'synthetic-cli-token'
print('Token CLI: bounded input, NUL/multiline rejection and secure pipe provisioning passed')
TOKEN_CLI_TEST
python3 - <<'PY'
import importlib.util, sys
sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location('provision', 'tools/ninjaone/provision.py')
p = importlib.util.module_from_spec(spec); spec.loader.exec_module(p)
assert p.challenge('dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk') == 'E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM'
assert p.callback_code('/callback?state=expected&code=synthetic-code', 'expected') == 'synthetic-code'
for path in ('/callback?state=wrong&code=x', '/callback?state=expected&error=denied',
             '/callback?state=expected&code=x&code=y', '/wrong?state=expected&code=x'):
    try: p.callback_code(path, 'expected')
    except ValueError: pass
    else: raise AssertionError('invalid callback accepted')
print('PKCE: RFC 7636 S256 vector and callback/state rejection passed')
PY
${CC:-gcc} ${CFLAGS:--O2 -Wall -Wextra -Werror} -Icore tests/ninjaone_http_test.c \
    core/ninjaone/ninja_api.c core/ninjaone/ninja_model.c core/ninjaone/ninja_token.c \
    core/pocketpaths.c -o work/ninjaone-tests/http-test -lcjson -lcurl -lm
python3 tests/ninjaone_http_test.py
bash tests/ninjaone_lint.sh
