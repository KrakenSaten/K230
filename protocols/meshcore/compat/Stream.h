/* Host stand-in for the Arduino Print/Stream API.
 *
 * MeshCore's Identity.h and Utils.h include <Stream.h>: Identity::readFrom()/
 * writeTo() take a Stream and are how a MeshCore `.id` file is read and
 * written, and Utils::printHex() takes one. On a device that header comes
 * from the Arduino core. Here it is supplied over plain memory and over the
 * host's stdio.
 *
 * This shim is I/O only. Nothing cryptographic lives here: the keys, the
 * hashes, the cipher and the signatures all come from the vendored MeshCore
 * and Crypto sources - see README.md.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#pragma once

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define DEC 10
#define HEX 16
#define OCT 8
#define BIN 2

class Print {
public:
  virtual ~Print() = default;
  virtual size_t write(const uint8_t* buffer, size_t size) = 0;

  size_t print(char c) { return write(reinterpret_cast<const uint8_t*>(&c), 1); }
  size_t print(const char* s) {
    if (s == NULL) return 0;
    return write(reinterpret_cast<const uint8_t*>(s), strlen(s));
  }
  size_t print(int v, int base = DEC);
  size_t print(unsigned int v, int base = DEC) { return print((int) v, base); }
  size_t println() { return print('\n'); }
  size_t println(const char* s) { return print(s) + println(); }

  /* Arduino's Print::printf(), which MeshCore's packet logging uses. */
  size_t printf(const char* fmt, ...) __attribute__((format(printf, 2, 3)));
};

class Stream : public Print {
public:
  virtual size_t readBytes(uint8_t* buffer, size_t length) = 0;
};

/* A Stream over a caller-owned byte buffer. Reads and writes stop at the end
 * of it and report the short count, which is what MeshCore's readFrom()/
 * writeTo() check: a truncated identity file fails to load rather than
 * loading half a key. */
class MemStream : public Stream {
  uint8_t* _buf;
  size_t _cap, _len, _pos;
public:
  MemStream(uint8_t* buf, size_t cap, size_t len = 0)
      : _buf(buf), _cap(cap), _len(len), _pos(0) { }

  size_t length() const { return _len; }

  size_t write(const uint8_t* buffer, size_t size) override {
    size_t n = size;
    if (_pos + n > _cap) n = _cap - _pos;
    memcpy(&_buf[_pos], buffer, n);
    _pos += n;
    if (_pos > _len) _len = _pos;
    return n;
  }

  size_t readBytes(uint8_t* buffer, size_t length) override {
    size_t n = length;
    if (_pos + n > _len) n = _len - _pos;
    memcpy(buffer, &_buf[_pos], n);
    _pos += n;
    return n;
  }
};

/* The object MeshCore's `#if MESH_PACKET_LOGGING` blocks write to. On a
 * device that is the USB serial port; here it is the process's standard
 * error, so packet logging goes where every other Doors diagnostic goes and
 * never into a program's own output. Defined in port/mc_serial.cpp. */
class HostSerial : public Stream {
public:
  size_t write(const uint8_t* buffer, size_t size) override;
  size_t readBytes(uint8_t* buffer, size_t length) override;
};

extern HostSerial Serial;
