/* The Linux/Doors platform seam for the portable MeshCore core.
 *
 * MeshCore reaches the machine it runs on through four small abstract
 * classes it defines itself - mesh::MillisecondClock, mesh::RTCClock,
 * mesh::RNG and mesh::Radio - plus the Print/Stream pair it inherits from
 * Arduino. Everything in this header is an implementation of one of those on
 * an ordinary Linux process. No protocol logic lives here, and no vendored
 * MeshCore file was edited to make it fit: this is the whole adaptation
 * layer.
 *
 * mesh::Radio is deliberately NOT implemented here. The real one will be an
 * adapter over radiod's IPC, and that service does not exist yet; tests use
 * the in-memory one in test_support/mc_fake_radio.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef MC_PORT_H
#define MC_PORT_H

#include <stddef.h>
#include <stdint.h>

#include <Dispatcher.h>
#include <MeshCore.h>
#include <Utils.h>

namespace mcport {

/* ---- the protocol clock -------------------------------------------------
 *
 * mesh::MillisecondClock::getMillis() returns `unsigned long`, and
 * mesh::Dispatcher compares two of them by casting the difference to signed
 * (`(long)(now - deadline) > 0`, Dispatcher.cpp). On an ESP32 that is 32-bit
 * arithmetic and the cast is what makes the comparison survive the counter
 * wrapping every 49.7 days.
 *
 * On riscv64 and on the x86-64 host, `unsigned long` is 64 bits. The same
 * comparison is then 64-bit and correct for any interval a device will ever
 * see - but only if the value handed to it really is a 64-bit counter. A
 * 32-bit millisecond counter widened into a 64-bit `unsigned long` would be
 * strictly worse than the ESP32: it would still wrap at 2^32, and the signed
 * difference would then be a number near -2^32, which is not greater than
 * zero, so every deadline set before the wrap would read as "not yet
 * reached" - the dispatcher would stop transmitting rather than recover.
 * tests/meshcore_port_test.cpp demonstrates exactly that, against this class
 * and against a deliberately wrapping one.
 *
 * So this clock is a genuine monotonic 64-bit millisecond value taken from
 * CLOCK_MONOTONIC: it does not wrap, it does not go backwards, and it is not
 * affected by the wall clock being set. The static_assert below is what keeps
 * the promise honest - on a platform where `unsigned long` is 32 bits this
 * file fails to build rather than quietly reintroducing the wrap.
 */
static_assert(sizeof(unsigned long) >= 8,
              "the MeshCore protocol clock must be a 64-bit millisecond value; "
              "on a 32-bit-long platform mesh::Dispatcher's timing would wrap at 2^32");

class MonotonicClock : public mesh::MillisecondClock {
public:
  unsigned long getMillis() override;
};

/* Milliseconds since an unspecified fixed point, from CLOCK_MONOTONIC.
 * Exposed on its own because the tests and a future service want the value
 * without an object, and because it is the one function a platform other
 * than Linux would have to replace. */
unsigned long monotonicMillis(void);

/* ---- the wall clock -----------------------------------------------------
 *
 * MeshCore stamps adverts and messages with UNIX epoch seconds through
 * mesh::RTCClock. CLOCK_REALTIME is that, and setCurrentTime() is accepted
 * but not obeyed: a protocol library has no business calling settimeofday(),
 * and on the K230 the system clock belongs to sysd. A node running before
 * its clock has been set will therefore advertise a 1970 timestamp, which is
 * what an unsynced MeshCore device does too.
 */
class SystemRTCClock : public mesh::RTCClock {
public:
  uint32_t getCurrentTime() override;
  void setCurrentTime(uint32_t time) override;

  /* True once getCurrentTime() is past 2001-01-01, the same "the clock has
   * plainly been set" test pocketlog's readers apply. A caller that must not
   * sign a 1970 advert can check this first. */
  bool isSet();
};

/* ---- randomness ---------------------------------------------------------
 *
 * From the operating system's CSPRNG: getrandom(2), falling back to
 * /dev/urandom on a kernel or sandbox without the syscall. Radio noise is
 * NOT used as an entropy source here - MeshCore's own RNG on some boards
 * stirs in RSSI readings, but on Linux the kernel pool is both better seeded
 * and always available, and a protocol library that reached for the radio
 * would have to own one.
 */

/* Fills dest with sz cryptographically secure bytes. Returns false if the
 * host could not supply them, having written nothing a caller should use.
 * sz == 0 succeeds. This is the only entry point that reports failure; use
 * it wherever a failure can be handled. */
bool randomBytes(uint8_t* dest, size_t sz);

/* mesh::RNG over randomBytes(). The interface returns void, so a failure
 * cannot be reported through it: rather than hand MeshCore predictable bytes
 * to build a key or a MAC padding blob out of, this aborts the process. That
 * is the same stance tools/meshcore-frame takes, and it is the safe one - a
 * node that silently signed with a guessable key would be worse than a node
 * that stopped. */
class HostRNG : public mesh::RNG {
public:
  void random(uint8_t* dest, size_t sz) override;
};

#ifdef MC_RNG_TEST_HOOKS
/* Present only in the test build of port/mc_rng.cpp (see its comment). Makes
 * the next randomBytes() call fail, so the failure path can be exercised
 * without a kernel that refuses to produce entropy. */
void randomForceFailureForTest(bool on);
#endif

/* ---- logging ------------------------------------------------------------
 *
 * MeshCore logs through virtual hooks on mesh::Dispatcher (logRx, logTx,
 * logTxFail, getLogDateTime) and, when MESH_PACKET_LOGGING is compiled in,
 * through Serial. Both end up here, so a future MeshCore service can point
 * the whole library at core/pocketlog with one call without this library
 * depending on pocketlog - which would drag core/ and its libraries into
 * something that must stay portable.
 *
 * The levels are pocketlog's, in pocketlog's order, so the adapter is a cast.
 */
enum LogLevel { LOG_DEBUG = 0, LOG_INFO = 1, LOG_WARN = 2, LOG_ERROR = 3 };

typedef void (*LogSink)(LogLevel level, const char* line);

/* Replaces the sink. NULL restores the default, which writes to stderr.
 * Not thread-safe by design: call it once during start-up. */
void setLogSink(LogSink sink);

void logWrite(LogLevel level, const char* fmt, ...)
    __attribute__((format(printf, 2, 3)));

}  // namespace mcport

#endif  /* MC_PORT_H */
