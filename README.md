# hexscale

Real-time neural upscaling on Qualcomm Hexagon NPUs (CDSP/HTP) for Linux handhelds.

`hexscale` offloads super-resolution inference from the GPU to the Qualcomm Hexagon Tensor Processor (HTP) on Snapdragon SoCs (SM8550 / AYN Odin 2, SM8650, SM8750). Frames are routed between Vulkan and the CDSP via zero-copy `dma-buf` sharing, eliminating GPU shader overhead for spatial upscaling.

---

## Architecture

```
Game / Emulator (Vulkan)
       │
       │ vkQueuePresentKHR
       ▼
VK_LAYER_HEXSCALE (Vulkan Layer)
       │
       │ dma-buf export + Unix socket IPC
       ▼
hexscaled (C++20 daemon)
       │
       │ FastRPC (/dev/fastrpc-cdsp) or DRM QDA (/dev/accel/accel0)
       ▼
Qualcomm Hexagon HTP (INT8 Tensor Execution)
       │
       │ Upscaled buffer
       ▼
Gamescope / Wayland Compositor (scanout)
```

### Components

* **`daemon/` (`hexscaled`)**: C++20 daemon that maintains the FastRPC / QDA session, keeps the QNN model context resident in Hexagon TCM/L2 memory, maps `dma-buf` file descriptors into CDSP SMMU space, and handles IPC over `/run/hexscale/control.sock`.
* **`layer/` (`VK_LAYER_HEXSCALE`)**: Vulkan explicit layer that intercepts `vkQueuePresentKHR`, exports swapchain images as `dma-buf` handles, and synchronizes presentation with `hexscaled`.
* **`cli/` (`hexscale-cli`)**: Benchmark and debugging utility for testing inference latency, memory mapping overhead, and IPC round-trips without launching a game.
* **`decky/`**: Decky Loader plugin providing toggles, sharpness adjustments, and power profiles directly in the SteamOS Quick Access Menu (QAM).
* **`models/`**: Conversion tooling (`convert_qnn.py`) to compile ONNX super-resolution models into QNN context binaries for HTP targets.

---

## Quantization & HTP Throughput

The Hexagon Tensor Processor (HTP v73 on SM8550) achieves its rated throughput strictly with symmetric **INT8** quantization (`W8A8`).

* **Unquantized models (FP32 / FP16)**: Fall back to scalar DSP emulation, leading to high latency and frame drops.
* **INT8 quantized models (e.g. XLSR, QuickSRNet)**: Run natively on HTP tensor units with sub-millisecond execution times (< 1 ms for 720p $\to$ 1080p) and minimal package power draw (< 1W).

---

## Kernel Requirements

1. **Qualcomm FastRPC**: `CONFIG_QCOM_FASTRPC=m` (or `=y`), with `/dev/fastrpc-cdsp` accessible, or the DRM QDA driver (`/dev/accel/accel0`).
2. **CDSP Power Domain Scaling**: Kernel support for CDSP per-PD proxy performance states (Mukesh Ojha's upstream series, see [armada-packages#66](https://github.com/armada-os/armada-packages/pull/66)). Without this, the CDSP cannot scale up from its lowest idle frequency state.
3. **DMA-BUF Sharing**: `CONFIG_DMA_SHARED_BUFFER=y` for zero-copy buffer handoff between Turnip (Vulkan) and the CDSP SMMU.

---

## Building

### Dependencies
* C++20 compiler (`gcc` >= 13 or `clang` >= 16)
* CMake >= 3.20
* Vulkan headers and loader (`libvulkan-dev`)
* DRM development headers (`libdrm-dev`)
* (Optional) Qualcomm QNN SDK (for compiling context binaries from source)

### Compilation

```bash
git clone https://github.com/drewano/hexscale.git
cd hexscale
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

---

## Usage

### 1. Start the daemon

```bash
# Direct execution
./build/daemon/hexscaled

# Or via systemd
sudo systemctl enable --now hexscaled.service
```

### 2. Run a game with the Vulkan layer

```bash
export VK_LAYER_PATH="$(pwd)/build/layer:$VK_LAYER_PATH"
export ENABLE_HEXSCALE=1
./your_game_or_emulator
```

### 3. Run standalone benchmark

```bash
./build/cli/hexscale-cli --bench 200
```

---

## License

MIT
