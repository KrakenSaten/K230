#!/usr/bin/env python3
# Native OAuth PKCE login on a trusted local workstation; no hosted backend.
# Copyright (c) 2026 PocketOS authors.
# SPDX-License-Identifier: Apache-2.0
"""Emit a Native PKCE credential bundle into a secure pipe.

Enable Authorization Code, Refresh Token and Monitoring. Native redirect URLs
are fixed by NinjaOne: supply the exact value shown locally, never invent one.
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
         "https://us2.ninjarmm.com", "https://jp.ninjarmm.com", "https://fed.ninjarmm.com"}


def redirect_address(uri):
    p = urllib.parse.urlsplit(uri)
    if (p.scheme != "http" or p.hostname not in {"localhost", "127.0.0.1"} or
            p.username or p.password or p.query or p.fragment or (p.path and not p.path.startswith("/"))):
        raise ValueError("unsupported native redirect")
    return ("127.0.0.1", p.port or 80), p.path or "/"


def credential_bundle(data, base, client, tenant, issued):
    scopes = set(data.get("scope", "").split())
    expires = data.get("expires_in")
    if (data.get("token_type", "").lower() != "bearer" or "monitoring" not in scopes or
            not scopes <= {"monitoring", "offline_access"} or
            type(expires) is not int or not 1 <= expires <= 604800):
        raise ValueError("invalid token response")
    for key in ("access_token", "refresh_token"):
        if not isinstance(data.get(key), str) or not re.fullmatch(r"[A-Za-z0-9._~+/=-]{1,4095}", data[key]):
            raise ValueError("invalid token")
    return {"access_token": data["access_token"], "refresh_token": data["refresh_token"],
            "expires_at": issued + expires, "base": base, "client_id": client,
            "profile": hashlib.sha256((base + "\n" + tenant).encode()).hexdigest()[:24]}


def challenge(verifier):
    return base64.urlsafe_b64encode(hashlib.sha256(verifier.encode("ascii")).digest()).rstrip(b"=").decode("ascii")


def callback_code(path, state, callback_path="/callback"):
    parts = urllib.parse.urlsplit(path)
    fields = urllib.parse.parse_qs(parts.query, strict_parsing=True)
    if parts.path != callback_path or fields.get("state") != [state] or "error" in fields:
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
    parser.add_argument("--redirect-uri", required=True, help="exact fixed Native redirect shown by NinjaOne")
    parser.add_argument("--tenant-id", required=True, help="stable unique nonsecret tenant identifier for favorites")
    args = parser.parse_args()
    if not re.fullmatch(r"[A-Za-z0-9._~+/=-]{1,159}", args.client_id) or not args.tenant_id.strip():
        parser.error("invalid client ID or tenant identifier")
    try:
        address, callback_path = redirect_address(args.redirect_uri)
    except ValueError:
        parser.error("Native redirect must be an explicit HTTP localhost/127.0.0.1 URI")
    if sys.stdout.isatty():
        parser.error("token output must be piped to pos-ninjaone set-credentials, never a terminal")
    verifier, state = secrets.token_urlsafe(32), secrets.token_urlsafe(32)
    received = []

    class Callback(http.server.BaseHTTPRequestHandler):
        def log_message(self, *_args):
            pass  # Do not log the authorization code or callback URL.

        def do_GET(self):
            try:
                received.append(callback_code(self.path, state, callback_path))
                self.send_response(200)
                self.end_headers()
                self.wfile.write(b"Login received. You may close this window.")
            except ValueError:
                self.send_response(400)
                self.end_headers()
                self.wfile.write(b"Invalid login callback.")

    url = args.base + "/ws/oauth/authorize?" + urllib.parse.urlencode({
        "response_type": "code", "client_id": args.client_id, "redirect_uri": args.redirect_uri,
        "scope": "monitoring offline_access", "code_challenge": challenge(verifier),
        "code_challenge_method": "S256", "state": state})
    try:
        with http.server.HTTPServer(address, Callback) as server:
            server.timeout = 1
            if not webbrowser.open(url):
                raise ValueError("browser unavailable")
            deadline = time.monotonic() + 180
            while not received and time.monotonic() < deadline:
                server.handle_request()
            if not received:
                raise ValueError("login timed out")
        body = urllib.parse.urlencode({"grant_type": "authorization_code", "client_id": args.client_id,
                                     "redirect_uri": args.redirect_uri, "code": received[0],
                                     "code_verifier": verifier}).encode("ascii")
        request = urllib.request.Request(args.base + "/ws/oauth/token", body,
                                         {"Content-Type": "application/x-www-form-urlencoded"})
        opener = urllib.request.build_opener(NoRedirect())  # Default verified TLS.
        with opener.open(request, timeout=15) as response:
            raw = response.read(65537)
            if len(raw) > 65536:
                raise ValueError("oversized token response")
            data = json.loads(raw)
        bundle = credential_bundle(data, args.base, args.client_id, args.tenant_id, int(time.time()))
        print(json.dumps(bundle, separators=(",", ":")))  # Sole secret output, into secure pipe.
        print("Sign-in bundle sent to the provisioning pipe. Check the pipeline exit status.", file=sys.stderr)
    except Exception:
        # Never print an exception containing URLs, responses, or credentials.
        print("Local OAuth setup/login failed. Check native registration, redirect, scope and tenant PKCE support.", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
