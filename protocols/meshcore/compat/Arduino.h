/* Host stand-in for <Arduino.h>.
 *
 * Four vendored MeshCore sources include <Arduino.h>: AdvertDataHelpers.cpp
 * and TxtDataHelpers.cpp want the C library behind sprintf() and abs() for
 * their formatters, BaseChatMesh.h and ContactInfo.h include it only because
 * PlatformIO's dependency finder needs to see it, and Dispatcher.cpp includes
 * it for Serial when MESH_PACKET_LOGGING is on. Rather than edit four
 * vendored files, the include is answered here with what they actually want.
 *
 * No pin, bus, timer, RTOS or radio API is declared here, on purpose. This
 * layer is the MeshCore protocol and nothing else; a vendored file that
 * reached for digitalWrite() would fail to compile rather than quietly pull
 * hardware into a protocol library. millis() is deliberately absent too: the
 * protocol's clock arrives through mesh::MillisecondClock (see
 * port/mc_clock.h), which on this platform is a genuine 64-bit monotonic
 * counter, and an Arduino-shaped 32-bit millis() next to it would be an
 * invitation to use the wrong one.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <Stream.h>

/* MAX_GROUP_CHANNELS, which decides the size of BaseChatMesh's channel table
 * and therefore that class's layout. It is reached from here, and not from a
 * -D in two makefiles, because BaseChatMesh.h and ChannelDetails.h both
 * include <Arduino.h> before they expand it - so no translation unit anywhere
 * in this repository can see the class with a different value. mc_channels.h
 * explains the rest. */
#include "mc_channels.h"

/* MAX_CONTACTS, for the same reason and by the same route: it sizes
 * BaseChatMesh's contact table (BaseChatMesh.h:64). See mc_contacts.h. */
#include "mc_contacts.h"

/* avr-libc's ltoa(), which the Arduino cores inherit and which MeshCore's
 * float formatter calls (TxtDataHelpers.cpp, _ftoa). It is not in the C
 * library on Linux, so it is supplied here rather than by editing a vendored
 * file. avr-libc's semantics: base 10 is signed and prints a leading '-' for
 * a negative value; every other base treats the value as unsigned. The
 * buffer must have room for the digits, the sign and the terminator - the
 * one caller passes a 16-byte scratch buffer for a value that cannot exceed
 * ten digits. Returns the buffer, as avr-libc does. */
static inline char* ltoa(long value, char* buffer, int radix) {
  static const char digits[] = "0123456789abcdefghijklmnopqrstuvwxyz";
  char* out = buffer;
  char tmp[8 * sizeof(long) + 1];
  int n = 0;
  unsigned long v;

  if (radix < 2 || radix > 36) { *out = 0; return buffer; }

  if (radix == 10 && value < 0) {
    *out++ = '-';
    /* Negated through unsigned so LONG_MIN does not overflow. */
    v = 0UL - (unsigned long) value;
  } else {
    v = (unsigned long) value;
  }

  do {
    tmp[n++] = digits[v % (unsigned long) radix];
    v /= (unsigned long) radix;
  } while (v != 0);

  while (n > 0) *out++ = tmp[--n];
  *out = 0;
  return buffer;
}
