// Copyright (c) 2026 PocketOS authors.
// SPDX-License-Identifier: Apache-2.0

//go:build linux

package fake

import (
	"os"
	"syscall"
)

// statID is the part of paired.v1's identity the file system holds.
func statID(fi os.FileInfo) (fileID, bool) {
	st, ok := fi.Sys().(*syscall.Stat_t)
	if !ok {
		return fileID{}, false
	}
	return fileID{
		Dev: uint64(st.Dev), Ino: uint64(st.Ino), Size: st.Size,
		CtimeNs: st.Ctim.Nano(), MtimeNs: st.Mtim.Nano(),
	}, true
}
