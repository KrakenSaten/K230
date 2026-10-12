# R1-B pilot (2026-10-12): **INCONCLUSIVE**

Plan, command and thresholds: [R1B_PLAN.md](R1B_PLAN.md), commit `daf1d61`,
unchanged. The pilot stopped after epoch 20. It was not continued to 300.
- The held-out test was not read.
- Nothing was exported, deployed or merged.
- R0, R1-A and `traffic6_r0` are unchanged.

Records:
- [`data/traffic6_r0/r1b_pilot_gates.json`](data/traffic6_r0/r1b_pilot_gates.json)
- run in `C:\K230-work\yolox-train\runs\traffic6_r1b`
- logs `traffic6_r1b.session{1,2}.*.log`

## 1. Run

| | |
|---|---|
| Session 1 | 07:05:50 start (Oslo). Died 08:24:49 in epoch 6 (iteration 2,010) |
| Failure | `RuntimeError: level_zero backend failed with error: 20 (UR_RESULT_ERROR_DEVICE_LOST)` in YOLOX's SimOTA assignment (`yolo_head.py:454`, boolean-mask indexing) |
| Diagnosis | Windows System log: no event 08:20-08:30, no display-driver reset since start. XPU healthy afterwards: `xpu_check.py` ok, torch 2.13.0+xpu, driver 32.0.101.8531 unchanged. Cause unknown. R0 and R1-A ran the same assignment code about 61 h each without it. |
| Session 2 | One resume attempt at 08:27:12 from `latest_ckpt.pth` (epoch 5, sha256 `d519c7fd…`), configuration unchanged. Stopped normally after epoch 20 at 11:53:31. |
| Runtime (from timestamps) | Wall clock 4.78 h (`run.json` session 1 start 07:06:37 to session 2 end 11:53:31). Completed epochs 1-20, abandoned iterations excluded: 16,253 s training + 88 s validation = 4.54 h. Lost: **14.4 min**. That is 11.2 min of abandoned epoch-6 work (2,010 iterations, epoch-5 checkpoint 08:13:40 to the crash 08:24:49) plus a 3.2 min restart gap (to session 2 start 08:28:03). Corrected 2026-10-12: an in-progress message said "1.2 hours lost", which confused the time into the run with the time lost, and this record first said "about 13 min". |
| Logs | `iters.jsonl` holds the crashed session's partial epoch 6 (iterations 1-2,010) before the resumed epoch 6. `epochs.jsonl` holds epochs 1-20 once. |
| Peak VRAM | 2,894 MiB reserved (R0 2,726) |

## 2. Criteria (plain val from `epochs.jsonl`, against R0 at the same epoch)

| # | Criterion | Epoch 10 | Epoch 20 | R0 e10 / e20 | R1-A e10 / e20 |
|---|---|---|---|---|---|
| 1 | non-finite loss or a crash | losses finite; **session 1 crashed (GPU device lost)** | losses finite | | |
| 2 | mask share 0.05-0.11 (mosaic epochs) | 0.081-0.083 | 0.081-0.083 | | 0.129-0.131 |
| 3 | val FP ≤ 716 / ≤ 816 | **612** | **780** | 623 / 710 | 1,096 / 1,280 |
| 4 | bicycle AP50:95 ≥ 0.269 / ≥ 0.339 | **0.293** | **0.353** | 0.289 / 0.359 | 0.255 / 0.327 |
| 5 | person AP50:95 ≥ 0.195 / ≥ 0.262 | **0.218** | **0.282** | 0.215 / 0.282 | 0.209 / 0.264 |

Information only, not criteria:

| | Epoch 10 | Epoch 20 | R0 e10 / e20 |
|---|---|---|---|
| mAP50:95 | 0.243 | 0.301 | 0.238 / 0.310 |
| Precision | 0.808 | 0.803 | 0.804 / 0.816 |
| Car AP50:95 | 0.296 | 0.350 | 0.292 / 0.360 |

**Verdict: INCONCLUSIVE (owner decision, 2026-10-12).**
- Criteria 2-5 pass at both epochs.
- Criterion 1 is ambiguous for a GPU runtime fault followed by one resume.
  The owner ruled it neither pass nor reject.
- The gate script checked only finite losses for criterion 1. The crash is
  recorded here, not in its output.

## 3. Reading

**What it shows.** R1-B did not show R1-A's early failure:
- Val false positives were within 2 % of R0 at epoch 10 and 10 % at epoch 20.
  R1-A was at +76 % and +80 %.
- Bicycle and person AP were within 0.006 of R0. R1-A's bicycle AP was
  0.03 below.

**What it does not show.**
- Any improvement over R0. Car AP50:95 is 0.010 below R0 at epoch 20, and
  the pilot judges no car metric.
- Anything about medium-car recall, which only the full-run criteria
  (R1A_PLAN.md section 3) can judge.

**Uncertainty.**
- There is one run per arm, and YOLOX's loader workers reseed from `uuid4`.
- Session 2's resume adds a boundary. RNG, sampler and EMA are restored, but
  the augmentation draws differ anyway.
- Differences of this size (around 0.01 AP, around 10 % FP) are within the
  unknown run-to-run spread.
