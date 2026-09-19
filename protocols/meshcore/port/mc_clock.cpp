/* The protocol clock and the wall clock. See port/mc_port.h for why the
 * monotonic one must be 64-bit.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "mc_port.h"

#include <time.h>

namespace mcport {

unsigned long monotonicMillis(void) {
  /* The last value actually read. clock_gettime(CLOCK_MONOTONIC) cannot fail
   * on Linux with a valid timespec pointer, but if it ever did, returning
   * zero would make the clock leap backwards and every pending deadline in
   * the dispatcher fire at once. Holding the last value instead stops time,
   * which stalls transmission until the clock works again - a failure that
   * is visible and recoverable rather than one that corrupts the schedule.
   *
   * Not atomic: like the rest of the protocol core, this is written for the
   * single run-to-completion loop MeshCore is built around. A future service
   * that drives two meshes from two threads needs its own clock instance
   * per thread, or this made atomic. */
  static unsigned long last;

  struct timespec ts;
  /* CLOCK_MONOTONIC, not CLOCK_REALTIME: protocol timing must not jump when
   * NTP or sysd sets the wall clock. On the K230 that happens routinely -
   * the board has no RTC, so the clock starts at 1970 and is corrected some
   * seconds into the boot. A dispatcher timing its next transmission off the
   * wall clock would see that correction as decades of elapsed time. */
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return last;

  last = (unsigned long) ts.tv_sec * 1000UL + (unsigned long) (ts.tv_nsec / 1000000L);
  return last;
}

unsigned long MonotonicClock::getMillis() {
  return monotonicMillis();
}

uint32_t SystemRTCClock::getCurrentTime() {
  struct timespec ts;
  if (clock_gettime(CLOCK_REALTIME, &ts) != 0) return 0;
  return (uint32_t) ts.tv_sec;
}

void SystemRTCClock::setCurrentTime(uint32_t time) {
  /* Accepted and ignored. MeshCore calls this when a companion app or a CLI
   * command pushes a time down to the node; on Doors the system clock is
   * sysd's, and a protocol library calling settimeofday() would be both a
   * privilege it does not have and a decision it should not make. Logged at
   * debug so the call is visible when someone wonders why the node's idea of
   * the time did not change. */
  logWrite(LOG_DEBUG, "RTC set to %u ignored: the system clock belongs to sysd", (unsigned) time);
}

bool SystemRTCClock::isSet() {
  /* 2001-01-01T00:00:00Z. Anything below it is an unset clock counting up
   * from the epoch, not a real date. */
  return getCurrentTime() > 978307200u;
}

}  // namespace mcport
