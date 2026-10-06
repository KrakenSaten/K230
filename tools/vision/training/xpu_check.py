#!/usr/bin/env python3
"""Basic Intel XPU check before any training on an Intel GPU (Arc B580).

Proves that PyTorch runs on the XPU and not silently on the CPU:
  - torch.xpu is present and available, device name and memory
  - tensors allocated on xpu report device type xpu
  - a matmul on xpu matches the same matmul on the CPU
  - the XPU allocator's counters rise while the tensors live
  - an XPU matmul is clearly faster than the CPU one (timing, informative)
  - a small conv/backward/SGD step runs on xpu

Exits non-zero on any failure. Prints one JSON record.

Usage: xpu_check.py [--size 4096]
"""
import argparse
import json
import platform
import sys
import time

import torch


def mb(n):
    return round(n / 2**20, 1)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--size", type=int, default=4096)
    args = ap.parse_args()

    rec = {"python": sys.version.split()[0], "platform": platform.platform(), "torch": torch.__version__,
           "has_torch_xpu": hasattr(torch, "xpu")}
    if not rec["has_torch_xpu"] or not torch.xpu.is_available():
        rec["xpu_available"] = False
        print(json.dumps(rec, indent=1))
        raise SystemExit("torch.xpu is not available")
    rec["xpu_available"] = True
    rec["device_count"] = torch.xpu.device_count()
    p = torch.xpu.get_device_properties(0)
    rec["device_name"] = torch.xpu.get_device_name(0)
    rec["total_memory_mb"] = mb(getattr(p, "total_memory", 0))
    rec["driver_version"] = getattr(p, "driver_version", None)
    rec["device_props"] = {k: getattr(p, k) for k in ("platform_name", "type", "max_compute_units",
                                                      "gpu_eu_count", "has_fp16", "has_fp64",
                                                      "has_atomic64") if hasattr(p, k)}

    n = args.size
    g = torch.Generator().manual_seed(0)
    a_cpu = torch.randn(n, n, generator=g)
    b_cpu = torch.randn(n, n, generator=g)
    base = torch.xpu.memory_allocated()
    a = a_cpu.to("xpu")
    b = b_cpu.to("xpu")
    c = a @ b
    torch.xpu.synchronize()
    rec["tensor_device"] = str(c.device)
    assert c.device.type == "xpu", c.device
    rec["memory_allocated_mb"] = mb(torch.xpu.memory_allocated() - base)
    rec["memory_reserved_mb"] = mb(torch.xpu.memory_reserved())
    assert torch.xpu.memory_allocated() - base >= 3 * n * n * 4, "XPU allocator did not grow"

    ref = a_cpu @ b_cpu
    rel = float((c.cpu() - ref).abs().max() / ref.abs().max())
    rec["matmul_max_rel_err_vs_cpu"] = rel
    assert rel < 1e-2, rel

    def bench(fn, sync, reps=10):
        fn(); sync()
        t = time.perf_counter()
        for _ in range(reps):
            fn()
        sync()
        return (time.perf_counter() - t) / reps

    # nonzero and boolean-mask indexing against the CPU. YOLOX's label
    # assignment is built on them. torch 2.14.x+xpu (SYCL runtime 2026.1)
    # returned wrong indices here on an Arc B580 with driver 32.0.101.8531,
    # and training then hit a device assert / DEVICE_LOST (2026-10-06).
    bad = 0
    for size in (8, 64, 1000, 3549, 100000):
        for _ in range(20):
            m = torch.rand(size, generator=g) < 0.05
            bad += not torch.equal(m.to("xpu").nonzero().cpu(), m.nonzero())
    v = torch.randn(3549, 4, generator=g)
    for _ in range(100):
        m = torch.rand(3549, generator=g) < 0.05
        bad += not torch.equal(v.to("xpu")[m.to("xpu")].cpu(), v[m])
    rec["nonzero_mask_mismatches"] = "%d of 200" % bad
    assert bad == 0, "torch.nonzero / boolean masks are wrong on this XPU stack: %d of 200" % bad

    tx = bench(lambda: a @ b, torch.xpu.synchronize)
    tc = bench(lambda: a_cpu @ b_cpu, lambda: None, reps=3)
    rec["matmul_ms"] = {"xpu": round(tx * 1e3, 2), "cpu": round(tc * 1e3, 2)}
    rec["matmul_tflops_xpu_fp32"] = round(2 * n**3 / tx / 1e12, 2)

    # A training step: conv, loss, backward, optimiser, all on xpu.
    net = torch.nn.Sequential(torch.nn.Conv2d(3, 32, 3, padding=1), torch.nn.SiLU(),
                              torch.nn.Conv2d(32, 8, 3, padding=1)).to("xpu")
    opt = torch.optim.SGD(net.parameters(), lr=0.01, momentum=0.9)
    x = torch.randn(8, 3, 128, 128, device="xpu")
    losses = []
    for _ in range(5):
        loss = net(x).pow(2).mean()
        opt.zero_grad()
        loss.backward()
        opt.step()
        losses.append(loss.item())
    assert all(q.device.type == "xpu" for q in net.parameters())
    assert all(q.grad is not None and q.grad.device.type == "xpu" for q in net.parameters())
    rec["train_step_losses"] = [round(v, 5) for v in losses]
    assert losses[-1] < losses[0], losses
    rec["max_memory_allocated_mb"] = mb(torch.xpu.max_memory_allocated())
    rec["ok"] = True
    print(json.dumps(rec, indent=1))


if __name__ == "__main__":
    main()
