# Vision screen redesign: before and after

Simulator screenshots (568 x 1232 panel, fake camera, fake detector) from
branch `claude/k230-vision-ui-redesign-ef4926`, 2026-11-12. `before/` is
master dd9b1f64, `after/` this branch. What changed and why:
docs/apps/VISION.md, "The screen".

| Screen | Before | After |
| --- | --- | --- |
| DETECT, upright | ![](before/detect-portrait.png) | ![](after/detect-portrait.png) |
| TRACK, upright | ![](before/track-portrait.png) | ![](after/track-portrait.png) |
| The picker, upright | ![](before/picker-portrait.png) | ![](after/picker-portrait.png) |
| SEGMENT, upright | ![](before/segment-portrait.png) | ![](after/segment-portrait.png) |
| COLOR, upright | ![](before/color-portrait.png) | ![](after/color-portrait.png) |
| TRAFFIC, wide | ![](before/traffic-landscape.png) | ![](after/traffic-landscape.png) |
| The picker, wide | ![](before/picker-landscape.png) | ![](after/picker-landscape.png) |
| SEGMENT, wide | ![](before/segment-landscape.png) | ![](after/segment-landscape.png) |

The host's fake nets emulate neither segmentation model, so SEGMENT shows
its "Cannot segment" warning here and no key to its colours.
`after/segment-key-portrait-forced.png` shows the key's place and look: it
was taken from a build patched, for that one screenshot only, to feed the
screen the words `road 41%  vegetation 20%  building 12%`. The key with a
real model has not been seen.

![](after/segment-key-portrait-forced.png)
