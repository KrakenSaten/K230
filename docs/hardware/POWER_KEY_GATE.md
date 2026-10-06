# Power key: Unit B hardware smoke test (feat/k230-power-key)

Branch `feat/k230-power-key`, PR #63, code commit `a62d282`. Behaviour: DS §56.
This is a **general hardware smoke test**, not a case-by-case gate: the
owner's manual test "worked fine", and it does not confirm each case of the
suggested sequence individually.

## Unit B build identity

| | Before | Under test |
| --- | --- | --- |
| doors-shell | `608d8207` (PR #60 build, tree = master `ee0c1f8`) | `c8ff229a`, `BUILD_ID` `a62d282`, riscv64 DRM, pinned SDK toolchain |
| meshcored | `22171573` | `22171573` (unchanged) |

Shell-only hot swap; no SD reflash, kernel or DTB change. Rollback:
`/root/rollback-power-key/RESTORE.sh` (backup verified byte-identical to
`608d8207`).

## Observed (2026-10-06)

- The shell found the key by capability: `/dev/input/event0`, "K230 PMU
  Power Key", event stamps on CLOCK_MONOTONIC; one descriptor; the shell is
  its only reader; the touch node (`event1`) is still read as before.
- Owner's manual test: worked fine (general smoke).
- Shell log during it: 14 short presses, each toggling the screen off/on
  (including a burst about 0.2-0.8 s apart); 1 hold that opened the power
  menu, closed with Cancel; `ignored` 0, `lost` 0; no `ERROR` line;
  meshcored online and unchanged afterwards.
- Not exercised: Restart or Power off from the menu, the kernel's 5 s
  power-off, and (per the log) a rotation restart after the deploy.

## Host tests

New: `tests/power_key_test.c` 33/0 (in `make test`),
`tests/power_key_shell_test.sh` 0 failures (SDL shell through a FIFO);
8 mutants, all killed.

Known failures that **occur on master `ee0c1f8` identically** (WSL host,
validated from clones of both commits):

| Suite | Failures | Cause |
| --- | --- | --- |
| `initscript_test.sh` | 38 | S90 init-script tests under WSL (signals ignored by WSL children) |
| `first_boot_default_test.sh` | 4 | the same S90 path |
| `build_outputs_test.sh` | 1 | `tests/rift_rxlog_test` not git-ignored (separate fix) |
| `required_gates_test.sh` | 1 | follows from `build_outputs_test.sh` |

`rec_tool_test.sh` and `sysd_test.sh` each failed once while two clones ran
concurrently and passed when run alone.
