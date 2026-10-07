# DRAFT - not sent. For the product owner to review and send.

Prepared 2026-10-07 (docs/licensing/APACHE_2_READINESS.md §14.2, B9). The
V3.0.2 download page (https://www.xrvm.cn/community/download?id=4433353576298909696)
lists xuantie@service.alibaba.com; the owner decides whether and where to
send it.

---

**Subject:** Corresponding source for Xuantie-900-gcc-linux-6.6.0-glibc-x86_64-V3.0.2 (build B-20250410)

Hello,

We build a Linux image for a K230-based device with your toolchain
`Xuantie-900-gcc-linux-6.6.0-glibc-x86_64-V3.0.2-20250410.tar.gz`. `gcc -v`
reports "Xuantie-900 linux-6.6.0 glibc gcc Toolchain V3.0.2 B-20250410". The
image ships these runtime files from its `sysroot/lib64/lp64d`:
- glibc 2.33: libc, libm, libpthread, libdl, librt, libutil, libnsl,
  libresolv, libanl, libnss_files, libnss_dns, libcrypt.so.1, the dynamic
  loader and ldd;
- from GCC 14.1.1: libgcc_s, libstdc++ (with its GDB helper script),
  libatomic and libgomp.

To meet the LGPL-2.1 and GPL-3.0 obligations for these libraries, we need
their corresponding source. We could not find it:
- the V3.0.2 page offers binaries and manuals only;
- the newest tag in github.com/XUANTIE-RV/xuantie-gnu-toolchain is V3.0.1.

Could you please provide, or point us to:

1. the source archive or exact revisions of glibc, GCC and binutils used
   for build B-20250410 (for example the XUANTIE-RV/glibc and XUANTIE-RV/gcc
   commits);
2. any patches applied on top of those revisions;
3. the build scripts and configure options used for this build;
4. or, if V3.0.2 was built from the V3.0.1 sources plus known changes, a
   statement saying so.

Thank you.

Kind regards,
[name, project, contact]
