#!/bin/bash
# Copyright (c) 2026 PocketOS authors.
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p work/ninjaone-tests
${CC:-gcc} ${CFLAGS:--O2 -Wall -Wextra -Werror} -Icore \
    tests/ninjaone_auth_test.c core/ninjaone/ninja_api.c core/ninjaone/ninja_model.c \
    core/ninjaone/ninja_token.c core/pocketpaths.c -o work/ninjaone-tests/auth-test \
    -lcjson -lcurl -lm
python3 tests/ninjaone_auth_test.py
bash tests/ninjaone_lint.sh
