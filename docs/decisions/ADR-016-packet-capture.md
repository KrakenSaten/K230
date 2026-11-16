# ADR-016: Packet capture: ownership, privilege and privacy

Status: **Accepted** by the product owner, 2026-11-16, **subject to the security constraints documented in this ADR.** The rules under "Decision" (1 to 18) and the risks accepted under "Consequences" are conditions of the acceptance, not aims: PacketShark may capture only while it keeps them. In particular promiscuous mode stays off and is not offered until the kernel's reversion of it is verified on the hardware (rule 3), and the helper drops every capability and sets `no_new_privs` before it parses a frame (rule 17). Changing any of them is a change to this ADR and needs the owner again. The record of decisions is at the end. Implemented for milestones M0 to M3 on branch `feature/packetshark` (draft PR #136, not merged; docs/apps/PACKETSHARK.md); rules 9, 10 and 16 (SAVE, the saved captures, the save sheet's warning) are implemented with M8 on `claude/packetshark-m5-m6` (draft PR #139, not merged).
Date: 2026-11-15 (drafted with the design package), 2026-11-16 (added to the repository with M0 to M3; accepted)
Deciders: product owner (final), AI engineering partner (author)

## Context

PacketShark (`docs/apps/PACKETSHARK.md`) is a packet analyzer for Doors. It needs to read frames from a network interface, which is a raw socket, which today means root. What it reads is other people's data: addresses and device names always, and message contents if payload is kept.

Constraints already binding:

- ADR-002 point 2: services own hardware; apps never touch it. `netd` owns Wi-Fi configuration; nothing owns Ethernet beyond the image's `udhcpc`.
- ADR-004 and ADR-010 accepted per-operation helper processes as narrow exceptions for audio, each time on the grounds that the work happens only while the app's own screen is open, and each time saying a later need is decided against ADR-002, not by extension.
- All Doors processes run as root in v0; per-service users are a follow-up (`docs/ARCHITECTURE.md`).
- There is no app manifest and no permission model (ADR-002, "useful soon").
- The shell is one LVGL loop; nothing on it may block, and a fault in it takes the panel.
- The image has no `setcap` and no xattr tooling; sshd accepts root with an empty password (`docs/hardware/T-DISPLAY-K230.md`).
- The Recorder set the privacy precedent for capturing something sensitive: explicit start, a visible indicator, files 0600 in a 0700 folder, no network, nothing logged (`docs/apps/RECORDER.md`, "Privacy").

What PacketShark needs: a capture socket on one interface for as long as a capture runs (minutes), only while its screen is open, receive-only; a decoder for untrusted bytes; a bounded store; a file only when asked.

## Options

### A. `captured`, a supervised service (ADR-002 as written)

- Pro: one owner of capture system-wide; a pocketipc API other apps could use; the place for background capture later.
- Con: a permanently running process able to capture is exactly what a conservative privacy model should not have. Packet rows through a JSON contract designed for small messages. The largest change. Nothing but PacketShark wants the API.

### B. Capture inside the shell

- Pro: no IPC, no second process.
- Con: a parser of hostile input and a raw socket in the process that owns the display; a decoder fault or a slow write freezes or kills the panel. Against ADR-002 in letter and intent.

### C. Run `tcpdump` and parse its text

- Pro: no new capture code; tcpdump is on the image.
- Con: text parsing of a tool's human output as an interface; no bounded store, no field tree, no statistics; no control over what is kept; tcpdump running as root with no privilege drop.

### D. A per-session helper, `pos-capture`, started by the app (recommended)

- One process per capture, started by PacketShark over a socketpair with the Zabbix helper's spawn shape (`PR_SET_PDEATHSIG`, inherited descriptors closed), gone when the app is left.
- It holds the socket, the decoder, the packets and the file writer. The shell receives formatted text for what is on screen.
- It opens the capture as root, then drops every capability and sets `no_new_privs` before it parses a single frame.
- Pro: ADR-004's proven shape with no new mechanism; no capture-capable process exists unless a person pressed START and is looking at the app; a decoder fault takes the helper, and the shell says so; the kernel closes the socket and reverts promiscuous mode when the process dies for any reason.
- Con: not the letter of ADR-002; a fifth small line protocol; still uid 0, though without capabilities.

## Decision

**Option D.** Packet capture is owned, per session, by `pos-capture`, as a narrow exception to ADR-002 point 2 of the same shape as ADR-004 and ADR-010. Scope: a receive-only capture socket on one interface, for PacketShark, only while its screen is open.

The rules that go with it:

**Capture**

1. Nothing is captured without an explicit START on the app's own screen. Nothing starts a capture on boot, on a timer, over IPC or from another app.
2. Receive only. No code path transmits, injects, replays or alters traffic, and a lint holds that (`tests/packetshark_lint.sh`). No ARP spoofing, no redirection, no man-in-the-middle feature, now or as an option.
3. Promiscuous mode is off by default, needs a confirmation that says what it does, is set by socket membership so the kernel reverts it, and is never remembered. **Until the kernel's reversion is verified on the hardware it is not offered at all**: the M3 app shows it OFF and takes no tap on it; the helper accepts the flag for the later milestone.
4. One capture at a time: an exclusive lock in the runtime directory.
5. A capture ends when the app is left. There is no background capture in this decision; that would be a new ADR, with a system-wide indicator.
6. A capture stops by itself after 15 minutes unless the person chose otherwise for that session.

**What is kept**

7. Default: headers, and the whole message only for network plumbing (ARP, ICMP, DNS, DHCP, NTP and their IPv6 and multicast relatives). Full packets only by a confirmed choice, per session, never remembered. *(HEADERS accepted by the owner, 2026-11-15; DESIGN_DECISIONS D7.)*
8. Packets are held in the helper's memory, bounded (32 MiB), and are gone when the app is left. *(Memory only until SAVE and the helper as the owner of the packets accepted by the owner, 2026-11-15; D1, D10.)*
9. A file is written only by SAVE (a later milestone; M3 has no SAVE and writes no capture file), to `/root/Captures`, 0600 in a 0700 folder, never anywhere else and never to `/tmp`.
10. The app lists saved captures with their size and age and deletes them on request, with a confirmation.
11. Nothing is uploaded or sent. The app and its helper have no network client code.
12. Neither process logs packet content, addresses or names. The helper logs lifecycle lines only.

**What is shown**

13. The app's header states the capture state on every one of its screens, and a state chip and the HOLD and STOP buttons are on every tab.
14. The screen stays on while the app is in front, so a capture cannot continue behind a dark panel.
15. The setup screen states, before START, where packets are kept and what the stored-bytes mode keeps.
16. The save sheet states that files in `/root` are readable over SSH on this image until a device password exists.

**Privilege**

17. The helper drops all capabilities and sets `no_new_privs` after the capture is open. A new capture is a new process. The helper runs at nice 5 *(accepted by the owner, 2026-11-15; D5)*.
18. All packet bytes are treated as hostile: the decoder is bounds-checked, allocation-free, fuzzed, and never runs in the shell.

Revisit when: background capture is wanted; a second app wants packets; Doors gets per-service users (then the helper gets its own); `CONFIG_SECCOMP` is confirmed on the kernel (then a syscall allowlist).

## Consequences

Needed now: `core/pocketpcap`, `tools/capture` (`pos-capture`), `apps/packetshark`, their tests and lint, the launcher entry, one line in the shell's keep-awake table, and an explicit libpcap dependency in the `pocketos` Buildroot package (a dependency declaration, approved by the owner on 2026-11-16; libpcap is already in the image through the toolbox).

Useful soon: the Unit A gate (`docs/hardware/PACKETSHARK_GATE.md`); a seccomp allowlist; a shared helper-process client, which five apps would now use; a device password, which this app makes more pressing.

Future: a capture indicator derived from the kernel (`/proc/net/packet`), as `hw_activity.h` derives the microphone and camera LEDs, if capture ever outlives the app's screen; an app manifest with a `capture` permission.

Risks:

- The helper is uid 0 without capabilities, not an unprivileged user. It can still write files root owns. A decoder exploit could therefore modify files, though not open sockets, change interfaces or load code into the kernel. Accepted for v0.1 because the alternative needs platform work this ADR should not smuggle in.
- Saved captures are readable by anyone who can log in as root over the network. Stated on screen; not solvable here.
- A helper crash loses the unsaved session. Intended: the alternative is writing every capture to storage.
- DNS names and DHCP host names are kept by default. They are what the tool is for; the setup screen says so.

Migration cost to a `captured` service later: an IPC front for the helper's commands; `core/pocketpcap` moves unchanged.

## Evidence

Classes as in `AGENTS.md`. The hardware results of M0 to M3 are in `docs/apps/PACKETSHARK.md` ("Validation"), which is updated with each milestone; this list says where each claim stands.

- ADR-002, ADR-004 and ADR-010 wording; the Recorder's privacy rules; all processes root; no `setcap`: DOCUMENTED (repository) and VERIFIED on Unit A (feasibility report, 2026-11-15, §2.1, §6.2).
- Receive-only capture on `eth0` without promiscuous mode, the interface flags unchanged, the socket released on exit and on `kill -9`: VERIFIED on Unit A for the feasibility proof of concept and for `pos-capture` (2026-11-16, docs/apps/PACKETSHARK.md §9.2).
- libpcap 1.10.5 with TPACKET_V3 present on the v0.4.0 image: VERIFIED on Unit A (feasibility report §2.1).
- Promiscuous mode by `PACKET_MR_PROMISC` reverting when the socket closes: DOCUMENTED (Linux packet(7)); not exercised on the unit, which is why rule 3 keeps it off.
- Dropping all capabilities with `capset`, the bounding set and `no_new_privs`, and reading the open socket afterwards: VERIFIED on Unit A (2026-11-16): `CapInh`, `CapPrm`, `CapEff`, `CapBnd`, `CapAmb` all 0, `NoNewPrivs: 1`, and the capture went on reading frames (docs/apps/PACKETSHARK.md §9.2).
- `CONFIG_SECCOMP` on the v0.4.0 kernel: not checked.
- Helper lifecycle (spawn, watchdog, abandon, crash and kill reported): VERIFIED on the host for `pos-capture` (tests/psk_session_test.c, tests/packetshark_app_test.c) and on Unit A (open, START, STOP, LEAVE, `kill -9`, ten cycles with no helper left).
- The screen held awake while PacketShark is in front, and only then: VERIFIED on Unit A (`shell.info` power.held).
- Lint for receive-only, one libpcap include, a capture file written only by SAVE (new, 0600, never followed), saved captures never opened by the shell, no log line from a packet: `tests/packetshark_lint.sh`.
- SAVE to a 0700 folder as a 0600 file, refused under `/tmp` and while capturing, no part file left, the file read back by tcpdump: VERIFIED on the host (tests/pos_capture_test.sh, tests/psk_session_test.c, tests/packetshark_app_test.c). On the unit, as uid 0 without capabilities writing to `/root/Captures`: ASSUMED (the owner of a directory needs no capability to write in it); not yet run.

## Decision record

| Date | Decision | By |
| --- | --- | --- |
| 2026-11-15 | Accepted with the design package: the helper owns the packets and the shell keeps at most 48 rows (D1); the helper runs at nice 5 (D5); HEADERS is the default for what is stored (D7); captures stay in memory until SAVE (D10) | Product owner |
| 2026-11-16 | **ADR-016 approved, subject to the documented security constraints** (rules 1 to 18 and the accepted risks; see Status) | Product owner |
| 2026-11-16 | **The explicit libpcap dependency of the `pocketos` package approved** (`select BR2_PACKAGE_LIBPCAP`, `libpcap` in `POCKETOS_DEPENDENCIES`, `PACKETSHARK_PCAP=1`) | Product owner |
| 2026-11-16 | DS §59 (protocol colours, dense rows, launcher place and icon): first deferred until the owner had reviewed the mockups | Product owner |
| 2026-11-16 | **DS §59 accepted** as PacketShark's visual baseline after review of all eight mockups: protocol colours, 56 px packet rows, 36 px field rows, navigation, launcher place and the current icon concept. Minor adjustments allowed for readability, touch accuracy or LVGL rendering on Unit A; meaningful deviations recorded in DS §59.11 | Product owner |
