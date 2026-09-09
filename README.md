# hexscale

**Offloading Neural Super-Resolution and Frame Generation to Qualcomm Hexagon NPUs under Mainline Linux & SteamOS.**

`hexscale` is an open-source, hardware-accelerated project designed to offload real-time neural upscaling (super-resolution) and frame interpolation to the Qualcomm Hexagon Compute DSP / NPU (CDSP) on Snapdragon SoCs (specifically the **Snapdragon 8 Gen 2 / SM8550** on the **AYN Odin 2**) running mainline Linux and SteamOS-like environments ([Armada OS](https://github.com/armada-os/armada)).

---

## The Core Philosophy: "Zero GPU Penalty"

Handheld gaming consoles based on ARM64 SoCs operate under constrained thermal and electrical budgets (typically 5W to 15W total package power):

1. **GPU Starvation with Existing Upscalers**: Running traditional neural or compute-shader upscalers (like FSR, NIS, or Lossless Scaling LSFG) on mobile GPUs (e.g., Adreno 740) steals **20% to 35% of shader capacity**. If a game is already pushing the GPU to 100% to maintain 30 FPS, enabling GPU upscalers drops the base game's framerate, introducing severe stutter.
2. **Untapped Coprocessors**: The SoC integrates a dedicated **Hexagon 790 Tensor Processor (HTP v73)** capable of ~15 INT8 TOPS. In gaming today, this silicon is **0% utilized (dormant)**.
3. **The Solution**: By routing frames directly between Vulkan and the CDSP via zero-copy `dma-buf` descriptors, `hexscale` executes neural upscaling on the NPU in **sub-millisecond latency (< 1 ms)**, leaving **100% of the GPU shader budget free for the game** while saving significant battery power.

---

## Architecture Overview

```
┌─────────────────────────────────────────────────────────────────┐
│               Game / Emulator (e.g. 720p @ 60 FPS)              │
│               Renders via Vulkan (Mesa Turnip / Adreno)         │
└────────────────────────────────┬────────────────────────────────┘
                                 │ vkQueuePresentKHR
                                 ▼
┌─────────────────────────────────────────────────────────────────┐
│               Vulkan Layer (VK_LAYER_HEXSCALE)                  │
│               Exports VkImage as dma-buf descriptor             │
└────────────────────────────────┬────────────────────────────────┘
                                 │ IPC / Zero-Copy Memory Map
                                 ▼
┌─────────────────────────────────────────────────────────────────┐
│               Hexscale Daemon (hexscaled C++20)                 │
│               Controls /dev/fastrpc-cdsp & preloaded QNN context│
└────────────────────────────────┬────────────────────────────────┘
                                 │ FastRPC Bus
                                 ▼
┌─────────────────────────────────────────────────────────────────┐
│               Qualcomm Hexagon 790 NPU (HTP v73)                │
│    Executes XLSR-x1.5 INT8 (W8A8) in < 1.0 ms                   │
│    (Hexagon Tensor Processor / HVX Vector Pipeline)             │
└────────────────────────────────┬────────────────────────────────┘
                                 │ 1080p Upscaled Frame Buffer
                                 ▼
┌─────────────────────────────────────────────────────────────────┐
│               Gamescope / Wayland Compositor (1080p)            │
│               Presented directly to Odin 2 Display              │
└─────────────────────────────────────────────────────────────────┘
                                 ▲
                                 │ Real-time Socket IPC
┌────────────────────────────────┴────────────────────────────────┐
│               Decky Loader Plugin (SteamOS QAM)                 │
│   Toggles ON/OFF, Sharpness Slider, NPU Telemetry & Profiles    │
└─────────────────────────────────────────────────────────────────┘
```

---

## Repository Structure

* **`daemon/` (`hexscaled`)**: Resident C++20 background service. Manages the `/dev/fastrpc-cdsp` session, keeps the XLSR context binary resident in Hexagon L2/TCM memory, and listens on `/run/hexscale/control.sock`.
* **`cli/` (`hexscale-cli`)**: Standalone benchmark and control utility. Allows benchmarking inference latency in microsecond precision and testing upscaling without running a game.
* **`layer/` (`VK_LAYER_HEXSCALE`)**: Vulkan explicit layer that intercepts swapchain presentations and coordinates zero-copy `dma-buf` exchange.
* **`decky/`**: Native Steam Deck / SteamOS Quick Access Menu (QAM) plugin built with React, TypeScript, and Python.
* **`models/`**: Scripts and recipes to convert and quantize models (e.g. XLSR, SESR) for the Qualcomm Hexagon Tensor Processor.
* **`scripts/`**: Systemd unit files (`hexscaled.service`) and build automation.

---

## Active Target Models

### 1. Super-Resolution: **XLSR (Extremely Lightweight Super-Resolution)**
* **Origin**: Qualcomm AI Hub (official Snapdragon NPU model).
* **Quantization**: INT8 (W8A8).
* **Size**: **~45.6 KB**.
* **Measured Latency on NPU**: **< 1.0 ms** (1280x720 ➔ 1920x1080).
* **Power Draw**: < 1.0 Watt on Hexagon HTP.

### 2. Frame Generation (Phase 2): **ANVIL / Hybrid Optical Flow**
* **Origin**: Research paper *ANVIL: Accelerator-Native Video Interpolation* (arXiv:2603.26835).
* **Architecture**: GPU Vulkan compute shader for coarse motion vector smoothing + Hexagon NPU UNet-v3b for neural residual synthesis in INT8.

---

## Building and Testing

### Requirements
* C++20 compatible compiler (`g++` or `clang++`)
* CMake >= 3.20
* Vulkan SDK headers
* (Optional) Qualcomm AI Engine Direct (QNN) SDK for Snapdragon hardware compilation

### Build Instructions
```bash
git clone https://github.com/drewano/hexscale.git
cd hexscale
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

### Running the Standalone Benchmark
```bash
./build/cli/hexscale-cli --bench 200
```

### Querying Daemon Status
```bash
./build/cli/hexscale-cli --status
```

### Starting the System Daemon
```bash
sudo systemctl enable --now hexscaled.service
```

---

## Steam Decky Loader Plugin

The `decky/` directory contains the Steam Quick Access Menu interface.

### Features
* **NPU Upscaling Toggle**: Enable or disable Hexscale in real-time while gaming.
* **Texture Sharpness Slider**: Fine-tune contrast and edge sharpness (0% to 100%).
* **Clock Profiles**:
  * `Efficiency`: Minimal power draw.
  * `Balanced`: Dynamic frequency scaling.
  * `Burst`: Maximum HTP frequency for competitive latency.
* **Live Telemetry**: Displays real-time inference latency (ms), active model, and SoC status directly in the Steam overlay.

---

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.
