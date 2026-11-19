# Pip on the perch

Pip is the same owl across three modes. Tap near an eye and the gaze follows your finger. Stroke the face to pet Pip, hold for 700 ms to settle down, or tap a sleeping Pip to wake up. Feed offers one biscuit. Drag it to the beak, tap it, or choose Give. Settings includes a one-tap Rest/Wake alternative. F, Enter, R and Back retain their existing shortcuts.

## Design choice

The original screen gave two eyes most of the canvas, with a second row of equally prominent care and mode buttons. In landscape the controls sat at the bottom right, far from the face. We considered three directions.

| Direction | What it gives the companion |
| --- | --- |
| Glass-lensed desk robot | Mechanical shutters and lens reflections give clear gaze direction. Sleep and petting would need extra visual cues. |
| Pocket planetarium | Orbital motion works well with the night clock. The central object offers few clues about where to stroke or feed it. |
| Owl on a perch | Large facial disks frame the moving eyes. A beak gives the biscuit a destination. A single pose supports a watchful guard, a heavy-lidded night clock and an affectionate companion. |

The owl uses an original 256-square indexed illustration with live LVGL eyes and mouth. The 64 KiB source expands to a 192 KiB RGB888 image on opening. A theme or mode change updates that image. Ordinary blinks and glances redraw the changing features. RGB888 preserves the exact theme background at the edge of the illustration. BUDDY uses warm feathers, GUARD has cooler plumage, narrowed lids and more frequent glances, and NIGHT dims the whole scene. Reduced motion settles directly on each expression. Portrait puts the controls under the perch. Landscape puts them at eye level.

## Voice and ownership

Character sound starts Off, including when an older preference file is opened. Touch only includes intentional keyboard and touch interactions. All also voices a spontaneous yawn, a wake from sleep or a recognized-owner greeting. In Buddy, Preview steps through eight original calls. The yawn glides down, the purr trembles, and the nibble mixes two short, grainy pulses. The sound recipes are in the app and use no recorded samples.

This extends DeskBuddy's foreground use of the ADR-010 playback path. The child generates a missing 48 kHz mono WAV in DeskBuddy's runtime directory, then executes the existing `pos-record play` at the effective system volume. The same helper owns the audio lock, route and amplifier. Calls have a 260 ms minimum interval. Consecutive repeats wait 1.6 s, and spontaneous calls require 15 s since the last call. Busy or muted calls expire. NIGHT stays silent. GUARD's separately enabled visit chime still follows the existing new-visit rule.

## Shell captures

Both columns show the real SDL shell at panel resolution, with the default Doors theme and no camera helper. The originals came from master `90fd6f7d`. The redesigned captures came from the same shell with this change.

| Orientation | Original | Pip |
| --- | --- | --- |
| Portrait | [Before](before-portrait.png) | [After](buddy-portrait.png) |
| Landscape | [Before](before-landscape.png) | [After](buddy-landscape.png) |

The other captures show the [armed guard without a camera](guard.png), the [night clock](night.png), [Pip following the biscuit](snack.png) and [Settings at Large text](settings.png). The last two use Ice and Ember. The portrait and landscape comparison led to moving Settings into the controls beside Pip's name. The first render also exposed a faint rectangle from RGB565 quantization around the perch, corrected in the final captures.

## Validation

- The native SDL `pocketos-shell` and `deskbuddy_app_test` targets built with the repository's LVGL tree.
- `make deskbuddy-test` exercised the brain, personality, gestures, storage, fake vision pipeline, synthesized samples and helper lifecycle. `tests/style_lint.sh` passed.
- `tests/deskbuddy_shell_test.sh` passed. The end-to-end app test used a real LVGL pointer and the built vision helper on fake frames. It checked holding to rest, waking, preview selection, sound preference persistence, repeat suppression, synchronization with eating, silent NIGHT, and the independent GUARD chime.
- All eight generated calls played to completion through the real pos-record playback code using its file-backed host test backend. Sample tests checked the lead-in, final silence, headroom and lengths.
- The pinned Xuantie toolchain and Buildroot sysroot were unavailable here. A target build, K230 speaker audition, real touch and camera behavior, on-panel contrast, frame timing and memory measurements remain hardware checks. No deployment or flash was performed.
