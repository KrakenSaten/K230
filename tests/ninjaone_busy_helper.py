#!/usr/bin/env python3
# Copyright (c) 2026 PocketOS authors.
# SPDX-License-Identifier: Apache-2.0
"""Hold synthetic helper replies for the real shell's Refresh/Forget test."""
import json
import os
from pathlib import Path
import sys
import time

case, helper, operation = Path(sys.argv[1]), sys.argv[2], sys.argv[3]
if operation not in ('snapshot', 'forget'):
    raise SystemExit(2)
scenario = (case/'scenario').read_text()
entry = dict(pid=os.getpid(), operation=operation, scenario=scenario)
with (case/'calls').open('a') as calls:
    calls.write(json.dumps(entry) + '\n')

renewing = operation == 'snapshot' and scenario == 'held-renewal'
forgetting = operation == 'forget' and scenario == 'held-forget'
if renewing or forgetting:
    if renewing:
        # These frames were produced by the real helper's fake backend in this
        # test run; no duplicated IPC layout or credential bytes are involved.
        sys.stdout.buffer.write((case/'renewing.frame').read_bytes())
        sys.stdout.buffer.flush()
    entered = case/'held-entered.new'
    entered.write_text(json.dumps(entry))
    entered.replace(case/'held-entered')
    deadline = time.monotonic() + 10
    while not (case/'held-release').exists():
        if time.monotonic() >= deadline:
            raise SystemExit(3)
        time.sleep(.01)
    if renewing:
        sys.stdout.buffer.write((case/'renewed.frame').read_bytes())
        sys.stdout.buffer.flush()
        raise SystemExit(0)

# Every invocation is explicitly fake, including Forget. The fixture cannot
# reach a tenant or read or erase a real credential store.
if scenario.startswith('held-'):
    scenario = 'demo'
os.execv(helper, [helper, operation, '--fake', scenario])
