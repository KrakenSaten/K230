# DOORS Relay (`relay/`)

An optional, self-hosted relay that lets a paired phone reach its Doors unit when the local network does not let them meet (ADR-019 amendment 3). It is a blind pipe: the gateway's TLS 1.3 runs end to end inside it, and the relay never decrypts or impersonates either end. Off by default on every unit. Doors operates no relay.

Status: **phase 1, host-tested only. Not deployed, and no host was set up.** The unit's side and the app's side do not exist yet (phases 2 and 3).

- Protocol: [docs/api/relay.md](../docs/api/relay.md).
- Design: [RELAY_PROPOSAL.md](../docs/design/companion/RELAY_PROPOSAL.md).
- Threats: [RELAY_THREAT_MODEL.md](../docs/design/companion/RELAY_THREAT_MODEL.md).

## Build and test

The Go standard library is all it uses; there is no third-party module. The build environment is in `docs/BUILD_ENVIRONMENT.md` ("DOORS Relay").

```sh
cd relay
go vet ./...
go test -race ./...
go build -o doors-relay ./cmd/doors-relay
```

Fuzz targets, one at a time:

```sh
go test -run='^$' -fuzz='^FuzzFrames$'      -fuzztime=60s ./internal/wire/
go test -run='^$' -fuzz='^FuzzStart$'       -fuzztime=60s ./internal/wire/
go test -run='^$' -fuzz='^FuzzEndorsement$' -fuzztime=60s ./internal/wire/
```

## Set up (operator)

Run these on the relay host, not on a unit.

```sh
doors-relay keygen        -out /etc/doors-relay/relay.key   # prints the key's 16-digit and full fingerprint
doors-relay backup-keygen -out backup.key                    # prints the backup_hash line; then move backup.key offline
doors-relay enroll -config /etc/doors-relay/relay.json <device_id>
doors-relay serve  -config /etc/doors-relay/relay.json
```

- **The relay key:** a person compares the sixteen digits that `keygen` printed with the ones the unit shows at set-up.
- **Enrolling a unit:** `enroll` takes the unit's `device_id`, its 64-hex fingerprint, and prints it in both forms for comparison with the unit's Settings. The relay reads the configuration at start only, so restart it after enrolling.
- **Logs** go to standard error, as `key=value` lines (rule 41: fingerprint prefixes and truncated addresses only).

`relay.json`:

```json
{
  "listen": ":443",
  "name": "relay.example.org",
  "port": 443,
  "key_file": "relay.key",
  "backup_hash": "<64 hex from backup-keygen>",
  "units": ["<device_id, 64 hex>"],
  "limits": { "per_source_rate": 10, "per_source_unauth": 64, "max_conns": 1024, "session_idle_s": 90 }
}
```

- **`name` and `port`** are what units and phones dial; `port` is the public one. `listen` may differ, for example behind a port forward, or an SNI pass-through such as nginx `stream` with `ssl_preread`. A TLS-terminating proxy or CDN is not supported (rule 39).
- **`cert_file` (optional)** is a PEM chain whose leaf carries `key_file`'s key, for example from certbot with `--reuse-key`. The relay presents it instead of its self-signed certificate. Units and phones still decide by the leaf key alone.
- **`limits`** are optional; the values above are the defaults. Unknown members are refused.

**Rotation:**
1. Bring the backup online: `key_file` becomes the backup's key file.
2. Make a new backup with `backup-keygen` and put its hash in `backup_hash`.
3. Restart the relay.

Each unit proves the promotion through the relay alone, and phones follow by the unit's endorsement (`RELAY_PROPOSAL.md` §4.4).

## Layout

| Path | What |
| --- | --- |
| `cmd/doors-relay` | the server and its tools |
| `internal/wire` | the protocol: frames, the HTTP start, the signed bytes, test vectors, fuzz targets |
| `internal/server` | the relay, and its tests against fake units and phones (threat model §5) |
| `internal/fake` | Go stand-ins of the unit (gatewayd and its helper) and the phone (RelayTransport), with the checks those ends must make |
| `internal/liar`, `cmd/lying-relay` | a lying relay for tests of later phases; **never deploy it** |

**The lying relay** speaks the protocol but lies in named ways:
- it can present another key, an RSA key or TLS 1.2;
- it can sign bad backups and key proofs, or offer a backup nobody asked for;
- it can withhold `admitted` and ignore removals, and admit anyone;
- it can serve forged or stale endorsements;
- it can send oversized frames, garbage, ping floods and bogus invitations.

For example: `go run ./cmd/lying-relay -listen 127.0.0.1:8443 -lie admit-anyone,withhold-admitted`.
