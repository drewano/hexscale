# hexscale

**Offloading Neural Super-Resolution to Qualcomm Hexagon NPUs (CDSP) under Mainline Linux.**

`hexscale` is an experimental project exploring how to offload real-time neural upscaling and image enhancement to the Qualcomm Hexagon Compute DSP / NPU (CDSP) on modern Snapdragon SoCs (specifically the **SM8550 / Snapdragon 8 Gen 2**) running mainline Linux and SteamOS-like environments (such as [Armada OS](https://github.com/armada-os/armada)).

---

## The Problem & Motivation

Handheld gaming consoles based on ARM64 SoCs operate under constrained thermal and electrical budgets (typically 5W to 15W total package power):

1. **GPU Bottlenecks**: Modern super-resolution algorithms (such as temporal or neural upscalers) running on mobile GPUs (e.g., Qualcomm Adreno 740) consume a significant fraction of available shader cores and memory bandwidth, increasing heat and draining battery life.
2. **Untapped Silicon**: Qualcomm Snapdragon SoCs integrate a dedicated **Hexagon Tensor Processor (CDSP / NPU)** capable of massive parallel matrix math (INT8 / FP16) at exceptionally low power consumption.
3. **The Opportunity**: By routing frame buffers from the graphics pipeline directly into the CDSP via zero-copy DMA-BUF sharing, we can execute neural upscaling (e.g., 720p to 1080p) on the NPU, leaving **100% of the GPU shader budget free for the game**.

---

## Architectural Concept

```
┌─────────────────────────────────────────────────────────────┐
│                 Game / Emulator (ARM64)                     │
│                 Renders at 720p via Vulkan                  │
└──────────────────────────────┬──────────────────────────────┘
                               │
                               ▼
┌─────────────────────────────────────────────────────────────┐
│                    VRAM Framebuffer                         │
│                    (dma-buf handle)                         │
└──────────────────────────────┬──────────────────────────────┘
                               │ Zero-Copy Import
                               ▼
┌─────────────────────────────────────────────────────────────┐
│                 Linux Mainline FastRPC                      │
│                 /dev/fastrpc-cdsp                           │
└──────────────────────────────┬──────────────────────────────┘
                               │
                               ▼
┌─────────────────────────────────────────────────────────────┐
│             Qualcomm Hexagon NPU (CDSP)                     │
│    Executes lightweight INT8/FP16 Super-Resolution Model    │
│    (Hexagon Tensor Processor / HVX Vector Pipeline)         │
└──────────────────────────────┬──────────────────────────────┘
                               │
                               ▼
┌─────────────────────────────────────────────────────────────┐
│               Upscaled 1080p Output Buffer                  │
│               Composited via Gamescope / DRM/KMS            │
└─────────────────────────────────────────────────────────────┘
```

---

## Hardware Target

* **Primary Platform**: Qualcomm Snapdragon 8 Gen 2 / QCS8550 (SM8550)
  * Hardware: **AYN Odin 2** handheld console
  * OS: **Armada OS** (Fedora `bootc` container on Linux 7.2+ mainline)
  * NPU Subsystem: Qualcomm Hexagon 790 (HTP + HVX)
* **Future Platforms**: Snapdragon X Elite (X1E80100), Snapdragon 8 Gen 3 (SM8650)

---

## Roadmap

### Phase 1: Hardware & Subsystem Bring-Up
- [ ] Verify `/dev/fastrpc-cdsp` device node availability and permissions under mainline Linux 7.2+.
- [ ] Confirm `cdsp.mbn` firmware authentication via `qcom_q6v5_pas`.
- [ ] Verify power domain transitions (`cx`, `mxc`, `nsp`) and ensure CDSP active states do not cause kernel panics or sleep stalls.

### Phase 2: User-Space Pipeline & Zero-Copy Memory
- [ ] Build a minimal user-space harness utilizing `libfastrpc`.
- [ ] Implement zero-copy buffer sharing between DRM/GBM render targets and FastRPC shared memory buffers (`FASTRPC_IOCTL_MMAP`).
- [ ] Measure memory roundtrip latency (GPU VRAM -> FastRPC -> VRAM).

### Phase 3: Model Execution on Hexagon
- [ ] Evaluate inference toolchains:
  - Qualcomm Neural Processing Engine / QNN Execution Provider
  - Native Hexagon LLVM / DSP C++ shared library (`.so`)
- [ ] Benchmark lightweight neural upscaler architectures (e.g. Real-ESRGAN Compact, Anime4K-style CNN, or mobile INT8 architectures).
- [ ] Target budget: **< 5 ms inference latency** for 60 FPS frame pacing.

### Phase 4: Compositor & Gaming Integration
- [ ] Wrap the upscaling pipeline into a Vulkan post-processing layer or a native [Gamescope](https://github.com/ValveSoftware/gamescope) filter pass.
- [ ] Expose user configuration (toggle, sharpening factor, performance profiles) via Quick Access Menu / Decky plugin.

---

## Development & Research Notes

* **Upstream Kernel Context**: Recent patches on `lore.kernel.org/linux-arm-msm` (such as Mukesh Ojha's `remoteproc: Hawi CDSP support with per-PD`) establish per-power-domain scaling for the CDSP proxy rails, enabling safe performance scaling without stalling system suspend (`s2idle`).
* **Related Work**:
  * [Armada OS](https://github.com/armada-os/armada)
  * [FastRPC driver documentation](https://www.kernel.org/doc/html/latest/driver-api/fastrpc.html)
  * [Gamescope](https://github.com/ValveSoftware/gamescope)

---

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.
