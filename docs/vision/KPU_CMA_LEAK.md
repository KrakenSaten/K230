# KPU CMA leak: cause and fix (unit B, 2026-11-11)

Follow-up to the segmentation validation, where a standalone KPU harness lost
about 45 MB of CMA per run until reboot.

## Root cause

Three layers, one cause each:

| Layer | Finding | Class |
| --- | --- | --- |
| Kernel `mmz` driver (`drivers/misc/canaan/mmz/mmz.c`) | `MMZ_ALLOC_MEM` calls `dma_alloc_coherent()` and keeps no record per open file. `mmz_fops` has no `.release`. Memory returns only through an explicit `MMZ_FREE_MEM` ioctl, so whatever a process has not freed when it ends is lost until reboot. | VERIFIED (source), VERIFIED (behaviour) |
| `libmmz` and the nncase runtime | `kd_mpi_mmz_deinit()` frees every allocation on the library's list. The runtime keeps freed tensors in its host pool; `shrink_memory_pool()` empties it. Both work when called. | VERIFIED |
| Application | The harness (`kbench`) never made the two calls: 45 MB per run. `pos-vision` makes them when its last net closes (`release_pool()`), but three paths loaded a model without counting as alive, so the release never ran: `vision_kpu_describe()`, and `vision_kpu_open()` / `vision_net_open()` when a model loads and is then refused. | VERIFIED for `describe`; refused opens by code reading |

So the underlying defect is the driver's missing reclaim on close, which needs a
kernel change and is out of scope. Every user-space exit must therefore release
explicitly, and DOORS had three that did not.

## Fix

`core/pocketvision/vision_kpu_nncase.cpp`: a scope guard, `pool_hold`, counts as
a live object while a model is loaded and inspected, and calls `release_pool()`
when the scope ends. It is declared before the loading object in the three
functions above. A successful open still ends with `live == 1`; a refused or
described model now returns the pool. No other file changes.

Harnesses outside the repository must call, after destroying their runtime
objects: `shrink_memory_pool(); kd_mpi_mmz_deinit();`

## Measurements

Unit B, `CmaFree` from `/proc/meminfo`. Runs 2 and 3 read it after
`sync; echo 1 > /proc/sys/vm/drop_caches`, because page cache and tmpfs pages
also sit in CMA. Model: `det-r0-traffic6-yolox-tiny-416.kmodel` (5.9 MB).

| Run | Binary | Command | Before | After | Change |
| --- | --- | --- | --- | --- | --- |
| 1 | installed (`fcb9ebfd…`) | `describe` | 463.3 MB | 457.0 MB | -6.3 MB (caches not dropped) |
| 2 | installed | `describe` | 469.5 MB | 460.6 MB | **-8.9 MB** |
| 3 | fixed (`027d6004…`, stripped) | `describe` | 463.8 MB | 461.9 MB | -1.8 MB |
| 3 | fixed | `face` on the detector model (opens, refused by the caller, closed) | 461.9 MB | 462.5 MB | +0.5 MB |
| 3 | fixed | `describe` on a file that is not a model | 462.5 MB | 462.5 MB | 0 |

Run 3 held three invocations of the fixed binary in one session; net change
-1.3 MB. Idle readings moved by 1 to 3 MB between runs with nothing running, so
this is within noise, not a proven zero. All processes exited; none was left.

## Tests

- Cross-build of `pos-vision` with `POCKETVISION_KPU=1`: no warnings.
- `tests/vision_lint.sh`: 0 failures.
- Hardware: the three runs above. Test files removed from `/tmp`; nothing
  installed, no service touched, no reboot.

## Not tested

- A refused model in `vision_kpu_open()` on hardware (needs the camera path).
- A full inference with the fixed binary, and the live Vision session.
- The host test suite beyond the lint (the nncase file is not built on a host).

## Remaining risks

- SIGKILL, a crash or an OOM kill of `pos-vision` still loses its pool (about
  45 to 57 MB with a model running) until reboot. Only a driver change (track
  allocations per open file, free them in `.release`) can close this.
- `live` is not thread-safe; unchanged, and `pos-vision` uses the runtime from
  one thread.
- Unit B is about 15 MB of CMA down from runs 1 and 2 (465 MB free at the end,
  479 MB at the start) until its next reboot.
