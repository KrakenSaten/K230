/* Host stand-in for <Arduino.h>.
 *
 * One vendored MeshCore source, helpers/AdvertDataHelpers.cpp, includes
 * <Arduino.h> for the sprintf() its time formatter uses. The advert encoder
 * and parser in the same file - the part this tool needs - touch nothing from
 * the Arduino core. Rather than edit a vendored file, the include is answered
 * here with the C library it actually wants.
 *
 * No pin, bus, timer or radio API is declared here on purpose: this tool
 * never touches hardware.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <Stream.h>
