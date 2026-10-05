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

A service may also have topics: events a subscriber asks for by name, on
top of the subscription (`pocketipc_client_set_topics`,
`pocketipc_server_broadcast_topic`). A connection starts with none and loses
them with the connection; an event sent to a topic reaches only subscribed
connections holding it, so a client that never asked - an older one that
would not know the event - never receives it. meshcored's receive log is the
first (docs/api/mesh.md, "The receive log").

## Backpressure

Service-side client sockets are non-blocking. When a client stops reading
and its socket buffer fills, a write blocks the service's single loop, which
must never happen for one misbehaving client. Policy (bounded, tested in
`tests/pocketipc_test.c`):

- a write that would block waits for the peer to drain for at most
  `POCKETIPC_SEND_TIMEOUT_MS` (200 ms) per frame, header and body sharing one
  budget;
- if the peer still has not drained, the write fails with `ETIMEDOUT` and
  the service disconnects that client, dropping whatever it had not read;
- clients that read late but within the window lose nothing.

There is no output queue in v0: the kernel socket buffer is the queue.
Clients that subscribe to events must read continuously.

## Request deadlines

Backpressure bounds what a slow *client* can do to a service. The other
direction is the caller's own choice, and the two are not symmetric.

`pocketipc_call` waits for as long as the service takes. `pocketipc_call_timeout`
gives up after a caller-supplied number of milliseconds and reports a
**transport failure** (`*code` 0, message `"<method> timed out after N ms"`),
not an error response. A caller that times out **must close the connection**:
the response may still arrive, and reading it as the answer to the next
request would desynchronise the two. `shell_ipc_call_timeout` does this, and
reconnects on the next call.

Which calls should carry a deadline is a judgement about what the call means,
not a default to apply everywhere:

- a **periodic poll** should. If the service does not answer in time the
  caller can simply ask again, and the alternative is an unbounded wait on
  whatever thread the poll runs on. The shell's once-a-second `radio.status`
  poll uses 200 ms for this reason (`SHELL_IPC_UI_TIMEOUT_MS`), and so does
  every app tick on the LVGL thread: the Radio app's `radio.info`,
  `radio.status` and `radio.stats` refresh, and its mock inject button. On
  unit A (2026-09-08) the poll alone being bounded was not enough; the app
  tick made the same calls without a deadline and froze the panel until
  radiod answered again. The deadline covers **connecting** too
  (`pocketipc_connect_timeout`): a caller that times out drops its
  connection, but the kernel keeps that connection queued in the service's
  listen backlog until the service accepts it, so a service that is alive
  and accepting nothing fills its backlog with the caller's own abandoned
  connections within seconds, and a blocking `connect()` then sleeps with
  no limit before any request deadline can apply (unit A, v0.0.4). The
  bounded connect is non-blocking, retries until the deadline and leaves
  nothing queued when it gives up.
- a **request whose completion is the point** should not, unless the service
  reports completion separately. `radio.send` is synchronous and blocks
  radiod for the airtime; a deadline there would tell the user the packet
  failed while it was being transmitted. It keeps waiting. `radio.send_async`
  is the shape that *can* be given a deadline: the reply only says the
  request was accepted, and the outcome comes as an event.

A handler still cannot accept a request and answer the *same* request later:
it must send exactly one response before it returns, so a service is
unavailable to every client for as long as any one handler runs. What a
service can do is answer immediately with a handle and report the outcome as
an event, which is what radiod's asynchronous transmit does (`tx_id` plus
`radio.tx_done`, docs/api/radio.md). Deferred replies remain a design item;
so far nothing has needed them.

## Connection identity and disconnection

The server helper gives each accepted connection an id, 1 upwards, never
reused while the server lives (`pocketipc_client_id`). The file descriptor
is not an identity: the kernel hands the same number to the next client the
moment one closes, so per-connection state keyed by descriptor can be
inherited by a stranger. Ids restart at 1 with the service, which is safe
because a restart closes every connection there was.

A service that keeps per-connection state registers
`pocketipc_server_set_on_disconnect`. It is called once when a connection
goes away for any reason - the peer closed it, it sent a frame the reader
refused, or a write to it failed - with the id the handler saw, after the
connection is already gone. radiod releases its radio lease there; without
it, a protocol daemon that crashed would hold the radio until the service
restarted.

Two rules make it safe to do real work in that callback:

- It is **never nested**. A callback that broadcasts can itself drop a slow
  client, which would otherwise re-enter the callback from inside itself.
  Those are queued and delivered after the current one returns.
- It is **not called from `pocketipc_server_free`**. The service is shutting
  down, every connection is ending at once, and per-connection cleanup has
  nothing left to protect.

## Peer disappearance

The library never raises SIGPIPE: every frame is sent with `MSG_NOSIGNAL`,
so a peer that has gone away is reported as `-1` with `errno == EPIPE`
(clients see `pocketipc_call` return NULL with code 0, "send failed"). A
process using pocketipc does not need `signal(SIGPIPE, SIG_IGN)` for its
pocketipc sockets; sockets it opens by other means remain its own business.
Confirmed on the bench (2026-09-07): before this rule the shell died with
SIGPIPE on its next status poll whenever radiod crashed.

A client that holds a connection across a service restart therefore sees the
ordinary sequence: the write fails with `EPIPE`, the call reports a transport
failure, the fd is closed, and `pocketipc_connect` succeeds again once the
service is back. `tests/pocketipc_test.c` walks that whole sequence in
`test_service_restart`, which does the failing write in a child with SIGPIPE
at its default disposition so that a regression is reported as a named failed
check rather than killing the test binary.

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
