# PacketShark NEURAL: host-side experiments

**Run on 24 November 2026 on the build host** (Intel i7-1265U, WSL Ubuntu 22.04). The pinned Vision AI environment was used unchanged: nncase 2.11.0 with nncase-kpu 2.11.0, torch 2.1.2+cpu, onnx 1.16.2, onnxruntime 1.18.1. No unit was touched, no dataset was downloaded and no package was added.

Scripts, result JSON and the desk research are on the branch `research/packetshark-neural-lab` at [`29e1e8c`](https://github.com/KrakenSaten/K230/tree/29e1e8cc/research/packetshark-neural), under `research/packetshark-neural/`. Kmodels, ONNX files, bench inputs and binaries are in `C:\K230-artifacts\packetshark-neural\`, with `SHA256SUMS.txt`.

**How to read the numbers.**
- *Compiler placement* is VERIFIED: it is what nncase itself reported in its dumps.
- *Compiler time* is the compiler's own ideal roofline estimate: 800 MHz, 0.6 TFLOP/s, 3.2 GB/s off chip, 25.6 GB/s on chip. It leaves out the runtime's per-call overhead. It is **not a measurement**.
- *Simulator agreement* is VERIFIED functional equivalence. The simulator is not timed.
- E3 accuracy figures come from **generators written for this study**. They show whether a mechanism works (shapes, masks, quantization, abstention, evaluation protocol). They are **not** accuracy on real networks.

Each experiment was chosen to settle one architectural uncertainty:

| Experiment | Settles |
| --- | --- |
| **E1** operator placement | Can telemetry-shaped models run on the KPU at all, and which operators fall back to the CPU? |
| **E2** quantization on decisions | Does PTQ damage the decisions the outputs feed, not only the tensors? |
| **E3a** window detectors | Do small temporal models add anything over cheap statistics? What does visibility loss do to each? |
| **E3b** Traffic DNA | Does a learned embedding beat hand-crafted features? Can it say "unclassified"? |
| **E3c** rhythm | How many repetitions does a periodicity claim need inside one session? |
| **K1 kit** | Does the host-tensor runtime path compile and link for the device? Is the C reference exact? |

## E1: operator placement (`probe_ops.py`)

Each case is a tiny ONNX graph (opset 13, 17 for LayerNormalization) shaped like the helper's data: 16 features × 120 one-second bins, 64 conversations × 32 packets, or 32 names × 64 characters. Every case was compiled for target `k230` in three modes: no quantization, u8/u8 PTQ, and a16w8 PTQ (int16 activations, uint8 weights; NoClip calibration on 8 random samples). Each ran in the simulator on 4 held-out random inputs against onnxruntime. Placement comes from nncase's own `cpu_ops` list and KPU tile groups. The table shows u8. a16w8 placed the same operators the same way.

| Case | Shape | Placement (u8) | CPU ops nncase listed | KPU MACs (nncase's own count) | Compiler time | Sim vs float cos (min) |
| --- | --- | --- | --- | --- | --- | --- |
| Temporal conv as Conv2D (H = 1), k 3 | [1,16,1,120] → 32 ch | **KPU** | — | 184 k | 0.073 ms | 0.9997 |
| Dilated (d 4) / depthwise (k 5) / strided + MaxPool | same | **KPU** | — | 142–184 k | 0.071–0.076 ms | ≥ 0.9998 |
| TCN residual block (dilations 2, 4) | [1,32,1,120] | **KPU** | — | 737 k | 0.079 ms | 0.9999 |
| Two inputs: values × coverage mask, Concat, conv | 2 × [1,16,1,120] | **KPU** | — | 369 k | 0.207 ms | 0.9997 |
| MLP (Gemm 1920 → 64 → 16) | [1,1920] | **KPU** | — | 124 k | 0.108 ms | 0.9998 |
| Batch of 64 conversations (N = 64), 2 convs + GlobalAveragePool | [64,3,1,32] | **KPU** | — | 108 k | 0.116 ms | 0.99998 |
| One-hot name CNN (embedding as 1 × 1 conv), GlobalMaxPool, Gemm, Sigmoid | [1,40,1,64] | **KPU** | — | 369 k | 0.071 ms | 1.0 |
| **32 names × 64 chars** name CNN, MaxPool(1 × 64) | [1,40,32,64] | **KPU** | — | **65.6 M** | **0.229 ms** | 1.0 |
| **64 conversations as H**, AveragePool(1 × 8) | [1,3,64,32] | **KPU** | — | **13.3 M** | **0.101 ms** | 0.99994 |
| Same with ReduceMean over packets | [1,3,64,32] | KPU + CPU | Reduce | 6.9 M | 0.172 + 0.084 ms | 0.99998 |
| MatMul with constant weight (64 × 96 · 96 × 32) | [1,64,96] | KPU + CPU | Reshape | 3 k | 0.082 ms | 0.9999 |
| L2 normalisation (ReduceL2, Div) | [1,64,32] | KPU + CPU | Unary, Unary, Binary | 0 | 0.132 + 0.010 ms | 1.0 |
| GlobalAveragePool + ReduceMax | [1,32,1,120] | KPU + CPU | Reduce | 0 | 0.070 + 0.005 ms | 1.0 |
| Single-head attention (32 tokens × 32) | [1,32,32] | KPU + CPU | Softmax, 3 × Reshape | 3 k | 0.208 ms | 0.998 |
| LayerNorm written out (ReduceMean, Sub, Sqrt, Div...) | [1,32,64] | KPU (5 groups) + CPU | Reshape, Binary, Unary, Binary | 0 | 0.523 ms | 0.9997 |
| LSTM (hidden 16, 32 steps) | [32,1,8] | KPU (4 groups) + CPU | 3 × GetItem | — | 0.235 ms | **0.995** |
| Softmax | [1,4,32,32] | **CPU** | Softmax | — | 0.024 ms (CPU) | 1.0 |
| Gather (embedding table) | [1,64] int64 | **CPU** | Gather | — | — | 1.0 |
| ArgMax | [1,64,16] | **CPU** | ReduceArg | — | — | 1.0 |
| Conv1D with rank-3 tensors | [1,16,120] | **refused under PTQ** (ONNX shape inference error on `strides`); compiles without PTQ, then runs on the CPU | — | — | — | — |
| LayerNormalization (opset 17) | | **refused by the importer** | — | — | — | — |
| GRU | | **refused by the importer** (`Not Supported Onnx op: GRU`) | — | — | — | — |
| Erf (in GELU), with Sigmoid/Tanh/LeakyReLU | | **refused under PTQ** (no metric evaluator for Erf) | — | — | — | — |

Further findings:
- **Without quantization nothing reaches the KPU.** Every case compiled in "none" mode kept a float graph on the CPU, with no KPU tile groups. The float LSTM crashed the simulator (SIGSEGV).
- **a16w8 costs more than u8 in the compiler's own estimate** on MatMul-heavy graphs: 0.555 ms against 0.082 ms for the batched MatMul. On convolutions the two were close.
- **The KPU programs show fp16 as well as i8** even in u8 mode (`datatype: fp16` in the scheduled IL). What that means for accuracy is ASSUMED, not established. It may explain the very high simulator agreement.
- The desk research (R1) agrees on every operator it could check. It adds one fact E1 could not see: the `/dev/k230-gnne` driver does no arbitration between processes.

**What it settles.** Models for this engine must use one operator vocabulary: Conv2D with H = 1 (no rank-3 Conv), ReLU or LeakyReLU, Max/AveragePool, Add, Concat, constant Gemm and Sigmoid. They must be quantized, and normalisation, softmax, top-k and embedding lookups happen on the host. Within that vocabulary a whole model is one or two KPU tile groups, with no CPU round trips.

## E2: quantization judged on decisions (`compile_eval.py`, `eval_quant.py`)

Trained models from E3 were compiled in u8 and a16w8, with calibration from training data only. The simulator then ran over the complete validation and test sets: 600 + 1,800 windows, and 2,368 + 512 conversations.

| Model | Mode | Placement | kmodel | Compiler time | Decision-level result |
| --- | --- | --- | --- | --- | --- |
| Window autoencoder (34,384 params) | u8 | KPU, no CPU ops | 56 KB | 0.084 ms | AUROC 0.648 (float 0.649); score rank correlation with float **0.9999** |
| | a16w8 | KPU | 52 KB | 0.085 ms | AUROC 0.648; ρ 0.9999 |
| Supervised window TCN (18,577 params) | u8 | KPU, no CPU ops | 24 KB | 0.074 ms | AUROC 0.904 (float 0.904); TPR at 1 % alerts 0.725 (float 0.722); ρ 0.9999 |
| | a16w8 | KPU | 26 KB | 0.075 ms | AUROC 0.904; ρ 1.0 |
| DNA encoder `dna-b64` (20,960 params), 64 conversations per call | u8 | **KPU, no CPU ops** | 27 KB | **0.101 ms** | cosine to float: min 0.9947, p1 0.9985, median 0.99988; nearest-prototype agreement **99.79 %**; accuracy 0.9319 (float 0.9324) |
| | a16w8 | KPU | 29 KB | 0.117 ms | cosine min 0.9992; agreement 99.96 % |
| Same encoder with normalisation in the graph (`dna_h64`) | u8 | KPU + CPU (Gather, Reshape, Unary, Binary) | 31 KB | 0.49 + 0.06 ms | identical to `dna-b64` after host normalisation |
| Same encoder, one conversation per call (`dna_h1`) | u8 | KPU + CPU | 29 KB | 0.33 ms | agreement 100 % on 512 conversations |

**What it settles.**
- PTQ in u8 does not change any decision these models feed, so a16w8 is unnecessary here. That contrasts with YOLOX, which needed a16w8 (`docs/apps/VISION.md`). Telemetry inputs that are log-scaled and normalised on the host quantize well.
- Batching is where the KPU's time goes furthest. In the compiler's estimate, 64 conversations in one call cost 0.0016 ms each, against 0.33 ms for one conversation per call. That is about 200 times less per conversation, before the unmeasured call overhead.

## E3a: window detectors and the visibility gate (`synth_telemetry.py`, `exp_window.py`)

**Data.**
- **Window.** A generated 120-s window of 16 per-second features the helper can derive, plus coverage per second. The features are packets and kB in/out, broadcast, DNS queries/answers/failures/unanswered, DNS and handshake medians, SYNs, resets, retransmissions, ARP events and DHCP messages.
- **Sites.** 40 training sites (2,400 windows), 10 validation sites (600 benign windows, used only to set thresholds) and 15 test sites (1,800 windows). Each site has its own traffic level, resolver and path latencies, broadcast rate and error rates.
- **Benign windows** contain ordinary changes: downloads, idle stretches, surges, update bursts, video.
- **Faults** start inside the last 30 s:
  - five seen in training: resolver slow, resolver down, path loss, ARP storm, DHCP trouble;
  - three never seen: SERVFAIL, upstream latency, a subtle mix of three individually normal shifts.
- **Visibility artifacts** sit in the last 30 s, each 15 % of test windows in total:
  - HOLD: everything zero, coverage 0;
  - capture loss: 15–60 % of frames never reach the helper, answers lost so their queries look unanswered, coverage = 1 − loss;
  - one-way visibility: replies on another path, coverage 0 in the direction sense.

**Methods.**
- **B1** robust z: max over features of the recent median against the reference median/MAD.
- **B2**: B1's top three summed.
- **B3** two-sided CUSUM.
- **B4 domain rules**, one-sided. Rates are normalised by their volume (retransmissions per packet, unanswered per query, failures per answer, resets per SYN, by binomial z). DNS and handshake medians are compared against the reference MAD. ARP and DHCP counts use a Poisson z. Each rule abstains below a minimum count. Every rule uses only bins with coverage ≥ 0.98. The same rules were also run without the coverage gate.
- **N3** logistic regression over the eight B4 scores.
- **N1** TCN autoencoder, trained on benign and artifact windows only.
- **N2** supervised TCN, trained on the five seen fault types.

Thresholds were set at 1 % and 5 % alert rates on the benign validation sites.

| Method | AUROC fault vs benign | TPR seen @ 1 % | TPR unseen @ 1 % | Alerts on HOLD / loss / one-way @ 1 % |
| --- | --- | --- | --- | --- |
| B1 robust z | 0.569 | 0.000 | 0.000 | 0 / 0 / 0 |
| B2 top-3 z | 0.541 | 0.000 | 0.000 | 0 / 0 / 0 |
| B3 CUSUM | 0.581 | 0.049 | 0.014 | 0 / 0 / 0 |
| **B4 domain rules, gated** | **0.879** | **0.798** | **0.491** | 0.011 / 0.023 / 0.038 |
| B4 domain rules, **no gate** | 0.879 | 0.798 | 0.491 | 0.011 / **0.756** / **0.899** |
| N3 logistic over B4 | 0.808 | 0.768 | 0.260 | 0 / 0 / 0 |
| N1 TCN autoencoder | 0.649 | 0.124 | 0.160 | 0 / 0.023 / 0.013 |
| N2 supervised TCN | 0.904 | 0.722 | 0.516 | 0 / 0 / 0 |

True-positive rate per fault type at 1 % benign alerts:

| Method | resolver slow | resolver down | path loss | ARP storm | DHCP trouble | *SERVFAIL* | *upstream latency* | *subtle mix* |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| B4 gated | 0.52 | 0.74 | 0.82 | 1.00 | 0.89 | **0.74** | 0.44 | 0.26 |
| N3 logistic | 0.56 | 0.67 | 0.71 | 0.95 | 0.92 | **0.00** | 0.41 | 0.39 |
| N2 supervised | 0.81 | 0.51 | 0.71 | 0.69 | 0.86 | **0.00** | **0.96** | **0.64** |
| N1 autoencoder | 0.19 | 0.06 | 0.08 | 0.19 | 0.08 | 0.12 | 0.28 | 0.08 |

Fault types in italics were never seen in training.

**What it settles, within the limits of a generator.**

1. **"Something changed" is the wrong question.** Benign traffic changes all the time. Generic change detectors (B1–B3, N1) separate faults from benign change barely better than chance. What works is knowing which direction of which ratio is harmful (B4).
2. **The visibility gate is the most valuable single component.** Without it, the same rules raise alerts on 76 % of capture-loss windows and 90 % of one-way windows, because a lost answer looks exactly like an unanswered query. With it, 2–4 %. This answers the first brief's experiment 1 on the generator. It also shows that the gate still leaks: rates whose denominator spans a lossy bin. Fixing that is G0 work.
3. **A supervised model learns its catalogue.** N2 wins on fault types similar to its training (upstream latency 0.96, subtle mix 0.64 against the rules' 0.44 and 0.26). It finds **none** of the SERVFAIL windows, which the rules catch at 0.74.
4. **Learned fusion can suppress evidence.** The logistic regression gave the failure rule a negative weight (−0.43) because no training fault used it, and so missed every SERVFAIL. Learned weights over rules must therefore be non-negative, and a model may reorder evidence but never veto it.
5. **There is a learnable residual on multi-signal faults** (N2 on the subtle mix). Whether it survives real data is G3's question, and it is the only reason to keep a neural detector on the research track.

## E3b: Traffic DNA (`exp_dna.py`)

**Data.** Generated flows from ten traffic classes: web over TLS, video, bulk down, bulk up, voice, gaming, small API calls, IoT telemetry, interactive SSH and QUIC web. Training uses 60 sites; testing uses 20 different sites, whose RTT, MTU and IPv4/IPv6 overhead vary. 30 % of flows are truncated to 4–15 packets. Gaming and interactive SSH are never seen in training. Each flow is represented by its first 32 packets: signed length/1500, log IAT, a mask.

| Representation | Accuracy, known classes (prototype) | kNN-5 | Short flows (< 16 packets) | Full 32 packets | Open-set AUROC | Unknown rejected at 95 % known acceptance |
| --- | --- | --- | --- | --- | --- | --- |
| Hand-crafted, 24 dimensions | 0.889 | 0.906 | 0.713 | 0.973 | 0.652 | 0.0 % |
| **1-D CNN, 32-d (`dna-b64` weights)** | **0.933** | **0.932** | **0.793** | **1.000** | 0.704 | **0.2 %** |

**What it settles.** A small encoder beats hand-crafted features on the same information, most of all on short flows. Training took 14 s on the CPU. **Neither representation can say "unclassified" by a similarity threshold.** The unseen classes land confidently near known ones: gaming near voice, SSH near small API calls. So the product shows resemblance within the capture ("these four look alike"), and labels come only from deterministic evidence. A future labelled mode would need distance plus margin rejection calibrated on real data (R3). The same encoder runs 64 conversations per call on the KPU (E2).

## E3c: rhythm (`exp_rhythm.py`)

A periodic source with relative jitter (uniform ± j of the period) and missed events (probability q) was compared against Poisson and bursty (log-normal) arrivals with the same number of events. Two O(n) scores were used: one minus the coefficient of variation of the inter-arrival times, and the share of intervals near a multiple of the median (which tolerates missed events). Thresholds were set at 1 % false alerts, with 4,000 negatives per n.

| Jitter, missed | Events for TPR ≥ 0.9 (CV score) | (multiple-aware score) |
| --- | --- | --- |
| 1 %, none | 5 | 5 |
| 1 %, 20 % missed | 6 | 6 |
| 5 %, none | 5 | 5 |
| 5 %, 20 % missed | 8 | 10 |
| 15 %, none | 10 | 20 |
| 15 %, 20 % missed | 15 | 30 |
| 30 %, none | 15 | > 30 |
| 30 %, 20 % missed | 30 | > 30 |

**What it settles.** A rhythm needs at least 5 repetitions, and 10–30 once timing is loose. The default capture stops after 15 minutes. That is enough for a 30-s keepalive or a 120-s mDNS announcement. It is not enough for a 5-minute rhythm with any jitter. The rhythm lens must therefore show "needs N more repetitions" instead of guessing. No neural method is needed for this.

## K1 kit: built, not run (`poc/`)

| Check | Result |
| --- | --- |
| `infer-bench` for riscv64, `-DWITH_NNCASE` | Compiles and links (2026-11-24) against the pinned SDK's `libNncase.Runtime.Native`, `libnncase.rt_modules.k230`, `libfunctional_k230` and `libmmz` (nncase runtime 2.11.0, `k230_pocketos_defconfig` output). Host-tensor API only, no AI2D. Dynamic dependencies are the C/C++ runtime only. **Never run on a K230.** |
| C reference vs the PyTorch float model | cosine **1.000000** over 2,048 conversation embeddings (32 batches) |
| C reference vs the u8 simulator | cosine min 0.9947, mean 0.99976 |
| C reference speed on the x86 host (single thread, scalar, -O2) | 8.1 ms p50, 11.0 ms p95 per 64-conversation batch (13.3 M MACs). This is not a C908 figure. On the C908 it is ASSUMED to be several times slower. |

So the comparison K1 must make on the unit is between roughly 0.1 ms (the compiler's ideal KPU time, without overhead) and several to tens of milliseconds (the C reference on the C908, ASSUMED). The answer depends on the call overhead no document gives (R1).

## Reproduce

The recipe is in `research/packetshark-neural/README.md` on the research branch. In short:

```bash
# WSL Ubuntu-22.04, cwd outside the repository (the simulator writes gmodel_dump_dir/ into the cwd)
source ~/work/k230-ai/convert.env
python $L/probe_ops.py $R/probe_e1                          # E1
~/work/k230-ai/venv-train/bin/python $L/exp_window.py $R/e3a # E3a (also exp_dna.py, exp_rhythm.py)
python $L/compile_eval.py ae $R/e3a/ae.onnx $R/e3a/calib_x.npy $R/e2 $R/e3a/val_x.npy $R/e3a/test_x.npy
~/work/k230-ai/venv-train/bin/python $L/eval_quant.py $R     # E2
bash $L/poc/build.sh                                        # K1 kit
```

Seeds are fixed. A full run takes about 25 minutes on the build host, most of it in the simulator.
