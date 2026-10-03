# RIFT colour emoji - hardware gate (RUN on unit B 2026-10-03: PASS)

**Build on the unit when the gate finished:** doors-shell `5394acb` (riscv64
DRM build against the vendor `liblvgl` 9.5.0 in the sysroot, md5
`321b58ce52e5b7d68c879e01bee0c503`, 4,030,024 bytes stripped), on the v0.3.0
image (`2956eff`); meshcored and radiod as found. **Before the gate:**
doors-shell `582328c` (md5 `80f8de6daca70a242007396dc69c1e76`). The unit is
**left on `5394acb`**; rollback `/root/rollback-rift-emoji/RESTORE.sh` puts
back `582328c` and the `settings.conf` the gate found.

Branch `feat/rift-colour-emoji` (PR #37). Unit B (`K230-B`), Wi-Fi, text
size large, rotation automatic. Unit A (`Mstr_k230`, shell `9eda04e`) sent
the messages. Kit and captures: `C:\K230\out\rift-emoji-gate` (not in the
repository): `deploy.sh`, `send.sh`, `verify.sh`, `step.sh`, `caps/`.

## What changed on the unit

| Binary | Why |
| --- | --- |
| `/usr/bin/doors-shell` | RIFT draws Noto Color Emoji in message bodies, previews and claimed senders (docs/apps/RIFT.md, "Colour emoji") |

Nothing else. `/usr/share/doors/THIRD_PARTY_NOTICES.txt` on the unit is the
image's and does not carry the new entries; an image build does.

## Transmission

Ten **direct** messages from unit A to unit B at the configured 2 dBm,
authorised by the owner for this gate; no channel message, no advert. All
ten took the `direct` route (a path to B was known), so no repeater flood.
RIFT's composer on unit B was never used.

## Steps and results

| # | Step | Result |
| --- | --- | --- |
| 1 | Deploy the shell with a rollback; it starts on the home screen | PASS: build `5394acb`, md5 verified, landscape 1232x568 |
| 2 | Unit A sends the ten sample lines to K230-B (`sends/01..10.json`, 14 to 79 bytes) | PASS: 10 of 10 `acked` on unit A |
| 3 | The texts in unit B's meshcored are byte for byte what was sent - selectors, joiners, tags and skin tones included | PASS: 10 of 10 identical (stored and transmitted text unchanged) |
| 4 | Landscape, large: the thread draws every emoji in colour, composed: faces, 👍, ❤ and ❤️‍🔥, the family as one image, 🇳🇴 🇸🇪, 1️⃣, 🔥 🚀 🎉 📡 📷 🎵, 🏴󠁧󠁢󠁳󠁣󠁴󠁿 🏳️‍🌈 ✅ ❌ ⚠️; Plex text unchanged around them | PASS (`g05-gate-thread-large-land`) |
| 5 | Skin tones stripped: "Tone 👍🏻 👍🏿 👋🏽" shows 👍 👍 👋 | PASS |
| 6 | Messages sent by another node during the gate (`Mstr_m5`, emoji and 🇳🇴) draw in colour too | PASS (`g04-thread-large-land`) |
| 7 | Portrait, large: previews in the conversation list draw emoji in colour (Mono caption face) | PASS (`g08-portrait-comms-large`) |
| 8 | Portrait, large: the long line wraps with its emoji inline | PASS (`g09-portrait-thread-large`) |
| 9 | Text size changed live to small: thread and previews follow at once, emoji stay in colour | PASS (`g10-portrait-thread-small`) |
| 10 | No LVGL warning, no shell warning or error, no crash file since the swap | PASS: `shell.stdio.log` empty, no warnings in `shell.log` |
| 11 | Text size and rotation put back; `settings.conf` identical to what was found | PASS: md5 `c8ae097c…` both |

Resident memory of the shell after the gate: VmRSS 17,904 kB (anon 8,304,
file 9,600); CPU 3 %, load 0.11. No baseline was taken before the swap, so
this is a figure, not a delta.

## Not covered

- Medium text size on the unit (host-tested in `tests/rift_emoji_ui_test.c`).
- Outdoor and Night modes: the images keep their colours (by design, open
  for the owner).
- A full image flash: only the shell binary changed, so the hot swap covers
  the feature; the notices file reaches the unit with the next image.
