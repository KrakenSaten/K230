# Help & Feedback

Open **Settings → System → About → Help & Feedback**. Scan the QR code with
your phone to report a bug, suggest a feature, or share your thoughts about
DOORS. No GitHub account or sign-in is required; contact details are optional.
Include the displayed DOORS version and build when reporting a bug. The form
has its own category and version questions.

Feedback is submitted to **Microsoft Forms from your phone**. DOORS sends
nothing itself and can remain offline. It does not collect or attach logs,
identifiers, addresses, or other user data. The phone needs internet access.

Choose **Enlarge QR code** for a larger code. Back returns from that view to
the instructions, then to About; Home leaves System as usual. The new controls
join the standard keyboard focus group: Tab/Shift+Tab and Enter work normally.
A failed QR generation hides the code and gives a retry instruction.

The owner-maintained form URL is `DOORS_FEEDBACK_FORM_URL` in
`apps/system/feedback.h`. Preserve the entire URL, including `&lang=en-US`;
there is no short URL, browser action, prefill, or submission helper. Version
and build use the shell's existing `POCKETOS_VERSION` / `POCKETOS_BUILD_ID`
compile definitions, so they remain available without sysd.

## Build and validation

The pinned LVGL already bundles the Nayuki QR encoder and QR widget. The
simulator enables `LV_USE_QRCODE` in `ui/shell/lv_conf.defaults`;
`platforms/k230/scripts/apply_to_sdk.sh` enables the same flag in the pinned
SDK LVGL defaults. No dependency or toolchain version changes. The bundled
encoder's MIT notice is included in `THIRD_PARTY_NOTICES.txt`.

For an SDK that already extracted/built LVGL, after applying this commit run
`make -C "$POCKETOS_VENDOR_DIR/k230_linux_sdk" CONF=k230_pocketos_defconfig lvgl-dirclean`
and rebuild LVGL before rebuilding the Doors package. This regenerates the
shared LVGL configuration as well as its library; a shell-only rebuild against
the old configuration cannot provide the QR widget.

After the SDL CMake build:

```sh
SYSTEM_APP_TEST=path/to/system_app_test bash tests/feedback_shell_test.sh
```

This runs Small/Medium/Large in portrait, landscape and a square-cornered
1280×720 external-display layout, checks bounds, keyboard entry, Back,
repeated enlargement/cleanup, and a simulated QR-generation failure/retry.
If `zbarimg` is available, it independently decodes all 18 normal/enlarged
captures to the exact URL. Set `FEEDBACK_CAPTURE_DIR` to retain the PNGs.
These are software decoding checks, not evidence of scanning physical glass.

**Remaining hardware check:** on the K230 display, open the screen in both
orientations and scan the normal/enlarged code with a phone. Confirm the
English Microsoft form opens, without submitting anything. No device access,
deployment, merge or release is part of this change.
