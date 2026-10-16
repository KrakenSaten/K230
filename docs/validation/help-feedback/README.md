# Help & Feedback validation — 2026-10-16

Implementation: `95d759a`, based on freshly fetched `origin/master` `5d94eab`.
The SDL simulator was configured and built from the original local validation
commit `f123c2c`; its screenshots show DOORS 0.3.6, build `f123c2c`.
GitHub rejected that commit's private author email, so the two new commits
were rewritten using the repository's established GitHub noreply address.
`git diff f123c2c 95d759a` is empty: only commit metadata changed, and the
validated source is identical. The later evidence-only commit adds this
record and captures. Unrelated checkout work was preserved.

## Completed

| Check | Result |
| --- | --- |
| Host SDL `pocketos-shell` and `system_app_test` CMake build | PASS |
| `tests/feedback_shell_test.sh` | PASS: 183 checks, zero failures |
| Independent ZBar decode of the focused test's normal/enlarged PNGs | PASS: 18/18, exact owner URL including `&lang=en-US` |
| Full System app regression (`system_app_test`) | PASS: 1,506 checks, zero failures |
| Full shell pointer navigation and captures | PASS: 9 layout/text-size combinations |
| Independent ZBar decode of full-shell normal/enlarged PNGs | PASS: 18/18, exact owner URL |
| `tests/system_lint.sh` | PASS |
| `tests/notices_test.sh` | PASS |
| `tools/legal/gen_notices.sh --check` and `--verify-lvconf work/build/lv_conf.h` | PASS, QR encoder covered |
| QR MIT text against pinned LVGL `src/libs/qrcode/LICENSE.txt` | Byte-identical |
| SDK defaults edit against the pinned BSP defaults, applied twice in a fixture | PASS: exactly one `LV_USE_QRCODE 1` |
| Changed shell scripts: `bash -n`; `git diff --check` | PASS |

The matrix covers Small, Medium and Large at 568×1232 portrait, 1232×568
landscape and the existing 1280×720 external-display override with square
corners. Normal captures at all nine combinations and representative enlarged
captures were visually inspected. The focused harness checks horizontal
bounds, a fully visible QR/quiet zone on initial opening, keyboard group
membership and Enter, Back through both levels, repeated enlargement with a
stable object count, removal of keyboard targets on close, and a deliberately
failed QR update followed by a successful retry. No feedback polling or
submission occurs on ticks.

In the actual shell, each matrix case opened About → Help & Feedback using
simulator pointer input, captured both views, used the normal Back action
twice, returned Home, reopened System/feedback, and returned Home again.
No sysd or radiod was running. The only shell warning was the expected
`radiod unavailable`; no LVGL warning or error was logged. The full shell's
existing `shell.tap`, `shell.screenshot`, `shell.action`, `shell.open` and
`shell.info` simulator/IPC facilities were used; no production test hooks
were added for this screen.

The bounded final diff review found an early-focus scroll that clipped the
landscape/Large QR. The completed page now resets its scroll after layout,
and the initial-visibility regression check plus all full-shell decodes pass.

## Representative full-shell screenshots

| Layout | Instructions and QR | Enlarged QR |
| --- | --- | --- |
| Portrait, Large | [portrait-large.png](portrait-large.png) | [portrait-large-qr.png](portrait-large-qr.png) |
| Landscape, Large | [landscape-large.png](landscape-large.png) | [landscape-large-qr.png](landscape-large-qr.png) |
| External 1280×720, Large | [external-large.png](external-large.png) | [external-large-qr.png](external-large-qr.png) |

These images are native simulator captures, with no resizing or smoothing of
the QR. Integer modules and a four-module white quiet zone are supplied by
the screen, independent of the theme.

## Blocker and remaining hardware check

The pinned Xuantie K230 toolchain directory
`/opt/toolchain/Xuantie-900-gcc-linux-6.6.0-glibc-x86_64-V3.0.2/` is absent
in this cloud checkout (`/opt/toolchain` does not exist). No K230 cross-build
or SDK image build was completed, and no toolchain was installed or changed.
The SDK's LVGL must be rebuilt with the enabled QR configuration before a
shell/package build; see [build instructions](../../apps/HELP_FEEDBACK.md).
Host development packages were extracted into scratch space only.

**Still required locally:** build with the pinned K230 toolchain, then scan
the normal/enlarged QR from the actual K230 display with a phone in both
orientations. Verify the English Microsoft form opens; make no test submission.
Software decoding of PNGs does not establish physical-display scannability.
No device was accessed or deployed to, and no Microsoft form was changed,
opened by the implementation, or submitted during this work.
