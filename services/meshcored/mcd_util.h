/*
 * meshcored's own hex and validation. Small on purpose, and first-party on
 * purpose.
 *
 * MeshCore has a hex decoder of its own, mesh::Utils::fromHex(), and this
 * daemon deliberately does not use it for anything that arrives from outside.
 * That function validates the string's LENGTH and nothing else: a character
 * that is not hex becomes 0 and the call still returns true
 * (vendor/RIFT/src/Utils.cpp:218-229, recorded as known debt in
 * protocols/meshcore/README.md). Decoding radiod's payload_hex with it would
 * turn a corrupt event into a frame of plausible-looking bytes and hand it to
 * the protocol core as if it had been received. The decoder here refuses
 * instead, which keeps that upstream debt out of reach rather than fixing it.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef MCD_UTIL_H
#define MCD_UTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Decode hex into dst. Every character must be a hex digit and the string
 * must have even length; anything else is refused rather than substituted.
 * Returns the number of bytes written, or -1 when the string is not valid
 * hex or does not fit in dst_size. An empty string decodes to 0 bytes, which
 * callers that need at least one byte must reject themselves. */
int mcd_hex_decode(const char *hex, uint8_t *dst, size_t dst_size);

/* Lowercase hex of len bytes into dst, which must hold 2*len+1 characters.
 * Returns dst, or NULL when it does not fit. */
char *mcd_hex_encode(const uint8_t *src, size_t len, char *dst, size_t dst_size);

/* A public-key prefix as a client may give it: 2 to 64 hex characters, so
 * one byte to a whole key. Returns the number of bytes written to dst, or
 * -1. Odd-length, empty and over-long strings are refused: a caller that
 * cannot say how many bytes it means is not asking a precise question. */
int mcd_key_prefix_parse(const char *hex, uint8_t *dst, size_t dst_size);

/* Is every byte of text printable UTF-8 that a MeshCore message may carry?
 * The rule is narrow on purpose: no NUL (the wire format is NUL-terminated),
 * no control characters other than a plain newline or tab, and at most
 * max_len bytes. Returns true when the text may be sent. */
bool mcd_text_acceptable(const char *text, size_t max_len);

#endif /* MCD_UTIL_H */
