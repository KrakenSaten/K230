// Copyright (c) 2026 PocketOS authors.
// SPDX-License-Identifier: Apache-2.0

//go:build !linux

package fake

import "os"

// statID cannot tell a file's identity here, so nothing recorded matches
// and recovery drops every grant (fail-closed). gatewayd runs on Linux.
func statID(os.FileInfo) (fileID, bool) { return fileID{}, false }
