/* Randomness from the host CSPRNG. See port/mc_port.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
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
/* Compiled only into tests/meshcore_port_test, never into the library an
 * image would carry - the same arrangement sysd, netd and pos-wave use for
 * their test hooks. There is no other way to see the failure path: the
 * kernel pool does not fail on demand, and a randomness source that has
 * never been tested failing is a randomness source whose failure handling is
 * a guess. */
static bool g_force_failure;
void randomForceFailureForTest(bool on) { g_force_failure = on; }
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
    ssize_t n = getrandom(dest + got, sz - got, 0);
    if (n < 0) {
      if (errno == EINTR) continue;
      break;  /* older kernel or a sandbox without the syscall: fall through */
    }
    got += (size_t) n;
  }
#endif

  if (got < sz) {
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
      while (got < sz) {
        ssize_t n = read(fd, dest + got, sz - got);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        got += (size_t) n;
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
