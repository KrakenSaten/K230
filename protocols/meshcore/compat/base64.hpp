/* Host stand-in for <base64.hpp>.
 *
 * `BaseChatMesh.cpp:878` includes this header, inside the MAX_GROUP_CHANNELS
 * guard, for one call in `addChannel()`. Upstream satisfies it with an
 * Arduino library it does not vendor - `densaugeo/base64 @ ~1.4.0`, named in
 * `vendor/RIFT/platformio.ini:180` - so there is no upstream source in this
 * repository for this header to be faithful to. Turning channels on therefore
 * means supplying a decoder, and this is it: one function, freestanding, no
 * Arduino types, no allocation.
 *
 * ---- two deliberate differences from densaugeo -------------------------
 *
 * 1. IT DOES NOT WRITE A TERMINATOR. densaugeo's `decode_base64()` ends with
 *    `output[output_length] = '\0'`, one byte past the bytes it decoded. Its
 *    only caller here decodes into `GroupChannel::secret`, which is 32 bytes
 *    and is followed in `ChannelDetails` by `name[32]`
 *    (`helpers/ChannelDetails.h`), so a full 32-byte key writes a zero into
 *    `name[0]`. `addChannel()` happens to overwrite `name` immediately
 *    afterwards, so upstream never sees it - but it is a write past the end
 *    of a buffer, and reproducing it here to be "faithful" would be
 *    reproducing a defect into a service that runs on the air. A caller that
 *    wants a C string terminates it itself.
 *
 * 2. IT HAS A CAPACITY, AND THE THREE-ARGUMENT FORM SUPPLIES A SAFE ONE.
 *    densaugeo's three-argument signature - the one the vendored call uses -
 *    carries no output capacity at all, so a 100-character input overruns
 *    whatever it was given, and `addChannel()` only checks the decoded length
 *    *after* the write. That cannot be fixed in the signature without editing
 *    a vendored file, so it is fixed in the contract: the three-argument form
 *    decodes at most MC_BASE64_MAX_DECODE bytes and returns 0 for anything
 *    longer. Thirty-two is not arbitrary - it is the size of the only
 *    destination any caller inside this library's boundary passes, a
 *    `GroupChannel::secret`. A longer input then makes `addChannel()` see a
 *    length that is neither 16 nor 32 and return NULL, which is what it does
 *    for a bad key anyway.
 *
 * Nothing in this library or in services/meshcored calls `addChannel()`:
 *   upstream's own firmware documents why not (`examples/companion_radio/
 *   MyMesh.h:343-347` - it writes at `num_channels`, which counts only
 *   channels added through that method, so it overwrites a restored one).
 *   The service installs channels through `setChannel()` and calls
 *   `mcport::base64Decode()` below directly. The three-argument form exists
 *   so the vendored file compiles, and is correct rather than merely present.
 *
 * ---- what it accepts ---------------------------------------------------
 *
 * Strict standard base64, RFC 4648 alphabet, `=` padding required to a
 * multiple of four. Any character outside the alphabet - including a space,
 * a newline, or URL-safe `-`/`_` - is a refusal, not a zero byte. A decoder
 * that quietly maps unknown characters to 0 turns a mistyped key into a
 * different key that still "works", and two nodes then sit on channels they
 * both believe are the same one.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

/* The largest decode the three-argument compatibility form will perform: the
 * size of a mesh::GroupChannel::secret, which is the only buffer any caller
 * within this boundary hands it. See the header comment. */
#define MC_BASE64_MAX_DECODE 32

namespace mcport {

/* Decode `in_len` characters of base64 from `in` into at most `cap` bytes at
 * `out`. Returns the number of bytes written, or 0 for input this will not
 * decode: a length that is not a positive multiple of four, a character
 * outside the alphabet, padding anywhere but at the end, or a result that
 * does not fit `cap`. Nothing is written when 0 is returned.
 *
 * No terminator is written, and `out` is not touched beyond the bytes
 * decoded. */
inline size_t base64Decode(const unsigned char* in, size_t in_len, unsigned char* out, size_t cap)
{
    static const signed char kTable[128] = {
        /* 0x00 */ -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        /* 0x10 */ -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        /* 0x20 */ -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 62, -1, -1, -1, 63,
        /* 0x30 */ 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, -1, -1, -1, -1, -1, -1,
        /* 0x40 */ -1,  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14,
        /* 0x50 */ 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, -1, -1, -1, -1, -1,
        /* 0x60 */ -1, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40,
        /* 0x70 */ 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, -1, -1, -1, -1, -1,
    };
    size_t pad = 0;
    size_t want;
    size_t written = 0;
    size_t i;

    if (in == NULL || out == NULL || in_len == 0 || (in_len % 4) != 0) {
        return 0;
    }
    /* Padding is counted before anything is decoded, because how many bytes
     * the last quantum yields decides whether the result fits at all. */
    if (in[in_len - 1] == '=') {
        pad = (in[in_len - 2] == '=') ? 2 : 1;
    }
    want = (in_len / 4) * 3 - pad;
    if (want > cap) {
        return 0;
    }

    for (i = 0; i < in_len; i += 4) {
        signed char q[4];
        int j;

        for (j = 0; j < 4; j++) {
            unsigned char c = in[i + j];

            if (c == '=') {
                /* Only the last quantum may be padded, and only in its last
                 * two positions. "AB=C" and "AB==CD==" are refusals. */
                if (i + 4 != in_len || j < 2) {
                    return 0;
                }
                if (j == 2 && in[i + 3] != '=') {
                    return 0;
                }
                q[j] = 0;
                continue;
            }
            if (c >= 128 || kTable[c] < 0) {
                return 0;
            }
            q[j] = kTable[c];
        }
        /* Three bytes from four sextets, then as many as this quantum
         * actually carries - the padding has already been proven to be at
         * the end, so `want` is the whole bound needed here. */
        if (written < want) {
            out[written++] = (unsigned char)((q[0] << 2) | (q[1] >> 4));
        }
        if (written < want) {
            out[written++] = (unsigned char)(((q[1] & 0x0F) << 4) | (q[2] >> 2));
        }
        if (written < want) {
            out[written++] = (unsigned char)(((q[2] & 0x03) << 6) | q[3]);
        }
    }
    return written;
}

}  // namespace mcport

/* The signature `BaseChatMesh::addChannel()` calls. `unsigned int` rather
 * than `size_t`, as densaugeo declares it, so the vendored call site compiles
 * unchanged. */
inline unsigned int decode_base64(const unsigned char* input, unsigned int input_length,
                                  unsigned char* output)
{
    return (unsigned int)mcport::base64Decode(input, (size_t)input_length, output,
                                              MC_BASE64_MAX_DECODE);
}
