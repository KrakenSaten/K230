# Poker cloud validation — 2026-10-08

Base: `origin/master` at `e4bed1c`. Published Codex Cloud host environment,
GCC 14, pinned LVGL and headless SDL (`SDL_VIDEODRIVER=dummy`). No SDK,
dependency or unrelated application production code was changed.

| Check | Result |
| --- | --- |
| `make CC=gcc poker-test` | 20,150 engine checks, 3,538 view checks; layering/lifecycle lint passed |
| `make CC=gcc poker-san-test` | All 20,150 checks passed with AddressSanitizer and UndefinedBehaviorSanitizer |
| Exhaustive hand category validation | All 2,598,960 five-card combinations matched the known category frequencies |
| Explicit CMake shell / Poker / Blackjack / Solitaire app targets | Built successfully |
| Real LVGL `poker_app_test` | 134 checks passed, including pointer/key actions and 40 create/destroy cycles |
| `poker_shell_test.sh` | 22 cases passed at 568 × 1232; preflop/flop/showdown/split/game-over, all three text sizes, all six themes, normal/outdoor/night modes |
| Existing native `games-test` | Passed (2048, Solitaire, Blackjack and Poker) |
| Existing native `home_layout_test` | 369 checks passed |
| Existing Blackjack and Solitaire shell/app suites | Both passed, including portrait/landscape review states and cleanup |
| Existing launcher folder and group shell suites | Both passed, including repeated navigation and layout audits |
| License, notices and style audits | Passed; existing third-party attribution retained |

Poker shell audits found no clipped, overlapping, sizeless or truncated
LVGL objects. Screenshots were also visually inspected, including Large
text at a four-way split and night mode. Custom-drawn hand names reserve
two caption lines. Representative captures are committed beside this file;
the complete generated set is in `out/poker/screenshots/`.

## Failures outside Poker

- Full `make -j4 CC=gcc test` built the host tools and ran through the
  native suites until `term_session_test`: 52 checks, two failures,
  `no job of its session outlives it` and
  `closing ends a job that ignores SIGHUP and a stopped one`.
  An isolated rerun with the cloud child reaper reproduced both failures.
  Terminal sources and tests are identical to the base commit.
- An isolated `first_boot_default_test.sh` with ordinary `022` umask failed
  `the vendor's own files beside it are kept`. Its fixture expects
  `sensor.sh` to survive; the current SDK apply script deliberately removes
  it. The test and apply script are unchanged.
- Building the default CMake target stops in the existing
  `tests/rec_app_test.c:472`: `lv_area_intersect` is not declared by the
  pinned LVGL headers (the `_lv_area_intersect` alias expands to it).
  Explicit shell/card-app targets succeeded. Recorder and vendor sources
  are unchanged.
- The onboarding environment also recorded existing GCC 14 format warnings
  under the repository-wide `-Werror` build variant. No unrelated warning
  suppressions or production fixes were added.

## Limits

Physical K230 operation and an SDK/Xuantie cross build were not tested.
The app uses fixed blinds, a basic bounded AI, portrait presentation and
memory-only session resume; restarting the shell resets the table. There
are no real-money, payment or networking features.
