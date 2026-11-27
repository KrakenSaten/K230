# G0 held-out validation: the window interface

The independent generator ([G0.md](../../../docs/apps/packetshark/neural/G0.md) §4, step 2) produces windows in this form. A separate runner (`heldout.py`) judges them with the frozen rules and scores G0. The generator's author works from G0.md §1–2 and this page alone, and does not read the engine (`core/pocketpcap/ppc_intel.c`), E3a-R (`research/packetshark-neural/e3a-r/`) or the N1.1 record's results.

Everything below is what PacketShark's capture helper can observe of one comparison: the **base** minute (seconds 0–59), the **window** minute (seconds 60–119), and 5 seconds after it (120–124), so that the last questions' 5-second wait is covered. `T = 125`.

## Python API

```python
def generate(seed: int, n_windows: int) -> list[dict]:
    """Deterministic for a given seed."""
```

## One window (`dict`)

| Key | Type | Meaning |
| --- | --- | --- |
| `watched` | bool[125] | The capture was analysing frames in that second. False: on HOLD, so frames passed unseen and uncounted. |
| `frames` | int[125] | Frames the capture saw (analysed) in that second, of every kind. 0 when not watched. |
| `own_in` | int[125] | Of those, unicast frames addressed to this device (all kinds, DNS answers included) |
| `own_in_dns` | int[125] | Of `own_in`, this device's DNS answers |
| `own_out` | int[125] | Frames this device sent that the capture saw (its DNS questions included) |
| `arp` | int[125] | ARP request frames seen, from any device |
| `dhcp` | int[125] | DHCP DISCOVER frames seen, from any device |
| `lost_slot` | int[500] | Frames the kernel dropped, per 250 ms slot (4 a second), of every kind, whether watched or not. These frames were never seen. |
| `read_ok` | bool[500] | Whether the helper's lost-frame counter could be read at the end of that 250 ms slot. A good reading reports the sum of `lost_slot` since the previous good reading; a failed one reports nothing. |
| `f` | float[125] | The true share of frames dropped in that second. Ground truth only; the rules never see it, except the "oracle gate" comparison. |
| `qs` | list of tuples | This device's DNS questions **that the capture saw**, in time order: `(tq, seen_answer, failed, rtt_us, wait_watched, answered_by_network)`. `tq` is float seconds since the base began. `seen_answer`: its answer was seen within 5 s. `failed`: that answer carried an error code (only meaningful if seen). `rtt_us`: the true answer time in microseconds, whether or not the answer was seen. `wait_watched`: every second from `int(tq)` to `int(tq + 5)` was watched. `answered_by_network`: the resolver did send an answer (seen or not). Questions with `tq >= 120` are not listed. |
| `rose` | set of str | Ground truth: metrics whose true rate or level changed in the harmful direction in the window **without** a fault (a claim of them is never false). Names: `dns.time`, `dns.answered`, `dns.failed`, `dhcp.discover`, `arp.request`. |
| `fault` | str or None | Ground truth: the fault injected in the window, if any: `dns_down`, `dns_slow`, `servfail`, `arp_storm`, `dhcp_trouble` (material, G0 §1), or `subtle_latency` (1.3–1.8×, not material) |
| `art` | str or None | Ground truth: the visibility artifact in the window minute: `loss`, `hold`, `oneway`, or None |
| `art_base` | str or None | The same for the base minute |
| `selective` | bool | The window has burst (time-correlated) loss, as G0 §1 defines it |
| `material_slow` | bool | The true median answer time of the window (every answer the network sent) is at least 1.5 times and 20 ms above the base's |
| `read_fail` | bool | At least one counter reading failed in seconds 60–124 |
| `q` | float | This device's mean DNS questions a second at this site, for the density breakdown |

## Rules for the generator

- Model a network as it would look from a switch port or Wi-Fi station: the unit's own traffic plus broadcast and multicast. Choose your own site mix, traffic levels, fault magnitudes within G0 §1's material definitions, artifact lengths and shapes, and loss model. Kernel drops are by arrival time, not by frame kind.
- About half the windows should carry a fault and about half a visibility artifact, independently, so that some windows hold both. Include sparse and busy sites.
- Every count must be consistent: lost frames are never seen; a question lost to capture loss or HOLD is not in `qs`; an answer lost to capture loss is not seen; one-way visibility stops every frame to this device.
- Deterministic per seed. numpy only. No downloads.
