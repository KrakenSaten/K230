#!/bin/bash
# Copyright (c) 2026 Doors authors. SPDX-License-Identifier: Apache-2.0
# peerd's structural rules (docs/decisions/ADR-017):
#   1. libsodium is called from one file, peer_crypto.c, and nowhere else in
#      Doors: the shell and every other service stay free of it (ADR-007);
#   2. no cryptographic primitive is written here;
#   3. the parser, the replay window and the engine touch no socket, file or
#      clock, so they run under tests and a fuzzer; one file each owns the
#      network (peer_net.c) and the disk (peer_store.c);
#   4. the secret key is never logged and never put in a reply;
#   5. the one test hook is compiled into the test build only;
#   6. the randomness is the library's, never rand().
# Fails the build when any of them is broken.
set -u
cd "$(dirname "$0")/.." || exit 1
P=services/peerd
failed=0
check() { # <label> <files> <regex>: no line may match
    hits=$(grep -nE "$3" $2 2>/dev/null)
    if [ -n "$hits" ]; then
        echo "FAIL $1:"; echo "$hits" | head -10; failed=$((failed + 1))
    else
        echo "ok   $1"
    fi
}
want() { # <label> <files> <regex>: some line must match
    if grep -qE "$3" $2 2>/dev/null; then echo "ok   $1"; else
        echo "FAIL $1: not found"; failed=$((failed + 1)); fi
}
OTHERS=$(ls $P/*.c $P/*.h | grep -v "peer_crypto\.c$")
check "libsodium is included by peer_crypto.c alone" "$OTHERS" 'include[[:space:]]*[<"]sodium'
check "and called by it alone" "$OTHERS" '\b(crypto_[a-z0-9_]+|sodium_[a-z_]+|randombytes[a-z_]*)[[:space:]]*\('
ELSEWHERE=$(git ls-files 'apps/*.c' 'apps/*.h' 'ui/*.c' 'ui/*.h' 'core/*.c' 'core/*.h' 'services/*.c' 'services/*.h' \
            'services/*.cpp' 'tools/*.c' 'tools/*.h' 2>/dev/null | grep -v "^$P/")
if [ -n "$ELSEWHERE" ]; then
    check "nothing else in Doors includes libsodium" "$ELSEWHERE" 'include[[:space:]]*[<"]sodium'
fi
check "no primitive is written here: no curve, cipher or hash arithmetic of our own" "$P/*.c" \
      '\b(fe25519|ge25519|chacha|salsa|poly1305|sha512|sha256|blake2)[a-z0-9_]*[[:space:]]*\('
PURE="$P/peer_wire.c $P/peer_wire.h $P/peer_replay.c $P/peer_replay.h $P/peer_core.c $P/peer_core.h"
check "the parser, the window and the engine open no socket" "$PURE" \
      '\b(socket|bind|sendto|sendmsg|recvfrom|recvmsg|connect|getifaddrs)[[:space:]]*\('
check "nor a file" "$PURE" '\b(fopen|open|creat|unlink|rename|mkdir|fsync|read|write)[[:space:]]*\('
check "nor read a clock" "$PURE" '\b(clock_gettime|gettimeofday|time)[[:space:]]*\('
check "sockets belong to peer_net.c" "$(ls $P/*.c | grep -v 'peer_net\.c$')" \
      '\b(socket|bind|sendto|sendmsg|recvfrom|recvmsg|getifaddrs)[[:space:]]*\('
check "files belong to peer_store.c" "$(ls $P/*.c | grep -v 'peer_store\.c$')" \
      '\b(fopen|creat|unlink|rename|fsync|ftruncate)[[:space:]]*\('
check "the secret key is not logged" "$P/*.c" 'LOG_[A-Z]+\([^;]*(->sk|x_sk|\bsk\b|secret)'
check "nor put in a reply" "$P/main.c" 'pc_identity_secret|->sk|x_sk'
check "randomness is the library's, never rand()" "$P/*.c" '\b(rand|srand|random|srandom|drand48)[[:space:]]*\('
check "the test hook is read by peer_net.c alone" "$(ls $P/*.c $P/*.h | grep -v 'peer_net\.c$')" 'PEERD_TEST|getenv'
want "and only in a hooks build" "$P/peer_net.c" '^#if PEERD_TEST_HOOKS'
want "every return of the library that can fail is checked: sodium_init" "$P/peer_crypto.c" 'sodium_init\(\) < 0'
want "the key conversion" "$P/peer_crypto.c" 'crypto_sign_ed25519_pk_to_curve25519\([^;]*\) != 0'
want "the key exchange" "$P/peer_crypto.c" 'rc = crypto_kx_(client|server)_session_keys'
want "the tag" "$P/peer_crypto.c" 'crypto_aead_xchacha20poly1305_ietf_decrypt\('
want "the signature" "$P/peer_crypto.c" 'crypto_sign_verify_detached\([^;]*\) == 0'
want "an identity is never regenerated over a state that has one" "$P/peer_store.c" 'return fail\(out, PS_FAULT_IDENTITY\)'
want "nor an epoch begun again" "$P/peer_store.c" 'return fail\(out, PS_FAULT_EPOCH\)'
echo "peerd_lint: $failed failure(s)"
exit $((failed > 0))
