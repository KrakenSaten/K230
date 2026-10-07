/* meshcore-frame: hex, host randomness and the identity file.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "mcf.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <Stream.h>

#if defined(__linux__)
#include <sys/random.h>
#endif

namespace mcf {

/* ---- hex --------------------------------------------------------------- */

static int hexDigit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool hexDecode(const char* hex, uint8_t* out, size_t max, size_t* len) {
  if (hex == NULL) return false;
  size_t n = strlen(hex);
  if (n == 0 || n % 2 != 0 || n / 2 > max) return false;
  for (size_t i = 0; i < n; i += 2) {
    int hi = hexDigit(hex[i]);
    int lo = hexDigit(hex[i + 1]);
    if (hi < 0 || lo < 0) return false;
    out[i / 2] = (uint8_t)((hi << 4) | lo);
  }
  *len = n / 2;
  return true;
}

void hexEncode(char* dest, const uint8_t* src, size_t len) {
  static const char digits[] = "0123456789abcdef";
  for (size_t i = 0; i < len; i++) {
    dest[i * 2] = digits[src[i] >> 4];
    dest[i * 2 + 1] = digits[src[i] & 0x0F];
  }
  dest[len * 2] = 0;
}

/* ---- host randomness ---------------------------------------------------
 * A short read or an error here would mean a key with less entropy than it
 * claims, which is worse than no key at all, so there is no degraded path:
 * the tool stops. */
void HostRNG::random(uint8_t* dest, size_t sz) {
  size_t got = 0;

#if defined(__linux__)
  while (got < sz) {
    ssize_t n = getrandom(dest + got, sz - got, 0);
    if (n < 0) {
      if (errno == EINTR) continue;
      break;  /* older kernel or a sandbox without the syscall: fall through */
    }
    got += (size_t)n;
  }
#endif

  if (got < sz) {
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
      while (got < sz) {
        ssize_t n = read(fd, dest + got, sz - got);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        got += (size_t)n;
      }
      close(fd);
    }
  }

  if (got < sz) {
    fprintf(stderr, "meshcore-frame: no usable source of randomness on this host\n");
    exit(3);
  }
}

/* ---- identity ----------------------------------------------------------
 * Read and written by mesh::LocalIdentity over a memory Stream, so the file
 * is byte-for-byte a MeshCore `.id` and nothing here interprets key material.
 * Mode 0600: the file holds a private key. */

bool identityLoad(mesh::LocalIdentity& id, const char* path, char* err) {
  FILE* f = fopen(path, "rb");
  if (f == NULL) {
    snprintf(err, MCF_ERR_SIZE, "cannot open identity file %s: %s", path, strerror(errno));
    return false;
  }
  uint8_t buf[MCF_IDENTITY_FILE_SIZE];
  size_t n = fread(buf, 1, sizeof(buf), f);
  bool too_long = (fgetc(f) != EOF);
  fclose(f);

  if (n != sizeof(buf) || too_long) {
    snprintf(err, MCF_ERR_SIZE, "identity file %s is %s; a MeshCore .id is %d bytes",
             path, n != sizeof(buf) ? "too short" : "too long", MCF_IDENTITY_FILE_SIZE);
    return false;
  }

  MemStream s(buf, sizeof(buf), sizeof(buf));
  if (!id.readFrom(s)) {
    snprintf(err, MCF_ERR_SIZE, "identity file %s could not be read as a MeshCore identity", path);
    return false;
  }
  /* The check MeshCore makes before it will use a key for ECDH
   * (LocalIdentity::validatePrivateKey): the two halves of the pair must
   * agree and the shared secret must not be degenerate. Without it a file of
   * 96 arbitrary bytes loads happily and only fails later, as an advert
   * nobody can verify - a long way from the file that caused it. */
  const uint8_t* prv = &buf[PUB_KEY_SIZE];
  if (!mesh::LocalIdentity::validatePrivateKey(prv)) {
    snprintf(err, MCF_ERR_SIZE, "identity file %s does not hold a usable Ed25519 key pair", path);
    return false;
  }
  /* And the public key the file carries must be the one that private key
   * derives, not merely a valid key of its own: MeshCore re-derives it when a
   * file holds the private key alone (LocalIdentity::readFrom), so the two
   * must agree or signatures would be attributed to the wrong node. */
  mesh::LocalIdentity derived;
  derived.readFrom(prv, PRV_KEY_SIZE);
  if (memcmp(derived.pub_key, id.pub_key, PUB_KEY_SIZE) != 0) {
    snprintf(err, MCF_ERR_SIZE, "identity file %s: the public key is not the one its private key derives",
             path);
    return false;
  }
  return true;
}

bool identitySave(const mesh::LocalIdentity& id, const char* path, char* err) {
  uint8_t buf[MCF_IDENTITY_FILE_SIZE];
  MemStream s(buf, sizeof(buf));
  if (!id.writeTo(s) || s.length() != sizeof(buf)) {
    snprintf(err, MCF_ERR_SIZE, "could not serialise the identity");
    return false;
  }

  int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
  if (fd < 0) {
    snprintf(err, MCF_ERR_SIZE, "cannot create identity file %s: %s", path, strerror(errno));
    return false;
  }
  size_t done = 0;
  while (done < sizeof(buf)) {
    ssize_t n = write(fd, buf + done, sizeof(buf) - done);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) {
      snprintf(err, MCF_ERR_SIZE, "cannot write identity file %s: %s", path, strerror(errno));
      close(fd);
      return false;
    }
    done += (size_t)n;
  }
  if (close(fd) != 0) {
    snprintf(err, MCF_ERR_SIZE, "cannot close identity file %s: %s", path, strerror(errno));
    return false;
  }
  return true;
}

bool identityCreate(mesh::LocalIdentity& id, char* err) {
  HostRNG rng;
  /* MeshCore's own constructor: a SEED_SIZE seed from the RNG through
   * ed25519_create_keypair(). */
  for (int tries = 0; tries < 8; tries++) {
    mesh::LocalIdentity candidate(&rng);
    /* MeshCore refuses 00- and FF-prefixed public keys, because the first
     * byte is the node's path hash (Identity::copyHashTo) and those two
     * values collide with reserved markers. Draw again. */
    if (candidate.pub_key[0] == 0x00 || candidate.pub_key[0] == 0xFF) continue;
    id = candidate;
    return true;
  }
  snprintf(err, MCF_ERR_SIZE, "could not generate a usable key pair");
  return false;
}

}  /* namespace mcf */
