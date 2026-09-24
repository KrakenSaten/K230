# ADR-006: Who owns the camera

Status: Proposed (awaiting the product owner). Branch `feat/camera-app-design`,
not merged.
Date: 2026-09-24
Deciders: product owner (final), AI engineering partner (author)

## Context

Doors gets its first camera user: the Camera app (docs/apps/CAMERA.md), a
live picture, a shutter and a look at the photo just taken. It needs the
camera only while its own screen is open, and never in the background.

Constraints already binding:

- ADR-002 point 2: "Services own hardware exclusively." Apps "never touch
  hardware; they talk to services over pocketipc" (ui/shell/app.h). ADR-002
  names a later `camerad` and a `camera.*` API.
- ADR-004 settled audio with a per-operation helper process, as a narrow
  exception for Wave, and says a later need "is decided against ADR-002, not
  by extending this". So this is its own decision.
- The shell is one LVGL loop; nothing on it may block (the owner's standing
  rule for Wave and Files). Files has since brought one worker thread into
  the shell (apps/files/files_job.h), so a thread is possible, but it is not
  an owner of hardware.

Facts that shape it (docs/hardware/CAMERA_PLATFORM_RESEARCH.md):

- The camera is V4L2 on the vendor's out-of-tree `vvcam` modules, driven by
  a closed, prebuilt `isp_media_server` daemon that the vendor boot script
  starts. CONFIRMED in source and in unit A's boot log.
- Nobody has taken a frame from it on unit A yet. The vendor's own Camera
  app carries a "restart ISP" button, which says the stack can get stuck.
- A preview frame has to be converted on the CPU (NV16 to RGB565, turned and
  scaled), and a photo encoded as JPEG. Both cost real CPU on the C908, and
  neither amount is measured.
- A rotation restarts the whole shell by `execv` (ui/shell/shell.c), so
  whoever holds the camera has to let go when the shell goes away.

## Options

### A. In the shell: the app opens the camera, a worker thread streams

- Pro: least code; no copy between processes; lowest latency.
- Con: contradicts ADR-002 and app.h as written; a driver stuck in an ioctl
  holds a thread the app must join when it closes, and the join freezes the
  panel; vendor code faults (the closed ISP stack, libjpeg) happen in the
  shell; conversion and encoding CPU is spent in the shell's process.

### B. `camerad`: a resident service over pocketipc (ADR-002 as written)

- Pro: the letter of ADR-002; one owner for every future camera user; a
  place to keep the ISP warm between apps; a public `camera.*` contract.
- Con: a permanent daemon, init script, supervisor entry and versioned API
  for a device that is used for minutes at a time from one screen; pictures
  cannot travel in pocketipc's JSON, so it needs shared memory and descriptor
  passing that pocketipc does not have today; the largest change of the
  three, most of it contract work before a single frame has been seen.

### C. A helper process per Camera session, on a small camera library (recommended)

- `core/pocketcam`: the camera layer - a backend seam (the fake backend for
  hosts, the v4l2 backend to be written), pixel conversion, the photo store
  and the still encoder.
- `pos-camera`: a process the app starts when its screen opens and ends when
  it closes. It is the only thing that opens the camera. Commands and events
  go over a socketpair, one line each (core/pocketcam/pocketcam_proto.h);
  pictures go through a sealed shared-memory region with explicit slot
  ownership, and the app copies each picture out before it hands the slot
  back.
- Pro: no camera, conversion or encoding in the shell; a helper stuck in the
  driver is killed by the session's watchdog and the panel keeps working;
  PR_SET_PDEATHSIG ends it with the shell (and with its rotation restart);
  nothing holds the camera when the screen is closed; the bench tool
  (`pos-camera probe`, `pos-camera snap`) and the app are one binary; the
  whole path is host-testable end to end on the fake backend.
- Con: not a pocketipc service, so not the letter of ADR-002; a second small
  protocol outside `docs/api/`; a process start and a camera open on every
  visit to the screen (cost unmeasured, see U6); one copy of each preview
  picture between processes.

## Decision (proposed)

**Option C.** The camera still has exactly one owner at a time and the app
never opens it; the owner lives as long as the screen that needs it.
ADR-002 stays the rule; this records where the camera departs from it and
why, in the same shape as ADR-004 without extending ADR-004.

Revisit and move to Option B when any of these becomes true:

- a second user needs the camera while the Camera screen is not open
  (a background scan, another app sharing the stream);
- the ISP open cost measured on unit A (U6) is too high to pay on every
  visit, so the camera has to stay warm;
- anything outside the shell needs the camera (`doors camera ...`).

`pocketcam` then becomes camerad's backend unchanged, and the session's line
protocol becomes the first draft of `camera.*`.

## Consequences

Needed now (on the branch): `core/pocketcam`, `pos-camera`, the Camera app
and their tests; a fake backend that is labelled as such on screen.

Needed before a real camera is used: the v4l2 backend, written and gated on
unit A (CAMERA_PLATFORM_RESEARCH.md, "Work after v0.0.12"); libjpeg as a build
dependency of the pocketos package (it is already in the image); a decision on
vvcam's and `isp_media_server`'s licence status (docs/LICENSING.md already
lists them as open).

Risks: another program that opens `/dev/video1` - the vendor launcher, a
bench `v4l2-ctl` - gets EBUSY from the driver, and the app says the camera is
busy; there is no lock of Doors' own yet (U8 decides whether one is needed).
A helper stuck in the kernel cannot be killed; it is left as a zombie and the
next open reports busy until the driver lets go.

Migration cost to B: a daemon wrapper around what `pos-camera session`
already does, plus descriptor passing in pocketipc for the shared memory.

## Evidence

- ADR-002 and ADR-004 wording, app.h: DOCUMENTED (repository).
- vvcam, `isp_media_server`, `/dev/video1..3` on unit A: VERIFIED
  (docs/hardware/BRINGUP_SESSION_2026-09-07.md §11.1, unit A dmesg).
- The vendor app's "restart ISP" action: DOCUMENTED (vendor launcher source).
- Conversion and encoding cost on the C908, ISP open time: ASSUMED significant,
  unmeasured.
- Helper behaviour - watchdog, crash, death signal, fifty opens and closes, no
  descriptor or child left: VERIFIED on the host (tests/camera_session_test.c,
  tests/camera_app_test.c); not on hardware.
