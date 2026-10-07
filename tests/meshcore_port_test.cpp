/*
 * protocols/meshcore: the Linux/Doors platform seam.
 *
 * The protocol itself is tested in tests/meshcore_core_test.cpp. This file is
 * about the four small things the port supplies underneath it - the
 * monotonic clock, the wall clock, the host CSPRNG and the logging sink - and
 * about one claim in particular: that the protocol clock is a genuine 64-bit
 * millisecond value and not an ESP32 32-bit counter widened into a 64-bit
 * `unsigned long`.
 *
 * That claim is worth a test rather than a comment because the failure it
 * prevents is silent. mesh::Dispatcher decides whether a deadline has passed
 * with `(long)(now - deadline) > 0` (vendor/RIFT src/Dispatcher.cpp,
 * millisHasNowPassed). On an ESP32, where both types are 32 bits, that cast
 * is what makes the comparison survive the counter wrapping. On riscv64 and
 * on the x86-64 host both types are 64 bits, so the same line is correct for
 * any interval a device will see - but only for a counter that actually
 * counts in 64 bits. Widen a wrapping 32-bit counter into it and the
 * comparison does not merely lose precision: at the wrap it reports every
 * outstanding deadline as still in the future, and the dispatcher stops
 * transmitting. The tests below show both halves of that.
 *
 * Nothing here needs LVGL, DRM, RadioLib, SPI, GPIO, an Arduino runtime or
 * any hardware, which is itself part of what is being checked.
 *
 * Built and run by protocols/meshcore/Makefile:
 *   make meshcore-core-test        (from the top of the repository)
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <Mesh.h>
#include <MeshCore.h>
#include <helpers/SimpleMeshTables.h>
#include <helpers/StaticPoolPacketManager.h>

#include "mc_fake_radio.h"
#include "mc_port.h"

static int failed;
static int checks;

static void check(const char* name, bool ok) {
  printf("%s %s\n", ok ? "ok  " : "FAIL", name);
  checks++;
  failed += !ok;
}

/* ---- 1. the monotonic clock --------------------------------------------- */

static void test_monotonic_clock(void) {
  /* The promise the port makes, asserted at compile time in mc_port.h and
   * again here so the running binary states it too. */
  check("unsigned long is 64 bits on this build", sizeof(unsigned long) == 8);

  mcport::MonotonicClock clock;

  unsigned long first = clock.getMillis();
  check("the clock is not stuck at zero", first > 0);

  /* Non-decreasing over a long run of reads. A wall clock would fail this
   * across an NTP step; CLOCK_MONOTONIC cannot. */
  bool non_decreasing = true;
  unsigned long prev = first;
  for (int i = 0; i < 200000; i++) {
    unsigned long now = clock.getMillis();
    if (now < prev) { non_decreasing = false; break; }
    prev = now;
  }
  check("200000 reads are non-decreasing", non_decreasing);

  /* And it does advance. A clock that never moved would also be
   * non-decreasing, so this is the other half of the claim. Busy-wait rather
   * than sleep: this has to finish whatever the scheduler is doing. */
  struct timespec start, now_ts;
  clock_gettime(CLOCK_MONOTONIC, &start);
  unsigned long before = clock.getMillis();
  for (;;) {
    clock_gettime(CLOCK_MONOTONIC, &now_ts);
    long elapsed_ns = (now_ts.tv_sec - start.tv_sec) * 1000000000L
                    + (now_ts.tv_nsec - start.tv_nsec);
    if (elapsed_ns > 5000000L) break;   /* 5 ms */
  }
  unsigned long after = clock.getMillis();
  check("the clock advances", after > before);
  check("it advanced by a plausible number of milliseconds",
        after - before >= 4 && after - before < 2000);

  /* The free function and the class must agree - a future service may use
   * either. */
  unsigned long a = mcport::monotonicMillis();
  unsigned long b = clock.getMillis();
  check("the free function and the class agree", b >= a && b - a < 100);

  /* The value has to be large enough to be a real millisecond count since
   * boot, not a truncated one. Nothing can be asserted about the upper bits
   * on a freshly booted machine, but a value that had been masked to 32 bits
   * could never exceed 2^32-1, and one that is genuinely 64-bit arithmetic
   * must survive being added to without wrapping. */
  unsigned long far_future = clock.getMillis() + 0x100000000UL;
  check("adding 2^32 ms to the clock does not wrap", far_future > clock.getMillis());
}

/* ---- 2 and 3. dispatcher timing across 2^32 ----------------------------
 *
 * mesh::Dispatcher::millisHasNowPassed() and futureMillis() are protected, so
 * they are reached here the way any subclass reaches them. This node does
 * nothing else: it exists to ask the dispatcher's own timing helpers a
 * question at three values of the clock.
 */
class TimingProbe : public mesh::Mesh {
public:
  TimingProbe(mesh::Radio& radio, mesh::MillisecondClock& ms, mesh::RNG& rng,
              mesh::RTCClock& rtc, mesh::PacketManager& mgr, mesh::MeshTables& tables)
      : mesh::Mesh(radio, ms, rng, rtc, mgr, tables) { }

  bool hasPassed(unsigned long t) const { return millisHasNowPassed(t); }
  unsigned long future(int ms) const { return futureMillis(ms); }
};

/* The three values the task names, as a 64-bit clock sees them. */
static const unsigned long BELOW = 0xFFFFFFFFUL;        /* 2^32 - 1 */
static const unsigned long AT    = 0x100000000UL;       /* 2^32     */
static const unsigned long ABOVE = 0x100000001UL;       /* 2^32 + 1 */

static void test_dispatcher_timing_across_2_32(void) {
  mctest::FakeAir air;
  mctest::FakeRadio radio(air);
  mcport::HostRNG rng;
  mctest::TestRTCClock rtc;
  StaticPoolPacketManager mgr(8);
  SimpleMeshTables tables;

  const unsigned long points[3] = { BELOW, AT, ABOVE };
  const char* names[3] = { "2^32 - 1", "2^32", "2^32 + 1" };

  for (int i = 0; i < 3; i++) {
    mctest::TestClock clock(points[i]);
    TimingProbe probe(radio, clock, rng, rtc, mgr, tables);

    char label[128];

    /* A deadline 100 ms in the past has passed. */
    snprintf(label, sizeof(label), "at %s a deadline 100 ms in the past has passed", names[i]);
    check(label, probe.hasPassed(points[i] - 100));

    /* A deadline 100 ms in the future has not. */
    snprintf(label, sizeof(label), "at %s a deadline 100 ms ahead has not passed", names[i]);
    check(label, !probe.hasPassed(points[i] + 100));

    /* futureMillis lands where it should, and is not yet due. */
    unsigned long due = probe.future(500);
    snprintf(label, sizeof(label), "at %s futureMillis(500) is now + 500", names[i]);
    check(label, due == points[i] + 500);

    snprintf(label, sizeof(label), "at %s that deadline is not yet due", names[i]);
    check(label, !probe.hasPassed(due));

    /* Advance past it and it becomes due. This is the step that crosses the
     * boundary when the clock started at 2^32 - 1. */
    clock.advance(501);
    snprintf(label, sizeof(label), "at %s the deadline is due after advancing past it", names[i]);
    check(label, probe.hasPassed(due));
  }

  /* The crossing itself, in one deadline: set at 2^32 - 1000, due at
   * 2^32 + 1000, checked from both sides. A 32-bit counter cannot express
   * this interval at all. */
  {
    mctest::TestClock clock(AT - 1000);
    TimingProbe probe(radio, clock, rng, rtc, mgr, tables);
    unsigned long due = probe.future(2000);
    check("a deadline set before 2^32 and due after it is not due before",
          due == AT + 1000 && !probe.hasPassed(due));
    clock.set(AT);
    check("still not due exactly at 2^32", !probe.hasPassed(due));
    clock.set(AT + 1001);
    check("due once the clock passes it", probe.hasPassed(due));
  }
}

/* The other half: the same helpers, driven by a clock that counts in a
 * 64-bit `unsigned long` but wraps at 2^32 - which is what emulating an
 * ESP32 millis() here would produce. The point of these checks is that they
 * assert BROKEN behaviour. If a future change makes them fail, the emulation
 * has stopped being broken, which would be news; what must never happen is
 * the port adopting this clock. */
static void test_a_widened_32_bit_clock_would_fail(void) {
  mctest::FakeAir air;
  mctest::FakeRadio radio(air);
  mcport::HostRNG rng;
  mctest::TestRTCClock rtc;
  StaticPoolPacketManager mgr(8);
  SimpleMeshTables tables;

  /* 1000 ms before the wrap, with a deadline 2000 ms out. On a real 64-bit
   * clock that deadline is at 2^32 + 1000 and everything behaves - the block
   * above proves it. Here the clock wraps to 0 instead. */
  mctest::WrappingClock wrapping(AT - 1000);
  TimingProbe broken(radio, wrapping, rng, rtc, mgr, tables);

  unsigned long due = broken.future(2000);
  check("WIDENED-32 the deadline is computed before the wrap",
        due == AT + 1000 && !broken.hasPassed(due));

  /* Advance 2001 ms. Real elapsed time is now past the deadline. */
  wrapping.advance(2001);
  check("WIDENED-32 the clock has wrapped to just above zero",
        wrapping.getMillis() == 1001UL);

  /* And this is the failure: the deadline is in the past by 1 ms of real
   * time, and the dispatcher reports it as not reached. Every queued
   * transmission behind such a deadline would stop. */
  check("WIDENED-32 a passed deadline reads as NOT passed, which is the bug",
        !broken.hasPassed(due));

  /* It does not recover on its own either: the deadline stays unreachable
   * for another 2^32 ms - 49.7 days - of counting. */
  wrapping.advance(60UL * 60UL * 1000UL);   /* an hour later */
  check("WIDENED-32 an hour later it is still reported as not passed",
        !broken.hasPassed(due));

  /* The same deadline, the same elapsed time, on the port's real clock. */
  mctest::TestClock genuine(AT - 1000);
  TimingProbe good(radio, genuine, rng, rtc, mgr, tables);
  unsigned long due2 = good.future(2000);
  genuine.advance(2001);
  check("REAL-64 the same deadline after the same elapsed time HAS passed",
        good.hasPassed(due2));
  check("REAL-64 the clock kept counting past 2^32",
        genuine.getMillis() == AT + 1001);
}

/* The outbound queue is where a 2^32 crossing could actually bite, and it is
 * worth its own checks rather than trusting the helpers above.
 *
 * mesh::PacketManager's interface is uint32_t on both sides -
 * queueOutbound(..., uint32_t scheduled_for) and getNextOutbound(uint32_t
 * now) - while the dispatcher's clock is `unsigned long`. Every deadline is
 * therefore truncated to 32 bits on the way in, and every "now" is truncated
 * the same way on the way out, and PacketQueue compares them with the
 * wrap-safe `(int32_t)(scheduled - now) > 0` (vendor/RIFT
 * src/helpers/StaticPoolPacketManager.cpp). Consistent truncation plus a
 * wrap-safe compare is correct across the boundary for any delay shorter
 * than about 24.8 days, which every MeshCore delay is by four orders of
 * magnitude. These checks hold that reasoning to the code.
 *
 * This is the one place the 32-bit window genuinely survives into the port,
 * and it survives correctly. It is not a reason to feed the dispatcher a
 * 32-bit clock: the block above shows what that does.
 */
static void test_outbound_queue_across_2_32(void) {
  StaticPoolPacketManager mgr(8);

  mesh::Packet* pkt = mgr.allocNew();
  check("the pool hands out a packet", pkt != NULL);
  if (pkt == NULL) return;

  /* Now = 2^32 - 1000, due 2000 ms later, i.e. at 2^32 + 1000. */
  const unsigned long now_before = AT - 1000;
  const unsigned long due64 = now_before + 2000;
  mgr.queueOutbound(pkt, 0, (uint32_t) due64);

  check("nothing is due 1000 ms before the wrap",
        mgr.getOutboundCount((uint32_t) now_before) == 0);
  check("nothing is due 500 ms before the wrap",
        mgr.getOutboundCount((uint32_t) (AT - 500)) == 0);
  check("nothing is due exactly at 2^32",
        mgr.getOutboundCount((uint32_t) AT) == 0);
  check("nothing is due 999 ms after the wrap",
        mgr.getOutboundCount((uint32_t) (AT + 999)) == 0);
  check("it is due 1001 ms after the wrap",
        mgr.getOutboundCount((uint32_t) (AT + 1001)) == 1);

  mesh::Packet* got = mgr.getNextOutbound((uint32_t) (AT + 1001));
  check("and the queue hands it back once it is due", got == pkt);
  check("the queue is then empty",
        mgr.getOutboundCount((uint32_t) (AT + 5000)) == 0);
  if (got) mgr.free(got);
  check("the pool is whole again", mgr.getFreeCount() == 8);

  /* The same thing one wrap further out, to show it is the arithmetic and
   * not the particular value of 2^32. */
  mesh::Packet* p2 = mgr.allocNew();
  const unsigned long far = 3UL * AT - 750;
  mgr.queueOutbound(p2, 0, (uint32_t) (far + 1500));
  check("nothing is due before a later wrap", mgr.getOutboundCount((uint32_t) far) == 0);
  check("it is due after that later wrap",
        mgr.getOutboundCount((uint32_t) (far + 1501)) == 1);
  mesh::Packet* g2 = mgr.getNextOutbound((uint32_t) (far + 1501));
  if (g2) mgr.free(g2);
  check("the pool is whole after the later wrap too", mgr.getFreeCount() == 8);
}

/* ---- 4. the host RNG ---------------------------------------------------- */

static void test_host_rng(void) {
  uint8_t buf[256];

  /* Zero bytes is a success, and must not touch the buffer. */
  memset(buf, 0xA5, sizeof(buf));
  check("a zero-length request succeeds", mcport::randomBytes(buf, 0));
  check("a zero-length request writes nothing", buf[0] == 0xA5);

  /* A NULL destination is refused rather than crashed on. */
  check("a NULL destination is refused", !mcport::randomBytes(NULL, 8));

  /* Every requested size is filled. The buffer is pre-set to a constant and
   * the test requires the result not to be that constant repeated - which is
   * what a short read leaving the tail untouched would look like. */
  static const size_t sizes[] = { 1, 2, 15, 16, 17, 31, 32, 64, 128, 255, 256 };
  bool all_filled = true;
  bool all_varied = true;
  for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
    size_t n = sizes[i];
    memset(buf, 0x00, sizeof(buf));
    if (!mcport::randomBytes(buf, n)) { all_filled = false; continue; }

    /* The tail past n must be untouched. */
    for (size_t j = n; j < sizeof(buf); j++) {
      if (buf[j] != 0x00) { all_filled = false; break; }
    }
    /* And for anything above a couple of bytes, the filled part must not be
     * all one value. (For n == 1 or 2 that would be a real possibility, so
     * they are excluded rather than made flaky.) */
    if (n >= 8) {
      bool varied = false;
      for (size_t j = 1; j < n; j++) if (buf[j] != buf[0]) { varied = true; break; }
      if (!varied) all_varied = false;
    }
  }
  check("every requested size is filled exactly", all_filled);
  check("the bytes are not a constant", all_varied);

  /* Two draws must differ. With 32 bytes, a collision is a broken RNG rather
   * than bad luck. */
  uint8_t a[32], b[32];
  bool ok_a = mcport::randomBytes(a, sizeof(a));
  bool ok_b = mcport::randomBytes(b, sizeof(b));
  check("two 32-byte draws both succeed", ok_a && ok_b);
  check("two 32-byte draws differ", memcmp(a, b, sizeof(a)) != 0);

  /* A crude spread check over a large sample: every byte value should appear
   * at least once in 64 KiB of output. This catches a source that is stuck
   * on a subset far more cheaply than a statistical test, and cannot fail by
   * chance on a working CSPRNG. */
  {
    static uint8_t big[65536];
    bool seen[256];
    memset(seen, 0, sizeof(seen));
    if (mcport::randomBytes(big, sizeof(big))) {
      for (size_t i = 0; i < sizeof(big); i++) seen[big[i]] = true;
      bool all = true;
      for (int v = 0; v < 256; v++) if (!seen[v]) all = false;
      check("all 256 byte values appear in 64 KiB of output", all);
    } else {
      check("all 256 byte values appear in 64 KiB of output", false);
    }
  }

  /* ---- the failure path ----
   * The kernel pool does not fail on request, so this is the one place a
   * test hook is used: port/mc_rng.cpp is compiled a second time with
   * MC_RNG_TEST_HOOKS for the test binaries only, and the shipped object in
   * libmeshcore.a does not contain it (tests/meshcore_lint.sh checks that).
   * Without it the error return would never have been executed. */
  mcport::randomForceFailureForTest(true);
  memset(buf, 0xC3, sizeof(buf));
  check("a failing source reports failure", !mcport::randomBytes(buf, 32));
  check("a failing source does not silently succeed with stale bytes", buf[0] == 0xC3);
  check("a zero-length request still succeeds while the source is failing",
        mcport::randomBytes(buf, 0));

  mcport::randomForceFailureForTest(false);
  check("the source recovers when the fault is cleared", mcport::randomBytes(buf, 32));
}

/* ---- 4b. the two sources underneath randomBytes() -----------------------
 *
 * randomBytes() has two: getrandom(2), and /dev/urandom for a kernel or a
 * sandbox without it. On every machine this is built on the first one always
 * succeeds, so until these seams existed the second had never once been
 * executed - nor had the loop's handling of an interrupted call, of a short
 * result, or of a source that opens and then gives nothing. The test build of
 * port/mc_rng.cpp injects at the point each source returns, so everything
 * after that is the shipped code taking its real path, and two counters say
 * which source actually supplied the bytes rather than leaving the test to
 * assume it.
 *
 * The policy is not what is under test and does not change: getrandom first,
 * /dev/urandom second, never radio noise.
 */
static void test_host_rng_fallback(void) {
  uint8_t buf[64];
  uint8_t other[32];

  /* ---- 1. getrandom succeeds ---- */
  mcport::randomSetGetrandomFaultForTest(mcport::MC_GETRANDOM_NORMAL);
  mcport::randomSetUrandomPathForTest(NULL);
  mcport::randomResetCountersForTest();
  memset(buf, 0, sizeof(buf));
  check("getrandom alone fills the request", mcport::randomBytes(buf, 32));
  check("and all 32 bytes came from getrandom",
        mcport::randomBytesFromGetrandomForTest() == 32);
  check("so the fallback was never reached",
        mcport::randomBytesFromUrandomForTest() == 0);

  /* ---- 2 and 3. getrandom is unavailable, /dev/urandom serves ----
   * ENOSYS is what an older kernel gives, and what a seccomp filter is most
   * likely to give; either way it is the condition the fallback exists for. */
  mcport::randomSetGetrandomFaultForTest(mcport::MC_GETRANDOM_UNAVAILABLE);
  mcport::randomResetCountersForTest();
  memset(buf, 0, sizeof(buf));
  check("the request still succeeds with getrandom unavailable",
        mcport::randomBytes(buf, 32));
  check("and every byte came from /dev/urandom",
        mcport::randomBytesFromUrandomForTest() == 32);
  check("with none from getrandom",
        mcport::randomBytesFromGetrandomForTest() == 0);

  bool varied = false;
  for (size_t i = 1; i < 32; i++) if (buf[i] != buf[0]) varied = true;
  check("the fallback's bytes are not a constant", varied);
  check("a second fallback draw succeeds", mcport::randomBytes(other, sizeof(other)));
  check("and differs from the first", memcmp(buf, other, sizeof(other)) != 0);

  /* ---- 4. both mechanisms fail ---- */
  mcport::randomSetUrandomPathForTest("/nonexistent/meshcore-no-such-random-device");
  mcport::randomResetCountersForTest();
  memset(buf, 0xC3, sizeof(buf));
  check("with both sources gone the call fails", !mcport::randomBytes(buf, 32));
  check("and it wrote nothing a caller could mistake for entropy", buf[0] == 0xC3);
  check("neither source supplied a byte",
        mcport::randomBytesFromGetrandomForTest() == 0
        && mcport::randomBytesFromUrandomForTest() == 0);

  /* A fallback that opens and then yields nothing is a different failure
   * from one that will not open: read() returns 0, and the loop has to give
   * up rather than ask again for ever. */
  mcport::randomSetUrandomPathForTest("/dev/null");
  memset(buf, 0xC3, sizeof(buf));
  check("a fallback that opens but gives nothing fails rather than spinning",
        !mcport::randomBytes(buf, 32));
  check("and it too wrote nothing", buf[0] == 0xC3);

  /* ---- the getrandom loop itself ---- */
  mcport::randomSetUrandomPathForTest(NULL);

  /* Interrupted before any byte was written. The loop must retry; counting
   * the -1 as progress, or giving up on it, would both be wrong. */
  mcport::randomSetGetrandomFaultForTest(mcport::MC_GETRANDOM_EINTR_ONCE);
  mcport::randomResetCountersForTest();
  memset(buf, 0, sizeof(buf));
  check("an interrupted getrandom is retried, not abandoned",
        mcport::randomBytes(buf, 32));
  check("and the retry, not the fallback, supplied the bytes",
        mcport::randomBytesFromGetrandomForTest() == 32
        && mcport::randomBytesFromUrandomForTest() == 0);

  /* A short result. The kernel is entitled to return fewer bytes than asked
   * for; one byte at a time is the extreme of that. */
  mcport::randomSetGetrandomFaultForTest(mcport::MC_GETRANDOM_SHORT);
  mcport::randomResetCountersForTest();
  memset(buf, 0, sizeof(buf));
  check("a one-byte-at-a-time getrandom still fills the request",
        mcport::randomBytes(buf, 32));
  check("all 32 bytes accumulated from getrandom",
        mcport::randomBytesFromGetrandomForTest() == 32);

  /* Zero for a non-zero request. No working kernel does this; the point is
   * what happened if one ever did. The loop only tested for a negative
   * return, so `got` never advanced and nothing in the condition changed -
   * it spun for ever, which a test cannot report. port/mc_rng.cpp now breaks
   * on a zero return, so the call falls through to the fallback. */
  mcport::randomSetGetrandomFaultForTest(mcport::MC_GETRANDOM_ZERO);
  mcport::randomResetCountersForTest();
  memset(buf, 0, sizeof(buf));
  check("a getrandom returning zero does not loop for ever",
        mcport::randomBytes(buf, 32));
  check("it falls through to /dev/urandom instead",
        mcport::randomBytesFromGetrandomForTest() == 0
        && mcport::randomBytesFromUrandomForTest() == 32);

  mcport::randomSetUrandomPathForTest("/nonexistent/meshcore-no-such-random-device");
  check("and with no fallback either it fails rather than spinning",
        !mcport::randomBytes(buf, 32));

  /* Back to the shipped behaviour, for everything that runs after this. */
  mcport::randomSetGetrandomFaultForTest(mcport::MC_GETRANDOM_NORMAL);
  mcport::randomSetUrandomPathForTest(NULL);
  mcport::randomResetCountersForTest();
  check("the port is back on its normal sources", mcport::randomBytes(buf, 32));
  check("served by getrandom again", mcport::randomBytesFromGetrandomForTest() == 32);
}

/* ---- the wall clock ----------------------------------------------------- */

static void test_rtc(void) {
  mcport::SystemRTCClock rtc;
  uint32_t t = rtc.getCurrentTime();

  /* This build is being run in 2026, so the host clock is set. If it ever
   * runs on a board before time sync, isSet() is how a caller finds out
   * rather than this test guessing. */
  check("the wall clock reports a set time on a host", rtc.isSet());
  check("the wall clock is a plausible epoch second", t > 1700000000u);

  /* getCurrentTimeUnique() is MeshCore's own: it must never return the same
   * value twice, because an advert timestamp is part of what makes an advert
   * distinguishable from a replay of itself. */
  uint32_t a = rtc.getCurrentTimeUnique();
  uint32_t b = rtc.getCurrentTimeUnique();
  uint32_t c = rtc.getCurrentTimeUnique();
  check("getCurrentTimeUnique never repeats", a < b && b < c);

  /* setCurrentTime is accepted and ignored: the system clock is sysd's. */
  rtc.setCurrentTime(1000000000u);
  check("setCurrentTime does not move the host clock", rtc.getCurrentTime() > 1700000000u);
}

/* ---- the logging sink --------------------------------------------------- */

static char g_captured[512];
static int g_capture_count;
static mcport::LogLevel g_capture_level;

static void capturingSink(mcport::LogLevel level, const char* line) {
  g_capture_level = level;
  snprintf(g_captured, sizeof(g_captured), "%s", line);
  g_capture_count++;
}

static void test_logging(void) {
  g_capture_count = 0;
  mcport::setLogSink(capturingSink);

  mcport::logWrite(mcport::LOG_WARN, "path %u hops to %s", 3u, "K230-B");
  check("the sink receives the formatted line", strcmp(g_captured, "path 3 hops to K230-B") == 0);
  check("the sink receives the level", g_capture_level == mcport::LOG_WARN);
  check("the sink was called once", g_capture_count == 1);

  /* A line longer than the buffer is truncated, not overflowed. */
  char big[2000];
  memset(big, 'x', sizeof(big) - 1);
  big[sizeof(big) - 1] = 0;
  mcport::logWrite(mcport::LOG_ERROR, "%s", big);
  check("an oversized line is truncated rather than overflowing",
        strlen(g_captured) < sizeof(g_captured) && g_capture_count == 2);

  /* NULL restores the default, which writes to stderr. Restored here so the
   * rest of the run does not keep writing into a dead capture buffer. */
  mcport::setLogSink(NULL);
  mcport::logWrite(mcport::LOG_DEBUG, "back to the default sink");
  check("clearing the sink stops the capture", g_capture_count == 2);
}

/* ---- 5. the core initialises and runs, with no hardware -----------------
 *
 * A mesh::Mesh over the fake radio and the real port. If this links and runs,
 * the protocol core needs no LVGL, no DRM, no RadioLib, no SPI or GPIO, no
 * Arduino runtime and no device: tests/meshcore_lint.sh makes the same point
 * statically by checking what the library's objects are allowed to reference.
 */
class MinimalNode : public mesh::Mesh {
public:
  int adverts_seen;

  MinimalNode(mesh::Radio& radio, mesh::MillisecondClock& ms, mesh::RNG& rng,
              mesh::RTCClock& rtc, mesh::PacketManager& mgr, mesh::MeshTables& tables)
      : mesh::Mesh(radio, ms, rng, rtc, mgr, tables), adverts_seen(0) { }

protected:
  void onAdvertRecv(mesh::Packet*, const mesh::Identity&, uint32_t, const uint8_t*, size_t) override {
    adverts_seen++;
  }
};

static void test_one_loop_with_a_fake_radio(void) {
  mctest::FakeAir air;
  mctest::FakeRadio radio(air);
  mctest::TestClock clock;
  mctest::TestRTCClock rtc;
  mcport::HostRNG rng;
  StaticPoolPacketManager mgr(16);
  SimpleMeshTables tables;

  MinimalNode node(radio, clock, rng, rtc, mgr, tables);
  node.self_id = mesh::LocalIdentity(&rng);
  node.begin();
  check("a mesh node starts with no radio hardware", true);

  /* One loop with nothing to do must not transmit, crash or block. */
  node.loop();
  check("one idle loop sends nothing", radio.framesSent() == 0);

  /* Queue a signed advert and turn the handle until it goes out. */
  mesh::Packet* advert = node.createAdvert(node.self_id, (const uint8_t*) "K230", 4);
  check("the node builds a signed advert", advert != NULL);
  if (advert) node.sendFlood(advert);

  int loops = 0;
  while (radio.framesSent() == 0 && loops < 200) {
    node.loop();
    clock.advance(50);
    loops++;
  }
  check("the advert reaches the radio", radio.framesSent() == 1);
  check("it took a sane number of loops", loops > 0 && loops < 200);
  check("the frame was carried by the fake air", air.framesCarried() == 1);

  /* The dispatcher hands the frame to the radio in one loop and returns the
   * packet to the pool in the next, once isSendComplete() says so, so the
   * pool is legitimately one short at the instant the loop above exits. One
   * more turn settles it. */
  check("the outbound packet is still held while the send completes",
        mgr.getFreeCount() == 15);
  node.loop();
  check("the packet pool did not leak", mgr.getFreeCount() == 16);

  /* And keep looping with nothing left to do. */
  for (int i = 0; i < 50; i++) { node.loop(); clock.advance(100); }
  check("idling after a send transmits nothing more", radio.framesSent() == 1);
  check("the pool is still whole", mgr.getFreeCount() == 16);
}

/* ---- 6. the delayed inbound queue --------------------------------------
 *
 * Dispatcher::checkRecv() does not always process a flood packet where it
 * finds it. It asks the radio to score the reception and computes
 * `(pow(10, 0.85 - score) - 1) * air_time` (Dispatcher.cpp:55-57); at 50 ms
 * or more the packet goes to _mgr->queueInbound() instead, and comes back
 * out of getNextInbound() at the top of a later loop (Dispatcher.cpp:139).
 * Holding a marginal reception back is how MeshCore lets a node that heard
 * the same flood more cleanly retransmit it first.
 *
 * Nothing had ever taken that branch. The fake radio scored every packet
 * 1.0, which makes the delay negative, so every test went down the immediate
 * path and queueInbound(), getNextInbound() and the delayed processing were
 * dead code as far as this suite was concerned. One radio here scores 0.5 -
 * per radio and per test, so no other test's timing or reading changes.
 *
 * At 0.5 the factor is 10^0.35 - 1, about 1.24, against an airtime this
 * radio charges at 1 ms a byte: a ~100-byte advert gives roughly 130 ms,
 * well over the 50 ms threshold and far under the dispatcher's 32 s cap. The
 * clock is wound by hand, so which side of the deadline a loop falls on is
 * decided here rather than by how fast the machine is.
 */
struct ScoredLink {
  mctest::FakeAir air;
  mctest::FakeRadio radio_tx;
  mctest::FakeRadio radio_rx;
  mctest::TestClock clock;
  mctest::TestRTCClock rtc_tx;
  mctest::TestRTCClock rtc_rx;
  mcport::HostRNG rng;
  StaticPoolPacketManager mgr_tx;
  StaticPoolPacketManager mgr_rx;
  SimpleMeshTables tables_tx;
  SimpleMeshTables tables_rx;
  MinimalNode tx;
  MinimalNode rx;

  explicit ScoredLink(float rx_score)
      : radio_tx(air), radio_rx(air), mgr_tx(16), mgr_rx(16),
        tx(radio_tx, clock, rng, rtc_tx, mgr_tx, tables_tx),
        rx(radio_rx, clock, rng, rtc_rx, mgr_rx, tables_rx) {
    radio_rx.setPacketScore(rx_score);
    tx.self_id = mesh::LocalIdentity(&rng);
    rx.self_id = mesh::LocalIdentity(&rng);
    tx.begin();
    rx.begin();
  }

  /* One signed advert onto the air, turning only the sender - so the frame
   * ends up sitting in the receiver's radio queue with nothing having looked
   * at it yet. */
  bool sendOneAdvert() {
    mesh::Packet* advert = tx.createAdvert(tx.self_id, (const uint8_t*) "K230", 4);
    if (advert == NULL) return false;
    tx.sendFlood(advert);
    for (int i = 0; i < 200 && radio_tx.framesSent() == 0; i++) {
      tx.loop();
      clock.advance(10);
    }
    return radio_tx.framesSent() == 1;
  }
};

static void test_delayed_inbound_queue(void) {
  /* ---- the immediate branch, for contrast ---- */
  {
    ScoredLink link(1.0f);
    check("a clean reception: the advert reaches the air", link.sendOneAdvert());
    check("and nothing has looked at it yet", link.rx.adverts_seen == 0);

    link.rx.loop();
    check("a packet scoring 1.0 is processed in the loop it arrives in",
          link.rx.adverts_seen == 1);
    check("and nothing is left holding a packet from the pool",
          link.mgr_rx.getFreeCount() == 16);
  }

  /* ---- the delayed branch ---- */
  ScoredLink link(0.5f);
  check("a marginal reception: the advert reaches the air", link.sendOneAdvert());

  const unsigned long t0 = link.clock.getMillis();
  link.rx.loop();
  check("a packet scoring 0.5 is NOT processed in the loop it arrives in",
        link.rx.adverts_seen == 0);
  check("it is held in the inbound queue, so the pool is one short",
        link.mgr_rx.getFreeCount() == 15);

  /* Turning the handle is not what releases it; the clock is. */
  for (int i = 0; i < 20; i++) link.rx.loop();
  check("more loops at the same instant do not release it",
        link.rx.adverts_seen == 0 && link.mgr_rx.getFreeCount() == 15);
  check("and the clock really did not move", link.clock.getMillis() == t0);

  link.clock.advance(2000);
  link.rx.loop();
  check("once the deadline has passed getNextInbound hands it back",
        link.rx.adverts_seen == 1);
  check("and the packet returns to the pool", link.mgr_rx.getFreeCount() == 16);

  for (int i = 0; i < 20; i++) { link.rx.loop(); link.clock.advance(100); }
  check("a delayed advert is delivered exactly once", link.rx.adverts_seen == 1);
  check("and the receiver put nothing back on the air", link.radio_rx.framesSent() == 0);
}

int main(void) {
  test_monotonic_clock();
  test_dispatcher_timing_across_2_32();
  test_outbound_queue_across_2_32();
  test_a_widened_32_bit_clock_would_fail();
  test_host_rng();
  test_host_rng_fallback();
  test_rtc();
  test_logging();
  test_one_loop_with_a_fake_radio();
  test_delayed_inbound_queue();

  printf("meshcore_port_test: %d check(s), %d failure(s)\n", checks, failed);
  return failed ? 1 : 0;
}
