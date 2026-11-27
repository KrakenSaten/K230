// Copyright (c) 2026 PocketOS authors.
// SPDX-License-Identifier: Apache-2.0

package wire

import (
	"bufio"
	"bytes"
	"errors"
	"net/http"
	"strconv"
	"strings"
)

// The HTTP start of every relay connection (rule 39; RELAY_PROPOSAL.md §5.1).
const (
	ALPN     = "http/1.1"
	Path     = "/doors-relay/1"
	Protocol = "doors-relay/1"
	// MaxHead bounds a request or response head: the start line, the header
	// lines and the empty line, CRLFs included.
	MaxHead = 2048
)

var (
	// ErrHeadTooLong is a head above MaxHead bytes.
	ErrHeadTooLong = errors.New("wire: HTTP head above 2048 bytes")
	// ErrHeadMalformed is a head that begins with the empty line, or has a
	// line that does not end in CRLF.
	ErrHeadMalformed = errors.New("wire: malformed HTTP head")

	// Switching is the relay's whole answer to the Upgrade request.
	Switching = []byte("HTTP/1.1 101 Switching Protocols\r\n" +
		"Connection: Upgrade\r\n" +
		"Upgrade: " + Protocol + "\r\n\r\n")

	// NotFound is the relay's answer to anything else; the connection then
	// ends. It is what an ordinary Go server sends, without a Date.
	NotFound = []byte("HTTP/1.1 404 Not Found\r\n" +
		"Content-Type: text/plain; charset=utf-8\r\n" +
		"X-Content-Type-Options: nosniff\r\n" +
		"Content-Length: 19\r\n" +
		"Connection: close\r\n\r\n" +
		"404 page not found\n")
)

// ReadHead reads one HTTP head: a start line, header lines, and the empty
// line, every one ending in CRLF, of at most MaxHead bytes in all. It reads
// nothing past the empty line. An empty first line is malformed (no
// leading empty line is skipped).
func ReadHead(br *bufio.Reader) ([]byte, error) {
	var head []byte
	line := 0 // where the current line starts in head
	for {
		frag, err := br.ReadSlice('\n')
		head = append(head, frag...)
		if len(head) > MaxHead {
			return nil, ErrHeadTooLong
		}
		switch {
		case err == bufio.ErrBufferFull:
			continue
		case err != nil:
			return nil, err
		}
		if !bytes.HasSuffix(head[line:], []byte("\r\n")) {
			return nil, ErrHeadMalformed
		}
		if len(head)-line == 2 {
			if line == 0 {
				return nil, ErrHeadMalformed
			}
			return head, nil
		}
		line = len(head)
	}
}

// crlfOnly reports whether every CR is followed by LF and every LF follows a CR.
func crlfOnly(b []byte) bool {
	for i, c := range b {
		switch c {
		case '\r':
			if i+1 >= len(b) || b[i+1] != '\n' {
				return false
			}
		case '\n':
			if i == 0 || b[i-1] != '\r' {
				return false
			}
		}
	}
	return true
}

// StartRequest is the exact request a unit or a phone sends. The port is
// written in Host only when it is not 443.
func StartRequest(name string, port uint16) []byte {
	host := name
	if port != 443 {
		host += ":" + strconv.Itoa(int(port))
	}
	return []byte("GET " + Path + " HTTP/1.1\r\n" +
		"Host: " + host + "\r\n" +
		"Connection: Upgrade\r\n" +
		"Upgrade: " + Protocol + "\r\n\r\n")
}

// CheckStart reports whether head is exactly the relay's Upgrade request:
// GET of Path over HTTP/1.1 with three headers, each once and nothing else:
// Host naming this relay (with or without its port), Connection: Upgrade
// and Upgrade: doors-relay/1. Header names and these values compare without
// regard to ASCII case. TLS (SNI, ALPN) is checked by the caller.
func CheckStart(head []byte, name string, port uint16) bool {
	if len(head) > MaxHead || !crlfOnly(head) {
		return false
	}
	req, err := http.ReadRequest(bufio.NewReader(bytes.NewReader(head)))
	if err != nil {
		return false
	}
	if req.Method != http.MethodGet || req.RequestURI != Path || req.Proto != "HTTP/1.1" ||
		req.URL.Host != "" || req.ContentLength != 0 || len(req.TransferEncoding) != 0 {
		return false
	}
	host := req.Host
	if !strings.EqualFold(host, name) && !strings.EqualFold(host, name+":"+strconv.Itoa(int(port))) {
		return false
	}
	seen := 0
	for k, v := range req.Header {
		switch k {
		case "Host":
			if len(v) != 1 || v[0] != host {
				return false
			}
		case "Connection":
			if len(v) != 1 || !strings.EqualFold(v[0], "Upgrade") {
				return false
			}
			seen++
		case "Upgrade":
			if len(v) != 1 || !strings.EqualFold(v[0], Protocol) {
				return false
			}
			seen++
		default:
			return false
		}
	}
	return seen == 2
}

// ErrNotSwitching is any answer to the HTTP start but the 101.
var ErrNotSwitching = errors.New("wire: the relay did not switch protocols")

// ReadSwitching reads the relay's answer to the HTTP start and accepts only
// the 101 with Upgrade: doors-relay/1.
func ReadSwitching(br *bufio.Reader) error {
	head, err := ReadHead(br)
	if err != nil {
		return err
	}
	if !crlfOnly(head) {
		return ErrNotSwitching
	}
	resp, err := http.ReadResponse(bufio.NewReader(bytes.NewReader(head)), nil)
	if err != nil {
		return ErrNotSwitching
	}
	if resp.StatusCode != http.StatusSwitchingProtocols ||
		!strings.EqualFold(resp.Header.Get("Upgrade"), Protocol) ||
		!strings.EqualFold(resp.Header.Get("Connection"), "Upgrade") {
		return ErrNotSwitching
	}
	return nil
}
