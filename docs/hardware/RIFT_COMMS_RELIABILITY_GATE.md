# RIFT comms reliability gate (feat/rift-comms-reliability)

ACK and delivery state, RESEND, REPLY and CONTACTS on real hardware. Each
step lists what to do and what PASS looks like. Deploy both binaries from
the branch (`doors-shell` and `meshcored`): RESEND needs the service's
`mesh.send` `resend`, and an older meshcored refuses it.

## Done unattended, 2026-10-05

| | |
|---|---|
| unit B (.187) | **shell only** hot-swapped to the branch build (riscv64 `pocketos-shell` md5 `5b43a11a`), rollback `/root/rollback-rift-reliability/RESTORE.sh`; meshcored left as found (RX LOG build `2fcc0f9a`). Nothing transmitted. |
| CONTACTS | **PASS** in portrait: reached from COMMS' `CONTACTS ›`, COMMS lit; `293 OF 293` - the service's own count of stored contacts; A to Z; CHAT / REPEATER / ROOM shown per row with the key prefix and the age heard; RECENT empty with "No direct conversation held yet." (unit B holds no DM). Captures in the branch kit `out/rel/hw/caps/b-03-contacts.png`, `b-04-recent.png` (not committed). |
| restored | unit B back on shell `16522b79` (md5 checked), rollback directory kept. |
| unit A (.171) | did not answer ping; nothing done. |

Not done: anything that transmits. There was no second unit and no peer the
owner controls; a DM to a stranger's node on the public mesh is not a test.

## Still to run (needs the owner)

Two units, or one unit and a MeshCore companion the owner holds (T-Deck,
phone app), both on the branch build where they are Doors units. Clock set on
both. Text size Large, landscape, as unit B is used; repeat 2 and 5 in
portrait.

1. **Delivered.** Write to the peer from RIFT. PASS: the caption goes
   `SENT · FLOOD` (first message) and then `DELIVERED · ACK n s`; the next
   one goes `SENT · DIRECT` and then DELIVERED. Never DELIVERED before the
   peer has it.
2. **No ACK.** Peer powered off (or out of range). Send. PASS: `SENT · …`
   until the deadline (`ack_timeout_ms`, seconds), then `NO ACK` in the warn
   colour; the text stays in the thread.
3. **Late ACK.** As 2, but power the peer up right after NO ACK shows, within
   about a minute (MeshCore retransmits nothing by itself, so a late ACK
   needs the peer to have had the message: easiest with the peer out of range
   for the ACK's way back only - hard to stage; skip if it cannot be made to
   happen and say so). PASS: NO ACK turns to DELIVERED.
4. **RESEND.** Hold the NO ACK message: the bar shows `NO ACK · you: …`,
   COPY, RESEND, CLOSE (no REPLY). Peer on again. RESEND. PASS: the same row
   goes `SENT · TRY 2 · …` then `DELIVERED · TRY 2 · ACK n s`; no second row
   on the sender. On the peer, the text arrives (a Doors peer and the
   MeshCore app show it as a second copy of the first attempt if that one
   had arrived - upstream files both).
5. **By key.** Composer empty, LEFT: the newest message is selected with
   its actions; UP/DOWN move, LEFT/RIGHT pick, ENTER does it, ESC closes and
   the composer has the keys again.
6. **meshcored restart.** With a NO ACK message in the thread,
   `/etc/init.d/S65meshcored restart` on the sender. PASS: the message stays,
   `NO ACK · SERVICE RESTARTED`; RESEND sends it as a new message, which
   replaces the row and is delivered.
6b. **Channel send: transmitted and heard back.** Needs no peer, only a
   repeater in range (unit B hears several). Send on Public. PASS: the
   caption goes `SENT · FLOOD · NO ACK ON CHANNELS`, then within seconds
   `TRANSMITTED · NOT HEARD BACK`, and - when a repeater relays it -
   `HEARD BACK ×N · M HOP(S)`, N rising as more copies arrive; `doors call
   meshcored mesh.messages` shows `transmitted: true`, `heard_back`,
   `heard_back_hops` on that message, and its state stays `sent_flood`. It
   never says DELIVERED. With the antenna removed or the radio switched off
   from SYSTEM after accepting, `NOT TRANSMITTED` may show instead (warn).
   This sends one line on the public channel: the owner's call.
7. **Reply.** On a channel both nodes hold, hold the peer's line: REPLY is
   offered, RESEND is not. REPLY puts `@[Name] "…" ` into the composer;
   finish it with an emoji and SEND. PASS: RIFT on both units draws
   `↳ Name?: …` above the answer, the emoji intact; on a non-Doors client
   (MeshCore app, T-Deck) the line arrives as plain readable text.
8. **COPY.** Hold any message, COPY: its words are in the composer, nothing
   sent.
9. **CONTACTS by key and search.** `C` on the COMMS list opens CONTACTS;
   type part of a name, then a 4-hex key prefix: the list narrows; Enter on
   a repeater says why it takes no messages; Enter on a chat node opens its
   conversation. RECENT lists the peer of step 1 first.

## Restore

`/root/rollback-rift-reliability/RESTORE.sh` puts back the shell found on
the unit when the rollback was made. A gate that also swaps meshcored must
make its own rollback of `/usr/sbin/meshcored` first, as the RX LOG gate's
`/root/rollback-rift-rx-log/RESTORE.sh` did.
