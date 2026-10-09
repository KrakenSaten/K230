# Vision detector A/B - unit A gate (PASS, 2026-10-09)

Branch `feat/vision-model-switch` (docs/apps/VISION.md, "Detector A/B on a
bench unit"). A practical comparison of DOORS' R0 YOLOX-Tiny 416 with the
upstream YOLOX-Tiny 416 on live scenes. It is **not** an identical-input
benchmark and changes no release: the reference comparison remains the
2026-10-04 set (R0 47/91 vehicles, upstream 82/91), which was not rerun.

Evidence: VERIFIED on unit A unless marked otherwise. "Observed" = seen on the
screen by the product owner, not measured.

## Unit and build

| | |
|---|---|
| Unit | unit A, wlan0 MAC `88:3b:dc:b7:9e:c7`, 172.16.5.208 (office network) |
| Found | Doors 0.3.5, `BUILD_ID=3d4ea6e` (tag v0.3.5); the running shell carried the same id; `yolov8n.kmodel` installed by hand |
| Deployed 08:26 UTC | integration build `bddb56e` = v0.3.5 `3d4ea6e` + `36dc5b9` (this branch's change, cherry-picked unchanged; local branch `integration/unitA-v035-vision-ab`), cross-built with the pinned Xuantie gcc 14.1.1 against the pinned SDK sysroot, 0 warnings |
| Replaced | `/usr/bin/doors-shell` only: sha256 `38abf837…bb10` -> `bb012ede…f05a`; only doors-shell was stopped (radiod, meshcored, netd, sysd kept their pids) |
| Added | `/usr/share/doors/vision/det-r0-traffic6-yolox-tiny-416.kmodel` (`94a20ac0…d268`), `det-upstream-yolox-tiny-416.kmodel` (`8c304651…e354`) |
| Rollback | `/root/rollback-vision-ab/RESTORE.sh` puts the v0.3.5 shell back and removes both kmodels; `RESTORE-SETTINGS.sh` restores Vision's `settings.v1` (optional: v0.3.5 ignores the `detector=` line) |

The deployed shell predates the log-label wording fix on this branch (below);
it logs the stats line as `vision: detector X: ...`.

## Results

| Check | Result |
|---|---|
| Vision opens with the model named | `MODEL: UPSTREAM` on the button, status led by `UPSTREAM` (observed); log `detector UPSTREAM in force` |
| Switching both ways, repeatedly | 14 switches plus 3 opens: 17 confirmed loads, 0 failures, 0 WARN/ERROR in the shell log |
| Old helper cleanup | every switch: the old helper said `bye` (camera, nets and KPU pool closed) in 448-491 ms before the next started; new detector in force 130-260 ms later; no pause noticed in the UI (observed) |
| Leave and reopen | no `pos-vision` left after Vision closed; reopened on the last confirmed detector (`detector=upstream` stored) |
| Model-free modes | COLOR, EDGE and LINE TRACE work (observed) |
| Resources | helper RSS 7.2-7.4 MB throughout; CmaFree 222 MB afterwards; no KPU/OOM/segfault in dmesg; doors-shell pid unchanged, no supervisor restart |

**Inference timing** (helper stats, logged every 10 s): KPU R0 55-68 ms,
upstream 57-65 ms; about 12.5 fps, pre 3-5 ms, post 12-14 ms for both; 0
bad tensors. One line read `UPSTREAM ... KPU 0 ms` while a model-free mode
ran (no detector inference in that mode); the stats line now names the mode.

**Detection quality** (observed, same camera position, live street scene):
R0 misses some cars that upstream finds. Upstream also boxes traffic lights,
which R0 cannot: it scores only its six traffic classes.

## Not tested on hardware

- The failure paths (a missing or unloadable A/B file, a helper that dies
  opening one, no fallback): host tests only (`tests/vision_model_test.c`).
- `tests/vision_shell_test.sh` (no host LVGL build on the build PC); the
  button and layout were checked on unit A instead, in the orientation used.
- Unit B: not touched.
