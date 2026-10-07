# services/meshcored — the MeshCore protocol service

The first protocol daemon on top of `radiod`. It owns the MeshCore runtime and
nothing below it: no SPI device, no GPIO, no radio library, no LoRa driver.

```
SX1262 -> radiod -> [radio.*] -> meshcored -> [mesh.*] -> a client
```

It speaks direct messages and group channels. A channel is a pre-shared key:
the key never leaves this service, the one-byte hash derived from it is what
goes on the air, and nothing acknowledges what is sent on one. See
[docs/api/mesh.md](../../docs/api/mesh.md), "What a channel is, and is not".

Full documentation:

| | |
| --- | --- |
| The service, its ownership, persistence, safety and how to enable it | [docs/services/MESHCORED.md](../../docs/services/MESHCORED.md) |
| The `mesh.*` IPC surface | [docs/api/mesh.md](../../docs/api/mesh.md) |
| The radio it sits on | [docs/api/radio.md](../../docs/api/radio.md) |
| The protocol core it links | [protocols/meshcore/README.md](../../protocols/meshcore/README.md) |

## The files

| | |
| --- | --- |
| `main.c` | the daemon: arguments, signals, the event loop, the service state |
| `mcd.h` | the shared vocabulary: state, profile, counters, configuration |
| `radio_link.c/.h` | the radiod connection — an asynchronous pocketipc *client*, because the library's synchronous one discards events while it waits |
| `tx_map.c/.h` | the map between a MeshCore transmit and a radiod one, and the completion deadline |
| `api.c` | the `mesh.*` methods and the event shapes |
| `mcd_util.c/.h` | strict hex and input validation |
| `mesh_runtime.h` | **the seam** — the one door between the C daemon and the C++ protocol core |
| `mesh_runtime.cpp` | the MeshCore runtime and the `mesh::Radio` adapter |
| `mesh_store.h/.cpp` | the identity, node-table and channel files |

The seam is the thing to keep. Above `mesh_runtime.h` everything speaks JSON
and knows no MeshCore; below it everything is the protocol core and knows no
JSON, no sockets and no radiod. `tests/meshcored_lint.sh` checks both halves
by inspection, because that boundary stops being true quietly.

## Building and testing

Not part of `make all` or `make test`: it links `protocols/meshcore`, which
needs two upstream checkouts an ordinary Doors build does not have.

```sh
make ENABLE_MESHCORED=1 meshcored   # build
make meshcored-test                 # unit, runtime, lint and two host suites,
                                    # plain then under ASan and UBSan
```

`make meshcored-test` runs, in order: the four unit suites plain, the same
four sanitised, the boundary lint, `tests/meshcored_service_test.sh` (against
the real radiod on its mock backend) and `tests/meshcored_harness_test.sh`
(two whole meshcored processes over a mock air).

Nothing here touches hardware, and the service is disabled in every image
until an operator switches it on per unit.

Copyright (c) 2026 PocketOS authors. Licensed under the Apache License, Version 2.0 (see LICENSE at the repository root).
