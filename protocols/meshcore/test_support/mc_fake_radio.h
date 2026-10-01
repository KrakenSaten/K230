/* An in-memory radio and a hand-wound clock, for exercising the protocol
 * core without hardware.
 *
 * This is test support. It is built into the test binaries and into nothing
 * else: no image carries it, and the library target does not compile it. It
 * lives beside the library rather than in tests/ because it implements
 * mesh::Radio, which is a MeshCore interface, and because the eventual real
 * adapter over radiod will sit next to it and be held to the same shape.
 *
 * What it models: a shared "air" that every attached radio hears. A frame
 * handed to startSendRaw() is delivered to every OTHER radio on the same
 * air, immediately and without loss, duplication, collision or range. What
 * it does not model: timing, propagation, SNR variation, half-duplex
 * deafness, or a channel that is ever busy. A two-node exchange over this is
 * evidence that the protocol core encodes, signs, routes and verifies
 * correctly; it is not evidence about radio behaviour, which is what the
 * accepted P0 on-air gate (docs/hardware/MESHCORE_INTEROP_GATE.md) covers.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef MC_FAKE_RADIO_H
#define MC_FAKE_RADIO_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <Dispatcher.h>
#include <MeshCore.h>

namespace mctest {

/* A clock the test winds by hand. Deterministic, and it starts well above
 * zero: mesh::Dispatcher refills its transmit budget from the time elapsed
 * since a zero-initialised stamp, so a clock starting at zero would give a
 * node no budget to transmit with until the test had advanced it anyway. */
class TestClock : public mesh::MillisecondClock {
  unsigned long _now;
public:
  explicit TestClock(unsigned long start = 100000UL) : _now(start) { }
  unsigned long getMillis() override { return _now; }
  void advance(unsigned long ms) { _now += ms; }
  void set(unsigned long ms) { _now = ms; }
};

/* A clock that counts in 64 bits but wraps at 2^32, the way an ESP32
 * millis() widened into a 64-bit `unsigned long` would. Nothing in the port
 * uses it; tests/meshcore_port_test.cpp uses it to show what the port is
 * avoiding. See port/mc_port.h. */
class WrappingClock : public mesh::MillisecondClock {
  unsigned long _now;
public:
  explicit WrappingClock(unsigned long start = 0) : _now(start & 0xFFFFFFFFUL) { }
  unsigned long getMillis() override { return _now; }
  void advance(unsigned long ms) { _now = (_now + ms) & 0xFFFFFFFFUL; }
  void set(unsigned long ms) { _now = ms & 0xFFFFFFFFUL; }
};

/* A fixed RTC, so an advert's timestamp is the same on every run. */
class TestRTCClock : public mesh::RTCClock {
  uint32_t _now;
public:
  explicit TestRTCClock(uint32_t start = 1789000000u) : _now(start) { }
  uint32_t getCurrentTime() override { return _now; }
  void setCurrentTime(uint32_t t) override { _now = t; }
};

class FakeRadio;

/* The shared medium. Every radio attached to it hears every frame any other
 * radio sends. */
class FakeAir {
public:
  static const int MAX_RADIOS = 4;

  FakeAir() : _count(0), _frames(0), _bytes(0) { memset(_radios, 0, sizeof(_radios)); }

  bool attach(FakeRadio* r) {
    if (_count >= MAX_RADIOS) return false;
    _radios[_count++] = r;
    return true;
  }

  /* A radio leaving the air takes its entry with it. Without this, a radio
   * built on the stack - a third node introduced for one test, say - leaves
   * a dangling pointer behind when its scope ends, and the next broadcast
   * writes through it. ASan found exactly that here before the destructor
   * below existed. */
  void detach(const FakeRadio* r) {
    for (int i = 0; i < _count; i++) {
      if (_radios[i] != r) continue;
      for (int j = i; j + 1 < _count; j++) _radios[j] = _radios[j + 1];
      _radios[--_count] = NULL;
      return;
    }
  }

  int attachedCount() const { return _count; }

  void broadcast(const FakeRadio* from, const uint8_t* bytes, int len);

  int framesCarried() const { return _frames; }
  long bytesCarried() const { return _bytes; }

private:
  FakeRadio* _radios[MAX_RADIOS];
  int _count;
  int _frames;
  long _bytes;
};

class FakeRadio : public mesh::Radio {
public:
  static const int MAX_QUEUED = 16;

  explicit FakeRadio(FakeAir& air)
      : _air(air), _head(0), _tail(0), _sent(0), _dropped(0),
        _score(1.0f), _last_len(0) {
    memset(_len, 0, sizeof(_len));
    memset(_buf, 0, sizeof(_buf));
    memset(_last, 0, sizeof(_last));
    _air.attach(this);
  }

  /* The air must outlive its radios, which is why World declares it first. */
  ~FakeRadio() { _air.detach(this); }

  FakeRadio(const FakeRadio&) = delete;
  FakeRadio& operator=(const FakeRadio&) = delete;

  /* ---- mesh::Radio ---- */

  int recvRaw(uint8_t* bytes, int sz) override {
    if (_head == _tail) return 0;
    int n = _len[_head];
    if (n > sz) n = sz;
    memcpy(bytes, _buf[_head], (size_t) n);
    _head = (_head + 1) % MAX_QUEUED;
    return n;
  }

  /* 1 ms per byte, an order of magnitude in the right place for SF7-ish LoRa
   * and, more to the point, a number that makes the dispatcher's airtime
   * budget behave the way it will on a real link rather than being free. */
  uint32_t getEstAirtimeFor(int len_bytes) override { return (uint32_t) (len_bytes > 0 ? len_bytes : 1); }

  /* Per radio, and 1.0 by default: above the dispatcher's 50 ms threshold
   * calculation, so a flood packet is processed on the spot instead of
   * sitting in the delayed inbound queue. Real scoring is the radio's
   * business; this radio has no noise to score.
   *
   * setPacketScore() exists because the delayed branch was therefore never
   * taken by any test. Dispatcher::calcRxDelay() is
   * `(10^(0.85 - score) - 1) * air_time`, so a score below 0.85 gives a
   * positive delay and - once it clears 50 ms - a packet that goes through
   * queueInbound() and comes back out of getNextInbound(). It is set on one
   * radio in one test rather than lowered globally, so every other test
   * keeps the immediate path and its plain reading. */
  float packetScore(float, int) override { return _score; }
  void setPacketScore(float score) { _score = score; }

  bool startSendRaw(const uint8_t* bytes, int len) override {
    if (len <= 0 || len > MAX_TRANS_UNIT) return false;
    _air.broadcast(this, bytes, len);
    _sent++;
    return true;
  }

  /* Instant: the frame is already in every other radio's queue by the time
   * the dispatcher asks. */
  bool isSendComplete() override { return true; }
  void onSendFinished() override { }
  bool isInRecvMode() const override { return true; }
  bool isReceiving() override { return false; }
  float getLastRSSI() const override { return -60.0f; }
  float getLastSNR() const override { return 10.0f; }

  /* ---- delivery, called by FakeAir ---- */
  void deliver(const uint8_t* bytes, int len) {
    if (len > 0 && len <= (int) sizeof(_last)) {
      memcpy(_last, bytes, (size_t) len);
      _last_len = len;
    }
    int next = (_tail + 1) % MAX_QUEUED;
    if (next == _head) { _dropped++; return; }  /* receiver not draining */
    if (len > (int) sizeof(_buf[0])) { _dropped++; return; }
    memcpy(_buf[_tail], bytes, (size_t) len);
    _len[_tail] = len;
    _tail = next;
  }

  int framesSent() const { return _sent; }
  int framesDropped() const { return _dropped; }
  bool hasPending() const { return _head != _tail; }

  /* A copy of the last frame this radio heard, kept aside from the receive
   * queue so it survives the dispatcher draining it. An eavesdropper test
   * needs the bytes that were really on the air, not a re-encoding of them:
   * this is what an attacker with a receiver would have. */
  const uint8_t* lastHeard() const { return _last; }
  int lastHeardLen() const { return _last_len; }
  void forgetLastHeard() { _last_len = 0; }

private:
  FakeAir& _air;
  uint8_t _buf[MAX_QUEUED][MAX_TRANS_UNIT];
  int _len[MAX_QUEUED];
  int _head, _tail;
  int _sent, _dropped;
  float _score;
  uint8_t _last[MAX_TRANS_UNIT];
  int _last_len;
};

inline void FakeAir::broadcast(const FakeRadio* from, const uint8_t* bytes, int len) {
  _frames++;
  _bytes += len;
  for (int i = 0; i < _count; i++) {
    if (_radios[i] == NULL || _radios[i] == from) continue;
    _radios[i]->deliver(bytes, len);
  }
}

}  // namespace mctest

#endif  /* MC_FAKE_RADIO_H */
