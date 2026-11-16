# PacketShark: a packet analyzer for Doors

**Status: IN DEVELOPMENT.** Milestones M0 to M3 of the design package are implemented on branch `feature/packetshark` (draft pull request). ADR-016 (ownership, privilege, privacy) and DS §59 (protocol colours, dense rows, launcher place) are **PROPOSED**, not accepted. The owner accepted four decisions on 2026-11-15: the helper owns the packets and the shell keeps at most 48 rows (D1), the helper runs at nice 5 (D5), HEADERS is the default for what is kept (D7), and captures stay in memory until SAVE (D10).

PacketShark captures the traffic an Ethernet interface sees, counts it and, in later milestones, lists, decodes, filters and saves it. It is **receive-only**: it never transmits, injects, spoofs or redirects anything.

Contents:

1. What is in this version, and what is not
2. What the unit can see
3. Architecture
4. What is kept, and for how long
5. Privilege
6. The line protocol
7. The screens
8. Building and running
9. Validation
10. Known limits and open items

The design package these milestones implement (product spec, architecture, UI/UX, implementation plan, decision log) was written before the code, outside the repository; the parts that bind the code are in ADR-016, DS §59 and this document. The mockups are in [../design/packetshark/](../design/packetshark/).

---

## 1. What is in this version, and what is not

| Milestone | Scope | State |
| --- | --- | --- |
| M0 | ADR-016, DS §59, this document, the mockups, the libpcap dependency declaration | Done; ADR-016 and DS §59 await the owner |
| M1 | `core/pocketpcap`: the decoder, the row formatter, the `.pcap` reader and writer; `pos-capture decode` | Done, host-tested |
| M2 | `pos-capture session`: live, replay and synthetic sources, the bounded store, counters and bins, the line protocol, the lock, the privilege drop, `pos-capture bench` | Done, host-tested; on Unit A see §9 |
| M3 | The app: setup, START, live counters, HOLD, STOP, DISCARD, the error panel, the launcher entry, the screen kept on | Done, host-tested; on Unit A see §9 |
| M4 | Traffic graph, protocol distribution | Not started |
| M5 | Packet list, display filter, sort | Not started (the helper already answers `window` for the newest or a given packet, without a filter) |
| M6 | Packet inspector, pins | Not started (the store already pins) |
| M7 | Signals: DNS failures, resets, retransmissions, unreachables, unanswered SYNs | Not started (the counters are carried as zeros) |
| M8 | SAVE and saved captures | Not started: **nothing is ever written to storage in this version** |
| M9 | Landscape layout of its own, keyboard keys, final icon | Not started (landscape uses the portrait column) |
| M10 | The Unit A gate at line rate | Not started |

Out of scope for PacketShark v0.1 altogether: Wi-Fi monitor mode (the installed driver configuration cannot do it), Wi-Fi capture in managed mode (listed, not offered, until validated on a test network), LoRa, any transmit path.

## 2. What the unit can see

On a switched network, without a mirror port, an interface receives broadcast, multicast and its own traffic (VERIFIED on Unit A, feasibility study 2026-11-15). That answers "is this network healthy from where I stand" and shows this device's own conversations. Seeing other devices' traffic needs a mirror port, a hub or a tap, and promiscuous mode, which **this version does not offer**: the kernel's reversion of the flag when the socket closes has not been verified on the RTL8152 (§5).

## 3. Architecture

Two processes per capture (DESIGN_DECISIONS D1, accepted):

```
 doors-shell (LVGL thread)                      pos-capture (one per START)
  apps/packetshark/                socketpair     tools/capture/pos_capture.c
   psk_app.c, ui/psk_live.c        text lines      core/pocketpcap/
   ui/psk_sheets.c              <------------->     ppc_source_*  libpcap | file | synth
   psk_session.c  spawn, poll, reap                 ppc_decode    frame -> 80-byte record
   psk_model.c    counters, 48-row cache            ppc_store     summary ring, byte ring, pins
   core/pocketpcap/ppc_proto.c only                 ppc_stats     counters, bins, time base
                                                    ppc_format    row text, on demand
                                                    ppc_proto     the line protocol
```

- **The helper is the model; the shell is a viewport.** Hostile bytes are parsed only in `pos-capture`. The shell links one file of the capture library, the protocol (`tests/packetshark_lint.sh` holds it). A decoder fault takes the helper, and the app says so.
- **The helper pattern is Zabbix's** (ADR-007): `socketpair`, `fork`, `PR_SET_PDEATHSIG` with a parent check, every inherited descriptor closed, stderr to `/dev/null`, `execv`; non-blocking reads from a 33 ms LVGL timer; hello within 3 s; while capturing, silence over 5 s kills it; an over-long line or more than 16 unreadable lines kills it; leaving the app sends `quit`, then SIGTERM, then SIGKILL, in at most 500 ms.
- **One helper serves one capture.** It drops its capabilities once its socket is open, so it cannot open another; NEW CAPTURE and DISCARD end it and the next START spawns a new one.
- **One capture at a time:** an exclusive `flock` on `$POCKETOS_RUNTIME_DIR/capture.lock`, taken before the socket opens and released by STOP.
- **One thread, `poll()`** over the capture descriptor and the socketpair; at most 32 × 256 frames per wake, then the commands are read.
- **nice 5** (D5, accepted): under contention the shell wins the one hart; the cost appears as counted kernel drops, not a frozen panel. Whether that costs drops at line rate is measured in the M10 gate.
- **Shed mode:** when 50 dispatches in a row come back full, frames are classified and counted (`shed`) but not stored until a dispatch comes back short.
- **The app spawns no helper until START.** Opening PacketShark captures nothing.

Files:

| Path | What |
| --- | --- |
| `core/pocketpcap/ppc_record.h` | The 80-byte record, classes, flags |
| `core/pocketpcap/ppc_decode.[ch]` | Decoder, the HEADERS rule, protocol words |
| `core/pocketpcap/ppc_format.[ch]` | Row text, DNS names (compression, loop and forward-pointer refusal) |
| `core/pocketpcap/ppc_pcapfile.[ch]` | Classic pcap writer (nanosecond) and reader |
| `core/pocketpcap/ppc_store.[ch]` | Summary ring (65,536 records), byte ring (16 MiB, monotonic offsets), 8 pins |
| `core/pocketpcap/ppc_stats.[ch]` | Counters, 120 one-second bins, the clock-step time base |
| `core/pocketpcap/ppc_proto.[ch]` | The line protocol (§6) |
| `core/pocketpcap/ppc_source*.c` | Sources: libpcap (`ppc_source_pcap.c`, the only file that includes `<pcap/pcap.h>`), none, file, synth |
| `tools/capture/pos_capture.c` | `pos-capture session`, `decode`, `bench`, `backend` |
| `apps/packetshark/` | The app (`psk_app.c`, `ui/`), its helper client and model |

## 4. What is kept, and for how long

- **Decoded:** Ethernet II, 802.1Q and 802.1ad (two tags), 802.3 length frames, ARP, IPv4 (options, fragments), IPv6 (hop-by-hop, routing, fragment, destination options), TCP, UDP, ICMP, ICMPv6 (neighbour discovery, MLD named), IGMP; by port DNS, mDNS, LLMNR, DHCP (message type from option 53), DHCPv6, NTP, NetBIOS-NS, SSDP. Every read is bounds-checked; a header that runs out sets `malformed`; nothing allocates (checked over a million frames).
- **Stored, HEADERS (default; D7, accepted):** link, network and transport headers; plus the whole message, up to 1024 bytes, for ARP, ICMP, ICMPv6, DNS, mDNS, LLMNR, DHCP, DHCPv6, NTP and NetBIOS-NS. No other payload: a plain-text HTTP request leaves nothing of its body in memory (tests: S4). DNS names and DHCP host names are kept; they are what the tool is for, and the setup screen says what HEADERS keeps.
- **Stored, FULL:** everything captured, up to 65,535 bytes a frame, chosen per capture after a confirmation, never remembered.
- **Where:** in the helper's memory only (D10, accepted): at most 65,536 records of 80 bytes (5 MiB) and 16 MiB of stored bytes, allocated once at START. **Nothing is written to storage**; there is no SAVE in this version. Leaving the app, DISCARD, NEW CAPTURE, or a helper crash discards the packets.
- **How long:** a capture stops by itself after 15 minutes by default (5, 15, 60 minutes, or no limit after a confirmation that is not remembered).
- **Logged:** lifecycle lines only, never an address, a name or a byte of a packet (`pos-capture.log`, `shell.log`; tests: S7).
- **Sent anywhere:** nothing. Neither process has network client code.

## 5. Privilege

All Doors processes run as root (docs/ARCHITECTURE.md). After its capture socket is open, the helper sets `no_new_privs`, clears the bounding set, empties its capability sets with `capset`, reads `/proc/self/status` back and reports what it found (`note caps ...`). A decoder fault can then still write files root owns, but cannot open sockets, change interfaces or load code into the kernel. On Unit A every capability set, the bounding set included, reads 0 with `NoNewPrivs: 1` while the capture runs (VERIFIED, §9.2). File capabilities, a dedicated user and seccomp are not available on the image or not checked (ADR-016).

Promiscuous mode is not offered in this version. The helper accepts the flag (libpcap sets it by socket membership, which the kernel reverts when the socket closes, DOCUMENTED, packet(7)), but the app shows OFF and takes no tap on the row until the reversion is verified on the hardware.

## 6. The line protocol

Tab-separated lines of at most 1024 bytes on the helper's stdin and stdout (`core/pocketpcap/ppc_proto.h` is the reference). Commands may also be typed with spaces, so the helper can be driven by hand from a serial console:

```
$ pos-capture session
hello	1	0.4.0	pcap
state	idle	none	-	0	1
start eth0 headers 0 all 900
note	caps	dropped bnd=empty
state	capturing	none	eth0	100	1
stats	250	12	12	0	0	0	0	1834	0	0	1	12	12	0	0	...
window 1 tail 9 0
rows.begin	1	0	3	12	1	12
row	4	1203466	2011	in	4	ARP	60	0	02:..	broadcast	Who has ...
...
stop
quit
```

Commands: `start <iface> <headers|full> <promisc> <all|ip|names|noself> <limit_s>`, `hold <0|1>`, `stop`, `discard`, `window <req> <tail|seq:N> <before> <after>` (at most 48 rows), `stats`, `quit`. Events: `hello`, `state`, `stats` (4 Hz), `bin` (each elapsed second), `rows.begin`/`row`/`rows.end`, `error <code> <text>`, `note`, `bye`. Snapshot lines (`stats`, `bin`) are dropped and counted (`ui_skipped`) when the shell has not read the previous ones; answers are queued in a 64 KiB buffer or refused with `error busy`. Sequence numbers start at 1 and are never reused; a packet that left the store is simply not there.

## 7. The screens

Three tabs, LIVE, PACKETS and INSPECT (disabled in this version), and a capture bar at the foot. The header's hint always states the capture: `NOT CAPTURING` (`SIMULATED` in the simulator), `CAPTURING · ETH0`, `HOLD · ETH0`, `STOPPED · ETH0`, `CAPTURE ENDED`.

| View | What it shows | Bar |
| --- | --- | --- |
| Setup | READY or NO LINK; the interfaces from sysd (`system.status.network`; Ethernet with a carrier is selectable, Wi-Fi listed and disabled); capture options (stored bytes, stop after, capture-filter preset, promiscuous OFF); the privacy note | START CAPTURE |
| Capturing, hold | CAPTURING or HOLD chip, interface, link speed, stored-bytes mode, elapsed; packets per second, throughput; packets, bytes, dropped, buffer fill; stored, held, shed; a banner for kernel drops, shedding, or what HOLD is counting | FILTER (later), HOLD or RESUME, STOP |
| Stopped | STOPPED and why (BY YOU, TIME LIMIT, LINK LOST) | SAVE (later), NEW CAPTURE, DISCARD |
| Helper stopped | The error panel: what happened and its signal, that the interface was released, that the packets went with the helper and nothing was written; the last session's figures | NEW CAPTURE |

FULL, no time limit, DISCARD, NEW CAPTURE with packets, and Back with packets in memory each ask first, the safe choice accented. The screen stays on while PacketShark is in front (`awake_apps`, ADR-016 rule 14), so a capture never runs behind a dark panel.

Screenshots of every state, both orientations: `PACKETSHARK_SHOTS=<dir>` on `packetshark_app_test`.

## 8. Building and running

```sh
# host: helper and every PacketShark suite
make tools/capture/pos-capture packetshark-test
make packetshark-san-test                  # ASan + UBSan, fuzz 60 s
# host build with a live source: point it at libpcap headers and a library
make PACKETSHARK_PCAP=1 PACKETSHARK_PCAP_CFLAGS=-I<dir with pcap/pcap.h> \
     PACKETSHARK_PCAP_LIBS=<path to libpcap.so> tools/capture/pos-capture
# simulator: captures synthetic traffic by default
cmake -S ui/shell -B <build> -DLVGL_DIR=<lvgl> -DPOCKETOS_DISPLAY=sdl && cmake --build <build>
SHELL_BIN=<build>/pocketos-shell bash tests/packetshark_shell_test.sh
```

The image builds `pos-capture` with libpcap (`PACKETSHARK_PCAP=1` in `platforms/k230/package/pocketos/pocketos.mk`; the package selects `BR2_PACKAGE_LIBPCAP`, the same 1.10.5 the toolbox's tcpdump already put in the image). `-DPOCKETOS_WITH_PACKETSHARK=OFF` builds the shell without the app.

Overrides: `POCKETOS_CAPTURE_HELPER` (the helper's path), `POCKETOS_CAPTURE_SOURCE` (`synth:PPS[:SEED]`, `file:PATH`, or empty for the chosen interface). Fixtures: `tests/ppc_mkfixtures <dir>` writes the decoder fixtures (documentation addresses and example.net names only); their expected rows are `tests/data/packetshark/*.expected`. No real capture is ever committed.

## 9. Validation

### 9.1 Host (build host, x86-64, WSL Ubuntu 22.04, GCC 11.4)

| Suite | What it covers | Result |
| --- | --- | --- |
| `ppc_decode_test` | each protocol; QinQ and extension headers; fragments; HEADERS keeps no HTTP payload and the whole DHCP message; every cut of four frames on the heap; DNS compression, loop, forward pointer, label limit; a million frames with no allocation | pass |
| `ppc_pcapfile_test` | write and read back; byte-swapped microsecond files; damaged files refused | pass |
| `ppc_store_test` | seq never reused; both rings evict independently; a pin whole after 200,000 packets (F7); allocation failure | pass |
| `ppc_stats_test` | seen = stored + held + shed (T1); bins; a 56-year clock step and one back leave relative time continuous (T3) | pass |
| `ppc_proto_test` | every command and event round-trips; damaged lines refused; longest lines fit | pass |
| `ppc_fuzz` | decode + store rule + format + DNS names on mutated fixtures (no libFuzzer on this host: its own mutation driver); 60 s under ASan and UBSan in `packetshark-san-test` | no finding |
| `pos_capture_test.sh` | every fixture against its expected rows and against the host's tcpdump; a session through a pipe; busy lock; SIGTERM; clock step; time limit; damaged replay; S4, S5, S7; the bench | pass |
| `psk_model_test`, `psk_session_test` | the model; the client against the real helper: crash, hang, no hello, missing helper, protocol damage, abandon under 500 ms, twenty cycles with no child or descriptor left | pass |
| `packetshark_app_test`, `packetshark_shell_test.sh` | every M3 state in both orientations; targets and safe area; confirmations; crash and `kill -9` shown within a second; twenty opens and closes leave no helper, timer or object | pass |
| `packetshark_lint.sh` | receive only; one libpcap include; the hot path allocates and formats nothing; the shell links only the protocol; one file created (the lock); no log line from a packet | pass |

Host bench (`pos-capture bench`, x86-64, not a device figure): decode 32 ns/frame, decode + store 95 ns, one row of text 443 ns. Device figures: §9.2.

### 9.2 Unit A (2026-11-16)

Doors 0.4.0 (`21baeef0`) on Unit A, eth0 up at 100 Mb/s on the owner's LAN. Built from `6755ab9a` against the v0.4.0 SDK sysroot and run from a non-persistent kit in `/tmp` (the kit's shell was built with the simulator's test hooks, so `shell.tap` could press its buttons from the console; the image build has none). The installed system was restored and checked byte for byte afterwards. The full record is outside the repository (`K230-artifacts/packetshark-m0-m3/REPORT.md`); no capture and no address from it is in the repository.

| Check | Result | Class |
| --- | --- | --- |
| Real capture on eth0, 30 s | 91 frames (ARP including the unit's own replies, switch RRCP, NDP router advertisements, SSDP, UDP), 0 kernel and 0 interface drops; protocol words and µs gaps as expected | VERIFIED |
| Capabilities while capturing (S9) | `CapInh`, `CapPrm`, `CapEff`, `CapBnd`, `CapAmb` all 0; `NoNewPrivs: 1` | VERIFIED |
| Priority (D5) | nice 5 | VERIFIED |
| Interface flags (S2) | `0x1003` before, during and after: never promiscuous | VERIFIED |
| Capture socket (S1) | present only between START and STOP or the helper's end (`/proc/net/packet`) | VERIFIED |
| `kill -9` of the helper (S3) | socket released, flags unchanged; the app shows the error panel and stays usable | VERIFIED |
| Lock, down interface, missing interface | `busy`, `down`, `nodev` | VERIFIED |
| Capture-filter presets on eth0 | `ip`, `names`, `noself` start and stop cleanly | VERIFIED |
| Memory with both rings full (P3) | 315,000 synthetic frames at ~19,860 pps, FULL: peak RSS 23.2 MB (budget 32 MiB), 0 drops | VERIFIED |
| Bench, C908 | decode 158 ns/frame; decode + store 688 ns; one row of text 4.7 µs (libpcap dispatch not included) | VERIFIED |
| App: open, START, HOLD, RESUME, STOP, Back → LEAVE | header hint follows each state; opening captures nothing; LEAVE ends the helper | VERIFIED |
| Screen held awake | while PacketShark is in front only | VERIFIED |
| Ten open → START → home cycles | no helper left, shell descriptors unchanged (14), shell RSS +128 kB | VERIFIED |
| CPU, quiet LAN, LIVE showing | shell 1.3 % of the hart, helper 0.05 % | VERIFIED |
| Other services | sysd, netd, radiod, meshcored unaffected (same PIDs) | VERIFIED |
| Logs | lifecycle lines only; no address | VERIFIED |
| Carrier lost while capturing | not tested (needs the cable pulled) | — |
| Line rate (P1, P2) | not tested (M10, needs an authorised isolated traffic source) | — |

Two labels were corrected after this run (`a0d26736`: "KILLED (SIG 9)" instead of "CRASHED (SIG 9)" for SIGKILL, "MBIT/S" for the link speed); they are host-tested and were not redeployed.

## 10. Known limits and open items

- **Owner decisions pending:** ADR-016 (the helper as an exception to ADR-002 point 2; the screen kept on; promiscuous mode), DS §59 (protocol hues, 56 and 36 px rows, launcher place and icon), and the libpcap dependency declaration (proposed in the pull request).
- No SAVE: a capture cannot be kept. A helper crash, leaving the app, or the time limit with the app closed loses it (D10, accepted; said on screen).
- Promiscuous mode not offered (§5).
- Wi-Fi listed and not offered (D18): managed-mode capture is untested.
- Landscape uses the portrait column; the wide layout is M9.
- The relative-time origin is corrected for clock steps; frames still in the kernel ring across a step can read time 0 (an edge, not measured).
- GRO-merged records (r8152) are labelled `[GRO]`; whether they occur on the unit is measured in M10.
- No seccomp; no dedicated user (ADR-016).
- Signals (M7) and the display filter (M5) are not implemented; their protocol fields are zeros.
