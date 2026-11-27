// Copyright (c) 2026 PocketOS authors.
// SPDX-License-Identifier: Apache-2.0

package wire

import (
	"encoding/binary"
	"errors"
	"io"
)

// MaxFrame is the largest frame payload (the JSON text), in bytes.
const MaxFrame = 4096

// ErrFrame is a length prefix of 0 or above MaxFrame.
var ErrFrame = errors.New("wire: frame length out of range")

// ReadFrame reads one frame: a 4-byte big-endian length, then that many
// bytes. It never allocates more than MaxFrame for the payload.
func ReadFrame(r io.Reader) ([]byte, error) {
	var h [4]byte
	if _, err := io.ReadFull(r, h[:]); err != nil {
		return nil, err
	}
	n := binary.BigEndian.Uint32(h[:])
	if n == 0 || n > MaxFrame {
		return nil, ErrFrame
	}
	b := make([]byte, n)
	if _, err := io.ReadFull(r, b); err != nil {
		if err == io.EOF {
			err = io.ErrUnexpectedEOF
		}
		return nil, err
	}
	return b, nil
}

// AppendFrame appends the length prefix and the payload to dst.
func AppendFrame(dst, payload []byte) []byte {
	dst = binary.BigEndian.AppendUint32(dst, uint32(len(payload)))
	return append(dst, payload...)
}

// WriteFrame writes one frame in a single Write.
func WriteFrame(w io.Writer, payload []byte) error {
	if len(payload) == 0 || len(payload) > MaxFrame {
		return ErrFrame
	}
	_, err := w.Write(AppendFrame(make([]byte, 0, 4+len(payload)), payload))
	return err
}

// ReadMsg reads and decodes one frame.
func ReadMsg(r io.Reader) (Msg, error) {
	b, err := ReadFrame(r)
	if err != nil {
		return nil, err
	}
	return Decode(b)
}

// WriteMsg encodes and writes one frame.
func WriteMsg(w io.Writer, m Msg) error { return WriteFrame(w, Encode(m)) }
