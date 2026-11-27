// Copyright (c) 2026 PocketOS authors.
// SPDX-License-Identifier: Apache-2.0

//go:build !linux

package fake

import "os"

// statID cannot tell a file's identity here. The fake unit's two-file
// rule then refuses every change of paired.v1 or relay.v1 (pairing fails),
// so the tests that use Dir need Linux, as gatewayd does.
func statID(os.FileInfo) (fileID, bool) { return fileID{}, false }
