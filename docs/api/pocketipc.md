# pocketipc v0: PocketOS service IPC

Status: draft, implemented by `core/pocketipc` and `services/radiod`.

pocketipc is the one way applications, the shell and the `pos` CLI talk to
PocketOS services (ADR-002). It is deliberately small: Unix-domain stream
sockets, length-prefixed UTF-8 JSON, request/response plus events.

## Transport

- One Unix-domain socket per service under the runtime directory:
  `$POCKETOS_RUNTIME_DIR` if set, else `/run/pocketos`. Example:
  `/run/pocketos/radiod.sock`.
- The service creates the socket with mode 0660. Access control is by file
  permission; there is no in-band authentication in v0.
- Framing: 4-byte big-endian unsigned length, then exactly that many bytes of
  UTF-8 JSON encoding one object. Maximum frame size is 1 MiB. A peer that
  sends a longer frame or invalid JSON is disconnected.

## Messages

Request (client to service):

```json
{"id": 7, "method": "radio.status", "params": {}}
```

- `id`: integer chosen by the client, echoed in the response. Clients may
  pipeline requests; responses may arrive in any order.
- `method`: `<service>.<name>`; unknown methods yield error code 1.
- `params`: object, may be omitted.

Response (service to client), exactly one per request:

```json
{"id": 7, "result": {"state": "idle"}}
{"id": 7, "error": {"code": 3, "message": "frequency outside region EU868"}}
```

Event (service to client, only to subscribed clients, no `id`):

```json
{"event": "radio.rx", "data": {"payload_hex": "48656c6c6f", "rssi_dbm": -87.5}}
```

Subscriptions are per connection, set with `<service>.subscribe` and cleared
with `<service>.unsubscribe` or by disconnecting.

## Backpressure

Service-side client sockets are non-blocking. When a client stops reading
and its socket buffer fills, a write blocks the service's single loop, which
must never happen for one misbehaving client. Policy (bounded, tested in
`tests/pocketipc_test.c`):

- a write that would block waits for the peer to drain for at most
  `POCKETIPC_SEND_TIMEOUT_MS` (200 ms) per frame;
- if the peer still has not drained, the write fails with `ETIMEDOUT` and
  the service disconnects that client, dropping whatever it had not read;
- clients that read late but within the window lose nothing.

There is no output queue in v0: the kernel socket buffer is the queue.
Clients that subscribe to events must read continuously.

## Peer disappearance

The library never raises SIGPIPE: every frame is sent with `MSG_NOSIGNAL`,
so a peer that has gone away is reported as `-1` with `errno == EPIPE`
(clients see `pocketipc_call` return NULL with code 0, "send failed"). A
process using pocketipc does not need `signal(SIGPIPE, SIG_IGN)` for its
pocketipc sockets; sockets it opens by other means remain its own business.
Confirmed on the bench (2026-09-07): before this rule the shell died with
SIGPIPE on its next status poll whenever radiod crashed.

## Error codes

| Code | Meaning |
| --- | --- |
| 1 | unknown method |
| 2 | invalid params |
| 3 | rejected by policy (for example regulatory guard) |
| 4 | hardware or backend failure |
| 5 | busy, retry later |
| 6 | not supported by this backend |

## Versioning

Every service answers `<service>.info` with `api_version` (integer). Adding
fields to results or events is backwards compatible. Renaming or removing
anything, or changing a field's type, bumps `api_version`. Clients must
ignore unknown fields.

## Why not D-Bus

D-Bus is present in the vendor rootfs because wpa_supplicant and BlueZ use
it, and PocketOS may talk to those over D-Bus inside `netd`. For PocketOS'
own services it adds a daemon dependency, XML introspection and a heavier
client library for little gain on a two-process system. Revisit if
application sandboxing needs a broker.
