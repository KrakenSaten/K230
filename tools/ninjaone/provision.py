#!/usr/bin/env python3
# Native OAuth PKCE login on a trusted local workstation; no hosted backend.
# Copyright (c) 2026 PocketOS authors.
# SPDX-License-Identifier: Apache-2.0
"""Emit one monitoring access token into a secure pipe, never to a terminal.

Register a Native app with Authorization Code and monitoring only. Register
http://127.0.0.1:8765/callback exactly; tenant acceptance of this loopback URI
must be checked locally. Refresh tokens are intentionally not requested.
"""
import argparse
import base64
import hashlib
import http.server
import json
import re
import secrets
import sys
import time
import urllib.parse
import urllib.request
import webbrowser

BASES = {"https://app.ninjarmm.com", "https://eu.ninjarmm.com",
         "https://oc.ninjarmm.com", "https://ca.ninjarmm.com",
         "https://us2.ninjarmm.com", "https://app.ninjaone.com"}
REDIRECT = "http://127.0.0.1:8765/callback"


def challenge(verifier):
    return base64.urlsafe_b64encode(hashlib.sha256(verifier.encode("ascii")).digest()).rstrip(b"=").decode("ascii")


def callback_code(path, state):
    parts = urllib.parse.urlsplit(path)
    fields = urllib.parse.parse_qs(parts.query, strict_parsing=True)
    if parts.path != "/callback" or fields.get("state") != [state] or "error" in fields:
        raise ValueError("invalid callback")
    code = fields.get("code", [])
    if len(code) != 1 or not code[0] or len(code[0]) > 4096:
        raise ValueError("invalid code")
    return code[0]


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, *_args, **_kwargs):
        raise ValueError("redirect refused")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base", required=True, choices=sorted(BASES))
    parser.add_argument("--client-id", required=True)
    args = parser.parse_args()
    if sys.stdout.isatty():
        parser.error("token output must be piped to pos-ninjaone set-token, never a terminal")
    verifier, state = secrets.token_urlsafe(32), secrets.token_urlsafe(32)
    received = []

    class Callback(http.server.BaseHTTPRequestHandler):
        def log_message(self, *_args):
            pass  # Do not log the authorization code or callback URL.

        def do_GET(self):
            try:
                received.append(callback_code(self.path, state))
                self.send_response(200)
                self.end_headers()
                self.wfile.write(b"Login received. You may close this window.")
            except ValueError:
                self.send_response(400)
                self.end_headers()
                self.wfile.write(b"Invalid login callback.")

    url = args.base + "/ws/oauth/authorize?" + urllib.parse.urlencode({
        "response_type": "code", "client_id": args.client_id, "redirect_uri": REDIRECT,
        "scope": "monitoring", "code_challenge": challenge(verifier),
        "code_challenge_method": "S256", "state": state})
    try:
        with http.server.HTTPServer(("127.0.0.1", 8765), Callback) as server:
            server.timeout = 1
            if not webbrowser.open(url):
                raise ValueError("browser unavailable")
            deadline = time.monotonic() + 180
            while not received and time.monotonic() < deadline:
                server.handle_request()
            if not received:
                raise ValueError("login timed out")
        body = urllib.parse.urlencode({"grant_type": "authorization_code", "client_id": args.client_id,
                                     "redirect_uri": REDIRECT, "code": received[0],
                                     "code_verifier": verifier}).encode("ascii")
        request = urllib.request.Request(args.base + "/ws/oauth/token", body,
                                         {"Content-Type": "application/x-www-form-urlencoded"})
        opener = urllib.request.build_opener(NoRedirect())  # Default verified TLS.
        with opener.open(request, timeout=15) as response:
            raw = response.read(65537)
            if len(raw) > 65536:
                raise ValueError("oversized token response")
            data = json.loads(raw)
        token = data.get("access_token")
        if (data.get("token_type", "").lower() != "bearer" or
                set(data.get("scope", "").split()) != {"monitoring"} or
                not isinstance(token, str) or not re.fullmatch(r"[A-Za-z0-9._~+/=-]{1,4095}", token)):
            raise ValueError("invalid token response")
        print(token)  # The sole token output, into the user-selected secure pipe.
        print("Monitoring token issued to the provisioning pipe. Check its exit status; log in again after expiry.", file=sys.stderr)
    except Exception:
        # Never print an exception containing URLs, responses, or credentials.
        print("Local OAuth setup/login failed. Check native registration, redirect, scope and tenant PKCE support.", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
