/* Host stand-in for the Crypto library's global RNG object.
 *
 * rweather/Crypto declares `extern RNGClass RNG` and defines it in RNG.cpp,
 * which harvests entropy from an Arduino's watchdog jitter, TRNG register or
 * EEPROM seed - none of which exist here, and all of which come with
 * <Arduino.h>. Ed25519.cpp and Curve25519.cpp reference that object from
 * Ed25519::generatePrivateKey() and Curve25519::dh1(), so it has to exist
 * for the library to link even when nothing calls those entry points.
 *
 * It is supplied here over the operating system's CSPRNG - the same source
 * mcport::HostRNG uses. This is a platform integration, not a substitute
 * algorithm: nothing here weakens or replaces any part of the crypto, and if
 * those two entry points were called they would get real entropy rather than
 * a stub.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include <RNG.h>

#include "mc_port.h"

RNGClass::RNGClass() { }
RNGClass::~RNGClass() { }

void RNGClass::rand(uint8_t* data, size_t len) {
  mcport::HostRNG rng;
  rng.random(data, len);
}

RNGClass RNG;
