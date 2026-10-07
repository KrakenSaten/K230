# DRAFT - not sent. For the product owner to review and send.

Prepared 2026-10-07 (docs/licensing/APACHE_2_READINESS.md §14.2, B5 and B6).
The owner decides whether, how and to whom to send it (for example a
GitHub issue on kendryte/nncase and kendryte/k230_linux_sdk, or Canaan's
developer contact).

---

**Subject:** Redistribution terms for the K230 nncase runtime archive and isp_media_server in a Linux image

Hello,

We build Doors, a Linux-based user environment for a K230 handheld board
(LILYGO T-Display K230), from the K230 Linux SDK
(github.com/kendryte/k230_linux_sdk, commit
22d02c6b6783a57a3aca7eb3160e313e772cb710). We would like to distribute
SD-card images publicly and want to do so on terms you have stated. We could
not find a licence statement for two components and would be grateful for
one.

1. **The nncase 2.11.0 K230 runtime archive**
   `nncase_k230_v2.11.0_runtime_linux.tgz` (sha256
   28680932ac879d8591fbaaaab7b8c1ee2d305c2a82471fb2f38c449316cfb91f), from
   https://github.com/kendryte/nncase/releases/tag/v2.11.0. We link its
   static libraries `libNncase.Runtime.Native.a`,
   `libnncase.rt_modules.k230.a` and `libfunctional_k230.a` into one
   program.
   - The repository is Apache-2.0, and we are applying that licence to the
     generic runtime. The archive itself contains no licence file, and the
     README says the K230 sources are not open source.
   - Do `libnncase.rt_modules.k230.a` and `libfunctional_k230.a` also fall
     under Apache-2.0, as your `nncase-kpu` 2.11.0 package on PyPI does?
   - If not, under what terms may they be redistributed in binary form
     inside a device image?
   - What attribution or notice should accompany them?

2. **`isp_media_server`** (V6.5.0) and the `vvcam` user-space files. The
   SDK's `buildroot-overlay/package/vvcam` installs `/usr/bin/isp_media_server`
   (prebuilt; the SDK copy's sha256 is
   873d181cd184f0f1770b0bf60676db0a8359ea04593cdd36bf9ddc5d6bcb40e5), `libvvcam.so` and the
   `/etc/vvcam` tuning files; `S31canaan_isp` starts it, and the camera
   needs it.
   - Under what licence may `isp_media_server` be redistributed in binary
     form inside a device image?
   - Do VeriSilicon's terms also apply to it? If so, what do they require?
   - Are `libvvcam.so` and the `/etc/vvcam` files covered by the SDK's
     BSD-2-Clause licence?
   - Does `isp_media_server` need anything else that would have to be
     redistributed with it? We see `libmxml` linked.

A short written statement (for example a LICENSE file added to the release
or package, or a reply we may quote) would be enough for us. Thank you.

Kind regards,
[name, project, contact]
