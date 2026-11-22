#!/usr/bin/env python3
# One pocketipc request to a service's socket, for shell tests that need
# typed parameters or events (pos call passes text only).
#
#   pocketipc_call.py <socket> <method> [json params]
#       prints "OK <result json>" or "ERR <code> <message>"
#   pocketipc_call.py <socket> --listen <seconds> <subscribe method> <ready file>
#       subscribes, touches the ready file, then prints one "EVENT <name> <data json>"
#       line per event until the time is up
#
# Framing: a 4-byte big-endian length, then that many bytes of JSON.
#
# Copyright (c) 2026 Doors authors.
# SPDX-License-Identifier: Apache-2.0
import json
import socket
import struct
import sys
import time


def send(sock, obj):
    data = json.dumps(obj).encode()
    sock.sendall(struct.pack(">I", len(data)) + data)


def recv(sock):
    head = b""
    while len(head) < 4:
        part = sock.recv(4 - len(head))
        if not part:
            raise EOFError
        head += part
    (n,) = struct.unpack(">I", head)
    body = b""
    while len(body) < n:
        part = sock.recv(n - len(body))
        if not part:
            raise EOFError
        body += part
    return json.loads(body)


def main():
    path = sys.argv[1]
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.settimeout(5)
    sock.connect(path)
    if sys.argv[2] == "--listen":
        seconds = float(sys.argv[3])
        send(sock, {"id": 1, "method": sys.argv[4]})
        while "id" not in recv(sock):
            pass
        open(sys.argv[5], "w").close()
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            sock.settimeout(max(0.05, end - time.monotonic()))
            try:
                msg = recv(sock)
            except (socket.timeout, EOFError):
                break
            if "event" in msg:
                print("EVENT", msg["event"], json.dumps(msg.get("data"), sort_keys=True), flush=True)
        return 0
    params = json.loads(sys.argv[3]) if len(sys.argv) > 3 else None
    req = {"id": 1, "method": sys.argv[2]}
    if params is not None:
        req["params"] = params
    send(sock, req)
    while True:
        msg = recv(sock)
        if msg.get("id") == 1:
            break
    if "error" in msg:
        print("ERR", msg["error"].get("code"), msg["error"].get("message"))
    else:
        print("OK", json.dumps(msg.get("result"), sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
