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
- It opens the capture as root, then drops every capability and sets `no_new_privs` before it parses a single frame. *(Since amendment 2, 2026-11-28, it also leaves uid 0 for an unprivileged identity of its own before that frame; rule 17.)*
- Pro: ADR-004's proven shape with no new mechanism; no capture-capable process exists unless a person pressed START and is looking at the app; a decoder fault takes the helper, and the shell says so; the kernel closes the socket and reverts promiscuous mode when the process dies for any reason.
- Con: not the letter of ADR-002; a fifth small line protocol; still uid 0, though without capabilities *(until amendment 2: an unprivileged identity since 2026-11-28)*.

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
9. A file is written only by SAVE (a later milestone; M3 has no SAVE and writes no capture file), to `/root/Captures`, 0600 in a 0700 folder, never anywhere else and never to `/tmp`. *(Amendment 2, 2026-11-28: the folder, never a file, is prepared when a capture starts, while the helper may still reach it: made 0700 if missing, owned by the capture identity of rule 17, refused if it is a link or belongs to anyone else. SAVE writes into it only through the descriptor opened then. A folder that already exists is handed over only if it is the identity's, or a root-owned folder named `Captures` in a parent writable by root alone; any other folder a path names is refused and left as it is. The files already in it keep their owner. The identity may place other entries in its folder; the app lists only single-name regular files with the helper's names.)*
10. The app lists saved captures with their size and age and deletes them on request, with a confirmation.
11. Nothing is uploaded or sent. The app and its helper have no network client code.
12. Neither process logs packet content, addresses or names. The helper logs lifecycle lines only.

**What is shown**

13. The app's header states the capture state on every one of its screens, and a state chip and the HOLD and STOP buttons are on every tab.
14. The screen stays on while the app is in front, so a capture cannot continue behind a dark panel.
15. The setup screen states, before START, where packets are kept and what the stored-bytes mode keeps.
16. The save sheet states that files in `/root` are readable over SSH on this image until a device password exists.

**Privilege**

17. The helper drops all capabilities and sets `no_new_privs` after the capture is open. A new capture is a new process. The helper runs at nice 5 *(accepted by the owner, 2026-11-15; D5)*. **Amendment 2 (approved by the owner on 2026-11-28): least privilege before untrusted data.** After its capture socket, its lock, its log and its captures folder are open, and before it reads a frame, the helper leaves uid 0 for its own unprivileged service identity, `pos-capture` (uid and gid 65532, defined in the image, used numerically), with no supplementary groups. It gives itself a network namespace of its own: the capture socket it already holds keeps working, and any socket made later reaches no network and no abstract local socket. It empties every capability set, the bounding set included, and sets `no_new_privs`. It reads all of it back from `/proc/self/status` and checks that uid 0 cannot be taken back. If any step fails or does not read back as expected, the capture is refused (`error privilege`), no frame is processed, and the helper ends at once, because a failed step may have left it partly privileged: it fails closed. The identity change clears the parent-death signal, so the helper sets it again and checks that its parent is still the shell. A session closes every inherited descriptor before it opens its own. The log keeps the one descriptor it opened before the drop. **Amendment 3 (approved by the owner on 2026-11-28): a system call allowlist.** The drop's last step, after the parent-death signal is set again and checked, installs a seccomp filter for the rest of the helper's life (`capture_confine()`):
    - It allows only what the helper does from then on: reading frames and commands, writing its lines, its log and its saves, SAVE's calls through the folder's descriptor, the capture socket's counters, waiting, time, memory, signals and the end.
    - The capture socket is **receive-only**: `write`, `writev` and `sendto` on it are refused, as is `sendto` to any address. No socket is made, bound or connected; no socket option is set, so no promiscuous membership, filter or transmit ring; `sendmsg` and `recvmsg` are refused. No descriptor is copied (`dup`, `dup3`, `fcntl` `F_DUPFD`), so the socket keeps its one number. After STOP that number stays taken by `/dev/null`, so no later file gets it. No `ioctl` is made, and no executable memory is mapped.
    - Any other system call, or a call through another ABI, ends the helper at once (`SECCOMP_RET_KILL_PROCESS`, which the kernel's audit logs).
    - The filter is installed with `SECCOMP_FILTER_FLAG_TSYNC`, then proved: `/proc/self/status` must show `Seccomp: 2`, and `socket()` must be refused. A capture socket on a standard descriptor is refused. Any failure refuses the capture as in amendment 2.
    - The system call names are the kernel's for both architectures the helper runs on. On riscv64 the C library uses `ppoll`, `renameat2`, `newfstatat` and `clock_nanosleep` where x86-64 also has `poll`, `renameat` and `nanosleep`. The numbers were checked against the target's glibc.

    The image also sets `fs.protected_hardlinks = 1` and `fs.protected_symlinks = 1` (`/etc/sysctl.d/60-doors-links.conf`, applied at boot by `S02sysctl`).
18. All packet bytes are treated as hostile: the decoder is bounds-checked, allocation-free, fuzzed, and never runs in the shell.

Revisit when: background capture is wanted; a second app wants packets. *(Done by amendment 2: the helper has its own identity.)* `CONFIG_SECCOMP_FILTER` is set in the image's kernel build (VERIFIED in the build configuration, 2026-11-27). *(Done by amendment 3: a system call allowlist.)*

## Consequences

Needed now: `core/pocketpcap`, `tools/capture` (`pos-capture`), `apps/packetshark`, their tests and lint, the launcher entry, one line in the shell's keep-awake table, and an explicit libpcap dependency in the `pocketos` Buildroot package (a dependency declaration, approved by the owner on 2026-11-16; libpcap is already in the image through the toolbox).

Useful soon: the Unit A gate (`docs/hardware/PACKETSHARK_GATE.md`); a shared helper-process client, which five apps would now use; a device password, which this app makes more pressing.

Future: a capture indicator derived from the kernel (`/proc/net/packet`), as `hw_activity.h` derives the microphone and camera LEDs, if capture ever outlives the app's screen; an app manifest with a `capture` permission.

Risks:

- *(Until amendment 2.)* The helper was uid 0 without capabilities, not an unprivileged user. An owner's file permissions need no capability, so it could still open, by permission alone, files and device nodes that root owns. A decoder exploit was therefore less contained than this line first said: "though not open sockets, change interfaces or load code into the kernel" overstated it. Accepted for v0.1; corrected on 2026-11-28.
- *(Amendment 2 alone, before amendment 3.)* The helper runs as `pos-capture`. A decoder exploit holds that identity:
  - **What it can still do:**
    - Use the capture socket. That socket can be bound to another interface, made to send frames and set promiscuous, because nothing but the helper's own code keeps rules 2, 3 and 11 on a socket it already holds.
    - Use the other descriptors the helper holds: the lock, the log (which it could grow or truncate) and the captures folder.
    - Read what any user may read.
    - Write, replace or delete the captures in its folder, and place links or other entries there.
    - Make ordinary sockets, but only in its own network namespace, where they reach no network.
  - **What it can no longer do:** open root's files, device nodes or the system's 0660 local sockets; write anywhere else; reach the network or another process through a new socket; or become root again.

  Kernel bugs stay reachable through system calls.
- *(Since amendment 3, 2026-11-28.)* A decoder exploit holds `pos-capture`, the descriptors already open and the filter's few calls:
  - **What it can still do:**
    - Receive on the capture socket and read its counters.
    - Write lines to the shell, which reads them as from any helper, and write to its log (grow it, or write false lines). It cannot truncate the log.
    - Create, write, rename and delete regular files in its captures folder. It cannot make a link, a symbolic link or a folder there.
    - Read what any user may read.
    - Reach the kernel through the allowed calls only.
  - **What it can no longer do:**
    - Send a frame, bind the socket elsewhere, set it promiscuous or change its filter. Rules 2, 3 and 11 now hold against a compromised helper, not only by its own code.
    - Make any socket or copy a descriptor.
    - Run a program, map executable memory, trace or signal another process, or change its own process settings.
    - Become root again.
  - Rules 2 and 3 are still kept first by the helper's own code: libpcap opens the socket receive-only and not promiscuous, before the filter exists.
- *(Since amendment 2.)* The identity owns a folder that root reads. The app is safe: it lists with `fstatat(AT_SYMLINK_NOFOLLOW)`, single-name regular files only, and deletes with `unlinkat`. *(Since amendment 3.)* The filter refuses `linkat` and `symlinkat` to the helper. The image sets `fs.protected_hardlinks` and `fs.protected_symlinks` to 1, so no less privileged process can give root a second name of a file it cannot reach, or a link in `/tmp` to follow.
- `pos-capture decode FILE`, a developer command, parses a file as whoever runs it and does not drop privileges. Nothing in the product calls it. Do not run it as root on a file from the captures folder.
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
- `CONFIG_SECCOMP` and `CONFIG_SECCOMP_FILTER`: set in the `k230_pocketos_defconfig` kernel build (VERIFIED in the build configuration, 2026-11-27); no filter is installed yet.
- The identity change of amendment 2, VERIFIED on the host as root (WSL Ubuntu 22.04: `tests/capture_priv_test.c`, `tests/pos_capture_priv_test.sh`, and a live capture on the host's `eth0` with a libpcap build):
  - uid and gid 65532 in every slot, no groups;
  - every capability set empty, the bounding set included; `no_new_privs`; not dumpable; uid 0 refused afterwards;
  - its own network namespace: an ordinary socket reaches no network;
  - root's files and a root-owned 0660 device node refused to the identity;
  - no inherited descriptor kept, including one above 1023;
  - SAVE through the prepared folder; a root-owned folder not named `Captures` refused and left as it was;
  - the parent-death signal set again;
  - a failed reduction refusing the capture, processing no frame and ending the helper;
  - on a real AF_PACKET socket: frames read after the change, promiscuous mode never set, the capture saved.

  Not yet run on a unit. `CONFIG_NET_NS` is set in the image's kernel build (VERIFIED in the build configuration).
- An independent security review of amendment 2 (28 November 2026) found no P0. Its findings were addressed: the risk wording corrected; the network namespace, the folder hand-over rule, every inherited descriptor closed and the helper ending after a failed reduction added; links and second names kept out of the app's list. Its two recommendations, the image's `fs.protected_*` settings and a seccomp filter, are amendment 3.
- The system call filter of amendment 3, VERIFIED on the host (WSL Ubuntu 22.04, x86-64; `tests/capture_priv_test.c` and `tests/pos_capture_priv_test.sh`, as root and without):
  - it installs and reads back as `Seccomp: 2`;
  - as root on a real AF_PACKET socket: `write`, `writev`, `sendto` with or without an address, promiscuous membership, `SO_ATTACH_FILTER`, `SO_DETACH_FILTER`, `bind`, `connect` and `ioctl` are refused;
  - new sockets, socket pairs, `sendmsg`, `dup`, `dup3`, `fcntl` `F_DUPFD` and executable memory are refused;
  - `prctl`, `execve`, `ptrace`, `kill` and the x32 ABI end the process with SIGSYS;
  - after STOP a save never gets the socket's number;
  - every PacketShark helper test runs under it (`make packetshark-test`), and a live capture on `eth0` reads frames and its drop counters under it.

  The riscv64 build's call numbers match the target glibc 2.33 (VERIFIED by disassembly: `send` uses `sendto` 206, `recv` `recvfrom` 207, `poll` `ppoll` 73, `renameat` `renameat2` 276, `fstat` and `fstatat` `newfstatat` 79). Not yet run on a unit.
- Helper lifecycle (spawn, watchdog, abandon, crash and kill reported): VERIFIED on the host for `pos-capture` (tests/psk_session_test.c, tests/packetshark_app_test.c) and on Unit A (open, START, STOP, LEAVE, `kill -9`, ten cycles with no helper left).
- The screen held awake while PacketShark is in front, and only then: VERIFIED on Unit A (`shell.info` power.held).
- Lint for receive-only, one libpcap include, a capture file written only by SAVE (new, 0600, never followed), saved captures never opened by the shell, no log line from a packet: `tests/packetshark_lint.sh`.
- SAVE to a 0700 folder as a 0600 file, refused under `/tmp` and while capturing, no part file left, the file read back by tcpdump: VERIFIED on the host (tests/pos_capture_test.sh, tests/psk_session_test.c, tests/packetshark_app_test.c). On the unit, as uid 0 with every capability set empty writing to `/root/Captures`: VERIFIED on Unit B (2026-11-16, docs/apps/PACKETSHARK.md §9.5).

## Decision record

| Date | Decision | By |
| --- | --- | --- |
| 2026-11-28 | **Amendment 3 approved** (F1 completion): a seccomp allowlist installed as the drop's last step, the capture socket receive-only under it, fail closed (rule 17); `fs.protected_hardlinks` and `fs.protected_symlinks` set to 1 in the image. Every other rule stands | Product owner |
| 2026-11-28 | **Amendment 2 approved** (F1 hardening, mitigation 1): the helper runs under its own unprivileged service identity before it reads untrusted data, and fails closed (rule 17); the captures folder is prepared at START and owned by that identity (rule 9); the risk wording corrected. Every other rule stands | Product owner |
| 2026-11-22 | **Amendment 1 accepted** (`ADR-016-amendment-1-companion.md`): rules 1 and 11 reworded, rules 1a, 11a, 19, 20 and 21 added, for a paired companion; every other rule and accepted risk stands. Listed first here because it changes the rules below | Product owner |
| 2026-11-15 | Accepted with the design package: the helper owns the packets and the shell keeps at most 48 rows (D1); the helper runs at nice 5 (D5); HEADERS is the default for what is stored (D7); captures stay in memory until SAVE (D10) | Product owner |
| 2026-11-16 | **ADR-016 approved, subject to the documented security constraints** (rules 1 to 18 and the accepted risks; see Status) | Product owner |
| 2026-11-16 | **The explicit libpcap dependency of the `pocketos` package approved** (`select BR2_PACKAGE_LIBPCAP`, `libpcap` in `POCKETOS_DEPENDENCIES`, `PACKETSHARK_PCAP=1`) | Product owner |
| 2026-11-16 | DS §59 (protocol colours, dense rows, launcher place and icon): first deferred until the owner had reviewed the mockups | Product owner |
| 2026-11-16 | **DS §59 accepted** as PacketShark's visual baseline after review of all eight mockups: protocol colours, 56 px packet rows, 36 px field rows, navigation, launcher place and the current icon concept. Minor adjustments allowed for readability, touch accuracy or LVGL rendering on Unit A; meaningful deviations recorded in DS §59.11 | Product owner |
