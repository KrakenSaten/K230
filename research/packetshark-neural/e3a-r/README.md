# E3a-R: E3a redone with the corrected methodology

**Synthetic only; not field accuracy.** The method, results and limits are in [N1.1.md §3](../../../docs/apps/packetshark/neural/N1.1.md#3-e3a-redone-e3a-r-researchpacketshark-neurale3a-r).

`e3a_r.py` generates 60-second windows of the helper's own signals: this device's DNS questions and answers, its frames in and out, and ARP and DHCP. It adds a lost-frame counter read every 250 ms whose readings fail in spells. Faults and visibility artifacts (loss, HOLD, one-way) are drawn independently, so they overlap. It judges every window with four methods:

| Method | What it does |
| --- | --- |
| naive | No visibility handling |
| gate | E3a's binary gate, on the estimated coverage |
| oracle-gate | The binary gate, on the true coverage |
| loss-aware | The N1.1 engine's rules, mirrored line by line from `core/pocketpcap/ppc_intel.c` |

It reports, per seed:

- false claims;
- recall, split by the artifact a fault overlaps;
- how each miss happened.

It needs only Python 3 and numpy. It downloads nothing, trains nothing and touches no unit.

```sh
# WSL Ubuntu 22.04 (numpy 2.2), about 25 s
python3 e3a_r.py OUT_DIR                 # 10 seeds x 40 sites x 60 windows
python3 e3a_r.py OUT_DIR --seeds 1 --sites 5 --windows 20   # smoke run
```

`results/e3a_r_results.json` is the run quoted in N1.1.md: the configuration, the summary (mean, SD, minimum and maximum over the seeds) and every seed's scores. The seeds are fixed (`1000 + seed`), so a run reproduces it.

The original E3a (`exp_window.py`, `synth_telemetry.py`) is on the branch `research/packetshark-neural-lab`. Its gate figures stay withdrawn.
