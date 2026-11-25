# infer-bench — kit for the first KPU proof of concept (K1)

Research code for `docs/apps/packetshark/neural/ROADMAP.md` §1. **Not a DOORS component.** It is never installed, captures nothing, opens no socket and writes no file except the one named by `--dump`. Running it on a unit needs the owner's authorization.

**Run on Unit A on 25 November 2026** (owner-authorized, non-destructive, P7 skipped): see [K1-UNIT-A.md](K1-UNIT-A.md) for the measurements and `k1/` for the on-unit driver and host scripts.

## What it does

It runs the DNA encoder `dna-b64` (input `[1,3,64,32]` float32, 64 conversations × 3 channels × 32 packets; output `[1,32,64,1]` float32) two ways on the same input batches:

- `kpu`: the nncase 2.11 runtime with plain host tensors, no AI2D. The calls are `hrt::create(dt_float32, …, pool_shared)`, map, memcpy, unmap, `hrt::sync(sync_write_back)`, `run()`, then `output_tensor` → `to_host` → map_read. On exit it calls `shrink_memory_pool()` and `kd_mpi_mmz_deinit()`, as `pos-vision` does since docs/vision/KPU_CMA_LEAK.md.
- `ref`: a portable C reference of the same network, reading the same weights.

It prints one JSON object per line:

- `load`, `first_call`;
- `kpu_copy_in`, `kpu_sync`, `kpu_run`, `kpu_copy_out` and `kpu_call` (each as p50/p95/p99/max in µs);
- `ref_call`;
- `agree`, with per-conversation cosine against `--expect` and between kpu and ref, and the share of conversations whose nearest other conversation in the batch is unchanged (`nn_same`) or within the three nearest (`nn_top3`);
- `dump`, when `--dump F` is given: the kpu outputs `[N][32*64]` float32 written to `F`;
- `mem` (CmaFree, MemAvailable, VmRSS, VmHWM) at start, loaded, before_release, released and end.

## Build (WSL, this PC)

```bash
bash poc/build.sh            # -> ~/work/psk-neural/poc-build/{infer-bench-host, infer-bench}
```

`infer-bench` is riscv64, linked statically against `libNncase.Runtime.Native`, `libnncase.rt_modules.k230`, `libfunctional_k230` and `libmmz` from the vendor SDK output `k230_pocketos_defconfig` (nncase runtime 2.11.0). Its dynamic dependencies are only the C/C++ runtime libraries the image already has. Verified on 2026-11-24: it compiles and links. Rebuilt on 2026-11-25 with `--dump` and the nearest-neighbour check, then run on Unit A ([K1-UNIT-A.md](K1-UNIT-A.md)).

## Inputs (produced by `exp_dna.py` and `compile_eval.py`)

| File | Content |
| --- | --- |
| `dna_b64.weights.f32` | c1, c2, c3, fc weights then biases, float32 little-endian (20,960 floats, `WEIGHTS_N` in the source) |
| `dna_b64.test_in.f32` | 32 test batches, `[32][3][64][32]` float32 |
| `dna_b64.float_out.f32` | the float model's outputs for those batches, `[32][32][64]` |
| `dna_b64__u8/model.kmodel`, `dna_b64__a16w8/model.kmodel` | the compiled models (E2) |

## On the host

```bash
B=~/work/psk-neural/poc-build R=~/work/psk-neural/runs/e3b
$B/infer-bench-host --mode ref --in $R/dna_b64.test_in.f32 --batches 32 \
    --weights $R/dna_b64.weights.f32 --expect $R/dna_b64.float_out.f32 --iters 320
```

This checks the C reference against PyTorch (the `agree` line, expected cosine ≈ 1.0) and gives an x86 time. The x86 time is not a C908 time.

## On a unit (K1, needs authorization)

Done on Unit A on 2026-11-25 except P7. As run, the protocol differed from the list below in three ways. P3 ran separately with `--profile`, because profiling raised the run step from 109 to 279 µs. The P8 hog was a shell loop that ends when a stop file appears, so that nothing had to be killed. P10 ran against the P9 instance while it held the lock. The scripts that were used are in `k1/`.

Use Unit B. Vision must be closed and no capture running. Work from `/tmp/k1` and remove it at the end.

1. `CmaFree` idle three times, each after `sync; echo 1 > /proc/sys/vm/drop_caches`. This gives the noise.
2. P1–P5: `./infer-bench --mode both --kmodel u8.kmodel --weights w.f32 --in in.f32 --batches 32 --expect sim_u8.f32 --iters 1000 --profile --lock /tmp/k1/kpu.lock`. Repeat with the a16w8 model.
3. P6: read the `mem` lines, then `CmaFree` after drop_caches.
4. P7: `./infer-bench … --hold &`, then `kill -9` it once it prints `hold`, then `CmaFree` after drop_caches. Do this three times.
5. P8: repeat step 2 while a CPU hog runs at nice 0, for example `yes > /dev/null`.
6. P9: `--interval-ms 1000 --duration-s 600`.
7. P10: start a second instance with the same `--lock` while the first holds it. It must print `"ok":false` and exit 3.
8. Remove `/tmp/k1`. Read `CmaFree` once more. Record everything in a report beside the GO/NO-GO table of ROADMAP §1.

The serial gotchas recorded for the Vision bench apply. Keep console lines short. Put the commands in a script on the unit rather than typing long lines.
