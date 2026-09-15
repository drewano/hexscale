# hexscale

Contrast-adaptive sharpening (CAS) for Qualcomm handhelds running Linux
(AYN Odin 2 / SM8550 and friends), applied as a Vulkan layer at presentation
time — no custom OS image required.

`hexscale` intercepts `vkQueuePresentKHR`, runs a compute-shader CAS pass over
the presented frame, and copies the sharpened result back into the swapchain
image before the compositor (Gamescope) receives it. It combines naturally
with compositor-side upscaling (FSR/linear): the game renders at 720p, hexscale
sharpens the fine details, Gamescope scales to the 1080p panel.

---

## What works and what doesn't (honest state)

**Working today (validated on an Odin 2):**
- Vulkan implicit layer with a real, visible CAS sharpening pass (GPU/Adreno).
- Fail-open design: any error, unsupported format or dead daemon silently
  falls back to passthrough — the layer cannot black-screen or freeze a game.
- Zero blocking in the present path (telemetry on a background thread).
- Real GPU timing via timestamp queries, surfaced in the daemon status.
- `hexscaled` control daemon (enable/sharpness/profile over IPC), Decky plugin
  for the Quick Access Menu.
- FastRPC/CDSP foundation: `SCM_RIGHTS` dma-buf registration and
  `FASTRPC_IOCTL_MMAP` into the CDSP SMMU (proven on hardware, ready for a
  future NPU consumer).

**Not working (and why):**
- **Neural inference on the Hexagon HTP.** The QNN path requires Qualcomm's
  proprietary QNN SDK (`libQnnHtp.so` + a context binary compiled offline).
  The SDK is license-restricted and cannot ship in this repo or CI. The
  daemon keeps the loader and dma-buf plumbing; without the SDK it reports
  `GPU-CAS` and everything still works. `models/convert_qnn.py` documents the
  offline pipeline you would run with the SDK installed.

---

## Architecture

```
Game / Emulator (Vulkan, e.g. DXVK)
       │
       │ vkQueuePresentKHR
       ▼
VK_LAYER_HEXSCALE (implicit layer)
  ├─ CAS compute pass (contrast-adaptive sharpening, Adreno GPU)
  ├─ copy back into the swapchain image → presented image is sharpened
  └─ async telemetry (frames, GPU ms) ──► hexscaled (unix socket)
                                            │
                                            ├─ enable / sharpness / profile
                                            ├─ optional: dma-buf → CDSP SMMU
                                            │   (foundation for NPU inference)
                                            └─ status → Decky plugin (QAM)
Gamescope / compositor receives the sharpened frame and upscales it
```

### Components
* **`layer/`** (`libVkLayer_hexscale.so`): Vulkan implicit layer doing the CAS
  pass at present time, in-place on swapchain images.
* **`daemon/`** (`hexscaled`): control daemon — IPC (`/run/hexscale/control.sock`),
  FastRPC session, optional dma-buf mapping, live sharpness control.
* **`cli/`** (`hexscale-cli`): CPU benchmark utility for the IPC/processing path.
* **`decky/`**: Quick Access Menu plugin (toggle, sharpness slider, profile).
* **`models/convert_qnn.py`**: documented (dry-run) pipeline to compile an ONNX
  super-resolution model into an HTP context binary with the QNN SDK.

---

## Install (no custom image)

Grab `hexscale-arm64.tar.gz` from [Releases](../../releases) (or CI artifacts),
copy it to the device, then:

```bash
tar xzf hexscale-arm64.tar.gz && cd hexscale && ./scripts/install.sh
```

This installs into `~/.local` and starts a user-level daemon. Everything is
gated behind `ENABLE_HEXSCALE=1`, so nothing changes until you opt in.

## Enable

```bash
# Per-game (Steam launch options):
ENABLE_HEXSCALE=1 VK_LAYER_PATH=$HOME/.local/share/vulkan/implicit_layer.d %command%

# Whole session (persisted, re-login required):
mkdir -p ~/.config/environment.d
cat > ~/.config/environment.d/60-hexscale.conf << EOF
ENABLE_HEXSCALE=1
VK_LAYER_PATH=$HOME/.local/share/vulkan/implicit_layer.d
EOF
```

Controls:
* `HEXSCALE_SHARPNESS=0..1` — default 0.5; overridden live by the Decky
  plugin / daemon.
* `DISABLE_HEXSCALE=1` — kill switch.
* The daemon can toggle processing off entirely (layer polls it each second).

## Building from source

```bash
./scripts/build-shader.sh   # regenerate SPIR-V (needs glslangValidator)
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

CI builds natively on `ubuntu-24.04-arm` so the artifacts actually run on the
handhelds. Tags (`v*`) publish a GitHub release.

---

## Kernel notes (NPU path only)

1. Qualcomm FastRPC (`CONFIG_QCOM_FASTRPC`), `/dev/fastrpc-cdsp`.
2. CDSP per-PD scaling (Mukesh Ojha's series — see
   [armada-packages#66](https://github.com/armada-os/armada-packages/pull/66)).
3. `CONFIG_DMA_SHARED_BUFFER=y` for zero-copy buffer sharing.

GPU sharpening itself needs none of these — only a Vulkan 1.1 driver.

---

## License

MIT
