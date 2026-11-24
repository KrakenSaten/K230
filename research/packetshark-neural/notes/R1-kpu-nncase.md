# R1 — K230 KPU and nncase 2.11 for small non-image models (desk + local source research, 2026-11-24)

Research by a specialist agent; nothing compiled, downloaded or run on a device. Classes: VERIFIED (read in source/headers), VERIFIED(sym) (a lowering rule, symbol or error string exists in the closed nncase-kpu 2.11.0 binary — proves a KPU path exists, not that a node will be placed there), DOCUMENTED (vendor docs), ASSUMED.

Paths: SDK = `~/work/t-display-k230/k230_linux_sdk` (WSL); RT = `SDK/output/k230_pocketos_defconfig/build/libnncase/nncase/include/nncase/runtime` (libnncase 2.11.0); PKG = `~/work/k230-ai/venv-convert/lib/python3.10/site-packages/nncase`; K230DLL = `PKG/modules/kpu/Nncase.Modules.K230.dll`.

## Placement (no official per-op KPU/CPU table exists)

The kmodel holds RuntimeModules of kind `k230` (KPU) and `stackvm` (CPU) (VERIFIED, `RT/k230/runtime_module.h`, `RT/stackvm/runtime_module.h`).

| Op | Placement | Class |
|---|---|---|
| Conv1D | importer unsqueezes to Conv2D (VERIFIED `Nncase.Importer/Onnx/Conv2D.cs` v2.11.0); KPU `GNNEConv2D` (sym). **E1 on this host: rank-3 Conv compiles without PTQ but fails under PTQ (ShapeInferenceError on strides) — export Conv2D with H=1** | VERIFIED + E1 |
| Dilated / depthwise conv | KPU (`conv_dw_dilation`, `GNNEPdp0DW`) | VERIFIED(sym) + E1 |
| MatMul/Gemm with constant weight | KPU, rewritten to 1×1 Conv2D (`BroadcastMatMulToConv2D`) | VERIFIED(sym) + E1 |
| MatMul dynamic×dynamic (attention) | KPU (`K230DynamicGNNEMatMul`; "int16 for both inputs not supported") | VERIFIED(sym) + E1 |
| LSTM | KPU path (`ToGNNELSTM`, piecewise sigmoid/tanh) and a stackvm kernel | VERIFIED(sym) + E1 (KPU + 3 CPU GetItem) |
| GRU | **not importable in 2.11** (no importer visitor, no stackvm op; `onnx_ops.md` marks it unsupported). The K230 Linux nncase doc table says "GRU yes" — DOCUMENTED but contradicted | VERIFIED + E1 |
| Softmax, LayerNorm, Gather, Where, ArgMax/ArgMin, Reshape | CPU (RVV-optimised): the vendor RVV page says the KPU does not accelerate them; ONNX LayerNormalization-17 not importable (export decomposed; `FoldLayerNormPattern1-5` fuse) | DOCUMENTED + E1 |
| ReduceMean/Max | KPU path (`ReduceToGlobalReduceWindow`, `GNNEPdp0Reduce`; "Unsupported MFU_PDP_OP MIN/SUM") — **E1: standalone ReduceMean/ReduceMax over an axis was placed on the CPU; GlobalAveragePool/AveragePool/MaxPool stayed on the KPU** | VERIFIED(sym) + E1 |
| Sigmoid/Tanh, Add/Sub/Mul/Div, Clamp, (Leaky/P)ReLU, HardSwish/HardSigmoid | KPU activation unit (`*ToFakeActivation`, piecewise fit) | VERIFIED(sym) + E1 (Sigmoid) |
| GELU | KPU if the erf form folds (`GeluToFakeActivation`, `FoldGeneralGelu`); ONNX Gelu-20 not importable. **E1: our erf graph did not fold; `Erf` alone has no PTQ evaluator → compile aborted** | VERIFIED(sym) + E1 |
| Transpose | KPU for rank ≤ 4 and supported perms | VERIFIED(sym) |
| Concat, Pad | KPU (`ConcatFusion`, `GNNEPad`) | VERIFIED(sym) + E1 (Concat) |
| Equal/Greater/Less, TopK | CPU (stackvm `compare`, `top_k`) | VERIFIED / ASSUMED |

Restrictions: KPU rank ≤ 4 ("GNNE not support more than 4D"); dynamic shapes only via ShapeBucket (off by default) and "The K230 Dynamic function Only accept Int32 Params"; vendor practice is fixed-length padding (TTS splits text into (1,50) chunks, `ai_demo/tts_zh/tts_src/main.cc:135`). No published channel-alignment rule (alignment helpers exist; one assertion "...[1] % 24 == 0", context unknown).

## Quantization

- `quant_type` / `w_quant_type` ∈ {uint8, int8, int16}, **not both int16** (DOCUMENTED; VERIFIED(sym)). Hints that depthwise conv and reduce have int16 limits (`DisableConvDWPermitInt16Quant`, `DisableConvPdp0ReducePermitInt16Quant`). a16w8 is the only int16 mode proven to stay on the KPU.
- `calibrate_method` NoClip or **Kld (the 2.11 default)**; `finetune_weights_method` NoFineTuneWeights, UseSquant, UseAdaRound (undocumented).
- Mixed quantization: `export_quant_scheme` with dump_ir → edit JSON (per-output DataType u8/i8/i16/f16/f32, ranges by tensor or channel) → `quant_scheme` (+ strict mode) (DOCUMENTED, `docs/MixQuant.md` v2.11.0). Per-channel weight ranges for conv and constant MatMul (sym); activations per-tensor (ASSUMED).
- `dump_quant_error` gives a per-layer error report.
- Placement inspection: `dump_ir` → `10_FusionToTirPass/mac.csv` and `13_DumpTraceInfoPass/` (estimate_fps.py with a `cpu_ops` list, draw_roofline.py). **On the device: `interpreter::set_profiling(1)` + `op_profile::print()` gives per-op CPU timings** (VERIFIED `RT/interpreter.h:105`, `RT/stackvm/op_profile.h`).

## Host-tensor runtime API (no AI2D) — VERIFIED in headers

- `nncase::runtime::interpreter` (`RT/interpreter.h`): `load_model(gsl::span<const gsl::byte>, bool copy_buffer=false)` / `load_model(std::istream&)`, `inputs_size()`, `outputs_size()`, `input_desc(i).datatype`, `input_shape(i)`, `output_shape(i)`, `input_tensor(i, t)`, `output_tensor(i[, t])`, `run()`, `set_profiling(uint8_t)`.
- `host_runtime_tensor` (`hrt`, `RT/runtime_tensor.h`): `create(typecode_t, dims_t, memory_pool_t = pool_shared_first)`, overloads wrapping an existing buffer (copy flag, pool, physical address) and a deleter variant; `memory_pool_t {pool_shared_first, pool_cpu_only, pool_shared}`; `hrt::map`, `hrt::sync(t, sync_op_t, force)`.
- Vendor pattern (`ai_demo/common/ai_base.cc`, `kws/kws.cc`): create inputs/outputs with `hrt::create(desc.datatype, shape, hrt::pool_shared)` once, write via `to_host()…map(map_write)` + memcpy, `hrt::sync(t, sync_write_back, true)`, `run()`, read via `map_read`.
- Release: `nncase::runtime::shrink_memory_pool()` (`RT/util.h:590`) and `kd_mpi_mmz_deinit()` (`sysroot/usr/include/mmz.h`); shared buffers come from `k230::mmz_allocator` on `/dev/mmz`. Vendor programs release only in a destructor, which never runs on SIGKILL (matches docs/vision/KPU_CMA_LEAK.md).

## Published figures

- Datasheet (DOCUMENTED): INT8 and INT16; ResNet50 ≥ 85 fps, MobileNetV2 ≥ 670 fps, YOLOv5s ≥ 38 fps (INT8); 2 MB + 2 MB SRAM for the KPU. No TOPS figure on Kendryte's pages ("6 TOPS equivalent" is a reseller claim). nncase README 2.11.0 (u8/u8): mobilenetv2 600 fps, resnet50V2 86 fps.
- Compiler roofline constants (VERIFIED(sym) and in the dumps): 800 MHz, 0.6 TFLOP/s, on-chip 25.6 GB/s, off-chip 3.2 GB/s, CPU 1.6 GHz; the driver sets the AI clock to 800 MHz (`drivers/misc/canaan/gnne/k230_gnne.c`).
- **Per-call overhead: not documented.** A community MLPerf-Tiny port reports ResNet-8 at ~2.3 ms per inference (upper bound).
- Several interpreters in one process: works (TTS chains three).
- **Several processes: `/dev/k230-gnne` has only open/release/poll/fasync; `gnne_mutex` is declared but never used; one global interrupt flag; userspace programs the registers. No arbitration between processes (VERIFIED). Concurrent use from two processes must be assumed unsafe.**

## Vendor non-vision examples (SDK `buildroot-overlay/package/ai_demo/`)

- `kws` (WeNet KWS): two float32 host inputs, features [1,30,40] and a streaming cache [1,256,105]; outputs logits + new cache — the same streaming shape as telemetry windows (VERIFIED).
- `tts_zh`: FastSpeech2 encoder/decoder + HiFiGAN; int32 phoneme ids [1,50] → embedding Gather (VERIFIED).
- Whether their recurrent ops ran on the KPU is unknown (kmodels not downloaded).

## Three biggest risks

1. CPU-fallback fragmentation (each KPU/CPU boundary adds quantize/dequantize and a DDR trip; int16 weights fell back entirely at 49.7 s). Keep graphs to Conv, constant MatMul, activations, pools, Concat, Pad; do embedding (or one-hot + 1×1 conv), softmax, top-k on the host; check every model with dumps and `set_profiling`.
2. Quantization of heavy-tailed telemetry (per-tensor activations; u8 collapsed YOLOX). Normalise on the host (log1p, z-score), calibrate with real traffic, try a16w8 first, check `dump_quant_error`.
3. Ownership of the KPU and CMA: no kernel arbitration; MMZ not reclaimed on kill; static shapes only. One process at a time owns the KPU and releases on SIGTERM.
