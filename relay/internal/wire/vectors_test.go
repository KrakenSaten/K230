// Copyright (c) 2026 PocketOS authors.
// SPDX-License-Identifier: Apache-2.0

package wire

// Outputs of SHA-256 and HKDF-SHA256 for the inputs of TestVectors, as
// computed by Go's crypto/sha256 and crypto/hkdf, and checked once against
// Python's hashlib and hmac (RFC 5869 by hand). docs/api/relay.md repeats
// them.
const (
	vectorLookup = "49c1cfcfa13f22964f87c99ea33aceaa7805ccd95cd928e8995a0edd5fabe5d4"
	vectorSlot   = "33ec232b74cf3cc0693b1d9205f5829d"
)
