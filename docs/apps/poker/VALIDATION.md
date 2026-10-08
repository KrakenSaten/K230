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

## Follow-up checks — 2026-10-08, `26e9b2c`

Clean clone in the WSL build host (docs/BUILD_ENVIRONMENT.md), which has no
ripgrep. On `498e8bd` three checks failed there: `poker_lint.sh` (it called
`rg`), `app_icons_test.sh` (Poker reused Blackjack's mask: 26 referencing
files, 25 expected) and `doors_ui_assets_test.sh` (27 launcher apps, 26
generator colours). `26e9b2c` adds Poker's own icon and moves the lint to GNU
grep. Then passed: `poker_lint.sh`, `gen_doors_ui.py --check`,
`art_format_test` (23), `app_icons_test.sh`, `doors_ui_assets_test.sh`,
`make all`, `poker_app_test` (134), `poker_shell_test.sh` (22 cases),
`launcher_folder_shell_test.sh` and `launcher_groups_shell_test.sh`. Nine
lint mutants (engine `malloc`, `lv_timer_create`, `fork(`, a second key sink,
Blackjack's mask, no portal icon, no manifest row, a missing engine directory,
not in Games) each made the lint fail. The engine suites were not rerun: no
engine or view code changed.

## Unit B hardware — 2026-10-08

**Build on the unit: doors-shell `52adb0e`** (master `0677a0e` + `498e8bd`,
riscv64 DRM Release with the pinned SDK toolchain, 0 warnings, md5
`1c1b8054eeca5842f198dd8f2949e63f`), hot-swapped for the PR #70 shell
`bc2d83d` over SSH; radiod, meshcored, sysd, netd, supervisors and init
scripts untouched and not restarted, settings/state fingerprint (15 files)
unchanged. The unit did **not** carry `26e9b2c`: its launcher drew the
fallback portal (shell.log: `icon-poker.bin: No such file or directory`).
Observed by the owner, with panel captures and shell.log:

| Check | Result |
| --- | --- |
| Games → Poker, table opens | Pass |
| Raise (minimum target 40), call, all-in | Pass; a four-way all-in settled 4 × 820 = 3,280 |
| Showdown | Pass: flush over two pair over pair; payout and stacks correct, folded cards hidden |
| Next hand | Pass: button and blinds one seat clockwise, stacks carried |
| Bust, game over, RESTART | Pass: only RESTART enabled; 1,000 per seat after it |
| Back and reopen | Pass: the hand resumed |
| BOOT key held: Home | Pass (resume after it not reported) |
| Open from landscape (keyboard attached) | Pass: shown in portrait |
| Close Poker with the keyboard attached | Pass: Home back in landscape (1232 × 568) |
| Hand across a rotation | Fresh table (as documented): keyboard attach re-executed the shell, then opening Poker again |

Chips summed to 4,000 in every capture. One `keyboard: the controller
stopped answering` warning appeared once while the keyboard was being
handled; it did not recur with the keyboard attached and is not attributed
to Poker. Not tested on hardware: the new icon (`26e9b2c`), text sizes,
themes, a Fold-only hand, the restart confirmation prompt on its own.

## Limits

The app uses fixed blinds, a basic bounded AI, portrait presentation and
memory-only session resume; restarting the shell resets the table, and a
rotation restarts it (see POKER.md). There are no real-money, payment or
networking features.
