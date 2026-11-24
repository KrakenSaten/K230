# Shell: methods served by the app in front (implementation plan)

Status: **plan, not implemented.** The hook `docs/design/companion/CONTRACTS.md` section 3 asks for: a way for an app inside the shell to serve named methods on `shell.sock` while it is in front. PacketShark needs it for `capture.*` (ADR-016 amendment 1, Proposed) and Vision for `vision.*` (ADR-006 and ADR-019 amendments 1, Proposed). The hook itself exposes nothing to a phone: until gatewayd has rows for a method, only a local process that can open the socket (`doors call shell ...`) can reach it. Building the hook therefore does not need the amendments; using it from the gateway does.
Date: 2026-11-22

## 1. What is there today

| Fact | Where |
| --- | --- |
| An app is a `struct pocketos_app` with `create`, `tick`, `destroy` and optional members appended over time; a zero member means "as before" and the API version stays 0 | `ui/shell/app.h` |
| Requests on `shell.sock` are handled in one function, a chain of `strcmp` over 24 `shell.*` names, on the LVGL thread, each ending in `pocketipc_server_reply` | `ui/shell/shell.c`, `on_shell_request` |
| A request that acts like a person wakes the screen first; reading requests are listed as exceptions | same function, its first block |
| A client becomes an event listener with `shell.subscribe` | same function |
| The shell knows the app in front (`sh.app`) and its private pointer | `ui/shell/shell.c` |
| pocketipc frames may be 1 MiB | `core/pocketipc/pocketipc.h`, `POCKETIPC_MAX_FRAME` |
| Everything on the LVGL thread that waits on a service has a 200 ms deadline, because one unbounded call froze the panel | `ui/shell/shell_ipc.h` |
| No app serves a method, and no method name outside `shell.` exists on this socket | `docs/api/shell.md` |

## 2. The hook

Three members appended to `struct pocketos_app`, all zero for every existing app:

```c
/* The methods this app serves on shell.sock while it is in front: every
 * name begins with this prefix ("capture.", "vision."). NULL: none. */
const char *ipc_prefix;
/* Answer one request from memory. Returns 0 and sets *result (the shell
 * frees it), or a pocketipc error code and sets *message to a static string.
 * Called on the LVGL thread, never from inside an LVGL event. Must not call
 * a service, touch a file, wait, or start anything that outlives the call
 * except through the app's own state machine. */
int (*ipc_request)(void *priv, const char *method, const cJSON *params,
                   cJSON **result, const char **message);
/* Methods (full names, NULL-terminated) that may be called while the app is
 * NOT in front: the shell opens the app first, as shell.open would, and then
 * calls ipc_request. NULL: none. PacketShark lists capture.request_start;
 * Vision lists nothing, so nothing outside the unit can open the camera. */
const char *const *ipc_opens;
```

And one call for the app to raise an event, checked against its own prefix:

```c
void pocketos_shell_app_event(const char *event, cJSON *data); /* to shell.subscribe listeners */
```

**Dispatch**, in `on_shell_request`, before the final "unknown method":

1. The method begins with `shell.`: as today. Nothing below applies.
2. No registered app has a prefix the method begins with: error 1, unknown method.
3. The owning app is in front: call `ipc_request`, reply with its result or error.
4. The owning app is not in front and the method is in its `ipc_opens`: refuse with error 3 when the unit is locked or showing an alert; otherwise open the app and call `ipc_request` once it is created (same loop iteration; `create` is synchronous).
5. The owning app is not in front otherwise: reply `{"available": false}`. This is the contract's own answer (`capture.status`, `vision.status`), given by the shell so that an app that is not running needs no code for it.

**Rules the shell enforces**, not the app:

- App methods never wake the screen and are not activity, except a method in `ipc_opens` (it is the request of a person, at a distance). They are added to the exception list at the top of `on_shell_request`.
- Two prefixes may not overlap, and none may begin with `shell.`; checked once at start, a violation is a startup failure in every build.
- A handler's time is measured (monotonic clock around the call). Over 2 ms it is counted and, in a debug or test build, logged with the method name; `shell.info` reports the worst time and the count, so a gate can read it.
- A result is serialised with a size limit per prefix (8 KiB by default; `vision.` 128 KiB for `vision.pull`); a larger one is replaced by error 4.
- Events raised through `pocketos_shell_app_event` must carry the app's prefix; another name is dropped and counted.

**What the hook does not do.** It does not authenticate: `shell.sock` is a local socket with the access it has today, and the gateway decides who may call what before it calls. It does not queue: a method arrives, is answered, and is gone. It lets an app run nothing while closed.

## 3. Steps

Each step builds and is tested on the host before the next; none touches a unit.

| Step | What | Files | Done when |
| --- | --- | --- | --- |
| H1 | The three members, `pocketos_shell_app_event`, dispatch rules 1 to 5, the timing counters, the prefix check | `ui/shell/app.h`, `ui/shell/shell.c`, `docs/api/shell.md` | `tests/shell_ipc_test.sh` extended: a probe app compiled only into the simulator's test build serves `probe.echo` and `probe.open`; in front it answers, behind it says `available: false`, `probe.open` opens it, a locked unit refuses it, an unknown prefix is error 1, an over-long result is error 4, a slow handler is counted |
| H2 | PacketShark serves `capture.status` from `psk_model` | `apps/packetshark/psk_app.c`, a new `apps/packetshark/psk_remote.c` (pure C: model in, cJSON out) | `tests/packetshark_shell_test.sh`: the figures of a replayed capture equal the screen's; `tests/packetshark_lint.sh` extended so that `psk_remote.c` includes nothing that holds an address, a name, a row or packet bytes |
| H3 | `capture.hold`, `capture.stop`; the events `capture.state` and `capture.stats` | same | shell test: hold and stop change the app exactly as the on-screen keys do; an event per second while capturing |
| H4 | `capture.request_start`: the sheet, its 60 s refusal, the header mark while a companion reads (ADR-016 rules 1a and 20) | PacketShark UI files, DS section for the sheet | shell test and screenshots; the sheet cannot be raised while a capture runs or another sheet is up |
| G1 | gatewayd: a non-blocking uplink to the shell (the `gw_uplink` meshcored already uses; the present blocking `upstream_call` takes no parameters), scopes, topic and rows for `capture.*`, the 30 s request limit, lint and tests | `services/gatewayd/gw_policy.[ch]`, `main.c`, `tests/gatewayd_lint.sh`, `tests/gatewayd_*` | `make gatewayd-test`; then the Android `GatewayCapture` against the WSL harness with a fake shell that speaks H2 to H4 |
| V1.. | Vision, only if its amendments are accepted, after the measurements of `VISION_PREVIEW.md` section 8 | helper, Vision app, gatewayd | separate plan |

H1 to H3 change nothing a phone can reach. H4 and G1 are the first that need ADR-016 amendment 1 accepted. H1 alone is about 150 lines of C and its test; it is the next practical step and blocks everything else.

## 4. Decisions H1 needs from the owner

1. Is `{"available": false}` from the shell (rule 5) right for every app method, or should a closed app's methods be "unknown method"? The plan assumes the first, because both contracts already say it.
2. May a method in `ipc_opens` open an app at all (rule 4)? ADR-016 amendment 1 rule 21 says the start request "opens it". Without that, a start request works only while PacketShark is already in front.
3. The 2 ms handler budget and the 8 KiB default result limit: accept, or name others.

## 5. Risks

- A handler that blocks freezes the panel. The rule is in the header, the time is measured, and the first two apps answer from structures they already hold.
- The request chain in `on_shell_request` is long; the hook adds one branch at its end and no new `strcmp` per app.
- A local process that can open the socket can now call an app's methods. It could already drive the whole shell (`shell.key`, `shell.open`).
