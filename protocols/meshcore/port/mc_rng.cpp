/* Randomness from the host CSPRNG. See port/mc_port.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "mc_port.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/random.h>
#endif

namespace mcport {

#ifdef MC_RNG_TEST_HOOKS
/* Compiled only into the test binaries, never into the library an image
 * would carry - the same arrangement sysd, netd and pos-wave use for their
 * test hooks, and tests/meshcore_lint.sh checks that none of these symbols
 * reached libmeshcore.a.
 *
 * There is no other way to see either half of this function work. The kernel
 * pool does not fail on demand, and on every machine this is built on
 * getrandom(2) is present and succeeds - so the /dev/urandom fallback, which
 * exists for older kernels and for sandboxes that filter the syscall, had
 * never been executed by anything. A fallback that has never run is a guess,
 * and so is the error handling around it.
 *
 * These seams change how the SOURCES behave, not what randomBytes() does
 * about it: the fault injection below sits where the syscall's return value
 * would, and the path taken afterwards is the shipped code. */
static bool        g_force_failure;
static int         g_getrandom_fault;
static const char* g_urandom_path;
static size_t      g_from_getrandom;
static size_t      g_from_urandom;

void randomForceFailureForTest(bool on) { g_force_failure = on; }
void randomSetGetrandomFaultForTest(int fault) { g_getrandom_fault = fault; }
void randomSetUrandomPathForTest(const char* path) { g_urandom_path = path; }

void randomResetCountersForTest(void) { g_from_getrandom = g_from_urandom = 0; }
size_t randomBytesFromGetrandomForTest(void) { return g_from_getrandom; }
size_t randomBytesFromUrandomForTest(void) { return g_from_urandom; }

#define MC_RNG_URANDOM_PATH   (g_urandom_path != NULL ? g_urandom_path : "/dev/urandom")
#define MC_RNG_COUNT_GETRANDOM(n) (g_from_getrandom += (size_t) (n))
#define MC_RNG_COUNT_URANDOM(n)   (g_from_urandom += (size_t) (n))
#else
#define MC_RNG_URANDOM_PATH   "/dev/urandom"
#define MC_RNG_COUNT_GETRANDOM(n) ((void) 0)
#define MC_RNG_COUNT_URANDOM(n)   ((void) 0)
#endif

#if defined(__linux__)
/* getrandom(2), with the test seam in front of it. In the shipped build this
 * compiles to the bare syscall. */
static ssize_t mcGetrandom(uint8_t* dest, size_t sz) {
#ifdef MC_RNG_TEST_HOOKS
  switch (g_getrandom_fault) {
    case MC_GETRANDOM_UNAVAILABLE:
      /* An older kernel, or a seccomp sandbox that filters the call. This is
       * the condition the /dev/urandom fallback exists for. */
      errno = ENOSYS;
      return -1;
    case MC_GETRANDOM_EINTR_ONCE:
      /* Interrupted by a signal before any byte was written. One shot: the
       * loop must retry rather than give up or count a byte. */
      g_getrandom_fault = MC_GETRANDOM_NORMAL;
      errno = EINTR;
      return -1;
    case MC_GETRANDOM_SHORT:
      /* A short read. The kernel is allowed to return fewer bytes than asked
       * for, and the loop has to accumulate. */
      if (sz > 1) sz = 1;
      break;
    case MC_GETRANDOM_ZERO:
      /* Not something a working kernel does for a non-zero request. It is
       * here because if it ever did, a loop that only tested for n < 0 would
       * spin forever; see the `n == 0` break below. */
      return 0;
    default:
      break;
  }
#endif
  return getrandom(dest, sz, 0);
}
#endif

bool randomBytes(uint8_t* dest, size_t sz) {
  if (sz == 0) return true;
  if (dest == NULL) return false;

#ifdef MC_RNG_TEST_HOOKS
  if (g_force_failure) return false;
#endif

  size_t got = 0;

#if defined(__linux__)
  while (got < sz) {
    ssize_t n = mcGetrandom(dest + got, sz - got);
    if (n < 0) {
      if (errno == EINTR) continue;
      break;  /* older kernel or a sandbox without the syscall: fall through */
    }
    if (n == 0) break;  /* a source producing nothing will not start now */
    got += (size_t) n;
    MC_RNG_COUNT_GETRANDOM(n);
  }
#endif

  if (got < sz) {
    int fd = open(MC_RNG_URANDOM_PATH, O_RDONLY);
    if (fd >= 0) {
      while (got < sz) {
        ssize_t n = read(fd, dest + got, sz - got);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        got += (size_t) n;
        MC_RNG_COUNT_URANDOM(n);
      }
      close(fd);
    }
  }

  return got == sz;
}

void HostRNG::random(uint8_t* dest, size_t sz) {
  if (randomBytes(dest, sz)) return;

  /* mesh::RNG::random() returns void, so there is nowhere to report this.
   * The alternative to stopping is handing MeshCore whatever was already in
   * the buffer to make a private key, a MAC padding blob or an ephemeral
   * agreement key out of. */
  logWrite(LOG_ERROR, "no usable source of randomness on this host; refusing to continue");
  abort();
}

}  // namespace mcport
