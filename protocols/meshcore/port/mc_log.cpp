/* The logging seam, and the Serial object MeshCore's packet logging writes
 * to. See port/mc_port.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "mc_port.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

namespace mcport {

static LogSink g_sink;

static void defaultSink(LogLevel level, const char* line) {
  static const char* const names[] = { "DEBUG", "INFO ", "WARN ", "ERROR" };
  fprintf(stderr, "meshcore %s %s\n", names[level], line);
}

void setLogSink(LogSink sink) { g_sink = sink; }

void logWrite(LogLevel level, const char* fmt, ...) {
  char line[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);

  (g_sink ? g_sink : defaultSink)(level, line);
}

}  // namespace mcport

/* ---- the Serial object -------------------------------------------------
 * MeshCore's `#if MESH_PACKET_LOGGING` blocks in Dispatcher.cpp write whole
 * packet summaries to Serial a fragment at a time - print(), then printf(),
 * then a hex dump - so routing each fragment through mcport::logWrite()
 * would produce one log record per fragment. They go to stderr instead,
 * which is where the default sink writes anyway and where the init scripts
 * already capture a service's output. Packet logging is off unless a build
 * defines MESH_PACKET_LOGGING; this object exists either way so the library
 * links the same in both. */
size_t HostSerial::write(const uint8_t* buffer, size_t size) {
  if (buffer == NULL || size == 0) return 0;
  return fwrite(buffer, 1, size, stderr);
}

size_t HostSerial::readBytes(uint8_t* buffer, size_t length) {
  /* Nothing in the protocol core reads from Serial. A device's Serial is the
   * companion link, which is not part of this layer. */
  (void) buffer;
  (void) length;
  return 0;
}

HostSerial Serial;

size_t Print::print(int v, int base) {
  char buf[40];
  int n;
  switch (base) {
    case HEX: n = snprintf(buf, sizeof(buf), "%X", (unsigned) v); break;
    case OCT: n = snprintf(buf, sizeof(buf), "%o", (unsigned) v); break;
    case BIN: {
      unsigned u = (unsigned) v;
      n = 0;
      if (u == 0) { buf[n++] = '0'; }
      else {
        char rev[32];
        int r = 0;
        while (u) { rev[r++] = (char) ('0' + (u & 1)); u >>= 1; }
        while (r) buf[n++] = rev[--r];
      }
      buf[n] = 0;
      break;
    }
    default: n = snprintf(buf, sizeof(buf), "%d", v); break;
  }
  if (n < 0) return 0;
  return write(reinterpret_cast<const uint8_t*>(buf), strlen(buf));
}

size_t Print::printf(const char* fmt, ...) {
  char line[512];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  if (n < 0) return 0;
  size_t len = strlen(line);
  return write(reinterpret_cast<const uint8_t*>(line), len);
}
