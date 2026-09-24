# Camera on the T-Display K230: platform research

Date: 2026-09-24. Branch `feat/camera-app-design` (from master `f14658f`).
Desk research only: no hardware was touched, nothing was flashed or deployed.
Unit A was not used.

Classes used here, as the Camera brief asked, mapped onto AGENTS.md's:

- **CONFIRMED** - VERIFIED on unit A, or DOCUMENTED directly in the source that
  builds the image (the vendor tree, the Buildroot output of the RC1 build).
- **LIKELY** - strong indirect evidence; ASSUMED until a test says so.
- **UNKNOWN** - needs unit A.

Sources: the Buildroot output of the pinned SDK in WSL
(`~/work/t-display-k230/k230_linux_sdk/output/k230_pocketos_defconfig`, rebuilt
2026-09-24 for RC1), `vendor/T-Display-K230` (BSP overlay `k230_bsp/overlay/
buildroot-overlay`, called BSP below, and the SDK overlay `k230_linux_sdk/
buildroot-overlay`, called SDK below), `schematic_text.txt`, and unit A's
bring-up records under `docs/hardware/`.

## 1. The camera connector

| Finding | Evidence | Class |
| --- | --- | --- |
| The board has three camera sites: **CAMERA1**, a 24-pin board-to-board connector (`BTB-DF30FC-24DS-0.4V`), and **FPC1** and **FPC2**, 22-pin flex connectors | `schematic_text.txt` page 4 "Video", lines 2335-2336, 2647-2648, 2672-2673 | CONFIRMED (DOCUMENTED) |
| CAMERA1 is on **CSI2** (two lanes, D4/D5), its I2C is `CAM2` = the SoC's **I2C4**, reset `CAM2_RST` = **GPIO21**, clock `MCLK1` = **GPIO13** | schematic lines 214, 222, 319, 3004-3058; device tree below | CONFIRMED (DOCUMENTED + the device tree agrees) |
| FPC1 carries CSI0 and I2C0 (GPIO48/49); FPC2 carries CSI1 and I2C1. Nothing in the Linux device tree uses either | schematic lines 2995-3041, 3141-3151 | LIKELY |
| Camera rails: 2.8 V, 1.8 V and 1.5 V from XC6206 regulators on 3.3 V; which pin gets which is not readable from the text export | schematic lines 2339-2349, 2379, 2400-2419 | CONFIRMED (rails exist); UNKNOWN (pinout) |
| "OV5647" is printed beside CAMERA1, but the fitted and configured sensor is a **GalaxyCore GC2093** | schematic 2373/2416; device tree; unit A I2C scan | CONFIRMED |
| **Correction:** `docs/hardware/T-DISPLAY-K230.md` gave the camera I2C as SDA GPIO49 / SCL GPIO48 (copied from the vendor's `HARDWARE_PINMAP.md`). The device tree and the running board use **IO7/IO8** (I2C4); GPIO48/49 are I2C0, which goes to FPC1. Fixed on this branch | BSP `linux/0036-add-gc2093-camera.patch`; `k230-canmv-rm69a10.dts` `cam_i2c4_pins`; unit A: `0-0037` answers on Linux `i2c-0` (= I2C4) | CONFIRMED |

## 2. What the Linux image has

| Finding | Evidence | Class |
| --- | --- | --- |
| The image is **Linux only** (k230_linux_sdk); the camera is not behind RT-Smart or the big-core MPP (`kd_mpi_vicap_*`) | `docs/BUILD_ENVIRONMENT.md`; `platforms/k230/vendor_sdk_commit.txt` | CONFIRMED |
| Board device tree `k230-canmv-rm69a10.dts`: `&i2c4 { gc2093@37 }`, `&mipi0 { id = <2>; /* CSI2 */ reset-gpios = <&gpio0_ports 21 ...>; canaan,sensor-mclk-id = <1>; -parent = <9>; -div = <25>; }`, `&v4l2isp { dev0-sensor-name = "gc2093"; dev0-sensor-mode = <0>; dev0-sensor-i2c-bus = <0>; }` | RC1 build tree `linux-7d4e1f44.../arch/riscv/boot/dts/canaan/k230-canmv-rm69a10.dts`; `platforms/k230/configs/k230_pocketos_defconfig:27` | CONFIRMED |
| Kernel: `CONFIG_VIDEO_DEV`, `MEDIA_CONTROLLER`, `V4L2_FWNODE`, `VIDEOBUF2_DMA_CONTIG`, `VIDEOBUF2_VMALLOC`, `CMA`, `VPU_CANAAN` (a V4L2 mem-to-mem H.264/HEVC/JPEG codec) are built in. **No in-kernel sensor driver**: every `CONFIG_VIDEO_{OV,IMX,GC}*` is unset | RC1 kernel `.config` | CONFIRMED |
| The camera stack is **vvcam**, out of tree: `vvcam_isp`, `vvcam_mipi`, `vvcam_vb`, `vvcam_isp_subdev`, `vvcam_video mcm_mask=1`, loaded by the vendor's `S31canaan_isp`, which also starts **`isp_media_server`** - a prebuilt, stripped binary with no source that loads the sensor code from `libvvcam.so` | RC1 target `/etc/init.d/S31canaan_isp`, `/lib/modules/6.6.36/updates/vvcam_*.ko`; SDK `package/vvcam/Makefile` | CONFIRMED |
| The GC2093 is driven **from user space** (`libvvcam.so` opens `/dev/i2c-0` with `I2C_SLAVE_FORCE`, 0x37 or 0x7e) | BSP `package/vvcam/src/gc2093.c:150-240` | CONFIRMED (DOCUMENTED) |
| On unit A the chain probes: `vvcam-mipi 9000a800.mipi.0: start probe 2`, `enabled sensor mclk1 parent=9 div=25`, `sensor=gc2093 mode=0 i2c_bus=0`, `register 3 video nodes`; the GC2093 answers at 0x37 on `i2c-0`; `isp_media_server` runs | `docs/hardware/hwcheck-unitA/device-hwcheck-19700101_000523/dmesg.txt:387-408`; `BRINGUP_SESSION_2026-09-07.md` §11.1 | CONFIRMED (VERIFIED on unit A) |
| Device nodes on unit A: `video0` = VPU (mvx), **`video1`, `video2`, `video3` = `vvcam-video.0.0 ... 0.2`**, `video4` = the non-AI 2D engine, `v4l-subdev0` = the ISP | `docs/hardware/hwcheck-unitA/v006-2026-09-09/pos-hwcheck-lora.txt:52-56`; bring-up §11.1 | CONFIRMED (VERIFIED) |
| The three vvcam nodes are the ISP's three outputs - main path (MP) and self paths SP1, SP2 - of one ISP | SDK `package/vvcam/v4l2/video/vvcam_pipeline_link.h:184-196` | CONFIRMED (DOCUMENTED) |
| **No frame has ever been captured on unit A** | `POST_BRINGUP_REVIEW_2026-09-07.md:116` ("not exercised") | CONFIRMED |
| Also in the image: `v4l2-ctl`, `v4l2-compliance`, the vendor's `v4l2-drm`, `k230_camera_capture`, libjpeg 9 and libpng (with headers in the sysroot), FFmpeg, OpenCV | RC1 target and sysroot | CONFIRMED |

## 3. Formats, sizes, how the vendor reads it

| Finding | Evidence | Class |
| --- | --- | --- |
| GC2093 modes: **1920 x 1080 only**, RAW10 RGGB, 2 lanes, 24 MHz; mode 0 at 30 fps, mode 1 at 60 fps; the device tree picks mode 0 | BSP `package/vvcam/src/gc2093.c:583-676` | CONFIRMED (DOCUMENTED) |
| Capture formats a vvcam node offers: NV16, NV12, YUYV, BGR24, BG3P, raw Bayer 8/10/12, P010. Minimum 32 x 16, width aligned to 16, height to 8 | SDK `vvcam_video_register.c:85-153`, `vvcam_video_driver.h:81-85` | CONFIRMED (DOCUMENTED) |
| The ISP output scales: the vendor asks `/dev/video1` for 640 x 360 from the 1080p sensor, and a vendor script for 240 x 240. The scaler lives in the closed `isp_media_server` | vendor launcher `main.c:106-107`; SDK `root/script/sensor.sh:34` | LIKELY |
| **The vendor Camera app:** a worker thread streams `/dev/video1` at **640 x 360 NV16**, MMAP, 4 buffers, 250 ms dequeue timeout, converts to RGB565 on the CPU (rotated 90 in portrait) into an LVGL canvas every 66 ms, and saves its JPEG **from that preview picture** | vendor `k230_launcher/k230_phone_ui/src/main.c:103-128, 10298-10440, 10700-10760` | CONFIRMED (DOCUMENTED) |
| The vendor's still tool `k230_camera_capture`: `/dev/video1`, **1920 x 1080 NV16** (NV12 optional), 5 MMAP buffers, skips 3 frames, rotates 90 by default, writes JPEG with libjpeg | vendor `camera_capture.c:18-24, 784-858` | CONFIRMED (DOCUMENTED) |
| The vendor app also has **"restart ISP"** (`killall isp_media_server` and start it again) | vendor launcher `main.c:10794` | CONFIRMED (DOCUMENTED); why it was needed is UNKNOWN |
| The vendor's preview orientation: rotate 90 in portrait; in landscape no rotation but a **vertical flip** | vendor launcher `main.c:10360-10380` | CONFIRMED that the code does it; what it means for our panel rotation is UNKNOWN (U4) |

## 4. Display path: can the preview be zero-copy?

| Finding | Evidence | Class |
| --- | --- | --- |
| The display controller has 7 planes: OSD4 primary, three video planes (`video_1..3`, NV12 capable), a cursor, two more OSD overlays; rotation on all | SDK `linux/0023-add-gdma-vo-rotation.patch:835-900`; BSP `linux/0043` | CONFIRMED (DOCUMENTED) |
| A vendor zero-copy path exists: DRM buffers exported as dma-buf and queued to the camera as `V4L2_MEMORY_DMABUF`, shown on a video plane (`v4l2-drm`) | SDK `package/display/src/display.c:370-440`; BSP `v4l2-drm/src/lib.c:224-253` | CONFIRMED in source; never run on unit A |
| LVGL draws on the primary plane with **staging** (a copy into the scanout buffer) and plane rotation; whether a video plane is free under it, and whether staging and rotation hold with one in use, is unchecked | BSP lvgl patches 0002, 0004 | UNKNOWN (U12) |
| CMA: 512 MiB reserved at 0x2000_0000; RAM 1 GiB | unit A dmesg | CONFIRMED (VERIFIED) |

**For v1 the preview is not zero-copy.** It is converted on the CPU into an
RGB565 picture and drawn by LVGL, the way the vendor app does it. A video
plane under a transparent LVGL layer would save that work, but it changes how
the shell owns the display and is unproven on this panel; it is a later
option, not a v1 dependency.

## 5. Pins and ownership

| Finding | Evidence | Class |
| --- | --- | --- |
| The camera path (IO7, IO8, IO13, GPIO21) shares no pin with the radio (5, 14-17, 19, 20, 44), touch (23, 24, 36, 37), panel (22, 25), SD (54-59), Wi-Fi (45), audio (32-35) or keyboard (42, 43, 46, 47) | `T-DISPLAY-K230.md` pin tables; patch 0036 | CONFIRMED |
| GPIO21 is claimed by the kernel as `reset` (the camera reset); nothing in Doors touches it | unit A gpioinfo, bring-up §11.1 | CONFIRMED (VERIFIED) |
| **Shared bus:** BSP patch 0058 routes the same I2C4 controller also to IO46/IO47 ("keeping the existing camera I2C4 pins IO7/IO8 active"). Doors' keyboard driver takes IO46/47 over by `/dev/mem` and bit-bangs them, and its header says nothing in the device tree claims them. Camera I2C traffic (the ISP daemon talks to the GC2093 while streaming) may therefore reach the keyboard bus, and the other way round | BSP `linux/0058-...patch`; `ui/shell/kbd_bus_k230.c:5-6`; `KEYBOARD_BRINGUP_2026-09-10.md:84-90` | LIKELY (the routing); UNKNOWN (whether it disturbs anything) - U9 |
| No address clash: camera 0x37; keyboard base board 0x34, 0x6B, 0x55, 0x20-0x27 | as above | CONFIRMED |

## 6. Consequences for the design

- The real backend is **V4L2 on `/dev/video1`**, not a vendor MPP API. Every
  call it needs is standard V4L2; the vendor's `v4l2-drm/src/lib.c` does the
  same sequence. Nothing about it is K230-specific except which node and
  which format to ask for, and the sensor mounting - which is why they sit
  behind `pocketcam`'s backend seam and never reach the app.
- The ISP daemon must be running. Doors' image keeps `S31canaan_isp` (it runs
  at boot today); the backend reports "no camera" when the node is missing and
  "camera busy" when the driver says EBUSY.
- A still at full size probably means stopping the preview, reconfiguring to
  1920 x 1080 and starting again (the vendor tool does this in a separate
  process). Whether SP1/SP2 could stream the preview while MP takes the still
  is UNKNOWN (U2). The backend interface allows either: `still()` may stop
  the stream and the caller restarts it.
- The sensor mounting (rotation and mirror) is a board property the backend
  reports (`pocketcam_info.mount_rotation/mount_mirror`), measured on unit A
  (U4). The fake reports 0.
- No resolution selector, exposure control or camera switch in v1: one
  sensor, one mode, and no evidence yet of which ISP controls work through
  V4L2 rather than the daemon.

## 7. Unknowns that need unit A

| # | Question | How to answer it on unit A |
| --- | --- | --- |
| U1 | Does `/dev/video1` deliver frames to a process that is not the vendor's, with `isp_media_server` running as Doors boots it? | `v4l2-ctl -d /dev/video1 --set-fmt-video=width=640,height=360,pixelformat=NV16 --stream-mmap --stream-count=30`; then `pos-camera snap` once the backend exists |
| U2 | Which node streams which size; can SP1/SP2 preview while MP takes a still? | `v4l2-ctl --list-formats-ext` and parallel streams on video1..3 |
| U3 | Real frame rate at 640 x 360 and at 1920 x 1080; the scaler's limits | `--stream-mmap` with `--verbose` timestamps |
| U4 | Sensor orientation and mirroring relative to the panel at each shell rotation | a still of a marked object; set `mount_rotation`/`mount_mirror` |
| U5 | Colour range of the ISP output (full or limited BT.601): the converter assumes limited | a still of a white and a black card; compare against `v4l2-ctl` colorspace |
| U6 | Time to open, to first frame, and to restart the stream for a still; whether the ISP daemon ever needs a restart | timestamps in `pos-camera` events |
| U7 | CPU of the conversion at 528 x 938 (portrait) and 802 x 452 (landscape) at 10 fps; JPEG encode time of 1080 x 1920 | `top` / `perf stat` on the helper while previewing and capturing |
| U8 | What a second opener gets (EBUSY at open, at REQBUFS, at STREAMON?) | open `/dev/video1` twice |
| U9 | Does streaming disturb the keyboard bus on IO46/47, or keyboard traffic the camera? | stream while typing on a fitted keyboard base; watch both |
| U10 | Which module is fitted where; which rails feed it; the real MCLK | read the board; measure |
| U11 | Power draw streaming vs idle; is the sensor streaming while no one reads? | supply current with and without the Camera screen open |
| U12 | Is a DRM video plane free under LVGL, and does staging/rotation survive it? | only if the CPU conversion (U7) turns out too costly |
| U13 | How the MMAP buffers are mapped (cached, write-combined, uncached); a CPU reading an uncached NV16 frame pixel by pixel can be many times slower than from cached memory | time a conversion straight from the buffer against one from a `memcpy`'d copy |

## 8. Performance and resource risks, and the constraints chosen

| Risk | v1 constraint |
| --- | --- |
| Pixel conversion cost on the C908 (U7) | Preview capped at **10 fps** in the helper (`POCKETCAM_PREVIEW_MIN_INTERVAL_MS` 100); frames past the cap are dropped **before** conversion. Preview source 640 x 360 (the vendor's proven size), scaled to the picture box by nearest neighbour. Raise only after U7 |
| The LVGL thread blocking | Nothing camera-related runs on it: the helper converts, encodes and writes; the app only copies one finished picture (at most ~1 MB) per frame and polls non-blocking every 33 ms. The only waits are destroy() and Try again, bounded at 300 ms + 200 ms |
| Large buffers | Fixed at open: 8 MiB of shared memory (4 slots of 1024 x 1024 RGB565, only touched pages are backed), two RGB565 pictures in the app at the box size (~1 MB each), backend buffers in the driver. Nothing is allocated per frame |
| Repeated malloc/free | None per frame anywhere; the conversion tables live on the helper's stack |
| Cache coherency / DMA ownership (U13) | A frame is used only between DQBUF and QBUF, and never after release; if U13 shows uncached buffers, the backend copies a frame out once before converting |
| Pixel format | NV16 as the vendor uses it; NV12 supported by the converter in case it is cheaper to fetch |
| Encoding cost | JPEG quality 88 in the helper, never in the shell; the capture watchdog is 20 s |
| Power (U11) | The camera is open only while the screen is; review stops the stream |
| Re-initialisation cost (U6) | Paid on every visit in v1; a warm camera is the trigger to move to camerad (ADR-006) |
| Storage on the root filesystem | 500 photos / 64 MiB cap, and 48 MiB must stay free (pocketcam_store.h) |

## 9. Work after v0.0.12

In order; nothing before step 1 needs the hardware, everything from it does.

1. **Bench, no Doors code:** answer U1, U2, U3, U8 and U13 with `v4l2-ctl`
   on unit A over SSH, with the Camera screen closed.
2. **The v4l2 backend** in `core/pocketcam` (`pocketcam_v4l2_ops`): open
   `/dev/video1` `O_RDWR | O_NONBLOCK`; `VIDIOC_QUERYCAP`; `VIDIOC_S_FMT`
   NV16 640 x 360; `VIDIOC_REQBUFS` MMAP x4, `QUERYBUF` + `mmap`, `QBUF` all;
   `STREAMON`; `poll` + `DQBUF` bounded by the caller's timeout; `QBUF` on
   release. `still()`: `STREAMOFF`, re-`S_FMT` 1920 x 1080, `REQBUFS`, skip 3
   frames, dequeue one; the caller restarts the preview. Close unmaps and
   closes in every path. Map errno: ENOENT/ENODEV -> -ENODEV, EBUSY ->
   -EBUSY, a missing `isp_media_server` -> -ENODEV with a reason.
3. **Mounting (U4) and colour range (U5)** measured and set in the backend.
4. **libjpeg in the build:** add `jpeg` to `POCKETOS_DEPENDENCIES` in
   `platforms/k230/package/pocketos/pocketos.mk` and build with
   `POCKETCAM_JPEG=1` (already compile-checked for riscv64 on this branch).
   It is already in the image, so this adds a build dependency, not a
   package - the same step alsa-lib took for Wave.
5. **Packaging:** add `usr/bin/pos-camera` to `platforms/k230/scripts/
   deploy.sh`'s lists (and its staging test), like `pos-wave`.
6. **Measure U6, U7, U11** with the app; revisit the 10 fps cap and the
   capture watchdog; check U9 with a keyboard base fitted.
7. **Unit A gate** for the app (a gate sheet like FILES_GATE.md), then the
   owner's decision on ADR-006 and DS §34.
8. **Licensing:** `vvcam` and `isp_media_server` are open item (b) in
   `docs/LICENSING.md`; a Camera that depends on them inherits that.
