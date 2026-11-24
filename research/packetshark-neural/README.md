# PacketShark NEURAL — research lab (isolated, not production)

Host-side experiments and desk research behind `docs/apps/packetshark/NEURAL.md` and `docs/apps/packetshark/neural/` (draft PR #186). **Nothing here is built by `make`, linked into DOORS or installed on a unit.** The branch `research/packetshark-neural-lab` is not meant to be merged. It records what was run, so that the numbers in the design documents can be reproduced and challenged.

- Data: **all training and test data is generated** by the scripts below. No dataset was downloaded. Numbers measured on generated data show whether a mechanism works (shapes, masks, quantization, abstention, evaluation protocol). They do not show how a method behaves on a real network.
- Units: none was touched. The bench kit in `poc/` has been compiled but never run on a K230.
- Toolchain: the pinned Vision AI environment on the build host (`C:\K230-artifacts\ai-env\SETUP.md`). It provides nncase 2.11.0 + nncase-kpu 2.11.0 (venv-convert) and torch 2.1.2+cpu / onnx 1.16.2 (venv-train) in WSL `Ubuntu-22.04`. No package was added or upgraded.

## Layout

| Path | What |
| --- | --- |
| `probe_ops.py` | **E1** operator placement: tiny ONNX graphs, telemetry-shaped, compiled for k230 in three modes. Reads the compiler's own placement report (`cpu_ops` in the generated `estimate_fps.py`, KPU tile groups in `mac.csv`) and its roofline estimate; compares simulator vs onnxruntime |
| `compile_eval.py` | **E2** compile a trained model (u8, a16w8), run the simulator over whole input sets |
| `eval_quant.py` | **E2** judge quantization on the decisions (AUROC, rank correlation, nearest-prototype agreement) |
| `synth_telemetry.py` | generator: 120-s windows of the helper's per-second telemetry with faults, benign events and three visibility artifacts (HOLD, capture loss, one-way) |
| `exp_window.py` | **E3a** detectors: robust z, CUSUM, domain rules (with and without the visibility gate), logistic fusion, TCN autoencoder, supervised TCN |
| `exp_dna.py` | **E3b** Traffic DNA: generated flows, hand-crafted baseline vs 1-D CNN embedding, open-set rejection; exports `dna_h1`, `dna_h64`, `dna_b64` and the C-reference weights |
| `exp_rhythm.py` | **E3c** how many repetitions a periodicity claim needs at a 1 % false-alert rate |
| `summarize.py` | prints the result tables (stdlib only) |
| `poc/` | **K1 kit**: `infer_bench.cpp` (nncase host-tensor path + C reference), `build.sh`, `README.md` |
| `results/` | the JSON results of the runs quoted in `docs/apps/packetshark/neural/EXPERIMENTS.md` |
| `notes/` | desk research R1–R6 with their sources |

## Reproduce (WSL, from Git Bash: prefix `MSYS_NO_PATHCONV=1 wsl.exe -d Ubuntu-22.04 -e bash -c '…'`)

Run nncase jobs with a working directory outside the repository (the simulator leaves `gmodel_dump_dir/` in the cwd).

```bash
L=/mnt/c/k230-neural-lab/research/packetshark-neural      # this directory
R=~/work/psk-neural/runs; mkdir -p $R; cd $R
T=~/work/k230-ai/venv-train/bin/python
source ~/work/k230-ai/convert.env                         # nncase for the python below

python $L/probe_ops.py $R/probe_e1                         # E1: 24 cases x 3 modes, ~10 min
python $L/probe_ops.py $R/probe_e1b charcnn_batch32_names dna_batch64_avgpool
python $L/probe_ops.py --reparse $R/probe_e1               # re-read the dumps without compiling

$T $L/exp_window.py $R/e3a                                 # E3a, ~1 min
$T $L/exp_dna.py $R/e3b                                    # E3b, ~40 s
$T $L/exp_rhythm.py $R/e3c                                 # E3c, ~1 min

for m in ae sup; do python $L/compile_eval.py $m e3a/$m.onnx e3a/calib_x.npy e2 e3a/val_x.npy e3a/test_x.npy; done
python $L/compile_eval.py dna_b64 e3b/dna_b64.onnx e3b/dna_calib_h64.npy e2 e3b/dna_test_h64.npy
python $L/compile_eval.py dna_h64 e3b/dna_h64.onnx e3b/dna_calib_h64.npy e2 e3b/dna_test_h64.npy
python3 -c "import numpy as n; n.save('e3b/dna_test_h1_first512.npy', n.load('e3b/dna_test_h1.npy')[:512])"
python $L/compile_eval.py dna_h1 e3b/dna_h1.onnx e3b/dna_calib_h1.npy e2 e3b/dna_test_h1_first512.npy
$T $L/eval_quant.py $R                                     # E2 decisions
python3 $L/summarize.py $R

bash $L/poc/build.sh                                       # K1 kit (host + riscv64)
```

Seeds are fixed in every script. Training runs single-process on the CPU. nncase compiles and the simulator were deterministic across the runs made here, but that was not tested formally.

## Known limits of the lab

- The generators encode their author's model of a network. A detector that matches the generator's assumptions looks good on it. That is why every comparison here includes a baseline written from the same assumptions, and why no number here may be quoted as accuracy on real traffic.
- The compiler's KPU time is its own ideal roofline estimate (0.6 TFLOP/s, 3.2 GB/s off chip, 800 MHz). It leaves out the runtime's call, copy and cache-sync overhead. K1 measures that.
- The simulator is functional, not timed.
